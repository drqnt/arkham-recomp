// Sound. FMOD drives a real output unit through a render callback. The Bink library uses the old Sound Manager, which no
// longer exists on the host, so its channels are played here. Both produce samples on a guest thread of their own and hand
// them to the system's audio thread through a ring, because that thread must not wait for the guest gate.
#include "shim.h"
#include "shim_reg.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

u32 cf_wrap(CFTypeRef o);

// ---------- a ring of interleaved float samples, written by one thread and read by the audio thread ----------
typedef struct {
    float *buf;
    u32 cap, channels;  // cap is in frames and a power of two
    _Atomic u32 rd, wr;
    _Atomic float gain_l, gain_r;
    _Atomic int paused;
    dispatch_semaphore_t space;
} Ring;

static void ring_init(Ring *r, u32 frames, u32 channels) {
    r->cap = frames;
    r->channels = channels;
    r->buf = calloc((size_t)frames * channels, sizeof(float));
    atomic_store(&r->gain_l, 1.0f);
    atomic_store(&r->gain_r, 1.0f);
    r->space = dispatch_semaphore_create(0);
}
static inline u32 ring_fill(const Ring *r) { return atomic_load(&r->wr) - atomic_load(&r->rd); }
static void ring_push(Ring *r, const float *src, u32 frames) {
    u32 w = atomic_load(&r->wr);
    for (u32 i = 0; i < frames; i++) {
        memcpy(r->buf + (size_t)((w + i) & (r->cap - 1)) * r->channels, src + (size_t)i * r->channels, r->channels * sizeof(float));
    }
    atomic_store(&r->wr, w + frames);
}
// Runs on the system's audio thread: no locks, no guest code.
static OSStatus ring_render(Ring *r, UInt32 frames, AudioBufferList *io) {
    float *out = io->mBuffers[0].mData;
    u32 ch = r->channels;
    u32 have = atomic_load(&r->paused) ? 0 : ring_fill(r);
    u32 n = have < frames ? have : frames;
    u32 rd = atomic_load(&r->rd);
    float gl = atomic_load(&r->gain_l), gr = atomic_load(&r->gain_r);
    for (u32 i = 0; i < n; i++) {
        const float *s = r->buf + (size_t)((rd + i) & (r->cap - 1)) * ch;
        for (u32 k = 0; k < ch; k++) out[i * ch + k] = s[k] * (k == 1 ? gr : gl);
    }
    memset(out + (size_t)n * ch, 0, (size_t)(frames - n) * ch * sizeof(float));
    atomic_store(&r->rd, rd + n);
    dispatch_semaphore_signal(r->space);
    return noErr;
}

// ---------- CoreAudio properties: the old calls the game makes, answered through the current object API ----------
static AudioObjectPropertyAddress dev_addr(u32 chan, u32 is_input, u32 id) {
    AudioObjectPropertyAddress a = {id, is_input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput, chan};
    return a;
}
#define FOURCC(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))
#define PROP_UID FOURCC('u', 'i', 'd', ' ')
#define PROP_NAME FOURCC('n', 'a', 'm', 'e')
#define PROP_STREAM_CONFIG FOURCC('s', 'l', 'a', 'y')

