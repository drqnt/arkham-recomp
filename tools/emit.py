"""Translate one parsed x86 instruction into C statements.

Every emitter returns C text. Register state lives in locals named after the x86
registers, flags in cf/zf/sf/of/pf/af, XMM registers in X(n). See runtime/rt.h."""
from __future__ import annotations

from x86 import Ins, Op

C, Z, S, O, P, A = 1, 2, 4, 8, 16, 32
ALL = C | Z | S | O | P | A
NOZF = ALL & ~C  # inc/dec keep CF

CONDS = {
    "e": ("zf", Z), "ne": ("!zf", Z), "b": ("cf", C), "ae": ("!cf", C),
    "be": ("(cf|zf)", C | Z), "a": ("!(cf|zf)", C | Z), "s": ("sf", S), "ns": ("!sf", S),
    "l": ("(sf!=of)", S | O), "ge": ("(sf==of)", S | O), "le": ("(zf|(sf!=of))", Z | S | O),
    "g": ("(!zf&&sf==of)", Z | S | O), "o": ("of", O), "no": ("!of", O), "p": ("pf", P), "np": ("!pf", P),
}
TYPES = {1: "u8", 2: "u16", 4: "u32", 8: "u64"}
STYPES = {1: "i8", 2: "i16", 4: "i32", 8: "i64"}
BITS = {1: 8, 2: 16, 4: 32, 8: 64}


class Unsupported(Exception):
    pass


def hx(v: int) -> str:
    return f"0x{v & 0xFFFFFFFF:x}u"


def addr(op: Op) -> str:
    parts = []
    if op.base:
        parts.append(op.base)
    if op.index:
        parts.append(f"{op.index}*{op.scale}" if op.scale != 1 else op.index)
    if op.disp or not parts:
        parts.append(hx(op.disp))
    expr = "+".join(parts)
    if op.seg == "gs":
        return f"(GSBASE+{expr})"
    if op.seg == "fs":
        raise Unsupported("fs segment")
    return expr


def size_of(op: Op, default: int = 4) -> int:
    return op.size or default


def rd(op: Op, size: int = 0) -> str:
    k = op.kind
    if k == "r32":
        return op.reg
    if k == "r16":
        return f"(u16){op.reg}"
    if k == "r8":
        return f"(u8)({op.reg}>>8)" if op.high else f"(u8){op.reg}"
    if k == "imm":
        s = size or 4
        return hx(op.val & ((1 << (8 * s)) - 1)) if s < 4 else hx(op.val)
    if k == "mem":
        s = op.size or size or 4
        return f"R{BITS[s]}({addr(op)})"
    raise Unsupported(f"rd {k}")


def wr(op: Op, expr: str) -> str:
    k = op.kind
    if k == "r32":
        return f"{op.reg}={expr};"
    if k == "r16":
        return f"{op.reg}=({op.reg}&0xffff0000u)|(u16)({expr});"
    if k == "r8":
        if op.high:
            return f"{op.reg}=({op.reg}&0xffff00ffu)|((u32)(u8)({expr})<<8);"
        return f"{op.reg}=({op.reg}&0xffffff00u)|(u8)({expr});"
    if k == "mem":
        return f"R{BITS[op.size or 4]}({addr(op)})={expr};"
    raise Unsupported(f"wr {k}")


def opsize(ops: list[Op]) -> int:
    for o in ops:
        if o.kind in ("r32", "r16", "r8") or (o.kind == "mem" and o.size):
            return o.size
    return 4


def fl_zsp(live: int, bits: int, r: str = "r") -> str:
    s = ""
    if live & Z:
        s += f"zf=({r}==0);"
    if live & S:
        s += f"sf=({r}>>{bits - 1})&1;"
    if live & P:
        s += f"pf=PAR({r});"
    return s


def e_add(i: Ins, live: int) -> str:
    d, s = i.ops
    sz = opsize(i.ops)
    T, b = TYPES[sz], BITS[sz]
    f = fl_zsp(live, b)
    if live & C:
        f += "cf=r<a;"
    if live & O:
        f += f"of=(((a^r)&(b^r))>>{b - 1})&1;"
    if live & A:
        f += "af=((a^b^r)>>4)&1;"
    return f"{{{T} a={rd(d, sz)},b={rd(s, sz)};{T} r=({T})(a+b);{wr(d, 'r')}{f}}}"


def _sub_flags(live: int, b: int) -> str:
    f = fl_zsp(live, b)
    if live & C:
        f += "cf=a<b;"
    if live & O:
        f += f"of=(((a^b)&(a^r))>>{b - 1})&1;"
    if live & A:
        f += "af=((a^b^r)>>4)&1;"
    return f


def e_sub(i: Ins, live: int) -> str:
    d, s = i.ops
    sz = opsize(i.ops)
    T, b = TYPES[sz], BITS[sz]
    if d.kind == s.kind == "r32" and d.reg == s.reg:
        f = "zf=1;" if live & Z else ""
        f += "sf=0;" if live & S else ""
        f += "cf=0;" if live & C else ""
        f += "of=0;" if live & O else ""
        f += "pf=1;" if live & P else ""
        f += "af=0;" if live & A else ""
        return f"{d.reg}=0;{f}"
    return f"{{{T} a={rd(d, sz)},b={rd(s, sz)};{T} r=({T})(a-b);{wr(d, 'r')}{_sub_flags(live, b)}}}"


def e_cmp(i: Ins, live: int) -> str:
    d, s = i.ops
    sz = opsize(i.ops)
    T, b = TYPES[sz], BITS[sz]
    if not live:
        return ""
    return f"{{{T} a={rd(d, sz)},b={rd(s, sz)};{T} r=({T})(a-b);{_sub_flags(live, b)}}}"


def _logic_flags(live: int, b: int) -> str:
    f = fl_zsp(live, b)
    if live & C:
        f += "cf=0;"
    if live & O:
        f += "of=0;"
    if live & A:
        f += "af=0;"
    return f


def e_logic(opc: str):
    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        sz = opsize(i.ops)
        T, b = TYPES[sz], BITS[sz]
        if opc == "^" and d.kind == s.kind == "r32" and d.reg == s.reg:
            f = "zf=1;" if live & Z else ""
            f += "sf=0;" if live & S else ""
            f += "cf=0;" if live & C else ""
            f += "of=0;" if live & O else ""
            f += "pf=1;" if live & P else ""
            f += "af=0;" if live & A else ""
            return f"{d.reg}=0;{f}"
        return f"{{{T} r=({T})(({rd(d, sz)}){opc}({rd(s, sz)}));{wr(d, 'r')}{_logic_flags(live, b)}}}"
    return fn


def e_test(i: Ins, live: int) -> str:
    d, s = i.ops
    sz = opsize(i.ops)
    T, b = TYPES[sz], BITS[sz]
    if not live:
        return ""
    return f"{{{T} r=({T})(({rd(d, sz)})&({rd(s, sz)}));{_logic_flags(live, b)}}}"


