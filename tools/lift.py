#!/usr/bin/env python3
"""Static recompiler driver: i386 Mach-O -> C. Reads the user's own installed binary and writes gen/.

Nothing under gen/ or build/ is ever tracked or shared: it is derived from the game."""
from __future__ import annotations

import argparse
import bisect
import collections
import hashlib
import json
import multiprocessing as mp
import os
import pickle
import re
import subprocess
import sys
import time
from array import array

sys.path.insert(0, os.path.dirname(__file__))
import emit as emit_module  # noqa: E402
from emit import ALL, CONDS, Unsupported, emit, flag_effect, hx, rd, opsize, TYPES, STYPES  # noqa: E402
from eh import all_landing_pads  # noqa: E402
from game_path import game_binary, game_bink  # noqa: E402
from rebase_dylib import rebase_dylib  # noqa: E402
from macho import MachO  # noqa: E402
from x86 import Ins, Op, parse_lines  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
GEN = os.path.join(ROOT, "gen")
CONFIG = os.path.join(ROOT, "config")
OBJDUMP = ["xcrun", "llvm-objdump", "-d", "-M", "intel", "--no-show-raw-insn"]
ENV = dict(os.environ, DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer")
SHARD_BITS = 17
IMP_BASE = 0xEF000000
IMP_STEP = 64
TERMINATORS = {"ret", "retn", "hlt", "ud2", "int3", "<unknown>", "jmp", "iretd", "retf", "lret", "ljmp"}


# ---------- disassembly ----------

def ensure_disasm(binary: str, tag: str = "", sections: tuple[str, ...] = ("__text", "google_malloc")):
    """Linear sweep over code sections. Returns the path and a sorted (addrs, offsets) index."""
    os.makedirs(BUILD, exist_ok=True)
    dis = os.path.join(BUILD, f"dis{tag}.txt")
    idx = os.path.join(BUILD, f"dis{tag}.idx")
    if not os.path.exists(idx):
        with open(dis, "wb") as fh:
            for sec in sections:
                subprocess.run(OBJDUMP + [f"--section={sec}", binary], stdout=fh, check=True, env=ENV)
        addrs, offs = array("I"), array("Q")
        pos = 0
        pat = re.compile(rb"^\s*([0-9a-f]+):\s")
        with open(dis, "rb") as fh:
            for line in fh:
                m = pat.match(line)
                if m:
                    addrs.append(int(m.group(1), 16))
                    offs.append(pos)
                pos += len(line)
        with open(idx, "wb") as fh:
            pickle.dump((addrs, offs), fh, protocol=4)
    with open(idx, "rb") as fh:
        addrs, offs = pickle.load(fh)
    return dis, addrs, offs


class World:
    """Everything workers share: image metadata and the disassembly index."""

    def __init__(self, binary: str, extra_roots: set[int], tag: str = "", import_base: int = 0):
        self.binary = binary
        self.tag = tag
        self.m = MachO(binary)
        m = self.m
        code_sections = tuple(n for n in ("__text", "google_malloc") if m.section(n))
        self.dis, self.addrs, self.offs = ensure_disasm(binary, tag, code_sections)
        self.code_ranges = []
        for name in code_sections:
            sec = m.section(name)
            self.code_ranges.append((sec.addr, sec.end))
        gm = m.section("google_malloc")
        # Every exception landing pad is an entry point: the unwinder enters it with restored registers.
        self.landing_pads = all_landing_pads(m)[0] if m.section("__eh_frame") else set()
        hard = set(m.function_starts)
        if m.entry:
            hard.add(m.entry)
        if gm:
            hard.add(gm.addr)
        roots = hard | set(extra_roots) | self.landing_pads
        if not m.function_starts:
            # No linker table in an old dylib: use its symbols and standard prologues for extents, and also start from
            # anything that follows a return or that data points at. Those extra roots never cut a function short.
            prologues, others = self.scan_function_starts()
            hard |= prologues
            roots |= prologues | others
        self.roots = sorted(r for r in roots if self.in_code(r))
        self.rootset = set(self.roots)
        # Function extents come only from the linker's table. An extra root (say, a jump into the middle of a
        # function, or one derived from data decoded as code) gets its own copy and never cuts a real function short.
        self.hard_starts = sorted({r for r in hard if self.in_code(r)})
        # imports
        self.imports: dict[str, int] = {}
        self._import_names: dict[int, str] = {}
        self.bind_at: dict[int, tuple[str, int]] = {}
        defined = m.defined_by_name()
        for b in m.binds:
            if b.ordinal > 0 or b.ordinal < 0 or b.symbol not in defined:
                idx = self.imports.setdefault(b.symbol, import_base + len(self.imports))
                self.bind_at[b.addr] = (b.symbol, b.addend)
            else:
                self.bind_at[b.addr] = (b.symbol, b.addend)
        self.local = defined
        # lazy stubs: "jmp dword ptr [ptr]" (FF 25 imm32), 6 bytes each
        self.stubs: dict[int, tuple[str, int]] = {}
        st = m.section("__symbol_stub")
        for a in range(st.addr, st.end, 6):
            raw = m.bytes_at(a, 6)
            if raw[:2] != b"\xff\x25":
                continue
            ptr = int.from_bytes(raw[2:6], "little")
            if ptr in self.bind_at and self.bind_at[ptr][0] in self.imports:
                self.stubs[a] = ("imp", self.imports[self.bind_at[ptr][0]])
            else:
                self.stubs[a] = ("fn", m.read_u32(ptr))
        self.stub_section = (st.addr, st.end)

    def scan_function_starts(self) -> tuple[set[int], set[int]]:
        m = self.m
        prologues = {a for a in m.defined_symbols() if self.in_code(a)}
        others = set()
        for r in m.rebases:
            off = m.vm_to_file(r)
            if off is not None:
                v = m.read_u32(r)
                if self.in_code(v):
                    others.add(v)
        pat = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*(?:<.*)?$")
        prev_mn, prev_ops, prev_addr, after_end = "", "", 0, False
        with open(self.dis, errors="replace") as fh:
            for line in fh:
                g = pat.match(line)
                if not g:
                    continue
                a, mn, ops = int(g.group(1), 16), g.group(2), g.group(3)
                if prev_mn == "push" and prev_ops == "ebp" and mn == "mov" and ops == "ebp, esp":
                    prologues.add(prev_addr)
                if after_end and mn not in ("nop", "int3"):
                    others.add(a)
                    after_end = False
                if mn in ("ret", "retl") or (mn == "jmp" and not ops.startswith("0x")):
                    after_end = True
                prev_mn, prev_ops, prev_addr = mn, ops, a
        return prologues, others

    def import_names(self) -> dict[int, str]:
        if not self._import_names:
            self._import_names = {i: n for n, i in self.imports.items()}
        return self._import_names

    def in_code(self, a: int) -> bool:
        return any(lo <= a < hi for lo, hi in self.code_ranges)

    def end_of(self, f: int) -> int:
        i = bisect.bisect_right(self.hard_starts, f)
        nxt = self.hard_starts[i] if i < len(self.hard_starts) else 0xFFFFFFFF
        for lo, hi in self.code_ranges:
            if lo <= f < hi:
                return min(nxt, hi)
        return nxt

    def slice_lines(self, f: int, end: int, fh) -> list[str]:
        lo = bisect.bisect_left(self.addrs, f)
        hi = bisect.bisect_left(self.addrs, end, lo)
        if lo >= len(self.offs):
            return []
        last = min(hi, len(self.offs) - 1)
        start, stop = self.offs[lo], self.offs[last]
        fh.seek(start)
        data = fh.read(stop - start + 200)
        lines = data.decode("latin-1").splitlines()
        return lines[: (hi - lo) + 1]


W: World | None = None
HOOKS: set[int] = set()
hook_file = os.path.join(CONFIG, "hooks.json")
if os.path.exists(hook_file):
    HOOKS = {int(x, 16) for x in json.load(open(hook_file))}


def redecode(a: int, stop: int) -> list[Ins]:
    out = subprocess.run(
        OBJDUMP + [f"--start-address={a:#x}", f"--stop-address={stop:#x}", W.binary],
        capture_output=True, text=True, env=ENV, check=True,
    ).stdout
    return parse_lines(out.splitlines())


# ---------- function analysis ----------

class Fn:
    def __init__(self, addr: int, end: int):
        self.addr = addr
        self.end = end
        self.ins: dict[int, Ins] = {}
        self.reach: dict[int, Ins] = {}
        self.tables: dict[int, list[int]] = {}
        self.unresolved: list[int] = []
        self.external: set[int] = set()
        self.redecodes = 0

    def fetch(self, a: int) -> Ins | None:
        i = self.ins.get(a)
        if i is not None:
            return i
        if not (self.addr <= a < self.end):
            return None
        self.redecodes += 1
        for j in redecode(a, min(self.end, a + 2048)):
            if j.addr not in self.ins:
                self.ins[j.addr] = j
            if j.next == 0:
                j.next = 0  # resolved by walker bound check below
        i = self.ins.get(a)
        return i


JCC = {f"j{k}" for k in CONDS}


def imm_target(i: Ins) -> int | None:
    o = i.ops
    if len(o) == 1 and o[0].kind == "imm":
        return o[0].val & 0xFFFFFFFF
    return None


def pic_bases(fn: Fn) -> list[int]:
    out = []
    for i in fn.ins.values():
        if i.mn == "call":
            t = imm_target(i)
            if t is not None and t == i.next:
                out.append(t)
    return out


def recover_table(fn: Fn, jmp: Ins, order: list[int]) -> list[int] | None:
    """PIC jump table: mov r,[base+4*idx+D]; add r,base; jmp r."""
    reg = jmp.ops[0].reg
    pos = bisect.bisect_left(order, jmp.addr)
    disp = None
    bound = None
    for k in range(pos - 1, max(pos - 14, -1), -1):
        i = fn.ins[order[k]]
        try:
            ops = i.ops
        except ValueError:
            continue
        if disp is None:
            for o in ops:
                if o.kind == "mem" and o.scale == 4 and o.index and o.disp:
                    disp = o.disp
                    break
        elif i.mn == "cmp" and len(ops) == 2 and ops[1].kind == "imm":
            bound = ops[1].val & 0xFFFFFFFF
            break
        elif i.mn in ("ret", "jmp"):
            break
    if disp is None:
        return None
    m = W.m
    best = None
    for b in pic_bases(fn) or [fn.addr]:
        base_addr = (b + disp) & 0xFFFFFFFF
        targets = []
        limit = (bound + 1) if bound is not None and bound < 4096 else 4096
        for n in range(limit):
            e = m.read_u32((base_addr + 4 * n) & 0xFFFFFFFF)
            t = (b + e) & 0xFFFFFFFF
            if not (fn.addr <= t < fn.end):
                break
            targets.append(t)
        if targets and (bound is None or len(targets) == bound + 1):
            if best is None or len(targets) > len(best):
                best = targets
    return best


def walk(fn: Fn) -> None:
    # A landing pad's copy shares the parsed instructions of the real function it lives in.
    i = bisect.bisect_right(W.hard_starts, fn.addr) - 1
    hard = W.hard_starts[i] if i >= 0 else fn.addr
    if getattr(walk, "cache_key", None) != hard:
        lines = W.slice_lines(hard, fn.end, walk.fh)
        parsed = parse_lines(lines)
        cache = {}
        for ins in parsed:
            if ins.addr < fn.end:
                cache[ins.addr] = ins
        if parsed and parsed[-1].next == 0:
            parsed[-1].next = fn.end
        walk.cache_key = hard
        walk.cache = cache
    fn.ins = walk.cache
    work = [fn.addr]
    seen = fn.reach
    while work:
        a = work.pop()
        while a not in seen:
            if not (fn.addr <= a < fn.end):
                fn.external.add(a)
                break
            i = fn.fetch(a)
            if i is None:
                i = Ins(a, "<unknown>", "")
                i.next = a + 1
            if i.next == 0:
                i.next = fn.end
            seen[a] = i
            mn = i.mn
            if mn == "jmp":
                t = imm_target(i)
                if t is not None:
                    if fn.addr <= t < fn.end:
                        work.append(t)
                    else:
                        fn.external.add(t)
                else:
                    o = i.ops[0]
                    if o.kind == "r32":
                        tb = recover_table(fn, i, sorted(fn.ins))
                        if tb:
                            fn.tables[a] = tb
                            work.extend(tb)
                        else:
                            fn.unresolved.append(a)
                break
            if mn in JCC or mn in ("jecxz", "loop", "loope", "loopne"):
                t = imm_target(i)
                if fn.addr <= t < fn.end:
                    work.append(t)
                else:
                    fn.external.add(t)
                a = i.next
                continue
            if mn in TERMINATORS:
                break
            if mn == "call":
                t = imm_target(i)
                if t is not None and t != i.next:
                    fn.external.add(t)
                a = i.next
                continue
            if mn not in _KNOWN_OK and not _emittable(i):
                break
            a = i.next


_KNOWN_OK = {"mov", "lea", "push", "pop", "add", "sub", "cmp", "test", "movss", "nop"}


def _emittable(i: Ins) -> bool:
    try:
        emit(i, 0)
        return True
    except (Unsupported, ValueError, KeyError, IndexError):
        return False


def successors(fn: Fn, i: Ins) -> list[int]:
    mn = i.mn
    if mn == "jmp":
        t = imm_target(i)
        if t is not None:
            return [t] if t in fn.reach else []
        return list(fn.tables.get(i.addr, []))
    if mn in TERMINATORS:
        return []
    if mn in JCC or mn in ("jecxz", "loop", "loope", "loopne"):
        t = imm_target(i)
        res = [i.next] if i.next in fn.reach else []
        if t in fn.reach:
            res.append(t)
        return res
    return [i.next] if i.next in fn.reach else []


def liveness(fn: Fn) -> dict[int, int]:
    """Per-instruction live-out flag mask. Calls, returns and tail jumps need no flags."""
    order = sorted(fn.reach, reverse=True)
    succ = {a: successors(fn, fn.reach[a]) for a in order}
    live_in = dict.fromkeys(order, 0)
    live_out = dict.fromkeys(order, 0)
    effects = {}
    for a in order:
        i = fn.reach[a]
        try:
            effects[a] = flag_effect(i)
        except ValueError:
            effects[a] = (0, 0)
    changed = True
    while changed:
        changed = False
        for a in order:
            i = fn.reach[a]
            out = 0
            # "call next; pop reg" only reads the program counter, so flags stay live across it.
            is_pc_read = i.mn == "call" and i.ops[0].kind == "imm" and (i.ops[0].val & 0xFFFFFFFF) == i.next
            if i.mn != "call" or is_pc_read:
                for s in succ[a]:
                    out |= live_in[s]
            r, w = effects[a]
            inn = r | (out & ~w)
            if inn != live_in[a] or out != live_out[a]:
                live_in[a], live_out[a] = inn, out
                changed = True
    return live_out


# ---------- C emission ----------

FRAME_WAIT_BRANCH = 0x182dffe
# Functions translated as an immediate return. The first writes Feral's shader usage log at exit through std::ofstream.
EMPTY_FUNCTIONS = {0x497b0}
# Called directly by the engine and changing the Direct3D layer's state, so it has to run in order on that layer's thread (runtime/d3d_queue.c).
D3D_QUEUE_FUNCTIONS = {0x1e4ab50}
# Ordered loads and stores trap when a value crosses a 16-byte boundary, which costs microseconds each time. These functions walk
# byte streams with unaligned reads and writes, and none of them coordinates with another thread through that memory, so they use
# plain accesses. Find more with ARKHAM_TRACE_UNALIGNED=1.
UNALIGNED_FUNCTIONS = {
    0x1c05da0,  # launcher: searches a large buffer for a marker, one byte at a time
    0x2804180,  # LZO decompression of game packages
    0x1be4870,  # text parser on UTF-16 strings
    0x1ec4370,  # reads a packed bit stream
    0x1ec4800,  # constructor of a packed struct array
    0xe101767e,  # Bink: video block copy
    0xe1016b5a,  # the same
    0x378d00,  # package loading: converts names and paths held in packed records
    0x32dfe0,  # package loading: reads packed records
}
# Library calls so frequent and so simple that translated code makes them itself, with the arguments still on the guest stack.
INLINE_IMPORTS = {
    "_pthread_getspecific": "eax=rt_tsd_get(c,R32(esp));",
    "_pthread_setspecific": "rt_tsd_set(c,R32(esp),R32(esp+4));eax=0;",
    "_memcpy": "{u32 d_=R32(esp);memmove(M+d_,M+R32(esp+4),R32(esp+8));eax=d_;}",
    "_memset": "{u32 d_=R32(esp);memset(M+d_,(int)R32(esp+4),R32(esp+8));eax=d_;}",
    "___bzero": "memset(M+R32(esp),0,R32(esp+4));",
    "_memcmp": "eax=(u32)memcmp(M+R32(esp),M+R32(esp+4),R32(esp+8));",
    "_strlen": "eax=(u32)strlen((const char*)(M+R32(esp)));",
    "_wcscmp": "eax=(u32)wcscmp((const wchar_t*)(M+R32(esp)),(const wchar_t*)(M+R32(esp+4)));",
    # The same as the shims in runtime/shims_libc.c do, without the trip through rt_imp: these are called hundreds of thousands of times a
    # second in play (ARKHAM_TRACE_IMPORTS, 2026-11-08). Floats and doubles come in on the stack and go out on the x87 stack, as there.
    "___tolower": "eax=(u32)tolower((int)R32(esp));",
    "___toupper": "eax=(u32)toupper((int)R32(esp));",
    "___isnanf": "eax=(u32)isnan(RF(esp));",
    "___isinff": "eax=(u32)isinf(RF(esp));",
    "___isnand": "eax=(u32)isnan(RD(esp));",
    "___isinfd": "eax=(u32)isinf(RD(esp));",
    "_strcmp": "eax=(u32)strcmp((const char*)(M+R32(esp)),(const char*)(M+R32(esp+4)));",
    "_strncmp": "eax=(u32)strncmp((const char*)(M+R32(esp)),(const char*)(M+R32(esp+4)),R32(esp+8));",
    "_strcasecmp": "eax=(u32)strcasecmp((const char*)(M+R32(esp)),(const char*)(M+R32(esp+4)));",
    "_malloc_size": "eax=rt_guest_block_size(R32(esp));",
    "_pthread_self": "eax=rt_pthread_self_id();",
    **{f"_{n}": f"FPUSH((double){n}(RF(esp)));" for n in ("sinf", "cosf", "tanf", "asinf", "acosf", "atanf", "expf", "logf", "log10f",
                                                        "floorf", "ceilf", "roundf", "rintf")},
    **{f"_{n}": f"FPUSH((double){n}(RF(esp),RF(esp+4)));" for n in ("atan2f", "powf", "fmodf", "fmaxf", "fminf")},
    **{f"_{n}": f"FPUSH({n}(RD(esp)));" for n in ("sin", "cos", "tan", "asin", "acos", "atan", "exp", "log", "log10", "floor", "ceil",
                                                "round", "rint")},
    **{f"_{n}": f"FPUSH({n}(RD(esp),RD(esp+8)));" for n in ("atan2", "pow", "fmod")},
}
# These unwind through guest frames, so they need every register in memory.
NOT_LEAF = {"___cxa_throw", "___cxa_rethrow", "__Unwind_Resume", "___cxa_begin_catch", "___cxa_end_catch", "___cxa_free_exception",
            "___cxa_allocate_exception", "__ZSt18uncaught_exceptionv"}
_no_gate: tuple[set[str], set[str]] | None = None


def is_leaf_import(name: str) -> bool:
    """Shims that skip the runtime's lock never run guest code: the list lives in rt_shim_no_gate in runtime/shims_misc.c."""
    global _no_gate
    if _no_gate is None:
        src = open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "runtime", "shims_misc.c")).read()
        src = src[src.index("int rt_shim_no_gate("):]
        src = src[:src.index("// Find a shim for a function")]
        names_part, gl_part = src.split('if (!strncmp(name, "_gl", 3))', 1)
        _no_gate = (set(re.findall(r'"(_[^"]+)"', names_part)), set(re.findall(r'"(_[^"]+)"', gl_part)))
    if name in NOT_LEAF:
        return False
    if name.startswith("_gl"):
        return name not in _no_gate[1]
    return name in _no_gate[0]


