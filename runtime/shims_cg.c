// CoreGraphics and a few Carbon queries the game makes outside OpenGL: displays, the keyboard layout, images, the event clock and the OS version.
// Built for the Mac and the phone. On the phone the one display is its screen, as the app shell reports it (rt_ios_screen_* in ios_appkit.m).
#include "shim.h"
#include "shim_reg.h"

#if TARGET_OS_IPHONE
#include <CoreGraphics/CoreGraphics.h>
#else
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#endif
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern u32 cf_wrap(CFTypeRef o);
extern CFTypeRef cf_unwrap(u32 g);
#define OBJ(n) cf_unwrap(ARG(n))

static int trace_cg = -1;
static int tracing(void) {
    if (trace_cg < 0) trace_cg = getenv("ARKHAM_TRACE_GL") != NULL;
    return trace_cg;
}

// ---------- displays ----------
#if TARGET_OS_IPHONE
extern double rt_ios_screen_w, rt_ios_screen_h, rt_ios_screen_scale;
#define PHONE_DISPLAY 1u
// The game lists display modes to offer resolutions. The phone's are its screen in pixels and two smaller sizes of the same shape, kept as
// dictionaries since iOS has no display mode objects; the CGDisplayMode calls below read them.
static CFDictionaryRef phone_mode(double scale_of_native, int is_default) {
    double w = rt_ios_screen_w * rt_ios_screen_scale * scale_of_native, h = rt_ios_screen_h * rt_ios_screen_scale * scale_of_native;
    int iw = (int)w & ~7, ih = (int)h & ~7, flags = 0x3 | (is_default ? 0x4 : 0), hz = 120;  // valid, safe, default
    CFStringRef keys[4] = {CFSTR("w"), CFSTR("h"), CFSTR("flags"), CFSTR("hz")};
    CFNumberRef vals[4] = {CFNumberCreate(NULL, kCFNumberIntType, &iw), CFNumberCreate(NULL, kCFNumberIntType, &ih), CFNumberCreate(NULL, kCFNumberIntType, &flags),
                           CFNumberCreate(NULL, kCFNumberIntType, &hz)};
    CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (int i = 0; i < 4; i++) CFRelease(vals[i]);
    return d;
}
static int mode_int(u32 g, CFStringRef key) {
    CFTypeRef m = cf_unwrap(g);
    int v = 0;
    if (m && CFGetTypeID(m) == CFDictionaryGetTypeID()) {
        CFNumberRef n = CFDictionaryGetValue((CFDictionaryRef)m, key);
        if (n) CFNumberGetValue(n, kCFNumberIntType, &v);
    }
    return v;
}
SHIM(CGMainDisplayID) { RET(PHONE_DISPLAY); }
SHIM(CGGetActiveDisplayList) {
    if (ARG(0) && ARG(1)) G32(ARG(1)) = PHONE_DISPLAY;
    if (ARG(2)) G32(ARG(2)) = 1;
    RET(0);
}
SHIM(CGDisplayBounds) {
    u32 b = ARG(0);
    *(f32a *)GP(b) = 0;
    *(f32a *)GP(b + 4) = 0;
    *(f32a *)GP(b + 8) = (float)rt_ios_screen_w;
    *(f32a *)GP(b + 12) = (float)rt_ios_screen_h;
    RET(b);
    c->esp += 4;
}
// In millimetres, roughly a large phone's.
SHIM(CGDisplayScreenSize) {
    float w = 150, h = 70;
    memcpy(&c->eax, &w, 4);
    memcpy(&c->edx, &h, 4);
}
SHIM(CGDisplayIDToOpenGLDisplayMask) { RET(1); }
SHIM(CGDisplayCopyDisplayMode) { RET(cf_wrap(phone_mode(1.0, 1))); }
SHIM(CGDisplayCopyAllDisplayModes) {
    CFDictionaryRef modes[3] = {phone_mode(1.0, 1), phone_mode(0.75, 0), phone_mode(0.5, 0)};
    CFArrayRef a = CFArrayCreate(NULL, (const void **)modes, 3, &kCFTypeArrayCallBacks);
    for (int i = 0; i < 3; i++) CFRelease(modes[i]);
    if (tracing()) fprintf(stderr, "[cg] display modes: %d x %d and two smaller\n", mode_int(cf_wrap(CFArrayGetValueAtIndex(a, 0)), CFSTR("w")), mode_int(cf_wrap(CFArrayGetValueAtIndex(a, 0)), CFSTR("h")));
    RET(cf_wrap(a));
}
SHIM(CGDisplayModeGetWidth) { RET((u32)mode_int(ARG(0), CFSTR("w"))); }
SHIM(CGDisplayModeGetHeight) { RET((u32)mode_int(ARG(0), CFSTR("h"))); }
SHIM(CGDisplayModeGetIOFlags) { RET((u32)mode_int(ARG(0), CFSTR("flags"))); }
SHIM(CGDisplayModeGetRefreshRate) { RETF(mode_int(ARG(0), CFSTR("hz"))); }
SHIM(CGWindowLevelForKey) { RET(0); }
#else
SHIM(CGMainDisplayID) { RET(CGMainDisplayID()); }
SHIM(CGGetActiveDisplayList) {
    uint32_t n = 0;
    CGError e = CGGetActiveDisplayList(ARG(0), NP(ARG(1)), &n);
    if (ARG(2)) G32(ARG(2)) = n;
    RET(e);
}
// CGRect comes back through a hidden pointer that the callee pops.
SHIM(CGDisplayBounds) {
    CGRect r = CGDisplayBounds(ARG(1));
    u32 b = ARG(0);
    *(f32a *)GP(b) = (float)r.origin.x;
    *(f32a *)GP(b + 4) = (float)r.origin.y;
    *(f32a *)GP(b + 8) = (float)r.size.width;
    *(f32a *)GP(b + 12) = (float)r.size.height;
    RET(b);
    c->esp += 4;
}
SHIM(CGDisplayScreenSize) {
    CGSize s = CGDisplayScreenSize(ARG(0));
    float w = (float)s.width, h = (float)s.height;
    memcpy(&c->eax, &w, 4);
    memcpy(&c->edx, &h, 4);
}
SHIM(CGDisplayIDToOpenGLDisplayMask) { RET(CGDisplayIDToOpenGLDisplayMask(ARG(0))); }
SHIM(CGDisplayCopyDisplayMode) { RET(cf_wrap(CGDisplayCopyDisplayMode(ARG(0)))); }
SHIM(CGDisplayCopyAllDisplayModes) { RET(cf_wrap(CGDisplayCopyAllDisplayModes(ARG(0), NULL))); }
SHIM(CGDisplayModeGetWidth) { RET(CGDisplayModeGetWidth((CGDisplayModeRef)cf_unwrap(ARG(0)))); }
SHIM(CGDisplayModeGetHeight) { RET(CGDisplayModeGetHeight((CGDisplayModeRef)cf_unwrap(ARG(0)))); }
SHIM(CGDisplayModeGetIOFlags) { RET(CGDisplayModeGetIOFlags((CGDisplayModeRef)cf_unwrap(ARG(0)))); }
SHIM(CGDisplayModeGetRefreshRate) { RETF(CGDisplayModeGetRefreshRate((CGDisplayModeRef)cf_unwrap(ARG(0)))); }
SHIM(CGWindowLevelForKey) { RET(CGWindowLevelForKey((CGWindowLevelKey)ARG(0))); }
#endif
SHIM(CGDisplayModeCopyPixelEncoding) { RET(cf_wrap(CFStringCreateWithCString(NULL, "--------RRRRRRRRGGGGGGGGBBBBBBBB", kCFStringEncodingUTF8))); }
SHIM(CGDisplayModeRelease) {}
SHIM(cg_ok) { RET(0); }
SHIM(CGGetDisplayTransferByTable) {
    if (ARG(4)) G32(ARG(4)) = 0;
    RET(0);
}

