// Objective-C bridge. Guest messages to system classes and objects run on the real host runtime: the method's
// type encoding says how to convert each argument between the 32-bit guest stack and arm64 registers.
// Guest-defined classes (the legacy __OBJC segment) are handled by guest_class.c-style lookup further down.
#include "shim.h"
void rt_gl_context_changed(void);
#include <dispatch/dispatch.h>
#include <ImageIO/ImageIO.h>
#include <dlfcn.h>
#include "shim_reg.h"

#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#if TARGET_OS_IPHONE
// AppKit's classes are the stand-ins in ios_appkit.m there, under names of their own.
Class rt_ios_class(const char *name);
#define objc_getClass rt_ios_class
#endif

extern u32 cf_wrap(CFTypeRef o);
extern CFTypeRef cf_unwrap(u32 g);

static int trace;
#define IMAGE_END 0x03900000u

// A class reference in guest code is a pointer to the class name string, unless a real object was wrapped.
static int looks_like_name(u32 r) {
    if (r < 0x1000 || r >= IMAGE_END) return 0;
    const char *s = GS(r);
    for (int i = 0; i < 64; i++) {
        if (!s[i]) return i > 0;
        unsigned char ch = (unsigned char)s[i];
        if (!(ch == '_' || (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))) return 0;
    }
    return 0;
}

static id proxy_host(u32 p);
extern const unsigned rt_objc_class_addr, rt_objc_class_size, rt_objc_cat_addr, rt_objc_cat_size;

static id host_receiver(u32 r) {
    if (!r) return nil;
    if (rt_is_handle(r)) return (id)rt_handle_get(r);
    if (r >= 0x04000000u && r < 0xE0000000u) {
        id h = proxy_host(r);
        if (h) return h;
    }
    if (looks_like_name(r)) return (id)objc_getClass(GS(r));
    if (r >= rt_objc_class_addr && r < rt_objc_class_addr + rt_objc_class_size) return (id)objc_getClass(GS(G32(r + 8)));
    if (r >= 0x1000 && r < IMAGE_END) return (id)cf_unwrap(r);
    return nil;
}

static const char *skip_quals(const char *t) {
    while (*t && strchr("rnNoORV", *t)) t++;
    return t;
}
// Advance past one type in an encoding, including trailing stack-offset digits.
static const char *next_type(const char *t) {
    t = skip_quals(t);
    while (*t == '^') t = skip_quals(t + 1);  // a pointer's target is part of the same type
    int depth = 0;
    do {
        char ch = *t;
        if (!ch) return t;
        if (ch == '{' || ch == '(' || ch == '[') depth++;
        else if (ch == '}' || ch == ')' || ch == ']') depth--;
        else if (ch == '@' && t[1] == '"') {
            t += 2;
            while (*t && *t != '"') t++;
        } else if (ch == '"') {
            t++;
            while (*t && *t != '"') t++;
        }
        t++;
    } while (depth > 0);
    while (*t >= '0' && *t <= '9') t++;
    return t;
}

// Selector-specific knowledge: CGFloat is float on the guest but NSTimeInterval is double on both, and the
// host's encoding says 'd' for each. These selectors take real doubles from the guest.
static int guest_double_selector(const char *sel) {
    return strstr(sel, "TimeInterval") || strstr(sel, "dateWith") || strstr(sel, "sleepUntil") || strstr(sel, "afterDelay") ||
           strstr(sel, "NSTimer") || strstr(sel, "scheduledTimer") || strstr(sel, "numberWithDouble") ||
           strstr(sel, "initWithDouble") || strstr(sel, "setDouble");
}

typedef struct { u64 x[6]; double d[8]; int nx, nd; } Regs;
static void put_x(Regs *r, u64 v) { if (r->nx < 6) r->x[r->nx++] = v; }
static void put_d(Regs *r, double v) { if (r->nd < 8) r->d[r->nd++] = v; }
static void put_s(Regs *r, float v) {
    if (r->nd < 8) {
        double d = 0;
        memcpy(&d, &v, 4);
        r->d[r->nd++] = d;
    }
}

// Host-side out slots for pointer-to-object arguments such as NSError**.
typedef struct { u32 guest; id val; } OutSlot;

static u32 marshal_args(Method m, const char *sel, u32 ap, Regs *r, OutSlot *outs, int *nout) {
    unsigned n = method_getNumberOfArguments(m);
    const char *t = method_getTypeEncoding(m);
    t = next_type(t);  // return type
    t = next_type(t);  // self
    t = next_type(t);  // _cmd
    for (unsigned i = 2; i < n; i++) {
        const char *q = skip_quals(t);
        char k = *q;
        u32 v = G32(ap);
        switch (k) {
            case '@': case '#':
                put_x(r, (u64)host_receiver(v));
                ap += 4;
                break;
            case ':':
                put_x(r, v ? (u64)sel_registerName(GS(v)) : 0);
                ap += 4;
                break;
            case '*':
                put_x(r, v ? (u64)GP(v) : 0);
                ap += 4;
                break;
            case '^':
                if (q[1] == '@' && v && *nout < 4) {
                    outs[*nout].guest = v;
                    outs[*nout].val = nil;
                    put_x(r, (u64)&outs[*nout].val);
                    (*nout)++;
                } else if (rt_is_handle(v)) {
                    // An opaque system object such as a CGImageRef, known to the guest only by its handle.
                    put_x(r, (u64)rt_handle_get(v));
                } else {
                    put_x(r, v ? (u64)GP(v) : 0);
                }
                ap += 4;
                break;
            case 'c': case 'C': case 'B': case 's': case 'S': case 'i': case 'I': case 'l': case 'L':
                put_x(r, (k == 'i' || k == 'l') ? (u64)(i64)(i32)v : (u64)v);
                ap += 4;
                break;
            case 'q':
                put_x(r, (u64)(i64)(i32)v);  // NSInteger: 32-bit in the guest
                ap += 4;
                break;
            case 'Q':
                put_x(r, (u64)v);  // NSUInteger: 32-bit in the guest
                ap += 4;
                break;
            case 'f':
                put_s(r, *(f32a *)(GMEM + ap));
                ap += 4;
                break;
            case 'd':
                if (guest_double_selector(sel)) {
                    put_d(r, *(f64a *)(GMEM + ap));
                    ap += 8;
                } else {  // CGFloat
                    put_d(r, *(f32a *)(GMEM + ap));
                    ap += 4;
                }
                break;
            case '{':
                if (!strncmp(q, "{_NSRange", 9) || !strncmp(q, "{?=QQ", 5)) {
                    put_x(r, v);
                    put_x(r, G32(ap + 4));
                    ap += 8;
                } else if (!strncmp(q, "{CGRect", 7) || !strncmp(q, "{_NSRect", 8)) {
                    for (int j = 0; j < 4; j++) put_d(r, *(f32a *)(GMEM + ap + (u32)j * 4));
                    ap += 16;
                } else if (!strncmp(q, "{CGPoint", 8) || !strncmp(q, "{CGSize", 7) || !strncmp(q, "{_NSPoint", 9) || !strncmp(q, "{_NSSize", 8)) {
                    for (int j = 0; j < 2; j++) put_d(r, *(f32a *)(GMEM + ap + (u32)j * 4));
                    ap += 8;
                } else {
                    fprintf(stderr, "[objc] unsupported struct argument %s in %s\n", q, sel);
                    put_x(r, 0);
                    ap += 4;
                }
                break;
            default:
                fprintf(stderr, "[objc] unsupported argument type %c in %s\n", k, sel);
                put_x(r, 0);
                ap += 4;
                break;
        }
        t = next_type(t);
    }
    return ap;
}

typedef u64 (*msg_x)(id, SEL, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double);
typedef double (*msg_d)(id, SEL, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double);
typedef float (*msg_f)(id, SEL, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double);
typedef CGRect (*msg_r)(id, SEL, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double);
typedef CGPoint (*msg_p)(id, SEL, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double);
typedef struct { u64 a, b; } U128;
typedef U128 (*msg_u)(id, SEL, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double);

#define CALLARGS(R) (R).x[0], (R).x[1], (R).x[2], (R).x[3], (R).x[4], (R).x[5], (R).d[0], (R).d[1], (R).d[2], (R).d[3], (R).d[4], (R).d[5], (R).d[6], (R).d[7]

void rt_note_webview(id v);
static int fast_enumerate(CPU *c, id recv, u32 ap);
extern char *rt_format_guest(const char *fmt, u32 va);

