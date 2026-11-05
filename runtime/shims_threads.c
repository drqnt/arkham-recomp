// pthreads, semaphores, atomics and GCD. Only one thread runs guest code at a time, so every primitive
// is plain state in guest memory plus one host-side "something changed" event to sleep on.
#include "shim.h"
#include "shim_reg.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t ev_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ev_cv = PTHREAD_COND_INITIALIZER;
static u64 ev_gen;

void rt_event_notify(void) {
    pthread_mutex_lock(&ev_mu);
    ev_gen++;
    pthread_cond_broadcast(&ev_cv);
    pthread_mutex_unlock(&ev_mu);
}

// Give up the gate until something changes or a short timeout passes.
static void block_a_little(void) {
    pthread_mutex_lock(&ev_mu);
    u64 g = ev_gen;
    rt_gate_release();
    struct timespec ts;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec;
    ts.tv_nsec = (tv.tv_usec + 2000) * 1000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    while (ev_gen == g) {
        if (pthread_cond_timedwait(&ev_cv, &ev_mu, &ts) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&ev_mu);
    rt_gate_acquire();
}

// ---------- per-thread data ----------
#define MAX_KEYS 512
typedef struct GThread {
    CPU *cpu;
    u32 id;
    u32 start, arg;
    u32 retval;
    int done;
    int detached;
    u32 tsd[MAX_KEYS];
    u32 tls;
    pthread_t host;
} GThread;

static __thread GThread *tl_self;
static GThread *threads[4096];
static u32 next_tid = 1;
static u32 key_dtor[MAX_KEYS];
static u32 key_used[MAX_KEYS];

#define TID_BASE 0xED000000u

static GThread *self_thread(void) {
    if (!tl_self) {
        GThread *t = calloc(1, sizeof *t);
        t->cpu = rt_current_cpu();
        t->id = next_tid++;
        t->host = pthread_self();
        threads[t->id] = t;
        tl_self = t;
    }
    return tl_self;
}
static GThread *from_tid(u32 h) {
    u32 k = (h - TID_BASE) / 16;
    return k < 4096 ? threads[k] : NULL;
}
static u32 to_tid(GThread *t) { return TID_BASE + t->id * 16; }

// ---------- mutexes: {sig, owner, count, type, ...} in the guest pthread_mutex_t ----------
#define M_OWNER(m) G32((m) + 4)
#define M_COUNT(m) G32((m) + 8)
#define M_TYPE(m) G32((m) + 12)
#define PTM_RECURSIVE 2

SHIM(pthread_mutexattr_init) { G32(ARG(0)) = 0x4D555458; G32(ARG(0) + 4) = 0; RET(0); }
SHIM(pthread_mutexattr_destroy) { RET(0); }
SHIM(pthread_mutexattr_settype) { G32(ARG(0) + 4) = ARG(1); RET(0); }
SHIM(pthread_mutex_init) {
    u32 m = ARG(0);
    memset(GP(m), 0, 44);
    G32(m) = 0x32AAABA7;
    if (ARG(1)) M_TYPE(m) = G32(ARG(1) + 4);
    RET(0);
}
SHIM(pthread_mutex_destroy) { RET(0); }
static int mutex_lock(u32 m) {
    u32 me = to_tid(self_thread());
    if (M_OWNER(m) == me) {
        if (M_TYPE(m) == PTM_RECURSIVE) {
            M_COUNT(m)++;
            return 0;
        }
        return 11;  // EDEADLK
    }
    while (M_OWNER(m)) block_a_little();
    M_OWNER(m) = me;
    M_COUNT(m) = 1;
    return 0;
}
static int mutex_unlock(u32 m) {
    u32 me = to_tid(self_thread());
    if (M_OWNER(m) != me) return 1;
    if (--M_COUNT(m) == 0) {
        M_OWNER(m) = 0;
        rt_event_notify();
    }
    return 0;
}
SHIM(pthread_mutex_lock) { RET(mutex_lock(ARG(0))); }
SHIM(pthread_mutex_unlock) { RET(mutex_unlock(ARG(0))); }
SHIM(pthread_mutex_trylock) {
    u32 m = ARG(0);
    u32 me = to_tid(self_thread());
    if (M_OWNER(m) == me && M_TYPE(m) == PTM_RECURSIVE) {
        M_COUNT(m)++;
        RET(0);
    } else if (M_OWNER(m)) {
        RET(16);
    } else {
        M_OWNER(m) = me;
        M_COUNT(m) = 1;
        RET(0);
    }
}

// ---------- condition variables: a generation counter; POSIX allows spurious wakeups ----------
#define C_GEN(cv) G32((cv) + 4)
SHIM(pthread_cond_init) { memset(GP(ARG(0)), 0, 28); RET(0); }
SHIM(pthread_cond_destroy) { RET(0); }
SHIM(pthread_cond_signal) { C_GEN(ARG(0))++; rt_event_notify(); RET(0); }
SHIM(pthread_cond_broadcast) { C_GEN(ARG(0))++; rt_event_notify(); RET(0); }
SHIM(pthread_cond_wait) {
    u32 cv = ARG(0), m = ARG(1);
    u32 g = C_GEN(cv);
    u32 cnt = M_COUNT(m);
    M_COUNT(m) = 1;
    mutex_unlock(m);
    while (C_GEN(cv) == g) block_a_little();
    mutex_lock(m);
    M_COUNT(m) = cnt;
    RET(0);
}
SHIM(pthread_cond_timedwait) {
    u32 cv = ARG(0), m = ARG(1), ts = ARG(2);
    u64 deadline = (u64)G32(ts) * 1000000ull + G32(ts + 4) / 1000;
    u32 g = C_GEN(cv);
    u32 cnt = M_COUNT(m);
    M_COUNT(m) = 1;
    mutex_unlock(m);
    u32 rc = 0;
    while (C_GEN(cv) == g) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if ((u64)tv.tv_sec * 1000000ull + (u64)tv.tv_usec >= deadline) {
            rc = 60;
            break;
        }
        block_a_little();
    }
    mutex_lock(m);
    M_COUNT(m) = cnt;
    RET(rc);
}

