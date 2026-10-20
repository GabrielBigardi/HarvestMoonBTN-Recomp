/* High-level reimplementation of the PS1 kernel ("BIOS").  No Sony BIOS image is needed:
 * every A0h/B0h/C0h function the game uses is implemented natively, operating directly on
 * guest memory.  The exception handler (interrupt chains, events, HookEntryInt /
 * ReturnFromException) is modelled after the original kernel's documented behaviour. */
#include "psx.h"
#include "gpu.h"
#include <setjmp.h>
#include <ctype.h>
#include <stdarg.h>

/* kernel RAM layout (first 64KB) */
#define K_A0_TABLE   0x0200u
#define K_C0_TABLE   0x0674u
#define K_B0_TABLE   0x0874u
#define K_EXCB       0xE000u     /* 4 priority chain heads, 8 bytes each */
#define K_PCB        0xE020u
#define K_TCB        0xE100u     /* 4 TCBs * C0h */
#define K_EVCB       0xE400u     /* NUM_EVCB * 1Ch */
#define K_DEFAULT_EXIT 0xE800u   /* default "exit" structure for ResetEntryInt */
#define K_PADBUF     0xE900u     /* PAD_init2 internal buffers (2*22h) */
#define K_DIRENT_TMP 0xEA00u
#define K_DCB        0xEB00u     /* 3 device control blocks * 50h */
#define K_DCB_STR    0xEC00u     /* device name strings */
#define EXC_STACK_TOP 0x80008FF0u
#define NUM_EVCB     16
#define NUM_TCB      4

/* fake kernel function addresses (BIOS ROM space; never executed, recognised by the dispatcher) */
#define FAKE_A0 0xBFC10000u
#define FAKE_B0 0xBFC20000u
#define FAKE_C0 0xBFC30000u
#define FAKE_DEV 0xBFC40000u   /* device driver entry points (not called by this game directly) */

int g_in_exception;
static jmp_buf exc_jb;
static int exc_jb_valid;

static struct {
    uint32_t hook;                /* HookEntryInt structure address (0 = default exit) */
    int clear_rcnt[4];
    int clear_pad;
    uint32_t pad_buf[2], pad_size[2];
    int pads_started;
    uint32_t pad_dr_dest;
    int card_started;
    uint32_t heap_lo, heap_hi;
    uint32_t rand_seed;
    uint32_t last_err;
    /* deferred event deliveries (memory card completions) */
    struct { uint32_t cls, spec; int frames; } pend[16];
    int npend;
} k;

/* ------------------------------------------------------------------ guest helpers */
static uint32_t gstrlen(uint32_t a) { uint32_t n = 0; while (ram_r8(a + n)) n++; return n; }
static void gread_str(uint32_t a, char *dst, size_t n) {
    size_t i = 0;
    if (!a) { dst[0] = 0; return; }
    for (; i + 1 < n; i++) { char ch = (char)MEM_R8(a + (uint32_t)i); dst[i] = ch; if (!ch) return; }
    dst[i] = 0;
}

void call_guest(CPU *c, uint32_t addr) {
    c->r[31] = 0x80000080u;   /* sentinel: the recompiled code returns with a C return */
    call_addr(c, addr);
}

/* ------------------------------------------------------------------ events */
static uint32_t evcb(uint32_t ev) { return K_EVCB + (ev & 0xFFFF) * 0x1C; }

static void deliver_event(CPU *c, uint32_t cls, uint32_t spec) {
    for (int i = 0; i < NUM_EVCB; i++) {
        uint32_t e = K_EVCB + (uint32_t)i * 0x1C;
        if (ram_r32(e + 4) != 0x2000) continue;
        if (ram_r32(e) != cls || ram_r32(e + 8) != spec) continue;
        uint32_t mode = ram_r32(e + 0xC);
        if (mode == 0x2000) {
            ram_w32(e + 4, 0x4000);
        } else if (mode == 0x1000) {
            uint32_t fn = ram_r32(e + 0x10);
            if (fn) {
                CPU save = *c;
                call_guest(c, fn);
                uint64_t cyc = c->cycles, ne = c->next_event;
                *c = save;
                c->cycles = cyc;
                c->next_event = ne;
            }
        }
    }
}

static void undeliver_event(uint32_t cls, uint32_t spec) {
    for (int i = 0; i < NUM_EVCB; i++) {
        uint32_t e = K_EVCB + (uint32_t)i * 0x1C;
        if (ram_r32(e + 4) == 0x4000 && ram_r32(e + 0xC) == 0x2000 && ram_r32(e) == cls && ram_r32(e + 8) == spec)
            ram_w32(e + 4, 0x2000);
    }
}

static void defer_event(uint32_t cls, uint32_t spec, int frames) {
    if (k.npend >= 16) { LOGW("bios: deferred event queue full"); return; }
    k.pend[k.npend].cls = cls;
    k.pend[k.npend].spec = spec;
    k.pend[k.npend].frames = frames;
    k.npend++;
}

/* ------------------------------------------------------------------ pads */
static void write_pad_buffer(uint32_t buf, int port) {
    if (!buf) return;
    if (g_pad_connected[port]) {
        uint16_t b = (uint16_t)~g_pad_buttons[port];
        ram_w8(buf + 0, 0x00);
        ram_w8(buf + 1, 0x41);
        ram_w8(buf + 2, (uint8_t)(b & 0xFF));
        ram_w8(buf + 3, (uint8_t)(b >> 8));
    } else {
        ram_w8(buf + 0, 0xFF);
        ram_w8(buf + 1, 0x00);
    }
}

