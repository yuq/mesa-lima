/*
 * Copyright 2007 VMware, Inc.
 * Copyright 2016 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * \file
 *
 * Common helper functions for PBO up- and downloads.
 */

#include "state_tracker/st_context.h"
#include "state_tracker/st_pbo.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "cso_cache/cso_context.h"
#include "tgsi/tgsi_ureg.h"

void *
st_pbo_create_vs(struct st_context *st)
{
   struct ureg_program *ureg;
   struct ureg_src in_pos;
   struct ureg_src in_instanceid;
   struct ureg_dst out_pos;
   struct ureg_dst out_layer;

   ureg = ureg_create(PIPE_SHADER_VERTEX);
   if (!ureg)
      return NULL;

   in_pos = ureg_DECL_vs_input(ureg, TGSI_SEMANTIC_POSITION);

   out_pos = ureg_DECL_output(ureg, TGSI_SEMANTIC_POSITION, 0);

   if (st->pbo.layers) {
      in_instanceid = ureg_DECL_system_value(ureg, TGSI_SEMANTIC_INSTANCEID, 0);

      if (!st->pbo.use_gs)
         out_layer = ureg_DECL_output(ureg, TGSI_SEMANTIC_LAYER, 0);
   }

   /* out_pos = in_pos */
   ureg_MOV(ureg, out_pos, in_pos);

   if (st->pbo.layers) {
      if (st->pbo.use_gs) {
         /* out_pos.z = i2f(gl_InstanceID) */
         ureg_I2F(ureg, ureg_writemask(out_pos, TGSI_WRITEMASK_Z),
                        ureg_scalar(in_instanceid, TGSI_SWIZZLE_X));
      } else {
         /* out_layer = gl_InstanceID */
         ureg_MOV(ureg, out_layer, in_instanceid);
      }
   }

   ureg_END(ureg);

   return ureg_create_shader_and_destroy(ureg, st->pipe);
}

void *
st_pbo_create_gs(struct st_context *st)
{
   static const int zero = 0;
   struct ureg_program *ureg;
   struct ureg_dst out_pos;
   struct ureg_dst out_layer;
   struct ureg_src in_pos;
   struct ureg_src imm;
   unsigned i;

   ureg = ureg_create(PIPE_SHADER_GEOMETRY);
   if (!ureg)
      return NULL;

   ureg_property(ureg, TGSI_PROPERTY_GS_INPUT_PRIM, PIPE_PRIM_TRIANGLES);
   ureg_property(ureg, TGSI_PROPERTY_GS_OUTPUT_PRIM, PIPE_PRIM_TRIANGLE_STRIP);
   ureg_property(ureg, TGSI_PROPERTY_GS_MAX_OUTPUT_VERTICES, 3);

   out_pos = ureg_DECL_output(ureg, TGSI_SEMANTIC_POSITION, 0);
   out_layer = ureg_DECL_output(ureg, TGSI_SEMANTIC_LAYER, 0);

   in_pos = ureg_DECL_input(ureg, TGSI_SEMANTIC_POSITION, 0, 0, 1);

   imm = ureg_DECL_immediate_int(ureg, &zero, 1);

   for (i = 0; i < 3; ++i) {
      struct ureg_src in_pos_vertex = ureg_src_dimension(in_pos, i);

      /* out_pos = in_pos[i] */
      ureg_MOV(ureg, out_pos, in_pos_vertex);

      /* out_layer.x = f2i(in_pos[i].z) */
      ureg_F2I(ureg, ureg_writemask(out_layer, TGSI_WRITEMASK_X),
                     ureg_scalar(in_pos_vertex, TGSI_SWIZZLE_Z));

      ureg_EMIT(ureg, ureg_scalar(imm, TGSI_SWIZZLE_X));
   }

   ureg_END(ureg);

   return ureg_create_shader_and_destroy(ureg, st->pipe);
}

