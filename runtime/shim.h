// Host implementations of the imports the game calls, plus helpers for guest argument access.
#pragma once
#include "rt.h"
#include <TargetConditionals.h>  // TARGET_OS_IPHONE: the phone build, tools/build.py --ios

#define RT_HOST_RETURN 0xEEFFFFF0u

CPU *rt_current_cpu(void);
CPU *rt_new_cpu(u32 stack_size);
void rt_bind_cpu(CPU *c);
u32 rt_call_guest(CPU *c, u32 addr, int nargs, const u32 *args);
int rt_import_index(const char *name);
const char *rt_game_root(void);
// The game's main thread and run loop (rt.c): the process's main thread on the Mac, the game's own thread on the phone.
struct __CFRunLoop *rt_guest_main_loop(void);
int rt_on_guest_main(void);
u32 rt_import_addr(const char *name);
void rt_gate_acquire(void);
void rt_gate_acquire_now(void);
void rt_gate_release(void);
void rt_gate_acquire_serial(void);  // these two only do something with ARKHAM_SERIAL, when running guest code takes the gate
void rt_gate_release_serial(void);
int rt_gate_held(void);
void rt_glq_drain(void);
int rt_host_thunk_dispatch(CPU *c, u32 target);

// Guest heap and page allocator (lives inside the 4 GB guest window).
void rt_heap_init(void);
u32 rt_guest_alloc_pages(u32 size);
void rt_guest_free_pages(u32 addr, u32 size);
u32 rt_guest_malloc(u32 size);
u32 rt_guest_memalign(u32 align, u32 size);
u32 rt_guest_calloc(u32 n, u32 size);
u32 rt_guest_realloc(u32 addr, u32 size);
void rt_guest_free(u32 addr);
u32 rt_guest_block_size(u32 addr);
u32 rt_guest_strdup(const char *s);

void rt_shims_process_init(CPU *c, int argc, u32 argv, u32 envp);

// Argument 0 is at [esp+4] when a shim runs: [esp] holds the return address.
#define GMEM rt_mem
#define ARG(n) (*(u32a *)(GMEM + c->esp + 4u + (u32)(n) * 4u))
#define ARGQ(n) (*(u64a *)(GMEM + c->esp + 4u + (u32)(n) * 4u))
#define ARGF(n) (*(f32a *)(GMEM + c->esp + 4u + (u32)(n) * 4u))
#define ARGD(n) (*(f64a *)(GMEM + c->esp + 4u + (u32)(n) * 4u))
#define GP(a) ((void *)(GMEM + (u32)(a)))
#define GS(a) ((const char *)(GMEM + (u32)(a)))
#define NP(a) ((a) ? (void *)(GMEM + (u32)(a)) : NULL)
#define NS(a) ((a) ? (const char *)(GMEM + (u32)(a)) : NULL)
#define G32(a) (*(u32a *)(GMEM + (u32)(a)))
#define G16(a) (*(u16a *)(GMEM + (u32)(a)))
#define G8(a) (*(u8 *)(GMEM + (u32)(a)))
#define TOGUEST(p) ((u32)((const u8 *)(p)-GMEM))
#define RET(v) (c->eax = (u32)(v))
#define RET64(v) (c->eax = (u32)(u64)(v), c->edx = (u32)((u64)(v) >> 32))
#define RETF(v) FPUSH((double)(v))

#define SHIM(name) static void shim_##name(CPU *c)