// ---------- keyboard layout ----------
#if TARGET_OS_IPHONE
// No keyboard layouts on the phone: one stand-in input source, whose layout data is zeros, which UCKeyTranslate (shims_sys.c) never reads there.
static CFTypeRef phone_source(void) { return CFStringCreateWithCString(NULL, "com.apple.keylayout.US", kCFStringEncodingUTF8); }
SHIM(TISCopyCurrentKeyboardLayoutInputSource) { RET(cf_wrap(phone_source())); }
SHIM(TISCopyCurrentASCIICapableKeyboardLayoutInputSource) { RET(cf_wrap(phone_source())); }
SHIM(TISCopyCurrentASCIICapableKeyboardInputSource) { RET(cf_wrap(phone_source())); }
SHIM(TISCopyCurrentKeyboardInputSource) { RET(cf_wrap(phone_source())); }
// Answered by the property asked for, named as Carbon names them (shims_cf.c gives the game those names on the phone).
SHIM(TISGetInputSourceProperty) {
    static CFDataRef zeros;
    static CFArrayRef languages;
    if (!zeros) {
        static const u8 z[256];
        zeros = CFDataCreate(NULL, z, sizeof z);
        CFStringRef en = CFSTR("en");
        languages = CFArrayCreate(NULL, (const void **)&en, 1, &kCFTypeArrayCallBacks);
    }
    CFTypeRef key = cf_unwrap(ARG(1)), v = NULL;
    if (key && CFGetTypeID(key) == CFStringGetTypeID()) {
        if (CFEqual(key, CFSTR("TISPropertyUnicodeKeyLayoutData"))) v = zeros;
        else if (CFEqual(key, CFSTR("TISPropertyInputSourceID"))) v = CFSTR("com.apple.keylayout.US");
        else if (CFEqual(key, CFSTR("TISPropertyLocalizedName"))) v = CFSTR("U.S.");
        else if (CFEqual(key, CFSTR("TISPropertyInputSourceLanguages"))) v = languages;
    }
    if (!v) fprintf(stderr, "[cg] no keyboard layout property to answer for %08x\n", ARG(1));
    RET(v ? cf_wrap(v) : 0);
}
SHIM(LMGetKbdType) { RET(40); }  // an ANSI keyboard
#else
// The game names its keys after what the current layout types, so W would be bound as a Cyrillic letter on a Russian layout.
// Answering with the ASCII layout keeps the key names and bindings the same whatever input language is selected.
SHIM(TISCopyCurrentKeyboardLayoutInputSource) { RET(cf_wrap(TISCopyCurrentASCIICapableKeyboardLayoutInputSource())); }
SHIM(TISCopyCurrentASCIICapableKeyboardLayoutInputSource) { RET(cf_wrap(TISCopyCurrentASCIICapableKeyboardLayoutInputSource())); }
SHIM(TISCopyCurrentASCIICapableKeyboardInputSource) { RET(cf_wrap(TISCopyCurrentASCIICapableKeyboardInputSource())); }
SHIM(TISCopyCurrentKeyboardInputSource) { RET(cf_wrap(TISCopyCurrentASCIICapableKeyboardInputSource())); }
SHIM(TISGetInputSourceProperty) { RET(cf_wrap(TISGetInputSourceProperty((TISInputSourceRef)cf_unwrap(ARG(0)), (CFStringRef)cf_unwrap(ARG(1))))); }
SHIM(LMGetKbdType) { RET(LMGetKbdType()); }
#endif

