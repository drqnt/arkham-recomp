// Direct3D 9 on Metal, in place of Feral's layer and Apple's OpenGL. ARKHAM_RENDERER=metal turns it on: see docs/METAL_LAYER.md.
//
// The engine calls COM methods through the vtables d3d_queue.c patches. A method with an implementation here runs on the Direct3D thread instead of Feral's code. Objects are small guest
// allocations holding the vtable the engine calls through, and the index of a host object. For now Feral's device is still created, which makes an OpenGL context that nothing draws to,
// and methods with nothing to do with resources or drawing still go to it.
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CABase.h>
#import <QuartzCore/CAMetalLayer.h>
#include "d3d_shader.h"
#include "shim.h"

#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *);

// ---------- objects ----------
enum { VT_QUERY = 0x35ea798u, VT_SURFACE = 0x35ea96cu, VT_TEXTURE = 0x35eac24u, VT_CUBE = 0x35e9964u, VT_INDEX = 0x35ea6fcu, VT_VERTEX = 0x35eaefcu, VT_DECL = 0x35eaf68u, VT_PS = 0x35ec6d8u, VT_VS = 0x35eafe8u };
typedef enum { O_FREE, O_TEXTURE, O_CUBE, O_SURFACE, O_VB, O_IB, O_DECL, O_VS, O_PS, O_QUERY } Kind;
#define MAX_LEVELS 16
typedef struct Obj Obj;
typedef struct {
    u8 stream, type, usage, index;
    u16 offset;
} Element;
struct Obj {
    Kind kind;
    u32 guest;
    int refs;
    // textures and surfaces
    id<MTLTexture> tex, srgb, view;  // view: for reading a render target whose format needs channels filled in
    u32 format, width, height, levels, usage;
    int samples, is_depth, has_stencil;
    u32 asked_samples;  // what the engine asked for, when ARKHAM_MTL_SAMPLES gave it another count
    u32 surfaces[MAX_LEVELS * 6];
    // shadow maps drawn ahead of the scene: the copies, and which is next this frame
#define MAX_RENAMES 32
    id<MTLTexture> phys[MAX_RENAMES], phys_view[MAX_RENAMES];
    u32 rename_n;
    u64 rename_frame, written_cb;
    // where this surface was last averaged into
    id<MTLTexture> resolve_tex;
    u32 resolve_level, resolve_slice;
    // stencil: its value everywhere outside the box, when that's known
    int s_known;
    u32 s_value, s_box[4];
    u32 locked[MAX_LEVELS * 6];
    u32 lock_flags[MAX_LEVELS * 6];
    // surfaces
    Obj *parent;
    u32 level, slice;
    // buffers
    id<MTLBuffer> buffer;
    u32 size, lock_mirror, index_format;
    // vertex declarations
    Element elements[32];
    int n_elements;
    float attr_scale[16][4];
    u32 attr_format[16];
    u32 slots;
    // shaders
    id<MTLLibrary> lib;
    id<MTLLibrary> all_outputs;  // vertex shaders: the translation that writes every output, for pixel shaders that read more than this one writes
    char fn[20], all_fn[20];     // the entry functions' names in those: vs_main or ps_main, or s_<hash of the text> in a library built ahead
    D3dShaderInfo info;
    u32 *code;
    char *msl;  // until the first draw that needs it: most shaders the game creates are never drawn with
    // queries
    u32 query_type, slot;
    u64 serial;
    u64 uid;  // never reused, unlike the object's address and its id: caches key on this
};
#define MAX_OBJS (1u << 18)
static Obj *objs[MAX_OBJS];
// Every freed id goes back on the list: a list shorter than what the game frees at once lost ids for good, and enough of them would run past the table.
static u32 n_objs = 1, free_list[MAX_OBJS], n_free;
static u64 next_uid = 1;

static Obj *obj_new(Kind kind, u32 vtable) {
    u32 id;
    if (n_free) {
        id = free_list[--n_free];
    } else if (n_objs < MAX_OBJS) {
        id = n_objs++;
    } else {
        fprintf(stderr, "[mtl] more than %u Direct3D objects alive at once\n", MAX_OBJS);
        abort();
    }
    Obj *o = calloc(1, sizeof *o);
    objs[id] = o;
    o->uid = next_uid++;
    o->kind = kind;
    o->refs = 1;
    o->guest = rt_guest_calloc(1, 16);
    G32(o->guest) = vtable;
    G32(o->guest + 8) = id;
    return o;
}
static inline Obj *obj_of(u32 guest) { return guest ? objs[G32(guest + 8)] : NULL; }
static void obj_free(Obj *o) {
    u32 id = G32(o->guest + 8);
    [o->tex release];
    [o->srgb release];
    [o->view release];
    [o->resolve_tex release];
    for (int i = 0; i < MAX_RENAMES; i++) {
        [o->phys[i] release];
        [o->phys_view[i] release];
    }
    [o->buffer release];
    [o->lib release];
    [o->all_outputs release];
    free(o->code);
    free(o->msl);
    rt_guest_free(o->guest);
    free(o);
    objs[id] = NULL;
    free_list[n_free++] = id;
}

// ---------- the device's state ----------
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLCommandBuffer> cb;
static id<MTLRenderCommandEncoder> enc;
static CAMetalLayer *layer;
static _Atomic int hud_wanted = -1;  // Apple's Metal HUD: 1 on, 0 off, -1 until ARKHAM_METAL_HUD or the phone's settings say (rt_set_metal_hud)
static _Atomic int layer_ready;
static dispatch_semaphore_t frames_in_flight;
static _Atomic u64 completed_serial;
static u64 cb_serial = 1;
static Obj *back_buffer;
static u32 rs[256], ss[16][16];
static Obj *rt_surface, *ds_surface;
static u32 vp[4];
static float vp_z[2];
static u32 scissor[4];
static float clip_planes[2][4];
static Obj *cur_vs, *cur_ps, *cur_decl, *cur_ib, *cur_tex[16];
static struct {
    Obj *vb;
    u32 offset, stride;
} streams[16];
static float vs_const[256][4], ps_const[224][4];
static int vs_const_dirty = 1, ps_const_dirty = 1;
static u32 vs_const_at, ps_const_at;

// What the next pass starts by clearing.
static struct {
    int color, depth, stencil;
    float rgba[4], z;
    u32 s;
} pending_clear;

// Upload space for constants and for draws from memory, one per frame in flight.
#define TRANSIENT_FRAMES 3
#define TRANSIENT_SIZE (48u << 20)
#define TRANSIENT_CHAIN 8
static id<MTLBuffer> transient[TRANSIENT_FRAMES][TRANSIENT_CHAIN];
static u32 transient_frame, transient_link, transient_at;
static id<MTLBuffer> vs_const_buf, ps_const_buf;
static id<MTLBuffer> visibility;
#define VIS_SLOTS (1u << 15)
static u32 vis_next;
static Obj *open_query;
static id<MTLTexture> dummy_2d, dummy_cube, dummy_depth;
static id<MTLBuffer> dummy_attr;
static u64 frame_no;
static u32 pass_draws;
// What a frame spent, reported once a second.
static u64 n_quad_clears, n_skipped_clears, n_resolves_in_pass, n_passes, n_created, ticks_drawable, ticks_inflight, ticks_query_wait, query_wait_from;
static _Atomic u64 gpu_ns;
extern u64 rt_d3dq_busy_ticks;

// What the current encoder has been told.
typedef struct {
    id<MTLRenderPipelineState> pipeline;
    id<MTLDepthStencilState> depth;
    u32 cull, stencil_ref;
    float bias, slope;
    int clamp_depth;
    u32 vp[4], sc[4];
    float vpz[2];
    int have_vp, have_sc, have_cull, have_bias, have_stencil;
    id<MTLBuffer> vb[16];
    u32 vb_off[16];
    id<MTLBuffer> vs_buf, ps_buf;
    u32 vs_off, ps_off;
    id<MTLTexture> tex[16];
    id<MTLSamplerState> samp[16];
    int dummy_bound;
} EncState;
static EncState e;

// ARKHAM_MTL_LABELS=1 names each pass for Instruments' Metal System Trace, which then shows what every pass costs on the GPU in a normal frame.
static int labels_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("ARKHAM_MTL_LABELS") != NULL;
    return on;
}
static void complain(const char *what) {
    static const char *seen[64];
    for (int i = 0; i < 64; i++) {
        if (seen[i] == what) return;
        if (!seen[i]) {
            seen[i] = what;
            fprintf(stderr, "[mtl] %s\n", what);
            return;
        }
    }
}

// ---------- formats ----------
#define FMT_DXT1 0x31545844u
#define FMT_DXT3 0x33545844u
#define FMT_DXT5 0x35545844u
typedef struct {
    MTLPixelFormat pf, srgb;
    u32 block, bytes;  // block size in pixels, bytes per block
    int depth, stencil;
    MTLTextureSwizzleChannels swizzle;
} FormatInfo;
static FormatInfo format_info(u32 f) {
    MTLTextureSwizzleChannels same = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
    FormatInfo i = {MTLPixelFormatInvalid, MTLPixelFormatInvalid, 1, 4, 0, 0, same};
    switch (f) {
        case FMT_DXT1: i.pf = MTLPixelFormatBC1_RGBA; i.srgb = MTLPixelFormatBC1_RGBA_sRGB; i.block = 4; i.bytes = 8; break;
        case FMT_DXT3: i.pf = MTLPixelFormatBC2_RGBA; i.srgb = MTLPixelFormatBC2_RGBA_sRGB; i.block = 4; i.bytes = 16; break;
        case FMT_DXT5: i.pf = MTLPixelFormatBC3_RGBA; i.srgb = MTLPixelFormatBC3_RGBA_sRGB; i.block = 4; i.bytes = 16; break;
        case 21: case 22: i.pf = MTLPixelFormatBGRA8Unorm; i.srgb = MTLPixelFormatBGRA8Unorm_sRGB; break;
        case 50:
            i.pf = MTLPixelFormatR8Unorm;
            i.srgb = MTLPixelFormatR8Unorm_sRGB;
            i.bytes = 1;
            i.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne);
            break;
        case 34:  // G16R16, read as (r, g, 1, 1)
            i.pf = MTLPixelFormatRG16Unorm;
            i.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne);
            break;
        case 36: i.pf = MTLPixelFormatRGBA16Unorm; i.bytes = 8; break;
        case 113: i.pf = MTLPixelFormatRGBA16Float; i.bytes = 8; break;
        case 112:
            i.pf = MTLPixelFormatRG16Float;
            i.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne);
            break;
        case 114:
            i.pf = MTLPixelFormatR32Float;
            i.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleOne, MTLTextureSwizzleOne, MTLTextureSwizzleOne);
            break;
        case 75: i.pf = MTLPixelFormatDepth32Float_Stencil8; i.depth = 1; i.stencil = 1; i.bytes = 8; break;
        case 77: i.pf = MTLPixelFormatDepth32Float; i.depth = 1; break;
        default: break;
    }
    return i;
}

