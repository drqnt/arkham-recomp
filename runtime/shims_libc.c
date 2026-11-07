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
#include <os/lock.h>
#include <stdatomic.h>
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
// Where in the game each handle was made, for the report when they run out: the shim's return address.
static int cmp_u32(const void *a, const void *b) {
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}
static u32 handle_from[HANDLE_MAX];
u32 rt_handle_new(void *p) {
    if (!p) return 0;
    CPU *cpu = rt_current_cpu();
    u32 from = cpu ? G32(cpu->esp) : 0;
    pthread_mutex_lock(&handle_mu);
    for (u32 i = 0; i < HANDLE_MAX; i++) {
        u32 k = (handle_next + i) % HANDLE_MAX;
        if (k && !handles[k]) {
            handles[k] = p;
            handle_from[k] = from;
            handle_next = k + 1;
            pthread_mutex_unlock(&handle_mu);
            return HANDLE_BASE + k * 16;
        }
    }
    fprintf(stderr, "*** out of guest handles; the places in the game that made the most of them:\n");
    static u32 sorted[HANDLE_MAX];
    memcpy(sorted, handle_from, sizeof sorted);
    qsort(sorted, HANDLE_MAX, sizeof *sorted, cmp_u32);
    for (int top = 0; top < 10; top++) {
        u32 best = 0, best_n = 0, run = 0;
        for (u32 j = 0; j < HANDLE_MAX; j++) {
            run = j && sorted[j] == sorted[j - 1] ? run + 1 : 1;
            if (sorted[j] && run > best_n && (j + 1 == HANDLE_MAX || sorted[j + 1] != sorted[j])) best = sorted[j], best_n = run;
        }
        if (!best_n) break;
        fprintf(stderr, "***   %u from %08x\n", best_n, best);
        for (u32 j = 0; j < HANDLE_MAX; j++)
            if (sorted[j] == best) sorted[j] = 0;
        qsort(sorted, HANDLE_MAX, sizeof *sorted, cmp_u32);
    }
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
// ---------- case in paths ----------
// The Mac's disks ignore case in names and the phone's don't, and the game isn't consistent: it asks for BMAAData/Binaries and BMGame where
// the folders are called binaries and BmGame. On the phone a path that isn't there is looked for again ignoring case, and what that finds is
// remembered for the path as asked, checked against the folder it was found in, which changes whenever a name in it does. That matters: the
// engine asks about some 75,000 paths at startup, 41,000 of them localization files that aren't there in any case (Engine.RUS in BmGame's folder
// and the like), and looked for afresh one name at a time, that took about 10 s on the phone, and long again whenever new packages came in
// (docs/IOS_PORT.md, controls v7). On the Mac paths go through untouched, unless ARKHAM_CI_PATHS=1, for measuring this.
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
typedef struct { char *dir; struct timespec mtime; char **names; int n; } DirNames;
enum { DIR_CACHE = 512 };
static DirNames dir_cache[DIR_CACHE];
static int dir_cache_next;
static pthread_mutex_t dir_mu = PTHREAD_MUTEX_INITIALIZER;
static const DirNames *names_of(const char *dir) {
    struct stat st;
    if (stat(dir, &st) || !S_ISDIR(st.st_mode)) return NULL;
    for (int i = 0; i < DIR_CACHE; i++) {
        DirNames *d = &dir_cache[i];
        if (d->dir && !strcmp(d->dir, dir)) {
            if (d->mtime.tv_sec == st.st_mtimespec.tv_sec && d->mtime.tv_nsec == st.st_mtimespec.tv_nsec) return d;
            for (int k = 0; k < d->n; k++) free(d->names[k]);
            free(d->names);
            free(d->dir);
            memset(d, 0, sizeof *d);
        }
    }
    DirNames *d = &dir_cache[dir_cache_next];
    dir_cache_next = (dir_cache_next + 1) % DIR_CACHE;
    if (d->dir) {
        for (int k = 0; k < d->n; k++) free(d->names[k]);
        free(d->names);
        free(d->dir);
        memset(d, 0, sizeof *d);
    }
    DIR *h = opendir(dir);
    if (!h) return NULL;
    int cap = 64;
    d->names = malloc(sizeof(char *) * (size_t)cap);
    for (struct dirent *e; (e = readdir(h));) {
        if (d->n == cap) d->names = realloc(d->names, sizeof(char *) * (size_t)(cap *= 2));
        d->names[d->n++] = strdup(e->d_name);
    }
    closedir(h);
    d->dir = strdup(dir);
    d->mtime = st.st_mtimespec;
    return d;
}
// The path as it is on the disk, in buf, or the path itself when it is there already or nothing matches, looked for one name at a time: for a
// path relative to the working folder, and for all of them with ARKHAM_CI_NO_MEMO=1, the way it was before the lookups were remembered.
static const char *ci_walk(const char *path, char *buf, size_t cap) {
    struct stat st;
    if (!path || !*path || !lstat(path, &st)) return path;
    char cur[PATH_MAX];
    if (path[0] == '/') cur[0] = 0;
    else if (!getcwd(cur, sizeof cur)) return path;
    pthread_mutex_lock(&dir_mu);
    const char *p = path;
    int fixed = 0, lost = 0;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char comp[NAME_MAX + 1];
        if (len > NAME_MAX) {
            lost = 1;
            break;
        }
        memcpy(comp, p, len);
        comp[len] = 0;
        p += len;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            char *slash = strrchr(cur, '/');
            if (slash) *slash = 0;
            continue;
        }
        size_t cl = strlen(cur);
        if (cl + 1 + len >= sizeof cur) {
            lost = 1;
            break;
        }
        snprintf(cur + cl, sizeof cur - cl, "/%s", comp);
        if (!lost && lstat(cur, &st)) {
            cur[cl] = 0;
            const DirNames *d = names_of(cl ? cur : "/");
            const char *match = NULL;
            for (int k = 0; d && k < d->n && !match; k++)
                if (!strcasecmp(d->names[k], comp)) match = d->names[k];
            snprintf(cur + cl, sizeof cur - cl, "/%s", match ? match : comp);
            if (match) fixed = 1;
            else lost = 1;  // not there in any case: the rest stays as asked, for a file about to be made
        }
    }
    pthread_mutex_unlock(&dir_mu);
    if (!fixed || strlen(cur) >= cap) return path;
    memcpy(buf, cur, strlen(cur) + 1);
    return buf;
}
static int ci_on(void) {
#if TARGET_OS_IPHONE
    return 1;
#else
    static int on = -1;
    if (on < 0) on = getenv("ARKHAM_CI_PATHS") != NULL;
    return on;
#endif
}
typedef struct Memo {
    struct Memo *next;
    u64 hash;
    char *asked, *real;  // real is NULL when the path as asked is the answer
    char *check;         // the folder the answer was found in, or the deepest one there was
    struct timespec mtime;
    int exists;
    unsigned gen;        // ci_gen when that folder was last seen unchanged
    u64 checked_ms;      // and when
} Memo;
enum { MEMO_BUCKETS = 1 << 14, MEMO_MAX = 1 << 16 };
static Memo *memo[MEMO_BUCKETS];
static unsigned memo_n;
static u64 ci_lookups, ci_remembered, ci_found, ci_ns;
// The game making, renaming or removing a name, through the shims below: what's remembered is checked against its folder again before it's used.
// Otherwise that happens at most every 2 s, for names made some other way: one stat() for each folder, not one for each of thousands of lookups.
static _Atomic unsigned ci_gen;
static void ci_changed(void) { atomic_fetch_add(&ci_gen, 1); }
static u64 ci_now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }
static u64 path_hash(const char *s) {
    u64 h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (u8)*s) * 1099511628211ull;
    return h;
}
static void memo_put(const char *asked, u64 h, const char *real, int exists, const char *check, struct timespec mtime) {
    Memo *m = NULL;
    for (Memo *e = memo[h & (MEMO_BUCKETS - 1)]; e && !m; e = e->next)
        if (e->hash == h && !strcmp(e->asked, asked)) m = e;
    if (!m) {
        if (memo_n >= MEMO_MAX) {  // the game asking about this many different paths is unlikely: start over
            for (int i = 0; i < MEMO_BUCKETS; i++)
                for (Memo *e = memo[i], *nx; e; e = nx) {
                    nx = e->next;
                    free(e->asked), free(e->real), free(e->check), free(e);
                }
            memset(memo, 0, sizeof memo);
            memo_n = 0;
        }
        m = calloc(1, sizeof *m);
        m->hash = h;
        m->asked = strdup(asked);
        m->next = memo[h & (MEMO_BUCKETS - 1)];
        memo[h & (MEMO_BUCKETS - 1)] = m;
        memo_n++;
    } else {
        free(m->real);
        free(m->check);
    }
    m->real = strcmp(real, asked) ? strdup(real) : NULL;
    m->exists = exists;
    m->check = strdup(check);
    m->mtime = mtime;
    m->gen = atomic_load(&ci_gen);
    m->checked_ms = ci_now_ms();
}
// An absolute path as it is on the disk, into real (PATH_MAX), and whether it's there; with dir_mu held. Through what's remembered for it, if its
// folder hasn't changed since; else its folder the same way, and then its last name in that folder, as asked or in any case.
static int resolve(const char *path, char *real, char *check, struct timespec *mtime) {
    struct stat st;
    u64 h = path_hash(path);
    for (Memo *m = memo[h & (MEMO_BUCKETS - 1)]; m; m = m->next) {
        if (m->hash != h || strcmp(m->asked, path)) continue;
        unsigned g = atomic_load(&ci_gen);
        u64 now = ci_now_ms();
        if (m->gen != g || now - m->checked_ms > 2000) {
            if (stat(m->check, &st) || st.st_mtimespec.tv_sec != m->mtime.tv_sec || st.st_mtimespec.tv_nsec != m->mtime.tv_nsec) break;
            m->gen = g;
            m->checked_ms = now;
        }
        snprintf(real, PATH_MAX, "%s", m->real ? m->real : path);
        snprintf(check, PATH_MAX, "%s", m->check);
        *mtime = m->mtime;
        ci_remembered++;
        return m->exists;
    }
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    if (!lstat(path, &st)) {
        snprintf(real, PATH_MAX, "%.*s", (int)n, path);
        check[0] = 0;
        return 1;
    }
    size_t cut = n;
    while (cut > 0 && path[cut - 1] != '/') cut--;  // path[cut..n) is the last name
    char parent[PATH_MAX], name[NAME_MAX + 1], rp[PATH_MAX];
    if (n - cut > NAME_MAX || cut >= sizeof parent) {
        snprintf(real, PATH_MAX, "%s", path);
        check[0] = 0;
        return 0;
    }
    size_t pl = cut > 1 ? cut - 1 : 1;  // the parent without its last slash; "/" for the root
    memcpy(parent, path, pl);
    parent[pl] = 0;
    memcpy(name, path + cut, n - cut);
    name[n - cut] = 0;
    int pexists = resolve(parent, rp, check, mtime);
    int exists = 0;
    const char *sep = strcmp(rp, "/") ? "/" : "";
    if (!pexists) {
        snprintf(real, PATH_MAX, "%s%s%s", rp, sep, name);  // check and mtime stay the parent's: the deepest folder there was
    } else if (!strcmp(name, ".") || !strcmp(name, "..")) {
        snprintf(real, PATH_MAX, "%s%s%s", rp, sep, name);
        exists = !lstat(real, &st);
        check[0] = 0;
    } else {
        snprintf(real, PATH_MAX, "%s%s%s", rp, sep, name);
        exists = strcmp(rp, parent) && !lstat(real, &st);  // the folder was found in another case, the name as asked
        if (!exists) {
            const DirNames *d = names_of(rp);
            for (int k = 0; d && k < d->n && !exists; k++)
                if (!strcasecmp(d->names[k], name)) {
                    snprintf(real, PATH_MAX, "%s%s%s", rp, sep, d->names[k]);
                    exists = 1;
                }
        }
        snprintf(check, PATH_MAX, "%s", rp);
        if (!stat(rp, &st)) *mtime = st.st_mtimespec;
    }
    if (check[0]) memo_put(path, h, real, exists, check, *mtime);
    return exists;
}
static void ci_report(void) {
    static u64 last_ns, told_lookups;
    u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - last_ns < 10000000000ull || ci_lookups == told_lookups) return;
    last_ns = now;
    told_lookups = ci_lookups;
    fprintf(stderr, "[ci] %llu paths not there as asked, %llu answered from memory, %llu found in another case, %.0f ms looking, %u remembered\n",
            ci_lookups, ci_remembered, ci_found, (double)ci_ns / 1e6, memo_n);
}
// The path as it is on the disk, in buf, or the path itself when it is there already or nothing matches.
static const char *ci_path(const char *path, char *buf, size_t cap) {
    if (!path || !*path || !ci_on()) return path;
    static int old = -1;
    if (old < 0) old = getenv("ARKHAM_CI_NO_MEMO") != NULL;
    if (path[0] != '/' || old) {
        struct stat st;
        if (!lstat(path, &st)) return path;
        u64 t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        const char *r = ci_walk(path, buf, cap);
        pthread_mutex_lock(&dir_mu);
        ci_lookups++;
        ci_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
        ci_report();
        pthread_mutex_unlock(&dir_mu);
        return r;
    }
    if (strlen(path) >= PATH_MAX) return path;
    char real[PATH_MAX], check[PATH_MAX];
    struct timespec mtime = {0, 0};
    u64 t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&dir_mu);
    u64 remembered = ci_remembered;
    int exists = resolve(path, real, check, &mtime);
    if (!exists || strcmp(real, path) || ci_remembered != remembered) {  // not there as asked
        ci_lookups++;
        if (exists && strcmp(real, path)) ci_found++;
        ci_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
        ci_report();
    }
    pthread_mutex_unlock(&dir_mu);
    if (!strcmp(real, path) || strlen(real) >= cap) return path;
    memcpy(buf, real, strlen(real) + 1);
    return buf;
}
// For the other files that take paths from the game (shims_cf.c's file manager on the phone).
const char *rt_ci_path(const char *path, char *buf, size_t cap) { return ci_path(path, buf, cap); }
#define CIP(n) ci_path(GS(ARG(n)), ci_buf##n, sizeof ci_buf##n)
#define CIBUF(n) char ci_buf##n[PATH_MAX]

