#!/usr/bin/env python3
r"""Put your copy of Monster Truck Madness 2's files in game/, from the disc.

    py -3 tools/gamefiles.py "Monster Truck Madness 2.iso"   # a 2048-byte ISO
    py -3 tools/gamefiles.py E:\                             # the CD in a drive, or a copy of it

The disc keeps the game in two folders: ENGLISH\ (Monster.ex_, which is a
plain PE despite the name, the renderer DLLs, UI/SOUND/TRUCK2.POD, videos)
and SHARED\ (the track PODs, music, the other renderer DLLs). The ACME
installer merged them into one folder; so does this, except SHARED\VIDEOS,
which the game opens as Shared\Videos\ under its folder. Setup, DirectX 5 and
Indeo are not needed and not copied.

Disc images are read with pcrecomp's ISO9660 walker
(tools/assets/iso_peek.py), so nothing is mounted. It supplies your own copy
to the local build; nothing is downloaded, and none of it is ever committed.
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'game')
LANG = 'ENGLISH/'
SKIP = ('SETUP', 'DSETUP', 'MSVCRT.DLL', 'EULA.TXT')     # installer and redistributables
RENAME = {'MONSTER.EX_': 'Monster.exe'}


def target(p):
    """Where disc path p goes under game/, or None."""
    up = p.upper()
    for pre in (LANG, 'SHARED/'):
        if up.startswith(pre):
            rel = p[len(pre):]
            if rel.upper().startswith(SKIP):
                return None
            if pre == 'SHARED/' and rel.upper().startswith('VIDEOS/'):
                return 'Shared/' + rel          # opened as Shared\Videos\MSLogo.smk
            return RENAME.get(rel.upper(), rel)
    return None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    if os.path.isdir(src):
        files = [(os.path.relpath(os.path.join(d, f), src).replace('\\', '/'), os.path.join(d, f))
                 for d, _, fs in os.walk(src) for f in fs]
        reader = None
    else:
        sys.path.insert(0, os.path.join(os.environ.get('PCRECOMP', os.path.join(ROOT, '..', 'pcrecomp')),
                                        'tools', 'assets'))
        import iso_peek
        reader = iso_peek.Reader(src)
        files = [(p, (off, n)) for p, off, n in iso_peek.walk(reader)]
    n = 0
    for p, where in files:
        rel = target(p)
        if rel is None:
            continue
        dst = os.path.join(OUT, *rel.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if reader is None:
            shutil.copyfile(where, dst)
        else:
            off, size = where
            with open(dst, 'wb') as f:
                f.write(reader.read(off, size))
        n += 1
    if not os.path.exists(os.path.join(OUT, 'Monster.exe')):
        sys.exit('no %sMONSTER.EX_ in %s: is this the Monster Truck Madness 2 disc?' % (LANG, src))
    print('%d files in %s' % (n, OUT))


if __name__ == '__main__':
    main()
