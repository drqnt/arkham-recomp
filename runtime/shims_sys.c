// Small system services the engine needs: UUIDs, machine name, zlib, and hardware queries answered honestly with "none".
#include "shim.h"
#include "shim_reg.h"

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <CoreGraphics/CoreGraphics.h>
#else
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#endif
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

u32 cf_wrap(CFTypeRef o);
CFTypeRef cf_unwrap(u32 h);

// ---------- CFUUID ----------
SHIM(CFUUIDCreate) { RET(cf_wrap(CFUUIDCreate(NULL))); }
// A 16-byte struct comes back through a hidden pointer that the callee pops.
SHIM(CFUUIDGetUUIDBytes) {
    u32 out = ARG(0);
    CFUUIDBytes b = CFUUIDGetUUIDBytes((CFUUIDRef)cf_unwrap(ARG(1)));
    memcpy(GP(out), &b, sizeof b);
    RET(out);
    c->esp += 4;
}
SHIM(CFUUIDCreateString) { RET(cf_wrap(CFUUIDCreateString(NULL, (CFUUIDRef)cf_unwrap(ARG(1))))); }
SHIM(CSCopyMachineName) { RET(cf_wrap(CFStringCreateWithCString(NULL, "Mac", kCFStringEncodingUTF8))); }

// ---------- zlib: the guest's z_stream is the 32-bit layout, the host stream lives behind its state field ----------
typedef struct {
    u32 next_in, avail_in, total_in, next_out, avail_out, total_out, msg, state, zalloc, zfree, opaque;
    i32 data_type;
    u32 adler, reserved;
} GZ;
static void gz_in(GZ *g, z_stream *z) {
    z->next_in = g->next_in ? (Bytef *)GP(g->next_in) : NULL;
    z->avail_in = g->avail_in;
    z->next_out = g->next_out ? (Bytef *)GP(g->next_out) : NULL;
    z->avail_out = g->avail_out;
}
static void gz_out(GZ *g, const z_stream *z) {
    g->next_in += g->avail_in - z->avail_in;
    g->next_out += g->avail_out - z->avail_out;
    g->total_in += g->avail_in - z->avail_in;
    g->total_out += g->avail_out - z->avail_out;
    g->avail_in = z->avail_in;
    g->avail_out = z->avail_out;
    g->adler = (u32)z->adler;
    g->data_type = z->data_type;
    g->msg = 0;
}
static z_stream *gz_new(GZ *g) {
    z_stream *z = calloc(1, sizeof *z);
    g->state = rt_handle_new(z);
    g->total_in = g->total_out = 0;
    g->msg = 0;
    return z;
}
static z_stream *gz_of(GZ *g) { return (z_stream *)rt_handle_get(g->state); }
static void gz_free(GZ *g) {
    z_stream *z = gz_of(g);
    rt_handle_free(g->state);
    g->state = 0;
    free(z);
}
SHIM(inflateInit_) {
    GZ *g = GP(ARG(0));
    z_stream *z = gz_new(g);
    gz_in(g, z);
    RET(inflateInit(z));
}
SHIM(inflateInit2_) {
    GZ *g = GP(ARG(0));
    z_stream *z = gz_new(g);
    gz_in(g, z);
    RET(inflateInit2(z, (int)ARG(1)));
}
SHIM(deflateInit2_) {
    GZ *g = GP(ARG(0));
    z_stream *z = gz_new(g);
    gz_in(g, z);
    RET(deflateInit2(z, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5)));
}
SHIM(inflate) {
    GZ *g = GP(ARG(0));
    z_stream *z = gz_of(g);
    gz_in(g, z);
    int r = inflate(z, (int)ARG(1));
    gz_out(g, z);
    RET(r);
}
SHIM(deflate) {
    GZ *g = GP(ARG(0));
    z_stream *z = gz_of(g);
    gz_in(g, z);
    int r = deflate(z, (int)ARG(1));
    gz_out(g, z);
    RET(r);
}
SHIM(inflateReset) {
    GZ *g = GP(ARG(0));
    g->total_in = g->total_out = 0;
    RET(inflateReset(gz_of(g)));
}
SHIM(deflateReset) {
    GZ *g = GP(ARG(0));
    g->total_in = g->total_out = 0;
    RET(deflateReset(gz_of(g)));
}
SHIM(inflateEnd) {
    GZ *g = GP(ARG(0));
    int r = inflateEnd(gz_of(g));
    gz_free(g);
    RET(r);
}
SHIM(deflateEnd) {
    GZ *g = GP(ARG(0));
    int r = deflateEnd(gz_of(g));
    gz_free(g);
    RET(r);
}
SHIM(adler32) { RET((u32)adler32(ARG(0), ARG(1) ? (const Bytef *)GP(ARG(1)) : NULL, ARG(2))); }
SHIM(crc32) { RET((u32)crc32(ARG(0), ARG(1) ? (const Bytef *)GP(ARG(1)) : NULL, ARG(2))); }
SHIM(compress) {
    uLongf n = G32(ARG(1));
    int r = compress((Bytef *)GP(ARG(0)), &n, (const Bytef *)GP(ARG(2)), ARG(3));
    G32(ARG(1)) = (u32)n;
    RET(r);
}
SHIM(uncompress) {
    uLongf n = G32(ARG(1));
    int r = uncompress((Bytef *)GP(ARG(0)), &n, (const Bytef *)GP(ARG(2)), ARG(3));
    G32(ARG(1)) = (u32)n;
    RET(r);
}

