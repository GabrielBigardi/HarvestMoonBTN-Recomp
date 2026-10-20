/* gpu_test.c - sanity tests for runtime/gpu.c
 *
 * Build (from the repo root):
 *   clang -std=c11 -O2 -Wall -Wextra -Iruntime runtime/tests/gpu_test.c runtime/gpu.c -o gpu_test.exe
 * Run:
 *   gpu_test.exe [vram.bmp] [display.bmp]
 */
#include "../gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) {                                                   \
            g_pass++;                                                 \
        } else {                                                      \
            g_fail++;                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

static void gp0(uint32_t w) { gpu_write_gp0(w); }
static void gp1(uint32_t w) { gpu_write_gp1(w); }
static uint16_t px(int x, int y) { return gpu_vram()[(y & 511) * 1024 + (x & 1023)]; }
static uint32_t xy(int x, int y) { return ((uint32_t)(y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF); }
static uint16_t rgb15(int r, int g, int b) { return (uint16_t)(r | (g << 5) | (b << 10)); }

static void env_default(void)
{
    gp0(0xE1000000);
    gp0(0xE2000000);
    gp0(0xE3000000);
    gp0(0xE4000000 | (511u << 10) | 1023u);
    gp0(0xE5000000);
    gp0(0xE6000000);
}

static void fill(int x, int y, int w, int h, uint32_t rgb24)
{
    gp0(0x02000000 | (rgb24 & 0xFFFFFF));
    gp0(xy(x, y));
    gp0(xy(w, h));
}

static void clear_vram(void)
{
    fill(0, 0, 0x3FF, 256, 0); /* 3FFh rounds up to 400h */
    fill(0, 256, 0x3FF, 256, 0);
}

static void upload(int x, int y, int w, int h, const uint16_t *data)
{
    int n = w * h, i;
    gp0(0xA0000000);
    gp0(xy(x, y));
    gp0(xy(w, h));
    for (i = 0; i < n; i += 2) {
        uint32_t lo = data[i];
        uint32_t hi = (i + 1 < n) ? data[i + 1] : 0;
        gp0(lo | (hi << 16));
    }
}

/* ---------------------------------------------------------------------- */
/* reference coverage: pixel (x,y) is inside when ceil(L(y)) <= x < ceil(R(y)) */

typedef struct { int x, y; } P2;

static long long ceil_div(long long a, long long b)
{
    long long q = a / b;
    if ((a % b) != 0 && a > 0)
        q++;
    return q;
}

static int ref_cover(P2 a, P2 b, P2 c, int x, int y)
{
    P2 e[3][2];
    long long xs[3];
    int n = 0, i;
    e[0][0] = a; e[0][1] = b;
    e[1][0] = b; e[1][1] = c;
    e[2][0] = c; e[2][1] = a;
    for (i = 0; i < 3; i++) {
        P2 lo = e[i][0], hi = e[i][1];
        if (lo.y > hi.y) { P2 t = lo; lo = hi; hi = t; }
        if (lo.y == hi.y || y < lo.y || y >= hi.y)
            continue;
        xs[n++] = ceil_div((long long)lo.x * (hi.y - lo.y) + (long long)(y - lo.y) * (hi.x - lo.x),
                           hi.y - lo.y);
    }
    if (n < 2)
        return 0;
    if (xs[0] > xs[1]) { long long t = xs[0]; xs[0] = xs[1]; xs[1] = t; }
    return x >= xs[0] && x < xs[1];
}

static uint32_t rng = 12345;
static int rnd(int lo, int hi) /* inclusive */
{
    rng = rng * 1103515245u + 12345u;
    return lo + (int)((rng >> 8) % (uint32_t)(hi - lo + 1));
}

static void test_random_triangles(void)
{
    const int CX0 = 0, CY0 = 0, CX1 = 255, CY1 = 255;
    const int OX = 7, OY = -5;
    int t, x, y, bad_tris = 0, bad_px = 0;

    env_default();
    gp0(0xE3000000 | (uint32_t)(CY0 << 10) | (uint32_t)CX0);
    gp0(0xE4000000 | (uint32_t)(CY1 << 10) | (uint32_t)CX1);
    gp0(0xE5000000 | ((uint32_t)(OY & 0x7FF) << 11) | (uint32_t)(OX & 0x7FF));

    for (t = 0; t < 3000; t++) {
        P2 v[3];
        int i, bad = 0;
        int range = (t < 1000) ? 12 : 300; /* small ones stress edge cases */
        fill(0, 0, 320, 320, 0);
        for (i = 0; i < 3; i++) {
            v[i].x = rnd(-40, -40 + range);
            v[i].y = rnd(-40, -40 + range);
        }
        gp0(0x20FFFFFF);
        for (i = 0; i < 3; i++)
            gp0(xy(v[i].x, v[i].y));
        for (i = 0; i < 3; i++) {
            v[i].x += OX;
            v[i].y += OY;
        }
        for (y = 0; y < 320; y++)
            for (x = 0; x < 320; x++) {
                int exp = ref_cover(v[0], v[1], v[2], x, y) &&
                          x >= CX0 && x <= CX1 && y >= CY0 && y <= CY1;
                int got = px(x, y) != 0;
                if (exp != got) {
                    bad = 1;
                    bad_px++;
                }
            }
        if (bad && bad_tris++ < 5)
            printf("  triangle mismatch: (%d,%d) (%d,%d) (%d,%d)\n", v[0].x, v[0].y, v[1].x,
                   v[1].y, v[2].x, v[2].y);
    }
    CHECK(bad_tris == 0, "random triangle coverage: %d triangles / %d pixels differ", bad_tris, bad_px);

    /* Random quads drawn additively: shared edge must be covered exactly once */
    gp0(0xE1000020); /* abr = 1 (B+F) */
    bad_tris = 0;
    for (t = 0; t < 1000; t++) {
        P2 v[4];
        int i, bad = 0;
        fill(0, 0, 320, 320, 0);
        for (i = 0; i < 4; i++) {
            v[i].x = rnd(-20, 280);
            v[i].y = rnd(-20, 280);
        }
        gp0(0x2A080808); /* flat quad, semi-transparent, colour 1/1/1 in 5 bit */
        for (i = 0; i < 4; i++)
            gp0(xy(v[i].x, v[i].y));
        for (i = 0; i < 4; i++) {
            v[i].x += OX;
            v[i].y += OY;
        }
        for (y = 0; y < 320; y++)
            for (x = 0; x < 320; x++) {
                int in = x >= CX0 && x <= CX1 && y >= CY0 && y <= CY1;
                int exp = in ? ref_cover(v[0], v[1], v[2], x, y) + ref_cover(v[1], v[2], v[3], x, y) : 0;
                int got = px(x, y) & 31;
                if (exp != got)
                    bad = 1;
            }
        if (bad && bad_tris++ < 5)
            printf("  quad mismatch: (%d,%d) (%d,%d) (%d,%d) (%d,%d)\n", v[0].x, v[0].y, v[1].x,
                   v[1].y, v[2].x, v[2].y, v[3].x, v[3].y);
    }
    CHECK(bad_tris == 0, "random additive quads: %d mismatches", bad_tris);
    env_default();
}

/* ---------------------------------------------------------------------- */

static void test_basic_triangle(void)
{
    int x, y, ok = 1, count = 0;
    env_default();
    fill(0, 0, 16, 8, 0);
    gp0(0x200000FF); /* flat red */
    gp0(xy(0, 0));
    gp0(xy(4, 0));
    gp0(xy(0, 4));
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            int exp = (y < 4) && (x < 4 - y);
            uint16_t p = px(x, y);
            if (p)
                count++;
            if (exp != (p != 0) || (p && p != 0x001F))
                ok = 0;
        }
    CHECK(ok && count == 10, "triangle (0,0),(4,0),(0,4): coverage wrong (%d pixels)", count);
    CHECK(px(3, 0) == 0x1F && px(4, 0) == 0 && px(0, 3) == 0x1F && px(0, 4) == 0 && px(1, 3) == 0,
          "triangle edge pixels");

    /* pointy-top triangle: top vertex row is empty, sample points are integer coords */
    fill(0, 0, 16, 8, 0);
    gp0(0x2000FF00);
    gp0(xy(2, 0));
    gp0(xy(0, 4));
    gp0(xy(4, 4));
    CHECK(px(2, 0) == 0 && px(2, 1) == rgb15(0, 31, 0) && px(1, 1) == 0 && px(3, 1) == 0 &&
          px(1, 2) == rgb15(0, 31, 0) && px(3, 2) == 0 && px(0, 3) == 0 && px(1, 3) == rgb15(0, 31, 0),
          "pointy triangle coverage");

    /* too wide: |dx| = 1024 -> not drawn */
    fill(0, 0, 32, 32, 0);
    gp0(0x20FFFFFF);
    gp0(xy(-1000, 0));
    gp0(xy(24, 0));
    gp0(xy(0, 20));
    CHECK(px(1, 1) == 0, "triangle wider than 1023 must be skipped");
    gp0(0x20FFFFFF);
    gp0(xy(-999, 0));
    gp0(xy(24, 0));
    gp0(xy(0, 20));
    CHECK(px(1, 1) != 0, "triangle 1023 wide must be drawn");
}

static void test_gouraud_and_dither(void)
{
    int x, y, ok;
    env_default();
    fill(0, 0, 64, 64, 0);

    /* gouraud quad, dither off: core (left-most) vertex pixel has its exact colour */
    gp0(0x380000FF);           /* v0 red */
    gp0(xy(10, 10));
    gp0(0x0000FF00);           /* v1 green */
    gp0(xy(50, 10));
    gp0(0x00FF0000);           /* v2 blue */
    gp0(xy(10, 50));
    gp0(0x00FFFFFF);           /* v3 white */
    gp0(xy(50, 50));
    CHECK(px(10, 10) == rgb15(31, 0, 0), "gouraud: v0 pixel = %04X", px(10, 10));
    CHECK(px(49, 49) != 0 && px(50, 50) == 0 && px(9, 10) == 0, "gouraud quad extent");

    /* dithered constant gouraud 808080h: (128 + d) >> 3 = 15 for d<0 else 16 */
    fill(0, 0, 64, 64, 0);
    gp0(0xE1000200); /* dither on */
    gp0(0x30808080);
    gp0(xy(0, 0));
    gp0(0x00808080);
    gp0(xy(32, 0));
    gp0(0x00808080);
    gp0(xy(0, 32));
    {
        static const int m[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };
        ok = 1;
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) {
                int c = (128 + m[y & 3][x & 3]) >> 3;
                if (px(x, y) != rgb15(c, c, c))
                    ok = 0;
            }
        CHECK(ok, "gouraud dither pattern");
    }
    /* flat untextured polygons are never dithered */
    fill(0, 0, 64, 64, 0);
    gp0(0x20808080);
    gp0(xy(0, 0));
    gp0(xy(32, 0));
    gp0(xy(0, 32));
    CHECK(px(0, 0) == rgb15(16, 16, 16) && px(1, 1) == rgb15(16, 16, 16), "flat polygon not dithered");
    gp0(0xE1000000);
}

