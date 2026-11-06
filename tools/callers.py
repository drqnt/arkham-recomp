#!/usr/bin/env python3
"""List the guest call sites of imported functions, with the function each one is in: tools/callers.py _name ..."""
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from game_path import game_binary
from macho import MachO

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
m = MachO(game_binary())
stub = m.section("__symbol_stub")
ptr_name = {b.addr: b.symbol for b in m.binds}
want = set(sys.argv[1:])
targets = {}
step = stub.reserved2 if getattr(stub, "reserved2", 0) else 6
for a in range(stub.addr, stub.addr + stub.size, step):
    d = m.bytes_at(a, 6)
    if d[:2] == b"\xff\x25":
        name = ptr_name.get(struct.unpack("<I", d[2:6])[0])
        if name in want:
            targets[a] = name
fs = sorted(m.function_starts)
pat = re.compile(r"^\s*([0-9a-f]+):\s*(?:call|jmp)\s+0x([0-9a-f]+)")
with open(f"{ROOT}/build/dis.txt", errors="replace") as fh:
    for line in fh:
        g = pat.match(line)
        if g and int(g.group(2), 16) in targets:
            at = int(g.group(1), 16)
            fn = fs[bisect.bisect_right(fs, at) - 1]
            print(f"{targets[int(g.group(2), 16)]}: {at:#x} in F_{fn:07x}")