// ---------- creating things ----------
static u32 level_dim(u32 v, u32 level) {
    v >>= level;
    return v ? v : 1;
}
static id<MTLTexture> make_texture(MTLTextureType type, u32 fmt, u32 w, u32 h, u32 levels, int samples, int target, id<MTLTexture> *srgb_view) {
    FormatInfo fi = format_info(fmt);
    if (fi.pf == MTLPixelFormatInvalid) {
        fprintf(stderr, "[mtl] texture format %x isn't known\n", fmt);
        return nil;
    }
    MTLTextureDescriptor *d = [[MTLTextureDescriptor new] autorelease];
    d.textureType = type;
    d.pixelFormat = fi.pf;
    d.width = w;
    d.height = h;
    d.mipmapLevelCount = levels;
    d.sampleCount = samples > 1 ? (NSUInteger)samples : 1;
    // A render target can't have a swizzle of its own, only a view for reading it through.
    int swizzled = fi.swizzle.red != MTLTextureSwizzleRed || fi.swizzle.green != MTLTextureSwizzleGreen || fi.swizzle.blue != MTLTextureSwizzleBlue || fi.swizzle.alpha != MTLTextureSwizzleAlpha;
    if (!target) d.swizzle = fi.swizzle;
    d.usage = MTLTextureUsageShaderRead | (target ? MTLTextureUsageRenderTarget : 0) | (fi.srgb != MTLPixelFormatInvalid || (target && swizzled) ? MTLTextureUsagePixelFormatView : 0);
    d.storageMode = target ? MTLStorageModePrivate : MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    if (t && srgb_view && fi.srgb != MTLPixelFormatInvalid && samples <= 1) *srgb_view = [t newTextureViewWithPixelFormat:fi.srgb textureType:type levels:NSMakeRange(0, levels) slices:NSMakeRange(0, type == MTLTextureTypeCube ? 6 : 1) swizzle:fi.swizzle];
    return t;
}
static u32 full_levels(u32 w, u32 h) {
    u32 n = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        n++;
    }
    return n;
}
static Obj *make_surface_of(Obj *t, u32 level, u32 slice) {
    Obj *s = obj_new(O_SURFACE, VT_SURFACE);
    s->parent = t;
    s->level = level;
    s->slice = slice;
    s->tex = [t->tex retain];
    s->format = t->format;
    s->width = level_dim(t->width, level);
    s->height = level_dim(t->height, level);
    s->samples = t->samples;
    s->usage = t->usage;
    FormatInfo fi = format_info(t->format);
    s->is_depth = fi.depth;
    s->has_stencil = fi.stencil;
    return s;
}
static void m_CreateTexture(CPU *c) {  // this, width, height, levels, usage, format, pool, out, shared
    u32 w = ARG(1), h = ARG(2), levels = ARG(3), usage = ARG(4), fmt = ARG(5);
    if (!levels) levels = full_levels(w, h);
    Obj *o = obj_new(O_TEXTURE, VT_TEXTURE);
    o->format = fmt;
    o->width = w;
    o->height = h;
    o->levels = levels;
    o->usage = usage;
    o->samples = 1;
    FormatInfo fi = format_info(fmt);
    o->is_depth = fi.depth;
    o->has_stencil = fi.stencil;
    o->tex = make_texture(MTLTextureType2D, fmt, w, h, levels, 1, (usage & 3) != 0, &o->srgb);
    if ((usage & 3) && o->tex) o->view = [o->tex newTextureViewWithPixelFormat:fi.pf textureType:MTLTextureType2D levels:NSMakeRange(0, levels) slices:NSMakeRange(0, 1) swizzle:fi.swizzle];
    G32(ARG(7)) = o->guest;
    RET(0);
}
static void m_CreateCubeTexture(CPU *c) {  // this, edge, levels, usage, format, pool, out, shared
    u32 w = ARG(1), levels = ARG(2), usage = ARG(3), fmt = ARG(4);
    if (!levels) levels = full_levels(w, w);
    Obj *o = obj_new(O_CUBE, VT_CUBE);
    o->format = fmt;
    o->width = o->height = w;
    o->levels = levels;
    o->usage = usage;
    o->samples = 1;
    o->tex = make_texture(MTLTextureTypeCube, fmt, w, w, levels, 1, (usage & 3) != 0, &o->srgb);
    if ((usage & 3) && o->tex) o->view = [o->tex newTextureViewWithPixelFormat:o->tex.pixelFormat textureType:MTLTextureTypeCube levels:NSMakeRange(0, levels) slices:NSMakeRange(0, 6) swizzle:format_info(fmt).swizzle];
    G32(ARG(6)) = o->guest;
    RET(0);
}
// ARKHAM_MTL_SAMPLES=n: targets the engine asks to be multisampled get n samples instead, while the engine is still told what it asked for. For measuring what multisampling
// costs the GPU; 1 turns it off.
static int samples_for(u32 asked) {
    static int forced = -1;
    if (forced < 0) {
        const char *v = getenv("ARKHAM_MTL_SAMPLES");
        forced = v ? atoi(v) : 0;
    }
    if (asked <= 1) return 1;
    return forced > 0 ? forced : (int)asked;
}
static void m_CreateRenderTarget(CPU *c) {  // this, width, height, format, samples, quality, lockable, out, shared
    u32 w = ARG(1), h = ARG(2), fmt = ARG(3), samples = ARG(4);
    Obj *o = obj_new(O_SURFACE, VT_SURFACE);
    o->format = fmt;
    o->width = w;
    o->height = h;
    o->samples = samples_for(samples);
    o->asked_samples = samples > 1 ? samples : 0;
    o->usage = 1;
    o->tex = make_texture(o->samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D, fmt, w, h, 1, o->samples, 1, NULL);
    G32(ARG(7)) = o->guest;
    RET(0);
}
static void m_CreateDepthStencilSurface(CPU *c) {  // this, width, height, format, samples, quality, discard, out, shared
    u32 w = ARG(1), h = ARG(2), fmt = ARG(3), samples = ARG(4);
    Obj *o = obj_new(O_SURFACE, VT_SURFACE);
    FormatInfo fi = format_info(fmt);
    o->format = fmt;
    o->width = w;
    o->height = h;
    o->samples = samples_for(samples);
    o->asked_samples = samples > 1 ? samples : 0;
    o->usage = 2;
    o->is_depth = fi.depth;
    o->has_stencil = fi.stencil;
    o->tex = make_texture(o->samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D, fmt, w, h, 1, o->samples, 1, NULL);
    G32(ARG(7)) = o->guest;
    RET(0);
}
static void m_CreateVertexBuffer(CPU *c) {  // this, length, usage, fvf, pool, out, shared
    Obj *o = obj_new(O_VB, VT_VERTEX);
    o->size = ARG(1);
    o->usage = ARG(2);
    o->buffer = [dev newBufferWithLength:o->size ? o->size : 16 options:MTLResourceStorageModeShared];
    G32(ARG(5)) = o->guest;
    RET(0);
}
static void m_CreateIndexBuffer(CPU *c) {  // this, length, usage, format, pool, out, shared
    Obj *o = obj_new(O_IB, VT_INDEX);
    o->size = ARG(1);
    o->usage = ARG(2);
    o->index_format = ARG(3);
    o->buffer = [dev newBufferWithLength:o->size ? o->size : 16 options:MTLResourceStorageModeShared];
    G32(ARG(5)) = o->guest;
    RET(0);
}
static void m_CreateVertexDeclaration(CPU *c) {  // this, elements, out
    Obj *o = obj_new(O_DECL, VT_DECL);
    u32 p = ARG(1);
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) o->attr_scale[i][k] = 1;
    for (int i = 0; i < 32; i++, p += 8) {
        u32 stream = G16(p);
        if (stream == 0xFF) break;
        Element *el = &o->elements[o->n_elements++];
        el->stream = (u8)stream;
        el->offset = G16(p + 2);
        el->type = G8(p + 4);
        el->usage = G8(p + 6);
        el->index = G8(p + 7);
        int slot = d3dsh_attribute_slot(el->usage, el->index);
        if (slot < 0) continue;
        o->slots |= 1u << slot;
        float *sc = o->attr_scale[slot];
        MTLVertexFormat vf = MTLVertexFormatFloat4;
        switch (el->type) {
            case 0: vf = MTLVertexFormatFloat; break;
            case 1: vf = MTLVertexFormatFloat2; break;
            case 2: vf = MTLVertexFormatFloat3; break;
            case 3: vf = MTLVertexFormatFloat4; break;
            case 4: vf = MTLVertexFormatUChar4Normalized_BGRA; break;
            // Integer formats can't be read as floats, so they come in normalized and the shader scales them back.
            case 5: vf = MTLVertexFormatUChar4Normalized; sc[0] = sc[1] = sc[2] = sc[3] = 255; break;
            case 6: vf = MTLVertexFormatShort2Normalized; sc[0] = sc[1] = 32767; break;
            case 7: vf = MTLVertexFormatShort4Normalized; sc[0] = sc[1] = sc[2] = sc[3] = 32767; break;
            case 8: vf = MTLVertexFormatUChar4Normalized; break;
            case 9: vf = MTLVertexFormatShort2Normalized; break;
            case 10: vf = MTLVertexFormatShort4Normalized; break;
            case 11: vf = MTLVertexFormatUShort2Normalized; break;
            case 12: vf = MTLVertexFormatUShort4Normalized; break;
            case 15: vf = MTLVertexFormatHalf2; break;
            case 16: vf = MTLVertexFormatHalf4; break;
            default: fprintf(stderr, "[mtl] vertex type %u isn't known\n", el->type);
        }
        o->attr_format[slot] = (u32)vf;
    }
    G32(ARG(2)) = o->guest;
    RET(0);
}
static MTLCompileOptions *compile_options(void) {
    static MTLCompileOptions *opt;
    if (!opt) {
        opt = [MTLCompileOptions new];
        opt.preserveInvariance = YES;
    }
    return opt;
}
static u64 compile_ticks;
static id<MTLLibrary> compile_msl(const char *src) {
    u64 t0 = mach_absolute_time();
    NSError *err = nil;
    NSString *text = [[NSString alloc] initWithUTF8String:src];
    id<MTLLibrary> lib = [dev newLibraryWithSource:text options:compile_options() error:&err];
    [text release];
    compile_ticks += mach_absolute_time() - t0;
    if (!lib) fprintf(stderr, "[mtl] a shader doesn't compile: %s\n", err.localizedDescription.UTF8String);
    return lib;
}
// Compiles every file of a list into Metal's cache, on all cores, and reports how many were there already. Returns how many failed, or -1 when the list can't be read.
static long prebuild_list(const char *list) {
    void *outer = objc_autoreleasePoolPush();
    NSString *text = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:list] encoding:NSUTF8StringEncoding error:nil];
    if (!text) {
        fprintf(stderr, "can't read %s\n", list);
        objc_autoreleasePoolPop(outer);
        return -1;
    }
    NSArray<NSString *> *files = [text componentsSeparatedByString:@"\n"];
    MTLCompileOptions *opt = compile_options();
    static _Atomic long fine, known, failed;
    // The first compile in a process waits for Metal's compiler service, tens of milliseconds even from the cache. Without this, a short list counts as not cached at all.
    [[dev newLibraryWithSource:@"#include <metal_stdlib>\nfragment float4 warm_up() { return float4(1.0); }\n" options:opt error:nil] release];
    double start = CACurrentMediaTime();
    dispatch_apply(files.count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
        if (!files[i].length) return;
        void *pool = objc_autoreleasePoolPush();
        NSString *src = [NSString stringWithContentsOfFile:files[i] encoding:NSUTF8StringEncoding error:nil];
        NSError *err = nil;
        double t0 = CACurrentMediaTime();
        id<MTLLibrary> lib = src ? [dev newLibraryWithSource:src options:opt error:&err] : nil;
        double took = CACurrentMediaTime() - t0;
        if (lib) {
            atomic_fetch_add(&fine, 1);
            if (took < 0.002) atomic_fetch_add(&known, 1);
        } else {
            atomic_fetch_add(&failed, 1);
        }
        [lib release];
        objc_autoreleasePoolPop(pool);
    });
    printf("%ld shaders, %ld of them already in the cache, %ld failed, %.0f s%s\n", atomic_load(&fine), atomic_load(&known), atomic_load(&failed), CACurrentMediaTime() - start,
           getenv("MTL_HUD_ENABLED") ? ", for the Metal HUD" : "");
    fflush(stdout);
    objc_autoreleasePoolPop(outer);
    return atomic_load(&failed);
}
// build/arkham --prebuild-shaders list-of-files, through tools/prebuild_shaders.sh: compiles shaders into Metal's disk cache ahead of play. Metal keeps entries per executable,
// so another program with the same bundle identifier fills the same file with entries the game never finds. Only the game's own binary can do this.
int rt_mtl_prebuild(const char *list) {
    dev = MTLCreateSystemDefaultDevice();
    long failed = prebuild_list(list);
    return failed != 0;
}
static void create_shader(CPU *c, Kind kind, u32 vtable) {  // this, code, out
    const u32 *code = (const u32 *)GP(ARG(1));
    Obj *o = obj_new(kind, vtable);
    n_created++;
    u32 words = d3dsh_words(code, 1u << 18);
    o->code = malloc((size_t)words * 4);
    memcpy(o->code, code, (size_t)words * 4);
    o->msl = d3dsh_translate(o->code, 0, &o->info);
    if (o->info.error) fprintf(stderr, "[mtl] shader translation: %s\n", o->info.error);
    G32(ARG(2)) = o->guest;
    RET(0);
}
static u64 n_compiled, n_uncached, n_prebuilt;
static u64 code_hash(Obj *o);
// ---------- shaders built ahead (tools/build_ios_shaders.py) ----------
// Libraries of compiled shaders, each function named s_<FNV-1a of its MSL text>: on the phone in Documents/Shaders, or where ARKHAM_SHADER_LIBS says.
// A shader whose text has such a function needs no compile from source; any other text, a translator changed since among them, is compiled as before.
static NSDictionary<NSString *, id<MTLLibrary>> *prebuilt;
static u64 text_hash(const char *s) {
    u64 h = 0xcbf29ce484222325ull;
    for (; *s; s++) h = (h ^ (u8)*s) * 0x100000001b3ull;
    return h ? h : 1;
}
static void load_prebuilt(void) {
    const char *env = getenv("ARKHAM_SHADER_LIBS");
    NSString *dir = env ? @(env) : nil;
#if TARGET_OS_IPHONE
    if (!dir) dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/Shaders"];
#endif
    if (!dir) return;
    double t0 = CACurrentMediaTime();
    NSMutableDictionary *names = [NSMutableDictionary dictionary];
    int libs = 0;
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]) {
        if (![f hasSuffix:@".metallib"]) continue;
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:f]] error:&err];
        if (!lib) {
            fprintf(stderr, "[mtl] can't open %s: %s\n", f.UTF8String, err.localizedDescription.UTF8String);
            continue;
        }
        libs++;
        for (NSString *n in lib.functionNames) names[n] = lib;
        [lib release];  // the dictionary holds it
    }
    if (libs) {
        prebuilt = [names copy];
        fprintf(stderr, "[mtl] %lu shaders built ahead, in %d libraries, read in %.0f ms\n", (unsigned long)prebuilt.count, libs, (CACurrentMediaTime() - t0) * 1000);
    }
}
// A shader from Metal's cache takes well under a millisecond, and one compiled now about 40. Those are named, as the files in logs/msl are, to see what the prebuild missed.
static id<MTLLibrary> compile_shader(Obj *o, const char *src, int all_outputs, char fn[20]) {
    snprintf(fn, 20, "%s", o->kind == O_PS ? "ps_main" : "vs_main");
    if (prebuilt) {
        char name[20];
        snprintf(name, sizeof name, "s_%016llx", (unsigned long long)text_hash(src));
        id<MTLLibrary> lib = prebuilt[@(name)];
        if (lib) {
            memcpy(fn, name, sizeof name);
            n_prebuilt++;
            return [lib retain];
        }
    }
    u64 t0 = mach_absolute_time();
    id<MTLLibrary> lib = compile_msl(src);
    double ms = (double)(mach_absolute_time() - t0) * 125.0 / 3.0 / 1e6;
    if (ms > 5) {
        n_uncached++;
        fprintf(stderr, "[mtl] %s_%016llx%s compiled in %.0f ms, not from the cache\n", o->kind == O_PS ? "ps" : "vs", (unsigned long long)code_hash(o), all_outputs ? " with all outputs" : "", ms);
    }
    return lib;
}
static id<MTLLibrary> library_of(Obj *o) {
    if (!o->lib && o->msl) {
        o->lib = compile_shader(o, o->msl, 0, o->fn);
        n_compiled++;
        free(o->msl);
        o->msl = NULL;
    }
    return o->lib;
}
static void m_CreateVertexShader(CPU *c) { create_shader(c, O_VS, VT_VS); }
static void m_CreatePixelShader(CPU *c) { create_shader(c, O_PS, VT_PS); }
static void m_CreateQuery(CPU *c) {  // this, type, out
    u32 out = ARG(2);
    if (!out) {
        RET(ARG(1) == 8 || ARG(1) == 9 ? 0 : 0x8876086Au);
        return;
    }
    Obj *o = obj_new(O_QUERY, VT_QUERY);
    o->query_type = ARG(1);
    G32(out) = o->guest;
    RET(0);
}
static void m_AddRef(CPU *c) {
    Obj *o = obj_of(ARG(0));
    if (o && o->parent) o = o->parent;
    if (o) o->refs++;
    RET(o ? (u32)o->refs : 0);
}
static void end_pass(void);
static int surface_in_a_pass(Obj *s);
static void m_Release(CPU *c) {
    Obj *o = obj_of(ARG(0));
    if (o && o->parent) o = o->parent;  // a texture's surfaces share its count, as in Direct3D
    if (!o) {
        RET(0);
        return;
    }
    int left = --o->refs;
    if (left <= 0) {
        for (u32 i = 0; i < MAX_LEVELS * 6; i++) {
            Obj *s = o->surfaces[i] ? obj_of(o->surfaces[i]) : NULL;
            if (s) {
                if (s == rt_surface || s == ds_surface || surface_in_a_pass(s)) end_pass();
                if (s == rt_surface) rt_surface = NULL;
                if (s == ds_surface) ds_surface = NULL;
                obj_free(s);
            }
        }
        if (o == rt_surface || o == ds_surface || surface_in_a_pass(o)) end_pass();
        if (o == rt_surface) rt_surface = NULL;
        if (o == ds_surface) ds_surface = NULL;
        for (int i = 0; i < 16; i++) {
            if (cur_tex[i] == o) cur_tex[i] = NULL;
            if (streams[i].vb == o) streams[i].vb = NULL;
        }
        if (cur_ib == o) cur_ib = NULL;
        if (cur_vs == o) cur_vs = NULL;
        if (cur_ps == o) cur_ps = NULL;
        if (cur_decl == o) cur_decl = NULL;
        if (open_query == o) open_query = NULL;
        obj_free(o);
    }
    RET((u32)(left > 0 ? left : 0));
}
static void m_GetSurfaceLevel(CPU *c) {  // this, level, out
    Obj *t = obj_of(ARG(0));
    u32 level = ARG(1);
    if (!t || level >= t->levels || level >= MAX_LEVELS) {
        RET(0x8876086Cu);
        return;
    }
    if (!t->surfaces[level]) {
        Obj *s = make_surface_of(t, level, 0);
        t->surfaces[level] = s->guest;
    }
    t->refs++;
    G32(ARG(2)) = t->surfaces[level];
    RET(0);
}
static void m_SurfaceGetDesc(CPU *c) {  // this, out
    Obj *s = obj_of(ARG(0));
    u32 d = ARG(1);
    if (!s) {
        RET(0x8876086Cu);
        return;
    }
    G32(d + 0) = s->format;
    G32(d + 4) = 1;  // surface
    G32(d + 8) = s->usage;
    G32(d + 12) = (s->usage & 3) ? 0 : 1;
    G32(d + 16) = s->asked_samples ? s->asked_samples : s->samples > 1 ? (u32)s->samples : 0;
    G32(d + 20) = 0;
    G32(d + 24) = s->width;
    G32(d + 28) = s->height;
    RET(0);
}
static void m_GetBackBuffer(CPU *c) {  // this, swap chain, index, type, out
    if (back_buffer) back_buffer->refs++;
    G32(ARG(4)) = back_buffer ? back_buffer->guest : 0;
    RET(0);
}

// ---------- locking ----------
static void lock_level(CPU *c, Obj *t, u32 index, u32 level, u32 out, u32 rect, u32 flags) {
    FormatInfo fi = format_info(t->format);
    u32 w = level_dim(t->width, level), h = level_dim(t->height, level);
    u32 bw = (w + fi.block - 1) / fi.block, bh = (h + fi.block - 1) / fi.block, pitch = bw * fi.bytes;
    if (!t->locked[index]) {
        t->locked[index] = rt_guest_malloc(pitch * bh);
        // Reads get what's in the texture. Writes that only touch part of it need the rest kept.
        if ((flags & 0x10) || rect) {
            if (t->tex.storageMode == MTLStorageModeShared)
                [t->tex getBytes:GP(t->locked[index]) bytesPerRow:pitch bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level slice:index / MAX_LEVELS];
            else
                complain("a read of a render target isn't done yet");
        }
    }
    t->lock_flags[index] = flags;
    u32 at = t->locked[index];
    if (rect) at += G32(rect + 4) / fi.block * pitch + G32(rect) / fi.block * fi.bytes;
    G32(out) = pitch;
    G32(out + 4) = at;
    RET(0);
}
static void unlock_level(CPU *c, Obj *t, u32 index, u32 level) {
    u32 mem = t->locked[index];
    if (!mem) {
        RET(0);
        return;
    }
    FormatInfo fi = format_info(t->format);
    u32 w = level_dim(t->width, level), h = level_dim(t->height, level);
    u32 bw = (w + fi.block - 1) / fi.block, pitch = bw * fi.bytes;
    if (!(t->lock_flags[index] & 0x10) && t->tex.storageMode == MTLStorageModeShared) {
        [t->tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level slice:index / MAX_LEVELS withBytes:GP(mem) bytesPerRow:pitch bytesPerImage:0];
    } else if (!(t->lock_flags[index] & 0x10)) {
        complain("a write to a render target isn't done yet");
    }
    rt_guest_free(mem);
    t->locked[index] = 0;
    RET(0);
}
static void m_TexLock(CPU *c) {  // this, level, locked, rect, flags
    Obj *t = obj_of(ARG(0));
    lock_level(c, t, ARG(1), ARG(1), ARG(2), ARG(3), ARG(4));
}
static void m_TexUnlock(CPU *c) { unlock_level(c, obj_of(ARG(0)), ARG(1), ARG(1)); }
static void m_CubeLock(CPU *c) {  // this, face, level, locked, rect, flags
    Obj *t = obj_of(ARG(0));
    lock_level(c, t, ARG(1) * MAX_LEVELS + ARG(2), ARG(2), ARG(3), ARG(4), ARG(5));
}
static void m_CubeUnlock(CPU *c) { unlock_level(c, obj_of(ARG(0)), ARG(1) * MAX_LEVELS + ARG(2), ARG(2)); }
static void buffer_lock(CPU *c) {  // this, offset, size, out, flags
    Obj *b = obj_of(ARG(0));
    if (!b->lock_mirror) {
        b->lock_mirror = rt_guest_malloc(b->size ? b->size : 16);
        memcpy(GP(b->lock_mirror), b->buffer.contents, b->size);
    }
    G32(ARG(3)) = b->lock_mirror + ARG(1);
    RET(0);
}
static void buffer_unlock(CPU *c) {
    Obj *b = obj_of(ARG(0));
    if (b->lock_mirror) {
        memcpy(b->buffer.contents, GP(b->lock_mirror), b->size);
        rt_guest_free(b->lock_mirror);
        b->lock_mirror = 0;
    }
    RET(0);
}
static void m_VBDesc(CPU *c) {  // this, out: format, type, usage, pool, size, fvf
    Obj *b = obj_of(ARG(0));
    u32 d = ARG(1);
    G32(d) = 100;  // vertex data
    G32(d + 4) = 3;
    G32(d + 8) = b->usage;
    G32(d + 12) = 1;
    G32(d + 16) = b->size;
    G32(d + 20) = 0;
    RET(0);
}
static void m_IBDesc(CPU *c) {  // this, out: format, type, usage, pool, size
    Obj *b = obj_of(ARG(0));
    u32 d = ARG(1);
    G32(d) = b->index_format;
    G32(d + 4) = 4;
    G32(d + 8) = b->usage;
    G32(d + 12) = 1;
    G32(d + 16) = b->size;
    RET(0);
}