UNALIGNED_RANGES = [(0x1850000, 0x1870000)]  # UnrealScript: opcodes read their operands from the bytecode stream

CMP_COND = {
    "e": "a==b", "ne": "a!=b", "b": "a<b", "ae": "a>=b", "be": "a<=b", "a": "a>b",
    "l": "(S)a<(S)b", "ge": "(S)a>=(S)b", "le": "(S)a<=(S)b", "g": "(S)a>(S)b",
    "s": "(S)(a-b)<0", "ns": "(S)(a-b)>=0",
}
TEST_COND = {
    "e": "(a&b)==0", "ne": "(a&b)!=0", "s": "(S)(a&b)<0", "ns": "(S)(a&b)>=0",
    "l": "(S)(a&b)<0", "ge": "(S)(a&b)>=0", "le": "(S)(a&b)<=0", "g": "(S)(a&b)>0",
    "b": "0", "ae": "1", "be": "(a&b)==0", "a": "(a&b)!=0",
}


class Emitter:
    def __init__(self, fn: Fn, stats: collections.Counter):
        self.fn = fn
        self.stats = stats
        self.refs: set[str] = set()
        self.out: list[str] = []

    def fname(self, t: int) -> str:
        n = f"F_{t:07x}"
        self.refs.add(n)
        return n

    def tail(self, t: int) -> str:
        lo, hi = W.stub_section
        if lo <= t < hi:
            st = W.stubs.get(t)
            if st and st[0] == "imp":
                name = W.import_names().get(st[1], "")
                if name in INLINE_IMPORTS:
                    # A jump to the import, as at the end of a wrapper: its ret pops our caller's return address, and the arguments are
                    # then at esp, where the inline form reads them. Feral's string compare ends this way, 3.4 million times a second.
                    return f"esp+=4u;{INLINE_IMPORTS[name]}SAVE;return;"
                return f"SAVE;rt_imp(c,{st[1]});return;"
            if st and st[0] == "fn":
                t = st[1]
        if not W.in_code(t):
            return f'SAVE;rt_trap(c,{hx(t)},"tail outside code");return;'
        return f"SAVE;__attribute__((musttail)) return {self.fname(t)}(c,M);"

    def call(self, t: int, nxt: int) -> str:
        pre = f"esp-=4;R32(esp)={hx(nxt)};SAVE;"
        lo, hi = W.stub_section
        if lo <= t < hi:
            st = W.stubs.get(t)
            if st and st[0] == "imp":
                name = W.import_names().get(st[1], "")
                if name in INLINE_IMPORTS:
                    return INLINE_IMPORTS[name]
                if is_leaf_import(name):
                    # The shim reads its arguments through esp and answers in eax and edx. Nothing else needs to reach memory.
                    return f"esp-=4;R32(esp)={hx(nxt)};c->esp=esp;rt_imp(c,{st[1]});esp=c->esp;eax=c->eax;edx=c->edx;"
                return f"{pre}rt_imp(c,{st[1]});LOAD;"
            if st and st[0] == "fn":
                t = st[1]
        if not W.in_code(t):
            return f'{pre}rt_trap(c,{hx(t)},"call outside code");LOAD;'
        return f"{pre}{self.fname(t)}(c,M);LOAD;"

    def jump(self, i: Ins, t: int) -> str:
        fn = self.fn
        if t in fn.reach:
            poll = "POLL;" if t <= i.addr else ""
            return f"{poll}goto L_{t:x};"
        return self.tail(t)

    def run(self, peephole: bool = True) -> str:
        fn = self.fn
        live = liveness(fn)
        order = sorted(fn.reach)
        leaders = set()
        for a in order:
            i = fn.reach[a]
            if i.mn == "jmp" or i.mn in JCC or i.mn in ("jecxz", "loop", "loope", "loopne"):
                t = imm_target(i) if i.mn != "jmp" or i.ops[0].kind == "imm" else None
                if t is not None and t in fn.reach:
                    leaders.add(t)
                for tt in fn.tables.get(a, []):
                    leaders.add(tt)
        out = [f"void F_{fn.addr:07x}(CPU *restrict c,u8 *restrict M){{LOCALS;"]
        pend = None
        for idx, a in enumerate(order):
            i = fn.reach[a]
            if a in leaders:
                out.append(f"L_{a:x}:;")
                pend = None
            mn = i.mn
            try:
                if pend is not None:
                    stmt = self.with_cond(i, pend)
                    pend = None
                    out.append(stmt)
                else:
                    nxt = fn.reach.get(i.next)
                    if peephole and mn in ("cmp", "test") and nxt is not None and i.next not in leaders:
                        cc = reader_cc(nxt)
                        if cc in (CMP_COND if mn == "cmp" else TEST_COND) and not (live.get(nxt.addr, 0) & ALL):
                            pend = (mn, i)
                            continue
                    out.append(self.ctl(i, live.get(a, 0)))
            except (Unsupported, ValueError, KeyError, IndexError) as e:
                self.stats[f"unimpl:{mn}"] += 1
                out.append(f'rt_unimpl(c,{hx(a)},"{mn} {i.text}");')
            # explicit fallthrough when the next address in the order is not the real next
            if mn not in TERMINATORS and idx + 1 < len(order) and order[idx + 1] != i.next and pend is None:
                if i.next in fn.reach:
                    out.append(f"goto L_{i.next:x};")
                    leaders.add(i.next)
                else:
                    out.append(self.tail(i.next))
            if mn not in TERMINATORS and idx + 1 == len(order) and pend is None:
                if i.next in fn.reach:
                    out.append(f"goto L_{i.next:x};")
                    leaders.add(i.next)
                else:
                    out.append(self.tail(i.next))
        out.append("}")
        return "\n".join(out)

    def with_cond(self, r: Ins, pend) -> str:
        kind, w = pend
        ops = w.ops
        sz = opsize(ops)
        T, ST = TYPES[sz], STYPES[sz]
        cc = reader_cc(r)
        tbl = CMP_COND if kind == "cmp" else TEST_COND
        cond = tbl[cc].replace("(S)", f"({ST})")
        head = f"{{{T} a={rd(ops[0], sz)},b={rd(ops[1], sz)};"
        mn = r.mn
        if mn in JCC:
            t = imm_target(r)
            return f"{head}if({cond}){{{self.jump(r, t)}}}}}"
        if mn.startswith("set"):
            from emit import wr
            return f"{head}{wr(r.ops[0]._replace(size=1), f'({cond})?1:0')}}}"
        from emit import wr
        d, s = r.ops
        return f"{head}if({cond}){{{wr(d, rd(s, d.size or 4))}}}}}"

    def ctl(self, i: Ins, live: int) -> str:
        mn = i.mn
        a = i.addr
        if mn == "call":
            o = i.ops[0]
            if o.kind == "imm":
                t = o.val & 0xFFFFFFFF
                if t == i.next:  # PIC base idiom: push only
                    return f"esp-=4;R32(esp)={hx(t)};"
                return self.call(t, i.next)
            return f"{{u32 t={rd(o, 4)};esp-=4;R32(esp)={hx(i.next)};SAVE;rt_dispatch(c,t);LOAD;}}"
        if mn == "jmp":
            o = i.ops[0]
            if o.kind == "imm":
                return self.jump(i, o.val & 0xFFFFFFFF)
            tb = self.fn.tables.get(a)
            if tb:
                cases = "".join(f"case {hx(t)}:goto L_{t:x};" for t in sorted(set(tb)))
                # A recovered table can be a false match on a tail call through a vtable, so a miss goes to the dispatcher.
                return (f"POLL;{{u32 jt_={rd(o, 4)};switch(jt_){{{cases}"
                        f"default:c->jt=jt_;SAVE;__attribute__((musttail)) return rt_dispatch_jt(c,M);}}}}")
            return f"c->jt={rd(o, 4)};SAVE;__attribute__((musttail)) return rt_dispatch_jt(c,M);"
        if mn in ("ret", "retn"):
            n = i.ops[0].val if i.ops else 0
            return f"esp+={4 + n}u;SAVE;return;"
        if mn in JCC:
            t = imm_target(i)
            cond = CONDS[mn[1:]][0]
            # The engine's frame pacing: this branch means "no time to wait". Taking it always removes the frame limit.
            if a == FRAME_WAIT_BRANCH:
                cond = f"rt_no_frame_limit||({cond})"
            return f"if({cond}){{{self.jump(i, t)}}}"
        if mn == "jecxz":
            return f"if(ecx==0){{{self.jump(i, imm_target(i))}}}"
        if mn == "loop":
            return f"ecx--;if(ecx){{{self.jump(i, imm_target(i))}}}"
        if mn == "loope":
            return f"ecx--;if(ecx&&zf){{{self.jump(i, imm_target(i))}}}"
        if mn == "loopne":
            return f"ecx--;if(ecx&&!zf){{{self.jump(i, imm_target(i))}}}"
        if mn in ("hlt", "ud2", "int3", "<unknown>", "iretd", "retf", "lret", "ljmp"):
            self.stats[f"trap:{mn}"] += 1
            return f'SAVE;rt_trap(c,{hx(a)},"{mn}");return;'
        s = emit(i, live)
        return s


