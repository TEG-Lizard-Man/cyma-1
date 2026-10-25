/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 UI (M5): drawing on top of gfx.c. Part of the unity build: included by ui/ui.c, itself included by app.c
 * after gfx.c, so swap16, glyph(), felucca_font_t and cv_px are in scope.
 *
 * A surface (sf_t) is a buffer that holds one rectangle of the screen: a 184 x 40 plate strip, a 60 x 36 knob cell,
 * the 28 x 184 ladder. Everything draws in SCREEN coordinates and the surface clips, so one overlay (a pill, a
 * ripple, a tour chevron) drawn into every surface it crosses comes out seamless across strip and rail edges with no
 * bookkeeping. Pixels are the panel's byte-swapped RGB565, as gfx.c keeps them.
 *
 * Alpha is 0..32 (5 bits) for everything drawn over a surface: the RGB565 blend below does all three channels in
 * one 32-bit multiply (fields spread apart by 0x07E0F81F), and 32 levels are plenty for anti-aliased edges. The
 * plate's own layers (surface, sand) blend in RGB888 and dither instead (plate.c, sand.c). */

typedef struct {
    uint16_t *px;
    int32_t x0, y0, w, h;                /* px[0] is screen (x0, y0); w x h pixels */
    int32_t cx0, cy0, cx1, cy1;          /* the clip in screen coordinates (max exclusive) */
} sf_t;

static void sf_init(sf_t *s, uint16_t *px, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    s->px = px;
    s->x0 = s->cx0 = x0;
    s->y0 = s->cy0 = y0;
    s->w = w;
    s->h = h;
    s->cx1 = x0 + w;
    s->cy1 = y0 + h;
}

/* does the rectangle [x0, x1) x [y0, y1) touch the surface's clip? (an overlay skips itself cheaply) */
static int sf_hits(const sf_t *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    return x1 > s->cx0 && x0 < s->cx1 && y1 > s->cy0 && y0 < s->cy1;
}

/* RGB565 blend: dsw a byte-swapped pixel, c an unswapped colour, a 0..32; returns byte-swapped */
static inline uint16_t sf_mix(uint16_t dsw, uint32_t c, uint32_t a)
{
    uint32_t d = (uint32_t)((dsw >> 8) | ((dsw & 0xFFu) << 8));
    uint32_t bg = (d | d << 16) & 0x07E0F81Fu, fg = (c | c << 16) & 0x07E0F81Fu, r;
    r = ((((fg - bg) * a) >> 5) + bg) & 0x07E0F81Fu;
    r |= r >> 16;
    return (uint16_t)(((r >> 8) & 0xFFu) | ((r & 0xFFu) << 8));
}

static inline void sf_px(sf_t *s, int32_t x, int32_t y, uint32_t c, uint32_t a)
{
    uint16_t *p;
    if (x < s->cx0 || x >= s->cx1 || y < s->cy0 || y >= s->cy1 || !a)
        return;
    p = &s->px[(y - s->y0) * s->w + (x - s->x0)];
    *p = a >= 32u ? swap16(c) : sf_mix(*p, c, a);
}

/* coverage 0..1 times alpha 0..32, rounded */
static inline uint32_t sf_cov(float cov, uint32_t a)
{
    return cov <= 0.0f ? 0u : cov >= 1.0f ? a : (uint32_t)(cov * (float)a + 0.5f);
}

static void sf_rect(sf_t *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c, uint32_t a)
{
    int32_t x1 = x + w, y1 = y + h, i, j;
    uint16_t sc = swap16(c);
    if (x < s->cx0) x = s->cx0;
    if (y < s->cy0) y = s->cy0;
    if (x1 > s->cx1) x1 = s->cx1;
    if (y1 > s->cy1) y1 = s->cy1;
    if (!a)
        return;
    for (j = y; j < y1; j++) {
        uint16_t *p = &s->px[(j - s->y0) * s->w + (x - s->x0)];
        if (a >= 32u)
            for (i = x; i < x1; i++)
                *p++ = sc;
        else
            for (i = x; i < x1; i++, p++)
                *p = sf_mix(*p, c, a);
    }
}

