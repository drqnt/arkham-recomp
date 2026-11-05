// Hand-written OpenGL and CGL shims: the calls whose memory or handles can't be forwarded mechanically,
// plus CoreGraphics display queries.
#define GL_SILENCE_DEPRECATION
#include "shim.h"
#include "shim_reg.h"
#include "shims_gl.h"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

extern u32 cf_wrap(CFTypeRef o);
extern CFTypeRef cf_unwrap(u32 g);

void gl_missing(const char *name) {
    static int seen;
    if (seen++ < 40) fprintf(stderr, "[gl] host has no %s\n", name);
}

void *gl_ptr_or_offset(u32 v, unsigned binding) {
    GLint bound = 0;
    glGetIntegerv(binding, &bound);
    if (bound) return (void *)(uintptr_t)v;
    return v ? (void *)(rt_mem + v) : NULL;
}

static int trace_gl;

// ---------- strings handed back to guest memory ----------
SHIM(glGetString) {
    const char *s = (const char *)glGetString(ARG(0));
    if (trace_gl) fprintf(stderr, "[gl] glGetString(%#x) = %s\n", ARG(0), s ? s : "(null)");
    RET(s ? rt_guest_strdup(s) : 0);
}
SHIM(glGetStringi) {
    const char *s = (const char *)glGetStringi(ARG(0), ARG(1));
    RET(s ? rt_guest_strdup(s) : 0);
}

// ---------- shaders ----------
SHIM(glShaderSource) {
    GLsizei n = (GLsizei)ARG(1);
    const char **strs = calloc((size_t)n + 1, sizeof(char *));
    for (GLsizei i = 0; i < n; i++) strs[i] = GS(G32(ARG(2) + (u32)i * 4));
    if (trace_gl) fprintf(stderr, "[gl] glShaderSource shader=%u: %.200s\n", ARG(0), strs[0]);
    glShaderSource(ARG(0), n, strs, NP(ARG(3)));
    free(strs);
}
SHIM(glTransformFeedbackVaryings) {
    GLsizei n = (GLsizei)ARG(1);
    const char **strs = calloc((size_t)n + 1, sizeof(char *));
    for (GLsizei i = 0; i < n; i++) strs[i] = GS(G32(ARG(2) + (u32)i * 4));
    glTransformFeedbackVaryings(ARG(0), n, strs, ARG(3));
    free(strs);
}

// ---------- buffer mapping through guest-visible shadow copies, since host pointers don't fit in 32 bits ----------
typedef struct { GLenum target; u8 *host; u32 guest; u32 size; GLbitfield access; int used; } MapSlot;
static MapSlot maps[32];
#define GL_MAP_READ 0x1
#define GL_MAP_WRITE 0x2
#define GL_MAP_INVALIDATE_RANGE 0x4
#define GL_MAP_INVALIDATE_BUFFER 0x8
#define GL_MAP_FLUSH_EXPLICIT 0x10

static u32 map_begin(GLenum target, u8 *host, u32 size, GLbitfield access) {
    if (!host) return 0;
    for (int i = 0; i < 32; i++) {
        if (!maps[i].used) {
            u32 g = rt_guest_malloc(size ? size : 16);
            if ((access & GL_MAP_READ) || !(access & (GL_MAP_INVALIDATE_RANGE | GL_MAP_INVALIDATE_BUFFER))) memcpy(GP(g), host, size);
            maps[i] = (MapSlot){target, host, g, size, access, 1};
            return g;
        }
    }
    fprintf(stderr, "[gl] too many mapped buffers\n");
    return 0;
}
SHIM(glMapBuffer) {
    GLenum target = ARG(0), acc = ARG(1);
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    u8 *h = glMapBuffer(target, acc);
    GLbitfield access = acc == GL_READ_ONLY ? GL_MAP_READ : acc == GL_WRITE_ONLY ? GL_MAP_WRITE : (GL_MAP_READ | GL_MAP_WRITE);
    RET(map_begin(target, h, (u32)size, access));
}
SHIM(glMapBufferRange) {
    GLenum target = ARG(0);
    u32 size = ARG(2);
    u8 *h = glMapBufferRange(target, (GLintptr)(i32)ARG(1), (GLsizeiptr)size, ARG(3));
    RET(map_begin(target, h, size, ARG(3)));
}
SHIM(glFlushMappedBufferRange) {
    GLenum target = ARG(0);
    u32 off = ARG(1), len = ARG(2);
    for (int i = 0; i < 32; i++) {
        if (maps[i].used && maps[i].target == target) {
            memcpy(maps[i].host + off, GP(maps[i].guest + off), len);
            break;
        }
    }
    glFlushMappedBufferRange(target, (GLintptr)off, (GLsizeiptr)len);
}
SHIM(glUnmapBuffer) {
    GLenum target = ARG(0);
    for (int i = 0; i < 32; i++) {
        if (maps[i].used && maps[i].target == target) {
            if ((maps[i].access & GL_MAP_WRITE) && !(maps[i].access & GL_MAP_FLUSH_EXPLICIT)) memcpy(maps[i].host, GP(maps[i].guest), maps[i].size);
            rt_guest_free(maps[i].guest);
            maps[i].used = 0;
            break;
        }
    }
    RET(glUnmapBuffer(target));
}

