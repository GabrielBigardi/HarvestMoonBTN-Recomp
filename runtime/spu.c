/* PS1 SPU (Sound Processing Unit) emulation.
 *
 * Written from the psx-spx "Sound Processing Unit (SPU)" documentation:
 *   - 24 voices: SPU-ADPCM block decoding (5 prediction filters), pitch counter with
 *     pitch modulation, 4-point Gaussian interpolation, ADSR envelope, volume sweep,
 *     noise generator, loop/end flags, ENDX.
 *   - Reverb (Schroeder reverberator running at 22050 Hz on SPU RAM) with the 39-tap
 *     FIR resampler on its input (44100->22050) and output (22050->44100).
 *   - CD audio input with volume, capture buffers, SPU RAM transfers (FIFO + DMA),
 *     IRQ address matching, SPUCNT/SPUSTAT.
 *
 * All state (including the 512 KiB of SPU RAM) lives in one static struct so that
 * save states are a plain memcpy. */
#include "spu.h"
#include <string.h>

#define SPU_RAM_BYTES 0x80000u
#define SPU_RAM_HW    (SPU_RAM_BYTES / 2u)   /* RAM size in halfwords */
#define NUM_VOICES    24

/* Register indices (halfword index = byte offset / 2). */
#define R_MVOLL   (0x180 / 2)
#define R_MVOLR   (0x182 / 2)
#define R_EVOLL   (0x184 / 2)
#define R_EVOLR   (0x186 / 2)
#define R_KON0    (0x188 / 2)
#define R_KON1    (0x18A / 2)
#define R_KOFF0   (0x18C / 2)
#define R_KOFF1   (0x18E / 2)
#define R_PMON0   (0x190 / 2)
#define R_PMON1   (0x192 / 2)
#define R_NON0    (0x194 / 2)
#define R_NON1    (0x196 / 2)
#define R_EON0    (0x198 / 2)
#define R_EON1    (0x19A / 2)
#define R_ESA     (0x1A2 / 2)
#define R_IRQA    (0x1A4 / 2)
#define R_TSA     (0x1A6 / 2)
#define R_FIFO    (0x1A8 / 2)
#define R_SPUCNT  (0x1AA / 2)
#define R_RAMCTRL (0x1AC / 2)
#define R_AVOLL   (0x1B0 / 2)
#define R_AVOLR   (0x1B2 / 2)
#define R_RVB     (0x1C0 / 2)    /* reverb configuration block, 32 registers */

/* Reverb register indices relative to R_RVB. */
enum {
    RV_dAPF1 = 0x00, RV_dAPF2, RV_vIIR, RV_vCOMB1, RV_vCOMB2, RV_vCOMB3, RV_vCOMB4, RV_vWALL,
    RV_vAPF1, RV_vAPF2, RV_mLSAME, RV_mRSAME, RV_mLCOMB1, RV_mRCOMB1, RV_mLCOMB2, RV_mRCOMB2,
    RV_dLSAME, RV_dRSAME, RV_mLDIFF, RV_mRDIFF, RV_mLCOMB3, RV_mRCOMB3, RV_mLCOMB4, RV_mRCOMB4,
    RV_dLDIFF, RV_dRDIFF, RV_mLAPF1, RV_mRAPF1, RV_mLAPF2, RV_mRAPF2, RV_vLIN, RV_vRIN
};

/* SPUCNT bits */
#define CNT_CD_ENABLE   0x0001u
#define CNT_CD_REVERB   0x0004u
#define CNT_REVERB      0x0080u
#define CNT_IRQ_ENABLE  0x0040u
#define CNT_UNMUTE      0x4000u
#define CNT_ENABLE      0x8000u

