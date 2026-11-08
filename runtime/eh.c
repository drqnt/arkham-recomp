// C++ exceptions for translated code. Unreal uses them for ordinary control flow: StaticLoadObject throws a string and
// catches it itself. Guest frames are unwound with the game's own __eh_frame and LSDA tables. Each landing pad is a
// separately translated entry point that runs on top of the current native stack; when a catch handler's function
// finally returns, libunwind drops the native stack back to that function's native caller.
#include "shim.h"
#include "shim_reg.h"

#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern const unsigned rt_eh_frame_addr, rt_eh_frame_size;
u32 rt_find_base(u32 ti, u32 obj, u32 dst);

// ---------- reading DW_EH_PE encoded values from guest memory ----------
static u32 rd_uleb(u32 *a) {
    u32 r = 0, s = 0;
    u8 b;
    do {
        b = G8(*a);
        (*a)++;
        r |= (u32)(b & 0x7F) << s;
        s += 7;
    } while (b & 0x80);
    return r;
}
static i32 rd_sleb(u32 *a) {
    i32 r = 0;
    u32 s = 0;
    u8 b;
    do {
        b = G8(*a);
        (*a)++;
        r |= (i32)((u32)(b & 0x7F) << s);
        s += 7;
    } while (b & 0x80);
    if (s < 32 && (b & 0x40)) r |= -(1 << s);
    return r;
}
static u32 rd_enc(u32 *a, u8 enc) {
    if (enc == 0xFF) return 0;
    u32 at = *a, v;
    switch (enc & 0x0F) {
        case 0x00: case 0x03: case 0x0B: v = G32(at); *a += 4; break;
        case 0x01: v = rd_uleb(a); break;
        case 0x09: v = (u32)rd_sleb(a); break;
        case 0x02: v = G16(at); *a += 2; break;
        case 0x0A: v = (u32)(i32)(i16)G16(at); *a += 2; break;
        case 0x04: case 0x0C: v = G32(at); *a += 8; break;
        default: fprintf(stderr, "*** unknown eh encoding %#x\n", enc); _Exit(76);
    }
    if ((enc & 0x70) == 0x10 && v) v += at;
    if ((enc & 0x80) && v) v = G32(v);
    return v;
}

// ---------- the frame table ----------
typedef struct { u32 begin, end, lsda, cfi, cfi_end, cie; } Fde;
typedef struct { u32 addr, code_align, init, init_end; i32 data_align; u8 fde_enc, lsda_enc; } Cie;
static Fde *fdes;
static u32 nfdes;
static Cie cies[8192];
static u32 ncies;

