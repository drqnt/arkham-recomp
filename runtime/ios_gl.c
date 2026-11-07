// On the phone, in place of gl_queue.c, pipelines.c and shims_gl.c (tools/build.py --ios). iOS has no desktop OpenGL, and the game draws through the Metal layer there,
// so Feral's OpenGL path only needs to be quiet: what the rest of the runtime calls in those files does nothing, and gl_gen.c finds no OpenGL functions to call.
#include "shim.h"
#include "shim_reg.h"
#include "shims_gl.h"

#include <stdio.h>
#include <string.h>

const ShimReg rt_shims_gl[] = {{NULL, NULL}};
unsigned long rt_gl_draws;

void rt_gl_init(void) {}
void rt_gl_context_changed(void) {}
// Called by gl_gen.c for each OpenGL function the game asks for: logged once each, since on the phone every one of them is missing.
void gl_missing(const char *name) {
    enum { SEEN = 256 };
    static const char *seen[SEEN];
    for (int i = 0; i < SEEN; i++) {
        if (seen[i] == name) return;
        if (!seen[i]) {
            seen[i] = name;
            fprintf(stderr, "[gl] %s is called, and the phone has no OpenGL\n", name);
            return;
        }
    }
}
void *gl_ptr_or_offset(u32 v, unsigned binding) {
    (void)binding;
    return v ? GP(v) : NULL;
}

// gl_queue.c: OpenGL calls never go to a queue of their own here.
u8 rt_glq_kind(const char *name) {
    (void)name;
    return 0;
}
int rt_glq_route(CPU *c, void (*f)(CPU *), u8 kind, int gated, const char *name) {
    (void)c, (void)f, (void)kind, (void)gated, (void)name;
    return 0;
}
void rt_glq_drain(void) {}

// pipelines.c: capturing OpenGL programs for the Mac's shader cache.
void rt_pipe_capture_start(const char *path) { (void)path; }
void rt_pipe_capture_stop(void) {}