// ---------- thread lifetime ----------
static void *thread_tramp(void *p) {
    GThread *t = p;
    rt_gate_acquire();
    tl_self = t;
    CPU *c = rt_new_cpu(2u << 20);
    t->cpu = c;
    u32 a[1] = {t->arg};
    t->retval = rt_call_guest(c, t->start, 1, a);
    t->done = 1;
    rt_event_notify();
    rt_gate_release();
    return NULL;
}
SHIM(pthread_attr_init) { memset(GP(ARG(0)), 0, 40); RET(0); }
SHIM(pthread_attr_destroy) { RET(0); }
SHIM(pthread_attr_setstacksize) { RET(0); }
SHIM(pthread_create) {
    GThread *t = calloc(1, sizeof *t);
    t->id = next_tid++;
    t->start = ARG(2);
    t->arg = ARG(3);
    threads[t->id] = t;
    G32(ARG(0)) = to_tid(t);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t->host, &attr, thread_tramp, t);
    if (getenv("ARKHAM_TRACE_THREADS")) fprintf(stderr, "[thread] created %u start=%08x\n", t->id, t->start);
    RET(r);
}
SHIM(pthread_join) {
    GThread *t = from_tid(ARG(0));
    if (!t) {
        RET(3);
        return;
    }
    while (!t->done) block_a_little();
    if (ARG(1)) G32(ARG(1)) = t->retval;
    RET(0);
}
SHIM(pthread_detach) { RET(0); }
SHIM(pthread_self) { RET(to_tid(self_thread())); }
SHIM(pthread_equal) { RET(ARG(0) == ARG(1)); }
SHIM(pthread_exit) {
    GThread *t = self_thread();
    t->retval = ARG(0);
    t->done = 1;
    rt_event_notify();
    rt_gate_release();
    pthread_exit(NULL);
}
SHIM(pthread_main_np) { RET(self_thread()->id == 1); }
SHIM(pthread_kill) { RET(0); }
SHIM(pthread_cancel) { RET(0); }
SHIM(pthread_setname_np) { RET(0); }
SHIM(pthread_getname_np) { if (ARG(2)) G8(ARG(1)) = 0; RET(0); }
SHIM(pthread_getschedparam) { G32(ARG(1)) = 1; G32(ARG(2)) = 31; RET(0); }
SHIM(pthread_setschedparam) { RET(0); }
SHIM(sched_get_priority_max) { RET(47); }
SHIM(sched_get_priority_min) { RET(15); }
SHIM(sched_yield) { rt_gate_yield(); RET(0); }
SHIM(pthread_mach_thread_np) { RET(ARG(0)); }
SHIM(mach_thread_self) { RET(to_tid(self_thread())); }
SHIM(mach_port_deallocate) { RET(0); }
SHIM(mach_host_self) { RET(0x207); }

