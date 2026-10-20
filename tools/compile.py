#!/usr/bin/env python3
"""Compile the runtime + the recompiled game code (generated/) into the native executable.
Normally run through ../build.py, which first creates generated/ from your disc.

Windows: uses the llvm-mingw toolchain + SDL2 from deps/ (tools/fetch_deps.py downloads them);
         --linux cross-builds the Linux version with Zig (fetch_deps.py --linux).
Linux:   uses the system C compiler ($CC, default cc) and the SDL2 headers (sdl2-config or
         pkg-config sdl2). SDL2 itself is loaded at runtime.

usage: compile.py [--release] [--linux] [--clean] [--jobs N]
"""
import os, sys, subprocess, shutil, platform, hashlib
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, 'deps')
IS_WIN = platform.system() == 'Windows'


def need(path, what, linux=False):
    if not os.path.exists(path):
        sys.exit('%s not found (%s)\nrun:  python tools/fetch_deps.py%s' % (what, path, ' --linux' if linux else ''))
    return path


def sh(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def make_sdl_stub(cc, target, objdir, platform_obj):
    """Build a stub libSDL2-2.0.so.0 exporting the SDL symbols we use, so the cross-linked
    binary records a dependency on the system's real SDL2 without needing it at build time."""
    nm = os.path.join(DEPS, 'llvm-mingw', 'bin', 'llvm-nm.exe')
    out = subprocess.check_output([nm, '-u', platform_obj], text=True)
    syms = sorted({l.split()[-1] for l in out.splitlines() if l.split() and l.split()[-1].startswith('SDL_')})
    stub_c = os.path.join(objdir, 'sdl_stub.c')
    with open(stub_c, 'w') as f:
        for sym in syms:
            f.write('void %s(void) {}' % sym + chr(10))
    so = os.path.join(objdir, 'libSDL2-2.0.so.0')
    code, msg = sh(cc + target + ['-shared', '-fPIC', stub_c, '-o', so, '-Wl,-soname,libSDL2-2.0.so.0'])
    if code:
        print(msg)
        sys.exit(1)
    return so


def output_dir(release, linux_cross):
    return os.path.join(ROOT, 'build', ('linux-' if linux_cross else '') + ('release' if release else 'debug'))


def build(release=False, linux_cross=False, clean=False, jobs=None):
    """compile + link; returns the path of the executable"""
    linux_cross = linux_cross and IS_WIN
    jobs = jobs or os.cpu_count() or 4
    if not os.path.exists(os.path.join(ROOT, 'generated', 'funcs.h')):
        sys.exit('generated/ is missing - run build.py, which recompiles the game code from your disc first')
    outdir = output_dir(release, linux_cross)
    objdir = os.path.join(outdir, 'obj')
    if clean and os.path.exists(outdir):
        shutil.rmtree(outdir)
    os.makedirs(objdir, exist_ok=True)

    if linux_cross:
        # cross-compile from Windows with zig; glibc 2.17 baseline, SDL2 resolved at runtime
        need(os.path.join(DEPS, 'llvm-mingw', 'bin', 'llvm-nm.exe'), 'llvm-mingw')
        cc = [need(os.path.join(DEPS, 'zig', 'zig.exe'), 'Zig', True), 'cc']
        target = ['-target', 'x86_64-linux-gnu.2.17']
        sdl_cflags = ['-I' + need(os.path.join(DEPS, 'SDL2-src', 'include'), 'SDL2 headers', True), '-D_REENTRANT']
        sdl_libs = []
        exe = os.path.join(outdir, 'HarvestMoonBTN')
    elif IS_WIN:
        cc = need(os.path.join(DEPS, 'llvm-mingw', 'bin', 'clang.exe'), 'llvm-mingw')
        sdl = need(os.path.join(DEPS, 'SDL2-2.32.10', 'x86_64-w64-mingw32'), 'SDL2 for MinGW')
        sdl_cflags = ['-I' + os.path.join(sdl, 'include', 'SDL2')]
        sdl_libs = ['-L' + os.path.join(sdl, 'lib'), '-lSDL2']
        exe = os.path.join(outdir, 'HarvestMoonBTN.exe')
        target = ['--target=x86_64-w64-mingw32']
        cc = [cc]
    else:
        cc = os.environ.get('CC', 'cc')
        try:
            sdl_cflags = subprocess.check_output(['sdl2-config', '--cflags'], text=True).split()
            sdl_libs = subprocess.check_output(['sdl2-config', '--libs'], text=True).split()
        except Exception:
            sdl_cflags = subprocess.check_output(['pkg-config', '--cflags', 'sdl2'], text=True).split()
        sdl_libs = ['-ldl']   # SDL2 is loaded at runtime (runtime/sdl_dyn.h)
        exe = os.path.join(outdir, 'HarvestMoonBTN')
        target = []
        cc = cc.split()

    common = target + ['-std=gnu11', '-fno-strict-aliasing', '-fwrapv', '-I' + os.path.join(ROOT, 'runtime'),
                       '-I' + os.path.join(ROOT, 'generated')]
    opt_gen = ['-O2'] if release else ['-O1']
    opt_rt = ['-O2', '-g'] if release else ['-O1', '-g']
    warn_rt = ['-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-missing-field-initializers']
    warn_gen = ['-w']

    srcs = []
    rt = os.path.join(ROOT, 'runtime')
    for f in sorted(os.listdir(rt)):
        if f.endswith('.c'):
            flags = common + opt_rt + warn_rt
            if f == 'platform_sdl.c':
                flags = flags + sdl_cflags
            srcs.append((os.path.join(rt, f), flags))
    gen = os.path.join(ROOT, 'generated')
    for f in sorted(os.listdir(gen)):
        if f.endswith('.c'):
            srcs.append((os.path.join(gen, f), common + opt_gen + warn_gen))

    def compile_one(item):
        src, flags = item
        obj = os.path.join(objdir, os.path.basename(src)[:-2] + '.o')
        # rebuild if source, header set or flags changed
        key = hashlib.sha1((' '.join(flags)).encode()).hexdigest()[:8]
        stamp = obj + '.' + key
        hdr_t = max(os.path.getmtime(os.path.join(rt, h)) for h in os.listdir(rt) if h.endswith('.h'))
        if os.path.exists(obj) and os.path.exists(stamp):
            t = os.path.getmtime(obj)
            if t >= os.path.getmtime(src) and t >= hdr_t:
                return obj, 0, ''
        code, out = sh(cc + flags + ['-c', src, '-o', obj])
        if code == 0:
            for old in os.listdir(objdir):
                if old.startswith(os.path.basename(obj) + '.'):
                    os.remove(os.path.join(objdir, old))
            open(stamp, 'w').close()
        return obj, code, out

    objs = []
    failed = False
    with ThreadPoolExecutor(jobs) as ex:
        for obj, code, out in ex.map(compile_one, srcs):
            objs.append(obj)
            if out.strip():
                print(out.strip())
            if code:
                failed = True
    if failed:
        sys.exit('BUILD FAILED')
    link = cc + target + objs + sdl_libs + ['-o', exe]
    if linux_cross:
        link += ['-lm', '-ldl', '-lpthread', '-Wl,-z,stack-size=16777216', '-s']
    elif IS_WIN:
        link += ['-static-libgcc', '-Wl,--stack,16777216']
        link += ['-mwindows'] if release else ['-mconsole']
        link += ['-lwinmm']
    else:
        link += ['-lm', '-Wl,-z,stacksize=16777216']
    code, out = sh(link)
    if out.strip():
        print(out.strip())
    if code:
        sys.exit('LINK FAILED')
    if IS_WIN and not linux_cross:
        dll = os.path.join(DEPS, 'SDL2-2.32.10', 'x86_64-w64-mingw32', 'bin', 'SDL2.dll')
        dst = os.path.join(outdir, 'SDL2.dll')
        if not (os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(dll)):
            shutil.copy(dll, dst)
    print('built', exe)
    return exe


if __name__ == '__main__':
    jobs = int(sys.argv[sys.argv.index('--jobs') + 1]) if '--jobs' in sys.argv else None
    build('--release' in sys.argv, '--linux' in sys.argv, '--clean' in sys.argv, jobs)
