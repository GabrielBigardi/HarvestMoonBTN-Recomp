/*
 * GTE (COP2 Geometry Transformation Engine) - exact fixed-point reimplementation.
 *
 * Written from the psx-spx "Geometry Transformation Engine (GTE)" documentation:
 * register layout and read/write quirks, command formulas, saturation/FLAG rules
 * and the UNR (unsigned Newton-Raphson) perspective divider.
 *
 * Arithmetic model
 *   - MAC1..3 use a 44-bit signed accumulator. After every single addition the sum
 *     is range-checked (FLAG.30-25) and wrapped to 44 bits. The sf shift is applied
 *     when the accumulator is stored into the 32-bit MAC register.
 *   - MAC0 sums are computed wide; FLAG.16/15 report results outside the signed
 *     32-bit range and the MAC0 register keeps the low 32 bits. The limiters fed
 *     from the MAC0 path (SX2/SY2, IR0, OTZ) see the wide value.
 *   - Limiters (IR0..3, SZ3/OTZ, SX2/SY2, color FIFO) clamp and raise their FLAG bit.
 *   - FLAG is cleared at the start of each command; FLAG.31 is the OR of bits 30-23
 *     and 18-13.
 *
 * Relies on two's complement conversions and arithmetic right shift of negative
 * values (true for every supported compiler).
 */
#include "gte.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    int16_t m[3][3];                 /* [row][column] */
} GteMatrix;

typedef struct {
    /* Control registers (cop2r32-63) */
    GteMatrix rt;                    /* rotation matrix              (cnt0-4)   */
    GteMatrix llm;                   /* light source matrix          (cnt8-12)  */
    GteMatrix lcm;                   /* light color matrix           (cnt16-20) */
    int32_t   tr[3];                 /* translation vector           (cnt5-7)   */
    int32_t   bk[3];                 /* background color             (cnt13-15) */
    int32_t   fc[3];                 /* far color                    (cnt21-23) */
    int32_t   ofx, ofy;              /* screen offset, 16.16         (cnt24-25) */
    int32_t   dqb;                   /* depth cueing offset          (cnt28)    */
    uint32_t  flag;                  /* FLAG, bit 31 kept up to date (cnt31)    */
    uint16_t  h;                     /* projection distance, unsigned (cnt26)   */
    int16_t   dqa;                   /* depth cueing coefficient     (cnt27)    */
    int16_t   zsf3, zsf4;            /* average Z scale factors      (cnt29-30) */

    /* Data registers (cop2r0-31) */
    int32_t   mac[4];                /* MAC0..MAC3                   (r24-27)   */
    uint32_t  res1;                  /* prohibited, plain storage    (r23)      */
    uint32_t  lzcs;                  /* leading bit count source     (r30)      */
    int16_t   v[3][3];               /* V0..V2 as (x, y, z)          (r0-5)     */
    int16_t   ir[4];                 /* IR0..IR3                     (r8-11)    */
    int16_t   sxy[3][2];             /* SXY0 (oldest)..SXY2, (x, y)  (r12-14)   */
    uint16_t  sz[4];                 /* SZ0 (oldest)..SZ3            (r16-19)   */
    uint16_t  otz;                   /* average Z                    (r7)       */
    uint8_t   rgbc[4];               /* R, G, B, CODE                (r6)       */
    uint8_t   rgb[3][4];             /* RGB0 (oldest)..RGB2          (r20-22)   */
} GteState;

static GteState g;

/* ------------------------------------------------------------------------- */
/* FLAG bits                                                                  */
/* ------------------------------------------------------------------------- */

#define FLAG_MAC_POS(i)  (1u << (31 - (i)))   /* i=1..3 -> bits 30..28 */
#define FLAG_MAC_NEG(i)  (1u << (28 - (i)))   /* i=1..3 -> bits 27..25 */
#define FLAG_IR(i)       (1u << (25 - (i)))   /* i=1..3 -> bits 24..22 */
#define FLAG_COLOR(i)    (1u << (22 - (i)))   /* i=1..3 (R,G,B) -> bits 21..19 */
#define FLAG_SZ_OTZ      (1u << 18)
#define FLAG_DIVIDE      (1u << 17)
#define FLAG_MAC0_POS    (1u << 16)
#define FLAG_MAC0_NEG    (1u << 15)
#define FLAG_SX          (1u << 14)
#define FLAG_SY          (1u << 13)
#define FLAG_IR0         (1u << 12)
#define FLAG_WRITABLE    0x7FFFF000u          /* bits 30..12 */
#define FLAG_ERROR_SRC   0x7F87E000u          /* bits 30..23 and 18..13 */
#define FLAG_ERROR       0x80000000u