def e_adc(i: Ins, live: int) -> str:
    d, s = i.ops
    sz = opsize(i.ops)
    T, b = TYPES[sz], BITS[sz]
    f = fl_zsp(live, b)
    if live & C:
        f += f"cf=(w>>{b})&1;"
    if live & O:
        f += f"of=(((a^r)&(b^r))>>{b - 1})&1;"
    if live & A:
        f += "af=((a^b^r)>>4)&1;"
    return f"{{{T} a={rd(d, sz)},b={rd(s, sz)};u64 w=(u64)a+b+cf;{T} r=({T})w;{wr(d, 'r')}{f}}}"


def e_sbb(i: Ins, live: int) -> str:
    d, s = i.ops
    sz = opsize(i.ops)
    T, b = TYPES[sz], BITS[sz]
    f = fl_zsp(live, b)
    if live & C:
        f += "cf=((u64)a<(u64)b+ci);"
    if live & O:
        f += f"of=(((a^b)&(a^r))>>{b - 1})&1;"
    if live & A:
        f += "af=((a^b^r)>>4)&1;"
    return f"{{{T} a={rd(d, sz)},b={rd(s, sz)};u32 ci=cf;{T} r=({T})(a-b-ci);{wr(d, 'r')}{f}}}"


def e_inc(i: Ins, live: int) -> str:
    d = i.ops[0]
    sz = opsize(i.ops)
    T, b = TYPES[sz], BITS[sz]
    f = fl_zsp(live, b)
    if live & O:
        f += f"of=(r==(({T})1<<{b - 1}));"
    if live & A:
        f += "af=(r&0xf)==0;"
    return f"{{{T} r=({T})({rd(d, sz)}+1);{wr(d, 'r')}{f}}}"


def e_dec(i: Ins, live: int) -> str:
    d = i.ops[0]
    sz = opsize(i.ops)
    T, b = TYPES[sz], BITS[sz]
    f = fl_zsp(live, b)
    if live & O:
        f += f"of=(r==({T})(({T})1<<{b - 1})-1);"
    if live & A:
        f += "af=(r&0xf)==0xf;"
    return f"{{{T} r=({T})({rd(d, sz)}-1);{wr(d, 'r')}{f}}}"


def e_neg(i: Ins, live: int) -> str:
    d = i.ops[0]
    sz = opsize(i.ops)
    T, b = TYPES[sz], BITS[sz]
    f = fl_zsp(live, b)
    if live & C:
        f += "cf=(a!=0);"
    if live & O:
        f += f"of=(a==(({T})1<<{b - 1}));"
    if live & A:
        f += "af=(a&0xf)!=0;"
    return f"{{{T} a={rd(d, sz)};{T} r=({T})(0-a);{wr(d, 'r')}{f}}}"


def e_not(i: Ins, live: int) -> str:
    d = i.ops[0]
    sz = opsize(i.ops)
    T = TYPES[sz]
    return f"{{{T} r=({T})~({rd(d, sz)});{wr(d, 'r')}}}"


def _count(cnt: Op) -> tuple[str, bool]:
    if cnt.kind == "imm":
        return f"{cnt.val & 31}u", True
    return "(ecx&31u)", False


def e_shift(kind: str):
    def fn(i: Ins, live: int) -> str:
        d = i.ops[0]
        cnt = i.ops[1] if len(i.ops) > 1 else Op("imm", val=1)
        sz = d.size or 4
        T, ST, b = TYPES[sz], STYPES[sz], BITS[sz]
        n, is_imm = _count(cnt)
        if is_imm and cnt.val & 31 == 0:
            return ""
        f = fl_zsp(live, b)
        if kind == "shl":
            body = f"{T} r=({T})((u32)a<<n);"
            if live & C:
                f += f"cf=(n<={b})?(((u64)a>>({b}-n))&1):0;"
            if live & O:
                f += f"if(n==1)of=((r>>{b - 1})&1)^(((u64)a>>{b - 1})&1);"
        elif kind == "shr":
            body = f"{T} r=({T})((u32)a>>n);"
            if live & C:
                f += "cf=(n<=32)?((a>>(n-1))&1):0;"
            if live & O:
                f += f"if(n==1)of=(a>>{b - 1})&1;"
        else:  # sar
            body = f"{T} r=({T})((i32)({ST})a>>n);"
            if live & C:
                f += f"cf=((i32)({ST})a>>(n-1))&1;"
            if live & O:
                f += "if(n==1)of=0;"
        return f"{{u32 n={n};if(n){{{T} a={rd(d, sz)};{body}{wr(d, 'r')}{f}}}}}"
    return fn


def e_rot(kind: str):
    def fn(i: Ins, live: int) -> str:
        d = i.ops[0]
        cnt = i.ops[1] if len(i.ops) > 1 else Op("imm", val=1)
        sz = d.size or 4
        T, b = TYPES[sz], BITS[sz]
        n, is_imm = _count(cnt)
        if is_imm and cnt.val & 31 == 0:
            return ""
        f = ""
        if kind == "rol":
            body = f"{T} r=k?({T})(((u32)a<<k)|((u32)a>>({b}-k))):a;"
            if live & C:
                f += "cf=r&1;"
            if live & O:
                f += f"if(n==1)of=((r>>{b - 1})&1)^(r&1);"
        else:
            body = f"{T} r=k?({T})(((u32)a>>k)|((u32)a<<({b}-k))):a;"
            if live & C:
                f += f"cf=(r>>{b - 1})&1;"
            if live & O:
                f += f"if(n==1)of=((r>>{b - 1})^(r>>{b - 2}))&1;"
        return f"{{u32 n={n};if(n){{u32 k=n%{b}u;{T} a={rd(d, sz)};{body}{wr(d, 'r')}{f}}}}}"
    return fn


def e_rcl(i: Ins, live: int) -> str:
    d = i.ops[0]
    cnt = i.ops[1] if len(i.ops) > 1 else Op("imm", val=1)
    sz = d.size or 4
    T, b = TYPES[sz], BITS[sz]
    n, _ = _count(cnt)
    return (f"{{u32 n=({n})%{b + 1}u;{T} r={rd(d, sz)};u32 cy=cf;while(n--){{u32 t=(r>>{b - 1})&1;"
            f"r=({T})((r<<1)|cy);cy=t;}}{wr(d, 'r')}cf=cy;of=((r>>{b - 1})&1)^cy;}}")


def e_rcr(i: Ins, live: int) -> str:
    d = i.ops[0]
    cnt = i.ops[1] if len(i.ops) > 1 else Op("imm", val=1)
    sz = d.size or 4
    T, b = TYPES[sz], BITS[sz]
    n, _ = _count(cnt)
    return (f"{{u32 n=({n})%{b + 1}u;{T} r={rd(d, sz)};u32 cy=cf;while(n--){{u32 t=r&1;"
            f"r=({T})((r>>1)|((u32)cy<<{b - 1}));cy=t;}}{wr(d, 'r')}cf=cy;of=((r>>{b - 1})^(r>>{b - 2}))&1;}}")


