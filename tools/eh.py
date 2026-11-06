#!/usr/bin/env python3
"""Parse the guest's __eh_frame and __gcc_except_tab: every landing pad becomes its own entry point for the lifter."""
from __future__ import annotations

import struct
import sys


def uleb(d: bytes, o: int) -> tuple[int, int]:
    r = s = 0
    while True:
        b = d[o]
        o += 1
        r |= (b & 0x7F) << s
        s += 7
        if not b & 0x80:
            return r, o


def sleb(d: bytes, o: int) -> tuple[int, int]:
    r = s = 0
    while True:
        b = d[o]
        o += 1
        r |= (b & 0x7F) << s
        s += 7
        if not b & 0x80:
            if b & 0x40:
                r -= 1 << s
            return r, o


class Image:
    """Bytes of __TEXT by virtual address."""

    def __init__(self, m):
        self.m = m
        seg = next(s for s in m.segments if s.name == "__TEXT")
        self.base = seg.vmaddr
        self.data = m.data[seg.fileoff:seg.fileoff + seg.filesize]

    def off(self, a: int) -> int:
        return a - self.base


def read_enc(img: Image, a: int, enc: int) -> tuple[int, int]:
    """Read a DW_EH_PE-encoded value at virtual address a. Returns (value, next address)."""
    if enc == 0xFF:
        return 0, a
    d = img.data
    o = img.off(a)
    fmt = enc & 0x0F
    if fmt == 0x00 or fmt == 0x03:
        v, n = struct.unpack_from("<I", d, o)[0], o + 4
    elif fmt == 0x0B:
        v, n = struct.unpack_from("<i", d, o)[0], o + 4
    elif fmt == 0x01:
        v, n = uleb(d, o)
    elif fmt == 0x09:
        v, n = sleb(d, o)
    elif fmt == 0x02:
        v, n = struct.unpack_from("<H", d, o)[0], o + 2
    elif fmt == 0x0A:
        v, n = struct.unpack_from("<h", d, o)[0], o + 2
    elif fmt == 0x04:
        v, n = struct.unpack_from("<Q", d, o)[0], o + 8
    elif fmt == 0x0C:
        v, n = struct.unpack_from("<q", d, o)[0], o + 8
    else:
        raise ValueError(f"encoding {enc:#x}")
    if (enc & 0x70) == 0x10 and v:
        v = (v + a) & 0xFFFFFFFF
    return v, a + (n - o)


def parse_fdes(m):
    """Yield (begin, end, lsda) for every FDE, plus a summary of the CIE encodings seen."""
    img = Image(m)
    sec = m.section("__eh_frame")
    a, end = sec.addr, sec.end
    cies = {}
    out = []
    d = img.data
    while a < end:
        o = img.off(a)
        length = struct.unpack_from("<I", d, o)[0]
        if length == 0:
            break
        cid = struct.unpack_from("<I", d, o + 4)[0]
        body = a + 8
        nxt = a + 4 + length
        if cid == 0:
            p = img.off(body) + 1
            q = d.index(b"\0", p)
            aug = d[p:q].decode()
            p = q + 1
            _, p = uleb(d, p)
            _, p = sleb(d, p)
            _, p = uleb(d, p)
            fde_enc, lsda_enc = 0, 0xFF
            if aug.startswith("z"):
                _, p = uleb(d, p)
                for ch in aug[1:]:
                    if ch == "P":
                        penc = d[p]
                        _, na = read_enc(img, img.base + p + 1, penc)
                        p = img.off(na)
                    elif ch == "L":
                        lsda_enc = d[p]
                        p += 1
                    elif ch == "R":
                        fde_enc = d[p]
                        p += 1
            cies[a] = (fde_enc, lsda_enc, aug)
        else:
            cie_addr = a + 4 - cid
            fde_enc, lsda_enc, aug = cies[cie_addr]
            begin, q = read_enc(img, body, fde_enc)
            rng, q = read_enc(img, q, fde_enc & 0x0F)
            lsda = 0
            if aug.startswith("z"):
                alen, po = uleb(d, img.off(q))
                if lsda_enc != 0xFF and alen:
                    lsda, _ = read_enc(img, img.base + po, lsda_enc)
            out.append((begin, begin + rng, lsda))
        a = nxt
    return out, img


def landing_pads(fde, img):
    """Landing pad addresses from one function's LSDA."""
    begin, _end, lsda = fde
    d = img.data
    a = lsda
    start_enc = d[img.off(a)]
    a += 1
    lp_start = begin
    if start_enc != 0xFF:
        lp_start, a = read_enc(img, a, start_enc)
    tt_enc = d[img.off(a)]
    a += 1
    if tt_enc != 0xFF:
        _, o = uleb(d, img.off(a))
        a = img.base + o
    cs_enc = d[img.off(a)]
    a += 1
    cs_len, o = uleb(d, img.off(a))
    a = img.base + o
    cs_end = a + cs_len
    pads = []
    while a < cs_end:
        _s, a = read_enc(img, a, cs_enc)
        _l, a = read_enc(img, a, cs_enc)
        lp, a = read_enc(img, a, cs_enc)
        _act, o = uleb(d, img.off(a))
        a = img.base + o
        if lp:
            pads.append(lp_start + lp)
    return pads


def all_landing_pads(m) -> tuple[set[int], int, int]:
    fdes, img = parse_fdes(m)
    pads: set[int] = set()
    with_lsda = 0
    for f in fdes:
        if f[2]:
            with_lsda += 1
            pads.update(landing_pads(f, img))
    return pads, len(fdes), with_lsda


if __name__ == "__main__":
    import os
    sys.path.insert(0, os.path.dirname(__file__))
    from game_path import game_binary
    from macho import MachO

    mo = MachO(game_binary())
    pads_, n, w = all_landing_pads(mo)
    print(f"FDEs {n}, with LSDA {w}, distinct landing pads {len(pads_)}")
    fd, _ = parse_fdes(mo)
    print("sample", [(hex(a), hex(b), hex(c)) for a, b, c in fd[:3]])
    print("sample pads", [hex(x) for x in sorted(pads_)[:6]])
    tot = mo.section("__text").size
    print(f"landing pads per MB of code: {len(pads_) / (tot / 1e6):.0f}")
