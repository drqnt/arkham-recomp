// Gamepads. Feral's code finds them through IOHIDManager and reads each control by usage, with a profile for each model in the game's Resources/InputDevices.
// iOS has no IOHIDManager, and macOS would need a profile for every model, so controllers come from Apple's GameController framework and are shown to the game as
// one of two models it has profiles for: PlayStation controllers as the DualShock 4 (AnalogTriggers/PS4Dualshock.plist), and Xbox and every other controller as
// the wired Xbox 360 controller (360Driver/XBox360Wired.plist), which every Xbox controller since works like and which the game shows Xbox buttons for.
// ARKHAM_FAKE_PAD="seconds:page:usage:value,..." shows one with no hardware, for scripted runs, and ARKHAM_FAKE_PAD_TYPE=xbox or ps picks which.
#import <CoreFoundation/CoreFoundation.h>
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include "shim.h"
#include "shim_reg.h"
#include <mach/mach_time.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u32 cf_wrap(CFTypeRef o);
CFTypeRef cf_unwrap(u32 g);

// What a control reads from the GameController framework.
enum { LX, LY, RX, RY, LT, RT, HAT, B_SOUTH, B_EAST, B_WEST, B_NORTH, LB, RB, START, BACK, L3, R3, HOME, D_UP, D_DOWN, D_LEFT, D_RIGHT };
typedef struct {
    u8 page, usage, what;
    int rest, last, fake;
    u32 guest;
} Control;
typedef struct {
    const char *name;
    int vendor, product;
    const char *product_name, *maker;
    int signed_sticks;  // -32768..32767 with up negative, rather than 0..255
    Control *controls;
    int n;
    CFMutableDictionaryRef device;
    CFSetRef set;
    u32 guest;
} Profile;

static Control ps_controls[] = {
    {1, 48, LX, 127}, {1, 49, LY, 127}, {1, 50, RX, 127}, {1, 53, RY, 127}, {1, 51, LT, 0},  {1, 52, RT, 0},  {1, 57, HAT, 8},
    {9, 1, B_WEST},   {9, 2, B_SOUTH},  {9, 3, B_EAST},   {9, 4, B_NORTH},  {9, 5, LB},      {9, 6, RB},      {9, 10, START},
    {9, 11, L3},      {9, 12, R3},      {9, 14, BACK},
};
static Control xbox_controls[] = {
    {1, 48, LX},     {1, 49, LY},     {1, 51, RX},     {1, 52, RY},     {1, 50, LT},     {1, 53, RT},     {9, 1, B_SOUTH},
    {9, 2, B_EAST},  {9, 3, B_WEST},  {9, 4, B_NORTH}, {9, 5, LB},      {9, 6, RB},      {9, 7, L3},      {9, 8, R3},
    {9, 9, START},   {9, 10, BACK},   {9, 11, HOME},   {9, 12, D_UP},   {9, 13, D_DOWN}, {9, 14, D_LEFT}, {9, 15, D_RIGHT},
};
static Profile profiles[] = {
    {"PlayStation", 1356, 1476, "Wireless Controller", "Sony Computer Entertainment", 0, ps_controls, sizeof ps_controls / sizeof ps_controls[0]},
    {"Xbox", 1118, 654, "Controller", "Microsoft", 1, xbox_controls, sizeof xbox_controls / sizeof xbox_controls[0]},
};

static u32 g_manager, g_value;
static u32 input_cb, input_ctx, match_cb, match_ctx, removal_cb, removal_ctx;
static Profile *shown;  // the model the game has been told about
static int have_timer;
static Control *current;
static int current_value;
static GCController *pad;
static int fake_pad = -1;
static double started;

