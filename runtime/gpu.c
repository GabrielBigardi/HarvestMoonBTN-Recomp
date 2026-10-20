/*
 * gpu.c - PlayStation GPU (CXD8561, "v2") reimplementation.
 *
 *   - GP0 command FIFO / parameter assembly, polylines, VRAM transfers
 *   - GP1 display control, GPUSTAT, GPUREAD (incl. GP1(10h) info latch)
 *   - exact software rasterizer drawing into the 1024x512 16-bit VRAM
 *
 * Written from the psx-spx GPU documentation. The rasterization rules are
 * the ones of the real hardware:
 *
 *   Polygons: triangles are sorted by Y; edges are walked in 32.32 fixed
 *   point (start fraction = 1 - 2^-21, slopes rounded away from zero), so a
 *   pixel (x,y) is covered when the integer sample point (x,y) lies inside
 *   the triangle with a top-left fill convention (left/top edges inclusive,
 *   right/bottom exclusive).  Colour/UV interpolants are set up from the
 *   vertex deltas with 12 fractional bits (truncating division), stored as
 *   8.24 values starting at the "core" (left-most) vertex plus 0.5, and
 *   evaluated per pixel with wrapping 32-bit arithmetic.  Quads are drawn as
 *   triangles (v0,v1,v2) and (v1,v2,v3).
 *
 *   Lines: DDA in 32.32 fixed point over max(|dx|,|dy|)+1 pixels, both end
 *   points inclusive, colour in 20.12.
 *
 *   Rectangles: no dithering, texture coordinates stepped per pixel (8-bit
 *   wrap, then texture window), optional X/Y flip (GP0(E1h) bits 12/13).
 *
 * All commands complete instantly. All state, including VRAM, lives in one
 * static struct so it can be saved/restored with memcpy.
 */
#include "gpu.h"

#include <stdlib.h>
#include <string.h>

#define VRAM_W 1024
#define VRAM_H 512

/* Emulate the GPU's CLUT cache: the palette is only (re)loaded from VRAM when
 * a textured primitive uses a different CLUT/colour-depth than the previous
 * one, or after GP0(01h)/GP1(00h)/GP1(01h). Set to 0 to always reload. */
#ifndef GPU_EMULATE_CLUT_CACHE
#define GPU_EMULATE_CLUT_CACHE 1
#endif

#define CLUT_TAG_INVALID 0xFFFFFFFFu

enum {
    GP0_STATE_COMMAND = 0,   /* accumulating command words */
    GP0_STATE_VRAM_WRITE,    /* GP0(A0h) data words */
    GP0_STATE_POLYLINE       /* GP0(48h..5Fh) further vertices */
};

typedef struct {
    int32_t x, y;
    int32_t r, g, b;
} LinePt;

typedef struct {
    uint16_t vram[VRAM_W * VRAM_H];

    /* ---- GP0 command assembly ---- */
    uint32_t cmd[16];
    int32_t  cmd_count;
    int32_t  cmd_need;
    int32_t  gp0_state;

    /* polyline continuation */
    uint32_t pl_op;
    uint32_t pl_words[2];
    int32_t  pl_count;
    LinePt   pl_prev;

    /* CPU -> VRAM transfer */
    int32_t  wr_x, wr_y, wr_w, wr_h, wr_cx, wr_cy;
    /* VRAM -> CPU transfer */
    int32_t  rd_active;
    int32_t  rd_x, rd_y, rd_w, rd_h, rd_cx, rd_cy;
    uint32_t read_latch;

    /* ---- GP0(E1h) draw mode ---- */
    uint32_t tp_x;          /* texture page base X in halfwords (0..960) */
    uint32_t tp_y;          /* texture page base Y in lines (0/256) */
    uint32_t abr;           /* semi-transparency mode */
    uint32_t tex_mode;      /* 0=4bpp 1=8bpp 2,3=15bpp */
    uint32_t dither;
    uint32_t dfe;           /* drawing to display area allowed */
    uint32_t tex_disable;
    uint32_t flip_x, flip_y;

    /* ---- GP0(E2h) texture window ---- */
    uint32_t tww, twh, twx, twy;
    uint32_t tw_and_u, tw_or_u, tw_and_v, tw_or_v;

    /* ---- GP0(E3h..E5h) ---- */
    int32_t  clip_x0, clip_y0, clip_x1, clip_y1;
    int32_t  offs_x, offs_y;

    /* ---- GP0(E6h) ---- */
    uint32_t mask_set;      /* 0 or 0x8000 */
    uint32_t mask_check;    /* 0 or 0x8000 */

    /* ---- CLUT cache ---- */
    uint32_t clut_tag;
    uint16_t clut[256];

    /* ---- GP1 ---- */
    uint32_t irq;
    uint32_t display_off;
    uint32_t dma_dir;
    uint32_t disp_x, disp_y;
    uint32_t h_start, h_end;
    uint32_t v_start, v_end;
    uint32_t disp_mode;     /* GP1(08h) bits 0-7 */
    uint32_t tex_disable_allow;

    /* ---- timing ---- */
    uint32_t field;         /* interlace field currently displayed */
    uint32_t line_parity;   /* GPUSTAT.31 for non-interlaced modes */
} GpuState;

static GpuState S;

/* dither_lut[y&3][x&3][v]: (v + offset) / 8 saturated to 0..31, v = 0..511 */
static uint8_t dither_lut[4][4][512];
static int dither_lut_ready;

static const int dither_matrix[4][4] = {
    { -4,  0, -3,  1 },
    {  2, -2,  3, -1 },
    { -3,  1, -4,  0 },
    {  3, -1,  2, -2 },
};

static void build_dither_lut(void)
{
    int y, x, v;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            for (v = 0; v < 512; v++) {
                int t = v + dither_matrix[y][x];
                if (t < 0) t = 0;
                t >>= 3;
                if (t > 31) t = 31;
                dither_lut[y][x][v] = (uint8_t)t;
            }
    dither_lut_ready = 1;
}

/* LUT row used when dithering is off: offset 0 */
#define NO_DITHER_LUT (dither_lut[2][3])

/* ======================================================================= */
/* Helpers                                                                  */
/* ======================================================================= */

static inline int32_t sext11(uint32_t v)
{
    return (int32_t)((v & 0x7FFu) ^ 0x400u) - 0x400;
}

static inline int32_t fp_int(int64_t v)
{
    return (int32_t)(v >> 32);
}

static void recalc_texwin(void)
{
    S.tw_and_u = ~(S.tww << 3) & 0xFFu;
    S.tw_or_u  = (S.twx & S.tww) << 3;
    S.tw_and_v = ~(S.twh << 3) & 0xFFu;
    S.tw_or_v  = (S.twy & S.twh) << 3;
}