def e_shld(i: Ins, live: int) -> str:
    d, s, cnt = i.ops
    if opsize(i.ops) != 4:
        raise Unsupported("shld16")
    n, _ = _count(cnt)
    f = fl_zsp(live, 32)
    if live & C:
        f += "cf=(a>>(32-n))&1;"
    if live & O:
        f += "if(n==1)of=((a^r)>>31)&1;"
    return f"{{u32 n={n};if(n){{u32 a={rd(d)};u32 b={rd(s)};u32 r=(a<<n)|(b>>(32-n));{wr(d, 'r')}{f}}}}}"


def e_shrd(i: Ins, live: int) -> str:
    d, s, cnt = i.ops
    if opsize(i.ops) != 4:
        raise Unsupported("shrd16")
    n, _ = _count(cnt)
    f = fl_zsp(live, 32)
    if live & C:
        f += "cf=(a>>(n-1))&1;"
    if live & O:
        f += "if(n==1)of=((a^r)>>31)&1;"
    return f"{{u32 n={n};if(n){{u32 a={rd(d)};u32 b={rd(s)};u32 r=(a>>n)|(b<<(32-n));{wr(d, 'r')}{f}}}}}"


def e_imul(i: Ins, live: int) -> str:
    ops = i.ops
    if len(ops) == 1:
        sz = ops[0].size or 4
        if sz == 4:
            f = "cf=of=(w!=(i64)(i32)w);" if live & (C | O) else ""
            return f"{{i64 w=(i64)(i32)eax*(i64)(i32)({rd(ops[0])});eax=(u32)w;edx=(u32)((u64)w>>32);{f}}}"
        if sz == 2:
            f = "cf=of=(w!=(i32)(i16)w);" if live & (C | O) else ""
            return (f"{{i32 w=(i32)(i16)eax*(i32)(i16)({rd(ops[0])});eax=(eax&0xffff0000u)|(u16)w;"
                    f"edx=(edx&0xffff0000u)|(u16)(w>>16);{f}}}")
        f = "cf=of=(w!=(i16)(i8)w);" if live & (C | O) else ""
        return f"{{i16 w=(i16)(i8)eax*(i16)(i8)({rd(ops[0])});eax=(eax&0xffff0000u)|(u16)w;{f}}}"
    d = ops[0]
    a = ops[1] if len(ops) == 3 else ops[0]
    b = ops[2] if len(ops) == 3 else ops[1]
    sz = d.size
    if sz == 4:
        f = "cf=of=(w!=(i64)(i32)w);" if live & (C | O) else ""
        bx = f"(i32)({rd(b, 4)})"
        return f"{{i64 w=(i64)(i32)({rd(a)})*(i64){bx};{wr(d, '(u32)w')}{f}}}"
    f = "cf=of=(w!=(i32)(i16)w);" if live & (C | O) else ""
    return f"{{i32 w=(i32)(i16)({rd(a, 2)})*(i32)(i16)({rd(b, 2)});{wr(d, '(u16)w')}{f}}}"


def e_mul(i: Ins, live: int) -> str:
    s = i.ops[0]
    sz = s.size or 4
    if sz == 4:
        f = "cf=of=(edx!=0);" if live & (C | O) else ""
        return f"{{u64 w=(u64)eax*(u64)({rd(s)});eax=(u32)w;edx=(u32)(w>>32);{f}}}"
    if sz == 2:
        f = "cf=of=((w>>16)!=0);" if live & (C | O) else ""
        return f"{{u32 w=(u32)(u16)eax*(u32)({rd(s)});eax=(eax&0xffff0000u)|(u16)w;edx=(edx&0xffff0000u)|(u16)(w>>16);{f}}}"
    f = "cf=of=((w>>8)!=0);" if live & (C | O) else ""
    return f"{{u32 w=(u32)(u8)eax*(u32)({rd(s)});eax=(eax&0xffff0000u)|(u16)w;{f}}}"


def e_div(i: Ins, live: int) -> str:
    s = i.ops[0]
    sz = s.size or 4
    a = hx(i.addr)
    if sz == 4:
        return (f"{{u32 d={rd(s)};if(!d)rt_trap(c,{a},\"div0\");u64 n=((u64)edx<<32)|eax;u64 q=n/d;"
                f"if(q>>32)rt_trap(c,{a},\"div ovf\");edx=(u32)(n%d);eax=(u32)q;}}")
    if sz == 2:
        return (f"{{u32 d={rd(s)};if(!d)rt_trap(c,{a},\"div0\");u32 n=((u32)(u16)edx<<16)|(u16)eax;"
                f"eax=(eax&0xffff0000u)|(u16)(n/d);edx=(edx&0xffff0000u)|(u16)(n%d);}}")
    return (f"{{u32 d={rd(s)};if(!d)rt_trap(c,{a},\"div0\");u32 n=(u16)eax;"
            f"eax=(eax&0xffff0000u)|(u8)(n/d)|((u32)(u8)(n%d)<<8);}}")


def e_idiv(i: Ins, live: int) -> str:
    s = i.ops[0]
    sz = s.size or 4
    a = hx(i.addr)
    if sz == 4:
        return (f"{{i32 d=(i32)({rd(s)});if(!d)rt_trap(c,{a},\"idiv0\");i64 n=(i64)(((u64)edx<<32)|eax);"
                f"i64 q=(d==-1)?(i64)(0-(u64)n):n/d;if(q!=(i64)(i32)q)rt_trap(c,{a},\"idiv ovf\");"
                f"edx=(d==-1)?0u:(u32)(n%d);eax=(u32)q;}}")
    if sz == 2:
        return (f"{{i32 d=(i16)({rd(s)});if(!d)rt_trap(c,{a},\"idiv0\");i32 n=(i32)(((u32)(u16)edx<<16)|(u16)eax);"
                f"eax=(eax&0xffff0000u)|(u16)(n/d);edx=(edx&0xffff0000u)|(u16)(n%d);}}")
    return (f"{{i32 d=(i8)({rd(s)});if(!d)rt_trap(c,{a},\"idiv0\");i32 n=(i16)eax;"
            f"eax=(eax&0xffff0000u)|(u8)(n/d)|((u32)(u8)(n%d)<<8);}}")


def e_mov(i: Ins, live: int) -> str:
    d, s = i.ops
    if s.kind == "seg" or d.kind == "seg":
        raise Unsupported("segment mov")
    sz = d.size or s.size or 4
    return wr(d._replace(size=sz) if d.kind == "mem" else d, rd(s, sz))


def e_movzx(i: Ins, live: int) -> str:
    d, s = i.ops
    return wr(d, f"(u32)({rd(s, s.size)})")


def e_movsx(i: Ins, live: int) -> str:
    d, s = i.ops
    return wr(d, f"(u32)(i32)({STYPES[s.size]})({rd(s, s.size)})")


