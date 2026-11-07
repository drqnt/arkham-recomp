// Runtime core: guest memory, image loading, dispatch, thread gate, trap reporting, process start.
#include "rt.h"
#include <time.h>
#include <sys/ucontext.h>
#include "shim.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

u8 *rt_mem;
volatile u32 rt_gate_wait;

#define IMP_BASE 0xEF000000u
#define IMP_STEP 64u

// ---------- diagnostics ----------

static void dump_backtrace(void) {
    void *frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
}

static void dump_cpu(CPU *c) {
    fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x\n  esp=%08x ebp=%08x esi=%08x edi=%08x\n", c->eax, c->ecx,
            c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    fprintf(stderr, "  guest stack:");
    for (u32 i = 0; i < 12; i++) {
        u32 a = c->esp + i * 4;
        if (a >= 0x4000u) fprintf(stderr, " %08x", *(u32 *)(rt_mem + a));
    }
    fprintf(stderr, "\n");
    // Name the C++ objects in the registers and near the top of the stack from their RTTI.
    u32 regs[8] = {c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi};
    static const char *const reg_names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    for (int i = 0; i < 8; i++) {
        u32 p = regs[i];
        if (p < 0x04000000u || p >= 0xE0000000u || (p & 3)) continue;
        u32 vt = *(u32 *)(rt_mem + p);
        if (vt < 0x1000u || vt >= 0x04000000u || (vt & 3)) continue;
        u32 ti = *(u32 *)(rt_mem + vt - 4);
        if (ti < 0x1000u || ti >= 0x04000000u) continue;
        u32 nm = *(u32 *)(rt_mem + ti + 4);
        if (nm < 0x1000u || nm >= 0x04000000u) continue;
        fprintf(stderr, "  %s %08x is a %.60s\n", reg_names[i], p, (const char *)rt_mem + nm);
    }
    for (u32 i = 0; i < 40; i++) {
        u32 p = *(u32 *)(rt_mem + c->esp + i * 4);
        if (p < 0x04000000u || p >= 0xE0000000u || (p & 3)) continue;
        u32 vt = *(u32 *)(rt_mem + p);
        if (vt < 0x1000u || vt >= 0x04000000u || (vt & 3)) continue;
        u32 ti = *(u32 *)(rt_mem + vt - 4);
        if (ti < 0x1000u || ti >= 0x04000000u) continue;
        u32 nm = *(u32 *)(rt_mem + ti + 4);
        if (nm < 0x1000u || nm >= 0x04000000u) continue;
        fprintf(stderr, "  [esp+%x] %08x is a %.60s\n", i * 4, p, (const char *)rt_mem + nm);
    }
}

_Noreturn void rt_trap(CPU *c, u32 addr, const char *msg) {
    fprintf(stderr, "\n*** guest trap at %08x: %s\n", addr, msg);
    dump_cpu(c);
    dump_backtrace();
    fflush(stderr);
    _exit(70);
}

_Noreturn void rt_unimpl(CPU *c, u32 addr, const char *msg) {
    fprintf(stderr, "\n*** unimplemented instruction at %08x: %s\n", addr, msg);
    dump_cpu(c);
    dump_backtrace();
    fflush(stderr);
    _exit(71);
}

static __thread CPU *tl_cpu;
CPU *rt_current_cpu(void) { return tl_cpu; }