static int audio_trace = -1;
static void trace_prop(const char *what, u32 obj, u32 id, u32 chan, u32 input, int r, u32 size) {
    if (audio_trace < 0) audio_trace = getenv("ARKHAM_TRACE_AUDIO") != NULL;
    if (!audio_trace) return;
    fprintf(stderr, "[audio] %s obj=%u '%c%c%c%c' chan=%u input=%u -> %d size=%u\n", what, obj, (char)(id >> 24), (char)(id >> 16), (char)(id >> 8), (char)id, chan, input, r, size);
}
SHIM(AudioHardwareGetProperty) {
    AudioObjectPropertyAddress a = {ARG(0), kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = G32(ARG(1));
    OSStatus r = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, GP(ARG(2)));
    G32(ARG(1)) = sz;
    trace_prop("hw get", 1, ARG(0), 0, 0, r, sz);
    RET((i32)r);
}
SHIM(AudioHardwareGetPropertyInfo) {
    AudioObjectPropertyAddress a = {ARG(0), kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    OSStatus r = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    if (ARG(1)) G32(ARG(1)) = sz;
    if (ARG(2)) G8(ARG(2)) = 0;
    trace_prop("hw info", 1, ARG(0), 0, 0, r, sz);
    RET((i32)r);
}
SHIM(AudioDeviceGetProperty) {
    u32 dev = ARG(0), id = ARG(3), gsize = ARG(4), gout = ARG(5);
    AudioObjectPropertyAddress a = dev_addr(ARG(1), ARG(2) & 0xFF, id);
    // A device's UID is a CF object, a handle on the guest's side.
    if (id == PROP_UID) {
        CFStringRef s = NULL;
        UInt32 sz = sizeof s;
        a.mScope = kAudioObjectPropertyScopeGlobal;
        a.mElement = kAudioObjectPropertyElementMain;
        OSStatus r = AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &s);
        if (!r) {
            G32(gout) = cf_wrap(s);
            G32(gsize) = 4;
        }
        RET((i32)r);
        return;
    }
    // The old C-string name is gone from the object API.
    if (id == PROP_NAME) {
        CFStringRef s = NULL;
        UInt32 sz = sizeof s;
        AudioObjectPropertyAddress n = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        OSStatus r = AudioObjectGetPropertyData(dev, &n, 0, NULL, &sz, &s);
        if (!r && s) {
            u32 cap = G32(gsize);
            if (cap) {
                CFStringGetCString(s, (char *)GP(gout), cap, kCFStringEncodingUTF8);
                G32(gsize) = (u32)strlen(GS(gout)) + 1;
            }
            CFRelease(s);
        }
        RET((i32)r);
        return;
    }
    // A buffer list holds pointers, so its layout differs between 32 and 64 bits.
    if (id == PROP_STREAM_CONFIG) {
        UInt32 hsz = 0;
        OSStatus r = AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &hsz);
        if (!r) {
            AudioBufferList *l = malloc(hsz);
            r = AudioObjectGetPropertyData(dev, &a, 0, NULL, &hsz, l);
            if (!r) {
                u32 need = 4 + l->mNumberBuffers * 12;
                if (need <= G32(gsize)) {
                    G32(gout) = l->mNumberBuffers;
                    for (u32 i = 0; i < l->mNumberBuffers; i++) {
                        G32(gout + 4 + i * 12) = l->mBuffers[i].mNumberChannels;
                        G32(gout + 8 + i * 12) = l->mBuffers[i].mDataByteSize;
                        G32(gout + 12 + i * 12) = 0;
                    }
                }
                G32(gsize) = need;
            }
            free(l);
        }
        trace_prop("dev get", dev, id, ARG(1), ARG(2) & 0xFF, r, G32(gsize));
        RET((i32)r);
        return;
    }
    UInt32 sz = G32(gsize);
    OSStatus r = AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, GP(gout));
    G32(gsize) = sz;
    trace_prop("dev get", dev, id, ARG(1), ARG(2) & 0xFF, r, sz);
    RET((i32)r);
}
SHIM(AudioDeviceGetPropertyInfo) {
    u32 dev = ARG(0), id = ARG(3);
    if (id == PROP_UID || id == PROP_NAME) {
        if (ARG(4)) G32(ARG(4)) = id == PROP_UID ? 4 : 256;
        if (ARG(5)) G8(ARG(5)) = 0;
        RET(0);
        return;
    }
    AudioObjectPropertyAddress a = dev_addr(ARG(1), ARG(2) & 0xFF, id);
    UInt32 sz = 0;
    OSStatus r = AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &sz);
    if (!r && id == PROP_STREAM_CONFIG) {
        AudioBufferList *l = malloc(sz);
        r = AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, l);
        if (!r) sz = 4 + l->mNumberBuffers * 12;
        free(l);
    }
    if (ARG(4)) G32(ARG(4)) = sz;
    if (ARG(5)) G8(ARG(5)) = 0;
    trace_prop("dev info", dev, id, ARG(1), ARG(2) & 0xFF, r, sz);
    RET((i32)r);
}
SHIM(AudioDeviceSetProperty) {
    AudioObjectPropertyAddress a = dev_addr(ARG(2), ARG(3) & 0xFF, ARG(4));
    RET((i32)AudioObjectSetPropertyData(ARG(0), &a, 0, NULL, ARG(5), GP(ARG(6))));
}

// ---------- output units for FMOD ----------
#define PROP_STREAM_FORMAT 8
#define PROP_RENDER_CALLBACK 23
#define SCOPE_INPUT 1
#define OUT_CHUNK 512u
#define OUT_TARGET 1536u

