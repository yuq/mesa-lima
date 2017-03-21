/*
 * Copyright © 2011 Intel Corporation
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
#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "compiler/brw_eu_defines.h"
#include "brw_util.h"
#include "brw_wm.h"
#include "program/program.h"
#include "program/prog_parameter.h"
#include "program/prog_statevars.h"
#include "main/framebuffer.h"
#include "intel_batchbuffer.h"

static void
upload_wm_state(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   /* BRW_NEW_FS_PROG_DATA */
   const struct brw_wm_prog_data *prog_data =
      brw_wm_prog_data(brw->wm.base.prog_data);
   bool writes_depth = prog_data->computed_depth_mode != BRW_PSCDEPTH_OFF;
   uint32_t dw1, dw2;

   /* _NEW_BUFFERS */
   const bool multisampled_fbo = _mesa_geometric_samples(ctx->DrawBuffer) > 1;

   dw1 = dw2 = 0;
   dw1 |= GEN7_WM_STATISTICS_ENABLE;
   dw1 |= GEN7_WM_LINE_AA_WIDTH_1_0;
   dw1 |= GEN7_WM_LINE_END_CAP_AA_WIDTH_0_5;
   dw1 |= GEN7_WM_POINT_RASTRULE_UPPER_RIGHT;

   /* _NEW_LINE */
   if (ctx->Line.StippleFlag)
      dw1 |= GEN7_WM_LINE_STIPPLE_ENABLE;

   /* _NEW_POLYGON */
   if (ctx->Polygon.StippleFlag)
      dw1 |= GEN7_WM_POLYGON_STIPPLE_ENABLE;

   if (prog_data->uses_src_depth)
      dw1 |= GEN7_WM_USES_SOURCE_DEPTH;

   if (prog_data->uses_src_w)
      dw1 |= GEN7_WM_USES_SOURCE_W;

   dw1 |= prog_data->computed_depth_mode << GEN7_WM_COMPUTED_DEPTH_MODE_SHIFT;
   dw1 |= prog_data->barycentric_interp_modes <<
      GEN7_WM_BARYCENTRIC_INTERPOLATION_MODE_SHIFT;

   /* _NEW_COLOR, _NEW_MULTISAMPLE _NEW_BUFFERS */
   /* Enable if the pixel shader kernel generates and outputs oMask.
    */
   if (prog_data->uses_kill ||
       _mesa_is_alpha_test_enabled(ctx) ||
       _mesa_is_alpha_to_coverage_enabled(ctx) ||
       prog_data->uses_omask) {
      dw1 |= GEN7_WM_KILL_ENABLE;
   }

   /* _NEW_BUFFERS | _NEW_COLOR */
   if (brw_color_buffer_write_enabled(brw) || writes_depth ||
       prog_data->has_side_effects || dw1 & GEN7_WM_KILL_ENABLE) {
      dw1 |= GEN7_WM_DISPATCH_ENABLE;
   }
   if (multisampled_fbo) {
      /* _NEW_MULTISAMPLE */
      if (ctx->Multisample.Enabled)
         dw1 |= GEN7_WM_MSRAST_ON_PATTERN;
      else
         dw1 |= GEN7_WM_MSRAST_OFF_PIXEL;

      if (prog_data->persample_dispatch)
         dw2 |= GEN7_WM_MSDISPMODE_PERSAMPLE;
      else
         dw2 |= GEN7_WM_MSDISPMODE_PERPIXEL;
   } else {
      dw1 |= GEN7_WM_MSRAST_OFF_PIXEL;
      dw2 |= GEN7_WM_MSDISPMODE_PERSAMPLE;
   }

   if (prog_data->uses_sample_mask) {
      dw1 |= GEN7_WM_USES_INPUT_COVERAGE_MASK;
   }

   /* BRW_NEW_FS_PROG_DATA */
   if (prog_data->early_fragment_tests)
      dw1 |= GEN7_WM_EARLY_DS_CONTROL_PREPS;
   else if (prog_data->has_side_effects)
      dw1 |= GEN7_WM_EARLY_DS_CONTROL_PSEXEC;

   /* The "UAV access enable" bits are unnecessary on HSW because they only
    * seem to have an effect on the HW-assisted coherency mechanism which we
    * don't need, and the rasterization-related UAV_ONLY flag and the
    * DISPATCH_ENABLE bit can be set independently from it.
    * C.f. gen8_upload_ps_extra().
    *
    * BRW_NEW_FRAGMENT_PROGRAM | BRW_NEW_FS_PROG_DATA | _NEW_BUFFERS | _NEW_COLOR
    */
   if (brw->is_haswell &&
       !(brw_color_buffer_write_enabled(brw) || writes_depth) &&
       prog_data->has_side_effects)
      dw2 |= HSW_WM_UAV_ONLY;

   BEGIN_BATCH(3);
   OUT_BATCH(_3DSTATE_WM << 16 | (3 - 2));
   OUT_BATCH(dw1);
   OUT_BATCH(dw2);
   ADVANCE_BATCH();
}

const struct brw_tracked_state gen7_wm_state = {
   .dirty = {
      .mesa  = _NEW_BUFFERS |
               _NEW_COLOR |
               _NEW_LINE |
               _NEW_MULTISAMPLE |
               _NEW_POLYGON,
      .brw   = BRW_NEW_BATCH |
               BRW_NEW_BLORP |
               BRW_NEW_FS_PROG_DATA,
   },
   .emit = upload_wm_state,
};