static uint16_t g_clut[16];

static void test_textures(void)
{
    uint16_t tex4[4 * 16];
    uint16_t tex15[64 * 64];
    int x, y, ok;
    const int CLUT_X = 0, CLUT_Y = 500;
    const uint32_t clut_attr = (uint32_t)((CLUT_X / 16) | (CLUT_Y << 6));

    env_default();

    /* CLUT: entry 0 transparent, entry 15 has the semi-transparency bit */
    for (x = 0; x < 16; x++)
        g_clut[x] = (uint16_t)((x * 2) | ((31 - x * 2) << 10) | ((x & 3) << 7));
    g_clut[0] = 0;
    g_clut[15] |= 0x8000;
    upload(CLUT_X, CLUT_Y, 16, 1, g_clut);

    /* 4bpp 16x16 texture at page 10 (x=640), y=256: texel(u,v) = (u+v)&15 */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 4; x++) {
            uint16_t w = 0;
            int k;
            for (k = 0; k < 4; k++)
                w |= (uint16_t)(((x * 4 + k + y) & 15) << (k * 4));
            tex4[y * 4 + x] = w;
        }
    upload(640, 256, 4, 16, tex4);

    gp0(0xE1000000 | 10u | 0x10u); /* page 10, y 256, 4bpp */

    /* raw textured 16x16 sprite at (300,20) */
    fill(288, 0, 128, 128, 0x400000); /* background: dark blue (8) */
    gp0(0x7D000000);
    gp0(xy(300, 20));
    gp0(clut_attr << 16);
    ok = 1;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int idx = (x + y) & 15;
            uint16_t exp = idx ? g_clut[idx] : rgb15(0, 0, 8);
            if (px(300 + x, 20 + y) != exp)
                ok = 0;
        }
    CHECK(ok, "4bpp raw sprite");

    /* modulated by 404040h -> each 5-bit channel halved (floor) */
    gp0(0x7C404040);
    gp0(xy(320, 20));
    gp0((clut_attr << 16));
    ok = 1;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int idx = (x + y) & 15;
            uint16_t c = g_clut[idx], exp;
            if (!idx)
                exp = rgb15(0, 0, 8);
            else
                exp = (uint16_t)(((c & 31) >> 1) | ((((c >> 5) & 31) >> 1) << 5) |
                                 ((((c >> 10) & 31) >> 1) << 10) | (c & 0x8000));
            if (px(320 + x, 20 + y) != exp)
                ok = 0;
        }
    CHECK(ok, "4bpp modulated sprite");

    /* variable size sprite with a 16x16 texture window, offset (0,0) */
    gp0(0xE2000000 | (0x1Eu << 5) | 0x1Eu);
    gp0(0x65000000);
    gp0(xy(300, 40));
    gp0(clut_attr << 16);
    gp0(xy(40, 20));
    ok = 1;
    for (y = 0; y < 20; y++)
        for (x = 0; x < 40; x++) {
            int idx = ((x & 15) + (y & 15)) & 15;
            uint16_t exp = idx ? g_clut[idx] : rgb15(0, 0, 8);
            if (px(300 + x, 40 + y) != exp)
                ok = 0;
        }
    CHECK(ok, "texture window repeat");
    gp0(0xE2000000);

    /* semi-transparent textured sprite: only texels with bit15 blend (mode 1, B+F) */
    gp0(0xE1000000 | 10u | 0x10u | 0x20u);
    gp0(0x6F000000); /* 1x1 textured raw semi */
    gp0(xy(350, 20));
    gp0((clut_attr << 16) | 15u); /* u=15 -> index 15 (bit15 set) */
    gp0(0x6F000000);
    gp0(xy(351, 20));
    gp0((clut_attr << 16) | 1u); /* index 1 (opaque) */
    {
        uint16_t c15 = g_clut[15];
        int b = ((c15 >> 10) & 31) + 8;
        uint16_t exp = (uint16_t)((c15 & 0x3FF) | ((b > 31 ? 31 : b) << 10) | 0x8000);
        CHECK(px(350, 20) == exp, "semi-transparent texel blended: %04X vs %04X", px(350, 20), exp);
        CHECK(px(351, 20) == g_clut[1], "opaque texel not blended");
    }

    /* 15bpp texture, identity UV mapping on a quad reproduces the texture */
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            tex15[y * 64 + x] = (uint16_t)(((x >> 1) & 31) | (((y >> 1) & 31) << 5) | (((x ^ y) & 31) << 10) | 1);
    upload(512, 0, 64, 64, tex15);
    gp0(0x2D000000); /* textured quad, raw */
    gp0(xy(400, 300));
    gp0(0x00000000 | (clut_attr << 16));
    gp0(xy(464, 300));
    gp0(((uint32_t)(8 | (2 << 7)) << 16) | 64u); /* page x=512, 15bpp */
    gp0(xy(400, 364));
    gp0(64u << 8);
    gp0(xy(464, 364));
    gp0((64u << 8) | 64u);
    ok = 1;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            if (px(400 + x, 300 + y) != tex15[y * 64 + x]) {
                if (ok) printf("  first mismatch at %d,%d: %04X vs %04X\n", x, y, px(400 + x, 300 + y), tex15[y * 64 + x]);
                ok = 0;
            }
    CHECK(ok, "15bpp textured quad with identity UVs");
    CHECK(((gpu_read_status() >> 7) & 3) == 2 && (gpu_read_status() & 0xF) == 8,
          "polygon texpage updates GPUSTAT");

    /* half-scale mapping: u = floor(x/2 + 0.5) */
    gp0(0x2D000000);
    gp0(xy(480, 300));
    gp0(0);
    gp0(xy(544, 300));
    gp0(((uint32_t)(8 | (2 << 7)) << 16) | 32u);
    gp0(xy(480, 364));
    gp0(32u << 8);
    gp0(xy(544, 364));
    gp0((32u << 8) | 32u);
    ok = 1;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            if (px(480 + x, 300 + y) != tex15[((y + 1) >> 1) * 64 + ((x + 1) >> 1)])
                ok = 0;
    CHECK(ok, "15bpp textured quad at half scale");

    /* textured, modulated gouraud triangle (visual only) */
    gp0(0xE1000200 | 10u | 0x10u);
    gp0(0x34FF8080);
    gp0(xy(560, 300));
    gp0((clut_attr << 16) | 0x0000);
    gp0(0x0080FF80);
    gp0(xy(620, 300));
    gp0(((uint32_t)(10 | 0x10)) << 16 | 15u);
    gp0(0x008080FF);
    gp0(xy(560, 360));
    gp0(15u << 8);
    gp0(0xE1000000);
    env_default();
}