static void set_number(CFMutableDictionaryRef d, const char *key, int v) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFDictionarySetValue(d, k, n);
    CFRelease(n);
    CFRelease(k);
}
static void set_string(CFMutableDictionaryRef d, const char *key, const char *value) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8), s = CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, k, s);
    CFRelease(s);
    CFRelease(k);
}
// Objects the game gets to hold are real CF objects, so that its CFRetain and CFRelease work on them. Data objects, because small numbers aren't real objects.
static u32 opaque(void) { return cf_wrap(CFDataCreateMutable(NULL, 0)); }
static void make_objects(void) {
    if (g_value) return;
    g_value = opaque();
    for (size_t p = 0; p < sizeof profiles / sizeof profiles[0]; p++) {
        Profile *f = &profiles[p];
        f->device = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        set_number(f->device, "VendorID", f->vendor);
        set_number(f->device, "ProductID", f->product);
        set_number(f->device, "PrimaryUsagePage", 1);
        set_number(f->device, "PrimaryUsage", 5);
        set_number(f->device, "LocationID", 0x14100000 + (int)p);
        set_number(f->device, "VersionNumber", 256);
        set_string(f->device, "Product", f->product_name);
        set_string(f->device, "Manufacturer", f->maker);
        set_string(f->device, "Transport", "USB");
        f->guest = cf_wrap(f->device);
        const void *one = f->device;
        f->set = CFSetCreate(NULL, &one, 1, &kCFTypeSetCallBacks);
        for (int i = 0; i < f->n; i++) {
            Control *k = &f->controls[i];
            k->last = k->fake = k->rest;
            k->guest = opaque();
        }
    }
}
static Control *control_of(u32 guest) {
    for (size_t p = 0; p < sizeof profiles / sizeof profiles[0]; p++)
        for (int i = 0; i < profiles[p].n; i++)
            if (profiles[p].controls[i].guest == guest) return &profiles[p].controls[i];
    return NULL;
}
static Profile *profile_of_device(u32 guest) {
    for (size_t p = 0; p < sizeof profiles / sizeof profiles[0]; p++)
        if (profiles[p].guest == guest) return &profiles[p];
    return NULL;
}
static Profile *profile_for(GCController *k) {
    if (@available(macOS 11.3, iOS 14.5, *))
        if ([k.extendedGamepad isKindOfClass:GCDualSenseGamepad.class]) return &profiles[0];
    if (@available(macOS 11.0, iOS 14.0, *))
        if ([k.extendedGamepad isKindOfClass:GCDualShockGamepad.class]) return &profiles[0];
    return &profiles[1];
}

// A callback into the game on this thread, the same way a system call into a game method goes.
static void call_game(u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    int held = rt_gate_held();
    if (!held) rt_gate_acquire();
    CPU *c = rt_current_cpu();
    if (!c) c = rt_new_cpu(2u << 20);
    u32 args[4] = {a0, a1, a2, a3};
    rt_call_guest(c, fn, 4, args);
    if (!held) rt_gate_release();
}

