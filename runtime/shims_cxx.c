// The parts of GNU libstdc++ the game imports, reimplemented over guest memory with the i386 layouts.
#include "shim.h"
#include "shim_reg.h"

#include <stdio.h>
#include <stdlib.h>

// ---------- operator new / delete ----------
SHIM(Znwm) { RET(rt_guest_malloc(ARG(0))); }
SHIM(ZdlPv) { rt_guest_free(ARG(0)); }

// ---------- static-local guards ----------
SHIM(__cxa_guard_acquire) { RET(G8(ARG(0)) ? 0 : 1); }
SHIM(__cxa_guard_release) { G8(ARG(0)) = 1; }
SHIM(__cxa_guard_abort) {}
SHIM(__cxa_pure_virtual) { rt_trap(c, G32(c->esp), "pure virtual call"); }

// ---------- exceptions: reported loudly until a real path needs them ----------
static const char *type_name(u32 ti) { return ti ? GS(G32(ti + 4)) : "?"; }
SHIM(__cxa_allocate_exception) { RET(rt_guest_malloc(ARG(0) + 128) + 128); }
SHIM(__cxa_free_exception) {}
SHIM(__cxa_throw) {
    fprintf(stderr, "\n*** guest threw a C++ exception of type %s\n", type_name(ARG(1)));
    rt_trap(c, G32(c->esp), "__cxa_throw");
}
SHIM(__cxa_rethrow) { rt_trap(c, G32(c->esp), "__cxa_rethrow"); }
SHIM(__cxa_begin_catch) { RET(ARG(0)); }
SHIM(__cxa_end_catch) {}
SHIM(_Unwind_Resume) { rt_trap(c, G32(c->esp), "_Unwind_Resume"); }
SHIM(terminate) { rt_trap(c, G32(c->esp), "std::terminate"); }
SHIM(throw_bad_alloc) { rt_trap(c, G32(c->esp), "std::bad_alloc"); }
SHIM(throw_logic_error) { fprintf(stderr, "*** logic_error: %s\n", GS(ARG(0))); rt_trap(c, G32(c->esp), "logic_error"); }
SHIM(throw_length_error) { fprintf(stderr, "*** length_error: %s\n", GS(ARG(0))); rt_trap(c, G32(c->esp), "length_error"); }
SHIM(throw_out_of_range) { fprintf(stderr, "*** out_of_range: %s\n", GS(ARG(0))); rt_trap(c, G32(c->esp), "out_of_range"); }
SHIM(uncaught_exception) { RET(0); }
SHIM(set_new_handler) { RET(0); }
SHIM(noop) {}
SHIM(__cxa_demangle) {
    if (ARG(3)) G32(ARG(3)) = (u32)-2;
    RET(0);
}

// ---------- RTTI and dynamic_cast ----------
static u32 tv_si, tv_vmi, tv_class;

static int ti_equal(u32 a, u32 b) {
    if (a == b) return 1;
    u32 na = G32(a + 4), nb = G32(b + 4);
    return na == nb || (GS(na)[0] != '*' && !strcmp(GS(na), GS(nb)));
}

// Finds dst inside the object whose static type is ti at obj. Returns the guest pointer or 0.
static u32 find_base(u32 ti, u32 obj, u32 dst) {
    if (ti_equal(ti, dst)) return obj;
    u32 vp = G32(ti);
    if (vp == tv_si) return find_base(G32(ti + 8), obj, dst);
    if (vp == tv_vmi) {
        u32 count = G32(ti + 12);
        for (u32 i = 0; i < count; i++) {
            u32 base = G32(ti + 16 + i * 8);
            i32 off_flags = (i32)G32(ti + 20 + i * 8);
            i32 off = off_flags >> 8;
            u32 sub;
            if (off_flags & 1) {  // virtual base: the offset lives in the vtable
                u32 vt = G32(obj);
                sub = obj + (u32)(i32)G32(vt + (u32)off);
            } else {
                sub = obj + (u32)off;
            }
            u32 r = find_base(base, sub, dst);
            if (r) return r;
        }
    }
    return 0;
}

SHIM(__dynamic_cast) {
    u32 src = ARG(0), dst_type = ARG(2);
    if (!src) {
        RET(0);
        return;
    }
    u32 vt = G32(src);
    i32 top = (i32)G32(vt - 8);
    u32 whole_type = G32(vt - 4);
    u32 whole = src + (u32)top;
    RET(find_base(whole_type, whole, dst_type));
}

