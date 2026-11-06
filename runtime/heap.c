// Page and block allocators that live inside the guest window, so every result fits in 32 bits.
// Guest threads run in parallel, so everything public here takes the heap's own lock.
#include "shim.h"

#include <os/lock.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

static os_unfair_lock heap_lock = OS_UNFAIR_LOCK_INIT;
// Inside this file these names mean the versions that expect the lock to be held; the public ones are at the end.
#define rt_guest_alloc_pages h_alloc_pages
#define rt_guest_free_pages h_free_pages
#define rt_guest_malloc h_malloc
#define rt_guest_memalign h_memalign
#define rt_guest_block_size h_block_size
#define rt_guest_free h_free
#define rt_guest_calloc h_calloc
#define rt_guest_realloc h_realloc
#define rt_guest_strdup h_strdup

#define ARENA_START 0x04000000u
#define ARENA_END 0xE0000000u
#define PAGE 4096u
#define PAGE_UP(x) (((x) + PAGE - 1) & ~(PAGE - 1))

typedef struct FreeRange {
    u32 addr, size;
    struct FreeRange *next;
} FreeRange;

static u32 bump = ARENA_START;
static void heap_report(void);
static FreeRange *free_ranges;

u32 rt_guest_alloc_pages(u32 size) {
    size = PAGE_UP(size);
    FreeRange **pp = &free_ranges;
    for (FreeRange *f = free_ranges; f; pp = &f->next, f = f->next) {
        if (f->size >= size) {
            u32 a = f->addr;
            f->addr += size;
            f->size -= size;
            if (!f->size) {
                *pp = f->next;
                free(f);
            }
            memset(rt_mem + a, 0, size);
            return a;
        }
    }
    if (bump + size > ARENA_END || bump + size < bump) {
        fprintf(stderr, "*** guest address space exhausted allocating %u bytes\n", size);
        heap_report();
        _Exit(74);
    }
    u32 a = bump;
    bump += size;
    return a;
}

void rt_guest_free_pages(u32 addr, u32 size) {
    size = PAGE_UP(size);
    // Give the physical memory of a large freed range back to the system. Reuse zeroes it anyway.
    if (size >= (1u << 20)) {
        u64 lo = ((u64)addr + 0x3FFFu) & ~0x3FFFull, hi = ((u64)addr + size) & ~0x3FFFull;
        if (hi > lo) madvise(rt_mem + lo, (size_t)(hi - lo), MADV_FREE);
    }
    FreeRange **pp = &free_ranges;
    FreeRange *f = free_ranges;
    while (f && f->addr < addr) {
        pp = &f->next;
        f = f->next;
    }
    FreeRange *n = malloc(sizeof *n);
    n->addr = addr;
    n->size = size;
    n->next = f;
    *pp = n;
    if (f && addr + size == f->addr) {
        n->size += f->size;
        n->next = f->next;
        free(f);
    }
    // Merge with the predecessor when adjacent.
    for (FreeRange *p = free_ranges; p; p = p->next) {
        if (p->next == n && p->addr + p->size == n->addr) {
            p->size += n->size;
            p->next = n->next;
            free(n);
            break;
        }
    }
}

// Block allocator. Blocks up to SMALL_MAX come from finely spaced size classes carved out of 1 MB chunks, so little is
// lost to rounding. Anything larger takes whole pages, which go back to the page allocator and merge when freed.
#define HDR 16u
#define SMALL_MAX (32u * 1024u)
#define NCLASS (65 + 5 * 8 + 1)
#define MAGIC 0xA110C8EDu
#define FREE_MAGIC 0xF4EEB10Cu
// Links live in the header, so a guest writing to a block it already freed can't corrupt the free list.

static u32 class_head[NCLASS];
static u32 chunk_cur, chunk_end;
static u64 large_live, chunk_total;
static u32 untracked_allocs;

