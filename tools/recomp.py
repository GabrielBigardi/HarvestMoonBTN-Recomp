#!/usr/bin/env python3
"""Static recompiler: PS1 (MIPS R3000A) machine code -> C.

Every MIPS function becomes a C function `void f_<addr>(CPU *c)` (overlay functions are
prefixed with their module name).  Registers live in the CPU struct, memory accesses go
through the runtime's memory helpers, branches become gotos, jump tables become switch
statements and indirect calls go through the runtime dispatcher (call_addr).
"""
import struct, sys, os, json, re
from collections import defaultdict
from mips import decode, REG

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ----------------------------------------------------------------------------------------
# modules
# ----------------------------------------------------------------------------------------
class Module:
    def __init__(self, name, base, data, code_lo, code_hi, ovl_id=None, file_index=None):
        self.name = name
        self.base = base
        self.data = data
        self.end = base + len(data)
        self.code_lo = code_lo
        self.code_hi = code_hi
        self.ovl_id = ovl_id
        self.file_index = file_index
        self._cache = {}
        self.strong = set()
        self.weak = set()
        self.funcs = {}          # start -> FuncInfo
        self.jtables = {}        # jr address -> list of targets
        self.notes = []

    def in_code(self, a):
        return self.code_lo <= a < self.code_hi and (a & 3) == 0

    def word(self, a):
        return struct.unpack_from('<I', self.data, a - self.base)[0]

    def insn(self, a):
        i = self._cache.get(a)
        if i is None:
            i = decode(a, self.word(a))
            self._cache[a] = i
        return i

    @property
    def prefix(self):
        return 'f' if self.ovl_id is None else 'o%s' % self.name


class FuncInfo:
    def __init__(self, start):
        self.start = start
        self.body = set()        # instruction addresses executed as normal instructions
        self.delay = set()       # addresses emitted as delay slots
        self.labels = set()
        self.calls = set()
        self.tails = set()
        self.dyn_jr = []


def load_modules():
    exe = open(os.path.join(ROOT, 'extract', 'SLUS_011.15'), 'rb').read()
    mods = [Module('main', 0x80010000, exe[0x800:], 0x80011270, 0x80048B0C)]
    h = struct.unpack('<33I', open(os.path.join(ROOT, 'extract', 'A_FILE.HDT'), 'rb').read())
    f = open(os.path.join(ROOT, 'extract', 'A_FILE.BIN'), 'rb')
    ovl = json.load(open(os.path.join(ROOT, 'tools', 'overlays.json')))
    for o in ovl:
        idx = o['file']
        f.seek(h[idx])
        d = f.read(h[idx + 1] - h[idx])
        base = int(o['base'], 16)
        lo = int(o['code_lo'], 16)
        hi = int(o['code_hi'], 16)
        m = Module(o['name'], base, d, lo, hi, ovl_id=struct.unpack_from('<I', d, 0)[0], file_index=idx)
        mods.append(m)
    return mods


# ----------------------------------------------------------------------------------------
# analysis
# ----------------------------------------------------------------------------------------
def scan_constants(mod, addrs):
    """lui/addiu|ori constant pairs inside the given instruction addresses (straight-line tracking)."""
    out = set()
    hi = {}
    for a in sorted(addrs):
        i = mod.insn(a)
        if i.mn == 'lui':
            hi[i.rt] = (i.imm << 16, a)
            continue
        if i.mn in ('addiu', 'ori') and i.rs in hi and a - hi[i.rs][1] < 64:
            v = (hi[i.rs][0] + (i.imm if i.mn == 'ori' else i.simm)) & 0xFFFFFFFF
            out.add(v)
        d = i.dest()
        if d is not None:
            hi.pop(d, None)
    return out