/* Texture page attribute (GP0(E1h) or the polygon "page" field). */
static void set_texpage(uint32_t v, int full_e1)
{
    S.tp_x     = (v & 0xFu) * 64u;
    S.tp_y     = (v & 0x10u) ? 256u : 0u;
    S.abr      = (v >> 5) & 3u;
    S.tex_mode = (v >> 7) & 3u;
    if (S.tex_disable_allow)
        S.tex_disable = (v >> 11) & 1u;
    if (full_e1) {
        S.dither = (v >> 9) & 1u;
        S.dfe    = (v >> 10) & 1u;
        S.flip_x = (v >> 12) & 1u;
        S.flip_y = (v >> 13) & 1u;
    }
}

static void clut_invalidate(void)
{
    S.clut_tag = CLUT_TAG_INVALID;
}

static void clut_update(uint32_t raw_clut)
{
    uint32_t mode = S.tex_mode;
    uint32_t cx, cy, n, i;
    const uint16_t *row;

    if (mode >= 2)
        return;
#if GPU_EMULATE_CLUT_CACHE
    {
        uint32_t tag = (raw_clut & 0x7FFFu) | (mode << 16);
        if (tag == S.clut_tag)
            return;
        S.clut_tag = tag;
    }
#endif
    cx = (raw_clut & 0x3Fu) << 4;
    cy = (raw_clut >> 6) & 0x1FFu;
    n = mode ? 256u : 16u;
    row = &S.vram[cy << 10];
    for (i = 0; i < n; i++)
        S.clut[i] = row[(cx + i) & 1023u];
}

/* Fetch a texel (texture window + page + CLUT applied). 0 = transparent. */
static inline uint32_t tex_fetch(uint32_t u, uint32_t v)
{
    uint32_t tu = (u & S.tw_and_u) | S.tw_or_u;
    uint32_t tv = (v & S.tw_and_v) | S.tw_or_v;
    const uint16_t *row = &S.vram[((S.tp_y + tv) & 511u) << 10];
    uint32_t w;

    switch (S.tex_mode) {
    case 0:
        w = row[(S.tp_x + (tu >> 2)) & 1023u];
        return S.clut[(w >> ((tu & 3u) << 2)) & 0xFu];
    case 1:
        w = row[(S.tp_x + (tu >> 1)) & 1023u];
        return S.clut[(w >> ((tu & 1u) << 3)) & 0xFFu];
    default:
        return row[(S.tp_x + tu) & 1023u];
    }
}

/* Texture colour modulation: (texel5 * colour8) >> 4, then dither/saturate. */
static inline uint32_t modulate(uint32_t t, uint32_t r, uint32_t g, uint32_t b,
                                const uint8_t *lut)
{
    return (uint32_t)lut[((t & 0x1Fu) * r) >> 4]
         | ((uint32_t)lut[(((t >> 5) & 0x1Fu) * g) >> 4] << 5)
         | ((uint32_t)lut[(((t >> 10) & 0x1Fu) * b) >> 4] << 10);
}

static inline uint32_t blend_px(uint32_t bk, uint32_t fr, int mode)
{
    int32_t br = (int32_t)(bk & 31u), bg = (int32_t)((bk >> 5) & 31u), bb = (int32_t)((bk >> 10) & 31u);
    int32_t fr_ = (int32_t)(fr & 31u), fg = (int32_t)((fr >> 5) & 31u), fb = (int32_t)((fr >> 10) & 31u);
    int32_t r, g, b;

    switch (mode) {
    case 0: /* B/2 + F/2 */
        r = (br + fr_) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1;
        break;
    case 1: /* B + F */
        r = br + fr_; g = bg + fg; b = bb + fb;
        if (r > 31) r = 31;
        if (g > 31) g = 31;
        if (b > 31) b = 31;
        break;
    case 2: /* B - F */
        r = br - fr_; g = bg - fg; b = bb - fb;
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (b < 0) b = 0;
        break;
    default: /* B + F/4 */
        r = br + (fr_ >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2);
        if (r > 31) r = 31;
        if (g > 31) g = 31;
        if (b > 31) b = 31;
        break;
    }
    return (uint32_t)r | ((uint32_t)g << 5) | ((uint32_t)b << 10);
}

/* Write one pixel. rgb = 15-bit colour, bit15 = value of bit 15 for the
 * written pixel (texel bit 15, or 0 for untextured), blend = -1 or mode. */
static inline void plot(uint32_t x, uint32_t y, uint32_t rgb, uint32_t bit15, int blend)
{
    uint16_t *p = &S.vram[((y & 511u) << 10) | (x & 1023u)];
    uint32_t bk = *p;
    if (bk & S.mask_check)
        return;
    if (blend >= 0)
        rgb = blend_px(bk, rgb, blend);
    *p = (uint16_t)(rgb | bit15 | S.mask_set);
}

/* 480i with drawing to the displayed field disabled: lines of the field
 * currently being displayed are not drawn. */
static inline int line_skip(int32_t y)
{
    if ((S.disp_mode & 0x24u) != 0x24u || S.dfe)
        return 0;
    return ((uint32_t)y & 1u) == S.field;
}

/* ======================================================================= */
/* Polygons                                                                 */
/* ======================================================================= */

typedef struct {
    int32_t x, y;
    int32_t r, g, b;
    int32_t u, v;
} TriVtx;

typedef struct {
    uint32_t r, g, b, u, v;
} Interp;

typedef struct {
    Interp dx, dy;
} InterpDelta;

typedef struct {
    int gouraud;
    int textured;
    int raw;            /* raw texture (no modulation) */
    int dither;         /* dithering applies to this primitive */
    int blend;          /* -1 = opaque, else semi-transparency mode */
    uint32_t flat_r, flat_g, flat_b;
    uint32_t flat_rgb15;
} PolyCtx;

static inline void interp_add(Interp *ig, const Interp *d, uint32_t count)
{
    ig->r += d->r * count;
    ig->g += d->g * count;
    ig->b += d->b * count;
    ig->u += d->u * count;
    ig->v += d->v * count;
}