static uint32_t pad_dr_value(void) {
    uint32_t v = 0;
    for (int p = 0; p < 2; p++) {
        uint16_t h = 0xFFFF;
        if (g_pad_connected[p]) {
            uint16_t b = (uint16_t)~g_pad_buttons[p];
            h = (uint16_t)(((b & 0xFF) << 8) | (b >> 8));
        }
        v |= (uint32_t)h << (16 * p);
    }
    return v;
}

void bios_vblank(void) {
    if (k.pads_started) {
        write_pad_buffer(k.pad_buf[0], 0);
        write_pad_buffer(k.pad_buf[1], 1);
        if (k.pad_dr_dest) ram_w32(k.pad_dr_dest, pad_dr_value());
    }
    for (int i = 0; i < k.npend;) {
        if (--k.pend[i].frames <= 0) {
            uint32_t cls = k.pend[i].cls, spec = k.pend[i].spec;
            k.pend[i] = k.pend[--k.npend];
            deliver_event(&g_cpu, cls, spec);
        } else {
            i++;
        }
    }
}

/* ------------------------------------------------------------------ exception handling */
static CPU exc_saved;

static void tcb_save(CPU *c) {
    uint32_t tcb = ram_r32(K_PCB);
    for (int i = 0; i < 32; i++) ram_w32(tcb + 8 + (uint32_t)i * 4, c->r[i]);
    ram_w32(tcb + 0x88, c->cur_func);
    ram_w32(tcb + 0x8C, c->hi);
    ram_w32(tcb + 0x90, c->lo);
    ram_w32(tcb + 0x94, c->sr);
    ram_w32(tcb + 0x98, c->cause);
}

static void return_from_exception(CPU *c) {
    (void)c;
    if (!g_in_exception || !exc_jb_valid) {
        LOGE("ReturnFromException outside of exception (func %08X)", c->cur_func);
        return;
    }
    longjmp(exc_jb, 1);
}

static void run_chain_element(CPU *c, uint32_t elem) {
    uint32_t f2 = ram_r32(elem + 4), f1 = ram_r32(elem + 8);
    if (!f1) return;
    c->r[29] = EXC_STACK_TOP;
    call_guest(c, f1);
    uint32_t r = c->r[2];
    if (r && f2) {
        c->r[4] = r;
        c->r[29] = EXC_STACK_TOP;
        call_guest(c, f2);
    }
}

static void default_timer_vblank_handlers(CPU *c) {
    /* EnqueueTimerAndVblankIrqs (priority 1): Vblank, Timer2, Timer1, Timer0 */
    static const int irqs[4] = { IRQ_VBLANK, IRQ_TMR2, IRQ_TMR1, IRQ_TMR0 };
    static const int rc[4] = { 3, 2, 1, 0 };
    for (int i = 0; i < 4; i++) {
        uint32_t bit = 1u << irqs[i];
        if (!(g_istat & g_imask & bit)) continue;
        deliver_event(c, 0xF2000000u + (uint32_t)rc[i], 0x0002);
        if (k.clear_rcnt[rc[i]]) {
            g_istat &= ~bit;
            return_from_exception(c);
        }
    }
}

void bios_exception_irq(CPU *c) {
    LOGT("IRQ entry: I_STAT=%03X I_MASK=%03X func=%08X", g_istat, g_imask, c->cur_func);
    exc_saved = *c;
    tcb_save(c);
    c->cause = 0x400;
    c->sr = (c->sr & ~0x3Fu) | ((c->sr << 2) & 0x3Cu);
    g_in_exception = 1;
    exc_jb_valid = 1;
    if (setjmp(exc_jb) == 0) {
        for (int prio = 0; prio < 4; prio++) {
            uint32_t elem = ram_r32(K_EXCB + (uint32_t)prio * 8);
            int guard = 0;
            while (elem && guard++ < 64) {
                uint32_t next = ram_r32(elem);
                run_chain_element(c, elem);
                elem = next;
            }
            if (prio == 1) default_timer_vblank_handlers(c);
            if (prio == 2 && k.clear_pad && (g_istat & g_imask & 1)) g_istat &= ~1u;
            if (prio == 3) {
                static const uint32_t cls[11] = { 1, 2, 3, 4, 5, 6, 6, 8, 0xB, 9, 0xA };
                uint32_t p = g_istat & g_imask;
                for (int b = 0; b < 11; b++)
                    if (p & (1u << b)) deliver_event(c, 0xF0000000u + cls[b], 0x1000);
            }
        }
        if (k.hook) {
            uint32_t h = k.hook;
            c->r[31] = ram_r32(h + 0x00);
            c->r[29] = ram_r32(h + 0x04);
            c->r[30] = ram_r32(h + 0x08);
            for (int i = 0; i < 8; i++) c->r[16 + i] = ram_r32(h + 0x0C + (uint32_t)i * 4);
            c->r[28] = ram_r32(h + 0x2C);
            c->r[2] = 1;
            call_addr(c, c->r[31]);
            LOGW("exception hook returned without ReturnFromException");
        }
    }
    /* ReturnFromException: restore the interrupted context */
    uint64_t cyc = c->cycles, ne = c->next_event;
    uint32_t sr = c->sr;
    *c = exc_saved;
    c->cycles = cyc;
    c->next_event = ne;
    c->sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
    g_in_exception = 0;
    exc_jb_valid = 0;
    c->next_event = 0;   /* re-check pending IRQs */
}

