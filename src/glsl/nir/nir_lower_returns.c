/*
 * Copyright © 2015 Intel Corporation
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

#include "nir.h"

static bool
assert_no_returns_block(nir_block *block, void *state)
{
   (void)state;

   nir_foreach_instr(block, instr) {
      if (instr->type != nir_instr_type_jump)
         continue;

      nir_jump_instr *jump = nir_instr_as_jump(instr);
      assert(jump->type != nir_jump_return);
   }

   return true;
}

bool
nir_lower_returns_impl(nir_function_impl *impl)
{
   bool progress = false;

   assert(impl->end_block->predecessors->entries == 1);

   struct set_entry *entry =
      _mesa_set_next_entry(impl->end_block->predecessors, NULL);

   nir_block *last_block = (nir_block *)entry->key;

   nir_instr *last_instr = nir_block_last_instr(last_block);
   if (last_instr && last_instr->type == nir_instr_type_jump) {
      nir_jump_instr *jump = nir_instr_as_jump(last_instr);
      assert(jump->type == nir_jump_return);
      nir_instr_remove(&jump->instr);
      progress = true;
   }

   nir_foreach_block(impl, assert_no_returns_block, NULL);

   return progress;
}

bool
nir_lower_returns(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         progress = nir_lower_returns_impl(overload->impl) || progress;
   }

   return progress;
}