// ---------- hardware queries: no such hardware is reported rather than leaving out-parameters unset ----------
#define IO_ERROR 0xE00002BCu
SHIM(IOMasterPort) { G32(ARG(1)) = 0; RET(IO_ERROR); }
SHIM(IOServiceGetMatchingServices) { G32(ARG(2)) = 0; RET(IO_ERROR); }
// ---------- which process is in front: the engine stops reading the mouse when it isn't this one ----------
#include <objc/message.h>
static int scripted_mouse;
SHIM(GetFrontProcess) {
#if TARGET_OS_IPHONE
    int active = 1;  // the game only runs while it's on screen
#else
    id app = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    int active = scripted_mouse || ((signed char (*)(id, SEL))objc_msgSend)(app, sel_registerName("isActive"));
#endif
    G32(ARG(0)) = 0;
    G32(ARG(0) + 4) = active ? 2 : 1;  // the current process is {0, 2}
    RET(0);
}
SHIM(SameProcess) {
    G8(ARG(2)) = G32(ARG(0)) == G32(ARG(1)) && G32(ARG(0) + 4) == G32(ARG(1) + 4);
    RET(0);
}

// ---------- camera control without moving the real pointer ----------
// In play the game reads the pointer and warps it back to the window's centre every frame. Fast motion of a real pointer that
// way trips the system's "shake to locate" enlargement. So while those warps keep coming the real pointer stays frozen and
// hidden, and the game sees a position built from the mouse's motion events. Warps stopping, or the app losing focus, end it.
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <time.h>
static pthread_mutex_t cap_mu = PTHREAD_MUTEX_INITIALIZER;
static int cap_active, cap_ready;
static double cap_x, cap_y, cap_dx, cap_dy;
static u64 cap_last_warp;
static inline u64 now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }
#if TARGET_OS_IPHONE
// The phone has no pointer: the camera comes from a controller there, through shims_hid.m, and later from touch.
void rt_input_release(void) {}
static void cap_setup(void) { cap_ready = 1; }
#else
static void cap_release(void) {
    pthread_mutex_lock(&cap_mu);
    if (cap_active) {
        cap_active = 0;
        CGAssociateMouseAndMouseCursorPosition(1);
        CGDisplayShowCursor(kCGDirectMainDisplay);
        fprintf(stderr, "[input] camera capture ended\n");
    }
    pthread_mutex_unlock(&cap_mu);
}
void rt_input_release(void) { cap_release(); }
static void cap_setup(void) {
    cap_ready = 1;
    dispatch_async(dispatch_get_main_queue(), ^{
        // Mouse moved and the three dragged types.
        unsigned long long mask = (1ull << 5) | (1ull << 6) | (1ull << 7) | (1ull << 27);
        id handler = (id) ^ id(id ev) {
            double dx = ((double (*)(id, SEL))objc_msgSend)(ev, sel_registerName("deltaX"));
            double dy = ((double (*)(id, SEL))objc_msgSend)(ev, sel_registerName("deltaY"));
            pthread_mutex_lock(&cap_mu);
            if (cap_active) {
                cap_dx += dx;
                cap_dy += dy;
            }
            pthread_mutex_unlock(&cap_mu);
            return ev;
        };
        ((id(*)(id, SEL, unsigned long long, id))objc_msgSend)((id)objc_getClass("NSEvent"), sel_registerName("addLocalMonitorForEventsMatchingMask:handler:"), mask, handler);
        dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), 100 * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(t, ^{
            id app = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
            int active = ((signed char (*)(id, SEL))objc_msgSend)(app, sel_registerName("isActive"));
            if (cap_active && (!active || now_ms() - cap_last_warp > 300)) cap_release();
        });
        dispatch_resume(t);
    });
}
#endif

// The phone's swipes to look around (ios/app/TouchControls.m): motion added to what the game reads as the mouse while it holds the camera, in its points.
void rt_touch_look(double dx, double dy) {
    pthread_mutex_lock(&cap_mu);
    if (cap_active) {
        cap_dx += dx;
        cap_dy += dy;
    }
    pthread_mutex_unlock(&cap_mu);
}