// ---------- COW std::string: object holds a char*, with {length, capacity, refcount} just before it ----------
static u32 empty_rep;
#define S_EMPTY (empty_rep + 12)
#define LEN(p) G32((p)-12)
#define CAP(p) G32((p)-8)
#define REF(p) G32((p)-4)
#define PCH(p) ((char *)GP(p))

static u32 rep_alloc(u32 cap) {
    u32 r = rt_guest_malloc(cap + 13);
    G32(r) = 0;
    G32(r + 4) = cap;
    G32(r + 8) = 0;
    G8(r + 12) = 0;
    return r;
}
static void str_release(u32 p) {
    if (p == S_EMPTY) return;
    i32 rc = (i32)REF(p);
    if (rc <= 0) rt_guest_free(p - 12);
    else REF(p) = (u32)(rc - 1);
}
static u32 str_new(const char *s, u32 n, u32 cap) {
    if (!n && !cap) return S_EMPTY;
    u32 r = rep_alloc(cap > n ? cap : n);
    u32 p = r + 12;
    if (n) memcpy(PCH(p), s, n);
    G8(p + n) = 0;
    G32(r) = n;
    return p;
}
static u32 str_share(u32 p) {
    if (p == S_EMPTY) return p;
    if ((i32)REF(p) < 0) return str_new(PCH(p), LEN(p), 0);
    REF(p)++;
    return p;
}
// Replace the [pos,pos+n1) range of this string with n2 bytes taken from a host copy of s.
static void str_splice(u32 self, u32 pos, u32 n1, const char *s, u32 n2) {
    u32 p = G32(self);
    u32 len = LEN(p);
    if (pos > len) pos = len;
    if (n1 > len - pos) n1 = len - pos;
    u32 nl = len - n1 + n2;
    char *copy = NULL;
    if (s && n2) {
        copy = malloc(n2);
        memcpy(copy, s, n2);
    }
    if (p != S_EMPTY && (i32)REF(p) <= 0 && nl <= CAP(p)) {
        char *b = PCH(p);
        memmove(b + pos + n2, b + pos + n1, len - pos - n1);
        if (copy) memcpy(b + pos, copy, n2);
        b[nl] = 0;
        LEN(p) = nl;
        REF(p) = 0;
    } else {
        u32 cap = nl;
        if (p != S_EMPTY && nl > CAP(p) && nl < 2 * CAP(p)) cap = 2 * CAP(p);
        u32 r = rep_alloc(cap);
        u32 q = r + 12;
        char *b = PCH(q);
        char *o = PCH(p);
        memcpy(b, o, pos);
        if (copy) memcpy(b + pos, copy, n2);
        memcpy(b + pos + n2, o + pos + n1, len - pos - n1);
        b[nl] = 0;
        G32(r) = nl;
        str_release(p);
        G32(self) = q;
    }
    free(copy);
}
SHIM(rep_create) {
    u32 cap = ARG(0), old = ARG(1);
    if (cap > old && cap < 2 * old) cap = 2 * old;
    RET(rep_alloc(cap));
}
SHIM(rep_destroy) { rt_guest_free(ARG(0)); }
SHIM(s_ctor_c) {
    u32 s = ARG(1);
    G32(ARG(0)) = str_new(GS(s), (u32)strlen(GS(s)), 0);
}
SHIM(s_ctor_cn) { G32(ARG(0)) = str_new(GS(ARG(1)), ARG(2), 0); }
SHIM(s_ctor_copy) { G32(ARG(0)) = str_share(G32(ARG(1))); }
SHIM(s_ctor_sub) {
    u32 o = G32(ARG(1)), pos = ARG(2), n = ARG(3), len = LEN(o);
    if (pos > len) pos = len;
    if (n > len - pos) n = len - pos;
    G32(ARG(0)) = str_new(PCH(o) + pos, n, 0);
}
SHIM(s_dtor) { str_release(G32(ARG(0))); }
SHIM(s_append_cn) {
    u32 self = ARG(0);
    str_splice(self, LEN(G32(self)), 0, GS(ARG(1)), ARG(2));
    RET(self);
}
SHIM(s_append_s) {
    u32 self = ARG(0), o = G32(ARG(1));
    str_splice(self, LEN(G32(self)), 0, PCH(o), LEN(o));
    RET(self);
}
SHIM(s_append_nc) {
    u32 self = ARG(0), n = ARG(1);
    char *t = malloc(n + 1);
    memset(t, (int)ARG(2), n);
    str_splice(self, LEN(G32(self)), 0, t, n);
    free(t);
    RET(self);
}
SHIM(s_assign_cn) {
    u32 self = ARG(0);
    str_splice(self, 0, LEN(G32(self)), GS(ARG(1)), ARG(2));
    RET(self);
}
SHIM(s_assign_s) {
    u32 self = ARG(0), o = G32(ARG(1)), p = G32(self);
    if (o != p) {
        u32 n = str_share(o);
        str_release(p);
        G32(self) = n;
    }
    RET(self);
}
SHIM(s_resize) {
    u32 self = ARG(0), n = ARG(1), len = LEN(G32(self));
    if (n > len) {
        char *t = malloc(n - len);
        memset(t, (int)ARG(2), n - len);
        str_splice(self, len, 0, t, n - len);
        free(t);
    } else if (n < len) {
        str_splice(self, n, len - n, NULL, 0);
    }
}
SHIM(s_replace) {
    u32 self = ARG(0);
    str_splice(self, ARG(1), ARG(2), GS(ARG(3)), ARG(4));
    RET(self);
}
SHIM(s_reserve) {
    u32 self = ARG(0), want = ARG(1), p = G32(self), len = LEN(p);
    if (want > CAP(p) || (i32)REF(p) > 0) {
        u32 q = str_new(PCH(p), len, want > len ? want : len);
        str_release(p);
        G32(self) = q;
    }
}
SHIM(s_mutate) {
    u32 self = ARG(0), pos = ARG(1), l1 = ARG(2), l2 = ARG(3);
    char *t = l2 ? calloc(1, l2) : NULL;
    str_splice(self, pos, l1, t, l2);
    free(t);
    REF(G32(self)) = (G32(self) == S_EMPTY) ? 0 : REF(G32(self));
}
SHIM(s_leak_hard) {
    u32 self = ARG(0), p = G32(self);
    if (p == S_EMPTY) return;
    if ((i32)REF(p) > 0) {
        u32 q = str_new(PCH(p), LEN(p), 0);
        str_release(p);
        G32(self) = q;
    }
    REF(G32(self)) = (u32)-1;
}
#define NPOS 0xFFFFFFFFu
SHIM(s_find_cmm) {
    u32 p = G32(ARG(0)), pos = ARG(2), n = ARG(3), len = LEN(p);
    const char *h = PCH(p), *s = GS(ARG(1));
    if (n == 0) {
        RET(pos <= len ? pos : NPOS);
        return;
    }
    for (u32 i = pos; i + n <= len; i++) {
        if (!memcmp(h + i, s, n)) {
            RET(i);
            return;
        }
    }
    RET(NPOS);
}
SHIM(s_find_c) {
    u32 p = G32(ARG(0)), pos = ARG(2), len = LEN(p);
    if (pos < len) {
        const char *h = PCH(p);
        const char *r = memchr(h + pos, (int)(u8)ARG(1), len - pos);
        if (r) {
            RET(r - h);
            return;
        }
    }
    RET(NPOS);
}
SHIM(s_rfind_c) {
    u32 p = G32(ARG(0)), pos = ARG(2), len = LEN(p);
    const char *h = PCH(p);
    if (len) {
        u32 i = pos < len - 1 ? pos : len - 1;
        for (;; i--) {
            if (h[i] == (char)ARG(1)) {
                RET(i);
                return;
            }
            if (!i) break;
        }
    }
    RET(NPOS);
}
static int cmp_range(const char *a, u32 na, const char *b, u32 nb) {
    u32 m = na < nb ? na : nb;
    int r = memcmp(a, b, m);
    return r ? r : (na < nb ? -1 : (na > nb ? 1 : 0));
}
SHIM(s_compare_c) {
    u32 p = G32(ARG(0));
    const char *s = GS(ARG(1));
    RET(cmp_range(PCH(p), LEN(p), s, (u32)strlen(s)));
}
SHIM(s_compare_ppc) {
    u32 p = G32(ARG(0)), pos = ARG(1), n = ARG(2), len = LEN(p);
    if (pos > len) pos = len;
    if (n > len - pos) n = len - pos;
    const char *s = GS(ARG(3));
    RET(cmp_range(PCH(p) + pos, n, s, (u32)strlen(s)));
}
SHIM(s_find_first_of) {
    u32 p = G32(ARG(0)), pos = ARG(2), n = ARG(3), len = LEN(p);
    const char *h = PCH(p), *s = GS(ARG(1));
    for (u32 i = pos; i < len; i++) {
        if (n && memchr(s, h[i], n)) {
            RET(i);
            return;
        }
    }
    RET(NPOS);
}
SHIM(s_find_first_not_of) {
    u32 p = G32(ARG(0)), pos = ARG(2), n = ARG(3), len = LEN(p);
    const char *h = PCH(p), *s = GS(ARG(1));
    for (u32 i = pos; i < len; i++) {
        if (!n || !memchr(s, h[i], n)) {
            RET(i);
            return;
        }
    }
    RET(NPOS);
}
SHIM(s_find_last_of) {
    u32 p = G32(ARG(0)), pos = ARG(2), n = ARG(3), len = LEN(p);
    const char *h = PCH(p), *s = GS(ARG(1));
    if (len && n) {
        u32 i = pos < len - 1 ? pos : len - 1;
        for (;; i--) {
            if (memchr(s, h[i], n)) {
                RET(i);
                return;
            }
            if (!i) break;
        }
    }
    RET(NPOS);
}
SHIM(s_find_last_not_of_c) {
    u32 p = G32(ARG(0)), pos = ARG(2), len = LEN(p);
    const char *h = PCH(p);
    if (len) {
        u32 i = pos < len - 1 ? pos : len - 1;
        for (;; i--) {
            if (h[i] != (char)ARG(1)) {
                RET(i);
                return;
            }
            if (!i) break;
        }
    }
    RET(NPOS);
}