typedef struct {
    AudioComponentInstance unit;
    u32 proc, refcon;
    u32 channels;
    Ring ring;
    pthread_t pump;
    _Atomic int running;
    int pump_started;
} GOut;

static OSStatus out_render(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)flags;
    (void)ts;
    (void)bus;
    GOut *o = ctx;
    if (!o->ring.buf || io->mNumberBuffers != 1) {
        for (UInt32 i = 0; i < io->mNumberBuffers; i++) memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize);
        return noErr;
    }
    return ring_render(&o->ring, frames, io);
}

// Calls the guest's render callback on a guest thread of its own whenever the ring runs low.
static void *out_pump(void *p) {
    GOut *o = p;
    rt_gate_acquire();
    CPU *c = rt_new_cpu(1u << 20);
    u32 g_flags = rt_guest_calloc(1, 4), g_ts = rt_guest_calloc(1, 64), g_list = rt_guest_calloc(1, 16);
    u32 bytes = OUT_CHUNK * o->channels * 4;
    u32 g_buf = rt_guest_calloc(1, bytes);
    rt_gate_release();
    double sample_time = 0;
    while (atomic_load(&o->running)) {
        if (ring_fill(&o->ring) >= OUT_TARGET) {
            dispatch_semaphore_wait(o->ring.space, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC));
            continue;
        }
        rt_gate_acquire();
        G32(g_flags) = 0;
        *(f64a *)GP(g_ts) = sample_time;
        G32(g_ts + 56) = kAudioTimeStampSampleTimeValid;
        G32(g_list) = 1;
        G32(g_list + 4) = o->channels;
        G32(g_list + 8) = bytes;
        G32(g_list + 12) = g_buf;
        u32 a[6] = {o->refcon, g_flags, g_ts, 0, OUT_CHUNK, g_list};
        rt_call_guest(c, o->proc, 6, a);
        ring_push(&o->ring, (const float *)GP(g_buf), OUT_CHUNK);
        rt_gate_release();
        sample_time += OUT_CHUNK;
    }
    return NULL;
}

