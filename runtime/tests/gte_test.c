/*
 * Standalone GTE test: hand-derived cases from the psx-spx GTE documentation.
 *
 * Build (from the project root):
 *   clang -std=c11 -O2 -Wall -Wextra -Iruntime runtime/gte.c runtime/tests/gte_test.c -o gte_test.exe
 */
#include "gte.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fails;

static void check(const char *what, uint32_t got, uint32_t want)
{
    g_checks++;
    if (got != want) {
        g_fails++;
        printf("FAIL  %-44s got %08X want %08X\n", what, (unsigned)got, (unsigned)want);
    }
}

/* register shorthands */
#define D(r)        gte_read_data(r)
#define C(r)        gte_read_ctrl(r)
#define WD(r, v)    gte_write_data((r), (uint32_t)(v))
#define WC(r, v)    gte_write_ctrl((r), (uint32_t)(v))
#define XY(x, y)    (((uint32_t)(uint16_t)(int16_t)(y) << 16) | (uint16_t)(int16_t)(x))
#define FLAG()      C(31)

/* COP2 command words */
#define OP_RTPS   0x0180001u
#define OP_RTPT   0x0280030u
#define OP_NCLIP  0x1400006u
#define OP_AVSZ3  0x158002Du
#define OP_AVSZ4  0x168002Eu
#define OP_SQR    0x0A00428u
#define OP_OP     0x170000Cu
#define OP_DPCS   0x0780010u
#define OP_DPCT   0x0F8002Au
#define OP_INTPL  0x0980011u
#define OP_GPF    0x190003Du
#define OP_GPL    0x1A0003Eu
#define OP_NCS    0x0C8041Eu
#define OP_NCCS   0x108041Bu
#define OP_NCDS   0x0E80413u
#define SF        (1u << 19)
#define LM        (1u << 10)
#define MVMVA(sf, mx, v, cv, lm) \
    (0x0400012u | ((uint32_t)(sf) << 19) | ((uint32_t)(mx) << 17) | ((uint32_t)(v) << 15) | \
     ((uint32_t)(cv) << 13) | ((uint32_t)(lm) << 10))

static void set_identity(int base)          /* matrix at ctrl base..base+4 = 1.0 diagonal */
{
    WC(base + 0, 0x00001000);                /* M11=1000h M12=0 */
    WC(base + 1, 0);                         /* M13=0     M21=0 */
    WC(base + 2, 0x00001000);                /* M22=1000h M23=0 */
    WC(base + 3, 0);                         /* M31=0     M32=0 */
    WC(base + 4, 0x1000);                    /* M33=1000h */
}

/* ------------------------------------------------------------------------- */

static void test_registers(void)
{
    gte_reset();
    WD(1, 0x12348000);        check("VZ0 sign-extends", D(1), 0xFFFF8000);
    WD(0, 0x8001FFFF);        check("VXY0 raw", D(0), 0x8001FFFF);
    WD(7, 0xFFFF1234);        check("OTZ zero-extends", D(7), 0x1234);
    WD(16, 0x12348000);       check("SZ0 zero-extends", D(16), 0x8000);
    WD(9, 0x00018000);        check("IR1 sign-extends", D(9), 0xFFFF8000);
    WD(8, 0x00000FFF);        check("IR0 positive", D(8), 0x0FFF);
    WD(6, 0xAABBCCDD);        check("RGBC raw", D(6), 0xAABBCCDD);
    WD(23, 0xDEADBEEF);       check("RES1 raw", D(23), 0xDEADBEEF);
    WD(24, 0x80000000);       check("MAC0 raw", D(24), 0x80000000);
    WD(30, 0xFFFFFFFF);       check("LZCS raw", D(30), 0xFFFFFFFF);

    WC(26, 0x8000);           check("H reads sign-extended (bug)", C(26), 0xFFFF8000);
    WC(26, 0x00017FFF);       check("H 16-bit", C(26), 0x7FFF);
    WC(27, 0x0000FF00);       check("DQA sign-extends", C(27), 0xFFFFFF00);
    WC(28, 0x89ABCDEF);       check("DQB 32-bit", C(28), 0x89ABCDEF);
    WC(29, 0x8000);           check("ZSF3 sign-extends", C(29), 0xFFFF8000);
    WC(30, 0x12347FFF);       check("ZSF4 16-bit", C(30), 0x7FFF);
    WC(4, 0x1234F000);        check("RT33 sign-extends", C(4), 0xFFFFF000);
    WC(12, 0x00000800);       check("L33", C(12), 0x0800);
    WC(20, 0xFFFF8001);       check("LB3", C(20), 0xFFFF8001);
    WC(1, 0x56781234);        check("RT13/RT21 packed", C(1), 0x56781234);
    WC(24, 0x80000001);       check("OFX 32-bit", C(24), 0x80000001);

    WC(31, 0xFFFFFFFF);       check("FLAG write all", FLAG(), 0xFFFFF000);
    WC(31, 0x00001000);       check("FLAG bit12 no error", FLAG(), 0x00001000);
    WC(31, 0x00080000);       check("FLAG bit19 no error", FLAG(), 0x00080000);
    WC(31, 0x00040000);       check("FLAG bit18 -> bit31", FLAG(), 0x80040000);
    WC(31, 0x00800000);       check("FLAG bit23 -> bit31", FLAG(), 0x80800000);
    WC(31, 0x80000000);       check("FLAG bit31 not writable", FLAG(), 0);

    WD(12, XY(1, 1)); WD(13, XY(2, 2)); WD(14, XY(3, 3));
    WD(15, XY(4, -4));
    check("SXYP push SXY0", D(12), XY(2, 2));
    check("SXYP push SXY1", D(13), XY(3, 3));
    check("SXYP push SXY2", D(14), XY(4, -4));
    check("SXYP mirrors SXY2", D(15), XY(4, -4));
    WD(14, XY(9, 9));
    check("SXY2 write no push", D(13), XY(3, 3));

    WD(9, 0x100); WD(10, 0x200); WD(11, 0x300);
    WD(29, 0x7FFF);           check("ORGB write ignored", D(9), 0x100);
    WD(30, 0); WD(31, 5);     check("LZCR write ignored", D(31), 32);
}