// ---------- std::map red-black tree: node {color, parent, left, right} ----------
#define COLOR(n) G32((n) + 0)
#define PARENT(n) G32((n) + 4)
#define LEFT(n) G32((n) + 8)
#define RIGHT(n) G32((n) + 12)
#define RED 0u
#define BLACK 1u

static u32 tree_inc(u32 x) {
    if (RIGHT(x)) {
        x = RIGHT(x);
        while (LEFT(x)) x = LEFT(x);
    } else {
        u32 y = PARENT(x);
        while (x == RIGHT(y)) {
            x = y;
            y = PARENT(y);
        }
        if (RIGHT(x) != y) x = y;
    }
    return x;
}
static u32 tree_dec(u32 x) {
    if (COLOR(x) == RED && PARENT(PARENT(x)) == x) {
        x = RIGHT(x);
    } else if (LEFT(x)) {
        u32 y = LEFT(x);
        while (RIGHT(y)) y = RIGHT(y);
        x = y;
    } else {
        u32 y = PARENT(x);
        while (x == LEFT(y)) {
            x = y;
            y = PARENT(y);
        }
        x = y;
    }
    return x;
}
SHIM(tree_increment) { RET(tree_inc(ARG(0))); }
SHIM(tree_decrement) { RET(tree_dec(ARG(0))); }

