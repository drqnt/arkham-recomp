// x86 SSE and x87 semantics, written lane by lane so behaviour stays obvious.
#pragma once
#define OPF static inline __attribute__((always_inline)) v128

#define PS(name, expr)                                                                          \
    OPF op_##name(v128 a, v128 b) {                                                             \
        v128 r;                                                                                 \
        for (int k = 0; k < 4; k++) {                                                           \
            float x = a.f[k], y = b.f[k];                                                       \
            r.f[k] = (expr);                                                                    \
        }                                                                                       \
        return r;                                                                               \
    }
#define PD(name, expr)                                                                          \
    OPF op_##name(v128 a, v128 b) {                                                             \
        v128 r;                                                                                 \
        for (int k = 0; k < 2; k++) {                                                           \
            double x = a.d[k], y = b.d[k];                                                      \
            r.d[k] = (expr);                                                                    \
        }                                                                                       \
        return r;                                                                               \
    }
#define INTOP(name, fld, n, T, expr)                                                            \
    OPF op_##name(v128 a, v128 b) {                                                             \
        v128 r;                                                                                 \
        for (int k = 0; k < n; k++) {                                                           \
            T x = a.fld[k], y = b.fld[k];                                                       \
            r.fld[k] = (T)(expr);                                                               \
        }                                                                                       \
        return r;                                                                               \
    }
#define BITOP(name, expr)                                                                       \
    OPF op_##name(v128 a, v128 b) {                                                             \
        v128 r;                                                                                 \
        r.q[0] = a.q[0] expr b.q[0];                                                            \
        r.q[1] = a.q[1] expr b.q[1];                                                            \
        return r;                                                                               \
    }
#define ANDN(name)                                                                              \
    OPF op_##name(v128 a, v128 b) {                                                             \
        v128 r;                                                                                 \
        r.q[0] = ~a.q[0] & b.q[0];                                                              \
        r.q[1] = ~a.q[1] & b.q[1];                                                              \
        return r;                                                                               \
    }

OPF op_addps(v128 a, v128 b) { v128 r; r.vf = a.vf + b.vf; return r; }
OPF op_subps(v128 a, v128 b) { v128 r; r.vf = a.vf - b.vf; return r; }
OPF op_mulps(v128 a, v128 b) { v128 r; r.vf = a.vf * b.vf; return r; }
OPF op_divps(v128 a, v128 b) { v128 r; r.vf = a.vf / b.vf; return r; }
PS(minps, x < y ? x : y)
PS(maxps, x > y ? x : y)
BITOP(andps, &) BITOP(orps, |) BITOP(xorps, ^) ANDN(andnps)
OPF op_addpd(v128 a, v128 b) { v128 r; r.vd = a.vd + b.vd; return r; }
OPF op_subpd(v128 a, v128 b) { v128 r; r.vd = a.vd - b.vd; return r; }
OPF op_mulpd(v128 a, v128 b) { v128 r; r.vd = a.vd * b.vd; return r; }
OPF op_divpd(v128 a, v128 b) { v128 r; r.vd = a.vd / b.vd; return r; }
PD(minpd, x < y ? x : y)
PD(maxpd, x > y ? x : y)
BITOP(andpd, &) BITOP(orpd, |) BITOP(xorpd, ^) ANDN(andnpd)
BITOP(pand, &) BITOP(por, |) BITOP(pxor, ^) ANDN(pandn)

INTOP(paddb, b, 16, u8, x + y) INTOP(paddw, w, 8, u16, x + y) INTOP(paddd, u, 4, u32, x + y) INTOP(paddq, q, 2, u64, x + y)
INTOP(psubb, b, 16, u8, x - y) INTOP(psubw, w, 8, u16, x - y) INTOP(psubd, u, 4, u32, x - y) INTOP(psubq, q, 2, u64, x - y)
INTOP(psubusb, b, 16, u8, x > y ? x - y : 0) INTOP(psubusw, w, 8, u16, x > y ? x - y : 0)
INTOP(paddusb, b, 16, u8, (u32)x + y > 255 ? 255 : x + y) INTOP(paddusw, w, 8, u16, (u32)x + y > 65535 ? 65535 : x + y)
INTOP(pmullw, s, 8, i16, (i32)x * y) INTOP(pmulhw, s, 8, i16, ((i32)x * y) >> 16)
INTOP(pavgb, b, 16, u8, ((u32)x + y + 1) >> 1) INTOP(pavgw, w, 8, u16, ((u32)x + y + 1) >> 1)
INTOP(pminub, b, 16, u8, x < y ? x : y) INTOP(pmaxub, b, 16, u8, x > y ? x : y)
INTOP(pminsw, s, 8, i16, x < y ? x : y) INTOP(pmaxsw, s, 8, i16, x > y ? x : y)
INTOP(pcmpeqb, b, 16, u8, x == y ? 0xff : 0) INTOP(pcmpeqw, w, 8, u16, x == y ? 0xffff : 0) INTOP(pcmpeqd, u, 4, u32, x == y ? ~0u : 0)
INTOP(pcmpgtb, c, 16, i8, x > y ? -1 : 0) INTOP(pcmpgtw, s, 8, i16, x > y ? -1 : 0) INTOP(pcmpgtd, i, 4, i32, x > y ? -1 : 0)