static void flag_update_error(void)
{
    g.flag &= FLAG_WRITABLE;
    if (g.flag & FLAG_ERROR_SRC)
        g.flag |= FLAG_ERROR;
}

/* ------------------------------------------------------------------------- */
/* Accumulators and limiters                                                  */
/* ------------------------------------------------------------------------- */

#define MAC44_MAX  INT64_C(0x7FFFFFFFFFF)
#define MAC44_MIN  (-INT64_C(0x80000000000))

/* Range-check one MAC1..3 partial sum (i = 1..3) and wrap it to 44 bits. */
static int64_t mac44(int i, int64_t v)
{
    if (v > MAC44_MAX)
        g.flag |= FLAG_MAC_POS(i);
    else if (v < MAC44_MIN)
        g.flag |= FLAG_MAC_NEG(i);
    return (int64_t)((uint64_t)v << 20) >> 20;
}

/* Range-check a MAC0 sum against the signed 32-bit range (value is not wrapped). */
static int64_t mac0_check(int64_t v)
{
    if (v > INT64_C(0x7FFFFFFF))
        g.flag |= FLAG_MAC0_POS;
    else if (v < -INT64_C(0x80000000))
        g.flag |= FLAG_MAC0_NEG;
    return v;
}

/* IR1..3 limiter (i = 1..3): -8000h..7FFFh, or 0..7FFFh when lm is set. */
static int16_t sat_ir(int i, int32_t v, int lm)
{
    const int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) {
        g.flag |= FLAG_IR(i);
        return (int16_t)lo;
    }
    if (v > 0x7FFF) {
        g.flag |= FLAG_IR(i);
        return 0x7FFF;
    }
    return (int16_t)v;
}

/* IR0 limiter: 0..1000h. */
static int16_t sat_ir0(int64_t v)
{
    if (v < 0) {
        g.flag |= FLAG_IR0;
        return 0;
    }
    if (v > 0x1000) {
        g.flag |= FLAG_IR0;
        return 0x1000;
    }
    return (int16_t)v;
}

/* SZ3 / OTZ limiter: 0..FFFFh. */
static uint16_t sat_z(int64_t v)
{
    if (v < 0) {
        g.flag |= FLAG_SZ_OTZ;
        return 0;
    }
    if (v > 0xFFFF) {
        g.flag |= FLAG_SZ_OTZ;
        return 0xFFFF;
    }
    return (uint16_t)v;
}

/* SX2 / SY2 limiter: -400h..3FFh. */
static int16_t sat_screen(int64_t v, uint32_t flag_bit)
{
    if (v < -0x400) {
        g.flag |= flag_bit;
        return -0x400;
    }
    if (v > 0x3FF) {
        g.flag |= flag_bit;
        return 0x3FF;
    }
    return (int16_t)v;
}

/* Color FIFO limiter (i = 1..3 for R, G, B): 0..FFh. */
static uint8_t sat_color(int i, int32_t v)
{
    if (v < 0) {
        g.flag |= FLAG_COLOR(i);
        return 0;
    }
    if (v > 0xFF) {
        g.flag |= FLAG_COLOR(i);
        return 0xFF;
    }
    return (uint8_t)v;
}

/* [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] with the lm-selected range. */
static void mac_to_ir(int lm)
{
    for (int i = 1; i <= 3; i++)
        g.ir[i] = sat_ir(i, g.mac[i], lm);
}

/* Color FIFO = [MAC1/16, MAC2/16, MAC3/16, CODE] (older entries move down). */
static void push_color(void)
{
    memcpy(g.rgb[0], g.rgb[1], sizeof g.rgb[0]);
    memcpy(g.rgb[1], g.rgb[2], sizeof g.rgb[1]);
    for (int i = 1; i <= 3; i++)
        g.rgb[2][i - 1] = sat_color(i, g.mac[i] >> 4);
    g.rgb[2][3] = g.rgbc[3];
}

static void push_sz(uint16_t z)
{
    g.sz[0] = g.sz[1];
    g.sz[1] = g.sz[2];
    g.sz[2] = g.sz[3];
    g.sz[3] = z;
}

static void push_sxy(int16_t x, int16_t y)
{
    memcpy(g.sxy[0], g.sxy[1], sizeof g.sxy[0]);
    memcpy(g.sxy[1], g.sxy[2], sizeof g.sxy[1]);
    g.sxy[2][0] = x;
    g.sxy[2][1] = y;
}

