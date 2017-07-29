/*
 * Copyright (c) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "util/u_memory.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_resource.h"
#include "lima_program.h"

static void
lima_screen_destroy(struct pipe_screen *pscreen)
{
   struct lima_screen *screen = lima_screen(pscreen);

   slab_destroy_parent(&screen->transfer_pool);

   lima_device_delete(screen->dev);
   FREE(screen);
}

static const char *
lima_screen_get_name(struct pipe_screen *pscreen)
{
   return "Mali400";
}

static const char *
lima_screen_get_vendor(struct pipe_screen *pscreen)
{
   return "lima";
}

static const char *
lima_screen_get_device_vendor(struct pipe_screen *pscreen)
{
   return "ARM";
}

static int
lima_screen_get_param(struct pipe_screen *pscreen, enum pipe_cap param)
{
   switch (param) {
   case PIPE_CAP_NPOT_TEXTURES:
      return 1;

   case PIPE_CAP_TWO_SIDED_STENCIL:
   case PIPE_CAP_MAX_DUAL_SOURCE_RENDER_TARGETS:
   case PIPE_CAP_ANISOTROPIC_FILTER:
   case PIPE_CAP_POINT_SPRITE:
      return 0;

   case PIPE_CAP_MAX_RENDER_TARGETS:
      return 1;

   case PIPE_CAP_OCCLUSION_QUERY:
   case PIPE_CAP_QUERY_TIME_ELAPSED:
      return 0;

   case PIPE_CAP_TEXTURE_SHADOW_MAP:
      return 1;

   case PIPE_CAP_TEXTURE_SWIZZLE:
      return 0;

   case PIPE_CAP_MAX_TEXTURE_2D_LEVELS:
      return LIMA_MAX_MIP_LEVELS;

   case PIPE_CAP_MAX_TEXTURE_3D_LEVELS:
   case PIPE_CAP_MAX_TEXTURE_CUBE_LEVELS:
   case PIPE_CAP_TEXTURE_MIRROR_CLAMP:
      return 0;

   case PIPE_CAP_BLEND_EQUATION_SEPARATE:
      return 1;

   case PIPE_CAP_SM3:
   case PIPE_CAP_MAX_STREAM_OUTPUT_BUFFERS:
   case PIPE_CAP_PRIMITIVE_RESTART:
   case PIPE_CAP_INDEP_BLEND_ENABLE:
   case PIPE_CAP_INDEP_BLEND_FUNC:
   case PIPE_CAP_MAX_TEXTURE_ARRAY_LAYERS:
   case PIPE_CAP_TGSI_FS_COORD_ORIGIN_UPPER_LEFT:
   case PIPE_CAP_TGSI_FS_COORD_ORIGIN_LOWER_LEFT:
   case PIPE_CAP_TGSI_FS_COORD_PIXEL_CENTER_HALF_INTEGER:
   case PIPE_CAP_TGSI_FS_COORD_PIXEL_CENTER_INTEGER:
   case PIPE_CAP_DEPTH_CLIP_DISABLE:
   case PIPE_CAP_SHADER_STENCIL_EXPORT:
   case PIPE_CAP_TGSI_INSTANCEID:
   case PIPE_CAP_VERTEX_ELEMENT_INSTANCE_DIVISOR:
   case PIPE_CAP_FRAGMENT_COLOR_CLAMPED:
   case PIPE_CAP_MIXED_COLORBUFFER_FORMATS:
   case PIPE_CAP_SEAMLESS_CUBE_MAP:
   case PIPE_CAP_SEAMLESS_CUBE_MAP_PER_TEXTURE:
   case PIPE_CAP_MIN_TEXEL_OFFSET:
   case PIPE_CAP_MAX_TEXEL_OFFSET:
   case PIPE_CAP_CONDITIONAL_RENDER:
   case PIPE_CAP_TEXTURE_BARRIER:
   case PIPE_CAP_MAX_STREAM_OUTPUT_SEPARATE_COMPONENTS:
   case PIPE_CAP_MAX_STREAM_OUTPUT_INTERLEAVED_COMPONENTS:
   case PIPE_CAP_STREAM_OUTPUT_PAUSE_RESUME:
   case PIPE_CAP_TGSI_CAN_COMPACT_CONSTANTS:
   case PIPE_CAP_VERTEX_COLOR_UNCLAMPED:
   case PIPE_CAP_VERTEX_COLOR_CLAMPED:
      return 0;

   case PIPE_CAP_GLSL_FEATURE_LEVEL:
      return 120;

   case PIPE_CAP_QUADS_FOLLOW_PROVOKING_VERTEX_CONVENTION:
   case PIPE_CAP_USER_VERTEX_BUFFERS:
   case PIPE_CAP_VERTEX_BUFFER_OFFSET_4BYTE_ALIGNED_ONLY:
   case PIPE_CAP_VERTEX_BUFFER_STRIDE_4BYTE_ALIGNED_ONLY:
   case PIPE_CAP_VERTEX_ELEMENT_SRC_OFFSET_4BYTE_ALIGNED_ONLY:
   case PIPE_CAP_COMPUTE:
   case PIPE_CAP_USER_CONSTANT_BUFFERS:
   case PIPE_CAP_CONSTANT_BUFFER_OFFSET_ALIGNMENT:
   case PIPE_CAP_START_INSTANCE:
   case PIPE_CAP_QUERY_TIMESTAMP:
   case PIPE_CAP_TEXTURE_MULTISAMPLE:
   case PIPE_CAP_MIN_MAP_BUFFER_ALIGNMENT:
   case PIPE_CAP_CUBE_MAP_ARRAY:
   case PIPE_CAP_TEXTURE_BUFFER_OBJECTS:
   case PIPE_CAP_TEXTURE_BUFFER_OFFSET_ALIGNMENT:
   case PIPE_CAP_BUFFER_SAMPLER_VIEW_RGBA_ONLY:
   case PIPE_CAP_TGSI_TEXCOORD:
   case PIPE_CAP_PREFER_BLIT_BASED_TEXTURE_TRANSFER:
   case PIPE_CAP_QUERY_PIPELINE_STATISTICS:
   case PIPE_CAP_TEXTURE_BORDER_COLOR_QUIRK:
   case PIPE_CAP_MAX_TEXTURE_BUFFER_SIZE:
      return 0;

   case PIPE_CAP_MAX_VIEWPORTS:
      return 1;

   case PIPE_CAP_ENDIANNESS:
   case PIPE_CAP_MIXED_FRAMEBUFFER_SIZES:
   case PIPE_CAP_TGSI_VS_LAYER_VIEWPORT:
   case PIPE_CAP_MAX_GEOMETRY_OUTPUT_VERTICES:
   case PIPE_CAP_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS:
   case PIPE_CAP_MAX_TEXTURE_GATHER_COMPONENTS:
   case PIPE_CAP_TEXTURE_GATHER_SM5:
   case PIPE_CAP_BUFFER_MAP_PERSISTENT_COHERENT:
   case PIPE_CAP_FAKE_SW_MSAA:
   case PIPE_CAP_TEXTURE_QUERY_LOD:
   case PIPE_CAP_MIN_TEXTURE_GATHER_OFFSET:
   case PIPE_CAP_MAX_TEXTURE_GATHER_OFFSET:
   case PIPE_CAP_SAMPLE_SHADING:
   case PIPE_CAP_TEXTURE_GATHER_OFFSETS:
   case PIPE_CAP_TGSI_VS_WINDOW_SPACE_POSITION:
   case PIPE_CAP_MAX_VERTEX_STREAMS:
   case PIPE_CAP_DRAW_INDIRECT:
   case PIPE_CAP_TGSI_FS_FINE_DERIVATIVE:
      return 0;

   case PIPE_CAP_VENDOR_ID:
      return 0x13B5;
   case PIPE_CAP_DEVICE_ID:
      return 0xFFFFFFFF;
   case PIPE_CAP_ACCELERATED:
      return 1;
   case PIPE_CAP_VIDEO_MEMORY:
      return 0;
   case PIPE_CAP_UMA:
      return 1;

   case PIPE_CAP_CONDITIONAL_RENDER_INVERTED:
   case PIPE_CAP_MAX_VERTEX_ATTRIB_STRIDE:
   case PIPE_CAP_SAMPLER_VIEW_TARGET:
   case PIPE_CAP_CLIP_HALFZ:
   case PIPE_CAP_VERTEXID_NOBASE:
   case PIPE_CAP_POLYGON_OFFSET_CLAMP:
   case PIPE_CAP_MULTISAMPLE_Z_RESOLVE:
   case PIPE_CAP_RESOURCE_FROM_USER_MEMORY:
   case PIPE_CAP_DEVICE_RESET_STATUS_QUERY:
   case PIPE_CAP_MAX_SHADER_PATCH_VARYINGS:
   case PIPE_CAP_TEXTURE_FLOAT_LINEAR:
   case PIPE_CAP_TEXTURE_HALF_FLOAT_LINEAR:
   case PIPE_CAP_DEPTH_BOUNDS_TEST:
   case PIPE_CAP_TGSI_TXQS:
   case PIPE_CAP_FORCE_PERSAMPLE_INTERP:
   case PIPE_CAP_SHAREABLE_SHADERS:
   case PIPE_CAP_COPY_BETWEEN_COMPRESSED_AND_PLAIN_FORMATS:
   case PIPE_CAP_CLEAR_TEXTURE:
   case PIPE_CAP_DRAW_PARAMETERS:
   case PIPE_CAP_TGSI_PACK_HALF_FLOAT:
   case PIPE_CAP_MULTI_DRAW_INDIRECT:
   case PIPE_CAP_MULTI_DRAW_INDIRECT_PARAMS:
   case PIPE_CAP_TGSI_FS_POSITION_IS_SYSVAL:
   case PIPE_CAP_TGSI_FS_FACE_IS_INTEGER_SYSVAL:
   case PIPE_CAP_SHADER_BUFFER_OFFSET_ALIGNMENT:
   case PIPE_CAP_INVALIDATE_BUFFER:
   case PIPE_CAP_GENERATE_MIPMAP:
   case PIPE_CAP_STRING_MARKER:
   case PIPE_CAP_SURFACE_REINTERPRET_BLOCKS:
   case PIPE_CAP_QUERY_BUFFER_OBJECT:
   case PIPE_CAP_QUERY_MEMORY_INFO:
   case PIPE_CAP_PCI_GROUP:
   case PIPE_CAP_PCI_BUS:
   case PIPE_CAP_PCI_DEVICE:
   case PIPE_CAP_PCI_FUNCTION:
   case PIPE_CAP_FRAMEBUFFER_NO_ATTACHMENT:
   case PIPE_CAP_ROBUST_BUFFER_ACCESS_BEHAVIOR:
   case PIPE_CAP_CULL_DISTANCE:
   case PIPE_CAP_PRIMITIVE_RESTART_FOR_PATCHES:
   case PIPE_CAP_TGSI_VOTE:
   case PIPE_CAP_MAX_WINDOW_RECTANGLES:
   case PIPE_CAP_POLYGON_OFFSET_UNITS_UNSCALED:
   case PIPE_CAP_VIEWPORT_SUBPIXEL_BITS:
   case PIPE_CAP_MIXED_COLOR_DEPTH_BITS:
   case PIPE_CAP_TGSI_ARRAY_COMPONENTS:
   case PIPE_CAP_STREAM_OUTPUT_INTERLEAVE_BUFFERS:
   case PIPE_CAP_TGSI_CAN_READ_OUTPUTS:
   case PIPE_CAP_NATIVE_FENCE_FD:
   case PIPE_CAP_GLSL_OPTIMIZE_CONSERVATIVELY:
   case PIPE_CAP_TGSI_FS_FBFETCH:
   case PIPE_CAP_TGSI_MUL_ZERO_WINS:
   case PIPE_CAP_DOUBLES:
   case PIPE_CAP_INT64:
   case PIPE_CAP_INT64_DIVMOD:
   case PIPE_CAP_TGSI_TEX_TXF_LZ:
   case PIPE_CAP_TGSI_CLOCK:
   case PIPE_CAP_POLYGON_MODE_FILL_RECTANGLE:
   case PIPE_CAP_SPARSE_BUFFER_PAGE_SIZE:
   case PIPE_CAP_TGSI_BALLOT:
   case PIPE_CAP_TGSI_TES_LAYER_VIEWPORT:
      return 0;
   }

   return 0;
}

static float
lima_screen_get_paramf(struct pipe_screen *pscreen, enum pipe_capf param)
{
   switch (param) {
   case PIPE_CAPF_MAX_LINE_WIDTH:
   case PIPE_CAPF_MAX_LINE_WIDTH_AA:
   case PIPE_CAPF_MAX_POINT_WIDTH:
   case PIPE_CAPF_MAX_POINT_WIDTH_AA:
      return 255.0f;
   case PIPE_CAPF_MAX_TEXTURE_ANISOTROPY:
      return 16.0f;
   case PIPE_CAPF_MAX_TEXTURE_LOD_BIAS:
      return 16.0f;
   case PIPE_CAPF_GUARD_BAND_LEFT:
   case PIPE_CAPF_GUARD_BAND_TOP:
   case PIPE_CAPF_GUARD_BAND_RIGHT:
   case PIPE_CAPF_GUARD_BAND_BOTTOM:
      return 0.0f;
   }

   return 0;
}

static int
get_vertex_shader_param(struct lima_screen *screen,
                        enum pipe_shader_cap param)
{
   switch (param) {
   case PIPE_SHADER_CAP_MAX_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_ALU_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_TEX_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_TEX_INDIRECTIONS:
   case PIPE_SHADER_CAP_MAX_CONTROL_FLOW_DEPTH:
      return 0;

   case PIPE_SHADER_CAP_MAX_INPUTS:
      return 16; /* attributes */

   case PIPE_SHADER_CAP_MAX_OUTPUTS:
      return 16; /* varying */

   case PIPE_SHADER_CAP_MAX_CONST_BUFFER_SIZE:
      return 4096; /* need investigate */
   case PIPE_SHADER_CAP_MAX_CONST_BUFFERS:
      return 1;

   case PIPE_SHADER_CAP_MAX_TEMPS:
   case PIPE_SHADER_CAP_TGSI_CONT_SUPPORTED:
   case PIPE_SHADER_CAP_INDIRECT_INPUT_ADDR:
   case PIPE_SHADER_CAP_INDIRECT_OUTPUT_ADDR:
   case PIPE_SHADER_CAP_INDIRECT_TEMP_ADDR:
   case PIPE_SHADER_CAP_INDIRECT_CONST_ADDR:
   case PIPE_SHADER_CAP_SUBROUTINES:
   case PIPE_SHADER_CAP_INTEGERS:
   case PIPE_SHADER_CAP_MAX_TEXTURE_SAMPLERS:
      return 0;

   case PIPE_SHADER_CAP_PREFERRED_IR:
      return PIPE_SHADER_IR_NIR;

   case PIPE_SHADER_CAP_TGSI_SQRT_SUPPORTED:
   case PIPE_SHADER_CAP_MAX_SAMPLER_VIEWS:
   case PIPE_SHADER_CAP_TGSI_DROUND_SUPPORTED:
   case PIPE_SHADER_CAP_TGSI_DFRACEXP_DLDEXP_SUPPORTED:
   case PIPE_SHADER_CAP_TGSI_FMA_SUPPORTED:
   case PIPE_SHADER_CAP_TGSI_ANY_INOUT_DECL_RANGE:
   case PIPE_SHADER_CAP_MAX_UNROLL_ITERATIONS_HINT:
   case PIPE_SHADER_CAP_MAX_SHADER_BUFFERS:
   case PIPE_SHADER_CAP_SUPPORTED_IRS:
   case PIPE_SHADER_CAP_MAX_SHADER_IMAGES:
   case PIPE_SHADER_CAP_LOWER_IF_THRESHOLD:
      return 0;
   }
   return 0;
}

