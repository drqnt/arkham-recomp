// Apple's OpenGL builds Metal functions the first time a draw needs them, about 12 ms each, and that is what stalls a frame when a door opens.
// tools/build_shader_cache.sh builds the pixel shaders ahead of play. This file supports that: it collects the programs of Feral's layer, and
// with ARKHAM_TRACE_PIPES it says what is still being built during play. A slow draw is the sign of a new function, so that is when it looks
// at the state: normal draws pay nothing but the clock read the worker already does.
#define GL_SILENCE_DEPRECATION
#include "shim.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int trace = -1;
static const char *dump_dir;

static u64 hash_bytes(const void *p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    const u8 *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
static u64 program_hash(GLenum target) {
    GLint id = 0, len = 0;
    glGetProgramivARB(target, GL_PROGRAM_BINDING_ARB, &id);
    if (!id) return 0;
    glGetProgramivARB(target, GL_PROGRAM_LENGTH_ARB, &len);
    if (len <= 0) return 0;
    char *text = malloc((size_t)len + 1);
    glGetProgramStringARB(target, GL_PROGRAM_STRING_ARB, text);
    text[len] = 0;
    u64 h = hash_bytes(text, (size_t)len);
    if (dump_dir) {
        char path[512];
        snprintf(path, sizeof path, "%s/%016llx.txt", dump_dir, h);
        FILE *f = fopen(path, "wx");
        if (f) {
            fwrite(text, 1, (size_t)len, f);
            fclose(f);
        }
    }
    free(text);
    return h;
}
// Internal format and sample count of what a framebuffer attachment points at. Bindings are put back: the next command relies on them.
static void attachment_format(GLenum point, GLint *fmt, GLint *samples) {
    GLint type = 0, name = 0;
    *fmt = 0;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, point, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type == GL_NONE) return;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, point, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    if (type == GL_RENDERBUFFER) {
        GLint old = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &old);
        glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)name);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, fmt);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, samples);
        glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)old);
    } else if (type == GL_TEXTURE) {
        GLint face = 0, level = 0, old = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, point, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE, &face);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, point, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
        GLenum bind = face ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, query = face ? (GLenum)face : GL_TEXTURE_2D;
        glGetIntegerv(face ? GL_TEXTURE_BINDING_CUBE_MAP : GL_TEXTURE_BINDING_2D, &old);
        glBindTexture(bind, (GLuint)name);
        glGetTexLevelParameteriv(query, level, GL_TEXTURE_INTERNAL_FORMAT, fmt);
        glBindTexture(bind, (GLuint)old);
    }
}

// Metal gives a process another cache file when the usual one is being written at the moment it starts, and that file starts out empty.
void rt_pipe_log_cache_file(void) {
    if (!getenv("ARKHAM_TRACE_PIPES")) return;
    char path[MAXPATHLEN];
    for (int fd = 3; fd < 1024; fd++) {
        if (fcntl(fd, F_GETPATH, path) != -1 && strstr(path, "com.apple.metal") && strstr(path, ".data")) fprintf(stderr, "[pipe] Metal cache: %s\n", strrchr(path, '/') + 1);
    }
}

