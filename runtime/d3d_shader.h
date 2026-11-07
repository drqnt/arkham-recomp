// Direct3D 9 Shader Model 3 bytecode to Metal Shading Language.
#ifndef D3D_SHADER_H
#define D3D_SHADER_H
#include <stdint.h>

#define D3DSH_MAX_SAMPLERS 16
// What a pixel shader reads and a vertex shader writes: bits 0-7 are texcoords, 8-9 colors.
typedef struct {
    int is_pixel;
    uint32_t varyings;
    uint32_t attributes;           // vertex shaders: slots read, see d3dsh_attribute_slot
    uint32_t samplers;             // pixel shaders: which are used
    uint32_t cube_samplers, shadow_samplers;
    uint32_t words;                // length of the bytecode
    const char *error;             // set when something couldn't be translated
} D3dShaderInfo;

// Attribute slot of a Direct3D vertex element, or -1.
int d3dsh_attribute_slot(unsigned usage, unsigned index);
// Length of a shader in 32-bit words, 0 if it doesn't end properly.
uint32_t d3dsh_words(const uint32_t *code, uint32_t limit);
// Returns MSL source, to be freed, with a function named vs_main or ps_main. force_varyings adds outputs to a vertex shader for a pixel shader that reads more than it writes.
char *d3dsh_translate(const uint32_t *code, uint32_t force_varyings, D3dShaderInfo *info);
#endif