static int
get_fragment_shader_param(struct lima_screen *screen,
                          enum pipe_shader_cap param)
{
   switch (param) {
   case PIPE_SHADER_CAP_MAX_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_ALU_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_TEX_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_TEX_INDIRECTIONS:
   case PIPE_SHADER_CAP_MAX_CONTROL_FLOW_DEPTH:
      return 0;

   case PIPE_SHADER_CAP_MAX_INPUTS:
      return 16; /* varying */

   case PIPE_SHADER_CAP_MAX_OUTPUTS:
      return 0;

   case PIPE_SHADER_CAP_MAX_CONST_BUFFER_SIZE:
      return 4096; /* need investigate */
   case PIPE_SHADER_CAP_MAX_CONST_BUFFERS:
      return 1;

   case PIPE_SHADER_CAP_MAX_TEMPS:
   case PIPE_SHADER_CAP_TGSI_CONT_SUPPORTED:
   case PIPE_SHADER_CAP_INDIRECT_INPUT_ADDR:
   case PIPE_SHADER_CAP_INDIRECT_OUTPUT_ADDR:
   case PIPE_SHADER_CAP_INDIRECT_TEMP_ADDR:
   case PIPE_SHADER_CAP_INDIRECT_CONST_ADDR:
   case PIPE_SHADER_CAP_SUBROUTINES:
   case PIPE_SHADER_CAP_INTEGERS:
      return 0;

   case PIPE_SHADER_CAP_MAX_TEXTURE_SAMPLERS:
      return 16; /* need investigate */

   case PIPE_SHADER_CAP_PREFERRED_IR:
   case PIPE_SHADER_CAP_TGSI_SQRT_SUPPORTED:
   case PIPE_SHADER_CAP_MAX_SAMPLER_VIEWS:
   case PIPE_SHADER_CAP_TGSI_DROUND_SUPPORTED:
   case PIPE_SHADER_CAP_TGSI_DFRACEXP_DLDEXP_SUPPORTED:
   case PIPE_SHADER_CAP_TGSI_FMA_SUPPORTED:
   case PIPE_SHADER_CAP_TGSI_ANY_INOUT_DECL_RANGE:
   case PIPE_SHADER_CAP_MAX_UNROLL_ITERATIONS_HINT:
   case PIPE_SHADER_CAP_MAX_SHADER_BUFFERS:
   case PIPE_SHADER_CAP_SUPPORTED_IRS:
   case PIPE_SHADER_CAP_MAX_SHADER_IMAGES:
   case PIPE_SHADER_CAP_LOWER_IF_THRESHOLD:
      return 0;
   }
   return 0;
}

