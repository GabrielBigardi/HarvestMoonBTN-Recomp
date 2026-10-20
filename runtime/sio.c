/* SIO0: controller / memory card serial port, with the devices attached to it.
 *
 * The game polls the controllers itself (its own low-level driver talks to these registers
 * byte by byte, driven by the /ACK interrupt), so this models the port at the hardware
 * level: 8-bit full-duplex transfers at the programmed baud rate, the /ACK (DSR) pulse
 * after each byte the device wants to continue, and IRQ7.  Port 1 has a digital pad and a
 * memory card attached; port 2 is empty. */
#include "psx.h"

typedef enum { DEV_NONE, DEV_PAD, DEV_CARD } DevKind;

static struct {
    uint16_t mode, ctrl, baud;
    uint32_t stat;
    uint8_t rx[8];
    int rx_n;
    uint8_t tx_byte;
    int phase;                 /* 0 idle, 1 transferring, 2 waiting to assert /ACK, 3 /ACK low */
    bool ack_pending;
    /* device state for the current /CS selection */
    DevKind dev;
    int step;
    /* memory card command state */
    uint8_t mc_cmd, mc_msb, mc_lsb, mc_chk, mc_prev;
    uint8_t mc_buf[128];
    uint8_t mc_flag[2];
} s;

void sio_init(void) {
    memset(&s, 0, sizeof s);
    s.stat = 0x05;
    s.mc_flag[0] = s.mc_flag[1] = 0x08;   /* "new card" until the first write */
}

static int cur_port(void) { return (s.ctrl >> 13) & 1; }
static bool selected(void) { return (s.ctrl & 2) != 0; }

static uint64_t byte_cycles(void) {
    uint32_t factor = (s.mode & 3) == 2 ? 16 : (s.mode & 3) == 3 ? 64 : 1;
    uint32_t bit = (uint32_t)s.baud * factor & ~1u;
    if (bit < 2) bit = 2;
    return (uint64_t)bit * 8u;
}

static void update_irq(bool rising) {
    if (rising) {
        s.stat |= 0x200;
        irq_raise(IRQ_SIO0);
        LOGT("SIO0 IRQ raised: sr=%08X exc=%d I_MASK=%03X cyc=%llu next=%llu", g_cpu.sr, g_in_exception, g_imask,
             (unsigned long long)g_cpu.cycles, (unsigned long long)g_cpu.next_event);
    }
}

/* ------------------------------------------------------------------ devices */
static uint8_t pad_exchange(int port, uint8_t in, bool *ack) {
    (void)in;
    uint16_t btn = (uint16_t)~g_pad_buttons[port];
    switch (s.step) {
    case 1:                                        /* command: digital pads only know 42h */
        if (in != 0x42) { *ack = false; return 0xFF; }
        *ack = true; return 0x41;                  /* ID low: digital pad */
    case 2: *ack = true; return 0x5A;              /* ID high */
    case 3: *ack = true; return (uint8_t)(btn & 0xFF);
    case 4: *ack = false; return (uint8_t)(btn >> 8);
    default: *ack = false; return 0xFF;
    }
}

static uint8_t card_exchange(int port, uint8_t in, bool *ack) {
    uint8_t *card = memcard_data(port);
    int k = s.step;           /* 1 = command byte */
    uint8_t out = 0xFF;
    *ack = true;
    if (k == 1) {
        s.mc_cmd = in;
        if (in != 'R' && in != 'W' && in != 'S') { *ack = false; return s.mc_flag[port]; }
        return s.mc_flag[port];
    }
    switch (s.mc_cmd) {
    case 'R':
        switch (k) {
        case 2: out = 0x5A; break;
        case 3: out = 0x5D; break;
        case 4: s.mc_msb = in; out = 0x00; break;
        case 5: s.mc_lsb = in; out = s.mc_msb; break;
        case 6: out = 0x5C; break;
        case 7: out = 0x5D; break;
        case 8: {
            uint32_t sec = ((uint32_t)s.mc_msb << 8) | s.mc_lsb;
            if (sec > 0x3FF) { out = 0xFF; *ack = false; break; }
            memcpy(s.mc_buf, card + sec * 128, 128);
            s.mc_chk = s.mc_msb ^ s.mc_lsb;
            out = s.mc_msb;
            break;
        }
        case 9: out = s.mc_lsb; break;
        default:
            if (k >= 10 && k < 138) { out = s.mc_buf[k - 10]; s.mc_chk ^= out; }
            else if (k == 138) out = s.mc_chk;
            else if (k == 139) { out = 0x47; *ack = false; }
            else { out = 0xFF; *ack = false; }
            break;
        }
        break;
    case 'W':
        switch (k) {
        case 2: out = 0x5A; break;
        case 3: out = 0x5D; break;
        case 4: s.mc_msb = in; s.mc_chk = in; out = 0x00; break;
        case 5: s.mc_lsb = in; s.mc_chk ^= in; out = s.mc_msb; break;
        default:
            if (k >= 6 && k < 134) { s.mc_buf[k - 6] = in; s.mc_chk ^= in; out = s.mc_prev; }
            else if (k == 134) { out = s.mc_prev; s.mc_chk ^= in; /* chk now 0 if valid */ }
            else if (k == 135) out = 0x5C;
            else if (k == 136) out = 0x5D;
            else if (k == 137) {
                uint32_t sec = ((uint32_t)s.mc_msb << 8) | s.mc_lsb;
                *ack = false;
                if (sec > 0x3FF) out = 0xFF;
                else if (s.mc_chk != 0) out = 0x4E;
                else {
                    memcpy(card + sec * 128, s.mc_buf, 128);
                    memcard_mark_dirty(port);
                    s.mc_flag[port] &= (uint8_t)~0x08;
                    out = 0x47;
                }
            } else { out = 0xFF; *ack = false; }
            break;
        }
        break;
    case 'S': {
        static const uint8_t id[] = { 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80 };
        if (k >= 2 && k < 10) { out = id[k - 2]; *ack = k < 9; }
        else { out = 0xFF; *ack = false; }
        break;
    }
    }
    s.mc_prev = in;
    return out;
}

