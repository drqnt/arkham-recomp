// Answers what decides whether the game can run on an iPhone or iPad, before any real work is done for them: how much memory an app can have and
// where, whether traps for unaligned accesses work, what the GPU supports, and how fast things are next to the Mac this was developed on.
// It touches nothing of the game. The same code runs as a command-line tool on the Mac (PROBE_CLI) for a baseline.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <fcntl.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#include <os/proc.h>
#endif

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned char u8;

static double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

// ---------- output ----------
static FILE *out_file;
static void (*ui_line)(const char *);
static void say(const char *fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    printf("[probe] %s\n", b);
    fflush(stdout);
    if (out_file) {
        fprintf(out_file, "%s\n", b);
        fflush(out_file);
    }
    if (ui_line) ui_line(b);
}

// ---------- the device ----------
static u64 sysctl_u64(const char *name) {
    u64 v = 0;
    size_t n = sizeof v;
    if (sysctlbyname(name, &v, &n, NULL, 0)) return 0;
    return n == 4 ? (u64)(u32)v : v;
}
static void memory_now(u64 *footprint, u64 *remaining) {
    task_vm_info_data_t vm;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    *footprint = *remaining = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &n) == KERN_SUCCESS) {
        *footprint = vm.phys_footprint;
        *remaining = vm.limit_bytes_remaining;
    }
}
static u64 available_memory(void) {
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
    return os_proc_available_memory();
#else
    return 0;
#endif
}
static void test_device(void) {
    char model[64] = "", machine[64] = "";
    size_t n = sizeof model;
    sysctlbyname("hw.model", model, &n, NULL, 0);
    n = sizeof machine;
    sysctlbyname("hw.machine", machine, &n, NULL, 0);
    say("device: %s %s, %s", machine, model, NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
    say("memory: %.1f GB, cores: %llu performance, %llu efficiency", (double)sysctl_u64("hw.memsize") / 1e9, sysctl_u64("hw.perflevel0.logicalcpu"), sysctl_u64("hw.perflevel1.logicalcpu"));
    say("features: LRCPC %llu, LRCPC2 %llu, LSE2 %llu, TSO %llu", sysctl_u64("hw.optional.arm.FEAT_LRCPC"), sysctl_u64("hw.optional.arm.FEAT_LRCPC2"), sysctl_u64("hw.optional.arm.FEAT_LSE2"), sysctl_u64("hw.optional.arm.FEAT_TSO"));
    u64 fp, rem;
    memory_now(&fp, &rem);
    say("app memory limit: footprint %.0f MB, %.0f MB left by the task, os_proc_available_memory %.0f MB", (double)fp / 1e6, (double)rem / 1e6, (double)available_memory() / 1e6);
}

// ---------- the guest's 4 GB window ----------
static u8 *guest_window;
static void test_address_space(void) {
    static const u64 bases[] = {0x7000000000ull, 0x2000000000ull, 0x1000000000ull, 0x800000000ull, 0x300000000ull, 0x280000000ull};
    for (unsigned i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        void *p = mmap((void *)bases[i], 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        int exact = p == (void *)bases[i];
        say("4 GB window at %#llx: %s (got %p)", bases[i], exact ? "yes" : "no", p == MAP_FAILED ? NULL : p);
        if (p != MAP_FAILED) {
            if (exact && !guest_window) guest_window = p;
            else munmap(p, 1ull << 32);
        }
    }
    void *any = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    say("4 GB window anywhere: %p", any == MAP_FAILED ? NULL : any);
    if (any != MAP_FAILED) {
        if (!guest_window) guest_window = any;
        else munmap(any, 1ull << 32);
    }
    u64 low = 0, high = 256ull << 30;
    while (high - low > (1ull << 30)) {
        u64 mid = (low + high) / 2;
        void *p = mmap(NULL, mid, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) high = mid;
        else {
            munmap(p, mid);
            low = mid;
        }
    }
    say("largest single reservation: about %llu GB", low >> 30);
}
// How much can really be used, stopping well before the system kills the app.
static void test_memory_use(void) {
    if (!guest_window) {
        say("no window to touch");
        return;
    }
    u64 step = 256ull << 20, used = 0;
    double t0 = now_ms();
    u64 limit = 1ull << 32;
#if !(TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR)
    limit = 1ull << 30;
#endif
    while (used < limit) {
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
        if (available_memory() < 512ull << 20) {
            say("stopped with %.0f MB left", (double)available_memory() / 1e6);
            break;
        }
#endif
        for (u64 o = 0; o < step; o += 4096) guest_window[used + o] = 1;
        used += step;
        if (used % (1ull << 30) == 0) {
            u64 fp, rem;
            memory_now(&fp, &rem);
            say("touched %.1f GB: footprint %.0f MB, available %.0f MB", (double)used / 1e9, (double)fp / 1e6, (double)available_memory() / 1e6);
        }
    }
    say("usable guest memory: %.1f GB, in %.0f ms", (double)used / 1e9, now_ms() - t0);
    madvise(guest_window, used, MADV_FREE);
    munmap(guest_window, 1ull << 32);
    guest_window = NULL;
}

// ---------- unaligned accesses, the way translated code makes them ----------
static volatile u64 traps;
static void on_bus(int sig, siginfo_t *si, void *ctx) {
    (void)si;
    (void)sig;
    ucontext_t *uc = ctx;
    _STRUCT_ARM_THREAD_STATE64 *st = &uc->uc_mcontext->__ss;
    u32 ins = *(u32 *)st->__pc;
    u32 rt = ins & 31, rn = (ins >> 5) & 31;
    u8 *p = (u8 *)st->__x[rn];
    u32 v;
    if ((ins & 0xFFFFFC00u) == 0xB8BFC000u) {
        memcpy(&v, p, 4);
        st->__x[rt] = v;
    } else {
        v = (u32)st->__x[rt];
        memcpy(p, &v, 4);
    }
    st->__pc += 4;
    traps++;
}
static inline u32 ldapr32(const void *p) {
    u32 v;
    __asm__ volatile("ldapr %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline void stlr32(void *p, u32 v) { __asm__ volatile("stlr %w0, [%1]" : : "r"(v), "r"(p) : "memory"); }
static void test_unaligned(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_bus;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, NULL);
    static _Alignas(64) u8 buf[256];
    memset(buf, 7, sizeof buf);
    static const struct { const char *name; int off; int n; } cases[] = {{"aligned", 0, 20000000}, {"unaligned inside 16 bytes", 1, 20000000}, {"across 16 bytes (traps on the Mac)", 14, 200000}};
    for (unsigned k = 0; k < 3; k++) {
        traps = 0;
        double t0 = now_ms();
        u32 sum = 0;
        for (int i = 0; i < cases[k].n; i++) {
            sum += ldapr32(buf + cases[k].off);
            stlr32(buf + cases[k].off, sum);
        }
        double ms = now_ms() - t0;
        say("load and store %s: %.1f ns a pair, %llu traps", cases[k].name, ms * 1e6 / cases[k].n, traps);
    }
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = 0;
    sigaction(SIGBUS, &sa, NULL);
}

// ---------- a big stack, and code as large as the game's ----------
static void *touch_stack(void *arg) {
    volatile char *top = alloca(128 << 20);
    top[0] = 1;
    top[(128 << 20) - 1] = 1;
    *(int *)arg = 1;
    return NULL;
}
extern const u8 probe_large_text_start[], probe_large_text_end[];
static void test_stack_and_code(void) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    int e = pthread_attr_setstacksize(&a, 256u << 20);
    int ok = 0;
    pthread_t t;
    if (!e) e = pthread_create(&t, &a, touch_stack, &ok);
    if (!e) pthread_join(t, NULL);
    say("thread with a 256 MB stack: %s (error %d)", ok ? "works" : "failed", e);
#ifndef PROBE_CLI
    double t0 = now_ms();
    u32 end = *(const u32 *)probe_large_text_end;
    u32 sum = 0;
    for (const u8 *p = probe_large_text_start; p < probe_large_text_end; p += 16384) sum += *p;
    say("%.0f MB of code in the executable: reads back %#x, sum %u, %.0f ms to page in", (double)(probe_large_text_end - probe_large_text_start) / 1e6, end, sum, now_ms() - t0);
#endif
}

// ---------- processor speed ----------
static double bench_int(u64 n) {
    double t0 = now_ms();
    u64 h = 0x9E3779B97F4A7C15ull, s = 0;
    for (u64 i = 0; i < n; i++) {
        h = (h ^ (h >> 29)) * 0xBF58476D1CE4E5B9ull + i;
        s += h >> 60;
    }
    double ms = now_ms() - t0;
    __asm__ volatile("" : : "r"(s));
    return ms;
}
typedef struct { u64 n; double ms; } BenchArg;
static void *bench_thread(void *p) {
    BenchArg *b = p;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    b->ms = bench_int(b->n);
    return NULL;
}
static void test_cpu(void) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    u64 n = 300000000ull;
    bench_int(n / 10);
    say("one core, integer work: %.2f ns a step", bench_int(n) * 1e6 / (double)n);
    static _Alignas(64) u32 cell[16];
    double t0 = now_ms();
    u32 s = 0;
    for (int i = 0; i < 100000000; i++) {
        s += ldapr32(&cell[0]);
        stlr32(&cell[1], s);
    }
    say("ordered load and store (every guest memory access): %.2f ns a pair", (now_ms() - t0) * 1e6 / 1e8);
    size_t size = 64u << 20;
    u8 *a = malloc(size), *b = malloc(size);
    memset(a, 1, size);
    memset(b, 2, size);
    t0 = now_ms();
    void *(*volatile copy)(void *, const void *, size_t) = memcpy;
    for (int i = 0; i < 8; i++) copy(i & 1 ? a : b, i & 1 ? b : a, size);
    say("memcpy: %.1f GB/s", (double)size * 8 / 1e9 / ((now_ms() - t0) / 1000));
    free(a);
    free(b);
    static const int counts[] = {1, 2, 4, 6, 8, 12};
    for (unsigned k = 0; k < sizeof counts / sizeof counts[0]; k++) {
        int th = counts[k];
        if ((u64)th > sysctl_u64("hw.ncpu")) break;
        pthread_t t[16];
        BenchArg arg[16];
        t0 = now_ms();
        for (int i = 0; i < th; i++) {
            arg[i].n = 200000000ull;
            pthread_create(&t[i], NULL, bench_thread, &arg[i]);
        }
        double worst = 0;
        for (int i = 0; i < th; i++) {
            pthread_join(t[i], NULL);
            if (arg[i].ms > worst) worst = arg[i].ms;
        }
        say("%2d threads together: %.2f steps per ns in all, slowest thread %.2f ns a step", th, 2e8 * th / ((now_ms() - t0) * 1e6), worst * 1e6 / 2e8);
    }
}

// ---------- Metal ----------
static const char *msl_common =
    "#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V vs_main(uint vid [[vertex_id]], constant float4 *p [[buffer(0)]]) { V o; o.pos = p[vid]; o.uv = p[vid].xy; return o; }\n"
    "fragment float4 fs_fill(V i [[stage_in]]) { return float4(fract(i.uv * 37.0), 0.5, 1.0); }\n"
    "fragment float4 fs_light(V i [[stage_in]]) { return float4(0.01, 0.01, 0.01, 1.0); }\n";

static id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> dev, id<MTLLibrary> lib, const char *frag, MTLPixelFormat color, MTLPixelFormat depth, int samples, BOOL blend) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs_main"];
    d.fragmentFunction = [lib newFunctionWithName:[NSString stringWithUTF8String:frag]];
    d.rasterSampleCount = samples;
    d.colorAttachments[0].pixelFormat = color;
    d.depthAttachmentPixelFormat = depth;
    d.stencilAttachmentPixelFormat = depth;
    if (blend) {
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
    }
    NSError *e = nil;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) say("pipeline failed: %s", e.localizedDescription.UTF8String);
    return p;
}
static id<MTLTexture> make_target(id<MTLDevice> dev, MTLPixelFormat f, int w, int h, int samples) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    d.sampleCount = samples;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:d];
}
static void bench_gpu(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib, int w, int h, int samples) {
    MTLPixelFormat cf = MTLPixelFormatRGBA16Float, df = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLRenderPipelineState> fill = make_pipeline(dev, lib, "fs_fill", cf, df, samples, NO), light = make_pipeline(dev, lib, "fs_light", cf, df, samples, YES);
    id<MTLRenderPipelineState> small = make_pipeline(dev, lib, "fs_fill", MTLPixelFormatRG16Float, MTLPixelFormatInvalid, 1, NO);
    id<MTLTexture> color = make_target(dev, cf, w, h, samples), depth = make_target(dev, df, w, h, samples), resolve = make_target(dev, cf, w, h, 1), other = make_target(dev, MTLPixelFormatRG16Float, 512, 512, 1);
    MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionLess;
    dsd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> ds = [dev newDepthStencilStateWithDescriptor:dsd];
    enum { CX = 160, CY = 100, NV = CX * CY * 6 };
    id<MTLBuffer> grid = [dev newBufferWithLength:NV * 16 options:MTLResourceStorageModeShared];
    float *v = grid.contents;
    unsigned r = 12345;
    for (int y = 0, n = 0; y < CY; y++) {
        for (int x = 0; x < CX; x++) {
            float x0 = x * 2.0f / CX - 1, y0 = y * 2.0f / CY - 1, dx = 2.0f / CX, dy = 2.0f / CY;
            float q6[6][2] = {{x0, y0}, {x0 + dx, y0 + dy * 0.3f}, {x0 + dx * 0.2f, y0 + dy}, {x0 + dx, y0 + dy * 0.3f}, {x0 + dx, y0 + dy}, {x0 + dx * 0.2f, y0 + dy}};
            for (int k = 0; k < 6; k++, n++) {
                if (k % 3 == 0) r = r * 1664525u + 1013904223u;
                v[n * 4] = q6[k][0];
                v[n * 4 + 1] = q6[k][1];
                v[n * 4 + 2] = (float)(r >> 24) / 255.0f;
                v[n * 4 + 3] = 1;
            }
        }
    }
    static const float quad[24] = {-1, -1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
    for (int mode = 0; mode < 2; mode++) {
        int passes = mode ? 12 : 1;
        double total = 0;
        int frames = 12;
        for (int f = 0; f < frames; f++) {
            @autoreleasepool {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                for (int p = 0; p < passes; p++) {
                    if (p) {
                        MTLRenderPassDescriptor *o = [MTLRenderPassDescriptor renderPassDescriptor];
                        o.colorAttachments[0].texture = other;
                        o.colorAttachments[0].loadAction = MTLLoadActionClear;
                        o.colorAttachments[0].storeAction = MTLStoreActionStore;
                        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:o];
                        [e setRenderPipelineState:small];
                        [e setVertexBytes:quad length:sizeof quad atIndex:0];
                        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                        [e endEncoding];
                    }
                    MTLRenderPassDescriptor *m = [MTLRenderPassDescriptor renderPassDescriptor];
                    m.colorAttachments[0].texture = color;
                    m.colorAttachments[0].loadAction = p ? MTLLoadActionLoad : MTLLoadActionClear;
                    m.depthAttachment.texture = depth;
                    m.depthAttachment.loadAction = p ? MTLLoadActionLoad : MTLLoadActionClear;
                    m.depthAttachment.storeAction = MTLStoreActionStore;
                    m.stencilAttachment.texture = depth;
                    m.stencilAttachment.loadAction = p ? MTLLoadActionLoad : MTLLoadActionClear;
                    m.stencilAttachment.storeAction = MTLStoreActionStore;
                    if (samples > 1 && p == passes - 1) {
                        m.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
                        m.colorAttachments[0].resolveTexture = resolve;
                    } else {
                        m.colorAttachments[0].storeAction = MTLStoreActionStore;
                    }
                    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:m];
                    [e setDepthStencilState:ds];
                    if (!p) {
                        [e setRenderPipelineState:fill];
                        [e setVertexBuffer:grid offset:0 atIndex:0];
                        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:NV];
                    }
                    [e setRenderPipelineState:light];
                    [e setVertexBytes:quad length:sizeof quad atIndex:0];
                    // With one pass the twelve lights share it, as if the game had no shadow maps in between.
                    for (int l = 0; l < (mode ? 1 : 12); l++) [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                    [e endEncoding];
                }
                [cb commit];
                [cb waitUntilCompleted];
                if (f >= 2) total += (cb.GPUEndTime - cb.GPUStartTime) * 1000;
            }
        }
        say("GPU %dx%d x%d: %s: %.2f ms a frame", w, h, samples, mode ? "12 whole-screen lights, each in a pass of its own" : "12 whole-screen lights in one pass", total / (frames - 2));
    }
}
static NSString *generated_shader(unsigned seed) {
    NSMutableString *s = [NSMutableString stringWithUTF8String:msl_common];
    [s appendString:@"fragment float4 fs_gen(V i [[stage_in]], texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]], sampler s [[sampler(0)]], constant float4 *c [[buffer(0)]]) {\n float4 r0 = t0.sample(s, i.uv), r1 = t1.sample(s, i.uv), r2 = c[0], r3 = c[1];\n"];
    unsigned r = seed * 2654435761u + 77;
    for (int k = 0; k < 150; k++) {
        r = r * 1664525u + 1013904223u;
        int a = (r >> 8) % 4, b = (r >> 12) % 4, d = (r >> 16) % 4;
        float f = (float)((r >> 20) % 1000) / 1000.0f;
        switch ((r >> 4) % 5) {
            case 0: [s appendFormat:@" r%d = r%d * %f;\n", d, a, f]; break;
            case 1: [s appendFormat:@" r%d = r%d + r%d.wzyx;\n", d, a, b]; break;
            case 2: [s appendFormat:@" r%d = mix(r%d, r%d, %f);\n", d, a, b, f]; break;
            case 3: [s appendFormat:@" r%d.x = dot(r%d.xyz, r%d.xyz);\n", d, a, b]; break;
            case 4: [s appendFormat:@" r%d = t%d.sample(s, r%d.xy);\n", d, b & 1, a]; break;
        }
    }
    [s appendString:@" return r0 + r1 + r2 + r3;\n}\n"];
    return s;
}
static void bench_compile(id<MTLDevice> dev) {
    int n = 32;
    double t0 = now_ms();
    NSMutableArray *libs = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        NSError *e = nil;
        id<MTLLibrary> l = [dev newLibraryWithSource:generated_shader((unsigned)(1000 + i)) options:nil error:&e];
        if (!l) {
            say("library failed: %s", e.localizedDescription.UTF8String);
            return;
        }
        [libs addObject:l];
    }
    say("source to library, 150 line shader, one at a time: %.1f ms each", (now_ms() - t0) / n);
    t0 = now_ms();
    for (int i = 0; i < n; i++) make_pipeline(dev, libs[i], "fs_gen", MTLPixelFormatRGBA16Float, MTLPixelFormatDepth32Float_Stencil8, 4, YES);
    say("library to pipeline, one at a time: %.1f ms each", (now_ms() - t0) / n);
    NSMutableArray *libs2 = [NSMutableArray array];
    for (int i = 0; i < n; i++) [libs2 addObject:[dev newLibraryWithSource:generated_shader((unsigned)(5000 + i)) options:nil error:nil]];
    t0 = now_ms();
    dispatch_apply((size_t)n, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
        make_pipeline(dev, libs2[i], "fs_gen", MTLPixelFormatRGBA16Float, MTLPixelFormatDepth32Float_Stencil8, 4, YES);
    });
    say("library to pipeline, all threads at once: %.1f ms each on the wall clock", (now_ms() - t0) / n);
    t0 = now_ms();
    for (int i = 0; i < n; i++) make_pipeline(dev, libs[i], "fs_gen", MTLPixelFormatRGBA16Float, MTLPixelFormatDepth32Float_Stencil8, 4, YES);
    say("the same pipelines again: %.2f ms each", (now_ms() - t0) / n);
}
static void test_metal(void) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) {
        say("no Metal device");
        return;
    }
    say("GPU: %s, working set %.1f GB, largest buffer %.1f GB", dev.name.UTF8String, (double)dev.recommendedMaxWorkingSetSize / 1e9, (double)dev.maxBufferLength / 1e9);
    static const struct { MTLGPUFamily f; const char *n; } fam[] = {{MTLGPUFamilyApple7, "Apple7"}, {MTLGPUFamilyApple8, "Apple8"}, {MTLGPUFamilyApple9, "Apple9"}, {(MTLGPUFamily)1010, "Apple10"}, {MTLGPUFamilyMac2, "Mac2"}};
    NSMutableString *fs = [NSMutableString string];
    for (unsigned i = 0; i < sizeof fam / sizeof fam[0]; i++)
        if ([dev supportsFamily:fam[i].f]) [fs appendFormat:@" %s", fam[i].n];
    say("GPU families:%s", fs.UTF8String);
    int bc = -1;
    if ([dev respondsToSelector:@selector(supportsBCTextureCompression)]) bc = [dev supportsBCTextureCompression];
    say("DXT (BC) compressed textures, which all the game's textures are: %s", bc < 0 ? "not reported" : bc ? "supported" : "NOT supported");
    if (bc > 0) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC3_RGBA_sRGB width:256 height:256 mipmapped:YES];
        say("creating a BC3 sRGB texture: %s", [dev newTextureWithDescriptor:d] ? "works" : "fails");
    }
    say("sample counts: 2 %d, 4 %d, 8 %d", [dev supportsTextureSampleCount:2], [dev supportsTextureSampleCount:4], [dev supportsTextureSampleCount:8]);
