// On the phone, in place of gl_queue.c, pipelines.c and shims_gl.c (tools/build.py --ios). iOS has no desktop OpenGL, and the game draws through
// the Metal layer there. Feral's code still makes OpenGL contexts at start and asks them what they can do, so this answers those questions the
// way the Mac's OpenGL does (recorded with ARKHAM_GL_RECORD=1, logs/ios/glrec.txt), hands out object names, and lets every other call do nothing.
#include "shim.h"
#include "shim_reg.h"
#include "shims_gl.h"

#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

extern u32 cf_wrap(CFTypeRef o);
extern CFTypeRef cf_unwrap(u32 g);
unsigned long rt_gl_draws;

void rt_gl_init(void) {}
void rt_gl_context_changed(void) {}
// Called by gl_gen.c for each OpenGL function the game asks for: logged once each, since on the phone every one of them is missing.
void gl_missing(const char *name) {
    enum { SEEN = 256 };
    static const char *seen[SEEN];
    for (int i = 0; i < SEEN; i++) {
        if (seen[i] == name) return;
        if (!seen[i]) {
            seen[i] = name;
            fprintf(stderr, "[gl] %s does nothing, the phone has no OpenGL\n", name);
            return;
        }
    }
}
void *gl_ptr_or_offset(u32 v, unsigned binding) {
    (void)binding;
    return v ? GP(v) : NULL;
}

// gl_queue.c: OpenGL calls never go to a queue of their own here.
u8 rt_glq_kind(const char *name) {
    (void)name;
    return 0;
}
int rt_glq_route(CPU *c, void (*f)(CPU *), u8 kind, int gated, const char *name) {
    (void)c, (void)f, (void)kind, (void)gated, (void)name;
    return 0;
}
void rt_glq_drain(void) {}

// pipelines.c: capturing OpenGL programs for the Mac's shader cache.
void rt_pipe_capture_start(const char *path) { (void)path; }
void rt_pipe_capture_stop(void) {}

// ---------- which kind of context is current ----------
// The game makes a legacy (2.1) and a core (4.1) context and asks each the same things, getting different answers. The stand-in
// NSOpenGLContext (ios_appkit.m) knows which its pixel format asked for and says so when made current.
int rt_ios_gl_core;
int rt_ios_gl_context_is_core(void *ctx);
SHIM(CGLSetCurrentContext) {
    void *ctx = (void *)cf_unwrap(ARG(0));
    if (ctx) rt_ios_gl_core = rt_ios_gl_context_is_core(ctx);
    RET(0);
}
SHIM(cgl_ok) { RET(0); }
SHIM(CGLGetShareGroup) {
    static int group;
    RET(rt_handle_new(&group));
}

// ---------- the renderer: one, the phone's GPU ----------
static int renderer_info;
SHIM(CGLQueryRendererInfo) {
    if (ARG(1)) G32(ARG(1)) = rt_handle_new(&renderer_info);
    if (ARG(2)) G32(ARG(2)) = 1;
    RET(0);
}
SHIM(CGLDescribeRenderer) {
    i32 v = 0;
    switch (ARG(2)) {
        case 70: v = 0x1027600; break;  // kCGLRPRendererID: the Mac's Apple GPU renderer
        case 73: v = 1; break;          // accelerated
        case 84: v = 1; break;          // display mask
        case 129: v = 1; break;         // online
        // Video and texture memory in megabytes. The Mac says 53084; the phone gives an app about 3.5 GB in all (logs/probe_*.txt), so the
        // game should plan for much less. 1 GB or more gets Feral's best class of settings (Resources/specifications.xml).
        case 131: case 132: v = 2048; break;
        default: break;
    }
    if (ARG(3)) G32(ARG(3)) = (u32)v;
    RET(0);
}
SHIM(CGLDestroyRendererInfo) {
    rt_handle_free(ARG(0));
    RET(0);
}