static void draw_span(const PolyCtx *c, int32_t yi, int32_t x_start, int32_t x_bound,
                      Interp ig, const InterpDelta *d)
{
    int32_t x, w, x_adj;
    uint32_t y;

    if (line_skip(yi))
        return;

    x_adj = x_start;
    w = x_bound - x_start;
    x = sext11((uint32_t)x_start);

    if (x < S.clip_x0) {
        int32_t delta = S.clip_x0 - x;
        x_adj += delta;
        x += delta;
        w -= delta;
    }
    if (x + w > S.clip_x1 + 1)
        w = S.clip_x1 + 1 - x;
    if (w <= 0)
        return;

    interp_add(&ig, &d->dx, (uint32_t)x_adj);
    interp_add(&ig, &d->dy, (uint32_t)yi);

    y = (uint32_t)yi;

    if (c->textured) {
        const uint8_t (*drow)[512] = dither_lut[y & 3u];
        for (; w > 0; w--, x++) {
            uint32_t t = tex_fetch(ig.u >> 24, ig.v >> 24);
            if (t) {
                uint32_t rgb;
                if (c->raw) {
                    rgb = t & 0x7FFFu;
                } else if (c->gouraud) {
                    rgb = modulate(t, ig.r >> 24, ig.g >> 24, ig.b >> 24,
                                   c->dither ? drow[(uint32_t)x & 3u] : NO_DITHER_LUT);
                } else {
                    rgb = modulate(t, c->flat_r, c->flat_g, c->flat_b,
                                   c->dither ? drow[(uint32_t)x & 3u] : NO_DITHER_LUT);
                }
                plot((uint32_t)x, y, rgb, t & 0x8000u, (t & 0x8000u) ? c->blend : -1);
            }
            ig.u += d->dx.u;
            ig.v += d->dx.v;
            ig.r += d->dx.r;
            ig.g += d->dx.g;
            ig.b += d->dx.b;
        }
    } else if (c->gouraud) {
        const uint8_t (*drow)[512] = dither_lut[y & 3u];
        for (; w > 0; w--, x++) {
            uint32_t r = ig.r >> 24, g = ig.g >> 24, b = ig.b >> 24, rgb;
            if (c->dither) {
                const uint8_t *l = drow[(uint32_t)x & 3u];
                rgb = (uint32_t)l[r] | ((uint32_t)l[g] << 5) | ((uint32_t)l[b] << 10);
            } else {
                rgb = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
            }
            plot((uint32_t)x, y, rgb, 0, c->blend);
            ig.r += d->dx.r;
            ig.g += d->dx.g;
            ig.b += d->dx.b;
        }
    } else if (c->blend < 0 && !S.mask_check) {
        uint16_t *p = &S.vram[(y & 511u) << 10];
        uint16_t val = (uint16_t)(c->flat_rgb15 | S.mask_set);
        for (; w > 0; w--, x++)
            p[x] = val;
    } else {
        for (; w > 0; w--, x++)
            plot((uint32_t)x, y, c->flat_rgb15, 0, c->blend);
    }
}

static inline void swap_vtx(TriVtx *a, TriVtx *b)
{
    TriVtx t = *a;
    *a = *b;
    *b = t;
}

static inline int64_t poly_xfp(int32_t x)
{
    return (int64_t)x * 4294967296LL + 4294967296LL - 2048;
}

static inline int64_t poly_xfp_step(int32_t dx, int32_t dy)
{
    int64_t n = (int64_t)dx * 4294967296LL;
    if (n < 0)
        n -= dy - 1;
    else if (n > 0)
        n += dy - 1;
    return n / dy;
}

static inline uint32_t idelta(int32_t num, int32_t den)
{
    return (uint32_t)(((int64_t)num * 4096) / den) << 12;
}

