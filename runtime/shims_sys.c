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
#include <math.h>
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
// The phone has no pointer: there the camera's swipes are its motion (rt_touch_mouse below), and a controller's stick is the game's own.
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

// The phone's camera swipes (ios/app/TouchControls.m) as a mouse's motion, in pixels: while the game holds the cursor, what's added here goes to the
// engine at the next frame's reading of the pointer (touch_mouse_to_engine, below). Motion the game hasn't taken for a quarter of a second, as in a
// film or a menu that still warps, is dropped rather than turned all at once later.
static u64 cap_taken_ms;
void rt_touch_mouse(double dx, double dy) {
    pthread_mutex_lock(&cap_mu);
    if (cap_active) {
        if (now_ms() - cap_taken_ms > 250) cap_dx = cap_dy = 0;
        cap_dx += dx;
        cap_dy += dy;
    }
    pthread_mutex_unlock(&cap_mu);
}
// The phone's touches (ios/app/TouchControls.m). Whether the game holds the cursor, warping it back each frame as it does in play: then a tap mustn't
// be a click, which is a strike in play. And where a tap puts the cursor, for menus, which the game reads through HIGetMousePosition below.
int rt_game_holds_cursor(void) {
    pthread_mutex_lock(&cap_mu);
    int held = cap_active && now_ms() - cap_last_warp < 300;
    pthread_mutex_unlock(&cap_mu);
    return held;
}
static int touch_cursor;
static double touch_x, touch_y;
void rt_touch_cursor(double x, double y) {
    pthread_mutex_lock(&cap_mu);
    touch_cursor = 1;
    touch_x = x;
    touch_y = y;
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
// ARKHAM_TRACE_MOUSE=n: every nth read of the pointer, who asked (six callers up through the frame pointers) and what it got; for finding where
// the game turns the pointer's motion into input, and why it drops it while a pad's stick is held.
static void trace_mouse(CPU *c, double x, double y) {
    static int every = -1, n;
    if (every < 0) every = getenv("ARKHAM_TRACE_MOUSE") ? atoi(getenv("ARKHAM_TRACE_MOUSE")) : 0;
    if (every <= 0 || n++ % every) return;
    u32 at[6];
    at[0] = G32(c->esp);
    u32 bp = c->ebp;
    for (int i = 1; i < 6; i++) {
        at[i] = bp > 0x1000 && bp < 0xFFFFFFF0u ? G32(bp + 4) : 0;
        bp = bp > 0x1000 && bp < 0xFFFFFFF0u ? G32(bp) : 0;
    }
    unsigned rt_frames_presented(void);
    fprintf(stderr, "[mouse] read %d at frame %u: %.1f,%.1f from %08x < %08x < %08x < %08x < %08x < %08x\n", n, rt_frames_presented(), x, y, at[0], at[1], at[2], at[3],
            at[4], at[5]);
}
// Feral's input loop (F_19deea0) drops the pointer's move, counting down, while a counter of its window object (at +0x4d4) is above zero; the pad
// code raises it, so that a mouse doesn't fight a held stick, and walking holds one. Found with ARKHAM_TRACE_MOUSE and scripted runs (docs/IOS_PORT.md).
// When the pointer is read from there (F_1c40d00 for F_0048c10 for F_19deea0, found by the frames' return addresses), the counter is at hand:
// [F_19deea0's ebp - 0xc8] is the window object.
static u32 mouse_skip_counter(CPU *c) {
    u32 bp = c->ebp;
    if (bp < 0x1000 || G32(bp + 4) != 0x48c4bu) return 0;
    u32 bp1 = G32(bp);
    if (bp1 < 0x1000 || G32(bp1 + 4) != 0x19df02fu) return 0;
    u32 bp2 = G32(bp1);
    u32 window = bp2 > 0x1000 ? G32(bp2 - 0xc8) : 0;
    return window ? window + 0x4d4 : 0;
}
// The game drops the pointer's motion while a pad is in use, twice over. Feral's input loop (F_19deea0) hands the motion to the engine as the MouseX
// and MouseY axes, marked as not from a pad, and the engine ignores those while a pad is in use. And before that, the loop throws the motion away
// while the counter above is raised, which every event of a pad's stick does (they're handled inside the loop, after the pointer is read), so a thumb
// on the touch controls' stick, never quite still, kept it raised.
// On the phone the touch camera's motion therefore never goes through the pointer: at the loop's reading of it, the runtime calls the engine's
// InputAxis itself, with what the loop would pass (its viewport client, window and frame time, the names MouseX and MouseY) and marked as from a pad,
// and the pointer stays where the game put it. Then the camera turns through the game's own mouse bindings and sensitivity, walking or not.
// ARKHAM_MOUSE_AS_PAD=1, for the Mac's own mouse, marks the loop's two calls as from a pad instead: the engine's InputAxis, a virtual function of the
// viewport client, is called through the dispatcher, so a host function in front of it at that address sees each call and changes the last argument
// (bGamepad) of only those two, known by where they return to. That fixes the engine's half only.
guest_fn rt_guest_fn(u32 addr);           // rt.c: the translated function at a guest address
void rt_ht_add(u32 addr, guest_fn fn);  // rt.c: a host function in front of it for calls through the dispatcher
static guest_fn input_axis_orig;
static u32 input_axis_at;
static void input_axis_as_pad(CPU *c) {
    u32 ret = G32(c->esp);
    if (ret == 0x19df2eau || ret == 0x19df353u) G32(c->esp + 0x20) = 1;
    c->jt = input_axis_at;
    input_axis_orig(c);
}
static int mouse_as_pad(void) {
    static int on = -1;
    if (on < 0) on = getenv("ARKHAM_MOUSE_AS_PAD") != NULL;
    return on;
}
static void install_mouse_as_pad(u32 bp2) {
    if (input_axis_orig || !mouse_as_pad() || bp2 < 0x1000) return;
    u32 client = G32(bp2 - 0xc0);
    u32 vtable = client ? G32(client) : 0;
    u32 fn = vtable ? G32(vtable + 0x10) : 0;
    guest_fn orig = fn ? rt_guest_fn(fn) : NULL;
    if (!orig) return;
    input_axis_orig = orig;
    input_axis_at = fn;
    rt_ht_add(fn, input_axis_as_pad);
    fprintf(stderr, "[mouse] the pointer's motion reaches the engine as a pad's (its InputAxis at %08x)\n", fn);
}
#if TARGET_OS_IPHONE
// The touch camera's motion to the engine, as Feral's input loop would hand a mouse's, from inside that loop's reading of the pointer. bp2 is the loop's
// frame: [-0xc0] its viewport client, [-0xc8] its window object (the viewport is 4 bytes in), [+0xc] the frame's time, and [-0xbc] the address its
// code takes its globals from (0x19deeb1), where the names MouseX and MouseY are at +0x1aaf027 and +0x1aaf02b.
static void touch_mouse_to_engine(CPU *c, u32 bp2, double dx, double dy) {
    static u64 sent_frames, told_frames, told_ms;
    static double sent_x, sent_y;
    u32 client = G32(bp2 - 0xc0), window = G32(bp2 - 0xc8), base = G32(bp2 - 0xbc);
    u32 vtable = client ? G32(client) : 0, fn = vtable ? G32(vtable + 0x10) : 0;
    if (!fn || base != 0x19deeb1u) {
        static int warned;
        if (!warned++) fprintf(stderr, "[mouse] the touch camera can't reach the engine: client %08x, globals at %08x\n", client, base);
        return;
    }
    const u32 names[2] = {G32(base + 0x1aaf027u), G32(base + 0x1aaf02bu)};
    const double d[2] = {dx, dy};
    for (int k = 0; k < 2; k++) {
        if (d[k] == 0 || !names[k]) continue;
        float f = (float)d[k];
        u32 bits;
        memcpy(&bits, &f, 4);
        u32 args[8] = {client, window ? window + 4 : 0, 0, G32(names[k]), G32(names[k] + 4), bits, G32(bp2 + 0xc), 1};
        rt_call_guest(c, fn, 8, args);
    }
    sent_frames++;
    sent_x += fabs(dx);
    sent_y += fabs(dy);
    if (now_ms() - told_ms > 10000 && sent_frames != told_frames) {
        told_ms = now_ms();
        told_frames = sent_frames;
        fprintf(stderr, "[mouse] touch camera: %llu frames with motion, %.0f px across and %.0f px up and down so far, straight to the engine\n", sent_frames, sent_x, sent_y);
    }
}
#endif
SHIM(HIGetMousePosition) {
    u32 out = ARG(2);
    u32 bp2 = 0;  // Feral's input loop's frame, when the pointer is read from there (see mouse_skip_counter)
    {
        u32 bp = c->ebp;
        if (bp > 0x1000 && G32(bp + 4) == 0x48c4bu) {
            u32 bp1 = G32(bp);
            if (bp1 > 0x1000 && G32(bp1 + 4) == 0x19df02fu) bp2 = G32(bp1);
        }
        if (bp2) install_mouse_as_pad(bp2);
    }
    {
        static int trace = -1;
        if (trace < 0) trace = getenv("ARKHAM_TRACE_MOUSE_SKIP") != NULL;
        u32 at = mouse_skip_counter(c);
        if (trace && at) {
            static u32 last = 0xFFFFFFFFu;
            u32 v = G32(at);
            if (v != last) {
                unsigned rt_frames_presented(void);
                fprintf(stderr, "[mouse] skip counter %u at frame %u\n", v, rt_frames_presented());
                last = v;
            }
        }
    }
    CGPoint p = CGPointMake(scripted_x, scripted_y);
    int captured = 0;
    double touch_dx = 0, touch_dy = 0;
    if (!scripted_mouse) {
        pthread_mutex_lock(&cap_mu);
#if TARGET_OS_IPHONE
        // No system timer ends the capture on the phone (cap_setup), so it ends here once the game stops warping, as on the Mac after 300 ms.
        if (cap_active && now_ms() - cap_last_warp > 300) cap_active = 0;
#endif
        if (cap_active) {
#if TARGET_OS_IPHONE
            // The touch camera's motion goes to the engine below, from the input loop's reading only, and the pointer stays where the game put it.
            if (bp2) {
                touch_dx = cap_dx;
                touch_dy = cap_dy;
                cap_dx = cap_dy = 0;
                cap_taken_ms = now_ms();
            }
#else
            cap_x += cap_dx;
            cap_y += cap_dy;
            cap_dx = cap_dy = 0;
#endif
            p = CGPointMake(cap_x, cap_y);
            captured = 1;
        }
#if TARGET_OS_IPHONE
        else if (touch_cursor) {
            p = CGPointMake(touch_x, touch_y);
            captured = 1;
        }
#endif
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
    (void)touch_dx, (void)touch_dy;
#else
    (void)captured;
    if (bp2 && (touch_dx != 0 || touch_dy != 0)) touch_mouse_to_engine(c, bp2, touch_dx, touch_dy);
#endif
    if (out) {
        *(f32a *)GP(out) = (float)p.x;
        *(f32a *)GP(out + 4) = (float)p.y;
    }
    trace_mouse(c, p.x, p.y);
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
