#!/usr/bin/env python3
"""Write config/hooks.json with a function and all the functions it calls directly: tools/hook_callees.py ADDR [ADDR...]"""
import bisect
import json
import os
import pickle
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from game_path import game_binary
from macho import MachO

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
m = MachO(game_binary())
starts = sorted(m.function_starts)
stub = m.section("__symbol_stub")
with open(f"{ROOT}/build/dis.idx", "rb") as fh:
    addrs, offs = pickle.load(fh)
out = set()
for a in sys.argv[1:]:
    f = int(a, 16)
    end = starts[bisect.bisect_right(starts, f)]
    lo, hi = bisect.bisect_left(addrs, f), bisect.bisect_left(addrs, end)
    with open(f"{ROOT}/build/dis.txt", "rb") as fh:
        fh.seek(offs[lo])
        text = fh.read(offs[min(hi, len(offs) - 1)] - offs[lo]).decode("latin-1")
    out.add(f)
    for g in re.finditer(r"\bcall\s+0x([0-9a-f]+)", text):
        t = int(g.group(1), 16)
        if t != f and not (stub.addr <= t < stub.end) and t in set(starts):
            out.add(t)
with open(f"{ROOT}/config/hooks.json", "w") as fh:
    json.dump(sorted(hex(x)[2:] for x in out), fh)
print(len(out), "hooks")