// ---------- render passes ----------
// The engine goes back and forth between the scene and a small shadow map for every shadowed light. A return to the scene would store and reload the whole multisampled target, far
// more than the shadow map costs. So the scene's encoder stays open, parked, while the shadow pass is recorded in a command buffer that runs before the scene's. For that to mean the same
// thing, each of those passes draws into its own copy of the shadow map: a light's draws in the scene read the copy made for that light. Targets bind lazily for the same reason.
static void ensure_cb(void) {
    if (!cb) {
        cb = [[queue commandBuffer] retain];
    }
}
static Obj *enc_rt, *enc_ds, *parked_rt, *parked_ds;
// Targets bind lazily, so a pass can still be open on a surface the engine has moved away from.
static int surface_in_a_pass(Obj *s) { return s == enc_rt || s == enc_ds || s == parked_rt || s == parked_ds; }
static int enc_hoisted, enc_unknown_store;
static int pass_queries, parked_queries;  // queries issued in this pass
static id<MTLRenderCommandEncoder> parked;
static EncState parked_e;
static u32 parked_draws;
static int parked_unknown_store;
static id<MTLCommandBuffer> early_cb;
static inline int target_is_current(void) { return enc && enc_rt == rt_surface && enc_ds == ds_surface; }
// ARKHAM_TRACE_FRAME=seconds: one frame after that long, every pass alone on the GPU with its time, as [fmap] lines.
static int fmap_on;
static char fmap_pass[160], fmap_parked[160];
static void commit_cb(void);
static void fmap_finish(const char *what) {
    id<MTLCommandBuffer> w = [cb retain];
    commit_cb();
    [w waitUntilCompleted];
    fprintf(stderr, "[fmap] %.3f ms: %s\n", (w.GPUEndTime - w.GPUStartTime) * 1e3, what);
    [w release];
}
static void dump_target(Obj *rt);
// Ends the current encoder only.
static void close_enc(void) {
    if (!enc) return;
    Obj *dump_rt = enc_rt;
    if (__builtin_expect(labels_on(), 0)) enc.label = [enc.label stringByAppendingFormat:@" draws=%u", pass_draws];
    if (enc_unknown_store) [enc setColorStoreAction:MTLStoreActionStore atIndex:0];
    enc_unknown_store = 0;
    [enc endEncoding];
    [enc release];
    enc = nil;
    int hoisted = enc_hoisted;
    enc_hoisted = 0;
    enc_rt = enc_ds = NULL;
    memset(&e, 0, sizeof e);
    if (__builtin_expect(fmap_on, 0)) {
        size_t n = strlen(fmap_pass);
        snprintf(fmap_pass + n, sizeof fmap_pass - n, " draws=%u queries=%d%s", pass_draws, pass_queries, hoisted ? " ahead" : "");
        pass_queries = 0;
        if (hoisted) {
            id<MTLCommandBuffer> w = early_cb;
            early_cb = nil;
            [w commit];
            [w waitUntilCompleted];
            fprintf(stderr, "[fmap] %.3f ms: %s\n", (w.GPUEndTime - w.GPUStartTime) * 1e3, fmap_pass);
            [w release];
        } else {
            fmap_finish(fmap_pass);
        }
        if (dump_rt) dump_target(dump_rt);
    }
    // The engine starts its next frame by waiting for these answers. Everything after this pass needn't hold them up.
    if (pass_queries && !hoisted) {
        pass_queries = 0;
        static int off = -1;
        if (off < 0) off = getenv("ARKHAM_MTL_LATE_QUERIES") != NULL;
        if (!off) commit_cb();
    }
}
static void resume_parked(void) {
    enc = parked;
    parked = nil;
    enc_rt = parked_rt;
    enc_ds = parked_ds;
    parked_rt = parked_ds = NULL;
    e = parked_e;
    pass_draws = parked_draws;
    enc_unknown_store = parked_unknown_store;
    enc_hoisted = 0;
    pass_queries = parked_queries;
    if (__builtin_expect(fmap_on, 0)) memcpy(fmap_pass, fmap_parked, sizeof fmap_pass);
}
static void end_pass(void) {
    if (parked) {
        close_enc();
        resume_parked();
    }
    close_enc();
}
static int cb_presents;
static void commit_cb(void) {
    end_pass();
    if (early_cb) {
        [early_cb commit];
        [early_cb release];
        early_cb = nil;
    }
    if (!cb) return;
    u64 serial = cb_serial++;
    int presents = cb_presents;
    cb_presents = 0;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (!presents) atomic_fetch_add(&gpu_ns, (u64)((done.GPUEndTime - done.GPUStartTime) * 1e9));
        atomic_store(&completed_serial, serial);
    }];
    [cb commit];
    [cb release];
    cb = nil;
}
static id<MTLTexture> make_texture(MTLTextureType type, u32 fmt, u32 w, u32 h, u32 levels, int samples, int target, id<MTLTexture> *srgb_view);
static int can_rename(Obj *s) {
    Obj *t = s->parent;
    if (!t) return 1;
    return t->levels == 1 && (t->rename_frame != frame_no || t->rename_n < MAX_RENAMES);
}
static void rename_surface(Obj *s) {
    Obj *t = s->parent;
    if (!t) return;
    if (t->rename_frame != frame_no) {
        t->rename_frame = frame_no;
        t->rename_n = 0;
    }
    u32 i = t->rename_n++;
    if (!t->phys[0]) {
        t->phys[0] = [t->tex retain];
        t->phys_view[0] = [t->view retain];
    }
    if (!t->phys[i]) {
        t->phys[i] = make_texture(MTLTextureType2D, t->format, t->width, t->height, 1, 1, 1, NULL);
        t->phys_view[i] = [t->phys[i] newTextureViewWithPixelFormat:t->phys[i].pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:format_info(t->format).swizzle];
    }
    [t->tex release];
    [t->view release];
    [s->tex release];
    t->tex = [t->phys[i] retain];
    t->view = [t->phys_view[i] retain];
    s->tex = [t->phys[i] retain];
}
static void open_encoder(int hoisted) {
    id<MTLCommandBuffer> where;
    if (hoisted) {
        if (!early_cb) early_cb = [[queue commandBuffer] retain];
        where = early_cb;
        if (rt_surface) rename_surface(rt_surface);
        if (ds_surface) rename_surface(ds_surface);
    } else {
        ensure_cb();
        where = cb;
    }
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    u32 w = 0, h = 0;
    if (rt_surface) {
        d.colorAttachments[0].texture = rt_surface->tex;
        d.colorAttachments[0].level = rt_surface->level;
        d.colorAttachments[0].slice = rt_surface->slice;
        d.colorAttachments[0].loadAction = pending_clear.color ? MTLLoadActionClear : MTLLoadActionLoad;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        d.colorAttachments[0].clearColor = MTLClearColorMake(pending_clear.rgba[0], pending_clear.rgba[1], pending_clear.rgba[2], pending_clear.rgba[3]);
        // If this surface was averaged into a texture last time, the pass can do it as it ends, without a pass of its own.
        if (rt_surface->resolve_tex) {
            d.colorAttachments[0].resolveTexture = rt_surface->resolve_tex;
            d.colorAttachments[0].resolveLevel = rt_surface->resolve_level;
            d.colorAttachments[0].resolveSlice = rt_surface->resolve_slice;
            d.colorAttachments[0].storeAction = MTLStoreActionUnknown;
            enc_unknown_store = 1;
        }
        w = rt_surface->width;
        h = rt_surface->height;
        if (!hoisted && rt_surface->parent) rt_surface->parent->written_cb = cb_serial;
    }
    if (ds_surface) {
        d.depthAttachment.texture = ds_surface->tex;
        d.depthAttachment.loadAction = pending_clear.depth ? MTLLoadActionClear : MTLLoadActionLoad;
        d.depthAttachment.storeAction = MTLStoreActionStore;
        d.depthAttachment.clearDepth = pending_clear.z;
        if (ds_surface->has_stencil) {
            d.stencilAttachment.texture = ds_surface->tex;
            d.stencilAttachment.loadAction = pending_clear.stencil ? MTLLoadActionClear : MTLLoadActionLoad;
            d.stencilAttachment.storeAction = MTLStoreActionStore;
            d.stencilAttachment.clearStencil = pending_clear.s;
            if (pending_clear.stencil) {
                ds_surface->s_known = 1;
                ds_surface->s_value = pending_clear.s;
                memset(ds_surface->s_box, 0, sizeof ds_surface->s_box);
            }
        }
        if (!w || ds_surface->width < w) w = ds_surface->width;
        if (!h || ds_surface->height < h) h = ds_surface->height;
        if (!hoisted && ds_surface->parent) ds_surface->parent->written_cb = cb_serial;
    }
    d.renderTargetWidth = w;
    d.renderTargetHeight = h;
    if (!hoisted) d.visibilityResultBuffer = visibility;
    if (__builtin_expect(fmap_on || labels_on(), 0))
        snprintf(fmap_pass, sizeof fmap_pass, "pass %ux%u x%d color=%x depth=%x clear=%c%c%c%s", w, h, rt_surface ? rt_surface->samples : ds_surface ? ds_surface->samples : 1, rt_surface ? rt_surface->format : 0,
                 ds_surface ? ds_surface->format : 0, pending_clear.color ? 'C' : '-', pending_clear.depth ? 'D' : '-', pending_clear.stencil ? 'S' : '-', hoisted ? " ahead" : "");
    pending_clear.color = pending_clear.depth = pending_clear.stencil = 0;
    enc = [[where renderCommandEncoderWithDescriptor:d] retain];
    if (__builtin_expect(labels_on(), 0)) enc.label = @(fmap_pass);
    enc_rt = rt_surface;
    enc_ds = ds_surface;
    enc_hoisted = hoisted;
    n_passes++;
    pass_draws = 0;
    [enc setFrontFacingWinding:MTLWindingClockwise];
    // A query that spans passes keeps only what the last of them counted. The engine keeps its queries inside one.
    if (open_query && !hoisted) [enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:open_query->slot * 8];
}
static int hoist_off = -1;
static inline int target_can_go_ahead(void) {
    if (hoist_off < 0) hoist_off = getenv("ARKHAM_MTL_NO_AHEAD") != NULL;
    return !hoist_off && ds_surface && ds_surface->format == 77 && ds_surface->parent && rt_surface && can_rename(ds_surface) && can_rename(rt_surface);
}
// Makes the current encoder the one for the current target.
static void begin_pass(void) {
    if (target_is_current()) return;
    if (parked) {
        close_enc();
        if (parked_rt == rt_surface && parked_ds == ds_surface) {
            resume_parked();
            return;
        }
        if (target_can_go_ahead()) {
            open_encoder(1);
            return;
        }
        resume_parked();
        close_enc();
    } else if (enc) {
        if (enc_rt && enc_rt->samples > 1 && target_can_go_ahead()) {
            parked = enc;
            parked_rt = enc_rt;
            parked_ds = enc_ds;
            parked_e = e;
            parked_draws = pass_draws;
            parked_unknown_store = enc_unknown_store;
            parked_queries = pass_queries;
            pass_queries = 0;
            if (__builtin_expect(fmap_on, 0)) memcpy(fmap_parked, fmap_pass, sizeof fmap_pass);
            enc = nil;
            enc_unknown_store = 0;
            memset(&e, 0, sizeof e);
            open_encoder(1);
            return;
        }
        close_enc();
    }
    open_encoder(0);
}
static void flush_clears(void) {
    if (pending_clear.color || pending_clear.depth || pending_clear.stencil) begin_pass();
}
static void clear_by_drawing(u32 flags, const u32 *area, const float *rgba, float z, u32 stencil, u32 n_rects, u32 rects);
static void m_SetRenderTarget(CPU *c) {  // this, index, surface
    if (ARG(1) != 0) {
        RET(0);
        return;
    }
    Obj *s = obj_of(ARG(2));
    if (s != rt_surface) {
        // A clear of only depth and stencil doesn't depend on the color target. The engine sets another target, clears depth and stencil, and sets the scene back before
        // averaging it: done here, that clear ended the scene's pass, and the average then needed a pass of its own that loads the whole 4x target again. So when the new
        // target is the same size, the clear waits for it: drawn into its pass if that's open, or as that pass's load action.
        int ds_only = !pending_clear.color && (pending_clear.depth || pending_clear.stencil);
        int parked_here = parked && parked_rt == s && parked_ds == ds_surface;
        if (ds_only && s && rt_surface && ds_surface && !parked_here && s->width == rt_surface->width && s->height == rt_surface->height && s->samples == rt_surface->samples) {
            rt_surface = s;
            if (target_is_current()) {
                u32 area[4] = {0, 0, s->width, s->height};
                float none[4] = {0, 0, 0, 0};
                u32 flags = (pending_clear.depth ? 2u : 0u) | (pending_clear.stencil && ds_surface->has_stencil ? 4u : 0u);
                if (flags) clear_by_drawing(flags, area, none, pending_clear.z, pending_clear.s, 0, 0);
                n_quad_clears++;
                if (flags & 4) {
                    ds_surface->s_known = 1;
                    ds_surface->s_value = pending_clear.s;
                    memset(ds_surface->s_box, 0, 16);
                }
                pending_clear.depth = pending_clear.stencil = 0;
            }
        } else {
            flush_clears();
            rt_surface = s;
        }
    }
    // Setting a render target resets the viewport to all of it, and scissoring with it.
    if (s) {
        vp[0] = vp[1] = 0;
        vp[2] = s->width;
        vp[3] = s->height;
        vp_z[0] = 0;
        vp_z[1] = 1;
        scissor[0] = scissor[1] = 0;
        scissor[2] = s->width;
        scissor[3] = s->height;
    }
    RET(0);
}
static void m_SetDepthStencilSurface(CPU *c) {  // this, surface
    Obj *s = obj_of(ARG(1));
    if (s != ds_surface) {
        flush_clears();
        ds_surface = s;
    }
    RET(0);
}
static void m_SetViewport(CPU *c) {  // this, viewport: x, y, width, height, min z, max z
    u32 p = ARG(1);
    vp[0] = G32(p);
    vp[1] = G32(p + 4);
    vp[2] = G32(p + 8);
    vp[3] = G32(p + 12);
    memcpy(&vp_z[0], GP(p + 16), 4);
    memcpy(&vp_z[1], GP(p + 20), 4);
    RET(0);
}
static void m_SetScissorRect(CPU *c) {  // this, rect
    u32 p = ARG(1);
    scissor[0] = G32(p);
    scissor[1] = G32(p + 4);
    scissor[2] = G32(p + 8);
    scissor[3] = G32(p + 12);
    RET(0);
}
static void m_SetClipPlane(CPU *c) {  // this, index, plane
    if (ARG(1) < 2) memcpy(clip_planes[ARG(1)], GP(ARG(2)), 16);
    RET(0);
}
static void m_SetRenderState(CPU *c) {
    if (ARG(1) < 256) rs[ARG(1)] = ARG(2);
    RET(0);
}
static void m_SetSamplerState(CPU *c) {
    if (ARG(1) < 16 && ARG(2) < 16) ss[ARG(1)][ARG(2)] = ARG(3);
    RET(0);
}
static void m_SetTexture(CPU *c) {
    if (ARG(1) < 16) cur_tex[ARG(1)] = obj_of(ARG(2));
    RET(0);
}
static void m_SetVertexShader(CPU *c) {
    cur_vs = obj_of(ARG(1));
    RET(0);
}
static void m_SetPixelShader(CPU *c) {
    cur_ps = obj_of(ARG(1));
    RET(0);
}
static void m_SetVSConst(CPU *c) {  // this, start, data, count
    u32 start = ARG(1), n = ARG(3);
    if (start < 256) {
        if (start + n > 256) n = 256 - start;
        memcpy(vs_const[start], GP(ARG(2)), (size_t)n * 16);
        vs_const_dirty = 1;
    }
    RET(0);
}
static void m_SetPSConst(CPU *c) {
    u32 start = ARG(1), n = ARG(3);
    if (start < 224) {
        if (start + n > 224) n = 224 - start;
        memcpy(ps_const[start], GP(ARG(2)), (size_t)n * 16);
        ps_const_dirty = 1;
    }
    RET(0);
}
static void m_SetVertexDeclaration(CPU *c) {
    cur_decl = obj_of(ARG(1));
    RET(0);
}
static void m_SetStreamSource(CPU *c) {  // this, stream, buffer, offset, stride
    if (ARG(1) < 16) {
        streams[ARG(1)].vb = obj_of(ARG(2));
        streams[ARG(1)].offset = ARG(3);
        streams[ARG(1)].stride = ARG(4);
    }
    RET(0);
}
static void m_Nothing(CPU *c) { RET(0); }
static u32 stream_freq[16];
static void m_SetStreamSourceFreq(CPU *c) {  // this, stream, setting
    if (ARG(1) < 16) stream_freq[ARG(1)] = ARG(2) == 1 ? 0 : ARG(2);
    RET(0);
}
static void m_SetIndices(CPU *c) {
    cur_ib = obj_of(ARG(1));
    RET(0);
}

// ---------- pipelines ----------
// Shaders and declarations are in the key by uid: a freed object's address comes back for the next one, and the key would then find a pipeline built from the old one.
typedef struct {
    u64 vs, ps, decl;
    u32 strides[3], instance_rate[3];
    u32 color, depth, samples;
    u32 blend, src, dst, op, src_a, dst_a, op_a, mask;
    u8 alpha, clip;
} PipeKey;
typedef struct {
    PipeKey key;
    id<MTLRenderPipelineState> state;
} PipeEntry;
#define PIPES (1u << 16)
static PipeEntry *pipes;
static u32 n_pipes;
static u64 pipe_ticks;
static u64 hash_bytes(const void *p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    const u8 *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
static MTLBlendFactor blend_factor(u32 v) {
    switch (v) {
        case 1: return MTLBlendFactorZero;
        case 2: return MTLBlendFactorOne;
        case 3: return MTLBlendFactorSourceColor;
        case 4: return MTLBlendFactorOneMinusSourceColor;
        case 5: return MTLBlendFactorSourceAlpha;
        case 6: return MTLBlendFactorOneMinusSourceAlpha;
        case 7: return MTLBlendFactorDestinationAlpha;
        case 8: return MTLBlendFactorOneMinusDestinationAlpha;
        case 9: return MTLBlendFactorDestinationColor;
        case 10: return MTLBlendFactorOneMinusDestinationColor;
        case 11: return MTLBlendFactorSourceAlphaSaturated;
        case 14: return MTLBlendFactorBlendColor;
        case 15: return MTLBlendFactorOneMinusBlendColor;
        default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation blend_op(u32 v) {
    switch (v) {
        case 2: return MTLBlendOperationSubtract;
        case 3: return MTLBlendOperationReverseSubtract;
        case 4: return MTLBlendOperationMin;
        case 5: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
    }
}
// A pixel shader can read an output its vertex shader doesn't write, and Metal won't link that. Such a vertex shader gets a second translation that writes all ten, the extra ones as
// (0, 0, 0, 1), and one is enough for any pixel shader, since a vertex shader may write more than is read. Before, each combination got its own, which tools/prebuild_shaders.sh
// couldn't know about, and those were most of what stalled in play.
static id<MTLLibrary> vs_library_for(Obj *vs, u32 needs, const char **fn) {
    if (!(needs & ~vs->info.varyings)) {
        id<MTLLibrary> lib = library_of(vs);
        *fn = vs->fn;
        return lib;
    }
    if (!vs->all_outputs) {
        D3dShaderInfo info;
        char *msl = d3dsh_translate(vs->code, D3DSH_ALL_VARYINGS, &info);
        vs->all_outputs = msl ? compile_shader(vs, msl, 1, vs->all_fn) : nil;
        free(msl);
    }
    *fn = vs->all_fn;
    return vs->all_outputs;
}
static u64 n_pipe_waits;  // draws left out a frame because their pipeline was still being built
#if TARGET_OS_IPHONE
// On the phone a pipeline the system hasn't compiled before takes the GPU's compiler 4 ms on average and up to a hundred, and a new place asks for
// dozens at once: the game stopped for a third of a second at a time. So a new one is built off the D3D thread, several at once, and the frame waits
// for it only briefly: one compiled before comes back from the system's cache in a fraction of a millisecond. One that doesn't come in time leaves its
// draw out of that frame (a thing appears a frame or a few later, the first time a place is seen), and is there from the next frame it's ready.
// The settings can turn this off (rt_set_async_pipelines), and so can ARKHAM_SYNC_PIPELINES=1.
static _Atomic int async_pipelines = -1;
static u64 frame_wait_ns;
#define PIPE_WAIT_NS 2000000ull        // for one pipeline
#define PIPE_FRAME_WAIT_NS 4000000ull  // for all of a frame's
void rt_set_async_pipelines(int on) { atomic_store(&async_pipelines, on && !getenv("ARKHAM_SYNC_PIPELINES")); }
static id<MTLRenderPipelineState> build_in_background(PipeEntry *e, MTLRenderPipelineDescriptor *d, MTLFunctionConstantValues *cv, id<MTLLibrary> vlib,
                                                      NSString *vname, id<MTLLibrary> plib, NSString *pname, id<MTLFunction> fixed_ps) {
    // Three at a time, at utility priority: on the phone's efficiency cores, so that a new place's hundreds don't take the CPU from the game and its sound
    // (dozens at once at a higher priority left gaps in the sound).
    static dispatch_queue_t qs[3];
    static unsigned next_q;
    if (!qs[0])
        for (int i = 0; i < 3; i++) qs[i] = dispatch_queue_create("arkham.pipelines", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    dispatch_queue_t q = qs[next_q++ % 3];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(q, ^{
        void *pool = objc_autoreleasePoolPush();
        u64 t0 = mach_absolute_time();
        NSError *err = nil;
        d.vertexFunction = [[vlib newFunctionWithName:vname constantValues:cv error:&err] autorelease];
        if (!d.vertexFunction) fprintf(stderr, "[mtl] vertex function: %s\n", err.localizedDescription.UTF8String);
        if (fixed_ps) {
            d.fragmentFunction = fixed_ps;
        } else if (plib) {
            d.fragmentFunction = [[plib newFunctionWithName:pname constantValues:cv error:&err] autorelease];
            if (!d.fragmentFunction) fprintf(stderr, "[mtl] fragment function: %s\n", err.localizedDescription.UTF8String);
        }
        id<MTLRenderPipelineState> st = d.vertexFunction ? [dev newRenderPipelineStateWithDescriptor:d error:&err] : nil;
        if (!st && d.vertexFunction) fprintf(stderr, "[mtl] pipeline: %s\n", err.localizedDescription.UTF8String);
        __atomic_add_fetch(&pipe_ticks, mach_absolute_time() - t0, __ATOMIC_RELAXED);
        __atomic_store_n((void **)&e->state, (void *)st, __ATOMIC_RELEASE);
        dispatch_semaphore_signal(done);
        objc_autoreleasePoolPop(pool);
    });
    u64 left = frame_wait_ns < PIPE_FRAME_WAIT_NS ? PIPE_FRAME_WAIT_NS - frame_wait_ns : 0, t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (left) dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)MIN(left, PIPE_WAIT_NS)));
    frame_wait_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    dispatch_release(done);
    id<MTLRenderPipelineState> st = (id)__atomic_load_n((void **)&e->state, __ATOMIC_ACQUIRE);
    if (!st) n_pipe_waits++;
    return st;
}
#endif
static id<MTLRenderPipelineState> pipeline_for(const PipeKey *k, Obj *vs, Obj *ps, Obj *decl) {
    if (!pipes) pipes = calloc(PIPES, sizeof *pipes);
#if TARGET_OS_IPHONE
    if (atomic_load(&async_pipelines) < 0) atomic_store(&async_pipelines, !getenv("ARKHAM_SYNC_PIPELINES"));
#endif
    u64 h = hash_bytes(k, sizeof *k);
    for (u32 i = (u32)h % PIPES, tries = 0; tries < PIPES; i = (i + 1) % PIPES, tries++) {
        if (!pipes[i].state && !pipes[i].key.vs) {
            // Not built yet.
            if (!vs || !decl) return nil;
            MTLFunctionConstantValues *cv = [[MTLFunctionConstantValues new] autorelease];
            bool clip = k->clip, alpha = k->alpha;
            [cv setConstantValue:&clip type:MTLDataTypeBool atIndex:0];
            [cv setConstantValue:&alpha type:MTLDataTypeBool atIndex:1];
            NSError *err = nil;
            MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor new] autorelease];
            const char *vfn = "vs_main";
            id<MTLLibrary> vlib = vs_library_for(vs, ps ? ps->info.varyings : 0, &vfn);
            NSString *vname = @(vfn), *pname = nil;
            // ARKHAM_MTL_CHEAP_PS=1: every pixel shader replaced by one color, so the GPU has almost nothing to shade and a run shows what limits the CPU side.
            static int cheap = -1;
            static id<MTLFunction> cheap_fn;
            if (cheap < 0) {
                cheap = getenv("ARKHAM_MTL_CHEAP_PS") != NULL;
                if (cheap) cheap_fn = [compile_msl("#include <metal_stdlib>\nfragment float4 cheap_ps() { return float4(0.5, 0.5, 0.5, 1.0); }\n") newFunctionWithName:@"cheap_ps"];
            }
            id<MTLFunction> fixed_ps = nil;
            id<MTLLibrary> plib = nil;
            if (cheap && ps) {
                fixed_ps = cheap_fn;
            } else if (ps && library_of(ps)) {
                plib = ps->lib;
                pname = @(ps->fn);
            }
            MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
            u32 buffers_used = 0;
            for (int n = 0; n < decl->n_elements; n++) {
                const Element *el = &decl->elements[n];
                int slot = d3dsh_attribute_slot(el->usage, el->index);
                if (slot < 0 || !(vs->info.attributes >> slot & 1)) continue;
                vd.attributes[slot].format = (MTLVertexFormat)decl->attr_format[slot];
                vd.attributes[slot].offset = el->offset;
                vd.attributes[slot].bufferIndex = el->stream;
                buffers_used |= 1u << el->stream;
            }
            for (int s = 0; s < 3; s++) {
                if (!(buffers_used >> s & 1)) continue;
                vd.layouts[s].stride = k->strides[s] ? k->strides[s] : 16;
                if (!k->strides[s]) {
                    vd.layouts[s].stepFunction = MTLVertexStepFunctionConstant;
                    vd.layouts[s].stepRate = 0;
                } else if (k->instance_rate[s]) {
                    vd.layouts[s].stepFunction = MTLVertexStepFunctionPerInstance;
                    vd.layouts[s].stepRate = k->instance_rate[s];
                } else {
                    vd.layouts[s].stepFunction = MTLVertexStepFunctionPerVertex;
                    vd.layouts[s].stepRate = 1;
                }
            }
            // A shader may read what the declaration doesn't have: it gets (0, 0, 0, 1).
            for (int slot = 0; slot < 16; slot++) {
                if ((vs->info.attributes >> slot & 1) && !(decl->slots >> slot & 1)) {
                    vd.attributes[slot].format = MTLVertexFormatFloat4;
                    vd.attributes[slot].offset = 0;
                    vd.attributes[slot].bufferIndex = 15;
                    buffers_used |= 1u << 15;
                }
            }
            if (buffers_used >> 15 & 1) {
                vd.layouts[15].stride = 16;
                vd.layouts[15].stepFunction = MTLVertexStepFunctionConstant;
                vd.layouts[15].stepRate = 0;
            }
            d.vertexDescriptor = vd;
            d.rasterSampleCount = k->samples;
            if (k->color) {
                d.colorAttachments[0].pixelFormat = (MTLPixelFormat)k->color;
                d.colorAttachments[0].blendingEnabled = k->blend != 0;
                d.colorAttachments[0].sourceRGBBlendFactor = blend_factor(k->src);
                d.colorAttachments[0].destinationRGBBlendFactor = blend_factor(k->dst);
                d.colorAttachments[0].rgbBlendOperation = blend_op(k->op);
                d.colorAttachments[0].sourceAlphaBlendFactor = blend_factor(k->src_a);
                d.colorAttachments[0].destinationAlphaBlendFactor = blend_factor(k->dst_a);
                d.colorAttachments[0].alphaBlendOperation = blend_op(k->op_a);
                MTLColorWriteMask m = MTLColorWriteMaskNone;
                if (k->mask & 1) m |= MTLColorWriteMaskRed;
                if (k->mask & 2) m |= MTLColorWriteMaskGreen;
                if (k->mask & 4) m |= MTLColorWriteMaskBlue;
                if (k->mask & 8) m |= MTLColorWriteMaskAlpha;
                d.colorAttachments[0].writeMask = m;
            }
            if (k->depth) {
                d.depthAttachmentPixelFormat = (MTLPixelFormat)k->depth;
                if (k->depth == MTLPixelFormatDepth32Float_Stencil8) d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->depth;
            }
            pipes[i].key = *k;  // taken: until it's built, a draw that needs it is left out
            n_pipes++;
#if TARGET_OS_IPHONE
            if (atomic_load(&async_pipelines)) return build_in_background(&pipes[i], d, cv, vlib, vname, plib, pname, fixed_ps);
#endif
            u64 pipe_t0 = mach_absolute_time();
            d.vertexFunction = [[vlib newFunctionWithName:vname constantValues:cv error:&err] autorelease];
            if (!d.vertexFunction) fprintf(stderr, "[mtl] vertex function: %s\n", err.localizedDescription.UTF8String);
            if (fixed_ps) {
                d.fragmentFunction = fixed_ps;
            } else if (plib) {
                d.fragmentFunction = [[plib newFunctionWithName:pname constantValues:cv error:&err] autorelease];
                if (!d.fragmentFunction) fprintf(stderr, "[mtl] fragment function: %s\n", err.localizedDescription.UTF8String);
            }
            id<MTLRenderPipelineState> st = [dev newRenderPipelineStateWithDescriptor:d error:&err];
            pipe_ticks += mach_absolute_time() - pipe_t0;
            if (!st) fprintf(stderr, "[mtl] pipeline: %s\n", err.localizedDescription.UTF8String);
            pipes[i].state = st;
            return st;
        }
        if (!memcmp(&pipes[i].key, k, sizeof *k)) {
            id<MTLRenderPipelineState> st = (id)__atomic_load_n((void **)&pipes[i].state, __ATOMIC_ACQUIRE);
            if (!st) n_pipe_waits++;
            return st;
        }
    }
    return nil;
}