/* a filled disc, anti-aliased, centre and radius in pixels (pixel (x, y) is the square [x, x+1) x [y, y+1)) */
static void sf_disc(sf_t *s, float cx, float cy, float r, uint32_t c, uint32_t a)
{
    int32_t x, y, xa = (int32_t)fm_floorf(cx - r - 1.0f), xb = (int32_t)(cx + r + 1.0f);
    int32_t ya = (int32_t)fm_floorf(cy - r - 1.0f), yb = (int32_t)(cy + r + 1.0f);
    if (!sf_hits(s, xa, ya, xb + 1, yb + 1))
        return;
    for (y = ya; y <= yb; y++)
        for (x = xa; x <= xb; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            sf_px(s, x, y, c, sf_cov(r + 0.5f - fm_sqrtf(dx * dx + dy * dy), a));
        }
}

/* a ring of width wd about radius r, anti-aliased; walks only the annulus (a 40 px ripple touches ~500 pixels, not
 * its 6,700-pixel box) */
static void sf_ring(sf_t *s, float cx, float cy, float r, float wd, uint32_t c, uint32_t a)
{
    float ro = r + 0.5f * wd + 1.0f, ri = r - 0.5f * wd - 1.0f, hw = 0.5f * wd + 0.5f;
    int32_t y, ya = (int32_t)fm_floorf(cy - ro), yb = (int32_t)(cy + ro);
    if (!sf_hits(s, (int32_t)(cx - ro) - 1, ya, (int32_t)(cx + ro) + 2, yb + 1))
        return;
    if (ya < s->cy0) ya = s->cy0;
    if (yb >= s->cy1) yb = s->cy1 - 1;
    for (y = ya; y <= yb; y++) {
        float dy = (float)y + 0.5f - cy, o2 = ro * ro - dy * dy, i2 = ri * ri - dy * dy, xo, xi;
        int32_t side;
        if (o2 <= 0.0f)
            continue;
        xo = fm_sqrtf(o2);
        xi = (ri > 0.0f && i2 > 0.0f) ? fm_sqrtf(i2) : 0.0f;
        for (side = 0; side < 2; side++) {    /* the left span, then the right one (one span when no hole) */
            int32_t x, x0, x1;
            if (xi <= 0.0f) {
                if (side)
                    break;
                x0 = (int32_t)fm_floorf(cx - xo), x1 = (int32_t)(cx + xo);
            } else if (!side) {
                x0 = (int32_t)fm_floorf(cx - xo), x1 = (int32_t)(cx - xi);
            } else {
                x0 = (int32_t)fm_floorf(cx + xi), x1 = (int32_t)(cx + xo);
            }
            for (x = x0; x <= x1; x++) {
                float dx = (float)x + 0.5f - cx, d = fm_sqrtf(dx * dx + dy * dy) - r;
                sf_px(s, x, y, c, sf_cov(hw - fm_fabsf(d), a));
            }
        }
    }
}

/* Xiaolin Wu's line: one pixel wide, anti-aliased across, alpha a */
static void sf_line(sf_t *s, float x0, float y0, float x1, float y1, uint32_t c, uint32_t a)
{
    int steep = fm_fabsf(y1 - y0) > fm_fabsf(x1 - x0);
    float t, grad, y;
    int32_t x, xs, xe;
    if (steep) {
        t = x0, x0 = y0, y0 = t;
        t = x1, x1 = y1, y1 = t;
    }
    if (x0 > x1) {
        t = x0, x0 = x1, x1 = t;
        t = y0, y0 = y1, y1 = t;
    }
    grad = x1 - x0 > 1e-6f ? (y1 - y0) / (x1 - x0) : 0.0f;
    xs = (int32_t)fm_floorf(x0 + 0.5f);
    xe = (int32_t)fm_floorf(x1 + 0.5f);
    y = y0 + grad * ((float)xs + 0.5f - x0) - 0.5f;   /* centre of the line at the pixel centre, minus half a pixel */
    for (x = xs; x <= xe; x++, y += grad) {
        int32_t yi = (int32_t)fm_floorf(y);
        float f = y - (float)yi;
        if (steep) {
            sf_px(s, yi, x, c, sf_cov(1.0f - f, a));
            sf_px(s, yi + 1, x, c, sf_cov(f, a));
        } else {
            sf_px(s, x, yi, c, sf_cov(1.0f - f, a));
            sf_px(s, x, yi + 1, c, sf_cov(f, a));
        }
    }
}

