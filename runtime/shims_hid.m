// Gamepads. Feral's code finds them through IOHIDManager and reads each control by usage, with a profile for each model in the game's Resources/InputDevices.
// iOS has no IOHIDManager, and macOS needs a profile for every model, so any controller the system's GameController framework knows is shown to the game as the DualShock 4
// it already has a profile for, with the layout from PS4Dualshock.plist. ARKHAM_FAKE_PAD="seconds:page:usage:value,..." shows one with no hardware, for scripted runs.
#import <CoreFoundation/CoreFoundation.h>
#import <GameController/GameController.h>
#include "shim.h"
#include "shim_reg.h"
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u32 cf_wrap(CFTypeRef o);
CFTypeRef cf_unwrap(u32 g);

typedef struct {
    u8 page, usage;
    int rest, last, fake;
    u32 guest;
} Control;
enum { HAT_REST = 8, STICK_REST = 127 };
static Control controls[] = {
    {1, 48, STICK_REST}, {1, 49, STICK_REST}, {1, 50, STICK_REST}, {1, 53, STICK_REST},  // left stick x, y, right stick x, y
    {1, 51, 0},          {1, 52, 0},          {1, 57, HAT_REST},                         // left and right triggers, d-pad
    {9, 1, 0},           {9, 2, 0},           {9, 3, 0},           {9, 4, 0},            // square, cross, circle, triangle
    {9, 5, 0},           {9, 6, 0},           {9, 10, 0},                                // shoulders, start
    {9, 11, 0},          {9, 12, 0},          {9, 14, 0},                                // stick buttons, back
};
#define N_CONTROLS (sizeof controls / sizeof controls[0])

static CFMutableDictionaryRef device;
static CFSetRef device_set;
static u32 g_manager, g_device, g_value;
static u32 input_cb, input_ctx, match_cb, match_ctx, removal_cb, removal_ctx;
static int shown;       // the game has been told about the pad
static int have_timer;
static Control *current;
static int current_value;
static GCController *pad;
static int fake_pad = -1;
static double started;