static int trace_fs_early(void) { return getenv("ARKHAM_TRACE_FS") != NULL; }
// BmEngine.ini as the engine reads it, with lines changed and the file left as it is:
// - ARKHAM_LANGUAGE=ru (or fr, de, it, es, pl, en): the Language line. The Mac game has all of these installed, voice and text, and Feral's code sets
//   that line from Steam's language at every start, which for the Mac version is only ever English. Feral's own windows stay in English, all they come in.
// - bSmoothFrameRate, the engine's own frame limit at MaxSmoothedFrameRate (62), off unless ARKHAM_FRAME_LIMIT=1 (rt_no_frame_limit); a first start
//   on a new machine writes it on. ARKHAM_FPS_CAP is the runtime's own limit.
extern int rt_no_frame_limit;
// The input settings as the engine reads them with open() (BaseInput.ini, DefaultInput.ini, BmInput.ini, UserInput.ini), changed in the text it reads
// and kept on disk as they are. On the phone, where the mouse is the touch camera (rt_touch_mouse, shims_sys.c):
// - MouseSensitivity at a quarter of the game's 30. The pointer moves in whole pixels, and four times as many of them for the same turn let a slow
//   swipe turn smoothly rather than a quarter of a degree at a time (TouchControls.m sends four times as many).
// - The engine's mouse smoothing off. With one sample a frame, at 60 frames a second it doubles the motion of a frame a little shorter than 1/60 s
//   and not that of one a little longer, so the same swipe turned by different amounts.
// - The right stick's lines as the game has them, where controls v5 and v6 of this port (docs/IOS_PORT.md) had put them on the mouse's axes and
//   the engine had written that back into BmInput.ini.
// ARKHAM_TOUCH_MOUSE_INI=1 does the same on the Mac, for scripted runs, and then the engine's writes of those files go nowhere, so that the Mac's
// game keeps its own settings.
// The text keeps its length, since the engine may take the size from stat(), and values are set, not multiplied, since the engine writes what it read
// back into BmInput.ini, which is then read again at the next start.
static int input_rewrites(void) {
#if TARGET_OS_IPHONE
    return 1;
#else
    static int on = -1;
    if (on < 0) on = getenv("ARKHAM_TOUCH_MOUSE_INI") != NULL;
    return on;
#endif
}
// A value after "name=" in one command, overwritten in place, padded with spaces; 1 if that changed the text, 0 if not or if it doesn't fit.
static int set_field(char *cmd, char *cmd_end, const char *name, const char *value) {
    size_t nl = strlen(name), vl = strlen(value);
    char *at = memmem(cmd, (size_t)(cmd_end - cmd), name, nl);
    if (!at) return 0;
    char *v = at + nl, *ve = v;
    while (ve < cmd_end && *ve != ' ' && *ve != '|' && *ve != '"' && *ve != ')' && *ve != '\r') ve++;
    size_t room = (size_t)(ve - v);
    if (room < vl) return 0;
    char was[64];
    if (room > sizeof was) return 0;
    memcpy(was, v, room);
    memset(v, ' ', room);
    memcpy(v, value, vl);
    return memcmp(was, v, room) != 0;
}
// A whole command replaced by another no longer than it, padded with spaces to its length; 1 if that changed the text.
static int set_command(char *cmd, char *cmd_end, const char *text) {
    size_t room = (size_t)(cmd_end - cmd), n = strlen(text);
    if (n > room || room > 256) return 0;
    char was[256];
    memcpy(was, cmd, room);
    memset(cmd, ' ', room);
    memcpy(cmd, text, n);
    return memcmp(was, cmd, room) != 0;
}
// In a line of the right stick's, what controls v5 and v6 left: "Axis aMouseX Speed=60 DeadZone=0" (and aMouseY) where the game has aTurn at 1.0
// and aLookup at 0.8, or at 1.0 where the line also has the raw axis, both with a dead zone of 0.2; and the raw axes at another speed than 1.0.
static int right_stick_restore(char *line, char *line_end) {
    int changed = 0, raw = memmem(line, (size_t)(line_end - line), "Axis aRawRHJoy", 14) != NULL;
    for (char *q = line; q < line_end; q++) {
        size_t left = (size_t)(line_end - q);
        int mx = left > 13 && !strncmp(q, "Axis aMouseX ", 13), my = left > 13 && !strncmp(q, "Axis aMouseY ", 13);
        int rr = left > 14 && !strncmp(q, "Axis aRawRHJoy", 14);
        if (!mx && !my && !rr) continue;
        char *cmd_end = q;
        while (cmd_end < line_end && *cmd_end != '|' && *cmd_end != '"') cmd_end++;
        char *sp = memmem(q, (size_t)(cmd_end - q), "Speed=", 6);
        int neg = sp && sp[6] == '-';
        if (mx || my) {
            char cmd[64];
            snprintf(cmd, sizeof cmd, "Axis %s Speed=%s%s DeadZone=0.2", mx ? "aTurn" : "aLookup", neg ? "-" : "", mx || raw ? "1.0" : "0.8");
            changed += set_command(q, cmd_end, cmd);
        } else {
            changed += set_field(q, cmd_end, "Speed=", neg ? "-1.0" : "1.0");
        }
        q = cmd_end;
    }
    return changed;
}
static int input_rewrite(char *text, size_t len) {
    int changed = 0;
    char *end_all = text + len;
    static const char sensitivity[] = "MouseSensitivity=", smoothing[] = "bEnableMouseSmoothing=";
    for (char *line = text; line < end_all;) {
        char *nl = memchr(line, '\n', (size_t)(end_all - line));
        char *line_end = nl ? nl : end_all;
        size_t n = (size_t)(line_end - line);
        if (n > sizeof sensitivity - 1 && !strncmp(line, sensitivity, sizeof sensitivity - 1)) changed += set_field(line, line_end, sensitivity, "7.5");
        else if (n > sizeof smoothing - 1 && !strncmp(line, smoothing, sizeof smoothing - 1)) changed += set_field(line, line_end, smoothing, "0");
        else if (memmem(line, n, "XboxTypeS_RightX", 16) || memmem(line, n, "XboxTypeS_RightY", 16)) changed += right_stick_restore(line, line_end);
        line = line_end + 1;
    }
    return changed;
}
static int is_input_ini(const char *path) {
    size_t n = strlen(path);
    return n > 9 && !strcmp(path + n - 9, "Input.ini");
}
// The rewritten text in a file of its own, already unlinked, for open() to hand the engine; -1 for any other file, or one with nothing to change.
static int input_ini_file(const char *path) {
    if (!is_input_ini(path)) return -1;
    int in = open(path, O_RDONLY);
    if (in < 0) return -1;
    struct stat st;
    if (fstat(in, &st) || st.st_size <= 0 || st.st_size > (16 << 20)) {
        close(in);
        return -1;
    }
    size_t size = (size_t)st.st_size, got = 0;
    char *text = malloc(size);
    while (got < size) {
        ssize_t r = read(in, text + got, size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(in);
    int changed = got == size ? input_rewrite(text, size) : 0;
    int fd = -1;
    if (changed) {
        const char *dir = getenv("TMPDIR");
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s/arkham-input-XXXXXX", dir && *dir ? dir : "/tmp");
        fd = mkstemp(tmp);
        if (fd >= 0) {
            unlink(tmp);
            if (write(fd, text, size) != (ssize_t)size || lseek(fd, 0, SEEK_SET) < 0) {
                close(fd);
                fd = -1;
            }
        }
        static int told;
        if (fd >= 0 && told++ < 8)
            fprintf(stderr, "[input] the engine reads %s with %d settings changed for the touch camera\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, changed);
    }
    free(text);
    return fd;
}
static FILE *language_ini(const char *path, const char *mode) {
    static const char *const codes[][2] = {{"en", "int"}, {"fr", "fra"}, {"de", "deu"}, {"it", "ita"}, {"es", "esn"}, {"pl", "pol"}, {"ru", "rus"}};
    static const char tail[] = "/BmEngine.ini";
    const char *lang = getenv("ARKHAM_LANGUAGE"), *ue = NULL;
    size_t n = strlen(path), t = sizeof tail - 1;
    if (mode[0] != 'r' || n < t || strcmp(path + n - t, tail)) return NULL;
    for (size_t i = 0; lang && i < sizeof codes / sizeof codes[0]; i++)
        if (!strcmp(lang, codes[i][0])) ue = codes[i][1];
    if (!ue && !rt_no_frame_limit) return NULL;
    FILE *in = fopen(path, "rb");
    if (!in) return NULL;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    char *text = malloc((size_t)size + 64), *out = malloc((size_t)size + 64);
    size_t got = fread(text, 1, (size_t)size, in);
    fclose(in);
    text[got] = 0;
    size_t o = 0;
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        if (ue && !strncmp(line, "Language=", 9)) o += (size_t)sprintf(out + o, "Language=%s\n", ue);
        else if (rt_no_frame_limit && !strncmp(line, "bSmoothFrameRate=", 17)) o += (size_t)sprintf(out + o, "bSmoothFrameRate=FALSE\n");
        else {
            memcpy(out + o, line, len);
            o += len;
        }
        line += len;
    }
    free(text);
    if (ue) fprintf(stderr, "[lang] the engine reads Language=%s\n", ue);
    return fmemopen(out, o, "r");  // the buffer lives as long as the process: the engine reads this file a few times at start
}
SHIM(fopen) {
    FILE *f = language_ini(GS(ARG(0)), GS(ARG(1)));
#if !TARGET_OS_IPHONE
    // As in open() below: an experiment's input settings aren't written back into the Mac's game.
    if (!f && GS(ARG(1))[0] != 'r' && input_rewrites() && is_input_ini(GS(ARG(0)))) {
        f = fopen("/dev/null", GS(ARG(1)));
        fprintf(stderr, "[input] the engine's write of %s goes nowhere during this experiment\n", GS(ARG(0)));
    }
#endif
    CIBUF(0);
    if (!f) {
        f = fopen(CIP(0), GS(ARG(1)));
        if (f && GS(ARG(1))[0] != 'r') ci_changed();  // "w" and "a" make the file if it isn't there
    }
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
// With the place in the game that asked and the two callers above it, found through the frame pointers.
static void tfs_callers(CPU *c, u32 out[6]) {
    out[0] = G32(c->esp);
    u32 bp = c->ebp;
    for (int i = 1; i < 6; i++) {
        out[i] = bp > 0x1000 && bp < 0xFFFFFFF0u ? G32(bp + 4) : 0;
        bp = bp > 0x1000 && bp < 0xFFFFFFF0u ? G32(bp) : 0;
    }
}
#define TFS(what, path, res) do { if (trace_fs) { u32 cs_[6]; tfs_callers(c, cs_); fprintf(stderr, "[fs] %s %s -> %d (from %08x < %08x < %08x < %08x < %08x < %08x)\n", what, path, (int)(res), cs_[0], cs_[1], cs_[2], cs_[3], cs_[4], cs_[5]); } } while (0)
// With ARKHAM_LANGUAGE, also the answer to the file where Steam writes the game's language, which Feral's code reads at every start. The file stays as it is.
static int language_file(const char *path) {
    static const char tail[] = "/Contents/Resources/steam_language";
    const char *lang = getenv("ARKHAM_LANGUAGE");
    size_t n = strlen(path), t = sizeof tail - 1;
    if (!lang || !*lang || n < t || strcmp(path + n - t, tail)) return -1;
    // The process's own temporary folder: an iOS app may not write to /tmp.
    const char *dir = getenv("TMPDIR");
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s/arkham-language-XXXXXX", dir && *dir ? dir : "/tmp");
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    unlink(tmp);
    if (write(fd, lang, strlen(lang)) < 0 || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
SHIM(open) {
    int fd = language_file(GS(ARG(0)));
    CIBUF(0);
    if (fd < 0 && ((int)ARG(1) & O_ACCMODE) == O_RDONLY && input_rewrites()) fd = input_ini_file(CIP(0));
#if !TARGET_OS_IPHONE
    // An experiment on the Mac mustn't leave its input settings in the Mac's game: what the engine writes back to them goes nowhere.
    if (fd < 0 && ((int)ARG(1) & O_ACCMODE) != O_RDONLY && input_rewrites() && is_input_ini(GS(ARG(0)))) {
        fd = open("/dev/null", O_WRONLY);
        fprintf(stderr, "[input] the engine's write of %s goes nowhere during this experiment\n", GS(ARG(0)));
    }
#endif
    if (fd < 0) {
        fd = open(CIP(0), (int)ARG(1), (int)ARG(2));
        if (fd >= 0 && ((int)ARG(1) & O_CREAT)) ci_changed();
    }
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
SHIM(access) { CIBUF(0); int r = access(CIP(0), (int)ARG(1)); if (r) set_errno(); TFS("access", GS(ARG(0)), r); RET(r); }
SHIM(mkdir) { CIBUF(0); int r = mkdir(CIP(0), (mode_t)ARG(1)); if (r) set_errno(); else ci_changed(); RET(r); }
SHIM(rmdir) { CIBUF(0); int r = rmdir(CIP(0)); if (!r) ci_changed(); RET(r); }
SHIM(unlink) { CIBUF(0); int r = unlink(CIP(0)); if (!r) ci_changed(); RET(r); }
SHIM(remove) { CIBUF(0); int r = remove(CIP(0)); if (!r) ci_changed(); RET(r); }
SHIM(rename) { CIBUF(0); CIBUF(1); int r = rename(CIP(0), CIP(1)); if (!r) ci_changed(); RET(r); }
SHIM(chmod) { CIBUF(0); RET(chmod(CIP(0), (mode_t)ARG(1))); }
SHIM(ftruncate) { RET(ftruncate((int)ARG(0), (off_t)ARGQ(1))); }
SHIM(truncate) { CIBUF(0); RET(truncate(CIP(0), (off_t)ARGQ(1))); }
SHIM(fsync) { RET(fsync((int)ARG(0))); }
SHIM(isatty) { RET(isatty((int)ARG(0))); }
SHIM(dup) { RET(dup((int)ARG(0))); }
SHIM(dup2) { RET(dup2((int)ARG(0), (int)ARG(1))); }
SHIM(fcntl) { RET(fcntl((int)ARG(0), (int)ARG(1), (int)ARG(2))); }
SHIM(umask) { RET(umask((mode_t)ARG(0))); }
SHIM(readlink) { CIBUF(0); RET((i32)readlink(CIP(0), GP(ARG(1)), ARG(2))); }
SHIM(realpath) {
    char tmp[PATH_MAX];
    CIBUF(0);
    char *r = realpath(CIP(0), tmp);
    TFS("realpath", GS(ARG(0)), r != NULL);
    if (trace_fs && r) fprintf(stderr, "[fs]   = %s\n", tmp);
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
SHIM(stat64) { CIBUF(0); struct stat s; int r = stat(CIP(0), &s); if (r) set_errno(); else put_stat64(ARG(1), &s); TFS("stat", GS(ARG(0)), r); RET(r); }
SHIM(lstat64) { CIBUF(0); struct stat s; int r = lstat(CIP(0), &s); if (r) set_errno(); else put_stat64(ARG(1), &s); RET(r); }
SHIM(fstat64) { struct stat s; int r = fstat((int)ARG(0), &s); if (r) set_errno(); else put_stat64(ARG(1), &s); RET(r); }
SHIM(stat32) { CIBUF(0); struct stat s; int r = stat(CIP(0), &s); if (r) set_errno(); else put_stat32(ARG(1), &s); TFS("stat", GS(ARG(0)), r); RET(r); }
SHIM(lstat32) { CIBUF(0); struct stat s; int r = lstat(CIP(0), &s); if (r) set_errno(); else put_stat32(ARG(1), &s); RET(r); }
SHIM(fstat32) { struct stat s; int r = fstat((int)ARG(0), &s); if (r) set_errno(); else put_stat32(ARG(1), &s); RET(r); }

// Directories: guest dirent with 64-bit inodes (d_ino 8, d_seekoff 8, d_reclen 2, d_namlen 2, d_type 1, d_name 1024).
SHIM(opendir) { CIBUF(0); DIR *d = opendir(CIP(0)); TFS("opendir", GS(ARG(0)), d != NULL); RET(rt_handle_new(d)); }
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

        snprintf(buf, sizeof buf, "%s/Batman Arkham Asylum GOTY.app/Contents/MacOS/Batman Arkham Asylum GOTY", rt_game_root());
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
// Sleeping needs no lock, and the sound mixer sleeps between chunks: it mustn't have to wait for the lock to wake up. These only give up the
// gate in one-at-a-time mode, where a running thread holds it.
// ARKHAM_TRACE_SLEEP=1: every 5 s, who in the game slept, how often and for how long, by return address. For finding a frame limiter.
static void note_sleep(CPU *c, u32 us) {
    static int on = -1;
    if (on < 0) on = getenv("ARKHAM_TRACE_SLEEP") != NULL;
    if (!on) return;
    enum { SLOTS = 32 };
    static struct {
        u32 from, n;
        u64 us;
    } seen[SLOTS];
    static u64 last;
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    u32 from = G32(c->esp);
    os_unfair_lock_lock(&lock);
    for (int i = 0; i < SLOTS; i++) {
        if (seen[i].from == from || !seen[i].from) {
            seen[i].from = from;
            seen[i].n++;
            seen[i].us += us;
            break;
        }
    }
    u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000000u;
    if (now - last >= 5) {
        last = now;
        fprintf(stderr, "[sleep] per second:");
        for (int i = 0; i < SLOTS && seen[i].from; i++) fprintf(stderr, " from %#x %u times, %.1f ms;", seen[i].from, seen[i].n / 5, (double)seen[i].us / 5000.0);
        fprintf(stderr, "\n");
        memset(seen, 0, sizeof seen);
    }
    os_unfair_lock_unlock(&lock);
}
SHIM(usleep) {
    u32 us = ARG(0);
    note_sleep(c, us);
    int held = rt_gate_held();
    if (held) rt_gate_release();
    usleep(us);
    if (held) rt_gate_acquire();
    RET(0);
}
SHIM(sleep) {
    u32 s = ARG(0);
    int held = rt_gate_held();
    if (held) rt_gate_release();
    sleep(s);
    if (held) rt_gate_acquire();
    RET(0);
}
SHIM(nanosleep) {
    struct timespec ts = {(time_t)G32(ARG(0)), (long)G32(ARG(0) + 4)};
    note_sleep(c, (u32)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000));
    int held = rt_gate_held();
    if (held) rt_gate_release();
    nanosleep(&ts, NULL);
    if (held) rt_gate_acquire();
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
