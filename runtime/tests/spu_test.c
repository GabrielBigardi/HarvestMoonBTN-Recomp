/* SPU smoke/behaviour test.
 * Build (from the project root):
 *   deps/llvm-mingw/bin/clang.exe -std=c11 -O2 -Wall -Wextra -Iruntime runtime/spu.c runtime/tests/spu_test.c -o build/spu_test.exe
 * Run:  build/spu_test.exe [out_dir]   -> writes spu_test.wav and spu_test_reverb.wav */
#include "spu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- runtime stubs ---------------------------------------------------------------- */
static int g_irq_count;
static int g_cd_mode;            /* 0 = no CD audio, 1 = ramp test signal */
static int g_cd_phase;

int cdrom_audio_sample(int16_t *l, int16_t *r)
{
    if (!g_cd_mode) return 0;
    *l = (int16_t)((g_cd_phase * 64) & 0x3FFF);
    *r = (int16_t)(-(int16_t)((g_cd_phase * 64) & 0x3FFF));
    g_cd_phase++;
    return 1;
}

void spu_irq_callback(void) { g_irq_count++; }

/* ---- helpers ------------------------------------------------------------------------ */
static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define W(off, v) spu_write16((off), (uint16_t)(v))
#define R(off)    spu_read16(off)
#define VOFF(v, r) ((uint32_t)(v) * 16u + (r))

static void make_block(uint8_t *b, int shift, int filter, int flags, const int nib[28])
{
    int i;
    b[0] = (uint8_t)((filter << 4) | shift);
    b[1] = (uint8_t)flags;
    for (i = 0; i < 14; i++)
        b[2 + i] = (uint8_t)((nib[2 * i] & 0xF) | ((nib[2 * i + 1] & 0xF) << 4));
}

/* manual FIFO upload, 32 halfwords at a time, as the documented procedure does */
static void upload_fifo(uint32_t addr, const uint8_t *data, int len)
{
    int i = 0;
    uint16_t cnt = R(0x1AA);
    W(0x1AA, cnt & ~0x30);                        /* stop */
    W(0x1A6, addr / 8);
    while (i < len) {
        int k;
        for (k = 0; k < 32 && i < len; k++, i += 2)
            W(0x1A8, data[i] | (data[i + 1] << 8));
        W(0x1AA, (cnt & ~0x30) | 0x10);           /* manual write */
        CHECK((R(0x1AE) & 0x3F) == (((cnt & ~0x30) | 0x10) & 0x3F), "SPUSTAT mode mirror");
        CHECK((R(0x1AE) & 0x400) == 0, "transfer not busy");
    }
    W(0x1AA, cnt & ~0x30);
}

static void upload_dma(uint32_t addr, const uint8_t *data, int len)
{
    uint32_t words[256];
    int n = len / 4, i;
    for (i = 0; i < n; i++)
        words[i] = (uint32_t)data[4 * i] | ((uint32_t)data[4 * i + 1] << 8) |
                   ((uint32_t)data[4 * i + 2] << 16) | ((uint32_t)data[4 * i + 3] << 24);
    W(0x1A6, addr / 8);
    W(0x1AA, (R(0x1AA) & ~0x30) | 0x20);          /* DMA write */
    spu_dma_write(words, n);
    W(0x1AA, R(0x1AA) & ~0x30);
}

static void download_dma(uint32_t addr, uint8_t *data, int len)
{
    uint32_t words[256];
    int n = len / 4, i;
    W(0x1A6, addr / 8);
    W(0x1AA, (R(0x1AA) & ~0x30) | 0x30);          /* DMA read */
    spu_dma_read(words, n);
    W(0x1AA, R(0x1AA) & ~0x30);
    for (i = 0; i < n; i++) {
        data[4 * i + 0] = (uint8_t)words[i];
        data[4 * i + 1] = (uint8_t)(words[i] >> 8);
        data[4 * i + 2] = (uint8_t)(words[i] >> 16);
        data[4 * i + 3] = (uint8_t)(words[i] >> 24);
    }
}