/* ADSR phases */
enum { PH_ATTACK = 0, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

typedef struct {
    uint32_t counter;          /* pitch counter: bits 12+ = sample in block, bits 4..11 = interp index */
    int32_t  env;              /* ADSR level (ENVX) */
    int32_t  vol[2];           /* current L/R volume (VOLXL/VOLXR) */
    int32_t  hist[2];          /* ADPCM decoder history: [0]=old, [1]=older */
    int32_t  out;              /* post-envelope output of the last sample (VxOUTX) */
    uint16_t cur;              /* current ADPCM block address (8-byte units, 16-byte aligned) */
    uint16_t repeat;           /* repeat address register (8-byte units) */
    uint16_t env_counter;      /* ADSR step counter */
    uint16_t vol_counter[2];   /* volume sweep step counters */
    uint8_t  phase;            /* PH_* */
    uint8_t  flags;            /* flag byte of the ADPCM block being played */
    uint8_t  need_block;       /* fetch + decode the block at `cur` before the next sample */
    uint8_t  ignore_loop;      /* repeat address was written by software: ignore loop-start flags */
    int16_t  buf[32];          /* [0..2] = last 3 samples of previous block, [3..30] = current block */
} Voice;

typedef struct {
    uint16_t ram[SPU_RAM_HW];  /* 512 KiB sound RAM */
    uint16_t reg[0x200];       /* raw register file (written values) */
    Voice    voice[NUM_VOICES];
    uint32_t endx;             /* ENDX flags (bit n = voice n reached a loop-end block) */
    uint32_t irq_flag;         /* SPUSTAT bit 6 */
    uint32_t xfer_addr;        /* internal current transfer address (bytes) */
    int32_t  mvol[2];          /* current main volume L/R */
    uint16_t mvol_counter[2];
    uint16_t noise_level;
    uint16_t pad0;
    int32_t  noise_timer;
    uint32_t capture_pos;      /* capture buffer write index, 0..1FFh (halfwords) */
    uint32_t rvb_cur;          /* reverb current buffer address (halfword index) */
    uint32_t rvb_phase;        /* 0/1: which half of the 22050 Hz reverb period we're in */
    uint32_t rvb_in_pos;
    uint32_t rvb_out_pos;
    int16_t  rvb_in[2][64];    /* reverb input history at 44100 Hz (for the downsampling FIR) */
    int16_t  rvb_out[2][32];   /* reverb output history at 22050 Hz (for the upsampling FIR) */
} SpuState;

static SpuState S;

/* 4-point Gaussian interpolation table (psx-spx, entries 000h..1FFh) */
static const int16_t gauss_table[512] = {
    -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001,
    -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001, -0x0001,
     0x0000,  0x0000,  0x0000,  0x0000,  0x0000,  0x0000,  0x0000,  0x0001,
     0x0001,  0x0001,  0x0001,  0x0002,  0x0002,  0x0002,  0x0003,  0x0003,
     0x0003,  0x0004,  0x0004,  0x0005,  0x0005,  0x0006,  0x0007,  0x0007,
     0x0008,  0x0009,  0x0009,  0x000A,  0x000B,  0x000C,  0x000D,  0x000E,
     0x000F,  0x0010,  0x0011,  0x0012,  0x0013,  0x0015,  0x0016,  0x0018,
     0x0019,  0x001B,  0x001C,  0x001E,  0x0020,  0x0021,  0x0023,  0x0025,
     0x0027,  0x0029,  0x002C,  0x002E,  0x0030,  0x0033,  0x0035,  0x0038,
     0x003A,  0x003D,  0x0040,  0x0043,  0x0046,  0x0049,  0x004D,  0x0050,
     0x0054,  0x0057,  0x005B,  0x005F,  0x0063,  0x0067,  0x006B,  0x006F,
     0x0074,  0x0078,  0x007D,  0x0082,  0x0087,  0x008C,  0x0091,  0x0096,
     0x009C,  0x00A1,  0x00A7,  0x00AD,  0x00B3,  0x00BA,  0x00C0,  0x00C7,
     0x00CD,  0x00D4,  0x00DB,  0x00E3,  0x00EA,  0x00F2,  0x00FA,  0x0101,
     0x010A,  0x0112,  0x011B,  0x0123,  0x012C,  0x0135,  0x013F,  0x0148,
     0x0152,  0x015C,  0x0166,  0x0171,  0x017B,  0x0186,  0x0191,  0x019C,
     0x01A8,  0x01B4,  0x01C0,  0x01CC,  0x01D9,  0x01E5,  0x01F2,  0x0200,
     0x020D,  0x021B,  0x0229,  0x0237,  0x0246,  0x0255,  0x0264,  0x0273,
     0x0283,  0x0293,  0x02A3,  0x02B4,  0x02C4,  0x02D6,  0x02E7,  0x02F9,
     0x030B,  0x031D,  0x0330,  0x0343,  0x0356,  0x036A,  0x037E,  0x0392,
     0x03A7,  0x03BC,  0x03D1,  0x03E7,  0x03FC,  0x0413,  0x042A,  0x0441,
     0x0458,  0x0470,  0x0488,  0x04A0,  0x04B9,  0x04D2,  0x04EC,  0x0506,
     0x0520,  0x053B,  0x0556,  0x0572,  0x058E,  0x05AA,  0x05C7,  0x05E4,
     0x0601,  0x061F,  0x063E,  0x065C,  0x067C,  0x069B,  0x06BB,  0x06DC,
     0x06FD,  0x071E,  0x0740,  0x0762,  0x0784,  0x07A7,  0x07CB,  0x07EF,
     0x0813,  0x0838,  0x085D,  0x0883,  0x08A9,  0x08D0,  0x08F7,  0x091E,
     0x0946,  0x096F,  0x0998,  0x09C1,  0x09EB,  0x0A16,  0x0A40,  0x0A6C,
     0x0A98,  0x0AC4,  0x0AF1,  0x0B1E,  0x0B4C,  0x0B7A,  0x0BA9,  0x0BD8,
     0x0C07,  0x0C38,  0x0C68,  0x0C99,  0x0CCB,  0x0CFD,  0x0D30,  0x0D63,
     0x0D97,  0x0DCB,  0x0E00,  0x0E35,  0x0E6B,  0x0EA1,  0x0ED7,  0x0F0F,
     0x0F46,  0x0F7F,  0x0FB7,  0x0FF1,  0x102A,  0x1065,  0x109F,  0x10DB,
     0x1116,  0x1153,  0x118F,  0x11CD,  0x120B,  0x1249,  0x1288,  0x12C7,
     0x1307,  0x1347,  0x1388,  0x13C9,  0x140B,  0x144D,  0x1490,  0x14D4,
     0x1517,  0x155C,  0x15A0,  0x15E6,  0x162C,  0x1672,  0x16B9,  0x1700,
     0x1747,  0x1790,  0x17D8,  0x1821,  0x186B,  0x18B5,  0x1900,  0x194B,
     0x1996,  0x19E2,  0x1A2E,  0x1A7B,  0x1AC8,  0x1B16,  0x1B64,  0x1BB3,
     0x1C02,  0x1C51,  0x1CA1,  0x1CF1,  0x1D42,  0x1D93,  0x1DE5,  0x1E37,
     0x1E89,  0x1EDC,  0x1F2F,  0x1F82,  0x1FD6,  0x202A,  0x207F,  0x20D4,
     0x2129,  0x217F,  0x21D5,  0x222C,  0x2282,  0x22DA,  0x2331,  0x2389,
     0x23E1,  0x2439,  0x2492,  0x24EB,  0x2545,  0x259E,  0x25F8,  0x2653,
     0x26AD,  0x2708,  0x2763,  0x27BE,  0x281A,  0x2876,  0x28D2,  0x292E,
     0x298B,  0x29E7,  0x2A44,  0x2AA1,  0x2AFF,  0x2B5C,  0x2BBA,  0x2C18,
     0x2C76,  0x2CD4,  0x2D33,  0x2D91,  0x2DF0,  0x2E4F,  0x2EAE,  0x2F0D,
     0x2F6C,  0x2FCC,  0x302B,  0x308B,  0x30EA,  0x314A,  0x31AA,  0x3209,
     0x3269,  0x32C9,  0x3329,  0x3389,  0x33E9,  0x3449,  0x34A9,  0x3509,
     0x3569,  0x35C9,  0x3629,  0x3689,  0x36E8,  0x3748,  0x37A8,  0x3807,
     0x3867,  0x38C6,  0x3926,  0x3985,  0x39E4,  0x3A43,  0x3AA2,  0x3B00,
     0x3B5F,  0x3BBD,  0x3C1B,  0x3C79,  0x3CD7,  0x3D35,  0x3D92,  0x3DEF,
     0x3E4C,  0x3EA9,  0x3F05,  0x3F62,  0x3FBD,  0x4019,  0x4074,  0x40D0,
     0x412A,  0x4185,  0x41DF,  0x4239,  0x4292,  0x42EB,  0x4344,  0x439C,
     0x43F4,  0x444C,  0x44A3,  0x44FA,  0x4550,  0x45A6,  0x45FC,  0x4651,
     0x46A6,  0x46FA,  0x474E,  0x47A1,  0x47F4,  0x4846,  0x4898,  0x48E9,
     0x493A,  0x498A,  0x49D9,  0x4A29,  0x4A77,  0x4AC5,  0x4B13,  0x4B5F,
     0x4BAC,  0x4BF7,  0x4C42,  0x4C8D,  0x4CD7,  0x4D20,  0x4D68,  0x4DB0,
     0x4DF7,  0x4E3E,  0x4E84,  0x4EC9,  0x4F0E,  0x4F52,  0x4F95,  0x4FD7,
     0x5019,  0x505A,  0x509A,  0x50DA,  0x5118,  0x5156,  0x5194,  0x51D0,
     0x520C,  0x5247,  0x5281,  0x52BA,  0x52F3,  0x532A,  0x5361,  0x5397,
     0x53CC,  0x5401,  0x5434,  0x5467,  0x5499,  0x54CA,  0x54FA,  0x5529,
     0x5558,  0x5585,  0x55B2,  0x55DE,  0x5609,  0x5632,  0x565B,  0x5684,
     0x56AB,  0x56D1,  0x56F6,  0x571B,  0x573E,  0x5761,  0x5782,  0x57A3,
     0x57C3,  0x57E2,  0x57FF,  0x581C,  0x5838,  0x5853,  0x586D,  0x5886,
     0x589E,  0x58B5,  0x58CB,  0x58E0,  0x58F4,  0x5907,  0x5919,  0x592A,
     0x593A,  0x5949,  0x5958,  0x5965,  0x5971,  0x597C,  0x5986,  0x598F,
     0x5997,  0x599E,  0x59A4,  0x59A9,  0x59AD,  0x59B0,  0x59B2,  0x59B3,
};
/* 39-tap FIR used for reverb input/output resampling (psx-spx) */
static const int16_t reverb_fir[39] = {
    -0x0001,  0x0000,  0x0002,  0x0000, -0x000A,  0x0000,  0x0023,  0x0000,
    -0x0067,  0x0000,  0x010A,  0x0000, -0x0268,  0x0000,  0x0534,  0x0000,
    -0x0B90,  0x0000,  0x2806,  0x4000,  0x2806,  0x0000, -0x0B90,  0x0000,
     0x0534,  0x0000, -0x0268,  0x0000,  0x010A,  0x0000, -0x0067,  0x0000,
     0x0023,  0x0000, -0x000A,  0x0000,  0x0002,  0x0000, -0x0001,
};

static const int32_t adpcm_pos[5] = { 0, 60, 115, 98, 122 };
static const int32_t adpcm_neg[5] = { 0, 0, -52, -55, -60 };

/* ------------------------------------------------------------------------------------ */
/* helpers                                                                              */

static inline int32_t sx16(uint32_t v)
{
    v &= 0xFFFFu;
    return (int32_t)v - (int32_t)((v & 0x8000u) << 1);
}

static inline int32_t clamp16(int32_t v)
{
    return v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v);
}