static void rot_left(u32 x, u32 hdr) {
    u32 y = RIGHT(x);
    RIGHT(x) = LEFT(y);
    if (LEFT(y)) PARENT(LEFT(y)) = x;
    PARENT(y) = PARENT(x);
    if (x == PARENT(hdr)) PARENT(hdr) = y;
    else if (x == LEFT(PARENT(x))) LEFT(PARENT(x)) = y;
    else RIGHT(PARENT(x)) = y;
    LEFT(y) = x;
    PARENT(x) = y;
}
static void rot_right(u32 x, u32 hdr) {
    u32 y = LEFT(x);
    LEFT(x) = RIGHT(y);
    if (RIGHT(y)) PARENT(RIGHT(y)) = x;
    PARENT(y) = PARENT(x);
    if (x == PARENT(hdr)) PARENT(hdr) = y;
    else if (x == RIGHT(PARENT(x))) RIGHT(PARENT(x)) = y;
    else LEFT(PARENT(x)) = y;
    RIGHT(y) = x;
    PARENT(x) = y;
}
SHIM(tree_insert_rebalance) {
    u32 ins_left = ARG(0) & 0xFF, x = ARG(1), p = ARG(2), hdr = ARG(3);
    PARENT(x) = p;
    LEFT(x) = 0;
    RIGHT(x) = 0;
    COLOR(x) = RED;
    if (ins_left) {
        LEFT(p) = x;
        if (p == hdr) {
            PARENT(hdr) = x;
            RIGHT(hdr) = x;
        } else if (p == LEFT(hdr)) {
            LEFT(hdr) = x;
        }
    } else {
        RIGHT(p) = x;
        if (p == RIGHT(hdr)) RIGHT(hdr) = x;
    }
    while (x != PARENT(hdr) && COLOR(PARENT(x)) == RED) {
        u32 pp = PARENT(x), gp = PARENT(pp);
        if (pp == LEFT(gp)) {
            u32 y = RIGHT(gp);
            if (y && COLOR(y) == RED) {
                COLOR(pp) = BLACK;
                COLOR(y) = BLACK;
                COLOR(gp) = RED;
                x = gp;
            } else {
                if (x == RIGHT(pp)) {
                    x = pp;
                    rot_left(x, hdr);
                    pp = PARENT(x);
                    gp = PARENT(pp);
                }
                COLOR(pp) = BLACK;
                COLOR(gp) = RED;
                rot_right(gp, hdr);
            }
        } else {
            u32 y = LEFT(gp);
            if (y && COLOR(y) == RED) {
                COLOR(pp) = BLACK;
                COLOR(y) = BLACK;
                COLOR(gp) = RED;
                x = gp;
            } else {
                if (x == LEFT(pp)) {
                    x = pp;
                    rot_right(x, hdr);
                    pp = PARENT(x);
                    gp = PARENT(pp);
                }
                COLOR(pp) = BLACK;
                COLOR(gp) = RED;
                rot_left(gp, hdr);
            }
        }
    }
    COLOR(PARENT(hdr)) = BLACK;
}
SHIM(tree_erase_rebalance) {
    u32 z = ARG(0), hdr = ARG(1);
    u32 y = z, x = 0, xp = 0;
    if (!LEFT(y)) {
        x = RIGHT(y);
    } else if (!RIGHT(y)) {
        x = LEFT(y);
    } else {
        y = RIGHT(y);
        while (LEFT(y)) y = LEFT(y);
        x = RIGHT(y);
    }
    if (y != z) {
        PARENT(LEFT(z)) = y;
        LEFT(y) = LEFT(z);
        if (y != RIGHT(z)) {
            xp = PARENT(y);
            if (x) PARENT(x) = PARENT(y);
            LEFT(PARENT(y)) = x;
            RIGHT(y) = RIGHT(z);
            PARENT(RIGHT(z)) = y;
        } else {
            xp = y;
        }
        if (PARENT(hdr) == z) PARENT(hdr) = y;
        else if (LEFT(PARENT(z)) == z) LEFT(PARENT(z)) = y;
        else RIGHT(PARENT(z)) = y;
        PARENT(y) = PARENT(z);
        u32 t = COLOR(y);
        COLOR(y) = COLOR(z);
        COLOR(z) = t;
        y = z;
    } else {
        xp = PARENT(y);
        if (x) PARENT(x) = PARENT(y);
        if (PARENT(hdr) == z) PARENT(hdr) = x;
        else if (LEFT(PARENT(z)) == z) LEFT(PARENT(z)) = x;
        else RIGHT(PARENT(z)) = x;
        if (LEFT(hdr) == z) {
            if (!RIGHT(z)) {
                LEFT(hdr) = PARENT(z);
            } else {
                u32 m = x;
                while (LEFT(m)) m = LEFT(m);
                LEFT(hdr) = m;
            }
        }
        if (RIGHT(hdr) == z) {
            if (!LEFT(z)) {
                RIGHT(hdr) = PARENT(z);
            } else {
                u32 m = x;
                while (RIGHT(m)) m = RIGHT(m);
                RIGHT(hdr) = m;
            }
        }
    }
    if (COLOR(y) != RED) {
        while (x != PARENT(hdr) && (!x || COLOR(x) == BLACK)) {
            if (x == LEFT(xp)) {
                u32 w = RIGHT(xp);
                if (COLOR(w) == RED) {
                    COLOR(w) = BLACK;
                    COLOR(xp) = RED;
                    rot_left(xp, hdr);
                    w = RIGHT(xp);
                }
                if ((!LEFT(w) || COLOR(LEFT(w)) == BLACK) && (!RIGHT(w) || COLOR(RIGHT(w)) == BLACK)) {
                    COLOR(w) = RED;
                    x = xp;
                    xp = PARENT(xp);
                } else {
                    if (!RIGHT(w) || COLOR(RIGHT(w)) == BLACK) {
                        COLOR(LEFT(w)) = BLACK;
                        COLOR(w) = RED;
                        rot_right(w, hdr);
                        w = RIGHT(xp);
                    }
                    COLOR(w) = COLOR(xp);
                    COLOR(xp) = BLACK;
                    if (RIGHT(w)) COLOR(RIGHT(w)) = BLACK;
                    rot_left(xp, hdr);
                    break;
                }
            } else {
                u32 w = LEFT(xp);
                if (COLOR(w) == RED) {
                    COLOR(w) = BLACK;
                    COLOR(xp) = RED;
                    rot_right(xp, hdr);
                    w = LEFT(xp);
                }
                if ((!RIGHT(w) || COLOR(RIGHT(w)) == BLACK) && (!LEFT(w) || COLOR(LEFT(w)) == BLACK)) {
                    COLOR(w) = RED;
                    x = xp;
                    xp = PARENT(xp);
                } else {
                    if (!LEFT(w) || COLOR(LEFT(w)) == BLACK) {
                        COLOR(RIGHT(w)) = BLACK;
                        COLOR(w) = RED;
                        rot_left(w, hdr);
                        w = LEFT(xp);
                    }
                    COLOR(w) = COLOR(xp);
                    COLOR(xp) = BLACK;
                    if (LEFT(w)) COLOR(LEFT(w)) = BLACK;
                    rot_right(xp, hdr);
                    break;
                }
            }
        }
        if (x) COLOR(x) = BLACK;
    }
    RET(y);
}