int rt_no_frame_limit = 1;  // ARKHAM_FRAME_LIMIT=1 keeps the engine's own limit
void rt_input_release(void);
// Load-acquire and store-release fault on a value that crosses a 16-byte boundary. Plain accesses handle those, with fences for
// the ordering. Returns 1 when the faulting instruction was finished here.
// ARKHAM_TRACE_UNALIGNED=1 counts these by place and prints the busiest every few seconds: each one costs microseconds, so a
// loop that keeps landing here should go in UNALIGNED_SITES in tools/lift.py.
#define UT_N 4096u
static struct { u64 pc, n; } ut_table[UT_N];
static int ut_on;
static void ut_count(u64 pc) {
    for (u32 i = (u32)(pc >> 2) % UT_N, tries = 0; tries < 64; i = (i + 1) % UT_N, tries++) {
        u64 cur = __atomic_load_n(&ut_table[i].pc, __ATOMIC_RELAXED);
        if (!cur && __atomic_compare_exchange_n(&ut_table[i].pc, &cur, pc, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) cur = pc;
        if (cur == pc) {
            __atomic_fetch_add(&ut_table[i].n, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}
static void *ut_report(void *arg) {
    (void)arg;
    static u64 last[UT_N];
    u64 last_total = 0;
    for (;;) {
        sleep(5);
        u64 total = 0, delta[UT_N];
        for (u32 i = 0; i < UT_N; i++) {
            u64 n = ut_table[i].n;
            total += n;
            delta[i] = n - last[i];
            last[i] = n;
        }
        if (total == last_total) continue;
        fprintf(stderr, "[unaligned] %llu in all, %llu since last report\n", total, total - last_total);
        last_total = total;
        for (int shown = 0; shown < 14; shown++) {
            u32 best = UT_N;
            for (u32 i = 0; i < UT_N; i++)
                if (delta[i] && (best == UT_N || delta[i] > delta[best])) best = i;
            if (best == UT_N) break;
            Dl_info di = {0};
            const char *nm = dladdr((void *)ut_table[best].pc, &di) && di.dli_sname ? di.dli_sname : "?";
            fprintf(stderr, "[unaligned]   %10llu %s +%llu\n", delta[best], nm, ut_table[best].pc - (u64)di.dli_saddr);
            delta[best] = 0;
        }
    }
    return NULL;
}
static int finish_unaligned(ucontext_t *uc) {
    _STRUCT_ARM_THREAD_STATE64 *st = &uc->uc_mcontext->__ss;
    u32 ins = *(u32 *)st->__pc;
    if (ut_on) ut_count(st->__pc);
    u32 rt = ins & 31, rn = (ins >> 5) & 31;
    int size, load;
    i64 off = 0;
    switch (ins & 0xFFFFFC00u) {
        case 0x78BFC000u: size = 2; load = 1; break;  // ldaprh
        case 0xB8BFC000u: size = 4; load = 1; break;  // ldapr w
        case 0x489FFC00u: size = 2; load = 0; break;  // stlrh
        case 0x889FFC00u: size = 4; load = 0; break;  // stlr w
        default:
            switch (ins & 0xFFE00C00u) {
                case 0x59400000u: size = 2; load = 1; break;  // ldapurh
                case 0x99400000u: size = 4; load = 1; break;  // ldapur w
                case 0x59000000u: size = 2; load = 0; break;  // stlurh
                case 0x99000000u: size = 4; load = 0; break;  // stlur w
                default: return 0;
            }
            off = ((i64)(i32)(ins << 11)) >> 23;
    }
    u64 base = rn == 31 ? st->__sp : rn == 29 ? st->__fp : rn == 30 ? st->__lr : st->__x[rn];
    u8 *p = (u8 *)(base + (u64)off);
    u64 *reg = rt < 29 ? &st->__x[rt] : rt == 29 ? &st->__fp : &st->__lr;
    u32 v = 0;
    if (load) {
        memcpy(&v, p, (size_t)size);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (rt != 31) *reg = v;
    } else {
        if (rt != 31) v = (u32)*reg;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        memcpy(p, &v, (size_t)size);
    }
    st->__pc += 4;
    return 1;
}
static void on_fault(int sig, siginfo_t *si, void *ctx) {
    if (sig == SIGBUS && ctx && finish_unaligned(ctx)) return;
    rt_input_release();  // never leave the player's pointer frozen
    u64 fault = (u64)si->si_addr;
    u64 base = (u64)rt_mem;
    fprintf(stderr, "\n*** signal %d, host fault address %#llx", sig, fault);
    Dl_info di;
    if (dladdr((void *)fault, &di) && di.dli_sname) fprintf(stderr, " (%s in %s)", di.dli_sname, di.dli_fname);
    if (fault >= base && fault < base + (1ull << 32)) fprintf(stderr, " = guest %08x", (u32)(fault - base));
    fprintf(stderr, "\n");
    // A backtrace walks saved frame pointers, so it skips the function that actually faulted: name it from the context.
    ucontext_t *uc = ctx;
    if (uc && uc->uc_mcontext) {
        u64 pc = uc->uc_mcontext->__ss.__pc;
        Dl_info pi;
        if (dladdr((void *)pc, &pi) && pi.dli_sname)
            fprintf(stderr, "  faulted in %s + %llu (pc %#llx)\n", pi.dli_sname, pc - (u64)pi.dli_saddr, pc);
        fprintf(stderr, "  native x0-x3: %llx %llx %llx %llx lr %llx\n", uc->uc_mcontext->__ss.__x[0], uc->uc_mcontext->__ss.__x[1],
                uc->uc_mcontext->__ss.__x[2], uc->uc_mcontext->__ss.__x[3], uc->uc_mcontext->__ss.__lr);
        fprintf(stderr, "  native w8-w17:");
        for (int i = 8; i < 18; i++) fprintf(stderr, " %08x", (u32)uc->uc_mcontext->__ss.__x[i]);
        fprintf(stderr, "\n");
    }
    if (tl_cpu) dump_cpu(tl_cpu);
    dump_backtrace();
    fflush(stderr);
    _exit(72);
}

typedef void (*shim_fn)(CPU *);
// Functions the game looks up at run time get slots after the static imports.
#define DYN_MAX 16384u
static shim_fn dyn_table[DYN_MAX];
static u32 dyn_args[DYN_MAX];
static u32 dyn_n;
__thread u32 rt_dyn_cur_arg;
static __thread int tl_holding;
static __thread const char *rt_gate_who;  // for ARKHAM_TRACE_GATE
static int gate_trace;
static u8 dyn_no_gate[DYN_MAX];
static u8 dyn_glq[DYN_MAX];
static const char *dyn_names[DYN_MAX];
u8 rt_glq_kind(const char *name);
int rt_glq_route(CPU *c, void (*f)(CPU *), u8 kind, int gated, const char *name);
int rt_shim_no_gate(const char *name);

u32 rt_dyn_import_arg(shim_fn fn, u32 arg) {
    if (dyn_n == DYN_MAX) return 0;
    dyn_table[dyn_n] = fn;
    dyn_args[dyn_n] = arg;
    return IMP_BASE + (rt_import_n + dyn_n++) * IMP_STEP;
}
// For a shim that handles its own locking, with a value it reads from rt_dyn_cur_arg.
u32 rt_dyn_import_nogate(shim_fn fn, u32 arg) {
    if (dyn_n < DYN_MAX) dyn_no_gate[dyn_n] = 1;
    return rt_dyn_import_arg(fn, arg);
}
u32 rt_dyn_import(const char *name, shim_fn fn) {
    for (u32 i = 0; i < dyn_n; i++)
        if (dyn_table[i] == fn) return IMP_BASE + (rt_import_n + i) * IMP_STEP;
    if (dyn_n < DYN_MAX) {
        dyn_no_gate[dyn_n] = (u8)rt_shim_no_gate(name);
        dyn_glq[dyn_n] = rt_glq_kind(name);
        dyn_names[dyn_n] = name ? strdup(name) : NULL;
    }
    return rt_dyn_import_arg(fn, 0);
}

// ---------- function table ----------

#define HT_BITS 20
#define HT_SIZE (1u << HT_BITS)
static struct { u32 addr; guest_fn fn; } *ht;

static inline u32 ht_hash(u32 a) { return (a * 0x9E3779B1u) >> (32 - HT_BITS); }

static void ht_build(void) {
    ht = calloc(HT_SIZE, sizeof(*ht));
    for (unsigned i = 0; i < rt_fntab_n; i++) {
        u32 h = ht_hash(rt_fntab[i].addr);
        while (ht[h].addr) h = (h + 1) & (HT_SIZE - 1);
        ht[h].addr = rt_fntab[i].addr;
        ht[h].fn = rt_fntab[i].fn;
    }
}

static inline guest_fn ht_lookup(u32 a) {
    u32 h = ht_hash(a);
    for (;;) {
        u32 k = ht[h].addr;
        if (k == a) return ht[h].fn;
        if (!k) return NULL;
        h = (h + 1) & (HT_SIZE - 1);
    }
}

guest_fn rt_guest_fn(u32 addr) { return ht_lookup(addr); }

// Adds a host function at a guest address to the table, after it's built: a vtable slot the game calls millions of times a second then skips the
// search for imports below. The function finds out which address it was called through in c->jt. Readers may run meanwhile: they see the slot
// empty, or its address with the function already there, and one that misses the function goes the slow way, which also works.
void rt_ht_add(u32 addr, guest_fn fn) {
    u32 h = ht_hash(addr);
    while (ht[h].addr && ht[h].addr != addr) h = (h + 1) & (HT_SIZE - 1);
    ht[h].fn = fn;
    __atomic_store_n(&ht[h].addr, addr, __ATOMIC_RELEASE);
}
// The argument a dynamic import at this address was made with.
u32 rt_dyn_arg_at(u32 addr) {
    u32 k = (addr - IMP_BASE) / IMP_STEP - rt_import_n;
    return k < dyn_n ? dyn_args[k] : 0;
}

void rt_dispatch(CPU *c, u32 t) {
    guest_fn f = ht_lookup(t);
    if (__builtin_expect(f != NULL, 1)) {
        c->jt = t;
        f(c);
        return;
    }
    if (t >= IMP_BASE && t < IMP_BASE + rt_import_n * IMP_STEP) {
        rt_imp(c, (t - IMP_BASE) / IMP_STEP);
        return;
    }
    if (t >= IMP_BASE + rt_import_n * IMP_STEP && t < IMP_BASE + (rt_import_n + dyn_n) * IMP_STEP) {
        u32 k = (t - IMP_BASE) / IMP_STEP - rt_import_n;
        if (dyn_glq[k] && rt_glq_route(c, dyn_table[k], dyn_glq[k], !dyn_no_gate[k], dyn_names[k])) {
            c->esp += 4;
            return;
        }
        int lock = !dyn_no_gate[k] && !tl_holding;
        if (lock) rt_gate_acquire();
        rt_dyn_cur_arg = dyn_args[k];
        dyn_table[k](c);
        if (lock) rt_gate_release();
        c->esp += 4;
        return;
    }
    if (t == RT_HOST_RETURN) return;
    // A guest function pointer to a host callback slot.
    {
        int lock = !tl_holding;
        if (lock) rt_gate_acquire();
        int done = rt_host_thunk_dispatch(c, t);
        if (lock) rt_gate_release();
        if (done) return;
    }
    rt_trap(c, t, "indirect transfer to an unknown address (add it to config/roots.json)");
}

void rt_dispatch_jt(CPU *restrict c) { rt_dispatch(c, c->jt); }

// Call a guest function from host code using the guest cdecl convention.
static int rt_serial;  // ARKHAM_SERIAL=1 on a tools/build.py --serial binary: one guest thread at a time, for telling threading bugs from others
u32 rt_call_guest(CPU *c, u32 addr, int nargs, const u32 *args) {
    u8 *M = rt_mem;
    u32 esp = c->esp;
    u32 saved = esp;
    esp -= (u32)nargs * 4u;
    esp &= ~15u;
    for (int i = 0; i < nargs; i++) *(u32a *)(M + esp + (u32)i * 4u) = args[i];
    esp -= 4;
    *(u32a *)(M + esp) = RT_HOST_RETURN;
    c->esp = esp;
    // Guest code runs in parallel: the lock protects only the runtime's own state, not this call.
    int relock = tl_holding && !rt_serial;
    if (relock) rt_gate_release();
    rt_dispatch(c, addr);
    if (relock) rt_gate_acquire();
    c->esp = saved;
    return c->eax;
}

// ---------- imports ----------

static shim_fn *imp_table;
static int stub_all;
static u8 *imp_seen;

static void imp_missing(CPU *c, u32 idx) {
    const char *name = rt_import_names[idx];
    if (stub_all) {
        if (!imp_seen[idx]) {
            imp_seen[idx] = 1;
            fprintf(stderr, "[stub] %s -> 0\n", name);
        }
        c->eax = 0;
        c->edx = 0;
        return;
    }
    fprintf(stderr, "\n*** missing import: %s\n", name);
    dump_cpu(c);
    dump_backtrace();
    fflush(stderr);
    _exit(73);
}

// ARKHAM_TRACE_IMPORTS: calls per second of each import, to see which shims are hot.
static u32 *imp_counts;
static void imp_count(u32 idx) {
    imp_counts[idx]++;
    // ARKHAM_TRACE_IMPORTS=CF also logs each call to an import whose name starts with that.
    static const char *only = (const char *)1;
    if (only == (const char *)1) {
        only = getenv("ARKHAM_TRACE_IMPORTS");
        if (only && !*only) only = NULL;
    }
    if (only && idx < rt_import_n && !strncmp(rt_import_names[idx] + 1, only, strlen(only)) && !strstr(rt_import_names[idx], "Release") && !strstr(rt_import_names[idx], "Retain"))
        fprintf(stderr, "[call] %s\n", rt_import_names[idx]);
    static u64 last;
    u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000000;
    if (now - last < 10) return;
    last = now;
    fprintf(stderr, "[imports]");
    for (int n = 0; n < 150; n++) {
        u32 best = 0;
        for (u32 i = 1; i < rt_import_n; i++)
            if (imp_counts[i] > imp_counts[best]) best = i;
        if (!imp_counts[best]) break;
        fprintf(stderr, " %s:%u", rt_import_names[best], imp_counts[best] / 10);
        imp_counts[best] = 0;
    }
    fprintf(stderr, "\n");
    memset(imp_counts, 0, rt_import_n * sizeof(u32));
}
static u8 *imp_no_gate;
static u8 *imp_glq;
u8 rt_glq_kind(const char *name);
int rt_glq_route(CPU *c, void (*f)(CPU *), u8 kind, int gated, const char *name);
void rt_imp(CPU *c, u32 idx) {
    shim_fn f = imp_table[idx];
    if (__builtin_expect(imp_counts != NULL, 0)) imp_count(idx);
    if (imp_glq[idx] && f && rt_glq_route(c, f, imp_glq[idx], !imp_no_gate[idx], rt_import_names[idx])) {
        c->esp += 4;
        return;
    }
    int lock = !imp_no_gate[idx] && !tl_holding;
    if (lock) {
        rt_gate_who = rt_import_names[idx];
        rt_gate_acquire();
    }
    if (f) f(c);
    else imp_missing(c, idx);
    if (lock) rt_gate_release();
    c->esp += 4;  // pop the return address, as the callee's ret would have
}

int rt_import_index(const char *name) {
    for (unsigned i = 0; i < rt_import_n; i++)
        if (!strcmp(rt_import_names[i], name)) return (int)i;
    return -1;
}

u32 rt_import_addr(const char *name) {
    int i = rt_import_index(name);
    return i < 0 ? 0 : IMP_BASE + (u32)i * IMP_STEP;
}

// ---------- thread gate: at most one thread runs guest code at a time ----------

// In normal running this is a plain lock around shims that touch shared runtime state, and guest code runs in parallel.
// With ARKHAM_SERIAL=1 it is the old gate: a fair ticket lock held while guest code runs. Each ticket sleeps on its own
// condition variable, and a small timer thread raises rt_gate_wait, which guest code polls, once per time slice.
#define GATE_CVS 64
#include <mach/mach_time.h>
#include <os/lock.h>
static os_unfair_lock gate_fast = OS_UNFAIR_LOCK_INIT;
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv[GATE_CVS];
static u64 gate_next, gate_serving;
static u32 gate_waiters;

int rt_gate_held(void) { return tl_holding; }

static void *gate_slicer(void *arg) {
    (void)arg;
    for (;;) {
        usleep(2000);
        if (__atomic_load_n(&gate_waiters, __ATOMIC_RELAXED)) __atomic_store_n(&rt_gate_wait, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void gate_init(void) {
    rt_serial = getenv("ARKHAM_SERIAL") != NULL;
    gate_trace = getenv("ARKHAM_TRACE_GATE") != NULL;
    if (!rt_serial) return;
    for (int i = 0; i < GATE_CVS; i++) pthread_cond_init(&gate_cv[i], NULL);
    pthread_t t;
    pthread_create(&t, NULL, gate_slicer, NULL);
    pthread_detach(t);
}

// ARKHAM_TRACE_GATE=1: what holds this lock for over 2 ms and what waits that long for it, every five seconds. Sound needs the lock too, so
// anything that holds it for long is heard as a gap.
static __thread u64 tl_gate_since;
#define GATE_NAMES 64
static struct {
    const char *name;
    u64 held_ticks, wait_ticks, worst_hold, worst_wait;
    u32 holds, waits;
} gate_stats[GATE_NAMES];
static os_unfair_lock gate_stats_lock = OS_UNFAIR_LOCK_INIT;
static void gate_note(u64 ticks, int waited) {
    const char *name = rt_gate_who ? rt_gate_who : "(runtime)";
    os_unfair_lock_lock(&gate_stats_lock);
    for (int i = 0; i < GATE_NAMES; i++) {
        if (gate_stats[i].name == name || !gate_stats[i].name) {
            gate_stats[i].name = name;
            if (waited) {
                gate_stats[i].wait_ticks += ticks;
                gate_stats[i].waits++;
                if (ticks > gate_stats[i].worst_wait) gate_stats[i].worst_wait = ticks;
            } else {
                gate_stats[i].held_ticks += ticks;
                gate_stats[i].holds++;
                if (ticks > gate_stats[i].worst_hold) gate_stats[i].worst_hold = ticks;
            }
            break;
        }
    }
    static u64 last;
    u64 now = mach_absolute_time();
    if (now - last > 24000000ull * 5) {
        last = now;
        for (int i = 0; i < GATE_NAMES && gate_stats[i].name; i++) {
            if (gate_stats[i].holds || gate_stats[i].waits)
                fprintf(stderr, "[gate] %s: held %u times %.0f ms worst %.0f, waited %u times %.0f ms worst %.0f\n", gate_stats[i].name, gate_stats[i].holds, (double)gate_stats[i].held_ticks / 24000.0,
                        (double)gate_stats[i].worst_hold / 24000.0, gate_stats[i].waits, (double)gate_stats[i].wait_ticks / 24000.0, (double)gate_stats[i].worst_wait / 24000.0);
            const char *keep = gate_stats[i].name;
            memset(&gate_stats[i], 0, sizeof gate_stats[i]);
            gate_stats[i].name = keep;
        }
    }
    os_unfair_lock_unlock(&gate_stats_lock);
}
void rt_gate_acquire(void) {
    if (!rt_serial) {
        if (__builtin_expect(gate_trace, 0)) {
            u64 t0 = mach_absolute_time();
            os_unfair_lock_lock(&gate_fast);
            tl_holding = 1;
            tl_gate_since = mach_absolute_time();
            if (tl_gate_since - t0 > 48000) gate_note(tl_gate_since - t0, 1);
            return;
        }
        os_unfair_lock_lock(&gate_fast);
        tl_holding = 1;
        return;
    }
    tl_holding = 1;
    pthread_mutex_lock(&gate_mu);
    u64 me = gate_next++;
    gate_waiters++;
    while (gate_serving != me) pthread_cond_wait(&gate_cv[me % GATE_CVS], &gate_mu);
    gate_waiters--;
    pthread_mutex_unlock(&gate_mu);
}

// For a thread that another one is synchronously waiting on: in serial mode, ask whoever runs now to yield at once.
void rt_gate_acquire_now(void) {
    if (rt_serial) __atomic_store_n(&rt_gate_wait, 1, __ATOMIC_RELAXED);
    rt_gate_acquire();
}

void rt_gate_release(void) {
    tl_holding = 0;
    if (!rt_serial) {
        u64 held = __builtin_expect(gate_trace, 0) ? mach_absolute_time() - tl_gate_since : 0;
        os_unfair_lock_unlock(&gate_fast);
        if (held > 48000) gate_note(held, 0);
        return;
    }
    pthread_mutex_lock(&gate_mu);
    gate_serving++;
    pthread_cond_broadcast(&gate_cv[gate_serving % GATE_CVS]);
    pthread_mutex_unlock(&gate_mu);
}

void rt_gate_acquire_serial(void) {
    if (rt_serial) rt_gate_acquire();
}
void rt_gate_release_serial(void) {
    if (rt_serial) rt_gate_release();
}

void rt_gate_yield(void) {
    if (!rt_serial) {
        int held = tl_holding;
        if (held) rt_gate_release();
        sched_yield();
        if (held) rt_gate_acquire();
        return;
    }
    __atomic_store_n(&rt_gate_wait, 0, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&gate_waiters, __ATOMIC_RELAXED)) return;
    rt_gate_release();
    rt_gate_acquire();
}

// ---------- misc CPU helpers ----------

void rt_ldmxcsr(CPU *c, u32 v) { c->mxcsr = v; }

void rt_fninit(CPU *c) {
    c->fcw = 0x37F;
    c->fsw = 0;
    c->top = 0;
}

void rt_cpuid(CPU *c) {
    u32 leaf = c->eax;
    if (leaf == 0) {
        c->eax = 1;
        c->ebx = 0x756E6547;  // GenuineIntel
        c->edx = 0x49656E69;
        c->ecx = 0x6C65746E;
    } else if (leaf == 1) {
        c->eax = 0x000106A5;
        c->ebx = 0x00040800;
        c->ecx = 0x00000201;  // SSE3, SSSE3
        c->edx = 0x078BFBFF;  // through SSE2, CMOV, CX8
    } else {
        c->eax = c->ebx = c->ecx = c->edx = 0;
    }
}

double rt_ld80(const u8 *p) {
    u64 m;
    u16 se;
    memcpy(&m, p, 8);
    memcpy(&se, p + 8, 2);
    int e = se & 0x7FFF;
    double v;
    if (e == 0 && m == 0) v = 0.0;
    else if (e == 0x7FFF) v = (m << 1) ? NAN : INFINITY;
    else v = ldexp((double)m, e - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}

void rt_st80(u8 *p, double v) {
    u64 m = 0;
    u16 se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) {
        m = 0xC000000000000000ull;
        se |= 0x7FFF;
    } else if (isinf(v)) {
        m = 0x8000000000000000ull;
        se |= 0x7FFF;
    } else if (v != 0.0) {
        int e;
        double f = frexp(fabs(v), &e);
        m = (u64)ldexp(f, 64);
        se |= (u16)(e + 16382);
    }
    memcpy(p, &m, 8);
    memcpy(p + 8, &se, 2);
}

// ---------- guest memory layout ----------

static void map_guest(void) {
    void *p = mmap((void *)RT_MEM_BASE, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p != (void *)RT_MEM_BASE) {
        fprintf(stderr, "could not reserve 4 GB of guest address space at %#llx (got %p)\n", RT_MEM_BASE, p);
        exit(1);
    }
    rt_mem = p;
}

const u8 *rt_header_bytes;

// The Bink library goes at its fixed address and gets slid the way dyld would slide it.
static void load_bink(const char *game_path) {
    char path[1200];
    const char *macos = strstr(game_path, "/MacOS/");
    if (!macos) {
        fprintf(stderr, "*** can't work out where the Bink library is from %s\n", game_path);
        exit(1);
    }
    snprintf(path, sizeof path, "%.*s/Frameworks/libBinkMacx86.dylib", (int)(macos - game_path), game_path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        exit(1);
    }
    for (unsigned i = 0; i < rt_bink_segs_n; i++) {
        const rt_seg_entry *s = &rt_bink_segs[i];
        size_t got = 0;
        while (got < s->filesize) {
            ssize_t r = pread(fd, rt_mem + s->vmaddr + got, s->filesize - got, (off_t)(s->fileoff + got));
            if (r <= 0) {
                perror("reading the Bink library");
                exit(1);
            }
            got += (size_t)r;
        }
    }
    close(fd);
    for (unsigned i = 0; i < rt_bink_rebases_n; i++) *(u32a *)(rt_mem + rt_bink_rebases[i]) += rt_bink_base;
}

static void load_image(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        exit(1);
    }
    for (unsigned i = 0; i < rt_segs_n; i++) {
        const rt_seg_entry *s = &rt_segs[i];
        if (!s->filesize) continue;
        size_t got = 0;
        while (got < s->filesize) {
            ssize_t r = pread(fd, rt_mem + s->vmaddr + got, s->filesize - got, (off_t)(s->fileoff + got));
            if (r <= 0) {
                perror("reading game image");
                exit(1);
            }
            got += (size_t)r;
        }
    }
    close(fd);
    // Keep a copy of the Mach-O header: its first page is about to become the NULL trap.
    u8 *hdr = malloc(0xC000);
    memcpy(hdr, rt_mem + 0x1000, 0xC000);
    rt_header_bytes = hdr;
    load_bink(path);
    for (unsigned i = 0; i < rt_binds_n; i++) {
        u32 v = IMP_BASE + rt_binds[i].import * IMP_STEP + (u32)rt_binds[i].addend;
        *(u32a *)(rt_mem + rt_binds[i].addr) = v;
    }
}

void rt_register_shims(shim_fn *table);

// The game's folder: Steam's on the Mac, and on the phone the copy in the app's Documents (docs/IOS_PORT.md). ARKHAM_GAME_ROOT names another.
const char *rt_game_root(void) {
    static char buf[1024];
    if (!buf[0]) {
        const char *env = getenv("ARKHAM_GAME_ROOT");
        if (env) snprintf(buf, sizeof buf, "%s", env);
#if TARGET_OS_IPHONE
        else snprintf(buf, sizeof buf, "%s/Documents/Batman Arkham Asylum GOTY", getenv("HOME"));
#else
        else snprintf(buf, sizeof buf, "%s/Library/Application Support/Steam/steamapps/common/Batman Arkham Asylum GOTY", getenv("HOME"));
#endif
    }
    return buf;
}
static const char *binary_path(void) {
    const char *env = getenv("ARKHAM_BINARY");
    if (env) return env;
    static char buf[1200];
    snprintf(buf, sizeof buf, "%s/Batman Arkham Asylum GOTY.app/Contents/MacOS/Batman Arkham Asylum GOTY", rt_game_root());
    return buf;
}

CPU *rt_new_cpu(u32 stack_size) {
    CPU *c = calloc(1, sizeof(CPU));
    c->fcw = 0x37F;
    c->mxcsr = 0x1F80;
    u32 stk = rt_guest_alloc_pages(stack_size);
    c->esp = (stk + stack_size - 64) & ~15u;
    tl_cpu = c;
    return c;
}

void rt_bind_cpu(CPU *c) { tl_cpu = c; }

int rt_main_argc;
char **rt_main_argv;

static void *guest_main(void *arg) {
    (void)arg;
    rt_gate_acquire();
    CPU *c = rt_new_cpu(16u << 20);
    int nargs = rt_main_argc;
    // Lay out argc/argv/envp in guest memory the way the kernel would for `start`.
    u32 strings = rt_guest_alloc_pages(1u << 20);
    u32 sp = strings;
    u32 *ptrs = calloc((size_t)nargs + 1, sizeof(u32));
    for (int i = 0; i < nargs; i++) {
        size_t n = strlen(rt_main_argv[i]) + 1;
        memcpy(rt_mem + sp, rt_main_argv[i], n);
        ptrs[i] = sp;
        sp += (u32)n;
    }
    u32 esp = (c->esp - (u32)(nargs + 8) * 4u) & ~15u;
    u32 *st = (u32 *)(rt_mem + esp);
    st[0] = (u32)nargs;
    for (int i = 0; i < nargs; i++) st[1 + i] = ptrs[i];
    st[1 + nargs] = 0;  // argv terminator
    st[2 + nargs] = 0;  // envp terminator
    st[3 + nargs] = 0;  // apple terminator
    c->esp = esp;
    rt_shims_process_init(c, nargs, esp + 4u, esp + 4u + ((u32)nargs + 1u) * 4u);

    // Static constructors, in order, as dyld would run them.
    const u32 *inits = (const u32 *)(rt_mem + rt_modinit_addr);
    u32 args[3] = {(u32)nargs, esp + 4u, esp + 4u + ((u32)nargs + 1u) * 4u};
    fprintf(stderr, "[rt] running %u static initializers\n", rt_modinit_n);
    for (unsigned i = 0; i < rt_modinit_n; i++) {
        rt_call_guest(c, inits[i], 3, args);
    }
    fprintf(stderr, "[rt] entering game start at %08x\n", rt_entry_addr);
    c->esp = esp;
    if (!rt_serial) rt_gate_release();
    void rt_d3dq_start(void);
    rt_d3dq_start();
    rt_dispatch(c, rt_entry_addr);
    fprintf(stderr, "[rt] start returned\n");
    exit(0);
}

// The game's main thread and its run loop, where the game's timers, run loop sources and main queue work go. On the Mac that's the process's main
// thread, as AppKit wants. On the phone UIKit keeps the main thread, so the game's is a thread of its own (rt_main), and "main" means that one.
#include <CoreFoundation/CoreFoundation.h>
static CFRunLoopRef guest_loop;
static pthread_t guest_main_thread;
CFRunLoopRef rt_guest_main_loop(void) { return guest_loop ? guest_loop : CFRunLoopGetMain(); }
int rt_on_guest_main(void) { return guest_loop ? pthread_equal(pthread_self(), guest_main_thread) : pthread_main_np(); }

int rt_mtl_prebuild(const char *list);
#if TARGET_OS_IPHONE
// Called by the app shell (ios/app) on a thread of its own with a large stack: iOS gives the main thread 1 MB, and UIKit needs it anyway.
int rt_main(int argc, char **argv) {
    guest_loop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());
    guest_main_thread = pthread_self();
#else
int main(int argc, char **argv) {
#endif
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (argc == 3 && !strcmp(argv[1], "--prebuild-shaders")) return rt_mtl_prebuild(argv[2]);
#if !TARGET_OS_IPHONE
    int rt_steam_check(const char *dir);
    if (argc == 3 && !strcmp(argv[1], "--steam-check")) return rt_steam_check(argv[2]);
#endif
    rt_main_argc = argc;
    rt_main_argv = argv;
    gate_init();
    map_guest();
    ht_build();
    void rt_launcher_skip_install(void);
    rt_launcher_skip_install();
    rt_heap_init();
    load_image(binary_path());
    // Guest NULL page (one host page) faults on access.
    mprotect(rt_mem, 0x4000, PROT_NONE);

    imp_table = calloc(rt_import_n, sizeof(shim_fn));
    imp_seen = calloc(rt_import_n, 1);
    rt_register_shims(imp_table);
    stub_all = getenv("ARKHAM_STUB_ALL") != NULL;
    if (getenv("ARKHAM_TRACE_IMPORTS")) imp_counts = calloc(rt_import_n + 1, sizeof(u32));
    imp_no_gate = calloc(rt_import_n + 1, 1);
    imp_glq = calloc(rt_import_n + 1, 1);
    for (unsigned i = 0; i < rt_import_n; i++) {
        imp_no_gate[i] = (u8)rt_shim_no_gate(rt_import_names[i]);
        imp_glq[i] = rt_glq_kind(rt_import_names[i]);
    }
    rt_no_frame_limit = getenv("ARKHAM_FRAME_LIMIT") == NULL;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    stack_t alt = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20, .ss_flags = 0};
    sigaltstack(&alt, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    void rt_hitch_start(void);
    rt_hitch_start();
    if (getenv("ARKHAM_TRACE_UNALIGNED")) {
        ut_on = 1;
        pthread_t t;
        pthread_create(&t, NULL, ut_report, NULL);
        pthread_detach(t);
    }

    // Guest code runs on the real main thread on the Mac because AppKit requires it. Translated functions mirror guest calls on the native
    // stack, so the linker gives the main thread a large one (see tools/build.py); on the phone the app shell makes this thread with one.
    guest_main(NULL);
    return 0;
}
