// A second thread for OpenGL. The game's render thread spends over a third of each frame inside the OpenGL driver, so here it only
// writes commands into a ring and carries on, while a worker that owns the context executes them.
//
// A command is the real shim plus a copy of its guest arguments, and the ring lives in guest memory, so the worker runs the shim
// unchanged with its esp pointed at the copy. A function not listed below is synchronous: the render thread waits until everything
// before it, and the call itself, has run. Only calls that return nothing and read no memory later on may be asynchronous, so a
// missing entry costs speed and never correctness. ARKHAM_GL_DIRECT=1 turns the whole thing off.
//
// Occlusion query results stay synchronous. Reading them ahead of commands still in the ring returns 0 samples at times, the engine
// then takes an object for hidden and a wall vanishes for one frame. ARKHAM_SNAP_POPS in shims_gl.c detects that.
#include "shim.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*shim_fn)(CPU *);
void rt_gl_context_changed(void);
shim_fn rt_lookup_shim(const char *sym);

enum {
    Q_NONE,      // not an OpenGL function
    Q_SYNC,
    Q_ASYNC,
    Q_LOCK,      // CGLLockContext: the worker holds the real lock while it runs
    Q_UNLOCK,
    Q_SETCTX,
    Q_FLUSH,
    Q_BIND_BUFFER,
    Q_BIND_VAO,
    Q_ATTRIB_POINTER,
    Q_CLIENT_ENABLE,
    Q_CLIENT_DISABLE,
    Q_ATTRIB_ENABLE,
    Q_ATTRIB_DISABLE,
    Q_DRAW_INDEXED,
    Q_DRAW_RANGE,
    Q_DRAW_ARRAYS,
    Q_CLEAR,
    Q_BIND_FBO,
    Q_FBO_RENDERBUFFER,
    Q_FBO_TEXTURE,
    Q_SCISSOR,
    Q_ENABLE,
    Q_DISABLE,
    Q_STENCIL_MASK,
    Q_STENCIL_MASK_SEPARATE,
    Q_CLEAR_STENCIL,
    Q_BLIT,
    Q_RB_GONE,   // what a pending stencil clear applies to is being replaced
    Q_PAYLOAD,   // and up: an array argument copied into the ring, see payloads[]
};

// State setters with only scalar arguments.
static const char *const async_names[] = {
    "_glActiveTexture", "_glBindTexture", "_glBindProgramARB", "_glBeginQuery", "_glEndQuery", "_glFramebufferRenderbuffer",
    "_glFramebufferTexture2D", "_glBindFramebuffer", "_glBindRenderbuffer", "_glEnable", "_glDisable",
    "_glColorMask", "_glColorMaskIndexedEXT", "_glVertexAttribDivisorARB", "_glVertexAttribDivisor", "_glScissor", "_glViewport",
    "_glDepthMask", "_glDepthFunc", "_glDepthRange", "_glStencilFunc", "_glStencilFuncSeparate", "_glStencilOp", "_glStencilOpSeparate",
    "_glStencilMask", "_glStencilMaskSeparate", "_glBlendFunc", "_glBlendFuncSeparate", "_glBlendEquation", "_glBlendEquationSeparate",
    "_glBlendColor", "_glAlphaFunc", "_glCullFace", "_glFrontFace", "_glPolygonMode", "_glPolygonOffset", "_glClear", "_glClearColor",
    "_glClearDepth", "_glClearStencil", "_glLoadIdentity", "_glMatrixMode", "_glPushMatrix", "_glPopMatrix", "_glTexParameterf",
    "_glTexParameteri", "_glSamplerParameteri", "_glSamplerParameterf", "_glBindSampler", "_glUseProgram", "_glDrawBuffer", "_glReadBuffer",
    "_glFlush", "_glBlitFramebuffer", "_glMateriali", "_glMaterialf", "_glColorMaterial", "_glTexEnvf", "_glTexEnvi", "_glPixelStorei", NULL,
};
// Arrays read during the call: the argument holding the pointer, the argument holding a count (or -1 for one), and the bytes per count.
static const struct {
    const char *name;
    int ptr_arg, count_arg;
    u32 unit;
} payloads[] = {
    {"_glProgramLocalParameters4fvEXT", 3, 2, 16},
    {"_glProgramEnvParameters4fvEXT", 3, 2, 16},
    {"_glProgramLocalParameter4fvARB", 2, -1, 16},
    {"_glProgramEnvParameter4fvARB", 2, -1, 16},
    {"_glClipPlane", 1, -1, 32},
    {"_glMaterialfv", 2, -1, 16},
};
#define N_PAYLOADS (sizeof payloads / sizeof payloads[0])