#define VREG(v, i) S.reg[(v) * 8 + (i)]

static inline int irq_armed(void)
{
    return (S.reg[R_SPUCNT] & (CNT_ENABLE | CNT_IRQ_ENABLE)) == (CNT_ENABLE | CNT_IRQ_ENABLE) &&
           !S.irq_flag;
}

static void irq_raise(void)
{
    S.irq_flag = 1;
    spu_irq_callback();
}

/* Exact byte address match against the IRQ address (used for transfers, captures, reverb). */
static inline void irq_check_byte(uint32_t byte_addr)
{
    if (irq_armed() && ((uint32_t)S.reg[R_IRQA] << 3) == byte_addr)
        irq_raise();
}

/* ------------------------------------------------------------------------------------ */
/* envelope generator (shared by ADSR and volume sweep)                                 */

/* One 44.1 kHz envelope clock.
 *   rate    : 7-bit rate = (shift << 2) | step   (decay/release: shift << 2, step fixed)
 *   dec     : decreasing direction
 *   expo    : exponential mode
 *   neg     : negative phase (sweep only)
 *   allbits : rate value that means "never step" (7Fh, or 7Ch for decay/release) */
static int32_t env_step(int32_t level, uint32_t rate, int dec, int expo, int neg,
                        uint32_t allbits, uint16_t *counter)
{
    uint32_t shift = rate >> 2;
    int32_t  step  = 7 - (int32_t)(rate & 3);
    uint32_t inc;

    if (dec ^ neg)
        step = ~step;                               /* +7,+6,+5,+4 -> -8,-7,-6,-5 */
    if (shift < 11)
        step *= 1 << (11 - shift);
    inc = shift > 11 ? (0x8000u >> (shift - 11)) : 0x8000u;

    if (expo) {
        if (!dec) {
            if (level > 0x6000) {                   /* "fake" exponential increase */
                if (shift < 10)       step >>= 2;
                else if (shift >= 11) inc >>= 2;
                else                { step >>= 1; inc >>= 1; }
            }
        } else {
            step = (step * level) >> 15;            /* exponential decrease */
        }
    }
    if ((rate & allbits) != allbits && inc == 0)
        inc = 1;

    inc += *counter;
    if (!(inc & 0x8000u)) {
        *counter = (uint16_t)inc;
        return level;
    }
    *counter = 0;

    level += step;
    if (!dec) {
        if (level < -0x8000) level = -0x8000;
        if (level > 0x7FFF)  level = 0x7FFF;
    } else if (neg) {
        if (level < -0x8000) level = -0x8000;
        if (level > 0)       level = 0;
    } else {
        if (level < 0)       level = 0;
        if (level > 0x7FFF)  level = 0x7FFF;
    }
    return level;
}