// Variadic methods take their arguments on the stack on arm64, so format them here and call the plain equivalent.
static const char *ns_utf8(id s) { return ((const char *(*)(id, SEL))objc_msgSend)(s, sel_registerName("UTF8String")); }
static id ns_from_c(const char *z) {
    return ((id(*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), z);
}
char *rt_objc_describe(u32 g) {
    id o = host_receiver(g);
    if (!o) return strdup("(null)");
    id d = ((id(*)(id, SEL))objc_msgSend)(o, sel_registerName("description"));
    const char *u = d ? ns_utf8(d) : "";
    return strdup(u ? u : "");
}
// The nil-terminated variadic collection constructors, built through their count-based equivalents.
static int collection_send(CPU *c, id recv, const char *sn, u32 ap) {
    int pairs = !strcmp(sn, "dictionaryWithObjectsAndKeys:") || !strcmp(sn, "initWithObjectsAndKeys:");
    int plain = !strcmp(sn, "arrayWithObjects:") || !strcmp(sn, "setWithObjects:") || !strcmp(sn, "initWithObjects:");
    if (!pairs && !plain) return 0;
    id items[256];
    int n = 0;
    while (n < 256) {
        u32 g = G32(ap + (u32)n * 4);
        if (!g) break;
        items[n++] = host_receiver(g);
    }
    id r;
    int is_init = !strncmp(sn, "init", 4);
    if (pairs) {
        id vals[128], keys[128];
        int m = n / 2;
        for (int i = 0; i < m; i++) {
            vals[i] = items[2 * i];
            keys[i] = items[2 * i + 1];
        }
        SEL s = sel_registerName(is_init ? "initWithObjects:forKeys:count:" : "dictionaryWithObjects:forKeys:count:");
        r = ((id(*)(id, SEL, id *, id *, unsigned long))objc_msgSend)(recv, s, vals, keys, (unsigned long)m);
    } else {
        const char *nm = is_init ? "initWithObjects:count:" : (sn[0] == 'a' ? "arrayWithObjects:count:" : "setWithObjects:count:");
        r = ((id(*)(id, SEL, id *, unsigned long))objc_msgSend)(recv, sel_registerName(nm), items, (unsigned long)n);
    }
    RET(cf_wrap(r));
    return 1;
}

static int format_send(CPU *c, id recv, const char *sn, u32 ap) {
    int cls = 0;
    SEL plain;
    if (!strcmp(sn, "stringWithFormat:") || !strcmp(sn, "localizedStringWithFormat:")) cls = 1, plain = NULL;
    else if (!strcmp(sn, "initWithFormat:")) plain = sel_registerName("initWithString:");
    else if (!strcmp(sn, "appendFormat:")) plain = sel_registerName("appendString:");
    else if (!strcmp(sn, "stringByAppendingFormat:")) plain = sel_registerName("stringByAppendingString:");
    else return 0;
    id fmt = host_receiver(G32(ap));
    char *z = rt_format_guest(fmt ? ns_utf8(fmt) : "", ap + 4);
    id str = ns_from_c(z);
    free(z);
    if (cls) RET(cf_wrap(str));
    else RET(cf_wrap(((id(*)(id, SEL, id))objc_msgSend)(recv, plain, str)));
    return 1;
}

static u32 obj_wrap(id o);
static void proxy_forget(id h);

// stret points at the guest's hidden return buffer for struct-returning messages. start_class, when set,
// begins method lookup there (super calls into a system class) and dispatches straight to that implementation.
// The game's dialogs watch events with a block. A guest block means nothing to the system, and going through unconverted it became a monitor with
// no handler, which crashes AppKit at the next matching event: the key coming up after Cmd+Q, say.
u32 rt_block_copy(CPU *c, u32 b);
static void event_monitor_send(CPU *c, id recv, u32 ap) {
    // The mask is one word in this build's SDK and two in later ones, where the second is zero for any mask but "everything".
    u32 w0 = G32(ap), w1 = G32(ap + 4), w2 = G32(ap + 8);
    int wide = !w1 || w1 == 0xFFFFFFFFu;
    u32 block = rt_block_copy(c, wide ? w2 : w1);
    unsigned long long mask = wide ? ((unsigned long long)w1 << 32 | w0) : w0;
    id handler = (id) ^ id(id ev) {
        int held = rt_gate_held();
        if (!held) rt_gate_acquire();
        CPU *cpu = rt_current_cpu();
        if (!cpu) cpu = rt_new_cpu(2u << 20);
        u32 a[2] = {block, obj_wrap(ev)};
        u32 ret = rt_call_guest(cpu, G32(block + 12), 2, a);
        id out = host_receiver(ret);
        if (!held) rt_gate_release();
        return out;
    };
    id monitor = ((id(*)(id, SEL, unsigned long long, id))objc_msgSend)(recv, sel_registerName("addLocalMonitorForEventsMatchingMask:handler:"), mask, handler);
    fprintf(stderr, "[objc] the game watches events with mask %llx\n", mask);
    RET(obj_wrap(monitor));
}
static int host_send_from(CPU *c, u32 self, u32 selg, u32 ap, u32 stret, Class start_class) {
    id recv = host_receiver(self);
    const char *sn = GS(selg);
    if (!recv) return 0;
    if (!strcmp(sn, "countByEnumeratingWithState:objects:count:")) return fast_enumerate(c, recv, ap);
    if (strstr(sn, "Format:") && format_send(c, recv, sn, ap)) return 1;
    if (strstr(sn, "Objects") && collection_send(c, recv, sn, ap)) return 1;
    // Test runs answer the welcome dialog with its default button instead of waiting for a person.
    if (!strcmp(sn, "runModal") && getenv("ARKHAM_AUTO_ALERT")) {
        id msg = ((id(*)(id, SEL))objc_msgSend)(recv, sel_registerName("messageText"));
        id info = ((id(*)(id, SEL))objc_msgSend)(recv, sel_registerName("informativeText"));
        fprintf(stderr, "[objc] auto-answering runModal with the first button: %s / %s\n", msg ? ns_utf8(msg) : "", info ? ns_utf8(info) : "");
        RET(1000);
        return 1;
    }
    if (!strcmp(sn, "addLocalMonitorForEventsMatchingMask:handler:")) {
        event_monitor_send(c, recv, ap);
        return 1;
    }
    if (!strcmp(sn, "setMainFrameURL:")) rt_note_webview(recv);
    if (!strcmp(sn, "setMainFrameURL:") && getenv("ARKHAM_JS_DEBUG")) {
        extern id rt_web_debug_delegate(void);
        ((void (*)(id, SEL, id))objc_msgSend)(recv, sel_registerName("setUIDelegate:"), rt_web_debug_delegate());
    }
    if (!strcmp(sn, "operatingSystemVersion")) {
        // The same answer as Gestalt: the newest system the 32-bit build ever knew.
        if (stret) {
            G32(stret) = 10;
            G32(stret + 4) = 14;
            G32(stret + 8) = 6;
        }
        return 1;
    }
    if (recv == (id)objc_getClass("NSBundle") && !strcmp(sn, "mainBundle")) {
        extern id rt_game_bundle(void);
        RET(cf_wrap(rt_game_bundle()));
        return 1;
    }
    SEL sel = sel_registerName(sn);
    Class cls = start_class ? start_class : object_getClass(recv);
    Method m = class_getInstanceMethod(cls, sel);
    if (!m) return 0;
    void *fn = start_class ? (void *)method_getImplementation(m) : (void *)objc_msgSend;
    if (start_class && !strcmp(sn, "dealloc")) proxy_forget(recv);
    Regs r;
    memset(&r, 0, sizeof r);
    OutSlot outs[4];
    int nout = 0;
    marshal_args(m, sn, ap, &r, outs, &nout);
    if (trace && r.nx > 0 && (intptr_t)r.x[0] > 0x100000000LL) {
        // Show string arguments, which is what makes a message trace readable.
        const char *t2 = next_type(next_type(next_type(method_getTypeEncoding(m))));
        if (*skip_quals(t2) == '@') {
            id a0 = (id)r.x[0];
            if (((signed char (*)(id, SEL, Class))objc_msgSend)(a0, sel_registerName("isKindOfClass:"), objc_getClass("NSString"))) {
                const char *u = ((const char *(*)(id, SEL))objc_msgSend)(a0, sel_registerName("UTF8String"));
                fprintf(stderr, "[objc]   arg0 = \"%.200s\"\n", u ? u : "");
            }
        }
    }
    const char *rt = skip_quals(method_getTypeEncoding(m));
    char k = *rt;
    // (The bindings OpenGL shims remember belong to the current context.)
    // Quick reads of window and screen state run in place. Giving up the gate for them would cost a whole time slice each.
    static const char *const quick[] = {"frame", "isKeyWindow", "contentRectForFrameRect:", "screens", "mainScreen", "objectAtIndex:", "count", "backingScaleFactor", NULL};
    int keep_gate = 0;
    for (const char *const *q = quick; *q; q++) {
        if (!strcmp(sn, *q)) {
            keep_gate = 1;
            break;
        }
    }
    if (!strcmp(sn, "makeCurrentContext") || !strcmp(sn, "clearCurrentContext")) {
        rt_glq_drain();
        rt_gl_context_changed();
    }
    if (!keep_gate) rt_gate_release();
    #define GATE_BACK do { if (!keep_gate) rt_gate_acquire(); } while (0)
    switch (k) {
        case 'v':
            ((msg_x)fn)(recv, sel, CALLARGS(r));
            GATE_BACK;
            c->eax = 0;
            break;
        case '@': case '#': {
            id v = (id)((msg_x)fn)(recv, sel, CALLARGS(r));
            GATE_BACK;
            RET(k == '#' ? cf_wrap(v) : obj_wrap(v));
            break;
        }
        case '^': {
            // Opaque C pointers such as CGLContextObj travel through the handle table like objects do.
            void *v = (void *)((msg_x)fn)(recv, sel, CALLARGS(r));
            GATE_BACK;
            // A pointer into the guest window is a guest address coming back (say, from an NSValue).
            if ((u8 *)v >= rt_mem && (u8 *)v < rt_mem + (1ull << 32)) RET((u32)((u8 *)v - rt_mem));
            else RET(v ? cf_wrap(v) : 0);
            break;
        }
        case ':': {
            SEL v = (SEL)((msg_x)fn)(recv, sel, CALLARGS(r));
            GATE_BACK;
            RET(v ? rt_guest_strdup(sel_getName(v)) : 0);
            break;
        }
        case '*': {
            const char *v = (const char *)((msg_x)fn)(recv, sel, CALLARGS(r));
            GATE_BACK;
            RET(v ? rt_guest_strdup(v) : 0);
            break;
        }
        case 'c': case 'C': case 'B': case 's': case 'S': case 'i': case 'I': case 'l': case 'L': case 'q': case 'Q': {
            u64 v = ((msg_x)fn)(recv, sel, CALLARGS(r));
            GATE_BACK;
            RET((u32)v);
            c->edx = (u32)(v >> 32);
            break;
        }
        case 'f':
            {
                float v = ((msg_f)fn)(recv, sel, CALLARGS(r));
                GATE_BACK;
                RETF(v);
            }
            break;
        case 'd':
            {
                double v = ((msg_d)fn)(recv, sel, CALLARGS(r));
                GATE_BACK;
                RETF(v);
            }
            break;
        case '{':
            if (!strncmp(rt, "{CGRect", 7) || !strncmp(rt, "{_NSRect", 8)) {
                CGRect v = ((msg_r)fn)(recv, sel, CALLARGS(r));
                GATE_BACK;
                if (stret) {
                    *(f32a *)(GMEM + stret) = (float)v.origin.x;
                    *(f32a *)(GMEM + stret + 4) = (float)v.origin.y;
                    *(f32a *)(GMEM + stret + 8) = (float)v.size.width;
                    *(f32a *)(GMEM + stret + 12) = (float)v.size.height;
                }
            } else if (!strncmp(rt, "{CGPoint", 8) || !strncmp(rt, "{CGSize", 7) || !strncmp(rt, "{_NSPoint", 9) || !strncmp(rt, "{_NSSize", 8)) {
                CGPoint v = ((msg_p)fn)(recv, sel, CALLARGS(r));
                GATE_BACK;
                // Eight-byte structs come back in eax:edx on i386.
                float fx = (float)v.x, fy = (float)v.y;
                memcpy(&c->eax, &fx, 4);
                memcpy(&c->edx, &fy, 4);
            } else if (!strncmp(rt, "{_NSRange", 9)) {
                U128 v = ((msg_u)fn)(recv, sel, CALLARGS(r));
                GATE_BACK;
                c->eax = (u32)v.a;
                c->edx = (u32)v.b;
            } else {
                GATE_BACK;
                fprintf(stderr, "[objc] unsupported struct return %s from %s\n", rt, sn);
            }
            break;
        default:
            GATE_BACK;
            fprintf(stderr, "[objc] unsupported return type %c from %s\n", k, sn);
            break;
    }
    for (int i = 0; i < nout; i++) G32(outs[i].guest) = cf_wrap(outs[i].val);
    return 1;
}
static int host_send(CPU *c, u32 self, u32 selg, u32 ap, u32 stret) { return host_send_from(c, self, selg, ap, stret, Nil); }

// Fast enumeration: the guest's 32-bit state struct gets one batch holding a snapshot of every element.
// {state, itemsPtr, mutationsPtr, extra[5]}
typedef struct { unsigned long state; id *itemsPtr; unsigned long *mutationsPtr; unsigned long extra[5]; } HostFE;
typedef unsigned long (*fe_fn)(id, SEL, HostFE *, id *, unsigned long);
static int fast_enumerate(CPU *c, id recv, u32 ap) {
    u32 st = G32(ap);
    if (G32(st)) {
        RET(0);
        return 1;
    }
    SEL fe = sel_registerName("countByEnumeratingWithState:objects:count:");
    HostFE hs;
    memset(&hs, 0, sizeof hs);
    id buf[64];
    u32 cap = 64, n = 0;
    u32 list = rt_guest_malloc(cap * 4);
    unsigned long got;
    while ((got = ((fe_fn)objc_msgSend)(recv, fe, &hs, buf, 64)) > 0) {
        for (unsigned long i = 0; i < got; i++) {
            if (n == cap) {
                cap *= 2;
                list = rt_guest_realloc(list, cap * 4);
            }
            G32(list + n * 4) = obj_wrap(hs.itemsPtr[i]);
            n++;
        }
    }
    static u32 mutations;
    if (!mutations) mutations = rt_guest_calloc(1, 8);
    G32(st) = 1;
    G32(st + 4) = list;
    G32(st + 8) = mutations;
    RET(n);
    return 1;
}

static const char *recv_name(u32 r, char *buf, size_t n) {
    if (!r) return "nil";
    if (looks_like_name(r)) return GS(r);
    if (rt_is_handle(r)) {
        id o = (id)rt_handle_get(r);
        if (o) {
            snprintf(buf, n, "<%s %08x>", object_getClassName(o), r);
            return buf;
        }
    }
    snprintf(buf, n, "%08x", r);
    return buf;
}

// ---------- guest-defined classes, read from the game's legacy __OBJC segment ----------
// objc_class: {isa, super, name, version, info, instance_size, ivars, methodLists, cache, protocols, ...}
typedef struct GClass {
    u32 addr, meta;
    const char *name;
    const char *super_name;
    u32 inst_size;
} GClass;
static GClass gclasses[64];
static int ngclasses;
extern const unsigned rt_objc_class_addr, rt_objc_class_size, rt_objc_cat_addr, rt_objc_cat_size;

static GClass *gclass_by_name(const char *n) {
    if (!n) return NULL;
    for (int i = 0; i < ngclasses; i++)
        if (!strcmp(gclasses[i].name, n)) return &gclasses[i];
    return NULL;
}

static void gclasses_load(void) {
    for (u32 a = rt_objc_class_addr; a < rt_objc_class_addr + rt_objc_class_size && ngclasses < 64; a += 48) {
        u32 nm = G32(a + 8);
        if (!nm) continue;
        GClass *k = &gclasses[ngclasses++];
        k->addr = a;
        k->meta = G32(a);
        k->name = GS(nm);
        k->super_name = G32(a + 4) ? GS(G32(a + 4)) : NULL;
        k->inst_size = G32(a + 20);
    }
}

// Search one method list {obsolete, count, {name, types, imp}...} for a selector by string.
static u32 list_find(u32 ml, const char *sn) {
    if (!ml || ml == 0xFFFFFFFFu) return 0;
    u32 n = G32(ml + 4);
    for (u32 i = 0; i < n; i++) {
        u32 e = ml + 8 + i * 12;
        if (!strcmp(GS(G32(e)), sn)) return G32(e + 8);
    }
    return 0;
}

// Resolve a guest method on a class, walking up the guest part of the hierarchy, categories included.
static u32 gclass_method(GClass *k, const char *sn, int meta) {
    while (k) {
        u32 addr = meta ? k->meta : k->addr;
        u32 imp = list_find(G32(addr + 28), sn);
        if (imp) return imp;
        for (u32 a = rt_objc_cat_addr; a < rt_objc_cat_addr + rt_objc_cat_size; a += 28) {
            if (!strcmp(GS(G32(a + 4)), k->name)) {
                imp = list_find(G32(a + (meta ? 12 : 8)), sn);
                if (imp) return imp;
            }
        }
        k = gclass_by_name(k->super_name);
    }
    return 0;
}

// Decide whether a receiver belongs to a guest class. Sets *meta when the receiver is the class itself.
static GClass *guest_class_of(u32 r, int *meta) {
    if (r < 0x1000 || rt_is_handle(r)) return NULL;
    if (r >= rt_objc_class_addr && r < rt_objc_class_addr + rt_objc_class_size) {
        for (int i = 0; i < ngclasses; i++) {
            if (gclasses[i].addr == r) {
                *meta = 1;
                return &gclasses[i];
            }
        }
    }
    if (looks_like_name(r)) {
        GClass *k = gclass_by_name(GS(r));
        if (k) {
            *meta = 1;
            return k;
        }
        return NULL;
    }
    if (r >= 0x04000000u && r < 0xE0000000u) {
        u32 isa = G32(r);
        for (int i = 0; i < ngclasses; i++) {
            if (gclasses[i].addr == isa) {
                *meta = 0;
                return &gclasses[i];
            }
        }
    }
    return NULL;
}

static u32 entry_point;
int rt_gate_held(void);

// Behaviour every guest class inherits from NSObject, for when no guest method overrides it.
static void guest_root_message(CPU *c, GClass *k, int meta, u32 self, const char *sn, u32 ap) {
    c->edx = 0;
    if (meta && (!strcmp(sn, "alloc") || !strcmp(sn, "allocWithZone:") || !strcmp(sn, "new"))) {
        u32 o = rt_guest_calloc(1, k->inst_size + 64);
        G32(o) = k->addr;
        c->eax = o;
    } else if (!strncmp(sn, "init", 4) || !strcmp(sn, "retain") || !strcmp(sn, "autorelease") || !strcmp(sn, "self") ||
               !strcmp(sn, "class")) {
        c->eax = self;
    } else if (!strcmp(sn, "isKindOfClass:") || !strcmp(sn, "respondsToSelector:")) {
        c->eax = (!strcmp(sn, "respondsToSelector:") && gclass_method(k, GS(G32(ap)), meta)) ? 1 : 0;
    } else {
        if (trace) fprintf(stderr, "[objc] unanswered [%s %s]\n", k->name, sn);
        c->eax = 0;
    }
}

// Guest objects whose class derives from a system class pair a host instance with a guest-memory proxy: guest
// methods read their ivars from the proxy, while AppKit sees the host instance.
static CFMutableDictionaryRef h2p, p2h;

static id proxy_host(u32 p) {
    return (id)CFDictionaryGetValue(p2h, (const void *)(uintptr_t)p);
}
static void proxy_forget(id h) {
    u32 p = (u32)(uintptr_t)CFDictionaryGetValue(h2p, h);
    if (p) {
        CFDictionaryRemoveValue(h2p, h);
        CFDictionaryRemoveValue(p2h, (const void *)(uintptr_t)p);
    }
}
// The guest class for a host class, walking up through any plain subclass the system made (such as KVO's).
static GClass *guest_for_host_class(Class k) {
    while (k) {
        GClass *g = gclass_by_name(class_getName(k));
        if (g) return g;
        k = class_getSuperclass(k);
    }
    return NULL;
}
static u32 obj_wrap(id o) {
    if (!o) return 0;
    if ((intptr_t)o < 0) return cf_wrap(o);
    Class k = object_getClass(o);
    if (class_isMetaClass(k)) {
        GClass *g = gclass_by_name(class_getName((Class)o));
        return g ? g->addr : cf_wrap(o);
    }
    GClass *g = guest_for_host_class(k);
    if (!g) return cf_wrap(o);
    u32 p = (u32)(uintptr_t)CFDictionaryGetValue(h2p, o);
    if (!p) {
        p = rt_guest_calloc(1, g->inst_size + 64);
        G32(p) = g->addr;
        CFDictionarySetValue(h2p, o, (const void *)(uintptr_t)p);
        CFDictionarySetValue(p2h, (const void *)(uintptr_t)p, o);
    }
    return p;
}

// Returns 1 when a guest method ran: it has then already popped any hidden struct-return pointer itself.
static int send(CPU *c, u32 self, u32 sel, u32 ap, u32 stret) {
    char b[80];
    if (!self) {
        c->eax = 0;
        c->edx = 0;
        return 0;
    }
    const char *sn = GS(sel);
    int meta = 0;
    GClass *k = guest_class_of(self, &meta);
    if (k) {
        u32 imp = gclass_method(k, sn, meta);
        if (trace) fprintf(stderr, "[objc] [%s %s] guest %08x from %08x\n", k->name, sn, imp, G32(c->esp));
        if (!strcmp(sn, "setEntryPoint:")) entry_point = G32(ap);
        if (imp) {
            // The guest stack already holds {ret, self, _cmd, args...}: enter the method as if called directly.
            rt_dispatch(c, imp);
            c->esp -= 4;  // the method's ret already popped what rt_imp will pop again
            return 1;
        }
    }
    // Name the receiver first: init methods may free it and return a different object.
    const char *rn = trace ? recv_name(self, b, sizeof b) : "";
    int handled = host_send(c, self, sel, ap, stret);
    if (trace) fprintf(stderr, "[objc] [%s %s] %s from %08x -> %08x\n", rn, sn, handled ? "host" : "UNHANDLED", G32(c->esp), c->eax);
    if (!handled) {
        if (k) guest_root_message(c, k, meta, self, sn, ap);
        else {
            c->eax = 0;
            c->edx = 0;
        }
    }
    return 0;
}

SHIM(objc_msgSend) { send(c, ARG(0), ARG(1), c->esp + 12, 0); }
SHIM(objc_msgSendSuper) {
    // struct objc_super {receiver, class}: in this legacy ABI, the class is where the search STARTS, normally
    // given as the superclass's name or class pointer.
    u32 sup = ARG(0);
    u32 recv = G32(sup);
    u32 sel = ARG(1);
    u32 kl = G32(sup + 4);
    const char *sn = GS(sel);
    int meta = 0;
    guest_class_of(recv, &meta);
    GClass *s = NULL;
    for (int i = 0; i < ngclasses; i++)
        if (gclasses[i].addr == kl || gclasses[i].meta == kl) s = &gclasses[i];
    if (!s && looks_like_name(kl)) s = gclass_by_name(GS(kl));
    u32 imp = s ? gclass_method(s, sn, meta) : 0;
    if (trace) fprintf(stderr, "[objc] [super %s] from %s -> guest %08x\n", sn, s ? s->name : (looks_like_name(kl) ? GS(kl) : "?"), imp);
    if (imp) {
        // The method expects the receiver where objc_super was: same frame, first argument rewritten.
        ARG(0) = recv;
        rt_dispatch(c, imp);
        c->esp -= 4;
        return;
    }
    // Into a system class: dispatch directly to its implementation so overriding guest methods don't recurse.
    Class base = Nil;
    if (s) base = objc_getClass(s->name);
    else if (looks_like_name(kl)) base = objc_getClass(GS(kl));
    else if (rt_is_handle(kl)) base = (Class)rt_handle_get(kl);
    while (base && gclass_by_name(class_getName(base))) base = class_getSuperclass(base);
    Class lookup = (meta && base) ? object_getClass((id)base) : base;
    if (lookup) {
        // Marshal as an ordinary send, with the receiver standing where the objc_super pointer was.
        ARG(0) = recv;
        if (host_send_from(c, recv, sel, c->esp + 12, 0, lookup)) return;
    }
    if (!strcmp(sn, "init") || !strcmp(sn, "dealloc")) c->eax = recv;
    else c->eax = 0;
}
SHIM(objc_msgSend_fpret) { send(c, ARG(0), ARG(1), c->esp + 12, 0); }
// Struct returns: the hidden pointer comes first and the callee pops it.
SHIM(objc_msgSend_stret) {
    u32 buf = ARG(0);
    if (!send(c, ARG(1), ARG(2), c->esp + 16, buf)) c->esp += 4;
    RET(buf);
}

#define CLS(n) ((id)objc_getClass(n))
#define SELR(n) sel_registerName(n)
static id ns_str(const char *z) {
    return ((id(*)(id, SEL, const char *))objc_msgSend)(CLS("NSString"), SELR("stringWithUTF8String:"), z);
}
static const char *game_bundle_path(void) {
    static char path[1200];
    if (!path[0])
        snprintf(path, sizeof path, "%s/Batman Arkham Asylum GOTY.app", rt_game_root());
    return path;
}
static id game_bundle(void) {
    static id b;
    if (!b) b = ((id(*)(id, SEL, id))objc_msgSend)(CLS("NSBundle"), SELR("bundleWithPath:"), ns_str(game_bundle_path()));
    return b;
}
#if TARGET_OS_IPHONE
// The phone can't load AppKit nibs, so this makes what MainMenu.nib holds: the game's FeralApplication as the delegate, and the menu bar,
// which the game adds items to. Its actions do nothing there.
static id menu_item(const char *title, const char *key, unsigned long mods, id submenu) {
    id i = ((id(*)(id, SEL))objc_msgSend)(CLS("NSMenuItem"), SELR("alloc"));
    i = ((id(*)(id, SEL, id, SEL, id))objc_msgSend)(i, SELR("initWithTitle:action:keyEquivalent:"), ns_str(title), (SEL)0, ns_str(key));
    if (mods) ((void (*)(id, SEL, unsigned long))objc_msgSend)(i, SELR("setKeyEquivalentModifierMask:"), mods);
    if (submenu) ((void (*)(id, SEL, id))objc_msgSend)(i, SELR("setSubmenu:"), submenu);
    return i;
}
static id menu_of(const char *title, id *items, int n) {
    id m = ((id(*)(id, SEL))objc_msgSend)(CLS("NSMenu"), SELR("alloc"));
    m = ((id(*)(id, SEL, id))objc_msgSend)(m, SELR("initWithTitle:"), ns_str(title));
    for (int i = 0; i < n; i++) ((void (*)(id, SEL, id))objc_msgSend)(m, SELR("addItem:"), items[i]);
    return m;
}
static void load_main_nib(id app) {
    id del = ((id(*)(id, SEL))objc_msgSend)(CLS("FeralApplication"), SELR("alloc"));
    del = ((id(*)(id, SEL))objc_msgSend)(del, SELR("init"));
    ((void (*)(id, SEL, id))objc_msgSend)(app, SELR("setDelegate:"), del);
    id sep = CLS("NSMenuItem");
    const unsigned long cmd = 1ul << 20, opt = 1ul << 19;
    id app_items[9] = {
        menu_item("About FeralGame", "", 0, nil),
        ((id(*)(id, SEL))objc_msgSend)(sep, SELR("separatorItem")),
        menu_item("Services", "", 0, menu_of("Services", NULL, 0)),
        ((id(*)(id, SEL))objc_msgSend)(sep, SELR("separatorItem")),
        menu_item("Hide FeralGame", "h", cmd, nil),
        menu_item("Hide Others", "h", cmd | opt, nil),
        menu_item("Show All", "", 0, nil),
        ((id(*)(id, SEL))objc_msgSend)(sep, SELR("separatorItem")),
        menu_item("Quit FeralGame", "q", cmd, nil),
    };
    id top[2] = {menu_item("FeralGame", "", 0, menu_of("FeralGame", app_items, 9)), menu_item("Help", "", 0, menu_of("Help", NULL, 0))};
    ((void (*)(id, SEL, id))objc_msgSend)(app, SELR("setMainMenu:"), menu_of("AMainMenu", top, 2));
    fprintf(stderr, "[rt] main nib made by hand: delegate %s, menu present\n", del ? "FeralApplication" : "MISSING");
}
#else
// What NSApplicationMain does with the bundle's main nib, including its menu bar.
static void load_main_nib(id app) {
    char p[1300];
    snprintf(p, sizeof p, "%s/Contents/Resources/MainMenu.nib", game_bundle_path());
    id data = ((id(*)(id, SEL, id))objc_msgSend)(CLS("NSData"), SELR("dataWithContentsOfFile:"), ns_str(p));
    id nib = ((id(*)(id, SEL))objc_msgSend)(CLS("NSNib"), SELR("alloc"));
    nib = ((id(*)(id, SEL, id, id))objc_msgSend)(nib, SELR("initWithNibData:bundle:"), data, game_bundle());
    id tops = nil;
    if (nib) ((signed char (*)(id, SEL, id, id *))objc_msgSend)(nib, SELR("instantiateWithOwner:topLevelObjects:"), app, &tops);
    if (tops) ((id(*)(id, SEL))objc_msgSend)(tops, SELR("retain"));
    id menu = ((id(*)(id, SEL))objc_msgSend)(app, SELR("mainMenu"));
    fprintf(stderr, "[rt] main nib %s, menu %s\n", nib ? "loaded" : "NOT loaded", menu ? "present" : "absent");
}
#endif

typedef void (*msg_v0)(id, SEL);
typedef void (*msg_vq)(id, SEL, long);
typedef void (*msg_vb)(id, SEL, signed char);
static void snap_schedule(void);
// Holding A or S must walk, not open the accent menu. AppKit's press-and-hold also watches for key up in a way that crashed here.
static void disable_press_and_hold(void) {
    id keys[1] = {ns_str("ApplePressAndHoldEnabled")};
    id vals[1] = {((id(*)(id, SEL, signed char))objc_msgSend)(CLS("NSNumber"), SELR("numberWithBool:"), 0)};
    id dict = ((id(*)(id, SEL, const id *, const id *, unsigned long))objc_msgSend)(CLS("NSDictionary"), SELR("dictionaryWithObjects:forKeys:count:"), vals, keys, 1);
    id defaults = ((id(*)(id, SEL))objc_msgSend)(CLS("NSUserDefaults"), SELR("standardUserDefaults"));
    ((void (*)(id, SEL, id))objc_msgSend)(defaults, SELR("registerDefaults:"), dict);
}
SHIM(NSApplicationMain) {
    disable_press_and_hold();
    // The steps NSApplicationMain performs, using the game's own bundle rather than this process's.
    id app = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    u32 slot = rt_import_addr("_NSApp");
    if (slot) G32(slot) = cf_wrap(app);
    ((msg_vq)objc_msgSend)(app, sel_registerName("setActivationPolicy:"), 0);
    load_main_nib(app);
    snap_schedule();
    ((msg_vb)objc_msgSend)(app, sel_registerName("activateIgnoringOtherApps:"), 1);
    fprintf(stderr, "[rt] entering [NSApp run]\n");
    rt_gate_release();
    ((msg_v0)objc_msgSend)(app, sel_registerName("run"));
    rt_gate_acquire();
    fprintf(stderr, "[rt] [NSApp run] returned\n");
    RET(0);
}

// ---------- system code calling guest methods ----------
typedef struct { u64 x[8]; double d[8]; u64 x8; u64 pad; } HRegs;
__asm__(
    ".text\n.globl _guest_imp_tramp\n.p2align 2\n_guest_imp_tramp:\n"
    " stp x29, x30, [sp, #-16]!\n mov x29, sp\n sub sp, sp, #144\n"
    " stp x0, x1, [sp, #0]\n stp x2, x3, [sp, #16]\n stp x4, x5, [sp, #32]\n stp x6, x7, [sp, #48]\n"
    " stp d0, d1, [sp, #64]\n stp d2, d3, [sp, #80]\n stp d4, d5, [sp, #96]\n stp d6, d7, [sp, #112]\n"
    " str x8, [sp, #128]\n mov x0, sp\n bl _guest_imp_dispatch\n"
    " ldp x0, x1, [sp, #0]\n ldp d0, d1, [sp, #64]\n ldp d2, d3, [sp, #80]\n"
    " mov sp, x29\n ldp x29, x30, [sp], #16\n ret\n");
extern void guest_imp_tramp(void);

// Search one method list for a selector, also returning its type encoding.
static u32 list_find_types(u32 ml, const char *sn, const char **types) {
    if (!ml || ml == 0xFFFFFFFFu) return 0;
    u32 n = G32(ml + 4);
    for (u32 i = 0; i < n; i++) {
        u32 e = ml + 8 + i * 12;
        if (!strcmp(GS(G32(e)), sn)) {
            *types = GS(G32(e + 4));
            return G32(e + 8);
        }
    }
    return 0;
}
static u32 gclass_method_types(GClass *k, const char *sn, int meta, const char **types) {
    while (k) {
        u32 addr = meta ? k->meta : k->addr;
        u32 imp = list_find_types(G32(addr + 28), sn, types);
        if (imp) return imp;
        for (u32 a = rt_objc_cat_addr; a < rt_objc_cat_addr + rt_objc_cat_size; a += 28) {
            if (!strcmp(GS(G32(a + 4)), k->name)) {
                imp = list_find_types(G32(a + (meta ? 12 : 8)), sn, types);
                if (imp) return imp;
            }
        }
        k = gclass_by_name(k->super_name);
    }
    return 0;
}
// Guest code compares selectors by pointer, so a selector passed in from the system must be the very address the
// guest's own __message_refs entry holds for that name.
#define SEL_HT 8192u
static struct { const char *name; u32 g; } sel_ht[SEL_HT];
static u32 sel_hash(const char *s) {
    u32 h = 2166136261u;
    for (; *s; s++) h = (h ^ (u8)*s) * 16777619u;
    return h;
}
static void sel_insert(const char *name, u32 g) {
    u32 h = sel_hash(name) & (SEL_HT - 1);
    while (sel_ht[h].name) {
        if (!strcmp(sel_ht[h].name, name)) return;
        h = (h + 1) & (SEL_HT - 1);
    }
    sel_ht[h].name = name;
    sel_ht[h].g = g;
}
static u32 guest_sel(const char *name) {
    u32 h = sel_hash(name) & (SEL_HT - 1);
    while (sel_ht[h].name) {
        if (!strcmp(sel_ht[h].name, name)) return sel_ht[h].g;
        h = (h + 1) & (SEL_HT - 1);
    }
    u32 g = rt_guest_strdup(name);
    sel_insert(GS(g), g);
    return g;
}
extern const unsigned rt_objc_msgrefs_addr, rt_objc_msgrefs_size;
static void sels_load(void) {
    for (u32 a = rt_objc_msgrefs_addr; a < rt_objc_msgrefs_addr + rt_objc_msgrefs_size; a += 4) {
        u32 g = G32(a);
        if (g) sel_insert(GS(g), g);
    }
}

void guest_imp_dispatch(HRegs *r) {
    id self = (id)r->x[0];
    SEL sel = (SEL)r->x[1];
    const char *sn = sel_getName(sel);
    Class cls = object_getClass(self);
    int meta = class_isMetaClass(cls);
    GClass *g = guest_for_host_class(meta ? (Class)self : cls);
    const char *types = "v";
    u32 imp = g ? gclass_method_types(g, sn, meta, &types) : 0;
    if (!imp) {
        fprintf(stderr, "[objc] system called %s, which no guest class implements\n", sn);
        r->x[0] = 0;
        return;
    }
    int held = rt_gate_held();
    if (!held) rt_gate_acquire();
    CPU *c = rt_current_cpu();
    if (!c) c = rt_new_cpu(2u << 20);
    if (trace) fprintf(stderr, "[objc] system -> guest [%s %s] %08x\n", g->name, sn, imp);
    u32 a[40];
    int n = 0;
    const char *rett = skip_quals(types);
    u32 stret_buf = 0;
    if (!strncmp(rett, "{CGRect", 7) || !strncmp(rett, "{_NSRect", 8)) {
        stret_buf = rt_guest_malloc(16);
        a[n++] = stret_buf;
    }
    a[n++] = meta ? g->addr : obj_wrap(self);
    a[n++] = guest_sel(sn);
    const char *t = next_type(next_type(next_type(types)));
    int ix = 2, id_ = 0;
    while (*t && n < 36) {
        const char *q = skip_quals(t);
        char k = *q;
        u64 xv = ix < 8 ? r->x[ix] : 0;
        switch (k) {
            case '@':
                if (trace && xv && ((intptr_t)xv < 0 || xv > 0x100000000ULL) &&
                    ((signed char (*)(id, SEL, Class))objc_msgSend)((id)xv, sel_registerName("isKindOfClass:"), objc_getClass("NSString"))) {
                    fprintf(stderr, "[objc]   guest arg = \"%.160s\"\n", ns_utf8((id)xv));
                }
                a[n++] = obj_wrap((id)xv);
                ix++;
                break;
            case '#': a[n++] = obj_wrap((id)xv); ix++; break;
            case ':': a[n++] = xv ? guest_sel(sel_getName((SEL)xv)) : 0; ix++; break;
            case 'f': {  // CGFloat in the guest, a double on this side
                float f = (float)r->d[id_++];
                memcpy(&a[n++], &f, 4);
                break;
            }
            case 'd': {
                double dv = r->d[id_++];
                memcpy(&a[n], &dv, 8);
                n += 2;
                break;
            }
            case '{':
                if (!strncmp(q, "{_NSRange", 9)) {
                    a[n++] = (u32)xv;
                    a[n++] = (u32)(ix + 1 < 8 ? r->x[ix + 1] : 0);
                    ix += 2;
                } else {
                    int cnt = (!strncmp(q, "{CGRect", 7) || !strncmp(q, "{_NSRect", 8)) ? 4 : 2;
                    for (int j = 0; j < cnt; j++) {
                        float f = (float)r->d[id_++];
                        memcpy(&a[n++], &f, 4);
                    }
                }
                break;
            default:
                a[n++] = (u32)xv;
                ix++;
                break;
        }
        t = next_type(t);
    }
    u32 ret = rt_call_guest(c, imp, n, a);
    switch (*rett) {
        case 'v': break;
        case '@': case '#': r->x[0] = (u64)host_receiver(ret); break;
        case 'f': r->d[0] = c->st[c->top]; FPOP(); break;
        case 'd': r->d[0] = c->st[c->top]; FPOP(); break;
        case '{':
            if (stret_buf) {
                for (int j = 0; j < 4; j++) r->d[j] = *(f32a *)(GMEM + stret_buf + (u32)j * 4);
                rt_guest_free(stret_buf);
            } else if (!strncmp(rett, "{_NSRange", 9)) {
                // Two words in eax:edx, widened; NSNotFound is a different number on each side.
                r->x[0] = ret == 0x7FFFFFFFu ? 0x7FFFFFFFFFFFFFFFull : ret;
                r->x[1] = c->edx;
            } else {
                r->x[0] = ret;
            }
            break;
        case 'c': case 'C': case 'B': r->x[0] = ret & 0xFF; break;
        case 'i': case 'l': r->x[0] = (u64)(i64)(i32)ret; break;
        default: r->x[0] = ret; break;
    }
    if (!held) rt_gate_release();
}

// Debug aid: the launcher page's JavaScript console and errors, printed to stderr.
static void js_console(id self, SEL cmd, id view, id dict) {
    (void)self, (void)cmd, (void)view;
    id d = ((id(*)(id, SEL))objc_msgSend)(dict, sel_registerName("description"));
    fprintf(stderr, "[js] %s\n", ns_utf8(d));
}
static id web_debug_delegate(void) {
    static id inst;
    if (inst) return inst;
    Class k = objc_allocateClassPair(objc_getClass("NSObject"), "ArkhamWebDebugDelegate", 0);
    class_addMethod(k, sel_registerName("webView:addMessageToConsole:"), (IMP)js_console, "v@:@@");
    objc_registerClassPair(k);
    inst = ((id(*)(id, SEL))objc_msgSend)(((id(*)(id, SEL))objc_msgSend)((id)k, sel_registerName("alloc")), sel_registerName("init"));
    return inst;
}

// Debug aid: write the first visible window's content to a PNG after a delay (ARKHAM_SNAP=path, ARKHAM_SNAP_AFTER=secs).
static void snap_now(const char *path) {
    id app = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"), SELR("sharedApplication"));
    id wins = ((id(*)(id, SEL))objc_msgSend)(app, SELR("windows"));
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(wins, SELR("count"));
    // What the window server composites, which is what the player sees: this does include OpenGL surfaces.
    typedef CGImageRef (*list_image_fn)(CGRect, uint32_t, uint32_t, uint32_t);
    list_image_fn list_image = (list_image_fn)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
    for (unsigned long i = 0; i < n; i++) {
        id w = ((id(*)(id, SEL, unsigned long))objc_msgSend)(wins, SELR("objectAtIndex:"), i);
        CGRect fr = ((CGRect(*)(id, SEL))objc_msgSend)(w, SELR("frame"));
        id cv = ((id(*)(id, SEL))objc_msgSend)(w, SELR("contentView"));
        long num = ((long (*)(id, SEL))objc_msgSend)(w, SELR("windowNumber"));
        long lvl = ((long (*)(id, SEL))objc_msgSend)(w, SELR("level"));
        fprintf(stderr, "[snap] window %lu #%ld %s frame %.0f,%.0f %.0fx%.0f visible=%d level=%ld key=%d content=%s\n", i, num, class_getName(object_getClass(w)), fr.origin.x, fr.origin.y,
                fr.size.width, fr.size.height, ((signed char (*)(id, SEL))objc_msgSend)(w, SELR("isVisible")), lvl,
                ((signed char (*)(id, SEL))objc_msgSend)(w, SELR("isKeyWindow")), cv ? class_getName(object_getClass(cv)) : "nil");
        if (list_image && ((signed char (*)(id, SEL))objc_msgSend)(w, SELR("isVisible")) && fr.size.height >= 100) {
            CGImageRef im = list_image(CGRectNull, 8 /* including window */, (uint32_t)num, 1 /* ignore framing */);
            fprintf(stderr, "[snap] composited image of #%ld: %zux%zu\n", num, im ? CGImageGetWidth(im) : 0, im ? CGImageGetHeight(im) : 0);
            if (im) {
                char p2[1100];
                snprintf(p2, sizeof p2, "%s.window%lu.png", path, i);
                CFStringRef ps = CFStringCreateWithCString(NULL, p2, kCFStringEncodingUTF8);
                CFURLRef url = CFURLCreateWithFileSystemPath(NULL, ps, kCFURLPOSIXPathStyle, false);
                CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
                if (d) {
                    CGImageDestinationAddImage(d, im, NULL);
                    CGImageDestinationFinalize(d);
                    CFRelease(d);
                }
                CFRelease(url);
                CFRelease(ps);
                CGImageRelease(im);
            }
        }
    }
    for (unsigned long i = 0; i < n; i++) {
        id w = ((id(*)(id, SEL, unsigned long))objc_msgSend)(wins, SELR("objectAtIndex:"), i);
        if (!((signed char (*)(id, SEL))objc_msgSend)(w, SELR("isVisible"))) continue;
        id v = ((id(*)(id, SEL))objc_msgSend)(w, SELR("contentView"));
        CGRect b = ((CGRect(*)(id, SEL))objc_msgSend)(v, SELR("bounds"));
        if (b.size.height < 100) continue;
        id rep = ((id(*)(id, SEL, CGRect))objc_msgSend)(v, SELR("bitmapImageRepForCachingDisplayInRect:"), b);
        ((void (*)(id, SEL, CGRect, id))objc_msgSend)(v, SELR("cacheDisplayInRect:toBitmapImageRep:"), b, rep);
        id props = ((id(*)(id, SEL))objc_msgSend)(CLS("NSDictionary"), SELR("dictionary"));
        id png = ((id(*)(id, SEL, unsigned long, id))objc_msgSend)(rep, SELR("representationUsingType:properties:"), 4, props);
        ((signed char (*)(id, SEL, id, signed char))objc_msgSend)(png, SELR("writeToFile:atomically:"), ns_str(path), 1);
        fprintf(stderr, "[snap] wrote %s %.0fx%.0f\n", path, b.size.width, b.size.height);
        return;
    }
    fprintf(stderr, "[snap] no visible window\n");
}
// On-screen frame rate (ARKHAM_FPS=1): a small panel attached above the game's window, updated once a second.
static id fps_panel, fps_label;
void rt_fps_overlay(double fps, double worst_ms) {
#if TARGET_OS_IPHONE
    (void)fps, (void)worst_ms;
    return;  // the phone's line over the game is the app shell's (rt_ios_overlay)
#endif
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("ARKHAM_FPS") != NULL;
    if (!enabled) return;
    char text[64];
    snprintf(text, sizeof text, "%.0f FPS  %.0f ms", fps, worst_ms);
    char *copy = strdup(text);
    dispatch_async(dispatch_get_main_queue(), ^{
        id app = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"), SELR("sharedApplication"));
        id win = ((id(*)(id, SEL))objc_msgSend)(app, SELR("keyWindow"));
        if (!win) win = ((id(*)(id, SEL))objc_msgSend)(app, SELR("mainWindow"));
        if (win && !fps_panel) {
            CGRect r = CGRectMake(0, 0, 170, 26);
            fps_panel = ((id(*)(id, SEL, CGRect, unsigned long, unsigned long, signed char))objc_msgSend)(((id(*)(id, SEL))objc_msgSend)(CLS("NSPanel"), SELR("alloc")),
                SELR("initWithContentRect:styleMask:backing:defer:"), r, 128 /* non-activating */, 2, 0);
            ((void (*)(id, SEL, signed char))objc_msgSend)(fps_panel, SELR("setIgnoresMouseEvents:"), 1);
            ((void (*)(id, SEL, id))objc_msgSend)(fps_panel, SELR("setBackgroundColor:"), ((id(*)(id, SEL))objc_msgSend)(CLS("NSColor"), SELR("blackColor")));
            ((void (*)(id, SEL, double))objc_msgSend)(fps_panel, SELR("setAlphaValue:"), 0.8);
            fps_label = ((id(*)(id, SEL, id))objc_msgSend)(CLS("NSTextField"), SELR("labelWithString:"), ns_str(""));
            ((void (*)(id, SEL, id))objc_msgSend)(fps_label, SELR("setTextColor:"), ((id(*)(id, SEL))objc_msgSend)(CLS("NSColor"), SELR("whiteColor")));
            ((void (*)(id, SEL, CGRect))objc_msgSend)(fps_label, SELR("setFrame:"), CGRectMake(8, 4, 160, 18));
            ((void (*)(id, SEL, id))objc_msgSend)(((id(*)(id, SEL))objc_msgSend)(fps_panel, SELR("contentView")), SELR("addSubview:"), fps_label);
            CGRect f = ((CGRect(*)(id, SEL))objc_msgSend)(win, SELR("frame"));
            ((void (*)(id, SEL, CGPoint))objc_msgSend)(fps_panel, SELR("setFrameOrigin:"), CGPointMake(f.origin.x + f.size.width - 190, f.origin.y + f.size.height - 46));
            ((void (*)(id, SEL, id, long))objc_msgSend)(win, SELR("addChildWindow:ordered:"), fps_panel, 1);
        }
        if (fps_label) ((void (*)(id, SEL, id))objc_msgSend)(fps_label, SELR("setStringValue:"), ns_str(copy));
        free(copy);
    });
}

// Debug aid: press the launcher's Play button by script after a delay (ARKHAM_AUTO_PLAY=secs).
static id the_webview;
void rt_note_webview(id v) { the_webview = v; }
static void auto_play_schedule(void) {
    const char *after = getenv("ARKHAM_AUTO_PLAY");
    if (!after) return;
    CFRunLoopTimerRef t = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + atof(after), 0, 0, 0, ^(CFRunLoopTimerRef tt) {
        (void)tt;
        fprintf(stderr, "[rt] pressing Play by script\n");
        id js = ns_str("$('.main-playButton').click(); $('.main-playButton').length");
        id r = ((id(*)(id, SEL, id))objc_msgSend)(the_webview, SELR("stringByEvaluatingJavaScriptFromString:"), js);
        fprintf(stderr, "[rt] play buttons found: %s\n", r ? ns_utf8(r) : "(none)");
    });
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
    CFRelease(t);
}
// Debug aid: scripted input for test runs. ARKHAM_INPUT="40:key:36,45:click:900:600,50:move:10:10" posts real AppKit events
// at those seconds after launch. Coordinates are points from the window's top left corner. "50:shot:3" saves the next frame as logs/shots/shot-3.png,
// and "50:pad:49:0" sets an axis of the pad with no hardware (ARKHAM_FAKE_PAD; page 1, usage 49 is the left stick's Y on the PlayStation one).
void rt_request_snapshot(const char *name);  // mtl_device.m
void rt_fake_pad_set(int page, int usage, int value);  // shims_hid.m
void rt_set_scripted_mouse(double x, double y);
static id script_window(void) {
    id app = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"), SELR("sharedApplication"));
    id w = ((id(*)(id, SEL))objc_msgSend)(app, SELR("keyWindow"));
    if (w) return w;
    w = ((id(*)(id, SEL))objc_msgSend)(app, SELR("mainWindow"));
    if (w) return w;
    id all = ((id(*)(id, SEL))objc_msgSend)(app, SELR("windows"));
    return ((id(*)(id, SEL))objc_msgSend)(all, SELR("firstObject"));
}
static void script_post(id ev) {
    id app = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"), SELR("sharedApplication"));
    ((void (*)(id, SEL, id, signed char))objc_msgSend)(app, SELR("postEvent:atStart:"), ev, 0);
}
// One key event: type is 10 for down and 11 for up.
static void script_key_event(int code, unsigned long type) {
    id w = script_window();
    long num = w ? ((long (*)(id, SEL))objc_msgSend)(w, SELR("windowNumber")) : 0;
    unsigned short ch = code == 36 ? 13 : code == 53 ? 27 : code == 49 ? 32 : code == 126 ? 0xF700 : code == 125 ? 0xF701 : code == 123 ? 0xF702 : code == 124 ? 0xF703 :
                        code == 13 ? 'w' : code == 0 ? 'a' : code == 1 ? 's' : code == 2 ? 'd' : 0;
    id str = ((id(*)(id, SEL, const unsigned short *, unsigned long))objc_msgSend)(CLS("NSString"), SELR("stringWithCharacters:length:"), &ch, ch ? 1 : 0);
    typedef id (*key_fn)(id, SEL, unsigned long, CGPoint, unsigned long, double, long, id, id, id, signed char, unsigned short);
    SEL sel = SELR("keyEventWithType:location:modifierFlags:timestamp:windowNumber:context:characters:charactersIgnoringModifiers:isARepeat:keyCode:");
    id ev = ((key_fn)objc_msgSend)(CLS("NSEvent"), sel, type, CGPointZero, 0, CFAbsoluteTimeGetCurrent(), num, nil, str, str, 0, (unsigned short)code);
    // With no key window, because a test run is not the active app, AppKit would drop the key and beep. Hand it to the window.
    id app = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"), SELR("sharedApplication"));
    if (w && !((id(*)(id, SEL))objc_msgSend)(app, SELR("keyWindow"))) ((void (*)(id, SEL, id))objc_msgSend)(w, SELR("sendEvent:"), ev);
    else script_post(ev);
}
// Pressing Cmd by itself: the game answers by watching for key-ups, which the system doesn't deliver while Cmd is down.
static void script_command_flag(void) {
#if !TARGET_OS_IPHONE  // scripted runs are a Mac thing
    CGEventRef cg = CGEventCreateKeyboardEvent(NULL, 55, true);
    CGEventSetType(cg, kCGEventFlagsChanged);
    CGEventSetFlags(cg, kCGEventFlagMaskCommand | 0x8);
    id ev = ((id(*)(id, SEL, CGEventRef))objc_msgSend)(CLS("NSEvent"), SELR("eventWithCGEvent:"), cg);
    CFRelease(cg);
    id w = script_window();
    if (w) ((void (*)(id, SEL, id))objc_msgSend)(w, SELR("sendEvent:"), ev);
#endif
}
// Cmd+Q through the application's event queue, the way the real key arrives: AppKit routes it to the menu.
static void script_command_q(unsigned long type) {
    unsigned short ch = 'q';
    id str = ((id(*)(id, SEL, const unsigned short *, unsigned long))objc_msgSend)(CLS("NSString"), SELR("stringWithCharacters:length:"), &ch, 1);
    typedef id (*key_fn)(id, SEL, unsigned long, CGPoint, unsigned long, double, long, id, id, id, signed char, unsigned short);
    SEL sel = SELR("keyEventWithType:location:modifierFlags:timestamp:windowNumber:context:characters:charactersIgnoringModifiers:isARepeat:keyCode:");
    id w = script_window();
    long num = w ? ((long (*)(id, SEL))objc_msgSend)(w, SELR("windowNumber")) : 0;
    script_post(((key_fn)objc_msgSend)(CLS("NSEvent"), sel, type, CGPointZero, 1ul << 20, CFAbsoluteTimeGetCurrent(), num, nil, str, str, 0, (unsigned short)12));
}
static void script_key(int code) {
    script_key_event(code, 10);
    script_key_event(code, 11);
    fprintf(stderr, "[input] key %d\n", code);
}
// Relative mouse motion, which is what steers the camera in play.
void rt_scripted_mouse_get(double *x, double *y);
static void script_look(double dx, double dy) {
    // The engine turns the camera by how far the pointer is from where it last warped it.
    double x = 0, y = 0;
    rt_scripted_mouse_get(&x, &y);
    rt_set_scripted_mouse(x + dx, y + dy);
    fprintf(stderr, "[input] look %.0f %.0f\n", dx, dy);
}
static void script_mouse(int type, double x, double y) {
    id w = script_window();
    if (!w) return;
    long num = ((long (*)(id, SEL))objc_msgSend)(w, SELR("windowNumber"));
    CGRect fr = ((CGRect(*)(id, SEL))objc_msgSend)(w, SELR("frame"));
    id cv = ((id(*)(id, SEL))objc_msgSend)(w, SELR("contentView"));
    CGRect b = ((CGRect(*)(id, SEL))objc_msgSend)(cv, SELR("bounds"));
    rt_set_scripted_mouse(fr.origin.x + x, y);
    typedef id (*mouse_fn)(id, SEL, unsigned long, CGPoint, unsigned long, double, long, id, long, long, float);
    SEL sel = SELR("mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:clickCount:pressure:");
    CGPoint at = CGPointMake(x, b.size.height - y);
    script_post(((mouse_fn)objc_msgSend)(CLS("NSEvent"), sel, (unsigned long)type, at, 0, CFAbsoluteTimeGetCurrent(), num, nil, 0, type == 5 ? 0 : 1, type == 5 ? 0.0f : 1.0f));
}
// A left click where the pointer already is, which skips movies. Moving the pointer would turn the camera.
void rt_scripted_mouse_get(double *x, double *y);
static void script_press(void) {
    double x = 0, y = 0;
    rt_scripted_mouse_get(&x, &y);
    id w = script_window();
    if (!w) return;
    long num = ((long (*)(id, SEL))objc_msgSend)(w, SELR("windowNumber"));
    id cv = ((id(*)(id, SEL))objc_msgSend)(w, SELR("contentView"));
    CGRect b = ((CGRect(*)(id, SEL))objc_msgSend)(cv, SELR("bounds"));
    CGRect fr = ((CGRect(*)(id, SEL))objc_msgSend)(w, SELR("frame"));
    typedef id (*mouse_fn)(id, SEL, unsigned long, CGPoint, unsigned long, double, long, id, long, long, float);
    SEL sel = SELR("mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:clickCount:pressure:");
    CGPoint at = CGPointMake(x - fr.origin.x, b.size.height - y);
    for (unsigned long type = 1; type <= 2; type++)
        script_post(((mouse_fn)objc_msgSend)(CLS("NSEvent"), sel, type, at, 0, CFAbsoluteTimeGetCurrent(), num, nil, 0, 1, type == 1 ? 1.0f : 0.0f));
}
static void input_schedule(void) {
    const char *spec = getenv("ARKHAM_INPUT");
    if (!spec) return;
    // A scripted run leaves the player's real cursor alone.
    rt_set_scripted_mouse(900, 565);
    char *copy = strdup(spec), *save = NULL;
    double start = CFAbsoluteTimeGetCurrent();
    for (char *item = strtok_r(copy, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        double secs = 0, x = 0, y = 0;
        char kind[16] = "";
        if (sscanf(item, "%lf:%15[a-z]:%lf:%lf", &secs, kind, &x, &y) < 3) continue;
        int is_key = !strcmp(kind, "key"), is_click = !strcmp(kind, "click");
        int is_down = !strcmp(kind, "down"), is_up = !strcmp(kind, "up"), is_look = !strcmp(kind, "look"), is_quit = !strcmp(kind, "quit"), is_press = !strcmp(kind, "press"), is_cmdq = !strcmp(kind, "cmdq");
        int is_shot = !strcmp(kind, "shot"), is_pad = !strcmp(kind, "pad");
        CFRunLoopTimerRef t = CFRunLoopTimerCreateWithHandler(NULL, start + secs, 0, 0, 0, ^(CFRunLoopTimerRef tt) {
            (void)tt;
            if (is_pad) {
                rt_fake_pad_set(1, (int)x, (int)y);
            } else if (is_shot) {
                char name[32];
                snprintf(name, sizeof name, "shot-%d", (int)x);
                rt_request_snapshot(name);
                fprintf(stderr, "[input] shot %d\n", (int)x);
            } else if (is_press) {
                script_press();
            } else if (is_cmdq) {
                fprintf(stderr, "[input] cmd-q\n");
                script_command_flag();
                script_command_q(10);
                script_command_q(11);
            } else if (is_quit) {
                fprintf(stderr, "[input] quit\n");
                id app = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"), SELR("sharedApplication"));
                ((void (*)(id, SEL, id))objc_msgSend)(app, SELR("terminate:"), nil);
            } else if (is_key) {
                script_key((int)x);
            } else if (is_down || is_up) {
                script_key_event((int)x, is_down ? 10 : 11);
                fprintf(stderr, "[input] key %d %s\n", (int)x, is_down ? "down" : "up");
            } else if (is_look) {
                script_look(x, y);
            } else {
                script_mouse(5, x, y);
                if (is_click) {
                    script_mouse(1, x, y);
                    script_mouse(2, x, y);
                }
                fprintf(stderr, "[input] mouse %s %.0f,%.0f\n", is_click ? "click" : "move", x, y);
            }
        });
        CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
        CFRelease(t);
    }
    free(copy);
}
// Steering by hand once and repeating it. ARKHAM_RECORD=file writes the keys pressed once a level has started, and ARKHAM_REPLAY=file presses them again at the same moments.
// Times count from the first frame with over 300 draws, because loading takes a different time on each run. A scripted run ignores the real mouse, so keys are all there is.
static double level_started;
static FILE *record_file;
void rt_input_level_started(void) {
    if (level_started) return;
    level_started = CFAbsoluteTimeGetCurrent();
    fprintf(stderr, "[input] level started\n");
    const char *path = getenv("ARKHAM_REPLAY");
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return;
    double t;
    char kind[8];
    int code, n = 0;
    while (fscanf(f, "%lf %7s %d", &t, kind, &code) == 3) {
        int key = code, down = !strcmp(kind, "down");
        CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(NULL, level_started + t, 0, 0, 0, ^(CFRunLoopTimerRef tt) {
            (void)tt;
            script_key_event(key, down ? 10 : 11);
            fprintf(stderr, "[input] replay key %d %s\n", key, down ? "down" : "up");
        });
        CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
        CFRelease(timer);
        n++;
    }
    fclose(f);
    CFRunLoopWakeUp(CFRunLoopGetMain());
    fprintf(stderr, "[input] replaying %d key events\n", n);
}
static void record_schedule(void) {
    const char *path = getenv("ARKHAM_RECORD");
    if (!path) return;
    record_file = fopen(path, "w");
    if (!record_file) return;
    id (^handler)(id) = ^id(id ev) {
        if (level_started && !((signed char (*)(id, SEL))objc_msgSend)(ev, SELR("isARepeat"))) {
            unsigned long type = ((unsigned long (*)(id, SEL))objc_msgSend)(ev, SELR("type"));
            unsigned short code = ((unsigned short (*)(id, SEL))objc_msgSend)(ev, SELR("keyCode"));
            fprintf(record_file, "%.3f %s %d\n", CFAbsoluteTimeGetCurrent() - level_started, type == 10 ? "down" : "up", code);
            fflush(record_file);
        }
        return ev;
    };
    ((id(*)(id, SEL, unsigned long long, id))objc_msgSend)((id)objc_getClass("NSEvent"), SELR("addLocalMonitorForEventsMatchingMask:handler:"), (1ull << 10) | (1ull << 11), (id)(void *)handler);
    fprintf(stderr, "[input] recording keys to %s\n", path);
}
static void snap_schedule(void) {
    auto_play_schedule();
    input_schedule();
    record_schedule();
    const char *path = getenv("ARKHAM_SNAP");
    if (!path) return;
    const char *after = getenv("ARKHAM_SNAP_AFTER");
    double secs = after ? atof(after) : 15.0;
    CFRunLoopTimerRef t = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + secs, 0, 0, 0, ^(CFRunLoopTimerRef tt) {
        (void)tt;
        snap_now(path);
    });
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
    CFRelease(t);
}

