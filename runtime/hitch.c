// ARKHAM_TRACE_HITCH=1: when a frame is late, sample what every thread is doing and report it once the frame arrives.
// Threads are suspended only while their stack is copied, with no allocation or locks, so they can't be holding what this needs.
// Turning addresses into names is slow, so the monitor misses whatever happens while it writes a report.
#include "shim.h"

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LATE_MS 40u
#define MAX_THREADS 96
#define MAX_SAMPLES 400
#define DEPTH 48

typedef struct {
    thread_act_t port;
    u32 samples;
    u32 slowest_stop, slowest_sample;
    u64 stack[MAX_SAMPLES][DEPTH];
    u8 depth[MAX_SAMPLES];
} Track;

static Track *tracks;
static u32 n_tracks;
static u64 last_frame;  // host time of the last presented frame

void rt_hitch_tick(void) { __atomic_store_n(&last_frame, mach_absolute_time(), __ATOMIC_RELAXED); }

// OpenGL calls that took long since the last report. The worker writes, the monitor reads: approximate is fine.
#define SLOW_NAMES 32
static struct {
    const char *name;
    u64 ticks;
    u32 n;
} slow[SLOW_NAMES];
void rt_hitch_note_call(const char *name, u64 ticks) {
    if (ticks < 24000 || !name) return;  // 1 ms
    for (int i = 0; i < SLOW_NAMES; i++) {
        if (slow[i].name == name || !slow[i].name) {
            slow[i].name = name;
            slow[i].ticks += ticks;
            slow[i].n++;
            return;
        }
    }
}

static inline u64 to_ms(u64 ticks) { return ticks * 125 / 3 / 1000000; }

// The thread list is fetched ahead of time, because task_threads waits for the address space lock, and a stall that holds that lock
// is one of the things to catch.
static thread_act_array_t known;
static mach_msg_type_number_t n_known;
static void refresh_threads(void) {
    thread_act_array_t list;
    mach_msg_type_number_t count;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return;
    if (known) {
        for (mach_msg_type_number_t i = 0; i < n_known; i++) mach_port_deallocate(mach_task_self(), known[i]);
        vm_deallocate(mach_task_self(), (vm_address_t)known, n_known * sizeof(thread_act_t));
    }
    known = list;
    n_known = count;
}
static void sample_all(void) {
    thread_act_array_t list = known;
    mach_msg_type_number_t count = n_known;
    static thread_act_t self;
    if (!self) self = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        thread_act_t t = list[i];
        if (t == self) continue;
        // The system's sound thread has a deadline, and stopping it to look at it could be heard.
        static thread_act_t skip[8];
        static u32 n_skip, n_checked;
        static thread_act_t checked[256];
        int skipping = 0;
        for (u32 k = 0; k < n_skip; k++) skipping |= skip[k] == t;
        if (skipping) continue;
        int seen = 0;
        for (u32 k = 0; k < n_checked; k++) seen |= checked[k] == t;
        if (!seen && n_checked < 256) {
            checked[n_checked++] = t;
            char name[64] = "";
            pthread_t pt = pthread_from_mach_thread_np(t);
            if (pt) pthread_getname_np(pt, name, sizeof name);
            if (strstr(name, "com.apple.audio") && n_skip < 8) {
                skip[n_skip++] = t;
                continue;
            }
        }
        Track *tr = NULL;
        for (u32 k = 0; k < n_tracks; k++)
            if (tracks[k].port == t) tr = &tracks[k];
        if (!tr && n_tracks < MAX_THREADS) {
            tr = &tracks[n_tracks++];
            tr->port = t;
            tr->samples = 0;
            tr->slowest_stop = 0;
        }
        if (!tr || tr->samples >= MAX_SAMPLES) continue;
        u64 t0 = mach_absolute_time();
        if (thread_suspend(t) != KERN_SUCCESS) continue;
        arm_thread_state64_t st;
        mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
            u64 *out = tr->stack[tr->samples];
            u32 d = 0;
            out[d++] = arm_thread_state64_get_pc(st);
            u64 fp = arm_thread_state64_get_fp(st);
            while (d < DEPTH && fp >= 0x100000000ull && fp < 0x800000000000ull && !(fp & 7)) {
                u64 next = ((u64 *)fp)[0], lr = ((u64 *)fp)[1];
                if (!lr || next <= fp) break;
                out[d++] = lr;
                fp = next;
            }
            tr->depth[tr->samples] = (u8)d;
            tr->samples++;
        }
        thread_resume(t);
        // A thread stuck inside a kernel call doesn't stop until it comes back, which says a lot about it.
        u64 stop_ms = to_ms(mach_absolute_time() - t0);
        if (stop_ms > tr->slowest_stop) {
            tr->slowest_stop = (u32)stop_ms;
            tr->slowest_sample = tr->samples ? tr->samples - 1 : 0;
        }
    }
}