/* ------------------------------------------------------------------------- */
/* Matrix * vector                                                            */
/* ------------------------------------------------------------------------- */

/* acc + M[r]*V for one row, checking/wrapping after each of the three additions. */
static int64_t mat_row(int i, int64_t acc, const int16_t row[3], const int16_t vec[3])
{
    acc = mac44(i, acc + (int32_t)row[0] * vec[0]);
    acc = mac44(i, acc + (int32_t)row[1] * vec[1]);
    acc = mac44(i, acc + (int32_t)row[2] * vec[2]);
    return acc;
}

/* [IR] = [MAC] = (T*1000h + M*V) SAR shift; t == NULL means no translation. */
static void mat_vec(const GteMatrix *m, const int16_t vec[3], const int32_t *t, int shift, int lm)
{
    for (int r = 0; r < 3; r++) {
        int64_t acc = t ? (int64_t)t[r] * 0x1000 : 0;
        acc = mat_row(r + 1, acc, m->m[r], vec);
        g.mac[r + 1] = (int32_t)(acc >> shift);
    }
    mac_to_ir(lm);
}

/*
 * MVMVA with cv=2 (far color): the hardware drops the first two terms.
 * MAC = (M12*V2 + M13*V3) SAR shift, but FC*1000h + M11*V1 is still evaluated far
 * enough to raise its MAC overflow flag and its IR saturation flag (as if lm=0).
 */
static void mat_vec_far_color_bug(const GteMatrix *m, const int16_t vec[3], int shift, int lm)
{
    for (int r = 0; r < 3; r++) {
        const int i = r + 1;
        int64_t lost = mac44(i, (int64_t)g.fc[r] * 0x1000 + (int32_t)m->m[r][0] * vec[0]);
        (void)sat_ir(i, (int32_t)(lost >> shift), 0);

        int64_t acc = mac44(i, (int32_t)m->m[r][1] * vec[1]);
        acc = mac44(i, acc + (int32_t)m->m[r][2] * vec[2]);
        g.mac[i] = (int32_t)(acc >> shift);
    }
    mac_to_ir(lm);
}

/* ------------------------------------------------------------------------- */
/* UNR perspective division                                                   */
/* ------------------------------------------------------------------------- */

/* unr_table[i] = max(0, (40000h/(i+100h)+1)/2 - 101h), i = 000h..100h */
#define UNR_RAW(i)   ((0x40000 / ((i) + 0x100) + 1) / 2 - 0x101)
#define UNR(i)       ((uint8_t)(UNR_RAW(i) > 0 ? UNR_RAW(i) : 0))
#define UNR_ROW(b)   UNR((b) + 0x0), UNR((b) + 0x1), UNR((b) + 0x2), UNR((b) + 0x3), \
                     UNR((b) + 0x4), UNR((b) + 0x5), UNR((b) + 0x6), UNR((b) + 0x7), \
                     UNR((b) + 0x8), UNR((b) + 0x9), UNR((b) + 0xA), UNR((b) + 0xB), \
                     UNR((b) + 0xC), UNR((b) + 0xD), UNR((b) + 0xE), UNR((b) + 0xF)

static const uint8_t unr_table[0x101] = {
    UNR_ROW(0x00), UNR_ROW(0x10), UNR_ROW(0x20), UNR_ROW(0x30),
    UNR_ROW(0x40), UNR_ROW(0x50), UNR_ROW(0x60), UNR_ROW(0x70),
    UNR_ROW(0x80), UNR_ROW(0x90), UNR_ROW(0xA0), UNR_ROW(0xB0),
    UNR_ROW(0xC0), UNR_ROW(0xD0), UNR_ROW(0xE0), UNR_ROW(0xF0),
    UNR(0x100)
};

static int count_leading_zeros16(uint16_t x)
{
    int n = 0;
    while (n < 16 && !(x & 0x8000u)) {
        x = (uint16_t)(x << 1);
        n++;
    }
    return n;
}