// Make the guest classes real system classes, so nibs can instantiate them and AppKit can call their methods.
static void add_methods(Class k, u32 ml) {
    if (!ml || ml == 0xFFFFFFFFu) return;
    u32 n = G32(ml + 4);
    for (u32 i = 0; i < n; i++) {
        u32 e = ml + 8 + i * 12;
        // A host encoding built from the guest's would mislead AppKit, so use the superclass's when it has one.
        SEL sel = sel_registerName(GS(G32(e)));
        Class sup = class_getSuperclass(k);  // k itself isn't registered yet, so it can't be queried
        Method base = sup ? class_getInstanceMethod(sup, sel) : NULL;
        const char *enc = base ? method_getTypeEncoding(base) : GS(G32(e + 4));
        class_addMethod(k, sel, (IMP)guest_imp_tramp, enc);
    }
}
static void register_guest_classes(void) {
    int done[64] = {0};
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < ngclasses; i++) {
            if (done[i]) continue;
            GClass *g = &gclasses[i];
            Class sup = g->super_name ? objc_getClass(g->super_name) : Nil;
            if (g->super_name && !sup) {
                if (round == 7) fprintf(stderr, "[objc] no superclass %s for %s\n", g->super_name, g->name);
                continue;
            }
            Class k = objc_allocateClassPair(sup, g->name, 0);
            if (!k) {
                done[i] = 1;
                continue;
            }
            add_methods(k, G32(g->addr + 28));
            add_methods(object_getClass((id)k), G32(g->meta + 28));
            for (u32 a = rt_objc_cat_addr; a < rt_objc_cat_addr + rt_objc_cat_size; a += 28) {
                if (!strcmp(GS(G32(a + 4)), g->name)) {
                    add_methods(k, G32(a + 8));
                    add_methods(object_getClass((id)k), G32(a + 12));
                }
            }
            objc_registerClassPair(k);
            // The legacy class metadata isn't read for protocols. Without this one, AppKit doesn't deliver typed characters
            // to the view and plays the error sound for each of them.
            const char *ignored;
            if (gclass_method_types(g, "insertText:replacementRange:", 0, &ignored)) {
                Protocol *tic = objc_getProtocol("NSTextInputClient");
                if (tic) class_addProtocol(k, tic);
                else fprintf(stderr, "[objc] NSTextInputClient protocol not found\n");
            }
            done[i] = 1;
        }
    }
}