void bios_syscall(CPU *c) {
    switch (c->r[4]) {
    case 0: break;
    case 1: /* EnterCriticalSection */
        c->r[2] = ((c->sr & 0x401) == 0x401) ? 1 : 0;
        c->sr &= ~0x401u;
        break;
    case 2: /* ExitCriticalSection */
        c->sr |= 0x401u;
        c->next_event = 0;
        break;
    default:
        LOGW("syscall(%u) at func %08X", c->r[4], c->cur_func);
        deliver_event(c, 0xF0000010u, 0x4000);
        break;
    }
}

/* ------------------------------------------------------------------ memory card FS */
typedef struct { int used; int slot; int first; uint32_t size, pos; uint32_t mode; } Fd;
static Fd fds[16];
static struct { int slot; char pat[64]; int next; } search;

static int parse_bu(const char *path, char *name, size_t n) {
    if (strncmp(path, "bu", 2) != 0 || !isdigit((unsigned char)path[2]) || !isdigit((unsigned char)path[3]) || path[4] != ':')
        return -1;
    int slot = path[2] - '0';
    if (slot > 1) return -1;
    snprintf(name, n, "%s", path + 5);
    return slot;
}

static uint8_t *dir_frame(uint8_t *card, int blk) { return card + blk * 128; }
static void dir_checksum(uint8_t *f) { uint8_t x = 0; for (int i = 0; i < 127; i++) x ^= f[i]; f[127] = x; }

static int find_file(uint8_t *card, const char *name) {
    for (int b = 1; b < 16; b++) {
        uint8_t *f = dir_frame(card, b);
        if (f[0] == 0x51 && !strncmp((const char *)f + 0x0A, name, 20)) return b;
    }
    return -1;
}

static bool wild_match(const char *pat, const char *name) {
    for (int i = 0; i < 20; i++) {
        if (pat[i] == '*') return true;
        if (pat[i] == 0) return name[i] == 0;
        if (pat[i] == '?') { if (!name[i]) return true; continue; }
        if (pat[i] != name[i]) return false;
    }
    return true;
}

static int block_of(uint8_t *card, int first, uint32_t pos) {
    int b = first;
    for (uint32_t n = pos / 0x2000; n > 0 && b > 0; n--) {
        uint8_t *f = dir_frame(card, b);
        uint16_t nx = (uint16_t)(f[8] | (f[9] << 8));
        b = (nx == 0xFFFF) ? -1 : nx + 1;
    }
    return b;
}

static int32_t card_open(const char *path, uint32_t mode) {
    char name[64];
    int slot = parse_bu(path, name, sizeof name);
    if (slot < 0) { k.last_err = 0x13; LOGW("open(%s): unsupported device", path); return -1; }
    uint8_t *card = memcard_data(slot);
    if (!card) { k.last_err = 0x10; return -1; }
    int first = find_file(card, name);
    if (mode & 0x200) {
        if (first >= 0) { k.last_err = 0x11; return -1; }
        int nblk = (int)(mode >> 16);
        if (nblk < 1) nblk = 1;
        int got[16], n = 0;
        for (int b = 1; b < 16 && n < nblk; b++) if ((dir_frame(card, b)[0] & 0xF0) == 0xA0) got[n++] = b;
        if (n < nblk) { k.last_err = 0x1C; return -1; }
        for (int i = 0; i < nblk; i++) {
            uint8_t *f = dir_frame(card, got[i]);
            memset(f, 0, 128);
            f[0] = (uint8_t)(i == 0 ? 0x51 : (i == nblk - 1 ? 0x53 : 0x52));
            if (i == 0) {
                uint32_t sz = (uint32_t)nblk * 0x2000;
                memcpy(f + 4, &sz, 4);
                snprintf((char *)f + 0x0A, 21, "%s", name);
            }
            uint16_t nx = (i == nblk - 1) ? 0xFFFF : (uint16_t)(got[i + 1] - 1);
            f[8] = (uint8_t)nx;
            f[9] = (uint8_t)(nx >> 8);
            dir_checksum(f);
        }
        first = got[0];
        memcard_mark_dirty(slot);
        LOGI("memcard: created %s (%d blocks)", name, nblk);
    } else if (first < 0) {
        k.last_err = 0x02;
        return -1;
    }
    for (int fd = 2; fd < 16; fd++) {
        if (fds[fd].used) continue;
        fds[fd].used = 1;
        fds[fd].slot = slot;
        fds[fd].first = first;
        uint8_t *f = dir_frame(card, first);
        memcpy(&fds[fd].size, f + 4, 4);
        fds[fd].pos = 0;
        fds[fd].mode = mode;
        LOGD("memcard: open %s mode=%X -> fd %d", path, mode, fd);
        return fd;
    }
    k.last_err = 0x18;
    return -1;
}

static int32_t card_rw(int fd, uint32_t buf, uint32_t len, bool wr) {
    if (fd < 2 || fd > 15 || !fds[fd].used) { k.last_err = 9; return -1; }
    Fd *F = &fds[fd];
    uint8_t *card = memcard_data(F->slot);
    if (!card) { k.last_err = 0x10; return -1; }
    if ((len & 0x7F) || (F->pos & 0x7F) || F->pos >= F->size) { k.last_err = 0x16; return -1; }
    if (F->pos + len > F->size) len = F->size - F->pos;
    for (uint32_t done = 0; done < len; done += 128) {
        int b = block_of(card, F->first, F->pos);
        if (b < 0) { k.last_err = 0x10; return -1; }
        uint8_t *p = card + b * 0x2000 + (F->pos & 0x1FFF);
        if (wr) for (int i = 0; i < 128; i++) p[i] = (uint8_t)MEM_R8(buf + done + (uint32_t)i);
        else for (int i = 0; i < 128; i++) MEM_W8(buf + done + (uint32_t)i, p[i]);
        F->pos += 128;
    }
    if (wr) memcard_mark_dirty(F->slot);
    if (F->mode & 0x8000) {
        /* asynchronous mode: the kernel only queues the transfer and returns 0 ("accepted");
           completion is reported later through the file-handle and SwCARD events.  libmcrd
           re-issues the call for as long as it returns nonzero. */
        defer_event((uint32_t)fd, 0x0004, 1);
        defer_event(0xF4000001u, 0x0004, 1);
        return 0;
    }
    return (int32_t)len;
}