def resolve_jump_table(mod, jr_addr, body_hint):
    """Recognise gcc jump-table dispatch ending at `jr rX`. Returns list of targets or None."""
    jr = mod.insn(jr_addr)
    X = jr.rs
    seq = []
    a = jr_addr - 4
    while a >= mod.code_lo and len(seq) < 24:
        seq.append(mod.insn(a))
        a -= 4
    # find lw X, off(B)
    lw = None
    k = 0
    for k, i in enumerate(seq):
        if i.dest() == X:
            if i.mn == 'lw':
                lw = i
            break
    if lw is None:
        return None
    B = lw.rs
    off = lw.simm
    base = None
    rest = seq[k + 1:]

    def const_of(reg, start):
        """value of reg if it is a lui(/addiu) constant, scanning rest[start:] backwards in time."""
        for n in range(start, len(rest)):
            i = rest[n]
            if i.dest() == reg:
                if i.mn == 'lui':
                    return i.imm << 16
                if i.mn == 'addiu':
                    hv = const_of(i.rs, n + 1)
                    return None if hv is None else (hv + i.simm) & 0xFFFFFFFF
                return None
        return None

    defB = None
    j = 0
    for j, i in enumerate(rest):
        if i.dest() == B:
            defB = i
            break
    if defB is None:
        return None
    if defB.mn == 'addu':
        for P in (defB.rs, defB.rt):
            cv = const_of(P, j + 1)
            if cv is not None:
                base = cv
                break
    elif defB.mn in ('lui', 'addiu'):
        base = const_of(B, j)
    if base is None:
        return None
    table = (base + off) & 0xFFFFFFFF
    # table size from sltiu on the index
    count = None
    for i in rest:
        if i.mn == 'sltiu' and i.simm > 0:
            count = i.simm
            break
    # locate table data (may be in this module or main)
    targets = []
    n = 0
    while True:
        if count is not None and n >= count:
            break
        if not (mod.base <= table + n * 4 < mod.end):
            break
        t = mod.word(table + n * 4)
        if not mod.in_code(t):
            break
        if count is None and n > 512:
            break
        targets.append(t)
        n += 1
    if not targets:
        return None
    return targets


def trace_function(mod, start, strong, all_strong_global):
    fi = FuncInfo(start)
    work = [start]
    seen = set()

    def is_other_start(t):
        return t != start and t in strong

    while work:
        a = work.pop()
        if a in seen:
            continue
        if not mod.in_code(a):
            fi.notes = getattr(fi, 'notes', []) + ['flow leaves code at %08x' % a]
            continue
        seen.add(a)
        fi.body.add(a)
        i = mod.insn(a)
        k = i.kind
        if i.has_delay:
            fi.delay.add(a + 4)
        if k == 'branch':
            t = i.target
            if mod.in_code(t) and not is_other_start(t):
                work.append(t)
                fi.labels.add(t)
            else:
                fi.tails.add(t)
            if i.mn.endswith('al'):
                fi.calls.add(t)
            nxt = a + 8
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
        elif k == 'jump':
            t = i.target
            if mod.in_code(t) and not is_other_start(t) and t not in all_strong_global:
                work.append(t)
                fi.labels.add(t)
            else:
                fi.tails.add(t)
        elif k == 'call':
            fi.calls.add(i.target)
            nxt = a + 8
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
        elif k == 'callr':
            nxt = a + 8
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
        elif k == 'jumpr':
            if i.rs == 31:
                pass
            else:
                tg = mod.jtables.get(a)
                if tg is None:
                    tg = resolve_jump_table(mod, a, None)
                    if tg is not None:
                        mod.jtables[a] = tg
                if tg:
                    for t in tg:
                        work.append(t)
                        fi.labels.add(t)
                else:
                    fi.dyn_jr.append(a)
        elif k == 'invalid':
            pass  # emitted as a trap; flow stops
        else:
            nxt = a + 4
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
    return fi


