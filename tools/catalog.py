#!/usr/bin/env python3
"""Function catalog for Monster.exe (MTM2): pcrecomp disasm32.

    py -3 tools/catalog.py            # -> work/functions_monster.json

The entry point is the seed; the rest is recursive descent, prologues and a
data scan, plus work/seeds.json from earlier runs:

    py -3 ../pcrecomp/tools/disasm/seed_from_log.py scratch/run.log game/Monster.exe --seeds work/seeds.json

The catalog is derived from the game binary, so it lives in work/
and is never committed.
"""
import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'pcrecomp')), 'tools')
GAME = os.path.join(_HERE, 'game')

os.makedirs(os.path.join(_HERE, 'work'), exist_ok=True)     # gitignored, so absent in a fresh copy
# Addresses earlier runs found (indirect calls into code the scans missed),
# kept by pcrecomp seed_from_log.py; also derived from the binary, so in work/.
SEEDS = os.path.join(_HERE, 'work', 'seeds.json')
subprocess.run([sys.executable, os.path.join(TOOLS, 'disasm', 'disasm32.py'),
                os.path.join(GAME, 'Monster.exe'), '-o',
                os.path.join(_HERE, 'work', 'functions_monster.json')]
               + (['--seed-functions', SEEDS] if os.path.exists(SEEDS) else []), check=True)