#if !TARGET_OS_IPHONE
    say("Depth24 Stencil8 format: %d", dev.depth24Stencil8PixelFormatSupported);
#endif
    say("clamp to border sampling: %d", (int)[dev supportsFamily:MTLGPUFamilyApple7]);
    say("argument buffers tier %d, read-write textures tier %d", (int)dev.argumentBuffersSupport, (int)dev.readWriteTextureSupport);
    id<MTLCommandQueue> q = [dev newCommandQueue];
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:msl_common] options:nil error:&e];
    if (!lib) {
        say("the test shaders don't compile: %s", e.localizedDescription.UTF8String);
        return;
    }
    static const struct { int w, h, s; } sizes[] = {{2560, 1600, 1}, {2560, 1600, 2}, {2560, 1600, 4}, {1280, 800, 1}, {1280, 800, 4}};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) @autoreleasepool {
        if ([dev supportsTextureSampleCount:(NSUInteger)sizes[i].s]) bench_gpu(dev, q, lib, sizes[i].w, sizes[i].h, sizes[i].s);
    }
    bench_compile(dev);
}

// ---------- files ----------
static void test_files(void) {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (!docs) docs = NSTemporaryDirectory();
    NSError *err = nil;
    NSDictionary *v = [[NSURL fileURLWithPath:docs] resourceValuesForKeys:@[NSURLVolumeAvailableCapacityForImportantUsageKey] error:&err];
    NSNumber *free_ = v[NSURLVolumeAvailableCapacityForImportantUsageKey];
    say("free space: %.0f GB (the game's data is 9.2 GB)", free_.doubleValue / 1e9);
    NSString *path = [docs stringByAppendingPathComponent:@"probe_file.bin"];
    size_t chunk = 1u << 20, total = 1024;
    u8 *b = malloc(chunk);
    for (size_t i = 0; i < chunk; i++) b[i] = (u8)(i * 2654435761u >> 24);
    int fd = open(path.fileSystemRepresentation, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        say("can't write a test file");
        free(b);
        return;
    }
    double t0 = now_ms();
    for (size_t i = 0; i < total; i++) write(fd, b, chunk);
    fsync(fd);
    say("writing 1 GB: %.0f MB/s", (double)total / ((now_ms() - t0) / 1000));
    fcntl(fd, F_NOCACHE, 1);
    lseek(fd, 0, SEEK_SET);
    t0 = now_ms();
    for (size_t i = 0; i < total; i++) read(fd, b, chunk);
    say("reading it back: %.0f MB/s", (double)total / ((now_ms() - t0) / 1000));
    t0 = now_ms();
    unsigned r = 99;
    for (int i = 0; i < 2000; i++) {
        r = r * 1664525u + 1013904223u;
        pread(fd, b, 65536, (off_t)(r % (total - 1)) << 20);
    }
    say("random 64 KB reads: %.0f us each", (now_ms() - t0) * 1000 / 2000);
    close(fd);
    unlink(path.fileSystemRepresentation);
    free(b);
}

