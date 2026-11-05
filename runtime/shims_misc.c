// Registration of all shim tables and per-process guest state setup.
#include "shim.h"
#include "shim_reg.h"

#include <ctype.h>
#include <runetype.h>
#include <stdio.h>
#include <stdlib.h>

// malloc zones: the game's bundled tcmalloc tries to install itself as the system allocator. The host
// malloc shims never route through zones, so a harmless placeholder object is enough.
static u32 dummy_zone;
static u32 zone(void) {
    if (!dummy_zone) dummy_zone = rt_guest_calloc(1, 256);
    return dummy_zone;
}
SHIM(malloc_default_zone) { RET(zone()); }
SHIM(malloc_create_zone) { RET(zone()); }
SHIM(malloc_zone_noop) { RET(0); }
SHIM(malloc_zone_free) { rt_guest_free(ARG(1)); }

const ShimReg rt_shims_misc[] = {
    R(malloc_default_zone) R2("_malloc_default_purgeable_zone", malloc_default_zone) R(malloc_create_zone)
    R2("_malloc_destroy_zone", malloc_zone_noop) R2("_malloc_set_zone_name", malloc_zone_noop)
    R2("_malloc_zone_register", malloc_zone_noop) R2("_malloc_zone_unregister", malloc_zone_noop) R(malloc_zone_free)
    {NULL, NULL},
};

static void add_table(void (**table)(CPU *), const ShimReg *r) {
    for (; r->name; r++) {
        int i = rt_import_index(r->name);
        if (i >= 0) table[i] = r->fn;
    }
}

void rt_register_shims(void (**table)(CPU *)) {
    add_table(table, rt_shims_libc);
    add_table(table, rt_shims_threads);
    add_table(table, rt_shims_cxx);
    add_table(table, rt_shims_misc);
    add_table(table, rt_shims_cf);
    add_table(table, rt_shims_objc);
    add_table(table, rt_shims_gl_gen);
    add_table(table, rt_shims_gl);
}