SHIM(AudioComponentFindNext) {
    AudioComponent prev = ARG(0) ? rt_handle_get(ARG(0)) : NULL;
    AudioComponent found = AudioComponentFindNext(prev, (const AudioComponentDescription *)GP(ARG(1)));
    if (audio_trace > 0) fprintf(stderr, "[audio] component %p\n", (void *)found);
    RET(found ? rt_handle_new((void *)found) : 0);
}
SHIM(AudioComponentInstanceNew) {
    AudioComponentInstance unit = NULL;
    OSStatus r = AudioComponentInstanceNew(rt_handle_get(ARG(0)), &unit);
    if (!r) {
        GOut *o = calloc(1, sizeof *o);
        o->unit = unit;
        G32(ARG(1)) = rt_handle_new(o);
    }
    RET((i32)r);
}
SHIM(AudioComponentInstanceDispose) {
    GOut *o = rt_handle_get(ARG(0));
    if (!o) {
        RET((u32)kAudio_ParamError);
        return;
    }
    RET((i32)AudioComponentInstanceDispose(o->unit));
}
SHIM(AudioUnitSetProperty) {
    GOut *o = rt_handle_get(ARG(0));
    if (!o) {
        RET((u32)kAudio_ParamError);
        return;
    }
    u32 id = ARG(1), scope = ARG(2), elem = ARG(3), data = ARG(4), size = ARG(5);
    if (id == PROP_RENDER_CALLBACK) {
        o->proc = G32(data);
        o->refcon = G32(data + 4);
        AURenderCallbackStruct cb = {out_render, o};
        RET((i32)AudioUnitSetProperty(o->unit, id, scope, elem, &cb, sizeof cb));
        return;
    }
    if (id == PROP_STREAM_FORMAT && scope == SCOPE_INPUT) {
        const AudioStreamBasicDescription *f = (const AudioStreamBasicDescription *)GP(data);
        o->channels = f->mChannelsPerFrame;
        fprintf(stderr, "[audio] output %.0f Hz, %u channels, flags %#x, %u bits\n", f->mSampleRate, f->mChannelsPerFrame, f->mFormatFlags, f->mBitsPerChannel);
    }
    OSStatus r = AudioUnitSetProperty(o->unit, id, scope, elem, data ? GP(data) : NULL, size);
    if (audio_trace > 0 || r) fprintf(stderr, "[audio] unit set prop %u scope %u elem %u size %u -> %d\n", id, scope, elem, size, (int)r);
    RET((i32)r);
}
SHIM(AudioUnitGetProperty) {
    GOut *o = rt_handle_get(ARG(0));
    if (!o) {
        RET((u32)kAudio_ParamError);
        return;
    }
    UInt32 sz = G32(ARG(5));
    OSStatus r = AudioUnitGetProperty(o->unit, ARG(1), ARG(2), ARG(3), GP(ARG(4)), &sz);
    G32(ARG(5)) = sz;
    RET((i32)r);
}
SHIM(AudioUnitInitialize) {
    GOut *o = rt_handle_get(ARG(0));
    if (!o) {
        RET((u32)kAudio_ParamError);
        return;
    }
    RET((i32)AudioUnitInitialize(o->unit));
}
SHIM(AudioUnitUninitialize) {
    GOut *o = rt_handle_get(ARG(0));
    if (!o) {
        RET((u32)kAudio_ParamError);
        return;
    }
    RET((i32)AudioUnitUninitialize(o->unit));
}
SHIM(AudioOutputUnitStart) {
    GOut *o = rt_handle_get(ARG(0));
    if (!o) {
        RET((u32)kAudio_ParamError);
        return;
    }
    if (!o->pump_started && o->proc && o->channels) {
        o->pump_started = 1;
        ring_init(&o->ring, 8192, o->channels);
        atomic_store(&o->running, 1);
        pthread_create(&o->pump, NULL, out_pump, o);
    }
    OSStatus r = AudioOutputUnitStart(o->unit);
    fprintf(stderr, "[audio] output unit started: %d\n", (int)r);
    RET((i32)r);
}
SHIM(AudioOutputUnitStop) {
    GOut *o = rt_handle_get(ARG(0));
    if (!o) {
        RET((u32)kAudio_ParamError);
        return;
    }
    OSStatus r = AudioOutputUnitStop(o->unit);
    if (o->pump_started) {
        atomic_store(&o->running, 0);
        // The pump may be waiting for the gate to finish its last callback.
        rt_gate_release();
        pthread_join(o->pump, NULL);
        rt_gate_acquire();
        o->pump_started = 0;
    }
    RET((i32)r);
}

// ---------- the Sound Manager, for the Bink library's channels ----------
#define SND_QUIET 3
#define SND_FLUSH 4
#define SND_PAUSE 11
#define SND_RESUME 12
#define SND_CALLBACK 13
#define SND_VOLUME 46
#define SND_BUFFER 81
#define SND_NOT_ENOUGH_HARDWARE ((u32)-201)
#define SND_QUEUE 1024u
#define SND_RING 65536u
#define SND_LOW_WATER 2048u

typedef struct { u16 cmd; i16 p1; u32 p2; } SndCmd;
typedef struct {
    u32 handle, callback;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    SndCmd q[SND_QUEUE];
    u32 head, tail;
    int working, dying;
    _Atomic u32 generation;
    Ring ring;
    AudioComponentInstance unit;
    u32 rate, channels;
    int swap_known, swap;
} SndChan;

