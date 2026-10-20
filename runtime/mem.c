/* Memory map: RAM, scratchpad and the I/O register space. */
#include "psx.h"
#include "gpu.h"
#include "spu.h"

uint8_t g_ram[0x200000];
uint8_t g_scratch[0x400];

static uint32_t memctrl[16];
static uint32_t ram_size_reg = 0x00000B88;
static uint32_t cache_ctrl;

void mem_reset(void) {
    memset(g_ram, 0, sizeof g_ram);
    memset(g_scratch, 0, sizeof g_scratch);
    memset(memctrl, 0, sizeof memctrl);
}

static int warned_unmapped;

static void warn_access(const char *what, uint32_t a, uint32_t v) {
    if (warned_unmapped < 64) {
        warned_unmapped++;
        LOGW("unmapped %s %08X = %08X (in func %08X)", what, a, v, g_cpu.cur_func);
    }
}

/* ---------------------------------------------------------------- I/O reads */
static uint32_t io_read(uint32_t p, int width) {
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        spu_catch_up();
        if (width == 4)
            return spu_read16(p - 0x1F801C00) | ((uint32_t)spu_read16(p - 0x1F801C00 + 2) << 16);
        if (width == 1) {
            uint16_t h = spu_read16((p & ~1u) - 0x1F801C00);
            return (p & 1) ? (h >> 8) : (h & 0xFF);
        }
        return spu_read16(p - 0x1F801C00);
    }
    if (p >= 0x1F801800 && p < 0x1F801804) {
        uint32_t v = cdrom_read8(p & 3);
        if (width >= 2) v |= (uint32_t)cdrom_read8(p & 3) << 8;
        if (width == 4) { v |= (uint32_t)cdrom_read8(p & 3) << 16; v |= (uint32_t)cdrom_read8(p & 3) << 24; }
        return v;
    }
    if (p >= 0x1F801810 && p < 0x1F801818) {
        uint32_t v = (p & 4) ? gpu_read_status() : gpu_read_data();
        return v >> ((p & 3) * 8);
    }
    if (p >= 0x1F801080 && p < 0x1F801100) {
        uint32_t v = dma_read(p & 0x7C);
        return v >> ((p & 3) * 8);
    }
    if (p >= 0x1F801100 && p < 0x1F801130) {
        uint32_t v = timer_read(p & 0x3F);
        return v >> ((p & 3) * 8);
    }
    if (p >= 0x1F801070 && p < 0x1F801078) {
        uint32_t v = (p & 4) ? g_imask : g_istat;
        return v >> ((p & 3) * 8);
    }
    if (p >= 0x1F801040 && p < 0x1F801060) return sio_read(p - 0x1F801040, width);
    if (p >= 0x1F801000 && p < 0x1F801040) return memctrl[(p >> 2) & 15] >> ((p & 3) * 8);
    if (p >= 0x1F801060 && p < 0x1F801064) return ram_size_reg;
    if (p >= 0x1F801820 && p < 0x1F801828) {
        /* MDEC: not used by this game; report idle */
        return (p & 4) ? 0x80040000u : 0;
    }
    if (p >= 0x1F000000 && p < 0x1F800000) return 0xFFFFFFFFu;   /* expansion 1: open bus */
    if (p >= 0x1F802000 && p < 0x1F804000) return 0;              /* expansion 2 (debug) */
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0;              /* BIOS ROM (not present) */
    warn_access("read", p, 0);
    return 0;
}