// ---------- the questions ----------
static const char legacy_extensions[] =
    "GL_ARB_color_buffer_float GL_ARB_depth_buffer_float GL_ARB_depth_clamp GL_ARB_depth_texture GL_ARB_draw_buffers "
    "GL_ARB_draw_elements_base_vertex GL_ARB_draw_instanced GL_ARB_fragment_program GL_ARB_fragment_program_shadow GL_ARB_fragment_shader "
    "GL_ARB_framebuffer_object GL_ARB_framebuffer_sRGB GL_ARB_half_float_pixel GL_ARB_half_float_vertex GL_ARB_imaging GL_ARB_instanced_arrays "
    "GL_ARB_multisample GL_ARB_multitexture GL_ARB_occlusion_query GL_ARB_pixel_buffer_object GL_ARB_point_parameters GL_ARB_point_sprite "
    "GL_ARB_provoking_vertex GL_ARB_seamless_cube_map GL_ARB_shader_objects GL_ARB_shader_texture_lod GL_ARB_shading_language_100 GL_ARB_shadow "
    "GL_ARB_shadow_ambient GL_ARB_sync GL_ARB_texture_border_clamp GL_ARB_texture_compression GL_ARB_texture_compression_rgtc "
    "GL_ARB_texture_cube_map GL_ARB_texture_env_add GL_ARB_texture_env_combine GL_ARB_texture_env_crossbar GL_ARB_texture_env_dot3 "
    "GL_ARB_texture_float GL_ARB_texture_mirrored_repeat GL_ARB_texture_non_power_of_two GL_ARB_texture_rectangle GL_ARB_texture_rg "
    "GL_ARB_transpose_matrix GL_ARB_vertex_array_bgra GL_ARB_vertex_blend GL_ARB_vertex_buffer_object GL_ARB_vertex_program GL_ARB_vertex_shader "
    "GL_ARB_window_pos GL_EXT_abgr GL_EXT_bgra GL_EXT_bindable_uniform GL_EXT_blend_color GL_EXT_blend_equation_separate "
    "GL_EXT_blend_func_separate GL_EXT_blend_minmax GL_EXT_blend_subtract GL_EXT_clip_volume_hint GL_EXT_debug_label GL_EXT_debug_marker "
    "GL_EXT_draw_buffers2 GL_EXT_draw_range_elements GL_EXT_fog_coord GL_EXT_framebuffer_blit GL_EXT_framebuffer_multisample "
    "GL_EXT_framebuffer_multisample_blit_scaled GL_EXT_framebuffer_object GL_EXT_framebuffer_sRGB GL_EXT_geometry_shader4 "
    "GL_EXT_gpu_program_parameters GL_EXT_gpu_shader4 GL_EXT_multi_draw_arrays GL_EXT_packed_depth_stencil GL_EXT_packed_float "
    "GL_EXT_provoking_vertex GL_EXT_rescale_normal GL_EXT_secondary_color GL_EXT_separate_specular_color GL_EXT_shadow_funcs "
    "GL_EXT_stencil_two_side GL_EXT_stencil_wrap GL_EXT_texture_array GL_EXT_texture_compression_dxt1 GL_EXT_texture_compression_s3tc "
    "GL_EXT_texture_env_add GL_EXT_texture_filter_anisotropic GL_EXT_texture_integer GL_EXT_texture_lod_bias GL_EXT_texture_rectangle "
    "GL_EXT_texture_shared_exponent GL_EXT_texture_sRGB GL_EXT_texture_sRGB_decode GL_EXT_timer_query GL_EXT_transform_feedback "
    "GL_EXT_vertex_array_bgra GL_APPLE_aux_depth_stencil GL_APPLE_client_storage GL_APPLE_element_array GL_APPLE_fence GL_APPLE_float_pixels "
    "GL_APPLE_flush_buffer_range GL_APPLE_flush_render GL_APPLE_packed_pixels GL_APPLE_pixel_buffer GL_APPLE_rgb_422 GL_APPLE_row_bytes "
    "GL_APPLE_specular_vector GL_APPLE_texture_range GL_APPLE_transform_hint GL_APPLE_vertex_array_object GL_APPLE_vertex_point_size "
    "GL_APPLE_vertex_program_evaluators GL_APPLE_ycbcr_422 GL_ATI_separate_stencil GL_ATI_texture_env_combine3 GL_ATI_texture_float "
    "GL_IBM_rasterpos_clip GL_NV_blend_square GL_NV_conditional_render GL_NV_depth_clamp GL_NV_fog_distance GL_NV_fragment_program_option "
    "GL_NV_fragment_program2 GL_NV_light_max_exponent GL_NV_texgen_reflection GL_NV_texture_barrier GL_NV_vertex_program2_option "
    "GL_NV_vertex_program3 GL_SGI_color_matrix GL_SGIS_generate_mipmap GL_SGIS_texture_edge_clamp GL_SGIS_texture_lod";
