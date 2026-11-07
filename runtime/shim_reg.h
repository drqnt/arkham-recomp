#pragma once
#include "rt.h"
typedef struct { const char *name; void (*fn)(CPU *); } ShimReg;
#define R(n) {"_" #n, shim_##n},
#define R2(sym, n) {sym, shim_##n},
u32 rt_handle_new(void *p);
void *rt_handle_get(u32 h);
void rt_handle_free(u32 h);
int rt_is_handle(u32 h);
void rt_event_notify(void);
extern const ShimReg rt_shims_libc[];
extern const ShimReg rt_shims_threads[];
extern const ShimReg rt_shims_cxx[];
extern const ShimReg rt_shims_misc[];
extern const ShimReg rt_shims_cf[];
extern const ShimReg rt_shims_objc[];
void rt_objc_init(void);
extern const ShimReg rt_shims_gl[];
extern const ShimReg rt_shims_gl_gen[];
void rt_gl_init(void);
extern const ShimReg rt_shims_text[];
extern const ShimReg rt_shims_eh[];
extern const ShimReg rt_shims_sys[];
extern const ShimReg rt_shims_bink[];
extern const ShimReg rt_shims_hid[];
extern const ShimReg rt_shims_audio[];
extern const ShimReg rt_shims_bink_exports[];
void rt_text_init(void);
void rt_cf_init(u32 cfstring_start, u32 cfstring_end);
void rt_set_header_copy(u32 g);
void rt_libc_init(CPU *c);
void rt_cxx_init(CPU *c);
void rt_misc_init(CPU *c);