static uint32_t fill_dirent(uint8_t *card, int b, uint32_t de) {
    uint8_t *f = dir_frame(card, b);
    for (int i = 0; i < 20; i++) MEM_W8(de + (uint32_t)i, f[0x0A + i]);
    MEM_W8(de + 0x13, 0);
    MEM_W32(de + 0x14, 0x50);
    uint32_t sz;
    memcpy(&sz, f + 4, 4);
    MEM_W32(de + 0x18, sz);
    MEM_W32(de + 0x1C, 0);
    MEM_W32(de + 0x20, (uint32_t)b);
    MEM_W32(de + 0x24, 0);
    return de;
}

static uint32_t card_search(uint32_t de) {
    uint8_t *card = memcard_data(search.slot);
    if (!card) return 0;
    for (int b = search.next; b < 16; b++) {
        uint8_t *f = dir_frame(card, b);
        if (f[0] != 0x51) continue;
        char nm[21];
        memcpy(nm, f + 0x0A, 20);
        nm[20] = 0;
        if (wild_match(search.pat, nm)) {
            search.next = b + 1;
            return fill_dirent(card, b, de);
        }
    }
    search.next = 16;
    return 0;
}

static void card_lowlevel(CPU *c, uint32_t port, uint32_t sector, uint32_t buf, bool wr) {
    int slot = (port >> 4) & 1;
    uint8_t *card = memcard_data(slot);
    if (sector > 0x3FF) { c->r[2] = 0; return; }
    if (!card) {
        defer_event(0xF0000011u, 0x0100, 1);
        defer_event(0xF4000001u, 0x0100, 1);
        c->r[2] = 1;
        return;
    }
    uint8_t *p = card + sector * 128;
    if (wr) { for (int i = 0; i < 128; i++) p[i] = (uint8_t)MEM_R8(buf + (uint32_t)i); memcard_mark_dirty(slot); }
    else for (int i = 0; i < 128; i++) MEM_W8(buf + (uint32_t)i, p[i]);
    defer_event(0xF0000011u, 0x0004, 1);
    defer_event(0xF4000001u, 0x0004, 1);
    c->r[2] = 1;
}

/* ------------------------------------------------------------------ printf */
static void guest_printf(CPU *c, uint32_t fmt_addr, int first_arg_reg) {
    char fmt[512], out[1024];
    gread_str(fmt_addr, fmt, sizeof fmt);
    int argi = first_arg_reg;
    uint32_t sp = c->r[29];
    size_t o = 0;
    for (const char *f = fmt; *f && o < sizeof out - 64; f++) {
        if (*f != '%') { out[o++] = *f; continue; }
        char spec[32];
        int sl = 0;
        spec[sl++] = '%';
        f++;
        while (*f && strchr("-+ #0123456789.lh", *f) && sl < 30) spec[sl++] = *f++;
        if (!*f) break;
        char conv = *f;
        uint32_t arg = argi < 8 ? c->r[argi] : MEM_R32(sp + (uint32_t)argi * 4 - 16);
        argi++;
        int lsl = sl;
        spec[sl++] = conv;
        spec[sl] = 0;
        (void)lsl;
        if (conv == 's') {
            char s[256];
            gread_str(arg, s, sizeof s);
            o += (size_t)snprintf(out + o, sizeof out - o, "%s", s);
        } else if (conv == '%') {
            out[o++] = '%';
            argi--;
        } else if (strchr("dixXuoc", conv)) {
            char clean[32];
            int cl = 0;
            for (int i = 0; i < sl; i++) if (spec[i] != 'l' && spec[i] != 'h') clean[cl++] = spec[i];
            clean[cl] = 0;
            o += (size_t)snprintf(out + o, sizeof out - o, clean, arg);
        } else if (conv == 'p') {
            o += (size_t)snprintf(out + o, sizeof out - o, "%08X", arg);
        }
        if (o >= sizeof out) o = sizeof out - 1;
    }
    out[o] = 0;
    while (o && (out[o - 1] == '\n' || out[o - 1] == '\r')) out[--o] = 0;
    LOGI("[game] %s", out);
}

