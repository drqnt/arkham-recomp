// pthreads, semaphores, atomics and GCD. Guest threads run in parallel. Mutexes and condition variables keep their state in guest
// memory with atomic operations and sleep on a bucket chosen by address; rarer waits use one "something changed" event.
#include "shim.h"
#include "shim_reg.h"

#include <errno.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
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
void rt_block_a_little(void);
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

void rt_block_a_little(void) { block_a_little(); }

// Threads sleep on a bucket picked by the address they wait for. A waker changes the state first and only then looks for sleepers;
// a sleeper registers first and only then checks the state. Sequentially consistent operations make one of them see the other.
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    u32 waiters;
} Park;
#define PARK_N 64
static Park parks[PARK_N] = {[0 ... PARK_N - 1] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0}};
static inline Park *park_of(u64 a) { return &parks[((u32)(a >> 2) * 2654435761u) >> 26]; }
static inline void park_wake(u64 a) {
    Park *p = park_of(a);
    if (__atomic_load_n(&p->waiters, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&p->mu);
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
    }
}
// These primitives skip the shim lock, but in ARKHAM_SERIAL mode the thread holds the gate and must give it up to sleep.
static inline void park_enter(Park *p, int *held) {
    *held = rt_gate_held();
    if (*held) rt_gate_release();
    pthread_mutex_lock(&p->mu);
    __atomic_add_fetch(&p->waiters, 1, __ATOMIC_SEQ_CST);
}
static inline void park_leave(Park *p, int held) {
    __atomic_sub_fetch(&p->waiters, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&p->mu);
    if (held) rt_gate_acquire();
}
static inline void cpu_relax(void) { __asm__ volatile("isb"); }