static void adsr_tick(Voice *vc, uint32_t a1, uint32_t a2)
{
    uint32_t rate, allbits = 0x7F;
    int dec, expo;

    /* phase transitions on the current level */
    if (vc->phase == PH_ATTACK && vc->env >= 0x7FFF) {
        vc->phase = PH_DECAY;
        vc->env_counter = 0;
    }
    if (vc->phase == PH_DECAY && vc->env <= (int32_t)(((a1 & 0xF) + 1) * 0x800)) {
        vc->phase = PH_SUSTAIN;
        vc->env_counter = 0;
    }

    switch (vc->phase) {
    case PH_ATTACK:
        rate = (a1 >> 8) & 0x7F; dec = 0; expo = (int)(a1 >> 15);
        break;
    case PH_DECAY:
        rate = ((a1 >> 4) & 0xF) << 2; dec = 1; expo = 1; allbits = 0x7C;
        break;
    case PH_SUSTAIN:
        rate = (a2 >> 6) & 0x7F; dec = (int)((a2 >> 14) & 1); expo = (int)(a2 >> 15);
        break;
    default: /* release */
        if (vc->env == 0)
            return;                                 /* nothing left to release */
        rate = (a2 & 0x1F) << 2; dec = 1; expo = (int)((a2 >> 5) & 1); allbits = 0x7C;
        break;
    }
    vc->env = env_step(vc->env, rate, dec, expo, 0, allbits, &vc->env_counter);
}

/* volume register (voice VOLL/VOLR, main MVOLL/MVOLR) per 44.1 kHz clock */
static inline void sweep_tick(int32_t *level, uint16_t *counter, uint32_t r)
{
    if (r & 0x8000u)
        *level = env_step(*level, r & 0x7F, (int)((r >> 13) & 1), (int)((r >> 14) & 1),
                          (int)((r >> 12) & 1), 0x7F, counter);
}

static void volume_write(int32_t *level, uint16_t *counter, uint16_t v)
{
    if (v & 0x8000u) {
        *counter = 0;                               /* sweep starts from the current level */
    } else {
        int32_t x = v & 0x7FFF;                     /* 15-bit signed volume/2 */
        if (x & 0x4000) x -= 0x8000;
        *level = x * 2;
    }
}

/* ------------------------------------------------------------------------------------ */
/* voices                                                                               */

static void key_on(int i)
{
    Voice *vc = &S.voice[i];
    vc->cur = (uint16_t)(VREG(i, 3) & 0xFFFEu);
    vc->counter = 0;
    vc->env = 0;
    vc->env_counter = 0;
    vc->phase = PH_ATTACK;
    vc->hist[0] = vc->hist[1] = 0;
    vc->buf[0] = vc->buf[1] = vc->buf[2] = 0;
    vc->need_block = 1;
    vc->ignore_loop = 0;
    S.endx &= ~(1u << i);
}

static void key_off(int i)
{
    Voice *vc = &S.voice[i];
    if (vc->phase != PH_RELEASE) {
        vc->phase = PH_RELEASE;
        vc->env_counter = 0;
    }
}

static void force_off(Voice *vc)
{
    vc->phase = PH_RELEASE;
    vc->env = 0;
    vc->env_counter = 0;
}