// Synthesized properties of guest objects live in guest memory.
SHIM(objc_setProperty) { G32(ARG(0) + ARG(2)) = ARG(3); }
SHIM(objc_getProperty) { RET(G32(ARG(0) + ARG(2))); }

// @try: the exception-free path needs nothing more than a setjmp that returns 0.
SHIM(objc_exception_noop) { RET(0); }
SHIM(objc_exception_throw) {
    u32 e = ARG(0);
    char b[80];
    fprintf(stderr, "*** guest threw an Objective-C exception: %s\n", recv_name(e, b, sizeof b));
    rt_trap(c, G32(c->esp), "objc_exception_throw");
}
SHIM(setjmp_stub) { RET(0); }
SHIM(longjmp_stub) { rt_trap(c, G32(c->esp), "longjmp"); }
SHIM(objc_sync_enter) { RET(0); }
SHIM(objc_sync_exit) { RET(0); }
SHIM(NSClassFromString) {
    char b[256] = "";
    CFStringGetCString(cf_unwrap(ARG(0)), b, sizeof b, kCFStringEncodingUTF8);
    Class k = objc_getClass(b);
    RET(k ? cf_wrap((CFTypeRef)k) : 0);
}

extern CFArrayRef NSSearchPathForDirectoriesInDomains(unsigned long, unsigned long, signed char);
extern CFStringRef NSHomeDirectory(void);
extern CFStringRef NSTemporaryDirectory(void);
extern CFStringRef NSUserName(void);
SHIM(NSSearchPathForDirectoriesInDomains) { RET(cf_wrap(NSSearchPathForDirectoriesInDomains(ARG(0), ARG(1), (signed char)ARG(2)))); }
SHIM(NSHomeDirectory) { RET(cf_wrap(NSHomeDirectory())); }
SHIM(NSTemporaryDirectory) { RET(cf_wrap(NSTemporaryDirectory())); }
SHIM(NSUserName) { RET(cf_wrap(NSUserName())); }
SHIM(NSLog) {
    id fmt = host_receiver(ARG(0));
    char *z = rt_format_guest(fmt ? ns_utf8(fmt) : "", c->esp + 8);
    fprintf(stderr, "[NSLog] %s\n", z);
    free(z);
}
SHIM(NSSetUncaughtExceptionHandler) {}
SHIM(NSApplicationLoad) { RET(1); }