def e_lea(i: Ins, live: int) -> str:
    d, s = i.ops
    return wr(d, f"(u32)({addr(s._replace(seg=''))})")


def e_push(i: Ins, live: int) -> str:
    s = i.ops[0]
    sz = s.size if s.kind in ("r16",) or (s.kind == "mem" and s.size) else 4
    if sz == 2:
        return f"{{u16 v={rd(s, 2)};esp-=2;R16(esp)=v;}}"
    return f"{{u32 v={rd(s, 4)};esp-=4;R32(esp)=v;}}"


def e_pop(i: Ins, live: int) -> str:
    d = i.ops[0]
    if d.kind == "r32" and d.reg == "esp":
        return "esp=R32(esp);"
    sz = d.size or 4
    if sz == 2:
        return f"{{u16 v=R16(esp);esp+=2;{wr(d, 'v')}}}"
    return f"{{u32 v=R32(esp);esp+=4;{wr(d, 'v')}}}"


def e_leave(i: Ins, live: int) -> str:
    return "esp=ebp;ebp=R32(esp);esp+=4;"


def e_xchg(i: Ins, live: int) -> str:
    d, s = i.ops
    sz = opsize(i.ops)
    T = TYPES[sz]
    if d == s:
        return ""
    return f"{{{T} a={rd(d, sz)},b={rd(s, sz)};{wr(d, 'b')}{wr(s, 'a')}}}"


def e_xadd(i: Ins, live: int) -> str:
    d, s = i.ops
    sz = opsize(i.ops)
    T, b = TYPES[sz], BITS[sz]
    f = fl_zsp(live, b)
    if live & C:
        f += "cf=r<a;"
    if live & O:
        f += f"of=(((a^r)&(b^r))>>{b - 1})&1;"
    if live & A:
        f += "af=((a^b^r)>>4)&1;"
    return f"{{{T} a={rd(d, sz)},b={rd(s, sz)};{T} r=({T})(a+b);{wr(s, 'a')}{wr(d, 'r')}{f}}}"


def e_cmpxchg(i: Ins, live: int) -> str:
    d, s = i.ops
    sz = opsize(i.ops)
    T, b = TYPES[sz], BITS[sz]
    acc = {4: Op("r32", 4, "eax"), 2: Op("r16", 2, "eax"), 1: Op("r8", 1, "eax")}[sz]
    f = fl_zsp(live, b)
    if live & C:
        f += "cf=a<b;"
    if live & O:
        f += f"of=(((a^b)&(a^r))>>{b - 1})&1;"
    if live & A:
        f += "af=((a^b^r)>>4)&1;"
    return (f"{{{T} a={rd(acc)},b={rd(d, sz)};{T} r=({T})(a-b);if(a==b){{{wr(d, rd(s, sz))}}}"
            f"else{{{wr(acc, 'b')}}}{f}}}")


def e_cmpxchg8b(i: Ins, live: int) -> str:
    m = i.ops[0]
    a = addr(m)
    z = "zf=1;" if live & Z else ""
    nz = "zf=0;" if live & Z else ""
    return (f"{{u64 q=R64({a});if(q==(((u64)edx<<32)|eax)){{R64({a})=((u64)ecx<<32)|ebx;{z}}}"
            f"else{{eax=(u32)q;edx=(u32)(q>>32);{nz}}}}}")


def e_bswap(i: Ins, live: int) -> str:
    d = i.ops[0]
    return f"{d.reg}=__builtin_bswap32({d.reg});"


def e_bt(kind: str):
    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        sz = d.size or s.size or 4
        T, b = TYPES[sz], BITS[sz]
        upd = {"bt": "", "bts": "|", "btr": "&~", "btc": "^"}[kind]
        fl = "cf=(v>>k)&1;" if live & C else ""
        if d.kind == "mem" and s.kind != "imm":
            base = addr(d)
            w = "" if not upd else f"R{b}(ad)=({T})(v{upd}(({T})1<<k));"
            return (f"{{i32 ix=(i32)({rd(s, sz)});u32 ad=({base})+(u32)((ix>>{'5' if sz == 4 else '4'})*{sz});"
                    f"u32 k=(u32)ix&{b - 1}u;{T} v=R{b}(ad);{fl}{w}}}")
        w = "" if not upd else wr(d, f"({T})(v{upd}(({T})1<<k))")
        return f"{{u32 k=({rd(s, sz)})&{b - 1}u;{T} v={rd(d, sz)};{fl}{w}}}"
    return fn


def e_bsf(i: Ins, live: int) -> str:
    d, s = i.ops
    sz = d.size or 4
    z = "zf=(v==0);" if live & Z else ""
    return f"{{u32 v={rd(s, sz)};{z}if(v){{{wr(d, '(u32)__builtin_ctz(v)')}}}}}"


def e_bsr(i: Ins, live: int) -> str:
    d, s = i.ops
    sz = d.size or 4
    z = "zf=(v==0);" if live & Z else ""
    return f"{{u32 v={rd(s, sz)};{z}if(v){{{wr(d, '(u32)(31-__builtin_clz(v))')}}}}}"


def e_cdq(i: Ins, live: int) -> str:
    return "edx=(u32)((i32)eax>>31);"


def e_cwde(i: Ins, live: int) -> str:
    return "eax=(u32)(i32)(i16)eax;"


def e_cbw(i: Ins, live: int) -> str:
    return "eax=(eax&0xffff0000u)|(u16)(i16)(i8)eax;"


def e_cwd(i: Ins, live: int) -> str:
    return "edx=(edx&0xffff0000u)|(u16)((i32)(i16)eax>>31);"


def e_setcc(cc: str):
    def fn(i: Ins, live: int) -> str:
        return wr(i.ops[0]._replace(size=1), f"({CONDS[cc][0]})?1:0")
    return fn


def e_cmov(cc: str):
    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        sz = d.size or 4
        return f"if({CONDS[cc][0]}){{{wr(d, rd(s, sz))}}}"
    return fn


def e_nop(i: Ins, live: int) -> str:
    return ""