static void draw_triangle(const PolyCtx *c, const TriVtx *a, const TriVtx *b, const TriVtx *cc)
{
    TriVtx v[3];
    unsigned cv;
    int core;
    int32_t denom;
    InterpDelta d;
    Interp ig;
    int64_t base_coord, base_step, bound_us, bound_ls;
    int right_facing;
    unsigned vo, vp, i;
    struct {
        int32_t y_coord, y_bound;
        int64_t x_coord[2], x_step[2];
        int dec;
    } tp[2];

    v[0] = *a;
    v[1] = *b;
    v[2] = *cc;

    /* Core vertex (left-most, with the hardware's tie-breaking), tracked as
     * a one-hot mask through the Y sort. */
    if (v[1].x <= v[0].x)
        cv = (v[2].x <= v[1].x) ? 4u : 2u;
    else
        cv = (v[2].x < v[0].x) ? 4u : 1u;

    if (v[2].y < v[1].y) {
        swap_vtx(&v[1], &v[2]);
        cv = ((cv >> 1) & 2u) | ((cv << 1) & 4u) | (cv & 1u);
    }
    if (v[1].y < v[0].y) {
        swap_vtx(&v[0], &v[1]);
        cv = ((cv >> 1) & 1u) | ((cv << 1) & 2u) | (cv & 4u);
    }
    if (v[2].y < v[1].y) {
        swap_vtx(&v[1], &v[2]);
        cv = ((cv >> 1) & 2u) | ((cv << 1) & 4u) | (cv & 1u);
    }
    core = (int)(cv >> 1);

    if (v[0].y == v[2].y)
        return;
    if (v[2].y - v[0].y >= 512)
        return;
    if (abs(v[2].x - v[0].x) >= 1024 || abs(v[2].x - v[1].x) >= 1024 ||
        abs(v[1].x - v[0].x) >= 1024)
        return;

    denom = (v[1].x - v[0].x) * (v[2].y - v[1].y) - (v[2].x - v[1].x) * (v[1].y - v[0].y);
    if (!denom)
        return;

#define CALC_DX(f) ((v[1].f - v[0].f) * (v[2].y - v[1].y) - (v[2].f - v[1].f) * (v[1].y - v[0].y))
#define CALC_DY(f) ((v[1].x - v[0].x) * (v[2].f - v[1].f) - (v[2].x - v[1].x) * (v[1].f - v[0].f))
    memset(&d, 0, sizeof(d));
    if (c->gouraud) {
        d.dx.r = idelta(CALC_DX(r), denom);
        d.dx.g = idelta(CALC_DX(g), denom);
        d.dx.b = idelta(CALC_DX(b), denom);
        d.dy.r = idelta(CALC_DY(r), denom);
        d.dy.g = idelta(CALC_DY(g), denom);
        d.dy.b = idelta(CALC_DY(b), denom);
    }
    if (c->textured) {
        d.dx.u = idelta(CALC_DX(u), denom);
        d.dx.v = idelta(CALC_DX(v), denom);
        d.dy.u = idelta(CALC_DY(u), denom);
        d.dy.v = idelta(CALC_DY(v), denom);
    }
#undef CALC_DX
#undef CALC_DY

    /* Interpolants: core vertex value + 0.5, translated back to (0,0). */
    {
        const TriVtx *cvx = &v[core];
        ig.r = ((uint32_t)cvx->r << 24) + (1u << 23);
        ig.g = ((uint32_t)cvx->g << 24) + (1u << 23);
        ig.b = ((uint32_t)cvx->b << 24) + (1u << 23);
        ig.u = ((uint32_t)cvx->u << 24) + (1u << 23);
        ig.v = ((uint32_t)cvx->v << 24) + (1u << 23);
        interp_add(&ig, &d.dx, (uint32_t)(-cvx->x));
        interp_add(&ig, &d.dy, (uint32_t)(-cvx->y));
    }

    base_coord = poly_xfp(v[0].x);
    base_step = poly_xfp_step(v[2].x - v[0].x, v[2].y - v[0].y);

    if (v[1].y == v[0].y) {
        bound_us = 0;
        right_facing = v[1].x > v[0].x;
    } else {
        bound_us = poly_xfp_step(v[1].x - v[0].x, v[1].y - v[0].y);
        right_facing = bound_us > base_step;
    }
    if (v[2].y == v[1].y)
        bound_ls = 0;
    else
        bound_ls = poly_xfp_step(v[2].x - v[1].x, v[2].y - v[1].y);

    /* The hardware walks away from the core vertex: upper half bottom-up
     * when the core vertex isn't the top one, lower half bottom-up when the
     * core vertex is the bottom one. */
    vo = core ? 1u : 0u;
    vp = (core == 2) ? 3u : 0u;

    tp[vo].y_coord = v[0 ^ vo].y;
    tp[vo].y_bound = v[1 ^ vo].y;
    tp[vo].x_coord[right_facing] = poly_xfp(v[0 ^ vo].x);
    tp[vo].x_step[right_facing] = bound_us;
    tp[vo].x_coord[!right_facing] = base_coord + (int64_t)(v[vo].y - v[0].y) * base_step;
    tp[vo].x_step[!right_facing] = base_step;
    tp[vo].dec = (int)vo;

    tp[vo ^ 1].y_coord = v[1 ^ vp].y;
    tp[vo ^ 1].y_bound = v[2 ^ vp].y;
    tp[vo ^ 1].x_coord[right_facing] = poly_xfp(v[1 ^ vp].x);
    tp[vo ^ 1].x_step[right_facing] = bound_ls;
    tp[vo ^ 1].x_coord[!right_facing] = base_coord + (int64_t)(v[1 ^ vp].y - v[0].y) * base_step;
    tp[vo ^ 1].x_step[!right_facing] = base_step;
    tp[vo ^ 1].dec = (int)vp;

    for (i = 0; i < 2; i++) {
        int32_t yi = tp[i].y_coord;
        int32_t yb = tp[i].y_bound;
        int64_t lc = tp[i].x_coord[0], ls = tp[i].x_step[0];
        int64_t rc = tp[i].x_coord[1], rs = tp[i].x_step[1];

        if (tp[i].dec) {
            while (yi > yb) {
                int32_t y;
                yi--;
                lc -= ls;
                rc -= rs;
                y = sext11((uint32_t)yi);
                if (y < S.clip_y0)
                    break;
                if (y > S.clip_y1)
                    continue;
                draw_span(c, yi, fp_int(lc), fp_int(rc), ig, &d);
            }
        } else {
            while (yi < yb) {
                int32_t y = sext11((uint32_t)yi);
                if (y > S.clip_y1)
                    break;
                if (y >= S.clip_y0)
                    draw_span(c, yi, fp_int(lc), fp_int(rc), ig, &d);
                yi++;
                lc += ls;
                rc += rs;
            }
        }
    }
}

static void cmd_polygon(const uint32_t *p)
{
    uint32_t op = p[0] >> 24;
    int gouraud = (op >> 4) & 1;
    int quad = (op >> 3) & 1;
    int tex = (op >> 2) & 1;
    int abe = (op >> 1) & 1;
    int raw = op & 1;
    int nv = quad ? 4 : 3;
    TriVtx v[4];
    PolyCtx c;
    uint32_t clut = 0, tpage = 0;
    int i, idx = 0;

    for (i = 0; i < nv; i++) {
        uint32_t col, xy;
        if (i == 0 || gouraud)
            col = p[idx++];
        else
            col = p[0];
        v[i].r = (int32_t)(col & 0xFFu);
        v[i].g = (int32_t)((col >> 8) & 0xFFu);
        v[i].b = (int32_t)((col >> 16) & 0xFFu);
        xy = p[idx++];
        v[i].x = sext11(xy) + S.offs_x;
        v[i].y = sext11(xy >> 16) + S.offs_y;
        if (tex) {
            uint32_t uv = p[idx++];
            v[i].u = (int32_t)(uv & 0xFFu);
            v[i].v = (int32_t)((uv >> 8) & 0xFFu);
            if (i == 0)
                clut = uv >> 16;
            else if (i == 1)
                tpage = uv >> 16;
        } else {
            v[i].u = 0;
            v[i].v = 0;
        }
    }

    if (tex) {
        set_texpage(tpage, 0);
        clut_update(clut);
    }

    c.gouraud = gouraud;
    c.textured = tex;
    c.raw = tex && raw;
    c.dither = S.dither && (gouraud || (tex && !raw));
    c.blend = abe ? (int)S.abr : -1;
    c.flat_r = (uint32_t)v[0].r;
    c.flat_g = (uint32_t)v[0].g;
    c.flat_b = (uint32_t)v[0].b;
    c.flat_rgb15 = (c.flat_r >> 3) | ((c.flat_g >> 3) << 5) | ((c.flat_b >> 3) << 10);

    /* Fast path: modulation by 808080h without dithering is the identity. */
    if (tex && !raw && !gouraud && !c.dither &&
        c.flat_r == 0x80 && c.flat_g == 0x80 && c.flat_b == 0x80)
        c.raw = 1;

    draw_triangle(&c, &v[0], &v[1], &v[2]);
    if (quad)
        draw_triangle(&c, &v[1], &v[2], &v[3]);
}

/* ======================================================================= */
/* Lines                                                                    */
/* ======================================================================= */

static inline int64_t line_divide(int32_t delta, int32_t dk)
{
    int64_t n = (int64_t)delta * 4294967296LL;
    if (n < 0)
        n -= dk - 1;
    else if (n > 0)
        n += dk - 1;
    return n / dk;
}

