#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CYMA-1 plate generator (M1, spec section 3.1): offline eigenmodes of nine plate shapes, tracked through the morph
chain, written as C data (src/plates_data.c, const cy_plate_t CY_PLATES[9]).

    cd instrument && python3 tools/gen_plates.py --out src/plates_data.c --sheet build/plates_sheet.png --test

Why offline: an eigen-solve per shape is seconds of scipy, and every judge rated an on-device solver fatal. The
device only lerps these tables (src/geom.c).

What it does, in order (each step is the spec's, the comments say why):
  1. Masks. Each shape is an analytic inside test sampled on a 256 x 256 supersample of the unit plate square
     (u right, v down). A 64 x 64 cell is inside when >= 8 of its 4 x 4 subsamples are (coverage >= 0.5). Only the
     largest 4-connected component is kept: an island would be a second rigid body (a second zero eigenvalue).
  2. Operator. The 5-point FD Laplacian on the inside cells. Free (Neumann) edges: diagonal = inside neighbours,
     the natural boundary condition. Clamped (Dirichlet, DRUM): diagonal = 4, the outside neighbours are zero.
     Robin (only between DRUM and CIRCLE): diagonal = nb + alpha (4 - nb), alpha 1 clamped .. 0 free.
  3. Solve. eigsh(L + 1e-3 I, sigma=0, which='LM') with a fixed start vector (deterministic output), then sort and
     subtract the shift. Free operators carry the constant (rigid) mode: exactly one vector must be >0.99 aligned
     with the constant; it is dropped. 40 candidates remain per operator.
  4. BAR. Modes with > 98 % of their gradient energy in x are the bending modes; the n-th gets lambda_1 E_n, E_n the
     Euler free-free beam ratios, so that at plate stiffness (f ~ lambda) the bar rings like a real bar.
  5. Tracking (the cure for the "mushy mid-morph"). DRUM keeps its 32 lowest modes. Every edge i -> i+1 is walked in
     four steps (t = .25, .5, .75 and the keyframe), the in-between operators solved on the lerped SDF (geometric
     edges, with shape i+1's edge) or with the Robin link (DRUM -> CIRCLE). Each step matches the 32 tracked vectors
     to the 40 candidates by |overlap| with the Hungarian algorithm, after rotating each degenerate candidate
     subspace onto the tracked vectors (an eigenpair's basis is arbitrary; without the rotation a circle's dipole
     pair would split its overlap 0.7/0.7 and the sand would spin). Signs are aligned at every step.
     The lowest FORCE_LOW candidates of every step are always matched (a bonus in the assignment), so no shape ever
     loses its fundamental or low partials to a better-overlapping high mode.
  6. Tables per keyframe: log2(lambda / lambda_min), the 24 x 24 fields (cell centres, bilinear from 64 x 64 after
     extending free fields outward with the nearest inside value and clamped ones with 0), monopole, degen ids
     (|dl|/l < 2e-3), the 48 x 48 SDF (EDT of the 256 x 256 mask, plate units x 128, < 0 inside).

Options: --png FILE --name NAME --replace SHAPE builds a free-edge user plate from a silhouette (alpha channel, or
luminance < 50 %) in place of a slot (offline only; the default build never uses it). --sheet writes the contact
sheet (9 rows x outline + the first 8 tracked fields, drawn as sand on the nodal lines). --test runs the asserts.
Needs numpy, scipy and Pillow (miniconda python3 or .venv312)."""
import argparse
import datetime
import hashlib
import os
import sys

import numpy as np
import scipy
import scipy.sparse as sp
import scipy.sparse.linalg as sla
from scipy.ndimage import distance_transform_edt, label
from scipy.optimize import brentq, linear_sum_assignment

N = 64                      # solve grid over [0,1]^2
SS = 4                      # supersampling per solve cell (coverage rule, 4 x 4)
NS = N * SS                 # 256: the SDF raster
G = 24                      # CY_GRID: stored field cells
S = 48                      # CY_SDF
NMODES = 32                 # CY_NMODES: tracked modes
NCAND = 40                  # candidates per operator
SHIFT = 1e-3                # eigsh shift: L + 1e-3 I is non-singular for free plates
DEGEN_TOL = 2e-3            # |dl|/l: degen ids (spec) and the subspace rotation in tracking
FORCE_LOW = 16              # the lowest candidates every tracking step must match
ROBIN = "alpha = 1 - t"      # DRUM -> CIRCLE: the Robin weight at each in-between t (.75 .5 .25)
STEPS = (0.25, 0.5, 0.75)   # in-between operators per edge
EULER_SPEC = (1.0, 2.7565, 5.4039, 8.9330, 13.3443, 18.6379)   # free-free beam lambda ratios (spec section 3.1)
BESSEL = (1.0, 1.5933, 2.1355, 2.2954, 2.6531, 2.9173)       # clamped circular membrane frequency ratios (8.3)
PLATE_BYTES = 12 + 1 + 3 + 4 * NMODES + 4 * NMODES + NMODES + S * S + NMODES * G * G   # sizeof(cy_plate_t)

# ----------------------------------------------------------------------------------------------------- shapes ---
# Analytic signed distances (negative inside) in plate coordinates; only their sign is used (the stored SDF comes
# from the EDT of the raster, as the spec asks, so a PNG plate and an analytic one are treated alike).


def _circle(u, v, cu, cv, r):
    return np.hypot(u - cu, v - cv) - r


def _box(u, v, cu, cv, hu, hv):
    qu, qv = np.abs(u - cu) - hu, np.abs(v - cv) - hv
    return np.hypot(np.maximum(qu, 0), np.maximum(qv, 0)) + np.minimum(np.maximum(qu, qv), 0)


def _smin(a, b, k):         # polynomial smooth union (Quilez): rounds the joins over a width k
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0, 1)
    return b + (a - b) * h - k * h * (1 - h)


def sd_circle(u, v):
    return _circle(u, v, 0.5, 0.5, 0.46)


def sd_hex(u, v):           # circumradius .47, vertices at 0, 60, ... degrees: edge normals at 30 + 60 k
    ri = 0.47 * np.cos(np.pi / 6)
    d = np.full(np.shape(u), -1e9)
    for k in range(6):
        a = np.pi / 6 + k * np.pi / 3
        d = np.maximum(d, (u - 0.5) * np.cos(a) + (v - 0.5) * np.sin(a) - ri)
    return d


def sd_square(u, v):
    return _box(u, v, 0.5, 0.5, 0.41, 0.41)


def sd_triangle(u, v):      # equilateral, circumradius .5, centroid (.5, .54), apex up (v down): inradius .25
    d = np.full(np.shape(u), -1e9)
    for a in (np.pi / 2, np.pi / 2 + 2 * np.pi / 3, np.pi / 2 + 4 * np.pi / 3):   # outward normals: base down
        d = np.maximum(d, (u - 0.5) * np.cos(a) + (v - 0.54) * np.sin(a) - 0.25)
    return d


def sd_star(u, v):          # r(theta) = .31 + .15 cos(5 (theta + pi/2)): a tip straight up; union a circle r .2
    th = np.arctan2(v - 0.5, u - 0.5)
    r = np.hypot(u - 0.5, v - 0.5)
    return np.minimum(r - (0.31 + 0.15 * np.cos(5 * (th + np.pi / 2))), r - 0.20)


def sd_guitar(u, v):
    lower = _circle(u, v, 0.5, 0.66, 0.25)
    upper = _circle(u, v, 0.5, 0.34, 0.19)
    waist = _box(u, v, 0.5, 0.5, 0.15, 0.16)
    return _smin(_smin(lower, upper, 0.04), waist, 0.04)


def sd_stadium(u, v):       # capsule: segment (.26,.5)-(.74,.5), radius .22
    return np.hypot(u - np.clip(u, 0.26, 0.74), v - 0.5) - 0.22


def sd_bar(u, v):
    return _box(u, v, 0.5, 0.5, 0.45, 0.075)


# name, clamped, inside source (a signed-distance function or a 256 x 256 bool raster)
SHAPES = [
    ["DRUM", True, sd_circle], ["CIRCLE", False, sd_circle], ["HEX", False, sd_hex],
    ["SQUARE", False, sd_square], ["TRIANGLE", False, sd_triangle], ["STAR", False, sd_star],
    ["GUITAR", False, sd_guitar], ["STADIUM", False, sd_stadium], ["BAR", False, sd_bar],
]


def raster(src):
    """256 x 256 inside raster (subsample centres) of an analytic shape or a ready raster"""
    if isinstance(src, np.ndarray):
        return src.astype(bool)
    c = (np.arange(NS) + 0.5) / NS
    u, v = np.meshgrid(c, c)            # [row v][col u]
    return src(u, v) < 0


def sdf_from_raster(m):
    """signed distance in plate units, negative inside: EDT both ways with the half-pixel correction, so the zero
    level sits on the pixel edges rather than a pixel inside them. Padded so the raster's border counts as outside."""
    p = np.pad(m, 1, constant_values=False)
    d_in = distance_transform_edt(p)[1:-1, 1:-1]
    d_out = distance_transform_edt(~p)[1:-1, 1:-1]
    return np.where(m, -(d_in - 0.5), d_out - 0.5) / NS


def largest_component(mask):
    lab, n = label(mask)                # 4-connectivity: the 5-point stencil's coupling
    if n <= 1:
        return mask
    sizes = np.bincount(lab.ravel())[1:]
    return lab == (1 + int(np.argmax(sizes)))


def mask_from_inside256(m256):
    cov = m256.reshape(N, SS, N, SS).mean(axis=(1, 3))
    return largest_component(cov >= 0.5)


def bilinear_grid(F, n_out):
    """sample the last two axes of F (n x n, cell centres) at the n_out x n_out cell centres (sand.py down())"""
    n = F.shape[-1]
    c = (np.arange(n_out) + 0.5) / n_out * n - 0.5
    i0 = np.clip(np.floor(c).astype(int), 0, n - 2)
    f = np.clip(c - i0, 0.0, 1.0)
    A = F[..., i0, :] * (1 - f)[:, None] + F[..., i0 + 1, :] * f[:, None]
    return A[..., :, i0] * (1 - f) + A[..., :, i0 + 1] * f


# ---------------------------------------------------------------------------------------------------- solving ---
def laplacian(mask, bc, alpha=0.0):
    idx = -np.ones((N, N), int)
    n = int(mask.sum())
    idx[mask] = np.arange(n)
    pad = np.pad(idx, 1, constant_values=-1)
    rows, cols, nb = [], [], np.zeros(n)
    for da, db in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nbr = pad[1 + da:1 + da + N, 1 + db:1 + db + N][mask]
        ok = nbr >= 0
        rows.append(np.nonzero(ok)[0])
        cols.append(nbr[ok])
        nb += ok
    rows, cols = np.concatenate(rows), np.concatenate(cols)
    if bc == "free":
        diag = nb
    elif bc == "clamped":
        diag = np.full(n, 4.0)
    else:
        diag = nb + alpha * (4.0 - nb)
    A = sp.csr_matrix((-np.ones(len(rows)), (rows, cols)), shape=(n, n)) + sp.diags(diag)
    return A.tocsc()


SOLVE_LOG = []              # (label, n cells, rigid alignment, min lambda / max lambda) for --test


def solve(mask, bc, alpha=0.0, tag=""):
    """the NCAND lowest modes: (lambda[NCAND], V[NCAND, N, N] unit-norm with zeros outside)"""
    A = laplacian(mask, bc, alpha)
    n = A.shape[0]
    free = bc == "free"
    k = NCAND + (1 if free else 0)
    v0 = np.random.default_rng(0x5EED).standard_normal(n)   # fixed: ARPACK's own start is random
    w, V = sla.eigsh(A + SHIFT * sp.identity(n, format="csc"), k=k, sigma=0, which="LM", v0=v0, tol=1e-10)
    o = np.argsort(w)
    w, V = w[o] - SHIFT, V[:, o]
    V /= np.linalg.norm(V, axis=0)
    rigid = 0.0
    if free:
        align = np.abs(V.sum(axis=0)) / np.sqrt(n)
        hits = np.nonzero(align > 0.99)[0]
        if len(hits) != 1:
            raise SystemExit(f"gen_plates: {tag}: {len(hits)} modes are >0.99 constant (expected exactly one)")
        rigid = float(align[hits[0]])
        keep = np.ones(k, bool)
        keep[hits[0]] = False
        w, V = w[keep], V[:, keep]
        if not w.min() > 1e-6 * w.max():
            raise SystemExit(f"gen_plates: {tag}: a near-zero eigenvalue survived the rigid drop ({w.min():.3g})")
    F = np.zeros((NCAND, N, N))
    F[:, mask] = V[:, :NCAND].T
    SOLVE_LOG.append((tag, n, rigid, float(w.min() / w.max())))
    return w[:NCAND].copy(), F


def degenerate_groups(lam, tol=DEGEN_TOL):
    """chains of |dl|/l < tol in ascending order: lists of indices into lam"""
    o = np.argsort(lam, kind="stable")
    groups, cur = [], [o[0]]
    for a, b in zip(o[:-1], o[1:]):
        if abs(lam[b] - lam[a]) / max(abs(lam[a]), 1e-30) < tol:
            cur.append(b)
        else:
            groups.append(cur)
            cur = [b]
    groups.append(cur)
    return groups


def canonicalise(lam, F, mask):
    """make the root keyframe's basis reproducible: each degenerate subspace is diagonalised under the u mirror,
    then the v mirror (so a circle's pairs come out as the cos/sin forms with nodal diameters on the axes), and each
    vector's sign is fixed by its first clearly non-zero moment (1, u, v, uv, u^2 - v^2...)."""
    F = F.copy()
    for g in degenerate_groups(lam):
        if len(g) < 2:
            continue
        W = F[g].reshape(len(g), -1)
        for flip in (lambda X: X[:, :, ::-1], lambda X: X[:, ::-1, :]):
            M = W @ flip(W.reshape(len(g), N, N)).reshape(len(g), -1).T
            M = 0.5 * (M + M.T)
            ev, Q = np.linalg.eigh(M)
            W = Q[:, np.argsort(-ev, kind="stable")].T @ W
        F[g] = W.reshape(len(g), N, N)
    for k in range(len(F)):
        F[k] *= moment_sign(F[k], mask)
    return F


_c = (np.arange(N) + 0.5) / N - 0.5
_u, _v = np.meshgrid(_c, _c)
MOMENTS = [np.ones_like(_u), _u, _v, _u * _v, _u * _u - _v * _v, _u * _v * (_u * _u - _v * _v), _u ** 3, _v ** 3,
           _u ** 3 * _v, _u * _v ** 3, (_u + 0.37 * _v) ** 5]


def moment_sign(f, mask):
    """+1/-1 from the first clearly non-zero low moment: a reproducible sign for a vector nothing else orients"""
    for m in MOMENTS:
        s = float((f * m * mask).sum())
        if abs(s) > 1e-4 * np.abs(f).sum() / N:
            return 1.0 if s > 0 else -1.0
    return 1.0


def rotate_onto(prev, lam, F):
    """rotate every degenerate subspace of the candidates (lam, F) onto the tracked vectors prev (orthogonal
    Procrustes against the tracked vectors that project most into it); the members stay eigenvectors"""
    F = F.copy()
    P_all = prev.reshape(len(prev), -1)
    for g in degenerate_groups(lam):
        m = len(g)
        if m < 2:
            continue
        W = F[g].reshape(m, -1)
        P = W @ P_all.T                                   # m x 32 projections
        J = np.argsort(-np.linalg.norm(P, axis=0), kind="stable")[:m]
        U, _, Vt = np.linalg.svd(P[:, J])
        Q = Vt.T @ U.T
        F[g] = (Q @ W).reshape(m, N, N)
    return F


def euler_ratios(n):
    """free-free Euler-Bernoulli beam: roots of cos x cosh x = 1 (written cos x = 1/cosh x, stable), (x_n/x_1)^2"""
    xs = [brentq(lambda x: np.cos(x) - 1.0 / np.cosh(x), (k + 0.5) * np.pi - 1.0, (k + 0.5) * np.pi + 1.0)
          for k in range(1, n + 1)]
    E = [(x / xs[0]) ** 2 for x in xs]
    for i, e in enumerate(EULER_SPEC[:n]):            # the spec's table, to its printed precision
        assert abs(E[i] - e) < 2e-4, (i, E[i], e)
        E[i] = e
    return E


def bar_correct(lam, F, mask):
    """BAR: the n-th bending mode (> 98 % of its gradient energy along u) gets lambda_1 E_n"""
    lam = lam.copy()
    hm = mask[:, 1:] & mask[:, :-1]
    vm = mask[1:, :] & mask[:-1, :]
    ex = ((F[:, :, 1:] - F[:, :, :-1]) ** 2 * hm).sum(axis=(1, 2))
    ey = ((F[:, 1:, :] - F[:, :-1, :]) ** 2 * vm).sum(axis=(1, 2))
    frac = ex / (ex + ey)
    bend = [i for i in np.argsort(lam, kind="stable") if frac[i] > 0.98]
    E = euler_ratios(len(bend))
    l1 = lam[bend[0]]
    for n, i in enumerate(bend):
        lam[i] = l1 * E[n]
    return lam, bend, frac


# --------------------------------------------------------------------------------------------------- pipeline ---
class Keyframe:
    pass


def build(shapes, verbose=True):
    """solve, track and tabulate every keyframe; returns (keyframes, tracking log)"""
    say = print if verbose else (lambda *a, **k: None)
    m256 = [raster(s[2]) for s in shapes]
    sdf256 = [sdf_from_raster(m) for m in m256]
    masks = [mask_from_inside256(m) for m in m256]
    bcs = ["clamped" if s[1] else "free" for s in shapes]

    lam0, F0 = solve(masks[0], bcs[0], tag=shapes[0][0])
    o = np.argsort(lam0, kind="stable")[:NMODES]
    lam0, F0 = lam0[o], canonicalise(lam0[o], F0[o], masks[0])
    cur = F0
    kfs = []

    def store(i, lam, F, cand_idx=None, bend=None):
        k = Keyframe()
        k.name, k.clamped, k.mask, k.sdf256 = shapes[i][0], int(shapes[i][1]), masks[i], sdf256[i]
        k.lam, k.F, k.cand_idx, k.bend = lam.copy(), F.copy(), cand_idx, bend
        kfs.append(k)

    store(0, lam0, F0, cand_idx=np.arange(NMODES))
    track = []
    for i in range(len(shapes) - 1):
        edge = []
        for s, t in enumerate(STEPS + (1.0,)):
            if t == 1.0:
                lam, F = solve(masks[i + 1], bcs[i + 1], tag=shapes[i + 1][0])
                mask = masks[i + 1]
            elif bcs[i] == "clamped" and bcs[i + 1] == "free" and np.array_equal(masks[i], masks[i + 1]):
                mask = masks[i + 1]
                lam, F = solve(mask, "robin", 1.0 - t, tag=f"{shapes[i][0]}>{shapes[i + 1][0]} a{1.0 - t:g}")
            else:
                m = mask_from_inside256((1 - t) * sdf256[i] + t * sdf256[i + 1] < 0)
                mask = m
                lam, F = solve(m, bcs[i + 1], tag=f"{shapes[i][0]}>{shapes[i + 1][0]} t{t}")
            F = rotate_onto(cur, lam, F)
            O = np.abs(cur.reshape(NMODES, -1) @ F.reshape(NCAND, -1).T)          # 32 x 40
            low = np.argsort(lam, kind="stable")[:FORCE_LOW]
            bonus = np.zeros(NCAND)
            bonus[low] = 4.0                                                       # > any overlap sum gain
            r, c = linear_sum_assignment(-(O + bonus[None, :]))
            col = np.empty(NMODES, int)
            col[r] = c
            so = (cur.reshape(NMODES, -1) * F[col].reshape(NMODES, -1)).sum(axis=1)
            sgn = np.where(so < 0, -1.0, 1.0)
            for j in np.nonzero(np.abs(so) < 0.05)[0]:     # no real partner: a noise-level overlap's sign would
                sgn[j] = moment_sign(F[col[j]], mask)      # differ between BLAS builds; use a reproducible one
            ov = O[np.arange(NMODES), col]
            edge.append((t, float(ov.mean()), float(ov.min())))
            cur = F[col] * sgn[:, None, None]
            if t == 1.0:
                lam_k = lam[col]
                bend = None
                if shapes[i + 1][0] == "BAR" or (i + 1 == len(shapes) - 1 and shapes[i + 1][0] == "BAR"):
                    lam_c, bend_c, _ = bar_correct(lam, F, mask)
                    lam_k = lam_c[col]
                    inv = {int(cc): j for j, cc in enumerate(col)}
                    bend = [inv.get(int(b), -1) for b in bend_c]
                store(i + 1, lam_k, cur, cand_idx=col, bend=bend)
                kfs[-1].cand_lam = lam
        track.append(edge)
        say(f"  {shapes[i][0]:>8} > {shapes[i + 1][0]:<8} mean |overlap| " +
            "  ".join(f"t{t:.2f} {m:.3f} (min {mn:.2f})" for t, m, mn in edge))
    kfs[0].cand_lam = lam0
    for k in kfs:
        tabulate(k)
    return kfs, track


def tabulate(k):
    """the stored tables of one keyframe (every one asserted finite)"""
    # (rounded to 1e-7: below float32's resolution here, and it drops the 1e-16 noise that made exact twins and the
    # free plates' zero monopoles print differently from one numpy/BLAS to the next)
    k.log2_lambda = np.round(np.log2(k.lam / k.lam.min()), 7) + 0.0
    inside = k.mask
    k.monopole = np.round([f[inside].mean() / np.abs(f[inside]).max() for f in k.F], 7) + 0.0
    f24 = bilinear_grid(extend_out(k.F, inside, k.clamped), G)
    f24 = f24 / np.abs(f24).max(axis=(1, 2), keepdims=True) * 127.0
    k.field = np.clip(np.round(f24), -127, 127).astype(np.int8)
    d = bilinear_grid(k.sdf256, S) * 128.0
    k.sdf = np.clip(np.round(d), -127, 127).astype(np.int8)
    k.degen = np.zeros(NMODES, np.uint8)
    for gid, g in enumerate(degenerate_groups(k.lam)):
        k.degen[g] = gid
    for x in (k.lam, k.log2_lambda, k.monopole, f24, d):
        if not np.isfinite(x).all():
            raise SystemExit(f"gen_plates: {k.name}: a non-finite table value")


# ------------------------------------------------------------------------------------------------------ output ---
def cfloat(x):
    s = "%.9g" % float(x)
    if "." not in s and "e" not in s and "n" not in s:
        s += ".0"
    return s + "f"


def write_c(path, kfs, track, sha):
    out = []
    out.append("/* SPDX-License-Identifier: MPL-2.0 */")
    out.append("/* CYMA-1 plate tables (M1, spec section 3.1). GENERATED by tools/gen_plates.py, then committed: do not edit.")
    out.append(f" * generator sha1 {sha}  date {datetime.date.today().isoformat()}  numpy {np.__version__}  scipy {scipy.__version__}")
    out.append(f" * {len(kfs)} shapes x {PLATE_BYTES:,} B = {len(kfs) * PLATE_BYTES:,} B of flash. Per shape, in tracked mode order:")
    out.append(" *   log2_lambda  log2(lambda / lambda_min), Laplacian eigenvalues on a 64 x 64 FD grid (BAR: Euler bending)")
    out.append(" *   monopole     mean(phi) / max|phi| over the inside (about 0 on free plates: they are orthogonal to 1)")
    out.append(" *   degen        group ids, |dl|/l < 2e-3 within the shape, numbered by ascending lambda")
    out.append(" *   sdf          48 x 48 cell centres, plate units x 128, < 0 inside")
    out.append(" *   field        [mode][v][u] at the 24 x 24 cell centres, max |phi| = 127; outside: nearest inside (free) or 0")
    out.append(" * Tracking, mean matched |overlap| per step (t = .25 .5 .75 1):")
    for i, e in enumerate(track):
        out.append(f" *   {kfs[i].name:>8} > {kfs[i + 1].name:<8} " + " ".join(f"{m:.3f}" for _, m, _ in e))
    out.append(" */")
    out.append('#include "cy_plates.h"')
    out.append("")
    out.append("const cy_plate_t CY_PLATES[CY_NSHAPES] = {")
    for k in kfs:
        out.append(f"    {{ /* {k.name} */")
        out.append(f'        "{k.name}", {k.clamped}, {{0, 0, 0}},')
        for arr in (k.log2_lambda, k.monopole):
            vals = [cfloat(x) for x in arr]
            out.append("        {" + ", ".join(vals[:8]) + ",")
            for a in range(8, NMODES, 8):
                out.append("         " + ", ".join(vals[a:a + 8]) + ("," if a + 8 < NMODES else "},"))
        out.append("        {" + ", ".join(str(int(x)) for x in k.degen) + "},")
        out.append("        { /* sdf */")
        for r in range(S):
            out.append("         {" + ",".join(str(int(x)) for x in k.sdf[r]) + "},")
        out.append("        },")
        out.append("        { /* field */")
        for m in range(NMODES):
            out.append(f"         {{ /* mode {m} */")
            for r in range(G):
                out.append("          {" + ",".join(str(int(x)) for x in k.field[m, r]) + "},")
            out.append("         },")
        out.append("        },")
        out.append("    },")
    out.append("};")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


def _font(px):
    from PIL import ImageFont
    here = os.path.dirname(os.path.abspath(__file__))
    for p in [os.path.join(here, "..", "..", "reference", "fm1-base", "assets", "fonts"),
              "/System/Library/Fonts/Supplemental", "/Library/Fonts"]:
        if os.path.isdir(p):
            for fn in sorted(os.listdir(p)):
                if fn.lower().endswith((".ttf", ".otf")) and ("barlow" in fn.lower() or "arial" in fn.lower()):
                    try:
                        return ImageFont.truetype(os.path.join(p, fn), px)
                    except OSError:
                        pass
    return ImageFont.load_default()


def chladni_tile(field, sdf, px, clamped=False, tint=0.35):
    """a stored 24 x 24 field drawn the way sand shows it: dark plate, a faint warm/cold tint for the sign, and
    sand piled on the nodal lines (width from |phi| / |grad phi|, about a constant 1.3 px everywhere). Sampled like
    the device does: bilinear at cell centres from the int8 tables."""
    c = (np.arange(px) + 0.5) / px
    f = _bilin(field.astype(float) / 127.0, c)
    d = _bilin(sdf.astype(float) / 128.0, c)
    gy, gx = np.gradient(f, 1.0 / px)
    dist_px = np.abs(f) / (np.hypot(gx, gy) / px + 1e-6)
    sand = np.exp(-(dist_px / 1.3) ** 2)
    cover = np.clip(0.5 - d * px, 0, 1)
    void = np.array([10, 11, 16]) / 255
    plate = np.array([22, 30, 44]) / 255
    warm = np.array([92, 60, 40]) / 255
    cold = np.array([34, 64, 96]) / 255
    s = np.clip(f, -1, 1)[..., None]
    base = plate + tint * np.where(s > 0, s * (warm - plate), -s * (cold - plate))
    sandc = np.array([255, 242, 216]) / 255
    img = base * (1 - sand[..., None]) + sandc * sand[..., None]
    rim = np.exp(-((d * px) / (1.4 if clamped else 0.8)) ** 2)[..., None]
    img = img * (1 - 0.6 * rim) + np.array([180, 170, 150]) / 255 * 0.6 * rim
    img = void * (1 - cover[..., None]) + img * cover[..., None]
    return (np.clip(img, 0, 1) * 255).astype(np.uint8)


def _bilin(T, c):
    n = T.shape[0]
    x = c * n - 0.5
    i0 = np.clip(np.floor(x).astype(int), 0, n - 2)
    f = np.clip(x - i0, 0, 1)
    A = T[i0, :] * (1 - f)[:, None] + T[i0 + 1, :] * f[:, None]
    return A[:, i0] * (1 - f) + A[:, i0 + 1] * f


def write_sheet(path, kfs, track, ncol=8, px=112):
    from PIL import Image, ImageDraw
    pad, lab, head = 6, 30, 34
    W = pad + (ncol + 1) * (px + pad) + 200
    H = head + len(kfs) * (px + lab + pad) + pad
    img = Image.new("RGB", (W, H), (10, 11, 16))
    dr = ImageDraw.Draw(img)
    f_big, f_sm = _font(15), _font(11)
    dr.text((pad, 8), "CYMA-1 plates: outline + first 8 tracked modes (sand on nodal lines; label = membrane ratio, "
            "plate ratio)", fill=(230, 220, 200), font=f_big)
    for r, k in enumerate(kfs):
        y = head + r * (px + lab + pad)
        tile = chladni_tile(np.zeros((G, G), np.int8) + 127, k.sdf, px, k.clamped, tint=0.0)
        img.paste(Image.fromarray(tile), (pad, y))
        dr.text((pad + 4, y + px + 4), f"{r} {k.name}" + (" (clamped)" if k.clamped else ""), fill=(255, 200, 120),
                font=f_sm)
        lam = 2.0 ** k.log2_lambda
        for m in range(ncol):
            x = pad + (m + 1) * (px + pad)
            img.paste(Image.fromarray(chladni_tile(k.field[m], k.sdf, px, k.clamped)), (x, y))
            dr.text((x + 3, y + px + 4), f"{m}: {np.sqrt(lam[m]):.3f} / {lam[m]:.2f}", fill=(200, 200, 210),
                    font=f_sm)
        x = pad + (ncol + 1) * (px + pad) + 4
        if r == 0:
            dr.text((x, y + 4), "root: 32 lowest modes", fill=(160, 160, 170), font=f_sm)
        else:
            e = track[r - 1]
            dr.text((x, y + 4), f"tracked from {kfs[r - 1].name}", fill=(160, 160, 170), font=f_sm)
            for j, (t, mn, lo) in enumerate(e):
                dr.text((x, y + 20 + 14 * j), f"t {t:.2f}: mean |ov| {mn:.3f}  min {lo:.2f}",
                        fill=(120, 220, 140) if mn >= 0.6 else (255, 120, 110), font=f_sm)
        order = np.argsort(k.log2_lambda, kind="stable")
        dr.text((x, y + 82), "lowest: " + " ".join(str(int(i)) for i in order[:8]), fill=(160, 160, 170), font=f_sm)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    img.save(path)


# -------------------------------------------------------------------------------------------------------- test ---
def run_tests(kfs, track):
    fails = []

    def check(ok, msg):
        print(("  ok    " if ok else "  FAIL  ") + msg)
        if not ok:
            fails.append(msg)

    print("tests (spec 3.1 / 8.3):")
    free_solves = [s for s in SOLVE_LOG if s[2] > 0]
    check(all(s[2] > 0.99 for s in free_solves) and len(free_solves) > 0,
          f"rigid mode found and dropped in all {len(free_solves)} free solves (alignment >= "
          f"{min(s[2] for s in free_solves):.6f})")
    check(all(s[3] > 1e-6 for s in free_solves),
          f"no lambda < 1e-6 lambda_max after the drop (min ratio {min(s[3] for s in free_solves):.2e})")
    total = len(kfs) * PLATE_BYTES
    check(total <= 200 * 1024, f"data {total:,} B <= 204,800 B")
    bar = [k for k in kfs if k.name == "BAR"]
    if bar:
        b = bar[0]
        E = euler_ratios(6)
        got = [2.0 ** b.log2_lambda[j] for j in b.bend[:6]] if b.bend and min(b.bend[:6]) >= 0 else []
        check(len(got) == 6, f"BAR: the 6 lowest bending modes are tracked (tracked ids {b.bend[:6]})")
        if got:
            err = max(abs(g / E[n] - 1) for n, g in enumerate(got))
            check(err < 1e-5, f"BAR: bending lambda ratios = Euler (max err {err:.1e})")
            check(abs(min(2.0 ** b.log2_lambda) - 1.0) < 1e-6, "BAR: lambda_min is the first bending mode")
            eps = 50.0
            r = [np.sqrt((g + eps * g * g) / (1 + eps)) for g in got[:5]]
            perr = max(abs(x / E[n] - 1) for n, x in enumerate(r))
            check(perr < 0.01, f"BAR at stiffness 0.8: within 1 % of Euler (max {100 * perr:.2f} %)")
    drum = kfs[0]
    lam = np.sort(2.0 ** drum.log2_lambda)
    peaks = [lam[0]]
    for x in lam[1:]:
        if x / peaks[-1] > 1 + 5e-3:
            peaks.append(x)
    fr = np.sqrt(np.array(peaks[:6]))
    berr = np.abs(fr / np.array(BESSEL) - 1)
    check(berr.max() < 0.03, "DRUM (membrane) ratios vs Bessel: " + " ".join(f"{x:.4f}" for x in fr) +
          f" (max err {100 * berr.max():.2f} %)")
    means = [m for e in track for _, m, _ in e]
    check(np.mean(means) >= 0.6, f"mean tracked |overlap| {np.mean(means):.3f} >= 0.6 (all {len(means)} steps)")
    low = [(kfs[i].name, kfs[i + 1].name, t, m) for i, e in enumerate(track) for t, m, _ in e if m < 0.6]
    print(f"  info  {len(low)} of {len(means)} steps below 0.6 on their own (the masks of dissimilar neighbours "
          f"differ; 7 in-between steps were measured and do not improve the runtime lerp): " +
          ", ".join(f"{a}>{b} t{t:.2f} {m:.2f}" for a, b, t, m in low))
    fid = lerp_fidelity(kfs)
    print("  info  runtime lerp at t=.5 vs the true mid-shape modes, best |overlap| (all / lowest 16): " +
          "  ".join(f"{kfs[i].name[:3]}>{kfs[i + 1].name[:3]} {x:.2f}/{y:.2f}" for i, (x, y) in enumerate(fid)))
    check(min(y for _, y in fid) >= 0.7, f"the lowest 16 modes' mid-morph lerp stays >= 0.7 like a true mode "
          f"(worst edge {min(y for _, y in fid):.2f})")
    for k in kfs:
        mx = np.abs(k.field.astype(int)).max(axis=(1, 2))
        ok = (mx == 127).all() and k.sdf[S // 2, S // 2] < 0 and k.sdf[0, 0] > 0 and k.sdf[-1, -1] > 0
        lo = np.argsort(k.cand_lam, kind="stable")[:FORCE_LOW]
        has_low = set(int(x) for x in lo) <= set(int(x) for x in k.cand_idx)
        mono = np.abs(k.monopole).max() if not k.clamped else abs(k.monopole[0])
        ok &= has_low and abs(k.log2_lambda.min()) < 1e-9 and (k.log2_lambda >= 0).all()
        ok &= (mono < 0.02) if not k.clamped else (mono > 0.3)
        check(bool(ok), f"{k.name:>8}: |field| max 127, sdf sign, log2_lambda >= 0 (min 0), its {FORCE_LOW} lowest "
              f"modes tracked, monopole {'max ' if not k.clamped else 'mode 0 '}{mono:.3f}, "
              f"{len(set(k.degen.tolist()))} degen groups")
    return fails


_EXT = {}


def extend_out(F, mask, clamped):
    """fields outside the mask: the nearest inside value (free) or 0 (clamped), as stored and as geom.c lerps them.
    The nearest inside cells are found exactly (integer squared distances) and equidistant ones are averaged:
    distance_transform_edt's indices break such ties differently between scipy versions, which made the tables
    platform-dependent and a symmetric plate's extension lopsided."""
    if clamped:
        return np.where(mask[None], F, 0.0)
    key = mask.tobytes()
    if key not in _EXT:
        ins = np.argwhere(mask)
        out = np.argwhere(~mask)
        W = np.zeros((len(out), len(ins)), np.float32)
        for a in range(0, len(out), 512):
            o = out[a:a + 512]
            d2 = ((o[:, None, :] - ins[None, :, :]) ** 2).sum(-1)           # exact integers
            near = d2 == d2.min(axis=1, keepdims=True)
            W[a:a + 512] = near / near.sum(axis=1, keepdims=True)
        _EXT[key] = (ins, out, W)
    ins, out, W = _EXT[key]
    E = F.copy()
    E[:, out[:, 0], out[:, 1]] = F[:, ins[:, 0], ins[:, 1]] @ W.T
    return E


def lerp_fidelity(kfs):
    """what the device actually plays mid-morph: the lerp of the paired keyframe fields at t = .5, against the true
    eigenmodes of the t = .5 operator (best |overlap| per tracked mode). Per edge: (all 32, the lowest 16)."""
    out = []
    for a, b in zip(kfs[:-1], kfs[1:]):
        if a.clamped and not b.clamped and np.array_equal(a.mask, b.mask):
            m = b.mask
            _, F = solve(m, "robin", 0.5, tag="fidelity")
        else:
            m = mask_from_inside256(0.5 * a.sdf256 + 0.5 * b.sdf256 < 0)
            _, F = solve(m, "clamped" if b.clamped else "free", tag="fidelity")
        L = 0.5 * (extend_out(a.F, a.mask, a.clamped) + extend_out(b.F, b.mask, b.clamped)) * m[None]
        L /= np.linalg.norm(L.reshape(NMODES, -1), axis=1)[:, None, None] + 1e-12
        best = np.abs(L.reshape(NMODES, -1) @ F.reshape(len(F), -1).T).max(axis=1)
        rank = np.argsort(np.argsort(np.minimum(a.log2_lambda, b.log2_lambda), kind="stable"), kind="stable")
        out.append((float(best.mean()), float(best[rank < 16].mean())))
    return out


def load_png(path):
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    a = np.asarray(im).astype(float) / 255
    if a[..., 3].min() < 0.98:
        ink = a[..., 3] >= 0.5
    else:
        ink = (0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]) < 0.5
    ys, xs = np.nonzero(ink)
    if len(xs) == 0:
        raise SystemExit("gen_plates: the PNG has no silhouette")
    ink = ink[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    h, w = ink.shape
    side = int(round(NS * 0.92))                      # the plate square with the shapes' 4 % margin
    sc = side / max(h, w)
    tw, th = max(1, int(round(w * sc))), max(1, int(round(h * sc)))
    m = np.asarray(Image.fromarray((ink * 255).astype(np.uint8)).resize((tw, th), Image.LANCZOS)) >= 128
    out = np.zeros((NS, NS), bool)
    y0, x0 = (NS - th) // 2, (NS - tw) // 2
    out[y0:y0 + th, x0:x0 + tw] = m
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=None, help="C output (src/plates_data.c)")
    ap.add_argument("--sheet", default=None, help="contact sheet PNG")
    ap.add_argument("--test", action="store_true", help="run the spec assertions (exit 1 on a failure)")
    ap.add_argument("--png", default=None, help="silhouette PNG for a user plate (offline only)")
    ap.add_argument("--name", default="USER", help="the user plate's name (<= 11 chars)")
    ap.add_argument("--replace", default=None, help="slot to replace: index or name (not DRUM)")
    a = ap.parse_args(argv)
    np.seterr(divide="ignore", over="ignore", invalid="ignore")   # numpy 1.26 + Accelerate flags spurious FP errors in
    shapes = [list(s) for s in SHAPES]                            # matmul; every table is checked finite instead
    if a.png:
        if a.replace is None:
            raise SystemExit("gen_plates: --png needs --replace SHAPE")
        slot = int(a.replace) if a.replace.isdigit() else [s[0] for s in shapes].index(a.replace.upper())
        if slot == 0:
            raise SystemExit("gen_plates: DRUM is the tracking root (clamped); replace a free slot")
        shapes[slot] = [a.name.upper()[:11], False, load_png(a.png)]
    with open(os.path.abspath(__file__), "rb") as f:
        sha = hashlib.sha1(f.read()).hexdigest()
    print(f"gen_plates {sha[:12]}: solving {len(shapes)} shapes + {3 * (len(shapes) - 1)} in-between operators")
    kfs, track = build(shapes)
    for k in kfs:
        order = np.argsort(k.log2_lambda, kind="stable")
        print(f"  {k.name:>8}: membrane ratios " + " ".join(f"{np.sqrt(2.0 ** k.log2_lambda[i]):.3f}"
                                                              for i in order[:10]))
    means = [m for e in track for _, m, _ in e]
    print(f"  tracking: mean |overlap| {np.mean(means):.3f}, worst step {min(means):.3f}")
    if a.out:
        write_c(a.out, kfs, track, sha)
        print(f"  wrote {a.out} ({os.path.getsize(a.out):,} B of C, {len(kfs) * PLATE_BYTES:,} B of data)")
    if a.sheet:
        write_sheet(a.sheet, kfs, track)
        print(f"  wrote {a.sheet}")
    if a.test:
        fails = run_tests(kfs, track)
        if fails:
            print(f"gen_plates: {len(fails)} test(s) FAILED")
            return 1
        print("gen_plates: all tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
