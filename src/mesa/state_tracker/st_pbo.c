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

void
st_init_pbo_helpers(struct st_context *st)
{
   struct pipe_context *pipe = st->pipe;
   struct pipe_screen *screen = pipe->screen;

   st->pbo_upload.enabled =
      screen->get_param(screen, PIPE_CAP_TEXTURE_BUFFER_OBJECTS) &&
      screen->get_param(screen, PIPE_CAP_TEXTURE_BUFFER_OFFSET_ALIGNMENT) >= 1 &&
      screen->get_shader_param(screen, PIPE_SHADER_FRAGMENT, PIPE_SHADER_CAP_INTEGERS);
   if (!st->pbo_upload.enabled)
      return;

   st->pbo_upload.rgba_only =
      screen->get_param(screen, PIPE_CAP_BUFFER_SAMPLER_VIEW_RGBA_ONLY);

   if (screen->get_param(screen, PIPE_CAP_TGSI_INSTANCEID)) {
      if (screen->get_param(screen, PIPE_CAP_TGSI_VS_LAYER_VIEWPORT)) {
         st->pbo_upload.upload_layers = true;
      } else if (screen->get_param(screen, PIPE_CAP_MAX_GEOMETRY_OUTPUT_VERTICES) >= 3) {
         st->pbo_upload.upload_layers = true;
         st->pbo_upload.use_gs = true;
      }
   }

   /* Blend state */
   memset(&st->pbo_upload.blend, 0, sizeof(struct pipe_blend_state));
   st->pbo_upload.blend.rt[0].colormask = PIPE_MASK_RGBA;

   /* Rasterizer state */
   memset(&st->pbo_upload.raster, 0, sizeof(struct pipe_rasterizer_state));
   st->pbo_upload.raster.half_pixel_center = 1;
}

void
st_destroy_pbo_helpers(struct st_context *st)
{
   if (st->pbo_upload.fs) {
      cso_delete_fragment_shader(st->cso_context, st->pbo_upload.fs);
      st->pbo_upload.fs = NULL;
   }

   if (st->pbo_upload.gs) {
      cso_delete_geometry_shader(st->cso_context, st->pbo_upload.gs);
      st->pbo_upload.gs = NULL;
   }

   if (st->pbo_upload.vs) {
      cso_delete_vertex_shader(st->cso_context, st->pbo_upload.vs);
      st->pbo_upload.vs = NULL;
   }
}
