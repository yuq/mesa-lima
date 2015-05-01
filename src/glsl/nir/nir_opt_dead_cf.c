/*
 * Copyright © 2014 Connor Abbott
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
 *    Connor Abbott (cwabbott0@gmail.com)
 *
 */

#include "nir.h"
#include "nir_control_flow.h"

/*
 * This file implements an optimization that deletes statically unreachable
 * code. In NIR, one way this can happen if if an if statement has a constant
 * condition:
 *
 * if (true) {
 *    ...
 * }
 *
 * We delete the if statement and paste the contents of the always-executed
 * branch into the surrounding control flow, possibly removing more code if
 * the branch had a jump at the end.
 */

static void
remove_after_cf_node(nir_cf_node *node)
{
   nir_cf_node *end = node;
   while (!nir_cf_node_is_last(end))
      end = nir_cf_node_next(end);

   nir_cf_list list;
   nir_cf_extract(&list, nir_after_cf_node(node), nir_after_cf_node(end));
   nir_cf_delete(&list);
}

static void
opt_constant_if(nir_if *if_stmt, bool condition)
{
   void *mem_ctx = ralloc_parent(if_stmt);

   /* First, we need to remove any phi nodes after the if by rewriting uses to
    * point to the correct source.
    */
   nir_block *after = nir_cf_node_as_block(nir_cf_node_next(&if_stmt->cf_node));
   nir_block *last_block =
      nir_cf_node_as_block(condition ? nir_if_last_then_node(if_stmt)
                                     : nir_if_last_else_node(if_stmt));

   nir_foreach_instr_safe(after, instr) {
      if (instr->type != nir_instr_type_phi)
         break;

      nir_phi_instr *phi = nir_instr_as_phi(instr);
      nir_ssa_def *def = NULL;
      nir_foreach_phi_src(phi, phi_src) {
         if (phi_src->pred != last_block)
            continue;

         assert(phi_src->src.is_ssa);
         def = phi_src->src.ssa;
      }

      assert(def);
      assert(phi->dest.is_ssa);
      nir_ssa_def_rewrite_uses(&phi->dest.ssa, nir_src_for_ssa(def), mem_ctx);
      nir_instr_remove(instr);
   }

   /* The control flow list we're about to paste in may include a jump at the
    * end, and in that case we have to delete the rest of the control flow
    * list after the if since it's unreachable and the validator will balk if
    * we don't.
    */

   if (!exec_list_is_empty(&last_block->instr_list)) {
      nir_instr *last_instr = nir_block_last_instr(last_block);
      if (last_instr->type == nir_instr_type_jump)
         remove_after_cf_node(&if_stmt->cf_node);
   }

   /* Finally, actually paste in the then or else branch and delete the if. */
   struct exec_list *cf_list = condition ? &if_stmt->then_list
                                         : &if_stmt->else_list;

   nir_cf_list list;
   nir_cf_extract(&list, nir_before_cf_list(cf_list),
                  nir_after_cf_list(cf_list));
   nir_cf_reinsert(&list, nir_after_cf_node(&if_stmt->cf_node));
   nir_cf_node_remove(&if_stmt->cf_node);
}

static bool
dead_cf_cb(nir_block *block, void *state)
{
   bool *progress = state;

   nir_if *following_if = nir_block_get_following_if(block);
   if (!following_if)
      return true;

  nir_const_value *const_value =
     nir_src_as_const_value(following_if->condition);

  if (!const_value)
     return true;

   opt_constant_if(following_if, const_value->u[0] != 0);
   *progress = true;
   return true;
}

static bool
opt_dead_cf_impl(nir_function_impl *impl)
{
   bool progress = false;
   nir_foreach_block(impl, dead_cf_cb, &progress);

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_none);

   return progress;
}

bool
nir_opt_dead_cf(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_overload(shader, overload)
      if (overload->impl)
         progress |= opt_dead_cf_impl(overload->impl);

   return progress;
}
