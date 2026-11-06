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

// A pointer argument means a guest address or a buffer offset, depending on what is bound. This is asked thousands of times a
// frame and glGet is slow, so each thread remembers the bindings of its current context.
enum { BIND_ARRAY, BIND_ELEMENT, BIND_UNPACK, BIND_PACK, BIND_INDIRECT, BIND_COUNT };
static __thread struct { GLint v[BIND_COUNT]; u8 valid[BIND_COUNT]; } bound_cache;
static int bind_slot_of_query(unsigned binding) {
    switch (binding) {
        case GL_ARRAY_BUFFER_BINDING: return BIND_ARRAY;
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: return BIND_ELEMENT;
        case GL_PIXEL_UNPACK_BUFFER_BINDING: return BIND_UNPACK;
        case GL_PIXEL_PACK_BUFFER_BINDING: return BIND_PACK;
        case GL_DRAW_INDIRECT_BUFFER_BINDING: return BIND_INDIRECT;
        default: return -1;
    }
}
static int bind_slot_of_target(unsigned target) {
    switch (target) {
        case GL_ARRAY_BUFFER: return BIND_ARRAY;
        case GL_ELEMENT_ARRAY_BUFFER: return BIND_ELEMENT;
        case GL_PIXEL_UNPACK_BUFFER: return BIND_UNPACK;
        case GL_PIXEL_PACK_BUFFER: return BIND_PACK;
        case GL_DRAW_INDIRECT_BUFFER: return BIND_INDIRECT;
        default: return -1;
    }
}
static __thread void *current_ctx;
static __thread int current_known;
void rt_gl_context_changed(void) {
    memset(&bound_cache, 0, sizeof bound_cache);
    current_known = 0;
}
void *gl_ptr_or_offset(u32 v, unsigned binding) {
    int k = bind_slot_of_query(binding);
    GLint bound = 0;
    if (k < 0) {
        glGetIntegerv(binding, &bound);
    } else {
        if (!bound_cache.valid[k]) {
            glGetIntegerv(binding, &bound_cache.v[k]);
            bound_cache.valid[k] = 1;
        }
        bound = bound_cache.v[k];
    }
    if (bound) return (void *)(uintptr_t)v;
    return v ? (void *)(rt_mem + v) : NULL;
}
SHIM(glBindBuffer) {
    glBindBuffer(ARG(0), ARG(1));
    int k = bind_slot_of_target(ARG(0));
    if (k >= 0) {
        bound_cache.v[k] = (GLint)ARG(1);
        bound_cache.valid[k] = 1;
    }
}
// The element buffer binding belongs to the vertex array object.
SHIM(glBindVertexArray) {
    glBindVertexArray(ARG(0));
    bound_cache.valid[BIND_ELEMENT] = 0;
}
// Deleting a bound buffer unbinds it.
SHIM(glDeleteBuffers) {
    glDeleteBuffers((GLsizei)ARG(0), (const GLuint *)GP(ARG(1)));
    rt_gl_context_changed();
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
// Report what the translated GLSL did: a failed shader or link is the first thing to look for when a frame is wrong.
static int shader_fail_n, shader_ok_n, link_fail_n, link_ok_n;
SHIM(glCompileShader) {
    GLuint sh = ARG(0);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok) {
        shader_ok_n++;
        return;
    }
    int n = shader_fail_n++;
    if (n >= 20) return;
    char log[2048] = "";
    glGetShaderInfoLog(sh, sizeof log, NULL, log);
    fprintf(stderr, "[gl] shader %u failed to compile: %.400s\n", sh, log);
    GLint len = 0;
    glGetShaderiv(sh, GL_SHADER_SOURCE_LENGTH, &len);
    if (len > 0) {
        char *src = malloc((size_t)len + 1);
        glGetShaderSource(sh, len + 1, NULL, src);
        char path[64];
        snprintf(path, sizeof path, "logs/shader_fail_%d.glsl", n);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "// %s\n%s", log, src);
            fclose(f);
        }
        free(src);
    }
}
SHIM(glLinkProgram) {
    GLuint pr = ARG(0);
    glLinkProgram(pr);
    GLint ok = 0;
    glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (ok) {
        link_ok_n++;
        return;
    }
    if (link_fail_n++ < 20) {
        char log[2048] = "";
        glGetProgramInfoLog(pr, sizeof log, NULL, log);
        fprintf(stderr, "[gl] program %u failed to link: %.400s\n", pr, log);
    }
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
// The engine sets the context around every OpenGL call, hundreds of thousands of times a second.
SHIM(CGLSetCurrentContext) {
    void *ctx = CTX(0);
    if (current_known && ctx == current_ctx) {
        RET(0);
        return;
    }
    rt_gl_context_changed();
    current_ctx = ctx;
    current_known = 1;
    RET(CGLSetCurrentContext(ctx));
}
SHIM(CGLLockContext) { RET(CGLLockContext(CTX(0))); }
SHIM(CGLUnlockContext) { RET(CGLUnlockContext(CTX(0))); }
unsigned long rt_gl_draws;
void rt_hitch_tick(void);
void rt_pipe_mark(int stage);
void rt_fps_overlay(double fps, double worst_ms);
#include <time.h>
// Read the back buffer before it is shown: a window snapshot can't see an OpenGL surface.
// ARKHAM_SNAP_POPS=dir keeps the last three frames and saves them when a part of the picture changes for exactly one frame.
static void save_rgba_top_down(const char *path, u8 *flip, int w, int h) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(flip, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs, kCGImageAlphaPremultipliedLast);
    CGImageRef img = CGBitmapContextCreateImage(cg);
    CFStringRef ps = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, ps, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    if (d) {
        CGImageDestinationAddImage(d, img, NULL);
        CGImageDestinationFinalize(d);
        CFRelease(d);
    }
    CFRelease(url);
    CFRelease(ps);
    CGImageRelease(img);
    CGContextRelease(cg);
    CGColorSpaceRelease(cs);
}
static void pop_check(unsigned long frame, unsigned long draws) {
    static const char *dir = (const char *)1;
    if (dir == (const char *)1) dir = getenv("ARKHAM_SNAP_POPS");
    if (!dir) return;
    static u8 *buf[3];
    static int w, h, saved;
    static unsigned long n;
    GLint vp[4] = {0}, read_fb = 0, pack_buf = 0, read_buf = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buf);
    glGetIntegerv(GL_READ_BUFFER, &read_buf);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glGetIntegerv(GL_VIEWPORT, vp);
    if (!buf[0] && vp[2] > 0 && vp[3] > 0) {
        w = vp[2];
        h = vp[3];
        for (int i = 0; i < 3; i++) buf[i] = malloc((size_t)w * (size_t)h * 4);
    }
    if (buf[0] && vp[2] == w && vp[3] == h) {
        // Through pixel buffers, picked up a few frames later, because waiting for the GPU here makes the glitch go away.
        enum { PBOS = 4 };
        static GLuint pbo[PBOS];
        static unsigned long reads;
        if (!pbo[0]) {
            glGenBuffers(PBOS, pbo);
            for (int i = 0; i < PBOS; i++) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]);
                glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, NULL, GL_STREAM_READ);
            }
        }
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[reads % PBOS]);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void *)0);
        reads++;
        if (reads <= PBOS) goto pop_done;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[reads % PBOS]);
        const void *ready = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
        if (ready) memcpy(buf[n % 3], ready, (size_t)w * (size_t)h * 4);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        n++;
        static long from = -1;
        if (from < 0) from = getenv("ARKHAM_SNAP_POPS_FROM") ? atol(getenv("ARKHAM_SNAP_POPS_FROM")) : 0;
        if (n >= 3 && saved < 18 && draws > 400 && (long)frame >= from) {  // not movies and menus, which fade all the time
            u8 *c = buf[(n - 1) % 3], *m = buf[(n - 2) % 3], *p = buf[(n - 3) % 3];
            int blocks = 0, moved = 0;
            for (int by = 0; by + 32 <= h; by += 32) {
                for (int bx = 0; bx + 32 <= w; bx += 32) {
                    long dpm = 0, dmc = 0, dpc = 0;
                    for (int y = by; y < by + 32; y += 4) {
                        const u8 *rp = p + (size_t)y * (size_t)w * 4 + 1, *rm = m + (size_t)y * (size_t)w * 4 + 1, *rc = c + (size_t)y * (size_t)w * 4 + 1;
                        for (int x = bx; x < bx + 32; x += 4) {
                            int a = rp[x * 4], b = rm[x * 4], d = rc[x * 4];
                            dpm += abs(a - b);
                            dmc += abs(b - d);
                            dpc += abs(a - d);
                        }
                    }
                    if (dpm > 64 * 6 && dmc > 64 * 6 && dpc * 4 < dpm) blocks++;
                    if (dpc > 64 * 3) moved++;
                }
            }
            // Only in a still picture: anything moving or fading across the screen looks the same to this test.
            if (blocks >= 8 && moved * 4 < blocks) {
                fprintf(stderr, "[pop] frame %lu: %d blocks changed for one frame\n", frame - 1, blocks);
                const char *names[3] = {"before", "pop", "after"};
                u8 *which[3] = {p, m, c};
                u8 *flip = malloc((size_t)w * (size_t)h * 4);
                for (int i = 0; i < 3; i++) {
                    for (int y = 0; y < h; y++) {
                        memcpy(flip + (size_t)y * (size_t)w * 4, which[i] + (size_t)(h - 1 - y) * (size_t)w * 4, (size_t)w * 4);
                        u8 *row = flip + (size_t)y * (size_t)w * 4;
                        for (int x = 0; x < w; x++) row[x * 4 + 3] = 255;
                    }
                    char path[1024];
                    snprintf(path, sizeof path, "%s/f%06lu_%s.png", dir, frame - 1, names[i]);
                    save_rgba_top_down(path, flip, w, h);
                }
                free(flip);
                saved++;
            }
        }
    }