static uint8_t exchange(uint8_t in, bool *ack) {
    int port = cur_port();
    *ack = false;
    if (!selected()) return 0xFF;
    if (s.step == 0) {
        s.step = 1;
        if (in == 0x01 && g_pad_connected[port]) { s.dev = DEV_PAD; *ack = true; return 0xFF; }
        if (in == 0x81 && memcard_data(port)) { s.dev = DEV_CARD; *ack = true; return 0xFF; }
        s.dev = DEV_NONE;
        return 0xFF;
    }
    uint8_t r = 0xFF;
    if (s.dev == DEV_PAD) r = pad_exchange(port, in, ack);
    else if (s.dev == DEV_CARD) r = card_exchange(port, in, ack);
    s.step++;
    if (!*ack) s.dev = DEV_NONE;
    return r;
}

/* ------------------------------------------------------------------ transfer timing */
void sio_event(void) {
    switch (s.phase) {
    case 1: { /* byte shifted out/in */
        bool ack;
        uint8_t r = exchange(s.tx_byte, &ack);
        if (s.rx_n < 8) s.rx[s.rx_n++] = r;
        else s.rx[7] = r;
        s.stat |= 0x07;   /* TX ready, RX not empty, TX idle */
        if ((s.ctrl & 0x800) && s.rx_n >= (1 << ((s.ctrl >> 8) & 3))) update_irq(true);
        if (s.ctrl & 0x400) update_irq(true);
        if (ack) { s.phase = 2; sched_set(EV_SIO, g_cpu.cycles + 120); }
        else s.phase = 0;
        break;
    }
    case 2: /* device pulls /ACK low */
        s.stat |= 0x80;
        if (s.ctrl & 0x1000) update_irq(true);
        s.phase = 3;
        sched_set(EV_SIO, g_cpu.cycles + 100);
        break;
    case 3: /* /ACK released */
        s.stat &= ~0x80u;
        s.phase = 0;
        break;
    default: break;
    }
}

static void start_tx(void) {
    s.stat &= ~0x05u;
    s.stat &= ~0x80u;
    s.phase = 1;
    sched_set(EV_SIO, g_cpu.cycles + byte_cycles());
}

/* ------------------------------------------------------------------ registers */
uint32_t sio_read(uint32_t off, int width) {
    switch (off & 0x1F) {
    case 0x00: { /* RX data */
        uint32_t v = s.rx_n ? s.rx[0] : 0xFF;
        if (width >= 2) for (int i = 1; i < 4; i++) v |= (uint32_t)(i < s.rx_n ? s.rx[i] : s.rx[0]) << (8 * i);
        int pop = width == 4 ? 4 : 1;
        for (int i = 0; i < pop && s.rx_n; i++) { memmove(s.rx, s.rx + 1, 7); s.rx_n--; }
        if (!s.rx_n) s.stat &= ~0x02u;
        LOGT("SIO0 rx %02X (w%d)", v & 0xFF, width);
        return v;
    }
    case 0x04: return s.stat;
    case 0x08: return s.mode;
    case 0x0A: return s.ctrl;
    case 0x0E: return s.baud;
    default: return 0;
    }
}

void sio_write(uint32_t off, uint32_t v, int width) {
    (void)width;
    switch (off & 0x1F) {
    case 0x00:
        s.tx_byte = (uint8_t)v;
        LOGT("SIO0 tx %02X (ctrl=%04X step=%d)", v & 0xFF, s.ctrl, s.step);
        if (s.ctrl & 1) start_tx();
        break;
    case 0x08: s.mode = (uint16_t)(v & 0x1FF); break;
    case 0x0A: {
        uint16_t old = s.ctrl;
        LOGT("SIO0 ctrl %04X -> %04X (step=%d dev=%d)", old, v & 0xFFFF, s.step, s.dev);
        if (v & 0x40) {   /* reset */
            s.mode = 0; s.ctrl = 0; s.baud = 0; s.rx_n = 0; s.stat = 0x05; s.phase = 0;
            s.step = 0; s.dev = DEV_NONE;
            sched_cancel(EV_SIO);
            return;
        }
        if (v & 0x10) s.stat &= ~0x208u;   /* acknowledge */
        s.ctrl = (uint16_t)(v & ~0x50u);
        /* /CS deasserted or port changed: devices reset their transaction */
        if (!(s.ctrl & 2) || ((old ^ s.ctrl) & 0x2000)) { s.step = 0; s.dev = DEV_NONE; }
        if ((s.ctrl & 2) && !(old & 2)) { s.step = 0; s.dev = DEV_NONE; }
        break;
    }
    case 0x0E: s.baud = (uint16_t)v; break;
    default: break;
    }
}