/* Fetch and decode the 16-byte ADPCM block at the voice's current address. */
static void decode_block(Voice *vc)
{
    const uint16_t *p = &S.ram[(uint32_t)vc->cur << 2];
    uint32_t hdr = p[0];
    int32_t shift = (int32_t)(hdr & 0xF);
    uint32_t filt = (hdr >> 4) & 7;
    int32_t fp, fn, h0 = vc->hist[0], h1 = vc->hist[1];
    int16_t *d = &vc->buf[3];
    int w, k;

    if (irq_armed() && ((S.reg[R_IRQA] ^ vc->cur) & 0xFFFEu) == 0)
        irq_raise();

    if (shift > 12) shift = 9;
    if (filt > 4) filt = 4;
    fp = adpcm_pos[filt];
    fn = adpcm_neg[filt];

    for (w = 1; w < 8; w++) {
        uint32_t word = p[w];
        for (k = 0; k < 4; k++, word >>= 4) {
            int32_t s = sx16((word & 0xF) << 12) >> shift;
            s += (h0 * fp) >> 6;
            s += (h1 * fn) >> 6;
            s = clamp16(s);
            h1 = h0;
            h0 = s;
            *d++ = (int16_t)s;
        }
    }
    vc->hist[0] = h0;
    vc->hist[1] = h1;

    vc->flags = (uint8_t)(hdr >> 8);
    if ((vc->flags & 4) && !vc->ignore_loop)        /* loop start */
        vc->repeat = vc->cur;
    vc->need_block = 0;
}

/* The voice has played past the last sample of its block. */
static void block_end(int i, Voice *vc, uint32_t non)
{
    vc->counter -= 28u << 12;
    vc->buf[0] = vc->buf[28];
    vc->buf[1] = vc->buf[29];
    vc->buf[2] = vc->buf[30];
    if (vc->flags & 1) {                            /* loop end */
        S.endx |= 1u << i;
        vc->cur = (uint16_t)(vc->repeat & 0xFFFEu);
        if (!(vc->flags & 2) && !((non >> i) & 1))  /* no repeat: release, envelope = 0 */
            force_off(vc);
    } else {
        vc->cur = (uint16_t)(vc->cur + 2);
    }
    vc->need_block = 1;
}

static inline int32_t interpolate(const Voice *vc)
{
    uint32_t i = (vc->counter >> 4) & 0xFF;
    const int16_t *s = &vc->buf[vc->counter >> 12];
    return ((gauss_table[0x0FF - i] * s[0]) >> 15) +   /* oldest */
           ((gauss_table[0x1FF - i] * s[1]) >> 15) +   /* older  */
           ((gauss_table[0x100 + i] * s[2]) >> 15) +   /* old    */
           ((gauss_table[0x000 + i] * s[3]) >> 15);    /* new    */
}

/* ------------------------------------------------------------------------------------ */
/* reverb                                                                               */

/* Halfword address of (current buffer address + off_hw), wrapped within ESA..7FFFEh. */
static inline uint32_t rvb_addr(int32_t off_hw)
{
    uint32_t base = (uint32_t)S.reg[R_ESA] << 2;
    int32_t size = (int32_t)(SPU_RAM_HW - base);
    int32_t rel = ((int32_t)S.rvb_cur - (int32_t)base + off_hw) % size;
    if (rel < 0) rel += size;
    return base + (uint32_t)rel;
}

static inline int32_t rvb_read(int32_t off_hw)
{
    return sx16(S.ram[rvb_addr(off_hw)]);
}

static inline void rvb_write(int32_t off_hw, int32_t v)
{
    uint32_t a = rvb_addr(off_hw);
    S.ram[a] = (uint16_t)clamp16(v);
    irq_check_byte(a << 1);
}

#define RVR(i)  ((int32_t)S.reg[R_RVB + (i)])        /* address/displacement register */
#define RVV(i)  sx16(S.reg[R_RVB + (i)])             /* signed volume register */

/* One reverb channel step at 22050 Hz (ch 0 = left, 1 = right). `in` is the downsampled
 * reverb input. Returns the reverb output (before EVOL). Memory accesses follow the
 * documented hardware order; buffer writes only happen with reverb master enable. */