static void draw_line(LinePt p0, LinePt p1, int gouraud, int blend)
{
    int32_t adx = abs(p1.x - p0.x);
    int32_t ady = abs(p1.y - p0.y);
    int32_t k = adx > ady ? adx : ady;
    int64_t sx = 0, sy = 0, cx, cy;
    int32_t sr = 0, sg = 0, sb = 0;
    uint32_t cr, cg, cb;
    int32_t i;

    if (adx >= 1024 || ady >= 512)
        return;

    if (p0.x > p1.x && k) {
        LinePt t = p0;
        p0 = p1;
        p1 = t;
    }

    if (k) {
        sx = line_divide(p1.x - p0.x, k);
        sy = line_divide(p1.y - p0.y, k);
        if (gouraud) {
            sr = ((p1.r - p0.r) * 4096) / k;
            sg = ((p1.g - p0.g) * 4096) / k;
            sb = ((p1.b - p0.b) * 4096) / k;
        }
    }

    cx = (int64_t)p0.x * 4294967296LL + 2147483648LL - 1024;
    cy = (int64_t)p0.y * 4294967296LL + 2147483648LL;
    if (sy < 0)
        cy -= 1024;
    cr = ((uint32_t)p0.r << 12) | 0x800u;
    cg = ((uint32_t)p0.g << 12) | 0x800u;
    cb = ((uint32_t)p0.b << 12) | 0x800u;

    for (i = 0; i <= k; i++) {
        uint32_t x = (uint32_t)((uint64_t)cx >> 32) & 2047u;
        uint32_t y = (uint32_t)((uint64_t)cy >> 32) & 2047u;

        if (!line_skip((int32_t)y) &&
            (int32_t)x >= S.clip_x0 && (int32_t)x <= S.clip_x1 &&
            (int32_t)y >= S.clip_y0 && (int32_t)y <= S.clip_y1) {
            uint32_t r, g, b, rgb;
            if (gouraud) {
                r = (cr >> 12) & 0xFFu;
                g = (cg >> 12) & 0xFFu;
                b = (cb >> 12) & 0xFFu;
            } else {
                r = (uint32_t)p0.r;
                g = (uint32_t)p0.g;
                b = (uint32_t)p0.b;
            }
            if (S.dither) {
                const uint8_t *l = dither_lut[y & 3u][x & 3u];
                rgb = (uint32_t)l[r] | ((uint32_t)l[g] << 5) | ((uint32_t)l[b] << 10);
            } else {
                rgb = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
            }
            plot(x, y, rgb, 0, blend);
        }

        cx += sx;
        cy += sy;
        cr += (uint32_t)sr;
        cg += (uint32_t)sg;
        cb += (uint32_t)sb;
    }
}

static inline void line_color(LinePt *pt, uint32_t col)
{
    pt->r = (int32_t)(col & 0xFFu);
    pt->g = (int32_t)((col >> 8) & 0xFFu);
    pt->b = (int32_t)((col >> 16) & 0xFFu);
}

static inline void line_xy(LinePt *pt, uint32_t xy)
{
    pt->x = sext11(xy) + S.offs_x;
    pt->y = sext11(xy >> 16) + S.offs_y;
}

static void cmd_line(const uint32_t *p)
{
    uint32_t op = p[0] >> 24;
    int gouraud = (op >> 4) & 1;
    int poly = (op >> 3) & 1;
    int abe = (op >> 1) & 1;
    LinePt a, b;

    line_color(&a, p[0]);
    line_xy(&a, p[1]);
    if (gouraud) {
        line_color(&b, p[2]);
        line_xy(&b, p[3]);
    } else {
        b.r = a.r;
        b.g = a.g;
        b.b = a.b;
        line_xy(&b, p[2]);
    }

    draw_line(a, b, gouraud, abe ? (int)S.abr : -1);

    if (poly) {
        S.gp0_state = GP0_STATE_POLYLINE;
        S.pl_op = op;
        S.pl_count = 0;
        S.pl_prev = b;
    }
}

static void polyline_word(uint32_t w)
{
    int gouraud = (S.pl_op >> 4) & 1;
    int abe = (S.pl_op >> 1) & 1;
    LinePt b;

    if (S.pl_count == 0 && (w & 0xF000F000u) == 0x50005000u) {
        S.gp0_state = GP0_STATE_COMMAND;
        return;
    }
    S.pl_words[S.pl_count++] = w;
    if (S.pl_count < (gouraud ? 2 : 1))
        return;
    S.pl_count = 0;

    if (gouraud) {
        line_color(&b, S.pl_words[0]);
        line_xy(&b, S.pl_words[1]);
    } else {
        b.r = S.pl_prev.r;
        b.g = S.pl_prev.g;
        b.b = S.pl_prev.b;
        line_xy(&b, S.pl_words[0]);
    }
    draw_line(S.pl_prev, b, gouraud, abe ? (int)S.abr : -1);
    S.pl_prev = b;
}

/* ======================================================================= */
/* Rectangles                                                               */
/* ======================================================================= */

static void draw_sprite(int32_t x0, int32_t y0, int32_t w, int32_t h,
                        uint32_t u0, uint32_t v0, uint32_t color,
                        int tex, int raw, int blend)
{
    uint32_t r = color & 0xFFu, g = (color >> 8) & 0xFFu, b = (color >> 16) & 0xFFu;
    uint32_t fill = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
    int32_t x_start = x0, x_bound = x0 + w;
    int32_t y_start = y0, y_bound = y0 + h;
    uint8_t u = (uint8_t)u0, v = (uint8_t)v0;
    int u_inc = 1, v_inc = 1;
    int32_t x, y;

    if (tex) {
        if (S.flip_x) {
            u_inc = -1;
            u |= 1;
        }
        if (S.flip_y)
            v_inc = -1;
        /* modulation by 808080h is the identity (sprites are never dithered) */
        if (!raw && color == 0x808080u)
            raw = 1;
    }

    if (x_start < S.clip_x0) {
        if (tex)
            u = (uint8_t)(u + (S.clip_x0 - x_start) * u_inc);
        x_start = S.clip_x0;
    }
    if (y_start < S.clip_y0) {
        if (tex)
            v = (uint8_t)(v + (S.clip_y0 - y_start) * v_inc);
        y_start = S.clip_y0;
    }
    if (x_bound > S.clip_x1 + 1)
        x_bound = S.clip_x1 + 1;
    if (y_bound > S.clip_y1 + 1)
        y_bound = S.clip_y1 + 1;

    if (x_bound <= x_start)
        return;

    for (y = y_start; y < y_bound; y++) {
        if (!line_skip(y)) {
            if (tex) {
                uint8_t ur = u;
                for (x = x_start; x < x_bound; x++) {
                    uint32_t t = tex_fetch(ur, v);
                    if (t) {
                        uint32_t rgb = raw ? (t & 0x7FFFu) : modulate(t, r, g, b, NO_DITHER_LUT);
                        plot((uint32_t)x, (uint32_t)y, rgb, t & 0x8000u,
                             (t & 0x8000u) ? blend : -1);
                    }
                    ur = (uint8_t)(ur + u_inc);
                }
            } else if (blend < 0 && !S.mask_check) {
                uint16_t *row = &S.vram[((uint32_t)y & 511u) << 10];
                uint16_t val = (uint16_t)(fill | S.mask_set);
                for (x = x_start; x < x_bound; x++)
                    row[x] = val;
            } else {
                for (x = x_start; x < x_bound; x++)
                    plot((uint32_t)x, (uint32_t)y, fill, 0, blend);
            }
        }
        v = (uint8_t)(v + v_inc);
    }
}

