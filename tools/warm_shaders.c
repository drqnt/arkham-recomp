// Builds the game's pixel shaders into Metal's disk cache ahead of play, so that opening a door doesn't stop the game for them.
//
// Apple's OpenGL builds a Metal function the first time a draw needs it, about 12 ms each, and keeps the result in a cache on disk that
// outlives the process. A cache entry depends on the program text, a little of the context and the state that affects pixels (blending,
// color mask, render target format and sample count), and not on the vertex program it is drawn with. So one draw per program and state, that touches no
// pixels, in a process of its own leaves exactly what the game will look up later.
//
// usage: warm_shaders programs.bin samples: samples is the game's anti-aliasing level, MaxMultisamples in its BmEngine.ini, because shaders built
// for one level are of no use at another.
// programs.bin comes from ARKHAM_SHADER_EXTRACT, see tools/build_shader_cache.sh. Running again is cheap: what is cached takes 0.5 ms.
// Metal gives each running process its own cache file, so this must not run together with the game.
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int blend;
    GLenum src, dst, src_alpha, dst_alpha;
    int alpha_write;
} Variant;
static const Variant OPAQUE_ = {0, GL_ONE, GL_ZERO, GL_ONE, GL_ZERO, 1};
static const Variant ADDITIVE = {1, GL_ONE, GL_ONE, GL_ZERO, GL_ONE, 1};
static const Variant TRANSLUCENT = {1, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE, 1};
static const Variant OPAQUE_NO_ALPHA = {0, GL_ONE, GL_ZERO, GL_ONE, GL_ZERO, 0};
static const Variant TRANSLUCENT_NO_ALPHA = {1, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE, 0};

// Which passes a pixel shader runs in, from the names of its parameters: nearly every shader is only ever used in one state.
static int variants_of(const char *text, Variant *out) {
    int n = 0;
    if (strstr(text, "LightAttenuationTexture") || strstr(text, "LightColorAndFalloffExponent")) {
        out[n++] = ADDITIVE;
    } else if (strstr(text, "SHBasisCubeTextures") || strstr(text, "WorldIncidentLighting")) {
        out[n++] = ADDITIVE;
        out[n++] = OPAQUE_;
    } else if (strstr(text, "AmbientColorAndSkyFactor") || strstr(text, "UpperSkyColor")) {
        out[n++] = OPAQUE_;
        out[n++] = TRANSLUCENT;
    } else {
        out[n++] = OPAQUE_;
        out[n++] = ADDITIVE;
        out[n++] = TRANSLUCENT;
    }
    if (strstr(text, "SceneColorTexture")) {
        out[n++] = OPAQUE_NO_ALPHA;
        out[n++] = TRANSLUCENT_NO_ALPHA;
    }
    return n;
}

