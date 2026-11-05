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
from emit import ALL, CONDS, Unsupported, emit, flag_effect, hx, rd, opsize, TYPES, STYPES  # noqa: E402
from game_path import game_binary  # noqa: E402
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

def ensure_disasm(binary: str):
    """Linear sweep over code sections. Returns the path and a sorted (addrs, offsets) index."""
    os.makedirs(BUILD, exist_ok=True)
    dis = os.path.join(BUILD, "dis.txt")
    idx = os.path.join(BUILD, "dis.idx")
    if not os.path.exists(idx):
        with open(dis, "wb") as fh:
            for sec in ("__text", "google_malloc"):
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

    def __init__(self, binary: str, extra_roots: set[int]):
        self.binary = binary
        self.m = MachO(binary)
        self.dis, self.addrs, self.offs = ensure_disasm(binary)
        m = self.m
        self.code_ranges = []
        for name in ("__text", "google_malloc"):
            sec = m.section(name)
            self.code_ranges.append((sec.addr, sec.end))
        gm = m.section("google_malloc")
        roots = set(m.function_starts) | set(extra_roots)
        roots.add(m.entry)
        roots.add(gm.addr)
        self.roots = sorted(r for r in roots if self.in_code(r))
        self.rootset = set(self.roots)
        # Function extents come only from the linker's table. An extra root (say, a jump into the middle of a
        # function, or one derived from data decoded as code) gets its own copy and never cuts a real function short.
        self.hard_starts = sorted({r for r in set(m.function_starts) | {m.entry, gm.addr} if self.in_code(r)})
        # imports
        self.imports: dict[str, int] = {}
        self.bind_at: dict[int, tuple[str, int]] = {}
        defined = m.defined_by_name()
        for b in m.binds:
            if b.ordinal > 0 or b.ordinal < 0 or b.symbol not in defined:
                idx = self.imports.setdefault(b.symbol, len(self.imports))
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
    fh = walk.fh
    lines = W.slice_lines(fn.addr, fn.end, fh)
    parsed = parse_lines(lines)
    for i in parsed:
        if i.addr < fn.end:
            fn.ins[i.addr] = i
    if parsed and parsed[-1].next == 0:
        parsed[-1].next = fn.end
    order = sorted(fn.ins)
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
                        order = sorted(fn.ins)
                        tb = recover_table(fn, i, order)
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
            if i.mn != "call":
                for s in succ[a]:
                    out |= live_in[s]
            r, w = effects[a]
            inn = r | (out & ~w)
            if inn != live_in[a] or out != live_out[a]:
                live_in[a], live_out[a] = inn, out
                changed = True
    return live_out


# ---------- C emission ----------

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
                return f"SAVE;rt_imp(c,{st[1]});return;"
            if st and st[0] == "fn":
                t = st[1]
        if not W.in_code(t):
            return f'SAVE;rt_trap(c,{hx(t)},"tail outside code");return;'
        return f"SAVE;__attribute__((musttail)) return {self.fname(t)}(c);"

    def call(self, t: int, nxt: int) -> str:
        pre = f"esp-=4;R32(esp)={hx(nxt)};SAVE;"
        lo, hi = W.stub_section
        if lo <= t < hi:
            st = W.stubs.get(t)
            if st and st[0] == "imp":
                return f"{pre}rt_imp(c,{st[1]});LOAD;"
            if st and st[0] == "fn":
                t = st[1]
        if not W.in_code(t):
            return f'{pre}rt_trap(c,{hx(t)},"call outside code");LOAD;'
        return f"{pre}{self.fname(t)}(c);LOAD;"

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
        out = [f"void F_{fn.addr:07x}(CPU *restrict c){{LOCALS;"]
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
                return (f"POLL;switch({rd(o, 4)}){{{cases}"
                        f'default:SAVE;rt_trap(c,{hx(a)},"jump table miss");return;}}')
            return f"c->jt={rd(o, 4)};SAVE;__attribute__((musttail)) return rt_dispatch_jt(c);"
        if mn in ("ret", "retn"):
            n = i.ops[0].val if i.ops else 0
            return f"esp+={4 + n}u;SAVE;return;"
        if mn in JCC:
            t = imm_target(i)
            return f"if({CONDS[mn[1:]][0]}){{{self.jump(i, t)}}}"
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
        body = em.run()
        if hooked:
            # The real body becomes H_xxx; F_xxx logs around it and may be overridden in the runtime.
            body = body.replace(f"void F_{f:07x}(CPU *restrict c){{", f"static void H_{f:07x}(CPU *restrict c){{", 1)
            body += (f"\nvoid F_{f:07x}(CPU *restrict c){{rt_hook_enter(c,{hx(f)});H_{f:07x}(c);rt_hook_leave(c,{hx(f)});}}")
        bodies.append(body)
        decls |= em.refs
        defined.append(f"F_{f:07x}")
    fh.close()
    decls -= set(defined)
    path = os.path.join(GEN, f"s_{shard:04x}.c")
    text = ['#include "rt.h"\n']
    text += [f"void {d}(CPU*restrict);\n" for d in sorted(decls)]
    text += [f"void {d}(CPU*restrict);\n" for d in defined]
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