// ---------- thread-specific data ----------
SHIM(pthread_key_create) {
    for (u32 k = 1; k < MAX_KEYS; k++) {
        if (!key_used[k]) {
            key_used[k] = 1;
            key_dtor[k] = ARG(1);
            G32(ARG(0)) = k;
            RET(0);
            return;
        }
    }
    RET(35);
}
SHIM(pthread_key_delete) { if (ARG(0) < MAX_KEYS) key_used[ARG(0)] = 0; RET(0); }
SHIM(pthread_getspecific) { RET(ARG(0) < MAX_KEYS ? self_thread()->tsd[ARG(0)] : 0); }
SHIM(pthread_setspecific) { if (ARG(0) < MAX_KEYS) self_thread()->tsd[ARG(0)] = ARG(1); RET(0); }
SHIM(pthread_once) {
    u32 o = ARG(0);
    if (G32(o + 4) != 0x1BADC0DE) {
        G32(o + 4) = 0x1BADC0DE;
        rt_call_guest(c, ARG(1), 0, NULL);
    }
    RET(0);
}

// Native thread-local variables: __thread_vars descriptors point at a thunk through _tlv_bootstrap.
SHIM(_tlv_bootstrap) {
    GThread *t = self_thread();
    if (!t->tls) t->tls = rt_guest_calloc(1, 0x1000);
    RET(t->tls + G32(c->eax + 8));
}

// ---------- semaphores ----------
typedef struct NSem { char name[64]; int count; } NSem;
static NSem nsems[64];
SHIM(sem_open) {
    const char *n = GS(ARG(0));
    for (int i = 0; i < 64; i++) {
        if (!nsems[i].name[0] || !strcmp(nsems[i].name, n)) {
            if (!nsems[i].name[0]) {
                snprintf(nsems[i].name, sizeof nsems[i].name, "%s", n);
                nsems[i].count = (ARG(1) & 0x200) ? (int)ARG(3) : 0;
            }
            RET(rt_handle_new(&nsems[i]));
            return;
        }
    }
    RET(0xFFFFFFFFu);
}
SHIM(sem_close) { rt_handle_free(ARG(0)); RET(0); }
SHIM(sem_unlink) { RET(0); }
SHIM(sem_post) { NSem *s = rt_handle_get(ARG(0)); s->count++; rt_event_notify(); RET(0); }
SHIM(sem_wait) {
    NSem *s = rt_handle_get(ARG(0));
    while (s->count <= 0) block_a_little();
    s->count--;
    RET(0);
}

SHIM(dispatch_semaphore_create) {
    NSem *s = calloc(1, sizeof *s);
    s->count = (int)ARG(0);
    RET(rt_handle_new(s));
}
SHIM(dispatch_semaphore_signal) { NSem *s = rt_handle_get(ARG(0)); s->count++; rt_event_notify(); RET(0); }
SHIM(dispatch_semaphore_wait) {
    NSem *s = rt_handle_get(ARG(0));
    u64 timeout = ARGQ(1);
    u64 waited = 0;
    while (s->count <= 0) {
        if (timeout != ~0ull && waited >= 2000000ull) {
            // Timed waits are approximate: give up after the host-side polling budget.
            if (waited * 1000 >= timeout) {
                RET(1);
                return;
            }
        }
        block_a_little();
        waited += 2000;
    }
    s->count--;
    RET(0);
}
SHIM(dispatch_time) { RET64(ARGQ(1)); }

// ---------- atomics: the gate makes plain operations atomic ----------
SHIM(OSAtomicAdd32) { u32 v = G32(ARG(1)) + ARG(0); G32(ARG(1)) = v; RET(v); }
SHIM(OSAtomicAdd64) {
    u64 v = *(u64a *)GP(ARG(2)) + ARGQ(0);
    *(u64a *)GP(ARG(2)) = v;
    RET64(v);
}
SHIM(OSAtomicCompareAndSwap32) {
    if (G32(ARG(2)) == ARG(0)) {
        G32(ARG(2)) = ARG(1);
        RET(1);
    } else {
        RET(0);
    }
}
SHIM(OSMemoryBarrier) {}
SHIM(OSSpinLockLock) {
    u32 l = ARG(0);
    while (G32(l)) block_a_little();
    G32(l) = 1;
}
SHIM(OSSpinLockTry) {
    u32 l = ARG(0);
    if (G32(l)) {
        RET(0);
    } else {
        G32(l) = 1;
        RET(1);
    }
}
SHIM(OSSpinLockUnlock) { G32(ARG(0)) = 0; rt_event_notify(); }