OPF op_pmuludq(v128 a, v128 b) {
    v128 r;
    r.q[0] = (u64)a.u[0] * b.u[0];
    r.q[1] = (u64)a.u[2] * b.u[2];
    return r;
}
OPF op_pmaddwd(v128 a, v128 b) {
    v128 r;
    for (int k = 0; k < 4; k++) r.i[k] = (i32)a.s[2 * k] * b.s[2 * k] + (i32)a.s[2 * k + 1] * b.s[2 * k + 1];
    return r;
}
OPF op_psadbw(v128 a, v128 b) {
    v128 r;
    for (int h = 0; h < 2; h++) {
        u32 s = 0;
        for (int k = 0; k < 8; k++) {
            int d = (int)a.b[h * 8 + k] - b.b[h * 8 + k];
            s += d < 0 ? -d : d;
        }
        r.q[h] = s;
    }
    return r;
}

#define UNPACKL(name, fld, n)                                                                   \
    OPF op_##name(v128 a, v128 b) {                                                             \
        v128 r;                                                                                 \
        for (int k = 0; k < n / 2; k++) {                                                       \
            r.fld[2 * k] = a.fld[k];                                                            \
            r.fld[2 * k + 1] = b.fld[k];                                                        \
        }                                                                                       \
        return r;                                                                               \
    }
#define UNPACKH(name, fld, n)                                                                   \
    OPF op_##name(v128 a, v128 b) {                                                             \
        v128 r;                                                                                 \
        for (int k = 0; k < n / 2; k++) {                                                       \
            r.fld[2 * k] = a.fld[n / 2 + k];                                                    \
            r.fld[2 * k + 1] = b.fld[n / 2 + k];                                                \
        }                                                                                       \
        return r;                                                                               \
    }
UNPACKL(punpcklbw, b, 16) UNPACKL(punpcklwd, w, 8) UNPACKL(punpckldq, u, 4) UNPACKL(punpcklqdq, q, 2)
UNPACKH(punpckhbw, b, 16) UNPACKH(punpckhwd, w, 8) UNPACKH(punpckhdq, u, 4) UNPACKH(punpckhqdq, q, 2)
UNPACKL(unpcklps, u, 4) UNPACKH(unpckhps, u, 4) UNPACKL(unpcklpd, q, 2) UNPACKH(unpckhpd, q, 2)

static inline u8 rt_sat_u8(i32 v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }
static inline i8 rt_sat_i8(i32 v) { return v < -128 ? -128 : v > 127 ? 127 : (i8)v; }
static inline i16 rt_sat_i16(i32 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (i16)v; }
OPF op_packuswb(v128 a, v128 b) {
    v128 r;
    for (int k = 0; k < 8; k++) {
        r.b[k] = rt_sat_u8(a.s[k]);
        r.b[8 + k] = rt_sat_u8(b.s[k]);
    }
    return r;
}
OPF op_packsswb(v128 a, v128 b) {
    v128 r;
    for (int k = 0; k < 8; k++) {
        r.c[k] = rt_sat_i8(a.s[k]);
        r.c[8 + k] = rt_sat_i8(b.s[k]);
    }
    return r;
}
OPF op_packssdw(v128 a, v128 b) {
    v128 r;
    for (int k = 0; k < 4; k++) {
        r.s[k] = rt_sat_i16(a.i[k]);
        r.s[4 + k] = rt_sat_i16(b.i[k]);
    }
    return r;
}
OPF op_haddps(v128 a, v128 b) {
    v128 r;
    r.f[0] = a.f[0] + a.f[1]; r.f[1] = a.f[2] + a.f[3];
    r.f[2] = b.f[0] + b.f[1]; r.f[3] = b.f[2] + b.f[3];
    return r;
}
OPF op_haddpd(v128 a, v128 b) {
    v128 r;
    r.d[0] = a.d[0] + a.d[1];
    r.d[1] = b.d[0] + b.d[1];
    return r;
}
OPF op_phaddd(v128 a, v128 b) {
    v128 r;
    r.u[0] = a.u[0] + a.u[1]; r.u[1] = a.u[2] + a.u[3];
    r.u[2] = b.u[0] + b.u[1]; r.u[3] = b.u[2] + b.u[3];
    return r;
}
OPF op_pshufb(v128 a, v128 b) {
    v128 r;
    for (int k = 0; k < 16; k++) r.b[k] = (b.b[k] & 0x80) ? 0 : a.b[b.b[k] & 15];
    return r;
}
OPF op_sqrtps(v128 a) { v128 r; for (int k = 0; k < 4; k++) r.f[k] = __builtin_sqrtf(a.f[k]); return r; }
OPF op_sqrtpd(v128 a) { v128 r; for (int k = 0; k < 2; k++) r.d[k] = __builtin_sqrt(a.d[k]); return r; }
OPF op_rsqrtps(v128 a) { v128 r; for (int k = 0; k < 4; k++) r.f[k] = 1.0f / __builtin_sqrtf(a.f[k]); return r; }
OPF op_rcpps(v128 a) { v128 r; for (int k = 0; k < 4; k++) r.f[k] = 1.0f / a.f[k]; return r; }

