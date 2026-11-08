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
    int held = (cap_active || scripted_mouse) && now_ms() - cap_last_warp < 300;  // a scripted run's warps count too (CGWarpMouseCursorPosition)
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
// ARKHAM_MOUSE_AS_PAD=1 marks the loop's own two calls as from a pad too: the engine's InputAxis, a virtual function of the viewport client, is called
// through the dispatcher, so a host function in front of it at that address sees each call and changes the last argument (bGamepad) of only those two,
// known by where they return to. Not on the phone: the menus move their cursor by those calls, and marked as a pad's, taps clicked nowhere (v9).
guest_fn rt_guest_fn(u32 addr);           // rt.c: the translated function at a guest address
void rt_ht_add(u32 addr, guest_fn fn);  // rt.c: a host function in front of it for calls through the dispatcher
static guest_fn input_axis_orig;
static u32 input_axis_at;
#include <stdatomic.h>
// The name table numbers of XboxTypeS_RightX, _RightY, _LeftX, _LeftY, StrafeRight, MoveForward, TurnRight and TurnLeft; 0 until found.
static _Atomic u32 stick_idx[8];
static int mouse_as_pad(void) {
    static int on = -1;
    if (on < 0) on = getenv("ARKHAM_MOUSE_AS_PAD") != NULL;
    return on;
}
// Walking with a pad's left stick is what makes the game damp the camera's turning: measured on the Mac with the same route walked with the W key,
// where a turn of the mouse came out whole, whether or not marked as a pad's, and with the pad's stick, where it came out at a fifth (docs/IOS_PORT.md,
// controls v13). So the left stick's axes reach the engine marked as not a pad's (bGamepad 0), by name (XboxTypeS_LeftX and _LeftY, found in the
// engine's name table as the right stick's are), and the walking keeps the stick's own bindings and its analog speed. On the phone unless
// ARKHAM_MOVE_NOT_PAD=0; on the Mac with =1.
// Measured: it didn't help (the turning was damped as before), so it's only on asking, ARKHAM_MOVE_NOT_PAD=1; walking goes past the pad instead
// (move_to_engine, below).
int rt_move_not_pad(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("ARKHAM_MOVE_NOT_PAD");
        on = v && *v == '1';
    }
    return on;
}
static void input_axis_as_pad(CPU *c, u8 *m) {
    (void)m;
    u32 ret = G32(c->esp);
    if (mouse_as_pad() && (ret == 0x19df2eau || ret == 0x19df353u)) G32(c->esp + 0x20) = 1;
    if (rt_move_not_pad()) {
        u32 key = G32(c->esp + 0x10), lx = atomic_load(&stick_idx[2]), ly = atomic_load(&stick_idx[3]);
        if (key && (key == lx || key == ly) && G32(c->esp + 0x20)) {
            G32(c->esp + 0x20) = 0;
            static int told;
            if (!told++) fprintf(stderr, "[pad] the left stick's walking reaches the engine as not a pad's\n");
        }
    }
    c->jt = input_axis_at;
    input_axis_orig(c, RT_M);
}
static void install_mouse_as_pad(u32 bp2) {
    if (input_axis_orig || !(mouse_as_pad() || rt_move_not_pad()) || bp2 < 0x1000) return;
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
#define TOUCH_TURN_VSIGN 1.0  // the vertical's sign on the stick's axis against the mouse's, measured on the Mac
// ---------- the touch camera as the right stick ----------
// The game turns the camera from the mouse while Batman stands, but damps it while he walks, and from the pad's right stick it never does (the user's
// DualSense in the same place; v11 put the swipe on the stick's axes through the mouse's key, and that was damped too). So the swipe goes to the engine
// as the right stick itself: the keys XboxTypeS_RightX and XboxTypeS_RightY, with the stick's own bindings (aTurn and aLookup with a dead zone of 0.2,
// and the raw axes), and each frame the value that turns by exactly the swipe. The engine's Axis command (F_0f84320) maps a value v past the dead zone
// to (v - 0.2) / 0.8, so the value sent is 0.2 + 0.8 x what should arrive. Off with ARKHAM_TOUCH_STICK=0 on the phone; on the Mac with =1.
// The engine knows its keys by their number in its name table. Those two are found once, in the background: each entry of the table holds its number
// a few bytes before its characters, laid out as MouseX's entry shows, whose number F_19deea0's globals give.
#include <mach/mach.h>
#include <stdatomic.h>
// Only on asking (ARKHAM_TOUCH_STICK=1): measured on the Mac, swipes sent this way were damped while walking just as the mouse's were, and standing they
// didn't turn the camera at all, since the game smooths and speeds up a stick's turning, made for a thumb that holds it.
static int stick_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("ARKHAM_TOUCH_STICK");
        on = v && *v == '1';
    }
    return on;
}
// Every place in the guest's memory where text appears as width-byte characters with a terminator: calls found(guest address) until it says stop.
static void scan_guest_text(const char *text, int width, int (^found)(u32 guest)) {
    u8 pat[64];
    size_t n = strlen(text), len = (n + 1) * (size_t)width;
    if (len > sizeof pat) return;
    memset(pat, 0, len);
    for (size_t i = 0; i < n; i++) pat[i * (size_t)width] = (u8)text[i];
    vm_address_t a = (vm_address_t)RT_MEM_BASE;
    while (a < (vm_address_t)RT_MEM_BASE + 0x100000000ull) {
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        if (vm_region_64(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) return;
        if (a >= (vm_address_t)RT_MEM_BASE + 0x100000000ull) return;
        if (info.protection & VM_PROT_READ) {
            const u8 *p = (const u8 *)a, *end = p + size;
            while (p < end) {
                const u8 *hit = memmem(p, (size_t)(end - p), pat, len);
                if (!hit) break;
                u64 g = (u64)(uintptr_t)hit - RT_MEM_BASE;
                if (g >= 16 && found((u32)g)) return;
                p = hit + 1;
            }
        }
        a += size;
    }
}
static void *find_stick_names(void *arg) {
    u32 mouse_idx = (u32)(uintptr_t)arg;
    u64 t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    __block int width = 0, at = 0, shift = 0;
    // The layout, from MouseX: the characters' width, how far before them the number is, and whether it's stored doubled (a flag in its lowest bit).
    for (int w = 1; w <= 4 && !width; w *= 2) {
        scan_guest_text("MouseX", w, ^int(u32 g) {
            for (int off = 4; off <= 16; off += 4)
                for (int sh = 0; sh <= 1; sh++)
                    if (G32(g - (u32)off) >> sh == mouse_idx) {
                        width = w, at = off, shift = sh;
                        return 1;
                    }
            return 0;
        });
    }
    if (!width) {
        fprintf(stderr, "[mouse] the right stick's key names weren't found (MouseX is %u): the touch camera stays the mouse\n", mouse_idx);
        return NULL;
    }
    static const char *const names[8] = {"XboxTypeS_RightX", "XboxTypeS_RightY", "XboxTypeS_LeftX", "XboxTypeS_LeftY",
                                         "StrafeRight",      "MoveForward",      "TurnRight",       "TurnLeft"};
    for (int k = 0; k < 8; k++) {
        __block u32 idx = 0;
        scan_guest_text(names[k], width, ^int(u32 g) {
            u32 v = G32(g - (u32)at) >> shift;
            if (v && v < 1000000) idx = v;
            return idx != 0;
        });
        atomic_store(&stick_idx[k], idx);
    }
    fprintf(stderr, "[mouse] key names: right stick %u %u, left stick %u %u, walking %u %u, raw left stick %u %u (MouseX %u, %d-byte characters, the number %d bytes before them%s), found in %.0f ms\n",
            atomic_load(&stick_idx[0]), atomic_load(&stick_idx[1]), atomic_load(&stick_idx[2]), atomic_load(&stick_idx[3]), atomic_load(&stick_idx[4]),
            atomic_load(&stick_idx[5]), atomic_load(&stick_idx[6]), atomic_load(&stick_idx[7]), mouse_idx, width, at, shift ? ", doubled" : "",
            (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6);
    return NULL;
}
int rt_move_not_pad(void);
int rt_move_direct(void);
static void start_stick_names(u32 bp2) {
    static int started;
    if (started || !(stick_on() || rt_move_not_pad() || rt_move_direct())) return;
    u32 base = G32(bp2 - 0xbc), mouse_name = base == 0x19deeb1u ? G32(base + 0x1aaf027u) : 0;
    if (!mouse_name) return;
    started = 1;
    pthread_t t;
    pthread_create(&t, NULL, find_stick_names, (void *)(uintptr_t)G32(mouse_name));
    pthread_detach(t);
}
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
    double d[2] = {dx, -dy};  // as F_0048c10 gives them: right and up positive, the pointer's y growing downwards
    u32 stick[2] = {atomic_load(&stick_idx[0]), atomic_load(&stick_idx[1])};
    int as_stick = stick_on() && stick[0] && stick[1];
    // With the mouse's axes on the right stick's (rt_touch_turn, shims_libc.c), the value for this frame that turns by what the mouse would have:
    // a pixel was 7.5 rotation units (MouseSensitivity 7.5), and the engine turns the stick's axes by value x 100 x the frame's time x
    // LookRightScale (375) or LookUpScale (-300).
    int rt_touch_turn(void);
    float dt;
    memcpy(&dt, &(u32){G32(bp2 + 0xc)}, 4);
    if (!(dt > 0.0005f && dt < 0.25f)) dt = 1.0f / 60;
    if (rt_touch_turn() || as_stick) {
        d[0] = d[0] * 7.5 / (100.0 * dt * 375.0);
        d[1] = d[1] * 7.5 / (100.0 * dt * -300.0) * TOUCH_TURN_VSIGN;
    }
    if (as_stick)  // past the stick's dead zone, as the engine maps it back (F_0f84320)
        for (int k = 0; k < 2; k++)
            if (d[k] != 0) d[k] = d[k] > 0 ? 0.2 + 0.8 * d[k] : -0.2 + 0.8 * d[k];
    for (int k = 0; k < 2; k++) {
        if (d[k] == 0 || !names[k]) continue;
        float f = (float)d[k];
        u32 bits;
        memcpy(&bits, &f, 4);
        u32 args[8] = {client, window ? window + 4 : 0, 0, as_stick ? stick[k] : G32(names[k]), as_stick ? 0 : G32(names[k] + 4), bits, G32(bp2 + 0xc), 1};
        rt_call_guest(c, fn, 8, args);
    }
    sent_frames++;
    sent_x += fabs(dx);
    sent_y += fabs(dy);
    static int trace = -1;  // ARKHAM_TRACE_TOUCH_MOUSE=n: the first n hand-overs, with the frame they went in
    if (trace < 0) trace = getenv("ARKHAM_TRACE_TOUCH_MOUSE") ? atoi(getenv("ARKHAM_TRACE_TOUCH_MOUSE")) : 0;
    if (trace > 0) {
        trace--;
        unsigned rt_frames_presented(void);
        fprintf(stderr, "[mouse] to the engine at frame %u: %.1f %.1f px as %.4f %.4f, frame time %.4f\n", rt_frames_presented(), dx, -dy, d[0], d[1], dt);
    }
    if (now_ms() - told_ms > 10000 && sent_frames != told_frames) {
        told_ms = now_ms();
        told_frames = sent_frames;
        fprintf(stderr, "[mouse] touch camera: %llu frames with motion, %.0f px across and %.0f px up and down so far, straight to the engine\n", sent_frames, sent_x, sent_y);
    }
}
// ARKHAM_MOUSE_DIRECT=1 on the Mac: a scripted run's "look" steps go to the engine the same way, for measuring the touch camera's path.
static double direct_dx, direct_dy;
static int direct_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("ARKHAM_MOUSE_DIRECT") != NULL;
    return on;
}
int rt_scripted_direct_look(double dx, double dy) {
    if (!direct_on()) return 0;
    pthread_mutex_lock(&cap_mu);
    direct_dx += dx;
    direct_dy += dy;
    pthread_mutex_unlock(&cap_mu);
    return 1;
}
// Walking from the touch stick, straight to the engine as the left stick's axes, while the pad the game sees keeps its stick in the middle: the game
// damps the camera's turning while the pad's own stick is held, whatever the axis events say (marking them as not a pad's changed nothing, measured
// on the Mac), and a keyboard's walking, which goes to the same aBaseY, doesn't. Each frame the stick's position goes in, from the input loop's
// reading of the pointer as the touch camera does, through the stick's bindings (aStrafe and aBaseY with a dead zone of 0.3, and the raw axes), so
// the walking keeps its analog speed. rt_touch_move gives the position, up and right positive; ARKHAM_MOVE_DIRECT=0 on the phone goes back to the
// pad's stick; on the Mac it's on with =1, and a scripted run's "walk:x:y" steps set it.
static _Atomic float move_x, move_y;
static _Atomic int move_owned;  // in play: walking goes this way and the pad's stick stays in the middle; in menus the pad's stick pans and points
int rt_move_direct(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("ARKHAM_MOVE_DIRECT");
#if TARGET_OS_IPHONE
        on = !(v && *v == '0');
#else
        on = v && *v == '1';
#endif
    }
    return on;
}
void rt_touch_move(float x, float y, int in_play) {
    atomic_store(&move_x, in_play ? x : 0);
    atomic_store(&move_y, in_play ? y : 0);
    atomic_store(&move_owned, in_play);
}
// Walking goes this way only while the game holds the cursor, warping it back each frame as it does in play: on the map and in menus it doesn't, and
// there the left stick has to reach the game as the pad's, to pan the map and move through menus. The touch controls can't tell by themselves when
// their menu mode is off ("Прятать вне игры"), and the map, with its hundreds of draws, looks like play to them.
int rt_move_owned(void) { return rt_move_direct() && atomic_load(&move_owned) && rt_game_holds_cursor(); }
// For the pad with no hardware on the Mac, which carries the walking stick on the map and in menus as the phone's touch pad does.
void rt_touch_move_get(float *x, float *y) {
    *x = atomic_load(&move_x);
    *y = atomic_load(&move_y);
}
static void move_to_engine(CPU *c, u32 bp2) {
    static float sent_x, sent_y;
    int rt_move_owned(void);
    // Not as the left stick's own keys (XboxTypeS_LeftX and _LeftY): sent that way, from a pad or not, the game damped the camera while Batman walked.
    // As the keyboard's movement names instead, StrafeRight and MoveForward, which are bound to the same aStrafe and aBaseY with no dead zone; and the
    // stick's raw axes, which the keyboard's W, A, S and D feed too, through TurnRight and TurnLeft, rebound to them on the phone (shims_libc.c), as
    // nothing uses those: the D-pad they were on chooses gadgets. Measured on the Mac with the same route: walking this way, a swipe turned the camera
    // all the way, as walking with the W key did, against a fifth with the stick.
    u32 lx = atomic_load(&stick_idx[4]), ly = atomic_load(&stick_idx[5]), rx = atomic_load(&stick_idx[6]), ry = atomic_load(&stick_idx[7]);
    if (!rt_move_direct() || !lx || !ly) return;
    // On the map or in a menu the pad's stick carries it (shims_hid.m), and this sends one last zero if walking was going on.
    int owned = rt_move_owned();
    float x = owned ? atomic_load(&move_x) : 0, y = owned ? atomic_load(&move_y) : 0;
    if (x == 0 && y == 0 && sent_x == 0 && sent_y == 0) return;  // at rest, and the last frame's zero already sent
    u32 client = G32(bp2 - 0xc0), window = G32(bp2 - 0xc8);
    u32 vtable = client ? G32(client) : 0, fn = vtable ? G32(vtable + 0x10) : 0;
    if (!fn) return;
    const u32 keys[4] = {lx, ly, rx, ry};
    const float v[4] = {x, y, x, y};
    for (int k = 0; k < 4; k++) {
        if (!keys[k]) continue;
        u32 bits;
        memcpy(&bits, &v[k], 4);
        u32 args[8] = {client, window ? window + 4 : 0, 0, keys[k], 0, bits, G32(bp2 + 0xc), 1};
        rt_call_guest(c, fn, 8, args);
    }
    static int told;
    if (!told++) fprintf(stderr, "[pad] walking goes to the engine by the keyboard's movement names, with the pad's own stick left in the middle\n");
    sent_x = x, sent_y = y;
}
SHIM(HIGetMousePosition) {
    u32 out = ARG(2);
    u32 bp2 = 0;  // Feral's input loop's frame, when the pointer is read from there (see mouse_skip_counter)
    {
        u32 bp = c->ebp;
        if (bp > 0x1000 && G32(bp + 4) == 0x48c4bu) {
            u32 bp1 = G32(bp);
            if (bp1 > 0x1000 && G32(bp1 + 4) == 0x19df02fu) bp2 = G32(bp1);
        }
        if (bp2) {
            install_mouse_as_pad(bp2);
            start_stick_names(bp2);
        }
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
    if (bp2 && scripted_mouse && direct_on()) {
        pthread_mutex_lock(&cap_mu);
        touch_dx = direct_dx;
        touch_dy = direct_dy;
        direct_dx = direct_dy = 0;
        pthread_mutex_unlock(&cap_mu);
        if (touch_dx != 0 || touch_dy != 0) touch_mouse_to_engine(c, bp2, touch_dx, touch_dy);
    }
    if (bp2 && scripted_mouse) move_to_engine(c, bp2);
#else
    (void)captured;
    if (bp2 && (touch_dx != 0 || touch_dy != 0)) touch_mouse_to_engine(c, bp2, touch_dx, touch_dy);
    if (bp2 && captured) move_to_engine(c, bp2);
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
        pthread_mutex_lock(&cap_mu);
        cap_last_warp = now_ms();
        pthread_mutex_unlock(&cap_mu);
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
