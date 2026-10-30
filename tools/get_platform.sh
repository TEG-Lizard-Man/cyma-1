#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The Team EG platform that CYMA-1 is built on from v1.1.1: upstream fm1-base and felucca-bsp at their pinned
# commits, with the patches in platform/patches applied on top. The result is a git checkout, platform/fm1-base-teg
# (git-ignored), which tools/build_device.sh and instrument/tools/run_all.sh use by default.
#
#   tools/get_platform.sh [DEST]          DEST defaults to platform/fm1-base-teg; it must be inside this repository
#                                         for a device build (only the repository is mounted in the toolchain container)
#
# 1. The pins. reference/fm1-base, this repository's git submodule, pins upstream fm1-base 086cd0c, and fm1-base's own
#    bsp submodule pins felucca-bsp b413cd8. The script runs `git submodule update --init --recursive` for it: a
#    download from github.com/charlesvestal/fm1-base and github.com/hugelton/felucca-bsp the first time. In a copy
#    without git history (a downloaded zip), it fetches the same two commits from those URLs instead.
# 2. The patches. It copies both repositories to DEST, then applies them with `git am`:
#      platform/patches/fm1-base/*.patch     in DEST      (the bsp bump, the MASTER seed, TEG_FORK.md)
#      platform/patches/felucca-bsp/*.patch  in DEST/bsp  (usb.c: the USB retry in two steps)
#    The commits get the patches' own author, dates and messages, so their ids come out as pinned below.
# 3. The check. The files must match the pinned trees exactly, or the script fails. The commit ids are checked too,
#    but a git that writes commits differently only gets a warning, since the files are what the build reads.
#
# Running it again replaces DEST. It builds nothing, flashes nothing and never touches USB or MIDI.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
DEST=${1:-$ROOT/platform/fm1-base-teg}
PATCHES=$ROOT/platform/patches
FM1_BASE_URL=https://github.com/charlesvestal/fm1-base
BSP_URL=https://github.com/hugelton/felucca-bsp

# upstream
FM1_BASE_PIN=086cd0c6fcc1479f81c54b3929095ed81f87c6a7    # fm1-base: bsp -> b413cd8 (felucca-bsp#6)
BSP_PIN=b413cd888424249d417d5549b594e8492d399d3e         # felucca-bsp: Merge pull request #6
# after the patches: the commits, and the trees (the files)
TEG_BSP=7f4693af3a469620fbc219c0f40cb74eb4134620
TEG_BSP_TREE=68fa89838b6dc1f43e634887a7602bf465a2ca20
TEG_FM1=a0f2af24e62fc501aa1b57b7962c74db04f2563c
TEG_FM1_TREE=d8330ad7e03e5b8e2a8379b83abebc8643aa47d8

ID_NAME=TEG-Lizard-Man
ID_MAIL=349983952+TEG-Lizard-Man@users.noreply.github.com