/* (H*20000h/SZ3+1)/2 as computed by the hardware, 0..1FFFFh. */
static uint32_t unr_divide(uint16_t h, uint16_t sz3)
{
    if ((uint32_t)h >= (uint32_t)sz3 * 2) {
        g.flag |= FLAG_DIVIDE;
        return 0x1FFFF;
    }
    /* here sz3 >= 1 */
    const int z = count_leading_zeros16(sz3);
    const uint32_t n = (uint32_t)h << z;                       /* < 20000h            */
    uint32_t d = (uint32_t)sz3 << z;                           /* 8000h..FFFFh        */
    const uint32_t u = unr_table[(d - 0x7FC0) >> 7] + 0x101;  /* 101h..200h          */
    d = (0x2000080 - d * u) >> 8;                              /* 10000h..0FF01h      */
    d = (0x0000080 + d * u) >> 8;                              /* 20000h..10000h      */
    const uint64_t q = ((uint64_t)n * d + 0x8000) >> 16;
    return q > 0x1FFFF ? 0x1FFFF : (uint32_t)q;
}

/* ------------------------------------------------------------------------- */
/* Coordinate commands                                                        */
/* ------------------------------------------------------------------------- */

/* One RTPS step on vector vec; depth cueing (MAC0/IR0) only when depth_cue is set. */
static void rtp_vertex(const int16_t vec[3], int shift, int lm, int depth_cue)
{
    int64_t acc[3];
    for (int r = 0; r < 3; r++) {
        acc[r] = mat_row(r + 1, (int64_t)g.tr[r] * 0x1000, g.rt.m[r], vec);
        g.mac[r + 1] = (int32_t)(acc[r] >> shift);
    }

    /* IR1/IR2 saturate normally. IR3 is stored per lm, but FLAG.22 is decided
     * from the unshifted MAC3 SAR 12 against -8000h..7FFFh (as if lm=0). */
    g.ir[1] = sat_ir(1, g.mac[1], lm);
    g.ir[2] = sat_ir(2, g.mac[2], lm);
    {
        const int64_t z12 = acc[2] >> 12;
        const int32_t lo = lm ? 0 : -0x8000;
        const int32_t m3 = g.mac[3];
        if (z12 < -0x8000 || z12 > 0x7FFF)
            g.flag |= FLAG_IR(3);
        g.ir[3] = (int16_t)(m3 < lo ? lo : (m3 > 0x7FFF ? 0x7FFF : m3));
    }

    /* SZ3 = MAC3 SAR ((1-sf)*12), taken from the full accumulator. */
    push_sz(sat_z(acc[2] >> 12));

    const int64_t q = unr_divide(g.h, g.sz[3]);
    const int64_t sx = mac0_check((int64_t)g.ofx + (int64_t)g.ir[1] * q);
    const int64_t sy = mac0_check((int64_t)g.ofy + (int64_t)g.ir[2] * q);
    push_sxy(sat_screen(sx >> 16, FLAG_SX), sat_screen(sy >> 16, FLAG_SY));

    if (depth_cue) {
        const int64_t dq = mac0_check((int64_t)g.dqb + (int64_t)g.dqa * q);
        g.mac[0] = (int32_t)dq;
        g.ir[0] = sat_ir0(dq >> 12);
    }
}

static void cmd_rtps(int shift, int lm)
{
    rtp_vertex(g.v[0], shift, lm, 1);
}

static void cmd_rtpt(int shift, int lm)
{
    rtp_vertex(g.v[0], shift, lm, 0);
    rtp_vertex(g.v[1], shift, lm, 0);
    rtp_vertex(g.v[2], shift, lm, 1);   /* MAC0/IR0 come from the last vertex only */
}

static void cmd_nclip(void)
{
    const int64_t sx0 = g.sxy[0][0], sy0 = g.sxy[0][1];
    const int64_t sx1 = g.sxy[1][0], sy1 = g.sxy[1][1];
    const int64_t sx2 = g.sxy[2][0], sy2 = g.sxy[2][1];
    const int64_t v = sx0 * sy1 + sx1 * sy2 + sx2 * sy0 - sx0 * sy2 - sx1 * sy0 - sx2 * sy1;
    g.mac[0] = (int32_t)mac0_check(v);
}

static void avsz(int16_t zsf, int64_t sum)
{
    const int64_t v = mac0_check((int64_t)zsf * sum);
    g.mac[0] = (int32_t)v;
    g.otz = sat_z(v >> 12);
}

static void cmd_avsz3(void)
{
    avsz(g.zsf3, (int64_t)g.sz[1] + g.sz[2] + g.sz[3]);
}

static void cmd_avsz4(void)
{
    avsz(g.zsf4, (int64_t)g.sz[0] + g.sz[1] + g.sz[2] + g.sz[3]);
}

/* ------------------------------------------------------------------------- */
/* General purpose commands                                                   */
/* ------------------------------------------------------------------------- */

