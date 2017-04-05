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
#include "main/shaderapi.h"

void
upload_gs_state_for_tf(struct brw_context *brw)
{
   const struct gen_device_info *devinfo = &brw->screen->devinfo;

   BEGIN_BATCH(7);
   OUT_BATCH(_3DSTATE_GS << 16 | (7 - 2));
   OUT_BATCH(brw->ff_gs.prog_offset);
   OUT_BATCH(GEN6_GS_SPF_MODE | GEN6_GS_VECTOR_MASK_ENABLE);
   OUT_BATCH(0); /* no scratch space */
   OUT_BATCH((2 << GEN6_GS_DISPATCH_START_GRF_SHIFT) |
             (brw->ff_gs.prog_data->urb_read_length << GEN6_GS_URB_READ_LENGTH_SHIFT));
   OUT_BATCH(((devinfo->max_gs_threads - 1) << GEN6_GS_MAX_THREADS_SHIFT) |
             GEN6_GS_STATISTICS_ENABLE |
             GEN6_GS_SO_STATISTICS_ENABLE |
             GEN6_GS_RENDERING_ENABLE);
   OUT_BATCH(GEN6_GS_SVBI_PAYLOAD_ENABLE |
             GEN6_GS_SVBI_POSTINCREMENT_ENABLE |
             (brw->ff_gs.prog_data->svbi_postincrement_value <<
              GEN6_GS_SVBI_POSTINCREMENT_VALUE_SHIFT) |
             GEN6_GS_ENABLE);
   ADVANCE_BATCH();
}