static void test_lzc(void)
{
    static const struct { uint32_t in, out; } t[] = {
        { 0x00000000, 32 }, { 0xFFFFFFFF, 32 }, { 0x00000001, 31 }, { 0xFFFFFFFE, 31 },
        { 0x80000000, 1 },  { 0x7FFFFFFF, 1 },  { 0x00FF0000, 8 },  { 0xFF00FFFF, 8 },
        { 0xFFFF0000, 16 }, { 0x00008000, 16 }, { 0x40000000, 1 },  { 0xBFFFFFFF, 1 },
    };
    gte_reset();
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        char name[64];
        WD(30, t[i].in);
        snprintf(name, sizeof name, "LZCR(%08X)", (unsigned)t[i].in);
        check(name, D(31), t[i].out);
    }
}

static void test_irgb(void)
{
    gte_reset();
    WD(28, 0x7FFF);
    check("IRGB 7FFF -> IR1", D(9), 0xF80);
    check("IRGB 7FFF -> IR2", D(10), 0xF80);
    check("IRGB 7FFF -> IR3", D(11), 0xF80);
    check("ORGB round trip 7FFF", D(29), 0x7FFF);
    check("IRGB reads as ORGB", D(28), 0x7FFF);

    WD(28, 0xFFFF1234);
    check("IRGB 1234 -> IR1", D(9), 0xA00);
    check("IRGB 1234 -> IR2", D(10), 0x880);
    check("IRGB 1234 -> IR3", D(11), 0x200);
    check("ORGB round trip 1234", D(29), 0x1234);

    WD(9, -5); WD(10, 0x7FFF); WD(11, 0x0FFF);
    check("ORGB saturates (no flags)", D(29), 0x7FE0);
    check("ORGB sets no FLAG", FLAG(), 0);
}

static void setup_rtps_identity(void)
{
    gte_reset();
    set_identity(0);
    WC(24, 160 << 16);        /* OFX */
    WC(25, 120 << 16);        /* OFY */
    WC(26, 200);              /* H */
}