def e_string(i: Ins, live: int) -> str:
    """rep/repne string operations, always through a helper-free inline loop."""
    mn = i.mn
    sz = {"b": 1, "w": 2, "d": 4}[mn[-1]]
    T, b = TYPES[sz], BITS[sz]
    step = f"(c->df?(u32)-{sz}:{sz}u)"
    if mn.startswith("movs"):
        one = f"R{b}(edi)=R{b}(esi);esi+=st;edi+=st;"
    elif mn.startswith("stos"):
        one = f"R{b}(edi)=({T})eax;edi+=st;"
    elif mn.startswith("lods"):
        acc = {4: Op("r32", 4, "eax"), 2: Op("r16", 2, "eax"), 1: Op("r8", 1, "eax")}[sz]
        one = f"{wr(acc, f'R{b}(esi)')}esi+=st;"
    elif mn.startswith("scas"):
        one = f"{{{T} a=({T})eax,b=R{b}(edi);{T} r=({T})(a-b);edi+=st;zf=(r==0);sf=(r>>{b - 1})&1;cf=a<b;of=(((a^b)&(a^r))>>{b - 1})&1;pf=PAR(r);af=((a^b^r)>>4)&1;}}"
    elif mn.startswith("cmps"):
        one = f"{{{T} a=R{b}(esi),b=R{b}(edi);{T} r=({T})(a-b);esi+=st;edi+=st;zf=(r==0);sf=(r>>{b - 1})&1;cf=a<b;of=(((a^b)&(a^r))>>{b - 1})&1;pf=PAR(r);af=((a^b^r)>>4)&1;}}"
    else:
        raise Unsupported(mn)
    if not i.rep:
        return f"{{u32 st={step};{one}}}"
    if mn.startswith(("scas", "cmps")):
        brk = "if(!zf)break;" if i.rep in ("rep", "repe") else "if(zf)break;"
        return f"{{u32 st={step};while(ecx){{{one}ecx--;{brk}}}}}"
    return f"{{u32 st={step};while(ecx){{{one}ecx--;}}}}"


# ---------- SSE ----------

def X(n: int) -> str:
    return f"X({n})"


def vsrc(o: Op) -> str:
    if o.kind == "x":
        return X(o.val)
    if o.kind == "mem":
        return f"LDV({addr(o)})"
    raise Unsupported(f"vsrc {o.kind}")


def f32(o: Op) -> str:
    if o.kind == "x":
        return f"X({o.val}).f[0]"
    if o.kind == "mem":
        return f"RF({addr(o)})"
    raise Unsupported(f"f32 {o.kind}")


def f64(o: Op) -> str:
    if o.kind == "x":
        return f"X({o.val}).d[0]"
    if o.kind == "mem":
        return f"RD({addr(o)})"
    raise Unsupported(f"f64 {o.kind}")


def e_pack(name: str):
    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        return f"X({d.val})=op_{name}(X({d.val}),{vsrc(s)});"
    return fn


def e_unary(name: str):
    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        return f"X({d.val})=op_{name}({vsrc(s)});"
    return fn


def e_imm3(name: str):
    def fn(i: Ins, live: int) -> str:
        d, s, im = i.ops
        return f"X({d.val})=op_{name}(X({d.val}),{vsrc(s)},{im.val & 255}u);"
    return fn


def e_imm2(name: str):
    def fn(i: Ins, live: int) -> str:
        d, s, im = i.ops
        return f"X({d.val})=op_{name}({vsrc(s)},{im.val & 255}u);"
    return fn


def e_vshift(name: str):
    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        cnt = f"{s.val & 255}u" if s.kind == "imm" else f"({vsrc(s)}).q[0]"
        return f"X({d.val})=op_{name}(X({d.val}),{cnt});"
    return fn


def e_sca(op: str, ty: str):
    fld = "f" if ty == "ss" else "d"
    src = f32 if ty == "ss" else f64

    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        return f"X({d.val}).{fld}[0]{op}={src(s)};"
    return fn


def e_scafn(fnname: str, ty: str, binary: bool = True):
    fld = "f" if ty == "ss" else "d"
    src = f32 if ty == "ss" else f64

    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        if binary:
            return f"X({d.val}).{fld}[0]={fnname}(X({d.val}).{fld}[0],{src(s)});"
        return f"X({d.val}).{fld}[0]={fnname}({src(s)});"
    return fn


CMP_PRED = {
    "eq": "(a==b)", "lt": "(a<b)", "le": "(a<=b)", "unord": "(a!=a||b!=b)",
    "neq": "(a!=b)", "nlt": "!(a<b)", "nle": "!(a<=b)", "ord": "(a==a&&b==b)",
}


def e_cmpsca(pred: str, ty: str):
    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        if ty == "ss":
            return f"{{float a=X({d.val}).f[0],b={f32(s)};X({d.val}).u[0]={CMP_PRED[pred]}?~0u:0u;}}"
        return f"{{double a=X({d.val}).d[0],b={f64(s)};X({d.val}).q[0]={CMP_PRED[pred]}?~0ull:0ull;}}"
    return fn


def e_cmppk(pred: str, ty: str):
    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        if ty == "ps":
            return (f"{{v128 A=X({d.val}),B={vsrc(s)};for(int k=0;k<4;k++){{float a=A.f[k],b=B.f[k];"
                    f"X({d.val}).u[k]={CMP_PRED[pred]}?~0u:0u;}}}}")
        return (f"{{v128 A=X({d.val}),B={vsrc(s)};for(int k=0;k<2;k++){{double a=A.d[k],b=B.d[k];"
                f"X({d.val}).q[k]={CMP_PRED[pred]}?~0ull:0ull;}}}}")
    return fn


def e_comi(ty: str):
    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        T, f, src = ("float", "f", f32) if ty == "ss" else ("double", "d", f64)
        return (f"{{{T} a=X({d.val}).{f}[0],b={src(s)};if(a!=a||b!=b){{zf=pf=cf=1;}}else{{zf=(a==b);pf=0;cf=(a<b);}}"
                "of=sf=af=0;}")
    return fn


def e_movss(i: Ins, live: int) -> str:
    d, s = i.ops
    if d.kind == "x" and s.kind == "x":
        return f"X({d.val}).u[0]=X({s.val}).u[0];"
    if d.kind == "x":
        return f"{{u32 v=R32({addr(s)});X({d.val}).q[1]=0;X({d.val}).q[0]=v;}}"
    return f"R32({addr(d)})=X({s.val}).u[0];"


def e_movsd(i: Ins, live: int) -> str:
    d, s = i.ops
    if d.kind == "x" and s.kind == "x":
        return f"X({d.val}).q[0]=X({s.val}).q[0];"
    if d.kind == "x":
        return f"{{u64 v=R64({addr(s)});X({d.val}).q[0]=v;X({d.val}).q[1]=0;}}"
    return f"R64({addr(d)})=X({s.val}).q[0];"


def e_movv(i: Ins, live: int) -> str:
    d, s = i.ops
    if d.kind == "x":
        return f"X({d.val})={vsrc(s)};"
    return f"STV({addr(d)},X({s.val}));"


def e_movd(i: Ins, live: int) -> str:
    d, s = i.ops
    if d.kind == "x":
        return f"{{u32 v={rd(s, 4)};X({d.val}).q[1]=0;X({d.val}).q[0]=v;}}"
    return wr(d, f"X({s.val}).u[0]")


def e_movq(i: Ins, live: int) -> str:
    d, s = i.ops
    if d.kind == "x":
        if s.kind == "x":
            return f"{{u64 v=X({s.val}).q[0];X({d.val}).q[0]=v;X({d.val}).q[1]=0;}}"
        return f"{{u64 v=R64({addr(s)});X({d.val}).q[0]=v;X({d.val}).q[1]=0;}}"
    return f"R64({addr(d)})=X({s.val}).q[0];"