static void announce_service(void);
static void rumble_tick(void);
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int stick(const Profile *f, float v) {
    return f->signed_sticks ? clamp((int)(v * 32767.0f), -32768, 32767) : clamp((int)((v + 1.0f) * 127.5f + 0.5f), 0, 255);
}
static int hat(GCControllerDirectionPad *d) {
    int up = d.up.pressed, down = d.down.pressed, left = d.left.pressed, right = d.right.pressed;
    if (up && right) return 1;
    if (down && right) return 3;
    if (down && left) return 5;
    if (up && left) return 7;
    if (up) return 0;
    if (right) return 2;
    if (down) return 4;
    if (left) return 6;
    return 8;
}
static int read_control(const Profile *f, GCExtendedGamepad *g, const Control *k) {
    switch (k->what) {
        case LX: return stick(f, g.leftThumbstick.xAxis.value);
        case LY: return stick(f, -g.leftThumbstick.yAxis.value);
        case RX: return stick(f, g.rightThumbstick.xAxis.value);
        case RY: return stick(f, -g.rightThumbstick.yAxis.value);
        case LT: return clamp((int)(g.leftTrigger.value * 255.0f + 0.5f), 0, 255);
        case RT: return clamp((int)(g.rightTrigger.value * 255.0f + 0.5f), 0, 255);
        case HAT: return hat(g.dpad);
        case B_SOUTH: return g.buttonA.pressed;
        case B_EAST: return g.buttonB.pressed;
        case B_WEST: return g.buttonX.pressed;
        case B_NORTH: return g.buttonY.pressed;
        case LB: return g.leftShoulder.pressed;
        case RB: return g.rightShoulder.pressed;
        case START: return g.buttonMenu.pressed;
        case BACK: return g.buttonOptions.pressed;
        case L3: return g.leftThumbstickButton.pressed;
        case R3: return g.rightThumbstickButton.pressed;
        case HOME: return g.buttonHome.pressed;
        case D_UP: return g.dpad.up.pressed;
        case D_DOWN: return g.dpad.down.pressed;
        case D_LEFT: return g.dpad.left.pressed;
        case D_RIGHT: return g.dpad.right.pressed;
    }
    return k->rest;
}
static Profile *fake_profile(void) {
    const char *t = getenv("ARKHAM_FAKE_PAD_TYPE");
    return t && !strcmp(t, "ps") ? &profiles[0] : &profiles[1];
}
// Each of ARKHAM_FAKE_PAD's events is applied once, when its time comes, so that a scripted run's "pad" steps (shims_objc.c, on the script's own
// clock) aren't undone by an earlier event on the next tick.
static void fake_events(Profile *f) {
    static struct { double t; int page, usage, value, done; } *items;
    static int n_items = -1;
    if (n_items < 0) {
        n_items = 0;
        char *copy = strdup(getenv("ARKHAM_FAKE_PAD")), *save = NULL;
        for (char *item = strtok_r(copy, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
            double t;
            int page, usage, value;
            if (sscanf(item, "%lf:%d:%d:%d", &t, &page, &usage, &value) != 4) continue;
            items = realloc(items, sizeof *items * (size_t)(n_items + 1));
            items[n_items].t = t;
            items[n_items].page = page;
            items[n_items].usage = usage;
            items[n_items].value = value;
            items[n_items++].done = 0;
        }
        free(copy);
    }
    double now = CFAbsoluteTimeGetCurrent() - started;
    for (int j = 0; j < n_items; j++) {
        if (items[j].done || items[j].t > now) continue;
        items[j].done = 1;
        for (int i = 0; i < f->n; i++)
            if (f->controls[i].page == items[j].page && f->controls[i].usage == items[j].usage) f->controls[i].fake = items[j].value;
    }
}
// A scripted run's "pad" step: one control of the pad with no hardware set now.
unsigned rt_frames_presented(void);  // mtl_device.m
void rt_fake_pad_set(int page, int usage, int value) {
    Profile *f = fake_profile();
    for (int i = 0; i < f->n; i++)
        if (f->controls[i].page == page && f->controls[i].usage == usage) f->controls[i].fake = value;
    fprintf(stderr, "[pad] scripted %d:%d = %d at frame %u\n", page, usage, value, rt_frames_presented());
}
// ---------- the phone's touch controls (ios/app/TouchControls.m) ----------
// When no controller is connected, what the player's thumbs do on the screen is a pad of its own, shown to the game as the Xbox one so that its prompts
// name the buttons drawn. Sticks from -1 to 1 with up positive, triggers from 0 to 1, and a bit for each button, set from the app's main thread.
static struct {
    _Atomic float lx, ly, rx, ry, lt, rt;
    _Atomic u32 buttons;  // 1 << B_SOUTH and the rest, as in the enum above
    _Atomic int on, playstation;
} touch;
void rt_touch_pad(float lx, float ly, float rx, float ry, float lt, float rt, u32 buttons) {
    atomic_store(&touch.lx, lx);
    atomic_store(&touch.ly, ly);
    atomic_store(&touch.rx, rx);
    atomic_store(&touch.ry, ry);
    atomic_store(&touch.lt, lt);
    atomic_store(&touch.rt, rt);
    atomic_store(&touch.buttons, buttons);
}
void rt_touch_pad_on(int on) { atomic_store(&touch.on, on); }
// Which controller the touch controls are shown to the game as, so that its prompts match what they draw: 1 for the PlayStation one, 0 for Xbox.
void rt_touch_pad_playstation(int ps) { atomic_store(&touch.playstation, ps); }
// Whether a real controller is connected, for the app to hide its controls.
int rt_controller_connected(void) {
    for (GCController *k in GCController.controllers)
        if (k.extendedGamepad && !k.isSnapshot) return 1;
    return 0;
}
static void push_control(Control *k, int v);
static int read_touch(const Profile *f, const Control *k) {
    u32 b = atomic_load(&touch.buttons);
    switch (k->what) {
        case LX: return stick(f, atomic_load(&touch.lx));
        case LY: return stick(f, -atomic_load(&touch.ly));
        case RX: return stick(f, atomic_load(&touch.rx));
        case RY: return stick(f, -atomic_load(&touch.ry));
        case LT: return clamp((int)(atomic_load(&touch.lt) * 255.0f + 0.5f), 0, 255);
        case RT: return clamp((int)(atomic_load(&touch.rt) * 255.0f + 0.5f), 0, 255);
        case HAT: {
            int up = b >> D_UP & 1, down = b >> D_DOWN & 1, left = b >> D_LEFT & 1, right = b >> D_RIGHT & 1;
            if (up && right) return 1;
            if (down && right) return 3;
            if (down && left) return 5;
            if (up && left) return 7;
            return up ? 0 : right ? 2 : down ? 4 : left ? 6 : 8;
        }
        default: return (int)(b >> k->what & 1);
    }
}
static void tick(void) {
    if (fake_pad < 0) fake_pad = getenv("ARKHAM_FAKE_PAD") != NULL;
    GCExtendedGamepad *g = nil;
    Profile *want = NULL;
    int touching = 0;
    if (fake_pad) {
        want = fake_profile();
        fake_events(want);
    } else {
        for (GCController *k in GCController.controllers) {
            if (k.extendedGamepad) {
                if (pad != k) fprintf(stderr, "[pad] %s connected, shown to the game as %s\n", k.vendorName.UTF8String ?: "a controller", profile_for(k)->name);
                pad = k;
                g = k.extendedGamepad;
                want = profile_for(k);
                break;
            }
        }
        if (!g && pad) {
            fprintf(stderr, "[pad] disconnected\n");
            pad = nil;
        }
        if (!g && atomic_load(&touch.on)) {
            want = atomic_load(&touch.playstation) ? &profiles[0] : &profiles[1];
            touching = 1;
        }
    }
    if (shown && shown != want) {
        if (removal_cb) call_game(removal_cb, removal_ctx, 0, g_manager, shown->guest);
        shown = NULL;
    }
    if (!want) return;
    if (!shown) {
        shown = want;
        if (match_cb) call_game(match_cb, match_ctx, 0, g_manager, want->guest);
        announce_service();
    }
    rumble_tick();
    if (!input_cb) return;
    int vals[64], any = 0;
    for (int i = 0; i < want->n && i < 64; i++) {
        Control *k = &want->controls[i];
        vals[i] = fake_pad ? k->fake : touching ? read_touch(want, k) : read_control(want, g, k);
        any |= vals[i] != k->last;
    }
    if (!any) return;
    int held = rt_gate_held();
    if (!held) rt_gate_acquire();
    for (int i = 0; i < want->n && i < 64; i++) push_control(&want->controls[i], vals[i]);
    if (!held) rt_gate_release();
}
// One control's new value to the game, with the game's lock held.
static void push_control(Control *k, int v) {
    if (v == k->last) return;
    k->last = v;
    current = k;
    current_value = v;
    call_game(input_cb, input_ctx, 0, g_manager, g_value);
}
static void start_timer(void) {
    if (have_timer) return;
    have_timer = 1;
    started = CFAbsoluteTimeGetCurrent();
    CFRunLoopTimerRef t = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent(), 1.0 / 250.0, 0, 0, ^(CFRunLoopTimerRef unused) {
        (void)unused;
        tick();
    });
    CFRunLoopAddTimer(rt_guest_main_loop(), t, kCFRunLoopCommonModes);
    CFRelease(t);
}