static int
lima_screen_get_shader_param(struct pipe_screen *pscreen,
                             enum pipe_shader_type shader,
                             enum pipe_shader_cap param)
{
   struct lima_screen *screen = lima_screen(pscreen);

   switch (shader) {
   case PIPE_SHADER_FRAGMENT:
      return get_fragment_shader_param(screen, param);
   case PIPE_SHADER_VERTEX:
      return get_vertex_shader_param(screen, param);
   case PIPE_SHADER_COMPUTE:
   case PIPE_SHADER_GEOMETRY:
   case PIPE_SHADER_TESS_CTRL:
   case PIPE_SHADER_TESS_EVAL:
      return 0;

   default:
      return 0;
   }

   return 0;
}

static boolean
lima_screen_is_format_supported(struct pipe_screen *pscreen,
                                enum pipe_format format,
                                enum pipe_texture_target target,
                                unsigned sample_count, unsigned usage)
{
   switch (target) {
   case PIPE_BUFFER:
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_2D:
      break;
   default:
      return FALSE;
   }

   if (sample_count)
      return FALSE;

   if (usage & PIPE_BIND_RENDER_TARGET) {
      switch (format) {
      case PIPE_FORMAT_B8G8R8A8_UNORM:
      case PIPE_FORMAT_B8G8R8X8_UNORM:
         break;
      default:
         return FALSE;
      }
   }

   if (usage & PIPE_BIND_DEPTH_STENCIL) {
      switch (format) {
      case PIPE_FORMAT_Z16_UNORM:
         break;
      default:
         return FALSE;
      }
   }

   if (usage & PIPE_BIND_VERTEX_BUFFER) {
      switch (format) {
      case PIPE_FORMAT_R32G32B32_FLOAT:
         break;
      default:
         return FALSE;
      }
   }

   if (usage & PIPE_BIND_INDEX_BUFFER) {
      switch (format) {
      case PIPE_FORMAT_I8_UINT:
      case PIPE_FORMAT_I16_UINT:
      case PIPE_FORMAT_I32_UINT:
         break;
      default:
         return FALSE;
      }
   }

   if (usage & PIPE_BIND_SAMPLER_VIEW) {
      switch (format) {
      case PIPE_FORMAT_R8G8B8X8_UNORM:
      case PIPE_FORMAT_R8G8B8A8_UNORM:
         break;
      default:
         return FALSE;
      }
   }

   return TRUE;
}