static void test_rtps(void)
{
    /* Identity rotation, H == SZ3 -> quotient exactly 10000h. */
    setup_rtps_identity();
    WC(27, 0x10);             /* DQA */
    WC(28, 0x100000);         /* DQB */
    WD(16, 1); WD(17, 2); WD(18, 3); WD(19, 4);
    WD(12, XY(11, 11)); WD(13, XY(22, 22)); WD(14, XY(33, 33));
    WD(0, XY(100, 50)); WD(1, 200);
    gte_command(OP_RTPS);
    check("RTPS MAC1", D(25), 100);
    check("RTPS MAC2", D(26), 50);
    check("RTPS MAC3", D(27), 200);
    check("RTPS IR1", D(9), 100);
    check("RTPS IR2", D(10), 50);
    check("RTPS IR3", D(11), 200);
    check("RTPS SZ0 (shifted)", D(16), 2);
    check("RTPS SZ1 (shifted)", D(17), 3);
    check("RTPS SZ2 (shifted)", D(18), 4);
    check("RTPS SZ3", D(19), 200);
    check("RTPS SXY0 (shifted)", D(12), XY(22, 22));
    check("RTPS SXY1 (shifted)", D(13), XY(33, 33));
    check("RTPS SXY2 = (260,170)", D(14), XY(260, 170));
    check("RTPS MAC0 = DQB+DQA*10000h", D(24), 0x200000);
    check("RTPS IR0", D(8), 0x200);
    check("RTPS FLAG", FLAG(), 0);

    /* Translation added before projection. */
    setup_rtps_identity();
    WC(5, 10); WC(6, -20); WC(7, 100);
    WD(0, XY(90, 70)); WD(1, 100);
    gte_command(OP_RTPS);
    check("RTPS+TR IR1", D(9), 100);
    check("RTPS+TR IR2", D(10), 50);
    check("RTPS+TR SZ3", D(19), 200);
    check("RTPS+TR SXY2", D(14), XY(260, 170));

    /* Division overflow: H >= SZ3*2 -> quotient 1FFFFh, FLAG.17 + FLAG.31. */
    setup_rtps_identity();
    WD(0, XY(10, 20)); WD(1, 100);
    gte_command(OP_RTPS);
    check("RTPS div overflow SZ3", D(19), 100);
    check("RTPS div overflow SXY2", D(14), XY(179, 159));
    check("RTPS div overflow FLAG", FLAG(), 0x80020000);
    WC(27, 1);                /* DQA=1, DQB=0 -> MAC0 = quotient */
    gte_command(OP_RTPS);
    check("RTPS div overflow quotient", D(24), 0x1FFFF);
    check("RTPS div overflow IR0 = 1FFFFh>>12", D(8), 0x1F);
    check("RTPS div overflow FLAG (DQA=1)", FLAG(), 0x80020000);

    /* Just below the overflow threshold: H = SZ3*2-1. */
    WC(26, 199);
    gte_command(OP_RTPS);
    check("RTPS H=2*SZ3-1 no div flag", FLAG() & 0x00020000, 0);
    {
        uint32_t q = D(24);
        check("RTPS H=2*SZ3-1 quotient ~130417", (q >= 130416 && q <= 130418) ? 1 : 0, 1);
    }

    /* SZ3 = 0 -> overflow even with H = 0 */
    setup_rtps_identity();
    WC(26, 0);
    WD(0, 0); WD(1, 0);
    gte_command(OP_RTPS);
    check("RTPS SZ3=0 FLAG", FLAG(), 0x80020000);

    /* IR3 quirk, sf=0: IR3 saturates but FLAG.22 is tested on MAC3 SAR 12. */
    setup_rtps_identity();
    WC(26, 0);
    WD(0, 0); WD(1, 0x10);
    gte_command(OP_RTPS & ~SF);
    check("RTPS sf=0 MAC3", D(27), 0x10000);
    check("RTPS sf=0 IR3 saturated", D(11), 0x7FFF);
    check("RTPS sf=0 SZ3", D(19), 0x10);
    check("RTPS sf=0 no IR3 flag", FLAG(), 0);

    /* IR3 quirk, lm=1: negative MAC3 clamps IR3 to 0 without FLAG.22. */
    setup_rtps_identity();
    WC(26, 0);
    WD(0, 0); WD(1, -5);
    gte_command(OP_RTPS | LM);
    check("RTPS lm=1 MAC3", D(27), (uint32_t)-5);
    check("RTPS lm=1 IR3 clamped to 0", D(11), 0);
    check("RTPS lm=1 SZ3 saturated", D(19), 0);
    check("RTPS lm=1 FLAG (SZ3+div only)", FLAG(), 0x80060000);

    /* IR3 out of range in the SAR-12 domain: FLAG.22 + SZ3 saturation. */
    setup_rtps_identity();
    WC(4, 0x7FFF);
    WC(26, 0);
    WD(0, 0); WD(1, 0x7FFF);
    gte_command(OP_RTPS);
    check("RTPS big Z MAC3", D(27), 0x3FFF0);
    check("RTPS big Z IR3", D(11), 0x7FFF);
    check("RTPS big Z SZ3", D(19), 0xFFFF);
    check("RTPS big Z FLAG", FLAG(), 0x80440000);

    /* SX/SY saturation to -400h..3FFh */
    setup_rtps_identity();
    WC(24, 0); WC(25, 0); WC(26, 200);
    WD(0, XY(2000, -2000)); WD(1, 200);
    gte_command(OP_RTPS);
    check("RTPS SX/SY saturation", D(14), XY(0x3FF, -0x400));
    check("RTPS SX/SY FLAG", FLAG(), 0x80006000);
}

