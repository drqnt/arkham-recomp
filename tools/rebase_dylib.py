#!/usr/bin/env python3
"""Write a copy of a 32-bit dylib as if loaded at a fixed address, the way dyld would slide it. The lifter analyses that
copy in build/, so every address it emits is the final guest address. The copy stays out of the repo with the rest of build/."""
import os
import struct

from macho import MachO

LC_SEGMENT = 1
LC_SYMTAB = 2


def rebase_dylib(src: str, dst: str, base: int) -> None:
    m = MachO(src)
    d = bytearray(m.data)
    for addr in m.rebases:
        off = m.vm_to_file(addr)
        if off is not None:
            struct.pack_into("<I", d, off, (struct.unpack_from("<I", d, off)[0] + base) & 0xFFFFFFFF)
    ncmds = struct.unpack_from("<I", d, 16)[0]
    off = 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", d, off)
        if cmd == LC_SEGMENT:
            vm = struct.unpack_from("<I", d, off + 24)[0]
            struct.pack_into("<I", d, off + 24, vm + base)
            nsects = struct.unpack_from("<I", d, off + 48)[0]
            for k in range(nsects):
                so = off + 56 + k * 68
                struct.pack_into("<I", d, so + 32, struct.unpack_from("<I", d, so + 32)[0] + base)
        elif cmd == LC_SYMTAB:
            symoff, nsyms = struct.unpack_from("<II", d, off + 8)
            for k in range(nsyms):
                no = symoff + k * 12
                if d[no + 4] & 0xE0:
                    continue
                if (d[no + 4] & 0x0E) == 0x0E:
                    struct.pack_into("<I", d, no + 8, struct.unpack_from("<I", d, no + 8)[0] + base)
        off += size
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "wb") as fh:
        fh.write(d)
