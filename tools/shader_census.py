#!/usr/bin/env python3
"""What the game's Direct3D shaders use, from its own shader caches, as counts only: tools/shader_census.py"""
import collections as C
import mmap
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from game_path import game_root

OPCODES = {0: "nop", 1: "mov", 2: "add", 3: "sub", 4: "mad", 5: "mul", 6: "rcp", 7: "rsq", 8: "dp3", 9: "dp4", 10: "min", 11: "max", 12: "slt", 13: "sge", 14: "exp", 15: "log", 16: "lit",
           17: "dst", 18: "lrp", 19: "frc", 20: "m4x4", 21: "m4x3", 22: "m3x4", 23: "m3x3", 24: "m3x2", 25: "call", 26: "callnz", 27: "loop", 28: "ret", 29: "endloop", 30: "label", 31: "dcl",
           32: "pow", 33: "crs", 34: "sgn", 35: "abs", 36: "nrm", 37: "sincos", 38: "rep", 39: "endrep", 40: "if", 41: "ifc", 42: "else", 43: "endif", 44: "break", 45: "breakc", 46: "mova",
           47: "defb", 48: "defi", 64: "texcoord", 65: "texkill", 66: "tex", 67: "texbem", 68: "texbeml", 69: "texreg2ar", 70: "texreg2gb", 71: "texm3x2pad", 72: "texm3x2tex", 73: "texm3x3pad",
           74: "texm3x3tex", 76: "texm3x3spec", 77: "texm3x3vspec", 78: "expp", 79: "logp", 80: "cnd", 81: "def", 82: "texreg2rgb", 83: "texdp3tex", 84: "texm3x2depth", 85: "texdp3",
           86: "texm3x3", 87: "texdepth", 88: "cmp", 89: "bem", 90: "dp2add", 91: "dsx", 92: "dsy", 93: "texldd", 94: "setp", 95: "texldl", 96: "breakp"}
REGS = {0: "temp", 1: "input", 2: "const", 3: "addr/texture", 4: "rasterout", 5: "attrout", 6: "texcrdout/output", 7: "constint", 8: "colorout", 9: "depthout", 10: "sampler", 11: "const2",
        12: "const3", 13: "const4", 14: "constbool", 15: "loop", 16: "tempfloat16", 17: "misctype", 18: "label", 19: "predicate"}
USAGES = {0: "position", 1: "blendweight", 2: "blendindices", 3: "normal", 4: "psize", 5: "texcoord", 6: "tangent", 7: "binormal", 8: "tessfactor", 9: "positiont", 10: "color", 11: "fog", 12: "depth", 13: "sample"}
SAMPLERS = {0: "unknown", 2: "2d", 3: "cube", 4: "volume"}


def regtype(tok):
    return ((tok >> 28) & 7) | ((tok >> 8) & 0x18)


def census(path, stats, seen):
    with open(path, "rb") as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    for ver, kind in ((b"\x00\x03\xff\xff", "ps"), (b"\x00\x03\xfe\xff", "vs")):
        at = m.find(ver)
        while at >= 0:
            if m[at + 4:at + 6] == b"\xfe\xff" and m[at + 8:at + 12] == b"CTAB":
                walk(m, at, kind, stats, seen)
            at = m.find(ver, at + 4)


def walk(m, at, kind, stats, seen):
    i = at + 4
    code = []
    ops = C.Counter()
    size = len(m)
    while i + 4 <= size:
        tok = struct.unpack_from("<I", m, i)[0]
        if tok == 0x0000FFFF:
            break
        op = tok & 0xFFFF
        if op == 0xFFFE:
            i += 4 + (tok >> 16) * 4
            continue
        n = (tok >> 24) & 0xF
        params = struct.unpack_from(f"<{n}I", m, i + 4) if n else ()
        code.append((tok, params))
        i += 4 + n * 4
    else:
        return
    h = hash(m[at:i])
    if h in seen:
        return
    seen.add(h)
    s = stats[kind]
    s["shaders"] += 1
    s["instructions"].append(len(code))
    for tok, params in code:
        op = tok & 0xFFFF
        name = OPCODES.get(op, f"op{op}")
        s["ops"][name] += 1
        if tok & (1 << 28):
            s["ops"][name + " (predicated)"] += 1
        if name == "dcl":
            usage, target = params[0], params[1]
            rt = regtype(target)
            if rt == 10:
                s["samplers"][SAMPLERS.get((usage >> 27) & 15, "?")] += 1
            else:
                s["decl"][f"{REGS.get(rt, rt)} {USAGES.get(usage & 31, usage & 31)}"] += 1
            continue
        if name in ("def", "defi", "defb"):
            continue
        for k, p in enumerate(params):
            rt = regtype(p)
            s["regs"][REGS.get(rt, rt)] += 1
            if k == 0 and name not in ("if", "ifc", "rep", "loop", "breakc", "callnz", "call", "label", "texkill", "setp"):
                s["dstmod"][(p >> 20) & 15] += 1
            else:
                s["srcmod"][(p >> 24) & 15] += 1
                if p & (1 << 13):
                    s["relative"][REGS.get(rt, rt)] += 1


def main():
    stats = {k: {"shaders": 0, "instructions": [], "ops": C.Counter(), "regs": C.Counter(), "decl": C.Counter(), "samplers": C.Counter(), "dstmod": C.Counter(), "srcmod": C.Counter(), "relative": C.Counter()} for k in ("vs", "ps")}
    seen = set()
    base = os.path.join(game_root(), "BMAAData/BmGame/CookedPC")
    for name in ("GlobalShaderCache-PC-D3D-SM3.bin", "RefShaderCache-PC-D3D-SM3.upk"):
        census(os.path.join(base, name), stats, seen)
    for kind, s in stats.items():
        ins = sorted(s["instructions"])
        print(f"== {kind}: {s['shaders']} shaders, instructions median {ins[len(ins) // 2]}, 99% {ins[len(ins) * 99 // 100]}, longest {ins[-1]}")
        print(" opcodes:", ", ".join(f"{k} {v}" for k, v in s["ops"].most_common()))
        print(" registers:", ", ".join(f"{k} {v}" for k, v in s["regs"].most_common()))
        print(" relative addressing:", dict(s["relative"]))
        print(" declarations:", ", ".join(f"{k} {v}" for k, v in s["decl"].most_common(24)))
        print(" samplers:", dict(s["samplers"]))
        print(" destination modifiers (1 saturate, 2 partial precision, 4 centroid):", dict(s["dstmod"]))
        print(" source modifiers (1 negate, 2 bias, 3 bias+negate, 4 sign, 5 sign+negate, 6 complement, 7 x2, 8 x2+negate, 9 dz, 10 dw, 11 abs, 12 abs+negate, 13 not):", dict(s["srcmod"]))


main()