static Cie *cie_at(u32 addr) {
    for (u32 i = 0; i < ncies; i++)
        if (cies[i].addr == addr) return &cies[i];
    return NULL;
}
static int fde_cmp(const void *a, const void *b) {
    u32 x = ((const Fde *)a)->begin, y = ((const Fde *)b)->begin;
    return x < y ? -1 : x > y;
}
static void load_frames_once(void) {
    u32 a = rt_eh_frame_addr, end = a + rt_eh_frame_size;
    fdes = calloc(rt_eh_frame_size / 16 + 1, sizeof(Fde));
    u32 last_cie_addr = 0;
    Cie *last_cie = NULL;
    while (a < end) {
        u32 len = G32(a);
        if (!len) break;
        u32 idf = G32(a + 4), body = a + 8, nxt = a + 4 + len;
        if (idf == 0) {
            Cie *k = &cies[ncies++];
            k->addr = a;
            u32 p = body + 1;
            const char *aug = GS(p);
            p += (u32)strlen(aug) + 1;
            k->code_align = rd_uleb(&p);
            k->data_align = rd_sleb(&p);
            rd_uleb(&p);
            k->fde_enc = 0;
            k->lsda_enc = 0xFF;
            if (aug[0] == 'z') {
                u32 alen = rd_uleb(&p);
                u32 data_end = p + alen;
                for (const char *ch = aug + 1; *ch; ch++) {
                    if (*ch == 'P') {
                        u8 e = G8(p++);
                        rd_enc(&p, (u8)(e & 0x7F));
                    } else if (*ch == 'L') {
                        k->lsda_enc = G8(p++);
                    } else if (*ch == 'R') {
                        k->fde_enc = G8(p++);
                    }
                }
                p = data_end;
            }
            k->init = p;
            k->init_end = nxt;
        } else {
            u32 ca = a + 4 - idf;
            Cie *k = (ca == last_cie_addr) ? last_cie : cie_at(ca);
            last_cie_addr = ca;
            last_cie = k;
            u32 p = body;
            u32 begin = rd_enc(&p, k->fde_enc);
            u32 range = rd_enc(&p, (u8)(k->fde_enc & 0x0F));
            u32 alen = rd_uleb(&p);
            u32 data_end = p + alen;
            u32 lsda = 0;
            if (alen && k->lsda_enc != 0xFF) lsda = rd_enc(&p, k->lsda_enc);
            fdes[nfdes++] = (Fde){begin, begin + range, lsda, data_end, nxt, ca};
        }
        a = nxt;
    }
    qsort(fdes, nfdes, sizeof(Fde), fde_cmp);
}
static void load_frames(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_frames_once);
}
static Fde *find_fde(u32 pc) {
    u32 lo = 0, hi = nfdes;
    while (lo < hi) {
        u32 m = (lo + hi) / 2;
        if (pc < fdes[m].begin) hi = m;
        else if (pc >= fdes[m].end) lo = m + 1;
        else return &fdes[m];
    }
    return NULL;
}

// ---------- CFI: register numbers are the Darwin i386 ones (4 = ebp, 5 = esp) ----------
enum { R_EBX = 3, R_EBP = 4, R_ESP = 5, R_ESI = 6, R_EDI = 7, R_EIP = 8, NREG = 9 };
typedef struct { u32 r[NREG]; } Ctx;
typedef struct { int cfa_reg; i32 cfa_off; u8 saved[NREG]; i32 off[NREG]; } Rules;

static void run_cfi(u32 p, u32 end, const Cie *k, u32 *loc, u32 pc, Rules *ru, const Rules *init) {
    Rules stack[8];
    int sp = 0;
    while (p < end && *loc <= pc) {
        u8 op = G8(p++);
        u8 hi = op & 0xC0, lo = op & 0x3F;
        if (hi == 0x40) {
            *loc += lo * k->code_align;
            if (*loc > pc) return;
        } else if (hi == 0x80) {
            i32 o = (i32)rd_uleb(&p) * k->data_align;
            if (lo < NREG) {
                ru->saved[lo] = 1;
                ru->off[lo] = o;
            }
        } else if (hi == 0xC0) {
            if (lo < NREG && init) {
                ru->saved[lo] = init->saved[lo];
                ru->off[lo] = init->off[lo];
            }
        } else {
            switch (op) {
                case 0x00: break;
                case 0x01: *loc = rd_enc(&p, k->fde_enc); break;
                case 0x02: *loc += G8(p) * k->code_align; p += 1; break;
                case 0x03: *loc += G16(p) * k->code_align; p += 2; break;
                case 0x04: *loc += G32(p) * k->code_align; p += 4; break;
                case 0x05: {
                    u32 rg = rd_uleb(&p);
                    i32 o = (i32)rd_uleb(&p) * k->data_align;
                    if (rg < NREG) { ru->saved[rg] = 1; ru->off[rg] = o; }
                    break;
                }
                case 0x06: case 0x07: case 0x08: {
                    u32 rg = rd_uleb(&p);
                    if (rg < NREG) ru->saved[rg] = (op == 0x06 && init) ? init->saved[rg] : 0;
                    break;
                }
                case 0x09: rd_uleb(&p); rd_uleb(&p); break;
                case 0x0A: if (sp < 8) stack[sp++] = *ru; break;
                case 0x0B: if (sp > 0) *ru = stack[--sp]; break;
                case 0x0C: ru->cfa_reg = (int)rd_uleb(&p); ru->cfa_off = (i32)rd_uleb(&p); break;
                case 0x0D: ru->cfa_reg = (int)rd_uleb(&p); break;
                case 0x0E: ru->cfa_off = (i32)rd_uleb(&p); break;
                case 0x11: {
                    u32 rg = rd_uleb(&p);
                    i32 o = rd_sleb(&p) * k->data_align;
                    if (rg < NREG) { ru->saved[rg] = 1; ru->off[rg] = o; }
                    break;
                }
                case 0x12: ru->cfa_reg = (int)rd_uleb(&p); ru->cfa_off = rd_sleb(&p) * k->data_align; break;
                case 0x13: ru->cfa_off = rd_sleb(&p) * k->data_align; break;
                case 0x2E: rd_uleb(&p); break;
                default:
                    fprintf(stderr, "*** unknown CFI op %#x\n", op);
                    _Exit(76);
            }
        }
    }
}

