/*
 * Copyright © 2009 Intel Corporation
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
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "brw_util.h"
#include "intel_batchbuffer.h"
#include "main/macros.h"
#include "main/enums.h"
#include "main/glformats.h"
#include "main/stencil.h"

static void
gen6_upload_color_calc_state(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   struct gen6_color_calc_state *cc;

   cc = brw_state_batch(brw, sizeof(*cc), 64, &brw->cc.state_offset);
   memset(cc, 0, sizeof(*cc));

   /* _NEW_COLOR */
   cc->cc0.alpha_test_format = BRW_ALPHATEST_FORMAT_UNORM8;
   UNCLAMPED_FLOAT_TO_UBYTE(cc->cc1.alpha_ref_fi.ui, ctx->Color.AlphaRef);

   if (brw->gen < 9) {
      /* _NEW_STENCIL */
      cc->cc0.stencil_ref = _mesa_get_stencil_ref(ctx, 0);
      cc->cc0.bf_stencil_ref =
         _mesa_get_stencil_ref(ctx, ctx->Stencil._BackFace);
   }

   /* _NEW_COLOR */
   cc->constant_r = ctx->Color.BlendColorUnclamped[0];
   cc->constant_g = ctx->Color.BlendColorUnclamped[1];
   cc->constant_b = ctx->Color.BlendColorUnclamped[2];
   cc->constant_a = ctx->Color.BlendColorUnclamped[3];

   /* Point the GPU at the new indirect state. */
   if (brw->gen == 6) {
      BEGIN_BATCH(4);
      OUT_BATCH(_3DSTATE_CC_STATE_POINTERS << 16 | (4 - 2));
      OUT_BATCH(0);
      OUT_BATCH(0);
      OUT_BATCH(brw->cc.state_offset | 1);
      ADVANCE_BATCH();
   } else {
      BEGIN_BATCH(2);
      OUT_BATCH(_3DSTATE_CC_STATE_POINTERS << 16 | (2 - 2));
      OUT_BATCH(brw->cc.state_offset | 1);
      ADVANCE_BATCH();
   }
}

const struct brw_tracked_state gen6_color_calc_state = {
   .dirty = {
      .mesa = _NEW_COLOR |
              _NEW_STENCIL,
      .brw = BRW_NEW_BATCH |
             BRW_NEW_BLORP |
             BRW_NEW_CC_STATE |
             BRW_NEW_STATE_BASE_ADDRESS,
   },
   .emit = gen6_upload_color_calc_state,
};
