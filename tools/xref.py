#!/usr/bin/env python3
"""Find code referencing guest strings through the PIC idiom: tools/xref.py "string1" "string2" ..."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from game_path import game_binary
from macho import MachO

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
m = MachO(game_binary())
cs = m.section("__cstring")
off = m.vm_to_file(cs.addr)
blob = m.data[off:off + cs.size]
targets = {}
for t in sys.argv[1:]:
    p = blob.find(t.encode() + b"\0")
    while p >= 0:
        if p == 0 or blob[p - 1] == 0:
            targets[cs.addr + p] = t
            break
        p = blob.find(t.encode() + b"\0", p + 1)
print({hex(k): v for k, v in targets.items()})
pat = re.compile(rb"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)")
disp = re.compile(rb"\+ 0x([0-9a-f]{5,})\]")
base = None
with open(f"{ROOT}/build/dis.txt", "rb") as fh:
    for line in fh:
        g = pat.match(line)
        if not g:
            continue
        mn = g.group(2)
        if mn == b"pop":
            base = int(g.group(1), 16)
            continue
        d = disp.search(g.group(3))
        if d and base is not None:
            v = base + int(d.group(1), 16)
            if v in targets:
                print(f"{g.group(1).decode()} {targets[v]}")