// Step to the caller. Returns 0 at the bottom of the guest stack.
static int unwind_step(Ctx *x) {
    u32 pc = x->r[R_EIP] - 1;
    if (x->r[R_EIP] == RT_HOST_RETURN || x->r[R_EIP] < 0x1000u) return 0;
    Fde *f = find_fde(pc);
    if (!f) {
        // No unwind record: assume the standard ebp frame.
        u32 ebp = x->r[R_EBP];
        if (ebp < 0x1000u) return 0;
        x->r[R_EIP] = G32(ebp + 4);
        x->r[R_ESP] = ebp + 8;
        x->r[R_EBP] = G32(ebp);
        return 1;
    }
    Cie *k = cie_at(f->cie);
    Rules init, ru;
    memset(&init, 0, sizeof init);
    u32 loc = 0;
    run_cfi(k->init, k->init_end, k, &loc, 0xFFFFFFFFu, &init, NULL);
    ru = init;
    loc = f->begin;
    run_cfi(f->cfi, f->cfi_end, k, &loc, pc, &ru, &init);
    u32 cfa = x->r[ru.cfa_reg < NREG ? ru.cfa_reg : R_ESP] + (u32)ru.cfa_off;
    Ctx n = *x;
    for (int i = 0; i < NREG; i++)
        if (ru.saved[i]) n.r[i] = G32(cfa + (u32)ru.off[i]);
    n.r[R_ESP] = cfa;
    *x = n;
    return 1;
}

// ---------- LSDA ----------
typedef struct { u32 lp; u32 action; u32 actions_base; u32 ttype_base; u8 tt_enc; } CallSite;

static int lsda_lookup(const Fde *f, u32 ip, CallSite *cs) {
    u32 a = f->lsda;
    u8 start_enc = G8(a++);
    u32 lp_start = f->begin;
    if (start_enc != 0xFF) lp_start = rd_enc(&a, start_enc);
    cs->tt_enc = G8(a++);
    cs->ttype_base = 0;
    if (cs->tt_enc != 0xFF) {
        u32 off = rd_uleb(&a);
        cs->ttype_base = a + off;
    }
    u8 cs_enc = G8(a++);
    u32 cs_len = rd_uleb(&a);
    u32 cs_end = a + cs_len;
    cs->actions_base = cs_end;
    u32 rel = ip - f->begin;
    while (a < cs_end) {
        u32 s = rd_enc(&a, cs_enc), l = rd_enc(&a, cs_enc), lp = rd_enc(&a, cs_enc);
        u32 act = rd_uleb(&a);
        if (rel >= s && rel < s + l) {
            cs->lp = lp ? lp_start + lp : 0;
            cs->action = act;
            return 1;
        }
    }
    cs->lp = 0;
    cs->action = 0;
    return 0;
}

