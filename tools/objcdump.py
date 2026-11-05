#!/usr/bin/env python3
"""List the guest's legacy Objective-C classes and methods: tools/objcdump.py [ClassNameFilter]"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from game_path import game_binary
from macho import MachO

m = MachO(game_binary())


def cstr(a):
    off = m.vm_to_file(a)
    if off is None:
        return ""
    return m.data[off:m.data.index(b"\0", off)].decode("latin-1")


def methods(ml):
    out = []
    if not ml or ml == 0xFFFFFFFF:
        return out
    n = m.read_u32(ml + 4)
    for i in range(n):
        e = ml + 8 + i * 12
        out.append((cstr(m.read_u32(e)), cstr(m.read_u32(e + 4)), m.read_u32(e + 8)))
    return out


def classes():
    res = {}
    sec = m.section("__class", "__OBJC")
    meta = m.section("__meta_class", "__OBJC")
    for a in range(sec.addr, sec.end, 48):
        isa, sup, name, _ver, info, inst, ivars, ml = [m.read_u32(a + 4 * i) for i in range(8)]
        if not name:
            continue
        cname = cstr(name)
        supname = cstr(sup) if sup else ""
        res[cname] = dict(addr=a, isa=isa, super=supname, size=inst, inst=methods(ml), cls=methods(m.read_u32(isa + 28)) if isa else [])
    cats = m.section("__category", "__OBJC")
    catl = []
    for a in range(cats.addr, cats.end, 28):
        cn, kl, im, cm = [m.read_u32(a + 4 * i) for i in range(4)]
        catl.append(dict(name=cstr(cn), cls=cstr(kl), inst=methods(im), clsm=methods(cm)))
    return res, catl


if __name__ == "__main__":
    flt = sys.argv[1] if len(sys.argv) > 1 else ""
    res, catl = classes()
    print("classes", len(res), "categories", len(catl))
    for k, v in sorted(res.items()):
        if flt and flt not in k:
            print(f"  {k} : {v['super']} size={v['size']} inst={len(v['inst'])} cls={len(v['cls'])}") if flt == "*" else None
            continue
        print(f"{k} : {v['super']} size={v['size']} @{v['addr']:#x}")
        for n, t, imp in v["cls"]:
            print(f"  + {n}  {t}  {imp:#x}")
        for n, t, imp in v["inst"]:
            print(f"  - {n}  {t}  {imp:#x}")
    if flt == "*":
        for c in catl:
            print(f"  cat {c['cls']}({c['name']}) inst={len(c['inst'])} cls={len(c['clsm'])}")
    print("entry pointer value:", hex(m.read_u32(0x3492090)))