void *
st_pbo_create_upload_fs(struct st_context *st)
{
   struct pipe_context *pipe = st->pipe;
   struct pipe_screen *screen = pipe->screen;
   struct ureg_program *ureg;
   struct ureg_dst out;
   struct ureg_src sampler;
   struct ureg_src pos;
   struct ureg_src layer;
   struct ureg_src const0;
   struct ureg_dst temp0;

   ureg = ureg_create(PIPE_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   out     = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   sampler = ureg_DECL_sampler(ureg, 0);
   if (screen->get_param(screen, PIPE_CAP_TGSI_FS_POSITION_IS_SYSVAL)) {
      pos = ureg_DECL_system_value(ureg, TGSI_SEMANTIC_POSITION, 0);
   } else {
      pos = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_POSITION, 0,
                               TGSI_INTERPOLATE_LINEAR);
   }
   if (st->pbo.layers) {
      layer = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_LAYER, 0,
                                       TGSI_INTERPOLATE_CONSTANT);
   }
   const0  = ureg_DECL_constant(ureg, 0);
   temp0   = ureg_DECL_temporary(ureg);

   /* Note: const0 = [ -xoffset + skip_pixels, -yoffset, stride, image_height ] */

   /* temp0.xy = f2i(temp0.xy) */
   ureg_F2I(ureg, ureg_writemask(temp0, TGSI_WRITEMASK_XY),
                  ureg_swizzle(pos,
                               TGSI_SWIZZLE_X, TGSI_SWIZZLE_Y,
                               TGSI_SWIZZLE_Y, TGSI_SWIZZLE_Y));

   /* temp0.xy = temp0.xy + const0.xy */
   ureg_UADD(ureg, ureg_writemask(temp0, TGSI_WRITEMASK_XY),
                   ureg_swizzle(ureg_src(temp0),
                                TGSI_SWIZZLE_X, TGSI_SWIZZLE_Y,
                                TGSI_SWIZZLE_Y, TGSI_SWIZZLE_Y),
                   ureg_swizzle(const0,
                                TGSI_SWIZZLE_X, TGSI_SWIZZLE_Y,
                                TGSI_SWIZZLE_Y, TGSI_SWIZZLE_Y));

   /* temp0.x = const0.z * temp0.y + temp0.x */
   ureg_UMAD(ureg, ureg_writemask(temp0, TGSI_WRITEMASK_X),
                   ureg_scalar(const0, TGSI_SWIZZLE_Z),
                   ureg_scalar(ureg_src(temp0), TGSI_SWIZZLE_Y),
                   ureg_scalar(ureg_src(temp0), TGSI_SWIZZLE_X));

   if (st->pbo.layers) {
      /* temp0.x = const0.w * layer + temp0.x */
      ureg_UMAD(ureg, ureg_writemask(temp0, TGSI_WRITEMASK_X),
                      ureg_scalar(const0, TGSI_SWIZZLE_W),
                      ureg_scalar(layer, TGSI_SWIZZLE_X),
                      ureg_scalar(ureg_src(temp0), TGSI_SWIZZLE_X));
   }

   /* temp0.w = 0 */
   ureg_MOV(ureg, ureg_writemask(temp0, TGSI_WRITEMASK_W), ureg_imm1u(ureg, 0));

   /* out = txf(sampler, temp0.x) */
   ureg_TXF(ureg, out, TGSI_TEXTURE_BUFFER, ureg_src(temp0), sampler);

   ureg_release_temporary(ureg, temp0);

   ureg_END(ureg);

   return ureg_create_shader_and_destroy(ureg, pipe);
}

void
st_init_pbo_helpers(struct st_context *st)
{
   struct pipe_context *pipe = st->pipe;
   struct pipe_screen *screen = pipe->screen;

   st->pbo.upload_enabled =
      screen->get_param(screen, PIPE_CAP_TEXTURE_BUFFER_OBJECTS) &&
      screen->get_param(screen, PIPE_CAP_TEXTURE_BUFFER_OFFSET_ALIGNMENT) >= 1 &&
      screen->get_shader_param(screen, PIPE_SHADER_FRAGMENT, PIPE_SHADER_CAP_INTEGERS);
   if (!st->pbo.upload_enabled)
      return;

   st->pbo.rgba_only =
      screen->get_param(screen, PIPE_CAP_BUFFER_SAMPLER_VIEW_RGBA_ONLY);

   if (screen->get_param(screen, PIPE_CAP_TGSI_INSTANCEID)) {
      if (screen->get_param(screen, PIPE_CAP_TGSI_VS_LAYER_VIEWPORT)) {
         st->pbo.layers = true;
      } else if (screen->get_param(screen, PIPE_CAP_MAX_GEOMETRY_OUTPUT_VERTICES) >= 3) {
         st->pbo.layers = true;
         st->pbo.use_gs = true;
      }
   }

   /* Blend state */
   memset(&st->pbo.upload_blend, 0, sizeof(struct pipe_blend_state));
   st->pbo.upload_blend.rt[0].colormask = PIPE_MASK_RGBA;

   /* Rasterizer state */
   memset(&st->pbo.raster, 0, sizeof(struct pipe_rasterizer_state));
   st->pbo.raster.half_pixel_center = 1;
}

void
st_destroy_pbo_helpers(struct st_context *st)
{
   if (st->pbo.upload_fs) {
      cso_delete_fragment_shader(st->cso_context, st->pbo.upload_fs);
      st->pbo.upload_fs = NULL;
   }

   if (st->pbo.gs) {
      cso_delete_geometry_shader(st->cso_context, st->pbo.gs);
      st->pbo.gs = NULL;
   }

   if (st->pbo.vs) {
      cso_delete_vertex_shader(st->cso_context, st->pbo.vs);
      st->pbo.vs = NULL;
   }
}