static const char *sym_of(u64 pc) {
    Dl_info di;
    return dladdr((void *)pc, &di) && di.dli_sname ? di.dli_sname : "?";
}
static int is_waiting(const char *s) {
    static const char *const names[] = {"__psynch_cvwait", "__ulock_wait", "__ulock_wait2", "mach_msg2_trap", "__semwait_signal", "__workq_kernreturn",
                                        "semaphore_wait_trap", "semaphore_timedwait_trap", "kevent_id", "__select", "__psynch_mutexwait", "start_wqthread", NULL};
    for (const char *const *q = names; *q; q++)
        if (!strcmp(s, *q)) return 1;
    return 0;
}
// The function a stack is best described by: its innermost translated function, plus the system function it is inside, if any.
static void describe(const Track *tr, u32 s, char *out, size_t cap) {
    const u64 *st = tr->stack[s];
    u32 d = tr->depth[s];
    const char *leaf = sym_of(st[0]);
    size_t len = (size_t)snprintf(out, cap, "%s", leaf);
    int added = 0;
    for (u32 i = 1; i < d && added < 5 && len + 40 < cap; i++) {
        const char *f = sym_of(st[i]);
        if (strncmp(f, "F_", 2) && strncmp(f, "shim_", 5)) continue;
        len += (size_t)snprintf(out + len, cap - len, " < %s", f);
        added++;
    }
}
typedef struct {
    char text[260];
    u32 n;
} Line;
static void report(u64 ms) {
    static Line lines[64];
    u32 taken = 0;
    for (u32 k = 0; k < n_tracks; k++) taken = tracks[k].samples > taken ? tracks[k].samples : taken;
    fprintf(stderr, "[hitch] frame took %llu ms, %u samples\n", ms, taken);
    for (int i = 0; i < SLOW_NAMES && slow[i].name; i++) {
        if (slow[i].n) fprintf(stderr, "[hitch]  slow OpenGL: %s %u calls, %llu ms\n", slow[i].name, slow[i].n, to_ms(slow[i].ticks));
        slow[i].n = 0;
        slow[i].ticks = 0;
    }
    for (u32 k = 0; k < n_tracks; k++) {
        Track *tr = &tracks[k];
        if (tr->slowest_stop >= 20 && tr->samples) {
            char text[260], name[64] = "";
            describe(tr, tr->slowest_sample, text, sizeof text);
            pthread_t pt = pthread_from_mach_thread_np(tr->port);
            if (pt) pthread_getname_np(pt, name, sizeof name);
            fprintf(stderr, "[hitch]  thread %x %s took %u ms to stop, in: %s\n", tr->port, name, tr->slowest_stop, text);
        }
    }
    for (u32 k = 0; k < n_tracks; k++) {
        Track *tr = &tracks[k];
        u32 busy = 0, used = 0;
        for (u32 s = 0; s < tr->samples; s++) {
            char text[260];
            if (is_waiting(sym_of(tr->stack[s][0]))) {
                // Waiting for a lock counts: it says who is holding things up.
                const char *leaf = sym_of(tr->stack[s][0]);
                if (strcmp(leaf, "__ulock_wait2") && strcmp(leaf, "__psynch_mutexwait") && strcmp(leaf, "__psynch_cvwait")) continue;
                int guest = 0;
                for (u32 i = 1; i < tr->depth[s]; i++) guest |= !strncmp(sym_of(tr->stack[s][i]), "F_", 2);
                if (!guest) continue;
            }
            busy++;
            describe(tr, s, text, sizeof text);
            u32 j = 0;
            for (; j < used; j++)
                if (!strcmp(lines[j].text, text)) break;
            if (j == used && used < 64) {
                snprintf(lines[j].text, sizeof lines[j].text, "%s", text);
                lines[j].n = 0;
                used++;
            }
            if (j < 64) lines[j].n++;
        }
        // A thread that only waited in a condition variable is noise unless it waited on something in this frame.
        if (busy * 4 < tr->samples || !busy || tr->samples < 2) {
            tr->samples = 0;
            continue;
        }
        char name[64] = "";
        pthread_t pt = pthread_from_mach_thread_np(tr->port);
        if (pt) pthread_getname_np(pt, name, sizeof name);
        fprintf(stderr, "[hitch]  thread %x %s: %u of %u samples\n", tr->port, name, busy, tr->samples);
        for (int top = 0; top < 6; top++) {
            u32 best = used;
            for (u32 j = 0; j < used; j++)
                if (lines[j].n && (best == used || lines[j].n > lines[best].n)) best = j;
            if (best == used) break;
            fprintf(stderr, "[hitch]    %4u  %s\n", lines[best].n, lines[best].text);
            lines[best].n = 0;
        }
        tr->samples = 0;
    }
    n_tracks = 0;
}
static void *monitor(void *arg) {
    (void)arg;
    pthread_setname_np("hitch monitor");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    int late = 0;
    u64 seen = 0;
    u32 tick = 0;
    for (;;) {
        usleep(5000);
        u64 frame = __atomic_load_n(&last_frame, __ATOMIC_RELAXED);
        if (!late && (!known || ++tick % 200 == 0)) refresh_threads();
        if (!frame) continue;
        if (late && frame != seen) {
            report(to_ms(frame - seen));
            late = 0;
            continue;  // writing the report takes a while, so what was just read is out of date
        }
        if (to_ms(mach_absolute_time() - frame) >= LATE_MS) {
            if (!late) {
                seen = frame;
                late = 1;
            }
            sample_all();
        }
    }
    return NULL;
}
void rt_hitch_start(void) {
    if (!getenv("ARKHAM_TRACE_HITCH")) return;
    tracks = calloc(MAX_THREADS, sizeof *tracks);
    pthread_t t;
    pthread_create(&t, NULL, monitor, NULL);
    pthread_detach(t);
}
