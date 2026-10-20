/* Cycle-based event scheduler, interrupt controller, root counters (timers) and video timing.
 *
 * Time is measured in CPU cycles (33.8688 MHz).  Recompiled code adds one cycle per executed
 * instruction and calls events_run() whenever c->cycles reaches c->next_event (checked at
 * function entries and loop back-edges).  NTSC video timing: 263 lines of 3413 GPU clocks,
 * GPU clock = CPU clock * 11/7  ->  ~59.29 Hz frame rate (non-interlaced).
 */
#include "psx.h"
#include "gpu.h"
#include "spu.h"

uint32_t g_istat, g_imask;

static uint64_t ev_time[EV_COUNT];
static uint64_t g_frame;

#define GPU_CLK_PER_LINE 3413u
#define LINES_PER_FRAME  263u
#define VBLANK_LINE      243u        /* first line of vertical blanking */

/* cycle at which scanline `line` (absolute, since boot) begins */
static inline uint64_t line_start(uint64_t line) { return (line * GPU_CLK_PER_LINE * 7u + 10u) / 11u; }
/* absolute scanline index at cycle t */
static inline uint64_t line_at(uint64_t t) { return (t * 11u) / (GPU_CLK_PER_LINE * 7u); }

uint64_t frame_count(void) { return g_frame; }
uint32_t sched_line_in_frame(void) { return (uint32_t)(line_at(g_cpu.cycles) % LINES_PER_FRAME); }

/* ------------------------------------------------------------------------- IRQ */
void irq_raise(int n) {
    g_istat |= 1u << n;
    g_cpu.next_event = 0;
}
void irq_write_stat(uint32_t v) {
    if ((g_istat & 0x80) && !(v & 0x80)) LOGT("I_STAT bit7 acked by func %08X (exc=%d)", g_cpu.cur_func, g_in_exception);
    g_istat &= v & 0x7FF;
}
void irq_write_mask(uint32_t v) { g_imask = v & 0x7FF; g_cpu.next_event = 0; }
void irq_check(CPU *c) {
    if (g_in_exception) return;
    if ((g_istat & g_imask) && (c->sr & 1) && (c->sr & 0x400))
        bios_exception_irq(c);
}

/* ------------------------------------------------------------------------- scheduler */
void sched_init(void) {
    for (int i = 0; i < EV_COUNT; i++) ev_time[i] = UINT64_MAX;
    g_frame = 0;
    ev_time[EV_VBLANK] = line_start(VBLANK_LINE);
    g_cpu.next_event = 0;
}
void sched_set(int ev, uint64_t when) {
    ev_time[ev] = when;
    if (when < g_cpu.next_event) g_cpu.next_event = when;
}
void sched_cancel(int ev) { ev_time[ev] = UINT64_MAX; }
uint64_t sched_when(int ev) { return ev_time[ev]; }

void sched_update_next(CPU *c) {
    uint64_t n = UINT64_MAX;
    for (int i = 0; i < EV_COUNT; i++) if (ev_time[i] < n) n = ev_time[i];
    /* keep IRQs that are pending-but-masked by SR from spinning: they are re-checked
       when SR/I_MASK change (those force next_event = 0). */
    c->next_event = n;
}

/* ------------------------------------------------------------------------- SPU audio */
static uint64_t spu_cycle;
static int16_t spu_buf[4096 * 2];

void spu_catch_up(void) {
    uint64_t now = g_cpu.cycles;
    if (now <= spu_cycle) return;
    uint64_t n = (now - spu_cycle) / CYCLES_PER_SAMPLE;
    while (n > 0) {
        int chunk = n > 4096 ? 4096 : (int)n;
        spu_run(chunk, spu_buf);
        platform_audio_push(spu_buf, chunk);
        n -= (uint64_t)chunk;
        spu_cycle += (uint64_t)chunk * CYCLES_PER_SAMPLE;
    }
}

void spu_irq_callback(void) { irq_raise(IRQ_SPU); }

/* ------------------------------------------------------------------------- timers */
typedef struct {
    uint32_t mode;
    uint32_t target;
    uint32_t count;        /* counter value at `base` */
    uint64_t base;         /* cycle of last sync */
    int fired;             /* one-shot mode: IRQ already delivered */
} Timer;
static Timer tm[3];

static int dot_divider(void) {
    uint32_t st = gpu_read_status();
    if (st & (1u << 16)) return 7;                      /* 368 */
    switch ((st >> 17) & 3) {
    case 0: return 10;  /* 256 */
    case 1: return 8;   /* 320 */
    case 2: return 5;   /* 512 */
    default: return 4;  /* 640 */
    }
}