// ---------- exception records ----------
typedef struct Exc {
    u32 obj, ti, dest, adjusted;
    int handlers;
    Ctx ctx;
    u32 handler_begin;
    int handler_frame, frame;
    i32 handler_filter;
    int have_handler;
    unw_cursor_t resume;
    struct Exc *next_caught;
    u64 thrown_ns;  // when it was thrown, for the note at its handler (phase2)
    int cleanups;   // cleanup landing pads run on the way
} Exc;
#define EXC_TAG 0xE4C0DE11u
static __thread Exc *caught_top;
static __thread int uncaught;

// The record travels in the 128 bytes reserved in front of the thrown object.
static Exc *exc_of(u32 obj) {
    if (G32(obj - 16) == EXC_TAG) return *(Exc **)GP(obj - 12);
    return NULL;
}
static Exc *exc_new(u32 obj) {
    Exc *e = calloc(1, sizeof *e);
    e->obj = obj;
    G32(obj - 16) = EXC_TAG;
    *(Exc **)GP(obj - 12) = e;
    return e;
}

static const char *ti_name(u32 ti) {
    u32 base = 0xEF000000u;
    if (ti >= base && ti < base + rt_import_n * 64u) {
        const char *n = rt_import_names[(ti - base) / 64u];
        return !strncmp(n, "__ZTI", 5) ? n + 5 : n;
    }
    u32 nm = G32(ti + 4);
    if (!nm) return "?";
    const char *s = GS(nm);
    return s[0] == '*' ? s + 1 : s;
}
// Does a handler for type `want` accept this exception? Sets the pointer the handler receives.
static int type_matches(u32 want, u32 thrown, u32 obj, u32 *adjusted) {
    if (!want) {
        *adjusted = obj;
        return 1;
    }
    const char *w = ti_name(want), *t = ti_name(thrown);
    if (w[0] == 'P' && t[0] == 'P') {
        // Pointers: a handler may add const, and a void pointer accepts anything.
        const char *wb = w + 1, *tb = t + 1;
        while (*wb == 'K') wb++;
        while (*tb == 'K') tb++;
        if (!strcmp(wb, tb) || !strcmp(wb, "v")) {
            *adjusted = G32(obj);
            return 1;
        }
        return 0;
    }
    if (!strcmp(w, t)) {
        *adjusted = obj;
        return 1;
    }
    u32 base = 0xEF000000u;
    if (thrown < base) {
        u32 r = rt_find_base(thrown, obj, want);
        if (r) {
            *adjusted = r;
            return 1;
        }
    }
    return 0;
}
// Walk one call site's action chain. Returns 1 with the filter for a catch, 2 for a cleanup, 0 for neither.
static int eval_actions(const CallSite *cs, const Exc *e, i32 *filter, u32 *adjusted) {
    if (!cs->lp) return 0;
    if (!cs->action) {
        *filter = 0;
        return 2;
    }
    int has_cleanup = 0;
    u32 a = cs->actions_base + cs->action - 1;
    for (;;) {
        u32 at = a;
        i32 f = rd_sleb(&a);
        u32 next_pos = a;
        i32 next = rd_sleb(&a);
        if (f > 0) {
            u32 sz = 4;
            u8 fmt = cs->tt_enc & 0x0F;
            if (fmt == 0x02 || fmt == 0x0A) sz = 2;
            u32 ta = cs->ttype_base - (u32)f * sz;
            u32 want = rd_enc(&ta, cs->tt_enc);
            if (type_matches(want, e->ti, e->obj, adjusted)) {
                *filter = f;
                return 1;
            }
        } else if (f == 0) {
            has_cleanup = 1;
        }
        (void)at;
        if (!next) break;
        a = next_pos + (u32)next;
    }
    *filter = 0;
    return has_cleanup ? 2 : 0;
}

