/*
 * Copyright © 2012 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdbool.h>
#include "program/program.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "brw_wm.h"
#include "intel_batchbuffer.h"

void
gen8_upload_ps_extra(struct brw_context *brw,
                     const struct brw_wm_prog_data *prog_data)
{
   struct gl_context *ctx = &brw->ctx;
   uint32_t dw1 = 0;

   dw1 |= GEN8_PSX_PIXEL_SHADER_VALID;
   dw1 |= prog_data->computed_depth_mode << GEN8_PSX_COMPUTED_DEPTH_MODE_SHIFT;

   if (prog_data->uses_kill)
      dw1 |= GEN8_PSX_KILL_ENABLE;

   if (prog_data->num_varying_inputs != 0)
      dw1 |= GEN8_PSX_ATTRIBUTE_ENABLE;

   if (prog_data->uses_src_depth)
      dw1 |= GEN8_PSX_USES_SOURCE_DEPTH;

   if (prog_data->uses_src_w)
      dw1 |= GEN8_PSX_USES_SOURCE_W;

   if (prog_data->persample_dispatch)
      dw1 |= GEN8_PSX_SHADER_IS_PER_SAMPLE;

   /* _NEW_MULTISAMPLE | BRW_NEW_CONSERVATIVE_RASTERIZATION */
   if (prog_data->uses_sample_mask) {
      if (brw->gen >= 9) {
         if (prog_data->post_depth_coverage)
            dw1 |= BRW_PCICMS_DEPTH << GEN9_PSX_SHADER_NORMAL_COVERAGE_MASK_SHIFT;
         else if (prog_data->inner_coverage && ctx->IntelConservativeRasterization)
            dw1 |= BRW_PSICMS_INNER << GEN9_PSX_SHADER_NORMAL_COVERAGE_MASK_SHIFT;
         else
            dw1 |= BRW_PSICMS_NORMAL << GEN9_PSX_SHADER_NORMAL_COVERAGE_MASK_SHIFT;
      }
      else {
         dw1 |= GEN8_PSX_SHADER_USES_INPUT_COVERAGE_MASK;
      }
   }

   if (prog_data->uses_omask)
      dw1 |= GEN8_PSX_OMASK_TO_RENDER_TARGET;

   if (brw->gen >= 9 && prog_data->pulls_bary)
      dw1 |= GEN9_PSX_SHADER_PULLS_BARY;

   /* The stricter cross-primitive coherency guarantees that the hardware
    * gives us with the "Accesses UAV" bit set for at least one shader stage
    * and the "UAV coherency required" bit set on the 3DPRIMITIVE command are
    * redundant within the current image, atomic counter and SSBO GL APIs,
    * which all have very loose ordering and coherency requirements and
    * generally rely on the application to insert explicit barriers when a
    * shader invocation is expected to see the memory writes performed by the
    * invocations of some previous primitive.  Regardless of the value of "UAV
    * coherency required", the "Accesses UAV" bits will implicitly cause an in
    * most cases useless DC flush when the lowermost stage with the bit set
    * finishes execution.
    *
    * It would be nice to disable it, but in some cases we can't because on
    * Gen8+ it also has an influence on rasterization via the PS UAV-only
    * signal (which could be set independently from the coherency mechanism in
    * the 3DSTATE_WM command on Gen7), and because in some cases it will
    * determine whether the hardware skips execution of the fragment shader or
    * not via the ThreadDispatchEnable signal.  However if we know that
    * GEN8_PS_BLEND_HAS_WRITEABLE_RT is going to be set and
    * GEN8_PSX_PIXEL_SHADER_NO_RT_WRITE is not set it shouldn't make any
    * difference so we may just disable it here.
    *
    * Gen8 hardware tries to compute ThreadDispatchEnable for us but doesn't
    * take into account KillPixels when no depth or stencil writes are enabled.
    * In order for occlusion queries to work correctly with no attachments, we
    * need to force-enable here.
    *
    * BRW_NEW_FS_PROG_DATA | BRW_NEW_FRAGMENT_PROGRAM | _NEW_BUFFERS | _NEW_COLOR
    */
   if ((prog_data->has_side_effects || prog_data->uses_kill) &&
       !brw_color_buffer_write_enabled(brw))
      dw1 |= GEN8_PSX_SHADER_HAS_UAV;

   if (prog_data->computed_stencil) {
      assert(brw->gen >= 9);
      dw1 |= GEN9_PSX_SHADER_COMPUTES_STENCIL;
   }

   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_PS_EXTRA << 16 | (2 - 2));
   OUT_BATCH(dw1);
   ADVANCE_BATCH();
}

static void
upload_ps_extra(struct brw_context *brw)
{
   /* BRW_NEW_FS_PROG_DATA */
   gen8_upload_ps_extra(brw, brw_wm_prog_data(brw->wm.base.prog_data));
}

const struct brw_tracked_state gen8_ps_extra = {
   .dirty = {
      .mesa  = _NEW_BUFFERS | _NEW_COLOR,
      .brw   = BRW_NEW_BLORP |
               BRW_NEW_CONTEXT |
               BRW_NEW_FRAGMENT_PROGRAM |
               BRW_NEW_FS_PROG_DATA |
               BRW_NEW_CONSERVATIVE_RASTERIZATION,
   },
   .emit = upload_ps_extra,
};
