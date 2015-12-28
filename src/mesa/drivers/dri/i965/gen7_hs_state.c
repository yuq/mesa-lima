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

#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "intel_batchbuffer.h"

static void
gen7_upload_tcs_push_constants(struct brw_context *brw)
{
   struct brw_stage_state *stage_state = &brw->tcs.base;
   /* BRW_NEW_TESS_PROGRAMS */
   const struct brw_tess_ctrl_program *tcp =
      (struct brw_tess_ctrl_program *) brw->tess_ctrl_program;
   bool active = brw->tess_eval_program;

   if (active) {
      /* BRW_NEW_TCS_PROG_DATA */
      const struct brw_stage_prog_data *prog_data = &brw->tcs.prog_data->base.base;
      gen6_upload_push_constants(brw, &tcp->program.Base, prog_data,
                                      stage_state, AUB_TRACE_VS_CONSTANTS);
   }

   gen7_upload_constant_state(brw, stage_state, active, _3DSTATE_CONSTANT_HS);
}

const struct brw_tracked_state gen7_tcs_push_constants = {
   .dirty = {
      .mesa  = _NEW_PROGRAM_CONSTANTS,
      .brw   = BRW_NEW_BATCH |
               BRW_NEW_DEFAULT_TESS_LEVELS |
               BRW_NEW_PUSH_CONSTANT_ALLOCATION |
               BRW_NEW_TESS_PROGRAMS |
               BRW_NEW_TCS_PROG_DATA,
   },
   .emit = gen7_upload_tcs_push_constants,
};

static void
gen7_upload_hs_state(struct brw_context *brw)
{
   /* Disable the HS Unit */
   BEGIN_BATCH(7);
   OUT_BATCH(_3DSTATE_CONSTANT_HS << 16 | (7 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();

   BEGIN_BATCH(7);
   OUT_BATCH(_3DSTATE_HS << 16 | (7 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();

   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_BINDING_TABLE_POINTERS_HS << 16 | (2 - 2));
   OUT_BATCH(brw->hw_bt_pool.next_offset);
   ADVANCE_BATCH();
}

const struct brw_tracked_state gen7_hs_state = {
   .dirty = {
      .mesa  = 0,
      .brw   = BRW_NEW_CONTEXT,
   },
   .emit = gen7_upload_hs_state,
};