pop_done:
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buf);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glReadBuffer((GLenum)read_buf);
}
static void gl_save_frame(const char *path) {
    GLint vp[4] = {0}, read_fb = 0, pack_buf = 0, read_buf = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buf);
    glGetIntegerv(GL_READ_BUFFER, &read_buf);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glGetIntegerv(GL_VIEWPORT, vp);
    int w = vp[2], h = vp[3];
    if (w > 0 && h > 0) {
        u8 *px = malloc((size_t)w * (size_t)h * 4);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        // Rows come bottom-up; alpha is forced so a transparent back buffer still shows its colours.
        u8 *flip = malloc((size_t)w * (size_t)h * 4);
        unsigned long lit = 0;
        for (int y = 0; y < h; y++) {
            memcpy(flip + (size_t)y * (size_t)w * 4, px + (size_t)(h - 1 - y) * (size_t)w * 4, (size_t)w * 4);
            u8 *row = flip + (size_t)y * (size_t)w * 4;
            for (int x = 0; x < w; x++) {
                if (row[x * 4] | row[x * 4 + 1] | row[x * 4 + 2]) lit++;
                row[x * 4 + 3] = 255;
            }
        }
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef cg = CGBitmapContextCreate(flip, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs, kCGImageAlphaPremultipliedLast);
        CGImageRef img = CGBitmapContextCreateImage(cg);
        CFStringRef ps = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, ps, kCFURLPOSIXPathStyle, false);
        CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
        if (d) {
            CGImageDestinationAddImage(d, img, NULL);
            CGImageDestinationFinalize(d);
            CFRelease(d);
        }
        CFRelease(url);
        CFRelease(ps);
        CGImageRelease(img);
        CGContextRelease(cg);
        CGColorSpaceRelease(cs);
        fprintf(stderr, "[frame] saved %dx%d to %s, non-black pixels %lu of %lu\n", w, h, path, lit, (unsigned long)w * (unsigned long)h);
        free(flip);
        free(px);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buf);
    glReadBuffer((GLenum)read_buf);
    while (glGetError() != GL_NO_ERROR) {}
}
SHIM(CGLFlushDrawable) {
    CGLContextObj ctx = CTX(0);
    static unsigned long frame, last_draws;
    static const char *snap_dir;
    static int snap_checked;
    if (!snap_checked) {
        snap_checked = 1;
        snap_dir = getenv("ARKHAM_GLSNAP");
    }
    frame++;
    // Present without waiting for the display, so the frame rate shows what the game can do (ARKHAM_VSYNC=1 keeps waiting).
    static int vsync_checked;
    if (!vsync_checked) {
        vsync_checked = 1;
        GLint cur = -1;
        CGLGetParameter(ctx, kCGLCPSwapInterval, &cur);
        if (!getenv("ARKHAM_VSYNC")) {
            GLint zero = 0;
            CGLSetParameter(ctx, kCGLCPSwapInterval, &zero);
        }
        fprintf(stderr, "[gl] swap interval was %d\n", cur);
    }
    unsigned long draws = rt_gl_draws - last_draws;
    last_draws = rt_gl_draws;
    // Frames per second, for the log and the on-screen counter.
    static u64 fps_start, fps_frames, fps_worst, fps_prev;
    u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!fps_start) fps_start = now;
    if (fps_prev && now - fps_prev > fps_worst) fps_worst = now - fps_prev;
    fps_prev = now;
    fps_frames++;
    if (now - fps_start >= 1000000000ull) {
        double secs = (double)(now - fps_start) / 1e9, fps = (double)fps_frames / secs;
        fprintf(stderr, "[fps] %.1f, worst frame %.1f ms, %lu draws\n", fps, (double)fps_worst / 1e6, draws);
        rt_fps_overlay(fps, (double)fps_worst / 1e6);
        fps_start = now;
        fps_frames = 0;
        fps_worst = 0;
    }
    if (frame <= 20 || frame % 100 == 0) {
        GLenum err = glGetError();
        fprintf(stderr, "[frame] %lu draws=%lu total=%lu glerr=%#x shaders ok=%d bad=%d programs ok=%d bad=%d\n", frame, draws, rt_gl_draws, err,
                shader_ok_n, shader_fail_n, link_ok_n, link_fail_n);
    }
    if (snap_dir && (frame <= 3 || frame == 10 || frame == 30 || frame % 100 == 0)) {
        char path[1024];
        snprintf(path, sizeof path, "%s/frame_%05lu.png", snap_dir, frame);
        gl_save_frame(path);
    }
    pop_check(frame, draws);
    rt_gate_release();
    rt_pipe_mark(4);
    CGLError e = CGLFlushDrawable(ctx);
    rt_pipe_mark(5);
    rt_hitch_tick();
    // No limit unless asked for: ARKHAM_FPS_CAP=120 keeps the frame rate at or below 120.
    static double cap = -1;
    static u64 next_ns;
    if (cap < 0) {
        const char *v = getenv("ARKHAM_FPS_CAP");
        cap = v ? atof(v) : 0.0;
    }
    if (cap > 0) {
        u64 t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), step = (u64)(1e9 / cap);
        if (t < next_ns) {
            u64 wait = next_ns - t;
            struct timespec ts = {(time_t)(wait / 1000000000u), (long)(wait % 1000000000u)};
            nanosleep(&ts, NULL);
            next_ns += step;
        } else {
            next_ns = t + step;
        }
    }
    rt_gate_acquire();
    RET(e);
}
SHIM(CGLSetParameter) {
    if (ARG(1) == kCGLCPSwapInterval) fprintf(stderr, "[gl] game sets swap interval to %d\n", ARG(2) ? (int)G32(ARG(2)) : -1);
    RET(CGLSetParameter(CTX(0), ARG(1), NP(ARG(2))));
}
SHIM(CGLEnable) {
    // kCGLCEMPEngine (313): Apple's multithreaded engine packs each call and then runs it right here anyway, so it only adds work.
    if (ARG(1) == 313 && !getenv("ARKHAM_GL_MT")) {
        RET(0);
        return;
    }
    RET(CGLEnable(CTX(0), ARG(1)));
}
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
SHIM(CGGetDisplayTransferByTable) {
    if (ARG(4)) G32(ARG(4)) = 0;
    RET(0);
}

