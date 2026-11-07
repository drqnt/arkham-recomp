// Sound. FMOD drives a real output unit through a render callback. The Bink library uses the old Sound Manager, which no
// longer exists on the host, so its channels are played here. Both produce samples on a guest thread of their own and hand
// them to the system's audio thread through a ring, because that thread must not wait for the guest gate.
#include "shim.h"
#include "shim_reg.h"

#include <AudioToolbox/AudioToolbox.h>
#if !TARGET_OS_IPHONE
#include <CoreAudio/CoreAudio.h>
#endif
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
    _Atomic u32 silent;  // frames the system asked for that weren't ready: what the player hears as a gap
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
    if (!atomic_load(&r->paused)) atomic_fetch_add(&r->silent, frames - n);
    atomic_store(&r->rd, rd + n);
    dispatch_semaphore_signal(r->space);
    return noErr;
}

#if TARGET_OS_IPHONE
// ---------- the phone's one output ----------
// On a Mac FMOD and each of the Bink library's channels have an output unit of their own, and the system mixes them. On the phone they share one
// RemoteIO and are mixed here: with a RemoteIO for each, a cutscene's sound coming and going left the game silent afterwards. Sources at another rate
// than the output's are resampled, linearly.
#define MIX_RATE 48000.0
#define MIX_SOURCES 16
typedef struct {
    Ring *_Atomic ring;
    double step, pos;  // source frames per output frame, and where between two of them the output is
    float a[2], b[2];  // the two source frames around that place
    int primed;
} MixSource;
static MixSource mix_src[MIX_SOURCES];
static _Atomic u32 mix_passes;  // render calls so far: a source is safe to change once this has moved on twice
static AudioComponentInstance mix_unit;
static pthread_mutex_t mix_mu = PTHREAD_MUTEX_INITIALIZER;
// One frame of a source, as stereo, with its gains; zero when there is none, which counts as a gap.
static int mix_take(Ring *r, float out[2]) {
    if (atomic_load(&r->paused) || !ring_fill(r)) {
        out[0] = out[1] = 0;
        if (!atomic_load(&r->paused)) atomic_fetch_add(&r->silent, 1);
        return 0;
    }
    u32 rd = atomic_load(&r->rd);
    const float *s = r->buf + (size_t)(rd & (r->cap - 1)) * r->channels;
    float l = s[0], rt = r->channels > 1 ? s[1] : s[0];
    out[0] = l * atomic_load(&r->gain_l);
    out[1] = rt * atomic_load(&r->gain_r);
    atomic_store(&r->rd, rd + 1);
    return 1;
}
static OSStatus mix_render(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)ctx, (void)flags, (void)ts, (void)bus;
    float *out = io->mBuffers[0].mData;
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    for (int k = 0; k < MIX_SOURCES; k++) {
        MixSource *m = &mix_src[k];
        Ring *r = atomic_load(&m->ring);
        if (!r || !r->buf) continue;
        if (m->step == 1.0) {
            for (UInt32 i = 0; i < frames; i++) {
                float f[2];
                mix_take(r, f);
                out[i * 2] += f[0];
                out[i * 2 + 1] += f[1];
            }
        } else {
            if (!m->primed) {
                mix_take(r, m->a);
                mix_take(r, m->b);
                m->pos = 0;
                m->primed = 1;
            }
            for (UInt32 i = 0; i < frames; i++) {
                float w = (float)m->pos;
                out[i * 2] += m->a[0] + (m->b[0] - m->a[0]) * w;
                out[i * 2 + 1] += m->a[1] + (m->b[1] - m->a[1]) * w;
                for (m->pos += m->step; m->pos >= 1.0; m->pos -= 1.0) {
                    m->a[0] = m->b[0], m->a[1] = m->b[1];
                    mix_take(r, m->b);
                }
            }
        }
        dispatch_semaphore_signal(r->space);
    }
    atomic_fetch_add(&mix_passes, 1);
    return noErr;
}
static void mix_wait_pass(void) {
    u32 p = atomic_load(&mix_passes);
    for (int i = 0; i < 200 && atomic_load(&mix_passes) - p < 2; i++) usleep(1000);  // at most 0.2 s, if the output isn't running
}
static void mix_start(void) {
    if (mix_unit) return;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &mix_unit)) {
        fprintf(stderr, "[audio] no RemoteIO for the phone's output\n");
        mix_unit = NULL;
        return;
    }
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = MIX_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = 8;
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    AudioUnitSetProperty(mix_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = {mix_render, NULL};
    AudioUnitSetProperty(mix_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    AudioUnitInitialize(mix_unit);
    OSStatus r = AudioOutputUnitStart(mix_unit);
    fprintf(stderr, "[audio] the phone's output: %.0f Hz stereo, one RemoteIO for everything: %d\n", MIX_RATE, (int)r);
}
// The phone's output again after iOS stopped it: the app sent away and back, or an interruption that ended (ios/app/main.m). Starting a unit that
// runs already does nothing.
void rt_audio_resume(const char *why) {
    pthread_mutex_lock(&mix_mu);
    if (mix_unit) {
        OSStatus r = AudioOutputUnitStart(mix_unit);
        fprintf(stderr, "[audio] the phone's output started again (%s): %d\n", why, (int)r);
    }
    pthread_mutex_unlock(&mix_mu);
}
static void mix_add(Ring *r, double rate) {
    pthread_mutex_lock(&mix_mu);
    mix_start();
    for (int k = 0; k < MIX_SOURCES; k++) {
        if (atomic_load(&mix_src[k].ring)) continue;
        double step = rate > 0 ? rate / MIX_RATE : 1.0;
        mix_src[k].step = step > 0.999 && step < 1.001 ? 1.0 : step;  // within a tenth of a percent nobody hears the difference
        mix_src[k].primed = 0;
        atomic_store(&mix_src[k].ring, r);
        break;
    }
    pthread_mutex_unlock(&mix_mu);
}
// Off the output; the ring may be freed when this returns.
static void mix_remove(Ring *r) {
    pthread_mutex_lock(&mix_mu);
    int found = 0;
    for (int k = 0; k < MIX_SOURCES; k++)
        if (atomic_load(&mix_src[k].ring) == r) {
            atomic_store(&mix_src[k].ring, NULL);
            found = 1;
        }
    pthread_mutex_unlock(&mix_mu);
    if (found) mix_wait_pass();
}
#endif

#define FOURCC(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))
#define PROP_UID FOURCC('u', 'i', 'd', ' ')
#define PROP_NAME FOURCC('n', 'a', 'm', 'e')
#define PROP_STREAM_CONFIG FOURCC('s', 'l', 'a', 'y')
#if TARGET_OS_IPHONE
// ---------- the Mac's audio hardware calls, on the phone ----------
// iOS has no audio hardware objects. FMOD only asks which devices there are, which one is the default, and what each can play: one stereo output device
// at 48 kHz answers that, and its sound goes out through RemoteIO below.
static int audio_trace = -1;
#define FAKE_DEVICE 1u
#define PROP_DEVICES FOURCC('d', 'e', 'v', '#')
#define PROP_DEFAULT_OUT FOURCC('d', 'O', 'u', 't')
#define PROP_DEFAULT_IN FOURCC('d', 'I', 'n', ' ')
#define PROP_LAYOUT FOURCC('s', 'r', 'n', 'd')
#define PROP_RATE FOURCC('n', 's', 'r', 't')
#define UNKNOWN_PROPERTY FOURCC('w', 'h', 'o', '?')
static int fake_device_size(u32 id, u32 input, u32 *size) {
    switch (id) {
        case PROP_STREAM_CONFIG: *size = input ? 4 : 16; return 1;  // no buffers for input; one with two channels for output
        case PROP_LAYOUT: *size = 12 + 2 * 20; return 1;  // AudioChannelLayout with two channel descriptions
        case PROP_RATE: *size = 8; return 1;
        case PROP_UID: *size = 4; return 1;
        case PROP_NAME: *size = 256; return 1;
    }
    return 0;
}
static void note_unknown(const char *what, u32 id) {
    static int told;
    if (told++ < 20) fprintf(stderr, "[audio] the phone's stand-in device doesn't answer %s '%c%c%c%c'\n", what, (char)(id >> 24), (char)(id >> 16), (char)(id >> 8), (char)id);
}
SHIM(AudioHardwareGetProperty) {  // id, size, out
    u32 id = ARG(0);
    if (id != PROP_DEVICES && id != PROP_DEFAULT_OUT && id != PROP_DEFAULT_IN) {
        note_unknown("hardware property", id);
        RET(UNKNOWN_PROPERTY);
        return;
    }
    G32(ARG(2)) = id == PROP_DEFAULT_IN ? 0 : FAKE_DEVICE;
    G32(ARG(1)) = 4;
    RET(0);
}
SHIM(AudioHardwareGetPropertyInfo) {  // id, size, writable
    u32 id = ARG(0);
    int known = id == PROP_DEVICES || id == PROP_DEFAULT_OUT || id == PROP_DEFAULT_IN;
    if (ARG(1)) G32(ARG(1)) = known ? 4 : 0;
    if (ARG(2)) G8(ARG(2)) = 0;
    if (!known) note_unknown("hardware property", id);
    RET(known ? 0 : UNKNOWN_PROPERTY);
}
SHIM(AudioDeviceGetProperty) {  // device, channel, is input, id, size, out
    u32 id = ARG(3), input = ARG(2) & 0xFF, gsize = ARG(4), gout = ARG(5);
    switch (id) {
        case PROP_UID:
            G32(gout) = cf_wrap(CFStringCreateWithCString(NULL, "iPhone", kCFStringEncodingUTF8));
            G32(gsize) = 4;
            break;
        case PROP_NAME:
            if (G32(gsize)) {
                snprintf((char *)GP(gout), G32(gsize), "iPhone");
                G32(gsize) = 7;
            }
            break;
        case PROP_STREAM_CONFIG:
            if (input) {
                G32(gout) = 0;
                G32(gsize) = 4;
            } else {
                G32(gout) = 1;
                G32(gout + 4) = 2;
                G32(gout + 8) = 512 * 2 * 4;
                G32(gout + 12) = 0;
                G32(gsize) = 16;
            }
            break;
        case PROP_LAYOUT:
            memset(GP(gout), 0, 52);
            G32(gout + 8) = 2;    // mNumberChannelDescriptions, after a tag of 0 (use the descriptions) and an empty bitmap
            G32(gout + 12) = 1;   // left
            G32(gout + 32) = 2;   // right
            G32(gsize) = 52;
            break;
        case PROP_RATE:
            *(f64a *)GP(gout) = 48000.0;
            G32(gsize) = 8;
            break;
        default:
            note_unknown("device property", id);
            RET(UNKNOWN_PROPERTY);
            return;
    }
    RET(0);
}
SHIM(AudioDeviceGetPropertyInfo) {  // device, channel, is input, id, size, writable
    u32 sz = 0;
    int known = fake_device_size(ARG(3), ARG(2) & 0xFF, &sz);
    if (ARG(4)) G32(ARG(4)) = sz;
    if (ARG(5)) G8(ARG(5)) = 0;
    if (!known) note_unknown("device property", ARG(3));
    RET(known ? 0 : UNKNOWN_PROPERTY);
}
SHIM(AudioDeviceSetProperty) { RET(0); }
#else
// ---------- CoreAudio properties: the old calls the game makes, answered through the current object API ----------
static AudioObjectPropertyAddress dev_addr(u32 chan, u32 is_input, u32 id) {
    AudioObjectPropertyAddress a = {id, is_input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput, chan};
    return a;
}

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

#endif

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
    double rate;  // the phone: what FMOD said it plays at
    int mixed;    // the phone: its ring is a source of the one output
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

// The threads that run the game's sound code ahead of the output: at the highest priority short of real time, so that busy moments elsewhere (on
// the phone, the GPU's compiler building a new place's pipelines on every core) don't leave the output with nothing to play, which is a gap you hear.
static pthread_attr_t *sound_thread_attr(void) {
    static pthread_attr_t a;
    static int made;
    if (!made) {
        pthread_attr_init(&a);
        pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
        made = 1;
    }
    return &a;
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
    u64 last_report = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    while (atomic_load(&o->running)) {
        u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (now - last_report >= 1000000000ull) {
            last_report = now;
            u32 gap = atomic_exchange(&o->ring.silent, 0);
            if (gap) fprintf(stderr, "[audio] gap of %u frames\n", gap);
        }
        if (ring_fill(&o->ring) >= OUT_TARGET) {
            dispatch_semaphore_wait(o->ring.space, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC));
            continue;
        }
        // Nothing here touches the runtime's shared state, and waiting for its lock is what a player hears as a gap in the sound when
        // some other call takes long. Only one-at-a-time mode needs it, to run guest code at all.
        rt_gate_acquire_serial();
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
        rt_gate_release_serial();
        sample_time += OUT_CHUNK;
    }
    return NULL;
}