// ---------- std::list node {next, prev} ----------
#define NEXT(n) G32((n) + 0)
#define PREV(n) G32((n) + 4)
SHIM(list_hook) {
    u32 self = ARG(0), pos = ARG(1);
    NEXT(self) = pos;
    PREV(self) = PREV(pos);
    NEXT(PREV(pos)) = self;
    PREV(pos) = self;
}
SHIM(list_unhook) {
    u32 self = ARG(0), n = NEXT(self), p = PREV(self);
    NEXT(p) = n;
    PREV(n) = p;
}
SHIM(list_transfer) {
    u32 self = ARG(0), first = ARG(1), last = ARG(2);
    if (self != last) {
        NEXT(PREV(last)) = self;
        NEXT(PREV(first)) = last;
        NEXT(PREV(self)) = first;
        u32 t = PREV(self);
        PREV(self) = PREV(last);
        PREV(last) = PREV(first);
        PREV(first) = t;
    }
}
SHIM(list_swap) {
    u32 a = ARG(0), b = ARG(1);
    if (NEXT(a) != a) {
        if (NEXT(b) != b) {
            u32 t = NEXT(a); NEXT(a) = NEXT(b); NEXT(b) = t;
            t = PREV(a); PREV(a) = PREV(b); PREV(b) = t;
            PREV(NEXT(a)) = a; NEXT(PREV(a)) = a;
            PREV(NEXT(b)) = b; NEXT(PREV(b)) = b;
        } else {
            NEXT(b) = NEXT(a); PREV(b) = PREV(a);
            PREV(NEXT(b)) = b; NEXT(PREV(b)) = b;
            NEXT(a) = PREV(a) = a;
        }
    } else if (NEXT(b) != b) {
        NEXT(a) = NEXT(b); PREV(a) = PREV(b);
        PREV(NEXT(a)) = a; NEXT(PREV(a)) = a;
        NEXT(b) = PREV(b) = b;
    }
}

