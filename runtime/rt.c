// Runtime core: guest memory, image loading, dispatch, thread gate, trap reporting, process start.
#include "rt.h"
#include "shim.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
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

static void on_fault(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    u64 fault = (u64)si->si_addr;
    u64 base = (u64)rt_mem;
    fprintf(stderr, "\n*** signal %d, host fault address %#llx", sig, fault);
    Dl_info di;
    if (dladdr((void *)fault, &di) && di.dli_sname) fprintf(stderr, " (%s in %s)", di.dli_sname, di.dli_fname);
    if (fault >= base && fault < base + (1ull << 32)) fprintf(stderr, " = guest %08x", (u32)(fault - base));
    fprintf(stderr, "\n");
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
u32 rt_dyn_cur_arg;

u32 rt_dyn_import_arg(shim_fn fn, u32 arg) {
    if (dyn_n == DYN_MAX) return 0;
    dyn_table[dyn_n] = fn;
    dyn_args[dyn_n] = arg;
    return IMP_BASE + (rt_import_n + dyn_n++) * IMP_STEP;
}
u32 rt_dyn_import(const char *name, shim_fn fn) {
    (void)name;
    for (u32 i = 0; i < dyn_n; i++)
        if (dyn_table[i] == fn) return IMP_BASE + (rt_import_n + i) * IMP_STEP;
    return rt_dyn_import_arg(fn, 0);
}

// ---------- function table ----------

#define HT_BITS 19
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

void rt_dispatch(CPU *c, u32 t) {
    guest_fn f = ht_lookup(t);
    if (__builtin_expect(f != NULL, 1)) {
        f(c);
        return;
    }
    if (t >= IMP_BASE && t < IMP_BASE + rt_import_n * IMP_STEP) {
        rt_imp(c, (t - IMP_BASE) / IMP_STEP);
        return;
    }
    if (t >= IMP_BASE + rt_import_n * IMP_STEP && t < IMP_BASE + (rt_import_n + dyn_n) * IMP_STEP) {
        u32 k = (t - IMP_BASE) / IMP_STEP - rt_import_n;
        rt_dyn_cur_arg = dyn_args[k];
        dyn_table[k](c);
        c->esp += 4;
        return;
    }
    if (t == RT_HOST_RETURN) return;
    // A guest function pointer to a host callback slot.
    if (rt_host_thunk_dispatch(c, t)) return;
    rt_trap(c, t, "indirect transfer to an unknown address (add it to config/roots.json)");
}

void rt_dispatch_jt(CPU *restrict c) { rt_dispatch(c, c->jt); }

// Call a guest function from host code using the guest cdecl convention.
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
    rt_dispatch(c, addr);
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

void rt_imp(CPU *c, u32 idx) {
    shim_fn f = imp_table[idx];
    if (f) f(c);
    else imp_missing(c, idx);
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

// A fair ticket lock. Each ticket sleeps on its own condition variable, so a release wakes only the next thread.
// rt_gate_wait is the flag guest code polls: a small timer thread raises it once per time slice while anyone waits.
#define GATE_CVS 64
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv[GATE_CVS];
static u64 gate_next, gate_serving;
static u32 gate_waiters;
static __thread int tl_holding;

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
    for (int i = 0; i < GATE_CVS; i++) pthread_cond_init(&gate_cv[i], NULL);
    pthread_t t;
    pthread_create(&t, NULL, gate_slicer, NULL);
    pthread_detach(t);
}

void rt_gate_acquire(void) {
    tl_holding = 1;
    pthread_mutex_lock(&gate_mu);
    u64 me = gate_next++;
    gate_waiters++;
    while (gate_serving != me) pthread_cond_wait(&gate_cv[me % GATE_CVS], &gate_mu);
    gate_waiters--;
    pthread_mutex_unlock(&gate_mu);
}

void rt_gate_release(void) {
    tl_holding = 0;
    pthread_mutex_lock(&gate_mu);
    gate_serving++;
    pthread_cond_broadcast(&gate_cv[gate_serving % GATE_CVS]);
    pthread_mutex_unlock(&gate_mu);
}

void rt_gate_yield(void) {
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
    void *p = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("reserving 4 GB of guest address space");
        exit(1);
    }
    rt_mem = p;
}

const u8 *rt_header_bytes;

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
    for (unsigned i = 0; i < rt_binds_n; i++) {
        u32 v = IMP_BASE + rt_binds[i].import * IMP_STEP + (u32)rt_binds[i].addend;
        *(u32a *)(rt_mem + rt_binds[i].addr) = v;
    }
}

void rt_register_shims(shim_fn *table);

static const char *binary_path(void) {
    const char *env = getenv("ARKHAM_BINARY");
    if (env) return env;
    static char buf[1024];
    snprintf(buf, sizeof buf,
             "%s/Library/Application Support/Steam/steamapps/common/Batman Arkham Asylum GOTY/"
             "Batman Arkham Asylum GOTY.app/Contents/MacOS/Batman Arkham Asylum GOTY",
             getenv("HOME"));
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
    rt_dispatch(c, rt_entry_addr);
    fprintf(stderr, "[rt] start returned\n");
    exit(0);
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IOLBF, 0);
    rt_main_argc = argc;
    rt_main_argv = argv;
    gate_init();
    map_guest();
    ht_build();
    rt_heap_init();
    load_image(binary_path());
    // Guest NULL page (one host page) faults on access.
    mprotect(rt_mem, 0x4000, PROT_NONE);

    imp_table = calloc(rt_import_n, sizeof(shim_fn));
    imp_seen = calloc(rt_import_n, 1);
    rt_register_shims(imp_table);
    stub_all = getenv("ARKHAM_STUB_ALL") != NULL;

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

    // Guest code runs on the real main thread because AppKit requires it. Translated functions mirror guest
    // calls on the native stack, so the linker gives the main thread a large one (see tools/build.py).
    guest_main(NULL);
    return 0;
}