static void cmd_rect(const uint32_t *p)
{
    uint32_t op = p[0] >> 24;
    int tex = (op >> 2) & 1;
    int abe = (op >> 1) & 1;
    int raw = op & 1;
    uint32_t size = (op >> 3) & 3u;
    uint32_t color = p[0] & 0xFFFFFFu;
    uint32_t xy, u = 0, v = 0;
    int32_t x, y, w, h;
    int idx = 1;

    xy = p[idx++];
    x = sext11(xy);
    y = sext11(xy >> 16);

    if (tex) {
        uint32_t uv = p[idx++];
        u = uv & 0xFFu;
        v = (uv >> 8) & 0xFFu;
        clut_update(uv >> 16);
    }

    switch (size) {
    case 0: {
        uint32_t wh = p[idx++];
        w = (int32_t)(wh & 0x3FFu);
        h = (int32_t)((wh >> 16) & 0x1FFu);
        break;
    }
    case 1: w = 1; h = 1; break;
    case 2: w = 8; h = 8; break;
    default: w = 16; h = 16; break;
    }

    x = sext11((uint32_t)(x + S.offs_x));
    y = sext11((uint32_t)(y + S.offs_y));

    draw_sprite(x, y, w, h, u, v, color, tex, tex && raw, abe ? (int)S.abr : -1);
}

/* ======================================================================= */
/* VRAM commands                                                            */
/* ======================================================================= */

static void cmd_fill(const uint32_t *p)
{
    uint32_t c = p[0];
    uint16_t val = (uint16_t)(((c & 0xFFu) >> 3) | ((((c >> 8) & 0xFFu) >> 3) << 5) |
                              ((((c >> 16) & 0xFFu) >> 3) << 10));
    int32_t x0 = (int32_t)(p[1] & 0x3F0u);
    int32_t y0 = (int32_t)((p[1] >> 16) & 0x3FFu);
    int32_t w = (int32_t)(((p[2] & 0x3FFu) + 0xFu) & ~0xFu);
    int32_t h = (int32_t)((p[2] >> 16) & 0x1FFu);
    int32_t x, y;

    for (y = 0; y < h; y++) {
        int32_t dy = (y0 + y) & 511;
        uint16_t *row;
        if (line_skip(dy))
            continue;
        row = &S.vram[dy << 10];
        for (x = 0; x < w; x++)
            row[(x0 + x) & 1023] = val;
    }
}

static void cmd_copy(const uint32_t *p)
{
    int32_t sx = (int32_t)(p[1] & 0x3FFu);
    int32_t sy = (int32_t)((p[1] >> 16) & 0x3FFu);
    int32_t dx = (int32_t)(p[2] & 0x3FFu);
    int32_t dy = (int32_t)((p[2] >> 16) & 0x3FFu);
    int32_t w = (int32_t)(p[3] & 0x3FFu);
    int32_t h = (int32_t)((p[3] >> 16) & 0x1FFu);
    int32_t x, y, i;
    uint16_t tmp[128];

    if (!w)
        w = 0x400;
    if (!h)
        h = 0x200;

    /* The GPU copies through a small line buffer, in chunks of 128 pixels. */
    for (y = 0; y < h; y++) {
        const uint16_t *srow = &S.vram[((sy + y) & 511) << 10];
        uint16_t *drow = &S.vram[((dy + y) & 511) << 10];
        for (x = 0; x < w; x += 128) {
            int32_t n = w - x < 128 ? w - x : 128;
            for (i = 0; i < n; i++)
                tmp[i] = srow[(sx + x + i) & 1023];
            for (i = 0; i < n; i++) {
                uint16_t *dp = &drow[(dx + x + i) & 1023];
                if (!(*dp & S.mask_check))
                    *dp = (uint16_t)(tmp[i] | S.mask_set);
            }
        }
    }
}

static void vram_write_word(uint32_t w)
{
    int i;
    for (i = 0; i < 2; i++) {
        uint16_t *dp = &S.vram[((uint32_t)(S.wr_cy & 511) << 10) | (uint32_t)(S.wr_cx & 1023)];
        if (!(*dp & S.mask_check))
            *dp = (uint16_t)((w & 0xFFFFu) | S.mask_set);
        w >>= 16;
        S.wr_cx++;
        if (S.wr_cx == S.wr_x + S.wr_w) {
            S.wr_cx = S.wr_x;
            S.wr_cy++;
            if (S.wr_cy == S.wr_y + S.wr_h) {
                S.gp0_state = GP0_STATE_COMMAND;
                break;
            }
        }
    }
}

/* ======================================================================= */
/* GP0                                                                      */
/* ======================================================================= */

static int gp0_cmd_len(uint32_t op)
{
    switch (op >> 5) {
    case 0:
        return op == 0x02 ? 3 : 1;
    case 1: {
        int nv = (op & 0x08) ? 4 : 3;
        int len = 1 + nv * ((op & 0x04) ? 2 : 1);
        if (op & 0x10)
            len += nv - 1;
        return len;
    }
    case 2:
        return (op & 0x10) ? 4 : 3;
    case 3: {
        int len = 2;
        if (op & 0x04)
            len++;
        if (((op >> 3) & 3u) == 0)
            len++;
        return len;
    }
    case 4:
        return 4;
    case 5:
    case 6:
        return 3;
    default:
        return 1;
    }
}