static const char *const core_extensions[] = {
    "GL_ARB_blend_func_extended",
    "GL_ARB_draw_buffers_blend",
    "GL_ARB_draw_indirect",
    "GL_ARB_ES2_compatibility",
    "GL_ARB_explicit_attrib_location",
    "GL_ARB_gpu_shader_fp64",
    "GL_ARB_gpu_shader5",
    "GL_ARB_instanced_arrays",
    "GL_ARB_internalformat_query",
    "GL_ARB_occlusion_query2",
    "GL_ARB_sample_shading",
    "GL_ARB_sampler_objects",
    "GL_ARB_separate_shader_objects",
    "GL_ARB_shader_bit_encoding",
    "GL_ARB_shader_subroutine",
    "GL_ARB_shading_language_include",
    "GL_ARB_tessellation_shader",
    "GL_ARB_texture_buffer_object_rgb32",
    "GL_ARB_texture_cube_map_array",
    "GL_ARB_texture_gather",
    "GL_ARB_texture_query_lod",
    "GL_ARB_texture_rgb10_a2ui",
    "GL_ARB_texture_storage",
    "GL_ARB_texture_swizzle",
    "GL_ARB_timer_query",
    "GL_ARB_transform_feedback2",
    "GL_ARB_transform_feedback3",
    "GL_ARB_vertex_attrib_64bit",
    "GL_ARB_vertex_type_2_10_10_10_rev",
    "GL_ARB_viewport_array",
    "GL_EXT_debug_label",
    "GL_EXT_debug_marker",
    "GL_EXT_framebuffer_multisample_blit_scaled",
    "GL_EXT_texture_compression_s3tc",
    "GL_EXT_texture_filter_anisotropic",
    "GL_EXT_texture_sRGB_decode",
    "GL_APPLE_client_storage",
    "GL_APPLE_container_object_shareable",
    "GL_APPLE_flush_render",
    "GL_APPLE_rgb_422",
    "GL_APPLE_row_bytes",
    "GL_APPLE_texture_range",
    "GL_NV_texture_barrier",
};

