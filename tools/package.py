#!/usr/bin/env python3
"""Assemble a ready-to-play folder (and optionally a .zip) in dist/ from a build.
Normally run through ../build.py.

Two flavours:
  disc image (default)  the game reads your .cue/.bin; hmbtn.ini points at the image the
                        build was made from, and the "disc" folder is the fallback location
  data pack (--pak)     HarvestMoonBTN.pak (made from your disc by tools/makepak.py) is
                        placed next to the executable; no disc image needed

These folders contain the game's program code (and with --pak its data) translated from your
copy - they are for your own use, not for sharing.
"""
import os, shutil, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TPL = os.path.join(ROOT, 'dist-template')

GAME_DATA_DISC = """\
GAME DATA
---------
The game's graphics, music and text are read from your own disc image of Harvest Moon: Back to
Nature (USA, SLUS-01115) - the .cue + .bin pair (Mode2/2352).{remembered}
Put the .cue and .bin into the "disc" folder next to the executable (any file names), set
"disc =" in hmbtn.ini, or pass the path on the command line:
    HarvestMoonBTN "D:\\path\\to\\game.cue"
"""
REMEMBERED = """ hmbtn.ini ("disc =") points at the
image this folder was built from; if you move or delete that image, do one of the following."""

GAME_DATA_PAK = """\
GAME DATA
---------
The game's graphics, music and text are in HarvestMoonBTN.pak, which was made from your own
disc image and must stay in the same folder as the executable - the disc image itself is not
needed. "HarvestMoonBTN --verify-data" checks that file for damage.
"""

DISC_NOTE = ("Put your own disc image of Harvest Moon: Back to Nature (USA) here:\n"
             "the .cue file and its .bin file (any file names).\n")


def package(name, files, pak=None, disc_path=None, make_zip=False, dist=None):
    """files: [(source, name in folder)]; returns the folder path"""
    dist = dist or os.path.join(ROOT, 'dist')
    out = os.path.join(dist, name)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    for src, dst in files:
        shutil.copy2(src, os.path.join(out, dst))
    if pak:
        shutil.copy2(pak, os.path.join(out, 'HarvestMoonBTN.pak'))
    else:
        os.makedirs(os.path.join(out, 'disc'))
        with open(os.path.join(out, 'disc', 'PUT YOUR DISC IMAGE HERE.txt'), 'w') as f:
            f.write(DISC_NOTE)

    ini = open(os.path.join(TPL, 'hmbtn.ini'), encoding='utf-8').read()
    if disc_path and not pak:
        ini = ini.replace('#disc = ', 'disc = %s\n#disc = ' % os.path.abspath(disc_path), 1)
    with open(os.path.join(out, 'hmbtn.ini'), 'w', encoding='utf-8') as f:
        f.write(ini)
    readme = open(os.path.join(TPL, 'README.txt'), encoding='utf-8').read()
    with open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8') as f:
        game_data = GAME_DATA_PAK if pak else GAME_DATA_DISC.format(remembered=REMEMBERED if disc_path else '')
        f.write(readme.replace('{{GAME_DATA}}\n', game_data))
    shutil.copy2(os.path.join(TPL, 'LICENSE-SDL2.txt'), os.path.join(out, 'LICENSE-SDL2.txt'))

    if make_zip:
        zpath = out + '.zip'
        with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
            for dirpath, _, names in os.walk(out):
                for n in names:
                    full = os.path.join(dirpath, n)
                    info = zipfile.ZipInfo.from_file(full, os.path.join(name, os.path.relpath(full, out)))
                    if n == 'HarvestMoonBTN':
                        info.external_attr = 0o755 << 16
                    method = zipfile.ZIP_STORED if n.endswith('.pak') else zipfile.ZIP_DEFLATED   # already compressed
                    with open(full, 'rb') as fh:
                        z.writestr(info, fh.read(), method)
        print('packaged', zpath)
    print('packaged', out)
    return out