static void cmd_mvmva(uint32_t op, int shift, int lm)
{
    const int mx = (op >> 17) & 3;
    const int vx = (op >> 15) & 3;
    const int cv = (op >> 13) & 3;

    GteMatrix garbage;
    const GteMatrix *m;
    switch (mx) {
    case 0:  m = &g.rt;  break;
    case 1:  m = &g.llm; break;
    case 2:  m = &g.lcm; break;
    default: {
        /* Reserved: -R*10h, +R*10h, IR0 / RT13 x3 / RT22 x3 */
        const int16_t r16 = (int16_t)(g.rgbc[0] << 4);
        garbage.m[0][0] = (int16_t)-r16;
        garbage.m[0][1] = r16;
        garbage.m[0][2] = g.ir[0];
        garbage.m[1][0] = garbage.m[1][1] = garbage.m[1][2] = g.rt.m[0][2];
        garbage.m[2][0] = garbage.m[2][1] = garbage.m[2][2] = g.rt.m[1][1];
        m = &garbage;
        break;
    }
    }

    int16_t vec[3];
    if (vx < 3)
        memcpy(vec, g.v[vx], sizeof vec);
    else
        memcpy(vec, &g.ir[1], sizeof vec);

    switch (cv) {
    case 0:  mat_vec(m, vec, g.tr, shift, lm); break;
    case 1:  mat_vec(m, vec, g.bk, shift, lm); break;
    case 2:  mat_vec_far_color_bug(m, vec, shift, lm); break;
    default: mat_vec(m, vec, NULL, shift, lm); break;
    }
}

static void cmd_sqr(int shift, int lm)
{
    for (int i = 1; i <= 3; i++)
        g.mac[i] = (int32_t)(mac44(i, (int64_t)g.ir[i] * g.ir[i]) >> shift);
    mac_to_ir(lm);
}

/* Cross product of IR with D = (RT11, RT22, RT33). */
static void cmd_op(int shift, int lm)
{
    const int64_t d1 = g.rt.m[0][0], d2 = g.rt.m[1][1], d3 = g.rt.m[2][2];
    const int64_t ir1 = g.ir[1], ir2 = g.ir[2], ir3 = g.ir[3];
    g.mac[1] = (int32_t)(mac44(1, ir3 * d2 - ir2 * d3) >> shift);
    g.mac[2] = (int32_t)(mac44(2, ir1 * d3 - ir3 * d1) >> shift);
    g.mac[3] = (int32_t)(mac44(3, ir2 * d1 - ir1 * d2) >> shift);
    mac_to_ir(lm);
}

/* ------------------------------------------------------------------------- */
/* Color commands                                                             */
/* ------------------------------------------------------------------------- */

/* [MAC] = base SAR shift, [IR] = [MAC] (lm), color FIFO push. */
static void color_finish(const int64_t base[3], int shift, int lm)
{
    for (int i = 1; i <= 3; i++)
        g.mac[i] = (int32_t)(mac44(i, base[i - 1]) >> shift);
    mac_to_ir(lm);
    push_color();
}

/*
 * [MAC] = base + (FC - base) * IR0, then SAR shift, [IR] = [MAC] (lm), color push.
 * The intermediate (FC*1000h - base) SAR shift is saturated as if lm=0.
 */
static void color_depth_cue(const int64_t base[3], int shift, int lm)
{
    for (int i = 1; i <= 3; i++) {
        const int64_t diff = mac44(i, (int64_t)g.fc[i - 1] * 0x1000 - base[i - 1]);
        g.mac[i] = (int32_t)(diff >> shift);
        const int16_t t = sat_ir(i, g.mac[i], 0);
        const int64_t acc = mac44(i, base[i - 1] + (int64_t)g.ir[0] * t);
        g.mac[i] = (int32_t)(acc >> shift);
    }
    mac_to_ir(lm);
    push_color();
}

/* base = [R*IR1, G*IR2, B*IR3] SHL 4 using RGBC and the current IR. */
static void base_rgb_times_ir(int64_t base[3])
{
    for (int i = 0; i < 3; i++)
        base[i] = ((int64_t)g.rgbc[i] << 4) * g.ir[i + 1];
}

/* [IR] = [MAC] = (BK*1000h + LCM*IR) SAR shift */
static void light_color_matrix(int shift, int lm)
{
    int16_t vec[3];
    memcpy(vec, &g.ir[1], sizeof vec);
    mat_vec(&g.lcm, vec, g.bk, shift, lm);
}