// Classes step by 16 bytes up to 1 KB, then eight to each doubling.
static inline int class_of(u32 n) {
    if (n <= 1024) return (int)((n + 15) >> 4);
    u32 shift = 31u - (u32)__builtin_clz(n - 1);
    u32 idx = (n - 1 - (1u << shift)) >> (shift - 3);
    return (int)(65 + (shift - 10) * 8 + idx);
}
static inline u32 class_size(int k) {
    if (k <= 64) return (u32)k * 16u;
    u32 shift = 10u + ((u32)k - 65u) / 8u;
    u32 idx = ((u32)k - 65u) % 8u;
    return (1u << shift) + (idx + 1u) * (1u << (shift - 3));
}

// Allocation profile for leak hunting (ARKHAM_TRACE_HEAP=1): live bytes per guest call chain.
typedef struct { u32 a, b, c; long long live, count; u32 last; u32 chain[8]; } HeapSite;
#define SITES 8192
static HeapSite sites[SITES];
static int heap_trace = -1;
static u64 heap_live, heap_bump_report;
static void heap_report(void) {
    int top[16] = {0};
    for (int i = 0; i < SITES; i++) {
        for (int j = 0; j < 16; j++) {
            if (sites[i].live > sites[top[j]].live) {
                memmove(&top[j + 1], &top[j], (size_t)(15 - j) * sizeof top[0]);
                top[j] = i;
                break;
            }
        }
    }
    u64 free_bytes = 0;
    for (FreeRange *f = free_ranges; f; f = f->next) free_bytes += f->size;
    u64 class_free = 0;
    for (int k = 1; k < NCLASS; k++) {
        u32 n = 0;
        for (u32 a = class_head[k]; a && n < 2000000u; a = G32(a - HDR + 12)) n++;
        class_free += (u64)n * class_size(k);
    }
    fprintf(stderr, "[heap] tracked live %.0f MB, arena used %.0f MB, free page ranges %.0f MB, free small blocks %.0f MB, large live %.0f MB, chunks %.0f MB, untracked allocs %u\n", (double)heap_live / 1e6,
            (double)(bump - ARENA_START) / 1e6, (double)free_bytes / 1e6, (double)class_free / 1e6, (double)large_live / 1e6, (double)chunk_total / 1e6, untracked_allocs);
    for (int j = 0; j < 16; j++) {
        HeapSite *h = &sites[top[j]];
        if (h->live <= 0) continue;
        fprintf(stderr, "[heap]   %8.1f MB in %7lld blocks from %08x < %08x < %08x", (double)h->live / 1e6, h->count, h->a, h->b, h->c);
        // What the newest block holds and how it got here tell what is piling up.
        if (j < 4 && h->last) {
            char text[80];
            u32 n = 0;
            for (; n < 76; n++) {
                u16 ch = G16(h->last + n * 2);
                if (ch < 32 || ch > 126) break;
                text[n] = (char)ch;
            }
            text[n] = 0;
            fprintf(stderr, " text='%s' via", text);
            for (int k = 0; k < 8 && h->chain[k]; k++) fprintf(stderr, " %08x", h->chain[k]);
        }
        fprintf(stderr, "\n");
    }
}
// Returns a site number to keep with the block.
static HeapSite *heap_last_site;
static u32 heap_note_alloc(u32 size) {
    if (heap_trace < 0) heap_trace = getenv("ARKHAM_TRACE_HEAP") != NULL;
    if (!heap_trace) return 0;
    CPU *c = rt_current_cpu();
    if (!c) return 0;
    u32 a = G32(c->esp), b = 0, d = 0;
    u32 ebp = c->ebp;
    if (ebp > 0x1000u && ebp < 0xE0000000u) {
        b = G32(ebp + 4);
        u32 up = G32(ebp);
        if (up > 0x1000u && up < 0xE0000000u) d = G32(up + 4);
    }
    u32 hash = (a * 31u + b * 17u + d) % SITES;
    for (u32 i = 0; i < 64; i++) {
        u32 k = (hash + i) % SITES;
        HeapSite *h = &sites[k];
        if (!h->a || (h->a == a && h->b == b && h->c == d)) {
            h->a = a;
            h->b = b;
            h->c = d;
            h->live += size;
            h->count++;
            heap_live += size;
            u32 fp = ebp;
            for (int n = 0; n < 8; n++) {
                if (fp < 0x1000u || fp >= 0xE0000000u) {
                    h->chain[n] = 0;
                    break;
                }
                h->chain[n] = G32(fp + 4);
                fp = G32(fp);
            }
            heap_last_site = h;
            if (bump - heap_bump_report > 200000000u) {
                heap_bump_report = bump;
                heap_report();
            }
            return k + 1;
        }
    }
    untracked_allocs++;
    return 0;
}
static void heap_note_block(u32 block) {
    if (heap_last_site) heap_last_site->last = block;
    heap_last_site = NULL;
}
static void heap_note_free(u32 site, u32 size) {
    if (heap_trace <= 0 || !site || site > SITES) return;
    sites[site - 1].live -= size;
    sites[site - 1].count--;
    heap_live -= size;
}