u8 rt_glq_kind(const char *name) {
    if (!name) return Q_NONE;
    if (!strncmp(name, "_CGL", 4)) {
        if (!strcmp(name, "_CGLLockContext")) return Q_LOCK;
        if (!strcmp(name, "_CGLUnlockContext")) return Q_UNLOCK;
        if (!strcmp(name, "_CGLSetCurrentContext")) return Q_SETCTX;
        if (!strcmp(name, "_CGLFlushDrawable")) return Q_FLUSH;
        return Q_SYNC;
    }
    if (strncmp(name, "_gl", 3)) return Q_NONE;
    static const struct { const char *name; u8 kind; } tracked[] = {
        {"_glClear", Q_CLEAR}, {"_glBindFramebuffer", Q_BIND_FBO}, {"_glFramebufferRenderbuffer", Q_FBO_RENDERBUFFER}, {"_glFramebufferTexture2D", Q_FBO_TEXTURE},
        {"_glScissor", Q_SCISSOR}, {"_glEnable", Q_ENABLE}, {"_glDisable", Q_DISABLE}, {"_glStencilMask", Q_STENCIL_MASK}, {"_glStencilMaskSeparate", Q_STENCIL_MASK_SEPARATE},
        {"_glClearStencil", Q_CLEAR_STENCIL}, {"_glBlitFramebuffer", Q_BLIT}, {"_glDeleteRenderbuffers", Q_RB_GONE}, {"_glRenderbufferStorage", Q_RB_GONE},
        {"_glRenderbufferStorageMultisample", Q_RB_GONE}, {"_glDeleteFramebuffers", Q_RB_GONE},
    };
    for (unsigned i = 0; i < sizeof tracked / sizeof tracked[0]; i++)
        if (!strcmp(tracked[i].name, name)) return tracked[i].kind;
    for (const char *const *q = async_names; *q; q++)
        if (!strcmp(*q, name)) return Q_ASYNC;
    for (unsigned i = 0; i < N_PAYLOADS; i++)
        if (!strcmp(payloads[i].name, name)) return (u8)(Q_PAYLOAD + i);
    if (!strcmp(name, "_glBindBuffer")) return Q_BIND_BUFFER;
    if (!strcmp(name, "_glBindVertexArray")) return Q_BIND_VAO;
    if (!strcmp(name, "_glVertexAttribPointer")) return Q_ATTRIB_POINTER;
    if (!strcmp(name, "_glEnableClientState")) return Q_CLIENT_ENABLE;
    if (!strcmp(name, "_glDisableClientState")) return Q_CLIENT_DISABLE;
    if (!strcmp(name, "_glEnableVertexAttribArray")) return Q_ATTRIB_ENABLE;
    if (!strcmp(name, "_glDisableVertexAttribArray")) return Q_ATTRIB_DISABLE;
    if (!strcmp(name, "_glDrawArrays")) return Q_DRAW_ARRAYS;
    if (!strcmp(name, "_glDrawRangeElements") || !strcmp(name, "_glDrawRangeElementsBaseVertex")) return Q_DRAW_RANGE;
    if (!strcmp(name, "_glDrawElements") || !strcmp(name, "_glDrawElementsBaseVertex") || !strcmp(name, "_glDrawElementsInstanced") ||
        !strcmp(name, "_glDrawElementsInstancedBaseVertex") || !strcmp(name, "_glDrawArraysInstanced"))
        return Q_DRAW_INDEXED;
    return Q_SYNC;
}

#define RING_SIZE (16u << 20)
#define ARG_WORDS 16
typedef struct {
    shim_fn fn;  // NULL: skip to the start of the ring
    u32 size;
    u8 gated, setctx;
    u8 keep;  // its payload stays in use by the next command, so the space isn't handed back yet
    u8 pad;
    const char *name;
    u32 args[ARG_WORDS];
} Cmd;

static u8 *ring;
static u32 ring_guest;
// Only the render thread writes q_head and only the worker writes q_tail. They count bytes and never wrap.
static _Alignas(128) u64 q_head;
static _Alignas(128) u64 q_tail;
static _Alignas(128) u32 worker_asleep;
static _Alignas(128) u32 foreign_waiters;  // other threads that want the context's real lock
static u32 foreign_dirty;                  // they may have changed bindings behind the worker's back
static u64 last_flush_end;
static pthread_mutex_t sleep_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sleep_cv = PTHREAD_COND_INITIALIZER;
static pthread_t glq_client;  // the thread whose calls go through the ring: the last one to present a frame
static int glq_off = -1;
static int trace;
static void *adopt_ctx;
static int time_cmds;
void rt_hitch_note_call(const char *name, u64 ticks);
void rt_pipe_note_slow_draw(const char *name, u64 ticks);
extern int rt_pipe_capturing;
extern int rt_pipe_frame_map;
void rt_pipe_frame_note(const char *name, const u32 *args);
void rt_pipe_capture_program(const u32 *args);
// Real context locks this thread holds, and how many of them announced it to the worker. A thread can lock before the worker exists
// and unlock after, or lock as the render thread (nothing real happens) and unlock after another thread has taken over presenting.
static __thread u32 tl_locks, tl_announced;

// What the render thread has told OpenGL, to see whether a draw would read guest memory after the call returns.
static u32 s_array, s_element, s_last_ctx;
static int s_array_known, s_element_known;
static u32 s_client_attribs, s_enabled_attribs;
// Fixed-function arrays (glVertexPointer and friends, as the movie player uses). A draw waits for the worker while any is on.
static u32 s_client_states;
// Attributes pointing at guest memory. The game draws small quads from its stack, so a draw copies the vertices into the ring.
typedef struct {
    u32 size, type, norm, stride, ptr;
    shim_fn f;
    u8 gated;
} Attr;
static Attr s_attr[32];
static u64 wait_ticks;