/* ---------------------------------------------------------------- I/O writes */
static void io_write(uint32_t p, uint32_t v, int width) {
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        spu_catch_up();
        if (width == 4) {
            spu_write16(p - 0x1F801C00, (uint16_t)v);
            spu_write16(p - 0x1F801C00 + 2, (uint16_t)(v >> 16));
        } else if (width == 2) {
            spu_write16(p - 0x1F801C00, (uint16_t)v);
        } else {
            uint32_t a = (p & ~1u) - 0x1F801C00;
            uint16_t h = spu_read16(a);
            h = (p & 1) ? (uint16_t)((h & 0x00FF) | ((v & 0xFF) << 8)) : (uint16_t)((h & 0xFF00) | (v & 0xFF));
            spu_write16(a, h);
        }
        return;
    }
    if (p >= 0x1F801800 && p < 0x1F801804) { cdrom_write8(p & 3, (uint8_t)v); return; }
    if (p == 0x1F801810) { gpu_write_gp0(v); return; }
    if (p == 0x1F801814) { gpu_write_gp1(v); return; }
    if (p >= 0x1F801080 && p < 0x1F801100) {
        uint32_t off = p & 0x7C, sh = (p & 3) * 8;
        if (width < 4) {
            uint32_t mask = (width == 1 ? 0xFFu : 0xFFFFu) << sh;
            v = (dma_read(off) & ~mask) | ((v << sh) & mask);
        }
        dma_write(off, v);
        return;
    }
    if (p >= 0x1F801100 && p < 0x1F801130) { timer_write(p & 0x3F, v & 0xFFFF); return; }
    if (p >= 0x1F801070 && p < 0x1F801078) {
        if (p & 4) irq_write_mask(v);
        else irq_write_stat(v);
        return;
    }
    if (p >= 0x1F801040 && p < 0x1F801060) { sio_write(p - 0x1F801040, v, width); return; }
    if (p >= 0x1F801000 && p < 0x1F801040) { memctrl[(p >> 2) & 15] = v; return; }
    if (p >= 0x1F801060 && p < 0x1F801064) { ram_size_reg = v; return; }
    if (p >= 0x1F801820 && p < 0x1F801828) { LOGD("MDEC write %08X=%08X", p, v); return; }
    if (p >= 0x1F802000 && p < 0x1F804000) return;            /* expansion 2: POST/debug */
    if (p >= 0x1FC00000 && p < 0x1FC80000) return;            /* BIOS ROM: libcard/libapi patch writes */
    if (p >= 0x1F000000 && p < 0x1F800000) return;
    warn_access("write", p, v);
}

/* ---------------------------------------------------------------- slow paths */
static inline int is_scratch(uint32_t a) {
    return (a & 0x7FFFFC00u) == 0x1F800000u;    /* KUSEG/KSEG0 only (not KSEG1) */
}

uint32_t mem_read8_slow(uint32_t a) {
    if (is_scratch(a)) return g_scratch[a & 0x3FF];
    if ((a & 0xFFFFFF00u) == 0xFFFE0100u) return cache_ctrl >> ((a & 3) * 8);
    return io_read(a & 0x1FFFFFFF, 1) & 0xFF;
}
uint32_t mem_read16_slow(uint32_t a) {
    if (is_scratch(a)) { uint16_t v; memcpy(&v, g_scratch + (a & 0x3FE), 2); return v; }
    if ((a & 0xFFFFFF00u) == 0xFFFE0100u) return cache_ctrl >> ((a & 2) * 8);
    return io_read(a & 0x1FFFFFFF, 2) & 0xFFFF;
}
uint32_t mem_read32_slow(uint32_t a) {
    if (is_scratch(a)) { uint32_t v; memcpy(&v, g_scratch + (a & 0x3FC), 4); return v; }
    if ((a & 0xFFFFFF00u) == 0xFFFE0100u) return cache_ctrl;
    return io_read(a & 0x1FFFFFFF, 4);
}
void mem_write8_slow(uint32_t a, uint32_t v) {
    if (is_scratch(a)) { g_scratch[a & 0x3FF] = (uint8_t)v; return; }
    if ((a & 0xFFFFFF00u) == 0xFFFE0100u) return;
    io_write(a & 0x1FFFFFFF, v & 0xFF, 1);
}
void mem_write16_slow(uint32_t a, uint32_t v) {
    if (is_scratch(a)) { uint16_t t = (uint16_t)v; memcpy(g_scratch + (a & 0x3FE), &t, 2); return; }
    if ((a & 0xFFFFFF00u) == 0xFFFE0100u) return;
    io_write(a & 0x1FFFFFFF, v & 0xFFFF, 2);
}
void mem_write32_slow(uint32_t a, uint32_t v) {
    if (is_scratch(a)) { memcpy(g_scratch + (a & 0x3FC), &v, 4); return; }
    if ((a & 0xFFFFFF00u) == 0xFFFE0100u) { cache_ctrl = v; return; }
    io_write(a & 0x1FFFFFFF, v, 4);
}
