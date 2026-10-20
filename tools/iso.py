"""Minimal PS1 Mode2/2352 disc image reader + ISO9660 walker."""
import os, struct, sys, json

SECTOR = 2352

class Disc:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.nsect = os.path.getsize(path) // SECTOR

    def raw(self, lba):
        self.f.seek(lba * SECTOR)
        return self.f.read(SECTOR)

    def user(self, lba):
        s = self.raw(lba)
        return s[24:24 + 2048]

    def submode(self, lba):
        s = self.raw(lba)
        return s[16], s[17], s[18], s[19]  # file, channel, submode, coding

    def read_extent(self, lba, size):
        out = bytearray()
        n = (size + 2047) // 2048
        for i in range(n):
            out += self.user(lba + i)
        return bytes(out[:size])

def parse_dir(disc, lba, size, path=''):
    entries = []
    data = disc.read_extent(lba, size)
    off = 0
    while off < len(data):
        ln = data[off]
        if ln == 0:
            off = (off // 2048 + 1) * 2048
            continue
        rec = data[off:off + ln]
        ext = struct.unpack_from('<I', rec, 2)[0]
        dsz = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        off += ln
        if name in (b'\x00', b'\x01'):
            continue
        name = name.decode('ascii', 'replace')
        full = path + '/' + name
        if flags & 2:
            entries.append({'path': full, 'lba': ext, 'size': dsz, 'dir': True})
            entries += parse_dir(disc, ext, dsz, full)
        else:
            entries.append({'path': full, 'lba': ext, 'size': dsz, 'dir': False})
    return entries

def resolve_bin(path):
    """a .cue sheet -> the path of its (first) .bin track; anything else is returned as is"""
    if path.lower().endswith('.cue'):
        d = os.path.dirname(os.path.abspath(path))
        for line in open(path, encoding='latin-1'):
            if line.strip().upper().startswith('FILE') and '"' in line:
                return os.path.join(d, line.split('"')[1])
        raise SystemExit('no FILE entry in ' + path)
    return path

def find_file(disc, name):
    """ISO9660 lookup by name (without ;1 version), case-insensitive"""
    for e in walk(disc):
        if not e['dir'] and e['path'].upper().lstrip('/').split(';')[0] == name.upper():
            return e
    return None

def walk(disc):
    pvd = disc.user(16)
    assert pvd[1:6] == b'CD001'
    root = pvd[156:156 + 34]
    rlba = struct.unpack_from('<I', root, 2)[0]
    rsz = struct.unpack_from('<I', root, 10)[0]
    return parse_dir(disc, rlba, rsz)

if __name__ == '__main__':
    d = Disc(sys.argv[1])
    ents = walk(d)
    outdir = sys.argv[2] if len(sys.argv) > 2 else None
    for e in ents:
        if e['dir']:
            continue
        n = (e['size'] + 2047) // 2048
        forms = set()
        for i in range(min(n, 64)):
            forms.add(2 if d.submode(e['lba'] + i)[2] & 0x20 else 1)
        e['form2'] = 2 in forms
        print(f"{e['path']:40s} lba={e['lba']:6d} size={e['size']:10d} {'FORM2' if e['form2'] else ''}")
        if outdir and not e['form2']:
            p = os.path.join(outdir, e['path'].lstrip('/').split(';')[0])
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(d.read_extent(e['lba'], e['size']))
    if outdir:
        json.dump(ents, open(os.path.join(outdir, '_files.json'), 'w'), indent=1)
