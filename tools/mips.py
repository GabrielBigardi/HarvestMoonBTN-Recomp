"""MIPS R3000A (PS1) instruction decoder / disassembler used by the recompiler."""

REG = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
       't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
       't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


class Insn:
    __slots__ = ('addr', 'word', 'op', 'rs', 'rt', 'rd', 'sa', 'imm', 'simm',
                 'target', 'mn', 'kind')

    def __init__(self, addr, word):
        self.addr = addr
        self.word = word
        op = word >> 26
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.imm = word & 0xFFFF
        self.simm = sx16(self.imm)
        self.target = None
        self.kind = 'alu'   # alu, load, store, branch, jump, jumpr, call, callr, sys, cop, invalid
        self.op = op
        self.mn = self._decode()

    def _decode(self):
        w = self.word
        op = self.op
        if op == 0:
            fn = w & 63
            m = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav',
                 8: 'jr', 9: 'jalr', 12: 'syscall', 13: 'break',
                 16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo',
                 24: 'mult', 25: 'multu', 26: 'div', 27: 'divu',
                 32: 'add', 33: 'addu', 34: 'sub', 35: 'subu', 36: 'and', 37: 'or',
                 38: 'xor', 39: 'nor', 42: 'slt', 43: 'sltu'}.get(fn)
            if m is None:
                self.kind = 'invalid'
                return 'invalid'
            if m == 'jr':
                self.kind = 'jumpr'
            elif m == 'jalr':
                self.kind = 'callr'
            elif m in ('syscall', 'break'):
                self.kind = 'sys'
            return m
        if op == 1:
            m = {0: 'bltz', 1: 'bgez', 16: 'bltzal', 17: 'bgezal'}.get(self.rt)
            if m is None:
                # R3000 decodes other values as bltz/bgez based on bit0 (and link if bits 4..1 == 8)
                m = 'bgez' if self.rt & 1 else 'bltz'
                if (self.rt & 0x1E) == 0x10:
                    m += 'al'
            self.kind = 'branch'
            self.target = (self.addr + 4 + (self.simm << 2)) & 0xFFFFFFFF
            return m
        if op in (2, 3):
            self.target = ((self.addr + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            self.kind = 'jump' if op == 2 else 'call'
            return 'j' if op == 2 else 'jal'
        if 4 <= op <= 7:
            self.kind = 'branch'
            self.target = (self.addr + 4 + (self.simm << 2)) & 0xFFFFFFFF
            return ['beq', 'bne', 'blez', 'bgtz'][op - 4]
        m = {8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu', 12: 'andi', 13: 'ori',
             14: 'xori', 15: 'lui',
             32: 'lb', 33: 'lh', 34: 'lwl', 35: 'lw', 36: 'lbu', 37: 'lhu', 38: 'lwr',
             40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw', 46: 'swr',
             50: 'lwc2', 58: 'swc2'}.get(op)
        if m:
            if op >= 32:
                self.kind = 'load' if m.startswith('l') else 'store'
            return m
        if op in (16, 17, 18, 19):
            self.kind = 'cop'
            cop = op - 16
            if self.rs & 0x10:
                if cop == 0:
                    fn = w & 63
                    if fn == 0x10:
                        return 'rfe'
                    return 'cop0_%x' % fn
                if cop == 2:
                    return 'gte_cmd'
                self.kind = 'invalid'
                return 'invalid'
            sub = {0: 'mfc', 2: 'cfc', 4: 'mtc', 6: 'ctc'}.get(self.rs)
            if sub is None:
                if self.rs == 8:  # BCzF/BCzT - never used on PSX
                    self.kind = 'invalid'
                    return 'invalid'
                self.kind = 'invalid'
                return 'invalid'
            if cop not in (0, 2):
                self.kind = 'invalid'
                return 'invalid'
            return '%s%d' % (sub, cop)
        self.kind = 'invalid'
        return 'invalid'

    @property
    def has_delay(self):
        return self.kind in ('branch', 'jump', 'jumpr', 'call', 'callr')

    def dest(self):
        """GPR written by this instruction (excluding link writes), or None."""
        m = self.mn
        if self.kind in ('alu',):
            if m in ('mult', 'multu', 'div', 'divu', 'mthi', 'mtlo'):
                return None
            if self.op == 0:
                return self.rd or None
            return self.rt or None
        if self.kind == 'load' and m not in ('lwc2',):
            return self.rt or None
        if m in ('mfc0', 'mfc2', 'cfc2'):
            return self.rt or None
        if m == 'jalr':
            return self.rd or None
        return None

    def __str__(self):
        return disasm(self)


def disasm(i):
    m = i.mn
    r = REG
    if m == 'invalid':
        return '.word 0x%08x' % i.word
    if i.word == 0:
        return 'nop'
    if m in ('sll', 'srl', 'sra'):
        return '%s %s, %s, %d' % (m, r[i.rd], r[i.rt], i.sa)
    if m in ('sllv', 'srlv', 'srav'):
        return '%s %s, %s, %s' % (m, r[i.rd], r[i.rt], r[i.rs])
    if m == 'jr':
        return 'jr %s' % r[i.rs]
    if m == 'jalr':
        return 'jalr %s, %s' % (r[i.rd], r[i.rs])
    if m in ('syscall', 'break'):
        return '%s 0x%x' % (m, (i.word >> 6) & 0xFFFFF)
    if m in ('mfhi', 'mflo'):
        return '%s %s' % (m, r[i.rd])
    if m in ('mthi', 'mtlo'):
        return '%s %s' % (m, r[i.rs])
    if m in ('mult', 'multu', 'div', 'divu'):
        return '%s %s, %s' % (m, r[i.rs], r[i.rt])
    if i.op == 0:
        return '%s %s, %s, %s' % (m, r[i.rd], r[i.rs], r[i.rt])
    if m in ('bltz', 'bgez', 'bltzal', 'bgezal', 'blez', 'bgtz'):
        return '%s %s, 0x%08x' % (m, r[i.rs], i.target)
    if m in ('beq', 'bne'):
        return '%s %s, %s, 0x%08x' % (m, r[i.rs], r[i.rt], i.target)
    if m in ('j', 'jal'):
        return '%s 0x%08x' % (m, i.target)
    if m == 'lui':
        return 'lui %s, 0x%x' % (r[i.rt], i.imm)
    if m in ('andi', 'ori', 'xori'):
        return '%s %s, %s, 0x%x' % (m, r[i.rt], r[i.rs], i.imm)
    if m in ('addi', 'addiu', 'slti', 'sltiu'):
        return '%s %s, %s, %d' % (m, r[i.rt], r[i.rs], i.simm)
    if i.kind in ('load', 'store'):
        if m in ('lwc2', 'swc2'):
            return '%s $%d, %d(%s)' % (m, i.rt, i.simm, r[i.rs])
        return '%s %s, %d(%s)' % (m, r[i.rt], i.simm, r[i.rs])
    if m in ('mfc0', 'mtc0', 'mfc2', 'mtc2', 'cfc2', 'ctc2'):
        return '%s %s, $%d' % (m, r[i.rt], i.rd)
    if m == 'gte_cmd':
        return 'gte 0x%07x' % (i.word & 0x1FFFFFF)
    return m


def decode(addr, word):
    return Insn(addr, word)
