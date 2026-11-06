"""Read-only parser for a thin i386 Mach-O: segments, sections, function starts,
data-in-code, rebases, binds and the symbol table."""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_UNIXTHREAD = 0x5
LC_DYSYMTAB = 0xB
LC_LOAD_DYLIB = 0xC
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_FUNCTION_STARTS = 0x26
LC_DATA_IN_CODE = 0x29
LC_DYLD_INFO_ONLY = 0x80000022

DIC_KINDS = {1: "data", 2: "jt8", 3: "jt16", 4: "jt32", 5: "abs_jt32"}


@dataclass
class Section:
    name: str
    segname: str
    addr: int
    size: int
    fileoff: int
    flags: int
    reserved1: int
    reserved2: int

    @property
    def end(self) -> int:
        return self.addr + self.size


@dataclass
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int
    initprot: int
    sections: list = field(default_factory=list)


@dataclass
class Bind:
    addr: int
    symbol: str
    ordinal: int
    weak: bool
    lazy: bool
    addend: int = 0


@dataclass
class Symbol:
    name: str
    addr: int
    type: int
    sect: int
    desc: int


class MachO:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as fh:
            self.data = fh.read()
        magic, _cpu, _sub, self.filetype, ncmds, _sz, self.flags = struct.unpack_from("<IiiIIII", self.data, 0)
        if magic != 0xFEEDFACE:
            raise ValueError("not a thin 32-bit Mach-O")
        self.segments: list[Segment] = []
        self.sections: list[Section] = []
        self.dylibs: list[str] = []
        self.function_starts: list[int] = []
        self.data_in_code: list[tuple[int, int, str]] = []
        self.rebases: list[int] = []
        self.binds: list[Bind] = []
        self.symbols: list[Symbol] = []
        self.indirect: list[int] = []
        self.entry = 0
        off = 28
        cmds = []
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", self.data, off)
            cmds.append((cmd, off, size))
            off += size
        for cmd, off, _size in cmds:
            if cmd == LC_SEGMENT:
                self._segment(off)
            elif cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB):
                name_off = struct.unpack_from("<I", self.data, off + 8)[0]
                end = self.data.index(b"\0", off + name_off)
                self.dylibs.append(self.data[off + name_off:end].decode())
        for cmd, off, _size in cmds:
            if cmd == LC_SYMTAB:
                self._symtab(off)
            elif cmd == LC_DYSYMTAB:
                ioff, n = struct.unpack_from("<II", self.data, off + 56)
                self.indirect = list(struct.unpack_from(f"<{n}I", self.data, ioff))
            elif cmd == LC_FUNCTION_STARTS:
                self._function_starts(off)
            elif cmd == LC_DATA_IN_CODE:
                self._data_in_code(off)
            elif cmd in (LC_DYLD_INFO_ONLY, 0x22):
                self._dyld_info(off)
            elif cmd == LC_UNIXTHREAD:
                # flavor, count, then x86_thread_state32: eip is register index 10.
                self.entry = struct.unpack_from("<I", self.data, off + 16 + 10 * 4)[0]

    def _segment(self, off: int) -> None:
        name = self.data[off + 8:off + 24].rstrip(b"\0").decode()
        vmaddr, vmsize, fileoff, filesize, _maxp, initprot, nsects, _fl = struct.unpack_from("<IIIIiiII", self.data, off + 24)
        seg = Segment(name, vmaddr, vmsize, fileoff, filesize, initprot)
        for i in range(nsects):
            so = off + 56 + i * 68
            sname = self.data[so:so + 16].rstrip(b"\0").decode()
            segname = self.data[so + 16:so + 32].rstrip(b"\0").decode()
            addr, size, foff, _align, _reloff, _nreloc, flags, r1, r2 = struct.unpack_from("<IIIIIIIII", self.data, so + 32)
            sec = Section(sname, segname, addr, size, foff, flags, r1, r2)
            seg.sections.append(sec)
            self.sections.append(sec)
        self.segments.append(seg)

    def _symtab(self, off: int) -> None:
        symoff, nsyms, stroff, _strsize = struct.unpack_from("<IIII", self.data, off + 8)
        for i in range(nsyms):
            strx, ntype, sect, desc, value = struct.unpack_from("<IBBHI", self.data, symoff + i * 12)
            end = self.data.index(b"\0", stroff + strx)
            name = self.data[stroff + strx:end].decode("latin-1")
            self.symbols.append(Symbol(name, value, ntype, sect, desc))

    @staticmethod
    def _uleb(data: bytes, off: int) -> tuple[int, int]:
        res = shift = 0
        while True:
            b = data[off]
            off += 1
            res |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return res, off

    @staticmethod
    def _sleb(data: bytes, off: int) -> tuple[int, int]:
        res = shift = 0
        while True:
            b = data[off]
            off += 1
            res |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                if b & 0x40:
                    res -= 1 << shift
                return res, off

    def _function_starts(self, off: int) -> None:
        fo, fs = struct.unpack_from("<II", self.data, off + 8)
        text = next(s for s in self.segments if s.name == "__TEXT")
        p, end = fo, fo + fs
        addr = text.vmaddr
        while p < end:
            d, p = self._uleb(self.data, p)
            if d == 0:
                break
            addr += d
            self.function_starts.append(addr)

    def _data_in_code(self, off: int) -> None:
        fo, fs = struct.unpack_from("<II", self.data, off + 8)
        for i in range(fs // 8):
            foff, length, kind = struct.unpack_from("<IHH", self.data, fo + i * 8)
            self.data_in_code.append((self.file_to_vm(foff), length, DIC_KINDS.get(kind, str(kind))))

    def file_to_vm(self, foff: int) -> int:
        for seg in self.segments:
            if seg.fileoff <= foff < seg.fileoff + seg.filesize:
                return foff - seg.fileoff + seg.vmaddr
        raise ValueError(hex(foff))

    def vm_to_file(self, addr: int) -> int | None:
        for seg in self.segments:
            if seg.vmaddr <= addr < seg.vmaddr + seg.filesize:
                return addr - seg.vmaddr + seg.fileoff
        return None

    def read_u32(self, addr: int) -> int:
        off = self.vm_to_file(addr)
        return struct.unpack_from("<I", self.data, off)[0] if off is not None else 0

    def bytes_at(self, addr: int, n: int) -> bytes:
        off = self.vm_to_file(addr)
        return self.data[off:off + n] if off is not None else b""

    def _dyld_info(self, off: int) -> None:
        r_off, r_sz, b_off, b_sz, w_off, w_sz, l_off, l_sz, _e_off, _e_sz = struct.unpack_from("<10I", self.data, off + 8)
        self._rebase_ops(r_off, r_sz)
        self._bind_ops(b_off, b_sz, lazy=False)
        self._bind_ops(l_off, l_sz, lazy=True)
        self.weak_bind_size = w_sz

    def _rebase_ops(self, off: int, size: int) -> None:
        d, end = self.data, off + size
        seg = addr = 0
        while off < end:
            byte = d[off]
            off += 1
            op, imm = byte & 0xF0, byte & 0x0F
            if op == 0x00:
                break
            if op == 0x10:
                continue
            if op == 0x20:
                seg = imm
                addr, off = self._uleb(d, off)
                addr += self.segments[seg].vmaddr
            elif op == 0x30:
                v, off = self._uleb(d, off)
                addr += v
            elif op == 0x40:
                addr += imm * 4
            elif op == 0x50:
                for _ in range(imm):
                    self.rebases.append(addr)
                    addr += 4
            elif op == 0x60:
                n, off = self._uleb(d, off)
                for _ in range(n):
                    self.rebases.append(addr)
                    addr += 4
            elif op == 0x70:
                self.rebases.append(addr)
                v, off = self._uleb(d, off)
                addr += 4 + v
            elif op == 0x80:
                n, off = self._uleb(d, off)
                skip, off = self._uleb(d, off)
                for _ in range(n):
                    self.rebases.append(addr)
                    addr += 4 + skip

    def _bind_ops(self, off: int, size: int, lazy: bool) -> None:
        d, end = self.data, off + size
        seg = addr = ordinal = addend = 0
        sym = ""
        weak = False
        while off < end:
            byte = d[off]
            off += 1
            op, imm = byte & 0xF0, byte & 0x0F
            if op == 0x00:
                if not lazy:
                    break
                continue  # lazy info is a run of records separated by DONE
            if op == 0x10:
                ordinal = imm
            elif op == 0x20:
                ordinal, off = self._uleb(d, off)
            elif op == 0x30:
                ordinal = -1 if imm == 0xF else (-2 if imm == 0xE else 0)
            elif op == 0x40:
                weak = bool(imm & 1)
                e = d.index(b"\0", off)
                sym = d[off:e].decode("latin-1")
                off = e + 1
            elif op == 0x50:
                pass
            elif op == 0x60:
                addend, off = self._sleb(d, off)
            elif op == 0x70:
                seg = imm
                addr, off = self._uleb(d, off)
                addr += self.segments[seg].vmaddr
            elif op == 0x80:
                v, off = self._uleb(d, off)
                addr = (addr + v) & 0xFFFFFFFF
            elif op == 0x90:
                self.binds.append(Bind(addr, sym, ordinal, weak, lazy, addend))
                addr += 4
            elif op == 0xA0:
                self.binds.append(Bind(addr, sym, ordinal, weak, lazy, addend))
                v, off = self._uleb(d, off)
                addr = (addr + 4 + v) & 0xFFFFFFFF
            elif op == 0xB0:
                self.binds.append(Bind(addr, sym, ordinal, weak, lazy, addend))
                addr += 4 + imm * 4
            elif op == 0xC0:
                n, off = self._uleb(d, off)
                skip, off = self._uleb(d, off)
                for _ in range(n):
                    self.binds.append(Bind(addr, sym, ordinal, weak, lazy, addend))
                    addr += 4 + skip

    def section(self, name: str, seg: str | None = None) -> Section | None:
        for sec in self.sections:
            if sec.name == name and (seg is None or sec.segname == seg):
                return sec
        return None

    def defined_symbols(self) -> dict[int, str]:
        """Address to name for symbols defined in this image (N_SECT, not debug)."""
        out: dict[int, str] = {}
        for s in self.symbols:
            if s.type & 0xE0:  # stab
                continue
            if (s.type & 0x0E) == 0x0E and s.addr:
                out.setdefault(s.addr, s.name)
        return out

    def defined_by_name(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for s in self.symbols:
            if s.type & 0xE0:
                continue
            if (s.type & 0x0E) == 0x0E:
                out.setdefault(s.name, s.addr)
        return out