SHIM(AudioComponentFindNext) {
    AudioComponent prev = ARG(0) ? rt_handle_get(ARG(0)) : NULL;
    AudioComponentDescription d = *(const AudioComponentDescription *)GP(ARG(1));
#if TARGET_OS_IPHONE
    // The Mac's output units, the hardware one and the default one, are RemoteIO on the phone.
    if (d.componentType == kAudioUnitType_Output && (d.componentSubType == FOURCC('a', 'h', 'a', 'l') || d.componentSubType == FOURCC('d', 'e', 'f', ' ') ||
                                                     d.componentSubType == FOURCC('s', 'y', 's', ' ')))
        d.componentSubType = kAudioUnitSubType_RemoteIO;
#endif
    AudioComponent found = AudioComponentFindNext(prev, &d);
    if (audio_trace > 0) fprintf(stderr, "[audio] component %p\n", (void *)found);
    RET(found ? rt_handle_new((void *)found) : 0);
}
#if TARGET_OS_IPHONE
// FMOD's output unit on the phone has no RemoteIO of its own: it keeps what FMOD sets, and its sound goes to the phone's one output above.
SHIM(AudioComponentInstanceNew) {
    GOut *o = calloc(1, sizeof *o);
    G32(ARG(1)) = rt_handle_new(o);
    RET(0);
}
SHIM(AudioComponentInstanceDispose) {
    GOut *o = rt_handle_get(ARG(0));
    if (!o) {
        RET((u32)kAudio_ParamError);
        return;
    }
    rt_handle_free(ARG(0));
    RET(0);
}
#else
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
#endif
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
#if TARGET_OS_IPHONE
        RET(0);