static inline void spin_pause(u32 i) {
    if (i < 4000) __asm__ volatile("isb");
    else sched_yield();
}
static inline u64 tail_now(void) { return __atomic_load_n(&q_tail, __ATOMIC_ACQUIRE); }

// ---------- worker ----------
static void wake_worker(void) {
    if (__atomic_load_n(&worker_asleep, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&sleep_mu);
        pthread_cond_signal(&sleep_cv);
        pthread_mutex_unlock(&sleep_mu);
    }
}
static void *worker_main(void *arg) {
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_setname_np("gl worker");
    CPU *w = rt_new_cpu(1u << 20);
    u64 tail = 0;
    CGLContextObj held = NULL;
    for (;;) {
        u64 head = __atomic_load_n(&q_head, __ATOMIC_SEQ_CST);
        if (head == tail) {
            if (held) {
                CGLUnlockContext(held);
                held = NULL;
            }
            for (u32 i = 0; i < 2000 && __atomic_load_n(&q_head, __ATOMIC_RELAXED) == tail; i++) __asm__ volatile("isb");
            if (__atomic_load_n(&q_head, __ATOMIC_RELAXED) != tail) continue;
            pthread_mutex_lock(&sleep_mu);
            __atomic_store_n(&worker_asleep, 1, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(&q_head, __ATOMIC_SEQ_CST) == tail) pthread_cond_wait(&sleep_cv, &sleep_mu);
            __atomic_store_n(&worker_asleep, 0, __ATOMIC_SEQ_CST);
            pthread_mutex_unlock(&sleep_mu);
            continue;
        }
        if (__atomic_load_n(&foreign_waiters, __ATOMIC_SEQ_CST)) {
            if (held) {
                CGLUnlockContext(held);
                held = NULL;
            }
            for (u32 i = 0; __atomic_load_n(&foreign_waiters, __ATOMIC_SEQ_CST); i++) spin_pause(i);
        }
        if (__atomic_exchange_n(&foreign_dirty, 0, __ATOMIC_SEQ_CST)) rt_gl_context_changed();
        while (tail != head) {
            u32 off = (u32)(tail % RING_SIZE);
            Cmd *cmd = (Cmd *)(ring + off);
            if (!cmd->fn) {
                tail += cmd->size;
            } else {
                if (cmd->setctx && held) {
                    CGLUnlockContext(held);
                    held = NULL;
                }
                if (!held && !cmd->setctx) {
                    held = CGLGetCurrentContext();
                    if (held) CGLLockContext(held);
                }
                w->esp = ring_guest + off + (u32)offsetof(Cmd, args) - 4u;
                if (__builtin_expect(rt_pipe_capturing, 0) && cmd->name && !strcmp(cmd->name, "_glProgramStringARB")) rt_pipe_capture_program(cmd->args);
                u64 t0 = time_cmds ? mach_absolute_time() : 0;
                if (cmd->gated) rt_gate_acquire();
                cmd->fn(w);
                if (cmd->gated) rt_gate_release();
                if (__builtin_expect(rt_pipe_frame_map != 0, 0)) rt_pipe_frame_note(cmd->name, cmd->args);
                if (time_cmds) {
                    u64 took = mach_absolute_time() - t0;
                    rt_hitch_note_call(cmd->name, took);
                    if (took >= 24000 && cmd->name && !strncmp(cmd->name, "_glDraw", 7)) rt_pipe_note_slow_draw(cmd->name, took);
                }
                // The caller of a synchronous command reads these once it sees the tail move past it.
                __atomic_store_n(&cmd->args[0], w->eax, __ATOMIC_RELAXED);
                __atomic_store_n(&cmd->args[1], w->edx, __ATOMIC_RELAXED);
                tail += cmd->size;
            }
            if (!cmd->fn || !cmd->keep) __atomic_store_n(&q_tail, tail, __ATOMIC_RELEASE);
            if (__atomic_load_n(&foreign_waiters, __ATOMIC_RELAXED)) break;
        }
    }
    return NULL;
}
static void adopt_cmd(CPU *c) {
    (void)c;
    if (adopt_ctx) CGLSetCurrentContext(adopt_ctx);
    rt_gl_context_changed();
}

