// Hand-written OpenGL and CGL shims: the calls whose memory or handles can't be forwarded mechanically. Mac only: displays, images and the
// rest the phone needs too are in shims_cg.c.
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
static void record_string(const char *what, u32 a, u32 b, const char *s, u32 from);
SHIM(glGetString) {
    const char *s = (const char *)glGetString(ARG(0));
    record_string("String", ARG(0), 0, s, G32(c->esp));
    if (trace_gl) fprintf(stderr, "[gl] glGetString(%#x) = %s\n", ARG(0), s ? s : "(null)");
    RET(s ? rt_guest_strdup(s) : 0);
}
SHIM(glGetStringi) {
    const char *s = (const char *)glGetStringi(ARG(0), ARG(1));
    record_string("Stringi", ARG(0), ARG(1), s, G32(c->esp));
    RET(s ? rt_guest_strdup(s) : 0);
}

// ARKHAM_GL_RECORD=1: what the Mac's OpenGL answers to the game's questions, as [glrec] lines. With Metal drawing, these questions at start
// are nearly all the OpenGL the game still uses, and the phone, which has no OpenGL, answers them the same way (ios_gl.c).
static int gl_record = -1;
static int recording(void) {
    if (gl_record < 0) gl_record = getenv("ARKHAM_GL_RECORD") != NULL;
    return gl_record;
}
#define UNSET 0x7EADBEEF
static void record_ints(const char *what, u32 a, u32 b, const GLint *v, u32 from) {
    int n = 0;
    while (n < 16 && v[n] != UNSET) n++;
    fprintf(stderr, "[glrec] %s %#x %#x from %08x:", what, a, b, from);
    for (int i = 0; i < n; i++) fprintf(stderr, " %d", v[i]);
    fprintf(stderr, "\n");
}
SHIM(glGetIntegerv) {
    GLint v[16];
    for (int i = 0; i < 16; i++) v[i] = UNSET;
    glGetIntegerv(ARG(0), v);
    if (recording()) record_ints("Integerv", ARG(0), 0, v, G32(c->esp));
    int n = 0;
    while (n < 16 && v[n] != UNSET) n++;
    if (ARG(1)) memcpy(GP(ARG(1)), v, (size_t)n * 4);
}
SHIM(glGetFloatv) {
    GLfloat v[16];
    for (int i = 0; i < 16; i++) memcpy(&v[i], &(GLint){UNSET}, 4);
    glGetFloatv(ARG(0), v);
    int n = 0;
    while (n < 16 && memcmp(&v[n], &(GLint){UNSET}, 4)) n++;
    if (recording()) {
        fprintf(stderr, "[glrec] Floatv %#x from %08x:", ARG(0), G32(c->esp));
        for (int i = 0; i < n; i++) fprintf(stderr, " %g", v[i]);
        fprintf(stderr, "\n");
    }
    if (ARG(1)) memcpy(GP(ARG(1)), v, (size_t)n * 4);
}
SHIM(glGetProgramivARB) {
    GLint v[16];
    for (int i = 0; i < 16; i++) v[i] = UNSET;
    static void (*get)(GLenum, GLenum, GLint *);  // from the legacy profile, not in gl3.h
    if (!get) get = dlsym(RTLD_DEFAULT, "glGetProgramivARB");
    if (get) get(ARG(0), ARG(1), v);
    if (recording()) record_ints("ProgramivARB", ARG(0), ARG(1), v, G32(c->esp));
    int n = 0;
    while (n < 16 && v[n] != UNSET) n++;
    if (ARG(2)) memcpy(GP(ARG(2)), v, (size_t)n * 4);
}
SHIM(glGetQueryiv) {
    GLint v[16];
    for (int i = 0; i < 16; i++) v[i] = UNSET;
    glGetQueryiv(ARG(0), ARG(1), v);
    if (recording()) record_ints("Queryiv", ARG(0), ARG(1), v, G32(c->esp));
    int n = 0;
    while (n < 16 && v[n] != UNSET) n++;
    if (ARG(2)) memcpy(GP(ARG(2)), v, (size_t)n * 4);
}
SHIM(glCheckFramebufferStatus) {
    GLenum r = glCheckFramebufferStatus(ARG(0));
    if (recording()) fprintf(stderr, "[glrec] CheckFramebufferStatus %#x from %08x: %#x\n", ARG(0), G32(c->esp), r);
    RET(r);
}
static void record_string(const char *what, u32 a, u32 b, const char *s, u32 from) {
    if (recording()) fprintf(stderr, "[glrec] %s %#x %u from %08x: %s\n", what, a, b, from, s ? s : "(null)");
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
        void rt_pipe_log_cache_file(void);
        rt_pipe_log_cache_file();
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
        {
            void rt_glq_report(void);
            static int reports;
            if (++reports % 30 == 0) rt_glq_report();
        }
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
    // With a Direct3D trace, only the traced frames.
    int rt_d3d_trace_picture(void);
    static int snap_traced = -1;
    if (snap_traced < 0) snap_traced = getenv("ARKHAM_D3D_TRACE") != NULL;
    if (snap_dir && (snap_traced ? rt_d3d_trace_picture() : frame <= 3 || frame == 10 || frame == 30 || frame % 100 == 0)) {
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

void rt_gl_init(void) { trace_gl = getenv("ARKHAM_TRACE_GL") != NULL; }

const ShimReg rt_shims_gl[] = {
    R(glGetString) R(glGetStringi) R(glGetIntegerv) R(glGetFloatv) R(glGetProgramivARB) R(glGetQueryiv) R(glCheckFramebufferStatus) R(glShaderSource) R(glTransformFeedbackVaryings) R(glMapBuffer) R(glMapBufferRange)
    R(glBindBuffer) R(glBindVertexArray) R(glDeleteBuffers) R(glCompileShader) R(glLinkProgram) R(glFlushMappedBufferRange) R2("_glFlushMappedBufferRangeAPPLE", glFlushMappedBufferRange) R(glUnmapBuffer)
    R(glFenceSync) R(glClientWaitSync) R(glDeleteSync) R(glFinish)
    R(CGLSetCurrentContext) R(CGLLockContext) R(CGLUnlockContext) R(CGLFlushDrawable) R(CGLSetParameter) R(CGLEnable) R(CGLDisable)
    R(CGLClearDrawable) R(CGLGetShareGroup) R(CGLQueryRendererInfo) R(CGLDescribeRenderer) R(CGLDestroyRendererInfo)
    {NULL, NULL},
};
