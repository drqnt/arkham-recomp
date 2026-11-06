// Services the translated Bink library asks of the system: Multiprocessing Services, Carbon memory and time calls.
#include "shim.h"
#include "shim_reg.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

void rt_block_a_little(void);
void rt_event_notify(void);

#define MP_TIMEOUT ((u32)-29296)
#define DURATION_FOREVER 0x7FFFFFFF

static u64 now_us(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000; }
// A Duration counts milliseconds when positive and microseconds when negative.
static u64 deadline_of(i32 d) {
    if (d == DURATION_FOREVER) return ~0ull;
    return now_us() + (d >= 0 ? (u64)d * 1000 : (u64)(-(i64)d));
}

// ---------- critical regions: recursive locks that belong to a thread ----------
typedef struct { CPU *owner; int count; } MPRegion;
SHIM(MPCreateCriticalRegion) {
    G32(ARG(0)) = rt_handle_new(calloc(1, sizeof(MPRegion)));
    RET(0);
}
SHIM(MPDeleteCriticalRegion) {
    free(rt_handle_get(ARG(0)));
    rt_handle_free(ARG(0));
    RET(0);
}
SHIM(MPEnterCriticalRegion) {
    MPRegion *r = rt_handle_get(ARG(0));
    u64 end = deadline_of((i32)ARG(1));
    while (r->owner && r->owner != c) {
        if (now_us() >= end) {
            RET(MP_TIMEOUT);
            return;
        }
        rt_block_a_little();
    }
    r->owner = c;
    r->count++;
    RET(0);
}
SHIM(MPExitCriticalRegion) {
    MPRegion *r = rt_handle_get(ARG(0));
    if (--r->count == 0) {
        r->owner = NULL;
        rt_event_notify();
    }
    RET(0);
}

// ---------- semaphores ----------
typedef struct { i32 count, max; } MPSem;
SHIM(MPCreateSemaphore) {
    MPSem *s = calloc(1, sizeof *s);
    s->max = (i32)ARG(0);
    s->count = (i32)ARG(1);
    G32(ARG(2)) = rt_handle_new(s);
    RET(0);
}
SHIM(MPDeleteSemaphore) {
    free(rt_handle_get(ARG(0)));
    rt_handle_free(ARG(0));
    RET(0);
}
SHIM(MPSignalSemaphore) {
    MPSem *s = rt_handle_get(ARG(0));
    if (s->count < s->max) s->count++;
    rt_event_notify();
    RET(0);
}
SHIM(MPWaitOnSemaphore) {
    MPSem *s = rt_handle_get(ARG(0));
    u64 end = deadline_of((i32)ARG(1));
    while (s->count <= 0) {
        if (now_us() >= end) {
            RET(MP_TIMEOUT);
            return;
        }
        rt_block_a_little();
    }
    s->count--;
    RET(0);
}

// ---------- time: the guest's AbsoluteTime is nanoseconds, as its mach_absolute_time is ----------
SHIM(UpTime) { RET64(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)); }
SHIM(AbsoluteToNanoseconds) { RET64(ARGQ(0)); }

// ---------- Carbon memory ----------
SHIM(NewPtr) { RET(rt_guest_malloc(ARG(0))); }
SHIM(DisposePtr) { rt_guest_free(ARG(0)); }
SHIM(MaxBlock) { RET(0x20000000u); }
// A handle is a pointer to the block's address.
SHIM(TempNewHandle) {
    u32 h = rt_guest_malloc(4);
    G32(h) = rt_guest_malloc(ARG(0));
    if (ARG(1)) G16(ARG(1)) = 0;
    RET(h);
}
SHIM(TempHLock) { if (ARG(1)) G16(ARG(1)) = 0; }
SHIM(TempHUnlock) { if (ARG(1)) G16(ARG(1)) = 0; }
SHIM(TempDisposeHandle) {
    u32 h = ARG(0);
    if (h) {
        rt_guest_free(G32(h));
        rt_guest_free(h);
    }
    if (ARG(1)) G16(ARG(1)) = 0;
}

// ---------- old system interfaces the library probes for ----------
SHIM(GetSharedLibrary) { RET((u32)-2801); }  // cfragNoLibraryErr
SHIM(FindSymbol) { RET((u32)-2804); }        // cfragNoSymbolErr

const ShimReg rt_shims_bink[] = {
    R(MPCreateCriticalRegion) R(MPDeleteCriticalRegion) R(MPEnterCriticalRegion) R(MPExitCriticalRegion)
    R(MPCreateSemaphore) R(MPDeleteSemaphore) R(MPSignalSemaphore) R(MPWaitOnSemaphore)
    R(UpTime) R(AbsoluteToNanoseconds) R(NewPtr) R(DisposePtr) R(MaxBlock)
    R(TempNewHandle) R(TempHLock) R(TempHUnlock) R(TempDisposeHandle)
    R(GetSharedLibrary) R(FindSymbol)
    {NULL, NULL},
};
