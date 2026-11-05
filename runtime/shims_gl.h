#pragma once
#include "rt.h"
#define GL_SILENCE_DEPRECATION
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_DRAW_INDIRECT_BUFFER_BINDING 0x8F43
void *gl_ptr_or_offset(u32 v, unsigned binding);
void gl_missing(const char *name);