static void test_fill_copy(void)
{
    int x, y, ok;
    env_default();
    fill(0x100, 400, 0x40, 16, 0);
    fill(0x105, 400, 17, 4, 0x00F8F8F8);
    CHECK(px(0x100, 400) == 0x7FFF && px(0x11F, 403) == 0x7FFF && px(0x120, 400) == 0 &&
          px(0x100, 404) == 0, "fill rounding (x&3F0, w rounded up to 16)");

    /* fill wraps horizontally */
    fill(0x3F0, 420, 0x20, 2, 0x0000F800);
    CHECK(px(0x3FF, 420) == rgb15(0, 31, 0) && px(0x00F, 421) == rgb15(0, 31, 0) &&
          px(0x010, 420) != rgb15(0, 31, 0), "fill wraps at x=1024");

    /* fill ignores mask settings */
    gp0(0xE6000003);
    fill(0x140, 400, 16, 1, 0x000000F8);
    CHECK(px(0x140, 400) == 0x001F, "fill not affected by mask");
    gp0(0xE6000000);

    /* copy the sprite area */
    gp0(0x80000000);
    gp0(xy(300, 20));
    gp0(xy(700, 20));
    gp0(xy(36, 16));
    ok = 1;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 36; x++)
            if (px(700 + x, 20 + y) != px(300 + x, 20 + y))
                ok = 0;
    CHECK(ok, "VRAM to VRAM copy");

    /* copy with mask check: destination pixels with bit15 survive */
    fill(760, 60, 16, 1, 0);
    gp0(0xE6000001);
    gp0(0x68FFFFFF); /* 1x1 white with mask set */
    gp0(xy(762, 60));
    gp0(0xE6000002);
    gp0(0x80000000);
    gp0(xy(300, 20));
    gp0(xy(760, 60));
    gp0(xy(4, 1));
    CHECK(px(762, 60) == 0xFFFF && px(763, 60) == px(303, 20), "copy honours mask check");
    gp0(0xE6000000);
}