static OSStatus snd_render(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)flags;
    (void)ts;
    (void)bus;
    return ring_render(&((SndChan *)ctx)->ring, frames, io);
}
static void snd_open_output(SndChan *s, u32 rate, u32 channels) {
    if (s->unit) {
        if (s->rate == rate && s->channels == channels) return;
        AudioOutputUnitStop(s->unit);
        AudioComponentInstanceDispose(s->unit);
        s->unit = NULL;
    }
    float gl = 1, gr = 1;
    if (s->ring.buf) {
        gl = atomic_load(&s->ring.gain_l);
        gr = atomic_load(&s->ring.gain_r);
        free(s->ring.buf);
    }
    dispatch_semaphore_t sem = s->ring.space;
    int paused = atomic_load(&s->ring.paused);
    ring_init(&s->ring, SND_RING, channels);
    if (sem) s->ring.space = sem;
    atomic_store(&s->ring.gain_l, gl);
    atomic_store(&s->ring.gain_r, gr);
    atomic_store(&s->ring.paused, paused);
    s->rate = rate;
    s->channels = channels;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &s->unit)) {
        fprintf(stderr, "[audio] no default output for video sound\n");
        s->unit = NULL;
        return;
    }
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = channels * 4;
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 32;
    AudioUnitSetProperty(s->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = {snd_render, s};
    AudioUnitSetProperty(s->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    AudioUnitInitialize(s->unit);
    OSStatus r = AudioOutputUnitStart(s->unit);
    fprintf(stderr, "[audio] video sound %u Hz, %u channels: %d\n", rate, channels, (int)r);
}
// An extended sound header holds 16-bit samples, big-endian by the old convention. Check the data rather than assume:
// the wrong byte order makes neighbouring samples jump far apart.
static void snd_detect_order(SndChan *s, const u8 *p, u32 samples, u32 channels) {
    if (s->swap_known) return;
    u64 le = 0, be = 0;
    if (samples > 4096) samples = 4096;
    for (u32 i = channels; i < samples; i++) {
        const u8 *a = p + (size_t)i * 2, *b = p + (size_t)(i - channels) * 2;
        i16 al = (i16)(a[0] | (a[1] << 8)), bl = (i16)(b[0] | (b[1] << 8));
        i16 ab = (i16)(a[1] | (a[0] << 8)), bb = (i16)(b[1] | (b[0] << 8));
        le += (u64)abs((int)al - (int)bl);
        be += (u64)abs((int)ab - (int)bb);
    }
    if (le + be == 0) return;
    s->swap = be < le;
    s->swap_known = 1;
    fprintf(stderr, "[audio] video sound byte order: %s\n", s->swap ? "big-endian" : "little-endian");
}
static void snd_play_buffer(SndChan *s, u32 hdr) {
    u32 data = G32(hdr), channels = G32(hdr + 4), fixed = G32(hdr + 8);
    u32 frames = G32(hdr + 0x16);
    u32 bits = G16(hdr + 0x30);
    u32 gen = atomic_load(&s->generation);
    if (!channels || channels > 8 || bits != 16) {
        static int warned;
        if (!warned++) fprintf(stderr, "[audio] unexpected video sound format: %u channels, %u bits\n", channels, bits);
        return;
    }
    snd_open_output(s, fixed >> 16, channels);
    const u8 *p = (const u8 *)GP(data);
    snd_detect_order(s, p, frames * channels, channels);
    float tmp[512 * 8];
    for (u32 done = 0; done < frames;) {
        while (s->ring.cap - ring_fill(&s->ring) < 512) {
            if (atomic_load(&s->generation) != gen) return;
            dispatch_semaphore_wait(s->ring.space, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC));
        }
        u32 n = frames - done < 512 ? frames - done : 512;
        for (u32 i = 0; i < n * channels; i++) {
            const u8 *q = p + (size_t)(done * channels + i) * 2;
            i16 v = s->swap ? (i16)(q[1] | (q[0] << 8)) : (i16)(q[0] | (q[1] << 8));
            tmp[i] = (float)v / 32768.0f;
        }
        ring_push(&s->ring, tmp, n);
        done += n;
    }
    // A buffer counts as played once little of it is left, which keeps the video's clock close to what is heard.
    while (ring_fill(&s->ring) > SND_LOW_WATER && atomic_load(&s->generation) == gen && !atomic_load(&s->ring.paused)) {
        dispatch_semaphore_wait(s->ring.space, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC));
    }
}
static void *snd_thread(void *p) {
    SndChan *s = p;
    rt_gate_acquire();
    CPU *c = rt_new_cpu(1u << 20);
    u32 g_cmd = rt_guest_calloc(1, 8);
    rt_gate_release();
    for (;;) {
        pthread_mutex_lock(&s->mu);
        s->working = 0;
        while (s->head == s->tail && !s->dying) pthread_cond_wait(&s->cv, &s->mu);
        if (s->dying) {
            pthread_mutex_unlock(&s->mu);
            return NULL;
        }
        SndCmd cmd = s->q[s->head++ % SND_QUEUE];
        s->working = 1;
        pthread_mutex_unlock(&s->mu);
        if (cmd.cmd == SND_BUFFER) {
            snd_play_buffer(s, cmd.p2);
        } else if (cmd.cmd == SND_CALLBACK) {
            rt_gate_acquire();
            G16(g_cmd) = cmd.cmd;
            G16(g_cmd + 2) = (u16)cmd.p1;
            G32(g_cmd + 4) = cmd.p2;
            u32 a[2] = {s->handle, g_cmd};
            rt_call_guest(c, s->callback, 2, a);
            rt_gate_release();
        }
    }
}
SHIM(SndNewChannel) {
    SndChan *s = calloc(1, sizeof *s);
    s->callback = ARG(3);
    s->handle = rt_handle_new(s);
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->ring.space = dispatch_semaphore_create(0);
    atomic_store(&s->ring.gain_l, 1.0f);
    atomic_store(&s->ring.gain_r, 1.0f);
    pthread_create(&s->thread, NULL, snd_thread, s);
    G32(ARG(0)) = s->handle;
    RET(0);
}
SHIM(SndDisposeChannel) {
    SndChan *s = rt_handle_get(ARG(0));
    pthread_mutex_lock(&s->mu);
    s->dying = 1;
    s->head = s->tail = 0;
    atomic_fetch_add(&s->generation, 1);
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
    // The channel's thread may be waiting for the gate to run a callback.
    rt_gate_release();
    pthread_join(s->thread, NULL);
    rt_gate_acquire();
    if (s->unit) {
        AudioOutputUnitStop(s->unit);
        AudioComponentInstanceDispose(s->unit);
    }
    rt_handle_free(s->handle);
    RET(0);
}
SHIM(SndDoCommand) {
    SndChan *s = rt_handle_get(ARG(0));
    SndCmd cmd = {G16(ARG(1)), (i16)G16(ARG(1) + 2), G32(ARG(1) + 4)};
    pthread_mutex_lock(&s->mu);
    if (s->tail - s->head >= SND_QUEUE) {
        pthread_mutex_unlock(&s->mu);
        RET((u32)-203);  // queueFull
        return;
    }
    s->q[s->tail++ % SND_QUEUE] = cmd;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
    RET(0);
}
SHIM(SndDoImmediate) {
    SndChan *s = rt_handle_get(ARG(0));
    u32 cmd = G16(ARG(1)), p2 = G32(ARG(1) + 4);
    switch (cmd) {
        case SND_FLUSH:
            pthread_mutex_lock(&s->mu);
            s->head = s->tail = 0;
            pthread_mutex_unlock(&s->mu);
            atomic_fetch_add(&s->generation, 1);
            break;
        case SND_QUIET:
            atomic_fetch_add(&s->generation, 1);
            atomic_store(&s->ring.rd, atomic_load(&s->ring.wr));
            break;
        case SND_PAUSE:
            atomic_store(&s->ring.paused, 1);
            break;
        case SND_RESUME:
            atomic_store(&s->ring.paused, 0);
            break;
        case SND_VOLUME:
            atomic_store(&s->ring.gain_l, (float)(p2 & 0xFFFF) / 256.0f);
            atomic_store(&s->ring.gain_r, (float)(p2 >> 16) / 256.0f);
            break;
        default: {
            static u8 seen[256];
            if (cmd < 256 && !seen[cmd]) {
                seen[cmd] = 1;
                fprintf(stderr, "[audio] Sound Manager command %u ignored\n", cmd);
            }
        }
    }
    RET(0);
}
SHIM(SndChannelStatus) {
    SndChan *s = rt_handle_get(ARG(0));
    u32 len = ARG(1), st = ARG(2);
    memset(GP(st), 0, len);
    pthread_mutex_lock(&s->mu);
    int busy = s->head != s->tail || s->working;
    pthread_mutex_unlock(&s->mu);
    if (s->ring.buf && ring_fill(&s->ring)) busy = 1;
    if (len > 12) G8(st + 12) = (u8)busy;
    if (len > 14) G8(st + 14) = (u8)atomic_load(&s->ring.paused);
    RET(0);
}

const ShimReg rt_shims_audio[] = {
    R(AudioHardwareGetProperty) R(AudioHardwareGetPropertyInfo) R(AudioDeviceGetProperty) R(AudioDeviceGetPropertyInfo) R(AudioDeviceSetProperty)
    R(AudioComponentFindNext) R(AudioComponentInstanceNew) R(AudioComponentInstanceDispose)
    R(AudioUnitSetProperty) R(AudioUnitGetProperty) R(AudioUnitInitialize) R(AudioUnitUninitialize)
    R(AudioOutputUnitStart) R(AudioOutputUnitStop)
    R(SndNewChannel) R(SndDisposeChannel) R(SndDoCommand) R(SndDoImmediate) R(SndChannelStatus)
    {NULL, NULL},
};