def e_movlp(i: Ins, live: int) -> str:
    d, s = i.ops
    if d.kind == "x":
        return f"X({d.val}).q[0]=R64({addr(s)});"
    return f"R64({addr(d)})=X({s.val}).q[0];"


def e_movhp(i: Ins, live: int) -> str:
    d, s = i.ops
    if d.kind == "x":
        return f"X({d.val}).q[1]=R64({addr(s)});"
    return f"R64({addr(d)})=X({s.val}).q[1];"


def e_movhlps(i: Ins, live: int) -> str:
    d, s = i.ops
    return f"X({d.val}).q[0]=X({s.val}).q[1];"


def e_movlhps(i: Ins, live: int) -> str:
    d, s = i.ops
    return f"X({d.val}).q[1]=X({s.val}).q[0];"


def e_movmsk(name: str):
    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        return wr(d, f"op_{name}(X({s.val}))")
    return fn


def e_cvtsi2ss(i: Ins, live: int) -> str:
    d, s = i.ops
    return f"X({d.val}).f[0]=(float)(i32)({rd(s, 4)});"


def e_cvtsi2sd(i: Ins, live: int) -> str:
    d, s = i.ops
    return f"X({d.val}).d[0]=(double)(i32)({rd(s, 4)});"


def e_cvt2si(fnname: str, src):
    def fn(i: Ins, live: int) -> str:
        d, s = i.ops
        return wr(d, f"{fnname}({src(s)})")
    return fn


def e_cvtss2sd(i: Ins, live: int) -> str:
    d, s = i.ops
    return f"X({d.val}).d[0]=(double){f32(s)};"


def e_cvtsd2ss(i: Ins, live: int) -> str:
    d, s = i.ops
    return f"X({d.val}).f[0]=(float){f64(s)};"


def e_pinsrw(i: Ins, live: int) -> str:
    d, s, im = i.ops
    src = rd(s._replace(size=2) if s.kind == "mem" else s, 2)
    return f"X({d.val}).w[{im.val & 7}]=(u16)({src});"


def e_pextrw(i: Ins, live: int) -> str:
    d, s, im = i.ops
    return wr(d, f"(u32)X({s.val}).w[{im.val & 7}]")


def e_ldmxcsr(i: Ins, live: int) -> str:
    return f"rt_ldmxcsr(c,R32({addr(i.ops[0])}));"


def e_stmxcsr(i: Ins, live: int) -> str:
    return f"R32({addr(i.ops[0])})=c->mxcsr;"


# ---------- x87 ----------

def _fmem(o: Op) -> str:
    a = addr(o)
    if o.size == 4:
        return f"(double)RF({a})"
    if o.size == 8:
        return f"RD({a})"
    if o.size == 10:
        return f"rt_ld80(M+(u32)({a}))"
    raise Unsupported(f"fmem size {o.size}")


def _fint(o: Op) -> str:
    a = addr(o)
    if o.size == 2:
        return f"(double)(i16)R16({a})"
    if o.size == 4:
        return f"(double)(i32)R32({a})"
    if o.size == 8:
        return f"(double)(i64)R64({a})"
    raise Unsupported(f"fint size {o.size}")


def e_fld(i: Ins, live: int) -> str:
    s = i.ops[0]
    if s.kind == "st":
        return f"{{double v=ST({s.val});FPUSH(v);}}"
    return f"FPUSH({_fmem(s)});"


def _fstore(o: Op, val: str) -> str:
    a = addr(o)
    if o.size == 4:
        return f"RF({a})=(float){val};"
    if o.size == 8:
        return f"RD({a})={val};"
    if o.size == 10:
        return f"rt_st80(M+(u32)({a}),{val});"
    raise Unsupported(f"fstore size {o.size}")


def e_fst(pop: bool):
    def fn(i: Ins, live: int) -> str:
        d = i.ops[0]
        p = "FPOP();" if pop else ""
        if d.kind == "st":
            return f"ST({d.val})=ST(0);{p}"
        return f"{_fstore(d, 'ST(0)')}{p}"
    return fn


def e_fild(i: Ins, live: int) -> str:
    return f"FPUSH({_fint(i.ops[0])});"


def _fistore(o: Op, val: str) -> str:
    a = addr(o)
    if o.size == 2:
        return f"R16({a})=rt_f2i16({val});"
    if o.size == 4:
        return f"R32({a})=rt_f2i32({val});"
    if o.size == 8:
        return f"R64({a})=rt_f2i64({val});"
    raise Unsupported(f"fist size {o.size}")


def e_fist(pop: bool):
    def fn(i: Ins, live: int) -> str:
        p = "FPOP();" if pop else ""
        return f"{_fistore(i.ops[0], 'rt_frnd(c,ST(0))')}{p}"
    return fn


def e_fisttp(i: Ins, live: int) -> str:
    return f"{_fistore(i.ops[0], 'trunc(ST(0))')}FPOP();"


FARITH = {"add": "a+b", "sub": "a-b", "subr": "b-a", "mul": "a*b", "div": "a/b", "divr": "b/a"}


def e_farith(kind: str, pop: bool = False):
    expr = FARITH[kind]

    def fn(i: Ins, live: int) -> str:
        ops = i.ops
        if len(ops) == 1 and ops[0].kind == "mem":
            return f"{{double a=ST(0),b={_fmem(ops[0])};ST(0)={expr};}}"
        if not ops:
            ds, ss = 1, 0
        elif len(ops) == 1:
            ds, ss = (ops[0].val, 0) if pop else (0, ops[0].val)
        else:
            ds, ss = ops[0].val, ops[1].val
        p = "FPOP();" if pop else ""
        return f"{{double a=ST({ds}),b=ST({ss});ST({ds})={expr};{p}}}"
    return fn


def e_fiarith(kind: str):
    expr = FARITH[kind]

    def fn(i: Ins, live: int) -> str:
        return f"{{double a=ST(0),b={_fint(i.ops[0])};ST(0)={expr};}}"
    return fn


def e_fxch(i: Ins, live: int) -> str:
    n = i.ops[0].val if i.ops else 1
    return f"{{double t=ST(0);ST(0)=ST({n});ST({n})=t;}}"


def e_fconst(v: str):
    def fn(i: Ins, live: int) -> str:
        return f"FPUSH({v});"
    return fn


def e_funary(expr: str):
    def fn(i: Ins, live: int) -> str:
        return f"ST(0)={expr.format('ST(0)')};"
    return fn


def e_fcomi(pop: bool):
    def fn(i: Ins, live: int) -> str:
        n = i.ops[-1].val if i.ops else 1
        p = "FPOP();" if pop else ""
        return (f"{{double a=ST(0),b=ST({n});if(a!=a||b!=b){{zf=pf=cf=1;}}else{{zf=(a==b);pf=0;cf=(a<b);}}"
                f"of=sf=af=0;{p}}}")
    return fn


