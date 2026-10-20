#!/usr/bin/env python3
"""Build and run the runtime unit tests (GTE, GPU, SPU, memory-card file system).
They test the hardware/BIOS reimplementation only and need no game files.

usage: python tools/run_tests.py
"""
import glob, os, platform, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RT = os.path.join(ROOT, 'runtime')
OUT = os.path.join(ROOT, 'build', 'tests')

if platform.system() == 'Windows':
    CC = [os.path.join(ROOT, 'deps', 'llvm-mingw', 'bin', 'clang.exe')]
    EXE = '.exe'
else:
    CC = os.environ.get('CC', 'cc').split()
    EXE = ''

# test -> runtime sources it needs (None = the whole runtime except main + frontend)
TESTS = {
    'gte_test': ['gte.c'],
    'gpu_test': ['gpu.c'],
    'spu_test': ['spu.c'],
    'memcard_fs_test': None,
}


def main():
    os.makedirs(OUT, exist_ok=True)
    everything = [f for f in sorted(glob.glob(os.path.join(RT, '*.c')))
                  if os.path.basename(f) not in ('main.c', 'platform_sdl.c')]
    failed = []
    for name, deps in TESTS.items():
        srcs = [os.path.join(RT, 'tests', name + '.c')]
        srcs += everything if deps is None else [os.path.join(RT, d) for d in deps]
        exe = os.path.join(OUT, name + EXE)
        cmd = CC + ['-std=gnu11', '-O2', '-w', '-I' + RT] + srcs + ['-o', exe]
        if platform.system() != 'Windows':
            cmd.append('-lm')
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(r.stdout + r.stderr)
            failed.append(name + ' (build)')
            continue
        r = subprocess.run([exe] + ([OUT] if name == 'spu_test' else []), cwd=OUT, capture_output=True, text=True)
        last = (r.stdout.strip().splitlines() or [''])[-1]
        print('%-16s %s  %s' % (name, 'ok  ' if r.returncode == 0 else 'FAIL', last))
        if r.returncode:
            failed.append(name)
    if failed:
        sys.exit('failed: ' + ', '.join(failed))


if __name__ == '__main__':
    main()