static bool timer_stopped(int i) {
    Timer *t = &tm[i];
    if (!(t->mode & 1)) return false;
    uint32_t sm = (t->mode >> 1) & 3;
    if (i == 2) return sm == 0 || sm == 3;
    return false;   /* T0/T1 sync modes: approximated as free running */
}

/* number of timer ticks between cycles a and b */
static uint64_t timer_ticks(int i, uint64_t a, uint64_t b) {
    uint32_t src = (tm[i].mode >> 8) & 3;
    if (i == 0 && (src & 1)) {
        uint64_t d = (uint64_t)dot_divider() * 7u;
        return (b * 11u) / d - (a * 11u) / d;
    }
    if (i == 1 && (src & 1)) return line_at(b) - line_at(a);
    if (i == 2 && (src & 2)) return b / 8u - a / 8u;
    return b - a;
}

/* cycle at which `ticks` more ticks will have elapsed after cycle `from` */
static uint64_t timer_cycle_after(int i, uint64_t from, uint64_t ticks) {
    uint32_t src = (tm[i].mode >> 8) & 3;
    if (i == 0 && (src & 1)) {
        uint64_t d = (uint64_t)dot_divider() * 7u;
        uint64_t k = (from * 11u) / d + ticks;
        return (k * d + 10u) / 11u;
    }
    if (i == 1 && (src & 1)) return line_start(line_at(from) + ticks);
    if (i == 2 && (src & 2)) return (from / 8u + ticks) * 8u;
    return from + ticks;
}

static void timer_sync(int i) {
    Timer *t = &tm[i];
    uint64_t now = g_cpu.cycles;
    if (timer_stopped(i)) { t->base = now; return; }
    uint64_t ticks = timer_ticks(i, t->base, now);
    t->base = now;
    if (!ticks) return;
    uint64_t cnt = (uint64_t)t->count + ticks;
    uint32_t tgt = t->target & 0xFFFF;
    if (t->mode & 8) {
        /* reset to 0 after reaching target */
        if (t->count <= tgt) {
            if (cnt >= tgt) t->mode |= 0x800;
            if (cnt > tgt) cnt = (cnt - tgt - 1) % ((uint64_t)tgt + 1);
        } else {
            if (cnt > 0xFFFF) {
                t->mode |= 0x1000;
                cnt -= 0x10000;
                if (cnt >= tgt) t->mode |= 0x800;
                if (cnt > tgt) cnt = (cnt - tgt - 1) % ((uint64_t)tgt + 1);
            }
        }
    } else {
        if (t->count < tgt && cnt >= tgt) t->mode |= 0x800;
        if (cnt > 0xFFFF) {
            t->mode |= 0x1000;
            cnt &= 0xFFFF;
            if (cnt >= tgt) t->mode |= 0x800;
        }
    }
    t->count = (uint32_t)cnt;
}

static void timer_schedule(int i) {
    Timer *t = &tm[i];
    int ev = EV_TIMER0 + i;
    if (!(t->mode & 0x30) || timer_stopped(i) || ((t->mode & 0x40) == 0 && t->fired)) {
        sched_cancel(ev);
        return;
    }
    uint32_t tgt = t->target & 0xFFFF;
    uint64_t best = UINT64_MAX;
    if (t->mode & 0x10) {
        uint64_t d;
        if (t->count < tgt) d = tgt - t->count;
        else if (t->mode & 8) d = (uint64_t)(0x10000 - t->count) + tgt;   /* wraps via FFFF first */
        else d = (uint64_t)(0x10000 - t->count) + tgt;
        if (t->count == tgt) d = (t->mode & 8) ? (uint64_t)tgt + 1 : 0x10000;
        if (d < best) best = d;
    }
    if (t->mode & 0x20) {
        uint64_t d = 0xFFFFu - t->count;
        if ((t->mode & 8) && t->count <= tgt) d = UINT64_MAX;    /* never reaches FFFF */
        if (d == 0) d = 0x10000;
        if (d < best) best = d;
    }
    if (best == UINT64_MAX) { sched_cancel(ev); return; }
    sched_set(ev, timer_cycle_after(i, g_cpu.cycles, best));
}

static void timer_event(int i) {
    Timer *t = &tm[i];
    /* the event is scheduled exactly at the target / FFFFh crossing */
    timer_sync(i);
    if ((t->mode & 0x40) || !t->fired) {
        t->fired = 1;
        if (t->mode & 0x80) t->mode ^= 0x400;     /* toggle mode */
        irq_raise(IRQ_TMR0 + i);                  /* pulse mode: bit10 reads back as 1 */
    }
    timer_schedule(i);
}