// ---------- locating the native frame to return into ----------
typedef struct { void *fn; u32 addr; } RevEntry;
static RevEntry *rev;
static int rev_cmp(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const RevEntry *)a)->fn, y = (uintptr_t)((const RevEntry *)b)->fn;
    return x < y ? -1 : x > y;
}
static void build_rev(void) {
    rev = malloc(rt_fntab_n * sizeof *rev);
    for (unsigned i = 0; i < rt_fntab_n; i++) rev[i] = (RevEntry){(void *)rt_fntab[i].fn, rt_fntab[i].addr};
    qsort(rev, rt_fntab_n, sizeof *rev, rev_cmp);
}
static u32 guest_of_native(uintptr_t ip) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, build_rev);
    // The linker folds neighbouring unwind entries, so libunwind's start_ip is not the function: bisect the address.
    u32 lo = 0, hi = rt_fntab_n;
    while (lo < hi) {
        u32 m = (lo + hi) / 2;
        if ((uintptr_t)rev[m].fn <= ip) lo = m + 1;
        else hi = m;
    }
    if (!lo || ip - (uintptr_t)rev[lo - 1].fn > 0x100000u) return 0;
    return rev[lo - 1].addr;
}
static u32 function_identity(u32 guest) {
    Fde *f = find_fde(guest);
    return f ? f->begin : guest;
}
// A running catch handler sits on top of frames that are already dead. Each run records where its function's caller
// is, so a later walk can step over the dead part.
typedef struct Run { struct Run *prev; unw_cursor_t resume; } Run;
static __thread Run *run_top;

// What throwing costs, in the log every 5 s while the game throws: how many, the time finding the handler in the game's own tables (phase 1) and the
// time finding its native frame with libunwind, with the number of steps that took.
static _Atomic u64 eh_throws, eh_steps, eh_ns_find, eh_ns_native;
static void eh_report(void) {
    static _Atomic u64 last_ns, told;
    u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), n = atomic_load(&eh_throws);
    if (now - atomic_load(&last_ns) < 5000000000ull || n == atomic_load(&told)) return;
    atomic_store(&last_ns, now);
    atomic_store(&told, n);
    fprintf(stderr, "[eh] %llu throws so far: %.0f ms finding handlers, %.0f ms finding their native frames in %llu steps\n", n,
            (double)atomic_load(&eh_ns_find) / 1e6, (double)atomic_load(&eh_ns_native) / 1e6, atomic_load(&eh_steps));
}
// Also once a second from the frame rate line (mtl_device.m), since the game may throw a burst at startup and then none for a long time.
void rt_eh_report(void) { eh_report(); }
// Native frames mirror guest frames, so the k-th native frame of the handler's function is the handler's frame.
static int find_native_caller(unw_cursor_t *out, u32 identity, int occurrence) {
    unw_context_t uc;
    unw_cursor_t cur;
    unw_getcontext(&uc);
    unw_init_local(&cur, &uc);
    Run *r = run_top;
    int seen = 0;
    while (unw_step(&cur) > 0) {
        atomic_fetch_add(&eh_steps, 1);
        unw_word_t sp = 0, ip = 0;
        unw_get_reg(&cur, UNW_REG_SP, &sp);
        if (r && sp > (uintptr_t)r) {
            cur = r->resume;
            r = r->prev;
        }
        unw_get_reg(&cur, UNW_REG_IP, &ip);
        u32 g = guest_of_native((uintptr_t)ip - 1);
        if (g && function_identity(g) == identity && ++seen == occurrence) {
            if (unw_step(&cur) <= 0) return 0;
            *out = cur;
            return 1;
        }
    }
    return 0;
}