void rt_cxx_init(CPU *c) {
    (void)c;
    empty_rep = rt_import_addr("__ZNSs4_Rep20_S_empty_rep_storageE");
    tv_si = rt_import_addr("__ZTVN10__cxxabiv120__si_class_type_infoE") + 8;
    tv_vmi = rt_import_addr("__ZTVN10__cxxabiv121__vmi_class_type_infoE") + 8;
    tv_class = rt_import_addr("__ZTVN10__cxxabiv117__class_type_infoE") + 8;
}

const ShimReg rt_shims_cxx[] = {
    R2("__Znwm", Znwm) R2("__Znam", Znwm) R2("__ZdlPv", ZdlPv) R2("__ZdaPv", ZdlPv)
    R2("___cxa_guard_acquire", __cxa_guard_acquire) R2("___cxa_guard_release", __cxa_guard_release)
    R2("___cxa_guard_abort", __cxa_guard_abort) R2("___cxa_pure_virtual", __cxa_pure_virtual)
    R2("___cxa_allocate_exception", __cxa_allocate_exception) R2("___cxa_free_exception", __cxa_free_exception)
    R2("___cxa_throw", __cxa_throw) R2("___cxa_rethrow", __cxa_rethrow) R2("___cxa_begin_catch", __cxa_begin_catch)
    R2("___cxa_end_catch", __cxa_end_catch) R2("__Unwind_Resume", _Unwind_Resume) R2("__ZSt9terminatev", terminate)
    R2("__ZSt17__throw_bad_allocv", throw_bad_alloc) R2("__ZSt19__throw_logic_errorPKc", throw_logic_error)
    R2("__ZSt20__throw_length_errorPKc", throw_length_error) R2("__ZSt20__throw_out_of_rangePKc", throw_out_of_range)
    R2("__ZSt18uncaught_exceptionv", uncaught_exception) R2("__ZSt15set_new_handlerPFvvE", set_new_handler)
    R2("__ZNSt8ios_base4InitC1Ev", noop) R2("__ZNSt8ios_base4InitD1Ev", noop) R2("__ZNSt9exceptionD1Ev", noop)
    R2("__ZNSt9exceptionD2Ev", noop) R2("__ZNSt9bad_allocD1Ev", noop) R2("__ZNSt6localeD1Ev", noop)
    R2("___cxa_demangle", __cxa_demangle) R2("___dynamic_cast", __dynamic_cast)
    R2("__ZNSs4_Rep9_S_createEmmRKSaIcE", rep_create) R2("__ZNSs4_Rep10_M_destroyERKSaIcE", rep_destroy)
    R2("__ZNSsC1EPKcRKSaIcE", s_ctor_c) R2("__ZNSsC1EPKcmRKSaIcE", s_ctor_cn) R2("__ZNSsC1ERKSs", s_ctor_copy)
    R2("__ZNSsC2ERKSs", s_ctor_copy) R2("__ZNSsC1ERKSsmm", s_ctor_sub) R2("__ZNSsD1Ev", s_dtor)
    R2("__ZNSs6appendEPKcm", s_append_cn) R2("__ZNSs6appendERKSs", s_append_s) R2("__ZNSs6appendEmc", s_append_nc)
    R2("__ZNSs6assignEPKcm", s_assign_cn) R2("__ZNSs6assignERKSs", s_assign_s) R2("__ZNSs6resizeEmc", s_resize)
    R2("__ZNSs7replaceEmmPKcm", s_replace) R2("__ZNSs7reserveEm", s_reserve) R2("__ZNSs9_M_mutateEmmm", s_mutate)
    R2("__ZNSs12_M_leak_hardEv", s_leak_hard) R2("__ZNKSs4findEPKcmm", s_find_cmm) R2("__ZNKSs4findEcm", s_find_c)
    R2("__ZNKSs5rfindEcm", s_rfind_c) R2("__ZNKSs7compareEPKc", s_compare_c) R2("__ZNKSs7compareEmmPKc", s_compare_ppc)
    R2("__ZNKSs13find_first_ofEPKcmm", s_find_first_of) R2("__ZNKSs17find_first_not_ofEPKcmm", s_find_first_not_of)
    R2("__ZNKSs12find_last_ofEPKcmm", s_find_last_of) R2("__ZNKSs16find_last_not_ofEcm", s_find_last_not_of_c)
    R2("__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base", tree_increment)
    R2("__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base", tree_increment)
    R2("__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base", tree_decrement)
    R2("__ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base", tree_decrement)
    R2("__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_", tree_insert_rebalance)
    R2("__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_", tree_erase_rebalance)
    R2("__ZNSt15_List_node_base4hookEPS_", list_hook) R2("__ZNSt15_List_node_base6unhookEv", list_unhook)
    R2("__ZNSt15_List_node_base8transferEPS_S0_", list_transfer) R2("__ZNSt15_List_node_base4swapERS_S0_", list_swap)
    {NULL, NULL},
};
