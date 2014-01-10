/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <sys/errno.h>

#include "main/condrender.h"
#include "main/glheader.h"
#include "main/mtypes.h"
#include "main/state.h"
#include "brw_context.h"
#include "brw_draw.h"
#include "brw_state.h"
#include "intel_batchbuffer.h"


static void
brw_emit_gpgpu_walker(struct brw_context *brw, const GLuint *num_groups)
{
   _mesa_problem(&brw->ctx, "TODO: implement brw_emit_gpgpu_walker");
}


static void
brw_dispatch_compute(struct gl_context *ctx, const GLuint *num_groups)
{
   struct brw_context *brw = brw_context(ctx);
   int estimated_buffer_space_needed;
   bool fail_next = false;

   if (!_mesa_check_conditional_render(ctx))
      return;

   if (ctx->NewState)
      _mesa_update_state(ctx);

   brw_validate_textures(brw);

   const int sampler_state_size = 16; /* 16 bytes */
   estimated_buffer_space_needed = 512; /* batchbuffer commands */
   estimated_buffer_space_needed += (BRW_MAX_TEX_UNIT *
                                     (sampler_state_size +
                                      sizeof(struct gen5_sampler_default_color)));
   estimated_buffer_space_needed += 1024; /* push constants */
   estimated_buffer_space_needed += 512; /* misc. pad */

   /* Flush the batch if it's approaching full, so that we don't wrap while
    * we've got validated state that needs to be in the same batch as the
    * primitives.
    */
   intel_batchbuffer_require_space(brw, estimated_buffer_space_needed,
                                   RENDER_RING);
   intel_batchbuffer_save_state(brw);

 retry:
   brw->no_batch_wrap = true;
   brw_upload_compute_state(brw);

   brw_emit_gpgpu_walker(brw, num_groups);

   brw->no_batch_wrap = false;

   if (dri_bufmgr_check_aperture_space(&brw->batch.bo, 1)) {
      if (!fail_next) {
         intel_batchbuffer_reset_to_saved(brw);
         intel_batchbuffer_flush(brw);
         fail_next = true;
         goto retry;
      } else {
         if (intel_batchbuffer_flush(brw) == -ENOSPC) {
            static bool warned = false;

            if (!warned) {
               fprintf(stderr, "i965: Single compute shader dispatch "
                       "exceeded available aperture space\n");
               warned = true;
            }
         }
      }
   }

   /* Now that we know we haven't run out of aperture space, we can safely
    * reset the dirty bits.
    */
   brw_compute_state_finished(brw);

   if (brw->always_flush_batch)
      intel_batchbuffer_flush(brw);

   brw_state_cache_check_size(brw);

   /* Note: since compute shaders can't write to framebuffers, there's no need
    * to call brw_postdraw_set_buffers_need_resolve().
    */
}


void
brw_init_compute_functions(struct dd_function_table *functions)
{
   functions->DispatchCompute = brw_dispatch_compute;
}
