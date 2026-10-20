/* DMA controller.  Transfers complete instantly (the CPU is not stalled), then the channel
 * completion interrupt is raised exactly as the hardware would at the end of the transfer. */
#include "psx.h"
#include "gpu.h"
#include "spu.h"

typedef struct { uint32_t madr, bcr, chcr; } DmaChan;
static DmaChan ch[7];
static uint32_t dpcr = 0x07654321, dicr;

void dma_init(void) {
    memset(ch, 0, sizeof ch);
    dpcr = 0x07654321;
    dicr = 0;
}

static void dicr_update(void) {
    uint32_t flags = (dicr >> 24) & 0x7F, en = (dicr >> 16) & 0x7F;
    bool master = (dicr & 0x8000) || ((dicr & 0x800000) && (flags & en));
    uint32_t old = dicr & 0x80000000u;
    dicr = (dicr & 0x7FFFFFFFu) | (master ? 0x80000000u : 0);
    if (master && !old) irq_raise(IRQ_DMA);
}

static void finish(int n) {
    ch[n].chcr &= ~0x11000000u;
    if (dicr & (1u << (16 + n))) {
        dicr |= 1u << (24 + n);
    }
    dicr_update();
}

static uint32_t tmpbuf[0x10000 * 2];

static void run_channel(int n) {
    DmaChan *d = &ch[n];
    uint32_t chcr = d->chcr;
    uint32_t sync = (chcr >> 9) & 3;
    bool from_ram = chcr & 1;
    int step = (chcr & 2) ? -4 : 4;
    uint32_t addr = d->madr & 0x1FFFFC;

    if (n == 2 && sync == 2) {
        /* GPU linked list (ordering table) */
        int guard = 0;
        for (;;) {
            uint32_t hdr = ram_r32(addr);
            uint32_t cnt = hdr >> 24;
            for (uint32_t k = 0; k < cnt; k++) {
                uint32_t w = ram_r32((addr + 4 + k * 4) & 0x1FFFFC);
                gpu_write_gp0(w);
            }
            if (hdr & 0x800000) break;
            addr = hdr & 0x1FFFFC;
            if (++guard > 0x100000) { LOGE("GPU DMA linked list loop"); break; }
        }
        d->madr = 0x00FFFFFF;
        finish(n);
        return;
    }

    uint32_t words;
    if (sync == 0) {
        words = d->bcr & 0xFFFF;
        if (!words) words = 0x10000;
    } else {
        uint32_t bs = d->bcr & 0xFFFF, ba = d->bcr >> 16;
        if (!bs) bs = 0x10000;
        if (!ba) ba = 0x10000;
        words = bs * ba;
        if (words > 0x20000) words = 0x20000;
    }

    switch (n) {
    case 6: /* OTC: reverse clear ordering table */
        for (uint32_t k = 0; k < words; k++) {
            uint32_t a = (addr - k * 4) & 0x1FFFFC;
            ram_w32(a, (k == words - 1) ? 0x00FFFFFF : ((a - 4) & 0x1FFFFF));
        }
        break;
    case 2: /* GPU block */
        if (from_ram) {
            for (uint32_t k = 0; k < words; k++) tmpbuf[k] = ram_r32((addr + (uint32_t)step * k) & 0x1FFFFC);
            gpu_dma_write(tmpbuf, (int)words);
        } else {
            gpu_dma_read(tmpbuf, (int)words);
            for (uint32_t k = 0; k < words; k++) ram_w32((addr + (uint32_t)step * k) & 0x1FFFFC, tmpbuf[k]);
        }
        break;
    case 3: /* CDROM -> RAM */
        cdrom_dma_read(tmpbuf, (int)words);
        for (uint32_t k = 0; k < words; k++) ram_w32((addr + (uint32_t)step * k) & 0x1FFFFC, tmpbuf[k]);
        break;
    case 4: /* SPU */
        spu_catch_up();
        if (from_ram) {
            for (uint32_t k = 0; k < words; k++) tmpbuf[k] = ram_r32((addr + (uint32_t)step * k) & 0x1FFFFC);
            spu_dma_write(tmpbuf, (int)words);
        } else {
            spu_dma_read(tmpbuf, (int)words);
            for (uint32_t k = 0; k < words; k++) ram_w32((addr + (uint32_t)step * k) & 0x1FFFFC, tmpbuf[k]);
        }
        break;
    default:
        LOGW("DMA channel %d transfer ignored (chcr=%08X)", n, chcr);
        break;
    }
    if (sync != 0) {
        d->madr = (addr + (uint32_t)step * words) & 0xFFFFFF;
        if (sync == 1) d->bcr &= 0xFFFF;   /* block count reaches zero */
    }
    finish(n);
}

uint32_t dma_read(uint32_t off) {
    int n = (int)(off >> 4);
    if (n < 7) {
        switch (off & 0xC) {
        case 0: return ch[n].madr;
        case 4: return ch[n].bcr;
        case 8: return ch[n].chcr;
        default: return 0;
        }
    }
    if (off == 0x70) return dpcr;
    if (off == 0x74) return dicr;
    return 0;
}

void dma_write(uint32_t off, uint32_t v) {
    int n = (int)(off >> 4);
    if (n < 7) {
        switch (off & 0xC) {
        case 0: ch[n].madr = v & 0xFFFFFF; break;
        case 4: ch[n].bcr = v; break;
        case 8:
            if (n == 6) v = (v & 0x51000000u) | 2;   /* OTC: only start/trigger bits writable, step -4 */
            ch[n].chcr = v;
            if ((v & 0x01000000u) && (((v >> 9) & 3) != 0 || (v & 0x10000000u))) {
                if (!(dpcr & (8u << (n * 4))))
                    LOGD("DMA%d started while disabled in DPCR", n);
                run_channel(n);
            }
            break;
        }
        return;
    }
    if (off == 0x70) { dpcr = v; return; }
    if (off == 0x74) {
        uint32_t ack = v & 0x7F000000u;
        dicr = (dicr & 0xFF000000u & ~ack) | (v & 0x00FF803Fu);
        dicr_update();
        return;
    }
}
