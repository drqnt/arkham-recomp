// A thread for Feral's Direct3D 9 layer. The render thread spends about half its time in that layer translating Direct3D calls into
// OpenGL, so here the engine's calls go into a ring and this thread runs them, passing OpenGL on to gl_queue.c. The result is a chain:
// game thread, render thread (engine), this thread (Feral's layer), OpenGL worker.
//
// The engine only reaches the layer through COM interfaces, so their vtable slots are replaced with a shim at startup. As in gl_queue.c, a
// method not listed as asynchronous is synchronous: the caller waits until it has run on this thread, so a missing entry costs speed and
// never correctness. Only methods whose result the engine doesn't need, and whose memory arguments are copied, may be asynchronous.
// ARKHAM_D3D_DIRECT=1 turns the whole thing off.
#include "shim.h"

#include <mach/mach_time.h>
#include <os/lock.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void (*shim_fn)(CPU *);
u32 rt_dyn_import_nogate(shim_fn fn, u32 arg);
guest_fn rt_guest_fn(u32 addr);
extern __thread u32 rt_dyn_cur_arg;

enum {
    K_SYNC,
    K_ASYNC,
    K_LOCAL,    // has no state: runs on the calling thread
    K_ARRAY,    // asynchronous, with up to two memory arguments copied into the ring
    K_DRAW_UP,  // DrawPrimitiveUP
    K_DRAW_IUP, // DrawIndexedPrimitiveUP
    K_PRESENT,
    K_REF,            // AddRef and Release: asynchronous, because the count is only ever touched on this thread
    K_RENDER_STATE,   // the rest skip a call that sets what is already set, which most of them do
    K_SAMPLER_STATE,
    K_STREAM_FREQ,
    K_INDICES,
    K_UNSET_STATE,    // synchronous, and changes state behind the engine's back
    K_QUERY_DATA,
    K_SURFACE_LEVEL,
    K_CREATE_TEXTURE,
};

typedef struct {
    u32 orig;
    guest_fn fn;
    int host;     // fn is the Metal renderer's, not a guest function
    int missing;  // a method of a Metal object that nothing implements
    int noted;    // the engine's call to Feral's layer has been logged
    const char *name;
    u8 kind;
    signed char ptr[2];  // argument numbers, this being 0; -1 for none
    signed char count;   // argument with the element count; -1 for one element
    u32 unit[2];
} Method;

#define MAX_METHODS 512
static Method methods[MAX_METHODS];
static u32 n_methods;