#else
        AURenderCallbackStruct cb = {out_render, o};
        RET((i32)AudioUnitSetProperty(o->unit, id, scope, elem, &cb, sizeof cb));
#endif
        return;
    }
    if (id == PROP_STREAM_FORMAT && scope == SCOPE_INPUT) {
        const AudioStreamBasicDescription *f = (const AudioStreamBasicDescription *)GP(data);
        o->channels = f->mChannelsPerFrame;
#if TARGET_OS_IPHONE
        o->rate = f->mSampleRate > 0 ? f->mSampleRate : MIX_RATE;
#endif
        fprintf(stderr, "[audio] output %.0f Hz, %u channels, flags %#x, %u bits\n", f->mSampleRate, f->mChannelsPerFrame, f->mFormatFlags, f->mBitsPerChannel);
    }
#if TARGET_OS_IPHONE
    if (audio_trace > 0) fprintf(stderr, "[audio] unit set prop %u scope %u elem %u size %u, kept\n", id, scope, elem, size);
    RET(0);
    return;
#endif
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
#if TARGET_OS_IPHONE
    // What the phone's output plays, for a stream format; anything else is told what it was set to, or zeros.
    void *out = GP(ARG(4));
    memset(out, 0, sz);
    if (ARG(1) == PROP_STREAM_FORMAT && sz >= sizeof(AudioStreamBasicDescription)) {
        AudioStreamBasicDescription f = {0};
        f.mSampleRate = MIX_RATE;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
        f.mBytesPerPacket = f.mBytesPerFrame = 8;
        f.mFramesPerPacket = 1;
        f.mChannelsPerFrame = 2;
        f.mBitsPerChannel = 32;
        memcpy(out, &f, sizeof f);
        sz = sizeof f;
    } else if (ARG(1) == 14 && sz >= 4) {  // kAudioUnitProperty_MaximumFramesPerSlice
        G32(ARG(4)) = 4096;
    } else {
        static u32 told[32];
        int seen = 0;
        for (int i = 0; i < 32 && !seen; i++) {
            if (told[i] == ARG(1)) seen = 1;
            else if (!told[i]) {
                told[i] = ARG(1);
                fprintf(stderr, "[audio] FMOD asks property %u of its output, answered with zeros\n", ARG(1));
                break;
            }
        }
    }
    G32(ARG(5)) = sz;
    RET(0);