static void test_rtpt(void)
{
    gte_reset();
    set_identity(0);
    WC(26, 0x100);
    WC(27, 0x200);            /* would saturate IR0 for vertex 0, not for vertex 2 */
    WD(0, XY(1, 2)); WD(1, 0x100);
    WD(2, XY(3, 4)); WD(3, 0x200);
    WD(4, XY(5, 6)); WD(5, 0x400);
    gte_command(OP_RTPT);
    check("RTPT SXY0", D(12), XY(1, 2));
    check("RTPT SXY1", D(13), XY(1, 2));
    check("RTPT SXY2", D(14), XY(1, 1));
    check("RTPT SZ1", D(17), 0x100);
    check("RTPT SZ2", D(18), 0x200);
    check("RTPT SZ3", D(19), 0x400);
    check("RTPT IR1 (last vertex)", D(9), 5);
    check("RTPT MAC0 (last vertex)", D(24), 0x800000);
    check("RTPT IR0 (last vertex)", D(8), 0x800);
    check("RTPT FLAG", FLAG(), 0);
}

static void test_nclip(void)
{
    gte_reset();
    WD(12, XY(0, 0)); WD(13, XY(10, 0)); WD(14, XY(0, 10));
    gte_command(OP_NCLIP);
    check("NCLIP ccw positive", D(24), 100);
    WD(12, XY(0, 0)); WD(13, XY(0, 10)); WD(14, XY(10, 0));
    gte_command(OP_NCLIP);
    check("NCLIP cw negative", D(24), (uint32_t)-100);
    WD(12, XY(0, 0)); WD(13, XY(5, 5)); WD(14, XY(10, 10));
    gte_command(OP_NCLIP);
    check("NCLIP collinear zero", D(24), 0);
    check("NCLIP FLAG", FLAG(), 0);
    WD(12, XY(0x7FFF, 0)); WD(13, XY(0, 0x7FFF)); WD(14, XY(-0x8000, -0x8000));
    gte_command(OP_NCLIP);
    check("NCLIP MAC0 overflow truncated", D(24), 0xBFFE0001);
    check("NCLIP MAC0 overflow FLAG", FLAG(), 0x80010000);
}

static void test_avsz(void)
{
    gte_reset();
    WD(16, 100); WD(17, 200); WD(18, 300); WD(19, 400);
    WC(29, 0x555);
    gte_command(OP_AVSZ3);
    check("AVSZ3 MAC0", D(24), 0x555 * 900);
    check("AVSZ3 OTZ", D(7), (0x555 * 900) >> 12);
    check("AVSZ3 FLAG", FLAG(), 0);
    WC(30, 0x400);
    gte_command(OP_AVSZ4);
    check("AVSZ4 MAC0", D(24), 0x400 * 1000);
    check("AVSZ4 OTZ", D(7), 250);
    WC(29, 0xFFFF);           /* ZSF3 = -1 */
    gte_command(OP_AVSZ3);
    check("AVSZ3 negative MAC0", D(24), (uint32_t)-900);
    check("AVSZ3 negative OTZ", D(7), 0);
    check("AVSZ3 negative FLAG", FLAG(), 0x80040000);
    WC(29, 0x7FFF);
    WD(17, 0xFFFF); WD(18, 0xFFFF); WD(19, 0xFFFF);
    gte_command(OP_AVSZ3);
    check("AVSZ3 large OTZ", D(7), 0xFFFF);
    check("AVSZ3 large FLAG", FLAG(), 0x80050000);
}