def analyse(mods, extra):
    main = mods[0]
    # initial strong entries
    main.strong.add(0x80011278)
    for name, addrs in extra.items():
        for m in mods:
            if m.name == name:
                m.strong.update(int(x, 16) for x in addrs)

    def mod_for(m, t):
        if m.in_code(t):
            return m
        if main.in_code(t):
            return main
        return None

    # sweep starts: code following `jr ra` + delay slot (and nop padding)
    for m in mods:
        a = m.code_lo
        while a < m.code_hi - 8:
            if m.word(a) == 0x03E00008:
                b = a + 8
                while b < m.code_hi and m.word(b) == 0:
                    b += 4
                if b < m.code_hi and m.insn(b).kind != 'invalid':
                    m.weak.add(b)
            a += 4
        m.weak.add(m.code_lo)

    def promote(m, fi):
        new = 0
        for t in fi.calls:
            tm = mod_for(m, t)
            if tm is not None and t not in tm.strong:
                tm.strong.add(t)   # a jal target is always a function start
                new += 1
        for t in scan_constants(m, fi.body | fi.delay):
            own = mod_for(m, t)
            tms = [own] if own is not None else [o for o in mods if o is not m and o.in_code(t)]
            for tm in tms:
                ok = (tm.in_code(t) and tm.insn(t).kind != 'invalid') if tm is m else plausible_start(tm, t, strict=True)
                if t not in tm.strong and ok:
                    tm.strong.add(t)
                    new += 1
        return new

    rounds = 0
    while True:
        rounds += 1
        new = 0
        for m in mods:
            m.funcs = {}
            m.jtables = {}
            for s in sorted(m.strong | m.weak):
                m.funcs[s] = trace_function(m, s, m.strong, m.strong)
        for m in mods:
            for fi in list(m.funcs.values()):
                new += promote(m, fi)
        if new == 0 or rounds > 12:
            break
    # data words pointing into code (function pointer tables, callbacks) -> extra weak starts
    jt = defaultdict(set)
    for m in mods:
        for tg in m.jtables.values():
            jt[m.name].update(tg)
    extra_weak = defaultdict(set)
    for m in mods:
        for off in range(0, len(m.data) - 3, 4):
            a = m.base + off
            if m.in_code(a):
                continue
            v = m.word(a)
            for tm in ((m, main) if m is not main else (main,)):
                if tm.in_code(v) and v not in tm.funcs and v not in jt[tm.name] and plausible_start(tm, v):
                    extra_weak[tm.name].add(v)
    for m in mods:
        for w in sorted(extra_weak[m.name]):
            m.weak.add(w)
            m.funcs[w] = trace_function(m, w, m.strong, m.strong)
        m.weak -= m.strong
    return rounds


def plausible_start(m, t, strict=False):
    if not m.in_code(t):
        return False
    i = m.insn(t)
    if i.kind == 'invalid':
        return False
    if strict:
        return (i.mn == 'addiu' and i.rt == 29 and i.rs == 29 and i.simm < 0) or t == m.code_lo or             (m.base <= t - 8 and m.word(t - 8) == 0x03E00008)
    # previous instructions: jr ra / delay, or nop padding, or start of code
    p2 = t - 8
    if t == m.code_lo or t == m.base + 4:
        return True
    if m.base <= p2 and m.word(p2) == 0x03E00008:
        return True
    if m.base <= t - 4 and m.word(t - 4) == 0 and (i.mn == 'addiu' and i.rt == 29):
        return True
    if i.mn == 'addiu' and i.rt == 29 and i.rs == 29 and i.simm < 0:
        return True
    # j at end of previous function (tail jump) followed by delay
    if m.base <= p2:
        pi = m.insn(p2)
        if pi.kind in ('jump',) or (pi.kind == 'jumpr'):
            return True
    return False


# ----------------------------------------------------------------------------------------
# C emission
# ----------------------------------------------------------------------------------------
def R(n):
    return '0' if n == 0 else 'R[%d]' % n


def addr_expr(i):
    if i.rs == 0:
        return '0x%08Xu' % (i.simm & 0xFFFFFFFF)
    if i.simm == 0:
        return 'R[%d]' % i.rs
    return '(R[%d] + %du)' % (i.rs, i.simm & 0xFFFFFFFF) if i.simm > 0 else '(R[%d] - %du)' % (i.rs, -i.simm)