static void write_wav(const char *path, const int16_t *s, int frames)
{
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)frames * 4, v32;
    uint16_t v16;
    if (!f) { printf("cannot write %s\n", path); return; }
    fwrite("RIFF", 1, 4, f); v32 = 36 + data; fwrite(&v32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v32 = 16; fwrite(&v32, 4, 1, f);
    v16 = 1; fwrite(&v16, 2, 1, f);               /* PCM */
    v16 = 2; fwrite(&v16, 2, 1, f);               /* stereo */
    v32 = 44100; fwrite(&v32, 4, 1, f);
    v32 = 44100 * 4; fwrite(&v32, 4, 1, f);
    v16 = 4; fwrite(&v16, 2, 1, f);
    v16 = 16; fwrite(&v16, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(s, 4, (size_t)frames, f);
    fclose(f);
}

static int peak(const int16_t *s, int frames)
{
    int i, p = 0;
    for (i = 0; i < frames * 2; i++) {
        int a = s[i] < 0 ? -s[i] : s[i];
        if (a > p) p = a;
    }
    return p;
}

static void basic_setup(void)
{
    spu_init();
    W(0x1AC, 0x0004);                             /* RAM_CTRL */
    W(0x1AA, 0xC000);                             /* SPU enable, unmute */
    W(0x180, 0x3FFF);                             /* main volume (fixed) */
    W(0x182, 0x3FFF);
}

/* Test sample layout */
#define SQ_ADDR   0x1000u   /* 3-block looping square wave: [loop start] [normal] [end+repeat] */
#define ONE_ADDR  0x1100u   /* 2-block one-shot: [normal] [end (mute)] */
#define REF_ADDR  0x1200u   /* 2-block loop with filtered/shifted data for decode check */
#define SIL_ADDR  0x1300u   /* silent looping block */

static uint8_t g_ref_blocks[32];

static void upload_samples(void)
{
    uint8_t sq[48], one[32], sil[16], back[48];
    int nib[28], i;

    for (i = 0; i < 28; i++) nib[i] = i < 14 ? 7 : -7;    /* square: 14 high, 14 low */
    make_block(sq + 0, 0, 0, 4, nib);
    make_block(sq + 16, 0, 0, 0, nib);
    make_block(sq + 32, 0, 0, 3, nib);
    upload_fifo(SQ_ADDR, sq, 48);

    for (i = 0; i < 28; i++) nib[i] = (i & 4) ? 5 : -5;
    make_block(one + 0, 0, 0, 0, nib);
    make_block(one + 16, 0, 0, 1, nib);
    upload_dma(ONE_ADDR, one, 32);

    srand(1234);
    for (i = 0; i < 28; i++) nib[i] = (rand() % 16) - 8;
    make_block(g_ref_blocks + 0, 6, 2, 4, nib);
    for (i = 0; i < 28; i++) nib[i] = (rand() % 16) - 8;
    make_block(g_ref_blocks + 16, 5, 3, 3, nib);
    upload_dma(REF_ADDR, g_ref_blocks, 32);

    memset(nib, 0, sizeof nib);
    make_block(sil, 0, 0, 7, nib);
    upload_fifo(SIL_ADDR, sil, 16);

    download_dma(SQ_ADDR, back, 48);
    CHECK(memcmp(back, sq, 48) == 0, "FIFO upload / DMA readback mismatch");
    download_dma(ONE_ADDR, back, 32);
    CHECK(memcmp(back, one, 32) == 0, "DMA upload / DMA readback mismatch");
}

static void setup_voice(int v, uint32_t addr, uint16_t pitch, uint16_t adsr1, uint16_t adsr2, uint16_t vol)
{
    W(VOFF(v, 0x0), vol);
    W(VOFF(v, 0x2), vol);
    W(VOFF(v, 0x4), pitch);
    W(VOFF(v, 0x6), addr / 8);
    W(VOFF(v, 0x8), adsr1);
    W(VOFF(v, 0xA), adsr2);
}

static void key_on(uint32_t mask)  { W(0x188, mask & 0xFFFF); W(0x18A, mask >> 16); }
static void key_off(uint32_t mask) { W(0x18C, mask & 0xFFFF); W(0x18E, mask >> 16); }
static uint32_t endx(void) { return R(0x19C) | ((uint32_t)R(0x19E) << 16); }

/* ---- tests ------------------------------------------------------------------------ */

static void test_registers(void)
{
    int v;
    basic_setup();
    for (v = 0; v < 24; v++) {
        W(VOFF(v, 0x0), 0x1234 + v); W(VOFF(v, 0x2), 0x2345 + v); W(VOFF(v, 0x4), 0x3456 + v);
        W(VOFF(v, 0x6), 0x4567 + v); W(VOFF(v, 0x8), 0x5678 + v); W(VOFF(v, 0xA), 0x6789 + v);
        W(VOFF(v, 0xE), 0x789A + v);
    }
    for (v = 0; v < 24; v++) {
        CHECK(R(VOFF(v, 0x0)) == 0x1234 + v && R(VOFF(v, 0x2)) == 0x2345 + v &&
              R(VOFF(v, 0x4)) == 0x3456 + v && R(VOFF(v, 0x6)) == 0x4567 + v &&
              R(VOFF(v, 0x8)) == 0x5678 + v && R(VOFF(v, 0xA)) == 0x6789 + v &&
              R(VOFF(v, 0xE)) == 0x789A + v, "voice %d register readback", v);
        CHECK(R(0x200 + 4 * v) == (uint16_t)(((0x1234 + v) & 0x7FFF) << 1), "voice %d current vol L", v);
    }
    W(0x18C, 0xABCD); W(0x18E, 0x00EF);
    CHECK(R(0x18C) == 0xABCD && R(0x18E) == 0x00EF, "KOFF readback");
    W(0x190, 0x1111); W(0x194, 0x2222); W(0x198, 0x3333);
    CHECK(R(0x190) == 0x1111 && R(0x194) == 0x2222 && R(0x198) == 0x3333, "PMON/NON/EON readback");
    for (v = 0x1C0; v < 0x200; v += 2) W((uint32_t)v, 0x1000 + v);
    for (v = 0x1C0; v < 0x200; v += 2) CHECK(R((uint32_t)v) == 0x1000 + v, "reverb reg %X", v);
    W(0x1A0, 0xBEEF); W(0x1BC, 0xCAFE); W(0x1BE, 0xF00D); W(0x260, 0x1357); W(0x3FE, 0x2468);
    CHECK(R(0x1A0) == 0xBEEF && R(0x1BC) == 0xCAFE && R(0x1BE) == 0xF00D &&
          R(0x260) == 0x1357 && R(0x3FE) == 0x2468, "unknown register readback");
    W(0x1A2, 0xF000); W(0x1A4, 0x0123); W(0x1A6, 0x0456); W(0x1B0, 0x7000); W(0x1B2, 0x8000);
    CHECK(R(0x1A2) == 0xF000 && R(0x1A4) == 0x0123 && R(0x1A6) == 0x0456 &&
          R(0x1B0) == 0x7000 && R(0x1B2) == 0x8000, "control register readback");
    W(0x1AA, 0xC0B5);
    CHECK((R(0x1AE) & 0x3F) == 0x35 && (R(0x1AE) & 0x80) && (R(0x1AE) & 0x200) && !(R(0x1AE) & 0x400),
          "SPUSTAT mirrors SPUCNT (got %04X)", R(0x1AE));
    CHECK(R(0x1AA) == 0xC0B5, "SPUCNT readback");
    W(0x180, 0x2000);
    CHECK(R(0x180) == 0x2000 && R(0x1B8) == 0x4000, "main volume / current main volume");
}

static void test_adpcm_decode(void)
{
    /* voice 1 (captured to 0x800) plays the reference sample at pitch 1000h, so the
     * interpolation index stays 0 and the capture buffer holds the interpolated,
     * envelope-scaled samples. Compare against an independent decode. */
    int16_t dec[3 + 28 * 6];
    int32_t h0 = 0, h1 = 0;
    static const int fpos[5] = { 0, 60, 115, 98, 122 }, fneg[5] = { 0, 0, -52, -55, -60 };
    int b, i, t, ok = 1, start_pos;
    uint16_t cap[160];
    uint8_t raw[320];

    basic_setup();
    upload_samples();
    memset(dec, 0, sizeof dec);
    for (b = 0; b < 6; b++) {                     /* blocks 0,1,0,1,... (loop) */
        const uint8_t *blk = g_ref_blocks + (b & 1) * 16;
        int shift = blk[0] & 15, f = blk[0] >> 4;
        for (i = 0; i < 28; i++) {
            int n = (blk[2 + i / 2] >> ((i & 1) * 4)) & 15;
            int32_t s = (int16_t)(n << 12) >> shift;
            s += (h0 * fpos[f]) >> 6;
            s += (h1 * fneg[f]) >> 6;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            h1 = h0; h0 = s;
            dec[3 + b * 28 + i] = (int16_t)s;
        }
    }
    /* sustain level F, sustain rate 7Fh: envelope will stay at whatever we force */
    setup_voice(1, REF_ADDR, 0x1000, 0x000F, 0x1FC0, 0x3FFF);
    spu_run(100, NULL);
    start_pos = 100;
    key_on(1u << 1);
    W(VOFF(1, 0xC), 0x7FFF);                      /* force ENVX to full */
    spu_run(150, NULL);
    download_dma(0x800 + (uint32_t)start_pos * 2, raw, 300);
    memcpy(cap, raw, 300);
    for (t = 0; t < 150; t++) {
        int32_t o = ((0x12C7 * dec[t]) >> 15) + ((0x59B3 * dec[t + 1]) >> 15) +
                    ((0x1307 * dec[t + 2]) >> 15) + ((-1 * dec[t + 3]) >> 15);
        int32_t exp = (o * 0x7FFF) >> 15;
        if ((int16_t)cap[t] != exp) {
            if (ok) printf("  decode mismatch at t=%d: got %d expected %d\n", t, (int16_t)cap[t], exp);
            ok = 0;
        }
    }
    CHECK(ok, "ADPCM decode + gaussian interpolation (via voice 1 capture) matches reference");
    CHECK(endx() & 2, "ENDX set for looping reference sample");
    CHECK(R(VOFF(1, 0xE)) == REF_ADDR / 8, "repeat address set by loop-start flag (got %04X)", R(VOFF(1, 0xE)));
    CHECK(R(VOFF(1, 0xC)) == 0x7FFF, "sustain held at 7FFF");
}

static void test_main(const char *outdir)
{
    enum { FRAMES = 44100 };
    static int16_t buf[FRAMES * 2];
    int16_t env_trace[FRAMES / 64 + 1];
    int pos = 0, chunk = 64, rises = 1, k, i, koff_at = -1;
    int endx0_at = -1, endx1_at = -1, irq_at = -1;
    char path[512];

    basic_setup();
    upload_samples();

    /* voice 0: square loop, attack linear shift 10 (+14/sample), decay shift 4,
     * sustain level Ah (5800h) held, release linear shift 10 (-16/sample) */
    setup_voice(0, SQ_ADDR, 0x0800, 0x284A, 0x1FCA, 0x3FFF);
    /* voice 1: one-shot, fast attack */
    setup_voice(1, ONE_ADDR, 0x1000, 0x000F, 0x1FC0, 0x2000);
    /* voices 2..23 parked on the silent loop */
    for (i = 2; i < 24; i++) setup_voice(i, SIL_ADDR, 0x1000, 0, 0, 0);

    /* IRQ on the second block of the square loop */
    W(0x1A4, (SQ_ADDR + 16) / 8);
    W(0x1AA, 0xC040);

    key_on(0x00FFFFFF);
    CHECK(endx() == 0, "ENDX cleared by key on");

    for (k = 0; pos < FRAMES; k++) {
        int n = FRAMES - pos < chunk ? FRAMES - pos : chunk;
        if (koff_at < 0 && pos >= 26460) { key_off(1); koff_at = pos; }  /* voice 0 at ~0.6 s */
        spu_run(n, buf + pos * 2);
        pos += n;
        env_trace[k] = (int16_t)R(VOFF(0, 0xC));
        if (endx0_at < 0 && (endx() & 1)) endx0_at = pos;
        if (endx1_at < 0 && (endx() & 2)) endx1_at = pos;
        if (irq_at < 0 && g_irq_count) irq_at = pos;
    }

    snprintf(path, sizeof path, "%s/spu_test.wav", outdir);
    write_wav(path, buf, FRAMES);
    printf("  wrote %s\n", path);

    /* envelope rises monotonically during the attack (~2341 samples) */
    for (i = 1; i < 2300 / 64; i++)
        if (env_trace[i] <= env_trace[i - 1]) rises = 0;
    CHECK(rises, "envelope rises during attack");
    CHECK(env_trace[2300 / 64] > 0x7800, "attack reaches near 7FFFh (got %04X)", (uint16_t)env_trace[2300 / 64]);
    CHECK(env_trace[3000 / 64] <= 0x5800 && env_trace[3000 / 64] > 0x5000,
          "decay stops at sustain level 5800h (got %04X)", (uint16_t)env_trace[3000 / 64]);
    CHECK(env_trace[26400 / 64] == env_trace[3000 / 64], "sustain held until key off");
    CHECK(env_trace[(koff_at + 1500) / 64] == 0, "release reaches 0 within ~1408 samples (got %04X)",
          (uint16_t)env_trace[(koff_at + 1500) / 64]);
    printf("  env: t=1024:%04X t=2048:%04X t=3008:%04X t=koff+640:%04X\n", (uint16_t)env_trace[15],
           (uint16_t)env_trace[31], (uint16_t)env_trace[46], (uint16_t)env_trace[(koff_at + 640) / 64]);

    CHECK(peak(buf + 2 * 100, 200) > 0, "non-silent at start");
    CHECK(peak(buf + 2 * 22050, 4410) > 6000, "loop still sounding at 0.5 s (peak %d)", peak(buf + 2 * 22050, 4410));
    CHECK(peak(buf + 2 * 30000, 14100) == 0, "silent after release");
    /* 3 blocks * 28 samples at pitch 800h = 168 samples */
    CHECK(endx0_at > 0 && endx0_at <= 192 + 64, "ENDX for looping voice set around 168 samples (at %d)", endx0_at);
    CHECK(endx1_at > 0 && endx1_at <= 64, "ENDX for one-shot set around 56 samples (at %d)", endx1_at);
    CHECK(R(VOFF(1, 0xC)) == 0, "one-shot envelope forced to 0 at end");
    CHECK(irq_at > 0 && irq_at <= 128, "voice fetch IRQ fired (at %d, count %d)", irq_at, g_irq_count);
    CHECK(g_irq_count == 1, "IRQ fires once until acknowledged (count %d)", g_irq_count);
    CHECK(R(0x1AE) & 0x40, "SPUSTAT IRQ flag set");
    W(0x1AA, 0xC000);
    CHECK(!(R(0x1AE) & 0x40), "IRQ acknowledged by clearing SPUCNT bit 6");
    W(0x1AA, 0xC040);
    spu_run(200, NULL);                           /* voice 0 still fetches the blocks */
    CHECK(g_irq_count == 2, "IRQ fires again after re-enable (count %d)", g_irq_count);
}

static void test_capture_and_cd(void)
{
    int16_t out[2 * 600];
    uint8_t raw[16];
    int i, ok = 1;
    basic_setup();
    g_cd_mode = 1; g_cd_phase = 0;
    spu_run(600, out);
    for (i = 0; i < 600; i++) if (out[2 * i] || out[2 * i + 1]) ok = 0;
    CHECK(ok, "CD audio silent while SPUCNT bit 0 clear");
    download_dma(0x000 + 2 * 100, raw, 8);
    CHECK((raw[0] | raw[1] << 8) == ((100 * 64) & 0x3FFF) ||
          (raw[0] | raw[1] << 8) == ((612 * 64) & 0x3FFF), "CD L capture buffer");
    W(0x1B0, 0x7FFF); W(0x1B2, 0x7FFF);
    W(0x1AA, 0xC001);
    W(0x180, 0x3FFF); W(0x182, 0x3FFF);
    spu_run(600, out);
    CHECK(peak(out, 600) > 3000, "CD audio audible with SPUCNT bit 0 (peak %d)", peak(out, 600));
    W(0x1AA, 0x0001);                              /* SPU disabled + muted: CD still passes */
    spu_run(600, out);
    CHECK(peak(out, 600) > 3000, "CD audio passes while SPU muted/disabled");
    /* capture IRQ: IRQ address inside CD-left capture buffer */
    g_irq_count = 0;
    W(0x1AA, 0xC041);
    W(0x1A4, 0x0010);
    spu_run(600, NULL);
    CHECK(g_irq_count == 1, "capture buffer IRQ (count %d)", g_irq_count);
    g_cd_mode = 0;
}

static void test_noise_pmod(void)
{
    int16_t out[2 * 4096];
    basic_setup();
    upload_samples();
    setup_voice(0, SIL_ADDR, 0x1000, 0x000F, 0x1FC0, 0x3FFF);
    W(0x194, 0x0001);                              /* voice 0 noise */
    W(0x1AA, 0xC000 | (0xC << 10) | (1 << 8));
    key_on(1);
    spu_run(4096, out);
    CHECK(peak(out + 2 * 100, 3000) > 4000, "noise voice audible (peak %d)", peak(out + 2 * 100, 3000));
    CHECK(endx() & 1, "noise voice still runs its ADPCM loop (ENDX)");
    /* pitch modulation: voice 2 modulated by voice 1 */
    basic_setup();
    upload_samples();
    setup_voice(1, SQ_ADDR, 0x0010, 0x000F, 0x1FC0, 0x0000);
    setup_voice(2, SQ_ADDR, 0x0400, 0x000F, 0x1FC0, 0x3FFF);
    W(0x190, 0x0004);
    key_on(6);
    spu_run(4096, out);
    CHECK(peak(out + 2 * 100, 3000) > 4000, "pitch-modulated voice audible");
}

static void test_reverb(const char *outdir)
{
    /* "Hall" preset from psx-spx */
    static const uint16_t hall[32] = {
        0x01A5,0x0139,0x6000,0x5000,0x4C00,0xB800,0xBC00,0xC000,
        0x6000,0x5C00,0x15BA,0x11BB,0x14C2,0x10BD,0x11BC,0x0DC1,
        0x11C0,0x0DC3,0x0DC0,0x09C1,0x0BC4,0x07C1,0x0A00,0x06CD,
        0x09C2,0x05C1,0x05C0,0x041A,0x0274,0x013A,0x8000,0x8000 };
    enum { FRAMES = 44100 * 2 };
    static int16_t buf[FRAMES * 2];
    static uint32_t zero[0x10000 / 4];
    char path[512];
    int i, p_tail, p_tail_dry;

    basic_setup();
    upload_samples();
    W(0x1A2, (0x80000 - 0xADE0) / 8);
    W(0x1A6, (0x80000 - 0xADE0) / 8);
    spu_dma_write(zero, 0xADE0 / 4);
    for (i = 0; i < 32; i++) W(0x1C0 + 2 * (uint32_t)i, hall[i]);
    W(0x184, 0x3000); W(0x186, 0x3000);
    W(0x198, 0x0001);                              /* EON voice 0 */
    W(0x1AA, 0xC080);                              /* reverb master enable */
    setup_voice(0, SQ_ADDR, 0x0600, 0x284A, 0x1FC6, 0x3000);
    key_on(1);
    spu_run(11025, buf);
    key_off(1);
    spu_run(FRAMES - 11025, buf + 2 * 11025);
    snprintf(path, sizeof path, "%s/spu_test_reverb.wav", outdir);
    write_wav(path, buf, FRAMES);
    printf("  wrote %s\n", path);
    /* release of voice 0 takes < 0.1 s; anything at 0.5 s..0.8 s is reverb tail */
    p_tail = peak(buf + 2 * 22050, 13230);
    CHECK(R(VOFF(0, 0xC)) == 0, "dry voice released");
    CHECK(p_tail > 200, "reverb tail present after key off (peak %d)", p_tail);

    /* master enable off: no new writes; the buffer is still read out */
    basic_setup();
    upload_samples();
    W(0x1A2, (0x80000 - 0xADE0) / 8);
    W(0x1A6, (0x80000 - 0xADE0) / 8);
    spu_dma_write(zero, 0xADE0 / 4);
    for (i = 0; i < 32; i++) W(0x1C0 + 2 * (uint32_t)i, hall[i]);
    W(0x184, 0x3000); W(0x186, 0x3000);
    W(0x198, 0x0001);
    W(0x1AA, 0xC000);
    setup_voice(0, SQ_ADDR, 0x0600, 0x284A, 0x1FC6, 0x3000);
    key_on(1);
    spu_run(11025, buf);
    key_off(1);
    spu_run(FRAMES - 11025, buf + 2 * 11025);
    p_tail_dry = peak(buf + 2 * 22050, 13230);
    CHECK(p_tail_dry == 0, "no reverb with master enable off (peak %d)", p_tail_dry);
}

static void test_reverb_delay(void)
{
    /* "Delay (one-shot echo)" preset: SAME writes at +1FFFh*8, COMB1 reads at +1005h*8,
     * so the echo arrives (1FFFh-1005h)*4 = 16360 reverb ticks (2*16360 output samples)
     * later, plus the APF delays (2 x dAPF*4 ticks) and the two resampling FIRs. */
    static const uint16_t delay[32] = {
        0x0001,0x0001,0x7FFF,0x7FFF,0x0000,0x0000,0x0000,0x0000,
        0x0000,0x0000,0x1FFF,0x0FFF,0x1005,0x0005,0x0000,0x0000,
        0x1005,0x0005,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
        0x0000,0x0000,0x1004,0x1002,0x0004,0x0002,0x8000,0x8000 };
    enum { FRAMES = 44100 };
    static int16_t buf[FRAMES * 2];
    static uint32_t zero[0x18040 / 4];
    int i, onset = -1, dry_end = -1;

    basic_setup();
    upload_samples();
    W(0x1A2, (0x80000 - 0x18040) / 8);
    W(0x1A6, (0x80000 - 0x18040) / 8);
    spu_dma_write(zero, 0x18040 / 4);
    for (i = 0; i < 32; i++) W(0x1C0 + 2 * (uint32_t)i, delay[i]);
    W(0x184, 0x7FFF); W(0x186, 0x7FFF);
    W(0x198, 0x0001);
    W(0x1AA, 0xC080);
    setup_voice(0, SQ_ADDR, 0x0800, 0x000F, 0x1FC0, 0x3FFF);  /* instant attack, release shift 0 */
    key_on(1);
    spu_run(441, buf);
    key_off(1);
    spu_run(FRAMES - 441, buf + 2 * 441);
    for (i = 441; i < FRAMES; i++)
        if (buf[2 * i] == 0 && buf[2 * i + 1] == 0) { dry_end = i; break; }
    for (i = 2000; i < FRAMES; i++)
        if (buf[2 * i] > 1000 || buf[2 * i] < -1000) { onset = i; break; }
    printf("  delay preset: dry ends at %d, echo onset at %d (expected ~%d)\n", dry_end, onset, 2 * 16360);
    CHECK(onset >= 2 * 16360 && onset <= 2 * 16360 + 100, "echo arrives after the programmed delay");
    CHECK(peak(buf + 2 * (onset + 50), 300) > 4000, "echo has substantial level (peak %d)",
          peak(buf + 2 * (onset + 50), 300));
}

static void test_state_and_perf(void)
{
    static int16_t a[2 * 4410], b[2 * 4410];
    void *st;
    int i;
    clock_t t0;
    double secs;

    basic_setup();
    upload_samples();
    W(0x1A2, (0x80000 - 0xADE0) / 8);
    W(0x184, 0x2000); W(0x186, 0x2000);
    W(0x1D4, 0x15BA); W(0x1D6, 0x11BB); W(0x1C4, 0x6000); W(0x1C6, 0x5000);
    W(0x1FC, 0x8000); W(0x1FE, 0x8000);
    W(0x198, 0xFFFF); W(0x19A, 0x00FF);
    for (i = 0; i < 24; i++)
        setup_voice(i, i & 1 ? SQ_ADDR : REF_ADDR, (uint16_t)(0x0400 + i * 0x123), 0x284A, 0x1FC0, 0x0800);
    W(0x190, 0xAAAA);
    W(0x1AA, 0xC080);
    key_on(0x00FFFFFF);
    spu_run(1000, NULL);

    st = malloc(spu_state_size());
    spu_save_state(st);
    spu_run(4410, a);
    spu_load_state(st);
    spu_run(4410, b);
    CHECK(memcmp(a, b, sizeof a) == 0, "save/load state is deterministic");
    CHECK(peak(a, 4410) > 0, "24-voice mix audible");
    free(st);

    t0 = clock();
    for (i = 0; i < 60 * 10; i++)
        spu_run(4410, a);                          /* 60 s of audio, 24 voices + reverb */
    secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("  perf: 60 s of audio (24 voices, reverb) in %.3f s CPU (%.1fx real time)\n", secs, 60.0 / secs);
    CHECK(secs < 6.0, "faster than 10x real time");
    printf("  state size: %u bytes\n", (unsigned)spu_state_size());
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";
    printf("registers\n");       test_registers();
    printf("adpcm decode\n");    test_adpcm_decode();
    printf("main scenario\n");   g_irq_count = 0; test_main(outdir);
    printf("capture / cd\n");    test_capture_and_cd();
    printf("noise / pmod\n");    test_noise_pmod();
    printf("reverb\n");          test_reverb(outdir); test_reverb_delay();
    printf("state / perf\n");    test_state_and_perf();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