/* a capsule: the segment (x0, y0)-(x1, y1) thickened to radius rad, anti-aliased (the bow mark, a mallet's stick) */
static void sf_capsule(sf_t *s, float x0, float y0, float x1, float y1, float rad, uint32_t c, uint32_t a)
{
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy, il = l2 > 1e-9f ? 1.0f / l2 : 0.0f;
    int32_t x, y, xa = (int32_t)fm_floorf(fm_minf(x0, x1) - rad - 1.0f), xb = (int32_t)(fm_maxf(x0, x1) + rad + 1.0f);
    int32_t ya = (int32_t)fm_floorf(fm_minf(y0, y1) - rad - 1.0f), yb = (int32_t)(fm_maxf(y0, y1) + rad + 1.0f);
    if (!sf_hits(s, xa, ya, xb + 1, yb + 1))
        return;
    for (y = ya; y <= yb; y++)
        for (x = xa; x <= xb; x++) {
            float px = (float)x + 0.5f - x0, py = (float)y + 0.5f - y0;
            float t = fm_clampf((px * dx + py * dy) * il, 0.0f, 1.0f), ex = px - t * dx, ey = py - t * dy;
            sf_px(s, x, y, c, sf_cov(rad + 0.5f - fm_sqrtf(ex * ex + ey * ey), a));
        }
}

/* a filled triangle, anti-aliased by the distance to its nearest edge (chevrons, drops) */
static void sf_tri(sf_t *s, float ax, float ay, float bx, float by, float cx, float cy, uint32_t c, uint32_t a)
{
    float P[3][2] = {{ax, ay}, {bx, by}, {cx, cy}}, N[3][3];
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    int32_t x, y, k, xa = (int32_t)fm_floorf(fm_minf(ax, fm_minf(bx, cx))) - 1, xb = (int32_t)(fm_maxf(ax, fm_maxf(bx, cx))) + 1;
    int32_t ya = (int32_t)fm_floorf(fm_minf(ay, fm_minf(by, cy))) - 1, yb = (int32_t)(fm_maxf(ay, fm_maxf(by, cy))) + 1;
    if (!sf_hits(s, xa, ya, xb + 1, yb + 1) || fm_fabsf(area) < 1e-6f)
        return;
    for (k = 0; k < 3; k++) {                    /* inward unit normals: n . p + d >= 0 inside */
        float ex = P[(k + 1) % 3][0] - P[k][0], ey = P[(k + 1) % 3][1] - P[k][1], il = fm_rsqrtf(ex * ex + ey * ey);
        float nx = -ey * il, ny = ex * il;
        if (area < 0.0f)
            nx = -nx, ny = -ny;
        N[k][0] = nx, N[k][1] = ny, N[k][2] = -(nx * P[k][0] + ny * P[k][1]);
    }
    for (y = ya; y <= yb; y++)
        for (x = xa; x <= xb; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f, d = 1e9f;
            for (k = 0; k < 3; k++)
                d = fm_minf(d, N[k][0] * px + N[k][1] * py + N[k][2]);
            sf_px(s, x, y, c, sf_cov(d + 0.5f, a));
        }
}

/* a rounded rectangle at integer position and size, corner radius r, anti-aliased corners; the pills */
static void sf_rrect(sf_t *s, int32_t x, int32_t y, int32_t w, int32_t h, float r, uint32_t c, uint32_t a)
{
    int32_t j, i;
    float rr = fm_minf(r, 0.5f * (float)(h < w ? h : w));
    int32_t ri = (int32_t)(rr + 0.999f);
    if (!sf_hits(s, x, y, x + w, y + h) || !a)
        return;
    for (j = y; j < y + h; j++) {
        float fy = (float)j + 0.5f, dy = 0.0f;
        if (j < s->cy0 || j >= s->cy1)
            continue;
        if (fy < (float)y + rr)
            dy = (float)y + rr - fy;
        else if (fy > (float)(y + h) - rr)
            dy = fy - ((float)(y + h) - rr);
        if (dy <= 0.0f) {                         /* a straight row: solid */
            sf_rect(s, x, j, w, 1, c, a);
            continue;
        }
        for (i = 0; i < ri && i < w; i++) {       /* the two corner runs, by the distance to the corner centres */
            float dx = (float)x + rr - ((float)(x + i) + 0.5f);
            uint32_t aa = sf_cov(rr + 0.5f - fm_sqrtf(dx * dx + dy * dy), a);
            sf_px(s, x + i, j, c, aa);
            sf_px(s, x + w - 1 - i, j, c, aa);
        }
        if (w > 2 * ri)
            sf_rect(s, x + ri, j, w - 2 * ri, 1, c, sf_cov(rr + 0.5f - dy, a));
    }
}

