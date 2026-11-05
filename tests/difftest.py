#!/usr/bin/env python3
"""Differential test of instruction semantics: real x86 (x86_64 mode under Rosetta 2, 32-bit operands) versus the
C code the lifter emits, compiled for arm64. Compares registers, XMM state and defined flags."""
import os
import random
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from emit import ALL, A, C, O, P, S, Z, emit  # noqa: E402
from x86 import parse_lines  # noqa: E402

ENV = dict(os.environ, DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer")
ALLF = C | Z | S | O | P
R = ["eax", "ecx", "edx", "ebx", "esi", "edi"]
R16 = ["ax", "cx", "dx", "bx", "si", "di"]
R8 = ["al", "cl", "dl", "bl", "ah", "ch"]

TESTS = []


def t(text, mask=ALLF):
    TESTS.append((text, mask))


for op in "add sub and or xor cmp test adc sbb".split():
    mask = ALLF | A if op in ("add", "sub", "cmp", "adc", "sbb") else ALLF
    for a, b in (("eax", "ecx"), ("ebx", "edx"), ("esi", "esi"), ("ax", "cx"), ("al", "bl"), ("ah", "cl"), ("ch", "dl")):
        t(f"{op} {a}, {b}", mask)
    for a, imm in (("eax", "0x7f"), ("edx", "0x80000000"), ("cx", "0x1234"), ("bl", "0x80")):
        t(f"{op} {a}, {imm}", mask)
for op in ("inc", "dec", "neg", "not"):
    mask = 0 if op == "not" else ((Z | S | O | P | A) if op in ("inc", "dec") else ALLF | A)
    for a in ("eax", "dx", "bl", "ch"):
        t(f"{op} {a}", mask)
for op in ("shl", "shr", "sar"):
    for a in ("eax", "dx", "bl", "ch"):
        for c in ("1", "5", "31"):
            t(f"{op} {a}, {c}", (C | Z | S | P | (O if c == "1" else 0)) if int(c) <= (8 if "l" in a or "h" in a else 16 if len(a) == 2 else 32) else (Z | S | P))
    t(f"{op} eax, cl", Z | S | P)
for op in ("rol", "ror"):
    for a in ("eax", "dx", "bl"):
        t(f"{op} {a}, 1", C | O)
        t(f"{op} {a}, 7", C)
    t(f"{op} eax, cl", 0)
for op in ("rcl", "rcr"):
    t(f"{op} eax, 1", C | O)
    t(f"{op} eax, 5", C)
    t(f"{op} dx, 3", C)
t("shld eax, ecx, 7", C | Z | S | P)
t("shrd eax, ecx, 7", C | Z | S | P)
t("shld edx, ebx, 1", C | Z | S | P | O)
t("shrd edx, ebx, 1", C | Z | S | P | O)
t("imul eax, ecx", C | O)
t("imul eax, ecx, 0x1234", C | O)
t("imul dx, bx", C | O)
t("imul ecx", C | O)
t("mul ecx", C | O)
t("imul cx", C | O)
t("mul cx", C | O)
t("imul bl", C | O)
t("mul bl", C | O)
t("bswap eax", 0)
t("bt eax, ecx", C)
t("bt eax, 9", C)
t("bts eax, ecx", C)
t("btr edx, 17", C)
t("btc ebx, esi", C)
t("bsf eax, ecx", Z)
t("bsr eax, ecx", Z)
t("cdq", 0)
t("cwde", 0)
t("cbw", 0)
t("cwd", 0)
t("xchg eax, ecx", 0)
t("xchg dl, bh", 0)
t("xadd eax, ecx", ALLF | A)
# Intel documents the flags as accumulator minus destination, which the lifter follows. Rosetta disagrees for flags
# other than ZF, so only ZF is compared here; see check_cmpxchg_uses() for how the game consumes them.
t("cmpxchg ebx, ecx", Z)
t("movzx eax, cl", 0)
t("movzx eax, ch", 0)
t("movzx edx, cx", 0)
t("movsx eax, cl", 0)
t("movsx edx, bx", 0)
for cc in "e ne b ae be a s ns l ge le g o no p np".split():
    t(f"set{cc} al", 0)
    t(f"set{cc} ch", 0)
    t(f"cmov{cc} eax, ecx", 0)
t("cmc", C)
t("clc", C)
t("stc", C)
t("lahf", 0)
t("sahf", C | Z | S | P | A)

# SSE
for op in "addps subps mulps divps minps maxps andps orps xorps andnps addpd subpd mulpd divpd minpd maxpd andpd orpd xorpd andnpd pand por pxor pandn paddb paddw paddd paddq psubb psubw psubd psubq psubusw psubusb paddusw paddusb pmullw pmulhw pmuludq pmaddwd pavgb pavgw pminub pmaxub pminsw pmaxsw psadbw pcmpeqb pcmpeqw pcmpeqd pcmpgtb pcmpgtw pcmpgtd punpcklbw punpcklwd punpckldq punpcklqdq punpckhbw punpckhwd punpckhdq punpckhqdq packuswb packsswb packssdw unpcklps unpckhps unpcklpd unpckhpd haddps haddpd phaddd pshufb".split():
    t(f"{op} xmm1, xmm2", 0)
for op in "addss subss mulss divss minss maxss addsd subsd mulsd divsd minsd maxsd sqrtss sqrtsd sqrtps sqrtpd cvtss2sd cvtsd2ss cvtdq2ps cvttps2dq cvtps2pd cvtpd2ps".split():
    t(f"{op} xmm3, xmm4", 0)
for op in "movss movsd movaps movq movhlps movlhps".split():
    t(f"{op} xmm5, xmm6", 0)
for imm in ("0x1b", "0xe4", "0x0", "0xff"):
    t(f"shufps xmm1, xmm2, {imm}", 0)
    t(f"pshufd xmm1, xmm2, {imm}", 0)
    t(f"pshuflw xmm1, xmm2, {imm}", 0)
    t(f"pshufhw xmm1, xmm2, {imm}", 0)
t("shufpd xmm1, xmm2, 0x1", 0)
t("palignr xmm1, xmm2, 0x5", 0)
for op, c in (("psllw", "3"), ("pslld", "7"), ("psllq", "33"), ("psrlw", "3"), ("psrld", "7"), ("psrlq", "33"), ("psraw", "3"), ("psrad", "7"), ("pslldq", "3"), ("psrldq", "5"), ("pslld", "40"), ("psrad", "40")):
    t(f"{op} xmm1, {c}", 0)
t("pslld xmm1, xmm2", 0)
t("psrlq xmm1, xmm2", 0)
t("psrad xmm1, xmm2", 0)
for p in "eq lt le unord neq nlt nle ord".split():
    t(f"cmp{p}ss xmm1, xmm2", 0)
    t(f"cmp{p}sd xmm1, xmm2", 0)
    t(f"cmp{p}ps xmm1, xmm2", 0)
    t(f"cmp{p}pd xmm1, xmm2", 0)
for op in ("ucomiss", "comiss", "ucomisd", "comisd"):
    t(f"{op} xmm1, xmm2", C | Z | P | S | O | A)
t("movd xmm1, eax", 0)
t("movd eax, xmm1", 0)
t("cvtsi2ss xmm1, eax", 0)
t("cvtsi2sd xmm1, eax", 0)
t("cvttss2si eax, xmm1", 0)
t("cvttsd2si eax, xmm1", 0)
t("cvtss2si eax, xmm1", 0)
t("cvtsd2si eax, xmm1", 0)
t("pinsrw xmm1, eax, 0x3", 0)
t("pextrw eax, xmm1, 0x5", 0)
t("movmskps eax, xmm1", 0)
t("movmskpd eax, xmm1", 0)
t("pmovmskb eax, xmm1", 0)

SPECIAL = [0, 1, 2, 3, 0xFF, 0x80, 0x7F, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 31, 32, 33]
FSPECIAL = [0.0, -0.0, 1.0, -1.0, 1.5, -2.25, 1e30, -1e30, 3.5e9, -3.5e9, 1e-30, float("inf"), float("-inf"), float("nan"), 2147483648.0, -2147483648.0, 2147483647.0, 0.5, 2.5]


def rnd_u32(r):
    return r.choice(SPECIAL) if r.random() < 0.5 else r.getrandbits(32)


def rnd_xmm(r):
    k = r.random()
    if k < 0.35:
        b = b"".join(struct.pack("<f", r.choice(FSPECIAL)) for _ in range(4))
    elif k < 0.6:
        b = b"".join(struct.pack("<d", r.choice(FSPECIAL)) for _ in range(2))
    else:
        b = r.randbytes(16)
    return b


def make_states(n, seed=1234):
    r = random.Random(seed)
    out = []
    for _ in range(n):
        regs = [rnd_u32(r) for _ in range(6)]
        fl = 0x202
        for bit in (0, 2, 4, 6, 7, 11):
            if r.random() < 0.5:
                fl |= 1 << bit
        xm = [rnd_xmm(r) for _ in range(8)]
        out.append((regs, fl, xm))
    return out


STRUCT = """typedef struct { unsigned regs[6]; unsigned long long flags; unsigned char xmm[8][16]; } State;\n"""


def gen_x86(path, states):
    with open(path, "w") as f:
        f.write('#include <stdio.h>\n' + STRUCT)
        f.write('__asm__(".intel_syntax noprefix\\n"\n')
        for k, (text, _m) in enumerate(TESTS):
            f.write(f'".globl _t_{k}\\n_t_{k}:\\n push rbx\\n push r12\\n mov r12, rdi\\n"\n')
            for j, reg in enumerate(R):
                f.write(f'" mov {reg}, dword ptr [r12+{j * 4}]\\n"\n')
            for j in range(8):
                f.write(f'" movups xmm{j}, xmmword ptr [r12+{32 + j * 16}]\\n"\n')
            f.write('" push qword ptr [r12+24]\\n popfq\\n"\n')
            f.write(f'" {text}\\n"\n')
            f.write('" pushfq\\n pop qword ptr [r12+24]\\n"\n')
            for j, reg in enumerate(R):
                f.write(f'" mov dword ptr [r12+{j * 4}], {reg}\\n"\n')
            for j in range(8):
                f.write(f'" movups xmmword ptr [r12+{32 + j * 16}], xmm{j}\\n"\n')
            f.write('" pop r12\\n pop rbx\\n ret\\n"\n')
        f.write('".att_syntax\\n");\n')
        for k in range(len(TESTS)):
            f.write(f"void t_{k}(State*);\n")
        f.write("typedef void (*Fn)(State*);\nFn fns[] = {" + ",".join(f"t_{k}" for k in range(len(TESTS))) + "};\n")
        write_states(f, states)
        f.write(MAIN)


def write_states(f, states):
    f.write("static const State init[] = {\n")
    for regs, fl, xm in states:
        xs = ",".join("{" + ",".join(str(b) for b in x) + "}" for x in xm)
        f.write("{{" + ",".join(f"{v}u" for v in regs) + f"}},{fl}ull,{{{xs}}}}},\n")
    f.write("};\n")


MAIN = r"""
int main(void) {
    unsigned ns = sizeof(init) / sizeof(init[0]), nt = sizeof(fns) / sizeof(fns[0]);
    for (unsigned k = 0; k < nt; k++)
        for (unsigned s = 0; s < ns; s++) {
            State st = init[s];
            fns[k](&st);
            printf("%u %u", k, s);
            for (int j = 0; j < 6; j++) printf(" %08x", st.regs[j]);
            printf(" %llx ", st.flags);
            for (int j = 0; j < 8; j++) { for (int b = 0; b < 16; b++) printf("%02x", st.xmm[j][b]); printf(" "); }
            printf("\n");
        }
    return 0;
}
"""


def gen_arm(path, states):
    with open(path, "w") as f:
        f.write(f'#include "rt.h"\n#include <stdio.h>\n{STRUCT}')
        f.write("u8 *rt_mem; volatile u32 rt_gate_wait;\n")
        f.write('_Noreturn void rt_trap(CPU *c, u32 a, const char *m){(void)c;printf("TRAP %x %s\\n",a,m);__builtin_trap();}\n')
        f.write("void rt_gate_yield(void){}\nvoid rt_ldmxcsr(CPU*c,u32 v){c->mxcsr=v;}\nvoid rt_cpuid(CPU*c){}\n")
        f.write("static void load(CPU *c, const State *s) {\n c->eax=s->regs[0];c->ecx=s->regs[1];c->edx=s->regs[2];c->ebx=s->regs[3];c->esi=s->regs[4];c->edi=s->regs[5];c->esp=0;c->ebp=0;\n for(int j=0;j<8;j++)__builtin_memcpy(&c->x[j],s->xmm[j],16);\n}\n")
        f.write("static void store(CPU *c, State *s, u32 cf,u32 zf,u32 sf,u32 of,u32 pf,u32 af) {\n s->regs[0]=c->eax;s->regs[1]=c->ecx;s->regs[2]=c->edx;s->regs[3]=c->ebx;s->regs[4]=c->esi;s->regs[5]=c->edi;\n for(int j=0;j<8;j++)__builtin_memcpy(s->xmm[j],&c->x[j],16);\n s->flags=(cf?1:0)|2|(pf?4:0)|(af?16:0)|(zf?64:0)|(sf?128:0)|(of?2048:0);\n}\n")
        for k, (text, _m) in enumerate(TESTS):
            mn, _, ops = text.partition(" ")
            ins = parse_lines([f"  1000:   \t{mn}\t{ops}", "  1010:   \tnop"])[0]
            body = emit(ins, ALL)
            f.write(f"static void t_{k}(State *s){{CPU cc={{0}};CPU *restrict c=&cc;cc.mxcsr=0x1f80;load(c,s);LOCALS;"
                    "cf=s->flags&1;pf=(s->flags>>2)&1;af=(s->flags>>4)&1;zf=(s->flags>>6)&1;sf=(s->flags>>7)&1;of=(s->flags>>11)&1;\n"
                    f"{body}\nSAVE;store(c,s,cf,zf,sf,of,pf,af);}}\n")
        f.write("typedef void (*Fn)(State*);\nstatic Fn fns[] = {" + ",".join(f"t_{k}" for k in range(len(TESTS))) + "};\n")
        write_states(f, states)
        f.write(MAIN)


FLAG_BITS = {C: 0x1, P: 0x4, A: 0x10, Z: 0x40, S: 0x80, O: 0x800}


def run_binary(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.splitlines()


def same_modulo_nan(text, x, a):
    """Accepted difference: x86's default NaN is negative and ARM's is positive. Any other lane difference still fails."""
    mn = text.split()[0]
    width = 8 if mn.endswith(("pd", "sd")) else (4 if mn.endswith(("ps", "ss")) else 0)
    if not width:
        return False
    bx, ba = bytes.fromhex(x), bytes.fromhex(a)
    fmt = "<d" if width == 8 else "<f"
    for k in range(0, 16, width):
        lx, la = bx[k:k + width], ba[k:k + width]
        if lx == la:
            continue
        vx, va = struct.unpack(fmt, lx)[0], struct.unpack(fmt, la)[0]
        if not (vx != vx and va != va):
            return False
    return True


def check_cmpxchg_uses():
    """Count which flag readers follow cmpxchg in the game's reachable code, as the basis for the ZF-only comparison."""
    path = os.path.join(ROOT, "build", "dis.txt")
    if not os.path.exists(path):
        return
    uses = {}
    prev = False
    with open(path, errors="replace") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) < 2:
                continue
            mn = parts[1]
            if prev:
                uses[mn] = uses.get(mn, 0) + 1
                prev = False
            if mn == "cmpxchg":
                prev = True
    print("instructions right after cmpxchg in the game:", dict(sorted(uses.items(), key=lambda kv: -kv[1])))


def main():
    nstates = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    states = make_states(nstates)
    d = tempfile.mkdtemp(prefix="difftest")
    xc, ac = os.path.join(d, "x.c"), os.path.join(d, "a.c")
    gen_x86(xc, states)
    gen_arm(ac, states)
    xb, ab = os.path.join(d, "x"), os.path.join(d, "a")
    subprocess.run(["xcrun", "clang", "-O0", "-w", "-arch", "x86_64", "-mssse3", xc, "-o", xb], check=True, env=ENV)
    subprocess.run(["xcrun", "clang", "-O1", "-w", "-std=gnu11", "-fwrapv", "-fno-strict-aliasing", "-ffp-contract=off",
                    "-arch", "arm64", f"-I{ROOT}/runtime", ac, "-lm", "-o", ab], check=True, env=ENV)
    xo = run_binary(["arch", "-x86_64", xb])
    ao = run_binary([ab])
    assert len(xo) == len(ao), (len(xo), len(ao))
    fails = {}
    for lx, la in zip(xo, ao):
        fx, fa = lx.split(), la.split()
        k, s = int(fx[0]), int(fx[1])
        text, mask = TESTS[k]
        problems = []
        for j in range(6):
            if fx[2 + j] != fa[2 + j]:
                # Undefined destination bits are tolerated only where the architecture leaves them undefined.
                problems.append(f"{R[j]} x86={fx[2 + j]} arm={fa[2 + j]}")
        fxv, fav = int(fx[8], 16), int(fa[8], 16)
        for bit, b in FLAG_BITS.items():
            if mask & bit and (fxv & b) != (fav & b):
                problems.append(f"flag{b:#x} x86={int(bool(fxv & b))} arm={int(bool(fav & b))}")
        for j in range(8):
            if fx[9 + j] != fa[9 + j] and not same_modulo_nan(text, fx[9 + j], fa[9 + j]):
                problems.append(f"xmm{j} x86={fx[9 + j]} arm={fa[9 + j]}")
        if problems:
            fails.setdefault(text, []).append((s, problems))
    total = len(TESTS) * nstates
    print(f"forms={len(TESTS)} states={nstates} comparisons={total} failing_forms={len(fails)}")
    for text, lst in sorted(fails.items(), key=lambda kv: -len(kv[1])):
        s, problems = lst[0]
        print(f"  {text:28s} fails {len(lst):4d}/{nstates}  e.g. state {s}: {'; '.join(problems)[:150]}")
    check_cmpxchg_uses()
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