// ---------- going on in a frame libunwind found ----------
// What unw_resume does, the arm64 way: the frame's callee-saved registers, frame pointer, stack and program counter, then a jump there, as if
// the call it made had just returned. The phone's unw_resume (iOS 27) took 2.5 s at the startup's one throw, all inside libunwind, against
// 0.1 s for the whole stretch on the Mac (v19's log). ARKHAM_EH_UNW_RESUME=1 uses libunwind's again.
#if defined(__aarch64__)
__attribute__((noreturn, naked)) static void jump_to_regs(const u64 *r) {  // x19..x28, fp, sp, pc, d8..d15
    __asm__ volatile("ldp x19, x20, [x0, #0]\n"
                     "ldp x21, x22, [x0, #16]\n"
                     "ldp x23, x24, [x0, #32]\n"
                     "ldp x25, x26, [x0, #48]\n"
                     "ldp x27, x28, [x0, #64]\n"
                     "ldp x29, x16, [x0, #80]\n"
                     "ldr x17, [x0, #96]\n"
                     "ldp d8, d9, [x0, #104]\n"
                     "ldp d10, d11, [x0, #120]\n"
                     "ldp d12, d13, [x0, #136]\n"
                     "ldp d14, d15, [x0, #152]\n"
                     "mov sp, x16\n"
                     "br x17\n");
}
#endif
_Noreturn static void resume_at(unw_cursor_t *cur) {
#if defined(__aarch64__)
    static int libunwind = -1;
    if (libunwind < 0) libunwind = getenv("ARKHAM_EH_UNW_RESUME") != NULL;
    if (!libunwind) {
        u64 r[21];
        int ok = 1;
        for (int i = 0; i < 10; i++) {
            unw_word_t v = 0;
            ok &= unw_get_reg(cur, UNW_ARM64_X19 + i, &v) == 0;
            r[i] = v;
        }
        unw_word_t fp = 0, sp = 0, pc = 0;
        ok &= unw_get_reg(cur, UNW_ARM64_FP, &fp) == 0;
        ok &= unw_get_reg(cur, UNW_REG_SP, &sp) == 0;
        ok &= unw_get_reg(cur, UNW_REG_IP, &pc) == 0;
        r[10] = fp, r[11] = sp, r[12] = pc;
        for (int i = 0; i < 8; i++) {
            unw_fpreg_t d = 0;
            ok &= unw_get_fpreg(cur, UNW_ARM64_D8 + i, &d) == 0;
            memcpy(&r[13 + i], &d, 8);
        }
        if (ok && sp && pc && !(sp & 15)) jump_to_regs(r);
    }
#endif
    unw_resume(cur);
    abort();
}

// ---------- the two phases ----------
static void set_guest_regs(CPU *c, const Ctx *x, u32 eax, u32 edx) {
    c->ebx = x->r[R_EBX];
    c->ebp = x->r[R_EBP];
    c->esp = x->r[R_ESP];
    c->esi = x->r[R_ESI];
    c->edi = x->r[R_EDI];
    c->eax = eax;
    c->edx = edx;
}

