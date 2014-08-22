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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "brw_fs.h"
#include "brw_fs_live_variables.h"
#include "brw_cfg.h"

/** @file brw_fs_cmod_propagation.cpp
 *
 * Implements a pass that propagates the conditional modifier from a CMP x 0.0
 * instruction into the instruction that generated x. For instance, in this
 * sequence
 *
 *    add(8)          g70<1>F    g69<8,8,1>F    4096F
 *    cmp.ge.f0(8)    null       g70<8,8,1>F    0F
 *
 * we can do the comparison as part of the ADD instruction directly:
 *
 *    add.ge.f0(8)    g70<1>F    g69<8,8,1>F    4096F
 */

static bool
opt_cmod_propagation_local(fs_visitor *v, bblock_t *block)
{
   bool progress = false;
   int ip = block->end_ip + 1;

   foreach_inst_in_block_reverse_safe(fs_inst, inst, block) {
      ip--;

      if (inst->opcode != BRW_OPCODE_CMP ||
          inst->predicate != BRW_PREDICATE_NONE ||
          !inst->dst.is_null() ||
          inst->src[0].file != GRF ||
          inst->src[0].abs ||
          inst->src[0].negate ||
          !inst->src[1].is_zero())
         continue;

      foreach_inst_in_block_reverse_starting_from(fs_inst, scan_inst, inst,
                                                  block) {
         if (scan_inst->overwrites_reg(inst->src[0])) {
            if (scan_inst->is_partial_write() ||
                scan_inst->dst.reg_offset != inst->src[0].reg_offset)
               break;

            if (scan_inst->can_do_cmod() &&
                (scan_inst->conditional_mod == BRW_CONDITIONAL_NONE ||
                 scan_inst->conditional_mod == inst->conditional_mod)) {
               scan_inst->conditional_mod = inst->conditional_mod;
               inst->remove(block);
               progress = true;
            }
            break;
         }

         if (scan_inst->reads_flag() || scan_inst->writes_flag())
            break;
      }
   }

   return progress;
}

bool
fs_visitor::opt_cmod_propagation()
{
   bool progress = false;

   foreach_block_reverse(block, cfg) {
      progress = opt_cmod_propagation_local(this, block) || progress;
   }

   if (progress)
      invalidate_live_intervals();

   return progress;
}