// x86 returns the "integer indefinite" value when a conversion is out of range; ARM saturates.
static inline u32 rt_cvtt_f(float v) { return (v > -2147483904.0f && v < 2147483648.0f) ? (u32)(i32)v : 0x80000000u; }
static inline u32 rt_cvtt_d(double v) { return (v > -2147483649.0 && v < 2147483648.0) ? (u32)(i32)v : 0x80000000u; }
static inline u32 rt_cvt_f(float v) { return rt_cvtt_f(__builtin_rintf(v)); }
static inline u32 rt_cvt_d(double v) { return rt_cvtt_d(__builtin_rint(v)); }
OPF op_cvtdq2ps(v128 a) { v128 r; for (int k = 0; k < 4; k++) r.f[k] = (float)a.i[k]; return r; }
OPF op_cvttps2dq(v128 a) { v128 r; for (int k = 0; k < 4; k++) r.u[k] = rt_cvtt_f(a.f[k]); return r; }
OPF op_cvtps2dq(v128 a) { v128 r; for (int k = 0; k < 4; k++) r.u[k] = rt_cvt_f(a.f[k]); return r; }
OPF op_cvtps2pd(v128 a) { v128 r; r.d[0] = a.f[0]; r.d[1] = a.f[1]; return r; }
OPF op_cvtpd2ps(v128 a) { v128 r; r.f[0] = (float)a.d[0]; r.f[1] = (float)a.d[1]; r.u[2] = r.u[3] = 0; return r; }
OPF op_cvtdq2pd(v128 a) { v128 r; r.d[0] = a.i[0]; r.d[1] = a.i[1]; return r; }
OPF op_cvttpd2dq(v128 a) { v128 r; r.u[0] = rt_cvtt_d(a.d[0]); r.u[1] = rt_cvtt_d(a.d[1]); r.q[1] = 0; return r; }

OPF op_shufps(v128 a, v128 b, u32 m) {
    v128 r;
    r.u[0] = a.u[m & 3]; r.u[1] = a.u[(m >> 2) & 3];
    r.u[2] = b.u[(m >> 4) & 3]; r.u[3] = b.u[(m >> 6) & 3];
    return r;
}
OPF op_shufpd(v128 a, v128 b, u32 m) {
    v128 r;
    r.q[0] = a.q[m & 1];
    r.q[1] = b.q[(m >> 1) & 1];
    return r;
}
OPF op_palignr(v128 a, v128 b, u32 m) {
    u8 t[32];
    __builtin_memcpy(t, &b, 16);
    __builtin_memcpy(t + 16, &a, 16);
    v128 r;
    for (u32 k = 0; k < 16; k++) r.b[k] = (m + k) < 32 ? t[m + k] : 0;
    return r;
}
OPF op_pshufd(v128 a, u32 m) {
    v128 r;
    r.u[0] = a.u[m & 3]; r.u[1] = a.u[(m >> 2) & 3];
    r.u[2] = a.u[(m >> 4) & 3]; r.u[3] = a.u[(m >> 6) & 3];
    return r;
}
OPF op_pshuflw(v128 a, u32 m) {
    v128 r = a;
    for (int k = 0; k < 4; k++) r.w[k] = a.w[(m >> (2 * k)) & 3];
    return r;
}
OPF op_pshufhw(v128 a, u32 m) {
    v128 r = a;
    for (int k = 0; k < 4; k++) r.w[4 + k] = a.w[4 + ((m >> (2 * k)) & 3)];
    return r;
}