static void test_lines(void)
{
    int x, cnt;
    env_default();
    fill(576, 0, 64, 64, 0);

    gp0(0x40FFFFFF);
    gp0(xy(580, 40));
    gp0(xy(600, 40));
    for (cnt = 0, x = 576; x < 620; x++)
        cnt += px(x, 40) != 0;
    CHECK(cnt == 21 && px(580, 40) && px(600, 40), "horizontal line inclusive (%d px)", cnt);

    gp0(0x40FFFFFF);
    gp0(xy(600, 44));
    gp0(xy(580, 44));
    for (cnt = 0, x = 576; x < 620; x++)
        cnt += px(x, 44) != 0;
    CHECK(cnt == 21, "reversed line (%d px)", cnt);

    gp0(0x40FF00FF);
    gp0(xy(580, 2));
    gp0(xy(590, 30));
    CHECK(px(580, 2) != 0 && px(590, 30) != 0, "steep line endpoints");

    /* gouraud line */
    gp0(0x500000FF);
    gp0(xy(580, 50));
    gp0(0x00FF0000);
    gp0(xy(635, 55));
    CHECK(px(580, 50) == rgb15(31, 0, 0) && px(635, 55) != 0, "gouraud line endpoints");

    /* overlapping vertices: one pixel */
    gp0(0x4000FF00);
    gp0(xy(620, 10));
    gp0(xy(620, 10));
    CHECK(px(620, 10) == rgb15(0, 31, 0) && px(621, 10) == 0, "zero-length line draws 1 pixel");

    /* polyline (mono): square, then terminator, then a normal command */
    gp0(0x4800FFFF);
    gp0(xy(600, 15));
    gp0(xy(615, 15));
    gp0(xy(615, 30));
    gp0(xy(600, 30));
    gp0(xy(600, 15));
    gp0(0x55555555);
    gp0(0x68FF0000); /* 1x1 blue rect after the polyline */
    gp0(xy(608, 22));
    CHECK(px(600, 22) && px(615, 22) && px(608, 15) && px(608, 30) &&
          px(608, 22) == rgb15(0, 0, 31) && px(607, 22) == 0, "polyline + terminator");

    /* gouraud polyline: terminator is checked on the colour word */
    gp0(0x58FF0000);
    gp0(xy(580, 60));
    gp0(0x0000FF00);
    gp0(xy(600, 60));
    gp0(0x000000FF);
    gp0(xy(620, 62));
    gp0(0x50005000);
    gp0(0x68FFFFFF);
    gp0(xy(630, 63));
    CHECK(px(620, 62) == rgb15(31, 0, 0) && px(630, 63) == 0x7FFF, "gouraud polyline");
}