static void set_number(const char *key, int v) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFDictionarySetValue(device, k, n);
    CFRelease(n);
    CFRelease(k);
}
static void set_string(const char *key, const char *value) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8), s = CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8);
    CFDictionarySetValue(device, k, s);
    CFRelease(s);
    CFRelease(k);
}
// Objects the game gets to hold are real CF objects, so that its CFRetain and CFRelease work on them. Data objects, because small numbers aren't real objects.
static u32 opaque(void) { return cf_wrap(CFDataCreateMutable(NULL, 0)); }
static void make_objects(void) {
    if (device) return;
    device = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    set_number("VendorID", 1356);
    set_number("ProductID", 1476);
    set_number("PrimaryUsagePage", 1);
    set_number("PrimaryUsage", 5);
    set_number("LocationID", 0x14100000);
    set_number("VersionNumber", 256);
    set_string("Product", "Wireless Controller");
    set_string("Manufacturer", "Sony Computer Entertainment");
    set_string("Transport", "USB");
    g_device = cf_wrap(device);
    const void *one = device;
    device_set = CFSetCreate(NULL, &one, 1, &kCFTypeSetCallBacks);
    g_value = opaque();
    for (size_t i = 0; i < N_CONTROLS; i++) {
        controls[i].last = controls[i].rest;
        controls[i].fake = controls[i].rest;
        controls[i].guest = opaque();
    }
}
static Control *control_of(u32 guest) {
    for (size_t i = 0; i < N_CONTROLS; i++)
        if (controls[i].guest == guest) return &controls[i];
    return NULL;
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

static int stick(float v) {
    int x = (int)((v + 1.0f) * 127.5f + 0.5f);
    return x < 0 ? 0 : x > 255 ? 255 : x;
}
static int trigger(float v) {
    int x = (int)(v * 255.0f + 0.5f);
    return x < 0 ? 0 : x > 255 ? 255 : x;
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
    return HAT_REST;
}
static int read_control(GCExtendedGamepad *g, const Control *k) {
    if (k->page == 1) {
        switch (k->usage) {
            case 48: return stick(g.leftThumbstick.xAxis.value);
            case 49: return stick(-g.leftThumbstick.yAxis.value);
            case 50: return stick(g.rightThumbstick.xAxis.value);
            case 53: return stick(-g.rightThumbstick.yAxis.value);
            case 51: return trigger(g.leftTrigger.value);
            case 52: return trigger(g.rightTrigger.value);
            case 57: return hat(g.dpad);
        }
        return k->rest;
    }
    switch (k->usage) {
        case 1: return g.buttonX.pressed;
        case 2: return g.buttonA.pressed;
        case 3: return g.buttonB.pressed;
        case 4: return g.buttonY.pressed;
        case 5: return g.leftShoulder.pressed;
        case 6: return g.rightShoulder.pressed;
        case 10: return g.buttonMenu.pressed;
        case 11: return g.leftThumbstickButton.pressed;
        case 12: return g.rightThumbstickButton.pressed;
        case 14: return g.buttonOptions.pressed;
    }
    return k->rest;
}
static void fake_events(void) {
    const char *spec = getenv("ARKHAM_FAKE_PAD");
    if (!spec) return;
    double now = CFAbsoluteTimeGetCurrent() - started;
    char *copy = strdup(spec), *save = NULL;
    for (char *item = strtok_r(copy, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        double t;
        int page, usage, value;
        if (sscanf(item, "%lf:%d:%d:%d", &t, &page, &usage, &value) != 4 || t > now) continue;
        for (size_t i = 0; i < N_CONTROLS; i++)
            if (controls[i].page == page && controls[i].usage == usage) controls[i].fake = value;
    }
    free(copy);
}
static void tick(void) {
    if (fake_pad < 0) fake_pad = getenv("ARKHAM_FAKE_PAD") != NULL;
    GCExtendedGamepad *g = nil;
    if (!fake_pad) {
        for (GCController *k in GCController.controllers) {
            if (k.extendedGamepad) {
                if (pad != k) fprintf(stderr, "[pad] %s connected\n", k.vendorName.UTF8String ?: "a controller");
                pad = k;
                g = k.extendedGamepad;
                break;
            }
        }
        if (!g && pad) {
            fprintf(stderr, "[pad] disconnected\n");
            pad = nil;
            if (shown && removal_cb) call_game(removal_cb, removal_ctx, 0, g_manager, g_device);
            shown = 0;
        }
        if (!g) return;
    } else {
        fake_events();
    }
    if (!shown) {
        shown = 1;
        if (match_cb) call_game(match_cb, match_ctx, 0, g_manager, g_device);
    }
    if (!input_cb) return;
    for (size_t i = 0; i < N_CONTROLS; i++) {
        Control *k = &controls[i];
        int v = fake_pad ? k->fake : read_control(g, k);
        if (v == k->last) continue;
        k->last = v;
        current = k;
        current_value = v;
        call_game(input_cb, input_ctx, 0, g_manager, g_value);
    }
}
static void start_timer(void) {
    if (have_timer) return;
    have_timer = 1;
    started = CFAbsoluteTimeGetCurrent();
    CFRunLoopTimerRef t = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent(), 1.0 / 250.0, 0, 0, ^(CFRunLoopTimerRef unused) {
        (void)unused;
        tick();
    });
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
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
    int present = fake_pad;
    for (GCController *k in GCController.controllers) present |= k.extendedGamepad != nil;
    if (!present) {
        RET(0);
        return;
    }
    shown = 1;
    CFRetain(device_set);
    RET(cf_wrap(device_set));
}
SHIM(IOHIDDeviceGetProperty) {
    CFTypeRef v = CFDictionaryGetValue(device, cf_unwrap(ARG(1)));
    static int noted[32];
    CFStringRef key = cf_unwrap(ARG(1));
    char name[64] = "";
    if (key) CFStringGetCString(key, name, sizeof name, kCFStringEncodingUTF8);
    unsigned h = 0;
    for (const char *p = name; *p; p++) h = h * 31 + (unsigned char)*p;
    if (!noted[h % 32]) {
        noted[h % 32] = 1;
        fprintf(stderr, "[pad] the game asks for %s%s\n", name, v ? "" : ", which isn't there");
    }
    RET(v ? cf_wrap(v) : 0);
}
SHIM(IOHIDValueGetElement) { RET(current ? current->guest : 0); }
SHIM(IOHIDValueGetIntegerValue) { RET(current_value); }
SHIM(IOHIDValueGetTimeStamp) {
    u64 t = mach_absolute_time();
    c->eax = (u32)t;
    c->edx = (u32)(t >> 32);
}
SHIM(IOHIDElementGetDevice) { RET(g_device); }
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

const ShimReg rt_shims_hid[] = {
    R(IOHIDManagerCreate) R(IOHIDManagerSetDeviceMatchingMultiple) R(IOHIDManagerOpen) R(IOHIDManagerClose) R(IOHIDManagerScheduleWithRunLoop) R(IOHIDManagerUnscheduleFromRunLoop)
    R(IOHIDManagerRegisterDeviceMatchingCallback) R(IOHIDManagerRegisterDeviceRemovalCallback) R(IOHIDManagerRegisterInputValueCallback) R(IOHIDManagerCopyDevices)
    R(IOHIDDeviceGetProperty) R(IOHIDValueGetElement) R(IOHIDValueGetIntegerValue) R(IOHIDValueGetTimeStamp)
    R(IOHIDElementGetDevice) R(IOHIDElementGetUsage) R(IOHIDElementGetUsagePage) R(IOHIDElementGetCookie) R(CFSetGetCount) R(CFSetGetValues)
    {NULL, NULL}};