static int32_t rvb_channel(int ch, int32_t in, int enable)
{
    const int32_t vIIR  = RVV(RV_vIIR),  vWALL = RVV(RV_vWALL);
    const int32_t vAPF1 = RVV(RV_vAPF1), vAPF2 = RVV(RV_vAPF2);
    const int32_t mSAME = RVR(RV_mLSAME + ch) * 4, dSAME = RVR(RV_dLSAME + ch) * 4;
    const int32_t mDIFF = RVR(RV_mLDIFF + ch) * 4, dDIFF = RVR(RV_dLDIFF + (ch ^ 1)) * 4;
    const int32_t mAPF1 = RVR(RV_mLAPF1 + ch) * 4, mAPF2 = RVR(RV_mLAPF2 + ch) * 4;
    const int32_t Lin = (in * RVV(RV_vLIN + ch)) >> 15;
    int32_t d_same, p_same, d_diff, p_diff, c1, c2, c3, c4, a1, a2;
    int32_t acc, apf1_in, apf1_out, apf2_in;

    d_same = rvb_read(dSAME);
    p_same = rvb_read(mSAME - 1);
    d_diff = rvb_read(dDIFF);
    if (enable) {   /* same side reflection */
        int32_t x = clamp16(Lin + ((d_same * vWALL) >> 15));
        rvb_write(mSAME, (((x - p_same) * vIIR) >> 15) + p_same);
    }
    p_diff = rvb_read(mDIFF - 1);
    c1 = rvb_read(RVR(RV_mLCOMB1 + ch) * 4);
    if (enable) {   /* different side reflection */
        int32_t x = clamp16(Lin + ((d_diff * vWALL) >> 15));
        rvb_write(mDIFF, (((x - p_diff) * vIIR) >> 15) + p_diff);
    }
    c2 = rvb_read(RVR(RV_mLCOMB2 + ch) * 4);
    c3 = rvb_read(RVR(RV_mLCOMB3 + ch) * 4);
    c4 = rvb_read(RVR(RV_mLCOMB4 + ch) * 4);
    a1 = rvb_read(mAPF1 - RVR(RV_dAPF1) * 4);
    a2 = rvb_read(mAPF2 - RVR(RV_dAPF2) * 4);

    /* early echo (comb filter) */
    acc = ((c1 * RVV(RV_vCOMB1)) >> 15) + ((c2 * RVV(RV_vCOMB2)) >> 15) +
          ((c3 * RVV(RV_vCOMB3)) >> 15) + ((c4 * RVV(RV_vCOMB4)) >> 15);
    /* late reverb: two all-pass filters */
    apf1_in  = clamp16(acc - ((a1 * vAPF1) >> 15));
    apf1_out = clamp16(((apf1_in * vAPF1) >> 15) + a1);
    apf2_in  = clamp16(apf1_out - ((a2 * vAPF2) >> 15));
    if (enable) {
        rvb_write(mAPF1, apf1_in);
        rvb_write(mAPF2, apf2_in);
    }
    return clamp16(((apf2_in * vAPF2) >> 15) + a2);
}

/* 44.1 kHz reverb clock: feed one input sample pair, produce one output sample pair
 * (after the upsampling FIR, before EVOL). */
static void reverb_tick(int32_t inl, int32_t inr, int32_t *outl, int32_t *outr)
{
    uint32_t p = S.rvb_in_pos;
    int ch;

    S.rvb_in[0][p] = (int16_t)clamp16(inl);
    S.rvb_in[1][p] = (int16_t)clamp16(inr);
    S.rvb_in_pos = (p + 1) & 63;

    if (S.rvb_phase) {
        /* 22050 Hz step: downsample, run the reverberator, upsample (odd-phase taps) */
        int enable = (S.reg[R_SPUCNT] & CNT_REVERB) != 0;
        uint32_t q = S.rvb_out_pos;
        int32_t res[2];

        for (ch = 0; ch < 2; ch++) {
            const int16_t *x = S.rvb_in[ch];
            int32_t acc = reverb_fir[19] * x[(p - 19) & 63];
            int k;
            for (k = 0; k < 39; k += 2)
                acc += reverb_fir[k] * x[(p - 38 + (uint32_t)k) & 63];
            S.rvb_out[ch][q] = (int16_t)rvb_channel(ch, clamp16(acc >> 15), enable);
        }
        S.rvb_out_pos = (q + 1) & 31;

        /* advance the buffer address: MAX(ESA, (addr+2) AND 7FFFEh) */
        S.rvb_cur = (S.rvb_cur + 1) & (SPU_RAM_HW - 1);
        if (S.rvb_cur < ((uint32_t)S.reg[R_ESA] << 2))
            S.rvb_cur = (uint32_t)S.reg[R_ESA] << 2;

        for (ch = 0; ch < 2; ch++) {
            const int16_t *y = S.rvb_out[ch];
            int32_t acc = 0;
            int j;
            for (j = 0; j < 20; j++)
                acc += reverb_fir[2 * j] * y[(q - (uint32_t)j) & 31];
            res[ch] = clamp16(acc >> 14);
        }
        *outl = res[0];
        *outr = res[1];
    } else {
        /* even phase of the zero-stuffed signal: only the centre tap (4000h) hits a sample */
        uint32_t q = (S.rvb_out_pos - 1 - 9) & 31;
        *outl = S.rvb_out[0][q];
        *outr = S.rvb_out[1][q];
    }
    S.rvb_phase ^= 1;
}

/* ------------------------------------------------------------------------------------ */
/* public API                                                                           */

void spu_init(void)
{
    int i;
    memset(&S, 0, sizeof S);
    for (i = 0; i < NUM_VOICES; i++) {
        S.voice[i].phase = PH_RELEASE;
        S.voice[i].need_block = 1;
    }
}

static uint16_t spustat(void)
{
    uint32_t cnt = S.reg[R_SPUCNT];
    uint32_t mode = (cnt >> 4) & 3;
    uint32_t st = cnt & 0x3F;
    if (S.irq_flag)  st |= 0x0040;
    if (cnt & 0x20)  st |= 0x0080;                  /* DMA r/w request (= SPUCNT bit 5) */
    if (mode == 2)   st |= 0x0100;                  /* DMA write request */
    if (mode == 3)   st |= 0x0200;                  /* DMA read request */
    /* bit 10 (transfer busy) = 0: transfers complete instantly */
    if ((S.reg[R_RAMCTRL] & 0x0C) && (S.capture_pos & 0x100))
        st |= 0x0800;                               /* writing 2nd half of capture buffers */
    return (uint16_t)st;
}

