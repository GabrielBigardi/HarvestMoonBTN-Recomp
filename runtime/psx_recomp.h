/* Interface between the recompiled game code (generated sources) and the native runtime. */
#pragma once
#include <stdint.h>
#include <string.h>
#include "gte.h"

#if defined(__GNUC__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif

typedef struct CPU {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t sr;            /* cop0 r12 */
    uint32_t cause;         /* cop0 r13 */
    uint32_t epc;           /* cop0 r14 */
    uint32_t cur_func;      /* address of the most recently entered function (diagnostics) */
    uint64_t cycles;        /* CPU cycles elapsed since boot (approximate: 1 per instruction) */
    uint64_t next_event;    /* cycle count at which events_run() must be called */
} CPU;

typedef void (*RecompFunc)(CPU *c);
/* w0/w1: the function's first two instruction words, used to verify that the overlay
 * holding this function is the one currently resident in RAM */
typedef struct { uint32_t addr; RecompFunc fn; uint32_t w0, w1; } FuncEntry;
typedef struct {
    const char *name;
    uint32_t base, end, code_lo, code_hi;
    uint32_t ovl_id;        /* 0xFFFFFFFF for the main executable */
    int file_index;         /* A_FILE.BIN sub-file index, -1 for main */
    const FuncEntry *funcs;
    int nfuncs;
} ModuleDesc;
extern const ModuleDesc g_modules[];
extern const int g_num_modules;

extern uint8_t g_ram[0x200000];
extern uint8_t g_scratch[0x400];

/* slow paths (I/O, scratchpad, BIOS, unmapped) */
uint32_t mem_read8_slow(uint32_t a);
uint32_t mem_read16_slow(uint32_t a);
uint32_t mem_read32_slow(uint32_t a);
void     mem_write8_slow(uint32_t a, uint32_t v);
void     mem_write16_slow(uint32_t a, uint32_t v);
void     mem_write32_slow(uint32_t a, uint32_t v);

/* RAM is 2MB mirrored 4x in the first 8MB of each segment (KUSEG/KSEG0/KSEG1). */
#define RAM_HIT(a) (((a) & 0x1F800000u) == 0)

static inline uint32_t MEM_R8(uint32_t a) {
    if (LIKELY(RAM_HIT(a))) return g_ram[a & 0x1FFFFF];
    return mem_read8_slow(a);
}
static inline uint32_t MEM_R16(uint32_t a) {
    if (LIKELY(RAM_HIT(a))) { uint16_t v; memcpy(&v, g_ram + (a & 0x1FFFFE), 2); return v; }
    return mem_read16_slow(a);
}
static inline uint32_t MEM_R32(uint32_t a) {
    if (LIKELY(RAM_HIT(a))) { uint32_t v; memcpy(&v, g_ram + (a & 0x1FFFFC), 4); return v; }
    return mem_read32_slow(a);
}
static inline void MEM_W8(uint32_t a, uint32_t v) {
    if (LIKELY(RAM_HIT(a))) { g_ram[a & 0x1FFFFF] = (uint8_t)v; return; }
    mem_write8_slow(a, v);
}
static inline void MEM_W16(uint32_t a, uint32_t v) {
    if (LIKELY(RAM_HIT(a))) { uint16_t t = (uint16_t)v; memcpy(g_ram + (a & 0x1FFFFE), &t, 2); return; }
    mem_write16_slow(a, v);
}
static inline void MEM_W32(uint32_t a, uint32_t v) {
    if (LIKELY(RAM_HIT(a))) { memcpy(g_ram + (a & 0x1FFFFC), &v, 4); return; }
    mem_write32_slow(a, v);
}

/* unaligned load/store (little endian R3000A semantics) */
static inline uint32_t psx_lwl(uint32_t addr, uint32_t cur) {
    uint32_t w = MEM_R32(addr & ~3u);
    switch (addr & 3) {
    case 0: return (cur & 0x00FFFFFFu) | (w << 24);
    case 1: return (cur & 0x0000FFFFu) | (w << 16);
    case 2: return (cur & 0x000000FFu) | (w << 8);
    default: return w;
    }
}
static inline uint32_t psx_lwr(uint32_t addr, uint32_t cur) {
    uint32_t w = MEM_R32(addr & ~3u);
    switch (addr & 3) {
    case 0: return w;
    case 1: return (cur & 0xFF000000u) | (w >> 8);
    case 2: return (cur & 0xFFFF0000u) | (w >> 16);
    default: return (cur & 0xFFFFFF00u) | (w >> 24);
    }
}
static inline void psx_swl(uint32_t addr, uint32_t v) {
    uint32_t a = addr & ~3u, w = MEM_R32(a);
    switch (addr & 3) {
    case 0: w = (w & 0xFFFFFF00u) | (v >> 24); break;
    case 1: w = (w & 0xFFFF0000u) | (v >> 16); break;
    case 2: w = (w & 0xFF000000u) | (v >> 8); break;
    default: w = v; break;
    }
    MEM_W32(a, w);
}
static inline void psx_swr(uint32_t addr, uint32_t v) {
    uint32_t a = addr & ~3u, w = MEM_R32(a);
    switch (addr & 3) {
    case 0: w = v; break;
    case 1: w = (w & 0x000000FFu) | (v << 8); break;
    case 2: w = (w & 0x0000FFFFu) | (v << 16); break;
    default: w = (w & 0x00FFFFFFu) | (v << 24); break;
    }
    MEM_W32(a, w);
}

static inline void psx_div(CPU *c, uint32_t n, uint32_t d) {
    int32_t sn = (int32_t)n, sd = (int32_t)d;
    if (sd == 0) { c->hi = n; c->lo = (sn >= 0) ? 0xFFFFFFFFu : 1u; }
    else if (n == 0x80000000u && sd == -1) { c->hi = 0; c->lo = 0x80000000u; }
    else { c->lo = (uint32_t)(sn / sd); c->hi = (uint32_t)(sn % sd); }
}
static inline void psx_divu(CPU *c, uint32_t n, uint32_t d) {
    if (d == 0) { c->hi = n; c->lo = 0xFFFFFFFFu; }
    else { c->lo = n / d; c->hi = n % d; }
}

/* control flow / system */
void call_addr(CPU *c, uint32_t addr);                         /* indirect call or tail jump */
void jumptable_miss(CPU *c, uint32_t site, uint32_t target);
void psx_syscall(CPU *c, uint32_t pc);
void psx_break(CPU *c, uint32_t pc, uint32_t code);
void psx_invalid(CPU *c, uint32_t pc, uint32_t word);
uint32_t cop0_read(CPU *c, int r);
void cop0_write(CPU *c, int r, uint32_t v);
void cop0_rfe(CPU *c);
void events_run(CPU *c);

extern int g_trace_calls;
void trace_func(CPU *c, uint32_t addr);

#define EVCHK() do { if (UNLIKELY(c->cycles >= c->next_event)) events_run(c); } while (0)
#define FUNC_ENTER(a) do { c->cur_func = (a); if (UNLIKELY(g_trace_calls)) trace_func(c, (a)); EVCHK(); } while (0)
#define RETURN_CHECK(a) ((void)0)
