#!/usr/bin/env python3
"""Download the build toolchain into deps/ (Windows hosts).

  deps/llvm-mingw     clang + LLVM for Windows (github.com/mstorsjo/llvm-mingw)
  deps/SDL2-2.32.10   SDL2 development files for MinGW (window, input, audio)
  with --linux, for cross-building the Linux version on Windows:
  deps/zig            Zig (used as a Linux cross C compiler)
  deps/SDL2-src       SDL2 headers

Every download is checked against a pinned SHA-256.
Linux hosts do not need this: install a C compiler and the SDL2 development package.

usage: python tools/fetch_deps.py [--linux] [--dest DIR]
"""
import hashlib, os, shutil, sys, tarfile, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEPS = [
    dict(name='llvm-mingw', top='llvm-mingw-20261006-ucrt-x86_64', linux=False,
         url='https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip',
         sha256='317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a'),
    dict(name='SDL2-2.32.10', top='SDL2-2.32.10', linux=False,
         url='https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip',
         sha256='f15cff5fca62ec9381a016ef1d42a95c638cd72d2f226ba5781c76fe43dbd1ac'),
    dict(name='zig', top='zig-x86_64-windows-0.17.0', linux=True,
         url='https://ziglang.org/download/0.17.0/zig-x86_64-windows-0.17.0.zip',
         sha256='b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428'),
    dict(name='SDL2-src', top='SDL2-2.32.10', linux=True,
         url='https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz',
         sha256='5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165'),
]


def download(url, path):
    with urllib.request.urlopen(url) as r, open(path, 'wb') as f:
        total = int(r.headers.get('Content-Length') or 0)
        done = 0
        while True:
            block = r.read(1 << 20)
            if not block:
                break
            f.write(block)
            done += len(block)
            if total:
                print('\r  %5.1f / %.1f MB' % (done / 1e6, total / 1e6), end='', flush=True)
    print()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def safe_extract(archive, dest):
    base = os.path.realpath(dest)
    if archive.endswith('.zip'):
        with zipfile.ZipFile(archive) as z:
            for n in z.namelist():
                if not os.path.realpath(os.path.join(dest, n)).startswith(base + os.sep):
                    raise SystemExit('unsafe path in archive: ' + n)
            z.extractall(dest)
    else:
        with tarfile.open(archive) as t:
            members = []
            for m in t.getmembers():
                if not os.path.realpath(os.path.join(dest, m.name)).startswith(base + os.sep):
                    raise SystemExit('unsafe entry in archive: ' + m.name)
                if m.issym() or m.islnk():
                    continue                 # links are not needed (and not extracted)
                members.append(m)
            if hasattr(tarfile, 'data_filter'):
                t.extractall(dest, members, filter='data')
            else:
                t.extractall(dest, members)


def main():
    linux = '--linux' in sys.argv
    dest = os.path.join(ROOT, 'deps')
    if '--dest' in sys.argv:
        dest = os.path.abspath(sys.argv[sys.argv.index('--dest') + 1])
    os.makedirs(dest, exist_ok=True)
    for d in DEPS:
        if d['linux'] and not linux:
            continue
        out = os.path.join(dest, d['name'])
        if os.path.isdir(out):
            print('%-14s already present' % d['name'])
            continue
        archive = os.path.join(dest, os.path.basename(d['url']))
        print('%-14s downloading %s' % (d['name'], d['url']))
        download(d['url'], archive)
        got = sha256(archive)
        if got != d['sha256']:
            os.remove(archive)
            raise SystemExit('checksum mismatch for %s\n  expected %s\n  got      %s' % (d['url'], d['sha256'], got))
        tmp = os.path.join(dest, '.extract-' + d['name'])
        shutil.rmtree(tmp, ignore_errors=True)
        print('%-14s extracting' % d['name'])
        safe_extract(archive, tmp)
        os.rename(os.path.join(tmp, d['top']), out)
        shutil.rmtree(tmp)
        os.remove(archive)
    print('toolchain ready in', dest)


if __name__ == '__main__':
    main()