// ---------- render thread ----------
static inline Cmd *reserve(u32 size) {
    u64 head = q_head;
    u32 off = (u32)(head % RING_SIZE);
    u32 gap = off + size > RING_SIZE ? RING_SIZE - off : 0;
    u64 need = head + gap + size - RING_SIZE;
    if ((i64)(tail_now() - need) < 0) {
        wake_worker();
        for (u32 i = 0; (i64)(tail_now() - need) < 0; i++) spin_pause(i);
    }
    if (gap) {
        Cmd *skip = (Cmd *)(ring + off);
        skip->fn = NULL;
        skip->size = gap;
        head += gap;
        __atomic_store_n(&q_head, head, __ATOMIC_SEQ_CST);
    }
    return (Cmd *)(ring + (u32)(head % RING_SIZE));
}
static inline void commit(u32 size) {
    __atomic_store_n(&q_head, q_head + size, __ATOMIC_SEQ_CST);
    wake_worker();
}
static void drain_to(u64 pos) {
    wake_worker();
    for (u32 i = 0; (i64)(tail_now() - pos) < 0; i++) spin_pause(i);
}
static const char *cur_name;  // of the command being queued, for timing
static inline Cmd *fill(CPU *c, shim_fn f, int gated, u32 extra) {
    u32 size = (u32)((sizeof(Cmd) + extra + 15u) & ~15u);
    Cmd *cmd = reserve(size);
    cmd->fn = f;
    cmd->name = cur_name;
    cmd->size = size;
    cmd->gated = (u8)gated;
    cmd->setctx = 0;
    cmd->keep = 0;
    memcpy(cmd->args, GP(c->esp + 4), sizeof cmd->args);
    return cmd;
}
// ARKHAM_TRACE_GLQ: what forces the render thread to wait.
static struct { const char *name; u64 n; } sync_names[256];
static u64 n_async;
static void note_sync(const char *name) {
    static u64 last, total;
    total++;
    for (int i = 0; i < 256; i++) {
        if (sync_names[i].name == name || !sync_names[i].name) {
            sync_names[i].name = name;
            sync_names[i].n++;
            break;
        }
    }
    u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000000;
    if (now - last < 5) return;
    last = now;
    fprintf(stderr, "[glq] per second: %llu asynchronous, %llu synchronous, waited %.0f ms:", n_async / 5, total / 5,
            (double)wait_ticks * 125.0 / 3.0 / 1e6 / 5.0);
    wait_ticks = 0;
    for (int i = 0; i < 256 && sync_names[i].name; i++) {
        if (sync_names[i].n >= 5) fprintf(stderr, " %s:%llu", sync_names[i].name, sync_names[i].n / 5);
        sync_names[i].n = 0;
    }
    fprintf(stderr, "\n");
    n_async = 0;
    total = 0;
}
static void run_sync(CPU *c, shim_fn f, int gated, const char *name) {
    if (trace) note_sync(name);
    Cmd *cmd = fill(c, f, gated, 0);
    u32 size = cmd->size;
    commit(size);
    u64 pos = q_head;
    u64 t0 = trace ? mach_absolute_time() : 0;
    for (u32 i = 0; (i64)(tail_now() - pos) < 0; i++) spin_pause(i);
    if (trace) wait_ticks += mach_absolute_time() - t0;
    c->eax = __atomic_load_n(&cmd->args[0], __ATOMIC_RELAXED);
    c->edx = __atomic_load_n(&cmd->args[1], __ATOMIC_RELAXED);
}
static inline void run_async(CPU *c, shim_fn f, int gated) {
    Cmd *cmd = fill(c, f, gated, 0);
    commit(cmd->size);
    if (trace) n_async++;
}
// A clear of the whole stencil buffer in the middle of a render pass makes the GPU load and store every tile of the target, about 1.6 ms at 4x
// on a multisampled 2560x1600 target. As the first thing in a pass it costs 0.05 ms. The game clears stencil at the end of each shadowed light's
// pass, seven times a frame in the first level, and the next thing that happens is usually a change of target. So a stencil clear waits here
// until a draw is going to read stencil, which is nearly always the first draw of a pass, or is dropped if the buffer gets cleared anyway.
// ARKHAM_STENCIL_NOW=1 turns this off.
#define TRACKED_FBOS 64
static int stencil_now = -1;
static u32 s_draw_fbo, s_read_fbo;
static u32 s_stencil_rb[TRACKED_FBOS];  // the renderbuffer holding each framebuffer's stencil, or 0
static int s_scissor_on, s_stencil_on;
static u32 s_scissor[4], s_stencil_mask = 0xFFFFFFFFu, s_clear_stencil;
static struct {
    int on;
    u32 fbo, rb;
    int scissor_on;
    u32 scissor[4], mask, value;
} pending;
static u64 n_clear_moved, n_clear_dropped, n_clear_forced;