static const char *gl_string(u32 name) {
    switch (name) {
        case 0x1F00: return "Apple";                                              // GL_VENDOR
        case 0x1F01: return "Apple A20 Pro GPU";                                  // GL_RENDERER
        case 0x1F02: return rt_ios_gl_core ? "4.1 Metal - 91.7" : "2.1 Metal - 91.7";  // GL_VERSION
        case 0x1F03: return rt_ios_gl_core ? NULL : legacy_extensions;            // GL_EXTENSIONS: core contexts list them one by one
        case 0x8B8C: return rt_ios_gl_core ? "4.10" : "1.20";                     // GL_SHADING_LANGUAGE_VERSION
        default: return NULL;
    }
}
SHIM(glGetString) {
    const char *s = gl_string(ARG(0));
    if (!s) fprintf(stderr, "[gl] no answer for glGetString(%#x)\n", ARG(0));
    RET(s ? rt_guest_strdup(s) : 0);
}
SHIM(glGetStringi) {
    u32 i = ARG(1);
    const u32 n = sizeof core_extensions / sizeof *core_extensions;
    RET(ARG(0) == 0x1F03 && i < n ? rt_guest_strdup(core_extensions[i]) : 0);
}
// pname, then the legacy and core answers (-1: that kind of context isn't asked).
static const i32 integers[][3] = {
    {0x0D31, 8, -1},          // GL_MAX_LIGHTS
    {0x84E2, 8, -1},          // GL_MAX_TEXTURE_UNITS
    {0x0D32, 6, 8},           // GL_MAX_CLIP_PLANES / GL_MAX_CLIP_DISTANCES
    {0x8871, 8, -1},          // GL_MAX_TEXTURE_COORDS
    {0x8824, 8, 8},           // GL_MAX_DRAW_BUFFERS
    {0x8D57, 8, 8},           // GL_MAX_SAMPLES
    {0x0D33, 16384, 16384},   // GL_MAX_TEXTURE_SIZE
    {0x8073, 2048, 2048},     // GL_MAX_3D_TEXTURE_SIZE
    {0x84FD, 16, 16},         // GL_MAX_TEXTURE_LOD_BIAS
    {0x8869, 16, 16},         // GL_MAX_VERTEX_ATTRIBS
    {0x8872, 16, 16},         // GL_MAX_TEXTURE_IMAGE_UNITS
    {0x8B4C, 16, 16},         // GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS
    {0x8B4D, 16, 80},         // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
    {0x8C29, 16, 16},         // GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS
    {0x8B4B, 124, -1},        // GL_MAX_VARYING_FLOATS
    {0x8B4A, 4096, 4096},     // GL_MAX_VERTEX_UNIFORM_COMPONENTS
    {0x8B49, 4096, 4096},     // GL_MAX_FRAGMENT_UNIFORM_COMPONENTS
    {0x8C8B, 4, 4},           // GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS
    {0x821D, -1, 43},         // GL_NUM_EXTENSIONS
    {0x8E81, -1, 16},         // GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS
    {0x8E82, -1, 16},         // GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS
    {0x9122, -1, 64},         // GL_MAX_VERTEX_OUTPUT_COMPONENTS
    {0x9124, -1, 128},        // GL_MAX_GEOMETRY_OUTPUT_COMPONENTS
    {0x9125, -1, 128},        // GL_MAX_FRAGMENT_INPUT_COMPONENTS
    {0x8A2F, -1, 80},         // GL_MAX_UNIFORM_BUFFER_BINDINGS
    {0x8A30, -1, 65536},      // GL_MAX_UNIFORM_BLOCK_SIZE
    {0x910E, -1, 8},          // GL_MAX_COLOR_TEXTURE_SAMPLES
    {0x910F, -1, 8},          // GL_MAX_DEPTH_TEXTURE_SAMPLES
    {0x9110, -1, 1},          // GL_MAX_INTEGER_SAMPLES
};
// The viewport is the one thing asked that changes: the game sets it, and reads it back to learn its drawing size.
static i32 viewport[4];
extern double rt_ios_screen_w, rt_ios_screen_h, rt_ios_screen_scale;
SHIM(glViewport) {
    viewport[0] = (i32)ARG(0), viewport[1] = (i32)ARG(1), viewport[2] = (i32)ARG(2), viewport[3] = (i32)ARG(3);
}
SHIM(glGetIntegerv) {
    u32 pname = ARG(0);
    i32 *out = ARG(1) ? (i32 *)GP(ARG(1)) : NULL;
    if (!out) return;
    if (pname == 0x0BA2) {  // GL_VIEWPORT
        if (!viewport[2]) viewport[2] = (i32)(rt_ios_screen_w * rt_ios_screen_scale), viewport[3] = (i32)(rt_ios_screen_h * rt_ios_screen_scale);
        memcpy(out, viewport, sizeof viewport);
        return;
    }
    for (unsigned i = 0; i < sizeof integers / sizeof *integers; i++)
        if ((u32)integers[i][0] == pname) {
            i32 v = integers[i][rt_ios_gl_core ? 2 : 1];
            *out = v < 0 ? 0 : v;
            return;
        }
    fprintf(stderr, "[gl] no answer for glGetIntegerv(%#x), called from %08x\n", pname, G32(c->esp));
    *out = 0;
}
SHIM(glGetFloatv) {
    float v = ARG(0) == 0x84FF ? 16.0f : 0.0f;  // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
    if (ARG(0) != 0x84FF) fprintf(stderr, "[gl] no answer for glGetFloatv(%#x)\n", ARG(0));
    if (ARG(1)) memcpy(GP(ARG(1)), &v, 4);
}
// The ARB assembly programs' limits, vertex (0x8620) and fragment (0x8804): the same but for environment parameters.
SHIM(glGetProgramivARB) {
    i32 v = 0;
    switch (ARG(1)) {
        case 0x88B5: v = ARG(0) == 0x8620 ? 256 : 128; break;  // GL_MAX_PROGRAM_ENV_PARAMETERS (vertex, fragment)
        case 0x88B4: v = 1024; break;                          // GL_MAX_PROGRAM_LOCAL_PARAMETERS
        case 0x88A7: v = 65535; break;                         // GL_MAX_PROGRAM_NATIVE_TEMPORARIES
        case 0x88A3: v = 262144; break;                        // GL_MAX_PROGRAM_NATIVE_INSTRUCTIONS
        case 0x88F5: v = 4; break;                             // an NV program limit, the Mac's answer
        default: fprintf(stderr, "[gl] no answer for glGetProgramivARB(%#x, %#x)\n", ARG(0), ARG(1)); break;
    }
    if (ARG(2)) G32(ARG(2)) = (u32)v;
}
SHIM(glGetQueryiv) {
    i32 v = 0;
    switch (ARG(0)) {
        case 0x8914: case 0x88BF: v = 32; break;  // samples passed, time elapsed: counter bits
        default: break;                           // GL_ANY_SAMPLES_PASSED_CONSERVATIVE and others: 0, as on the Mac
    }
    if (ARG(2)) G32(ARG(2)) = (u32)v;
}
SHIM(glCheckFramebufferStatus) { RET(0x8CD5); }  // GL_FRAMEBUFFER_COMPLETE
SHIM(glGetError) { RET(0); }
// Object names: never reused, so no two objects the game makes share one.
static u32 next_name = 1;
SHIM(gl_gen_names) {
    u32 n = ARG(0), out = ARG(1);
    for (u32 i = 0; out && i < n; i++) G32(out + i * 4) = next_name++;
}
SHIM(glCreateProgram) { RET(next_name++); }
SHIM(glCreateShader) { RET(next_name++); }

