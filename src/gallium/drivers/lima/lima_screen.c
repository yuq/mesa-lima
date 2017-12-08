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

#include <string.h>

#include "util/ralloc.h"
#include "util/u_debug.h"
#include "renderonly/renderonly.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_resource.h"
#include "lima_program.h"
#include "ir/lima_ir.h"

static void
lima_screen_destroy(struct pipe_screen *pscreen)
{
   struct lima_screen *screen = lima_screen(pscreen);

   slab_destroy_parent(&screen->transfer_pool);

   free(screen->ro);
   lima_device_delete(screen->dev);
   ralloc_free(screen);
}

static const char *
lima_screen_get_name(struct pipe_screen *pscreen)
{
   struct lima_screen *screen = lima_screen(pscreen);

   switch (screen->info.gpu_type) {
   case GPU_MALI400:
     return "Mali400";
   case GPU_MALI450:
     return "Mali450";
   }
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
   case PIPE_CAP_MAX_RENDER_TARGETS:
   case PIPE_CAP_TEXTURE_SHADOW_MAP:
   case PIPE_CAP_BLEND_EQUATION_SEPARATE:
   case PIPE_CAP_USER_CONSTANT_BUFFERS:
   case PIPE_CAP_MAX_VIEWPORTS:
   case PIPE_CAP_ACCELERATED:
   case PIPE_CAP_UMA:
   case PIPE_CAP_ALLOW_MAPPED_BUFFERS_DURING_EXECUTION:
   case PIPE_CAP_TGSI_VS_LOWER_VIEWPORT_TRANSFORM:
      return 1;

   case PIPE_CAP_MAX_TEXTURE_2D_LEVELS:
      return LIMA_MAX_MIP_LEVELS;

   case PIPE_CAP_GLSL_FEATURE_LEVEL:
      return 120;

   case PIPE_CAP_VENDOR_ID:
      return 0x13B5;
   case PIPE_CAP_DEVICE_ID:
      return 0xFFFFFFFF;

   default:
      return 0;
   }
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

   default:
      return 0.0f;
   }
}

static int
get_vertex_shader_param(struct lima_screen *screen,
                        enum pipe_shader_cap param)
{
   switch (param) {
   case PIPE_SHADER_CAP_MAX_INPUTS:
      return 16; /* attributes */

   case PIPE_SHADER_CAP_MAX_OUTPUTS:
      return LIMA_MAX_VARYING_NUM; /* varying */

   case PIPE_SHADER_CAP_MAX_CONST_BUFFER_SIZE:
      return 4096; /* need investigate */
   case PIPE_SHADER_CAP_MAX_CONST_BUFFERS:
      return 1;

   case PIPE_SHADER_CAP_PREFERRED_IR:
      return PIPE_SHADER_IR_NIR;

   default:
      return 0;
   }
}

static int
get_fragment_shader_param(struct lima_screen *screen,
                          enum pipe_shader_cap param)
{
   switch (param) {
   case PIPE_SHADER_CAP_MAX_INPUTS:
      return LIMA_MAX_VARYING_NUM - 1; /* varying, minus gl_Position */

   case PIPE_SHADER_CAP_MAX_CONST_BUFFER_SIZE:
      return 4096; /* need investigate */
   case PIPE_SHADER_CAP_MAX_CONST_BUFFERS:
      return 1;

   case PIPE_SHADER_CAP_MAX_TEXTURE_SAMPLERS:
      return 16; /* need investigate */

   case PIPE_SHADER_CAP_PREFERRED_IR:
      return PIPE_SHADER_IR_NIR;

   default:
      return 0;
   }
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

   default:
      return 0;
   }
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
   debug_checkpoint();
   return lima_program_get_compiler_options(shader);
}

bool lima_shader_debug_gp = false;
bool lima_shader_debug_pp = false;

struct pipe_screen *
lima_screen_create(int fd, struct renderonly *ro)
{
   struct lima_screen *screen;
   lima_device_handle dev;

   if (lima_device_create(fd, &dev))
      return NULL;

   screen = rzalloc(NULL, struct lima_screen);
   if (!screen)
      goto err_out0;

   screen->pp_ra = ppir_regalloc_init(screen);
   if (!screen->pp_ra)
      goto err_out1;

   screen->fd = fd;

   if (ro) {
      screen->ro = renderonly_dup(ro);
      if (!screen->ro) {
         fprintf(stderr, "Failed to dup renderonly object\n");
         ralloc_free(screen);
         return NULL;
      }
   }

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

   const char *shader_debug = debug_get_option("LIMA_SHADER_DEBUG", NULL);
   if (shader_debug) {
      if (!strcmp("all", shader_debug)) {
         lima_shader_debug_gp = true;
         lima_shader_debug_pp = true;
      }
      else if (!strcmp("gp", shader_debug))
         lima_shader_debug_gp = true;
      else if (!strcmp("pp", shader_debug))
         lima_shader_debug_pp = true;
      else
         fprintf(stderr, "lima: unsupport LIMA_SHADER_DEBUG value %s\n",
                 shader_debug);

      if (lima_shader_debug_gp)
         printf("lima: enable shader GP debug\n");
      if (lima_shader_debug_pp)
         printf("lima: enable shader PP debug\n");
   }

   const char *dump_command = debug_get_option("LIMA_DUMP_COMMAND_STREAM", NULL);
   if (dump_command) {
      printf("lima: dump command stream enabled\n");
      lima_dump_command_stream = true;
   }

   return &screen->base;

err_out1:
   ralloc_free(screen);
err_out0:
   lima_device_delete(dev);
   return NULL;
}