def emit_simple(i, mod):
    """C for a non-control-flow instruction (string, may be empty)."""
    m = i.mn
    rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
    if i.word == 0:
        return ''

    def W(r, expr):
        if r == 0:
            return ''
        return 'R[%d] = %s;' % (r, expr)
    if m == 'sll':
        return W(rd, '%s << %d' % (R(rt), sa)) if sa else W(rd, R(rt))
    if m == 'srl':
        return W(rd, '%s >> %d' % (R(rt), sa)) if sa else W(rd, R(rt))
    if m == 'sra':
        return W(rd, '(uint32_t)((int32_t)%s >> %d)' % (R(rt), sa))
    if m == 'sllv':
        return W(rd, '%s << (%s & 31)' % (R(rt), R(rs)))
    if m == 'srlv':
        return W(rd, '%s >> (%s & 31)' % (R(rt), R(rs)))
    if m == 'srav':
        return W(rd, '(uint32_t)((int32_t)%s >> (%s & 31))' % (R(rt), R(rs)))
    if m == 'mfhi':
        return W(rd, 'c->hi')
    if m == 'mflo':
        return W(rd, 'c->lo')
    if m == 'mthi':
        return 'c->hi = %s;' % R(rs)
    if m == 'mtlo':
        return 'c->lo = %s;' % R(rs)
    if m == 'mult':
        return '{ int64_t m_ = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s; c->lo = (uint32_t)m_; c->hi = (uint32_t)((uint64_t)m_ >> 32); }' % (R(rs), R(rt))
    if m == 'multu':
        return '{ uint64_t m_ = (uint64_t)%s * (uint64_t)%s; c->lo = (uint32_t)m_; c->hi = (uint32_t)(m_ >> 32); }' % (R(rs), R(rt))
    if m == 'div':
        return 'psx_div(c, %s, %s);' % (R(rs), R(rt))
    if m == 'divu':
        return 'psx_divu(c, %s, %s);' % (R(rs), R(rt))
    if m in ('add', 'addu'):
        if rt == 0:
            return W(rd, R(rs))
        if rs == 0:
            return W(rd, R(rt))
        return W(rd, '%s + %s' % (R(rs), R(rt)))
    if m in ('sub', 'subu'):
        return W(rd, '%s - %s' % (R(rs), R(rt)))
    if m == 'and':
        return W(rd, '%s & %s' % (R(rs), R(rt)))
    if m == 'or':
        if rt == 0:
            return W(rd, R(rs))
        if rs == 0:
            return W(rd, R(rt))
        return W(rd, '%s | %s' % (R(rs), R(rt)))
    if m == 'xor':
        return W(rd, '%s ^ %s' % (R(rs), R(rt)))
    if m == 'nor':
        return W(rd, '~(%s | %s)' % (R(rs), R(rt)))
    if m == 'slt':
        return W(rd, '(int32_t)%s < (int32_t)%s' % (R(rs), R(rt)))
    if m == 'sltu':
        return W(rd, '%s < %s' % (R(rs), R(rt)))
    if m in ('addi', 'addiu'):
        if rs == 0:
            return W(rt, '0x%Xu' % (i.simm & 0xFFFFFFFF))
        if i.simm == 0:
            return W(rt, R(rs))
        if i.simm > 0:
            return W(rt, '%s + %du' % (R(rs), i.simm))
        return W(rt, '%s - %du' % (R(rs), -i.simm))
    if m == 'slti':
        return W(rt, '(int32_t)%s < %d' % (R(rs), i.simm))
    if m == 'sltiu':
        return W(rt, '%s < 0x%Xu' % (R(rs), i.simm & 0xFFFFFFFF))
    if m == 'andi':
        return W(rt, '%s & 0x%Xu' % (R(rs), i.imm))
    if m == 'ori':
        return W(rt, '%s | 0x%Xu' % (R(rs), i.imm)) if rs else W(rt, '0x%Xu' % i.imm)
    if m == 'xori':
        return W(rt, '%s ^ 0x%Xu' % (R(rs), i.imm))
    if m == 'lui':
        return W(rt, '0x%08Xu' % (i.imm << 16))
    ae = addr_expr(i)
    if m == 'lb':
        return W(rt, '(uint32_t)(int32_t)(int8_t)MEM_R8(%s)' % ae) or 'MEM_R8(%s);' % ae
    if m == 'lbu':
        return W(rt, 'MEM_R8(%s)' % ae) or 'MEM_R8(%s);' % ae
    if m == 'lh':
        return W(rt, '(uint32_t)(int32_t)(int16_t)MEM_R16(%s)' % ae) or 'MEM_R16(%s);' % ae
    if m == 'lhu':
        return W(rt, 'MEM_R16(%s)' % ae) or 'MEM_R16(%s);' % ae
    if m == 'lw':
        return W(rt, 'MEM_R32(%s)' % ae) or 'MEM_R32(%s);' % ae
    if m == 'lwl':
        return W(rt, 'psx_lwl(%s, %s)' % (ae, R(rt)))
    if m == 'lwr':
        return W(rt, 'psx_lwr(%s, %s)' % (ae, R(rt)))
    if m == 'sb':
        return 'MEM_W8(%s, %s);' % (ae, R(rt))
    if m == 'sh':
        return 'MEM_W16(%s, %s);' % (ae, R(rt))
    if m == 'sw':
        return 'MEM_W32(%s, %s);' % (ae, R(rt))
    if m == 'swl':
        return 'psx_swl(%s, %s);' % (ae, R(rt))
    if m == 'swr':
        return 'psx_swr(%s, %s);' % (ae, R(rt))
    if m == 'lwc2':
        return 'gte_write_data(%d, MEM_R32(%s));' % (rt, ae)
    if m == 'swc2':
        return 'MEM_W32(%s, gte_read_data(%d));' % (ae, rt)
    if m == 'mfc0':
        return W(rt, 'cop0_read(c, %d)' % rd) or 'cop0_read(c, %d);' % rd
    if m == 'mtc0':
        return 'cop0_write(c, %d, %s);' % (rd, R(rt))
    if m == 'rfe':
        return 'cop0_rfe(c);'
    if m == 'mfc2':
        return W(rt, 'gte_read_data(%d)' % rd) or 'gte_read_data(%d);' % rd
    if m == 'cfc2':
        return W(rt, 'gte_read_ctrl(%d)' % rd) or 'gte_read_ctrl(%d);' % rd
    if m == 'mtc2':
        return 'gte_write_data(%d, %s);' % (rd, R(rt))
    if m == 'ctc2':
        return 'gte_write_ctrl(%d, %s);' % (rd, R(rt))
    if m == 'gte_cmd':
        return 'gte_command(0x%07Xu);' % (i.word & 0x1FFFFFF)
    if m == 'syscall':
        return 'psx_syscall(c, 0x%08Xu);' % i.addr
    if m == 'break':
        return 'psx_break(c, 0x%08Xu, %du);' % (i.addr, (i.word >> 6) & 0xFFFFF)
    if m.startswith('cop0_'):
        return '/* %s */' % m
    if m == 'invalid':
        return 'psx_invalid(c, 0x%08Xu, 0x%08Xu); return;' % (i.addr, i.word)
    raise ValueError('unhandled %s at %08x' % (m, i.addr))


