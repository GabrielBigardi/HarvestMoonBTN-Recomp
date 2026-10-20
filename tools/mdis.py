"""Disassembler / xref helper.
usage:
  dis.py range <start> <end>            disassemble main exe (+overlay with -o idx@addr)
  dis.py xref <addr>                    find lui/addiu constant refs and jal refs to addr
  dis.py func <addr>                    disassemble from addr until jr ra + delay
"""
import struct, sys
from mips import decode, REG

ROOT = __file__.replace('\\', '/').rsplit('/', 2)[0]


def load_images(ovl=None):
    imgs = []
    e = open(ROOT + '/extract/SLUS_011.15', 'rb').read()
    imgs.append((0x80010000, e[0x800:]))
    if ovl is not None:
        idx, addr = ovl
        h = open(ROOT + '/extract/A_FILE.HDT', 'rb').read()
        v = struct.unpack('<33I', h)
        f = open(ROOT + '/extract/A_FILE.BIN', 'rb')
        f.seek(v[idx])
        imgs.append((addr, f.read(v[idx + 1] - v[idx])))
    return imgs


class Mem:
    def __init__(self, imgs):
        self.imgs = imgs

    def word(self, a):
        for base, d in self.imgs:
            if base <= a < base + len(d) - 3:
                return struct.unpack_from('<I', d, a - base)[0]
        return None

    def insns(self, start, end):
        a = start
        while a < end:
            w = self.word(a)
            if w is None:
                break
            yield decode(a, w)
            a += 4


def annotate(mem, start, end):
    """disassemble with simple lui tracking for constant annotation"""
    out = []
    hi = {}
    for i in mem.insns(start, end):
        s = str(i)
        note = ''
        if i.mn == 'lui':
            hi[i.rt] = i.imm << 16
        elif i.mn in ('addiu', 'ori') and i.rs in hi:
            v = (hi[i.rs] + (i.simm if i.mn == 'addiu' else i.imm)) & 0xFFFFFFFF
            note = '  ; =0x%08x' % v
        elif i.kind in ('load', 'store') and i.rs in hi:
            v = (hi[i.rs] + i.simm) & 0xFFFFFFFF
            note = '  ; [0x%08x]' % v
        d = i.dest()
        if d is not None and i.mn != 'lui':
            hi.pop(d, None)
        if i.kind in ('jumpr',) and i.rs == 31:
            hi = {}
        out.append('%08x: %08x  %-36s%s' % (i.addr, i.word, s, note))
    return out


def xrefs(mem, target, start, end):
    res = []
    hi = {}
    for i in mem.insns(start, end):
        if i.mn in ('jal', 'j') and i.target == target:
            res.append((i.addr, str(i)))
        if i.mn == 'lui':
            hi[i.rt] = (i.imm << 16, i.addr)
            continue
        if i.rs in hi and (i.mn in ('addiu', 'ori') or i.kind in ('load', 'store')):
            base = hi[i.rs][0]
            v = (base + (i.imm if i.mn == 'ori' else i.simm)) & 0xFFFFFFFF
            if v == target or (i.kind in ('load', 'store') and abs(v - target) < 0):
                res.append((i.addr, str(i)))
        d = i.dest()
        if d is not None:
            hi.pop(d, None)
    return res


if __name__ == '__main__':
    args = sys.argv[1:]
    ovl = None
    if '-o' in args:
        k = args.index('-o')
        idx, addr = args[k + 1].split('@')
        ovl = (int(idx), int(addr, 16))
        del args[k:k + 2]
    mem = Mem(load_images(ovl))
    cmd = args[0]
    if cmd == 'range':
        print('\n'.join(annotate(mem, int(args[1], 16), int(args[2], 16))))
    elif cmd == 'func':
        a = int(args[1], 16)
        e = a
        while True:
            i = decode(e, mem.word(e))
            e += 4
            if i.mn == 'jr' and i.rs == 31:
                e += 4
                break
        print('\n'.join(annotate(mem, a, e)))
    elif cmd == 'xref':
        t = int(args[1], 16)
        for base, d in mem.imgs:
            for a, s in xrefs(mem, t, base, base + len(d)):
                print('%08x: %s' % (a, s))