static void test_blend_mask(void)
{
    static const int exp[4] = { 12, 24, 8, 18 };
    int m;
    env_default();
    fill(0x200, 440, 16, 4, 0x808080); /* 16 in each channel */
    for (m = 0; m < 4; m++) {
        gp0(0xE1000000 | ((uint32_t)m << 5));
        gp0(0x6A404040); /* 1x1, semi-transparent, colour 8 */
        gp0(xy(0x200 + m, 440));
    }
    for (m = 0; m < 4; m++)
        CHECK(px(0x200 + m, 440) == rgb15(exp[m], exp[m], exp[m]), "blend mode %d: %04X", m,
              px(0x200 + m, 440));
    gp0(0xE1000000);

    /* mask set */
    gp0(0xE6000001);
    gp0(0x680000F8);
    gp0(xy(0x200, 441));
    CHECK(px(0x200, 441) == (0x8000 | 31), "mask bit set on draw");
    CHECK((gpu_read_status() >> 11 & 3) == 1, "GPUSTAT mask bits");
    /* mask check protects it */
    gp0(0xE6000002);
    gp0(0x6800F800);
    gp0(xy(0x200, 441));
    CHECK(px(0x200, 441) == (0x8000 | 31), "mask check protects pixel");
    /* CPU->VRAM honours mask check too */
    {
        uint16_t d[2] = { 0x1234, 0x0567 };
        upload(0x200, 441, 2, 1, d);
        CHECK(px(0x200, 441) == (0x8000 | 31) && px(0x201, 441) == 0x0567, "A0h honours mask check");
    }
    gp0(0xE6000000);
    gp0(0x6800F800);
    gp0(xy(0x200, 441));
    CHECK(px(0x200, 441) == rgb15(0, 31, 0), "mask cleared/overwritten");
}