#else
    OSStatus r = AudioUnitGetProperty(o->unit, ARG(1), ARG(2), ARG(3), GP(ARG(4)), &sz);
    G32(ARG(5)) = sz;
    RET((i32)r);
#endif
}
SHIM(AudioUnitInitialize) {
    GOut *o = rt_handle_get(ARG(0));
    if (!o) {
        RET((u32)kAudio_ParamError);
        return;
    }
#if TARGET_OS_IPHONE
    RET(0);
#else
    RET((i32)AudioUnitInitialize(o->unit));
#endif
}
SHIM(AudioUnitUninitialize) {
    GOut *o = rt_handle_get(ARG(0));
    if (!o) {
        RET((u32)kAudio_ParamError);
        return;
    }
#if TARGET_OS_IPHONE
    RET(0);
#else
    RET((i32)AudioUnitUninitialize(o->unit));
#endif
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
        pthread_create(&o->pump, sound_thread_attr(), out_pump, o);
    }
#if TARGET_OS_IPHONE
    if (o->pump_started && !o->mixed) {
        mix_add(&o->ring, o->rate);
        o->mixed = 1;
    }
    OSStatus r = 0;
#else
    OSStatus r = AudioOutputUnitStart(o->unit);
#endif
    fprintf(stderr, "[audio] output unit started: %d\n", (int)r);
    RET((i32)r);
}
SHIM(AudioOutputUnitStop) {
    GOut *o = rt_handle_get(ARG(0));
    if (!o) {
        RET((u32)kAudio_ParamError);
        return;
    }
#if TARGET_OS_IPHONE
    OSStatus r = 0;
    if (o->mixed) {
        mix_remove(&o->ring);
        o->mixed = 0;
    }
#else
    OSStatus r = AudioOutputUnitStop(o->unit);
#endif
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
    int mixed;  // the phone: its ring is a source of the one output
} SndChan;

