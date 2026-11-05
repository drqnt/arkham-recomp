#!/usr/bin/env python3
"""Show disassembly around a guest address, with stub names resolved: tools/dis.py ADDR [before] [after]"""
import bisect
import os
import pickle
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from game_path import game_binary
from macho import MachO

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
addr = int(sys.argv[1], 16)
before = int(sys.argv[2]) if len(sys.argv) > 2 else 20
after = int(sys.argv[3]) if len(sys.argv) > 3 else 5
with open(f"{ROOT}/build/dis.idx", "rb") as fh:
    addrs, offs = pickle.load(fh)
m = MachO(game_binary())
stub = m.section("__symbol_stub")
ptr_name = {b.addr: b.symbol for b in m.binds}
defined = m.defined_symbols()
i = bisect.bisect_left(addrs, addr)
lo, hi = max(i - before, 0), min(i + after, len(offs) - 1)
with open(f"{ROOT}/build/dis.txt", "rb") as fh:
    fh.seek(offs[lo])
    data = fh.read(offs[hi] - offs[lo]).decode("latin-1")
for line in data.splitlines():
    line = re.sub(r"\s*<[^>]*>", "", line).rstrip()
    g = re.search(r"\b(call|jmp)\s+0x([0-9a-f]+)$", line)
    if g:
        t = int(g.group(2), 16)
        if stub.addr <= t < stub.end:
            raw = m.bytes_at(t, 6)
            p = int.from_bytes(raw[2:6], "little")
            line += f"   ; {ptr_name.get(p, hex(m.read_u32(p)))}"
        elif t in defined:
            line += f"   ; {defined[t]}"
    g = re.search(r"\[([a-z]+) \+ (0x[0-9a-f]+)\]", line)
    print(line)
