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
static int tracks_for_late;  // ARKHAM_TRACE_HITCH: reports of late frames

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
            // Only frames inside the thread's own stack, between where it is now and its top: on the phone a frame pointer can lead out of it, into a
            // guard page, and reading there ended the game.
            u64 lo = arm_thread_state64_get_sp(st), hi = 0;
            pthread_t owner = pthread_from_mach_thread_np(t);
            if (owner) hi = (u64)(uintptr_t)pthread_get_stackaddr_np(owner);
            if (!hi || hi <= lo) hi = lo;  // unknown: the leaf only
            while (d < DEPTH && fp >= lo && fp + 16 <= hi && !(fp & 7)) {
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
// The innermost frame of a profile line. In the runtime's own code it also gets the offset into its function, to 16 bytes, which tells a loop
// from the rest of the function. That was needed when the startup's 2.5 s on the phone showed only as "phase2" (eh.c).
// Not for translated functions (F_), where the name says enough. Outside the game's own executable, the image it's in instead, waits aside:
// v17's profile still showed the startup's 2.5 s as "phase2" with no offset, so not the runtime's phase2, and only the image can say whose.
static int is_waiting(const char *s);
static const char *leaf_of(u64 pc, char *buf, size_t cap) {
    Dl_info di;
    if (!dladdr((void *)pc, &di) || !di.dli_sname) return "?";
    static const void *main_base;
    if (!main_base) {
        Dl_info me;
        if (dladdr((void *)leaf_of, &me)) main_base = me.dli_fbase;
    }
    if (di.dli_fbase != main_base) {
        if (is_waiting(di.dli_sname) || !di.dli_fname) return di.dli_sname;
        const char *img = strrchr(di.dli_fname, '/');
        snprintf(buf, cap, "%s(%s)", di.dli_sname, img ? img + 1 : di.dli_fname);
        return buf;
    }
    if (!strncmp(di.dli_sname, "F_", 2)) return di.dli_sname;
    snprintf(buf, cap, "%s+0x%llx", di.dli_sname, (unsigned long long)((pc - (u64)(uintptr_t)di.dli_saddr) & ~15ull));
    return buf;
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
// ARKHAM_TRACE_STALL=s: when no frame has gone on screen for s seconds, counting from the start too, a report of what every thread is
// doing, again every s seconds while it lasts. For a game that hangs before its first frame, where a late-frame report never comes.
static u64 stall_ticks, started_at, stall_reported;
static void stall_check(u64 frame) {
    u64 now = mach_absolute_time(), since = frame ? frame : started_at;
    if (now - since < stall_ticks || now - stall_reported < stall_ticks) return;
    stall_reported = now;
    refresh_threads();
    for (int i = 0; i < 60; i++) {
        sample_all();
        usleep(5000);
    }
    fprintf(stderr, "[hitch] no frame for %llu ms%s; every thread, waiting or not, and its last stack:\n", to_ms(now - since), frame ? "" : " since the start");
    for (u32 k = 0; k < n_tracks; k++) {
        Track *tr = &tracks[k];
        if (!tr->samples) continue;
        char name[64] = "";
        pthread_t pt = pthread_from_mach_thread_np(tr->port);
        if (pt) pthread_getname_np(pt, name, sizeof name);
        u32 s = tr->samples - 1, d = tr->depth[s];
        char text[700];
        size_t len = 0;
        for (u32 i = 0; i < d && i < 14 && len + 60 < sizeof text; i++) len += (size_t)snprintf(text + len, sizeof text - len, "%s%s", i ? " < " : "", sym_of(tr->stack[s][i]));
        fprintf(stderr, "[hitch]  thread %x %s: %s\n", tr->port, name, len ? text : "(no stack)");
        tr->samples = 0;
    }
    n_tracks = 0;
}
// ARKHAM_TRACE_PROFILE=s: every s seconds, half a second of samples of every thread, waiting or not, and where each one was most often: in steady play,
// what a thread waits for is what holds the frame rate.
static u64 profile_ticks, profile_last, profile_until;  // ARKHAM_TRACE_PROFILE_FOR=s: only for the first s seconds, as for a startup
// ARKHAM_TRACE_PROFILE_PLAY=from:for:every, in seconds after the start: a second window once the first is over, as for play.
// The phone's first launch of a build takes 90:20:2, the game being in play by then, with a block only every 2 s so as to slow it less.
static u64 play_from, play_for, play_every;
static void describe_full(const Track *tr, u32 s, char *out, size_t cap) {
    size_t len = 0;
    int shown = 0;
    for (u32 i = 0; i < tr->depth[s] && shown < 7 && len + 60 < cap; i++) {
        char leaf[160];
        const char *f = i ? sym_of(tr->stack[s][i]) : leaf_of(tr->stack[s][i], leaf, sizeof leaf);
        if (i && !strcmp(f, "?")) continue;
        len += (size_t)snprintf(out + len, cap - len, "%s%s", shown ? " < " : "", f);
        shown++;
    }
}
static void profile_check(void) {
    u64 now = mach_absolute_time();
    if (profile_until && now > profile_until) {
        if (play_from) {
            profile_ticks = play_every;
            profile_last = play_from - play_every;  // the first block at play_from
            profile_until = play_from + play_for;
            play_from = 0;
            fprintf(stderr, "[profile] done for now; again in play\n");
            return;
        }
        profile_ticks = 0;
        fprintf(stderr, "[profile] done\n");
        return;
    }
    if (now < profile_last || now - profile_last < profile_ticks) return;
    profile_last = now;
    refresh_threads();
    for (int i = 0; i < 100; i++) {
        sample_all();
        usleep(5000);
    }
    fprintf(stderr, "[profile] where each thread was, in 100 samples over half a second:\n");
    static Line lines[96];
    for (u32 k = 0; k < n_tracks; k++) {
        Track *tr = &tracks[k];
        if (tr->samples < 20) {
            tr->samples = 0;
            continue;
        }
        u32 used = 0;
        for (u32 s = 0; s < tr->samples; s++) {
            char text[260];
            describe_full(tr, s, text, sizeof text);
            u32 j = 0;
            for (; j < used; j++)
                if (!strcmp(lines[j].text, text)) break;
            if (j == used && used < 96) {
                snprintf(lines[j].text, sizeof lines[j].text, "%s", text);
                lines[j].n = 0;
                used++;
            }
            if (j < 96) lines[j].n++;
        }
        // Threads that only ever sleep in the system's thread pool or a run loop say nothing.
        int quiet = used == 1 && (strstr(lines[0].text, "__workq_kernreturn") || strstr(lines[0].text, "mach_msg2_trap < mach_msg_overwrite < mach_msg <"));
        if (!quiet) {
            char name[64] = "";
            pthread_t pt = pthread_from_mach_thread_np(tr->port);
            if (pt) pthread_getname_np(pt, name, sizeof name);
            fprintf(stderr, "[profile]  thread %x %s:\n", tr->port, name);
            for (int top = 0; top < 4; top++) {
                u32 best = used;
                for (u32 j = 0; j < used; j++)
                    if (lines[j].n && (best == used || lines[j].n > lines[best].n)) best = j;
                if (best == used) break;
                fprintf(stderr, "[profile]    %3u  %s\n", lines[best].n, lines[best].text);
                lines[best].n = 0;
            }
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
        if (stall_ticks) stall_check(frame);
        if (profile_ticks) profile_check();
        if (!frame || !tracks_for_late) continue;
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
    const char *stall = getenv("ARKHAM_TRACE_STALL");
    tracks_for_late = getenv("ARKHAM_TRACE_HITCH") != NULL;
    if (stall && atof(stall) > 0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        stall_ticks = (u64)(atof(stall) * 1e9 * tb.denom / tb.numer);
        started_at = mach_absolute_time();
    }
    const char *prof = getenv("ARKHAM_TRACE_PROFILE");
    if (prof && atof(prof) > 0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        profile_ticks = (u64)(atof(prof) * 1e9 * tb.denom / tb.numer);
        profile_last = mach_absolute_time();
        const char *for_s = getenv("ARKHAM_TRACE_PROFILE_FOR");
        if (for_s && atof(for_s) > 0) profile_until = profile_last + (u64)(atof(for_s) * 1e9 * tb.denom / tb.numer);
        double from = 0, len = 0, every = 0;
        const char *play = getenv("ARKHAM_TRACE_PROFILE_PLAY");
        if (profile_until && play && sscanf(play, "%lf:%lf:%lf", &from, &len, &every) == 3 && len > 0 && every > 0) {
            u64 s = (u64)(1e9 * tb.denom / tb.numer);  // ticks a second
            play_from = profile_last + (u64)(from * s);
            play_for = (u64)(len * s);
            play_every = (u64)(every * s);
        }
    }
    if (!tracks_for_late && !stall_ticks && !profile_ticks) return;
    tracks = calloc(MAX_THREADS, sizeof *tracks);
    pthread_t t;
    pthread_create(&t, NULL, monitor, NULL);
    pthread_detach(t);
}