def reader_cc(i: Ins) -> str | None:
    mn = i.mn
    if mn in JCC:
        return mn[1:]
    if mn.startswith("set") and mn[3:] in CONDS:
        return mn[3:]
    if mn.startswith("cmov") and mn[4:] in CONDS:
        return mn[4:]
    return None


def lift_shard(job):
    shard, funcs = job
    fh = open(W.dis, "rb")
    walk.fh = fh
    stats: collections.Counter = collections.Counter()
    external: set[int] = set()
    unresolved: list[int] = []
    decls: set[str] = set()
    bodies: list[str] = []
    defined: list[str] = []
    n_ins = 0
    redecodes = 0
    for f in funcs:
        fn = Fn(f, W.end_of(f))
        walk(fn)
        hooked = f in HOOKS
        n_ins += len(fn.reach)
        redecodes += fn.redecodes
        external |= fn.external
        unresolved += fn.unresolved
        em = Emitter(fn, stats)
        emit_module.plain_memory = f in UNALIGNED_FUNCTIONS or any(lo <= f < hi for lo, hi in UNALIGNED_RANGES)
        body = em.run()
        emit_module.plain_memory = False
        if f in EMPTY_FUNCTIONS:
            em.refs = set()
            body = f"void F_{f:07x}(CPU *restrict c,u8 *restrict M){{c->eax=0;c->esp+=4;}}"
        if f in D3D_QUEUE_FUNCTIONS:
            body = body.replace(f"void F_{f:07x}(CPU *restrict c,u8 *restrict M){{", f"static void H_{f:07x}(CPU *restrict c,u8 *restrict M){{", 1)
            body += f"\nvoid rt_d3dq_function(CPU *c, guest_fn body);\nvoid F_{f:07x}(CPU *restrict c,u8 *restrict M){{rt_d3dq_function(c,H_{f:07x});}}"
        if hooked:
            # The real body becomes H_xxx; F_xxx logs around it and may be overridden in the runtime.
            body = body.replace(f"void F_{f:07x}(CPU *restrict c,u8 *restrict M){{", f"static void H_{f:07x}(CPU *restrict c,u8 *restrict M){{", 1)
            body += (f"\nvoid F_{f:07x}(CPU *restrict c,u8 *restrict M){{rt_hook_enter(c,{hx(f)});H_{f:07x}(c,M);rt_hook_leave(c,{hx(f)});}}")
        bodies.append(body)
        decls |= em.refs
        defined.append(f"F_{f:07x}")
    fh.close()
    decls -= set(defined)
    path = os.path.join(GEN, f"s_{shard:04x}.c")
    text = ['#include "rt.h"\n']
    text += [f"void {d}(CPU*restrict,u8*restrict);\n" for d in sorted(decls)]
    text += [f"void {d}(CPU*restrict,u8*restrict);\n" for d in defined]
    text.append("\n".join(bodies))
    text.append("\n")
    blob = "".join(text)
    digest = hashlib.sha1(blob.encode()).hexdigest()
    prev = None
    if os.path.exists(path):
        with open(path, "rb") as fh2:
            prev = hashlib.sha1(fh2.read()).hexdigest()
    if prev != digest:
        with open(path, "w") as fh2:
            fh2.write(blob)
    return dict(shard=shard, stats=stats, external=external, unresolved=unresolved, n_ins=n_ins,
                redecodes=redecodes, n_fn=len(funcs), changed=prev != digest)