/* ------------------------------------------------------------------ function tables */
static void bios_a0(CPU *c, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    switch (fn) {
    case 0x13: /* setjmp */
        MEM_W32(a0 + 0x00, c->r[31]);
        MEM_W32(a0 + 0x04, c->r[29]);
        MEM_W32(a0 + 0x08, c->r[30]);
        for (int i = 0; i < 8; i++) MEM_W32(a0 + 0x0C + (uint32_t)i * 4, c->r[16 + i]);
        MEM_W32(a0 + 0x2C, c->r[28]);
        c->r[2] = 0;
        break;
    case 0x15: /* strcat */
        if (a0 && a1) { uint32_t d = a0 + gstrlen(a0); uint8_t ch; do { ch = ram_r8(a1++); ram_w8(d++, ch); } while (ch); }
        c->r[2] = a0;
        break;
    case 0x16: /* strncat */
        if (a0 && a1) { uint32_t d = a0 + gstrlen(a0); while (a2-- && ram_r8(a1)) ram_w8(d++, ram_r8(a1++)); ram_w8(d, 0); }
        c->r[2] = a0;
        break;
    case 0x17: case 0x18: { /* strcmp / strncmp */
        if (!a0 || !a1) { c->r[2] = (uint32_t)(a0 == a1 ? 0 : (a0 ? 1 : -1)); break; }
        uint32_t n = fn == 0x18 ? a2 : 0xFFFFFFFFu;
        int32_t r = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t x = ram_r8(a0 + i), y = ram_r8(a1 + i);
            if (x != y) { r = (int32_t)x - (int32_t)y; break; }
            if (!x) break;
        }
        c->r[2] = (uint32_t)r;
        break;
    }
    case 0x19: /* strcpy */
        if (a0 && a1) { uint8_t ch; uint32_t d = a0; do { ch = ram_r8(a1++); ram_w8(d++, ch); } while (ch); c->r[2] = a0; }
        else c->r[2] = 0;
        break;
    case 0x1A: /* strncpy */
        if (a0 && a1) {
            uint32_t i = 0;
            for (; i < a2 && ram_r8(a1 + i); i++) ram_w8(a0 + i, ram_r8(a1 + i));
            for (; i < a2; i++) ram_w8(a0 + i, 0);
            c->r[2] = a0;
        } else c->r[2] = 0;
        break;
    case 0x1B: c->r[2] = a0 ? gstrlen(a0) : 0; break;   /* strlen */
    case 0x1C: case 0x1E: { /* index / strchr */
        uint32_t r = 0;
        if (a0) for (uint32_t p = a0;; p++) { uint8_t ch = ram_r8(p); if (ch == (uint8_t)a1) { r = p; break; } if (!ch) break; }
        c->r[2] = r;
        break;
    }
    case 0x1D: case 0x1F: { /* rindex / strrchr */
        uint32_t r = 0;
        if (a0) for (uint32_t p = a0;; p++) { uint8_t ch = ram_r8(p); if (ch == (uint8_t)a1) r = p; if (!ch) break; }
        c->r[2] = r;
        break;
    }
    case 0x25: c->r[2] = (uint32_t)toupper((int)(a0 & 0xFF)); break;
    case 0x26: c->r[2] = (uint32_t)tolower((int)(a0 & 0xFF)); break;
    case 0x27: /* bcopy(src,dst,len) */
        for (uint32_t i = 0; i < a2; i++) MEM_W8(a1 + i, MEM_R8(a0 + i));
        c->r[2] = a1;
        break;
    case 0x28: /* bzero */
        for (uint32_t i = 0; i < a1; i++) MEM_W8(a0 + i, 0);
        c->r[2] = a0;
        break;
    case 0x29: case 0x2D: { /* bcmp / memcmp */
        int32_t r = 0;
        for (uint32_t i = 0; i < a2; i++) {
            int32_t d = (int32_t)MEM_R8(a0 + i) - (int32_t)MEM_R8(a1 + i);
            if (d) { r = d; break; }
        }
        c->r[2] = (uint32_t)r;
        break;
    }
    case 0x2A: /* memcpy */
        if ((a0 & 0x1F800000u) == 0 && (a1 & 0x1F800000u) == 0) {
            for (uint32_t i = 0; i < a2; i++) ram_w8(a0 + i, ram_r8(a1 + i));
        } else {
            for (uint32_t i = 0; i < a2; i++) MEM_W8(a0 + i, MEM_R8(a1 + i));
        }
        c->r[2] = a0;
        break;
    case 0x2B: /* memset */
        for (uint32_t i = 0; i < a2; i++) MEM_W8(a0 + i, a1);
        c->r[2] = a0;
        break;
    case 0x2C: /* memmove */
        if (a0 < a1) for (uint32_t i = 0; i < a2; i++) MEM_W8(a0 + i, MEM_R8(a1 + i));
        else for (uint32_t i = a2; i > 0; i--) MEM_W8(a0 + i - 1, MEM_R8(a1 + i - 1));
        c->r[2] = a0;
        break;
    case 0x2E: { /* memchr */
        uint32_t r = 0;
        for (uint32_t i = 0; i < a2; i++) if (MEM_R8(a0 + i) == (a1 & 0xFF)) { r = a0 + i; break; }
        c->r[2] = r;
        break;
    }
    case 0x2F: /* rand */
        k.rand_seed = k.rand_seed * 0x41C64E6Du + 0x3039u;
        c->r[2] = (k.rand_seed >> 16) & 0x7FFF;
        break;
    case 0x30: k.rand_seed = a0; break;
    case 0x39: k.heap_lo = a0; k.heap_hi = a0 + a1; LOGD("InitHeap(%08X, %X)", a0, a1); break;
    case 0x3F: guest_printf(c, a0, 5); break;
    case 0x44: break;   /* FlushCache: nothing to do for recompiled code */
    case 0x49: gpu_write_gp0(a0); break;   /* GPU_cw */
    case 0x55: case 0x70: /* _bu_init */
        memset(fds, 0, sizeof fds);
        break;
    case 0x56: case 0x72: break;   /* _96_remove: no BIOS CD handlers installed */
    case 0x9F: break;              /* SetMemSize */
    case 0xA1: /* SystemErrorBootOrDiskFailure */
        fatal("game called SystemError(%c, %X)", (char)a0, a1);
    case 0xAB: { /* _card_info */
        int slot = (a0 >> 4) & 1;
        uint32_t spec = memcard_data(slot) ? 0x0004 : 0x0100;
        defer_event(0xF0000011u, spec, 1);
        defer_event(0xF4000001u, spec, 1);
        c->r[2] = 1;
        break;
    }
    case 0xAC: { /* _card_load */
        int slot = (a0 >> 4) & 1;
        uint32_t spec = memcard_data(slot) ? 0x0004 : 0x0100;
        defer_event(0xF4000001u, spec, 1);
        c->r[2] = 1;
        break;
    }
    default:
        LOGW("unimplemented BIOS A(%02X) a0=%08X a1=%08X (func %08X)", fn, a0, a1, c->cur_func);
        c->r[2] = 0;
        break;
    }
}