GTE_CYCLES = {0x01: 15, 0x06: 8, 0x0C: 6, 0x10: 8, 0x11: 8, 0x12: 8, 0x13: 19, 0x14: 13, 0x16: 44, 0x1B: 17,
              0x1C: 11, 0x1E: 14, 0x20: 30, 0x28: 5, 0x29: 8, 0x2A: 17, 0x2D: 5, 0x2E: 6, 0x30: 23, 0x3D: 5,
              0x3E: 5, 0x3F: 39}


def insn_cycles(i):
    """Approximate R3000A cost: 1 cycle per instruction, RAM loads stall ~4 cycles,
    multiply/divide results are read right away by compiled code (full latency)."""
    if i.kind == 'load':
        return 5
    m = i.mn
    if m in ('mult', 'multu'):
        return 7
    if m in ('div', 'divu'):
        return 36
    if m == 'gte_cmd':
        return 1 + GTE_CYCLES.get(i.word & 0x3F, 8)
    return 1


class Emitter:
    def __init__(self, mods):
        self.mods = mods
        self.main = mods[0]

    def fname(self, mod, a):
        return '%s_%08X' % (mod.prefix, a)

    def call_expr(self, mod, t):
        """C statement calling address t (direct when statically known)."""
        if t in (0xA0, 0xB0, 0xC0):
            return 'call_addr(c, 0x%Xu);' % t
        if mod.in_code(t) and t in mod.funcs:
            return '%s(c);' % self.fname(mod, t)
        if mod is not self.main and self.main.in_code(t) and t in self.main.funcs:
            return '%s(c);' % self.fname(self.main, t)
        return 'call_addr(c, 0x%08Xu);' % t

    def emit_function(self, mod, fi):
        out = []
        P = out.append
        name = self.fname(mod, fi.start)
        P('void %s(CPU *c) {' % name)
        P('  FUNC_ENTER(0x%08Xu);' % fi.start)
        order = sorted(fi.body)
        if order[0] != fi.start:
            P('  goto L_%08X;' % fi.start)
            fi.labels.add(fi.start)
        labels = set(fi.labels)
        # any address that is a fallthrough target of a non-contiguous emission gets a label
        emitted_next = {}
        for k, a in enumerate(order):
            emitted_next[a] = order[k + 1] if k + 1 < len(order) else None
        # first pass to know which addresses need labels (gotos for discontinuities)
        need = set(labels)
        for a in order:
            i = mod.insn(a)
            if i.has_delay:
                ft = a + 8
            else:
                ft = a + 4
            if i.kind in ('jump', 'jumpr') or i.kind == 'invalid':
                continue
            if ft in fi.body and emitted_next.get(a) != ft:
                need.add(ft)
            if i.has_delay and (a + 4) in fi.body and emitted_next.get(a) == a + 4:
                # standalone copy of the delay slot is emitted next; fall through must skip it
                need.add(ft)
        cyc = 0

        def flush():
            nonlocal cyc
            if cyc:
                P('  c->cycles += %d;' % cyc)
                cyc = 0

        for a in order:
            i = mod.insn(a)
            if a in need:
                flush()
                P('L_%08X:' % a)
            body_c = None
            if not i.has_delay:
                cyc += insn_cycles(i)
                if i.mn == 'syscall':
                    flush()
                s = emit_simple(i, mod)
                if s:
                    P('  ' + s + '  /* %08x: %s */' % (a, i))
                ft = a + 4
                if i.kind == 'invalid':
                    continue
                if ft not in fi.body or emitted_next.get(a) != ft:
                    flush()
                    if ft in fi.body:
                        P('  goto L_%08X;' % ft)
                    else:
                        P('  ' + self.call_expr(mod, ft) + ' return;  /* fallthrough */')
                continue
            # control transfer with delay slot
            d = mod.insn(a + 4) if mod.in_code(a + 4) else None
            cyc += 1 + (insn_cycles(d) if d is not None else 1)
            ds = emit_simple(d, mod) if d is not None and not d.has_delay else ''
            if d is not None and d.has_delay:
                ds = '/* branch in delay slot ignored */'
            dcomment = '  /* %08x: %s */' % (a + 4, d) if d is not None else ''
            P('  /* %08x: %s */' % (a, i))
            k = i.kind
            m = i.mn
            ft = a + 8
            flush()
            if k == 'branch':
                cond = {
                    'beq': '%s == %s' % (R(i.rs), R(i.rt)),
                    'bne': '%s != %s' % (R(i.rs), R(i.rt)),
                    'blez': '(int32_t)%s <= 0' % R(i.rs),
                    'bgtz': '(int32_t)%s > 0' % R(i.rs),
                    'bltz': '(int32_t)%s < 0' % R(i.rs),
                    'bgez': '(int32_t)%s >= 0' % R(i.rs),
                    'bltzal': '(int32_t)%s < 0' % R(i.rs),
                    'bgezal': '(int32_t)%s >= 0' % R(i.rs),
                }[m]
                always = (m == 'beq' and i.rs == i.rt) or (m == 'bgez' and i.rs == 0)
                link = m.endswith('al')
                t = i.target
                P('  {')
                if not always:
                    P('    int br_ = %s;' % cond)
                if link:
                    P('    R[31] = 0x%08Xu;' % (a + 8))
                if ds:
                    P('    ' + ds + dcomment)
                P('')
                guard = '' if always else 'if (br_) '
                if link:
                    P('    %s{ %s }' % (guard, self.call_expr(mod, t)))
                elif t in fi.body and t in fi.labels:
                    back = t <= a
                    if back:
                        P('    %s{ EVCHK(); goto L_%08X; }' % (guard, t))
                    else:
                        P('    %sgoto L_%08X;' % (guard, t))
                else:
                    P('    %s{ %s return; }' % (guard, self.call_expr(mod, t)))
                P('  }')
                if always and not link:
                    continue
            elif k == 'jump':
                t = i.target
                if ds:
                    P('  ' + ds + dcomment)
                if t in fi.body and t in fi.labels:
                    if t <= a:
                        P('  EVCHK(); goto L_%08X;' % t)
                    else:
                        P('  goto L_%08X;' % t)
                else:
                    P('  %s return;' % self.call_expr(mod, t))
                continue
            elif k == 'call':
                t = i.target
                P('  R[31] = 0x%08Xu;' % (a + 8))
                if ds:
                    P('  ' + ds + dcomment)
                P('  ' + self.call_expr(mod, t))
            elif k == 'callr':
                P('  { uint32_t t_ = %s;' % R(i.rs))
                if i.rd:
                    P('    R[%d] = 0x%08Xu;' % (i.rd, a + 8))
                if ds:
                    P('    ' + ds + dcomment)
                P('')
                P('    call_addr(c, t_); }')
            elif k == 'jumpr':
                if i.rs == 31:
                    if ds:
                        P('  ' + ds + dcomment)
                    P('  RETURN_CHECK(0x%08Xu); return;' % a)
                    continue
                P('  { uint32_t t_ = %s;' % R(i.rs))
                if ds:
                    P('    ' + ds + dcomment)
                P('')
                tg = mod.jtables.get(a)
                if tg:
                    P('    switch (t_) {')
                    for t in sorted(set(tg)):
                        P('      case 0x%08Xu: goto L_%08X;' % (t, t))
                    P('      default: jumptable_miss(c, 0x%08Xu, t_); return;' % a)
                    P('    }')
                else:
                    P('    call_addr(c, t_); return;')
                P('  }')
                continue
            # fallthrough after conditional branch / call
            if ft in fi.body:
                if emitted_next.get(a) != ft or (a + 4) in fi.body:
                    P('  goto L_%08X;' % ft)
            else:
                P('  ' + self.call_expr(mod, ft) + ' return;  /* fallthrough */')
        P('}')
        return '\n'.join(out)