// Run landing pads from e->ctx upward until the handler. Doesn't return.
_Noreturn static void phase2(CPU *c, Exc *e) {
    for (;;) {
        u32 pc = e->ctx.r[R_EIP] - 1;
        Fde *f = find_fde(pc);
        CallSite cs;
        if (f && f->lsda && lsda_lookup(f, pc, &cs) && cs.lp) {
            i32 filter = 0;
            u32 adjusted = 0;
            int kind = eval_actions(&cs, e, &filter, &adjusted);
            int is_handler = e->have_handler && e->frame == e->handler_frame;
            if (is_handler) {
                set_guest_regs(c, &e->ctx, e->obj, (u32)e->handler_filter);
                // Runs below the frame we will return to belong to frames this exception has unwound.
                Run run;
                run.resume = e->resume;
                unw_word_t target = 0;
                unw_get_reg(&run.resume, UNW_REG_SP, &target);
                run.prev = run_top;
                u64 w0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), steps = 0;
                while (run.prev && (uintptr_t)run.prev < target) {
                    run.prev = run.prev->prev;
                    steps++;
                }
                // The startup's one throw took 2.5 s on the phone before this point, all of it in phase2's own code (v15/v16 profiles, 2026-11-08):
                // what each of the first throws spent on the way to its handler.
                static _Atomic int told;
                if (atomic_fetch_add(&told, 1) < 8) {
                    u64 w1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                    fprintf(stderr, "[eh] a throw of %s reaches its handler %d frames up, %.1f ms after it was thrown, %d cleanups on the way; the walk over running handlers: %llu steps, %.1f ms\n",
                            ti_name(e->ti), e->frame, (double)(w0 - e->thrown_ns) / 1e6, e->cleanups, steps, (double)(w1 - w0) / 1e6);
                }
                run_top = &run;
                u64 thrown = e->thrown_ns;  // the catch's end frees the record
                rt_dispatch(c, cs.lp);
                // The function holding the handler has returned: continue in its native caller.
                run_top = run.prev;
                // The startup's 2.5 s on the phone were libunwind's unw_resume here: the handler's function returned 8 ms after the throw and
                // the game went on 2.6 s later (v19's log). resume_at does the same itself.
                static _Atomic int told_back;
                if (atomic_fetch_add(&told_back, 1) < 8)
                    fprintf(stderr, "[eh] the handler's function returned %.1f ms after the throw; on to its caller\n",
                            (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - thrown) / 1e6);
                resume_at(&run.resume);
            }
            if (kind == 2) {
                e->cleanups++;
                set_guest_regs(c, &e->ctx, e->obj, 0);
                rt_dispatch(c, cs.lp);
                rt_trap(c, cs.lp, "cleanup landing pad returned");
            }
        }
        if (!unwind_step(&e->ctx)) {
            fprintf(stderr, "\n*** uncaught guest exception of type %s\n", ti_name(e->ti));
            rt_trap(c, pc, "uncaught exception");
        }
        e->frame++;
    }
}

static void raise_exception(CPU *c, Exc *e) {
    load_frames();
    // The frame that called __cxa_throw: on entry to a shim, [esp] is the return address.
    Ctx top;
    memset(&top, 0, sizeof top);
    top.r[R_EBX] = c->ebx;
    top.r[R_EBP] = c->ebp;
    top.r[R_ESI] = c->esi;
    top.r[R_EDI] = c->edi;
    top.r[R_ESP] = c->esp + 4;
    top.r[R_EIP] = G32(c->esp);
    e->ctx = top;
    e->frame = 0;
    e->have_handler = 0;
    uncaught++;
    u64 t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    e->thrown_ns = t0;
    // Phase 1: find the handler without changing anything.
    Ctx x = top;
    int frame = 0;
    u32 identity_of_frame[4096];
    for (;;) {
        u32 pc = x.r[R_EIP] - 1;
        Fde *f = find_fde(pc);
        if (frame < 4096) identity_of_frame[frame] = f ? f->begin : pc;
        CallSite cs;
        if (f && f->lsda && lsda_lookup(f, pc, &cs)) {
            i32 filter = 0;
            u32 adjusted = 0;
            if (eval_actions(&cs, e, &filter, &adjusted) == 1) {
                e->handler_begin = f->begin;
                e->handler_frame = frame;
                e->handler_filter = filter;
                e->adjusted = adjusted;
                e->have_handler = 1;
                break;
            }
        }
        if (!unwind_step(&x)) break;
        frame++;
    }
    if (!e->have_handler) {
        fprintf(stderr, "\n*** no handler for guest exception of type %s\n", ti_name(e->ti));
        rt_trap(c, top.r[R_EIP], "uncaught exception");
    }
    int occurrence = 0;
    for (int i = 0; i <= e->handler_frame && i < 4096; i++)
        if (identity_of_frame[i] == e->handler_begin) occurrence++;
    u64 t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!find_native_caller(&e->resume, e->handler_begin, occurrence)) rt_trap(c, top.r[R_EIP], "no native frame for handler");
    u64 t2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    atomic_fetch_add(&eh_throws, 1);
    atomic_fetch_add(&eh_ns_find, t1 - t0);
    atomic_fetch_add(&eh_ns_native, t2 - t1);
    eh_report();
    phase2(c, e);
}