SHIM(IOHIDManagerCreate) {
    make_objects();
    if (!g_manager) g_manager = opaque();
    fprintf(stderr, "[pad] the game looks for controllers\n");
    RET(g_manager);
}
SHIM(IOHIDManagerSetDeviceMatchingMultiple) { (void)c; }
SHIM(IOHIDManagerOpen) { RET(0); }
SHIM(IOHIDManagerClose) { RET(0); }
SHIM(IOHIDManagerScheduleWithRunLoop) {
    (void)c;
    start_timer();
}
SHIM(IOHIDManagerUnscheduleFromRunLoop) { (void)c; }
SHIM(IOHIDManagerRegisterDeviceMatchingCallback) {
    match_cb = ARG(1);
    match_ctx = ARG(2);
}
SHIM(IOHIDManagerRegisterDeviceRemovalCallback) {
    removal_cb = ARG(1);
    removal_ctx = ARG(2);
}
SHIM(IOHIDManagerRegisterInputValueCallback) {
    input_cb = ARG(1);
    input_ctx = ARG(2);
}
SHIM(IOHIDManagerCopyDevices) {
    make_objects();
    if (fake_pad < 0) fake_pad = getenv("ARKHAM_FAKE_PAD") != NULL;
    Profile *f = fake_pad ? fake_profile() : NULL;
    for (GCController *k in GCController.controllers)
        if (!f && k.extendedGamepad) f = profile_for(k);
    if (!f) {
        RET(0);
        return;
    }
    shown = f;
    CFRetain(f->set);
    RET(cf_wrap(f->set));
}
SHIM(IOHIDDeviceGetProperty) {
    Profile *f = profile_of_device(ARG(0));
    CFTypeRef v = f ? CFDictionaryGetValue(f->device, cf_unwrap(ARG(1))) : NULL;
    RET(v ? cf_wrap(v) : 0);
}
SHIM(IOHIDValueGetElement) { RET(current ? current->guest : 0); }
SHIM(IOHIDValueGetIntegerValue) { RET(current_value); }
SHIM(IOHIDValueGetTimeStamp) {
    u64 t = mach_absolute_time();
    c->eax = (u32)t;
    c->edx = (u32)(t >> 32);
}
SHIM(IOHIDElementGetDevice) { RET(shown ? shown->guest : 0); }
SHIM(IOHIDElementGetUsage) {
    Control *k = control_of(ARG(0));
    RET(k ? k->usage : 0);
}
SHIM(IOHIDElementGetUsagePage) {
    Control *k = control_of(ARG(0));
    RET(k ? k->page : 0);
}
SHIM(IOHIDElementGetCookie) {
    Control *k = control_of(ARG(0));
    RET(k ? ((u32)k->page << 16 | k->usage) : 0);
}
SHIM(CFSetGetCount) { RET(CFSetGetCount(cf_unwrap(ARG(0)))); }
SHIM(CFSetGetValues) {
    CFSetRef s = cf_unwrap(ARG(0));
    CFIndex n = CFSetGetCount(s);
    const void **v = calloc((size_t)n + 1, sizeof *v);
    CFSetGetValues(s, v);
    for (CFIndex i = 0; i < n; i++) G32(ARG(1) + (u32)i * 4) = cf_wrap(v[i]);
    free(v);
}