uint16_t spu_read16(uint32_t off)
{
    off &= 0x3FE;
    if (off < 0x180) {
        const Voice *vc = &S.voice[off >> 4];
        switch ((off >> 1) & 7) {
        case 6:  return (uint16_t)vc->env;
        case 7:  return vc->repeat;
        default: return S.reg[off >> 1];
        }
    }
    if (off >= 0x200 && off < 0x260)
        return (uint16_t)S.voice[(off - 0x200) >> 2].vol[(off >> 1) & 1];
    switch (off) {
    case 0x19C: return (uint16_t)(S.endx & 0xFFFF);
    case 0x19E: return (uint16_t)((S.endx >> 16) & 0xFF);
    case 0x1AE: return spustat();
    case 0x1B8: return (uint16_t)S.mvol[0];
    case 0x1BA: return (uint16_t)S.mvol[1];
    default:    return S.reg[off >> 1];
    }
}

static inline void fifo_store(uint16_t v)
{
    S.ram[S.xfer_addr >> 1] = v;
    irq_check_byte(S.xfer_addr);
    S.xfer_addr = (S.xfer_addr + 2) & (SPU_RAM_BYTES - 1);
}

static inline uint16_t fifo_load(void)
{
    uint16_t v = S.ram[S.xfer_addr >> 1];
    irq_check_byte(S.xfer_addr);
    S.xfer_addr = (S.xfer_addr + 2) & (SPU_RAM_BYTES - 1);
    return v;
}

void spu_write16(uint32_t off, uint16_t v)
{
    int i;
    off &= 0x3FE;

    if (off < 0x180) {
        int n = (int)(off >> 4);
        Voice *vc = &S.voice[n];
        switch ((off >> 1) & 7) {
        case 0: case 1:
            S.reg[off >> 1] = v;
            volume_write(&vc->vol[(off >> 1) & 1], &vc->vol_counter[(off >> 1) & 1], v);
            break;
        case 6:                                     /* ENVX: jump the envelope level */
            vc->env = sx16(v);
            break;
        case 7:                                     /* repeat address */
            vc->repeat = v;
            vc->ignore_loop = 1;
            break;
        default:                                    /* pitch, start address, ADSR1, ADSR2 */
            S.reg[off >> 1] = v;
            break;
        }
        return;
    }
    if (off >= 0x200 && off < 0x260) {             /* internal current voice volume */
        S.voice[(off - 0x200) >> 2].vol[(off >> 1) & 1] = sx16(v);
        return;
    }

    switch (off) {
    case 0x180: case 0x182:
        S.reg[off >> 1] = v;
        volume_write(&S.mvol[(off >> 1) & 1], &S.mvol_counter[(off >> 1) & 1], v);
        break;
    case 0x188: case 0x18A: {                       /* KON */
        uint32_t bits = off == 0x188 ? v : ((uint32_t)(v & 0xFF) << 16);
        S.reg[off >> 1] = v;
        for (i = 0; i < NUM_VOICES; i++)
            if (bits & (1u << i)) key_on(i);
        break;
    }
    case 0x18C: case 0x18E: {                       /* KOFF */
        uint32_t bits = off == 0x18C ? v : ((uint32_t)(v & 0xFF) << 16);
        S.reg[off >> 1] = v;
        for (i = 0; i < NUM_VOICES; i++)
            if (bits & (1u << i)) key_off(i);
        break;
    }
    case 0x19C: case 0x19E:                         /* ENDX: read-only */
    case 0x1AE:                                     /* SPUSTAT: read-only */
        break;
    case 0x1A2:                                     /* reverb work area start */
        S.reg[off >> 1] = v;
        S.rvb_cur = (uint32_t)v << 2;
        break;
    case 0x1A6:                                     /* transfer address */
        S.reg[off >> 1] = v;
        S.xfer_addr = ((uint32_t)v << 3) & (SPU_RAM_BYTES - 1);
        break;
    case 0x1A8:                                     /* data FIFO (manual write) */
        S.reg[off >> 1] = v;
        fifo_store(v);
        break;
    case 0x1AA: {                                   /* SPUCNT */
        uint16_t old = S.reg[off >> 1];
        S.reg[off >> 1] = v;
        if (!(v & CNT_IRQ_ENABLE))
            S.irq_flag = 0;                         /* acknowledge */
        if ((old & CNT_ENABLE) && !(v & CNT_ENABLE))
            for (i = 0; i < NUM_VOICES; i++)
                force_off(&S.voice[i]);
        break;
    }
    case 0x1B8: case 0x1BA:                         /* internal current main volume */
        S.mvol[(off >> 1) & 1] = sx16(v);
        break;
    default:
        S.reg[off >> 1] = v;
        break;
    }
}

void spu_dma_write(const uint32_t *src, int words)
{
    int i;
    for (i = 0; i < words; i++) {
        fifo_store((uint16_t)(src[i] & 0xFFFF));
        fifo_store((uint16_t)(src[i] >> 16));
    }
}

void spu_dma_read(uint32_t *dst, int words)
{
    int i;
    for (i = 0; i < words; i++) {
        uint32_t lo = fifo_load();
        uint32_t hi = fifo_load();
        dst[i] = lo | (hi << 16);
    }
}