/* [IR] = [MAC] = (LLM*Vn) SAR shift, then the light color matrix step. */
static void normal_to_light(int n, int shift, int lm)
{
    mat_vec(&g.llm, g.v[n], NULL, shift, lm);
    light_color_matrix(shift, lm);
}

static void nc_single(int n, int shift, int lm)          /* NCS / NCT */
{
    normal_to_light(n, shift, lm);
    push_color();
}

static void ncc_single(int n, int shift, int lm)         /* NCCS / NCCT */
{
    int64_t base[3];
    normal_to_light(n, shift, lm);
    base_rgb_times_ir(base);
    color_finish(base, shift, lm);
}

static void ncd_single(int n, int shift, int lm)         /* NCDS / NCDT */
{
    int64_t base[3];
    normal_to_light(n, shift, lm);
    base_rgb_times_ir(base);
    color_depth_cue(base, shift, lm);
}

static void cmd_ncs(int shift, int lm)  { nc_single(0, shift, lm); }
static void cmd_nccs(int shift, int lm) { ncc_single(0, shift, lm); }
static void cmd_ncds(int shift, int lm) { ncd_single(0, shift, lm); }

static void cmd_nct(int shift, int lm)
{
    for (int n = 0; n < 3; n++)
        nc_single(n, shift, lm);
}

static void cmd_ncct(int shift, int lm)
{
    for (int n = 0; n < 3; n++)
        ncc_single(n, shift, lm);
}

static void cmd_ncdt(int shift, int lm)
{
    for (int n = 0; n < 3; n++)
        ncd_single(n, shift, lm);
}

static void cmd_cc(int shift, int lm)
{
    int64_t base[3];
    light_color_matrix(shift, lm);
    base_rgb_times_ir(base);
    color_finish(base, shift, lm);
}

static void cmd_cdp(int shift, int lm)
{
    int64_t base[3];
    light_color_matrix(shift, lm);
    base_rgb_times_ir(base);
    color_depth_cue(base, shift, lm);
}

static void cmd_dcpl(int shift, int lm)
{
    int64_t base[3];
    base_rgb_times_ir(base);
    color_depth_cue(base, shift, lm);
}

static void cmd_dpcs(int shift, int lm)
{
    int64_t base[3];
    for (int i = 0; i < 3; i++)
        base[i] = (int64_t)g.rgbc[i] << 16;
    color_depth_cue(base, shift, lm);
}

/* Three DPCS steps reading R,G,B from the bottom of the color FIFO (RGB0). */
static void cmd_dpct(int shift, int lm)
{
    for (int n = 0; n < 3; n++) {
        int64_t base[3];
        for (int i = 0; i < 3; i++)
            base[i] = (int64_t)g.rgb[0][i] << 16;
        color_depth_cue(base, shift, lm);
    }
}

static void cmd_intpl(int shift, int lm)
{
    int64_t base[3];
    for (int i = 0; i < 3; i++)
        base[i] = (int64_t)g.ir[i + 1] * 0x1000;
    color_depth_cue(base, shift, lm);
}

static void cmd_gpf(int shift, int lm)
{
    int64_t base[3];
    for (int i = 0; i < 3; i++)
        base[i] = (int64_t)g.ir[0] * g.ir[i + 1];
    color_finish(base, shift, lm);
}

static void cmd_gpl(int shift, int lm)
{
    int64_t base[3];
    for (int i = 0; i < 3; i++)
        base[i] = (int64_t)((uint64_t)(int64_t)g.mac[i + 1] << shift) + (int64_t)g.ir[0] * g.ir[i + 1];
    color_finish(base, shift, lm);
}

/* ------------------------------------------------------------------------- */
/* Public API: commands                                                       */
/* ------------------------------------------------------------------------- */