// Depth and stencil states.
typedef struct {
    u8 z, zwrite, zfunc, stencil, two, f, zf, p, fn, cf, czf, cp, cfn;
    u8 read, write;
} DepthKey;
typedef struct {
    DepthKey key;
    id<MTLDepthStencilState> state;
} DepthEntry;
#define DEPTHS 1024u
static DepthEntry depths[DEPTHS];
static MTLCompareFunction compare_of(u32 v) { return v >= 1 && v <= 8 ? (MTLCompareFunction)(v - 1) : MTLCompareFunctionAlways; }
static MTLStencilOperation stencil_op(u32 v) {
    switch (v) {
        case 2: return MTLStencilOperationZero;
        case 3: return MTLStencilOperationReplace;
        case 4: return MTLStencilOperationIncrementClamp;
        case 5: return MTLStencilOperationDecrementClamp;
        case 6: return MTLStencilOperationInvert;
        case 7: return MTLStencilOperationIncrementWrap;
        case 8: return MTLStencilOperationDecrementWrap;
        default: return MTLStencilOperationKeep;
    }
}
static id<MTLDepthStencilState> depth_state(void) {
    DepthKey k;
    memset(&k, 0, sizeof k);
    k.z = rs[7] != 0;
    k.zwrite = k.z && rs[14] != 0;
    k.zfunc = k.z ? (u8)rs[23] : 8;
    int have_stencil = ds_surface && ds_surface->has_stencil;
    k.stencil = rs[52] != 0 && have_stencil;
    if (k.stencil) {
        k.two = rs[185] != 0;
        k.f = (u8)rs[53];
        k.zf = (u8)rs[54];
        k.p = (u8)rs[55];
        k.fn = (u8)rs[56];
        k.read = (u8)rs[58];
        k.write = (u8)rs[59];
        if (k.two) {
            k.cf = (u8)rs[186];
            k.czf = (u8)rs[187];
            k.cp = (u8)rs[188];
            k.cfn = (u8)rs[189];
        }
    }
    u64 h = hash_bytes(&k, sizeof k);
    for (u32 i = (u32)h % DEPTHS, tries = 0; tries < DEPTHS; i = (i + 1) % DEPTHS, tries++) {
        if (!depths[i].state) {
            MTLDepthStencilDescriptor *d = [[MTLDepthStencilDescriptor new] autorelease];
            d.depthCompareFunction = k.z ? compare_of(k.zfunc) : MTLCompareFunctionAlways;
            d.depthWriteEnabled = k.zwrite;
            if (k.stencil) {
                MTLStencilDescriptor *fr = [[MTLStencilDescriptor new] autorelease];
                fr.stencilFailureOperation = stencil_op(k.f);
                fr.depthFailureOperation = stencil_op(k.zf);
                fr.depthStencilPassOperation = stencil_op(k.p);
                fr.stencilCompareFunction = compare_of(k.fn);
                fr.readMask = k.read;
                fr.writeMask = k.write;
                d.frontFaceStencil = fr;
                if (k.two) {
                    MTLStencilDescriptor *bk = [[MTLStencilDescriptor new] autorelease];
                    bk.stencilFailureOperation = stencil_op(k.cf);
                    bk.depthFailureOperation = stencil_op(k.czf);
                    bk.depthStencilPassOperation = stencil_op(k.cp);
                    bk.stencilCompareFunction = compare_of(k.cfn);
                    bk.readMask = k.read;
                    bk.writeMask = k.write;
                    d.backFaceStencil = bk;
                } else {
                    d.backFaceStencil = fr;
                }
            }
            depths[i].key = k;
            depths[i].state = [dev newDepthStencilStateWithDescriptor:d];
            return depths[i].state;
        }
        if (!memcmp(&depths[i].key, &k, sizeof k)) return depths[i].state;
    }
    return nil;
}

// Samplers.
typedef struct {
    u8 u, v, w, mag, min, mip, aniso, compare;
    u32 max_mip;
} SampKey;
typedef struct {
    SampKey key;
    id<MTLSamplerState> state;
} SampEntry;
#define SAMPS 1024u
static SampEntry samps[SAMPS];
static MTLSamplerAddressMode address_of(u32 v) {
    switch (v) {
        case 2: return MTLSamplerAddressModeMirrorRepeat;
        case 3: return MTLSamplerAddressModeClampToEdge;
        case 4: return MTLSamplerAddressModeClampToZero;
        case 5: return MTLSamplerAddressModeMirrorClampToEdge;
        default: return MTLSamplerAddressModeRepeat;
    }
}
static id<MTLSamplerState> sampler_for(u32 n, int compare) {
    SampKey k;
    memset(&k, 0, sizeof k);
    k.u = (u8)ss[n][1];
    k.v = (u8)ss[n][2];
    k.w = (u8)ss[n][3];
    k.mag = (u8)ss[n][5];
    k.min = (u8)ss[n][6];
    k.mip = (u8)ss[n][7];
    k.aniso = k.min == 3 || k.mag == 3 ? (u8)ss[n][10] : 1;
    k.compare = (u8)compare;
    k.max_mip = ss[n][9];
    u64 h = hash_bytes(&k, sizeof k);
    for (u32 i = (u32)h % SAMPS, tries = 0; tries < SAMPS; i = (i + 1) % SAMPS, tries++) {
        if (!samps[i].state) {
            MTLSamplerDescriptor *d = [[MTLSamplerDescriptor new] autorelease];
            d.sAddressMode = address_of(k.u);
            d.tAddressMode = address_of(k.v);
            d.rAddressMode = address_of(k.w);
            d.magFilter = k.mag >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            d.minFilter = k.min >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            d.mipFilter = k.mip == 0 ? MTLSamplerMipFilterNotMipmapped : k.mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
            d.maxAnisotropy = k.aniso > 1 ? k.aniso : 1;
            d.lodMinClamp = (float)k.max_mip;
            if (compare) d.compareFunction = MTLCompareFunctionLessEqual;
            samps[i].key = k;
            samps[i].state = [dev newSamplerStateWithDescriptor:d];
            return samps[i].state;
        }
        if (!memcmp(&samps[i].key, &k, sizeof k)) return samps[i].state;
    }
    return nil;
}

// ---------- drawing ----------
typedef struct {
    float half_pixel[4];
    float clip[2][4];
    float attr_scale[16][4];
} VsExtra;
typedef struct {
    float lod_bias[4][4];
    float alpha_ref;
    int alpha_func;
    float pad[2];
} PsExtra;