void rt_heap_init(void) {}

u32 rt_guest_malloc(u32 size) {
    if (!size) size = 1;
    if (size > SMALL_MAX) {
        u32 total = PAGE_UP(size + HDR);
        if (total < size) {
            fprintf(stderr, "*** guest allocation of %u bytes is too large\n", size);
            _Exit(74);
        }
        u32 a = rt_guest_alloc_pages(total);
        large_live += total;
        G32(a) = MAGIC;
        G32(a + 4) = 0xFFFF0000u | heap_note_alloc(size);
        G32(a + 8) = size;
        G32(a + 12) = total;
        heap_note_block(a + HDR);
        return a + HDR;
    }
    int k = class_of(size);
    u32 cls_size = class_size(k);
    u32 a = class_head[k];
    while (a && G32(a - HDR) != FREE_MAGIC) {
        // A neighbouring block overran this header: abandon the rest of this list rather than trust it.
        fprintf(stderr, "[heap] corrupt free block header at %08x in class %d, discarding that list\n", a, k);
        a = 0;
        class_head[k] = 0;
    }
    if (a) {
        class_head[k] = G32(a - HDR + 12);
    } else {
        u32 need = cls_size + HDR;
        if (chunk_cur + need > chunk_end) {
            chunk_cur = rt_guest_alloc_pages(1u << 20);
            chunk_end = chunk_cur + (1u << 20);
            chunk_total += 1u << 20;
        }
        a = chunk_cur + HDR;
        chunk_cur += need;
    }
    u32 h = a - HDR;
    G32(h) = MAGIC;
    G32(h + 4) = (u32)k;
    G32(h + 8) = size;
    G32(h + 12) = heap_note_alloc(size);
    heap_note_block(a);
    return a;
}

// Over-aligned blocks: a small header just before the aligned address points back at the real block.
#define ALIGNED_MAGIC 0xA11C0DEDu
u32 rt_guest_memalign(u32 align, u32 size) {
    if (align <= 16) return rt_guest_malloc(size);
    u32 raw = rt_guest_malloc(size + align + HDR);
    u32 a = (raw + HDR + align - 1) & ~(align - 1);
    G32(a - HDR) = ALIGNED_MAGIC;
    G32(a - HDR + 4) = raw;
    G32(a - HDR + 8) = size;
    return a;
}

u32 rt_guest_block_size(u32 a) {
    if (!a) return 0;
    u32 h = a - HDR;
    if (G32(h) != MAGIC && G32(h) != ALIGNED_MAGIC) return 0;
    return G32(h + 8);
}

// How much the block can hold without moving.
static u32 block_capacity(u32 a) {
    u32 h = a - HDR;
    if (G32(h) == ALIGNED_MAGIC) return G32(h + 8);
    u32 k = G32(h + 4);
    if ((k >> 16) == 0xFFFFu) return G32(h + 12) - HDR;
    return class_size((int)k);
}