// ---------- GCD: blocks are guest structures {isa, flags, reserved, invoke, ...} ----------
static void call_block(CPU *c, u32 block) {
    u32 a[1] = {block};
    rt_call_guest(c, G32(block + 12), 1, a);
}
SHIM(dispatch_once) {
    u32 pred = ARG(0);
    if (G32(pred) != 0xFFFFFFFFu) {
        G32(pred) = 0xFFFFFFFFu;
        call_block(c, ARG(1));
    }
}
// Work sent to the main queue must really run on the main thread (AppKit insists on it). The caller gives up the
// gate while waiting, and the main thread takes it to run the guest code. Other queues run inline.
#include <dispatch/dispatch.h>
typedef struct { u32 fn, arg; int is_block; int heap; } DCtx;
static void main_queue_tramp(void *p) {
    DCtx *d = p;
    int held = rt_gate_held();
    if (!held) rt_gate_acquire();
    CPU *c = rt_current_cpu();
    if (d->is_block) {
        call_block(c, d->fn);
    } else {
        u32 a[1] = {d->arg};
        rt_call_guest(c, d->fn, 1, a);
    }
    if (!held) rt_gate_release();
    if (d->heap) free(d);
}
static int is_main_queue(u32 q) {
    static u32 mq;
    if (!mq) mq = rt_import_addr("__dispatch_main_q");
    return q == mq;
}
static void run_on_main_sync(DCtx *d) {
    if (pthread_main_np()) {
        main_queue_tramp(d);
        return;
    }
    rt_gate_release();
    dispatch_sync_f(dispatch_get_main_queue(), d, main_queue_tramp);
    rt_gate_acquire();
}
// Blocks: {isa, flags, reserved, invoke, descriptor, captures...}; descriptor: {reserved, size, copy, dispose}.
#define BLK_NEEDS_FREE (1u << 24)
#define BLK_HAS_COPY_DISPOSE (1u << 25)
#define BLK_IS_GLOBAL (1u << 28)
static u32 block_copy(CPU *c, u32 b) {
    if (!b) return 0;
    u32 flags = G32(b + 4);
    if (flags & BLK_IS_GLOBAL) return b;
    if (flags & BLK_NEEDS_FREE) {
        G32(b + 4) = flags + 2;
        return b;
    }
    u32 desc = G32(b + 16);
    u32 size = G32(desc + 4);
    u32 n = rt_guest_malloc(size);
    memcpy(GP(n), GP(b), size);
    G32(n + 4) = (flags & ~0xFFFFu) | BLK_NEEDS_FREE | 2u;
    if (flags & BLK_HAS_COPY_DISPOSE) {
        u32 a[2] = {n, b};
        rt_call_guest(c, G32(desc + 8), 2, a);
    }
    return n;
}
static void run_on_main_async(u32 fn, u32 arg, int is_block) {
    DCtx *d = malloc(sizeof *d);
    *d = (DCtx){fn, arg, is_block, 1};
    dispatch_async_f(dispatch_get_main_queue(), d, main_queue_tramp);
}
SHIM(dispatch_sync) {
    if (is_main_queue(ARG(0))) {
        DCtx d = {ARG(1), 0, 1, 0};
        run_on_main_sync(&d);
    } else {
        call_block(c, ARG(1));
    }
}
SHIM(dispatch_async) {
    if (is_main_queue(ARG(0))) run_on_main_async(block_copy(c, ARG(1)), 0, 1);
    else call_block(c, ARG(1));
}
SHIM(dispatch_sync_f) {
    if (is_main_queue(ARG(0))) {
        DCtx d = {ARG(2), ARG(1), 0, 0};
        run_on_main_sync(&d);
    } else {
        u32 a[1] = {ARG(1)};
        rt_call_guest(c, ARG(2), 1, a);
    }
}
SHIM(dispatch_async_f) {
    if (is_main_queue(ARG(0))) {
        run_on_main_async(ARG(2), ARG(1), 0);
    } else {
        u32 a[1] = {ARG(1)};
        rt_call_guest(c, ARG(2), 1, a);
    }
}
SHIM(dispatch_get_global_queue) { RET(0xEC000010u); }
SHIM(dispatch_queue_create) { RET(0xEC000020u); }
SHIM(dispatch_group_create) { RET(0xEC000030u); }
SHIM(dispatch_group_async) { call_block(c, ARG(2)); }
SHIM(dispatch_group_wait) { RET(0); }
SHIM(dispatch_release) {}
SHIM(_Block_copy) { RET(block_copy(c, ARG(0))); }
SHIM(_Block_release) {}
// Called by copy helpers: dest, source, kind. Kinds: 3 object, 7 block, 8 __block variable, plus weak/helper bits.
SHIM(_Block_object_assign) {
    u32 dest = ARG(0), src = ARG(1), kind = ARG(2) & 0x1F;
    if (kind == 7) {
        G32(dest) = block_copy(c, src);
    } else if (kind == 8 && src) {
        // __block variable: {isa, forwarding, flags, size, [copy, dispose], value...}.
        u32 fwd = G32(src + 4);
        if (fwd != src) {
            G32(dest) = fwd;
        } else {
            u32 flags = G32(src + 8), size = G32(src + 12);
            u32 n = rt_guest_malloc(size);
            memcpy(GP(n), GP(src), size);
            G32(n + 4) = n;
            G32(src + 4) = n;
            if (flags & BLK_HAS_COPY_DISPOSE) {
                u32 a[2] = {n, src};
                rt_call_guest(c, G32(src + 16), 2, a);
            }
            G32(dest) = n;
        }
    } else {
        G32(dest) = src;
    }
}
SHIM(_Block_object_dispose) {}