// ---------- mouse and keyboard state ----------
// A scripted test run answers with its own position instead of the player's cursor.
static double scripted_x, scripted_y;
void rt_set_scripted_mouse(double x, double y) {
    scripted_mouse = 1;
    scripted_x = x;
    scripted_y = y;
}
void rt_scripted_mouse_get(double *x, double *y) {
    *x = scripted_x;
    *y = scripted_y;
}
// The game asks for 72 DPI global coordinates (origin at the top left); HIPoint is two floats on the guest.
SHIM(HIGetMousePosition) {
    u32 out = ARG(2);
    CGPoint p = CGPointMake(scripted_x, scripted_y);
    int captured = 0;
    if (!scripted_mouse) {
        pthread_mutex_lock(&cap_mu);
        if (cap_active) {
            cap_x += cap_dx;
            cap_y += cap_dy;
            cap_dx = cap_dy = 0;
            p = CGPointMake(cap_x, cap_y);
            captured = 1;
        }
        pthread_mutex_unlock(&cap_mu);
    }
#if !TARGET_OS_IPHONE
    if (!scripted_mouse && !captured) {
        CGEventRef e = CGEventCreate(NULL);
        if (e) {
            p = CGEventGetLocation(e);
            CFRelease(e);
        }
    }
#else
    (void)captured;
#endif
    if (out) {
        *(f32a *)GP(out) = (float)p.x;
        *(f32a *)GP(out + 4) = (float)p.y;
    }
    RET(0);
}
// The game measures mouse motion as the distance from where it last put the cursor, so a scripted cursor has to follow too.
SHIM(CGWarpMouseCursorPosition) {
    static u32 warps;
    if (warps++ % 600 == 0) fprintf(stderr, "[input] cursor warp %u to %.0f,%.0f\n", warps, ARGF(0), ARGF(1));
    if (scripted_mouse) {
        scripted_x = ARGF(0);
        scripted_y = ARGF(1);
        RET(0);
        return;
    }
    double x = ARGF(0), y = ARGF(1);
    if (!cap_ready) cap_setup();
    pthread_mutex_lock(&cap_mu);
    cap_last_warp = now_ms();
    cap_x = x;
    cap_y = y;
    int starting = !cap_active;
    if (starting) {
        cap_active = 1;
        cap_dx = cap_dy = 0;
#if !TARGET_OS_IPHONE
        CGWarpMouseCursorPosition(CGPointMake(x, y));
        CGAssociateMouseAndMouseCursorPosition(0);
        CGDisplayHideCursor(kCGDirectMainDisplay);
#endif
        fprintf(stderr, "[input] camera capture started\n");
    }
    pthread_mutex_unlock(&cap_mu);
    RET(0);
}
SHIM(CGAssociateMouseAndMouseCursorPosition) {
    static int last = -1;
    if ((int)(ARG(0) & 0xFF) != last) {
        last = (int)(ARG(0) & 0xFF);
        fprintf(stderr, "[input] game sets cursor association to %d, called from %08x\n", last, G32(c->esp));
    }
    // Camera capture decides this while it is on.
#if !TARGET_OS_IPHONE
    if (!scripted_mouse && !cap_active) RET(CGAssociateMouseAndMouseCursorPosition((boolean_t)(ARG(0) & 0xFF)));
    else RET(0);
#else
    RET(0);
#endif
}
#if TARGET_OS_IPHONE
// No pointer, keyboard layouts or keyboard state to ask on the phone: no characters, and no key down.
SHIM(CGSetLocalEventsSuppressionInterval) { RET(0); }
SHIM(UCKeyTranslate) {
    if (ARG(8)) G32(ARG(8)) = 0;
    RET(0);
}
SHIM(CGEventSourceKeyState) { RET(0); }
#else
SHIM(CGSetLocalEventsSuppressionInterval) { RET(CGSetLocalEventsSuppressionInterval(ARGD(0))); }
// The layout data was copied into guest memory by CFDataGetBytePtr, and its format holds offsets only.
SHIM(UCKeyTranslate) {
    UInt32 dead = ARG(6) ? G32(ARG(6)) : 0;
    UniCharCount actual = 0;
    OSStatus r = UCKeyTranslate((const UCKeyboardLayout *)GP(ARG(0)), (UInt16)ARG(1), (UInt16)ARG(2), ARG(3), ARG(4), ARG(5), &dead, ARG(7), &actual,
                                (UniChar *)GP(ARG(9)));
    if (ARG(6)) G32(ARG(6)) = dead;
    if (ARG(8)) G32(ARG(8)) = (u32)actual;
    RET((i32)r);
}
SHIM(CGEventSourceKeyState) { RET(CGEventSourceKeyState((CGEventSourceStateID)(i32)ARG(0), (CGKeyCode)(ARG(1) & 0xFFFF))); }
#endif

const ShimReg rt_shims_sys[] = {
    R(CFUUIDCreate) R(CFUUIDGetUUIDBytes) R(CFUUIDCreateString) R(CSCopyMachineName)
    R(inflateInit_) R(inflateInit2_) R(deflateInit2_) R(inflate) R(deflate) R(inflateReset) R(deflateReset)
    R(inflateEnd) R(deflateEnd) R(adler32) R(crc32) R(compress) R(uncompress)
    R(IOMasterPort) R(IOServiceGetMatchingServices)
    R(HIGetMousePosition) R(GetFrontProcess) R(SameProcess)
    R(UCKeyTranslate) R(CGWarpMouseCursorPosition) R(CGAssociateMouseAndMouseCursorPosition) R(CGSetLocalEventsSuppressionInterval) R(CGEventSourceKeyState)
    {NULL, NULL},
};