static void test_transfers_and_status(void)
{
    uint16_t d[6] = { 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666 };
    uint32_t w0, w1, w2, st;

    env_default();
    upload(900, 480, 3, 2, d);
    CHECK(px(900, 480) == 0x1111 && px(902, 480) == 0x3333 && px(900, 481) == 0x4444 &&
          px(902, 481) == 0x6666, "CPU->VRAM transfer");

    /* wrapping transfer at the right edge */
    {
        uint16_t e[4] = { 0xAAAA & 0x7FFF, 0x0BBB, 0x0CCC, 0x0DDD };
        upload(1023, 470, 2, 2, e);
        CHECK(px(1023, 470) == e[0] && px(0, 470) == e[1] && px(1023, 471) == e[2] && px(0, 471) == e[3],
              "CPU->VRAM wraps horizontally");
    }

    gp0(0xC0000000);
    gp0(xy(900, 480));
    gp0(xy(3, 2));
    st = gpu_read_status();
    CHECK(st & (1u << 27), "GPUSTAT.27 set while VRAM->CPU pending");
    w0 = gpu_read_data();
    w1 = gpu_read_data();
    w2 = gpu_read_data();
    CHECK(w0 == 0x22221111u && w1 == 0x44443333u && w2 == 0x66665555u, "VRAM->CPU data %08X %08X %08X",
          w0, w1, w2);
    CHECK(!(gpu_read_status() & (1u << 27)), "GPUSTAT.27 clear after transfer");

    gp1(0x04000003);
    gp0(0xC0000000);
    gp0(xy(900, 480));
    gp0(xy(2, 1));
    CHECK((gpu_read_status() & (1u << 25)) && ((gpu_read_status() >> 29) & 3) == 3, "DMA dir 3 request");
    {
        uint32_t buf[1];
        gpu_dma_read(buf, 1);
        CHECK(buf[0] == 0x22221111u, "dma read");
    }
    CHECK(!(gpu_read_status() & (1u << 25)), "DMA dir 3 request cleared");
    gp1(0x04000000);

    /* GP1(10h) info */
    gp0(0xE2000000 | 0x12345u);
    gp0(0xE3000000 | (10u << 10) | 20u);
    gp0(0xE4000000 | (300u << 10) | 400u);
    gp0(0xE5000000 | ((uint32_t)(-8 & 0x7FF) << 11) | 16u);
    gp1(0x10000007);
    CHECK(gpu_read_data() == 2, "GPU version 2");
    gp1(0x10000002);
    CHECK(gpu_read_data() == 0x12345u, "texture window readback");
    gp1(0x10000003);
    CHECK(gpu_read_data() == ((10u << 10) | 20u), "draw area TL readback");
    gp1(0x10000004);
    CHECK(gpu_read_data() == ((300u << 10) | 400u), "draw area BR readback");
    gp1(0x10000005);
    CHECK(gpu_read_data() == (((uint32_t)(-8 & 0x7FF) << 11) | 16u), "draw offset readback");
    gp1(0x10000000);
    CHECK(gpu_read_data() == (((uint32_t)(-8 & 0x7FF) << 11) | 16u), "index 0 keeps old latch");

    gp0(0x1F000000);
    CHECK(gpu_read_status() & (1u << 24), "IRQ set");
    gp1(0x02000000);
    CHECK(!(gpu_read_status() & (1u << 24)), "IRQ ack");
    env_default();
}