static void test_mvmva(void)
{
    gte_reset();
    set_identity(0);
    WC(5, 1); WC(6, -2); WC(7, 3);
    WD(0, XY(100, -200)); WD(1, 300);
    gte_command(MVMVA(1, 0, 0, 0, 0));
    check("MVMVA RT*V0+TR MAC1", D(25), 101);
    check("MVMVA RT*V0+TR MAC2", D(26), (uint32_t)-202);
    check("MVMVA RT*V0+TR MAC3", D(27), 303);
    check("MVMVA RT*V0+TR IR2", D(10), 0xFFFFFF36);
    check("MVMVA RT*V0+TR FLAG", FLAG(), 0);

    gte_command(MVMVA(1, 0, 0, 0, 1));
    check("MVMVA lm=1 IR2", D(10), 0);
    check("MVMVA lm=1 MAC2 unclamped", D(26), (uint32_t)-202);
    check("MVMVA lm=1 FLAG", FLAG(), 0x80800000);

    /* cv=2 far color bug: MAC = (M12*V2 + M13*V3) SAR 12, FC term only flags. */
    WC(21, 0x10000); WC(22, 0); WC(23, 0);
    gte_command(MVMVA(1, 0, 0, 2, 0));
    check("MVMVA FC-bug MAC1", D(25), 0);
    check("MVMVA FC-bug MAC2", D(26), (uint32_t)-200);
    check("MVMVA FC-bug MAC3", D(27), 300);
    check("MVMVA FC-bug IR1", D(9), 0);
    check("MVMVA FC-bug FLAG (IR1 from lost term)", FLAG(), 0x81000000);

    /* v=3 (IR vector), cv=3 (none) */
    WD(9, 7); WD(10, 8); WD(11, 9);
    gte_command(MVMVA(1, 0, 3, 3, 0));
    check("MVMVA RT*IR MAC1", D(25), 7);
    check("MVMVA RT*IR MAC3", D(27), 9);

    /* light / color matrix selection with BK */
    set_identity(8);
    WC(16, 0x00002000); WC(17, 0); WC(18, 0x00002000); WC(19, 0); WC(20, 0x2000);
    WC(13, 5); WC(14, 6); WC(15, 7);
    WD(2, XY(10, 20)); WD(3, 30);
    gte_command(MVMVA(1, 1, 1, 1, 0));
    check("MVMVA LLM*V1+BK MAC1", D(25), 15);
    check("MVMVA LLM*V1+BK MAC3", D(27), 37);
    gte_command(MVMVA(1, 2, 1, 3, 0));
    check("MVMVA LCM*V1 MAC2", D(26), 40);

    /* mx=3 garbage matrix: -R*10h, R*10h, IR0 / RT13 x3 / RT22 x3 */
    gte_reset();
    WD(6, 0x00000002);        /* R = 2 */
    WD(8, 3);                 /* IR0 */
    WC(1, 4);                 /* RT13 = 4 */
    WC(2, 5);                 /* RT22 = 5 */
    WD(0, XY(1, 1)); WD(1, 1);
    gte_command(MVMVA(0, 3, 0, 3, 0));
    check("MVMVA mx=3 MAC1", D(25), 3);
    check("MVMVA mx=3 MAC2", D(26), 12);
    check("MVMVA mx=3 MAC3", D(27), 15);

    /* 44-bit accumulator overflow (positive), wraps then saturates IR1 */
    gte_reset();
    WC(0, 0x7FFF);            /* RT11 */
    WC(5, 0x7FFFFFFF);
    WD(0, XY(0x7FFF, 0));
    gte_command(MVMVA(1, 0, 0, 0, 0));
    check("MVMVA MAC1 44-bit wrap (+)", D(25), 0x8003FFEF);
    check("MVMVA IR1 (+) wrap saturate", D(9), 0xFFFF8000);
    check("MVMVA FLAG (+) overflow", FLAG(), 0xC1000000);

    WC(5, 0x80000000);
    WD(0, XY(-0x8000, 0));
    gte_command(MVMVA(1, 0, 0, 0, 0));
    check("MVMVA MAC1 44-bit wrap (-)", D(25), 0x7FFC0008);
    check("MVMVA IR1 (-) wrap saturate", D(9), 0x7FFF);
    check("MVMVA FLAG (-) overflow", FLAG(), 0x89000000);
}

static void test_sqr_op(void)
{
    gte_reset();
    WD(9, 3); WD(10, -4); WD(11, 0x7FFF);
    gte_command(OP_SQR);
    check("SQR sf=0 MAC1", D(25), 9);
    check("SQR sf=0 MAC2", D(26), 16);
    check("SQR sf=0 MAC3", D(27), 0x3FFF0001);
    check("SQR sf=0 IR3", D(11), 0x7FFF);
    check("SQR sf=0 FLAG (bit22 not in bit31 mask)", FLAG(), 0x00400000);
    WD(9, 0x1000); WD(10, -0x2000); WD(11, 0x800);
    gte_command(OP_SQR | SF);
    check("SQR sf=1 MAC1", D(25), 0x1000);
    check("SQR sf=1 MAC2", D(26), 0x4000);
    check("SQR sf=1 MAC3", D(27), 0x400);
    check("SQR sf=1 FLAG", FLAG(), 0);

    gte_reset();
    WC(0, 1);                 /* RT11 = D1 */
    WC(2, 2);                 /* RT22 = D2 */
    WC(4, 3);                 /* RT33 = D3 */
    WD(9, 4); WD(10, 5); WD(11, 6);
    gte_command(OP_OP);
    check("OP MAC1", D(25), (uint32_t)-3);
    check("OP MAC2", D(26), 6);
    check("OP MAC3", D(27), (uint32_t)-3);
    check("OP IR1", D(9), 0xFFFFFFFD);
    check("OP FLAG", FLAG(), 0);
}