// ---------- running it all ----------
static void run_all(void) {
    say("--- device");
    test_device();
    say("--- address space");
    test_address_space();
    say("--- unaligned accesses");
    test_unaligned();
    say("--- stack and code size");
    test_stack_and_code();
    say("--- processor");
    test_cpu();
    say("--- graphics");
    test_metal();
    say("--- files");
    test_files();
    say("--- using memory, last because the system may kill an app that goes too far");
    test_memory_use();
    say("done");
}

#ifdef PROBE_CLI
int main(void) {
    @autoreleasepool {
        run_all();
    }
    return 0;
}
#else
static UITextView *text_view;
static NSMutableString *text;
static void show_line(const char *line) {
    NSString *s = [NSString stringWithUTF8String:line];
    dispatch_async(dispatch_get_main_queue(), ^{
        [text appendFormat:@"%@\n", s];
        text_view.text = text;
        [text_view scrollRangeToVisible:NSMakeRange(text.length ? text.length - 1 : 0, 1)];
    });
}
@interface ProbeSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation ProbeSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    UIViewController *vc = [UIViewController new];
    text_view = [[UITextView alloc] initWithFrame:vc.view.bounds];
    text_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    text_view.editable = NO;
    text_view.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    [vc.view addSubview:text_view];
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    text = [NSMutableString string];
    ui_line = show_line;
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    out_file = fopen([docs stringByAppendingPathComponent:@"probe.txt"].fileSystemRepresentation, "w");
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    pthread_t t;
    pthread_create(&t, &a, (void *(*)(void *))(void *)run_all, NULL);
    pthread_detach(t);
}
@end
@interface ProbeAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation ProbeAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = ProbeSceneDelegate.class;
    return c;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(ProbeAppDelegate.class));
    }
}
#endif
