"""Make a PNG contact sheet from BMP screenshots: sheet.py out.png frame1.bmp frame2.bmp ..."""
import sys, struct, zlib

def read_bmp(p):
    d = open(p, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    row = (w * 3 + 3) & ~3
    px = []
    for y in range(h):
        r = d[off + (h - 1 - y) * row: off + (h - 1 - y) * row + w * 3]
        px.append(bytes(b for i in range(w) for b in (r[i*3+2], r[i*3+1], r[i*3])))
    return w, h, px

def write_png(p, w, h, rows):
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(t, data):
        c = struct.pack('>I', len(data)) + t + data
        return c + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(p, 'wb').write(png)

if __name__ == '__main__':
    out = sys.argv[1]
    files = sys.argv[2:]
    imgs = [read_bmp(f) for f in files]
    cols = 4 if len(imgs) > 4 else len(imgs)
    cw = max(i[0] for i in imgs); ch = max(i[1] for i in imgs)
    nrows = (len(imgs) + cols - 1) // cols
    W, H = cols * (cw + 4), nrows * (ch + 4)
    canvas = [bytearray(b'\x40' * (W * 3)) for _ in range(H)]
    for k, (w, h, px) in enumerate(imgs):
        ox, oy = (k % cols) * (cw + 4), (k // cols) * (ch + 4)
        for y in range(h):
            canvas[oy + y][ox * 3: ox * 3 + w * 3] = px[y]
    write_png(out, W, H, [bytes(r) for r in canvas])
    print(out, W, H)