static void test_color(void)
{
    /* DPCS with IR0=0 returns RGBC */
    gte_reset();
    WD(6, 0x30204080);
    WD(20, 0x11); WD(21, 0x22); WD(22, 0x33);
    gte_command(OP_DPCS);
    check("DPCS IR0=0 MAC1", D(25), 0x800);
    check("DPCS IR0=0 IR3", D(11), 0x200);
    check("DPCS RGB0 (shifted)", D(20), 0x22);
    check("DPCS RGB1 (shifted)", D(21), 0x33);
    check("DPCS RGB2", D(22), 0x30204080);
    check("DPCS FLAG", FLAG(), 0);

    /* DPCS with IR0=1000h goes fully to FC */
    WD(8, 0x1000);
    WC(21, 0xFF0); WC(22, 0); WC(23, 0x100);
    gte_command(OP_DPCS);
    check("DPCS IR0=1000h MAC1", D(25), 0xFF0);
    check("DPCS IR0=1000h MAC2", D(26), 0);
    check("DPCS IR0=1000h MAC3", D(27), 0x100);
    check("DPCS IR0=1000h RGB2", D(22), 0x301000FF);
    check("DPCS IR0=1000h FLAG", FLAG(), 0);

    /* (FC-MAC) intermediate saturates as lm=0 and raises FLAG.24 */
    WD(8, 0);
    WC(21, 0x100000);
    gte_command(OP_DPCS | LM);
    check("DPCS FC intermediate sat MAC1", D(25), 0x800);
    check("DPCS FC intermediate sat FLAG", FLAG(), 0x81000000);

    /* DPCT works on RGB0..RGB2, CODE from RGBC */
    gte_reset();
    WD(6, 0xAB000000);
    WD(20, 0x00102030); WD(21, 0x00405060); WD(22, 0x00708090);
    gte_command(OP_DPCT);
    check("DPCT RGB0", D(20), 0xAB102030);
    check("DPCT RGB1", D(21), 0xAB405060);
    check("DPCT RGB2", D(22), 0xAB708090);
    check("DPCT MAC1", D(25), 0x900);
    check("DPCT MAC3", D(27), 0x700);

    /* INTPL */
    gte_reset();
    WD(9, 0x100); WD(10, 0x200); WD(11, 0x300);
    WD(8, 0x800);
    WC(21, 0x500); WC(22, 0); WC(23, 0x300);
    gte_command(OP_INTPL);
    check("INTPL MAC1", D(25), 0x300);
    check("INTPL MAC2", D(26), 0x100);
    check("INTPL MAC3", D(27), 0x300);
    check("INTPL RGB2", D(22), 0x00301030);

    /* GPF / GPL */
    gte_reset();
    WD(6, 0x5A000000);
    WD(8, 0x800);
    WD(9, 0x100); WD(10, 0x200); WD(11, -0x100);
    gte_command(OP_GPF | SF);
    check("GPF MAC1", D(25), 0x80);
    check("GPF MAC3", D(27), (uint32_t)-0x80);
    check("GPF IR3", D(11), 0xFFFFFF80);
    check("GPF RGB2", D(22), 0x5A001008);
    check("GPF FLAG (B underflow)", FLAG(), 0x00080000);
    gte_command(OP_GPL | SF);
    check("GPL MAC1", D(25), 0xC0);
    check("GPL MAC2", D(26), 0x180);
    check("GPL MAC3", D(27), (uint32_t)-0xC0);

    /* NCS / NCCS / NCDS with identity light/color matrices */
    gte_reset();
    set_identity(8);
    set_identity(16);
    WD(6, 0x30808080);
    WD(0, XY(0x800, 0x400)); WD(1, -0x100);
    gte_command(OP_NCS);
    check("NCS IR3 lm=1", D(11), 0);
    check("NCS RGB2", D(22), 0x30004080);
    check("NCS FLAG (bit22 not in bit31 mask)", FLAG(), 0x00400000);

    WD(0, XY(0x1000, 0x800)); WD(1, 0x400);
    gte_command(OP_NCCS);
    check("NCCS MAC1", D(25), 0x800);
    check("NCCS MAC2", D(26), 0x400);
    check("NCCS MAC3", D(27), 0x200);
    check("NCCS RGB2", D(22), 0x30204080);
    check("NCCS FLAG", FLAG(), 0);

    gte_command(OP_NCDS);
    check("NCDS IR0=0 RGB2 == NCCS", D(22), 0x30204080);
    WD(8, 0x1000);
    gte_command(OP_NCDS);
    check("NCDS IR0=1000h FC=0 RGB2", D(22), 0x30000000);
}