const ShimReg rt_shims_gl[] = {
    R(CGLSetCurrentContext) R2("_CGLLockContext", cgl_ok) R2("_CGLUnlockContext", cgl_ok) R2("_CGLFlushDrawable", cgl_ok) R2("_CGLSetParameter", cgl_ok)
    R2("_CGLEnable", cgl_ok) R2("_CGLDisable", cgl_ok) R2("_CGLClearDrawable", cgl_ok) R(CGLGetShareGroup) R(CGLQueryRendererInfo) R(CGLDescribeRenderer)
    R(CGLDestroyRendererInfo)
    R(glGetString) R(glGetStringi) R(glGetIntegerv) R(glGetFloatv) R(glGetProgramivARB) R(glGetQueryiv) R(glCheckFramebufferStatus) R(glGetError)
    R(glViewport) R(glCreateProgram) R(glCreateShader)
    R2("_glGenBuffers", gl_gen_names) R2("_glGenFencesAPPLE", gl_gen_names) R2("_glGenFramebuffers", gl_gen_names) R2("_glGenProgramPipelines", gl_gen_names)
    R2("_glGenProgramsARB", gl_gen_names) R2("_glGenQueries", gl_gen_names) R2("_glGenRenderbuffers", gl_gen_names) R2("_glGenSamplers", gl_gen_names)
    R2("_glGenTextures", gl_gen_names) R2("_glGenVertexArrays", gl_gen_names)
    {NULL, NULL},
};
