// libSystem: strings, memory, stdio, files, time, math. Guest pointers are 32-bit offsets into rt_mem.
#include "shim.h"
#include <pthread.h>
#include "shim_reg.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

// ---------- errno ----------
// Each guest thread has its own errno.
static __thread u32 tl_errno;
static u32 errno_addr(void) {
    if (!tl_errno) tl_errno = rt_guest_calloc(1, 16);
    return tl_errno;
}
SHIM(__error) { RET(errno_addr()); }
static inline void set_errno(void) { int e = errno; G32(errno_addr()) = (u32)e; }

// ---------- handles for opaque host pointers ----------
#define HANDLE_BASE 0xEE000000u
#define HANDLE_MAX 65536
static void *handles[HANDLE_MAX];
static u32 handle_next = 1;

static pthread_mutex_t handle_mu = PTHREAD_MUTEX_INITIALIZER;
u32 rt_handle_new(void *p) {
    if (!p) return 0;
    pthread_mutex_lock(&handle_mu);
    for (u32 i = 0; i < HANDLE_MAX; i++) {
        u32 k = (handle_next + i) % HANDLE_MAX;
        if (k && !handles[k]) {
            handles[k] = p;
            handle_next = k + 1;
            pthread_mutex_unlock(&handle_mu);
            return HANDLE_BASE + k * 16;
        }
    }
    fprintf(stderr, "*** out of guest handles\n");
    _Exit(75);
}
void *rt_handle_get(u32 h) {
    if (h < HANDLE_BASE || h >= HANDLE_BASE + HANDLE_MAX * 16) return NULL;
    return handles[(h - HANDLE_BASE) / 16];
}
void rt_handle_free(u32 h) {
    if (h >= HANDLE_BASE && h < HANDLE_BASE + HANDLE_MAX * 16) handles[(h - HANDLE_BASE) / 16] = NULL;
}
int rt_is_handle(u32 h) { return h >= HANDLE_BASE && h < HANDLE_BASE + HANDLE_MAX * 16; }

#define FILEP(h) ((FILE *)rt_handle_get(h))