static void emit(const char *sym, u32 a0, u32 a1, u32 a2, u32 a3) {
    shim_fn f = rt_lookup_shim(sym);
    if (!f) return;
    u32 size = (u32)((sizeof(Cmd) + 15u) & ~15u);
    Cmd *cmd = reserve(size);
    cmd->fn = f;
    cmd->name = sym;
    cmd->size = size;
    cmd->gated = 0;
    cmd->setctx = 0;
    cmd->keep = 0;
    cmd->args[0] = a0;
    cmd->args[1] = a1;
    cmd->args[2] = a2;
    cmd->args[3] = a3;
    commit(size);
}
static void set_scissor_on(int on) { emit(on ? "_glEnable" : "_glDisable", 0x0C11u, 0, 0, 0); }
static void flush_stencil_clear(void) {
    if (!pending.on) return;
    pending.on = 0;
    // Whichever framebuffer holds the buffer now will do, and normally that's the one bound.
    u32 fbo = s_draw_fbo < TRACKED_FBOS && s_stencil_rb[s_draw_fbo] == pending.rb ? s_draw_fbo : pending.fbo;
    int rebind = fbo != s_draw_fbo, reattach = fbo < TRACKED_FBOS && s_stencil_rb[fbo] != pending.rb;
    if (rebind) emit("_glBindFramebuffer", 0x8CA9u, fbo, 0, 0);
    if (reattach) emit("_glFramebufferRenderbuffer", 0x8CA9u, 0x821Au, 0x8D41u, pending.rb);
    int other_scissor = pending.scissor_on != s_scissor_on;
    int other_box = pending.scissor_on && memcmp(pending.scissor, s_scissor, sizeof s_scissor);
    if (other_scissor) set_scissor_on(pending.scissor_on);
    if (other_box) emit("_glScissor", pending.scissor[0], pending.scissor[1], pending.scissor[2], pending.scissor[3]);
    if (pending.mask != s_stencil_mask) emit("_glStencilMaskSeparate", 0x0404u, pending.mask, 0, 0);
    if (pending.value != s_clear_stencil) emit("_glClearStencil", pending.value, 0, 0, 0);
    emit("_glClear", 0x400u, 0, 0, 0);
    if (pending.value != s_clear_stencil) emit("_glClearStencil", s_clear_stencil, 0, 0, 0);
    if (pending.mask != s_stencil_mask) emit("_glStencilMaskSeparate", 0x0404u, s_stencil_mask, 0, 0);
    if (other_box) emit("_glScissor", s_scissor[0], s_scissor[1], s_scissor[2], s_scissor[3]);
    if (other_scissor) set_scissor_on(s_scissor_on);
    if (reattach) emit("_glFramebufferRenderbuffer", 0x8CA9u, 0x821Au, 0x8D41u, s_stencil_rb[fbo]);
    if (rebind) emit("_glBindFramebuffer", 0x8CA9u, s_draw_fbo, 0, 0);
}
static inline void before_draw(void) {
    if (__builtin_expect(pending.on, 0) && s_stencil_on && s_draw_fbo < TRACKED_FBOS && s_stencil_rb[s_draw_fbo] == pending.rb) {
        n_clear_moved++;
        flush_stencil_clear();
    }
}
// Returns 1 if the clear was dealt with here.
static int stencil_clear_route(u32 mask) {
    if (stencil_now < 0) stencil_now = getenv("ARKHAM_STENCIL_NOW") != NULL;
    if (!(mask & 0x400u)) return 0;
    u32 rb = s_draw_fbo < TRACKED_FBOS ? s_stencil_rb[s_draw_fbo] : 0;
    if (pending.on && pending.rb == rb && !s_scissor_on && (s_stencil_mask & 0xFFu) == 0xFFu) {
        // This clear covers everything the waiting one would have.
        pending.on = 0;
        n_clear_dropped++;
    } else if (pending.on) {
        n_clear_forced++;
        flush_stencil_clear();
    }
    if (mask != 0x400u || stencil_now || !rb || rb == 0xFFFFFFFFu) return 0;
    pending.on = 1;
    pending.fbo = s_draw_fbo;
    pending.rb = rb;
    pending.scissor_on = s_scissor_on;
    memcpy(pending.scissor, s_scissor, sizeof s_scissor);
    pending.mask = s_stencil_mask;
    pending.value = s_clear_stencil;
    return 1;
}
static inline u32 fbo_of_target(u32 target) { return target == 0x8CA8u ? s_read_fbo : s_draw_fbo; }
static void stencil_attached(u32 target, u32 attachment, u32 what) {
    if (attachment != 0x821Au && attachment != 0x8D20u) return;
    u32 fbo = fbo_of_target(target);
    // Attaching a different buffer to the framebuffer that was going to do the clear: it has to happen first.
    if (pending.on && fbo == pending.fbo && what && what != pending.rb) {
        n_clear_forced++;
        flush_stencil_clear();
    }
    if (fbo < TRACKED_FBOS) s_stencil_rb[fbo] = what;
}