SHIM(__cxa_throw) {
    Exc *e = exc_new(ARG(0));
    e->ti = ARG(1);
    e->dest = ARG(2);
    if (getenv("ARKHAM_TRACE_EH")) {
        // Unreal throws its error text: a pointer to UTF-16 characters.
        char text[512] = "";
        if (!strcmp(ti_name(e->ti), "Pt") || !strcmp(ti_name(e->ti), "PKt")) {
            u32 at = G32(e->obj);
            size_t n = 0;
            while (at && n < sizeof text - 1 && G16(at + n * 2)) {
                u16 ch = G16(at + n * 2);
                text[n++] = ch < 128 ? (char)ch : '?';
            }
            text[n] = 0;
        } else if (!strcmp(ti_name(e->ti), "i") && c->ebp > 0x1000u) {
            // The engine's fatal error path throws the number 1, with the message still an argument of that function.
            u32 at = G32(c->ebp + 0xc);
            size_t n = 0;
            while (at > 0x1000u && at < 0xE0000000u && n < sizeof text - 1 && G16(at + n * 2)) {
                u16 ch = G16(at + n * 2);
                text[n++] = ch < 128 ? (char)ch : '?';
            }
            text[n] = 0;
        }
        fprintf(stderr, "[eh] throw %s from %08x on thread %p: %s\n", ti_name(e->ti), G32(c->esp), (void *)pthread_self(), text);
    }
    raise_exception(c, e);
}
SHIM(__cxa_rethrow) {
    Exc *e = caught_top;
    if (!e) rt_trap(c, G32(c->esp), "rethrow with no active exception");
    caught_top = e->next_caught;
    e->handlers--;
    raise_exception(c, e);
}
SHIM(_Unwind_Resume) {
    Exc *e = exc_of(ARG(0));
    if (!e) rt_trap(c, G32(c->esp), "_Unwind_Resume of an unknown exception");
    // A cleanup finished: carry on from the frame above the one it ran in.
    if (!unwind_step(&e->ctx)) rt_trap(c, G32(c->esp), "ran out of frames while unwinding");
    e->frame++;
    phase2(c, e);
}
SHIM(__cxa_begin_catch) {
    Exc *e = exc_of(ARG(0));
    if (!e) {
        RET(ARG(0));
        return;
    }
    e->handlers++;
    if (uncaught > 0) uncaught--;
    e->next_caught = caught_top;
    caught_top = e;
    RET(e->adjusted);
}
SHIM(__cxa_end_catch) {
    Exc *e = caught_top;
    if (!e) return;
    if (--e->handlers <= 0) {
        caught_top = e->next_caught;
        u32 obj = e->obj, dest = e->dest;
        if (dest) {
            u32 a[1] = {obj};
            rt_call_guest(c, dest, 1, a);
        }
        G32(obj - 16) = 0;
        free(e);
        rt_guest_free(obj - 128);
    }
}
SHIM(uncaught_exception) { RET(uncaught > 0); }
SHIM(__cxa_free_exception) { rt_guest_free(ARG(0) - 128); }

const ShimReg rt_shims_eh[] = {
    R2("___cxa_throw", __cxa_throw) R2("___cxa_rethrow", __cxa_rethrow) R2("__Unwind_Resume", _Unwind_Resume)
    R2("___cxa_begin_catch", __cxa_begin_catch) R2("___cxa_end_catch", __cxa_end_catch)
    R2("___cxa_free_exception", __cxa_free_exception) R2("__ZSt18uncaught_exceptionv", uncaught_exception)
    {NULL, NULL},
};