void rt_guest_free(u32 a) {
    if (!a) return;
    u32 h = a - HDR;
    if (G32(h) == ALIGNED_MAGIC) {
        u32 raw = G32(h + 4);
        G32(h) = 0;
        rt_guest_free(raw);
        return;
    }
    if (G32(h) != MAGIC) {
        if (G32(h) != FREE_MAGIC) fprintf(stderr, "[heap] free of a non-heap guest pointer %08x\n", a);
        return;  // double frees are ignored, as they often are in practice
    }
    u32 k = G32(h + 4);
    if ((k >> 16) == 0xFFFFu) {
        heap_note_free(k & 0xFFFFu, G32(h + 8));
        G32(h) = 0;
        large_live -= G32(h + 12);
        rt_guest_free_pages(h, G32(h + 12));
        return;
    }
    heap_note_free(G32(h + 12), G32(h + 8));
    G32(h) = FREE_MAGIC;
    G32(h + 12) = class_head[k];
    class_head[k] = a;
}

u32 rt_guest_calloc(u32 n, u32 size) {
    u64 t = (u64)n * size;
    if (t >> 32) return 0;
    u32 a = rt_guest_malloc((u32)t);
    if (a) memset(rt_mem + a, 0, (size_t)t);
    return a;
}

u32 rt_guest_realloc(u32 a, u32 size) {
    if (!a) return rt_guest_malloc(size);
    u32 old = rt_guest_block_size(a);
    // A block that already fits stays put, unless shrinking would strand most of a page-sized block.
    if (old && size && G32(a - HDR) == MAGIC && size <= block_capacity(a) && (size > SMALL_MAX || block_capacity(a) <= SMALL_MAX)) {
        if (heap_trace > 0) {
            // Re-attribute the size change to the block's own site.
            u32 k = G32(a - HDR + 4);
            u32 site = (k >> 16) == 0xFFFFu ? (k & 0xFFFFu) : G32(a - HDR + 12);
            heap_note_free(site, old);
            if (site && site <= SITES) {
                sites[site - 1].live += size;
                sites[site - 1].count++;
                heap_live += size;
            }
        }
        G32(a - HDR + 8) = size;
        return a;
    }
    u32 n = rt_guest_malloc(size);
    if (n) memcpy(rt_mem + n, rt_mem + a, old < size ? old : size);
    rt_guest_free(a);
    return n;
}

u32 rt_guest_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    u32 a = rt_guest_malloc((u32)n);
    memcpy(rt_mem + a, s, n);
    return a;
}

#undef rt_guest_alloc_pages
#undef rt_guest_free_pages
#undef rt_guest_malloc
#undef rt_guest_memalign
#undef rt_guest_block_size
#undef rt_guest_free
#undef rt_guest_calloc
#undef rt_guest_realloc
#undef rt_guest_strdup
#define LOCKED(expr) do { os_unfair_lock_lock(&heap_lock); expr; os_unfair_lock_unlock(&heap_lock); } while (0)
u32 rt_guest_alloc_pages(u32 size) { u32 r; LOCKED(r = h_alloc_pages(size)); return r; }
void rt_guest_free_pages(u32 addr, u32 size) { LOCKED(h_free_pages(addr, size)); }
u32 rt_guest_malloc(u32 size) { u32 r; LOCKED(r = h_malloc(size)); return r; }
u32 rt_guest_memalign(u32 align, u32 size) { u32 r; LOCKED(r = h_memalign(align, size)); return r; }
u32 rt_guest_block_size(u32 a) { u32 r; LOCKED(r = h_block_size(a)); return r; }
void rt_guest_free(u32 a) { LOCKED(h_free(a)); }
u32 rt_guest_calloc(u32 n, u32 size) { u32 r; LOCKED(r = h_calloc(n, size)); return r; }
u32 rt_guest_realloc(u32 a, u32 size) { u32 r; LOCKED(r = h_realloc(a, size)); return r; }
u32 rt_guest_strdup(const char *s) { u32 r; LOCKED(r = h_strdup(s)); return r; }