// Space for this frame's constants and draws from memory. A frame that fills its buffer goes on in another of the same size, kept for the next time this frame's turn comes: starting
// again from the front would overwrite what the GPU hasn't read yet. Command buffers keep what they use alive, so a draw too big for any of them gets a buffer of its own.
static u32 transient_alloc(u32 size, void **out, id<MTLBuffer> *buf) {
    if (size > TRANSIENT_SIZE) {
        complain("a draw from memory bigger than the upload space");
        id<MTLBuffer> b = [[dev newBufferWithLength:size options:MTLResourceStorageModeShared] autorelease];
        *buf = b;
        *out = b.contents;
        return 0;
    }
    u32 at = (transient_at + 255u) & ~255u;
    if (at + size > TRANSIENT_SIZE) {
        if (transient_link + 1 < TRANSIENT_CHAIN) {
            transient_link++;
            if (!transient[transient_frame][transient_link]) {
                transient[transient_frame][transient_link] = [dev newBufferWithLength:TRANSIENT_SIZE options:MTLResourceStorageModeShared];
                fprintf(stderr, "[mtl] a frame needed more upload space: %u MB\n", (transient_link + 1) * (TRANSIENT_SIZE >> 20));
            }
        } else {
            complain("ran out of upload space in a frame");
            transient_link = 0;
        }
        at = 0;
    }
    transient_at = at + size;
    id<MTLBuffer> b = transient[transient_frame][transient_link];
    *buf = b;
    *out = (u8 *)b.contents + at;
    return at;
}
static inline void stencil_touch(Obj *ds, const u32 *a) {
    if (a[2] <= a[0] || a[3] <= a[1]) return;
    u32 *b = ds->s_box;
    if (b[2] <= b[0] || b[3] <= b[1]) {
        memcpy(b, a, 16);
        return;
    }
    if (a[0] < b[0]) b[0] = a[0];
    if (a[1] < b[1]) b[1] = a[1];
    if (a[2] > b[2]) b[2] = a[2];
    if (a[3] > b[3]) b[3] = a[3];
}
static void trace_draw(u32 type, u32 count);
// Hardware instancing, as SetStreamSourceFreq sets it up: the stream marked as indexed data says how many instances, and a stream marked as instance data moves on once every so many
// instances instead of once a vertex. Only indexed draws can do it.
#define FREQ_INDEXED 0x40000000u
#define FREQ_INSTANCE 0x80000000u
static int instancing(void) {
    for (int s = 0; s < 3; s++)
        if (stream_freq[s] & (FREQ_INDEXED | FREQ_INSTANCE)) return 1;
    return 0;
}
static u32 instance_count(void) {
    for (int s = 0; s < 3; s++)
        if (stream_freq[s] & FREQ_INDEXED) return (stream_freq[s] & 0x3FFFFFFFu) ? stream_freq[s] & 0x3FFFFFFFu : 1;
    return 1;
}
static u32 instance_rate(int s) {
    if (!(stream_freq[s] & FREQ_INSTANCE)) return 0;
    return (stream_freq[s] & 0x3FFFFFFFu) ? stream_freq[s] & 0x3FFFFFFFu : 1;
}
static int prepare_draw(u32 *strides_used, int instanced) {
    if (!cur_vs || !cur_decl) {
        complain("a draw without a vertex shader or declaration");
        return 0;
    }
    if (!rt_surface && !ds_surface) {
        complain("a draw without a target");
        return 0;
    }
    begin_pass();
    pass_draws++;
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = cur_vs->uid;
    k.ps = cur_ps ? cur_ps->uid : 0;
    k.decl = cur_decl->uid;
    for (int s = 0; s < 3; s++) {
        k.strides[s] = strides_used[s];
        k.instance_rate[s] = instanced ? instance_rate(s) : 0;
    }
    k.color = rt_surface ? (u32)rt_surface->tex.pixelFormat : 0;
    k.depth = ds_surface ? (u32)ds_surface->tex.pixelFormat : 0;
    k.samples = rt_surface ? (u32)rt_surface->samples : ds_surface ? (u32)ds_surface->samples : 1;
    if (rs[27]) {
        k.blend = 1;
        k.src = rs[19];
        k.dst = rs[20];
        k.op = rs[171];
        k.src_a = rs[206] ? rs[207] : rs[19];
        k.dst_a = rs[206] ? rs[208] : rs[20];
        k.op_a = rs[206] ? rs[209] : rs[171];
    }
    k.mask = rs[168] & 15;
    k.alpha = rs[15] != 0 && rs[25] != 8;
    k.clip = (rs[152] & 3) != 0;
    id<MTLRenderPipelineState> p = pipeline_for(&k, cur_vs, cur_ps, cur_decl);
    if (!p) return 0;
    if (p != e.pipeline) {
        [enc setRenderPipelineState:p];
        e.pipeline = p;
    }
    id<MTLDepthStencilState> ds = depth_state();
    if (ds != e.depth) {
        [enc setDepthStencilState:ds];
        e.depth = ds;
    }
    if (!e.have_stencil || e.stencil_ref != rs[57]) {
        [enc setStencilReferenceValue:rs[57]];
        e.stencil_ref = rs[57];
        e.have_stencil = 1;
    }
    u32 cull = rs[22];
    if (!e.have_cull || e.cull != cull) {
        [enc setCullMode:cull == 2 ? MTLCullModeFront : cull == 3 ? MTLCullModeBack : MTLCullModeNone];
        e.cull = cull;
        e.have_cull = 1;
    }
    float bias, slope;
    memcpy(&bias, &rs[195], 4);
    memcpy(&slope, &rs[175], 4);
    if (!e.have_bias || e.bias != bias || e.slope != slope) {
        // The scale Feral's layer gives glPolygonOffset. The game's values were chosen for it: as plain depth, as the Direct3D documentation has them, they're 34 times too strong,
        // and Batman's cape and cowl come through each other at the neck.
        [enc setDepthBias:bias * 500000.0f slopeScale:slope clamp:0];
        e.bias = bias;
        e.slope = slope;
        e.have_bias = 1;
    }
    if (rs[27] && (rs[19] == 14 || rs[19] == 15 || rs[20] == 14 || rs[20] == 15 || (rs[206] && (rs[207] == 14 || rs[207] == 15 || rs[208] == 14 || rs[208] == 15)))) {
        u32 v = rs[193];
        [enc setBlendColorRed:((v >> 16) & 255) / 255.0f green:((v >> 8) & 255) / 255.0f blue:(v & 255) / 255.0f alpha:(v >> 24) / 255.0f];
        complain("the game blends with a constant color");
    }
    // A draw with no depth test can still lose everything to clipping: the game's full-screen effects sit at depth exactly 0.
    int clamp_depth = !rs[7];
    if (e.clamp_depth != clamp_depth) {
        [enc setDepthClipMode:clamp_depth ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
        e.clamp_depth = clamp_depth;
    }
    u32 tw = rt_surface ? rt_surface->width : ds_surface ? ds_surface->width : 1, th = rt_surface ? rt_surface->height : ds_surface ? ds_surface->height : 1;
    if (!e.have_vp || memcmp(e.vp, vp, sizeof vp) || memcmp(e.vpz, vp_z, sizeof vp_z)) {
        // The game's full-screen effects use a viewport from -1,-1. Direct3D has these as unsigned, and OpenGL takes them as signed.
        MTLViewport v = {(int)vp[0], (int)vp[1], vp[2], vp[3], vp_z[0], vp_z[1]};
        [enc setViewport:v];
        memcpy(e.vp, vp, sizeof vp);
        memcpy(e.vpz, vp_z, sizeof vp_z);
        e.have_vp = 1;
    }
    u32 sc[4] = {0, 0, tw, th};
    if (rs[174]) {
        sc[0] = scissor[0] < tw ? scissor[0] : tw;
        sc[1] = scissor[1] < th ? scissor[1] : th;
        sc[2] = scissor[2] < tw ? scissor[2] : tw;
        sc[3] = scissor[3] < th ? scissor[3] : th;
    }
    if (!e.have_sc || memcmp(e.sc, sc, sizeof sc)) {
        MTLScissorRect r = {sc[0], sc[1], sc[2] > sc[0] ? sc[2] - sc[0] : 0, sc[3] > sc[1] ? sc[3] - sc[1] : 0};
        [enc setScissorRect:r];
        memcpy(e.sc, sc, sizeof sc);
        e.have_sc = 1;
    }
    // Constants.
    if (vs_const_dirty) {
        void *mem;
        vs_const_at = transient_alloc(256 * 16, &mem, &vs_const_buf);
        memcpy(mem, vs_const, 256 * 16);
        vs_const_dirty = 0;
    }
    if (e.vs_buf != vs_const_buf) {
        [enc setVertexBuffer:vs_const_buf offset:vs_const_at atIndex:16];
        e.vs_buf = vs_const_buf;
        e.vs_off = vs_const_at;
    } else if (e.vs_off != vs_const_at) {
        [enc setVertexBufferOffset:vs_const_at atIndex:16];
        e.vs_off = vs_const_at;
    }
    if (cur_ps) {
        if (ps_const_dirty) {
            void *mem;
            ps_const_at = transient_alloc(224 * 16, &mem, &ps_const_buf);
            memcpy(mem, ps_const, 224 * 16);
            ps_const_dirty = 0;
        }
        if (e.ps_buf != ps_const_buf) {
            [enc setFragmentBuffer:ps_const_buf offset:ps_const_at atIndex:0];
            e.ps_buf = ps_const_buf;
            e.ps_off = ps_const_at;
        } else if (e.ps_off != ps_const_at) {
            [enc setFragmentBufferOffset:ps_const_at atIndex:0];
            e.ps_off = ps_const_at;
        }
    }
    VsExtra vx;
    memset(&vx, 0, sizeof vx);
    // Direct3D 9 puts a pixel's center at a whole coordinate and Metal half a pixel further on, so what Direct3D draws at x is at x + 0.5 here.
    vx.half_pixel[0] = vp[2] ? 1.0f / (float)vp[2] : 0;
    vx.half_pixel[1] = vp[3] ? -1.0f / (float)vp[3] : 0;
    memcpy(vx.clip, clip_planes, sizeof vx.clip);
    memcpy(vx.attr_scale, cur_decl->attr_scale, sizeof vx.attr_scale);
    [enc setVertexBytes:&vx length:sizeof vx atIndex:17];
    if (!e.dummy_bound) {
        [enc setVertexBuffer:dummy_attr offset:0 atIndex:15];
        e.dummy_bound = 1;
    }
    if (rs[52] && ds_surface && ds_surface->has_stencil && (rs[59] & 255) && (rs[53] != 1 || rs[54] != 1 || rs[55] != 1 || (rs[185] && (rs[186] != 1 || rs[187] != 1 || rs[188] != 1)))) stencil_touch(ds_surface, sc);
    // Textures.
    if (cur_ps) {
        PsExtra px;
        memset(&px, 0, sizeof px);
        u32 used = cur_ps->info.samplers;
        for (u32 n = 0; n < 16; n++) {
            if (!(used >> n & 1)) continue;
            memcpy(&px.lod_bias[n / 4][n & 3], &ss[n][8], 4);
            Obj *t = cur_tex[n];
            if (enc_hoisted && t && t->written_cb == cb_serial) complain("a shadow pass that went ahead reads something the scene writes before it");
            int cube = (cur_ps->info.cube_samplers >> n) & 1, shadow = (cur_ps->info.shadow_samplers >> n) & 1;
            id<MTLTexture> tex = nil;
            if (t) tex = (ss[n][11] && t->srgb) ? t->srgb : t->view ? t->view : t->tex;
            if (shadow) {
                if (!t || !t->is_depth) tex = dummy_depth;
            } else if (cube) {
                if (!t || t->kind != O_CUBE) tex = dummy_cube;
            } else {
                if (!t || t->kind != O_TEXTURE) tex = dummy_2d;
            }
            if (e.tex[n] != tex) {
                [enc setFragmentTexture:tex atIndex:n];
                e.tex[n] = tex;
            }
            id<MTLSamplerState> s = sampler_for(n, shadow);
            if (e.samp[n] != s) {
                [enc setFragmentSamplerState:s atIndex:n];
                e.samp[n] = s;
            }
        }
        px.alpha_ref = (float)rs[24] / 255.0f;
        px.alpha_func = (int)rs[25];
        [enc setFragmentBytes:&px length:sizeof px atIndex:1];
    }
    return 1;
}
static MTLPrimitiveType prim_type(u32 t) {
    switch (t) {
        case 1: return MTLPrimitiveTypePoint;
        case 2: return MTLPrimitiveTypeLine;
        case 3: return MTLPrimitiveTypeLineStrip;
        case 5: return MTLPrimitiveTypeTriangleStrip;
        default: return MTLPrimitiveTypeTriangle;
    }
}
static u32 prim_verts(u32 t, u32 n) {
    switch (t) {
        case 1: return n;
        case 2: return n * 2;
        case 3: return n + 1;
        case 4: return n * 3;
        case 5: case 6: return n + 2;
        default: return 0;
    }
}
static void bind_streams(void) {
    for (int s = 0; s < 3; s++) {
        id<MTLBuffer> b = streams[s].vb ? streams[s].vb->buffer : nil;
        u32 off = streams[s].offset;
        if (e.vb[s] != b) {
            [enc setVertexBuffer:b offset:off atIndex:(NSUInteger)s];
            e.vb[s] = b;
            e.vb_off[s] = off;
        } else if (b && e.vb_off[s] != off) {
            [enc setVertexBufferOffset:off atIndex:(NSUInteger)s];
            e.vb_off[s] = off;
        }
    }
}
// ---------- FXAA ----------
// ARKHAM_FXAA=1: fast approximate anti-aliasing on the finished 3D picture, alone or on top of the game's MSAA, which it smooths further and can stand in for
// at fewer samples. The engine draws its last full-screen pass, tone mapping and the rest of post-processing, into the back buffer with no depth surface, and the
// interface after that with one: FXAA goes between, so text stays sharp. Only in frames with a 3D scene, more than 300 draws before that point.
// This is the usual FXAA scheme: an edge found from luma contrast, followed both ways to its ends, and the pixel sampled again shifted across it by how near the
// nearer end is, or by how much it stands out from its neighbours when that's more.
static const char *fxaa_src_text =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V fxaa_vs(uint id [[vertex_id]]) {\n"
    "    float2 q = float2((id << 1) & 2, id & 2);\n"
    "    V v; v.pos = float4(q.x * 2 - 1, 1 - q.y * 2, 0, 1); v.uv = q; return v;\n"
    "}\n"
    "static float luma(float4 c) { return dot(c.rgb, float3(0.299, 0.587, 0.114)); }\n"
    "fragment float4 fxaa_fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], constant float2 &px [[buffer(0)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    float2 uv = in.uv;\n"
    "    float4 m = t.sample(s, uv);\n"
    "    float lm = luma(m);\n"
    "    float ln = luma(t.sample(s, uv, int2(0, -1))), ls = luma(t.sample(s, uv, int2(0, 1)));\n"
    "    float lw = luma(t.sample(s, uv, int2(-1, 0))), le = luma(t.sample(s, uv, int2(1, 0)));\n"
    "    float hi = max(max(max(ln, ls), max(lw, le)), lm), lo = min(min(min(ln, ls), min(lw, le)), lm);\n"
    "    float range = hi - lo;\n"
    "    if (range < max(0.0312, hi * 0.125)) return m;\n"
    "    float lnw = luma(t.sample(s, uv, int2(-1, -1))), lne = luma(t.sample(s, uv, int2(1, -1)));\n"
    "    float lsw = luma(t.sample(s, uv, int2(-1, 1))), lse = luma(t.sample(s, uv, int2(1, 1)));\n"
    "    float sub = saturate(abs((2.0 * (ln + ls + lw + le) + lnw + lne + lsw + lse) / 12.0 - lm) / range);\n"
    "    sub = smoothstep(0.0, 1.0, sub);\n"
    "    sub = sub * sub * 0.75;\n"
    "    float eh = abs(lnw + lne - 2.0 * ln) + 2.0 * abs(lw + le - 2.0 * lm) + abs(lsw + lse - 2.0 * ls);\n"
    "    float ev = abs(lnw + lsw - 2.0 * lw) + 2.0 * abs(ln + ls - 2.0 * lm) + abs(lne + lse - 2.0 * le);\n"
    "    bool horz = eh >= ev;\n"
    "    float l1 = horz ? ln : lw, l2 = horz ? ls : le;\n"
    "    float g1 = abs(l1 - lm), g2 = abs(l2 - lm);\n"
    "    float across = horz ? px.y : px.x;\n"
    "    float edge_luma, grad;\n"
    "    if (g1 >= g2) { across = -across; edge_luma = 0.5 * (l1 + lm); grad = g1; } else { edge_luma = 0.5 * (l2 + lm); grad = g2; }\n"
    "    float2 c = uv;\n"
    "    if (horz) c.y += across * 0.5; else c.x += across * 0.5;\n"
    "    float2 along = horz ? float2(px.x, 0) : float2(0, px.y);\n"
    "    float2 u1 = c - along, u2 = c + along;\n"
    "    float e1 = luma(t.sample(s, u1)) - edge_luma, e2 = luma(t.sample(s, u2)) - edge_luma;\n"
    "    float stop = grad * 0.25;\n"
    "    bool d1 = abs(e1) >= stop, d2 = abs(e2) >= stop;\n"
    "    const float steps[10] = {1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0};\n"
    "    for (int i = 0; i < 10 && !(d1 && d2); i++) {\n"
    "        if (!d1) { u1 -= along * steps[i]; e1 = luma(t.sample(s, u1)) - edge_luma; d1 = abs(e1) >= stop; }\n"
    "        if (!d2) { u2 += along * steps[i]; e2 = luma(t.sample(s, u2)) - edge_luma; d2 = abs(e2) >= stop; }\n"
    "    }\n"
    "    float dist1 = horz ? uv.x - u1.x : uv.y - u1.y, dist2 = horz ? u2.x - uv.x : u2.y - uv.y;\n"
    "    bool nearer1 = dist1 < dist2;\n"
    "    float off = 0.5 - min(dist1, dist2) / (dist1 + dist2);\n"
    "    bool right_side = ((nearer1 ? e1 : e2) < 0.0) != (lm < edge_luma);\n"
    "    off = max(right_side ? off : 0.0, sub);\n"
    "    float2 f = uv;\n"
    "    if (horz) f.y += off * across; else f.x += off * across;\n"
    "    return float4(t.sample(s, f).rgb, m.a);\n"
    "}\n";
static int fxaa_done;  // this frame
static int fxaa_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("ARKHAM_FXAA");
        on = v && atoi(v) > 0;
    }
    return on;
}
static void apply_fxaa(void) {
    static id<MTLRenderPipelineState> pipe;
    static MTLPixelFormat pipe_format;
    static id<MTLTexture> copy;
    id<MTLTexture> bb = back_buffer->tex;
    if (!pipe || pipe_format != bb.pixelFormat) {
        static id<MTLLibrary> lib;
        if (!lib) lib = compile_msl(fxaa_src_text);
        MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor new] autorelease];
        d.vertexFunction = [[lib newFunctionWithName:@"fxaa_vs"] autorelease];
        d.fragmentFunction = [[lib newFunctionWithName:@"fxaa_fs"] autorelease];
        d.colorAttachments[0].pixelFormat = bb.pixelFormat;
        NSError *err = nil;
        [pipe release];
        pipe = [dev newRenderPipelineStateWithDescriptor:d error:&err];
        pipe_format = bb.pixelFormat;
        if (!pipe) {
            fprintf(stderr, "[mtl] FXAA pipeline: %s\n", err.localizedDescription.UTF8String);
            return;
        }
    }
    if (!copy || copy.width != bb.width || copy.height != bb.height || copy.pixelFormat != bb.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:bb.pixelFormat width:bb.width height:bb.height mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead;
        [copy release];
        copy = [dev newTextureWithDescriptor:d];
    }
    end_pass();
    ensure_cb();
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:bb toTexture:copy];
    [b endEncoding];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = bb;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> r = [cb renderCommandEncoderWithDescriptor:rp];
    if (__builtin_expect(labels_on(), 0)) r.label = @"FXAA";
    [r setRenderPipelineState:pipe];
    float px[2] = {1.0f / (float)bb.width, 1.0f / (float)bb.height};
    [r setFragmentBytes:px length:sizeof px atIndex:0];
    [r setFragmentTexture:copy atIndex:0];
    [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [r endEncoding];
}
static u64 n_draws;
// After each draw: FXAA once the engine's last full-screen pass of a 3D frame is in the back buffer.
static inline void after_draw(void) {
    if (__builtin_expect(fxaa_on(), 0) && !fxaa_done && rt_surface == back_buffer && !ds_surface && n_draws > 300) {
        fxaa_done = 1;
        apply_fxaa();
    }
}
static void m_DrawPrimitive(CPU *c) {  // this, type, start, primitives
    u32 st[3] = {streams[0].stride, streams[1].stride, streams[2].stride};
    if (!prepare_draw(st, 0)) {
        RET(0);
        return;
    }
    bind_streams();
    [enc drawPrimitives:prim_type(ARG(1)) vertexStart:ARG(2) vertexCount:prim_verts(ARG(1), ARG(3))];
    n_draws++;
    if (__builtin_expect(fmap_on, 0)) trace_draw(ARG(1), ARG(3));
    after_draw();
    RET(0);
}
static void m_DrawIndexedPrimitive(CPU *c) {  // this, type, base vertex, min index, vertices, start index, primitives
    u32 st[3] = {streams[0].stride, streams[1].stride, streams[2].stride};
    int instanced = instancing();
    if (!cur_ib || !prepare_draw(st, instanced)) {
        RET(0);
        return;
    }
    bind_streams();
    int wide = cur_ib->index_format == 102;
    u32 n = instanced ? instance_count() : 1;
    if (instanced) complain("the game draws with hardware instancing");
    [enc drawIndexedPrimitives:prim_type(ARG(1)) indexCount:prim_verts(ARG(1), ARG(6)) indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:cur_ib->buffer indexBufferOffset:ARG(5) * (wide ? 4u : 2u) instanceCount:n baseVertex:(NSInteger)(int)ARG(2) baseInstance:0];
    n_draws++;
    if (__builtin_expect(fmap_on, 0)) trace_draw(ARG(1), ARG(6));
    after_draw();
    RET(0);
}
static void m_DrawPrimitiveUP(CPU *c) {  // this, type, primitives, data, stride
    u32 stride = ARG(4), verts = prim_verts(ARG(1), ARG(2));
    u32 st[3] = {stride, streams[1].stride, streams[2].stride};
    if (!prepare_draw(st, 0)) {
        RET(0);
        return;
    }
    void *mem;
    id<MTLBuffer> vb;
    u32 at = transient_alloc(verts * stride, &mem, &vb);
    memcpy(mem, GP(ARG(3)), (size_t)verts * stride);
    [enc setVertexBuffer:vb offset:at atIndex:0];
    e.vb[0] = nil;
    [enc drawPrimitives:prim_type(ARG(1)) vertexStart:0 vertexCount:verts];
    n_draws++;
    if (__builtin_expect(fmap_on, 0)) trace_draw(ARG(1), ARG(2));
    after_draw();
    RET(0);
}
static void m_DrawIndexedPrimitiveUP(CPU *c) {  // this, type, min index, vertices, primitives, indices, format, data, stride
    u32 stride = ARG(8), verts = ARG(2) + ARG(3), count = prim_verts(ARG(1), ARG(4));
    int wide = ARG(6) == 102;
    u32 st[3] = {stride, streams[1].stride, streams[2].stride};
    if (!prepare_draw(st, 0)) {
        RET(0);
        return;
    }
    void *vm, *im;
    id<MTLBuffer> vb, ib;
    u32 vat = transient_alloc(verts * stride, &vm, &vb);
    memcpy(vm, GP(ARG(7)), (size_t)verts * stride);
    u32 ibytes = count * (wide ? 4u : 2u);
    u32 iat = transient_alloc(ibytes, &im, &ib);
    memcpy(im, GP(ARG(5)), ibytes);
    [enc setVertexBuffer:vb offset:vat atIndex:0];
    e.vb[0] = nil;
    [enc drawIndexedPrimitives:prim_type(ARG(1)) indexCount:count indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:iat];
    n_draws++;
    if (__builtin_expect(fmap_on, 0)) trace_draw(ARG(1), ARG(4));
    after_draw();
    RET(0);
}