id rt_game_bundle(void) { return game_bundle(); }
id rt_web_debug_delegate(void) { return web_debug_delegate(); }

// ---------- skipping Feral's launcher ----------
// The launcher is a web page in a window, run as a modal loop by F_2055740: it shows the window, pumps events until the page's Play stops the loop,
// hides it, and returns the byte at +0x40 of its object, which Play sets. The phone has no web view to show it in, so there the loop answers at
// once, as Play would. ARKHAM_SKIP_LAUNCHER=1 does the same on the Mac. It is only ever called through a vtable, so taking its place in the
// dispatcher's table is enough.
#define LAUNCHER_MODAL 0x2055740u
static void launcher_play(CPU *c) {
    u32 self = G32(c->esp + 4);
    fprintf(stderr, "[rt] launcher skipped: modal loop of %08x answered as Play, called from %08x\n", self, G32(c->esp));
    if (self) G8(self + 0x40) = 1;
    c->eax = 1;
    c->esp += 4;  // ret: a guest function pops its own return address
}
void rt_launcher_skip_install(void) {
#if TARGET_OS_IPHONE
    int skip = 1;
#else
    int skip = getenv("ARKHAM_SKIP_LAUNCHER") != NULL;
#endif
    if (!skip) return;
    void rt_ht_add(u32 addr, guest_fn fn);
    rt_ht_add(LAUNCHER_MODAL, launcher_play);
}

void rt_objc_init(void) {
    trace = getenv("ARKHAM_TRACE_OBJC") != NULL;
    h2p = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    p2h = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    gclasses_load();
    sels_load();
    register_guest_classes();
}

const ShimReg rt_shims_objc[] = {
    R(objc_setProperty) R(objc_getProperty) R2("_objc_exception_try_enter", objc_exception_noop) R2("_objc_exception_try_exit", objc_exception_noop) R(objc_exception_throw)
    R2("__setjmp", setjmp_stub) R2("_setjmp", setjmp_stub) R2("_longjmp", longjmp_stub)
    R(NSApplicationMain) R(objc_msgSend) R(objc_msgSendSuper) R(objc_msgSend_fpret) R(objc_msgSend_stret) R(objc_sync_enter) R(objc_sync_exit)
    R(NSClassFromString) R(NSSearchPathForDirectoriesInDomains) R(NSHomeDirectory) R(NSTemporaryDirectory) R(NSUserName)
    R(NSLog) R(NSSetUncaughtExceptionHandler) R(NSApplicationLoad)
    {NULL, NULL},
};