/* a horizontal bar from x0, len pixels long (fractional end anti-aliased), centred on row yc (fractional: split over
 * two rows) and th rows thick: the ladder's partials sit at their exact ratio, not the nearest row */
static void sf_hbar(sf_t *s, int32_t x0, float len, float yc, float th, uint32_t c, uint32_t a)
{
    float top = yc - 0.5f * th, bot = yc + 0.5f * th;
    int32_t y, x, n = (int32_t)len;
    float fr = len - (float)n;
    for (y = (int32_t)fm_floorf(top); (float)y < bot; y++) {
        float cv = fm_minf(bot, (float)y + 1.0f) - fm_maxf(top, (float)y);
        uint32_t av = sf_cov(cv, a);
        for (x = 0; x < n; x++)
            sf_px(s, x0 + x, y, c, av);
        sf_px(s, x0 + n, y, c, sf_cov(cv * fr, a));
    }
}

/* text: a felucca_font_t face, tracking px between glyphs, colour c at alpha a (0..32), each glyph pixel's 4-bit
 * coverage blended over what is there; returns the pen's end x. Clipped to the surface row by row, so a pill's
 * text spans plate strips. */
static int32_t sf_text(sf_t *s, int32_t x, int32_t y, const felucca_font_t *f, const char *str, uint32_t c, uint32_t a,
                       int32_t track)
{
    uint16_t sc = swap16(c);
    for (; *str; str++) {
        uint32_t gi = glyph(f, (uint8_t)*str), w = f->bw[gi], bpr = (w + 1u) / 2u, gx, gy;
        const uint8_t *gd = f->data + f->off[gi];
        int32_t gx0 = x - f->pad;
        if (w && sf_hits(s, gx0, y, gx0 + (int32_t)w, y + f->h))
            for (gy = 0; gy < f->h; gy++) {
                int32_t py = y + (int32_t)gy;
                uint16_t *row;
                if (py < s->cy0 || py >= s->cy1)
                    continue;
                row = &s->px[(py - s->y0) * s->w];
                for (gx = 0; gx < w; gx++) {
                    uint32_t v = gd[gy * bpr + gx / 2u];
                    int32_t px = gx0 + (int32_t)gx;
                    v = (gx & 1u) ? (v & 15u) : (v >> 4);
                    if (!v || px < s->cx0 || px >= s->cx1)
                        continue;
                    v = (v * a * 275u) >> 12;     /* 4-bit coverage x alpha/32 -> 0..32 */
                    row[px - s->x0] = v >= 32u ? sc : sf_mix(row[px - s->x0], c, v);
                }
            }
        x += f->adv[gi] + track;
    }
    return x;
}

static int32_t sf_text_w(const felucca_font_t *f, const char *str, int32_t track)
{
    int32_t w = 0, n = 0;
    for (; *str; str++, n++)
        w += f->adv[glyph(f, (uint8_t)*str)];
    return w + (n > 1 ? (n - 1) * track : 0);
}

/* ---- small string helpers (no libc on the device) ---- */
static uint32_t sf_strlen(const char *s)
{
    uint32_t n = 0;
    while (s && s[n])
        n++;
    return n;
}

static void sf_strcpy(char *d, const char *s, uint32_t max)
{
    uint32_t n = 0;
    while (s && s[n] && n + 1u < max) {
        d[n] = s[n];
        n++;
    }
    d[n] = 0;
}

static void sf_strcat(char *d, const char *s, uint32_t max)
{
    uint32_t n = sf_strlen(d);
    while (s && *s && n + 1u < max)
        d[n++] = *s++;
    d[n] = 0;
}

/* an unsigned integer, zero-padded to at least pad digits */
static void sf_utoa(char *d, uint32_t v, uint32_t pad)
{
    char t[12];
    uint32_t n = 0, i;
    do {
        uint32_t q = v / 10u;
        t[n++] = (char)('0' + (v - q * 10u));
        v = q;
    } while ((v || n < pad) && n < 11u);
    for (i = 0; i < n; i++)
        d[i] = t[n - 1u - i];
    d[n] = 0;
}

/* a cheap byte hash for change detection (FNV-1a) */
static uint32_t sf_hash(uint32_t h, const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--)
        h = (h ^ *b++) * 0x01000193u;
    return h;
}

/* a string's hash, up to its terminator (never the stale or uninitialised bytes after it: they would redraw a
 * screen region for nothing) */
static uint32_t sf_hash_s(uint32_t h, const char *s)
{
    return sf_hash(h, s, sf_strlen(s) + 1u);
}