static void bios_b0(CPU *c, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    switch (fn) {
    case 0x07: deliver_event(c, a0, a1); c->r[2] = 1; break;
    case 0x08: { /* OpenEvent */
        uint32_t r = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < NUM_EVCB; i++) {
            uint32_t e = K_EVCB + i * 0x1C;
            if (ram_r32(e + 4) == 0) {
                ram_w32(e + 0x00, a0);
                ram_w32(e + 0x04, 0x1000);
                ram_w32(e + 0x08, a1);
                ram_w32(e + 0x0C, a2);
                ram_w32(e + 0x10, a3);
                r = 0xF1000000u | i;
                break;
            }
        }
        LOGD("OpenEvent(%08X, %04X, %04X, %08X) -> %08X", a0, a1, a2, a3, r);
        c->r[2] = r;
        break;
    }
    case 0x09: ram_w32(evcb(a0) + 4, 0); c->r[2] = 1; break;            /* CloseEvent */
    case 0x0A: { /* WaitEvent */
        uint32_t e = evcb(a0);
        uint32_t st = ram_r32(e + 4);
        if (st == 0x4000) { ram_w32(e + 4, 0x2000); c->r[2] = 1; break; }
        if (st != 0x2000) { c->r[2] = 0; break; }
        int guard = 0;
        while (ram_r32(e + 4) != 0x4000) {
            c->cycles += 64;
            if (c->cycles >= c->next_event) events_run(c);
            if (++guard > 50000000) { LOGE("WaitEvent(%08X) never completes", a0); break; }
        }
        ram_w32(e + 4, 0x2000);
        c->r[2] = 1;
        break;
    }
    case 0x0B: { /* TestEvent */
        uint32_t e = evcb(a0);
        if (ram_r32(e + 4) == 0x4000) { ram_w32(e + 4, 0x2000); c->r[2] = 1; }
        else c->r[2] = 0;
        break;
    }
    case 0x0C: { uint32_t e = evcb(a0); if (ram_r32(e + 4)) ram_w32(e + 4, 0x2000); c->r[2] = 1; break; }
    case 0x0D: { uint32_t e = evcb(a0); if (ram_r32(e + 4)) ram_w32(e + 4, 0x1000); c->r[2] = 1; break; }
    case 0x12: /* InitPAD2 */
        k.pad_buf[0] = a0; k.pad_size[0] = a1;
        k.pad_buf[1] = a2; k.pad_size[1] = a3;
        for (uint32_t i = 0; i < a1 && a0; i++) MEM_W8(a0 + i, 0);
        for (uint32_t i = 0; i < a3 && a2; i++) MEM_W8(a2 + i, 0);
        c->r[2] = 2;
        break;
    case 0x13: k.pads_started = 1; c->r[2] = 1; break;   /* StartPAD2 */
    case 0x14: k.pads_started = 0; c->r[2] = 1; break;   /* StopPAD2 */
    case 0x15: /* PAD_init2 */
        if (a0 != 0x20000000u && a0 != 0x20000001u) { c->r[2] = 0; break; }
        k.pad_buf[0] = K_PADBUF; k.pad_buf[1] = K_PADBUF + 0x22;
        k.pad_dr_dest = a1;
        k.pads_started = 1;
        c->r[2] = 2;
        break;
    case 0x16: { uint32_t v = pad_dr_value(); ram_w32(k.pad_dr_dest, v); c->r[2] = v; break; }
    case 0x17: return_from_exception(c); break;
    case 0x18: k.hook = 0; c->r[2] = K_DEFAULT_EXIT; break;   /* ResetEntryInt */
    case 0x19: k.hook = a0; LOGD("HookEntryInt(%08X) ra=%08X", a0, MEM_R32(a0)); break;
    case 0x20: undeliver_event(a0, a1); c->r[2] = 1; break;
    case 0x32: { /* open */
        char path[128];
        gread_str(a0, path, sizeof path);
        c->r[2] = (uint32_t)card_open(path, a1);
        LOGD("open(%s, %X) -> %d (err %u)", path, a1, (int32_t)c->r[2], k.last_err);
        break;
    }
    case 0x33: { /* lseek */
        int fd = (int)a0;
        if (fd < 2 || fd > 15 || !fds[fd].used) { c->r[2] = 0xFFFFFFFFu; k.last_err = 9; break; }
        LOGD("lseek(%d, %X, %u)", fd, a1, a2);
        if (a2 == 0) fds[fd].pos = a1;
        else if (a2 == 1) fds[fd].pos += a1;
        else { c->r[2] = 0xFFFFFFFFu; k.last_err = 0x16; break; }
        c->r[2] = fds[fd].pos;
        break;
    }
    case 0x34: c->r[2] = (uint32_t)card_rw((int)a0, a1, a2, false); LOGD("read(%d, %08X, %X) -> %d", (int)a0, a1, a2, (int32_t)c->r[2]); break;
    case 0x35: c->r[2] = (uint32_t)card_rw((int)a0, a1, a2, true); LOGD("write(%d, %08X, %X) -> %d", (int)a0, a1, a2, (int32_t)c->r[2]); break;
    case 0x36: /* close */
        if ((int)a0 >= 2 && a0 < 16 && fds[a0].used) { fds[a0].used = 0; c->r[2] = a0; memcard_flush(); }
        else c->r[2] = 0xFFFFFFFFu;
        break;
    case 0x3F: { char s[256]; gread_str(a0, s, sizeof s); LOGI("[game] %s", s); break; }   /* puts */
    case 0x42: { /* firstfile */
        char path[128], name[64];
        gread_str(a0, path, sizeof path);
        int slot = parse_bu(path, name, sizeof name);
        if (slot < 0) { c->r[2] = 0; break; }
        search.slot = slot;
        snprintf(search.pat, sizeof search.pat, "%s", name[0] ? name : "*");
        search.next = 1;
        c->r[2] = card_search(a1);
        /* the kernel accesses the card (directory) here; that completion is signalled as
           a hardware card event, which libmcrd waits for */
        defer_event(0xF0000011u, memcard_data(slot) ? 0x0004 : 0x0100, 1);
        LOGD("firstfile(%s) -> %08X", path, c->r[2]);
        break;
    }
    case 0x43: /* nextfile */
        c->r[2] = card_search(a0);
        defer_event(0xF0000011u, memcard_data(search.slot) ? 0x0004 : 0x0100, 1);
        break;
    case 0x44: { /* rename */
        char p1[128], p2[128], n1[64], n2[64];
        gread_str(a0, p1, sizeof p1);
        gread_str(a1, p2, sizeof p2);
        int s1 = parse_bu(p1, n1, sizeof n1), s2 = parse_bu(p2, n2, sizeof n2);
        c->r[2] = 0;
        if (s1 < 0 || s1 != s2) break;
        uint8_t *card = memcard_data(s1);
        if (!card) break;
        int b = find_file(card, n1);
        if (b < 0 || find_file(card, n2) >= 0) break;
        uint8_t *f = dir_frame(card, b);
        memset(f + 0x0A, 0, 21);
        snprintf((char *)f + 0x0A, 21, "%s", n2);
        dir_checksum(f);
        memcard_mark_dirty(s1);
        memcard_flush();
        c->r[2] = 1;
        break;
    }
    case 0x45: { /* erase */
        char p[128], n[64];
        gread_str(a0, p, sizeof p);
        int s = parse_bu(p, n, sizeof n);
        c->r[2] = 0;
        uint8_t *card = s >= 0 ? memcard_data(s) : NULL;
        if (!card) break;
        int b = find_file(card, n);
        while (b > 0) {
            uint8_t *f = dir_frame(card, b);
            uint16_t nx = (uint16_t)(f[8] | (f[9] << 8));
            f[0] = (uint8_t)(0xA0 | (f[0] & 0x0F));
            dir_checksum(f);
            b = nx == 0xFFFF ? -1 : nx + 1;
        }
        memcard_mark_dirty(s);
        memcard_flush();
        c->r[2] = 1;
        break;
    }
    case 0x4A: k.card_started = 0; break;    /* InitCARD */
    case 0x4B: k.card_started = 1; break;    /* StartCARD */
    case 0x4C: k.card_started = 0; break;    /* StopCARD */
    case 0x4D: c->r[2] = 1; break;
    case 0x4E: card_lowlevel(c, a0, a1, a2, true); break;
    case 0x4F: card_lowlevel(c, a0, a1, a2, false); break;
    case 0x50: break;                        /* _new_card */
    case 0x51: /* Krom2RawAdd: the BIOS kanji font is not shipped; point into ROM space,
                  which reads back as zeros, so glyphs come out blank instead of garbage */
        LOGD("Krom2RawAdd(%04X)", a0);
        c->r[2] = (a0 >= 0x8140 && a0 <= 0x9872) ? 0xBFC66000u + ((a0 & 0xFFF) * 30u) : 0xFFFFFFFFu;
        break;
    case 0x54: case 0x55: c->r[2] = k.last_err; break;
    case 0x56: c->r[2] = K_C0_TABLE; break;
    case 0x57: c->r[2] = K_B0_TABLE; break;
    case 0x5B: c->r[2] = (uint32_t)k.clear_pad; k.clear_pad = (int)a0; break;   /* ChangeClearPad */
    case 0x5C: case 0x5D: c->r[2] = 1; break;  /* _card_status / _card_wait */
    default:
        LOGW("unimplemented BIOS B(%02X) a0=%08X a1=%08X (func %08X)", fn, a0, a1, c->cur_func);
        c->r[2] = 0;
        break;
    }
}