static void test_display(uint32_t *out)
{
    GpuDisplay d;
    int w, h;

    gp1(0x08000001);                             /* 320x240 NTSC */
    gp1(0x06000000 | (0xC60u << 12) | 0x260u);   /* x1=260h x2=C60h */
    gp1(0x07000000 | (0x100u << 10) | 0x10u);    /* y1=10h y2=100h */
    gp1(0x05000000 | (0u << 10) | 0u);
    gp1(0x03000000);
    gpu_get_display(&d);
    CHECK(d.width == 320 && d.height == 240 && d.enabled && !d.is_24bit && d.hres_mode == 2,
          "display 320x240 (got %dx%d)", d.width, d.height);
    CHECK((gpu_read_status() & 0x007F4000u) == (1u << 17), "GPUSTAT display bits %08X", gpu_read_status());

    gp1(0x08000040); /* 368 mode */
    gp1(0x06000000 | ((0x260u + 368 * 7) << 12) | 0x260u);
    gpu_get_display(&d);
    CHECK(d.width == 368 && d.hres_mode == 1, "368 mode width %d", d.width);

    gp1(0x08000027); /* 640x480 interlaced */
    gp1(0x06000000 | ((0x260u + 640 * 4) << 12) | 0x260u);
    gpu_get_display(&d);
    CHECK(d.width == 640 && d.height == 480 && d.interlaced, "640x480i (%dx%d)", d.width, d.height);
    {
        uint32_t s0 = gpu_read_status(), s1;
        gpu_vblank();
        s1 = gpu_read_status();
        CHECK(((s0 ^ s1) & 0x80002000u) == 0x80002000u, "interlace field toggles bits 13/31");
    }

    gp1(0x07000000 | (0x10u << 10) | 0x10u); /* invalid vertical range -> nominal */
    gp1(0x08000001);
    gpu_get_display(&d);
    CHECK(d.height == 240, "nominal height fallback");

    /* 24-bit conversion */
    {
        uint16_t b[3];
        /* bytes: R0 G0 B0 R1 G1 B1 = 10 20 30 40 50 60 */
        b[0] = 0x2010;
        b[1] = 0x4030;
        b[2] = 0x6050;
        upload(800, 300, 3, 1, b);
        gp1(0x08000011);
        gp1(0x07000000 | (0x100u << 10) | 0x10u);
        gp1(0x06000000 | (0xC60u << 12) | 0x260u);
        gp1(0x05000000 | (300u << 10) | 800u);
        gpu_render_display(out, &w, &h);
        CHECK(out[0] == 0xFF102030u && out[1] == 0xFF405060u, "24-bit display %08X %08X", out[0], out[1]);
    }

    /* final: 15-bit 640x480 view of the top-left of VRAM for the screenshot */
    gp1(0x08000001);
    gp1(0x05000000);
    gpu_render_display(out, &w, &h);
    {
        uint16_t p = px(10, 10);
        uint32_t r = p & 31;
        CHECK((out[10 * w + 10] & 0xFF0000u) >> 16 == ((r << 3) | (r >> 2)), "15-bit expansion");
    }
}