static void storage(int samples, GLenum format) {
    if (samples > 1) glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, format, 16, 16);
    else glRenderbufferStorage(GL_RENDERBUFFER, format, 16, 16);
}
static void make_target(int samples) {
    GLuint fbo, color, depth;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    storage(samples, GL_RGBA16F_ARB);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    storage(samples, GL_DEPTH24_STENCIL8);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "render target is incomplete\n");
        exit(1);
    }
}
static void make_textures(void) {
    static unsigned char blank[4 * 4 * 4];
    GLuint t2d, tcube;
    glGenTextures(1, &t2d);
    glBindTexture(GL_TEXTURE_2D, t2d);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, blank);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenTextures(1, &tcube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tcube);
    for (GLenum f = 0; f < 6; f++) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, blank);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    for (GLenum u = 0; u < 16; u++) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, t2d);
        glBindTexture(GL_TEXTURE_CUBE_MAP, tcube);
    }
}
static GLuint load(GLenum target, const char *text, GLsizei len) {
    GLuint id;
    glGenProgramsARB(1, &id);
    glBindProgramARB(target, id);
    glProgramStringARB(target, GL_PROGRAM_FORMAT_ASCII_ARB, len, text);
    GLint err = -1;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &err);
    if (err != -1) {
        glDeleteProgramsARB(1, &id);
        return 0;
    }
    return id;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: warm_shaders programs.bin samples\n");
        return 1;
    }
    int samples = atoi(argv[2]);
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    // No recovery is in every cache key: the game's context has it, and so must this one.
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFANoRecovery, kCGLPFAColorSize, 32, kCGLPFAAlphaSize, 8, 0};
    CGLPixelFormatObj pf;
    GLint n_formats;
    CGLContextObj ctx;
    if (CGLChoosePixelFormat(attrs, &pf, &n_formats) || !pf || CGLCreateContext(pf, NULL, &ctx)) {
        fprintf(stderr, "no OpenGL context\n");
        return 1;
    }
    CGLSetCurrentContext(ctx);
    // Settings Feral's layer makes once, that reach the generated code.
    glClampColorARB(GL_CLAMP_VERTEX_COLOR_ARB, GL_FALSE);
    glProvokingVertexEXT(GL_FIRST_VERTEX_CONVENTION_EXT);
    glColorMaterial(GL_FRONT, GL_DIFFUSE);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(0x86A6);  // the layer turns this on too
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
    make_target(samples);
    make_textures();
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    static char zeros[4096];
    glBufferData(GL_ARRAY_BUFFER, sizeof zeros, zeros, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, NULL);
    glViewport(0, 0, 16, 16);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 0, 0);  // nothing reaches a pixel
    glEnable(GL_VERTEX_PROGRAM_ARB);
    glEnable(GL_FRAGMENT_PROGRAM_ARB);
    glEnable(GL_MULTISAMPLE);
    static const char vp_text[] =
        "!!ARBvp1.0\nMOV result.position, vertex.attrib[0];\nMOV result.color, vertex.attrib[0];\nMOV result.color.secondary, vertex.attrib[0];\n"
        "MOV result.texcoord[0], vertex.attrib[0];\nMOV result.texcoord[1], vertex.attrib[0];\nMOV result.texcoord[2], vertex.attrib[0];\n"
        "MOV result.texcoord[3], vertex.attrib[0];\nMOV result.texcoord[4], vertex.attrib[0];\nMOV result.texcoord[5], vertex.attrib[0];\n"
        "MOV result.texcoord[6], vertex.attrib[0];\nMOV result.texcoord[7], vertex.attrib[0];\nEND\n";
    if (!load(GL_VERTEX_PROGRAM_ARB, vp_text, (GLsizei)strlen(vp_text))) {
        fprintf(stderr, "the vertex program doesn't load\n");
        return 1;
    }

    static char text[1 << 22];
    long programs = 0, draws = 0, bad = 0;
    double t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9, slow = 0;
    unsigned head[2];
    while (fread(head, 4, 2, f) == 2) {
        if (head[1] >= sizeof text || fread(text, 1, head[1], f) != head[1]) {
            fprintf(stderr, "programs.bin is damaged\n");
            return 1;
        }
        text[head[1]] = 0;
        if (head[0] != GL_FRAGMENT_PROGRAM_ARB) continue;
        programs++;
        GLuint id = load(GL_FRAGMENT_PROGRAM_ARB, text, (GLsizei)head[1]);
        if (!id) {
            bad++;
            continue;
        }
        Variant v[8];
        int n = variants_of(text, v);
        for (int i = 0; i < n; i++) {
            if (v[i].blend) glEnable(GL_BLEND);
            else glDisable(GL_BLEND);
            glBlendFuncSeparate(v[i].src, v[i].dst, v[i].src_alpha, v[i].dst_alpha);
            glColorMask(1, 1, 1, v[i].alpha_write);
            double a = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
            glDrawArrays(GL_TRIANGLES, 0, 3);
            slow += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9 - a;
            draws++;
        }
        glDeleteProgramsARB(1, &id);
        if (programs % 500 == 0) {
            glFinish();
            printf("%ld programs, %ld draws, %ld rejected, %.0f s, drawing took %.1f ms each\n", programs, draws, bad, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9 - t0, slow * 1000 / (double)(draws ? draws : 1));
            fflush(stdout);
        }
    }
    glFinish();
    printf("done: %ld programs, %ld draws, %ld rejected, %.0f s, glerr %#x\n", programs, draws, bad, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9 - t0, glGetError());
    return 0;
}