def init_worker(binary, roots):
    global W
    W = World(binary, roots)


def write_tables(world: World, bink: World, bink_base: int) -> None:
    fnlist = world.roots + bink.roots
    with open(os.path.join(GEN, "fntab.c"), "w") as fh:
        fh.write('#include "rt.h"\n')
        for f in fnlist:
            fh.write(f"void F_{f:07x}(CPU*restrict,u8*restrict);\n")
        fh.write("const rt_fn_entry rt_fntab[] = {\n")
        for f in fnlist:
            fh.write(f"{{{f:#x}u,F_{f:07x}}},\n")
        fh.write("};\nconst unsigned rt_fntab_n = sizeof(rt_fntab)/sizeof(rt_fntab[0]);\n")
    # The Bink library's imports continue the game's numbering; a name in both reaches the same shim.
    names = sorted(world.imports, key=world.imports.get) + sorted(bink.imports, key=bink.imports.get)
    with open(os.path.join(GEN, "imports.c"), "w") as fh:
        fh.write('#include "rt.h"\nconst char *const rt_import_names[] = {\n')
        for n in names:
            fh.write(f'"{n}",\n')
        fh.write("};\nconst unsigned rt_import_n = sizeof(rt_import_names)/sizeof(rt_import_names[0]);\n")
        fh.write("const rt_bind_entry rt_binds[] = {\n")
        count = 0
        for w in (world, bink):
            for addr, (sym, addend) in sorted(w.bind_at.items()):
                if sym in w.imports:
                    fh.write(f"{{{addr:#x}u,{w.imports[sym]}u,{addend}}},\n")
                    count += 1
        fh.write("};\n")
        fh.write(f"const unsigned rt_binds_n = {count}u;\n")
        fh.write(f"const unsigned rt_entry_addr = {world.m.entry:#x}u;\n")
        mi = world.m.section("__mod_init_func")
        fh.write(f"const unsigned rt_modinit_addr = {mi.addr:#x}u;\nconst unsigned rt_modinit_n = {mi.size // 4}u;\n")
        ehf = world.m.section("__eh_frame")
        fh.write(f"const unsigned rt_eh_frame_addr = {ehf.addr:#x}u;\nconst unsigned rt_eh_frame_size = {ehf.size:#x}u;\n")
        for label, sname in (("class", "__class"), ("cat", "__category"), ("msgrefs", "__message_refs")):
            sec = world.m.section(sname, "__OBJC")
            fh.write(f"const unsigned rt_objc_{label}_addr = {sec.addr:#x}u;\nconst unsigned rt_objc_{label}_size = {sec.size:#x}u;\n")
        cfs = world.m.section("__cfstring")
        fh.write(f"const unsigned rt_cfstring_addr = {cfs.addr:#x}u;\nconst unsigned rt_cfstring_size = {cfs.size:#x}u;\n")
        segs = [(s.vmaddr, s.vmsize, s.fileoff, s.filesize) for s in world.m.segments if s.name != "__PAGEZERO"]
        fh.write("const rt_seg_entry rt_segs[] = {\n")
        for v, vs, fo, fs in segs:
            fh.write(f"{{{v:#x}u,{vs:#x}u,{fo:#x}u,{fs:#x}u}},\n")
        fh.write("};\n" + f"const unsigned rt_segs_n = {len(segs)}u;\n")
    # The Bink library: where it goes, what the loader must slide, and a forwarder for each function the game calls.
    with open(os.path.join(GEN, "bink_tables.c"), "w") as fh:
        fh.write('#include "shim.h"\n#include "shim_reg.h"\n')
        fh.write(f"const unsigned rt_bink_base = {bink_base:#x}u;\n")
        segs = [(s.vmaddr, s.vmsize, s.fileoff, s.filesize) for s in bink.m.segments]
        fh.write("const rt_seg_entry rt_bink_segs[] = {\n")
        for v, vs, fo, fs in segs:
            fh.write(f"{{{v:#x}u,{vs:#x}u,{fo:#x}u,{fs:#x}u}},\n")
        fh.write("};\n" + f"const unsigned rt_bink_segs_n = {len(segs)}u;\n")
        fh.write("const unsigned rt_bink_rebases[] = {\n")
        for r in bink.m.rebases:
            fh.write(f"{r:#x}u,\n")
        fh.write("};\n" + f"const unsigned rt_bink_rebases_n = {len(bink.m.rebases)}u;\n")
        exports = {n: a for n, a in bink.m.defined_by_name().items() if n in world.imports and bink.in_code(a)}
        for n, a in sorted(exports.items()):
            # The translated function pops the return address itself, and rt_imp pops it again after a shim returns.
            fh.write(f"static void fwd{n}(CPU *c) {{ rt_dispatch(c, {a:#x}u); c->esp -= 4; }}\n")
        fh.write("const ShimReg rt_shims_bink_exports[] = {\n")
        for n in sorted(exports):
            fh.write(f'{{"{n}", fwd{n}}},\n')
        fh.write("{NULL, NULL},\n};\n")