// ---------- per-thread data ----------
#define MAX_KEYS 512
typedef struct GThread {
    CPU *cpu;
    u32 id;
    u32 start, arg;
    u32 retval;
    int done;
    int detached;
    int suspended;
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

static pthread_mutex_t tid_mu = PTHREAD_MUTEX_INITIALIZER;
static GThread *self_thread(void) {
    if (!tl_self) {
        GThread *t = calloc(1, sizeof *t);
        t->cpu = rt_current_cpu();
        t->host = pthread_self();
        pthread_mutex_lock(&tid_mu);
        t->id = next_tid++;
        threads[t->id] = t;
        pthread_mutex_unlock(&tid_mu);
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
static inline int owner_take(u32 *own, u32 me) {
    u32 cur = 0;
    return __atomic_compare_exchange_n(own, &cur, me, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
}
static int mutex_lock(u32 m) {
    u32 me = to_tid(self_thread());
    u32 *own = (u32 *)GP(m + 4);
    if (!owner_take(own, me)) {
        if (__atomic_load_n(own, __ATOMIC_RELAXED) == me) {
            if (M_TYPE(m) == PTM_RECURSIVE) {
                M_COUNT(m)++;
                return 0;
            }
            return 11;  // EDEADLK
        }
        // Most critical sections are short, so spin briefly before sleeping.
        int got = 0;
        for (int i = 0; i < 200 && !got; i++) {
            cpu_relax();
            got = !__atomic_load_n(own, __ATOMIC_RELAXED) && owner_take(own, me);
        }
        if (!got) {
            Park *p = park_of(m);
            int held;
            park_enter(p, &held);
            while (!owner_take(own, me)) pthread_cond_wait(&p->cv, &p->mu);
            park_leave(p, held);
        }
    }
    M_COUNT(m) = 1;
    return 0;
}
static int mutex_unlock(u32 m) {
    u32 me = to_tid(self_thread());
    u32 *own = (u32 *)GP(m + 4);
    if (__atomic_load_n(own, __ATOMIC_RELAXED) != me) return 1;
    if (--M_COUNT(m) == 0) {
        __atomic_store_n(own, 0, __ATOMIC_SEQ_CST);
        park_wake(m);
    }
    return 0;
}
SHIM(pthread_mutex_lock) { RET(mutex_lock(ARG(0))); }
SHIM(pthread_mutex_unlock) { RET(mutex_unlock(ARG(0))); }
SHIM(pthread_mutex_trylock) {
    u32 m = ARG(0);
    u32 me = to_tid(self_thread());
    u32 *own = (u32 *)GP(m + 4);
    if (owner_take(own, me)) {
        M_COUNT(m) = 1;
        RET(0);
    } else if (__atomic_load_n(own, __ATOMIC_RELAXED) == me && M_TYPE(m) == PTM_RECURSIVE) {
        M_COUNT(m)++;
        RET(0);
    } else {
        RET(16);
    }
}

// ---------- condition variables: a generation counter; POSIX allows spurious wakeups ----------
#define C_GEN(cv) ((u32 *)GP((cv) + 4))
SHIM(pthread_cond_init) { memset(GP(ARG(0)), 0, 28); RET(0); }
SHIM(pthread_cond_destroy) { RET(0); }
static void cond_wake(u32 cv) {
    __atomic_add_fetch(C_GEN(cv), 1, __ATOMIC_SEQ_CST);
    park_wake(cv);
}
SHIM(pthread_cond_signal) { cond_wake(ARG(0)); RET(0); }
SHIM(pthread_cond_broadcast) { cond_wake(ARG(0)); RET(0); }
// The caller still holds the mutex when it reads the generation, so a signal sent after it unlocks is seen.
static u32 cond_sleep(u32 cv, u32 m, const struct timespec *deadline) {
    u32 g = __atomic_load_n(C_GEN(cv), __ATOMIC_SEQ_CST);
    u32 cnt = M_COUNT(m);
    M_COUNT(m) = 1;
    mutex_unlock(m);
    Park *p = park_of(cv);
    int held;
    u32 rc = 0;
    park_enter(p, &held);
    while (__atomic_load_n(C_GEN(cv), __ATOMIC_SEQ_CST) == g) {
        if (!deadline) {
            pthread_cond_wait(&p->cv, &p->mu);
        } else if (pthread_cond_timedwait(&p->cv, &p->mu, deadline) == ETIMEDOUT) {
            if (__atomic_load_n(C_GEN(cv), __ATOMIC_SEQ_CST) == g) rc = 60;
            break;
        }
    }
    park_leave(p, held);
    mutex_lock(m);
    M_COUNT(m) = cnt;
    return rc;
}
SHIM(pthread_cond_wait) { RET(cond_sleep(ARG(0), ARG(1), NULL)); }
SHIM(pthread_cond_timedwait) {
    u32 ts = ARG(2);
    struct timespec deadline = {(time_t)(i32)G32(ts), (long)G32(ts + 4)};
    RET(cond_sleep(ARG(0), ARG(1), &deadline));
}

// ---------- thread lifetime ----------
// CPU time of the guest's threads, for the [fps] line: which one is the limit, measured instead of guessed from frame rates the GPU also sets.
#define TRACKED 64
static struct {
    thread_t port;
    u32 start;
    u64 last_us;
} tracked[TRACKED];
static _Atomic int n_tracked;
static void track_thread(u32 start) {
    int i = atomic_fetch_add(&n_tracked, 1);
    if (i >= TRACKED) return;
    tracked[i].start = start;
    tracked[i].port = mach_thread_self();
}
// The busiest guest threads since the last call, as CPU time a frame, with the address each started at.
void rt_thread_cpu_report(char *out, size_t cap, double frames) {
    struct {
        double ms;
        u32 start;
    } top[3] = {{0, 0}, {0, 0}, {0, 0}};
    int n = atomic_load(&n_tracked);
    if (n > TRACKED) n = TRACKED;
    for (int i = 0; i < n; i++) {
        if (!tracked[i].port) continue;
        thread_basic_info_data_t info;
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        if (thread_info(tracked[i].port, THREAD_BASIC_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS) continue;
        u64 us = (u64)info.user_time.seconds * 1000000u + (u64)info.user_time.microseconds + (u64)info.system_time.seconds * 1000000u + (u64)info.system_time.microseconds;
        double ms = (double)(us - tracked[i].last_us) / 1000.0 / (frames > 0 ? frames : 1);
        tracked[i].last_us = us;
        for (int k = 0; k < 3; k++) {
            if (ms > top[k].ms) {
                for (int m = 2; m > k; m--) top[m] = top[m - 1];
                top[k].ms = ms;
                top[k].start = tracked[i].start;
                break;
            }
        }
    }
    snprintf(out, cap, "%.2f (%x), %.2f (%x), %.2f (%x)", top[0].ms, top[0].start, top[1].ms, top[1].start, top[2].ms, top[2].start);
}
static void *thread_tramp(void *p) {
    GThread *t = p;
    track_thread(t->start);
    rt_gate_acquire();
    tl_self = t;
    while (t->suspended) block_a_little();
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
static int suspended_create;
SHIM(pthread_create) {
    GThread *t = calloc(1, sizeof *t);
    t->start = ARG(2);
    t->arg = ARG(3);
    t->suspended = suspended_create;
    pthread_mutex_lock(&tid_mu);
    t->id = next_tid++;
    threads[t->id] = t;
    pthread_mutex_unlock(&tid_mu);
    G32(ARG(0)) = to_tid(t);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if TARGET_OS_IPHONE
    // The phone's address space is tighter: the 4 GB window leaves about 2 GB in one piece (logs/probe_*.txt).
    pthread_attr_setstacksize(&attr, 32u << 20);
#else
    pthread_attr_setstacksize(&attr, 64u << 20);
#endif
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // The default class may land on efficiency cores, and the frame rate depends on these threads.
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    int r = pthread_create(&t->host, &attr, thread_tramp, t);
    if (getenv("ARKHAM_TRACE_THREADS")) fprintf(stderr, "[thread] created %u start=%08x\n", t->id, t->start);
    RET(r);
}
// The Bink library starts its threads suspended. A thread's Mach port is its id here, so thread_resume can find it.
SHIM(pthread_create_suspended_np) {
    suspended_create = 1;
    shim_pthread_create(c);
    suspended_create = 0;
}
SHIM(thread_resume) {
    GThread *t = from_tid(ARG(0));
    if (t) t->suspended = 0;
    rt_event_notify();
    RET(0);
}
SHIM(thread_policy_set) { RET(0); }
static void yield_now(void) {
    if (rt_gate_held()) rt_gate_yield();
    else sched_yield();
}
SHIM(pthread_yield_np) { yield_now(); RET(0); }
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
// The game hops to the main thread for a few queries that are only reads: the window's frame, whether it is key and the mouse
// position. It does that hundreds of times a second, and each hop stalls the game thread, so these places run where they are.
SHIM(pthread_main_np) {
    u32 from = G32(c->esp);
    if (from == 0x1c51020u || from == 0x1c50a23u || from == 0x1c40d76u) {
        RET(1);
        return;
    }
    RET(self_thread()->id == 1);
}
SHIM(pthread_kill) { RET(0); }
SHIM(pthread_cancel) { RET(0); }
SHIM(pthread_setname_np) { RET(0); }
SHIM(pthread_getname_np) { if (ARG(2)) G8(ARG(1)) = 0; RET(0); }
SHIM(pthread_getschedparam) { G32(ARG(1)) = 1; G32(ARG(2)) = 31; RET(0); }
SHIM(pthread_setschedparam) { RET(0); }
SHIM(sched_get_priority_max) { RET(47); }
SHIM(sched_get_priority_min) { RET(15); }
SHIM(sched_yield) { yield_now(); RET(0); }
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
_Static_assert(MAX_KEYS == RT_TSD_MAX, "translated code knows the slot count");
u32 rt_tsd_get_slow(CPU *c, u32 k) {
    c->tsd = self_thread()->tsd;
    return k < MAX_KEYS ? c->tsd[k] : 0;
}
void rt_tsd_set_slow(CPU *c, u32 k, u32 v) {
    c->tsd = self_thread()->tsd;
    if (k < MAX_KEYS) c->tsd[k] = v;
}
SHIM(pthread_getspecific) { RET(rt_tsd_get(c, ARG(0))); }
SHIM(pthread_setspecific) { rt_tsd_set(c, ARG(0), ARG(1)); RET(0); }
SHIM(pthread_once) {
    u32 o = ARG(0);
    // Whoever comes second waits for the first to finish, because it relies on what the routine set up.
    if (G32(o + 4) == 0x1BADC0DE) {
        RET(0);
        return;
    }
    if (G32(o + 4) == 0x1BADC0DD) {
        while (G32(o + 4) != 0x1BADC0DE) block_a_little();
        RET(0);
        return;
    }
    G32(o + 4) = 0x1BADC0DD;
    rt_call_guest(c, ARG(1), 0, NULL);
    G32(o + 4) = 0x1BADC0DE;
    rt_event_notify();
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
static inline int sem_take(NSem *s) {
    int n = __atomic_load_n(&s->count, __ATOMIC_SEQ_CST);
    while (n > 0)
        if (__atomic_compare_exchange_n(&s->count, &n, n - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 1;
    return 0;
}
static void sem_give(NSem *s) {
    __atomic_add_fetch(&s->count, 1, __ATOMIC_SEQ_CST);
    park_wake((u64)s);
}
// Returns 0 once taken, 1 when the deadline passed. A NULL deadline waits for good.
static int sem_sleep(NSem *s, const struct timespec *deadline) {
    if (sem_take(s)) return 0;
    Park *p = park_of((u64)s);
    int held, rc = 0;
    park_enter(p, &held);
    while (!sem_take(s)) {
        if (!deadline) {
            pthread_cond_wait(&p->cv, &p->mu);
        } else if (pthread_cond_timedwait(&p->cv, &p->mu, deadline) == ETIMEDOUT) {
            rc = !sem_take(s);
            break;
        }
    }
    park_leave(p, held);
    return rc;
}
SHIM(sem_post) { sem_give(rt_handle_get(ARG(0))); RET(0); }
SHIM(sem_wait) { sem_sleep(rt_handle_get(ARG(0)), NULL); RET(0); }

SHIM(dispatch_semaphore_create) {
    NSem *s = calloc(1, sizeof *s);
    s->count = (int)ARG(0);
    RET(rt_handle_new(s));
}
SHIM(dispatch_semaphore_signal) { sem_give(rt_handle_get(ARG(0))); RET(0); }
SHIM(dispatch_semaphore_wait) {
    NSem *s = rt_handle_get(ARG(0));
    u64 timeout = ARGQ(1);  // dispatch_time below just passes the delay through
    if (timeout == ~0ull) {
        sem_sleep(s, NULL);
        RET(0);
        return;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    u64 ns = (u64)tv.tv_sec * 1000000000ull + (u64)tv.tv_usec * 1000ull + timeout;
    struct timespec deadline = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    RET(sem_sleep(s, &deadline) ? 1 : 0);
}
SHIM(dispatch_time) { RET64(ARGQ(1)); }

// ---------- atomics: guest threads run in parallel, and inline lock-prefixed code touches the same words ----------
SHIM(OSAtomicAdd32) { RET(__atomic_add_fetch((u32 *)GP(ARG(1)), ARG(0), __ATOMIC_SEQ_CST)); }
SHIM(OSAtomicAdd64) {
    u64 v = __atomic_add_fetch((u64 *)GP(ARG(2)), ARGQ(0), __ATOMIC_SEQ_CST);
    RET64(v);
}
SHIM(OSAtomicCompareAndSwap32) {
    u32 expect = ARG(0);
    RET(__atomic_compare_exchange_n((u32 *)GP(ARG(2)), &expect, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1 : 0);
}
SHIM(OSMemoryBarrier) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static int spin_take(u32 l) {
    u32 expect = 0;
    return __atomic_compare_exchange_n((u32 *)GP(l), &expect, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SHIM(OSSpinLockLock) {
    u32 l = ARG(0);
    int held = rt_gate_held();
    if (held) rt_gate_release();
    for (u32 i = 0; !spin_take(l); i++) {
        if (i < 2000) cpu_relax();
        else sched_yield();
    }
    if (held) rt_gate_acquire();
}
SHIM(OSSpinLockTry) { RET(spin_take(ARG(0)) ? 1 : 0); }
SHIM(OSSpinLockUnlock) { __atomic_store_n((u32 *)GP(ARG(0)), 0, __ATOMIC_SEQ_CST); }

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
    if (!held) rt_gate_acquire_now();
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
#if TARGET_OS_IPHONE
// The phone's main queue belongs to UIKit, and the game's main thread is another (rt_main): work for it goes to that thread's run loop, which the
// stand-in NSApplication turns (ios_appkit.m).
#include <CoreFoundation/CoreFoundation.h>
static void to_guest_main(DCtx *d, dispatch_semaphore_t done) {
    CFRunLoopRef loop = rt_guest_main_loop();
    CFRunLoopPerformBlock(loop, kCFRunLoopCommonModes, ^{
        main_queue_tramp(d);
        if (done) dispatch_semaphore_signal(done);
    });
    CFRunLoopWakeUp(loop);
}
#endif
static void run_on_main_sync(DCtx *d) {
    if (rt_on_guest_main()) {
        main_queue_tramp(d);
        return;
    }
    rt_gate_release();
#if TARGET_OS_IPHONE
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    to_guest_main(d, done);
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    dispatch_release(done);
#else
    dispatch_sync_f(dispatch_get_main_queue(), d, main_queue_tramp);
#endif
    rt_gate_acquire_now();
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
#if TARGET_OS_IPHONE
    to_guest_main(d, NULL);
#else
    dispatch_async_f(dispatch_get_main_queue(), d, main_queue_tramp);
#endif
}
// ARKHAM_TRACE_DISPATCH: how often each place in the game synchronously waits for the main thread.
static void note_main_sync(u32 site) {
    static int on = -1;
    static struct { u32 site, n; } sites[64];
    static u64 last;
    if (on < 0) on = getenv("ARKHAM_TRACE_DISPATCH") != NULL;
    if (!on) return;
    for (int i = 0; i < 64; i++) {
        if (sites[i].site == site || !sites[i].site) {
            sites[i].site = site;
            sites[i].n++;
            break;
        }
    }
    u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000000;
    if (now - last >= 5) {
        last = now;
        fprintf(stderr, "[dispatch]");
        for (int i = 0; i < 64 && sites[i].site; i++) {
            fprintf(stderr, " %08x:%u", sites[i].site, sites[i].n);
            sites[i].n = 0;
        }
        fprintf(stderr, "\n");
    }
}
SHIM(dispatch_sync) {
    if (is_main_queue(ARG(0))) {
        note_main_sync(G32(c->esp));
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
u32 rt_block_copy(CPU *c, u32 b) { return block_copy(c, b); }
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
    R(pthread_create_suspended_np) R(thread_resume) R(thread_policy_set) R(pthread_yield_np) R2("_pthread_join$UNIX2003", pthread_join)
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