static void gp0_env(uint32_t w)
{
    switch (w >> 24) {
    case 0xE1:
        set_texpage(w, 1);
        break;
    case 0xE2:
        S.tww = w & 0x1Fu;
        S.twh = (w >> 5) & 0x1Fu;
        S.twx = (w >> 10) & 0x1Fu;
        S.twy = (w >> 15) & 0x1Fu;
        recalc_texwin();
        break;
    case 0xE3:
        S.clip_x0 = (int32_t)(w & 0x3FFu);
        S.clip_y0 = (int32_t)((w >> 10) & 0x3FFu);
        break;
    case 0xE4:
        S.clip_x1 = (int32_t)(w & 0x3FFu);
        S.clip_y1 = (int32_t)((w >> 10) & 0x3FFu);
        break;
    case 0xE5:
        S.offs_x = sext11(w);
        S.offs_y = sext11(w >> 11);
        break;
    case 0xE6:
        S.mask_set = (w & 1u) ? 0x8000u : 0u;
        S.mask_check = (w & 2u) ? 0x8000u : 0u;
        break;
    default:
        break;
    }
}

static void gp0_execute(const uint32_t *p)
{
    uint32_t op = p[0] >> 24;

    switch (op >> 5) {
    case 0:
        if (op == 0x01)
            clut_invalidate();
        else if (op == 0x02)
            cmd_fill(p);
        else if (op == 0x1F)
            S.irq = 1;
        break;
    case 1:
        cmd_polygon(p);
        break;
    case 2:
        cmd_line(p);
        break;
    case 3:
        cmd_rect(p);
        break;
    case 4:
        cmd_copy(p);
        break;
    case 5:
        S.wr_x = (int32_t)(p[1] & 0x3FFu);
        S.wr_y = (int32_t)((p[1] >> 16) & 0x3FFu);
        S.wr_w = (int32_t)(p[2] & 0x3FFu);
        S.wr_h = (int32_t)((p[2] >> 16) & 0x1FFu);
        if (!S.wr_w)
            S.wr_w = 0x400;
        if (!S.wr_h)
            S.wr_h = 0x200;
        S.wr_cx = S.wr_x;
        S.wr_cy = S.wr_y;
        S.gp0_state = GP0_STATE_VRAM_WRITE;
        break;
    case 6:
        S.rd_x = (int32_t)(p[1] & 0x3FFu);
        S.rd_y = (int32_t)((p[1] >> 16) & 0x3FFu);
        S.rd_w = (int32_t)(p[2] & 0x3FFu);
        S.rd_h = (int32_t)((p[2] >> 16) & 0x1FFu);
        if (!S.rd_w)
            S.rd_w = 0x400;
        if (!S.rd_h)
            S.rd_h = 0x200;
        S.rd_cx = S.rd_x;
        S.rd_cy = S.rd_y;
        S.rd_active = 1;
        break;
    default:
        gp0_env(p[0]);
        break;
    }
}

void gpu_write_gp0(uint32_t w)
{
    switch (S.gp0_state) {
    case GP0_STATE_VRAM_WRITE:
        vram_write_word(w);
        return;
    case GP0_STATE_POLYLINE:
        polyline_word(w);
        return;
    default:
        break;
    }

    if (S.cmd_count == 0)
        S.cmd_need = gp0_cmd_len(w >> 24);
    S.cmd[S.cmd_count++] = w;
    if (S.cmd_count < S.cmd_need)
        return;
    S.cmd_count = 0;
    gp0_execute(S.cmd);
}

void gpu_dma_write(const uint32_t *src, int words)
{
    int i;
    for (i = 0; i < words; i++)
        gpu_write_gp0(src[i]);
}

uint32_t gpu_read_data(void)
{
    if (S.rd_active) {
        uint32_t v = 0;
        int i;
        for (i = 0; i < 2; i++) {
            v |= (uint32_t)S.vram[((uint32_t)(S.rd_cy & 511) << 10) | (uint32_t)(S.rd_cx & 1023)]
                 << (16 * i);
            S.rd_cx++;
            if (S.rd_cx == S.rd_x + S.rd_w) {
                if (S.rd_cy + 1 == S.rd_y + S.rd_h) {
                    S.rd_active = 0;
                } else {
                    S.rd_cy++;
                    S.rd_cx = S.rd_x;
                }
            }
        }
        S.read_latch = v;
    }
    return S.read_latch;
}

void gpu_dma_read(uint32_t *dst, int words)
{
    int i;
    for (i = 0; i < words; i++)
        dst[i] = gpu_read_data();
}

/* ======================================================================= */
/* GP1                                                                      */
/* ======================================================================= */

static void reset_command_buffer(void)
{
    S.gp0_state = GP0_STATE_COMMAND;
    S.cmd_count = 0;
    S.cmd_need = 0;
    S.pl_count = 0;
    S.rd_active = 0;
    clut_invalidate();
}

static void soft_reset(void)
{
    reset_command_buffer();
    S.irq = 0;
    S.display_off = 1;
    S.dma_dir = 0;
    S.disp_x = 0;
    S.disp_y = 0;
    S.h_start = 0x200;
    S.h_end = 0x200 + 256 * 10;
    S.v_start = 0x10;
    S.v_end = 0x10 + 240;
    S.disp_mode = 0;
    S.field = 0;

    S.tp_x = S.tp_y = 0;
    S.abr = S.tex_mode = 0;
    S.dither = S.dfe = 0;
    S.tex_disable = 0;
    S.flip_x = S.flip_y = 0;
    S.tww = S.twh = S.twx = S.twy = 0;
    recalc_texwin();
    S.clip_x0 = S.clip_y0 = S.clip_x1 = S.clip_y1 = 0;
    S.offs_x = S.offs_y = 0;
    S.mask_set = S.mask_check = 0;
}

void gpu_write_gp1(uint32_t v)
{
    uint32_t cmd = (v >> 24) & 0x3Fu;

    switch (cmd) {
    case 0x00:
        soft_reset();
        break;
    case 0x01:
        reset_command_buffer();
        break;
    case 0x02:
        S.irq = 0;
        break;
    case 0x03:
        S.display_off = v & 1u;
        break;
    case 0x04:
        S.dma_dir = v & 3u;
        break;
    case 0x05:
        S.disp_x = v & 0x3FEu;  /* bit 0 is ignored by the hardware */
        S.disp_y = (v >> 10) & 0x1FFu;
        break;
    case 0x06:
        S.h_start = v & 0xFFFu;
        S.h_end = (v >> 12) & 0xFFFu;
        break;
    case 0x07:
        S.v_start = v & 0x3FFu;
        S.v_end = (v >> 10) & 0x3FFu;
        break;
    case 0x08:
        S.disp_mode = v & 0xFFu;
        if (!(S.disp_mode & 0x20u))
            S.field = 0;
        break;
    case 0x09:
        S.tex_disable_allow = v & 1u;
        break;
    default:
        if (cmd >= 0x10 && cmd <= 0x1F) {
            switch (v & 0xFu) {
            case 0x2:
                S.read_latch = S.tww | (S.twh << 5) | (S.twx << 10) | (S.twy << 15);
                break;
            case 0x3:
                S.read_latch = (uint32_t)S.clip_x0 | ((uint32_t)S.clip_y0 << 10);
                break;
            case 0x4:
                S.read_latch = (uint32_t)S.clip_x1 | ((uint32_t)S.clip_y1 << 10);
                break;
            case 0x5:
                S.read_latch = ((uint32_t)S.offs_x & 0x7FFu) | (((uint32_t)S.offs_y & 0x7FFu) << 11);
                break;
            case 0x7:
                S.read_latch = 2;
                break;
            case 0x8:
                S.read_latch = 0;
                break;
            default:
                break;  /* old latch value remains */
            }
        }
        break;
    }
}