// ---------- clears and copies ----------
// Clearing by drawing, for part of a target, or for one attachment in the middle of a pass: ending the pass would make the GPU store and reload everything.
static void clear_by_drawing(u32 flags, const u32 *area, const float *rgba, float z, u32 stencil, u32 n_rects, u32 rects) {
    static id<MTLLibrary> lib;
    if (!lib) {
        lib = compile_msl("#include <metal_stdlib>\nusing namespace metal;\n"
                          "vertex float4 clear_vs(uint id [[vertex_id]], constant float &z [[buffer(20)]]) { float2 q = float2((id << 1) & 2, id & 2); return float4(q.x * 2 - 1, 1 - q.y * 2, z, 1); }\n"
                          "fragment float4 clear_fs(constant float4 &color [[buffer(20)]]) { return color; }\n");
    }
    enum { MAX_CLEAR_PIPES = 64 };
    static struct {
        MTLPixelFormat color, depth;
        u32 samples, mask;
        id<MTLRenderPipelineState> state;
    } cache[MAX_CLEAR_PIPES];
    MTLPixelFormat cf = rt_surface ? rt_surface->tex.pixelFormat : MTLPixelFormatInvalid, df = ds_surface ? ds_surface->tex.pixelFormat : MTLPixelFormatInvalid;
    u32 samples = rt_surface ? (u32)rt_surface->samples : (u32)ds_surface->samples, mask = flags & 1;
    id<MTLRenderPipelineState> pipe = nil;
    for (int i = 0; i < MAX_CLEAR_PIPES && !pipe; i++) {
        if (cache[i].state && cache[i].color == cf && cache[i].depth == df && cache[i].samples == samples && cache[i].mask == mask) pipe = cache[i].state;
        else if (!cache[i].state) {
            MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor new] autorelease];
            d.vertexFunction = [[lib newFunctionWithName:@"clear_vs"] autorelease];
            d.fragmentFunction = [[lib newFunctionWithName:@"clear_fs"] autorelease];
            d.rasterSampleCount = samples;
            if (rt_surface) {
                d.colorAttachments[0].pixelFormat = cf;
                d.colorAttachments[0].writeMask = mask ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
            }
            if (ds_surface) {
                d.depthAttachmentPixelFormat = df;
                if (ds_surface->has_stencil) d.stencilAttachmentPixelFormat = df;
            }
            NSError *err = nil;
            cache[i].color = cf;
            cache[i].depth = df;
            cache[i].samples = samples;
            cache[i].mask = mask;
            cache[i].state = [dev newRenderPipelineStateWithDescriptor:d error:&err];
            if (!cache[i].state) fprintf(stderr, "[mtl] clear pipeline: %s\n", err.localizedDescription.UTF8String);
            pipe = cache[i].state;
        }
    }
    static id<MTLDepthStencilState> states[4];
    int ds_i = ((flags >> 1) & 1) | (((flags >> 2) & 1) << 1);
    if (!states[ds_i]) {
        MTLDepthStencilDescriptor *d = [[MTLDepthStencilDescriptor new] autorelease];
        d.depthCompareFunction = MTLCompareFunctionAlways;
        d.depthWriteEnabled = (flags & 2) != 0;
        if (flags & 4) {
            MTLStencilDescriptor *s = [[MTLStencilDescriptor new] autorelease];
            s.stencilCompareFunction = MTLCompareFunctionAlways;
            s.depthStencilPassOperation = MTLStencilOperationReplace;
            s.stencilFailureOperation = MTLStencilOperationReplace;
            s.depthFailureOperation = MTLStencilOperationReplace;
            s.readMask = 0xFF;
            s.writeMask = 0xFF;
            d.frontFaceStencil = s;
            d.backFaceStencil = s;
        }
        states[ds_i] = [dev newDepthStencilStateWithDescriptor:d];
    }
    u32 tw = rt_surface ? rt_surface->width : ds_surface->width, th = rt_surface ? rt_surface->height : ds_surface->height;
    begin_pass();
    [enc setRenderPipelineState:pipe];
    [enc setDepthStencilState:states[ds_i]];
    [enc setStencilReferenceValue:stencil & 255];
    [enc setCullMode:MTLCullModeNone];
    [enc setDepthBias:0 slopeScale:0 clamp:0];
    MTLViewport v = {0, 0, tw, th, 0, 1};
    [enc setViewport:v];
    [enc setVertexBytes:&z length:sizeof z atIndex:20];
    [enc setFragmentBytes:rgba length:16 atIndex:20];
    for (u32 i = 0; i < (n_rects ? n_rects : 1); i++) {
        u32 a[4] = {area[0], area[1], area[2], area[3]};
        if (n_rects) {
            const u32 *r = (const u32 *)GP(rects + i * 16);
            a[0] = r[0] > a[0] ? r[0] : a[0];
            a[1] = r[1] > a[1] ? r[1] : a[1];
            a[2] = r[2] < a[2] ? r[2] : a[2];
            a[3] = r[3] < a[3] ? r[3] : a[3];
        }
        if (a[2] <= a[0] || a[3] <= a[1]) continue;
        MTLScissorRect sr = {a[0], a[1], a[2] - a[0], a[3] - a[1]};
        [enc setScissorRect:sr];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    // What the encoder was told is no longer what prepare_draw remembers.
    e.pipeline = nil;
    e.depth = nil;
    e.have_vp = e.have_sc = e.have_cull = e.have_bias = e.have_stencil = 0;
    pass_draws++;
}
static void m_Clear(CPU *c) {  // this, rect count, rects, flags, color, z, stencil
    u32 flags = ARG(3), color = ARG(4), stencil = ARG(6);
    float z;
    u32 zbits = ARG(5);
    memcpy(&z, &zbits, 4);
    if (!rt_surface) flags &= ~1u;
    if (!ds_surface) flags &= ~2u;
    if (!ds_surface || !ds_surface->has_stencil) flags &= ~4u;
    if (!flags) {
        RET(0);
        return;
    }
    u32 tw = rt_surface ? rt_surface->width : ds_surface->width, th = rt_surface ? rt_surface->height : ds_surface->height;
    // Direct3D clears what's inside the viewport, and inside the scissor when that's on. The viewport's origin can be -1, as for the full-screen effects, so it's signed here too.
    long x0 = (int)vp[0], y0 = (int)vp[1], x1 = x0 + (long)vp[2], y1 = y0 + (long)vp[3];
    u32 area[4] = {x0 > 0 ? (u32)x0 : 0, y0 > 0 ? (u32)y0 : 0, x1 > 0 ? (u32)x1 : 0, y1 > 0 ? (u32)y1 : 0};
    if (rs[174]) {
        if (scissor[0] > area[0]) area[0] = scissor[0];
        if (scissor[1] > area[1]) area[1] = scissor[1];
        if (scissor[2] < area[2]) area[2] = scissor[2];
        if (scissor[3] < area[3]) area[3] = scissor[3];
    }
    if (area[2] > tw) area[2] = tw;
    if (area[3] > th) area[3] = th;
    int whole = area[0] == 0 && area[1] == 0 && area[2] == tw && area[3] == th && !ARG(1);
    int everything = (!rt_surface || (flags & 1)) && (!ds_surface || (flags & 2)) && (!ds_surface || !ds_surface->has_stencil || (flags & 4));
    // The scene's stencil is cleared after each light, all of it, though the light wrote to a small part: only that part needs it.
    if (flags == 4 && !ARG(1) && ds_surface->s_known && ds_surface->s_value == (stencil & 255)) {
        u32 *b = ds_surface->s_box;
        if (b[2] <= b[0] || b[3] <= b[1]) {
            n_skipped_clears++;
            RET(0);
            return;
        }
        u32 part[4] = {area[0] > b[0] ? area[0] : b[0], area[1] > b[1] ? area[1] : b[1], area[2] < b[2] ? area[2] : b[2], area[3] < b[3] ? area[3] : b[3]};
        int all_of_it = area[0] <= b[0] && area[1] <= b[1] && area[2] >= b[2] && area[3] >= b[3];
        float none[4] = {0, 0, 0, 0};
        if (part[2] > part[0] && part[3] > part[1]) {
            clear_by_drawing(4, part, none, z, stencil, 0, 0);
            n_quad_clears++;
        }
        if (all_of_it) memset(b, 0, 16);
        RET(0);
        return;
    }
    if (parked && !target_is_current() && parked_rt == rt_surface && parked_ds == ds_surface) begin_pass();
    int current = target_is_current();
    if (!whole || (current && pass_draws && !everything)) {
        float rgba[4] = {(float)((color >> 16) & 255) / 255.0f, (float)((color >> 8) & 255) / 255.0f, (float)(color & 255) / 255.0f, (float)(color >> 24) / 255.0f};
        clear_by_drawing(flags, area, rgba, z, stencil, ARG(1), ARG(2));
        n_quad_clears++;
        if ((flags & 4) && ds_surface) {
            if (whole) {
                ds_surface->s_known = 1;
                ds_surface->s_value = stencil & 255;
                memset(ds_surface->s_box, 0, 16);
            } else if (!ds_surface->s_known || ds_surface->s_value != (stencil & 255)) {
                stencil_touch(ds_surface, area);
            }
        }
        RET(0);
        return;
    }
    if (current) close_enc();
    if (flags & 1) {
        pending_clear.color = 1;
        pending_clear.rgba[0] = (float)((color >> 16) & 255) / 255.0f;
        pending_clear.rgba[1] = (float)((color >> 8) & 255) / 255.0f;
        pending_clear.rgba[2] = (float)(color & 255) / 255.0f;
        pending_clear.rgba[3] = (float)(color >> 24) / 255.0f;
    }
    if (flags & 2) {
        pending_clear.depth = 1;
        pending_clear.z = z;
    }
    if (flags & 4) {
        pending_clear.stencil = 1;
        pending_clear.s = stencil & 255;
    }
    RET(0);
}
static id<MTLRenderPipelineState> copy_pipelines[8];
static MTLPixelFormat copy_formats[8];
static id<MTLRenderPipelineState> simple_pipeline(MTLPixelFormat color) {
    static id<MTLLibrary> lib;
    if (!lib) {
        lib = compile_msl("#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; float2 uv; };\n"
                          "vertex V copy_vs(uint id [[vertex_id]], constant float4 &uv [[buffer(0)]]) { V o; float2 q = float2((id << 1) & 2, id & 2); o.uv = uv.xy + q * uv.zw; o.p = float4(q.x * 2 - 1, 1 - q.y * 2, 0, 1); return o; }\n"
                          "fragment float4 copy_fs(V i [[stage_in]], texture2d<float> t [[texture(0)]], constant float &linear [[buffer(0)]]) {\n"
                          "  constexpr sampler a(address::clamp_to_edge, filter::nearest), b(address::clamp_to_edge, filter::linear);\n"
                          "  return linear > 0.5 ? t.sample(b, i.uv) : t.sample(a, i.uv); }\n");
    }
    for (int i = 0; i < 8; i++) {
        if (copy_pipelines[i] && copy_formats[i] == color) return copy_pipelines[i];
        if (!copy_pipelines[i]) {
            MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor new] autorelease];
            d.vertexFunction = [[lib newFunctionWithName:@"copy_vs"] autorelease];
            d.fragmentFunction = [[lib newFunctionWithName:@"copy_fs"] autorelease];
            d.colorAttachments[0].pixelFormat = color;
            NSError *err = nil;
            copy_pipelines[i] = [dev newRenderPipelineStateWithDescriptor:d error:&err];
            if (!copy_pipelines[i]) fprintf(stderr, "[mtl] copy pipeline: %s\n", err.localizedDescription.UTF8String);
            copy_formats[i] = color;
            return copy_pipelines[i];
        }
    }
    return nil;
}
// Averages a multisampled surface into a texture this size.
static id<MTLTexture> resolve_temp(MTLPixelFormat fmt, u32 w, u32 h) {
    static id<MTLTexture> temps[8];
    for (int i = 0; i < 8; i++) {
        if (temps[i] && temps[i].pixelFormat == fmt && temps[i].width == w && temps[i].height == h) return temps[i];
        if (!temps[i]) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            d.storageMode = MTLStorageModePrivate;
            temps[i] = [dev newTextureWithDescriptor:d];
            return temps[i];
        }
    }
    return nil;
}
static void resolve_into(Obj *s, id<MTLTexture> into, u32 level, u32 slice) {
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassAttachmentDescriptor *a = s->is_depth ? (MTLRenderPassAttachmentDescriptor *)p.depthAttachment : (MTLRenderPassAttachmentDescriptor *)p.colorAttachments[0];
    a.texture = s->tex;
    a.loadAction = MTLLoadActionLoad;
    a.storeAction = MTLStoreActionStoreAndMultisampleResolve;
    a.resolveTexture = into;
    a.resolveLevel = level;
    a.resolveSlice = slice;
    id<MTLRenderCommandEncoder> r = [cb renderCommandEncoderWithDescriptor:p];
    if (__builtin_expect(labels_on(), 0)) r.label = [NSString stringWithFormat:@"resolve pass %lux%lu x%lu", (unsigned long)s->tex.width, (unsigned long)s->tex.height, (unsigned long)s->tex.sampleCount];
    [r endEncoding];
}
static void stretch(CPU *c, Obj *s, Obj *d);
static void m_StretchRect(CPU *c) {  // this, source, source rect, destination, destination rect, filter
    Obj *s = obj_of(ARG(1)), *d = obj_of(ARG(3));
    if (!s || !d) {
        RET(0);
        return;
    }
    stretch(c, s, d);
    if (__builtin_expect(fmap_on, 0) && cb) {
        char what[120];
        snprintf(what, sizeof what, "stretch %ux%u x%d %x -> %ux%u x%d %x", s->width, s->height, s->samples, s->format, d->width, d->height, d->samples, d->format);
        fmap_finish(what);
    }
}
static void stretch(CPU *c, Obj *s, Obj *d) {
    u32 sr[4] = {0, 0, s->width, s->height}, dr[4] = {0, 0, d->width, d->height};
    if (ARG(2)) memcpy(sr, GP(ARG(2)), 16);
    if (ARG(4)) memcpy(dr, GP(ARG(4)), 16);
    u32 sw = sr[2] - sr[0], sh = sr[3] - sr[1], dw = dr[2] - dr[0], dh = dr[3] - dr[1];
    int whole_d = !dr[0] && !dr[1] && dw == d->width && dh == d->height;
    int same_size = sw == dw && sh == dh;
    int plain_resolve = s->samples > 1 && d->samples <= 1 && whole_d && same_size && !sr[0] && !sr[1] && sw == s->width && sh == s->height && s->tex.pixelFormat == d->tex.pixelFormat;
    // ARKHAM_TRACE_RESOLVE=1: why an average needs a pass of its own, from the state as the copy comes.
    static int trace_resolve = -1, resolve_told;
    if (trace_resolve < 0) trace_resolve = getenv("ARKHAM_TRACE_RESOLVE") != NULL;
    char why[200] = "";
    if (__builtin_expect(trace_resolve, 0) && plain_resolve && !s->is_depth && resolve_told < 40)
        snprintf(why, sizeof why, "open pass on %s, store %s, averaged into %s, target set now %s, clear waiting %c%c%c, parked %s, draws in pass %u",
                 !enc ? "nothing" : enc_rt == s ? "it" : "another surface", enc_unknown_store ? "undecided" : "decided", s->resolve_tex == d->tex ? "the same texture" : s->resolve_tex ? "another texture" : "nothing yet",
                 rt_surface == s ? "it" : rt_surface == d ? "the destination" : "another surface", pending_clear.color ? 'C' : '-', pending_clear.depth ? 'D' : '-', pending_clear.stencil ? 'S' : '-',
                 !parked ? "nothing" : parked_rt == s ? "it" : "another surface", pass_draws);
    // A clear that waits for the next pass is on the target now set, so when that's neither surface of this copy, the clear can just as well come after it, and the pass on
    // the source stays open to average as it ends.
    if (plain_resolve && !s->is_depth && rt_surface != s && rt_surface != d) {
        if (parked && parked_rt == s) {
            close_enc();
            resume_parked();
        }
        if (enc && enc_rt == s && enc_unknown_store && s->resolve_tex == d->tex && s->resolve_level == d->level && s->resolve_slice == d->slice) {
            if (d->parent) d->parent->written_cb = cb_serial;
            [enc setColorStoreAction:MTLStoreActionStoreAndMultisampleResolve atIndex:0];
            if (__builtin_expect(labels_on(), 0)) enc.label = [enc.label stringByAppendingString:@" +resolve"];
            enc_unknown_store = 0;
            n_resolves_in_pass++;
            end_pass();
            RET(0);
            return;
        }
    }
    flush_clears();
    if (d->parent) d->parent->written_cb = cb_serial;
    if (d->is_depth) d->s_known = 0;
    if (plain_resolve && !s->is_depth) {
        if (parked && parked_rt == s) {
            close_enc();
            resume_parked();
        }
        // The pass on this surface is still open and was told where to average to: it does that as it ends.
        if (enc && enc_rt == s && enc_unknown_store && s->resolve_tex == d->tex && s->resolve_level == d->level && s->resolve_slice == d->slice) {
            [enc setColorStoreAction:MTLStoreActionStoreAndMultisampleResolve atIndex:0];
            enc_unknown_store = 0;
            n_resolves_in_pass++;
            end_pass();
            RET(0);
            return;
        }
        if (s->resolve_tex != d->tex) {
            [s->resolve_tex release];
            s->resolve_tex = [d->tex retain];
        }
        s->resolve_level = d->level;
        s->resolve_slice = d->slice;
    }
    end_pass();
    ensure_cb();
    id<MTLTexture> from = s->tex;
    u32 from_level = s->level, from_slice = s->slice;
    if (s->samples > 1 && d->samples <= 1) {
        if (plain_resolve) {
            if (why[0]) {
                resolve_told++;
                fprintf(stderr, "[resolve] a pass of its own for %ux%u x%d at frame %llu: %s\n", s->width, s->height, s->samples, (unsigned long long)frame_no, why);
            }
            resolve_into(s, d->tex, d->level, d->slice);
            RET(0);
            return;
        }
        from = resolve_temp(s->tex.pixelFormat, s->width, s->height);
        from_level = from_slice = 0;
        resolve_into(s, from, 0, 0);
    } else if (s->samples > 1 || d->samples > 1) {
        complain("a stretch into a multisampled surface isn't done");
        RET(0);
        return;
    }
    if (same_size && from.pixelFormat == d->tex.pixelFormat) {
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:from sourceSlice:from_slice sourceLevel:from_level sourceOrigin:MTLOriginMake(sr[0], sr[1], 0) sourceSize:MTLSizeMake(sw, sh, 1) toTexture:d->tex destinationSlice:d->slice destinationLevel:d->level destinationOrigin:MTLOriginMake(dr[0], dr[1], 0)];
        [b endEncoding];
    } else if (s->is_depth) {
        complain("a stretch between depth surfaces of different sizes isn't done");
    } else {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d->tex;
        rp.colorAttachments[0].level = d->level;
        rp.colorAttachments[0].slice = d->slice;
        rp.colorAttachments[0].loadAction = whole_d ? MTLLoadActionDontCare : MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> r = [cb renderCommandEncoderWithDescriptor:rp];
        [r setRenderPipelineState:simple_pipeline(d->tex.pixelFormat)];
        MTLViewport v = {dr[0], dr[1], dw, dh, 0, 1};
        [r setViewport:v];
        float fw = (float)level_dim(s->width, 0), fh = (float)level_dim(s->height, 0);
        float uv[4] = {(float)sr[0] / fw, (float)sr[1] / fh, (float)sw / fw, (float)sh / fh};
        float linear = same_size ? 0 : 1;
        [r setVertexBytes:uv length:sizeof uv atIndex:0];
        [r setFragmentBytes:&linear length:sizeof linear atIndex:0];
        [r setFragmentTexture:from atIndex:0];
        [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [r endEncoding];
    }
    RET(0);
}