// Debug hooks from config/hooks.json: logs arguments, CF strings reachable from them, and the result.
#include <CoreFoundation/CoreFoundation.h>
extern CFTypeRef cf_unwrap(u32 g);
static void describe_word(u32 v, int depth) {
    if (rt_is_handle(v)) {
        CFTypeRef o = rt_handle_get(v);
        if (o && CFGetTypeID(o) == CFStringGetTypeID()) {
            char b[200] = "";
            CFStringGetCString(o, b, sizeof b, kCFStringEncodingUTF8);
            fprintf(stderr, " \"%s\"", b);
        }
        return;
    }
    if (depth < 2 && v >= 0x04000000u && v < 0xE0000000u) {
        for (u32 i = 0; i < 28; i++) {
            u32 w = G32(v + i * 4);
            if (rt_is_handle(w) || (depth == 0 && w >= 0x04000000u && w < 0xE0000000u)) describe_word(w, depth + 1);
        }
    }
}
static __thread int hook_depth;
void rt_hook_enter(CPU *c, u32 addr) {
    hook_depth++;
    if (getenv("ARKHAM_HOOK_LIST2") && addr == strtoul(getenv("ARKHAM_HOOK_LIST2"), NULL, 16)) {
        u32 x = G32(c->esp + 4), head = x + 4, n = G32(head);
        int cnt = 0;
        for (; n != head && cnt < 300; cnt++, n = G32(n)) {
            if (n < 0x04000000u || n >= 0xE0000000u) {
                fprintf(stderr, "[list2] bad next %08x after %d nodes\n", n, cnt);
                break;
            }
            if (G32(n + 8) < 0x03000000u) fprintf(stderr, "[list2] node %d at %08x has vtable %08x next %08x prev %08x\n", cnt, n, G32(n + 8), G32(n), G32(n + 4));
        }
        fprintf(stderr, "[list2] walked %d nodes, end=%08x head=%08x\n", cnt, n, head);
    }
    fprintf(stderr, "[hook] d%d > %07x ret=%08x args", hook_depth, addr, G32(c->esp));
    for (u32 i = 0; i < 4; i++) fprintf(stderr, " %08x", G32(c->esp + 4 + i * 4));
    if (getenv("ARKHAM_HOOK_DEEP")) {
        describe_word(G32(c->esp + 4), 0);
        // C++ receiver: name its class from RTTI and show a few virtual slots.
        u32 self = G32(c->esp + 4);
        if (self >= 0x04000000u && self < 0xE0000000u) {
            u32 vt = G32(self);
            if (vt >= 0x034987a0u && vt < 0x03640000u) {
                u32 ti = G32(vt - 4);
                if (ti >= 0x03000000u && ti < 0x03800000u) fprintf(stderr, " class=%s", GS(G32(ti + 4)));
                fprintf(stderr, " vt[0x28]=%08x vt[0x2c]=%08x vt[0x30]=%08x", G32(vt + 0x28), G32(vt + 0x2c), G32(vt + 0x30));
            }
        }
    }
    fprintf(stderr, "\n");
}
void rt_hook_leave(CPU *c, u32 addr) {
    fprintf(stderr, "[hook] d%d < %07x eax=%08x\n", hook_depth, addr, c->eax);
    if (getenv("ARKHAM_HOOK_LIST") && addr == strtoul(getenv("ARKHAM_HOOK_LIST"), NULL, 16)) {
        // Walk the std::list at arg0+4 (the callee already popped its return address).
        u32 x = G32(c->esp), head = x + 4, n = G32(head);
        fprintf(stderr, "[list] x=%08x head=%08x first=%08x\n", x, head, n);
        for (int i = 0; n != head && i < 12; i++, n = G32(n)) {
            u32 vt = G32(n + 8);
            u32 ti = vt >= 0x03000000u && vt < 0x03900000u ? G32(vt - 4) : 0;
            fprintf(stderr, "[list]   node %08x next=%08x prev=%08x vt=%08x %s\n", n, G32(n), G32(n + 4), vt, ti >= 0x03000000u && ti < 0x03900000u ? GS(G32(ti + 4)) : "");
        }
    }
    hook_depth--;
}

int rt_host_thunk_dispatch(CPU *c, u32 target) {
    (void)c;
    (void)target;
    return 0;
}

// Point every bound reference of a data import at a larger guest object.
static void patch_import(const char *name, u32 guest) {
    int idx = rt_import_index(name);
    if (idx < 0) return;
    for (unsigned i = 0; i < rt_binds_n; i++) {
        if ((int)rt_binds[i].import == idx) *(u32a *)(rt_mem + rt_binds[i].addr) = guest + (u32)rt_binds[i].addend;
    }
}

void rt_shims_process_init(CPU *c, int argc, u32 argv, u32 envp) {
    (void)argc, (void)argv, (void)envp;
    rt_libc_init(c);
    rt_cxx_init(c);
    rt_objc_init();
    rt_gl_init();
    rt_cf_init(rt_cfstring_addr, rt_cfstring_addr + rt_cfstring_size);
    // The first host page stays unmapped to catch NULL dereferences, so keep the Mach header readable elsewhere.
    extern const u8 *rt_header_bytes;
    u32 hdr = rt_guest_malloc(0xC000);
    memcpy(GP(hdr), rt_header_bytes, 0xC000);
    rt_set_header_copy(hdr);
    // ctype tables, i386 layout: magic[8], encoding[32], two pointers, invalid_rune, then the tables.
    u32 rune = rt_guest_calloc(1, 52 + 1024 * 3 + 64);
    for (u32 i = 0; i < 256; i++) {
        G32(rune + 52 + i * 4) = _DefaultRuneLocale.__runetype[i];
        G32(rune + 52 + 1024 + i * 4) = (u32)_DefaultRuneLocale.__maplower[i];
        G32(rune + 52 + 2048 + i * 4) = (u32)_DefaultRuneLocale.__mapupper[i];
    }
    patch_import("__DefaultRuneLocale", rune);
}