// ---------- rumble ----------
// Feral's code finds a controller's force feedback through IOKit: a matching notification for IOHIDDevice services, their properties to pair one with the HID
// device, then the ForceFeedback API. Each controller shown to the game gets one service here, and the effects the game plays are added up into one intensity,
// which drives the controller's haptics through CoreHaptics. ARKHAM_TRACE_RUMBLE=1 logs the effects.
enum { SERVICE_BASE = 0x5A5A0010u, ITERATOR_BASE = 0x5A5A0100u, FF_DEVICE = 0x5A5A0200u };
typedef struct {
    int used, playing, periodic;
    u32 magnitude, gain, duration_us;
    double started;
} Effect;
static Effect effects[16];
static u32 device_gain = 10000;
static u32 hid_cb, hid_ctx, hid_iter;  // the game's notification for arriving IOHIDDevice services
static struct {
    u32 left[4];
    int n, at;
} iterators[8];
static int trace_rumble = -1;
static int motor_big, motor_small;  // set by the Xbox 360 driver's escape command, below
static CHHapticEngine *engine API_AVAILABLE(macos(11.0), ios(14.0));
static id<CHHapticAdvancedPatternPlayer> player API_AVAILABLE(macos(11.0), ios(14.0));
static GCController *engine_pad;
static float sent = -1;