// ---------- queries ----------
static void m_QueryIssue(CPU *c) {  // this, flags: 1 end, 2 begin
    Obj *q = obj_of(ARG(0));
    u32 flags = ARG(1);
    if (q->query_type == 9) {
        if (flags & 2) {
            begin_pass();
            q->slot = vis_next;
            vis_next = (vis_next + 1) % VIS_SLOTS;
            ((u64 *)visibility.contents)[q->slot] = 0;
            q->serial = ~0ull;
            open_query = q;
            if (enc_hoisted) {
                ((u64 *)visibility.contents)[q->slot] = 1;
            } else {
                [enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:q->slot * 8];
                pass_queries++;
            }
        }
        if (flags & 1) {
            if (parked) [parked setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
            else if (enc) [enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
            if (open_query == q) open_query = NULL;
            q->serial = cb_serial;
        }
    } else if (flags & 1) {
        q->serial = cb_serial;
    }
    RET(0);
}
static void m_QueryGetData(CPU *c) {  // this, data, size, flags
    Obj *q = obj_of(ARG(0));
    u32 data = ARG(1), size = ARG(2), flags = ARG(3);
    if (!q->serial) {  // never issued
        if (data && size >= 4) G32(data) = q->query_type == 9 ? 1 : 1;
        RET(0);
        return;
    }
    if (q->serial == cb_serial && (flags & 1)) {
        ensure_cb();  // there may be nothing recorded since the query ended, and the answer still has to arrive
        commit_cb();
    }
    if (q->serial == ~0ull || atomic_load(&completed_serial) < q->serial) {
        if (!query_wait_from) query_wait_from = mach_absolute_time();
        RET(1);  // S_FALSE: not yet
        return;
    }
    if (query_wait_from) {
        ticks_query_wait += mach_absolute_time() - query_wait_from;
        query_wait_from = 0;
    }
    if (data && size >= 4) G32(data) = q->query_type == 9 ? (u32)((u64 *)visibility.contents)[q->slot] : 1;
    RET(0);
}

// ---------- presenting ----------
static _Atomic int layer_asked;
#if TARGET_OS_IPHONE
// The app shell's view the game draws into (ios/app), set before the game starts.
UIView *rt_ios_game_view;
static void make_layer_on_main(void) {
    UIView *content = rt_ios_game_view;
    if (!content) {
        atomic_store(&layer_asked, 0);  // the shell isn't up yet: the next frame asks again
        return;
    }
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = dev;
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = NO;
    l.opaque = YES;
    l.maximumDrawableCount = 3;
    CGRect rt_ios_game_rect(void);  // ios/app/main.m: the whole screen held sideways, its top held upright; the app moves the layer as that changes
    l.frame = rt_ios_game_rect();
    UIScreen *screen = content.window.windowScene.screen;
    l.contentsScale = screen ? screen.nativeScale : 3.0;
    // ARKHAM_METAL_HUD=1: Apple's Metal performance HUD on the game's layer, as config.local's switch does on the Mac; or the phone's settings.
    if (atomic_load(&hud_wanted) < 0) {
        const char *hud = getenv("ARKHAM_METAL_HUD");
        atomic_store(&hud_wanted, hud && *hud == '1');
    }
    if (atomic_load(&hud_wanted) > 0) {
        if (@available(iOS 16.0, *)) l.developerHUDProperties = @{@"mode": @"default", @"logging": @"default"};
        fprintf(stderr, "[mtl] Metal HUD on\n");
    }
    [content.layer insertSublayer:l atIndex:0];  // under the app shell's own views: the frame rate line, and controls drawn over the game
    layer = [l retain];
    fprintf(stderr, "[mtl] drawing into a layer on the app's view, %.0f x %.0f points at %.0fx\n", content.bounds.size.width, content.bounds.size.height, l.contentsScale);
    atomic_store(&layer_ready, 1);
}
#else
static void log_views(NSView *v, int depth) {
    fprintf(stderr, "[mtl] view %*s%s at %.0f,%.0f size %.0f x %.0f\n", depth * 2, "", object_getClassName(v), v.frame.origin.x, v.frame.origin.y, v.frame.size.width, v.frame.size.height);
    for (NSView *s in v.subviews) log_views(s, depth + 1);
}
static void make_layer_on_main(void) {
    NSWindow *w = nil;
    for (NSWindow *candidate in NSApp.windows)
        if (candidate.isVisible && (!w || candidate.frame.size.width > w.frame.size.width)) w = candidate;
    if (!w) {
        atomic_store(&layer_asked, 0);  // not on screen yet: the next frame asks again
        return;
    }
    NSView *content = w.contentView;
    NSEdgeInsets safe = content.safeAreaInsets;
    fprintf(stderr, "[mtl] window %ld at %.0f,%.0f size %.0f x %.0f, screen %.0f x %.0f, safe area insets top %.0f bottom %.0f left %.0f right %.0f, style %#lx\n", (long)w.windowNumber, w.frame.origin.x, w.frame.origin.y, w.frame.size.width,
            w.frame.size.height, w.screen.frame.size.width, w.screen.frame.size.height, safe.top, safe.bottom, safe.left, safe.right, (unsigned long)w.styleMask);
    log_views(content, 0);
    NSView *v = [[NSView alloc] initWithFrame:content.bounds];
    v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = dev;
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = NO;
    l.opaque = YES;
    l.displaySyncEnabled = NO;
    l.maximumDrawableCount = 3;
    v.wantsLayer = YES;
    v.layer = l;
    [content addSubview:v positioned:NSWindowAbove relativeTo:nil];
    l.contentsScale = w.backingScaleFactor;
    layer = [l retain];
    fprintf(stderr, "[mtl] drawing into a layer on %s, %.0f x %.0f\n", object_getClassName(w), content.bounds.size.width, content.bounds.size.height);
    atomic_store(&layer_ready, 1);
}
#endif
static void write_png(u8 *px, u32 w, u32 h, const char *path);
static void save_png(id<MTLTexture> src, const char *path) {
    u32 w = (u32)src.width, h = (u32)src.height;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [[dev newTextureWithDescriptor:d] autorelease];
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
    [b copyFromTexture:src toTexture:t];
    [b endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    u8 *px = malloc((size_t)w * h * 4);
    [t getBytes:px bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    write_png(px, w, h, path);
    free(px);
}
static void write_png(u8 *px, u32 w, u32 h, const char *path) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef g = CGBitmapContextCreate(px, w, h, 8, w * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(g);
    CFStringRef sp = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, sp, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    if (dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CFRelease(url);
    CFRelease(sp);
    CGImageRelease(img);
    CGContextRelease(g);
    CGColorSpaceRelease(cs);
}
// ARKHAM_TRACE_DRAWS=dir, with ARKHAM_TRACE_FRAME and ARKHAM_MTL_NO_AHEAD: logs the state of every draw of that frame as [draw], and saves a crop of the target's color and alpha
// after each draw that changed it. ARKHAM_TRACE_CROP=x,y,w,h in pixels of the 2560x1600 target picks the place. Shader names are the file names tools/check_shaders.py writes to logs/msl.
static u64 code_hash(Obj *o) {
    if (!o || !o->code) return 0;
    u32 words = d3dsh_words(o->code, 1u << 18);
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < words * 4; i++) h = (h ^ ((const u8 *)o->code)[i]) * 0x100000001b3ull;
    return h ? h : 1;
}
static void trace_draw(u32 type, u32 count) {
    static const char *dir = (const char *)1;
    if (dir == (const char *)1) dir = getenv("ARKHAM_TRACE_DRAWS");
    if (!dir || !enc) return;
    static u32 n;
    u32 draw = n++;
    Obj *rt = rt_surface;
    float bias, slope;
    memcpy(&bias, &rs[195], 4);
    memcpy(&slope, &rs[175], 4);
    fprintf(stderr, "[draw] %u rt=%x x%d %ux%u ds=%x type=%u count=%u vs=%016llx ps=%016llx cull=%u z=%u,%u,%u bias=%g,%g blend=%u %u,%u,%u sep=%u %u,%u,%u mask=%x atest=%u,%u,%u stencil=%u clip=%x\n", draw, rt ? rt->format : 0, rt ? rt->samples : 0, rt ? rt->width : 0, rt ? rt->height : 0,
            ds_surface ? ds_surface->format : 0, type, count, code_hash(cur_vs), code_hash(cur_ps), rs[22], rs[7], rs[14], rs[23], bias, slope, rs[27], rs[19], rs[20], rs[171], rs[206], rs[207], rs[208], rs[209], rs[168], rs[15], rs[24], rs[25], rs[52], rs[152]);
    if (cur_ps) {
        char line[2048];
        int at = snprintf(line, sizeof line, "[draw] %u ps:", draw);
        for (int i = 0; i < 16 && at < 1800; i++) at += snprintf(line + at, sizeof line - at, " c%d=%.3g,%.3g,%.3g,%.3g", i, ps_const[i][0], ps_const[i][1], ps_const[i][2], ps_const[i][3]);
        for (int i = 0; i < 16 && at < 1980; i++) {
            if (!(cur_ps->info.samplers >> i & 1)) continue;
            Obj *t = cur_tex[i];
            at += snprintf(line + at, sizeof line - at, " s%d=%ux%u/%x", i, t ? t->width : 0, t ? t->height : 0, t ? t->format : 0);
        }
        fprintf(stderr, "%s\n", line);
    }
    fprintf(stderr, "[draw] %u viewport %d %d %d %d z %g %g, scissor on %u, %u %u %u %u, stream 0 stride %u offset %u\n", draw, (int)vp[0], (int)vp[1], (int)vp[2], (int)vp[3], vp_z[0], vp_z[1], rs[174], scissor[0], scissor[1], scissor[2], scissor[3], streams[0].stride, streams[0].offset);
    if (type == 5 && streams[0].vb && streams[0].vb->buffer) {
        // A non-indexed strip from a buffer is rare enough to show its vertices: raw data, then where the vertex shader's usual constants put them.
        const u8 *base = (const u8 *)streams[0].vb->buffer.contents + streams[0].offset;
        for (u32 i = 0; i < count + 2 && (i + 1) * streams[0].stride <= streams[0].vb->size; i++) {
            const float *p = (const float *)(base + i * streams[0].stride);
            float wp[4], cp[4];
            for (int k = 0; k < 4; k++) wp[k] = vs_const[5][k] * p[0] + vs_const[6][k] * p[1] + vs_const[7][k] * p[2] + vs_const[8][k];
            for (int k = 0; k < 4; k++) cp[k] = vs_const[0][k] * wp[0] + vs_const[1][k] * wp[1] + vs_const[2][k] * wp[2] + vs_const[3][k] * wp[3];
            fprintf(stderr, "[draw] %u vertex %u: %g %g %g %08x %08x %g %g %g world %g %g %g %g clip %g %g %g %g\n", draw, i, p[0], p[1], p[2], ((const u32 *)p)[3], ((const u32 *)p)[4], p[5], p[6], p[7], wp[0], wp[1], wp[2], wp[3], cp[0], cp[1], cp[2], cp[3]);
        }
    }
    if (!dir[0]) return;  // an empty name only logs
    // ARKHAM_TRACE_DRAWS_FROM=n: pictures only from that draw on, because each costs a wait for the GPU.
    static int from = -1;
    if (from < 0) {
        const char *v = getenv("ARKHAM_TRACE_DRAWS_FROM");
        from = v ? atoi(v) : 0;
    }
    if ((int)draw < from) return;
    commit_cb();
    ensure_cb();
    if (!rt || rt->is_depth || (rt->format != 113 && rt->format != 112 && rt->format != 21)) return;
    static u32 cx = 170, cy = 380, cw = 560, ch = 400;
    static int parsed;
    if (!parsed) {
        parsed = 1;
        const char *v = getenv("ARKHAM_TRACE_CROP");
        if (v) sscanf(v, "%u,%u,%u,%u", &cx, &cy, &cw, &ch);
    }
    u32 x = cx * rt->width / 2560, y = cy * rt->height / 1600, w = cw * rt->width / 2560, h = ch * rt->height / 1600;
    if (x + w > rt->width || y + h > rt->height) return;
    id<MTLTexture> src = rt->tex;
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (rt->samples > 1) {
        src = resolve_temp(rt->tex.pixelFormat, rt->width, rt->height);
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = rt->tex;
        p.colorAttachments[0].loadAction = MTLLoadActionLoad;
        p.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
        p.colorAttachments[0].resolveTexture = src;
        [[cmd renderCommandEncoderWithDescriptor:p] endEncoding];
    }
    u32 bytes = rt->format == 113 ? 8 : 4;
    id<MTLBuffer> buf = [[dev newBufferWithLength:(size_t)w * h * bytes options:MTLResourceStorageModeShared] autorelease];
    id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
    [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:w * bytes destinationBytesPerImage:(size_t)w * h * bytes];
    [b endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    static u8 *prev;
    static size_t prev_size;
    static Obj *prev_rt;
    size_t size = (size_t)w * h * bytes;
    int same = prev && prev_rt == rt && prev_size == size && !memcmp(prev, buf.contents, size);
    if (prev_size != size) {
        free(prev);
        prev = malloc(size);
        prev_size = size;
    }
    memcpy(prev, buf.contents, size);
    prev_rt = rt;
    if (same) return;
    u8 *color = malloc((size_t)w * h * 4), *alpha = malloc((size_t)w * h * 4);
    float lo = 1e30f, hi = -1e30f;
    u64 nans = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        float v[4] = {0, 0, 0, 1};
        if (rt->format == 113) {
            const __fp16 *s = (const __fp16 *)buf.contents + i * 4;
            for (int k = 0; k < 4; k++) v[k] = s[k];
        } else if (rt->format == 112) {
            const __fp16 *s = (const __fp16 *)buf.contents + i * 2;
            v[0] = s[0];
            v[1] = s[1];
        } else {
            const u8 *s = (const u8 *)buf.contents + i * 4;
            v[0] = s[2] / 255.0f;
            v[1] = s[1] / 255.0f;
            v[2] = s[0] / 255.0f;
            v[3] = s[3] / 255.0f;
        }
        if (v[0] != v[0] || v[1] != v[1] || v[2] != v[2] || v[3] != v[3]) nans++;
        float a = rt->format == 112 ? v[1] : v[3];
        if (a < lo) lo = a;
        if (a > hi) hi = a;
        u8 *c = color + i * 4, *d = alpha + i * 4;
        for (int k = 0; k < 3; k++) {
            float g = v[k] <= 0 ? 0 : pow(v[k], 0.45f);
            c[2 - k] = v[k] != v[k] ? (k == 1 ? 255 : 0) : g >= 1 ? 255 : (u8)(g * 255);
        }
        c[3] = 255;
        // Scene depth in alpha spans thousands of units, so it's shown on a log scale. Red marks a value that isn't a number, blue one below zero.
        float l = a > 0 ? log2f(1 + a) / 16 : 0;
        u8 g = l >= 1 ? 255 : (u8)(l * 255);
        d[0] = a < 0 ? 255 : g;
        d[1] = a < 0 ? 0 : g;
        d[2] = a != a ? 255 : a < 0 ? 0 : g;
        d[3] = 255;
    }
    fprintf(stderr, "[draw] %u changed the crop; alpha %g to %g, %llu pixels aren't numbers\n", draw, lo, hi, nans);
    char path[512];
    snprintf(path, sizeof path, "%s/d%05u_c.png", dir, draw);
    write_png(color, w, h, path);
    snprintf(path, sizeof path, "%s/d%05u_a.png", dir, draw);
    write_png(alpha, w, h, path);
    free(color);
    free(alpha);
}
// ARKHAM_FRAME_PNG=dir, with ARKHAM_TRACE_FRAME: the target after every pass of that frame, as pictures.
static void dump_target(Obj *rt) {
    const char *dir = getenv("ARKHAM_FRAME_PNG");
    if (!dir || rt->is_depth) return;
    static int n;
    char path[512];
    snprintf(path, sizeof path, "%s/p%03d_%ux%u_x%d_%x.png", dir, n++, rt->width, rt->height, rt->samples, rt->format);
    id<MTLTexture> src = rt->tex;
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (rt->samples > 1) {
        src = resolve_temp(rt->tex.pixelFormat, rt->width, rt->height);
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = rt->tex;
        p.colorAttachments[0].loadAction = MTLLoadActionLoad;
        p.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
        p.colorAttachments[0].resolveTexture = src;
        [[cmd renderCommandEncoderWithDescriptor:p] endEncoding];
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:rt->width height:rt->height mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> out = [[dev newTextureWithDescriptor:d] autorelease];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = out;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> r = [cmd renderCommandEncoderWithDescriptor:rp];
    [r setRenderPipelineState:simple_pipeline(MTLPixelFormatBGRA8Unorm)];
    float uv[4] = {0, 0, 1, 1}, linear = 0;
    [r setVertexBytes:uv length:sizeof uv atIndex:0];
    [r setFragmentBytes:&linear length:sizeof linear atIndex:0];
    [r setFragmentTexture:src atIndex:0];
    [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [r endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    save_png(out, path);
}
// The layer makes whoever asks for a drawable wait until the display has room for another frame. If that were the Direct3D thread, the game could never run faster than the display.
// So another thread does the waiting and leaves the drawable here. A frame that finds one goes on screen, and any other frame is simply not shown.
// ARKHAM_MTL_VSYNC=1 makes the Direct3D thread wait instead, which holds the game to the display's rate.
static _Atomic u32 drawable_w, drawable_h;
static _Atomic(void *) ready_drawable;
static dispatch_semaphore_t drawable_wanted;
static id<CAMetalDrawable> fetch_drawable(void) {
    CGSize want = CGSizeMake(atomic_load(&drawable_w), atomic_load(&drawable_h));
    if (!CGSizeEqualToSize(layer.drawableSize, want)) layer.drawableSize = want;
    return [layer nextDrawable];
}
static void *drawable_main(void *unused) {
    (void)unused;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    for (;;) {
        dispatch_semaphore_wait(drawable_wanted, DISPATCH_TIME_FOREVER);
        void *pool = objc_autoreleasePoolPush();
        id<CAMetalDrawable> dr = fetch_drawable();
        if (dr) {
            atomic_store(&ready_drawable, (void *)[dr retain]);
        } else {
            usleep(1000);
            dispatch_semaphore_signal(drawable_wanted);
        }
        objc_autoreleasePoolPop(pool);
    }
    return NULL;
}
static id<CAMetalDrawable> take_drawable(void);
static void watch_shown(id<CAMetalDrawable> dr);
// Copying into a drawable makes the GPU wait until the display has let that drawable go. In the one queue everything runs in order, so the next frames waited too: the game
// stopped at 240 FPS, two frames a refresh of a 120 Hz display, and its render thread polled for the GPU in the meantime. So the frame is copied to a texture of our own in
// the main queue, and a queue of its own waits for that and for the display, copies and presents. ARKHAM_MTL_ONE_QUEUE=1 brings the old way back.
#define PRESENT_SLOTS 3
static id<MTLCommandQueue> present_queue;
static id<MTLEvent> present_event;
static u64 present_value;
static id<MTLTexture> present_copy[PRESENT_SLOTS];
static _Atomic int present_busy[PRESENT_SLOTS];
static void present_on_its_own_queue(void) {
    if (!present_queue) {
        present_queue = [dev newCommandQueue];
        present_event = [dev newEvent];
    }
    int slot = -1;
    for (int i = 0; i < PRESENT_SLOTS; i++)
        if (!atomic_load(&present_busy[i])) {
            slot = i;
            break;
        }
    if (slot < 0) return;  // the display is behind: this frame isn't shown
    id<CAMetalDrawable> dr = take_drawable();
    if (!dr) return;
    id<MTLTexture> bb = back_buffer->tex;
    // One made before a change of resolution has the old size. Letting it go makes the next one the right size.
    if (dr.texture.width != bb.width || dr.texture.height != bb.height) {
        [dr release];
        return;
    }
    id<MTLTexture> copy = present_copy[slot];
    if (!copy || copy.width != bb.width || copy.height != bb.height || copy.pixelFormat != bb.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:bb.pixelFormat width:bb.width height:bb.height mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = 0;
        [copy release];
        copy = present_copy[slot] = [dev newTextureWithDescriptor:d];
    }
    atomic_store(&present_busy[slot], 1);
    ensure_cb();
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:bb toTexture:copy];
    [b endEncoding];
    u64 v = ++present_value;
    [cb encodeSignalEvent:present_event value:v];
    id<MTLCommandBuffer> p = [present_queue commandBuffer];
    [p encodeWaitForEvent:present_event value:v];
    b = [p blitCommandEncoder];
    [b copyFromTexture:copy toTexture:dr.texture];
    [b endEncoding];
    watch_shown(dr);
    [p presentDrawable:dr];
    [p addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        atomic_store(&present_busy[slot], 0);
    }];
    [p commit];
    [dr release];
}
// Returns a retained drawable, or nil when this frame isn't going on screen.
static id<CAMetalDrawable> take_drawable(void) {
    static int vsync = -1;
    if (vsync < 0) vsync = getenv("ARKHAM_MTL_VSYNC") != NULL;
    atomic_store(&drawable_w, back_buffer->width);
    atomic_store(&drawable_h, back_buffer->height);
    if (vsync) {
        u64 t0 = mach_absolute_time();
        id<CAMetalDrawable> dr = [fetch_drawable() retain];
        ticks_drawable += mach_absolute_time() - t0;
        return dr;
    }
    if (!drawable_wanted) {
        drawable_wanted = dispatch_semaphore_create(1);
        pthread_t t;
        pthread_create(&t, NULL, drawable_main, NULL);
        pthread_detach(t);
    }
    void *dr = atomic_exchange(&ready_drawable, NULL);
    if (dr) dispatch_semaphore_signal(drawable_wanted);
    return dr;
}
// What really reached the screen, for the [fps] line.
static _Atomic u64 n_shown, n_not_shown, shown_worst_us, shown_wait_us;
static _Atomic double shown_prev;
static void watch_shown(id<CAMetalDrawable> dr) {
    double asked = CACurrentMediaTime();
    [dr addPresentedHandler:^(id<MTLDrawable> d) {
        double at = d.presentedTime;
        if (at <= 0) {
            atomic_fetch_add(&n_not_shown, 1);
            return;
        }
        double was = atomic_exchange(&shown_prev, at);
        if (was > 0) {
            u64 gap = (u64)((at - was) * 1e6);
            if (gap > atomic_load(&shown_worst_us)) atomic_store(&shown_worst_us, gap);
        }
        atomic_fetch_add(&shown_wait_us, (u64)((at - asked) * 1e6));
        atomic_fetch_add(&n_shown, 1);
    }];
}
void rt_hitch_tick(void);
void rt_fps_overlay(double fps, double worst_ms);
static void *frame_pool;
static _Atomic double fps_cap = -1;  // frames a second at most; 0 for no limit, -1 until ARKHAM_FPS_CAP has been read
void rt_set_fps_cap(double n) { atomic_store(&fps_cap, n > 0 ? n : 0); }
// Apple's Metal HUD on the game's layer, switched as the game runs (the phone's settings) or by ARKHAM_METAL_HUD=1 at start (hud_wanted, above).
void rt_set_metal_hud(int on) {
    atomic_store(&hud_wanted, on);
#if TARGET_OS_IPHONE
    dispatch_async(dispatch_get_main_queue(), ^{
        if (layer) {
            if (@available(iOS 16.0, *)) layer.developerHUDProperties = on ? @{@"mode": @"default", @"logging": @"default"} : @{};  // no mode: hidden
        }
    });
#endif
}
// Frames the game has finished, for the phone's touch controls (ios/app/TouchControls.m): a game that has stopped warping the cursor while frames
// go on is in a menu, while one that has stopped both is only busy.
static _Atomic unsigned frames_total, last_draws;
unsigned rt_frames_presented(void) { return atomic_load(&frames_total); }
// And how many draws the last one took: play takes hundreds, a loading screen or a film a few.
unsigned rt_last_frame_draws(void) { return atomic_load(&last_draws); }
static void note_frame(u64 draws) {
    static u64 fps_start, fps_frames, fps_worst, fps_prev, draws_seen;
    atomic_fetch_add(&frames_total, 1);
    atomic_store(&last_draws, (unsigned)draws);
    u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!fps_start) fps_start = now;
    if (fps_prev && now - fps_prev > fps_worst) fps_worst = now - fps_prev;
    fps_prev = now;
    fps_frames++;
    draws_seen = draws;
    if (now - fps_start >= 1000000000ull) {
        double secs = (double)(now - fps_start) / 1e9, fps = (double)fps_frames / secs;
        double per_frame = 125.0 / 3.0 / 1e6 / (double)fps_frames;
        u64 shown = atomic_exchange(&n_shown, 0);
        char threads[96];
        void rt_thread_cpu_report(char *out, size_t cap, double frames);
        rt_thread_cpu_report(threads, sizeof threads, (double)fps_frames);
        double gpu_ms = (double)atomic_exchange(&gpu_ns, 0) / 1e6 / (double)fps_frames;
        fprintf(stderr, "[fps] %.1f, worst frame %.1f ms, %llu draws, %llu passes; ms a frame: thread busy %.1f, gpu %.1f, drawable %.2f, waiting for gpu %.2f, engine waiting for queries %.1f; %llu quad clears, %llu not needed, %llu resolves in pass; %llu of %llu shaders compiled in %.0f ms, %llu not from the cache, %llu built ahead, %u pipelines in %.0f ms (%llu draws left out waiting for one); on screen %.1f a second, %llu dropped, longest gap %.1f ms, %.1f ms from present; guest threads, CPU ms a frame: %s\n",
                fps, (double)fps_worst / 1e6, draws_seen, n_passes / fps_frames, (double)rt_d3dq_busy_ticks * per_frame, gpu_ms, (double)ticks_drawable * per_frame,
                (double)ticks_inflight * per_frame, (double)ticks_query_wait * per_frame, n_quad_clears / fps_frames, n_skipped_clears / fps_frames, n_resolves_in_pass / fps_frames, n_compiled, n_created, (double)compile_ticks * 125.0 / 3.0 / 1e6, n_uncached, n_prebuilt, n_pipes, (double)pipe_ticks * 125.0 / 3.0 / 1e6, n_pipe_waits,
                (double)shown / secs, atomic_exchange(&n_not_shown, 0), (double)atomic_exchange(&shown_worst_us, 0) / 1e3, shown ? (double)atomic_exchange(&shown_wait_us, 0) / 1e3 / (double)shown : 0.0, threads);
        rt_d3dq_busy_ticks = ticks_drawable = ticks_inflight = ticks_query_wait = 0;
        n_passes = n_quad_clears = n_skipped_clears = n_resolves_in_pass = 0;
        rt_fps_overlay(fps, (double)fps_worst / 1e6);
#if TARGET_OS_IPHONE
        void rt_ios_overlay(double fps, double gpu_ms, double shown_a_second);  // the app shell's line over the game (ios/app/main.m)
        rt_ios_overlay(fps, gpu_ms, (double)shown / secs);
#endif
        fps_start = now;
        fps_frames = 0;
        n_pipe_waits = 0;
        fps_worst = 0;
    }
    rt_hitch_tick();
}
// ARKHAM_COMPILE_TEST=list: once the window and its layer are up, compiles each file of the list one after another as the game would, logs each one's time and quits. One from Metal's
// cache takes well under a millisecond, so this shows whether the running game finds what tools/prebuild_shaders.sh compiled.
static void compile_test(const char *list) {
    NSString *text = [NSString stringWithContentsOfFile:@(list) encoding:NSUTF8StringEncoding error:nil];
    [[dev newLibraryWithSource:@"#include <metal_stdlib>\nfragment float4 warm_up() { return float4(1.0); }\n" options:compile_options() error:nil] release];
    int n = 0, cached = 0;
    double total = 0;
    for (NSString *path in [text componentsSeparatedByString:@"\n"]) {
        if (!path.length) continue;
        NSString *src = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
        double t0 = CACurrentMediaTime();
        [[dev newLibraryWithSource:src options:compile_options() error:nil] release];
        double ms = (CACurrentMediaTime() - t0) * 1e3;
        fprintf(stderr, "[compile-test] %s %.1f ms\n", path.lastPathComponent.UTF8String, ms);
        n++;
        cached += ms < 2;
        total += ms;
    }
    fprintf(stderr, "[compile-test] %d shaders, %d from the cache, %.0f ms in all, Metal HUD %s, device %s, queue %s\n", n, cached, total, getenv("MTL_HUD_ENABLED") ? "on" : "off",
            object_getClassName(dev), object_getClassName(queue));
    _exit(0);
}
#if TARGET_OS_IPHONE
// ---------- for the phone's touch controls (ios/app/TouchControls.m) ----------
// How light the picture is under each button, so that each can be drawn light over dark parts and dark over light ones: every 6th frame, while the app
// asks for it, the finished frame is copied into a texture of its own, its mip levels are made, and the level about 44 x 31 is read back once the GPU
// is done with it. A few hundredths of a millisecond of the GPU's time.
#define LUMA_LEVEL 5
static _Atomic int luma_wanted, luma_busy;
static id<MTLTexture> luma_tex;
static id<MTLBuffer> luma_buf;
static pthread_mutex_t luma_mu = PTHREAD_MUTEX_INITIALIZER;
static float *luma_cells;
static u32 luma_cw, luma_ch;
void rt_want_picture_luma(int on) { atomic_store(&luma_wanted, on); }
// The average lightness, 0 to 1, of a part of the picture given as shares of it from its top left; -1 until there is any.
double rt_picture_luma(double x0, double y0, double x1, double y1) {
    double sum = 0;
    int n = 0;
    pthread_mutex_lock(&luma_mu);
    if (luma_cells) {
        int cx0 = (int)fmax(0, floor(x0 * luma_cw)), cx1 = (int)fmin(luma_cw - 1, ceil(x1 * luma_cw) - 1);
        int cy0 = (int)fmax(0, floor(y0 * luma_ch)), cy1 = (int)fmin(luma_ch - 1, ceil(y1 * luma_ch) - 1);
        for (int y = cy0; y <= cy1; y++)
            for (int x = cx0; x <= cx1; x++) {
                sum += luma_cells[y * (int)luma_cw + x];
                n++;
            }
    }
    pthread_mutex_unlock(&luma_mu);
    return n ? sum / n : -1;
}
static void sample_luma(void) {
    if (!atomic_load(&luma_wanted) || !back_buffer || frame_no % 6 || atomic_load(&luma_busy)) return;
    id<MTLTexture> src = back_buffer->tex;
    if (src.sampleCount > 1 || (src.pixelFormat != MTLPixelFormatBGRA8Unorm && src.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB)) return;
    u32 w = (u32)src.width, h = (u32)src.height, lw = MAX(1u, w >> LUMA_LEVEL), lh = MAX(1u, h >> LUMA_LEVEL);
    if (!luma_tex || luma_tex.width != w || luma_tex.height != h || luma_tex.pixelFormat != src.pixelFormat) {
        [luma_tex release];
        [luma_buf release];
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:w height:h mipmapped:YES];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        luma_tex = [dev newTextureWithDescriptor:d];
        luma_buf = [dev newBufferWithLength:(NSUInteger)lw * lh * 4 options:MTLResourceStorageModeShared];
    }
    if (luma_tex.mipmapLevelCount <= LUMA_LEVEL) return;
    atomic_store(&luma_busy, 1);
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toTexture:luma_tex destinationSlice:0
        destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b generateMipmapsForTexture:luma_tex];
    [b copyFromTexture:luma_tex sourceSlice:0 sourceLevel:LUMA_LEVEL sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(lw, lh, 1) toBuffer:luma_buf
        destinationOffset:0 destinationBytesPerRow:lw * 4 destinationBytesPerImage:(NSUInteger)lw * lh * 4];
    [b endEncoding];
    id<MTLBuffer> buf = luma_buf;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        const u8 *px = buf.contents;
        float *cells = malloc((size_t)lw * lh * sizeof(float));
        for (u32 i = 0; i < lw * lh; i++) cells[i] = (0.0722f * px[4 * i] + 0.7152f * px[4 * i + 1] + 0.2126f * px[4 * i + 2]) / 255.0f;  // BGRA
        pthread_mutex_lock(&luma_mu);
        free(luma_cells);
        luma_cells = cells;
        luma_cw = lw;
        luma_ch = lh;
        pthread_mutex_unlock(&luma_mu);
        atomic_store(&luma_busy, 0);
    }];
}
#endif
// A picture of the next finished frame in <name>.png, written off the game's threads once the GPU has it, so the game doesn't wait: on the phone in
// Documents/Shots, on the Mac in ARKHAM_SHOTS or logs/shots (a scripted run's "shot" step, shims_objc.c).
static _Atomic(char *) shot_wanted;
void rt_request_snapshot(const char *name) { free(atomic_exchange(&shot_wanted, strdup(name))); }
static void take_snapshot(void) {
    char *name = atomic_exchange(&shot_wanted, NULL);
    if (!name) return;
    id<MTLTexture> src = back_buffer ? back_buffer->tex : nil;
    if (!src || src.sampleCount > 1 || (src.pixelFormat != MTLPixelFormatBGRA8Unorm && src.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB)) {
        free(name);
        return;
    }
    u32 w = (u32)src.width, h = (u32)src.height;
    id<MTLBuffer> buf = [dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
        destinationBytesPerRow:w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
    [b endEncoding];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            char dir[1024], path[1200];
#if TARGET_OS_IPHONE
            snprintf(dir, sizeof dir, "%s/Documents/Shots", getenv("HOME"));
#else
            snprintf(dir, sizeof dir, "%s", getenv("ARKHAM_SHOTS") ? getenv("ARKHAM_SHOTS") : "logs/shots");
#endif
            mkdir(dir, 0755);
            snprintf(path, sizeof path, "%s/%s.png", dir, name);
            write_png((u8 *)buf.contents, w, h, path);
            fprintf(stderr, "[snap] wrote %s\n", path);
            [buf release];
            free(name);
        });
    }];
}
// ARKHAM_PREBUILD_IN_GAME=list: the same as --prebuild-shaders, but from the running game once its window is up, then quits. With Apple's Metal HUD on, Metal files compiled
// shaders under another key, so the HUD's side of the cache can only be filled from a game whose HUD is showing. tools/prebuild_shaders.sh does this when config.local has it.
static void m_Present(CPU *c) {
    static const char *in_game = (const char *)1;
    if (in_game == (const char *)1) in_game = getenv("ARKHAM_PREBUILD_IN_GAME");
    if (__builtin_expect(in_game != NULL, 0) && atomic_load(&layer_ready) && frame_no >= 60) _exit(prebuild_list(in_game) != 0);
    static const char *ctest = (const char *)1;
    if (ctest == (const char *)1) ctest = getenv("ARKHAM_COMPILE_TEST");
    if (__builtin_expect(ctest != NULL, 0) && atomic_load(&layer_ready) && frame_no >= 60) compile_test(ctest);
    flush_clears();
    end_pass();
    ensure_cb();
    if (!atomic_load(&layer_ready) && !atomic_exchange(&layer_asked, 1)) {
        dispatch_async(dispatch_get_main_queue(), ^{
            make_layer_on_main();
        });
    }
    static const char *snap = (const char *)1;
    if (snap == (const char *)1) snap = getenv("ARKHAM_MTLSNAP");
    // With a Direct3D trace, only the traced frames.
    int rt_d3d_trace_picture(void);
    static int snap_traced = -1;
    if (snap_traced < 0) snap_traced = getenv("ARKHAM_D3D_TRACE") != NULL;
    if (snap && back_buffer && (snap_traced ? rt_d3d_trace_picture() : frame_no == 30 || frame_no == 120 || frame_no % 600 == 0)) {
        commit_cb();
        ensure_cb();
        char path[512];
        snprintf(path, sizeof path, "%s/m%06llu.png", snap, frame_no);
        save_png(back_buffer->tex, path);
    }
#if TARGET_OS_IPHONE
    if (back_buffer) sample_luma();
#endif
    if (back_buffer) take_snapshot();
    {
        static double map_at = -1;
        static u64 map_start;
        u64 now_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (map_at < 0) {
            const char *v = getenv("ARKHAM_TRACE_FRAME");
            map_at = v ? atof(v) : 0;
            map_start = now_ns;
        }
        // ARKHAM_TRACE_BUSY=frames: the same as ARKHAM_TRACE_FRAME, that many frames after the first with over 300 draws, for a place only seen right after a level has loaded.
        static int busy_at = -1;
        if (busy_at < 0) {
            const char *v = getenv("ARKHAM_TRACE_BUSY");
            busy_at = v ? atoi(v) : 0;
        }
        static int busy_seen;
        if (n_draws > 300) busy_seen = 1;
        int busy_now = 0;
        if (busy_at > 0 && busy_seen && --busy_at == 0) busy_now = 1;
        // ARKHAM_TRACE_LEVEL=seconds: the same, that long after a replayed level has started.
        double rt_d3d_level_seconds(void);
        static double level_at = -1;
        if (level_at < 0) {
            const char *v = getenv("ARKHAM_TRACE_LEVEL");
            level_at = v ? atof(v) : 0;
        }
        if (level_at > 0 && rt_d3d_level_seconds() >= level_at) {
            busy_now = 1;
            level_at = 0;
        }
        // ARKHAM_TRACE_KEY=1: the same each time the = key goes down, for a scene someone has to walk to.
        static int key_on = -1, key_was;
        if (key_on < 0) key_on = getenv("ARKHAM_TRACE_KEY") != NULL;
        if (key_on) {
#if TARGET_OS_IPHONE
            int down = 0;  // no keyboard to watch on the phone
#else
            int down = CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, 0x18);
#endif
            if (down && !key_was) busy_now = 1;
            key_was = down;
        }
        if (fmap_on) {
            fmap_on = 0;
            map_at = 0;
            fprintf(stderr, "[fmap] end of frame\n");
        } else if (busy_now || (map_at > 0 && (double)(now_ns - map_start) / 1e9 > map_at)) {
            commit_cb();
            ensure_cb();
            fmap_on = 1;
        }
    }
    static int no_present = -1;
    if (no_present < 0) no_present = getenv("ARKHAM_MTL_NOPRESENT") != NULL;
    static int one_queue = -1;
    if (one_queue < 0) one_queue = getenv("ARKHAM_MTL_ONE_QUEUE") != NULL;
    if (atomic_load(&layer_ready) && back_buffer && !no_present && one_queue) {
        id<CAMetalDrawable> dr = take_drawable();
        // One made before a change of resolution has the old size. Letting it go makes the next one the right size.
        if (dr && dr.texture.width == back_buffer->width && dr.texture.height == back_buffer->height) {
            commit_cb();
            ensure_cb();
            cb_presents = 1;
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:back_buffer->tex toTexture:dr.texture];
            [b endEncoding];
            watch_shown(dr);
            [cb presentDrawable:dr];
        }
        [dr release];
    } else if (atomic_load(&layer_ready) && back_buffer && !no_present) {
        present_on_its_own_queue();
    }
    dispatch_semaphore_t sem = frames_in_flight;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        dispatch_semaphore_signal(sem);
    }];
    commit_cb();
    frame_no++;