void spu_run(int samples, int16_t *out)
{
    int n;
    for (n = 0; n < samples; n++) {
        const uint32_t cnt  = S.reg[R_SPUCNT];
        const uint32_t pmon = ((uint32_t)S.reg[R_PMON0] | ((uint32_t)(S.reg[R_PMON1] & 0xFF) << 16)) & ~1u;
        const uint32_t non  = (uint32_t)S.reg[R_NON0] | ((uint32_t)(S.reg[R_NON1] & 0xFF) << 16);
        const uint32_t eon  = (uint32_t)S.reg[R_EON0] | ((uint32_t)(S.reg[R_EON1] & 0xFF) << 16);
        int32_t suml = 0, sumr = 0, rinl = 0, rinr = 0, rvl, rvr, outl, outr, prev_out = 0;
        int16_t cdl = 0, cdr = 0;
        int i;

        if (!cdrom_audio_sample(&cdl, &cdr))
            cdl = cdr = 0;

        /* ---- voices ---- */
        for (i = 0; i < NUM_VOICES; i++) {
            Voice *vc = &S.voice[i];
            uint32_t step;
            int32_t o = 0;

            if (vc->need_block)
                decode_block(vc);

            if (vc->env != 0) {
                int32_t smp = ((non >> i) & 1) ? sx16(S.noise_level) : interpolate(vc);
                o = (smp * vc->env) >> 15;
                if (o > 0x7FFF) o = 0x7FFF;
            }
            vc->out = o;
            if (o != 0) {
                int32_t l = (o * vc->vol[0]) >> 15;
                int32_t r = (o * vc->vol[1]) >> 15;
                suml += l;
                sumr += r;
                if ((eon >> i) & 1) {
                    rinl += l;
                    rinr += r;
                }
            }

            adsr_tick(vc, VREG(i, 4), VREG(i, 5));
            sweep_tick(&vc->vol[0], &vc->vol_counter[0], VREG(i, 0));
            sweep_tick(&vc->vol[1], &vc->vol_counter[1], VREG(i, 1));

            /* pitch counter */
            step = VREG(i, 2);
            if ((pmon >> i) & 1) {
                int32_t factor = prev_out + 0x8000;   /* 0.0 .. 1.99 */
                step = (uint32_t)((sx16(step) * factor) >> 15) & 0xFFFF;
            }
            if (step > 0x3FFF)
                step = 0x4000;
            vc->counter += step;
            if (vc->counter >= (28u << 12))
                block_end(i, vc, non);
            prev_out = o;
        }

        /* ---- noise generator ---- */
        {
            uint32_t lv = S.noise_level;
            uint32_t parity = ((lv >> 15) ^ (lv >> 12) ^ (lv >> 11) ^ (lv >> 10) ^ 1) & 1;
            int32_t reload = 0x20000 >> ((cnt >> 10) & 0xF);
            S.noise_timer -= (int32_t)(((cnt >> 8) & 3) + 4);
            if (S.noise_timer < 0) {
                S.noise_level = (uint16_t)((lv << 1) | parity);
                S.noise_timer += reload;
                if (S.noise_timer < 0)
                    S.noise_timer += reload;
            }
        }

        /* ---- capture buffers (CD L/R before volume, voice 1/3 after ADSR) ---- */
        {
            uint32_t pos = S.capture_pos;
            S.ram[0x000 + pos] = (uint16_t)cdl;
            S.ram[0x200 + pos] = (uint16_t)cdr;
            S.ram[0x400 + pos] = (uint16_t)S.voice[1].out;
            S.ram[0x600 + pos] = (uint16_t)S.voice[3].out;
            if ((S.reg[R_RAMCTRL] & 0x0C) && irq_armed()) {
                uint32_t ia = (uint32_t)S.reg[R_IRQA] << 3;
                if (ia < 0x1000 && (ia & 0x3FF) == pos * 2)
                    irq_raise();
            }
            S.capture_pos = (pos + 1) & 0x1FF;
        }

        /* ---- CD audio input ---- */
        if (cnt & CNT_CD_ENABLE) {
            int32_t l = (cdl * sx16(S.reg[R_AVOLL])) >> 15;
            int32_t r = (cdr * sx16(S.reg[R_AVOLR])) >> 15;
            if (cnt & CNT_CD_REVERB) {
                rinl += l;
                rinr += r;
            }
            outl = l;
            outr = r;
        } else {
            outl = outr = 0;
        }

        /* ---- reverb ---- */
        reverb_tick(rinl, rinr, &rvl, &rvr);
        /* mute (bit 14 clear) / SPU disable silence voices and reverb; CD audio passes */
        if ((cnt & (CNT_ENABLE | CNT_UNMUTE)) == (CNT_ENABLE | CNT_UNMUTE)) {
            outl += suml + ((rvl * sx16(S.reg[R_EVOLL])) >> 15);
            outr += sumr + ((rvr * sx16(S.reg[R_EVOLR])) >> 15);
        }

        /* ---- main volume ---- */
        outl = clamp16((clamp16(outl) * S.mvol[0]) >> 15);
        outr = clamp16((clamp16(outr) * S.mvol[1]) >> 15);
        sweep_tick(&S.mvol[0], &S.mvol_counter[0], S.reg[R_MVOLL]);
        sweep_tick(&S.mvol[1], &S.mvol_counter[1], S.reg[R_MVOLR]);

        if (out) {
            *out++ = (int16_t)outl;
            *out++ = (int16_t)outr;
        }
    }
}

size_t spu_state_size(void)
{
    return sizeof S;
}

void spu_save_state(void *dst)
{
    memcpy(dst, &S, sizeof S);
}

void spu_load_state(const void *src)
{
    memcpy(&S, src, sizeof S);
}