uint32_t gpu_read_status(void)
{
    uint32_t s = 0;
    int interlaced = (S.disp_mode & 0x20u) != 0;

    s |= (S.tp_x / 64u) & 0xFu;
    s |= (S.tp_y >= 256u) ? (1u << 4) : 0u;
    s |= S.abr << 5;
    s |= S.tex_mode << 7;
    s |= S.dither << 9;
    s |= S.dfe << 10;
    if (S.mask_set)
        s |= 1u << 11;
    if (S.mask_check)
        s |= 1u << 12;
    if (!interlaced || !S.field)
        s |= 1u << 13;
    /* bit 14 ("reverse") always 0 */
    s |= S.tex_disable << 15;
    s |= ((S.disp_mode >> 6) & 1u) << 16;
    s |= (S.disp_mode & 3u) << 17;
    s |= (S.disp_mode & 0x3Cu) << 17;     /* bits 2..5 -> 19..22 */
    s |= S.display_off << 23;
    s |= S.irq << 24;
    switch (S.dma_dir) {
    case 1:
    case 2:
        s |= 1u << 25;
        break;
    case 3:
        if (S.rd_active)
            s |= 1u << 25;
        break;
    default:
        break;
    }
    s |= 1u << 26;
    if (S.rd_active)
        s |= 1u << 27;
    s |= 1u << 28;
    s |= S.dma_dir << 29;
    if (interlaced ? S.field : S.line_parity)
        s |= 1u << 31;
    return s;
}

/* ======================================================================= */
/* Timing                                                                   */
/* ======================================================================= */

void gpu_vblank(void)
{
    if (S.disp_mode & 0x20u)
        S.field ^= 1u;
    else
        S.field = 0;
}

void gpu_set_line_parity(int odd)
{
    S.line_parity = odd ? 1u : 0u;
}

/* ======================================================================= */
/* Display                                                                  */
/* ======================================================================= */

void gpu_get_display(GpuDisplay *d)
{
    int div, nominal_w, w, h;
    int pal = (S.disp_mode >> 3) & 1;
    int interlaced = (S.disp_mode >> 5) & 1;
    int is480 = (S.disp_mode & 0x24u) == 0x24u;

    if (S.disp_mode & 0x40u) {
        div = 7;
        nominal_w = 368;
    } else {
        switch (S.disp_mode & 3u) {
        case 0: div = 10; nominal_w = 256; break;
        case 1: div = 8; nominal_w = 320; break;
        case 2: div = 5; nominal_w = 512; break;
        default: div = 4; nominal_w = 640; break;
        }
    }

    w = 0;
    if (S.h_end > S.h_start)
        w = ((int)(S.h_end - S.h_start) / div + 2) & ~3;
    if (w <= 0)
        w = nominal_w;
    if (w > VRAM_W)
        w = VRAM_W;

    if (S.v_end > S.v_start)
        h = (int)(S.v_end - S.v_start);
    else
        h = pal ? 288 : 240;
    if (is480)
        h *= 2;
    if (h > VRAM_H)
        h = VRAM_H;

    d->enabled = !S.display_off;
    d->vram_x = (int)S.disp_x;
    d->vram_y = (int)S.disp_y;
    d->width = w;
    d->height = h;
    d->is_24bit = (S.disp_mode >> 4) & 1;
    d->interlaced = interlaced;
    d->pal = pal;
    d->hres_mode = (int)(((S.disp_mode >> 6) & 1u) | ((S.disp_mode & 3u) << 1));
}

void gpu_render_display(uint32_t *out, int *w, int *h)
{
    GpuDisplay d;
    int x, y;

    gpu_get_display(&d);
    *w = d.width;
    *h = d.height;

    if (!d.enabled) {
        for (y = 0; y < d.width * d.height; y++)
            out[y] = 0xFF000000u;
        return;
    }

    for (y = 0; y < d.height; y++) {
        const uint16_t *row = &S.vram[(uint32_t)((d.vram_y + y) & 511) << 10];
        uint32_t *o = out + (size_t)y * (size_t)d.width;
        if (d.is_24bit) {
            uint32_t bx = (uint32_t)d.vram_x * 2u;
            for (x = 0; x < d.width; x++, bx += 3) {
                uint32_t c[3];
                int k;
                for (k = 0; k < 3; k++) {
                    uint32_t ba = (bx + (uint32_t)k) & 2047u;
                    c[k] = (row[ba >> 1] >> ((ba & 1u) * 8u)) & 0xFFu;
                }
                o[x] = 0xFF000000u | (c[0] << 16) | (c[1] << 8) | c[2];
            }
        } else {
            for (x = 0; x < d.width; x++) {
                uint32_t p = row[(d.vram_x + x) & 1023];
                uint32_t r = p & 31u, g = (p >> 5) & 31u, b = (p >> 10) & 31u;
                r = (r << 3) | (r >> 2);
                g = (g << 3) | (g >> 2);
                b = (b << 3) | (b >> 2);
                o[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
}

const uint16_t *gpu_vram(void)
{
    return S.vram;
}

/* ======================================================================= */
/* Init / state                                                             */
/* ======================================================================= */

void gpu_init(void)
{
    if (!dither_lut_ready)
        build_dither_lut();
    memset(&S, 0, sizeof(S));
    S.tex_disable_allow = 0;
    S.read_latch = 0;
    S.line_parity = 0;
    soft_reset();
}

size_t gpu_state_size(void)
{
    return sizeof(S);
}

void gpu_save_state(void *dst)
{
    memcpy(dst, &S, sizeof(S));
}

void gpu_load_state(const void *src)
{
    if (!dither_lut_ready)
        build_dither_lut();
    memcpy(&S, src, sizeof(S));
}