#if TARGET_OS_IPHONE
    frame_wait_ns = 0;
#endif
    note_frame(n_draws);
    n_draws = 0;
    fxaa_done = 0;
    {
        u64 t0 = mach_absolute_time();
        dispatch_semaphore_wait(frames_in_flight, DISPATCH_TIME_FOREVER);
        ticks_inflight += mach_absolute_time() - t0;
    }
    // ARKHAM_FPS_CAP=n, the same switch as on OpenGL: no limit unless asked for. The phone's settings change it as the game runs (rt_set_fps_cap).
    static u64 next_ns;
    if (fps_cap < 0) {
        const char *v = getenv("ARKHAM_FPS_CAP");
        fps_cap = v ? atof(v) : 0.0;
    }
    double cap = fps_cap;
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
    transient_frame = (transient_frame + 1) % TRANSIENT_FRAMES;
    transient_link = 0;
    transient_at = 0;
    vs_const_dirty = ps_const_dirty = 1;
    if (frame_pool) objc_autoreleasePoolPop(frame_pool);
    frame_pool = objc_autoreleasePoolPush();
    RET(0);
}

// ---------- starting up ----------
int rt_mtl_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("ARKHAM_RENDERER");
        on = v && !strcmp(v, "metal");
#if TARGET_OS_IPHONE
        on = 1;  // the phone has no OpenGL to draw with
#endif
    }
    return on;
}
static void start_device(void) {
    if (dev) return;
    dev = MTLCreateSystemDefaultDevice();
    load_prebuilt();
    queue = [dev newCommandQueueWithMaxCommandBufferCount:64];
    frames_in_flight = dispatch_semaphore_create(TRANSIENT_FRAMES - 1);
    for (int i = 0; i < TRANSIENT_FRAMES; i++) transient[i][0] = [dev newBufferWithLength:TRANSIENT_SIZE options:MTLResourceStorageModeShared];
    visibility = [dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared];
    static const float zero_one[4] = {0, 0, 0, 1};
    dummy_attr = [dev newBufferWithBytes:zero_one length:16 options:MTLResourceStorageModeShared];
    static const u32 white[6 * 4] = {0xFFFFFFFFu};
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
    dummy_2d = [dev newTextureWithDescriptor:d];
    [dummy_2d replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:white bytesPerRow:4];
    MTLTextureDescriptor *dc = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:1 mipmapped:NO];
    dummy_cube = [dev newTextureWithDescriptor:dc];
    for (int f = 0; f < 6; f++) [dummy_cube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:(NSUInteger)f withBytes:white bytesPerRow:4 bytesPerImage:4];
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:1 height:1 mipmapped:NO];
    dd.usage = MTLTextureUsageShaderRead;
    dd.storageMode = MTLStorageModePrivate;
    dummy_depth = [dev newTextureWithDescriptor:dd];
    // The defaults Direct3D starts with.
    rs[193] = 0xFFFFFFFF; rs[7] = 1; rs[14] = 1; rs[23] = 4; rs[22] = 3; rs[19] = 2; rs[20] = 1; rs[171] = 1; rs[207] = 2; rs[208] = 1; rs[209] = 1; rs[168] = 15; rs[25] = 8; rs[56] = 8; rs[189] = 8;
    rs[53] = rs[54] = rs[55] = rs[186] = rs[187] = rs[188] = 1;
    rs[58] = rs[59] = 0xFFFFFFFFu;
    for (int i = 0; i < 16; i++) {
        ss[i][1] = ss[i][2] = ss[i][3] = 1;
        ss[i][5] = ss[i][6] = 1;
        ss[i][10] = 1;
    }
    vp_z[1] = 1;
    fprintf(stderr, "[mtl] using %s\n", dev.name.UTF8String);
}
// The engine asks for the back buffer's size from Feral's device, so the buffer follows that.
void rt_mtl_set_back_buffer(u32 width, u32 height) {
    start_device();
    if (back_buffer && back_buffer->width == width && back_buffer->height == height) return;
    Obj *o = obj_new(O_SURFACE, VT_SURFACE);
    o->format = 21;
    o->width = width;
    o->height = height;
    o->samples = 1;
    o->usage = 1;
    o->tex = make_texture(MTLTextureType2D, 21, width, height, 1, 1, 1, NULL);
    back_buffer = o;
    rt_surface = o;
    vp[2] = scissor[2] = width;
    vp[3] = scissor[3] = height;
    fprintf(stderr, "[mtl] back buffer %u x %u\n", width, height);
}
// The device Feral's layer created knows the size the engine asked for, so the first request reads it from there.
u32 rt_d3dq_feral(CPU *c, const char *name, const u32 *args, int n);
static void m_GetBackBufferStart(CPU *c) {
    if (!back_buffer) {
        u32 out = rt_guest_calloc(1, 64);
        u32 get[5] = {ARG(0), 0, 0, 0, out};
        rt_d3dq_feral(c, "Device.GetBackBuffer", get, 5);
        u32 desc[2] = {G32(out), out + 8};
        rt_d3dq_feral(c, "Surface.GetDesc", desc, 2);
        rt_mtl_set_back_buffer(G32(out + 8 + 24), G32(out + 8 + 28));
        rt_guest_free(out);
    }
    m_GetBackBuffer(c);
}
// A change of resolution. Metal's objects carry on: Feral's layer does what it does to the window, and the back buffer follows.
static void m_Reset(CPU *c) {  // this, presentation parameters: width, height, ...
    u32 pp = ARG(1);
    commit_cb();
    u32 args[2] = {ARG(0), pp};
    u32 result = rt_d3dq_feral(c, "Device.Reset", args, 2);
    u32 w = G32(pp), h = G32(pp + 4);
    if (w && h && back_buffer && (back_buffer->width != w || back_buffer->height != h)) {
        if (rt_surface == back_buffer) rt_surface = NULL;
        rt_mtl_set_back_buffer(w, h);
    }
    fprintf(stderr, "[mtl] reset to %u x %u, Feral's layer says %#x\n", w, h, result);
    RET(result);
}
static void m_GetRenderTarget(CPU *c) {  // this, index, out
    Obj *s = ARG(1) == 0 ? rt_surface : NULL;
    if (s) (s->parent ? s->parent : s)->refs++;
    G32(ARG(2)) = s ? s->guest : 0;
    RET(s ? 0 : 0x8876086Cu);
}
static void m_GetDepthStencilSurface(CPU *c) {  // this, out
    Obj *s = ds_surface;
    if (s) (s->parent ? s->parent : s)->refs++;
    G32(ARG(1)) = s ? s->guest : 0;
    RET(s ? 0 : 0x8876086Cu);
}
static const struct {
    const char *name;
    void (*fn)(CPU *);
} table[] = {
    {"Device.CreateTexture", m_CreateTexture}, {"Device.CreateCubeTexture", m_CreateCubeTexture}, {"Device.CreateRenderTarget", m_CreateRenderTarget},
    {"Device.CreateDepthStencilSurface", m_CreateDepthStencilSurface}, {"Device.CreateVertexBuffer", m_CreateVertexBuffer}, {"Device.CreateIndexBuffer", m_CreateIndexBuffer},
    {"Device.CreateVertexDeclaration", m_CreateVertexDeclaration}, {"Device.CreateVertexShader", m_CreateVertexShader}, {"Device.CreatePixelShader", m_CreatePixelShader},
    {"Device.CreateQuery", m_CreateQuery}, {"Device.GetBackBuffer", m_GetBackBufferStart}, {"Device.SetRenderTarget", m_SetRenderTarget},
    {"Device.Reset", m_Reset}, {"Device.GetRenderTarget", m_GetRenderTarget}, {"Device.GetDepthStencilSurface", m_GetDepthStencilSurface},
    {"Device.SetDepthStencilSurface", m_SetDepthStencilSurface}, {"Device.BeginScene", m_Nothing}, {"Device.EndScene", m_Nothing}, {"Device.Clear", m_Clear},
    {"Device.SetViewport", m_SetViewport}, {"Device.SetScissorRect", m_SetScissorRect}, {"Device.SetClipPlane", m_SetClipPlane}, {"Device.SetRenderState", m_SetRenderState},
    {"Device.SetSamplerState", m_SetSamplerState}, {"Device.SetTexture", m_SetTexture}, {"Device.SetVertexShader", m_SetVertexShader}, {"Device.SetPixelShader", m_SetPixelShader},
    {"Device.SetVertexShaderConstantF", m_SetVSConst}, {"Device.SetPixelShaderConstantF", m_SetPSConst}, {"Device.SetVertexDeclaration", m_SetVertexDeclaration},
    {"Device.SetStreamSource", m_SetStreamSource}, {"Device.SetStreamSourceFreq", m_SetStreamSourceFreq}, {"Device.SetIndices", m_SetIndices}, {"Device.DrawPrimitive", m_DrawPrimitive},
    {"Device.DrawIndexedPrimitive", m_DrawIndexedPrimitive}, {"Device.DrawPrimitiveUP", m_DrawPrimitiveUP}, {"Device.DrawIndexedPrimitiveUP", m_DrawIndexedPrimitiveUP},
    {"Device.StretchRect", m_StretchRect}, {"Device.Present", m_Present}, {"Query.Issue", m_QueryIssue}, {"Query.GetData", m_QueryGetData}, {"Texture.GetSurfaceLevel", m_GetSurfaceLevel},
    {"Texture.LockRect", m_TexLock}, {"Texture.UnlockRect", m_TexUnlock}, {"CubeTexture.LockRect", m_CubeLock}, {"CubeTexture.UnlockRect", m_CubeUnlock},
    {"VertexBuffer.Lock", buffer_lock}, {"VertexBuffer.Unlock", buffer_unlock}, {"VertexBuffer.GetDesc", m_VBDesc}, {"IndexBuffer.Lock", buffer_lock}, {"IndexBuffer.Unlock", buffer_unlock},
    {"IndexBuffer.GetDesc", m_IBDesc}, {"Surface.GetDesc", m_SurfaceGetDesc},
};
// A method of one of our objects with no implementation: Feral's code would read our object as its own. The queue reports it by name.
void rt_mtl_missing(CPU *c) { RET(0x80004001u); }
// The implementation of a method, or NULL to leave it to Feral's layer.
void (*rt_mtl_method(const char *name, unsigned slot))(CPU *) {
    if (!rt_mtl_on()) return NULL;
    start_device();
    int device = !strncmp(name, "Device.", 7);
    if (!device) {  // the device's own reference count stays Feral's
        if (slot == 2) return m_AddRef;
        if (slot == 3) return m_Release;
    }
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(table[i].name, name)) return table[i].fn;
    return device ? NULL : rt_mtl_missing;
}