/* ------------------------------------------------------------------------- */
/* Division: compare RTPS quotient against the literal psx-spx unr_table.     */
/* ------------------------------------------------------------------------- */

static const uint8_t doc_unr_table[0x101] = {
    0xFF,0xFD,0xFB,0xF9,0xF7,0xF5,0xF3,0xF1,0xEF,0xEE,0xEC,0xEA,0xE8,0xE6,0xE4,0xE3,
    0xE1,0xDF,0xDD,0xDC,0xDA,0xD8,0xD6,0xD5,0xD3,0xD1,0xD0,0xCE,0xCD,0xCB,0xC9,0xC8,
    0xC6,0xC5,0xC3,0xC1,0xC0,0xBE,0xBD,0xBB,0xBA,0xB8,0xB7,0xB5,0xB4,0xB2,0xB1,0xB0,
    0xAE,0xAD,0xAB,0xAA,0xA9,0xA7,0xA6,0xA4,0xA3,0xA2,0xA0,0x9F,0x9E,0x9C,0x9B,0x9A,
    0x99,0x97,0x96,0x95,0x94,0x92,0x91,0x90,0x8F,0x8D,0x8C,0x8B,0x8A,0x89,0x87,0x86,
    0x85,0x84,0x83,0x82,0x81,0x7F,0x7E,0x7D,0x7C,0x7B,0x7A,0x79,0x78,0x77,0x75,0x74,
    0x73,0x72,0x71,0x70,0x6F,0x6E,0x6D,0x6C,0x6B,0x6A,0x69,0x68,0x67,0x66,0x65,0x64,
    0x63,0x62,0x61,0x60,0x5F,0x5E,0x5D,0x5D,0x5C,0x5B,0x5A,0x59,0x58,0x57,0x56,0x55,
    0x54,0x53,0x53,0x52,0x51,0x50,0x4F,0x4E,0x4D,0x4D,0x4C,0x4B,0x4A,0x49,0x48,0x48,
    0x47,0x46,0x45,0x44,0x43,0x43,0x42,0x41,0x40,0x3F,0x3F,0x3E,0x3D,0x3C,0x3C,0x3B,
    0x3A,0x39,0x39,0x38,0x37,0x36,0x36,0x35,0x34,0x33,0x33,0x32,0x31,0x31,0x30,0x2F,
    0x2E,0x2E,0x2D,0x2C,0x2C,0x2B,0x2A,0x2A,0x29,0x28,0x28,0x27,0x26,0x26,0x25,0x24,
    0x24,0x23,0x22,0x22,0x21,0x20,0x20,0x1F,0x1E,0x1E,0x1D,0x1D,0x1C,0x1B,0x1B,0x1A,
    0x19,0x19,0x18,0x18,0x17,0x16,0x16,0x15,0x15,0x14,0x14,0x13,0x12,0x12,0x11,0x11,
    0x10,0x0F,0x0F,0x0E,0x0E,0x0D,0x0D,0x0C,0x0C,0x0B,0x0A,0x0A,0x09,0x09,0x08,0x08,
    0x07,0x07,0x06,0x06,0x05,0x05,0x04,0x04,0x03,0x03,0x02,0x02,0x01,0x01,0x00,0x00,
    0x00,
};

