// Core definitions shared by generated code and the runtime.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float vf4 __attribute__((vector_size(16)));
typedef double vd2 __attribute__((vector_size(16)));
typedef u32 vu4 __attribute__((vector_size(16)));
typedef i32 vi4 __attribute__((vector_size(16)));
typedef u64 vq2 __attribute__((vector_size(16)));

typedef union v128 {
    float f[4];
    double d[2];
    u32 u[4];
    i32 i[4];
    u64 q[2];
    u16 w[8];
    i16 s[8];
    u8 b[16];
    i8 c[16];
    vf4 vf;
    vd2 vd;
    vu4 vu;
    vi4 vi;
    vq2 vq;
} v128;

typedef struct CPU {
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 df, mxcsr, jt, gsbase;
    v128 x[8];
    double st[8];
    u32 top;
    u16 fcw, fsw;
    void *thread;
} CPU;

typedef void (*guest_fn)(CPU *restrict);
typedef struct { u32 addr; guest_fn fn; } rt_fn_entry;
typedef struct { u32 addr; u32 import; i32 addend; } rt_bind_entry;
typedef struct { u32 vmaddr, vmsize, fileoff, filesize; } rt_seg_entry;

extern u8 *rt_mem;
extern volatile u32 rt_gate_wait;

extern const rt_fn_entry rt_fntab[];
extern const unsigned rt_fntab_n;
extern const char *const rt_import_names[];
extern const unsigned rt_import_n;
extern const rt_bind_entry rt_binds[];
extern const unsigned rt_binds_n;
extern const unsigned rt_entry_addr, rt_modinit_addr, rt_modinit_n;
extern const rt_seg_entry rt_segs[];
extern const unsigned rt_segs_n;
extern const unsigned rt_cfstring_addr, rt_cfstring_size;

// Trap and control-flow entry points used by generated code.
_Noreturn void rt_trap(CPU *c, u32 addr, const char *msg);
_Noreturn void rt_unimpl(CPU *c, u32 addr, const char *msg);
void rt_imp(CPU *c, u32 idx);
void rt_dispatch(CPU *c, u32 target);
void rt_dispatch_jt(CPU *restrict c);
void rt_gate_yield(void);
void rt_hook_enter(CPU *c, u32 addr);
void rt_hook_leave(CPU *c, u32 addr);
void rt_ldmxcsr(CPU *c, u32 v);
void rt_cpuid(CPU *c);
void rt_fninit(CPU *c);
double rt_ld80(const u8 *p);
void rt_st80(u8 *p, double v);

typedef u16 __attribute__((aligned(1), may_alias)) u16a;
typedef u32 __attribute__((aligned(1), may_alias)) u32a;
typedef u64 __attribute__((aligned(1), may_alias)) u64a;
typedef float __attribute__((aligned(1), may_alias)) f32a;
typedef double __attribute__((aligned(1), may_alias)) f64a;

#define R8(a) (*(u8 *)(M + (u32)(a)))
#define R16(a) (*(u16a *)(M + (u32)(a)))
#define R32(a) (*(u32a *)(M + (u32)(a)))
#define R64(a) (*(u64a *)(M + (u32)(a)))
#define RF(a) (*(f32a *)(M + (u32)(a)))
#define RD(a) (*(f64a *)(M + (u32)(a)))
#define X(n) (c->x[n])
#define GSBASE (c->gsbase)

#define LOCALS                                                                                   \
    u8 *const M = rt_mem;                                                                        \
    u32 eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, ebp = c->ebp,      \
        esi = c->esi, edi = c->edi;                                                              \
    u32 cf = 0, zf = 0, sf = 0, of = 0, pf = 0, af = 0;                                          \
    (void)cf, (void)zf, (void)sf, (void)of, (void)pf, (void)af
#define SAVE                                                                                     \
    c->eax = eax, c->ecx = ecx, c->edx = edx, c->ebx = ebx, c->esp = esp, c->ebp = ebp,          \
    c->esi = esi, c->edi = edi
#define LOAD                                                                                     \
    eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, ebp = c->ebp,          \
    esi = c->esi, edi = c->edi
#define POLL                                                                                     \
    do {                                                                                         \
        if (__builtin_expect(rt_gate_wait != 0, 0)) rt_gate_yield();                             \
    } while (0)
#define PAR(r) (!__builtin_parity((u8)(r)))
#define MINV(a, b) ((a) < (b) ? (a) : (b))
#define MAXV(a, b) ((a) > (b) ? (a) : (b))
#define RSQRTF(x) (1.0f / __builtin_sqrtf(x))
#define RCPF(x) (1.0f / (x))

#define ST(i) (c->st[(c->top + (i)) & 7])
#define FPUSH(v)                                                                                 \
    do {                                                                                         \
        double v_ = (v);                                                                         \
        c->top = (c->top - 1) & 7;                                                               \
        c->st[c->top] = v_;                                                                      \
    } while (0)
#define FPOP() (c->top = (c->top + 1) & 7)

#define LDV(a) rt_ldv(M + (u32)(a))
#define STV(a, v) rt_stv(M + (u32)(a), (v))

static inline v128 rt_ldv(const u8 *p) {
    v128 r;
    __builtin_memcpy(&r, p, 16);
    return r;
}
static inline void rt_stv(u8 *p, v128 v) { __builtin_memcpy(p, &v, 16); }

#include "rt_ops.h"