void timers_init(void) {
    memset(tm, 0, sizeof tm);
    for (int i = 0; i < 3; i++) tm[i].mode = 0x400;
}

uint32_t timer_read(uint32_t off) {
    int i = (int)(off >> 4);
    if (i > 2) return 0;
    timer_sync(i);
    switch (off & 0xC) {
    case 0: return tm[i].count & 0xFFFF;
    case 4: { uint32_t m = tm[i].mode; tm[i].mode &= ~0x1800u; return m; }
    case 8: return tm[i].target;
    }
    return 0;
}

void timer_write(uint32_t off, uint32_t v) {
    int i = (int)(off >> 4);
    if (i > 2) return;
    timer_sync(i);
    switch (off & 0xC) {
    case 0: tm[i].count = v & 0xFFFF; break;
    case 4:
        tm[i].mode = (v & 0x3FF) | 0x400;
        tm[i].count = 0;
        tm[i].fired = 0;
        tm[i].base = g_cpu.cycles;
        LOGD("timer%d mode=%04X target=%04X", i, v, tm[i].target);
        break;
    case 8: tm[i].target = v & 0xFFFF; break;
    }
    timer_schedule(i);
}

/* ------------------------------------------------------------------------- video */
static int call_pending;

/* testing hook: run a guest function on a scratch stack, preserving the CPU state */
static void run_call_test(CPU *c) {
    int k = call_pending - 1;
    call_pending = 0;
    CPU save = *c;
    c->r[4] = g_cfg.call_a0[k];
    c->r[29] = (save.r[29] - 0x2000) & ~7u;
    LOGI("calltest: %08X(%X)  SPUCNT=%04X CDVOL=%04X/%04X MAINVOL=%04X/%04X", g_cfg.call_addr[k], g_cfg.call_a0[k],
         spu_read16(0x1AA), spu_read16(0x1B0), spu_read16(0x1B2), spu_read16(0x180), spu_read16(0x182));
    call_guest(c, g_cfg.call_addr[k]);
    uint64_t cyc = c->cycles, ne = c->next_event;
    *c = save;
    c->cycles = cyc;
    c->next_event = ne;
}

static void on_vblank(void) {
    g_frame++;
    for (int k = 0; k < 4; k++)
        if (g_cfg.call_frame[k] && g_frame == (uint64_t)g_cfg.call_frame[k]) call_pending = k + 1;
    for (int k = 0; k < 4; k++)
        if (g_cfg.poke_frame[k] && g_frame == (uint64_t)g_cfg.poke_frame[k]) {
            if ((g_cfg.poke_addr[k] & 0x1F800000u) == 0) ram_w32(g_cfg.poke_addr[k], g_cfg.poke_val[k]);   /* RAM: 32-bit */
            else mem_write16_slow(g_cfg.poke_addr[k], g_cfg.poke_val[k]);                                 /* I/O: 16-bit */
        }
    irq_raise(IRQ_VBLANK);
    gpu_vblank();
    bios_vblank();
    spu_catch_up();
    platform_frame();
    uint64_t next_line = (line_at(g_cpu.cycles) / LINES_PER_FRAME + 1) * LINES_PER_FRAME + VBLANK_LINE;
    sched_set(EV_VBLANK, line_start(next_line));
}

/* ------------------------------------------------------------------------- dispatch */
static int in_events;

void events_run(CPU *c) {
    in_events++;
    for (;;) {
        uint64_t now = c->cycles;
        int fired = 0;
        for (int i = 0; i < EV_COUNT; i++) {
            if (ev_time[i] <= now) {
                ev_time[i] = UINT64_MAX;
                fired = 1;
                switch (i) {
                case EV_VBLANK: on_vblank(); break;
                case EV_TIMER0: case EV_TIMER1: case EV_TIMER2: timer_event(i - EV_TIMER0); break;
                case EV_CDROM: cdrom_event(); break;
                case EV_CDROM_READ: cdrom_read_event(); break;
                case EV_SIO: sio_event(); break;
                default: break;
                }
            }
        }
        if (!fired) break;
    }
    in_events--;
    if (in_events == 0) {
        /* Like the CPU after RFE: keep taking interrupts while one is pending and enabled
           (an IRQ raised while the previous handler ran must be serviced right away). */
        for (int guard = 0; guard < 16; guard++) {
            if (g_in_exception || !((g_istat & g_imask) && (c->sr & 1) && (c->sr & 0x400))) break;
            bios_exception_irq(c);
        }
    }
    if (call_pending && in_events == 0 && !g_in_exception) run_call_test(c);
    sched_update_next(c);
    if (g_quit) {
        memcard_flush();
        exit(0);
    }
}