die() { echo "get_platform: $*" >&2; exit 1; }
command -v git >/dev/null 2>&1 || die "git not found"
ls "$PATCHES"/felucca-bsp/*.patch "$PATCHES"/fm1-base/*.patch >/dev/null 2>&1 || die "no patches in $PATCHES"
case "$DEST" in /*) ;; *) DEST=$PWD/$DEST ;; esac

# ---- 1. the upstream pins
if [ "$(git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null)" = "$ROOT" ] && [ -f "$ROOT/.gitmodules" ]; then
    echo "get_platform: reference/fm1-base (git submodule update --init --recursive)"
    git -C "$ROOT" submodule update --init --recursive reference/fm1-base
    UP=$ROOT/reference/fm1-base
    [ "$(git -C "$UP" rev-parse HEAD)" = "$FM1_BASE_PIN" ] ||
        die "reference/fm1-base is at $(git -C "$UP" rev-parse --short HEAD), not 086cd0c: run  git submodule update --init --recursive"
    [ "$(git -C "$UP/bsp" rev-parse HEAD)" = "$BSP_PIN" ] ||
        die "reference/fm1-base/bsp is at $(git -C "$UP/bsp" rev-parse --short HEAD), not b413cd8: run  git submodule update --init --recursive"
    SRC_FM1=$UP
    SRC_BSP=$UP/bsp
else
    echo "get_platform: no git checkout here: fetching the pinned commits from $FM1_BASE_URL and $BSP_URL"
    SRC_FM1=$FM1_BASE_URL
    SRC_BSP=$BSP_URL
fi

# ---- 2. a fresh copy, patched: fm1-base first, while its bsp is still empty (a gitlink change to a populated
# submodule makes git apply warn), then felucca-bsp into it
am() {   # REPO PATCH...: git am with the patches' own identity and dates, so the commit ids are reproducible
    r=$1; shift
    GIT_COMMITTER_NAME=$ID_NAME GIT_COMMITTER_EMAIL=$ID_MAIL \
        git -C "$r" -c commit.gpgsign=false -c am.keepcr=false -c am.threeWay=false -c am.messageid=false \
        -c core.autocrlf=false am -q --committer-date-is-author-date "$@" ||
        die "a patch did not apply in $r (git -C \"$r\" am --abort to inspect)"
}
fetch() {   # SRC DIR PIN: DIR is a clone of SRC with PIN checked out on branch teg/platform
    git -c init.defaultBranch=main clone -q --no-checkout "$1" "$2"
    git -C "$2" cat-file -e "$3^{commit}" 2>/dev/null || git -C "$2" fetch -q origin "$3"
    git -C "$2" checkout -q -b teg/platform "$3"
}
rm -rf "$DEST"
mkdir -p "$(dirname "$DEST")"
fetch "$SRC_FM1" "$DEST" "$FM1_BASE_PIN"
am "$DEST" "$PATCHES"/fm1-base/*.patch
rmdir "$DEST/bsp"
fetch "$SRC_BSP" "$DEST/bsp" "$BSP_PIN"
am "$DEST/bsp" "$PATCHES"/felucca-bsp/*.patch
# the remotes name upstream, not the local copies they were cloned from
git -C "$DEST" remote set-url origin "$FM1_BASE_URL"
git -C "$DEST/bsp" remote set-url origin "$BSP_URL"
git -C "$DEST" config submodule.bsp.url "$BSP_URL"
git -C "$DEST" config submodule.bsp.active true

# ---- 3. the check
[ "$(git -C "$DEST/bsp" rev-parse 'HEAD^{tree}')" = "$TEG_BSP_TREE" ] ||
    die "bsp's files are not the pinned ones (tree $(git -C "$DEST/bsp" rev-parse 'HEAD^{tree}'), expected $TEG_BSP_TREE)"
[ "$(git -C "$DEST" rev-parse 'HEAD^{tree}')" = "$TEG_FM1_TREE" ] ||
    die "fm1-base's files are not the pinned ones (tree $(git -C "$DEST" rev-parse 'HEAD^{tree}'), expected $TEG_FM1_TREE)"
gb=$(git -C "$DEST/bsp" rev-parse HEAD)
gf=$(git -C "$DEST" rev-parse HEAD)
if [ "$gb" != "$TEG_BSP" ] || [ "$gf" != "$TEG_FM1" ]; then
    echo "get_platform: warning: the files are the pinned ones, but this git wrote other commit ids" \
         "(bsp $gb, fm1-base $gf; pinned $TEG_BSP, $TEG_FM1). The build reads only the files." >&2
fi
[ -f "$DEST/tools/build.py" ] && [ -f "$DEST/bsp/chip/crt0.S" ] || die "$DEST is incomplete"

echo "get_platform: $DEST"
echo "  fm1-base   086cd0c + $(ls "$PATCHES"/fm1-base/*.patch | wc -l | tr -d ' ') patches = $(git -C "$DEST" rev-parse --short HEAD) (branch teg/platform)"
echo "  felucca-bsp b413cd8 + $(ls "$PATCHES"/felucca-bsp/*.patch | wc -l | tr -d ' ') patch   = $(git -C "$DEST/bsp" rev-parse --short HEAD)"
echo "  files match the pinned trees ($(echo "$TEG_FM1_TREE" | cut -c1-7), bsp $(echo "$TEG_BSP_TREE" | cut -c1-7))"