static void test_state(void)
{
    size_t n = gpu_state_size();
    void *buf = malloc(n);
    uint16_t before = px(5, 5);
    env_default();
    gpu_save_state(buf);
    fill(0, 0, 16, 16, 0x00F800);
    CHECK(px(5, 5) != before || before == rgb15(0, 31, 0), "state test fill");
    gpu_load_state(buf);
    CHECK(px(5, 5) == before, "state restore");
    free(buf);
}

static void write_bmp(const char *path, const uint32_t *argb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int stride = (w * 3 + 3) & ~3, y, x;
    uint32_t size = 54u + (uint32_t)(stride * h);
    unsigned char hdr[54];
    unsigned char *row;
    if (!f) {
        printf("cannot write %s\n", path);
        return;
    }
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)size; hdr[3] = (unsigned char)(size >> 8);
    hdr[4] = (unsigned char)(size >> 16); hdr[5] = (unsigned char)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (unsigned char)w; hdr[19] = (unsigned char)(w >> 8);
    hdr[22] = (unsigned char)h; hdr[23] = (unsigned char)(h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    row = (unsigned char *)calloc((size_t)stride, 1);
    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            uint32_t c = argb[y * w + x];
            row[x * 3 + 0] = (unsigned char)c;
            row[x * 3 + 1] = (unsigned char)(c >> 8);
            row[x * 3 + 2] = (unsigned char)(c >> 16);
        }
        fwrite(row, 1, (size_t)stride, f);
    }
    free(row);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *vram_path = argc > 1 ? argv[1] : "gpu_test_vram.bmp";
    const char *disp_path = argc > 2 ? argv[2] : "gpu_test_display.bmp";
    uint32_t *img = (uint32_t *)malloc(1024 * 512 * sizeof(uint32_t));
    int x, y, w, h;

    gpu_init();
    CHECK(gpu_read_status() == 0x14802000u, "GPUSTAT after reset = %08X", gpu_read_status());
    gp1(0x00000000);
    CHECK(gpu_read_status() == 0x14802000u, "GPUSTAT after GP1(00h) = %08X", gpu_read_status());

    test_random_triangles();
    clear_vram();
    test_basic_triangle();
    test_gouraud_and_dither();
    test_textures();
    test_fill_copy();
    test_lines();
    test_blend_mask();
    test_transfers_and_status();
    test_state();

    /* some extra visual content: a big dithered gouraud triangle + semi-transparent overlay */
    env_default();
    gp0(0xE1000200);
    gp0(0x300000FF);
    gp0(xy(20, 120));
    gp0(0x0000FF00);
    gp0(xy(260, 140));
    gp0(0x00FF0000);
    gp0(xy(80, 280));
    gp0(0xE1000000);
    gp0(0x62FFFFFF);
    gp0(xy(60, 160));
    gp0(xy(120, 60));

    for (y = 0; y < 512; y++)
        for (x = 0; x < 1024; x++) {
            uint16_t p = px(x, y);
            uint32_t r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
            img[y * 1024 + x] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) |
                                (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
        }
    write_bmp(vram_path, img, 1024, 512);

    test_display(img);
    gpu_render_display(img, &w, &h);
    write_bmp(disp_path, img, w, h);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    free(img);
    return g_fail ? 1 : 0;
}
