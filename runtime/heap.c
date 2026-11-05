// Page and block allocators that live inside the guest window, so every result fits in 32 bits.
// All callers hold the guest gate, so no locking is needed here.
#include "shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define ARENA_START 0x04000000u
#define ARENA_END 0xE0000000u
#define PAGE 4096u
#define PAGE_UP(x) (((x) + PAGE - 1) & ~(PAGE - 1))

typedef struct FreeRange {
    u32 addr, size;
    struct FreeRange *next;
} FreeRange;

static u32 bump = ARENA_START;
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
        _Exit(74);
    }
    u32 a = bump;
    bump += size;
    return a;
}

void rt_guest_free_pages(u32 addr, u32 size) {
    size = PAGE_UP(size);
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

// Block allocator: power-of-two size classes with a 16-byte header holding the class and size.
#define HDR 16u
#define NCLASS 17  // classes up to LARGE: 16 << 16 is 1 MB
#define LARGE (1u << 20)
#define MAGIC 0xA110C8EDu
#define FREE_MAGIC 0xF4EEB10Cu
// Links live in the header, so a guest writing to a block it already freed can't corrupt the free list.

static u32 class_head[NCLASS];
static u32 chunk_cur, chunk_end;

static int class_of(u32 n) {
    u32 s = 16;
    for (int i = 0; i < NCLASS; i++, s <<= 1)
        if (n <= s) return i;
    return -1;
}

void rt_heap_init(void) {}

u32 rt_guest_malloc(u32 size) {
    if (!size) size = 1;
    if (size >= LARGE) {
        u32 total = PAGE_UP(size + HDR);
        u32 a = rt_guest_alloc_pages(total);
        G32(a) = MAGIC;
        G32(a + 4) = 0xFFFFFFFFu;
        G32(a + 8) = size;
        G32(a + 12) = total;
        return a + HDR;
    }
    int k = class_of(size);
    u32 cls_size = 16u << k;
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
            u32 sz = need > (1u << 20) ? need : (1u << 20);
            chunk_cur = rt_guest_alloc_pages(sz);
            chunk_end = chunk_cur + PAGE_UP(sz);
        }
        a = chunk_cur + HDR;
        chunk_cur += need;
    }
    u32 h = a - HDR;
    G32(h) = MAGIC;
    G32(h + 4) = (u32)k;
    G32(h + 8) = size;
    return a;
}

u32 rt_guest_block_size(u32 a) {
    if (!a) return 0;
    u32 h = a - HDR;
    if (G32(h) != MAGIC) return 0;
    return G32(h + 8);
}

void rt_guest_free(u32 a) {
    if (!a) return;
    u32 h = a - HDR;
    if (G32(h) != MAGIC) {
        if (G32(h) != FREE_MAGIC) fprintf(stderr, "[heap] free of a non-heap guest pointer %08x\n", a);
        return;  // double frees are ignored, as they often are in practice
    }
    u32 k = G32(h + 4);
    if (k == 0xFFFFFFFFu) {
        G32(h) = 0;
        rt_guest_free_pages(h, G32(h + 12));
        return;
    }
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