// ---------- which methods can wait ----------
static const struct {
    const char *name;
    u8 kind;
    signed char ptr0, ptr1, count;
    u32 unit0, unit1;
} rules[] = {
    {"Device.TestCooperativeLevel", K_LOCAL, -1, -1, -1, 0, 0},
    {"Surface.GetDesc", K_LOCAL, -1, -1, -1, 0, 0},  // reads fields that never change after creation
    {"Device.SetRenderState", K_RENDER_STATE, -1, -1, -1, 0, 0},
    {"Device.SetSamplerState", K_SAMPLER_STATE, -1, -1, -1, 0, 0},
    {"Device.Reset", K_UNSET_STATE, -1, -1, -1, 0, 0},
    {"StateBlock.Apply", K_UNSET_STATE, -1, -1, -1, 0, 0},
    {"Device.SetTextureStageState", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.SetTexture", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.SetVertexShader", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.SetPixelShader", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.SetVertexDeclaration", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.SetFVF", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.SetStreamSource", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.SetStreamSourceFreq", K_STREAM_FREQ, -1, -1, -1, 0, 0},
    {"Device.SetIndices", K_INDICES, -1, -1, -1, 0, 0},
    {"Device.SetRenderTarget", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.SetDepthStencilSurface", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.BeginScene", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.EndScene", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.DrawPrimitive", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.DrawIndexedPrimitive", K_ASYNC, -1, -1, -1, 0, 0},
    {"Device.SetVertexShaderConstantF", K_ARRAY, 2, -1, 3, 16, 0},
    {"Device.SetVertexShaderConstantI", K_ARRAY, 2, -1, 3, 16, 0},
    {"Device.SetVertexShaderConstantB", K_ARRAY, 2, -1, 3, 4, 0},
    {"Device.SetPixelShaderConstantF", K_ARRAY, 2, -1, 3, 16, 0},
    {"Device.SetPixelShaderConstantI", K_ARRAY, 2, -1, 3, 16, 0},
    {"Device.SetPixelShaderConstantB", K_ARRAY, 2, -1, 3, 4, 0},
    {"Device.SetScissorRect", K_ARRAY, 1, -1, -1, 16, 0},
    {"Device.SetViewport", K_ARRAY, 1, -1, -1, 24, 0},
    {"Device.SetClipPlane", K_ARRAY, 2, -1, -1, 16, 0},
    {"Device.SetTransform", K_ARRAY, 2, -1, -1, 64, 0},
    {"Device.Clear", K_ARRAY, 2, -1, 1, 16, 0},
    {"Device.StretchRect", K_ARRAY, 2, 4, -1, 16, 16},
    {"Device.DrawPrimitiveUP", K_DRAW_UP, 3, -1, -1, 0, 0},
    {"Device.DrawIndexedPrimitiveUP", K_DRAW_IUP, 5, 7, -1, 0, 0},
    {"Device.Present", K_PRESENT, 1, 2, -1, 16, 16},
    {"Query.Issue", K_ASYNC, -1, -1, -1, 0, 0},
    {"Query.GetData", K_QUERY_DATA, -1, -1, -1, 0, 0},
    {"Texture.GetSurfaceLevel", K_SURFACE_LEVEL, -1, -1, -1, 0, 0},
    {"Device.CreateTexture", K_CREATE_TEXTURE, -1, -1, -1, 0, 0},
    {"VertexBuffer.Unlock", K_ASYNC, -1, -1, -1, 0, 0},
    {"IndexBuffer.Unlock", K_ASYNC, -1, -1, -1, 0, 0},
    {"Texture.UnlockRect", K_ASYNC, -1, -1, -1, 0, 0},
    {"CubeTexture.UnlockRect", K_ASYNC, -1, -1, -1, 0, 0},
    {"Surface.UnlockRect", K_ASYNC, -1, -1, -1, 0, 0},
};

// Slot names from the Direct3D 9 headers. Every interface here starts with two destructor slots and the three of IUnknown.
static const char *const device_names =
    "TestCooperativeLevel GetAvailableTextureMem EvictManagedResources GetDirect3D GetDeviceCaps GetDisplayMode GetCreationParameters "
    "SetCursorProperties SetCursorPosition ShowCursor CreateAdditionalSwapChain GetSwapChain GetNumberOfSwapChains Reset Present GetBackBuffer "
    "GetRasterStatus SetDialogBoxMode SetGammaRamp GetGammaRamp CreateTexture CreateVolumeTexture CreateCubeTexture CreateVertexBuffer "
    "CreateIndexBuffer CreateRenderTarget CreateDepthStencilSurface UpdateSurface UpdateTexture GetRenderTargetData GetFrontBufferData StretchRect "
    "ColorFill CreateOffscreenPlainSurface SetRenderTarget GetRenderTarget SetDepthStencilSurface GetDepthStencilSurface BeginScene EndScene Clear "
    "SetTransform GetTransform MultiplyTransform SetViewport GetViewport SetMaterial GetMaterial SetLight GetLight LightEnable GetLightEnable "
    "SetClipPlane GetClipPlane SetRenderState GetRenderState CreateStateBlock BeginStateBlock EndStateBlock SetClipStatus GetClipStatus GetTexture "
    "SetTexture GetTextureStageState SetTextureStageState GetSamplerState SetSamplerState ValidateDevice SetPaletteEntries GetPaletteEntries "
    "SetCurrentTexturePalette GetCurrentTexturePalette SetScissorRect GetScissorRect SetSoftwareVertexProcessing GetSoftwareVertexProcessing "
    "SetNPatchMode GetNPatchMode DrawPrimitive DrawIndexedPrimitive DrawPrimitiveUP DrawIndexedPrimitiveUP ProcessVertices CreateVertexDeclaration "
    "SetVertexDeclaration GetVertexDeclaration SetFVF GetFVF CreateVertexShader SetVertexShader GetVertexShader SetVertexShaderConstantF "
    "GetVertexShaderConstantF SetVertexShaderConstantI GetVertexShaderConstantI SetVertexShaderConstantB GetVertexShaderConstantB SetStreamSource "
    "GetStreamSource SetStreamSourceFreq GetStreamSourceFreq SetIndices GetIndices CreatePixelShader SetPixelShader GetPixelShader "
    "SetPixelShaderConstantF GetPixelShaderConstantF SetPixelShaderConstantI GetPixelShaderConstantI SetPixelShaderConstantB GetPixelShaderConstantB "
    "DrawRectPatch DrawTriPatch DeletePatch CreateQuery";
#define RESOURCE "GetDevice SetPrivateData GetPrivateData FreePrivateData SetPriority GetPriority PreLoad GetType "
#define BASE_TEXTURE RESOURCE "SetLOD GetLOD GetLevelCount SetAutoGenFilterType GetAutoGenFilterType GenerateMipSubLevels "
static const struct {
    const char *cls;
    u32 vtable;
    const char *names;
} interfaces[] = {
    {"Device", 0x35ea2c8u, NULL},
    {"Query", 0x35ea798u, "GetDevice GetType GetDataSize Issue GetData"},
    {"Surface", 0x35ea96cu, RESOURCE "GetContainer GetDesc LockRect UnlockRect GetDC ReleaseDC"},
    {"Texture", 0x35eac24u, BASE_TEXTURE "GetLevelDesc GetSurfaceLevel LockRect UnlockRect AddDirtyRect"},
    {"CubeTexture", 0x35e9964u, BASE_TEXTURE "GetLevelDesc GetCubeMapSurface LockRect UnlockRect AddDirtyRect"},
    {"VolumeTexture", 0x35ec7b4u, BASE_TEXTURE "GetLevelDesc GetVolumeLevel LockBox UnlockBox AddDirtyBox"},
    {"Volume", 0x35eb094u, "GetDevice SetPrivateData GetPrivateData FreePrivateData GetContainer GetDesc LockBox UnlockBox"},
    {"IndexBuffer", 0x35ea6fcu, RESOURCE "Lock Unlock GetDesc"},
    {"VertexBuffer", 0x35eaefcu, RESOURCE "Lock Unlock GetDesc"},
    {"VertexDeclaration", 0x35eaf68u, "GetDevice GetDeclaration"},
    {"PixelShader", 0x35ec6d8u, "GetDevice GetFunction"},
    {"VertexShader", 0x35eafe8u, "GetDevice GetFunction"},
    {"StateBlock", 0x35ec8f0u, "GetDevice Capture Apply"},
    {"SwapChain", 0x35ed064u, "Present GetFrontBufferData GetBackBuffer GetRasterStatus GetDisplayMode GetDevice GetPresentParameters"},
};

// ---------- the ring ----------
#define RING_SIZE (32u << 20)
#define ARG_WORDS 16
typedef struct {
    u32 eax, edx;
    u32 done;
} Result;
typedef struct {
    guest_fn fn;  // NULL: skip to the start of the ring
    Result *result;
    u32 size;
    u32 method;
    u32 args[ARG_WORDS];
} Cmd;

static u8 *ring;
static u32 ring_guest;
static _Alignas(128) u64 q_head;
static _Alignas(128) u64 q_tail;
static _Alignas(128) u32 worker_asleep;
static os_unfair_lock q_lock = OS_UNFAIR_LOCK_INIT;  // several threads can call in, though it's nearly always the render thread
static pthread_mutex_t sleep_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sleep_cv = PTHREAD_COND_INITIALIZER;
static CPU *worker_cpu;
static int trace;
static u32 presents_pending;

static int trace_shaders;
static u32 method_create_vs = ~0u, method_create_ps = ~0u;
static int pipe_on;
static u64 pipe_t[6][256];
static u32 pipe_n[6];
void rt_pipe_mark(int stage) {
    if (pipe_on) pipe_t[stage][pipe_n[stage]++ & 255] = mach_absolute_time();
}
static inline void spin_pause(u32 i) {
    if (i < 4000) __asm__ volatile("isb");
    else sched_yield();
}
static inline void wake_worker(void) {
    if (__atomic_load_n(&worker_asleep, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&sleep_mu);
        pthread_cond_signal(&sleep_cv);
        pthread_mutex_unlock(&sleep_mu);
    }
}

static inline void call_on_worker(CPU *w, guest_fn fn, const u32 *args, u32 top) {
    u32 esp = (top - ARG_WORDS * 4u) & ~15u;
    memcpy(GP(esp), args, ARG_WORDS * 4u);
    esp -= 4;
    G32(esp) = RT_HOST_RETURN;
    w->esp = esp;
    fn(w);
}
// ARKHAM_SHADER_EXTRACT=out ARKHAM_SHADER_SOURCES=a;b: runs every shader in the game's shader caches through the layer's translator, once the
// device exists, so that the programs it hands to OpenGL can be collected ahead of play (rt_pipe_capture_program), and exits.
// The layer translates when a shader is created, from its bytecode alone, so what comes out is exactly what the game will ask for later.
void rt_pipe_capture_start(const char *path);
void rt_pipe_capture_stop(void);
void rt_glq_drain(void);
static const Method *method_named(const char *name) {
    for (u32 i = 0; i < n_methods; i++)
        if (!strcmp(methods[i].name, name)) return &methods[i];
    return NULL;
}
static u32 shader_words(const u32 *t, u32 max) {
    u32 i = 1;
    while (i < max) {
        u32 tok = t[i];
        if (tok == 0x0000FFFFu) return i + 1;
        if ((tok & 0xFFFFu) == 0xFFFEu) i += 1 + (tok >> 16);
        else i += 1 + ((tok >> 24) & 0xFu);
    }
    return 0;
}
static void extract_shaders(CPU *w, u32 top, u32 device) {
    const Method *create_ps = method_named("Device.CreatePixelShader"), *create_vs = method_named("Device.CreateVertexShader");
    const Method *release_ps = method_named("PixelShader.slot3"), *release_vs = method_named("VertexShader.slot3");
    const char *sources = getenv("ARKHAM_SHADER_SOURCES");
    if (!create_ps || !create_vs || !release_ps || !release_vs || !sources) {
        fprintf(stderr, "[extract] missing what it needs\n");
        _exit(3);
    }
    rt_pipe_capture_start(getenv("ARKHAM_SHADER_EXTRACT"));
    u32 scratch = rt_guest_alloc_pages(1u << 20), out = scratch + (1u << 20) - 64;
    enum { SEEN = 1 << 18 };
    u64 *seen = calloc(SEEN, sizeof(u64));
    u32 n = 0, dup = 0, failed = 0;
    u64 t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    char *list = strdup(sources), *save = NULL;
    for (char *path = strtok_r(list, ";", &save); path; path = strtok_r(NULL, ";", &save)) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "[extract] can't open a source\n");
            continue;
        }
        fseek(f, 0, SEEK_END);
        size_t size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        u8 *data = malloc(size);
        fread(data, 1, size, f);
        fclose(f);
        for (size_t i = 0; i + 16 < size; i++) {
            if (data[i] != 0x00 || data[i + 1] != 0x03 || data[i + 3] != 0xFF || (data[i + 2] != 0xFF && data[i + 2] != 0xFE)) continue;
            if (data[i + 4] != 0xFE || data[i + 5] != 0xFF || memcmp(data + i + 8, "CTAB", 4)) continue;
            u32 room = (u32)((size - i) / 4), limit = room < (1u << 17) ? room : (1u << 17);
            u8 *copy = GP(scratch);
            memcpy(copy, data + i, (size_t)limit * 4);
            u32 words = shader_words((const u32 *)copy, limit);
            if (!words) {
                failed++;
                continue;
            }
            u64 h = 0xcbf29ce484222325ull;
            for (u32 k = 0; k < words * 4; k++) h = (h ^ copy[k]) * 0x100000001b3ull;
            if (!h) h = 1;
            u32 slot = (u32)(h % SEEN);
            while (seen[slot] && seen[slot] != h) slot = (slot + 1) % SEEN;
            if (seen[slot]) {
                dup++;
                continue;
            }
            seen[slot] = h;
            int pixel = data[i + 2] == 0xFF;
            u32 args[ARG_WORDS] = {device, scratch, out};
            G32(out) = 0;
            call_on_worker(w, (pixel ? create_ps : create_vs)->fn, args, top);
            u32 shader = G32(out);
            if (w->eax || !shader) {
                failed++;
                continue;
            }
            u32 rel[ARG_WORDS] = {shader};
            call_on_worker(w, (pixel ? release_ps : release_vs)->fn, rel, top);
            if (++n % 1000 == 0) fprintf(stderr, "[extract] %u shaders, %.0f s\n", n, (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9);
            i += (size_t)words * 4 - 1;
        }
        free(data);
    }
    rt_glq_drain();
    rt_pipe_capture_stop();
    fprintf(stderr, "[extract] done: %u shaders, %u repeated, %u failed, %.0f s\n", n, dup, failed, (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9);
    fflush(stderr);
    _exit(0);
}
u64 rt_d3dq_busy_ticks;
static void *worker_main(void *arg) {
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_setname_np("d3d worker");
    CPU *w = rt_new_cpu(8u << 20);
    u32 top = w->esp;
    __atomic_store_n(&worker_cpu, w, __ATOMIC_RELEASE);
    u64 tail = 0;
    for (;;) {
        u64 head = __atomic_load_n(&q_head, __ATOMIC_SEQ_CST);
        if (head == tail) {
            for (u32 i = 0; i < 2000 && __atomic_load_n(&q_head, __ATOMIC_RELAXED) == tail; i++) __asm__ volatile("isb");
            if (__atomic_load_n(&q_head, __ATOMIC_RELAXED) != tail) continue;
            pthread_mutex_lock(&sleep_mu);
            __atomic_store_n(&worker_asleep, 1, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(&q_head, __ATOMIC_SEQ_CST) == tail) pthread_cond_wait(&sleep_cv, &sleep_mu);
            __atomic_store_n(&worker_asleep, 0, __ATOMIC_SEQ_CST);
            pthread_mutex_unlock(&sleep_mu);
            continue;
        }
        u64 busy_from = mach_absolute_time();
        while (tail != head) {
            Cmd *cmd = (Cmd *)(ring + (u32)(tail % RING_SIZE));
            if (cmd->fn) {
                call_on_worker(w, cmd->fn, cmd->args, top);
                if (trace_shaders && (cmd->method == method_create_vs || cmd->method == method_create_ps)) {
                    const u32 *t = (const u32 *)GP(cmd->args[1]);
                    u32 words = 0;
                    while (t[words] != 0x0000FFFFu && words < (1u << 20)) words++;
                    fprintf(stderr, "[d3dshader] %s words=%u t=%.3f\n", cmd->method == method_create_vs ? "vs" : "ps", words, (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9);
                }
                if (pipe_on && methods[cmd->method].kind == K_PRESENT) rt_pipe_mark(3);
                if (methods[cmd->method].kind == K_PRESENT && getenv("ARKHAM_SHADER_EXTRACT")) extract_shaders(w, top, cmd->args[0]);
                if (cmd->result) {
                    cmd->result->eax = w->eax;
                    cmd->result->edx = w->edx;
                    __atomic_store_n(&cmd->result->done, 1, __ATOMIC_RELEASE);
                }
            }
            tail += cmd->size;
            __atomic_store_n(&q_tail, tail, __ATOMIC_RELEASE);
        }
        rt_d3dq_busy_ticks += mach_absolute_time() - busy_from;
    }
    return NULL;
}

// ---------- calling in ----------
static inline u32 prim_vertices(u32 type, u32 prims) {
    switch (type) {
        case 1: return prims;
        case 2: return prims * 2;
        case 3: return prims + 1;
        case 4: return prims * 3;
        case 5:
        case 6: return prims + 2;
        default: return 0;
    }
}
static inline u32 arg_of(CPU *c, int i) { return G32(c->esp + 4 + 4u * (u32)i); }

// What the engine last set. It is the only caller, so if a value doesn't change, Direct3D's own rules make the call a no-op.
#define RENDER_STATES 256
#define SAMPLERS 32
#define SAMPLER_TYPES 16
#define STREAMS 16
static struct {
    u32 render[RENDER_STATES];
    u32 sampler[SAMPLERS][SAMPLER_TYPES];
    u32 freq[STREAMS];
    u32 indices;
    u8 render_set[RENDER_STATES], sampler_set[SAMPLERS][SAMPLER_TYPES], freq_set[STREAMS], indices_set;
} shadow;
// A texture's level surfaces last as long as it does, so GetSurfaceLevel only has to go to the layer once per level. The engine asks a
// dozen times a frame for its render targets, and each of those would otherwise wait for the whole queue.
#define LEVEL_CACHE 1024u
static struct {
    u32 texture, level, surface;
} level_cache[LEVEL_CACHE];
static u32 surface_addref_method;
static inline u32 level_slot(u32 texture, u32 level) { return ((texture >> 4) * 2654435761u + level) % LEVEL_CACHE; }
static u64 n_skipped;
// ARKHAM_TRACE_PIPE=1: when each frame reaches each stage, as averages. 0 first occlusion read, 1 last occlusion read done,
// 2 engine queued Present, 3 Direct3D thread finished Present, 4 and 5 around the real flush on the OpenGL thread.
static void pipe_report(void) {
    u32 last = pipe_n[5];
    if (last < 130) return;
    double d[6] = {0}, period = 0;
    int used = 0;
    for (u32 k = last - 100; k < last; k++) {
        u64 t2 = pipe_t[2][k & 255], t3 = pipe_t[3][k & 255], t4 = pipe_t[4][k & 255], t5 = pipe_t[5][k & 255], t2n = pipe_t[2][(k + 1) & 255];
        d[0] += (double)(t3 - t2);
        d[1] += (double)(t4 - t3);
        d[2] += (double)(t5 - t4);
        period += (double)(t2n - t2);
        used++;
    }
    u32 r = pipe_n[0], total = 0;
    double reads = 0;
    for (u32 k = r - 100; k < r; k++) {
        reads += (double)(pipe_t[1][k & 255] - pipe_t[0][k & 255]);
        total++;
    }
    double u = 125.0 / 3.0 / 1e6;
    fprintf(stderr, "[pipe] frame %.2f ms: occlusion reads %.2f, Present queued to done %.2f, done to flush %.2f, flush %.2f\n", period * u / used,
            reads * u / total, d[0] * u / used, d[1] * u / used, d[2] * u / used);
}
static u64 n_async, n_sync, wait_ticks;
static u64 method_sync[MAX_METHODS], method_async[MAX_METHODS];

// Must be called with q_lock held. Room for one command, wrapping if needed.
static inline Cmd *reserve(u32 size) {
    u64 head = q_head;
    u32 off = (u32)(head % RING_SIZE);
    u32 gap = off + size > RING_SIZE ? RING_SIZE - off : 0;
    u64 need = head + gap + size - RING_SIZE;
    if ((i64)(__atomic_load_n(&q_tail, __ATOMIC_ACQUIRE) - need) < 0) {
        wake_worker();
        for (u32 i = 0; (i64)(__atomic_load_n(&q_tail, __ATOMIC_ACQUIRE) - need) < 0; i++) spin_pause(i);
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
static void run_sync(CPU *c, const Method *m, u32 index) {
    Result r = {0, 0, 0};
    os_unfair_lock_lock(&q_lock);
    Cmd *cmd = reserve(sizeof(Cmd));
    cmd->fn = m->fn;
    cmd->result = &r;
    cmd->size = sizeof(Cmd);
    cmd->method = index;
    memcpy(cmd->args, GP(c->esp + 4), sizeof cmd->args);
    __atomic_store_n(&q_head, q_head + sizeof(Cmd), __ATOMIC_SEQ_CST);
    os_unfair_lock_unlock(&q_lock);
    wake_worker();
    u64 t0 = trace ? mach_absolute_time() : 0;
    for (u32 i = 0; !__atomic_load_n(&r.done, __ATOMIC_ACQUIRE); i++) spin_pause(i);
    if (trace) {
        wait_ticks += mach_absolute_time() - t0;
        n_sync++;
        method_sync[index]++;
    }
    c->eax = r.eax;
    c->edx = r.edx;
}
static void run_async(CPU *c, const Method *m, u32 index, u32 ptr0, u32 bytes0, u32 ptr1, u32 bytes1) {
    u32 extra = ((bytes0 + 15u) & ~15u) + ((bytes1 + 15u) & ~15u);
    u32 size = (u32)sizeof(Cmd) + extra;
    os_unfair_lock_lock(&q_lock);
    Cmd *cmd = reserve(size);
    cmd->fn = m->fn;
    cmd->result = NULL;
    cmd->size = size;
    cmd->method = index;
    memcpy(cmd->args, GP(c->esp + 4), sizeof cmd->args);
    u8 *data = (u8 *)(cmd + 1);
    if (bytes0) {
        memcpy(data, GP(ptr0), bytes0);
        cmd->args[(int)m->ptr[0]] = (u32)(data - rt_mem);
        data += (bytes0 + 15u) & ~15u;
    }
    if (bytes1) {
        memcpy(data, GP(ptr1), bytes1);
        cmd->args[(int)m->ptr[1]] = (u32)(data - rt_mem);
    }
    __atomic_store_n(&q_head, q_head + size, __ATOMIC_SEQ_CST);
    os_unfair_lock_unlock(&q_lock);
    wake_worker();
    c->eax = 0;
    if (trace) {
        n_async++;
        method_async[index]++;
    }
}
// Calls Feral's own implementation of a method from a method of the Metal renderer, on the same thread.
u32 rt_d3dq_feral(CPU *c, const char *name, const u32 *args, int n) {
    const Method *m = method_named(name);
    guest_fn fn = m ? rt_guest_fn(m->orig) : NULL;
    if (!fn) {
        fprintf(stderr, "[mtl] %s isn't in Feral's layer\n", name);
        return 0x80004001u;
    }
    u32 saved = c->esp, esp = ((saved - 256u - (u32)n * 4u) & ~15u);
    for (int i = 0; i < n; i++) G32(esp + (u32)i * 4u) = args[i];
    esp -= 4;
    G32(esp) = RT_HOST_RETURN;
    c->esp = esp;
    fn(c);
    c->esp = saved;
    return c->eax;
}
// Queues a call with explicit arguments, for a call the engine didn't make itself.
static void run_async_with(u32 index, u32 this_arg) {
    const Method *m = &methods[index];
    os_unfair_lock_lock(&q_lock);
    Cmd *cmd = reserve(sizeof(Cmd));
    cmd->fn = m->fn;
    cmd->result = NULL;
    cmd->size = sizeof(Cmd);
    cmd->method = index;
    cmd->args[0] = this_arg;
    __atomic_store_n(&q_head, q_head + sizeof(Cmd), __ATOMIC_SEQ_CST);
    os_unfair_lock_unlock(&q_lock);
    wake_worker();
}
static void note_stats(void) {
    static u64 last;
    u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000000;
    if (now - last < 5) return;
    last = now;
    fprintf(stderr, "[d3dq] per second: %llu skipped, %llu asynchronous, %llu synchronous, waited %.0f ms:", n_skipped / 5, n_async / 5, n_sync / 5,
            (double)wait_ticks * 125.0 / 3.0 / 1e6 / 5.0);
    for (u32 i = 0; i < n_methods; i++) {
        if (method_sync[i] >= 5) fprintf(stderr, " %s:%llu", methods[i].name, method_sync[i] / 5);
        method_sync[i] = 0;
    }
    fprintf(stderr, "\n[d3dq] asynchronous:");
    for (u32 i = 0; i < n_methods; i++) {
        if (method_async[i] >= 5) fprintf(stderr, " %s:%llu", methods[i].name, method_async[i] / 5);
        method_async[i] = 0;
    }
    fprintf(stderr, "\n");
    n_async = n_sync = wait_ticks = n_skipped = 0;
}
// The shim in every vtable slot. On return the dispatcher pops the return address, as for any other import.
extern int rt_d3d_census_on;
void rt_d3d_census(const char *m, CPU *c);
int rt_d3d_census_start(void);
int rt_d3dq_is_worker(CPU *c) { return c == worker_cpu; }
// With Metal on, Feral's layer still creates its device, and its code calls itself through these same tables with its own objects.
static int metal_on;
static u32 layer_lo = ~0u, layer_hi;
void rt_mtl_missing(CPU *c);
// ARKHAM_D3D_TRACE=from,step,count: every call the engine makes to Direct3D during one frame as [d3d] lines, from that many seconds after the first Present, then every step seconds.
// It sits in front of both renderers, so a run on each shows whether the engine asks for the same thing. Shaders go by the names of the files in logs/msl.
uint32_t d3dsh_words(const uint32_t *code, uint32_t limit);
static struct {
    int enabled, on, left;
    double from, step;
    u64 first, pending_hash;
    u32 pending;
} dtr = {.enabled = -1};
#define DTR_SHADERS (1u << 16)
static struct {
    u32 obj;
    u64 hash;
} dtr_shaders[DTR_SHADERS];
// When keys are recorded or replayed, the moment a level starts is what both go by, and the trace's times too.
void rt_input_level_started(void);
static struct {
    int on;
    u32 draws;
} lvl = {.on = -1};
static void level_note(const Method *m) {
    if (lvl.on < 0) lvl.on = getenv("ARKHAM_RECORD") || getenv("ARKHAM_REPLAY");
    if (!lvl.on) return;
    if (!strncmp(m->name, "Device.Draw", 11)) {
        lvl.draws++;
    } else if (!strcmp(m->name, "Device.Present")) {
        if (lvl.draws > 300) {
            rt_input_level_started();
            lvl.on = 0;
            dtr.first = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        }
        lvl.draws = 0;
    }
}
static u64 dtr_shader(u32 obj, u64 set) {
    for (u32 i = (obj * 2654435761u) >> 16, n = 0; n < DTR_SHADERS; i = (i + 1) & (DTR_SHADERS - 1), n++) {
        if (dtr_shaders[i].obj == obj || !dtr_shaders[i].obj) {
            if (set) {
                dtr_shaders[i].obj = obj;
                dtr_shaders[i].hash = set;
            }
            return dtr_shaders[i].hash;
        }
    }
    return 0;
}
// Seconds since the level started when keys are recorded or replayed, below zero before that.
double rt_d3d_level_seconds(void) {
    if (lvl.on != 0 || !dtr.first) return -1;
    return (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - dtr.first) / 1e9;
}
// True once for each traced frame, to save a picture of it.
int rt_d3d_trace_picture(void) {
    static int was;
    int on = dtr.on;
    int edge = on && !was;
    was = on;
    return edge;
}
static void dtr_call(CPU *c, const Method *m) {
    const char *n = m->name;
    if (dtr.pending) {
        u32 obj = G32(dtr.pending);
        if (obj) dtr_shader(obj, dtr.pending_hash);
        dtr.pending = 0;
    }
    if (!strcmp(n, "Device.CreatePixelShader") || !strcmp(n, "Device.CreateVertexShader")) {
        const u32 *code = (const u32 *)GP(arg_of(c, 1));
        u32 words = d3dsh_words(code, 1u << 18);
        u64 h = 0xcbf29ce484222325ull;
        for (u32 i = 0; i < words * 4; i++) h = (h ^ ((const u8 *)code)[i]) * 0x100000001b3ull;
        dtr.pending = arg_of(c, 2);
        dtr.pending_hash = h ? h : 1;
        return;
    }
    if (!strcmp(n, "Device.Present")) {
        if (lvl.on > 0) return;  // waiting for the level
        u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (!dtr.first) dtr.first = now;
        if (dtr.on) fprintf(stderr, "[d3d] end\n");
        dtr.on = 0;
        double t = (double)(now - dtr.first) / 1e9;
        if (dtr.left > 0 && t >= dtr.from) {
            dtr.on = 1;
            dtr.left--;
            dtr.from = t + dtr.step;
            fprintf(stderr, "[d3d] frame at %.1f s\n", t);
        }
        return;
    }
    if (!dtr.on) return;
    if (!strcmp(n, "Device.SetPixelShader") || !strcmp(n, "Device.SetVertexShader")) {
        fprintf(stderr, "[d3d] %s %016llx\n", n, (unsigned long long)dtr_shader(arg_of(c, 1), 0));
    } else if (!strcmp(n, "Device.SetPixelShaderConstantF")) {
        u32 start = arg_of(c, 1), count = arg_of(c, 3);
        const float *v = (const float *)GP(arg_of(c, 2));
        fprintf(stderr, "[d3d] %s %u %u:", n, start, count);
        for (u32 i = 0; i < count && i < 16; i++) fprintf(stderr, " %.4g,%.4g,%.4g,%.4g", v[i * 4], v[i * 4 + 1], v[i * 4 + 2], v[i * 4 + 3]);
        fputc('\n', stderr);
    } else if (strstr(n, "AddRef") || strstr(n, "Release") || strstr(n, "VertexShaderConstantF")) {
        return;
    } else {
        fprintf(stderr, "[d3d] %s %x %x %x %x %x %x\n", n, arg_of(c, 1), arg_of(c, 2), arg_of(c, 3), arg_of(c, 4), arg_of(c, 5), arg_of(c, 6));
    }
}
static void method_shim(CPU *c) {
    u32 index = rt_dyn_cur_arg;
    Method *m = &methods[index];
    if (__builtin_expect(rt_d3d_census_on > 0, 0) && c != worker_cpu) rt_d3d_census(m->name, c);
    if (__builtin_expect(lvl.on != 0, 0) && c != worker_cpu) level_note(m);
    if (__builtin_expect(dtr.enabled != 0, 0) && c != worker_cpu) {
        if (dtr.enabled < 0) {
            const char *v = getenv("ARKHAM_D3D_TRACE");
            dtr.enabled = v && sscanf(v, "%lf,%lf,%d", &dtr.from, &dtr.step, &dtr.left) == 3;
        }
        if (dtr.enabled) dtr_call(c, m);
    }
    if (metal_on) {
        u32 from = G32(c->esp);
        if (from >= layer_lo && from < layer_hi) {
            rt_dispatch(c, m->orig);
            c->esp -= 4;
            return;
        }
        if (m->missing) {
            if (!m->noted) {
                m->noted = 1;
                fprintf(stderr, "[mtl] %s isn't implemented, called from %#x\n", m->name, from);
            }
            c->eax = 0x80004001u;
            return;
        }
        if (!m->host && !m->noted) {
            m->noted = 1;
            fprintf(stderr, "[mtl] the engine calls %s in Feral's layer, from %#x\n", m->name, from);
        }
    }
    // Calls the layer makes on itself, and calls while the queue isn't running, go straight through.
    if (c == worker_cpu || m->kind == K_LOCAL) {
        if (m->host) {
            m->fn(c);
            return;
        }
        rt_dispatch(c, m->orig);
        c->esp -= 4;
        return;
    }
    if (trace) note_stats();
    u32 p0 = 0, b0 = 0, p1 = 0, b1 = 0;
    switch (m->kind) {
        case K_ASYNC:
            break;
        case K_REF:
            run_async(c, m, index, 0, 0, 0, 0);
            c->eax = 1;
            return;
        case K_RENDER_STATE: {
            u32 state = arg_of(c, 1), value = arg_of(c, 2);
            if (state < RENDER_STATES) {
                if (shadow.render_set[state] && shadow.render[state] == value) {
                    if (trace) n_skipped++;
                    c->eax = 0;
                    return;
                }
                shadow.render[state] = value;
                shadow.render_set[state] = 1;
            }
            break;
        }
        case K_SAMPLER_STATE: {
            u32 sampler = arg_of(c, 1), type = arg_of(c, 2), value = arg_of(c, 3);
            if (sampler < SAMPLERS && type < SAMPLER_TYPES) {
                if (shadow.sampler_set[sampler][type] && shadow.sampler[sampler][type] == value) {
                    if (trace) n_skipped++;
                    c->eax = 0;
                    return;
                }
                shadow.sampler[sampler][type] = value;
                shadow.sampler_set[sampler][type] = 1;
            }
            break;
        }
        case K_STREAM_FREQ: {
            u32 stream = arg_of(c, 1), value = arg_of(c, 2);
            if (stream < STREAMS) {
                if (shadow.freq_set[stream] && shadow.freq[stream] == value) {
                    if (trace) n_skipped++;
                    c->eax = 0;
                    return;
                }
                shadow.freq[stream] = value;
                shadow.freq_set[stream] = 1;
            }
            break;
        }
        case K_INDICES: {
            u32 ib = arg_of(c, 1);
            if (shadow.indices_set && shadow.indices == ib) {
                if (trace) n_skipped++;
                c->eax = 0;
                return;
            }
            shadow.indices = ib;
            shadow.indices_set = 1;
            break;
        }
        case K_QUERY_DATA: {
            if (pipe_on && G32(arg_of(c, 0) + 0xc) == 9) {
                static u32 reading_frame = ~0u;
                if (reading_frame != pipe_n[2]) {
                    reading_frame = pipe_n[2];
                    if (pipe_n[0] != pipe_n[1]) pipe_n[1] = pipe_n[0];
                    rt_pipe_mark(0);
                    pipe_n[1] = pipe_n[0] - 1;
                }
                run_sync(c, m, index);
                pipe_t[1][(pipe_n[0] - 1) & 255] = mach_absolute_time();
                pipe_n[1] = pipe_n[0];
                return;
            }
            run_sync(c, m, index);
            return;
        }
        case K_SURFACE_LEVEL: {  // this, level, out
            u32 tex = arg_of(c, 0), level = arg_of(c, 1), out = arg_of(c, 2);
            u32 k = level_slot(tex, level);
            if (level_cache[k].texture == tex && level_cache[k].level == level && level_cache[k].surface) {
                G32(out) = level_cache[k].surface;
                run_async_with(surface_addref_method, level_cache[k].surface);
                if (trace) n_skipped++;
                c->eax = 0;
                return;
            }
            run_sync(c, m, index);
            if (!c->eax) {
                level_cache[k].texture = tex;
                level_cache[k].level = level;
                level_cache[k].surface = G32(out);
            }
            return;
        }
        case K_CREATE_TEXTURE: {  // this, width, height, levels, usage, format, pool, out, shared
            u32 out = arg_of(c, 7);
            run_sync(c, m, index);
            // The new texture may sit at the address of a destroyed one.
            if (!c->eax)
                for (u32 i = 0; i < LEVEL_CACHE; i++)
                    if (level_cache[i].texture == G32(out)) level_cache[i].surface = 0;
            return;
        }
        case K_UNSET_STATE:
            memset(level_cache, 0, sizeof level_cache);
            memset(&shadow, 0, sizeof shadow);
            run_sync(c, m, index);
            return;
        case K_PRESENT:
            if (pipe_on) {
                rt_pipe_mark(2);
                static u32 reports;
                if (++reports % 120 == 0) pipe_report();
            }
            if (arg_of(c, 4)) {  // a dirty region isn't worth copying
                run_sync(c, m, index);
                return;
            }
            /* fall through */
        case K_ARRAY: {
            u32 n = m->count >= 0 ? arg_of(c, m->count) : 1;
            p0 = arg_of(c, m->ptr[0]);
            b0 = p0 ? n * m->unit[0] : 0;
            if (m->ptr[1] >= 0) {
                p1 = arg_of(c, m->ptr[1]);
                b1 = p1 ? m->unit[1] : 0;
            }
            break;
        }
        case K_DRAW_UP:  // this, type, primitives, data, stride
            p0 = arg_of(c, 3);
            b0 = prim_vertices(arg_of(c, 1), arg_of(c, 2)) * arg_of(c, 4);
            break;
        case K_DRAW_IUP: {  // this, type, min index, vertices, primitives, indices, format, data, stride
            shadow.indices_set = 0;  // Direct3D clears the index buffer after this call
            u32 fmt = arg_of(c, 6);
            p0 = arg_of(c, 5);
            b0 = prim_vertices(arg_of(c, 1), arg_of(c, 4)) * (fmt == 102 ? 4u : 2u);
            p1 = arg_of(c, 7);
            b1 = (arg_of(c, 2) + arg_of(c, 3)) * arg_of(c, 8);
            break;
        }
        default:
            run_sync(c, m, index);
            return;
    }
    if (b0 > (1u << 20) || b1 > (1u << 20)) {
        run_sync(c, m, index);
        return;
    }
    run_async(c, m, index, p0, b0, p1, b1);
}

// The one function outside the interfaces that changes the layer's state: it switches a texture between sRGB and linear for each draw.
int rt_mtl_on(void);
void rt_d3dq_function(CPU *c, guest_fn body) {
    if (rt_mtl_on()) {
        // The sampler state says the same thing, and Feral's function would look inside an object it didn't make.
        c->esp += 4;
        return;
    }
    if (c == worker_cpu || !ring) {
        body(c);
        return;
    }
    static Method direct;
    direct.fn = body;
    run_async(c, &direct, 0, 0, 0, 0, 0);
    c->esp += 4;
}

static void add_method(u32 vtable, u32 slot, const char *cls, const char *method) {
    if (n_methods >= MAX_METHODS) return;
    u32 where = vtable + slot * 4;
    u32 orig = G32(where);
    guest_fn fn = rt_guest_fn(orig);
    if (!fn) return;
    char name[96];
    snprintf(name, sizeof name, "%s.%s", cls, method);
    Method *m = &methods[n_methods];
    m->orig = orig;
    m->fn = fn;
    m->name = strdup(name);
    m->kind = K_SYNC;
    m->ptr[0] = m->ptr[1] = -1;
    m->count = -1;
    for (unsigned i = 0; i < sizeof rules / sizeof rules[0]; i++) {
        if (!strcmp(rules[i].name, name)) {
            m->kind = rules[i].kind;
            m->ptr[0] = rules[i].ptr0;
            m->ptr[1] = rules[i].ptr1;
            m->count = rules[i].count;
            m->unit[0] = rules[i].unit0;
            m->unit[1] = rules[i].unit1;
        }
    }
    if (slot == 2 || slot == 3) m->kind = K_REF;
    {
        void (*rt_mtl_method(const char *name, unsigned slot))(CPU *);
        void (*metal)(CPU *) = rt_mtl_method(m->name, slot);
        if (metal) {
            m->fn = metal;
            m->host = 1;
            m->missing = metal == rt_mtl_missing;
            metal_on = 1;
        }
    }
    if (orig < layer_lo) layer_lo = orig;
    if (orig + 0x400 > layer_hi) layer_hi = orig + 0x400;
    if (slot == 2 && !strcmp(cls, "Surface")) surface_addref_method = n_methods;
    if (!strcmp(name, "Device.CreateVertexShader")) method_create_vs = n_methods;
    if (!strcmp(name, "Device.CreatePixelShader")) method_create_ps = n_methods;
    G32(where) = rt_dyn_import_nogate(method_shim, n_methods);
    n_methods++;
}
void rt_d3dq_start(void) {
    if (getenv("ARKHAM_D3D_DIRECT")) return;
    trace = getenv("ARKHAM_TRACE_D3DQ") != NULL;
    pipe_on = getenv("ARKHAM_TRACE_PIPE") != NULL;
    trace_shaders = getenv("ARKHAM_TRACE_PIPES") != NULL;
    rt_d3d_census_start();
    ring_guest = rt_guest_alloc_pages(RING_SIZE);
    ring = rt_mem + ring_guest;
    pthread_t t;
    pthread_create(&t, NULL, worker_main, NULL);
    pthread_detach(t);
    while (!__atomic_load_n(&worker_cpu, __ATOMIC_ACQUIRE)) sched_yield();
    for (unsigned k = 0; k < sizeof interfaces / sizeof interfaces[0]; k++) {
        const char *list = interfaces[k].names ? interfaces[k].names : device_names;
        // Slots 0 and 1 are destructors, then come AddRef, Release and QueryInterface.
        for (u32 slot = 0; slot < 5; slot++) {
            char generic[16];
            snprintf(generic, sizeof generic, "slot%u", slot);
            add_method(interfaces[k].vtable, slot, interfaces[k].cls, generic);
        }
        char *copy = strdup(list), *save = NULL;
        u32 slot = 5;
        for (char *w = strtok_r(copy, " ", &save); w; w = strtok_r(NULL, " ", &save), slot++) add_method(interfaces[k].vtable, slot, interfaces[k].cls, w);
        free(copy);
    }
    fprintf(stderr, "[d3dq] %u methods go through the Direct3D thread, the layer's code is around %#x-%#x\n", n_methods, layer_lo, layer_hi);
}
