// ARKHAM_D3D_CENSUS=file: counts what the engine asks of Direct3D, with the arguments that decide what a replacement for Feral's layer has to do, and
// which OpenGL calls come from outside that layer. Rewrites the file every 20 seconds, sorted by name.
#include "shim.h"

#include <os/lock.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOTS (1u << 16)
static struct {
    char *key;
    u64 n;
} table[SLOTS];
static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
static const char *path;
int rt_d3d_census_on = -1;
static u64 last_dump;

static int by_key(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void dump(void) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    static char *keys[SLOTS];
    u32 n = 0;
    for (u32 i = 0; i < SLOTS; i++)
        if (table[i].key) keys[n++] = table[i].key;
    qsort(keys, n, sizeof keys[0], by_key);
    for (u32 k = 0; k < n; k++) {
        u32 h = 2166136261u;
        for (const char *p = keys[k]; *p; p++) h = (h ^ (u8)*p) * 16777619u;
        for (u32 i = h % SLOTS;; i = (i + 1) % SLOTS) {
            if (table[i].key == keys[k] || !strcmp(table[i].key, keys[k])) {
                fprintf(f, "%12llu %s\n", table[i].n, keys[k]);
                break;
            }
        }
    }
    fclose(f);
}
static void note(const char *fmt, ...) {
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    u32 h = 2166136261u;
    for (const char *p = b; *p; p++) h = (h ^ (u8)*p) * 16777619u;
    os_unfair_lock_lock(&lock);
    for (u32 i = h % SLOTS, tries = 0; tries < SLOTS; i = (i + 1) % SLOTS, tries++) {
        if (!table[i].key) table[i].key = strdup(b);
        if (!strcmp(table[i].key, b)) {
            table[i].n++;
            break;
        }
    }
    u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000000;
    if (now - last_dump >= 20) {
        last_dump = now;
        dump();
    }
    os_unfair_lock_unlock(&lock);
}
static inline u32 arg(CPU *c, int i) { return G32(c->esp + 4 + 4u * (u32)i); }

int rt_d3d_census_start(void) {
    path = getenv("ARKHAM_D3D_CENSUS");
    rt_d3d_census_on = path != NULL;
    return rt_d3d_census_on;
}
void rt_d3d_census_gl(const char *name) { note("GL outside the layer %s", name); }
void rt_d3d_census(const char *m, CPU *c) {
    note("%s", m);
    if (strncmp(m, "Device.", 7)) {
        if (strstr(m, ".Lock")) {
            if (!strcmp(m, "VertexBuffer.Lock") || !strcmp(m, "IndexBuffer.Lock")) note("  %s flags %x", m, arg(c, 4));
            else if (!strcmp(m, "Texture.LockRect")) note("  %s flags %x rect %d", m, arg(c, 4), arg(c, 3) != 0);
            else if (!strcmp(m, "CubeTexture.LockRect")) note("  %s flags %x rect %d", m, arg(c, 5), arg(c, 4) != 0);
            else if (!strcmp(m, "Surface.LockRect")) note("  %s flags %x rect %d", m, arg(c, 3), arg(c, 2) != 0);
        } else if (!strcmp(m, "Query.Issue")) {
            note("  %s flags %x", m, arg(c, 1));
        } else if (!strcmp(m, "Query.GetData")) {
            note("  %s size %u flags %x", m, arg(c, 2), arg(c, 3));
        }
        return;
    }
    const char *d = m + 7;
    if (!strcmp(d, "SetRenderState")) {
        note("  RS %3u", arg(c, 1));
        note("  RS %3u = %08x", arg(c, 1), arg(c, 2));
    } else if (!strcmp(d, "SetSamplerState")) {
        note("  SS type %2u = %x", arg(c, 2), arg(c, 3));
        note("  SS sampler %2u", arg(c, 1));
    } else if (!strcmp(d, "SetTextureStageState")) {
        note("  TSS stage %u type %2u = %x", arg(c, 1), arg(c, 2), arg(c, 3));
    } else if (!strcmp(d, "CreateTexture")) {
        note("  texture format %x usage %x pool %u levels %s", arg(c, 5), arg(c, 4), arg(c, 6), arg(c, 3) == 1 ? "1" : "many");
    } else if (!strcmp(d, "CreateCubeTexture")) {
        note("  cube format %x usage %x pool %u", arg(c, 4), arg(c, 3), arg(c, 5));
    } else if (!strcmp(d, "CreateVolumeTexture")) {
        note("  volume format %x usage %x pool %u", arg(c, 6), arg(c, 5), arg(c, 7));
    } else if (!strcmp(d, "CreateVertexBuffer")) {
        note("  vertex buffer usage %x fvf %x pool %u", arg(c, 2), arg(c, 3), arg(c, 4));
    } else if (!strcmp(d, "CreateIndexBuffer")) {
        note("  index buffer usage %x format %x pool %u", arg(c, 2), arg(c, 3), arg(c, 4));
    } else if (!strcmp(d, "CreateRenderTarget")) {
        note("  render target format %x samples %u lockable %u", arg(c, 3), arg(c, 4), arg(c, 6));
    } else if (!strcmp(d, "CreateDepthStencilSurface")) {
        note("  depth surface format %x samples %u discard %u", arg(c, 3), arg(c, 4), arg(c, 6));
    } else if (!strcmp(d, "CreateQuery")) {
        note("  query type %u", arg(c, 1));
    } else if (!strcmp(d, "Clear")) {
        note("  clear flags %x rects %u", arg(c, 3), arg(c, 1) != 0);
    } else if (!strcmp(d, "DrawPrimitive")) {
        note("  draw type %u", arg(c, 1));
    } else if (!strcmp(d, "DrawIndexedPrimitive")) {
        note("  indexed draw type %u", arg(c, 1));
    } else if (!strcmp(d, "DrawPrimitiveUP")) {
        note("  draw from memory type %u stride %u", arg(c, 1), arg(c, 4));
    } else if (!strcmp(d, "DrawIndexedPrimitiveUP")) {
        note("  indexed draw from memory type %u index format %x stride %u", arg(c, 1), arg(c, 6), arg(c, 8));
    } else if (!strcmp(d, "StretchRect")) {
        note("  stretch filter %u src rect %d dst rect %d", arg(c, 5), arg(c, 2) != 0, arg(c, 4) != 0);
    } else if (!strcmp(d, "SetFVF")) {
        note("  fvf %x", arg(c, 1));
    } else if (!strcmp(d, "SetRenderTarget")) {
        note("  render target index %u null %d", arg(c, 1), arg(c, 2) == 0);
    } else if (!strcmp(d, "SetStreamSourceFreq")) {
        note("  stream %u frequency %x", arg(c, 1), arg(c, 2));
    } else if (!strcmp(d, "SetStreamSource")) {
        note("  stream %u", arg(c, 1));
    } else if (!strcmp(d, "SetTransform")) {
        note("  transform %u", arg(c, 1));
    } else if (!strcmp(d, "CreateVertexDeclaration")) {
        u32 p = arg(c, 1);
        for (int i = 0; i < 32; i++, p += 8) {
            u32 stream = G16(p), type = G8(p + 4), method = G8(p + 5), usage = G8(p + 6), index = G8(p + 7);
            if (stream == 0xFF) break;
            note("  vertex element type %u method %u usage %u index %u", type, method, usage, index);
            note("  vertex stream %u", stream);
        }
    }
}