void gte_command(uint32_t op)
{
    const int shift = (op & (1u << 19)) ? 12 : 0;   /* sf */
    const int lm = (op >> 10) & 1;

    g.flag = 0;

    switch (op & 0x3F) {
    case 0x01: cmd_rtps(shift, lm);      break;
    case 0x06: cmd_nclip();              break;
    case 0x0C: cmd_op(shift, lm);        break;
    case 0x10: cmd_dpcs(shift, lm);      break;
    case 0x11: cmd_intpl(shift, lm);     break;
    case 0x12: cmd_mvmva(op, shift, lm); break;
    case 0x13: cmd_ncds(shift, lm);      break;
    case 0x14: cmd_cdp(shift, lm);       break;
    case 0x16: cmd_ncdt(shift, lm);      break;
    case 0x1B: cmd_nccs(shift, lm);      break;
    case 0x1C: cmd_cc(shift, lm);        break;
    case 0x1E: cmd_ncs(shift, lm);       break;
    case 0x20: cmd_nct(shift, lm);       break;
    case 0x28: cmd_sqr(shift, lm);       break;
    case 0x29: cmd_dcpl(shift, lm);      break;
    case 0x2A: cmd_dpct(shift, lm);      break;
    case 0x2D: cmd_avsz3();              break;
    case 0x2E: cmd_avsz4();              break;
    case 0x30: cmd_rtpt(shift, lm);      break;
    case 0x3D: cmd_gpf(shift, lm);       break;
    case 0x3E: cmd_gpl(shift, lm);       break;
    case 0x3F: cmd_ncct(shift, lm);      break;
    default:                             break;   /* undocumented: FLAG cleared only */
    }

    flag_update_error();
}

/* ------------------------------------------------------------------------- */
/* Public API: registers                                                      */
/* ------------------------------------------------------------------------- */

static uint32_t pack16(int16_t lo, int16_t hi)
{
    return (uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16);
}

static uint32_t sext16(int16_t v)
{
    return (uint32_t)(int32_t)v;
}

static int16_t lo16(uint32_t v)
{
    return (int16_t)(uint16_t)v;
}

static int16_t hi16(uint32_t v)
{
    return (int16_t)(uint16_t)(v >> 16);
}