#define VSHIFT(name, fld, n, bits, T, expr, sat)                                                \
    OPF op_##name(v128 a, u64 cnt) {                                                            \
        v128 r;                                                                                 \
        if (cnt >= bits) cnt = sat;                                                             \
        for (int k = 0; k < n; k++) {                                                           \
            T x = a.fld[k];                                                                     \
            r.fld[k] = (T)(expr);                                                               \
        }                                                                                       \
        return r;                                                                               \
    }
OPF rt_zero(void) { v128 r; r.q[0] = r.q[1] = 0; return r; }
OPF op_psllw(v128 a, u64 n) { if (n > 15) return rt_zero(); v128 r; for (int k = 0; k < 8; k++) r.w[k] = (u16)(a.w[k] << n); return r; }
OPF op_pslld(v128 a, u64 n) { if (n > 31) return rt_zero(); v128 r; for (int k = 0; k < 4; k++) r.u[k] = a.u[k] << n; return r; }
OPF op_psllq(v128 a, u64 n) { if (n > 63) return rt_zero(); v128 r; for (int k = 0; k < 2; k++) r.q[k] = a.q[k] << n; return r; }
OPF op_psrlw(v128 a, u64 n) { if (n > 15) return rt_zero(); v128 r; for (int k = 0; k < 8; k++) r.w[k] = (u16)(a.w[k] >> n); return r; }
OPF op_psrld(v128 a, u64 n) { if (n > 31) return rt_zero(); v128 r; for (int k = 0; k < 4; k++) r.u[k] = a.u[k] >> n; return r; }
OPF op_psrlq(v128 a, u64 n) { if (n > 63) return rt_zero(); v128 r; for (int k = 0; k < 2; k++) r.q[k] = a.q[k] >> n; return r; }
OPF op_psraw(v128 a, u64 n) { if (n > 15) n = 15; v128 r; for (int k = 0; k < 8; k++) r.s[k] = (i16)(a.s[k] >> n); return r; }
OPF op_psrad(v128 a, u64 n) { if (n > 31) n = 31; v128 r; for (int k = 0; k < 4; k++) r.i[k] = a.i[k] >> n; return r; }
OPF op_pslldq(v128 a, u64 n) {
    v128 r = rt_zero();
    if (n > 15) return r;
    for (int k = (int)n; k < 16; k++) r.b[k] = a.b[k - n];
    return r;
}
OPF op_psrldq(v128 a, u64 n) {
    v128 r = rt_zero();
    if (n > 15) return r;
    for (int k = 0; k + (int)n < 16; k++) r.b[k] = a.b[k + n];
    return r;
}

static inline u32 op_movmskps(v128 a) { return (a.u[0] >> 31) | ((a.u[1] >> 31) << 1) | ((a.u[2] >> 31) << 2) | ((a.u[3] >> 31) << 3); }
static inline u32 op_movmskpd(v128 a) { return (u32)(a.q[0] >> 63) | ((u32)(a.q[1] >> 63) << 1); }
static inline u32 op_pmovmskb(v128 a) { u32 r = 0; for (int k = 0; k < 16; k++) r |= (u32)(a.b[k] >> 7) << k; return r; }

// x87 integer conversions and rounding, using the rounding control in the FPU control word.
static inline double rt_frnd(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
        case 1: return floor(v);
        case 2: return ceil(v);
        case 3: return trunc(v);
        default: return rint(v);
    }
}
static inline u16 rt_f2i16(double v) { return (v > -32769.0 && v < 32768.0) ? (u16)(i16)v : 0x8000; }
static inline u32 rt_f2i32(double v) { return (v > -2147483649.0 && v < 2147483648.0) ? (u32)(i32)v : 0x80000000u; }
static inline u64 rt_f2i64(double v) { return (v >= -9223372036854775808.0 && v < 9223372036854775808.0) ? (u64)(i64)v : 0x8000000000000000ull; }
static inline void rt_fcom(CPU *c, double a, double b) {
    u16 s = c->fsw & (u16)~0x4500u;
    if (a != a || b != b) s |= 0x4500;
    else if (a < b) s |= 0x0100;
    else if (a == b) s |= 0x4000;
    c->fsw = s;
}
static inline u16 rt_fsw(CPU *c) { return (u16)((c->fsw & 0xC7FFu) | ((c->top & 7) << 11)); }