static uint32_t ref_divide(uint32_t h, uint32_t sz3, int *overflow)
{
    *overflow = 0;
    if (!(h < sz3 * 2)) {
        *overflow = 1;
        return 0x1FFFF;
    }
    int z = 0;
    while (!((sz3 << z) & 0x8000))
        z++;
    uint64_t n = (uint64_t)h << z;
    uint64_t d = (uint64_t)sz3 << z;
    uint64_t u = doc_unr_table[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    n = ((n * d) + 0x8000) >> 16;
    return n > 0x1FFFF ? 0x1FFFF : (uint32_t)n;
}

static void test_division(void)
{
    static const uint32_t hs[] = { 0, 1, 2, 0x7F, 0x100, 0x155, 0x200, 0x3E8, 0x1000,
                                   0x4000, 0x7FFF, 0x8000, 0xAAAA, 0xFE3F, 0xF015, 0xFFFF };
    long mismatches = 0, cases = 0;
    long max_dev = 0;

    gte_reset();
    WC(27, 1);                /* DQA = 1, DQB = 0 -> MAC0 = quotient */
    for (size_t hi = 0; hi < sizeof hs / sizeof hs[0]; hi++) {
        WC(26, hs[hi]);
        for (uint32_t sz = 0; sz <= 0xFFFF; sz++) {
            int ovf;
            uint32_t want = ref_divide(hs[hi], sz, &ovf);
            WC(7, sz);        /* TRZ -> SZ3 = sz with V0 = 0 and sf = 1 */
            gte_command(OP_RTPS);
            uint32_t got = D(24);
            int got_ovf = (FLAG() >> 17) & 1;
            cases++;
            if (D(19) != sz || got != want || got_ovf != ovf) {
                if (mismatches < 8)
                    printf("FAIL  divide H=%04X SZ3=%04X got %05X/%d want %05X/%d\n",
                           (unsigned)hs[hi], (unsigned)sz, (unsigned)got, got_ovf, (unsigned)want, ovf);
                mismatches++;
            }
            if (!ovf) {
                long exact = (long)((((uint64_t)hs[hi] * 0x20000 / sz) + 1) / 2);
                if (exact > 0x1FFFF)
                    exact = 0x1FFFF;
                long dev = labs((long)got - exact);
                if (dev > max_dev)
                    max_dev = dev;
            }
        }
    }
    g_checks++;
    if (mismatches)
        g_fails++;
    printf("info  UNR divide: %ld cases vs literal psx-spx table, %ld mismatches, "
           "max |UNR - exact| = %ld\n", cases, mismatches, max_dev);

    /* documented special cases: these give UNR 20000h, clamped without the flag */
    {
        int ovf;
        check("doc FE3F/7F20 ref clamp", ref_divide(0xFE3F, 0x7F20, &ovf), 0x1FFFF);
        gte_reset();
        WC(27, 1);
        WC(26, 0xFE3F); WC(7, 0x7F20);
        gte_command(OP_RTPS);
        check("RTPS FE3F/7F20 quotient", D(24), 0x1FFFF);
        check("RTPS FE3F/7F20 no div flag", FLAG() & 0x20000, 0);
    }
}

static void test_state_and_misc(void)
{
    gte_reset();
    set_identity(0);
    WC(24, 160 << 16); WC(26, 300); WC(31, 0x00040000);
    WD(0, XY(7, 8)); WD(1, 900); WD(30, 0x0000F000); WD(23, 0x1234);

    size_t n = gte_state_size();
    void *buf = malloc(n);
    if (!buf) {
        printf("FAIL  malloc\n");
        g_fails++;
        return;
    }
    gte_save_state(buf);
    gte_reset();
    check("reset clears VXY0", D(0), 0);
    check("reset clears FLAG", FLAG(), 0);
    check("reset clears H", C(26), 0);
    gte_load_state(buf);
    free(buf);
    check("load VXY0", D(0), XY(7, 8));
    check("load VZ0", D(1), 900);
    check("load LZCR", D(31), 16);
    check("load RES1", D(23), 0x1234);
    check("load H", C(26), 300);
    check("load FLAG", FLAG(), 0x80040000);
    check("load RT11", C(0), 0x1000);

    /* undocumented opcode: no crash, FLAG cleared, registers untouched */
    WD(24, 0x55AA55AA);
    gte_command(0x000003Cu);
    check("unknown op FLAG cleared", FLAG(), 0);
    check("unknown op MAC0 untouched", D(24), 0x55AA55AA);

    /* fake command number bits 20-24 are ignored */
    gte_reset();
    WD(12, XY(0, 0)); WD(13, XY(10, 0)); WD(14, XY(0, 10));
    gte_command(0x0140006u);  /* Wipeout 2097 style NCLIP encoding */
    check("NCLIP with odd fake bits", D(24), 100);
}

int main(void)
{
    test_registers();
    test_lzc();
    test_irgb();
    test_rtps();
    test_rtpt();
    test_nclip();
    test_avsz();
    test_mvmva();
    test_sqr_op();
    test_color();
    test_division();
    test_state_and_misc();

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