static uint32_t pack_bytes(const uint8_t b[4])
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void unpack_bytes(uint8_t b[4], uint32_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

/* Matrix registers: k=0..3 hold elements 2k (lsbs) and 2k+1 (msbs) in row-major
 * order, k=4 holds element 8 alone (read back sign-extended). */
static uint32_t matrix_read(const GteMatrix *m, int k)
{
    const int e = 2 * k;
    if (k == 4)
        return sext16(m->m[2][2]);
    return pack16(m->m[e / 3][e % 3], m->m[(e + 1) / 3][(e + 1) % 3]);
}

static void matrix_write(GteMatrix *m, int k, uint32_t v)
{
    const int e = 2 * k;
    if (k == 4) {
        m->m[2][2] = lo16(v);
        return;
    }
    m->m[e / 3][e % 3] = lo16(v);
    m->m[(e + 1) / 3][(e + 1) % 3] = hi16(v);
}

/* ORGB (and IRGB reads): IR1..3 / 80h saturated to 0..1Fh, no flags. */
static uint32_t read_orgb(void)
{
    uint32_t r = 0;
    for (int i = 0; i < 3; i++) {
        int32_t c = g.ir[i + 1] >> 7;
        if (c < 0)
            c = 0;
        else if (c > 0x1F)
            c = 0x1F;
        r |= (uint32_t)c << (5 * i);
    }
    return r;
}

/* LZCR: leading zeroes of LZCS if positive, leading ones if negative (1..32). */
static uint32_t read_lzcr(void)
{
    uint32_t x = g.lzcs;
    uint32_t n = 0;
    if (x & 0x80000000u)
        x = ~x;
    while (n < 32 && !(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}

uint32_t gte_read_data(int reg)
{
    reg &= 31;
    switch (reg) {
    case 0: case 2: case 4:
        return pack16(g.v[reg >> 1][0], g.v[reg >> 1][1]);
    case 1: case 3: case 5:
        return sext16(g.v[reg >> 1][2]);
    case 6:
        return pack_bytes(g.rgbc);
    case 7:
        return g.otz;
    case 8: case 9: case 10: case 11:
        return sext16(g.ir[reg - 8]);
    case 12: case 13: case 14:
        return pack16(g.sxy[reg - 12][0], g.sxy[reg - 12][1]);
    case 15:                                    /* SXYP mirrors SXY2 */
        return pack16(g.sxy[2][0], g.sxy[2][1]);
    case 16: case 17: case 18: case 19:
        return g.sz[reg - 16];
    case 20: case 21: case 22:
        return pack_bytes(g.rgb[reg - 20]);
    case 23:
        return g.res1;
    case 24: case 25: case 26: case 27:
        return (uint32_t)g.mac[reg - 24];
    case 28: case 29:                           /* IRGB reads like ORGB */
        return read_orgb();
    case 30:
        return g.lzcs;
    default:                                    /* 31: LZCR */
        return read_lzcr();
    }
}

void gte_write_data(int reg, uint32_t v)
{
    reg &= 31;
    switch (reg) {
    case 0: case 2: case 4:
        g.v[reg >> 1][0] = lo16(v);
        g.v[reg >> 1][1] = hi16(v);
        break;
    case 1: case 3: case 5:
        g.v[reg >> 1][2] = lo16(v);
        break;
    case 6:
        unpack_bytes(g.rgbc, v);
        break;
    case 7:
        g.otz = (uint16_t)v;
        break;
    case 8: case 9: case 10: case 11:
        g.ir[reg - 8] = lo16(v);
        break;
    case 12: case 13: case 14:
        g.sxy[reg - 12][0] = lo16(v);
        g.sxy[reg - 12][1] = hi16(v);
        break;
    case 15:                                    /* SXYP: move-on-write */
        push_sxy(lo16(v), hi16(v));
        break;
    case 16: case 17: case 18: case 19:
        g.sz[reg - 16] = (uint16_t)v;
        break;
    case 20: case 21: case 22:
        unpack_bytes(g.rgb[reg - 20], v);
        break;
    case 23:
        g.res1 = v;
        break;
    case 24: case 25: case 26: case 27:
        g.mac[reg - 24] = (int32_t)v;
        break;
    case 28:                                    /* IRGB: 5:5:5 -> IR1..3 * 80h */
        g.ir[1] = (int16_t)((v & 0x1F) << 7);
        g.ir[2] = (int16_t)(((v >> 5) & 0x1F) << 7);
        g.ir[3] = (int16_t)(((v >> 10) & 0x1F) << 7);
        break;
    case 30:
        g.lzcs = v;
        break;
    default:                                    /* 29 ORGB, 31 LZCR: read-only */
        break;
    }
}

uint32_t gte_read_ctrl(int reg)
{
    reg &= 31;
    switch (reg) {
    case 0: case 1: case 2: case 3: case 4:
        return matrix_read(&g.rt, reg);
    case 5: case 6: case 7:
        return (uint32_t)g.tr[reg - 5];
    case 8: case 9: case 10: case 11: case 12:
        return matrix_read(&g.llm, reg - 8);
    case 13: case 14: case 15:
        return (uint32_t)g.bk[reg - 13];
    case 16: case 17: case 18: case 19: case 20:
        return matrix_read(&g.lcm, reg - 16);
    case 21: case 22: case 23:
        return (uint32_t)g.fc[reg - 21];
    case 24:
        return (uint32_t)g.ofx;
    case 25:
        return (uint32_t)g.ofy;
    case 26:                                    /* hardware bug: H reads sign-extended */
        return sext16((int16_t)g.h);
    case 27:
        return sext16(g.dqa);
    case 28:
        return (uint32_t)g.dqb;
    case 29:
        return sext16(g.zsf3);
    case 30:
        return sext16(g.zsf4);
    default:                                    /* 31: FLAG */
        return g.flag;
    }
}

void gte_write_ctrl(int reg, uint32_t v)
{
    reg &= 31;
    switch (reg) {
    case 0: case 1: case 2: case 3: case 4:
        matrix_write(&g.rt, reg, v);
        break;
    case 5: case 6: case 7:
        g.tr[reg - 5] = (int32_t)v;
        break;
    case 8: case 9: case 10: case 11: case 12:
        matrix_write(&g.llm, reg - 8, v);
        break;
    case 13: case 14: case 15:
        g.bk[reg - 13] = (int32_t)v;
        break;
    case 16: case 17: case 18: case 19: case 20:
        matrix_write(&g.lcm, reg - 16, v);
        break;
    case 21: case 22: case 23:
        g.fc[reg - 21] = (int32_t)v;
        break;
    case 24:
        g.ofx = (int32_t)v;
        break;
    case 25:
        g.ofy = (int32_t)v;
        break;
    case 26:
        g.h = (uint16_t)v;
        break;
    case 27:
        g.dqa = lo16(v);
        break;
    case 28:
        g.dqb = (int32_t)v;
        break;
    case 29:
        g.zsf3 = lo16(v);
        break;
    case 30:
        g.zsf4 = lo16(v);
        break;
    default:                                    /* 31: FLAG, bits 30..12 writable */
        g.flag = v;
        flag_update_error();
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Public API: reset / save states                                            */
/* ------------------------------------------------------------------------- */

void gte_reset(void)
{
    memset(&g, 0, sizeof g);
}

size_t gte_state_size(void)
{
    return sizeof g;
}

void gte_save_state(void *dst)
{
    memcpy(dst, &g, sizeof g);
}

void gte_load_state(const void *src)
{
    memcpy(&g, src, sizeof g);
}