static const void *
lima_screen_get_compiler_options(struct pipe_screen *pscreen,
                                 enum pipe_shader_ir ir,
                                 enum pipe_shader_type shader)
{
   printf("dummy %s\n", __func__);
   return lima_program_get_compiler_options(shader);
}

struct pipe_screen *
lima_screen_create(int fd)
{
   struct lima_screen *screen;
   lima_device_handle dev;

   if (lima_device_create(fd, &dev))
      return NULL;

   screen = CALLOC_STRUCT(lima_screen);
   if (!screen)
      goto err_out0;

   screen->fd = fd;
   screen->dev = dev;
   if (lima_device_query_info(dev, &screen->info))
      goto err_out1;

   screen->base.destroy = lima_screen_destroy;
   screen->base.get_name = lima_screen_get_name;
   screen->base.get_vendor = lima_screen_get_vendor;
   screen->base.get_device_vendor = lima_screen_get_device_vendor;
   screen->base.get_param = lima_screen_get_param;
   screen->base.get_paramf = lima_screen_get_paramf;
   screen->base.get_shader_param = lima_screen_get_shader_param;
   screen->base.context_create = lima_context_create;
   screen->base.is_format_supported = lima_screen_is_format_supported;
   screen->base.get_compiler_options = lima_screen_get_compiler_options;

   lima_resource_screen_init(screen);

   slab_create_parent(&screen->transfer_pool, sizeof(struct lima_transfer), 16);

   screen->refcnt = 1;

   return &screen->base;

err_out1:
   FREE(screen);
err_out0:
   lima_device_delete(dev);
   return NULL;
}