static u32 service_of(const Profile *f) { return SERVICE_BASE + (u32)(f - profiles); }
static Profile *profile_of_service(u32 s) {
    u32 i = s - SERVICE_BASE;
    return i < sizeof profiles / sizeof profiles[0] ? &profiles[i] : NULL;
}
static u32 new_iterator(u32 service) {
    for (u32 i = 0; i < 8; i++) {
        if (iterators[i].n == 0 || iterators[i].at >= iterators[i].n) {
            iterators[i].n = service ? 1 : 0;
            iterators[i].at = 0;
            iterators[i].left[0] = service;
            if (!service) iterators[i].n = -1;  // used, but empty
            return ITERATOR_BASE + i;
        }
    }
    return 0;
}
static float rumble_now(void) {
    double now = CFAbsoluteTimeGetCurrent();
    float total = 0;
    for (int i = 0; i < 16; i++) {
        Effect *e = &effects[i];
        if (!e->used || !e->playing) continue;
        if (e->duration_us != 0xFFFFFFFFu && now - e->started > e->duration_us / 1e6) {
            e->playing = 0;
            continue;
        }
        total += (float)e->magnitude / 10000.0f * (float)e->gain / 10000.0f;
    }
    total *= (float)device_gain / 10000.0f;
    float motors = (float)(motor_big > motor_small ? motor_big : motor_small) / 255.0f;
    if (motors > total) total = motors;
    return total > 1 ? 1 : total;
}
// The engine is made at the first rumble, not when the controller connects: one made then, for the game's first "nothing", failed with "Couldn't communicate with a helper
// application" and nothing tried again for the whole game. A failure is now tried again every 2 seconds, and an engine the system stops or resets is made again.
static double engine_retry_at;
static _Atomic int engine_lost;
static void haptics_drop(void) API_AVAILABLE(macos(11.0), ios(14.0)) {
    [player cancelAndReturnError:nil];
    player = nil;
    [engine stopWithCompletionHandler:nil];
    engine = nil;
    engine_pad = nil;
}
static void haptics_failed(const char *step, NSError *err) API_AVAILABLE(macos(11.0), ios(14.0)) {
    static int told;
    if (told++ < 5)
        fprintf(stderr, "[pad] this controller's haptics didn't %s: %s (%s %ld), trying again in 2 s\n", step, err ? err.localizedDescription.UTF8String : "no reason given", err ? err.domain.UTF8String : "", (long)err.code);
    haptics_drop();
    engine_retry_at = CFAbsoluteTimeGetCurrent() + 2;
}
static int haptics_ready(void) API_AVAILABLE(macos(11.0), ios(14.0)) {
    if (atomic_exchange(&engine_lost, 0)) haptics_drop();
    if (engine_pad == pad && player) return 1;
    if (CFAbsoluteTimeGetCurrent() < engine_retry_at) return 0;
    haptics_drop();
    engine = [pad.haptics createEngineWithLocality:GCHapticsLocalityDefault];
    if (!engine) {
        haptics_failed("make an engine", nil);
        return 0;
    }
    engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
        if (trace_rumble) fprintf(stderr, "[pad] haptics stopped, reason %ld\n", (long)reason);
        atomic_store(&engine_lost, 1);
    };
    engine.resetHandler = ^{
        atomic_store(&engine_lost, 1);
    };
    NSError *err = nil;
    if (![engine startAndReturnError:&err]) {
        haptics_failed("start", err);
        return 0;
    }
    CHHapticEvent *ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                      parameters:@[ [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1],
                                                                    [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.3] ]
                                                    relativeTime:0
                                                        duration:30];
    CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[ ev ] parameters:@[] error:&err];
    player = pattern ? [engine createAdvancedPlayerWithPattern:pattern error:&err] : nil;
    if (!player) {
        haptics_failed("make a player", err);
        return 0;
    }
    player.loopEnabled = YES;
    engine_pad = pad;
    fprintf(stderr, "[pad] rumble goes to the controller's haptics\n");
    return 1;
}
// Returns whether the controller got it, so a rumble that couldn't be played is sent again.
static int haptics_send(float v) {
    if (trace_rumble < 0) trace_rumble = getenv("ARKHAM_TRACE_RUMBLE") != NULL;
    if (trace_rumble) fprintf(stderr, "[pad] rumble %.2f\n", v);
    if (@available(macOS 11.0, iOS 14.0, *)) {
        if (!pad || !pad.haptics) return 1;
        if (v <= 0.01f) {
            if (player && engine_pad == pad) [player stopAtTime:0 error:nil];
            return 1;
        }
        if (!haptics_ready()) return 0;
        NSError *err = nil;
        if (![player startAtTime:0 error:&err] ||
            ![player sendParameters:@[ [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:v relativeTime:0] ]
                             atTime:0
                              error:&err]) {
            haptics_failed("play", err);
            return 0;
        }
    }
    return 1;
}
static void rumble_tick(void) {
    float v = rumble_now();
    if (fabsf(v - sent) < 0.03f && !(v == 0 && sent > 0)) return;
    sent = haptics_send(v) ? v : -1;
}
static void read_effect(Effect *e, u32 p, u32 flags) {
    if (!p) return;
    if (flags & 1) e->duration_us = G32(p + 8);                     // FFEP_DURATION
    if (flags & 4) e->gain = G32(p + 16);                           // FFEP_GAIN
    if ((flags & 0x100) && G32(p + 48)) {                           // FFEP_TYPESPECIFICPARAMS
        u32 tp = G32(p + 48), size = G32(p + 44);
        if (e->periodic) e->magnitude = G32(tp);                    // FFPERIODIC.dwMagnitude
        else if (size >= 4) e->magnitude = (u32)abs((int)G32(tp));  // FFCONSTANTFORCE.lMagnitude, or FFRAMPFORCE.lStart
        if (e->magnitude > 10000) e->magnitude = 10000;
    }
}
static Effect *effect_of(u32 h) {
    u32 i = h - 0x5A5A0300u;
    return i < 16 ? &effects[i] : NULL;
}
static void announce_service(void) {
    if (hid_cb && shown) call_game(hid_cb, hid_ctx, new_iterator(service_of(shown)), 0, 0);
}

