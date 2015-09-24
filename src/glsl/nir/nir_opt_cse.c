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
 *
 * Authors:
 *    Jason Ekstrand (jason@jlekstrand.net)
 *
 */

#include "nir_instr_set.h"

/*
 * Implements common subexpression elimination
 */

struct cse_state {
   void *mem_ctx;
   bool progress;
};


static bool
src_is_ssa(nir_src *src, void *data)
{
   (void) data;
   return src->is_ssa;
}

static bool
dest_is_ssa(nir_dest *dest, void *data)
{
   (void) data;
   return dest->is_ssa;
}

static bool
nir_instr_can_cse(nir_instr *instr)
{
   /* We only handle SSA. */
   if (!nir_foreach_dest(instr, dest_is_ssa, NULL) ||
       !nir_foreach_src(instr, src_is_ssa, NULL))
      return false;

   switch (instr->type) {
   case nir_instr_type_alu:
   case nir_instr_type_tex:
   case nir_instr_type_load_const:
   case nir_instr_type_phi:
      return true;
   case nir_instr_type_intrinsic: {
      const nir_intrinsic_info *info =
         &nir_intrinsic_infos[nir_instr_as_intrinsic(instr)->intrinsic];
      return (info->flags & NIR_INTRINSIC_CAN_ELIMINATE) &&
             (info->flags & NIR_INTRINSIC_CAN_REORDER) &&
             info->num_variables == 0; /* not implemented yet */
   }
   case nir_instr_type_call:
   case nir_instr_type_jump:
   case nir_instr_type_ssa_undef:
      return false;
   case nir_instr_type_parallel_copy:
   default:
      unreachable("Invalid instruction type");
   }

   return false;
}

static nir_ssa_def *
nir_instr_get_dest_ssa_def(nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_alu:
      assert(nir_instr_as_alu(instr)->dest.dest.is_ssa);
      return &nir_instr_as_alu(instr)->dest.dest.ssa;
   case nir_instr_type_tex:
      assert(nir_instr_as_tex(instr)->dest.is_ssa);
      return &nir_instr_as_tex(instr)->dest.ssa;
   case nir_instr_type_load_const:
      return &nir_instr_as_load_const(instr)->def;
   case nir_instr_type_phi:
      assert(nir_instr_as_phi(instr)->dest.is_ssa);
      return &nir_instr_as_phi(instr)->dest.ssa;
   case nir_instr_type_intrinsic:
      assert(nir_instr_as_intrinsic(instr)->dest.is_ssa);
      return &nir_instr_as_intrinsic(instr)->dest.ssa;
   default:
      unreachable("We never ask for any of these");
   }
}

static void
nir_opt_cse_instr(nir_instr *instr, struct cse_state *state)
{
   if (!nir_instr_can_cse(instr))
      return;

   for (struct exec_node *node = instr->node.prev;
        !exec_node_is_head_sentinel(node); node = node->prev) {
      nir_instr *other = exec_node_data(nir_instr, node, node);
      if (nir_instrs_equal(instr, other)) {
         nir_ssa_def *other_def = nir_instr_get_dest_ssa_def(other);
         nir_ssa_def_rewrite_uses(nir_instr_get_dest_ssa_def(instr),
                                  nir_src_for_ssa(other_def));
         nir_instr_remove(instr);
         state->progress = true;
         return;
      }
   }

   for (nir_block *block = instr->block->imm_dom;
        block != NULL; block = block->imm_dom) {
      nir_foreach_instr_reverse(block, other) {
         if (nir_instrs_equal(instr, other)) {
            nir_ssa_def *other_def = nir_instr_get_dest_ssa_def(other);
            nir_ssa_def_rewrite_uses(nir_instr_get_dest_ssa_def(instr),
                                     nir_src_for_ssa(other_def));
            nir_instr_remove(instr);
            state->progress = true;
            return;
         }
      }
   }
}

static bool
nir_opt_cse_block(nir_block *block, void *void_state)
{
   struct cse_state *state = void_state;

   nir_foreach_instr_safe(block, instr)
      nir_opt_cse_instr(instr, state);

   return true;
}

static bool
nir_opt_cse_impl(nir_function_impl *impl)
{
   struct cse_state state;

   state.mem_ctx = ralloc_parent(impl);
   state.progress = false;

   nir_metadata_require(impl, nir_metadata_dominance);

   nir_foreach_block(impl, nir_opt_cse_block, &state);

   if (state.progress)
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);

   return state.progress;
}

bool
nir_opt_cse(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         progress |= nir_opt_cse_impl(overload->impl);
   }

   return progress;
}