BINK_BASE = 0xE1000000


def lift_image(binary: str, tag: str, import_base: int, args) -> World:
    global W
    roots_file = os.path.join(BUILD, f"extra_roots{tag}.json")
    extra: set[int] = set()
    if os.path.exists(roots_file):
        extra = set(json.load(open(roots_file)))
    manual = os.path.join(CONFIG, f"roots{tag}.json")
    if os.path.exists(manual):
        extra |= {int(x, 16) for x in json.load(open(manual))}
    ctx = mp.get_context("fork")
    for rnd in range(args.max_rounds):
        t0 = time.time()
        world = World(binary, extra, tag, import_base)
        by_shard = collections.defaultdict(list)
        for f in world.roots:
            by_shard[f >> SHARD_BITS].append(f)
        jobs = sorted(by_shard.items())
        if args.shards:
            jobs = jobs[: args.shards]
        W = world
        with ctx.Pool(args.jobs) as pool:
            results = pool.map(lift_shard, jobs, chunksize=1)
        stats: collections.Counter = collections.Counter()
        ext: set[int] = set()
        unresolved: list[int] = []
        for r in results:
            stats.update(r["stats"])
            ext |= r["external"]
            unresolved += r["unresolved"]
        new_roots = set()
        for t in ext:
            lo, hi = world.stub_section
            if lo <= t < hi:
                st = world.stubs.get(t)
                if st and st[0] == "fn" and world.in_code(st[1]) and st[1] not in world.rootset:
                    new_roots.add(st[1])
                continue
            if world.in_code(t) and t not in world.rootset:
                new_roots.add(t)
        for st in world.stubs.values():
            if st[0] == "fn" and world.in_code(st[1]) and st[1] not in world.rootset:
                new_roots.add(st[1])
        total_ins = sum(r["n_ins"] for r in results)
        print(f"{tag or 'game'} round {rnd}: shards={len(results)} functions={sum(r['n_fn'] for r in results)} reachable_ins={total_ins} "
              f"redecodes={sum(r['redecodes'] for r in results)} new_roots={len(new_roots)} "
              f"unresolved_jumps={len(unresolved)} changed={sum(r['changed'] for r in results)} {time.time() - t0:.0f}s")
        if args.shards or not new_roots:
            break
        extra |= new_roots
        with open(roots_file, "w") as fh:
            json.dump(sorted(extra), fh)
    report = {
        "functions": len(world.roots), "reachable_instructions": total_ins,
        "imports": len(world.imports), "binds": len(world.bind_at),
        "unresolved_jumps": [hex(x) for x in sorted(unresolved)],
        "stats": dict(stats.most_common()),
    }
    with open(os.path.join(BUILD, f"lift_report{tag}.json"), "w") as fh:
        json.dump(report, fh, indent=1)
    print(json.dumps({k: v for k, v in report.items() if k not in ("unresolved_jumps", "stats")}))
    print("unresolved sample", report["unresolved_jumps"][:10], "unsupported",
          {k: v for k, v in stats.items() if k.startswith(("unsupported", "trap"))})
    return world


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--shards", type=int, default=0, help="only lift this many shards (smoke test)")
    ap.add_argument("--max-rounds", type=int, default=8)
    args = ap.parse_args()
    os.makedirs(GEN, exist_ok=True)
    os.makedirs(CONFIG, exist_ok=True)
    game = lift_image(game_binary(), "", 0, args)
    rebased = os.path.join(BUILD, "libBink.rebased.dylib")
    rebase_dylib(game_bink(), rebased, BINK_BASE)
    bink = lift_image(rebased, "_bink", len(game.imports), args)
    write_tables(game, bink, BINK_BASE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