// What was set before the worker took over counts too: the movie player sets its vertex arrays once, ahead of the first frame.
static void read_tracked_state(void);
static void read_context_state(void) {
    read_tracked_state();
    GLint v = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &v);
    s_array = (u32)v;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &v);
    s_element = (u32)v;
    s_array_known = s_element_known = 1;
    static const GLenum states[] = {GL_VERTEX_ARRAY, GL_NORMAL_ARRAY, GL_COLOR_ARRAY, GL_INDEX_ARRAY, GL_TEXTURE_COORD_ARRAY,
                                    GL_EDGE_FLAG_ARRAY, GL_SECONDARY_COLOR_ARRAY, GL_FOG_COORD_ARRAY};
    s_client_states = 0;
    for (unsigned i = 0; i < sizeof states / sizeof states[0]; i++)
        if (glIsEnabled(states[i])) s_client_states |= 1u << (states[i] & 31);
    GLint count = 16;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &count);
    s_client_attribs = s_enabled_attribs = 0;
    shim_fn attrib_shim = rt_lookup_shim("_glVertexAttribPointer");
    for (GLint i = 0; i < count && i < 32; i++) {
        GLint on = 0, buffer = 0, size = 0, type = 0, norm = 0, stride = 0;
        void *ptr = NULL;
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &on);
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &norm);
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribPointerv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
        if (on) s_enabled_attribs |= 1u << i;
        if (!buffer && ptr) {
            s_client_attribs |= 1u << i;
            s_attr[i] = (Attr){(u32)size, (u32)type, (u32)norm, (u32)stride, (u32)((u8 *)ptr - rt_mem), attrib_shim, 0};
        }
    }
}
static void read_tracked_state(void) {
    GLint v = 0, box[4] = {0};
    glGetIntegerv(0x8CA6, &v);  // GL_DRAW_FRAMEBUFFER_BINDING
    s_draw_fbo = (u32)v;
    glGetIntegerv(0x8CAA, &v);  // GL_READ_FRAMEBUFFER_BINDING
    s_read_fbo = (u32)v;
    s_scissor_on = glIsEnabled(GL_SCISSOR_TEST);
    s_stencil_on = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    memcpy(s_scissor, box, sizeof box);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &v);
    s_stencil_mask = (u32)v;
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &v);
    s_clear_stencil = (u32)v;
    memset(s_stencil_rb, 0, sizeof s_stencil_rb);
    pending.on = 0;
}
// The context moves to the worker the first time a thread presents a frame, and if another thread takes over presenting.
static void adopt(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    void start_worker(void);
    pthread_once(&once, start_worker);
    if (glq_client) flush_stencil_clear();
    drain_to(q_head);
    adopt_ctx = CGLGetCurrentContext();
    // The usual pattern is lock, present, unlock, and from here on this thread's unlock will not reach the real lock.
    for (; tl_locks; tl_locks--) CGLUnlockContext(adopt_ctx);
    if (tl_announced) __atomic_sub_fetch(&foreign_waiters, tl_announced, __ATOMIC_SEQ_CST);
    tl_announced = 0;
    s_array_known = s_element_known = 0;
    s_client_attribs = 0;
    s_enabled_attribs = ~0u;
    s_client_states = 0;
    if (adopt_ctx) {
        read_context_state();
        CGLSetCurrentContext(NULL);
    }
    rt_gl_context_changed();
    glq_client = pthread_self();
    s_last_ctx = 0xFFFFFFFFu;
    u32 size = (u32)sizeof(Cmd);
    Cmd *cmd = reserve(size);
    cmd->fn = adopt_cmd;
    cmd->name = "adopt";
    cmd->size = size;
    cmd->gated = 0;
    cmd->setctx = 1;
    cmd->keep = 0;
    commit(size);
    drain_to(q_head);
    if (trace) fprintf(stderr, "[glq] the worker now runs OpenGL for this thread\n");
}
void start_worker(void) {
    ring_guest = rt_guest_alloc_pages(RING_SIZE);
    ring = rt_mem + ring_guest;
    pthread_t t;
    pthread_create(&t, NULL, worker_main, NULL);
    pthread_detach(t);
}

