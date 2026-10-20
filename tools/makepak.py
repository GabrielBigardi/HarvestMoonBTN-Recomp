#!/usr/bin/env python3
"""Build the game data pack (HarvestMoonBTN.pak) from the user's disc image.

The pack holds every sector the game can read (all files except the never-read DUMMY.DAT
filler), storing only each sector's subheader + payload.  Sync, header, EDC and ECC are
regenerated bit-exactly by the runtime (runtime/cdpak.c); a CRC32 of every original 16-sector
chunk is stored so the runtime verifies the reconstruction.

usage: makepak.py <disc.cue|disc.bin> <out.pak>
"""
import os, sys, struct, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso import Disc, walk, resolve_bin

MAGIC = b'HMBTNPAK'
VERSION = 1
SPC = 16   # sectors per chunk


def main():
    src, out = sys.argv[1], sys.argv[2]
    disc = Disc(resolve_bin(src))
    files = walk(disc)
    dummy = [e for e in files if e['path'].upper().startswith('/DUMMY.DAT')]
    stored = dummy[0]['lba'] if dummy else disc.nsect
    nchunks = (stored + SPC - 1) // SPC
    chunks, crcs = [], []
    for c in range(nchunks):
        raw = bytearray()
        rec = bytearray()
        for lba in range(c * SPC, min(stored, (c + 1) * SPC)):
            s = disc.raw(lba)
            raw += s
            form2 = bool(s[18] & 0x20)
            edc_zero = form2 and s[2348:2352] == b'\0\0\0\0'
            if s[15] != 2 or s[16:20] != s[20:24]:
                raise SystemExit('unexpected sector format at LBA %d' % lba)
            rec.append((1 if form2 else 0) | (2 if edc_zero else 0))
            rec += s[16:20]
            rec += s[24:24 + (2324 if form2 else 2048)]
        chunks.append(zlib.compress(bytes(rec), 9))
        crcs.append(zlib.crc32(bytes(raw)) & 0xFFFFFFFF)
    hdr = MAGIC + struct.pack('<6I', VERSION, disc.nsect, stored, SPC, nchunks, 0)
    table_size = (nchunks + 1) * 4 + nchunks * 4
    pos = len(hdr) + table_size
    offsets = []
    for ch in chunks:
        offsets.append(pos)
        pos += len(ch)
    offsets.append(pos)
    with open(out, 'wb') as f:
        f.write(hdr)
        f.write(struct.pack('<%dI' % (nchunks + 1), *offsets))
        f.write(struct.pack('<%dI' % nchunks, *crcs))
        for ch in chunks:
            f.write(ch)
    print('wrote %s: %d of %d sectors in %d chunks, %.1f MB' % (out, stored, disc.nsect, nchunks, pos / 1e6))


if __name__ == '__main__':
    main()