// ---------- keyboard layout ----------
// The game names its keys after what the current layout types, so W would be bound as a Cyrillic letter on a Russian layout.
// Answering with the ASCII layout keeps the key names and bindings the same whatever input language is selected.
SHIM(TISCopyCurrentKeyboardLayoutInputSource) { RET(cf_wrap(TISCopyCurrentASCIICapableKeyboardLayoutInputSource())); }
SHIM(TISCopyCurrentASCIICapableKeyboardLayoutInputSource) { RET(cf_wrap(TISCopyCurrentASCIICapableKeyboardLayoutInputSource())); }
SHIM(TISCopyCurrentASCIICapableKeyboardInputSource) { RET(cf_wrap(TISCopyCurrentASCIICapableKeyboardInputSource())); }
SHIM(TISCopyCurrentKeyboardInputSource) { RET(cf_wrap(TISCopyCurrentASCIICapableKeyboardInputSource())); }
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
    R(glBindBuffer) R(glBindVertexArray) R(glDeleteBuffers) R(glCompileShader) R(glLinkProgram) R(glFlushMappedBufferRange) R2("_glFlushMappedBufferRangeAPPLE", glFlushMappedBufferRange) R(glUnmapBuffer)
    R(glFenceSync) R(glClientWaitSync) R(glDeleteSync) R(glFinish)
    R(CGLSetCurrentContext) R(CGLLockContext) R(CGLUnlockContext) R(CGLFlushDrawable) R(CGLSetParameter) R(CGLEnable) R(CGLDisable)
    R(CGLClearDrawable) R(CGLGetShareGroup) R(CGLQueryRendererInfo) R(CGLDescribeRenderer) R(CGLDestroyRendererInfo)
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
