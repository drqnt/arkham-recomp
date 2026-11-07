// Translates every shader in the game's caches to Metal Shading Language, for checking: d3d2msl outdir file...  Everything it writes comes from the game, so it belongs under logs/.
#include "../runtime/d3d_shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    const char *out = argv[1];
    mkdir(out, 0755);
    enum { SEEN = 1 << 18 };
    uint64_t *seen = calloc(SEEN, 8);
    unsigned n = 0, dup = 0, bad = 0;
    for (int a = 2; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) {
            perror(argv[a]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        size_t size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *d = malloc(size + 8);
        fread(d, 1, size, f);
        fclose(f);
        for (size_t i = 0; i + 16 < size; i++) {
            if (d[i] != 0 || d[i + 1] != 3 || d[i + 3] != 0xFF || (d[i + 2] != 0xFF && d[i + 2] != 0xFE)) continue;
            if (d[i + 4] != 0xFE || d[i + 5] != 0xFF || memcmp(d + i + 8, "CTAB", 4)) continue;
            uint32_t limit = (uint32_t)((size - i) / 4);
            if (limit > (1u << 17)) limit = 1u << 17;
            uint32_t *code = malloc((size_t)limit * 4);
            memcpy(code, d + i, (size_t)limit * 4);
            uint32_t words = d3dsh_words(code, limit);
            if (!words) {
                free(code);
                continue;
            }
            uint64_t h = 0xcbf29ce484222325ull;
            for (uint32_t k = 0; k < words * 4; k++) h = (h ^ ((unsigned char *)code)[k]) * 0x100000001b3ull;
            if (!h) h = 1;
            uint32_t slot = (uint32_t)(h % SEEN);
            while (seen[slot] && seen[slot] != h) slot = (slot + 1) % SEEN;
            if (seen[slot]) {
                dup++;
                free(code);
                continue;
            }
            seen[slot] = h;
            D3dShaderInfo info;
            char *msl = d3dsh_translate(code, 0, &info);
            if (info.error) {
                bad++;
                fprintf(stderr, "error: %s\n", info.error);
            }
            if (msl) {
                char path[1024];
                snprintf(path, sizeof path, "%s/%s_%016llx.metal", out, info.is_pixel ? "ps" : "vs", (unsigned long long)h);
                FILE *o = fopen(path, "w");
                if (o) {
                    fputs(msl, o);
                    fclose(o);
                }
                free(msl);
            }
            n++;
            i += (size_t)words * 4 - 1;
            free(code);
        }
        free(d);
    }
    printf("%u shaders translated, %u repeated, %u with errors\n", n, dup, bad);
    return 0;
}