// ---------- sync objects: real pointers behind handles; waits release the gate ----------
SHIM(glFenceSync) { RET(rt_handle_new(glFenceSync(ARG(0), ARG(1)))); }
SHIM(glClientWaitSync) {
    GLsync s = rt_handle_get(ARG(0));
    GLbitfield flags = ARG(1);
    u64 timeout = ARGQ(2);
    rt_gate_release();
    GLenum r = glClientWaitSync(s, flags, timeout);
    rt_gate_acquire();
    RET(r);
}
SHIM(glDeleteSync) {
    glDeleteSync(rt_handle_get(ARG(0)));
    rt_handle_free(ARG(0));
}
SHIM(glFinish) {
    rt_gate_release();
    glFinish();
    rt_gate_acquire();
}

// ---------- CGL ----------
#define CTX(n) ((CGLContextObj)rt_handle_get(ARG(n)))
SHIM(CGLSetCurrentContext) { RET(CGLSetCurrentContext(CTX(0))); }
SHIM(CGLLockContext) { RET(CGLLockContext(CTX(0))); }
SHIM(CGLUnlockContext) { RET(CGLUnlockContext(CTX(0))); }
SHIM(CGLFlushDrawable) {
    CGLContextObj ctx = CTX(0);
    rt_gate_release();
    CGLError e = CGLFlushDrawable(ctx);
    rt_gate_acquire();
    RET(e);
}
SHIM(CGLSetParameter) { RET(CGLSetParameter(CTX(0), ARG(1), NP(ARG(2)))); }
SHIM(CGLEnable) { RET(CGLEnable(CTX(0), ARG(1))); }
SHIM(CGLDisable) { RET(CGLDisable(CTX(0), ARG(1))); }
SHIM(CGLClearDrawable) { RET(CGLClearDrawable(CTX(0))); }
SHIM(CGLGetShareGroup) { RET(rt_handle_new(CGLGetShareGroup(CTX(0)))); }
SHIM(CGLQueryRendererInfo) {
    CGLRendererInfoObj info = NULL;
    GLint n = 0;
    CGLError e = CGLQueryRendererInfo(ARG(0), &info, &n);
    if (!e) {
        G32(ARG(1)) = rt_handle_new(info);
        G32(ARG(2)) = (u32)n;
    }
    RET(e);
}
SHIM(CGLDescribeRenderer) {
    GLint v = 0;
    CGLError e = CGLDescribeRenderer(rt_handle_get(ARG(0)), (GLint)ARG(1), ARG(2), &v);
    if (trace_gl) fprintf(stderr, "[cgl] describe renderer %d prop %u = %d (err %d)\n", (int)ARG(1), ARG(2), v, e);
    G32(ARG(3)) = (u32)v;
    RET(e);
}
SHIM(CGLDestroyRendererInfo) {
    CGLError e = CGLDestroyRendererInfo(rt_handle_get(ARG(0)));
    rt_handle_free(ARG(0));
    RET(e);
}