def write_tables(world: World, fnlist: list[int]) -> None:
    with open(os.path.join(GEN, "fntab.c"), "w") as fh:
        fh.write('#include "rt.h"\n')
        for f in fnlist:
            fh.write(f"void F_{f:07x}(CPU*restrict);\n")
        fh.write("const rt_fn_entry rt_fntab[] = {\n")
        for f in fnlist:
            fh.write(f"{{{f:#x}u,F_{f:07x}}},\n")
        fh.write("};\nconst unsigned rt_fntab_n = sizeof(rt_fntab)/sizeof(rt_fntab[0]);\n")
    names = sorted(world.imports, key=world.imports.get)
    with open(os.path.join(GEN, "imports.c"), "w") as fh:
        fh.write('#include "rt.h"\nconst char *const rt_import_names[] = {\n')
        for n in names:
            fh.write(f'"{n}",\n')
        fh.write("};\nconst unsigned rt_import_n = sizeof(rt_import_names)/sizeof(rt_import_names[0]);\n")
        fh.write("const rt_bind_entry rt_binds[] = {\n")
        count = 0
        defined = world.m.defined_by_name()
        for addr, (sym, addend) in sorted(world.bind_at.items()):
            if sym in world.imports:
                fh.write(f"{{{addr:#x}u,{world.imports[sym]}u,{addend}}},\n")
                count += 1
        fh.write("};\n")
        fh.write(f"const unsigned rt_binds_n = {count}u;\n")
        fh.write(f"const unsigned rt_entry_addr = {world.m.entry:#x}u;\n")
        mi = world.m.section("__mod_init_func")
        fh.write(f"const unsigned rt_modinit_addr = {mi.addr:#x}u;\nconst unsigned rt_modinit_n = {mi.size // 4}u;\n")
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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--shards", type=int, default=0, help="only lift this many shards (smoke test)")
    ap.add_argument("--max-rounds", type=int, default=8)
    args = ap.parse_args()
    binary = game_binary()
    os.makedirs(GEN, exist_ok=True)
    os.makedirs(CONFIG, exist_ok=True)
    roots_file = os.path.join(BUILD, "extra_roots.json")
    extra: set[int] = set()
    if os.path.exists(roots_file):
        extra = set(json.load(open(roots_file)))
    manual = os.path.join(CONFIG, "roots.json")
    if os.path.exists(manual):
        extra |= {int(x, 16) for x in json.load(open(manual))}
    ctx = mp.get_context("fork")
    for rnd in range(args.max_rounds):
        t0 = time.time()
        world = World(binary, extra)
        by_shard = collections.defaultdict(list)
        for f in world.roots:
            by_shard[f >> SHARD_BITS].append(f)
        jobs = sorted(by_shard.items())
        if args.shards:
            jobs = jobs[: args.shards]
        global W
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
        for addr, _ in world.stubs.items():
            pass
        for st in world.stubs.values():
            if st[0] == "fn" and world.in_code(st[1]) and st[1] not in world.rootset:
                new_roots.add(st[1])
        total_ins = sum(r["n_ins"] for r in results)
        print(f"round {rnd}: shards={len(results)} functions={sum(r['n_fn'] for r in results)} reachable_ins={total_ins} "
              f"redecodes={sum(r['redecodes'] for r in results)} new_roots={len(new_roots)} "
              f"unresolved_jumps={len(unresolved)} changed={sum(r['changed'] for r in results)} {time.time() - t0:.0f}s", flush=True)
        if args.shards or not new_roots:
            break
        extra |= new_roots
        with open(roots_file, "w") as fh:
            json.dump(sorted(extra), fh)
    write_tables(world, world.roots)
    report = {
        "functions": len(world.roots), "reachable_instructions": total_ins,
        "imports": len(world.imports), "binds": len(world.bind_at),
        "unresolved_jumps": [hex(x) for x in sorted(unresolved)],
        "stats": dict(stats.most_common()),
    }
    with open(os.path.join(BUILD, "lift_report.json"), "w") as fh:
        json.dump(report, fh, indent=1)
    print(json.dumps({k: v for k, v in report.items() if k not in ("unresolved_jumps",)}, indent=0)[:6000])
    print("unresolved sample", report["unresolved_jumps"][:10])
    return 0


if __name__ == "__main__":
    sys.exit(main())