// ---------- images through the real ImageIO and CoreGraphics ----------
SHIM(CGImageSourceCreateWithData) { RET(cf_wrap(CGImageSourceCreateWithData((CFDataRef)OBJ(0), (CFDictionaryRef)OBJ(1)))); }
SHIM(CGImageSourceGetCount) { RET(CGImageSourceGetCount((CGImageSourceRef)OBJ(0))); }
SHIM(CGImageSourceCreateImageAtIndex) { RET(cf_wrap(CGImageSourceCreateImageAtIndex((CGImageSourceRef)OBJ(0), ARG(1), (CFDictionaryRef)OBJ(2)))); }
SHIM(CGImageGetWidth) { RET(CGImageGetWidth((CGImageRef)OBJ(0))); }
SHIM(CGImageGetHeight) { RET(CGImageGetHeight((CGImageRef)OBJ(0))); }
SHIM(CGImageGetBitsPerComponent) { RET(CGImageGetBitsPerComponent((CGImageRef)OBJ(0))); }
SHIM(CGImageGetBitsPerPixel) { RET(CGImageGetBitsPerPixel((CGImageRef)OBJ(0))); }
SHIM(CGImageGetBytesPerRow) { RET(CGImageGetBytesPerRow((CGImageRef)OBJ(0))); }
SHIM(CGImageGetBitmapInfo) { RET(CGImageGetBitmapInfo((CGImageRef)OBJ(0))); }
SHIM(CGImageGetDataProvider) { RET(cf_wrap(CGImageGetDataProvider((CGImageRef)OBJ(0)))); }
SHIM(CGDataProviderCopyData) { RET(cf_wrap(CGDataProviderCopyData((CGDataProviderRef)OBJ(0)))); }
SHIM(CGDataProviderCreateWithCFData) { RET(cf_wrap(CGDataProviderCreateWithCFData((CFDataRef)OBJ(0)))); }
SHIM(cf_release_any) {
    CFTypeRef o = OBJ(0);
    if (o) CFRelease(o);
}
SHIM(CGColorSpaceCreateDeviceRGB) { RET(cf_wrap(CGColorSpaceCreateDeviceRGB())); }
SHIM(CGColorSpaceCreateDeviceGray) { RET(cf_wrap(CGColorSpaceCreateDeviceGray())); }
SHIM(CGBitmapContextCreate) {
    RET(cf_wrap(CGBitmapContextCreate(NP(ARG(0)), ARG(1), ARG(2), ARG(3), ARG(4), (CGColorSpaceRef)OBJ(5), ARG(6))));
}
static CGRect arg_rect(CPU *c, int n) {
    return CGRectMake(ARGF(n), ARGF(n + 1), ARGF(n + 2), ARGF(n + 3));
}
SHIM(CGContextDrawImage) { CGContextDrawImage((CGContextRef)OBJ(0), arg_rect(c, 1), (CGImageRef)OBJ(5)); }
SHIM(CGContextClearRect) { CGContextClearRect((CGContextRef)OBJ(0), arg_rect(c, 1)); }
SHIM(CGContextFillRect) { CGContextFillRect((CGContextRef)OBJ(0), arg_rect(c, 1)); }
SHIM(CGContextFlush) { CGContextFlush((CGContextRef)OBJ(0)); }
SHIM(CGContextSynchronize) { CGContextSynchronize((CGContextRef)OBJ(0)); }
SHIM(CGContextSaveGState) { CGContextSaveGState((CGContextRef)OBJ(0)); }
SHIM(CGContextRestoreGState) { CGContextRestoreGState((CGContextRef)OBJ(0)); }
SHIM(CGContextScaleCTM) { CGContextScaleCTM((CGContextRef)OBJ(0), ARGF(1), ARGF(2)); }
SHIM(CGContextTranslateCTM) { CGContextTranslateCTM((CGContextRef)OBJ(0), ARGF(1), ARGF(2)); }
SHIM(CGContextSetBlendMode) { CGContextSetBlendMode((CGContextRef)OBJ(0), (CGBlendMode)ARG(1)); }
SHIM(CGContextSetRGBFillColor) { CGContextSetRGBFillColor((CGContextRef)OBJ(0), ARGF(1), ARGF(2), ARGF(3), ARGF(4)); }
SHIM(CGContextSetShouldAntialias) { CGContextSetShouldAntialias((CGContextRef)OBJ(0), (bool)ARG(1)); }