void rt_pipe_note_slow_draw(const char *name, u64 ticks) {
    if (trace < 0) {
        trace = getenv("ARKHAM_TRACE_PIPES") != NULL;
        dump_dir = getenv("ARKHAM_DUMP_PROGRAMS");
        if (dump_dir) mkdir(dump_dir, 0755);
    }
    if (!trace) return;
    u64 vp = program_hash(GL_VERTEX_PROGRAM_ARB), fp = program_hash(GL_FRAGMENT_PROGRAM_ARB);
    static const GLenum flags[] = {GL_VERTEX_PROGRAM_ARB, GL_FRAGMENT_PROGRAM_ARB, GL_BLEND, GL_ALPHA_TEST, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE,
                                   GL_FRAMEBUFFER_SRGB, GL_MULTISAMPLE, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SCISSOR_TEST, GL_FOG, GL_LIGHTING, GL_CLIP_PLANE0};
    u32 en = 0;
    for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; i++)
        if (glIsEnabled(flags[i])) en |= 1u << i;
    GLint src_c = 0, dst_c = 0, src_a = 0, dst_a = 0, eq_c = 0, eq_a = 0, alpha_func = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_c);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_c);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq_c);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eq_a);
    glGetIntegerv(GL_ALPHA_TEST_FUNC, &alpha_func);
    GLboolean cm[4] = {0};
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    GLint fbo = 0, samples = 0, color[4] = {0}, depth = 0, stencil = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    if (fbo) {
        for (int i = 0; i < 4; i++) attachment_format(GL_COLOR_ATTACHMENT0 + (GLenum)i, &color[i], &samples);
        attachment_format(GL_DEPTH_ATTACHMENT, &depth, &samples);
        attachment_format(GL_STENCIL_ATTACHMENT, &stencil, &samples);
    }
    char attribs[900] = "", tex[500] = "";
    size_t al = 0, tl = 0;
    // Arrays in client memory count from the lowest of their addresses, which is how the driver sees them.
    unsigned long lowest = ~0ul;
    for (GLuint i = 0; i < 16; i++) {
        GLint on = 0, buffer = 0;
        void *ptr = NULL;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &on);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
        glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
        if (on && !buffer && (unsigned long)ptr < lowest) lowest = (unsigned long)ptr;
    }
    for (GLuint i = 0; i < 16; i++) {
        GLint on = 0, size = 0, type = 0, norm = 0, stride = 0, buffer = 0, divisor = 0;
        void *ptr = NULL;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &on);
        if (!on) continue;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &norm);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
        glGetVertexAttribiv(i, 0x88FE, &divisor);
        glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
        al += (size_t)snprintf(attribs + al, sizeof attribs - al, "%u:%d,%x,%d,%d,%lu,%d,%d;", i, size, type, norm, stride, buffer ? (unsigned long)ptr : (unsigned long)ptr - lowest, divisor, buffer != 0);
    }
    GLint active = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    for (GLenum u = 0; u < 16; u++) {
        glActiveTexture(GL_TEXTURE0 + u);
        GLint t2 = 0, tc = 0, t3 = 0, fmt = 0, cmp = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &t2);
        glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &tc);
        glGetIntegerv(GL_TEXTURE_BINDING_3D, &t3);
        if (t2) {
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, &cmp);
            tl += (size_t)snprintf(tex + tl, sizeof tex - tl, "%u:2d,%x,%x;", u, fmt, cmp);
        }
        if (tc) {
            glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
            tl += (size_t)snprintf(tex + tl, sizeof tex - tl, "%u:cube,%x;", u, fmt);
        }
        if (t3) {
            glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
            tl += (size_t)snprintf(tex + tl, sizeof tex - tl, "%u:3d,%x;", u, fmt);
        }
    }
    glActiveTexture((GLenum)active);
    fprintf(stderr, "[pipe] %.1f ms %s vp=%016llx fp=%016llx en=%x blend=%x,%x,%x,%x,%x,%x alpha=%x mask=%d%d%d%d fbo=%d rt=%x,%x,%x,%x depth=%x stencil=%x samples=%d attribs=%s tex=%s\n",
            (double)ticks / 24000.0, name, vp, fp, en, src_c, dst_c, src_a, dst_a, eq_c, eq_a, alpha_func, cm[0], cm[1], cm[2], cm[3], fbo, color[0], color[1], color[2],
            color[3], depth, stencil, samples, attribs, tex);
}

// Keeps every program the layer hands to OpenGL, once each: ARKHAM_SHADER_EXTRACT in d3d_queue.c.
static FILE *capture_file;
static u64 *capture_seen;
#define CAPTURE_SEEN (1u << 18)
int rt_pipe_capturing;
static u32 capture_n;
void rt_pipe_capture_start(const char *path) {
    capture_file = fopen(path, "wb");
    capture_seen = calloc(CAPTURE_SEEN, sizeof(u64));
    if (!capture_file) {
        fprintf(stderr, "[extract] can't write the output\n");
        _exit(3);
    }
    __atomic_store_n(&rt_pipe_capturing, 1, __ATOMIC_SEQ_CST);
}
void rt_pipe_capture_stop(void) {
    __atomic_store_n(&rt_pipe_capturing, 0, __ATOMIC_SEQ_CST);
    fclose(capture_file);
    capture_file = NULL;
    fprintf(stderr, "[extract] %u different programs\n", capture_n);
}
// Runs on the OpenGL worker, in order: target, format, length, text.
void rt_pipe_capture_program(const u32 *args) {
    if (!capture_file) return;
    u32 target = args[0], len = args[2];
    const u8 *text = (const u8 *)GP(args[3]);
    u64 h = hash_bytes(text, len);
    if (!h) h = 1;
    u32 slot = (u32)(h % CAPTURE_SEEN);
    while (capture_seen[slot] && capture_seen[slot] != h) slot = (slot + 1) % CAPTURE_SEEN;
    if (capture_seen[slot]) return;
    capture_seen[slot] = h;
    capture_n++;
    fwrite(&target, 4, 1, capture_file);
    fwrite(&len, 4, 1, capture_file);
    fwrite(text, 1, len, capture_file);
}

