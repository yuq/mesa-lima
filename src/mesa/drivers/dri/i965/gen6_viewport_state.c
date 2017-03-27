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
#include "intel_batchbuffer.h"
#include "main/fbobject.h"
#include "main/framebuffer.h"
#include "main/viewport.h"

static void upload_viewport_state_pointers(struct brw_context *brw)
{
   BEGIN_BATCH(4);
   OUT_BATCH(_3DSTATE_VIEWPORT_STATE_POINTERS << 16 | (4 - 2) |
	     GEN6_CC_VIEWPORT_MODIFY |
	     GEN6_SF_VIEWPORT_MODIFY |
	     GEN6_CLIP_VIEWPORT_MODIFY);
   OUT_BATCH(brw->clip.vp_offset);
   OUT_BATCH(brw->sf.vp_offset);
   OUT_BATCH(brw->cc.vp_offset);
   ADVANCE_BATCH();
}

const struct brw_tracked_state gen6_viewport_state = {
   .dirty = {
      .mesa = 0,
      .brw = BRW_NEW_BATCH |
             BRW_NEW_BLORP |
             BRW_NEW_CC_VP |
             BRW_NEW_CLIP_VP |
             BRW_NEW_SF_VP |
             BRW_NEW_STATE_BASE_ADDRESS,
   },
   .emit = upload_viewport_state_pointers,
};