static void bios_c0(CPU *c, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5];
    switch (fn) {
    case 0x02: { /* SysEnqIntRP */
        uint32_t head = K_EXCB + (a0 & 3) * 8;
        MEM_W32(a1, ram_r32(head));
        ram_w32(head, a1);
        LOGD("SysEnqIntRP(%u, %08X) f1=%08X f2=%08X", a0, a1, MEM_R32(a1 + 8), MEM_R32(a1 + 4));
        c->r[2] = 0;
        break;
    }
    case 0x03: { /* SysDeqIntRP */
        uint32_t prev = K_EXCB + (a0 & 3) * 8;
        for (int guard = 0; guard < 64; guard++) {
            uint32_t cur = ram_r32(prev);
            if (!cur) break;
            if (cur == a1) { ram_w32(prev, ram_r32(cur)); break; }
            prev = cur;
        }
        c->r[2] = 0;
        break;
    }
    case 0x0A: /* ChangeClearRCnt */
        c->r[2] = (uint32_t)k.clear_rcnt[a0 & 3];
        k.clear_rcnt[a0 & 3] = (int)a1;
        break;
    default:
        LOGW("unimplemented BIOS C(%02X) a0=%08X a1=%08X (func %08X)", fn, a0, a1, c->cur_func);
        c->r[2] = 0;
        break;
    }
}