// ---------- strings and memory ----------
SHIM(strlen) { RET(strlen(GS(ARG(0)))); }
SHIM(strnlen) { RET(strnlen(GS(ARG(0)), ARG(1))); }
SHIM(strcmp) { RET(strcmp(GS(ARG(0)), GS(ARG(1)))); }
SHIM(strncmp) { RET(strncmp(GS(ARG(0)), GS(ARG(1)), ARG(2))); }
SHIM(strcasecmp) { RET(strcasecmp(GS(ARG(0)), GS(ARG(1)))); }
SHIM(strncasecmp) { RET(strncasecmp(GS(ARG(0)), GS(ARG(1)), ARG(2))); }
SHIM(strcoll) { RET(strcmp(GS(ARG(0)), GS(ARG(1)))); }
SHIM(strcpy) { strcpy(GP(ARG(0)), GS(ARG(1))); RET(ARG(0)); }
SHIM(strncpy) { strncpy(GP(ARG(0)), GS(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(strcat) { strcat(GP(ARG(0)), GS(ARG(1))); RET(ARG(0)); }
SHIM(strncat) { strncat(GP(ARG(0)), GS(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(strlcpy) { RET(strlcpy(GP(ARG(0)), GS(ARG(1)), ARG(2))); }
SHIM(strlcat) { RET(strlcat(GP(ARG(0)), GS(ARG(1)), ARG(2))); }
SHIM(__strcpy_chk) { strcpy(GP(ARG(0)), GS(ARG(1))); RET(ARG(0)); }
static u32 g_str_result(const char *base, u32 gbase, const char *r) { return r ? gbase + (u32)(r - base) : 0; }
SHIM(strchr) { const char *s = GS(ARG(0)); RET(g_str_result(s, ARG(0), strchr(s, (int)ARG(1)))); }
SHIM(strrchr) { const char *s = GS(ARG(0)); RET(g_str_result(s, ARG(0), strrchr(s, (int)ARG(1)))); }
SHIM(strstr) { const char *s = GS(ARG(0)); RET(g_str_result(s, ARG(0), strstr(s, GS(ARG(1))))); }
SHIM(strnstr) { const char *s = GS(ARG(0)); RET(g_str_result(s, ARG(0), strnstr(s, GS(ARG(1)), ARG(2)))); }
SHIM(strpbrk) { const char *s = GS(ARG(0)); RET(g_str_result(s, ARG(0), strpbrk(s, GS(ARG(1))))); }
SHIM(strspn) { RET(strspn(GS(ARG(0)), GS(ARG(1)))); }
SHIM(strcspn) { RET(strcspn(GS(ARG(0)), GS(ARG(1)))); }
SHIM(memcpy) { memmove(GP(ARG(0)), GS(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(__memcpy_chk) { memmove(GP(ARG(0)), GS(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(memmove) { memmove(GP(ARG(0)), GS(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(memset) { memset(GP(ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
SHIM(__bzero) { memset(GP(ARG(0)), 0, ARG(1)); }
SHIM(memcmp) { RET(memcmp(GS(ARG(0)), GS(ARG(1)), ARG(2))); }
SHIM(memchr) { const char *s = GS(ARG(0)); const char *r = memchr(s, (int)ARG(1), ARG(2)); RET(g_str_result(s, ARG(0), r)); }
SHIM(memset_pattern16) {
    u8 *d = GP(ARG(0));
    const u8 *p = (const u8 *)GS(ARG(1));
    for (u32 i = 0, n = ARG(2); i < n; i++) d[i] = p[i & 15];
}
SHIM(wcslen) { RET(wcslen((const wchar_t *)GP(ARG(0)))); }
SHIM(wcscmp) { RET(wcscmp((const wchar_t *)GP(ARG(0)), (const wchar_t *)GP(ARG(1)))); }
SHIM(wcsncmp) { RET(wcsncmp((const wchar_t *)GP(ARG(0)), (const wchar_t *)GP(ARG(1)), ARG(2))); }
SHIM(mbstowcs) { RET(mbstowcs(NP(ARG(0)), GS(ARG(1)), ARG(2))); }

// ---------- heap ----------
SHIM(malloc) { RET(rt_guest_malloc(ARG(0))); }
SHIM(calloc) { RET(rt_guest_calloc(ARG(0), ARG(1))); }
SHIM(realloc) { RET(rt_guest_realloc(ARG(0), ARG(1))); }
SHIM(free) { rt_guest_free(ARG(0)); }
SHIM(malloc_size) { RET(rt_guest_block_size(ARG(0))); }
SHIM(posix_memalign) {
    G32(ARG(0)) = rt_guest_memalign(ARG(1), ARG(2));
    RET(0);
}

// ---------- formatted output with guest varargs ----------
typedef struct { char *buf; size_t cap, len; } OutBuf;
static void ob_put(OutBuf *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 256;
        while (cap < o->len + n + 1) cap *= 2;
        o->buf = realloc(o->buf, cap);
        o->cap = cap;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = 0;
}

char *rt_objc_describe(u32 g);

// Formats per the i386 ABI: long and size_t are 4 bytes, doubles 8 bytes, everything 4-byte aligned on the stack.
static void gformat(OutBuf *o, const char *f, u32 va) {
    ob_put(o, "", 0);
    while (*f) {
        if (*f != '%') {
            const char *e = strchr(f, '%');
            size_t n = e ? (size_t)(e - f) : strlen(f);
            ob_put(o, f, n);
            f += n;
            continue;
        }
        const char *start = f++;
        if (*f == '%') {
            ob_put(o, "%", 1);
            f++;
            continue;
        }
        char spec[64];
        size_t sl = 0;
        spec[sl++] = '%';
        while (*f && strchr("-+ #0'", *f) && sl < 40) spec[sl++] = *f++;
        if (*f == '*') {
            sl += (size_t)snprintf(spec + sl, 16, "%d", (i32)G32(va));
            va += 4;
            f++;
        } else {
            while (isdigit((unsigned char)*f) && sl < 48) spec[sl++] = *f++;
        }
        if (*f == '.') {
            spec[sl++] = *f++;
            if (*f == '*') {
                sl += (size_t)snprintf(spec + sl, 16, "%d", (i32)G32(va));
                va += 4;
                f++;
            } else {
                while (isdigit((unsigned char)*f) && sl < 58) spec[sl++] = *f++;
            }
        }
        int ll = 0, wide = 0, longdbl = 0;
        for (;;) {
            if (*f == 'l') {
                if (f[1] == 'l') {
                    ll = 1;
                    f += 2;
                } else {
                    wide = 1;
                    f++;
                }
            } else if (*f == 'q' || *f == 'j') {
                ll = 1;
                f++;
            } else if (*f == 'L') {
                longdbl = 1;
                f++;
            } else if (*f == 'h' || *f == 'z' || *f == 't') {
                f++;
            } else {
                break;
            }
        }
        char conv = *f ? *f++ : 0;
        char tmp[512];
        int n = 0;
        switch (conv) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
                if (ll) {
                    spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
                    n = snprintf(tmp, sizeof tmp, spec, *(u64a *)(rt_mem + va));
                    va += 8;
                } else {
                    spec[sl++] = conv; spec[sl] = 0;
                    n = snprintf(tmp, sizeof tmp, spec, G32(va));
                    va += 4;
                }
                ob_put(o, tmp, (size_t)n);
                break;
            case 'c':
                spec[sl++] = 'c'; spec[sl] = 0;
                n = snprintf(tmp, sizeof tmp, spec, (int)G32(va));
                va += 4;
                ob_put(o, tmp, (size_t)n);
                break;
            case 'p':
                // Width and zero-padding matter: the game's formatter parses fixed-width %018p output back.
                spec[sl++] = 'p'; spec[sl] = 0;
                n = snprintf(tmp, sizeof tmp, spec, (void *)(uintptr_t)G32(va));
                va += 4;
                ob_put(o, tmp, (size_t)n);
                break;
            case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': case 'a': case 'A': {
                double v;
                if (longdbl) {
                    v = rt_ld80(rt_mem + va);
                    va += 12;
                } else {
                    v = *(f64a *)(rt_mem + va);
                    va += 8;
                }
                spec[sl++] = conv; spec[sl] = 0;
                n = snprintf(tmp, sizeof tmp, spec, v);
                ob_put(o, tmp, (size_t)n);
                break;
            }
            case 's': case 'S': {
                u32 p = G32(va);
                va += 4;
                if (!p) {
                    ob_put(o, "(null)", 6);
                } else if (wide || conv == 'S') {
                    const i32 *w = (const i32 *)(rt_mem + p);
                    while (*w) {
                        char ch = (*w < 128) ? (char)*w : '?';
                        ob_put(o, &ch, 1);
                        w++;
                    }
                } else {
                    spec[sl++] = 's'; spec[sl] = 0;
                    const char *s = GS(p);
                    size_t need = strlen(s) + 64;
                    char *big = malloc(need);
                    n = snprintf(big, need, spec, s);
                    ob_put(o, big, (size_t)n);
                    free(big);
                }
                break;
            }
            case '@': {
                // Objective-C object: its description, from the bridge.
                char *d = rt_objc_describe(G32(va));
                va += 4;
                ob_put(o, d, strlen(d));
                free(d);
                break;
            }
            case 'n':
                G32(G32(va)) = (u32)o->len;
                va += 4;
                break;
            default:
                ob_put(o, start, (size_t)(f - start));
                break;
        }
    }
}

// For the bridge: a malloc'ed string formatted from guest varargs.
char *rt_format_guest(const char *fmt, u32 va) {
    OutBuf o = {0};
    gformat(&o, fmt, va);
    return o.buf;
}

static u32 fmt_to_buffer(u32 dst, u32 cap, const char *fmt, u32 va) {
    OutBuf o = {0};
    gformat(&o, fmt, va);
    if (cap) {
        size_t n = o.len < cap - 1 ? o.len : cap - 1;
        memcpy(rt_mem + dst, o.buf, n);
        *(rt_mem + dst + n) = 0;
    }
    u32 r = (u32)o.len;
    free(o.buf);
    return r;
}

static u32 fmt_to_file(u32 fh, const char *fmt, u32 va) {
    OutBuf o = {0};
    gformat(&o, fmt, va);
    FILE *f = FILEP(fh);
    if (f) fwrite(o.buf, 1, o.len, f);
    u32 r = (u32)o.len;
    free(o.buf);
    return r;
}

#define VAP(n) (c->esp + 4u + (u32)(n) * 4u)
SHIM(printf) { RET(fmt_to_file(G32(rt_import_addr("___stdoutp")), GS(ARG(0)), VAP(1))); }
SHIM(fprintf) { RET(fmt_to_file(ARG(0), GS(ARG(1)), VAP(2))); }
SHIM(vfprintf) { RET(fmt_to_file(ARG(0), GS(ARG(1)), ARG(2))); }
SHIM(sprintf) { RET(fmt_to_buffer(ARG(0), 0x7FFFFFFF, GS(ARG(1)), VAP(2))); }
SHIM(snprintf) { RET(fmt_to_buffer(ARG(0), ARG(1), GS(ARG(2)), VAP(3))); }
SHIM(vsprintf) { RET(fmt_to_buffer(ARG(0), 0x7FFFFFFF, GS(ARG(1)), ARG(2))); }
SHIM(vsnprintf) { RET(fmt_to_buffer(ARG(0), ARG(1), GS(ARG(2)), ARG(3))); }
SHIM(__sprintf_chk) { RET(fmt_to_buffer(ARG(0), 0x7FFFFFFF, GS(ARG(3)), VAP(4))); }
SHIM(__snprintf_chk) { RET(fmt_to_buffer(ARG(0), ARG(1), GS(ARG(4)), VAP(5))); }
SHIM(asprintf) {
    OutBuf o = {0};
    gformat(&o, GS(ARG(1)), VAP(2));
    u32 a = rt_guest_malloc((u32)o.len + 1);
    memcpy(rt_mem + a, o.buf, o.len + 1);
    G32(ARG(0)) = a;
    RET(o.len);
    free(o.buf);
}
SHIM(puts) { RET(puts(GS(ARG(0)))); }
SHIM(putchar) { RET(putchar((int)ARG(0))); }
SHIM(fputs) { FILE *f = FILEP(ARG(1)); RET(f ? fputs(GS(ARG(0)), f) : -1); }

// sscanf: rewrite the format for host "long" width and pass guest pointers translated to host ones.
static int gscanf(const char *s, const char *fmt, u32 va) {
    char f2[1024];
    size_t k = 0;
    void *p[16] = {0};
    int np = 0;
    for (const char *q = fmt; *q && k < sizeof f2 - 2; q++) {
        if (*q != '%') {
            f2[k++] = *q;
            continue;
        }
        f2[k++] = *q++;
        if (*q == '%') {
            f2[k++] = *q;
            continue;
        }
        int star = 0;
        if (*q == '*') {
            star = 1;
            f2[k++] = *q++;
        }
        while (isdigit((unsigned char)*q)) f2[k++] = *q++;
        int ell = 0;
        if (*q == 'l' && q[1] != 'l') {
            ell = 1;
            q++;
        } else if (*q == 'z' || *q == 't') {
            q++;
        }
        while (*q && strchr("hlqjL", *q)) f2[k++] = *q++;
        if (*q == '[') {
            while (*q && *q != ']') f2[k++] = *q++;
        }
        if (ell && strchr("feEgGaA", *q)) f2[k++] = 'l';
        f2[k++] = *q;
        if (!star && np < 16) {
            p[np++] = GP(G32(va));
            va += 4;
        }
    }
    f2[k] = 0;
    return sscanf(s, f2, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
}
SHIM(sscanf) { RET(gscanf(GS(ARG(0)), GS(ARG(1)), VAP(2))); }

// The same for wide strings. The engine parses bone names with this, so a stub silently loses cape and rig data.
static int gwscanf(const wchar_t *s, const wchar_t *fmt, u32 va) {
    wchar_t f2[1024];
    size_t k = 0;
    void *p[16] = {0};
    int np = 0;
    for (const wchar_t *q = fmt; *q && k < 1022; q++) {
        if (*q != L'%') {
            f2[k++] = *q;
            continue;
        }
        f2[k++] = *q++;
        if (*q == L'%') {
            f2[k++] = *q;
            continue;
        }
        int star = 0;
        if (*q == L'*') {
            star = 1;
            f2[k++] = *q++;
        }
        while (*q >= L'0' && *q <= L'9') f2[k++] = *q++;
        int ell = 0;
        if (*q == L'l' && q[1] != L'l') {
            ell = 1;
            q++;
        } else if (*q == L'z' || *q == L't') {
            q++;
        }
        while (*q && wcschr(L"hlqjL", *q)) f2[k++] = *q++;
        // A guest long is 32 bits, so "l" stays only where it changes the type on both sides.
        if (ell && wcschr(L"feEgGaAsc[", *q)) f2[k++] = L'l';
        if (*q == L'[') {
            while (*q && *q != L']') f2[k++] = *q++;
        }
        f2[k++] = *q;
        if (!star && np < 16) {
            p[np++] = GP(G32(va));
            va += 4;
        }
    }
    f2[k] = 0;
    return swscanf(s, f2, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
}
SHIM(vswscanf) { RET(gwscanf((const wchar_t *)GP(ARG(0)), (const wchar_t *)GP(ARG(1)), ARG(2))); }

// ---------- number parsing ----------
static void set_endp(u32 endp, u32 gbase, const char *base, const char *e) {
    if (endp) G32(endp) = gbase + (u32)(e - base);
}
SHIM(atoi) { RET(atoi(GS(ARG(0)))); }
SHIM(atof) { RETF(atof(GS(ARG(0)))); }
SHIM(strtol) {
    const char *s = GS(ARG(0));
    char *e;
    long long v = strtoll(s, &e, (int)ARG(2));
    if (v > 2147483647LL) v = 2147483647LL;
    if (v < -2147483648LL) v = -2147483648LL;
    set_endp(ARG(1), ARG(0), s, e);
    RET((i32)v);
}
SHIM(strtoul) {
    const char *s = GS(ARG(0));
    char *e;
    unsigned long long v = strtoull(s, &e, (int)ARG(2));
    if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull;
    set_endp(ARG(1), ARG(0), s, e);
    RET((u32)v);
}
SHIM(strtoll) {
    const char *s = GS(ARG(0));
    char *e;
    long long v = strtoll(s, &e, (int)ARG(2));
    set_endp(ARG(1), ARG(0), s, e);
    RET64(v);
}
SHIM(strtoull) {
    const char *s = GS(ARG(0));
    char *e;
    unsigned long long v = strtoull(s, &e, (int)ARG(2));
    set_endp(ARG(1), ARG(0), s, e);
    RET64(v);
}
SHIM(strtod) {
    const char *s = GS(ARG(0));
    char *e;
    double v = strtod(s, &e);
    set_endp(ARG(1), ARG(0), s, e);
    RETF(v);
}
SHIM(strtof) {
    const char *s = GS(ARG(0));
    char *e;
    float v = strtof(s, &e);
    set_endp(ARG(1), ARG(0), s, e);
    RETF(v);
}

static void set_wendp(u32 endp, u32 gbase, const wchar_t *base, const wchar_t *e) {
    if (endp) G32(endp) = gbase + (u32)((const char *)e - (const char *)base);
}
SHIM(wcstol) {
    const wchar_t *s = GP(ARG(0));
    wchar_t *e;
    long long v = wcstoll(s, &e, (int)ARG(2));
    if (v > 2147483647LL) v = 2147483647LL;
    if (v < -2147483648LL) v = -2147483648LL;
    set_wendp(ARG(1), ARG(0), s, e);
    RET((i32)v);
}
SHIM(wcstoul) {
    const wchar_t *s = GP(ARG(0));
    wchar_t *e;
    unsigned long long v = wcstoull(s, &e, (int)ARG(2));
    if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull;
    set_wendp(ARG(1), ARG(0), s, e);
    RET((u32)v);
}
SHIM(wcstod) {
    const wchar_t *s = GP(ARG(0));
    wchar_t *e;
    double v = wcstod(s, &e);
    set_wendp(ARG(1), ARG(0), s, e);
    RETF(v);
}
// NXArchInfo for the architecture the guest believes it runs on: {name, cputype, cpusubtype, byteorder, description}.
SHIM(NXGetLocalArchInfo) {
    static u32 info;
    if (!info) {
        info = rt_guest_calloc(1, 20);
        G32(info) = rt_guest_strdup("i386");
        G32(info + 4) = 7;
        G32(info + 8) = 3;
        G32(info + 12) = 1;
        G32(info + 16) = rt_guest_strdup("Intel 80x86");
    }
    RET(info);
}

// ---------- ctype ----------
SHIM(__tolower) { RET(tolower((int)ARG(0))); }
SHIM(__toupper) { RET(toupper((int)ARG(0))); }
SHIM(__maskrune) { RET((u32)__maskrune((int)ARG(0), ARG(1))); }
SHIM(btowc) { RET((i32)ARG(0)); }
SHIM(wctob) { RET((i32)ARG(0)); }

// ---------- stdio ----------
static int trace_fs_early(void) { return getenv("ARKHAM_TRACE_FS") != NULL; }
SHIM(fopen) {
    FILE *f = fopen(GS(ARG(0)), GS(ARG(1)));
    if (!f) set_errno();
    if (trace_fs_early()) fprintf(stderr, "[fs] fopen %s %s -> %d\n", GS(ARG(0)), GS(ARG(1)), f != NULL);
    RET(rt_handle_new(f));
}
SHIM(fdopen) { RET(rt_handle_new(fdopen((int)ARG(0), GS(ARG(1))))); }
SHIM(fclose) {
    FILE *f = FILEP(ARG(0));
    RET(f ? fclose(f) : -1);
    rt_handle_free(ARG(0));
}
SHIM(fread) { FILE *f = FILEP(ARG(3)); RET(f ? fread(GP(ARG(0)), ARG(1), ARG(2), f) : 0); }
SHIM(fwrite) { FILE *f = FILEP(ARG(3)); RET(f ? fwrite(GS(ARG(0)), ARG(1), ARG(2), f) : 0); }
SHIM(fseek) { FILE *f = FILEP(ARG(0)); RET(f ? fseek(f, (i32)ARG(1), (int)ARG(2)) : -1); }
SHIM(fseeko) { FILE *f = FILEP(ARG(0)); RET(f ? fseeko(f, (off_t)ARGQ(1), (int)ARG(3)) : -1); }
SHIM(ftell) { FILE *f = FILEP(ARG(0)); RET(f ? ftell(f) : -1); }
SHIM(ftello) { FILE *f = FILEP(ARG(0)); i64 v = f ? ftello(f) : -1; RET64(v); }
SHIM(fflush) { FILE *f = FILEP(ARG(0)); RET(fflush(f)); }
SHIM(fgets) { FILE *f = FILEP(ARG(2)); char *r = f ? fgets(GP(ARG(0)), (int)ARG(1), f) : NULL; RET(r ? ARG(0) : 0); }
SHIM(feof) { FILE *f = FILEP(ARG(0)); RET(f ? feof(f) : 1); }
SHIM(ferror) { FILE *f = FILEP(ARG(0)); RET(f ? ferror(f) : 1); }
SHIM(fileno) { FILE *f = FILEP(ARG(0)); RET(f ? fileno(f) : -1); }
SHIM(getc) { FILE *f = FILEP(ARG(0)); RET(f ? getc(f) : -1); }
SHIM(ungetc) { FILE *f = FILEP(ARG(1)); RET(f ? ungetc((int)ARG(0), f) : -1); }
SHIM(rewind) { FILE *f = FILEP(ARG(0)); if (f) rewind(f); }
SHIM(setvbuf) { RET(0); }
SHIM(clearerr) { FILE *f = FILEP(ARG(0)); if (f) clearerr(f); }

// ---------- files ----------
static int trace_fs;
#define TFS(what, path, res) do { if (trace_fs) fprintf(stderr, "[fs] %s %s -> %d\n", what, path, (int)(res)); } while (0)
SHIM(open) {
    int fd = open(GS(ARG(0)), (int)ARG(1), (int)ARG(2));
    if (fd < 0) set_errno();
    TFS("open", GS(ARG(0)), fd);
    RET(fd);
}
SHIM(close) { RET(close((int)ARG(0))); }
SHIM(read) {
    ssize_t r = read((int)ARG(0), GP(ARG(1)), ARG(2));
    if (r < 0) set_errno();
    RET((i32)r);
}
SHIM(write) { ssize_t r = write((int)ARG(0), GS(ARG(1)), ARG(2)); if (r < 0) set_errno(); RET((i32)r); }
SHIM(pread) {
    ssize_t r = pread((int)ARG(0), GP(ARG(1)), ARG(2), (off_t)ARGQ(3));
    if (r < 0) set_errno();
    RET((i32)r);
}
SHIM(pwrite) { ssize_t r = pwrite((int)ARG(0), GS(ARG(1)), ARG(2), (off_t)ARGQ(3)); if (r < 0) set_errno(); RET((i32)r); }
SHIM(lseek) { off_t r = lseek((int)ARG(0), (off_t)ARGQ(1), (int)ARG(3)); if (r < 0) set_errno(); RET64(r); }
SHIM(access) { int r = access(GS(ARG(0)), (int)ARG(1)); if (r) set_errno(); TFS("access", GS(ARG(0)), r); RET(r); }
SHIM(mkdir) { int r = mkdir(GS(ARG(0)), (mode_t)ARG(1)); if (r) set_errno(); RET(r); }
SHIM(rmdir) { RET(rmdir(GS(ARG(0)))); }
SHIM(unlink) { RET(unlink(GS(ARG(0)))); }
SHIM(remove) { RET(remove(GS(ARG(0)))); }
SHIM(rename) { RET(rename(GS(ARG(0)), GS(ARG(1)))); }
SHIM(chmod) { RET(chmod(GS(ARG(0)), (mode_t)ARG(1))); }
SHIM(ftruncate) { RET(ftruncate((int)ARG(0), (off_t)ARGQ(1))); }
SHIM(truncate) { RET(truncate(GS(ARG(0)), (off_t)ARGQ(1))); }
SHIM(fsync) { RET(fsync((int)ARG(0))); }
SHIM(isatty) { RET(isatty((int)ARG(0))); }
SHIM(dup) { RET(dup((int)ARG(0))); }
SHIM(dup2) { RET(dup2((int)ARG(0), (int)ARG(1))); }
SHIM(fcntl) { RET(fcntl((int)ARG(0), (int)ARG(1), (int)ARG(2))); }
SHIM(umask) { RET(umask((mode_t)ARG(0))); }
SHIM(readlink) { RET((i32)readlink(GS(ARG(0)), GP(ARG(1)), ARG(2))); }
SHIM(realpath) {
    char tmp[PATH_MAX];
    char *r = realpath(GS(ARG(0)), tmp);
    if (!r) {
        set_errno();
        RET(0);
        return;
    }
    u32 dst = ARG(1) ? ARG(1) : rt_guest_malloc(PATH_MAX);
    strcpy(GP(dst), tmp);
    RET(dst);
}

// 32-bit stat with 64-bit inodes (stat$INODE64 / stat64): timespecs are two 32-bit fields.
static void put_stat64(u32 g, const struct stat *s) {
    G32(g + 0) = (u32)s->st_dev;
    G16(g + 4) = s->st_mode;
    G16(g + 6) = s->st_nlink;
    *(u64a *)(rt_mem + g + 8) = s->st_ino;
    G32(g + 16) = s->st_uid;
    G32(g + 20) = s->st_gid;
    G32(g + 24) = (u32)s->st_rdev;
    G32(g + 28) = (u32)s->st_atimespec.tv_sec;
    G32(g + 32) = (u32)s->st_atimespec.tv_nsec;
    G32(g + 36) = (u32)s->st_mtimespec.tv_sec;
    G32(g + 40) = (u32)s->st_mtimespec.tv_nsec;
    G32(g + 44) = (u32)s->st_ctimespec.tv_sec;
    G32(g + 48) = (u32)s->st_ctimespec.tv_nsec;
    G32(g + 52) = (u32)s->st_birthtimespec.tv_sec;
    G32(g + 56) = (u32)s->st_birthtimespec.tv_nsec;
    *(i64 *)(rt_mem + g + 60) = s->st_size;
    *(i64 *)(rt_mem + g + 68) = s->st_blocks;
    G32(g + 76) = (u32)s->st_blksize;
    G32(g + 80) = s->st_flags;
    G32(g + 84) = s->st_gen;
}
// Legacy 32-bit stat with 32-bit inodes.
static void put_stat32(u32 g, const struct stat *s) {
    G32(g + 0) = (u32)s->st_dev;
    G32(g + 4) = (u32)s->st_ino;
    G16(g + 8) = s->st_mode;
    G16(g + 10) = s->st_nlink;
    G32(g + 12) = s->st_uid;
    G32(g + 16) = s->st_gid;
    G32(g + 20) = (u32)s->st_rdev;
    G32(g + 24) = (u32)s->st_atimespec.tv_sec;
    G32(g + 28) = (u32)s->st_atimespec.tv_nsec;
    G32(g + 32) = (u32)s->st_mtimespec.tv_sec;
    G32(g + 36) = (u32)s->st_mtimespec.tv_nsec;
    G32(g + 40) = (u32)s->st_ctimespec.tv_sec;
    G32(g + 44) = (u32)s->st_ctimespec.tv_nsec;
    *(i64 *)(rt_mem + g + 48) = s->st_size;
    *(i64 *)(rt_mem + g + 56) = s->st_blocks;
    G32(g + 64) = (u32)s->st_blksize;
    G32(g + 68) = s->st_flags;
    G32(g + 72) = s->st_gen;
}
SHIM(stat64) { struct stat s; int r = stat(GS(ARG(0)), &s); if (r) set_errno(); else put_stat64(ARG(1), &s); TFS("stat", GS(ARG(0)), r); RET(r); }
SHIM(lstat64) { struct stat s; int r = lstat(GS(ARG(0)), &s); if (r) set_errno(); else put_stat64(ARG(1), &s); RET(r); }
SHIM(fstat64) { struct stat s; int r = fstat((int)ARG(0), &s); if (r) set_errno(); else put_stat64(ARG(1), &s); RET(r); }
SHIM(stat32) { struct stat s; int r = stat(GS(ARG(0)), &s); if (r) set_errno(); else put_stat32(ARG(1), &s); TFS("stat", GS(ARG(0)), r); RET(r); }
SHIM(lstat32) { struct stat s; int r = lstat(GS(ARG(0)), &s); if (r) set_errno(); else put_stat32(ARG(1), &s); RET(r); }
SHIM(fstat32) { struct stat s; int r = fstat((int)ARG(0), &s); if (r) set_errno(); else put_stat32(ARG(1), &s); RET(r); }

// Directories: guest dirent with 64-bit inodes (d_ino 8, d_seekoff 8, d_reclen 2, d_namlen 2, d_type 1, d_name 1024).
SHIM(opendir) { DIR *d = opendir(GS(ARG(0))); TFS("opendir", GS(ARG(0)), d != NULL); RET(rt_handle_new(d)); }
SHIM(closedir) { DIR *d = rt_handle_get(ARG(0)); RET(d ? closedir(d) : -1); rt_handle_free(ARG(0)); }
static u32 dirent_buf;
static void put_dirent(u32 g, const struct dirent *e) {
    *(u64a *)(rt_mem + g) = e->d_ino;
    *(u64a *)(rt_mem + g + 8) = e->d_seekoff;
    G16(g + 16) = e->d_reclen;
    G16(g + 18) = e->d_namlen;
    G8(g + 20) = e->d_type;
    strcpy(GP(g + 21), e->d_name);
}
// Plain readdir uses the legacy layout: d_ino u32, d_reclen u16, d_type u8, d_namlen u8, d_name[256].
SHIM(readdir) {
    DIR *d = rt_handle_get(ARG(0));
    struct dirent *e = d ? readdir(d) : NULL;
    if (!e) {
        RET(0);
        return;
    }
    if (!dirent_buf) dirent_buf = rt_guest_malloc(1100);
    u32 g = dirent_buf;
    G32(g) = (u32)e->d_ino;
    G16(g + 4) = (u16)(8 + e->d_namlen + 1);
    G8(g + 6) = e->d_type;
    G8(g + 7) = (u8)e->d_namlen;
    strncpy(GP(g + 8), e->d_name, 255);
    G8(g + 8 + 255) = 0;
    RET(g);
}
SHIM(readdir_r) {
    DIR *d = rt_handle_get(ARG(0));
    struct dirent *e = d ? readdir(d) : NULL;
    if (e) {
        put_dirent(ARG(1), e);
        G32(ARG(2)) = ARG(1);
    } else {
        G32(ARG(2)) = 0;
    }
    RET(0);
}

// ---------- memory mapping inside the guest window ----------
SHIM(mmap) {
    u32 len = ARG(1);
    u32 flags = ARG(3);
    int fd = (int)ARG(4);
    i64 off = (i64)ARGQ(5);
    u32 a = rt_guest_alloc_pages(len);
    if (!(flags & MAP_ANON) && fd >= 0) {
        ssize_t r = pread(fd, rt_mem + a, len, off);
        if (r < 0) {
            set_errno();
            RET(0xFFFFFFFFu);
            return;
        }
    }
    RET(a);
}
SHIM(munmap) { rt_guest_free_pages(ARG(0), ARG(1)); RET(0); }
SHIM(mprotect) { RET(0); }
SHIM(madvise) { RET(0); }
SHIM(msync) { RET(0); }
SHIM(sbrk) { RET(rt_guest_alloc_pages(ARG(0) ? ARG(0) : 4096)); }
SHIM(getpagesize) { RET(4096); }

// ---------- environment and process ----------
SHIM(getenv) {
    const char *v = getenv(GS(ARG(0)));
    RET(v ? rt_guest_strdup(v) : 0);
}
SHIM(unsetenv) { RET(0); }
SHIM(getpid) { RET(getpid()); }
SHIM(getppid) { RET(getppid()); }
SHIM(getuid) { RET(getuid()); }
SHIM(getgid) { RET(getgid()); }
SHIM(abort) {
    fprintf(stderr, "\n*** guest called abort()\n");
    rt_trap(c, G32(c->esp), "abort");
}
SHIM(_Exit) { _exit((int)ARG(0)); }
SHIM(exit) {
    rt_glq_drain();
    fprintf(stderr, "[rt] guest exit(%d)\n", (int)ARG(0));
    void *frames[48];
    int n = backtrace(frames, 48);
    backtrace_symbols_fd(frames, n, 2);
    fflush(NULL);
    _exit((int)ARG(0));
}
SHIM(__stack_chk_fail) { rt_trap(c, G32(c->esp), "stack smashing detected"); }
SHIM(raise) { fprintf(stderr, "[rt] raise(%d)\n", (int)ARG(0)); RET(0); }
SHIM(signal) { RET(0); }
SHIM(sigaction) { RET(0); }
SHIM(sigaltstack) { RET(0); }
SHIM(sigprocmask) { RET(0); }
SHIM(setrlimit) { RET(0); }
SHIM(atexit) { RET(0); }
SHIM(__cxa_atexit) { RET(0); }
SHIM(getprogname) { static u32 s; if (!s) s = rt_guest_strdup("Batman Arkham Asylum GOTY"); RET(s); }
SHIM(setlocale) { static u32 s; if (!s) s = rt_guest_strdup("C"); RET(s); }

extern int rt_main_argc;
SHIM(_NSGetExecutablePath) {
    const char *path = getenv("ARKHAM_BINARY");
    char buf[1200];
    if (!path) {
        snprintf(buf, sizeof buf,
                 "%s/Library/Application Support/Steam/steamapps/common/Batman Arkham Asylum GOTY/"
                 "Batman Arkham Asylum GOTY.app/Contents/MacOS/Batman Arkham Asylum GOTY",
                 getenv("HOME"));
        path = buf;
    }
    size_t n = strlen(path) + 1;
    u32 cap = G32(ARG(1));
    G32(ARG(1)) = (u32)n;
    if (n > cap) {
        RET(-1);
        return;
    }
    memcpy(GP(ARG(0)), path, n);
    RET(0);
}
SHIM(sysctlbyname) {
    const char *name = GS(ARG(0));
    u32 outp = ARG(1), lenp = ARG(2);
    if (getenv("ARKHAM_TRACE_FS")) fprintf(stderr, "[sysctl] %s\n", name);
    u32 cap = lenp ? G32(lenp) : 0;
    // Every CPU count answers 4: guest threads take turns anyway, so a big pool would only add switching.
    if (!strncmp(name, "hw.", 3) && strstr(name, "cpu") && !strstr(name, "freq") && !strstr(name, "type") && !strstr(name, "family") && !strstr(name, "brand")) {
        if (outp && cap >= 4) G32(outp) = 4;
        if (lenp) G32(lenp) = 4;
        RET(0);
        return;
    }
    if (!strcmp(name, "hw.cpufrequency") || !strcmp(name, "hw.cpufrequency_max")) {
        if (outp && cap >= 4) G32(outp) = 2400000000u;
        if (lenp) G32(lenp) = 4;
        RET(0);
        return;
    }
    if (!strcmp(name, "hw.memsize") && outp && cap >= 8) {
        *(u64a *)GP(outp) = 8ull << 30;
        G32(lenp) = 8;
        RET(0);
        return;
    }
    char buf[4096];
    size_t len = sizeof buf;
    int r = sysctlbyname(name, buf, &len, NULL, 0);
    if (r) {
        set_errno();
        RET(-1);
        return;
    }
    if (outp) memcpy(GP(outp), buf, len < cap ? len : cap);
    if (lenp) G32(lenp) = (u32)(len < cap || !outp ? len : cap);
    RET(0);
}
SHIM(sysctl) {
    u32 *mib = GP(ARG(0));
    if (ARG(2) && ARG(3) && mib[0] == CTL_HW) {
        if (mib[1] == HW_NCPU || mib[1] == HW_AVAILCPU) {
            G32(ARG(2)) = 4;
            G32(ARG(3)) = 4;
            RET(0);
            return;
        }
        if (mib[1] == HW_MEMSIZE) {
            *(u64a *)GP(ARG(2)) = 8ull << 30;
            G32(ARG(3)) = 8;
            RET(0);
            return;
        }
        if (mib[1] == HW_PHYSMEM) {
            G32(ARG(2)) = 0x7FFFFFFFu;
            G32(ARG(3)) = 4;
            RET(0);
            return;
        }
    }
    RET(-1);
}

// Mach memory queries describe a 64-bit host, which means nothing to the guest: report no regions and no statistics.
SHIM(mach_invalid_address) { RET(1); /* KERN_INVALID_ADDRESS */ }
SHIM(mach_failure) { RET(5); /* KERN_FAILURE */ }

// ---------- time ----------
// The guest sees what an Intel Mac reports: a nanosecond clock and a 1/1 timebase. Code that assumes that (this game
// does) would otherwise run its clock 41.7 times too slowly on Apple Silicon's 24 MHz counter.
SHIM(mach_absolute_time) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    u64 t = mach_absolute_time();
    u64 ns = t * tb.numer / tb.denom;
    RET64(ns);
}
SHIM(mach_timebase_info) {
    G32(ARG(0)) = 1;
    G32(ARG(0) + 4) = 1;
    RET(0);
}
SHIM(gettimeofday) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    G32(ARG(0)) = (u32)tv.tv_sec;
    G32(ARG(0) + 4) = (u32)tv.tv_usec;
    RET(0);
}
SHIM(time) {
    u32 t = (u32)time(NULL);
    if (ARG(0)) G32(ARG(0)) = t;
    RET(t);
}
SHIM(clock) { RET((u32)clock()); }
SHIM(usleep) {
    u32 us = ARG(0);
    rt_gate_release();
    usleep(us);
    rt_gate_acquire();
    RET(0);
}
SHIM(sleep) {
    u32 s = ARG(0);
    rt_gate_release();
    sleep(s);
    rt_gate_acquire();
    RET(0);
}
SHIM(nanosleep) {
    struct timespec ts = {(time_t)G32(ARG(0)), (long)G32(ARG(0) + 4)};
    rt_gate_release();
    nanosleep(&ts, NULL);
    rt_gate_acquire();
    RET(0);
}
static void put_tm(u32 g, const struct tm *t) {
    G32(g + 0) = (u32)t->tm_sec; G32(g + 4) = (u32)t->tm_min; G32(g + 8) = (u32)t->tm_hour;
    G32(g + 12) = (u32)t->tm_mday; G32(g + 16) = (u32)t->tm_mon; G32(g + 20) = (u32)t->tm_year;
    G32(g + 24) = (u32)t->tm_wday; G32(g + 28) = (u32)t->tm_yday; G32(g + 32) = (u32)t->tm_isdst;
    G32(g + 36) = (u32)t->tm_gmtoff; G32(g + 40) = 0;
}
static void get_tm(u32 g, struct tm *t) {
    memset(t, 0, sizeof *t);
    t->tm_sec = (int)G32(g); t->tm_min = (int)G32(g + 4); t->tm_hour = (int)G32(g + 8);
    t->tm_mday = (int)G32(g + 12); t->tm_mon = (int)G32(g + 16); t->tm_year = (int)G32(g + 20);
    t->tm_wday = (int)G32(g + 24); t->tm_yday = (int)G32(g + 28); t->tm_isdst = (int)G32(g + 32);
}
static u32 tm_buf;
SHIM(localtime) {
    if (!tm_buf) tm_buf = rt_guest_malloc(64);
    time_t tt = (time_t)(i32)G32(ARG(0));
    struct tm t;
    localtime_r(&tt, &t);
    put_tm(tm_buf, &t);
    RET(tm_buf);
}
SHIM(localtime_r) {
    time_t tt = (time_t)(i32)G32(ARG(0));
    struct tm t;
    localtime_r(&tt, &t);
    put_tm(ARG(1), &t);
    RET(ARG(1));
}
SHIM(gmtime) {
    if (!tm_buf) tm_buf = rt_guest_malloc(64);
    time_t tt = (time_t)(i32)G32(ARG(0));
    struct tm t;
    gmtime_r(&tt, &t);
    put_tm(tm_buf, &t);
    RET(tm_buf);
}
SHIM(gmtime_r) {
    time_t tt = (time_t)(i32)G32(ARG(0));
    struct tm t;
    gmtime_r(&tt, &t);
    put_tm(ARG(1), &t);
    RET(ARG(1));
}
SHIM(mktime) {
    struct tm t;
    get_tm(ARG(0), &t);
    RET((u32)mktime(&t));
}
SHIM(strftime) {
    struct tm t;
    get_tm(ARG(3), &t);
    RET(strftime(GP(ARG(0)), ARG(1), GS(ARG(2)), &t));
}
SHIM(difftime) { RETF((double)(i32)ARG(0) - (double)(i32)ARG(1)); }

// ---------- 64-bit arithmetic helpers from libgcc ----------
SHIM(__divdi3) { i64 a = (i64)ARGQ(0), b = (i64)ARGQ(2); RET64(b ? a / b : 0); }
SHIM(__moddi3) { i64 a = (i64)ARGQ(0), b = (i64)ARGQ(2); RET64(b ? a % b : 0); }
SHIM(__udivdi3) { u64 a = ARGQ(0), b = ARGQ(2); RET64(b ? a / b : 0); }
SHIM(__umoddi3) { u64 a = ARGQ(0), b = ARGQ(2); RET64(b ? a % b : 0); }
SHIM(__fixunsdfdi) { RET64((u64)ARGD(0)); }
SHIM(__fixunssfdi) { RET64((u64)ARGF(0)); }

// ---------- math: floats and doubles come in on the stack, results go out on the x87 stack ----------
#define MF1(n) SHIM(n) { RETF(n(ARGF(0))); }
#define MD1(n) SHIM(n) { RETF(n(ARGD(0))); }
MD1(sin) MD1(cos) MD1(tan) MD1(asin) MD1(acos) MD1(atan) MD1(sinh) MD1(cosh) MD1(tanh) MD1(exp) MD1(log) MD1(log10)
MD1(floor) MD1(ceil) MD1(round) MD1(rint) MD1(exp2)
MF1(sinf) MF1(cosf) MF1(tanf) MF1(asinf) MF1(acosf) MF1(atanf) MF1(expf) MF1(logf) MF1(log10f)
MF1(floorf) MF1(ceilf) MF1(roundf) MF1(rintf)
SHIM(atan2) { RETF(atan2(ARGD(0), ARGD(2))); }
SHIM(atan2f) { RETF(atan2f(ARGF(0), ARGF(1))); }
SHIM(pow) { RETF(pow(ARGD(0), ARGD(2))); }
SHIM(powf) { RETF(powf(ARGF(0), ARGF(1))); }
SHIM(fmod) { RETF(fmod(ARGD(0), ARGD(2))); }
SHIM(fmodf) { RETF(fmodf(ARGF(0), ARGF(1))); }
SHIM(fmaxf) { RETF(fmaxf(ARGF(0), ARGF(1))); }
SHIM(fminf) { RETF(fminf(ARGF(0), ARGF(1))); }
SHIM(ldexp) { RETF(ldexp(ARGD(0), (int)ARG(2))); }
SHIM(ldexpf) { RETF(ldexpf(ARGF(0), (int)ARG(1))); }
SHIM(frexp) { int e; double v = frexp(ARGD(0), &e); G32(ARG(2)) = (u32)e; RETF(v); }
SHIM(frexpf) { int e; float v = frexpf(ARGF(0), &e); G32(ARG(1)) = (u32)e; RETF(v); }
SHIM(modf) { double ip; double v = modf(ARGD(0), &ip); *(f64a *)GP(ARG(2)) = ip; RETF(v); }
SHIM(__isnand) { RET(isnan(ARGD(0))); }
SHIM(__isnanf) { RET(isnan(ARGF(0))); }
SHIM(__isinfd) { RET(isinf(ARGD(0))); }
SHIM(__isinff) { RET(isinf(ARGF(0))); }
SHIM(__isfinited) { RET(isfinite(ARGD(0))); }
SHIM(__signbitf) { RET(signbit(ARGF(0))); }
SHIM(rand) { RET(rand()); }
SHIM(srand) { srand(ARG(0)); }

SHIM(gethostname) { RET(gethostname(GP(ARG(0)), ARG(1))); }
SHIM(strerror) { static u32 b; if (!b) b = rt_guest_malloc(256); snprintf(GP(b), 256, "%s", strerror((int)ARG(0))); RET(b); }

// Guest qsort/bsearch call back into guest comparators.
static u32 qs_base, qs_cmp;
static void qs_swap(u32 a, u32 b, u32 sz) {
    u8 tmp[256];
    while (sz) {
        u32 n = sz < 256 ? sz : 256;
        memcpy(tmp, rt_mem + a, n);
        memcpy(rt_mem + a, rt_mem + b, n);
        memcpy(rt_mem + b, tmp, n);
        a += n;
        b += n;
        sz -= n;
    }
}
SHIM(qsort) {
    // Heap sort: no recursion and no scratch outside guest memory.
    u32 base = ARG(0), n = ARG(1), sz = ARG(2), cmp = ARG(3);
    (void)qs_base; (void)qs_cmp;
    #define EL(i) (base + (i) * sz)
    #define LESS(i, j) ({ u32 a_[2] = {EL(i), EL(j)}; (i32)rt_call_guest(c, cmp, 2, a_) < 0; })
    for (u32 start = n / 2; start-- > 0;) {
        u32 root = start;
        for (;;) {
            u32 child = 2 * root + 1;
            if (child >= n) break;
            if (child + 1 < n && LESS(child, child + 1)) child++;
            if (!LESS(root, child)) break;
            qs_swap(EL(root), EL(child), sz);
            root = child;
        }
    }
    for (u32 end = n; end-- > 1;) {
        qs_swap(EL(0), EL(end), sz);
        u32 root = 0;
        for (;;) {
            u32 child = 2 * root + 1;
            if (child >= end) break;
            if (child + 1 < end && LESS(child, child + 1)) child++;
            if (!LESS(root, child)) break;
            qs_swap(EL(root), EL(child), sz);
            root = child;
        }
    }
}
SHIM(bsearch) {
    u32 key = ARG(0), base = ARG(1), n = ARG(2), sz = ARG(3), cmp = ARG(4);
    u32 lo = 0, hi = n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        u32 a[2] = {key, base + mid * sz};
        i32 r = (i32)rt_call_guest(c, cmp, 2, a);
        if (r == 0) {
            RET(base + mid * sz);
            return;
        }
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    RET(0);
}

// Dynamic loading: the game only asks for optional system functions; report them as unavailable.
u32 rt_steam_dlopen(const char *name);
u32 rt_steam_dlsym(u32 handle, const char *sym);
SHIM(dlopen) {
    if (getenv("ARKHAM_TRACE_DL")) fprintf(stderr, "[dlopen] %s\n", NS(ARG(0)) ? NS(ARG(0)) : "(self)");
    RET(rt_steam_dlopen(NS(ARG(0))));
}
SHIM(dlsym) {
    if (getenv("ARKHAM_TRACE_DL")) fprintf(stderr, "[dlsym] %s\n", GS(ARG(1)));
    RET(rt_steam_dlsym(ARG(0), GS(ARG(1))));
}
SHIM(dlclose) { RET(0); }
SHIM(dlerror) { static u32 s; if (!s) s = rt_guest_strdup("not available"); RET(s); }
SHIM(dladdr) { RET(0); }
SHIM(_dyld_image_count) { RET(0); }
SHIM(_dyld_get_image_name) { RET(0); }
SHIM(_dyld_get_image_header) { RET(0x1000); }
SHIM(_dyld_get_image_vmaddr_slide) { RET(0); }
SHIM(_dyld_register_func_for_add_image) {}
SHIM(_dyld_register_func_for_remove_image) {}

void rt_libc_init(CPU *c) {
    trace_fs = getenv("ARKHAM_TRACE_FS") != NULL;
    u32 s = rt_import_addr("___stderrp");
    if (s) G32(s) = rt_handle_new(stderr);
    s = rt_import_addr("___stdoutp");
    if (s) G32(s) = rt_handle_new(stdout);
    s = rt_import_addr("___stdinp");
    if (s) G32(s) = rt_handle_new(stdin);
    s = rt_import_addr("_mach_task_self_");
    if (s) G32(s) = 0x103;
    s = rt_import_addr("___stack_chk_guard");
    if (s) G32(s) = 0x00C0FFEEu;
}

const ShimReg rt_shims_libc[] = {
    R2("_vm_region_recurse_64", mach_invalid_address) R2("_vm_region", mach_invalid_address) R2("_host_statistics64", mach_failure)
    R2("_task_info", mach_failure) R2("_task_threads", mach_failure) R2("_thread_info", mach_failure)
    R(__error) R(wcstol) R(wcstoul) R(wcstod) R(NXGetLocalArchInfo) R(strlen) R(strnlen) R(strcmp) R(strncmp) R(strcasecmp) R(strncasecmp) R(strcoll) R(strcpy) R(strncpy) R(strcat) R(strncat)
    R(strlcpy) R(strlcat) R(__strcpy_chk) R(strchr) R(strrchr) R(strstr) R(strnstr) R(strpbrk) R(strspn) R(strcspn)
    R(memcpy) R(__memcpy_chk) R(memmove) R(memset) R(__bzero) R(memcmp) R(memchr) R(memset_pattern16)
    R(wcslen) R(wcscmp) R(wcsncmp) R(mbstowcs) R(malloc) R(calloc) R(realloc) R(free) R(malloc_size) R(posix_memalign)
    R(printf) R(fprintf) R(vfprintf) R(sprintf) R(snprintf) R(vsprintf) R(vsnprintf) R(__sprintf_chk) R(__snprintf_chk)
    R(asprintf) R(puts) R(putchar) R(fputs) R2("_fputs$UNIX2003", fputs) R(sscanf) R(vswscanf) R(atoi) R(atof) R(strtol) R(strtoul) R(strtoll)
    R(strtoull) R(strtod) R2("_strtod$UNIX2003", strtod) R2("_strtof$UNIX2003", strtof)
    R(__tolower) R(__toupper) R(__maskrune) R(btowc) R(wctob)
    R(fopen) R2("_fopen$UNIX2003", fopen) R(fdopen) R(fclose) R(fread) R(fwrite) R2("_fwrite$UNIX2003", fwrite) R(fseek) R(fseeko)
    R(ftell) R(ftello) R(fflush) R(fgets) R(feof) R(ferror) R(fileno) R(getc) R(ungetc) R(rewind) R(setvbuf) R(clearerr)
    R(open) R2("_open$UNIX2003", open) R(close) R2("_close$UNIX2003", close) R(read) R2("_read$UNIX2003", read) R(write) R(pread) R(pwrite) R(lseek) R(access) R(mkdir) R(rmdir)
    R(unlink) R(remove) R(rename) R(chmod) R(ftruncate) R(truncate) R(fsync) R(isatty) R(dup) R(dup2) R(fcntl) R(umask) R(readlink)
    R2("_realpath$DARWIN_EXTSN", realpath)
    R2("_stat$INODE64", stat64) R2("_stat64", stat64) R2("_lstat", lstat32) R2("_stat", stat32) R2("_fstat", fstat32) R2("_fstat64", fstat64)
    R(opendir) R2("_opendir$INODE64$UNIX2003", opendir) R(closedir) R2("_closedir$UNIX2003", closedir) R(readdir) R2("_readdir_r$INODE64", readdir_r)
    R(mmap) R(munmap) R2("_mprotect$UNIX2003", mprotect) R(madvise) R(msync) R(sbrk) R(getpagesize)
    R(getenv) R(unsetenv) R(getpid) R(getppid) R(getuid) R(getgid) R(abort) R(_Exit) R(exit) R(__stack_chk_fail) R(raise) R(signal)
    R(sigaction) R(sigaltstack) R(sigprocmask) R(setrlimit) R(atexit) R(__cxa_atexit) R(getprogname) R(setlocale) R(_NSGetExecutablePath)
    R(sysctlbyname) R(sysctl) R(mach_absolute_time) R(mach_timebase_info) R(gettimeofday) R(time) R(clock) R(usleep)
    R2("_usleep$UNIX2003", usleep) R(sleep) R(nanosleep) R(localtime) R(localtime_r) R(gmtime) R(gmtime_r) R(mktime) R(strftime) R(difftime)
    R(__divdi3) R(__moddi3) R(__udivdi3) R(__umoddi3) R(__fixunsdfdi) R(__fixunssfdi)
    R(exp2) R(sin) R(cos) R(tan) R(asin) R(acos) R(atan) R(sinh) R(cosh) R(tanh) R(exp) R(log) R(log10) R(floor) R(ceil) R(round) R(rint)
    R(sinf) R(cosf) R(tanf) R(asinf) R(acosf) R(atanf) R(expf) R(logf) R(log10f) R(floorf) R(ceilf) R(roundf) R(rintf)
    R(atan2) R(atan2f) R(pow) R(powf) R(fmod) R(fmodf) R(fmaxf) R(fminf) R(ldexp) R(ldexpf) R(frexp) R(frexpf) R(modf)
    R(__isnand) R(__isnanf) R(__isinfd) R(__isinff) R(__isfinited) R(__signbitf) R(rand) R(srand) R(gethostname) R(strerror)
    R(qsort) R(bsearch) R(dlopen) R(dlsym) R(dlclose) R(dlerror) R(dladdr)
    R(_dyld_image_count) R(_dyld_get_image_name) R(_dyld_get_image_header) R(_dyld_get_image_vmaddr_slide)
    R(_dyld_register_func_for_add_image) R(_dyld_register_func_for_remove_image)
    {NULL, NULL},
};