static OSStatus snd_render(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)flags;
    (void)ts;
    (void)bus;
    return ring_render(&((SndChan *)ctx)->ring, frames, io);
}
static void snd_open_output(SndChan *s, u32 rate, u32 channels) {
#if TARGET_OS_IPHONE
    if (s->mixed) {
        if (s->rate == rate && s->channels == channels) return;
        mix_remove(&s->ring);
        s->mixed = 0;
    }
#else
    if (s->unit) {
        if (s->rate == rate && s->channels == channels) return;
        AudioOutputUnitStop(s->unit);
        AudioComponentInstanceDispose(s->unit);
        s->unit = NULL;
    }
#endif
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
#if TARGET_OS_IPHONE
    mix_add(&s->ring, rate);
    s->mixed = 1;
    fprintf(stderr, "[audio] video sound %u Hz, %u channels, into the phone's output\n", rate, channels);
#else
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
#endif
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
            rt_gate_acquire_serial();
            G16(g_cmd) = cmd.cmd;
            G16(g_cmd + 2) = (u16)cmd.p1;
            G32(g_cmd + 4) = cmd.p2;
            u32 a[2] = {s->handle, g_cmd};
            rt_call_guest(c, s->callback, 2, a);
            rt_gate_release_serial();
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
    pthread_create(&s->thread, sound_thread_attr(), snd_thread, s);
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
#if TARGET_OS_IPHONE
    if (s->mixed) mix_remove(&s->ring);
    s->mixed = 0;
#endif
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
