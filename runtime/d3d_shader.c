// Direct3D 9 Shader Model 3 bytecode to Metal Shading Language. It covers what the game's shaders use, see docs/METAL_LAYER.md, and reports anything else in info->error.
//
// Buffers: vertex streams are 0-15. A vertex shader reads its constants from buffer 16 and a D3dVsExtra from buffer 17, a pixel shader reads constants from buffer 0 and a D3dPsExtra
// from buffer 1. Those structures are declared in the prelude below and must match mtl_device. Function constants: 0 turns clip planes on, 1 turns the alpha test on.
#include "d3d_shader.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;

typedef struct {
    char *s;
    size_t n, cap;
} Buf;
static void add(Buf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (b->n + (size_t)need + 1 > b->cap) {
        b->cap = (b->n + (size_t)need + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    vsnprintf(b->s + b->n, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    b->n += (size_t)need;
}

enum { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_ADDR = 3, R_OUTPUT = 6, R_COLOROUT = 8, R_SAMPLER = 10, R_MISC = 17 };
enum {
    OP_NOP = 0, OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_RSQ = 7, OP_DP3 = 8, OP_DP4 = 9, OP_MIN = 10, OP_MAX = 11, OP_SLT = 12, OP_SGE = 13,
    OP_EXP = 14, OP_LOG = 15, OP_LRP = 18, OP_FRC = 19, OP_DCL = 31, OP_POW = 32, OP_ABS = 35, OP_NRM = 36, OP_SINCOS = 37, OP_IFC = 41, OP_ELSE = 42, OP_ENDIF = 43, OP_MOVA = 46,
    OP_TEXKILL = 65, OP_TEX = 66, OP_DEF = 81, OP_CMP = 88, OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92, OP_TEXLDL = 95,
};
static inline u32 reg_type(u32 t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }
static inline u32 reg_num(u32 t) { return t & 0x7FF; }

int d3dsh_attribute_slot(unsigned usage, unsigned index) {
    switch (usage) {
        case 0: return index == 0 ? 0 : -1;   // position
        case 1: return index == 0 ? 1 : -1;   // blend weight
        case 2: return index == 0 ? 2 : -1;   // blend indices
        case 3: return index == 0 ? 3 : -1;   // normal
        case 6: return index == 0 ? 4 : -1;   // tangent
        case 7: return index == 0 ? 5 : -1;   // binormal
        case 10: return index < 2 ? 6 + (int)index : -1;  // color
        case 5: return index < 8 ? 8 + (int)index : -1;   // texcoord
        default: return -1;
    }
}
uint32_t d3dsh_words(const uint32_t *t, uint32_t limit) {
    u32 i = 1;
    while (i < limit) {
        u32 tok = t[i];
        if (tok == 0x0000FFFFu) return i + 1;
        if ((tok & 0xFFFFu) == 0xFFFEu) i += 1 + (tok >> 16);
        else i += 1 + ((tok >> 24) & 0xFu);
    }
    return 0;
}

typedef struct {
    const u32 *code;
    int pixel;
    D3dShaderInfo *info;
    float defs[256][4];
    unsigned char is_def[256];
    int any_def, uses_crel;
    // what each input and output register means
    signed char input_usage[32], input_index[32];
    signed char output_usage[16], output_index[16];
    unsigned char sampler_kind[D3DSH_MAX_SAMPLERS];
} Ctx;

// A sampler's name in the constant table: Feral's layer takes projected reads of the shadow map as depth comparisons, and this is how to tell which sampler that is.
static void read_constant_table(Ctx *x, const u32 *comment, u32 words) {
    if (words < 8 || comment[0] != 0x42415443u) return;  // CTAB
    const unsigned char *base = (const unsigned char *)(comment + 1);
    size_t size = (size_t)(words - 1) * 4;
    u32 count = comment[1 + 3], info = comment[1 + 4];
    if (info + (size_t)count * 20 > size) return;
    for (u32 i = 0; i < count; i++) {
        const unsigned char *c = base + info + i * 20;
        u32 name = (u32)c[0] | (u32)c[1] << 8 | (u32)c[2] << 16 | (u32)c[3] << 24;
        unsigned set = (unsigned)c[4] | (unsigned)c[5] << 8, reg = (unsigned)c[6] | (unsigned)c[7] << 8;
        if (set != 3 || name >= size || reg >= D3DSH_MAX_SAMPLERS) continue;
        if (strstr((const char *)base + name, "ShadowDepth")) x->info->shadow_samplers |= 1u << reg;
    }
}

static const char *swizzle(u32 s, char *out) {
    if (s == 0xE4) {
        out[0] = 0;
        return out;
    }
    static const char n[] = "xyzw";
    out[0] = '.';
    for (int i = 0; i < 4; i++) out[1 + i] = n[(s >> (i * 2)) & 3];
    out[5] = 0;
    return out;
}
static void fail(Ctx *x, const char *why) {
    if (!x->info->error) x->info->error = why;
}
// Reads a source operand, moves past it, and writes an expression of type float4.
static const u32 *source(Ctx *x, const u32 *p, char *out, size_t cap) {
    u32 t = *p++, rel = 0;
    int relative = (t >> 13) & 1;
    if (relative) rel = *p++;
    u32 type = reg_type(t), num = reg_num(t);
    char name[64];
    switch (type) {
        case R_TEMP: snprintf(name, sizeof name, "r%u", num); break;
        case R_INPUT:
            if (x->pixel) {
                if (num >= 32 || x->input_usage[num] < 0) fail(x, "reads an input with no declaration");
                snprintf(name, sizeof name, "in.%c%d", num < 32 && x->input_usage[num] == 10 ? 'c' : 't', num < 32 ? x->input_index[num] : 0);
            } else {
                snprintf(name, sizeof name, "v%u", num);
            }
            break;
        case R_CONST:
            if (relative) {
                // Registers the shader defines itself win over the ones the engine sets, even when read through a0, as in Direct3D. SpeedTree leaves keep
                // their card corners that way, and terrain its morph constants.
                static const char n[] = "xyzw";
                if (x->any_def) x->uses_crel = 1;
                snprintf(name, sizeof name, x->any_def ? "crel(c, a0.%c + %u)" : "c[a0.%c + %u]", n[(rel >> 16) & 3], num);
            } else if (num < 256 && x->is_def[num]) {
                snprintf(name, sizeof name, "k%u", num);
            } else {
                snprintf(name, sizeof name, "c[%u]", num);
            }
            break;
        case R_ADDR: snprintf(name, sizeof name, "float4(a0)"); break;
        case R_OUTPUT: snprintf(name, sizeof name, "o%u", num); break;
        case R_COLOROUT: snprintf(name, sizeof name, "oc%u", num); break;
        case R_MISC:
            snprintf(name, sizeof name, num == 0 ? "vpos" : "vface");
            if (num > 1) fail(x, "unknown misc register");
            break;
        default:
            fail(x, "unknown source register");
            snprintf(name, sizeof name, "float4(0)");
    }
    char sw[8];
    swizzle((t >> 16) & 0xFF, sw);
    switch ((t >> 24) & 15) {
        case 0: snprintf(out, cap, "%s%s", name, sw); break;
        case 1: snprintf(out, cap, "(-%s%s)", name, sw); break;
        case 11: snprintf(out, cap, "abs(%s%s)", name, sw); break;
        case 12: snprintf(out, cap, "(-abs(%s%s))", name, sw); break;
        default:
            fail(x, "unknown source modifier");
            snprintf(out, cap, "%s%s", name, sw);
    }
    return p;
}
// Reads a destination operand: what to assign to and the part of it.
static const u32 *dest(Ctx *x, const u32 *p, char *name, size_t cap, char *mask, int *saturate) {
    u32 t = *p++;
    u32 type = reg_type(t), num = reg_num(t), m = (t >> 16) & 15;
    if ((t >> 13) & 1) {
        fail(x, "relative destination");
        p++;
    }
    *saturate = (t >> 20) & 1;
    switch (type) {
        case R_TEMP: snprintf(name, cap, "r%u", num); break;
        case R_OUTPUT: snprintf(name, cap, "o%u", num); break;
        case R_COLOROUT: snprintf(name, cap, "oc%u", num); break;
        case R_ADDR: snprintf(name, cap, "a0"); break;
        default:
            fail(x, "unknown destination register");
            snprintf(name, cap, "r0");
    }
    char *q = mask;
    if (m != 15) {
        *q++ = '.';
        if (m & 1) *q++ = 'x';
        if (m & 2) *q++ = 'y';
        if (m & 4) *q++ = 'z';
        if (m & 8) *q++ = 'w';
    }
    *q = 0;
    return p;
}
static void assign(Buf *b, const char *name, const char *mask, int saturate, const char *expr) {
    if (saturate) add(b, "    %s%s = saturate(float4(%s))%s;\n", name, mask, expr, mask);
    else add(b, "    %s%s = float4(%s)%s;\n", name, mask, expr, mask);
}
static void print_float(Buf *b, float f) {
    if (isfinite(f)) add(b, "%.9g", (double)f);
    else {
        u32 bits;
        memcpy(&bits, &f, 4);
        add(b, "as_type<float>(0x%08xu)", bits);
    }
    double d = (double)f;
    if (isfinite(f) && d == floor(d) && fabs(d) < 1e15) add(b, ".0");
}

static const char prelude_common[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "static inline float4 d3d_frc(float4 a) { return a - floor(a); }\n"
    "static inline float4 d3d_nrm(float4 a) { float d = dot(a.xyz, a.xyz); return d > 0.0 ? a / sqrt(d) : float4(0.0); }\n"
    "static inline float d3d_log(float a) { a = abs(a); return a > 0.0 ? log2(a) : -FLT_MAX; }\n"
    "static inline float d3d_pow(float a, float b) { return powr(abs(a), b); }\n";
static const char prelude_vs[] =
    "struct D3dVsExtra { float4 half_pixel; float4 clip[2]; float4 attr_scale[16]; };\n"
    "constant bool fc_clip [[function_constant(0)]];\n";
static const char prelude_ps[] =
    "struct D3dPsExtra { float4 lod_bias[4]; float alpha_ref; int alpha_func; float pad[2]; };\n"
    "constant bool fc_alpha [[function_constant(1)]];\n"
    "static inline bool d3d_alpha(float a, float r, int f) {\n"
    "    switch (f) { case 1: return false; case 2: return a < r; case 3: return a == r; case 4: return a <= r; case 5: return a > r; case 6: return a != r; case 7: return a >= r; default: return true; }\n"
    "}\n";

char *d3dsh_translate(const uint32_t *code, uint32_t force_varyings, D3dShaderInfo *info) {
    memset(info, 0, sizeof *info);
    Ctx x;
    memset(&x, 0, sizeof x);
    x.code = code;
    x.info = info;
    u32 version = code[0];
    x.pixel = (version >> 16) == 0xFFFF;
    info->is_pixel = x.pixel;
    memset(x.input_usage, -1, sizeof x.input_usage);
    memset(x.output_usage, -1, sizeof x.output_usage);
    info->words = d3dsh_words(code, 1u << 20);
    if ((version & 0xFFFF) != 0x0300 || !info->words) {
        info->error = "not Shader Model 3";
        return NULL;
    }
    // First pass: declarations and constants.
    u32 temps = 0, outputs_written = 0, inputs = 0;
    int uses_vpos = 0, uses_vface = 0, uses_addr = 0;
    for (u32 i = 1; i < info->words - 1;) {
        u32 tok = code[i];
        if ((tok & 0xFFFFu) == 0xFFFEu) {
            read_constant_table(&x, code + i + 1, tok >> 16);
            i += 1 + (tok >> 16);
            continue;
        }
        u32 op = tok & 0xFFFF, len = (tok >> 24) & 15;
        if (op == OP_DCL) {
            u32 usage = code[i + 1], target = code[i + 2], type = reg_type(target), num = reg_num(target);
            if (type == R_SAMPLER) {
                unsigned kind = (usage >> 27) & 15;
                if (num < D3DSH_MAX_SAMPLERS) {
                    x.sampler_kind[num] = (unsigned char)kind;
                    info->samplers |= 1u << num;
                    if (kind == 3) info->cube_samplers |= 1u << num;
                    else if (kind != 2) fail(&x, "sampler that is neither 2D nor cube");
                }
            } else if (type == R_INPUT && num < 32) {
                x.input_usage[num] = (signed char)(usage & 31);
                x.input_index[num] = (signed char)((usage >> 16) & 15);
                inputs |= 1u << num;
                if (x.pixel) {
                    if ((usage & 31) == 5 && ((usage >> 16) & 15) < 8) info->varyings |= 1u << ((usage >> 16) & 15);
                    else if ((usage & 31) == 10 && ((usage >> 16) & 15) < 2) info->varyings |= 1u << (8 + ((usage >> 16) & 15));
                    else fail(&x, "pixel input other than texcoord and color");
                } else {
                    int slot = d3dsh_attribute_slot(usage & 31, (usage >> 16) & 15);
                    if (slot < 0) fail(&x, "unknown vertex element");
                    else info->attributes |= 1u << slot;
                }
            } else if (type == R_OUTPUT && num < 16) {
                x.output_usage[num] = (signed char)(usage & 31);
                x.output_index[num] = (signed char)((usage >> 16) & 15);
                outputs_written |= 1u << num;
                if ((usage & 31) == 5 && ((usage >> 16) & 15) < 8) info->varyings |= 1u << ((usage >> 16) & 15);
                else if ((usage & 31) == 10 && ((usage >> 16) & 15) < 2) info->varyings |= 1u << (8 + ((usage >> 16) & 15));
                else if ((usage & 31) != 0) fail(&x, "vertex output other than position, texcoord and color");
            } else if (type == R_MISC) {
                if (num == 0) uses_vpos = 1;
                else uses_vface = 1;
            }
        } else if (op == OP_DEF) {
            u32 num = reg_num(code[i + 1]);
            if (num < 256) {
                x.is_def[num] = 1;
                x.any_def = 1;
                memcpy(x.defs[num], code + i + 2, 16);
            }
        } else {
            for (u32 k = 1; k <= len; k++) {
                u32 p = code[i + k], type = reg_type(p);
                if (type == R_TEMP && reg_num(p) < 32) temps |= 1u << reg_num(p);
                if (type == R_ADDR || ((p >> 13) & 1)) uses_addr = 1;
            }
        }
        i += 1 + len;
    }
    // Which sampler kind goes with a shadow name: a depth comparison only ever happens on 2D.
    info->shadow_samplers &= ~info->cube_samplers;

    Buf body = {0}, b = {0};
    add(&body, "    float4 r0 = 0.0");
    for (int i = 1; i < 32; i++)
        if (temps >> i & 1) add(&body, ", r%d = 0.0", i);
    add(&body, ";\n");
    for (int i = 0; i < 256; i++) {
        if (!x.is_def[i]) continue;
        add(&body, "    const float4 k%d = float4(", i);
        for (int k = 0; k < 4; k++) {
            if (k) add(&body, ", ");
            print_float(&body, x.defs[i][k]);
        }
        add(&body, ");\n");
    }
    if (uses_addr) add(&body, "    int4 a0 = 0;\n");
    if (x.pixel) {
        // Only the first color target is ever bound, so what a shader writes to the others goes nowhere.
        add(&body, "    float4 oc0 = 0.0, oc1 = 0.0, oc2 = 0.0, oc3 = 0.0;\n    (void)oc1; (void)oc2; (void)oc3;\n");
        if (uses_vpos) add(&body, "    const float4 vpos = float4(floor(in.pos.xy), 0.0, 0.0);\n");
        if (uses_vface) add(&body, "    const float4 vface = float4(in.front ? 1.0 : -1.0);\n");
    } else {
        for (int i = 0; i < 16; i++)
            if (outputs_written >> i & 1) add(&body, "    float4 o%d = float4(0.0, 0.0, 0.0, 1.0);\n", i);
        for (int i = 0; i < 32; i++) {
            if (!(inputs >> i & 1)) continue;
            int slot = d3dsh_attribute_slot((unsigned)x.input_usage[i], (unsigned)x.input_index[i]);
            add(&body, "    const float4 v%d = in.a%d * extra.attr_scale[%d];\n", i, slot, slot);
        }
    }
    // Second pass: the code.
    char s0[128], s1[128], s2[128], name[32], mask[8], expr[512];
    int sat;
    for (u32 i = 1; i < info->words - 1;) {
        u32 tok = code[i];
        if ((tok & 0xFFFFu) == 0xFFFEu) {
            i += 1 + (tok >> 16);
            continue;
        }
        u32 op = tok & 0xFFFF, len = (tok >> 24) & 15, ctl = (tok >> 16) & 0xFF;
        const u32 *p = code + i + 1;
        i += 1 + len;
        if (tok & (1u << 28)) fail(&x, "predicated instruction");
        switch (op) {
            case OP_NOP: case OP_DCL: case OP_DEF:
                break;
            case OP_MOV: case OP_ABS: case OP_FRC: case OP_NRM: case OP_DSX: case OP_DSY: case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG:
                p = dest(&x, p, name, sizeof name, mask, &sat);
                source(&x, p, s0, sizeof s0);
                switch (op) {
                    case OP_MOV: snprintf(expr, sizeof expr, "%s", s0); break;
                    case OP_ABS: snprintf(expr, sizeof expr, "abs(%s)", s0); break;
                    case OP_FRC: snprintf(expr, sizeof expr, "d3d_frc(%s)", s0); break;
                    case OP_NRM: snprintf(expr, sizeof expr, "d3d_nrm(%s)", s0); break;
                    case OP_DSX: snprintf(expr, sizeof expr, "dfdx(%s)", s0); break;
                    case OP_DSY: snprintf(expr, sizeof expr, "dfdy(%s)", s0); break;
                    case OP_RCP: snprintf(expr, sizeof expr, "1.0 / (%s).x", s0); break;
                    case OP_RSQ: snprintf(expr, sizeof expr, "1.0 / sqrt(abs((%s).x))", s0); break;
                    case OP_EXP: snprintf(expr, sizeof expr, "exp2((%s).x)", s0); break;
                    case OP_LOG: snprintf(expr, sizeof expr, "d3d_log((%s).x)", s0); break;
                }
                assign(&body, name, mask, sat, expr);
                break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_MIN: case OP_MAX: case OP_SLT: case OP_SGE: case OP_DP3: case OP_DP4: case OP_POW:
                p = dest(&x, p, name, sizeof name, mask, &sat);
                p = source(&x, p, s0, sizeof s0);
                source(&x, p, s1, sizeof s1);
                switch (op) {
                    case OP_ADD: snprintf(expr, sizeof expr, "%s + %s", s0, s1); break;
                    case OP_SUB: snprintf(expr, sizeof expr, "%s - %s", s0, s1); break;
                    case OP_MUL: snprintf(expr, sizeof expr, "%s * %s", s0, s1); break;
                    case OP_MIN: snprintf(expr, sizeof expr, "min(float4(%s), float4(%s))", s0, s1); break;
                    case OP_MAX: snprintf(expr, sizeof expr, "max(float4(%s), float4(%s))", s0, s1); break;
                    case OP_SLT: snprintf(expr, sizeof expr, "float4(float4(%s) < float4(%s))", s0, s1); break;
                    case OP_SGE: snprintf(expr, sizeof expr, "float4(float4(%s) >= float4(%s))", s0, s1); break;
                    case OP_DP3: snprintf(expr, sizeof expr, "dot((%s).xyz, (%s).xyz)", s0, s1); break;
                    case OP_DP4: snprintf(expr, sizeof expr, "dot(float4(%s), float4(%s))", s0, s1); break;
                    case OP_POW: snprintf(expr, sizeof expr, "d3d_pow((%s).x, (%s).x)", s0, s1); break;
                }
                assign(&body, name, mask, sat, expr);
                break;
            case OP_MAD: case OP_LRP: case OP_CMP: case OP_DP2ADD:
                p = dest(&x, p, name, sizeof name, mask, &sat);
                p = source(&x, p, s0, sizeof s0);
                p = source(&x, p, s1, sizeof s1);
                source(&x, p, s2, sizeof s2);
                switch (op) {
                    case OP_MAD: snprintf(expr, sizeof expr, "%s * %s + %s", s0, s1, s2); break;
                    case OP_LRP: snprintf(expr, sizeof expr, "mix(float4(%s), float4(%s), float4(%s))", s2, s1, s0); break;
                    case OP_CMP: snprintf(expr, sizeof expr, "select(float4(%s), float4(%s), float4(%s) >= 0.0)", s2, s1, s0); break;
                    case OP_DP2ADD: snprintf(expr, sizeof expr, "dot((%s).xy, (%s).xy) + (%s).x", s0, s1, s2); break;
                }
                assign(&body, name, mask, sat, expr);
                break;
            case OP_SINCOS:
                p = dest(&x, p, name, sizeof name, mask, &sat);
                source(&x, p, s0, sizeof s0);
                snprintf(expr, sizeof expr, "cos((%s).x), sin((%s).x), 0.0, 0.0", s0, s0);
                add(&body, "    { float4 t = float4(%s); %s%s = %st%s%s; }\n", expr, name, mask, sat ? "saturate(" : "", sat ? ")" : "", mask);
                break;
            case OP_MOVA:
                p = dest(&x, p, name, sizeof name, mask, &sat);
                source(&x, p, s0, sizeof s0);
                add(&body, "    a0%s = int4(floor(float4(%s) + 0.5))%s;\n", mask, s0, mask);
                break;
            case OP_TEXKILL: {
                u32 t = *p, num = reg_num(t);
                if (reg_type(t) != R_TEMP) fail(&x, "texkill on something other than a temporary");
                add(&body, "    if (any(r%u.xyz < 0.0)) discard_fragment();\n", num);
                break;
            }
            case OP_TEX: case OP_TEXLDL: {
                p = dest(&x, p, name, sizeof name, mask, &sat);
                p = source(&x, p, s0, sizeof s0);
                u32 st = *p, sw = (st >> 16) & 0xFF, n = reg_num(st);
                if (reg_type(st) != R_SAMPLER || n >= D3DSH_MAX_SAMPLERS) {
                    fail(&x, "texture read without a sampler");
                    break;
                }
                int cube = (info->cube_samplers >> n) & 1, shadow = (info->shadow_samplers >> n) & 1;
                int project = op == OP_TEX && (ctl & 1), bias = op == OP_TEX && (ctl & 2);
                char ls[8];
                swizzle(sw, ls);
                const char *lod = "";
                char lodtext[96];
                snprintf(lodtext, sizeof lodtext, "extra.lod_bias[%u].%c", n / 4, "xyzw"[n & 3]);
                if (shadow) {
                    if (bias) fail(&x, "shadow read with a bias");
                    if (op == OP_TEXLDL) snprintf(expr, sizeof expr, "s%u.sample_compare(ss%u, (%s).xy, (%s).z, level((%s).w))", n, n, s0, s0, s0);
                    else if (project) snprintf(expr, sizeof expr, "s%u.sample_compare(ss%u, (%s).xy / (%s).w, (%s).z / (%s).w)", n, n, s0, s0, s0, s0);
                    else snprintf(expr, sizeof expr, "s%u.sample_compare(ss%u, (%s).xy, (%s).z)", n, n, s0, s0);
                    add(&body, "    { float4 t = float4(%s); %s%s = t%s; }\n", expr, name, mask, mask);
                    (void)lod;
                    break;
                }
                char coord[160];
                if (cube) {
                    if (project) snprintf(coord, sizeof coord, "(%s).xyz / (%s).w", s0, s0);
                    else snprintf(coord, sizeof coord, "(%s).xyz", s0);
                } else {
                    if (project) snprintf(coord, sizeof coord, "(%s).xy / (%s).w", s0, s0);
                    else snprintf(coord, sizeof coord, "(%s).xy", s0);
                }
                if (op == OP_TEXLDL) snprintf(expr, sizeof expr, "s%u.sample(ss%u, %s, level((%s).w + %s))", n, n, coord, s0, lodtext);
                else if (bias) snprintf(expr, sizeof expr, "s%u.sample(ss%u, %s, bias((%s).w + %s))", n, n, coord, s0, lodtext);
                else snprintf(expr, sizeof expr, "s%u.sample(ss%u, %s, bias(%s))", n, n, coord, lodtext);
                if (sat) add(&body, "    { float4 t = saturate(%s%s); %s%s = t%s; }\n", expr, ls, name, mask, mask);
                else add(&body, "    { float4 t = %s%s; %s%s = t%s; }\n", expr, ls, name, mask, mask);
                break;
            }
            case OP_IFC: {
                static const char *const cmp[] = {"", ">", "==", ">=", "<", "!=", "<="};
                p = source(&x, p, s0, sizeof s0);
                source(&x, p, s1, sizeof s1);
                if (ctl < 1 || ctl > 6) fail(&x, "unknown comparison");
                add(&body, "    if ((%s).x %s (%s).x) {\n", s0, cmp[ctl <= 6 ? ctl : 0], s1);
                break;
            }
            case OP_ELSE:
                add(&body, "    } else {\n");
                break;
            case OP_ENDIF:
                add(&body, "    }\n");
                break;
            default:
                fail(&x, "unknown instruction");
                fprintf(stderr, "[d3dsh] unknown instruction %u\n", op);
        }
    }
    // The whole function.
    add(&b, "%s%s", prelude_common, x.pixel ? prelude_ps : prelude_vs);
    if (x.uses_crel) {
        add(&b, "inline float4 crel(constant float4 *c, int i) {\n    switch (i) {\n");
        for (int r = 0; r < 256; r++)
            if (x.is_def[r]) add(&b, "        case %d: return float4(%.9g, %.9g, %.9g, %.9g);\n", r, x.defs[r][0], x.defs[r][1], x.defs[r][2], x.defs[r][3]);
        add(&b, "        default: return c[i];\n    }\n}\n");
    }
    if (x.pixel) {
        add(&b, "struct PsIn {\n    float4 pos [[position]];\n");
        if (uses_vface) add(&b, "    bool front [[front_facing]];\n");
        for (int i = 0; i < 8; i++)
            if (info->varyings >> i & 1) add(&b, "    float4 t%d [[user(t%d)]];\n", i, i);
        for (int i = 0; i < 2; i++)
            if (info->varyings >> (8 + i) & 1) add(&b, "    float4 c%d [[user(c%d)]];\n", i, i);
        add(&b, "};\nstruct PsOut { float4 c0 [[color(0)]]; };\n");
        add(&b, "fragment PsOut ps_main(PsIn in [[stage_in]], constant float4 *c [[buffer(0)]], constant D3dPsExtra &extra [[buffer(1)]]");
        for (u32 n = 0; n < D3DSH_MAX_SAMPLERS; n++) {
            if (!(info->samplers >> n & 1)) continue;
            const char *type = (info->cube_samplers >> n & 1) ? "texturecube<float>" : (info->shadow_samplers >> n & 1) ? "depth2d<float>" : "texture2d<float>";
            add(&b, ", %s s%u [[texture(%u)]], sampler ss%u [[sampler(%u)]]", type, n, n, n, n);
        }
        add(&b, ") {\n%s", body.s);
        add(&b, "    if (fc_alpha && !d3d_alpha(oc0.a, extra.alpha_ref, extra.alpha_func)) discard_fragment();\n");
        add(&b, "    PsOut out;\n    out.c0 = oc0;\n    return out;\n}\n");
    } else {
        add(&b, "struct VsIn {\n");
        for (int slot = 0; slot < 16; slot++)
            if (info->attributes >> slot & 1) add(&b, "    float4 a%d [[attribute(%d)]];\n", slot, slot);
        add(&b, "};\nstruct VsOut {\n    float4 pos [[position, invariant]];\n    float clip [[clip_distance, function_constant(fc_clip)]] [2];\n");
        u32 all = info->varyings | force_varyings;
        for (int i = 0; i < 8; i++)
            if (all >> i & 1) add(&b, "    float4 t%d [[user(t%d)]];\n", i, i);
        for (int i = 0; i < 2; i++)
            if (all >> (8 + i) & 1) add(&b, "    float4 c%d [[user(c%d)]];\n", i, i);
        add(&b, "};\nvertex VsOut vs_main(VsIn in [[stage_in]], constant float4 *c [[buffer(16)]], constant D3dVsExtra &extra [[buffer(17)]]) {\n%s", body.s);
        add(&b, "    VsOut out;\n");
        for (int i = 0; i < 8; i++)
            if (all >> i & 1) add(&b, "    out.t%d = float4(0.0, 0.0, 0.0, 1.0);\n", i);
        for (int i = 0; i < 2; i++)
            if (all >> (8 + i) & 1) add(&b, "    out.c%d = float4(0.0, 0.0, 0.0, 1.0);\n", i);
        int have_pos = 0;
        for (int i = 0; i < 16; i++) {
            if (!(outputs_written >> i & 1)) continue;
            if (x.output_usage[i] == 0) {
                add(&b, "    float4 pos = o%d;\n", i);
                have_pos = 1;
            } else if (x.output_usage[i] == 5) {
                add(&b, "    out.t%d = o%d;\n", x.output_index[i], i);
            } else if (x.output_usage[i] == 10) {
                add(&b, "    out.c%d = o%d;\n", x.output_index[i], i);
            }
        }
        if (!have_pos) fail(&x, "no position output");
        add(&b, "    if (fc_clip) {\n        out.clip[0] = dot(pos, extra.clip[0]);\n        out.clip[1] = dot(pos, extra.clip[1]);\n    }\n");
        add(&b, "    pos.xy += extra.half_pixel.xy * pos.w;\n    out.pos = pos;\n    return out;\n}\n");
    }
    free(body.s);
    return b.s;
}