SHIM(IONotificationPortCreate) { RET(opaque()); }
SHIM(IONotificationPortGetRunLoopSource) {
    CFRunLoopSourceContext ctx = {0};
    RET(cf_wrap(CFRunLoopSourceCreate(NULL, 0, &ctx)));
}
SHIM(IOServiceAddMatchingNotification) {  // port, type, matching, callback, refcon, out iterator
    CFDictionaryRef m = cf_unwrap(ARG(2));
    CFStringRef cls = m ? CFDictionaryGetValue(m, CFSTR("IOProviderClass")) : NULL;
    int hid = cls && CFEqual(cls, CFSTR("IOHIDDevice"));
    int arrival = !strstr(GS(ARG(1)), "Terminate");
    u32 it = new_iterator(hid && arrival && shown ? service_of(shown) : 0);
    if (hid && arrival) {
        hid_cb = ARG(3);
        hid_ctx = ARG(4);
        hid_iter = it;
    }
    G32(ARG(5)) = it;
    RET(0);
}
SHIM(IOServiceGetMatchingService) {  // master port, matching: the game looks up its controller's service this way
    CFDictionaryRef m = cf_unwrap(ARG(1));
    CFStringRef cls = m ? CFDictionaryGetValue(m, CFSTR("IOProviderClass")) : NULL;
    RET(cls && CFEqual(cls, CFSTR("IOHIDDevice")) && shown ? service_of(shown) : 0);
}
SHIM(IOIteratorNext) {
    u32 i = ARG(0) - ITERATOR_BASE;
    if (i >= 8 || iterators[i].n <= 0 || iterators[i].at >= iterators[i].n) {
        RET(0);
        return;
    }
    RET(iterators[i].left[iterators[i].at++]);
}
SHIM(IOObjectRelease) { RET(0); }
SHIM(IOObjectRetain) { RET(0); }
SHIM(IOObjectConformsTo) { RET(profile_of_service(ARG(0)) && !strcmp(GS(ARG(1)), "IOHIDDevice")); }
SHIM(IORegistryEntryCreateCFProperty) {  // entry, key, allocator, options
    Profile *f = profile_of_service(ARG(0));
    CFTypeRef v = f ? CFDictionaryGetValue(f->device, cf_unwrap(ARG(1))) : NULL;
    RET(v ? cf_wrap(CFRetain(v)) : 0);
}
SHIM(IORegistryEntryCreateCFProperties) {  // entry, out dictionary, allocator, options
    Profile *f = profile_of_service(ARG(0));
    if (!f) {
        G32(ARG(1)) = 0;
        RET(0xE00002C0u);
        return;
    }
    G32(ARG(1)) = cf_wrap(CFDictionaryCreateMutableCopy(NULL, 0, f->device));
    RET(0);
}
SHIM(IORegistryEntryGetName) {
    Profile *f = profile_of_service(ARG(0));
    snprintf((char *)GP(ARG(1)), 128, "%s", f ? f->product_name : "");
    RET(f ? 0 : 0xE00002C0u);
}
SHIM(IORegistryEntryGetParentEntry) {
    G32(ARG(2)) = 0;
    RET(0xE00002C0u);
}
SHIM(FFIsForceFeedback) { RET(profile_of_service(ARG(0)) ? 0 : 0x80040201u); }
SHIM(FFCreateDevice) {
    if (!profile_of_service(ARG(0))) {
        RET(0x80040201u);
        return;
    }
    G32(ARG(1)) = FF_DEVICE;
    fprintf(stderr, "[pad] the game opened force feedback\n");
    RET(0);
}
SHIM(FFReleaseDevice) { RET(0); }
SHIM(FFDeviceGetForceFeedbackCapabilities) {
    u32 p = ARG(1);
    memset(GP(p), 0, 72);
    G32(p + 4) = 0x7F | 0x800;  // constant, ramp, square, sine, triangle, sawtooth up and down, custom
    G32(p + 12) = 2;            // FFCAP_ST_VIBRATION
    G32(p + 16) = 2;
    G8(p + 20) = 0;  // FFJOFS_X
    G8(p + 21) = 4;  // FFJOFS_Y
    G32(p + 52) = 16;
    G32(p + 56) = 16;
    RET(0);
}
SHIM(FFDeviceSetForceFeedbackProperty) {
    if (ARG(1) == 1 && ARG(2)) device_gain = G32(ARG(2));  // FFPROP_FFGAIN
    RET(0);
}
// The Xbox 360 driver's own commands, which the game uses instead of effects. FFEFFESCAPE is {size, command, in buffer, in size, out buffer, out size}.
// Command 0 with one byte turns manual control on or off, and command 1 with two bytes sets the two motors, big then small, while it's on.
static int manual_motors;
SHIM(FFDeviceEscape) {
    u32 e = ARG(1), cmd = G32(e + 4), in = G32(e + 8), size = G32(e + 12);
    if (cmd == 0 && size == 1 && in) manual_motors = G8(in) != 0;
    if (cmd == 1 && size == 2 && in && manual_motors) {
        int big = G8(in), small = G8(in + 1);
        if (trace_rumble < 0) trace_rumble = getenv("ARKHAM_TRACE_RUMBLE") != NULL;
        if (trace_rumble && (big != motor_big || small != motor_small)) fprintf(stderr, "[pad] motors %d %d\n", big, small);
        motor_big = big;
        motor_small = small;
    }
    RET(0);
}
SHIM(FFDeviceCreateEffect) {  // device, effect type uuid, FFEFFECT, out effect
    int i = 0;
    while (i < 16 && effects[i].used) i++;
    if (i == 16) {
        RET(0x80040209u);
        return;
    }
    Effect *e = &effects[i];
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->gain = 10000;
    e->duration_us = 0xFFFFFFFFu;
    CFUUIDRef type = cf_unwrap(ARG(1));
    CFUUIDBytes b = type ? CFUUIDGetUUIDBytes(type) : (CFUUIDBytes){0};
    e->periodic = b.byte3 >= 0x62 && b.byte3 <= 0x66;  // square, sine, triangle, sawtooth up and down: E559C462 to E559C466
    read_effect(e, ARG(2), 0x105);
    if (trace_rumble < 0) trace_rumble = getenv("ARKHAM_TRACE_RUMBLE") != NULL;
    if (trace_rumble) fprintf(stderr, "[pad] effect %d of type %02X, magnitude %u, duration %u us\n", i, b.byte3, e->magnitude, e->duration_us);
    G32(ARG(3)) = 0x5A5A0300u + (u32)i;
    RET(0);
}
SHIM(FFDeviceReleaseEffect) {
    Effect *e = effect_of(ARG(1));
    if (e) e->used = e->playing = 0;
    RET(0);
}
SHIM(FFEffectDownload) { RET(0); }
SHIM(FFEffectSetParameters) {  // effect, FFEFFECT, flags
    Effect *e = effect_of(ARG(0));
    if (e) {
        read_effect(e, ARG(1), ARG(2));
        if (ARG(2) & 0x20000000u) {  // FFEP_START
            e->playing = 1;
            e->started = CFAbsoluteTimeGetCurrent();
        }
    }
    RET(0);
}
SHIM(FFEffectStart) {
    Effect *e = effect_of(ARG(0));
    if (e) {
        e->playing = 1;
        e->started = CFAbsoluteTimeGetCurrent();
    }
    RET(0);
}
SHIM(FFEffectStop) {
    Effect *e = effect_of(ARG(0));
    if (e) e->playing = 0;
    RET(0);
}
SHIM(FFEffectGetEffectStatus) {
    Effect *e = effect_of(ARG(0));
    rumble_now();
    G32(ARG(1)) = e && e->playing ? 1 : 0;
    RET(0);
}