HEADER = '''/* generated by tools/recomp.py - do not edit */
#include "psx_recomp.h"
#define R (c->r)
'''


def generate(mods, outdir, per_file=60):
    em = Emitter(mods)
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.endswith('.c') or f.endswith('.h'):
            os.remove(os.path.join(outdir, f))
    decls = []
    tables = []
    files = []
    for m in mods:
        starts = sorted(m.funcs)
        for s in starts:
            decls.append('void %s(CPU *c);' % em.fname(m, s))
        chunk = []
        n = 0
        for k, s in enumerate(starts):
            chunk.append(em.emit_function(m, m.funcs[s]))
            if len(chunk) >= per_file or k == len(starts) - 1:
                fn = '%s_%03d.c' % (m.name, n)
                with open(os.path.join(outdir, fn), 'w') as fh:
                    fh.write(HEADER + '#include "funcs.h"\n\n' + '\n\n'.join(chunk) + '\n')
                files.append(fn)
                chunk = []
                n += 1
    with open(os.path.join(outdir, 'funcs.h'), 'w') as fh:
        fh.write('#pragma once\n' + '\n'.join(decls) + '\n')
    # module tables
    t = [HEADER, '#include "funcs.h"\n']
    for m in mods:
        t.append('static const FuncEntry tab_%s[] = {' % m.name)
        for s in sorted(m.funcs):
            t.append('  {0x%08Xu, %s, 0x%08Xu, 0x%08Xu},' % (s, em.fname(m, s), m.word(s), m.word(s + 4)))
        t.append('};')
    t.append('const ModuleDesc g_modules[] = {')
    for m in mods:
        ovl = 0xFFFFFFFF if m.ovl_id is None else m.ovl_id
        t.append('  {"%s", 0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu, %d, tab_%s, %d},' % (
            m.name, m.base, m.end, m.code_lo, m.code_hi, ovl,
            -1 if m.file_index is None else m.file_index, m.name, len(m.funcs)))
    t.append('};')
    t.append('const int g_num_modules = %d;' % len(mods))
    with open(os.path.join(outdir, 'modules.c'), 'w') as fh:
        fh.write('\n'.join(t) + '\n')
    files.append('modules.c')
    return files