def e_fcom(pops: int, unordered_int: bool = False):
    def fn(i: Ins, live: int) -> str:
        if i.ops and i.ops[0].kind == "mem":
            b = _fint(i.ops[0]) if unordered_int else _fmem(i.ops[0])
        else:
            b = f"ST({i.ops[0].val if i.ops else 1})"
        return f"rt_fcom(c,ST(0),{b});" + "FPOP();" * pops
    return fn


def e_fnstsw(i: Ins, live: int) -> str:
    d = i.ops[0]
    if d.kind == "mem":
        return f"R16({addr(d)})=rt_fsw(c);"
    return wr(d, "rt_fsw(c)")


def e_fnstcw(i: Ins, live: int) -> str:
    return f"R16({addr(i.ops[0])})=c->fcw;"


def e_fldcw(i: Ins, live: int) -> str:
    return f"c->fcw=R16({addr(i.ops[0])});"


def e_sahf(i: Ins, live: int) -> str:
    return "{u32 h=(eax>>8)&0xff;sf=(h>>7)&1;zf=(h>>6)&1;af=(h>>4)&1;pf=(h>>2)&1;cf=h&1;}"


def e_lahf(i: Ins, live: int) -> str:
    return "eax=(eax&0xffff00ffu)|(((sf<<7)|(zf<<6)|(af<<4)|(pf<<2)|2u|cf)<<8);"


def e_pushfd(i: Ins, live: int) -> str:
    return ("esp-=4;R32(esp)=(of<<11)|(c->df<<10)|(sf<<7)|(zf<<6)|(af<<4)|(pf<<2)|2u|cf;")


def e_popfd(i: Ins, live: int) -> str:
    return ("{u32 v=R32(esp);esp+=4;of=(v>>11)&1;c->df=(v>>10)&1;sf=(v>>7)&1;zf=(v>>6)&1;af=(v>>4)&1;pf=(v>>2)&1;cf=v&1;}")


def e_static(text: str):
    def fn(i: Ins, live: int) -> str:
        return text
    return fn


def e_cpuid(i: Ins, live: int) -> str:
    return "c->eax=eax;c->ecx=ecx;rt_cpuid(c);eax=c->eax;ebx=c->ebx;ecx=c->ecx;edx=c->edx;"


EMIT = {
    "add": e_add, "sub": e_sub, "cmp": e_cmp, "and": e_logic("&"), "or": e_logic("|"), "xor": e_logic("^"),
    "test": e_test, "adc": e_adc, "sbb": e_sbb, "inc": e_inc, "dec": e_dec, "neg": e_neg, "not": e_not,
    "shl": e_shift("shl"), "sal": e_shift("shl"), "shr": e_shift("shr"), "sar": e_shift("sar"),
    "rol": e_rot("rol"), "ror": e_rot("ror"), "rcl": e_rcl, "rcr": e_rcr, "shld": e_shld, "shrd": e_shrd,
    "imul": e_imul, "mul": e_mul, "div": e_div, "idiv": e_idiv,
    "mov": e_mov, "movzx": e_movzx, "movsx": e_movsx, "lea": e_lea, "push": e_push, "pop": e_pop,
    "leave": e_leave, "xchg": e_xchg, "xadd": e_xadd, "cmpxchg": e_cmpxchg, "cmpxchg8b": e_cmpxchg8b,
    "bswap": e_bswap, "bt": e_bt("bt"), "bts": e_bt("bts"), "btr": e_bt("btr"), "btc": e_bt("btc"),
    "bsf": e_bsf, "bsr": e_bsr, "cdq": e_cdq, "cwde": e_cwde, "cbw": e_cbw, "cwd": e_cwd,
    "nop": e_nop, "prefetcht0": e_nop, "prefetcht1": e_nop, "prefetcht2": e_nop, "prefetchnta": e_nop,
    "mfence": e_nop, "lfence": e_nop, "sfence": e_nop, "emms": e_nop, "wait": e_nop, "fwait": e_nop,
    "cld": e_static("c->df=0;"), "std": e_static("c->df=1;"),
    "clc": e_static("cf=0;"), "stc": e_static("cf=1;"), "cmc": e_static("cf=!cf;"),
    "sahf": e_sahf, "lahf": e_lahf, "pushfd": e_pushfd, "popfd": e_popfd, "cpuid": e_cpuid,
    "pause": e_static("POLL;"),
    "movsb": e_string, "movsw": e_string, "stosb": e_string, "stosw": e_string, "stosd": e_string,
    "lodsb": e_string, "lodsw": e_string, "lodsd": e_string, "scasb": e_string, "scasw": e_string, "scasd": e_string,
    "cmpsb": e_string, "cmpsw": e_string,
    "movss": e_movss, "movaps": e_movv, "movups": e_movv, "movapd": e_movv, "movupd": e_movv,
    "movdqa": e_movv, "movdqu": e_movv, "movd": e_movd, "movq": e_movq,
    "movlps": e_movlp, "movlpd": e_movlp, "movhps": e_movhp, "movhpd": e_movhp,
    "movhlps": e_movhlps, "movlhps": e_movlhps,
    "movmskps": e_movmsk("movmskps"), "movmskpd": e_movmsk("movmskpd"), "pmovmskb": e_movmsk("pmovmskb"),
    "addss": e_sca("+", "ss"), "subss": e_sca("-", "ss"), "mulss": e_sca("*", "ss"), "divss": e_sca("/", "ss"),
    "addsd": e_sca("+", "sd"), "subsd": e_sca("-", "sd"), "mulsd": e_sca("*", "sd"), "divsd": e_sca("/", "sd"),
    "minss": e_scafn("MINV", "ss"), "maxss": e_scafn("MAXV", "ss"), "minsd": e_scafn("MINV", "sd"), "maxsd": e_scafn("MAXV", "sd"),
    "sqrtss": e_scafn("__builtin_sqrtf", "ss", False), "sqrtsd": e_scafn("__builtin_sqrt", "sd", False),
    "rsqrtss": e_scafn("RSQRTF", "ss", False), "rcpss": e_scafn("RCPF", "ss", False),
    "ucomiss": e_comi("ss"), "comiss": e_comi("ss"), "ucomisd": e_comi("sd"), "comisd": e_comi("sd"),
    "cvtsi2ss": e_cvtsi2ss, "cvtsi2sd": e_cvtsi2sd, "cvtss2sd": e_cvtss2sd, "cvtsd2ss": e_cvtsd2ss,
    "cvttss2si": e_cvt2si("rt_cvtt_f", f32), "cvttsd2si": e_cvt2si("rt_cvtt_d", f64),
    "cvtss2si": e_cvt2si("rt_cvt_f", f32), "cvtsd2si": e_cvt2si("rt_cvt_d", f64),
    "pinsrw": e_pinsrw, "pextrw": e_pextrw, "ldmxcsr": e_ldmxcsr, "stmxcsr": e_stmxcsr,
    "fld": e_fld, "fst": e_fst(False), "fstp": e_fst(True), "fild": e_fild, "fist": e_fist(False), "fistp": e_fist(True),
    "fisttp": e_fisttp, "fxch": e_fxch, "fchs": e_funary("-{}"), "fabs": e_funary("fabs({})"),
    "fsqrt": e_funary("sqrt({})"), "fsin": e_funary("sin({})"), "fcos": e_funary("cos({})"), "frndint": e_funary("rt_frnd(c,{})"),
    "fld1": e_fconst("1.0"), "fldz": e_fconst("0.0"), "fldpi": e_fconst("M_PI"), "fldln2": e_fconst("M_LN2"),
    "fldlg2": e_fconst("0.30102999566398119521"), "fldl2e": e_fconst("M_LOG2E"), "fldl2t": e_fconst("3.32192809488736234787"),
    "fcomi": e_fcomi(False), "fucomi": e_fcomi(False), "fcomip": e_fcomi(True), "fucomip": e_fcomi(True),
    "fcom": e_fcom(0), "fcomp": e_fcom(1), "fcompp": e_fcom(2), "fucom": e_fcom(0), "fucomp": e_fcom(1), "fucompp": e_fcom(2),
    "ficom": e_fcom(0, True), "ficomp": e_fcom(1, True),
    "fnstsw": e_fnstsw, "fnstcw": e_fnstcw, "fldcw": e_fldcw, "fninit": e_static("rt_fninit(c);"),
    "fnclex": e_nop,
}