// ---------- displays ----------
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
SHIM(CGDisplayModeCopyPixelEncoding) { RET(cf_wrap(CFStringCreateWithCString(NULL, "--------RRRRRRRRGGGGGGGGBBBBBBBB", kCFStringEncodingUTF8))); }
SHIM(CGDisplayModeRelease) {}
SHIM(cg_ok) { RET(0); }
SHIM(CGWindowLevelForKey) { RET(CGWindowLevelForKey((CGWindowLevelKey)ARG(0))); }
SHIM(CGEventSourceKeyState) { RET(0); }
SHIM(CGGetDisplayTransferByTable) {
    if (ARG(4)) G32(ARG(4)) = 0;
    RET(0);
}

// ---------- keyboard layout ----------
SHIM(TISCopyCurrentKeyboardLayoutInputSource) { RET(cf_wrap(TISCopyCurrentKeyboardLayoutInputSource())); }
SHIM(TISCopyCurrentKeyboardInputSource) { RET(cf_wrap(TISCopyCurrentKeyboardInputSource())); }
SHIM(TISGetInputSourceProperty) { RET(cf_wrap(TISGetInputSourceProperty((TISInputSourceRef)cf_unwrap(ARG(0)), (CFStringRef)cf_unwrap(ARG(1))))); }
SHIM(LMGetKbdType) { RET(LMGetKbdType()); }

// ---------- images through the real ImageIO and CoreGraphics ----------
#include <ImageIO/ImageIO.h>
#define OBJ(n) cf_unwrap(ARG(n))
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

SHIM(GetCurrentEventTime) { RETF(GetCurrentEventTime()); }

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
    if (trace_gl) fprintf(stderr, "[gestalt] %c%c%c%c ok=%d\n", (char)(sel >> 24), (char)(sel >> 16), (char)(sel >> 8), (char)sel, ok);
    if (ok && ARG(1)) G32(ARG(1)) = (u32)v;
    RET(ok ? 0 : -5551);
}

void rt_gl_init(void) { trace_gl = getenv("ARKHAM_TRACE_GL") != NULL; }

const ShimReg rt_shims_gl[] = {
    R(glGetString) R(glGetStringi) R(glShaderSource) R(glTransformFeedbackVaryings) R(glMapBuffer) R(glMapBufferRange)
    R(glFlushMappedBufferRange) R2("_glFlushMappedBufferRangeAPPLE", glFlushMappedBufferRange) R(glUnmapBuffer)
    R(glFenceSync) R(glClientWaitSync) R(glDeleteSync) R(glFinish)
    R(CGLSetCurrentContext) R(CGLLockContext) R(CGLUnlockContext) R(CGLFlushDrawable) R(CGLSetParameter) R(CGLEnable) R(CGLDisable)
    R(CGLClearDrawable) R(CGLGetShareGroup) R(CGLQueryRendererInfo) R(CGLDescribeRenderer) R(CGLDestroyRendererInfo)
    R(CGMainDisplayID) R(CGGetActiveDisplayList) R(CGDisplayBounds) R(CGDisplayScreenSize) R(CGDisplayIDToOpenGLDisplayMask)
    R(CGDisplayCopyDisplayMode) R(CGDisplayCopyAllDisplayModes) R(CGDisplayModeGetWidth) R(CGDisplayModeGetHeight)
    R(CGDisplayModeGetIOFlags) R(CGDisplayModeGetRefreshRate) R(CGDisplayModeCopyPixelEncoding) R(CGDisplayModeRelease)
    R2("_SetFrontProcess", cg_ok) R2("_CGDisplayRegisterReconfigurationCallback", cg_ok) R2("_CGDisplayRemoveReconfigurationCallback", cg_ok)
    R2("_CGAssociateMouseAndMouseCursorPosition", cg_ok) R2("_CGWarpMouseCursorPosition", cg_ok) R2("_CGSetLocalEventsSuppressionInterval", cg_ok)
    R2("_CGSetDisplayTransferByTable", cg_ok) R2("_CGDisplayRestoreColorSyncSettings", cg_ok) R(CGGetDisplayTransferByTable)
    R(CGWindowLevelForKey) R(CGEventSourceKeyState) R(TISCopyCurrentKeyboardLayoutInputSource) R(TISCopyCurrentKeyboardInputSource)
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