def main():
    mods = load_modules()
    extra = {}
    xp = os.path.join(ROOT, 'tools', 'extra_entries.json')
    if os.path.exists(xp):
        extra = json.load(open(xp))
    rounds = analyse(mods, extra)
    report = []
    for m in mods:
        nb = sum(len(f.body) for f in m.funcs.values())
        dyn = sum(len(f.dyn_jr) for f in m.funcs.values())
        report.append('%-9s funcs=%5d strong=%5d weak=%4d insns=%7d jtables=%4d dyn_jr=%d' % (
            m.name, len(m.funcs), len(m.strong), len(m.weak), nb, len(m.jtables), dyn))
    print('analysis rounds:', rounds)
    print('\n'.join(report))
    # dynamic jr listing for review
    with open(os.path.join(ROOT, 'build', 'dyn_jr.txt'), 'w') as fh:
        for m in mods:
            for f in sorted(m.funcs.values(), key=lambda f: f.start):
                for a in f.dyn_jr:
                    fh.write('%s %08x in %08x: %s\n' % (m.name, a, f.start, m.insn(a)))
    files = generate(mods, os.path.join(ROOT, 'generated'))
    print('generated %d files' % len(files))


if __name__ == '__main__':
    main()


def coverage_report(mods):
    for m in mods:
        cov = set()
        for f in m.funcs.values():
            cov |= f.body
            cov |= f.delay
        gaps = []
        a = m.code_lo
        start = None
        while a < m.code_hi:
            if a not in cov and m.word(a) != 0:
                if start is None:
                    start = a
            else:
                if start is not None:
                    gaps.append((start, a))
                    start = None
            a += 4
        if start is not None:
            gaps.append((start, m.code_hi))
        tot = sum((e - s) // 4 for s, e in gaps)
        print('%-9s uncovered nonzero words: %d in %d gaps' % (m.name, tot, len(gaps)))
        for s, e in gaps[:400]:
            if (e - s) >= 8:
                print('    %08x-%08x (%d)' % (s, e, (e - s) // 4))