// Seconds since the machine started, the clock events carry.
#if TARGET_OS_IPHONE
SHIM(GetCurrentEventTime) { RETF((double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9); }
#else
SHIM(GetCurrentEventTime) { RETF(GetCurrentEventTime()); }
#endif

// The game asks which OS it runs on; it answers as the newest system the 32-bit build ever supported.
SHIM(Gestalt) {
    u32 sel = ARG(0);
    i32 v = 0;
    int ok = 1;
    switch (sel) {
        case 'sysv': v = 0x10A6; break;
        case 'sys1': v = 10; break;
        case 'sys2': v = 14; break;
        case 'sys3': v = 6; break;
        case 'ram ': v = 0x40000000; break;
        default: ok = 0; break;
    }
    if (tracing()) fprintf(stderr, "[gestalt] %c%c%c%c ok=%d\n", (char)(sel >> 24), (char)(sel >> 16), (char)(sel >> 8), (char)sel, ok);
    if (ok && ARG(1)) G32(ARG(1)) = (u32)v;
    RET(ok ? 0 : -5551);
}

const ShimReg rt_shims_cg[] = {
    R(CGMainDisplayID) R(CGGetActiveDisplayList) R(CGDisplayBounds) R(CGDisplayScreenSize) R(CGDisplayIDToOpenGLDisplayMask)
    R(CGDisplayCopyDisplayMode) R(CGDisplayCopyAllDisplayModes) R(CGDisplayModeGetWidth) R(CGDisplayModeGetHeight)
    R(CGDisplayModeGetIOFlags) R(CGDisplayModeGetRefreshRate) R(CGDisplayModeCopyPixelEncoding) R(CGDisplayModeRelease)
    R2("_SetFrontProcess", cg_ok) R2("_CGDisplayRegisterReconfigurationCallback", cg_ok) R2("_CGDisplayRemoveReconfigurationCallback", cg_ok)
    R2("_CGSetDisplayTransferByTable", cg_ok) R2("_CGDisplayRestoreColorSyncSettings", cg_ok) R(CGGetDisplayTransferByTable)
    R(CGWindowLevelForKey) R(TISCopyCurrentASCIICapableKeyboardLayoutInputSource) R(TISCopyCurrentASCIICapableKeyboardInputSource) R(TISCopyCurrentKeyboardLayoutInputSource) R(TISCopyCurrentKeyboardInputSource)
    R(TISGetInputSourceProperty) R(LMGetKbdType) R(Gestalt) R(GetCurrentEventTime)
    R(CGImageSourceCreateWithData) R(CGImageSourceGetCount) R(CGImageSourceCreateImageAtIndex) R(CGImageGetWidth) R(CGImageGetHeight)
    R(CGImageGetBitsPerComponent) R(CGImageGetBitsPerPixel) R(CGImageGetBytesPerRow) R(CGImageGetBitmapInfo) R(CGImageGetDataProvider)
    R(CGDataProviderCopyData) R(CGDataProviderCreateWithCFData) R2("_CGImageRelease", cf_release_any) R2("_CGColorSpaceRelease", cf_release_any)
    R2("_CGContextRelease", cf_release_any) R(CGColorSpaceCreateDeviceRGB) R(CGColorSpaceCreateDeviceGray) R(CGBitmapContextCreate)
    R(CGContextDrawImage) R(CGContextClearRect) R(CGContextFillRect) R(CGContextFlush) R(CGContextSynchronize) R(CGContextSaveGState)
    R(CGContextRestoreGState) R(CGContextScaleCTM) R(CGContextTranslateCTM) R(CGContextSetBlendMode) R(CGContextSetRGBFillColor)
    R(CGContextSetShouldAntialias)
    {NULL, NULL},
};
