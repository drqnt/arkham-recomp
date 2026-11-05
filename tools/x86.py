"""Parse llvm-objdump Intel-syntax lines into instructions and operands."""
from __future__ import annotations

import re
from functools import lru_cache
from typing import NamedTuple

REG32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
XMM = {f"xmm{i}": i for i in range(8)}
MM = {f"mm{i}": i for i in range(8)}
SEG = {"cs", "ds", "es", "ss", "fs", "gs"}
SIZES = {"byte": 1, "word": 2, "dword": 4, "fword": 6, "qword": 8, "tbyte": 10, "xmmword": 16, "ymmword": 32}

_MEM = re.compile(r"^(?:(byte|word|dword|fword|qword|tbyte|xmmword|ymmword) )?(?:ptr )?(?:(cs|ds|es|ss|fs|gs):)?\[(.*)\]$")
_TERM = re.compile(r"([+-]?)\s*([^+\s-][^+-]*?)\s*(?=[+-]|$)")
_NUM = re.compile(r"^-?(0x[0-9a-fA-F]+|[0-9]+)$")
_LINE = re.compile(r"^\s*([0-9a-f]+):\s+(.*?)\s*$")
_ST = re.compile(r"^st(?:\((\d)\))?$")


class Op(NamedTuple):
    kind: str  # r32 r16 r8 x mm st imm mem seg
    size: int = 0
    reg: str = ""
    high: bool = False
    val: int = 0
    base: str = ""
    index: str = ""
    scale: int = 1
    disp: int = 0
    seg: str = ""


@lru_cache(maxsize=200000)
def parse_op(text: str) -> Op:
    text = text.strip()
    if "<" in text:
        text = text[: text.index("<")].strip()
    if text in XMM:
        return Op("x", 16, val=XMM[text])
    if text in REG32:
        return Op("r32", 4, text)
    if text in REG8L:
        return Op("r8", 1, REG8L[text])
    if text in REG8H:
        return Op("r8", 1, REG8H[text], True)
    if text in REG16:
        return Op("r16", 2, REG16[text])
    if text in MM:
        return Op("mm", 8, val=MM[text])
    if text in SEG:
        return Op("seg", 2, text)
    m = _ST.match(text)
    if m:
        return Op("st", 10, val=int(m.group(1) or 0))
    if _NUM.match(text):
        return Op("imm", 0, val=int(text, 0))
    m = _MEM.match(text)
    if not m:
        raise ValueError(f"operand: {text!r}")
    size, seg, inner = m.groups()
    base = index = ""
    scale = 1
    disp = 0
    for sign, term in _TERM.findall(inner):
        term = term.strip()
        if "*" in term:
            s, r = term.split("*")
            if s.strip() in REG32:
                s, r = r, s
            index, scale = r.strip(), int(s.strip(), 0)
        elif term in REG32:
            if not base:
                base = term
            elif not index:
                index = term
            else:
                raise ValueError(f"mem: {text!r}")
        else:
            v = int(term, 0)
            disp += -v if sign == "-" else v
    return Op("mem", SIZES.get(size, 0), base=base, index=index, scale=scale, disp=disp & 0xFFFFFFFF, seg=seg or "")


class Ins:
    __slots__ = ("addr", "next", "mn", "text", "rep", "lock", "_ops")

    def __init__(self, addr: int, mn: str, text: str, rep: str = "", lock: bool = False):
        self.addr = addr
        self.next = 0
        self.mn = mn
        self.text = text
        self.rep = rep
        self.lock = lock
        self._ops = None

    @property
    def ops(self) -> list[Op]:
        if self._ops is None:
            self._ops = [parse_op(t) for t in split_ops(self.text)]
        return self._ops


def split_ops(text: str) -> list[str]:
    if not text:
        return []
    if "<" in text:
        text = text[: text.index("<")].strip()
    return [p for p in (s.strip() for s in text.split(",")) if p]


PREFIX_ONLY = {"lock", "rep", "repne", "repe", "data16", "addr16", "addr32", "cs", "ds", "es", "ss", "fs", "gs", "notrack"}


def parse_lines(lines) -> list[Ins]:
    """Parse objdump output lines into instructions with .next filled in.
    The final instruction's .next is 0 when no following line exists."""
    out: list[Ins] = []
    lock_at = -1
    prefix = ""
    for line in lines:
        m = _LINE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        body = m.group(2).split("##")[0].rstrip()
        parts = body.split("\t")
        parts = [p.strip() for p in parts if p.strip()]
        if not parts:
            continue
        rep = ""
        lock = False
        if parts[0] in ("rep", "repne", "repe") and len(parts) > 1:
            rep = parts[0]
            parts = parts[1:]
        if parts[0] == "lock" and len(parts) > 1:
            lock = True
            parts = parts[1:]
        mn = parts[0]
        text = parts[1] if len(parts) > 1 else ""
        if mn == "lock" and not text:
            lock_at = addr
            continue
        ins = Ins(addr, mn, text, rep, lock)
        if lock_at >= 0:
            ins.addr = lock_at
            ins.lock = True
            lock_at = -1
        if out:
            out[-1].next = ins.addr
        out.append(ins)
    return out