for _k in ("add", "sub", "subr", "mul", "div", "divr"):
    EMIT[f"f{_k}"] = e_farith(_k)
    EMIT[f"f{_k}p"] = e_farith(_k, True)
    EMIT[f"fi{_k}"] = e_fiarith(_k)

for _cc in CONDS:
    EMIT[f"set{_cc}"] = e_setcc(_cc)
    EMIT[f"cmov{_cc}"] = e_cmov(_cc)

for _n in ("addps subps mulps divps minps maxps andps orps xorps andnps addpd subpd mulpd divpd minpd maxpd andpd orpd "
           "xorpd andnpd pand por pxor pandn paddb paddw paddd paddq psubb psubw psubd psubq psubusw psubusb paddusw "
           "paddusb pmullw pmulhw pmuludq pmaddwd pavgb pavgw pminub pmaxub pminsw pmaxsw psadbw pcmpeqb pcmpeqw pcmpeqd "
           "pcmpgtb pcmpgtw pcmpgtd punpcklbw punpcklwd punpckldq punpcklqdq punpckhbw punpckhwd punpckhdq punpckhqdq "
           "packuswb packsswb packssdw unpcklps unpckhps unpcklpd unpckhpd haddps haddpd phaddd pshufb").split():
    EMIT[_n] = e_pack(_n)
for _n in ("sqrtps sqrtpd rsqrtps rcpps cvtdq2ps cvttps2dq cvtps2dq cvtps2pd cvtpd2ps cvtdq2pd cvttpd2dq").split():
    EMIT[_n] = e_unary(_n)
for _n in ("shufps", "shufpd", "palignr"):
    EMIT[_n] = e_imm3(_n)
for _n in ("pshufd", "pshuflw", "pshufhw"):
    EMIT[_n] = e_imm2(_n)
for _n in "psllw pslld psllq psrlw psrld psrlq psraw psrad pslldq psrldq".split():
    EMIT[_n] = e_vshift(_n)
for _p in CMP_PRED:
    EMIT[f"cmp{_p}ss"] = e_cmpsca(_p, "ss")
    EMIT[f"cmp{_p}sd"] = e_cmpsca(_p, "sd")
    EMIT[f"cmp{_p}ps"] = e_cmppk(_p, "ps")
    EMIT[f"cmp{_p}pd"] = e_cmppk(_p, "pd")


def is_string_form(i: Ins) -> bool:
    return "es:[" in i.text or "ds:[" in i.text


def emit(i: Ins, live: int) -> str:
    mn = i.mn
    if mn == "movsd":
        return e_string(i, live) if is_string_form(i) else e_movsd(i, live)
    if mn == "cmpsd" and is_string_form(i):
        return e_string(i, live)
    fn = EMIT.get(mn)
    if fn is None:
        raise Unsupported(mn)
    return fn(i, live)


def flag_effect(i: Ins) -> tuple[int, int]:
    """(reads, writes) as flag bitmasks. Partial or conditional writes count as reads too."""
    mn = i.mn
    if mn in _ARITH:
        return 0, ALL
    if mn in _LOGIC:
        return 0, ALL
    if mn in ("inc", "dec"):
        return 0, NOZF
    if mn in ("shl", "sal", "shr", "sar", "shld", "shrd"):
        ops = i.ops
        cnt = ops[-1] if len(ops) > 1 else None
        if cnt is None or cnt.kind == "imm":
            if cnt is not None and cnt.val & 31 == 0:
                return 0, 0
            if cnt is None or cnt.val & 31 == 1:
                return 0, ALL & ~A
            return O, ALL & ~A  # OF only changes when the count is 1
        return ALL, ALL & ~A
    if mn in ("rol", "ror"):
        ops = i.ops
        cnt = ops[1] if len(ops) > 1 else None
        if cnt is not None and cnt.kind == "imm" and cnt.val & 31 != 1:
            return (O, C) if cnt.val & 31 else (0, 0)
        return C | O, C | O
    if mn in ("rcl", "rcr"):
        return C, C | O
    if mn in ("imul", "mul"):
        return 0, C | O
    if mn in ("bt", "bts", "btr", "btc"):
        return 0, C
    if mn in ("bsf", "bsr"):
        return 0, Z
    if mn in ("ucomiss", "comiss", "ucomisd", "comisd", "fcomi", "fucomi", "fcomip", "fucomip"):
        return 0, ALL
    if mn in ("clc", "stc"):
        return 0, C
    if mn == "cmc":
        return C, C
    if mn == "sahf":
        return 0, ALL & ~O
    if mn == "lahf":
        return ALL & ~O, 0
    if mn == "pushfd":
        return ALL, 0
    if mn == "popfd":
        return 0, ALL
    if mn in ("adc", "sbb"):
        return C, ALL
    if mn == "cmpxchg8b":
        return Z, Z
    if mn.startswith(("scas", "cmps")) and (mn in ("scasb", "scasw", "scasd", "cmpsb", "cmpsw") or (mn == "cmpsd" and is_string_form(i))):
        return (ALL, ALL) if i.rep else (0, ALL)
    if mn.startswith("set") or mn.startswith("cmov"):
        cc = mn[3:] if mn.startswith("set") else mn[4:]
        if cc in CONDS:
            return CONDS[cc][1], 0
    if mn.startswith("j") and mn[1:] in CONDS:
        return CONDS[mn[1:]][1], 0
    return 0, 0


_ARITH = {"add", "sub", "cmp", "neg", "xadd", "cmpxchg"}
_LOGIC = {"and", "or", "xor", "test"}