const ShimReg rt_shims_threads[] = {
    R(pthread_mutexattr_init) R(pthread_mutexattr_destroy) R2("_pthread_mutexattr_destroy$UNIX2003", pthread_mutexattr_destroy)
    R(pthread_mutexattr_settype) R(pthread_mutex_init) R(pthread_mutex_destroy) R(pthread_mutex_lock) R(pthread_mutex_unlock)
    R(pthread_mutex_trylock) R(pthread_cond_init) R(pthread_cond_destroy) R(pthread_cond_signal) R(pthread_cond_broadcast)
    R(pthread_cond_wait) R(pthread_cond_timedwait) R(pthread_attr_init) R(pthread_attr_destroy) R(pthread_attr_setstacksize)
    R(pthread_create) R(pthread_join) R(pthread_detach) R(pthread_self) R(pthread_equal) R(pthread_exit) R(pthread_main_np)
    R(pthread_kill) R(pthread_cancel) R(pthread_setname_np) R(pthread_getname_np) R(pthread_getschedparam) R(pthread_setschedparam)
    R(sched_get_priority_max) R(sched_get_priority_min) R(sched_yield) R(pthread_mach_thread_np) R(mach_thread_self)
    R(mach_port_deallocate) R(mach_host_self) R(pthread_key_create) R(pthread_key_delete) R(pthread_getspecific)
    R(pthread_setspecific) R(pthread_once) R(_tlv_bootstrap) R(sem_open) R(sem_close) R(sem_unlink) R(sem_post) R(sem_wait)
    R(dispatch_semaphore_create) R(dispatch_semaphore_signal) R(dispatch_semaphore_wait) R(dispatch_time)
    R(OSAtomicAdd32) R2("_OSAtomicAdd32Barrier", OSAtomicAdd32) R(OSAtomicAdd64)
    R2("_OSAtomicCompareAndSwap32Barrier", OSAtomicCompareAndSwap32) R2("_OSAtomicCompareAndSwapInt", OSAtomicCompareAndSwap32)
    R2("_OSAtomicCompareAndSwapPtrBarrier", OSAtomicCompareAndSwap32) R(OSMemoryBarrier) R(OSSpinLockLock) R(OSSpinLockTry)
    R(OSSpinLockUnlock) R(dispatch_once) R(dispatch_sync) R(dispatch_async) R(dispatch_sync_f) R(dispatch_async_f)
    R(dispatch_group_create) R(dispatch_group_async) R(dispatch_group_wait) R(dispatch_get_global_queue) R(dispatch_queue_create) R(dispatch_release) R(_Block_copy) R(_Block_release)
    R(_Block_object_assign) R(_Block_object_dispose)
    {NULL, NULL},
};
