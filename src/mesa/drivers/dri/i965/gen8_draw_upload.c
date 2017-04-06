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

#include "main/bufferobj.h"
#include "main/context.h"
#include "main/enums.h"
#include "main/macros.h"

#include "brw_draw.h"
#include "brw_defines.h"
#include "brw_context.h"
#include "brw_state.h"

#include "intel_batchbuffer.h"
#include "intel_buffer_objects.h"

static void
gen8_emit_index_buffer(struct brw_context *brw)
{
   const struct _mesa_index_buffer *index_buffer = brw->ib.ib;
   uint32_t mocs_wb = brw->gen >= 9 ? SKL_MOCS_WB : BDW_MOCS_WB;

   if (index_buffer == NULL)
      return;

   BEGIN_BATCH(5);
   OUT_BATCH(CMD_INDEX_BUFFER << 16 | (5 - 2));
   OUT_BATCH(brw_get_index_type(index_buffer->index_size) | mocs_wb);
   OUT_RELOC64(brw->ib.bo, I915_GEM_DOMAIN_VERTEX, 0, 0);
   OUT_BATCH(brw->ib.size);
   ADVANCE_BATCH();
}

const struct brw_tracked_state gen8_index_buffer = {
   .dirty = {
      .mesa = 0,
      .brw = BRW_NEW_BATCH |
             BRW_NEW_BLORP |
             BRW_NEW_INDEX_BUFFER,
   },
   .emit = gen8_emit_index_buffer,
};

static void
gen8_emit_vf_topology(struct brw_context *brw)
{
   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_VF_TOPOLOGY << 16 | (2 - 2));
   OUT_BATCH(brw->primitive);
   ADVANCE_BATCH();
}

const struct brw_tracked_state gen8_vf_topology = {
   .dirty = {
      .mesa = 0,
      .brw = BRW_NEW_BLORP |
             BRW_NEW_PRIMITIVE,
   },
   .emit = gen8_emit_vf_topology,
};
