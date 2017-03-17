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

#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "brw_util.h"
#include "main/macros.h"
#include "main/fbobject.h"
#include "main/framebuffer.h"
#include "intel_batchbuffer.h"

static void
upload_sbe_state(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   /* BRW_NEW_FS_PROG_DATA */
   const struct brw_wm_prog_data *wm_prog_data =
      brw_wm_prog_data(brw->wm.base.prog_data);
   uint32_t num_outputs = wm_prog_data->num_varying_inputs;
   uint32_t dw1;
   uint32_t point_sprite_enables;
   int i;
   uint16_t attr_overrides[16];
   /* _NEW_BUFFERS */
   bool render_to_fbo = _mesa_is_user_fbo(ctx->DrawBuffer);
   uint32_t point_sprite_origin;

   /* FINISHME: Attribute Swizzle Control Mode? */
   dw1 = GEN7_SBE_SWIZZLE_ENABLE | num_outputs << GEN7_SBE_NUM_OUTPUTS_SHIFT;

   /* _NEW_POINT
    *
    * Window coordinates in an FBO are inverted, which means point
    * sprite origin must be inverted.
    */
   if ((ctx->Point.SpriteOrigin == GL_LOWER_LEFT) != render_to_fbo) {
      point_sprite_origin = GEN6_SF_POINT_SPRITE_LOWERLEFT;
   } else {
      point_sprite_origin = GEN6_SF_POINT_SPRITE_UPPERLEFT;
   }
   dw1 |= point_sprite_origin;

   /* _NEW_POINT | _NEW_LIGHT | _NEW_PROGRAM,
    * BRW_NEW_FS_PROG_DATA | BRW_NEW_FRAGMENT_PROGRAM |
    * BRW_NEW_GS_PROG_DATA | BRW_NEW_PRIMITIVE | BRW_NEW_TES_PROG_DATA |
    * BRW_NEW_VUE_MAP_GEOM_OUT
    */
   uint32_t urb_entry_read_length;
   uint32_t urb_entry_read_offset;
   calculate_attr_overrides(brw, attr_overrides, &point_sprite_enables,
                            &urb_entry_read_length, &urb_entry_read_offset);
   dw1 |= urb_entry_read_length << GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT |
          urb_entry_read_offset << GEN7_SBE_URB_ENTRY_READ_OFFSET_SHIFT;

   BEGIN_BATCH(14);
   OUT_BATCH(_3DSTATE_SBE << 16 | (14 - 2));
   OUT_BATCH(dw1);

   /* Output dwords 2 through 9 */
   for (i = 0; i < 8; i++) {
      OUT_BATCH(attr_overrides[i * 2] | attr_overrides[i * 2 + 1] << 16);
   }

   OUT_BATCH(point_sprite_enables); /* dw10 */
   OUT_BATCH(wm_prog_data->flat_inputs);
   OUT_BATCH(0); /* wrapshortest enables 0-7 */
   OUT_BATCH(0); /* wrapshortest enables 8-15 */
   ADVANCE_BATCH();
}

const struct brw_tracked_state gen7_sbe_state = {
   .dirty = {
      .mesa  = _NEW_BUFFERS |
               _NEW_LIGHT |
               _NEW_POINT |
               _NEW_POLYGON |
               _NEW_PROGRAM,
      .brw   = BRW_NEW_BLORP |
               BRW_NEW_CONTEXT |
               BRW_NEW_FRAGMENT_PROGRAM |
               BRW_NEW_FS_PROG_DATA |
               BRW_NEW_GS_PROG_DATA |
               BRW_NEW_TES_PROG_DATA |
               BRW_NEW_PRIMITIVE |
               BRW_NEW_VUE_MAP_GEOM_OUT,
   },
   .emit = upload_sbe_state,
};
