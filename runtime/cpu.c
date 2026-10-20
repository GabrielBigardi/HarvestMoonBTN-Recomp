/* CPU-side runtime: indirect-call dispatcher (overlay aware), COP0, syscall/break, diagnostics. */
#include "psx.h"

CPU g_cpu;
int g_trace_calls;

bool bios_rom_call(CPU *c, uint32_t addr);

/* ------------------------------------------------------------------ call trace ring */
#define TRACE_RING 64
static uint32_t trace_ring[TRACE_RING];
static unsigned trace_pos;

void trace_func(CPU *c, uint32_t addr) {
    trace_ring[trace_pos++ % TRACE_RING] = addr;
    if (g_trace_calls > 1) LOGI("-> %08X (ra=%08X a0=%08X a1=%08X)", addr, c->r[31], c->r[4], c->r[5]);
}

void dump_backtrace(void) {
    LOGE("current function: %08X", g_cpu.cur_func);
    if (g_trace_calls) {
        char line[1024];
        size_t o = 0;
        for (unsigned i = 0; i < TRACE_RING; i++) {
            uint32_t a = trace_ring[(trace_pos + i) % TRACE_RING];
            if (!a) continue;
            o += (size_t)snprintf(line + o, sizeof line - o, "%08X ", a);
            if (o > 900) { LOGE("  recent calls: %s", line); o = 0; }
        }
        if (o) LOGE("  recent calls: %s", line);
    }
    char regs[1024];
    size_t o = 0;
    for (int i = 0; i < 32; i++) o += (size_t)snprintf(regs + o, sizeof regs - o, "r%d=%08X%s", i, g_cpu.r[i], (i % 8 == 7) ? "\n" : " ");
    LOGE("registers:\n%s", regs);
}

/* ------------------------------------------------------------------ dispatcher */
static const FuncEntry *find_entry(const ModuleDesc *m, uint32_t a) {
    int lo = 0, hi = m->nfuncs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = m->funcs[mid].addr;
        if (v == a) return &m->funcs[mid];
        if (v < a) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static bool module_loaded(const ModuleDesc *m, const FuncEntry *e) {
    if (m->ovl_id == 0xFFFFFFFFu) return true;
    if (ram_r32(m->base) != m->ovl_id) return false;
    return ram_r32(e->addr) == e->w0 && ram_r32(e->addr + 4) == e->w1;
}

/* small direct-mapped cache of validated lookups */
#define DCACHE 4096
static struct { uint32_t addr; const FuncEntry *e; const ModuleDesc *m; } dcache[DCACHE];

RecompFunc dispatch_lookup(uint32_t addr, const char **modname) {
    uint32_t a = 0x80000000u | (addr & 0x1FFFFFFFu);
    unsigned h = (a >> 2) & (DCACHE - 1);
    if (dcache[h].addr == a && module_loaded(dcache[h].m, dcache[h].e)) {
        if (modname) *modname = dcache[h].m->name;
        return dcache[h].e->fn;
    }
    for (int i = 0; i < g_num_modules; i++) {
        const ModuleDesc *m = &g_modules[i];
        if (a < m->code_lo || a >= m->code_hi) continue;
        const FuncEntry *e = find_entry(m, a);
        if (!e || !module_loaded(m, e)) continue;
        dcache[h].addr = a;
        dcache[h].e = e;
        dcache[h].m = m;
        if (modname) *modname = m->name;
        return e->fn;
    }
    return NULL;
}

void call_addr(CPU *c, uint32_t addr) {
    uint32_t p = addr & 0x1FFFFFFFu;
    if (p == 0xA0 || p == 0xB0 || p == 0xC0) {
        bios_call(c, p, c->r[9]);
        return;
    }
    if (p >= 0x1FC00000u && p < 0x1FC80000u) {
        bios_rom_call(c, addr);
        return;
    }
    RecompFunc f = dispatch_lookup(addr, NULL);
    if (UNLIKELY(!f)) {
        /* describe which overlay (if any) is resident at that address */
        for (int i = 0; i < g_num_modules; i++) {
            const ModuleDesc *m = &g_modules[i];
            uint32_t a = 0x80000000u | p;
            if (a >= m->base && a < m->end)
                LOGE("  candidate module %s (id %X, resident id %X)%s", m->name, m->ovl_id, ram_r32(m->base),
                     find_entry(m, a) ? "" : " - no function entry at that address");
        }
        dump_backtrace();
        fatal("no recompiled function at %08X (called from function %08X, ra=%08X)", addr, c->cur_func, c->r[31]);
    }
    f(c);
}

void jumptable_miss(CPU *c, uint32_t site, uint32_t target) {
    dump_backtrace();
    fatal("jump table at %08X: unexpected target %08X (func %08X)", site, target, c->cur_func);
}

/* ------------------------------------------------------------------ COP0 / exceptions */
uint32_t cop0_read(CPU *c, int r) {
    switch (r) {
    case 12: return c->sr;
    case 13: return (c->cause & ~0x400u) | ((g_istat & g_imask) ? 0x400u : 0);
    case 14: return c->epc;
    case 15: return 0x00000002;
    default: return 0;
    }
}

void cop0_write(CPU *c, int r, uint32_t v) {
    switch (r) {
    case 12:
        if ((v & 0x10000) && !(c->sr & 0x10000)) LOGW("cache isolation enabled by game code (func %08X)", c->cur_func);
        c->sr = v;
        c->next_event = 0;
        break;
    case 13: c->cause = (c->cause & ~0x300u) | (v & 0x300u); break;
    default: break;
    }
}

void cop0_rfe(CPU *c) {
    c->sr = (c->sr & ~0x0Fu) | ((c->sr >> 2) & 0x0Fu);
    c->next_event = 0;
}

void psx_syscall(CPU *c, uint32_t pc) {
    (void)pc;
    bios_syscall(c);
    if (!g_in_exception) irq_check(c);
}

void psx_break(CPU *c, uint32_t pc, uint32_t code) {
    LOGW("break %X at %08X (func %08X) - ignored", code, pc, c->cur_func);
}

void psx_invalid(CPU *c, uint32_t pc, uint32_t word) {
    dump_backtrace();
    fatal("invalid instruction %08X at %08X (func %08X)", word, pc, c->cur_func);
}
