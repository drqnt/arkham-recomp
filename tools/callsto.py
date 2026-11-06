#!/usr/bin/env python3
"""List guest call sites of translated functions, with the function each is in: tools/callsto.py ADDR ... (hex)"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from game_path import game_binary
from macho import MachO

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
m = MachO(game_binary())
fs = sorted(m.function_starts)
want = {int(a, 16) for a in sys.argv[1:]}
pat = re.compile(r"^\s*([0-9a-f]+):\s*(call|jmp)\s+0x([0-9a-f]+)")
found = {t: [] for t in want}
with open(f"{ROOT}/build/dis.txt", errors="replace") as fh:
    for line in fh:
        g = pat.match(line)
        if g and int(g.group(3), 16) in want:
            at = int(g.group(1), 16)
            found[int(g.group(3), 16)].append(f"{at:x} in F_{fs[bisect.bisect_right(fs, at) - 1]:07x}")
for t, v in found.items():
    print(f"F_{t:07x}: " + (", ".join(v[:10]) if v else "no direct calls"))