void bios_call(CPU *c, uint32_t vec, uint32_t fn) {
    fn &= 0xFF;
    LOGT("BIOS %02X:%02X a0=%08X a1=%08X ra=%08X", vec, fn, c->r[4], c->r[5], c->r[31]);
    if (vec == 0xA0) bios_a0(c, fn);
    else if (vec == 0xB0) bios_b0(c, fn);
    else bios_c0(c, fn);
}

/* called by the dispatcher for addresses inside the fake BIOS ROM function space */
bool bios_rom_call(CPU *c, uint32_t addr) {
    uint32_t p = addr & 0x1FFFFFFFu;
    if (p < 0x1FC00000u || p >= 0x1FC80000u) return false;
    uint32_t off = p & 0xFFFF, region = (p >> 16) & 0xF;
    if ((off & 0xF) == 0 && region >= 1 && region <= 3) {
        uint32_t fn = off >> 4;
        if ((region == 1 && fn < 0xC0) || (region == 2 && fn < 0x100) || (region == 3 && fn < 0x80)) {
            bios_call(c, region == 1 ? 0xA0 : region == 2 ? 0xB0 : 0xC0, fn);
            return true;
        }
    }
    /* jump into internal kernel code (e.g. libapi/libcard BIOS patch helpers): nothing to do */
    LOGD("call into internal BIOS code %08X ignored (func %08X)", addr, c->cur_func);
    c->r[2] = 0;
    return true;
}

/* ------------------------------------------------------------------ init */
void bios_init(void) {
    memset(&k, 0, sizeof k);
    memset(fds, 0, sizeof fds);
    for (int i = 0; i < 4; i++) k.clear_rcnt[i] = 1;
    k.clear_pad = 1;
    k.rand_seed = 1;
    /* exception vector / call vectors (as data; never executed) */
    ram_w32(0x60, 2);
    ram_w32(0x68, 0xFF);
    static const uint32_t vec[4] = { 0x3C1A0000, 0x275A0C80, 0x03400008, 0x00000000 };
    for (int i = 0; i < 4; i++) { ram_w32(0x80 + (uint32_t)i * 4, vec[i]); ram_w32((uint32_t)i * 4, vec[i]); }
    ram_w32(0x00, 0x00000003);
    for (uint32_t i = 0; i < 0xC0; i++) ram_w32(K_A0_TABLE + i * 4, FAKE_A0 + i * 0x10);
    for (uint32_t i = 0; i < 0x100; i++) ram_w32(K_B0_TABLE + i * 4, FAKE_B0 + i * 0x10);
    for (uint32_t i = 0; i < 0x80; i++) ram_w32(K_C0_TABLE + i * 4, FAKE_C0 + i * 0x10);
    /* table of tables */
    ram_w32(0x100, K_EXCB); ram_w32(0x104, 0x20);
    ram_w32(0x108, K_PCB);  ram_w32(0x10C, 4);
    ram_w32(0x110, K_TCB);  ram_w32(0x114, NUM_TCB * 0xC0);
    ram_w32(0x120, K_EVCB); ram_w32(0x124, NUM_EVCB * 0x1C);
    ram_w32(K_PCB, K_TCB);
    ram_w32(K_TCB + 0, 0x4000);
    /* Device Control Blocks: libapi's firstfile() walks this table to find the "bu" driver */
    {
        static const char *names[3] = { "cdrom", "bu", "tty" };
        static const char *descs[3] = { "CD-ROM", "MEMORY CARD", "CONSOLE" };
        static const uint32_t flags[3] = { 0x14, 0x14, 0x01 };
        static const uint32_t secsz[3] = { 0x800, 0x80, 0x01 };
        for (uint32_t d = 0; d < 3; d++) {
            uint32_t dcb = K_DCB + d * 0x50, str = K_DCB_STR + d * 0x20;
            for (uint32_t i = 0; names[d][i]; i++) ram_w8(str + i, (uint8_t)names[d][i]);
            for (uint32_t i = 0; descs[d][i]; i++) ram_w8(str + 0x10 + i, (uint8_t)descs[d][i]);
            ram_w32(dcb + 0x00, str);
            ram_w32(dcb + 0x04, flags[d]);
            ram_w32(dcb + 0x08, secsz[d]);
            ram_w32(dcb + 0x0C, str + 0x10);
            for (uint32_t f = 0x10; f < 0x50; f += 4) ram_w32(dcb + f, FAKE_DEV + d * 0x100 + f);
        }
        ram_w32(0x150, K_DCB);
        ram_w32(0x154, 3 * 0x50);
    }
    /* default exit structure: ReturnFromException */
    ram_w32(K_DEFAULT_EXIT + 0, FAKE_B0 + 0x17 * 0x10);
    ram_w32(K_DEFAULT_EXIT + 4, EXC_STACK_TOP - 4);
}