// IOKit services: logged, to see what the game looks for beyond the HID manager (Feral's Xbox 360 support goes through a driver's service, rumble included).
static void note_service(const char *how, const char *name) {
    static char seen[8][64];
    for (int i = 0; i < 8; i++) {
        if (!strcmp(seen[i], name)) return;
        if (!seen[i][0]) {
            snprintf(seen[i], sizeof seen[i], "%s", name);
            fprintf(stderr, "[pad] the game looks for the service %s%s\n", how, name);
            return;
        }
    }
}
SHIM(IOServiceMatching) {
    note_service("", GS(ARG(0)));
    // A real matching dictionary, which the game may add to, and which the matching notification above reads its class from.
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef cls = CFStringCreateWithCString(NULL, GS(ARG(0)), kCFStringEncodingUTF8);
    CFDictionarySetValue(d, CFSTR("IOProviderClass"), cls);
    CFRelease(cls);
    RET(cf_wrap(d));
}
SHIM(IOServiceNameMatching) {
    note_service("named ", GS(ARG(0)));
    RET(0);
}
const ShimReg rt_shims_hid[] = {
    R(IOHIDManagerCreate) R(IOHIDManagerSetDeviceMatchingMultiple) R(IOHIDManagerOpen) R(IOHIDManagerClose) R(IOHIDManagerScheduleWithRunLoop) R(IOHIDManagerUnscheduleFromRunLoop)
    R(IOHIDManagerRegisterDeviceMatchingCallback) R(IOHIDManagerRegisterDeviceRemovalCallback) R(IOHIDManagerRegisterInputValueCallback) R(IOHIDManagerCopyDevices)
    R(IOHIDDeviceGetProperty) R(IOHIDValueGetElement) R(IOHIDValueGetIntegerValue) R(IOHIDValueGetTimeStamp)
    R(IOHIDElementGetDevice) R(IOHIDElementGetUsage) R(IOHIDElementGetUsagePage) R(IOHIDElementGetCookie) R(CFSetGetCount) R(CFSetGetValues)
    R(IOServiceMatching) R(IOServiceNameMatching) R(IONotificationPortCreate) R(IONotificationPortGetRunLoopSource) R(IOServiceAddMatchingNotification)
    R(IOServiceGetMatchingService) R(IOIteratorNext) R(IOObjectRelease) R(IOObjectRetain) R(IOObjectConformsTo) R(IORegistryEntryCreateCFProperty) R(IORegistryEntryCreateCFProperties)
    R(IORegistryEntryGetName) R(IORegistryEntryGetParentEntry) R(FFIsForceFeedback) R(FFCreateDevice) R(FFReleaseDevice) R(FFDeviceGetForceFeedbackCapabilities)
    R(FFDeviceSetForceFeedbackProperty) R(FFDeviceEscape) R(FFDeviceCreateEffect) R(FFDeviceReleaseEffect) R(FFEffectDownload) R(FFEffectSetParameters)
    R(FFEffectStart) R(FFEffectStop) R(FFEffectGetEffectStatus)
    {NULL, NULL}};