static inline u32 type_size(u32 type) {
    switch (type) {
        case 0x1400: case 0x1401: return 1;           // byte, unsigned byte
        case 0x1402: case 0x1403: case 0x140B: return 2;  // short, unsigned short, half float
        case 0x1404: case 0x1405: case 0x1406: return 4;  // int, unsigned int, float
        default: return 0;
    }
}
// Puts the vertices of every attribute in guest memory into the ring. Returns 0 if this draw has to wait for the worker instead.
static int queue_guest_attribs(u32 max_vertex) {
    u32 mask = s_client_attribs & s_enabled_attribs;
    if (!mask) return 1;
    if (!s_array_known || s_array) return 0;
    for (u32 m = mask; m; m &= m - 1) {
        const Attr *a = &s_attr[__builtin_ctz(m)];
        u32 ts = type_size(a->type), comps = a->size == 0x80E1u ? 4 : a->size;
        if (!ts || (u64)(max_vertex + 1ull) * (a->stride ? a->stride : comps * ts) > (1u << 18)) return 0;
    }
    for (u32 m = mask; m; m &= m - 1) {
        u32 i = (u32)__builtin_ctz(m);
        const Attr *a = &s_attr[i];
        u32 elem = (a->size == 0x80E1u ? 4 : a->size) * type_size(a->type);
        u32 bytes = max_vertex * (a->stride ? a->stride : elem) + elem;
        u32 size = (u32)((sizeof(Cmd) + bytes + 15u) & ~15u);
        Cmd *cmd = reserve(size);
        cmd->fn = a->f;
        cmd->name = "_glVertexAttribPointer";
        cmd->size = size;
        cmd->gated = a->gated;
        cmd->setctx = 0;
        cmd->keep = 1;
            memcpy(cmd + 1, GP(a->ptr), bytes);
        cmd->args[0] = i;
        cmd->args[1] = a->size;
        cmd->args[2] = a->type;
        cmd->args[3] = a->norm;
        cmd->args[4] = a->stride;
        cmd->args[5] = ring_guest + (u32)((u8 *)(cmd + 1) - ring);
        commit(size);
    }
    return 1;
}
// Called for every OpenGL import. Returns 1 when the call has been dealt with.
int rt_glq_route(CPU *c, void (*f)(CPU *), u8 kind, int gated, const char *name) {
    if (__builtin_expect(glq_off != 0, 0)) {
        if (glq_off < 0) {
            // In one-at-a-time mode the render thread would hold the gate the worker needs.
            glq_off = getenv("ARKHAM_GL_DIRECT") != NULL || getenv("ARKHAM_SERIAL") != NULL;
            trace = getenv("ARKHAM_TRACE_GLQ") != NULL;
            time_cmds = getenv("ARKHAM_TRACE_HITCH") != NULL;
        }
        if (glq_off) return 0;
    }
    if (!pthread_equal(pthread_self(), glq_client)) {
        if (kind == Q_FLUSH) {
            adopt();
        } else {
            // Another thread touching a context the worker may own: let the worker finish and step aside until it unlocks.
            if (kind == Q_LOCK) {
                tl_locks++;
                if (glq_client) {
                    drain_to(__atomic_load_n(&q_head, __ATOMIC_SEQ_CST));
                    tl_announced++;
                    __atomic_add_fetch(&foreign_waiters, 1, __ATOMIC_SEQ_CST);
                    __atomic_store_n(&foreign_dirty, 1, __ATOMIC_SEQ_CST);
                    rt_gl_context_changed();
                }
            } else if (kind == Q_UNLOCK) {
                if (!tl_locks) {
                    c->eax = 0;
                    return 1;
                }
                tl_locks--;
                if (tl_announced) {
                    tl_announced--;
                    __atomic_sub_fetch(&foreign_waiters, 1, __ATOMIC_SEQ_CST);
                }
            }
            return 0;
        }
    }
    u32 a0 = G32(c->esp + 4);
    cur_name = name;
    // ARKHAM_GLQ_SYNC=all or a list of names: run those synchronously, to find out which one misbehaves in the ring.
    static const char *sync_list = (const char *)1;
    if (sync_list == (const char *)1) sync_list = getenv("ARKHAM_GLQ_SYNC");
    if (sync_list && kind != Q_LOCK && kind != Q_UNLOCK && (!strcmp(sync_list, "all") || strstr(sync_list, name))) {
        c->eax = 0;
        run_sync(c, f, gated, name);
        return 1;
    }
    switch (kind) {
        case Q_LOCK:
        case Q_UNLOCK:
            c->eax = 0;
            return 1;
        case Q_SETCTX:
            c->eax = 0;
            // Feral's layer lets go of the context and takes it back about 2500 times a frame. Each time would mean giving up the context's lock
            // and forgetting the bindings on the worker, for nothing: no other thread is waiting for it. ARKHAM_CTX_RELEASE=1 passes these on.
            {
                static int release = -1;
                if (release < 0) release = getenv("ARKHAM_CTX_RELEASE") != NULL;
                if (!a0 && !release) return 1;
            }
            if (a0 == s_last_ctx) return 1;
            s_last_ctx = a0;
            {
                Cmd *cmd = fill(c, f, gated, 0);
                cmd->setctx = 1;
                commit(cmd->size);
            }
            return 1;
        case Q_FLUSH:
            // At most one frame in flight, so the picture stays close to the input.
            {
                u64 t0 = trace ? mach_absolute_time() : 0;
                drain_to(last_flush_end);
                if (trace) wait_ticks += mach_absolute_time() - t0;
            }
            c->eax = 0;
            run_async(c, f, gated);
            last_flush_end = q_head;
            return 1;
        case Q_ASYNC:
            run_async(c, f, gated);
            return 1;
        case Q_CLEAR:
            if (stencil_clear_route(a0)) return 1;
            run_async(c, f, gated);
            return 1;
        case Q_BIND_FBO: {
            u32 id = G32(c->esp + 8);
            if (a0 != 0x8CA8u) s_draw_fbo = id;
            if (a0 != 0x8CA9u) s_read_fbo = id;
            run_async(c, f, gated);
            return 1;
        }
        case Q_FBO_RENDERBUFFER:
            stencil_attached(a0, G32(c->esp + 8), G32(c->esp + 16));
            run_async(c, f, gated);
            return 1;
        case Q_FBO_TEXTURE:
            // Stencil in a texture can be read without a draw, so its clears aren't delayed.
            stencil_attached(a0, G32(c->esp + 8), G32(c->esp + 16) ? 0xFFFFFFFFu : 0);
            run_async(c, f, gated);
            return 1;
        case Q_SCISSOR:
            memcpy(s_scissor, GP(c->esp + 4), sizeof s_scissor);
            run_async(c, f, gated);
            return 1;
        case Q_ENABLE:
        case Q_DISABLE:
            if (a0 == 0x0C11u) s_scissor_on = kind == Q_ENABLE;
            else if (a0 == 0x0B90u) s_stencil_on = kind == Q_ENABLE;
            run_async(c, f, gated);
            return 1;
        case Q_STENCIL_MASK:
            s_stencil_mask = a0;
            run_async(c, f, gated);
            return 1;
        case Q_STENCIL_MASK_SEPARATE:
            if (a0 != 0x0405u) s_stencil_mask = G32(c->esp + 8);  // glClear uses the front face's mask
            run_async(c, f, gated);
            return 1;
        case Q_CLEAR_STENCIL:
            s_clear_stencil = a0;
            run_async(c, f, gated);
            return 1;
        case Q_BLIT:
            if (G32(c->esp + 4 + 32) & 0x400u) flush_stencil_clear();  // the mask: it copies stencil
            run_async(c, f, gated);
            return 1;
        case Q_RB_GONE:
            // The buffer's contents go away with it, but telling that apart isn't worth it for something this rare.
            flush_stencil_clear();
            run_sync(c, f, gated, name);
            return 1;
        case Q_BIND_BUFFER: {
            u32 target = a0, buf = G32(c->esp + 8);
            if (target == 0x8892u) {
                s_array = buf;
                s_array_known = 1;
            } else if (target == 0x8893u) {
                s_element = buf;
                s_element_known = 1;
            }
            run_async(c, f, gated);
            return 1;
        }
        case Q_BIND_VAO:
            s_element_known = 0;
            run_sync(c, f, gated, name);
            return 1;
        case Q_ATTRIB_POINTER:
            if (a0 < 32) {
                // With no buffer bound the pointer is guest memory, which a later draw reads. A null one reads nothing.
                int from_buffer = s_array_known && s_array;
                if (from_buffer || !G32(c->esp + 4 + 20)) s_client_attribs &= ~(1u << a0);
                else s_client_attribs |= 1u << a0;
                if (!from_buffer) {
                    s_attr[a0] = (Attr){G32(c->esp + 8), G32(c->esp + 12), G32(c->esp + 16), G32(c->esp + 20), G32(c->esp + 24), f, (u8)gated};
                }
            }
            run_async(c, f, gated);
            return 1;
        case Q_CLIENT_ENABLE:
            s_client_states |= 1u << (a0 & 31);
            run_sync(c, f, gated, name);
            return 1;
        case Q_CLIENT_DISABLE:
            s_client_states &= ~(1u << (a0 & 31));
            run_sync(c, f, gated, name);
            return 1;
        case Q_ATTRIB_ENABLE:
            if (a0 < 32) s_enabled_attribs |= 1u << a0;
            run_async(c, f, gated);
            return 1;
        case Q_ATTRIB_DISABLE:
            if (a0 < 32) s_enabled_attribs &= ~(1u << a0);
            run_async(c, f, gated);
            return 1;
        case Q_DRAW_ARRAYS: {
            u32 first = G32(c->esp + 8), count = G32(c->esp + 12);
            before_draw();
            if (count && !s_client_states && queue_guest_attribs(first + count - 1)) run_async(c, f, gated);
            else run_sync(c, f, gated, "draw arrays from guest memory");
            return 1;
        }
        case Q_DRAW_RANGE: {
            // mode, start, end, count, type, indices, and for the BaseVertex form a base vertex added to every index
            u32 end = G32(c->esp + 12), count = G32(c->esp + 16), type = G32(c->esp + 20), indices = G32(c->esp + 24);
            u32 base = !strcmp(name, "_glDrawRangeElementsBaseVertex") ? G32(c->esp + 28) : 0;
            before_draw();
            if (s_client_states || (i32)base < 0 || !s_element_known || !queue_guest_attribs(end + base)) {
                run_sync(c, f, gated, "draw from guest memory");
                return 1;
            }
            if (s_element) {
                run_async(c, f, gated);
                return 1;
            }
            u32 bytes = count * type_size(type);
            if (!bytes || bytes > (1u << 18)) {
                run_sync(c, f, gated, "draw from guest memory");
                return 1;
            }
            Cmd *cmd = fill(c, f, gated, bytes);
            memcpy(cmd + 1, GP(indices), bytes);
            cmd->args[5] = ring_guest + (u32)((u8 *)(cmd + 1) - ring);
            commit(cmd->size);
            if (trace) n_async++;
            return 1;
        }
        case Q_DRAW_INDEXED:
            before_draw();
            if (s_client_states || (s_client_attribs & s_enabled_attribs) || !s_element_known || !s_element) run_sync(c, f, gated, "draw from guest memory");
            else run_async(c, f, gated);
            return 1;
        case Q_SYNC:
            if (__builtin_expect(pending.on, 0) && !strcmp(name, "_glReadPixels")) flush_stencil_clear();
            run_sync(c, f, gated, name);
            return 1;
        default: {
            unsigned p = (unsigned)kind - Q_PAYLOAD;
            u32 count = payloads[p].count_arg >= 0 ? G32(c->esp + 4 + 4u * (u32)payloads[p].count_arg) : 1u;
            u32 bytes = count * payloads[p].unit;
            u32 from = G32(c->esp + 4 + 4u * (u32)payloads[p].ptr_arg);
            Cmd *cmd = fill(c, f, gated, bytes);
            memcpy(cmd + 1, GP(from), bytes);
            cmd->args[payloads[p].ptr_arg] = ring_guest + (u32)((u8 *)(cmd + 1) - ring);
            commit(cmd->size);
            if (trace) n_async++;
            return 1;
        }
    }
}
// Before Objective-C messages that act on an OpenGL context from outside the ring.
void rt_glq_drain(void) {
    if (glq_client) drain_to(__atomic_load_n(&q_head, __ATOMIC_SEQ_CST));
}
void rt_glq_report(void) { fprintf(stderr, "[glq] stencil clears: %llu moved to a draw, %llu covered by a later clear, %llu forced\n", n_clear_moved, n_clear_dropped, n_clear_forced); }
