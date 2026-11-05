#!/usr/bin/env python3
"""Find a C++ class's vtable through RTTI and print chosen slots: tools/vtable.py 9CBMAAGame 0x174 0x15c ..."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from game_path import game_binary
from macho import MachO

m = MachO(game_binary())
name = sys.argv[1].encode() + b"\0"
data = m.data
cs = m.section("__const", "__TEXT")
hits = []
pos = 0
while True:
    pos = data.find(name, pos)
    if pos < 0:
        break
    hits.append(m.file_to_vm(pos))
    pos += 1
dc = m.section("__const", "__DATA")
blob = data[dc.fileoff:dc.fileoff + dc.size]
words = struct.unpack(f"<{len(blob) // 4}I", blob[:len(blob) // 4 * 4])
ti = None
for i, w in enumerate(words):
    if w in hits:
        ti = dc.addr + (i - 1) * 4
        break
print("typeinfo", hex(ti) if ti else None)
for i, w in enumerate(words):
    if w == ti and words[i - 1] == 0:
        vt = dc.addr + (i + 1) * 4
        print("vtable", hex(vt), "slots:", " ".join(f"{s}={m.read_u32(vt + int(s, 16)):#x}" for s in sys.argv[2:]))