// ARKHAM_TRACE_FRAME=seconds: how two whole frames are built, that long after the first present. Each line is a render pass as OpenGL sees it,
// with what ended it. Every change of target can cost the GPU a full store and load of the target, which is why this is worth seeing.
int rt_pipe_frame_map = -1;
static double fmap_at;
static int fmap_frames_left = 2, fmap_on;
static u32 fmap_draws, fmap_queries;
static int fmap_need_pass;
#define FMAP_NAMES 128
static struct { const char *name; u32 n; } fmap_counts[FMAP_NAMES];
static double fmap_last;
static void fmap_flush_pass(const char *why, const u32 *a) {
    // Waiting for the GPU after each pass slows these two frames down, and gives each pass's own time.
    double before = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
    glFinish();
    double after = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
    fprintf(stderr, "[fmap]   cpu %.2f gpu %.2f ms, %u draws, %u queries, then %s %x %x %x %x %x %x %x %x %x %x\n", before - fmap_last, after - before, fmap_draws, fmap_queries, why, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
    fmap_last = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
    fmap_draws = fmap_queries = 0;
}
void rt_pipe_frame_note(const char *name, const u32 *args) {
    if (rt_pipe_frame_map < 0) {
        const char *v = getenv("ARKHAM_TRACE_FRAME");
        rt_pipe_frame_map = v != NULL;
        if (v) fmap_at = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9 + atof(v);
        if (!v) return;
    }
    if (!fmap_frames_left || !name) return;
    int flush = !strcmp(name, "_CGLFlushDrawable");
    if (!fmap_on) {
        if (flush && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9 >= fmap_at) {
            fmap_on = 1;
            fmap_need_pass = 1;
            glFinish();
            fmap_last = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
            fprintf(stderr, "[fmap] frame starts\n");
        }
        return;
    }
    int k = 0;
    for (; k < FMAP_NAMES && fmap_counts[k].name; k++)
        if (fmap_counts[k].name == name) break;
    if (k < FMAP_NAMES) {
        fmap_counts[k].name = name;
        fmap_counts[k].n++;
    }
    if (!strncmp(name, "_glDraw", 7)) {
        if (fmap_need_pass) {
            fmap_need_pass = 0;
            GLint fbo = 0, samples = 0, color = 0, depth = 0, vp[4] = {0};
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
            glGetIntegerv(GL_VIEWPORT, vp);
            if (fbo) {
                attachment_format(GL_COLOR_ATTACHMENT0, &color, &samples);
                attachment_format(GL_DEPTH_ATTACHMENT, &depth, &samples);
            }
            fprintf(stderr, "[fmap] pass fbo=%d color=%x depth=%x samples=%d viewport=%dx%d\n", fbo, color, depth, samples, vp[2], vp[3]);
        }
        fmap_draws++;
    } else if (!strncmp(name, "_glBeginQuery", 13)) {
        fmap_queries++;
    } else if (strstr(name, "Framebuffer") || !strcmp(name, "_glClear") || !strcmp(name, "_glFlush") || !strcmp(name, "_glFinish") || !strncmp(name, "_glReadPixels", 13) ||
               !strncmp(name, "_glCopyTex", 10) || !strncmp(name, "_glDrawBuffer", 13) || !strcmp(name, "_glGenerateMipmap")) {
        fmap_flush_pass(name + 1, args);
        fmap_need_pass = 1;
    }
    if (flush) {
        fmap_flush_pass("present", args);
        fprintf(stderr, "[fmap] calls this frame:");
        for (int i = 0; i < FMAP_NAMES && fmap_counts[i].name; i++) {
            fprintf(stderr, " %s:%u", fmap_counts[i].name + 1, fmap_counts[i].n);
            fmap_counts[i].n = 0;
        }
        fprintf(stderr, "\n");
        if (--fmap_frames_left) fprintf(stderr, "[fmap] frame starts\n");
    }
}
