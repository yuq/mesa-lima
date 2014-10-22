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

#include "nir.h"

/*
 * Implements a quick-and-dirty out-of-ssa pass.
 */

struct from_ssa_state {
   void *mem_ctx;
   void *dead_ctx;
   struct hash_table *ssa_table;
   nir_function_impl *current_impl;
};

static bool
rewrite_ssa_src(nir_src *src, void *void_state)
{
   struct from_ssa_state *state = void_state;

   if (src->is_ssa) {
      struct hash_entry *entry =
         _mesa_hash_table_search(state->ssa_table, src->ssa);
      assert(entry);
      memset(src, 0, sizeof *src);
      src->reg.reg = (nir_register *)entry->data;
   }

   return true;
}

static nir_register *
reg_create_from_def(nir_ssa_def *def, struct from_ssa_state *state)
{
   nir_register *reg = nir_local_reg_create(state->current_impl);
   reg->name = def->name;
   reg->num_components = def->num_components;
   reg->num_array_elems = 0;

   /* Might as well steal the use-def information from SSA */
   _mesa_set_destroy(reg->uses, NULL);
   reg->uses = def->uses;
   _mesa_set_destroy(reg->if_uses, NULL);
   reg->if_uses = def->if_uses;
   _mesa_set_add(reg->defs, _mesa_hash_pointer(def->parent_instr),
                 def->parent_instr);

   /* Add the new register to the table and rewrite the destination */
   _mesa_hash_table_insert(state->ssa_table, def, reg);

   return reg;
}

static bool
rewrite_ssa_dest(nir_dest *dest, void *void_state)
{
   struct from_ssa_state *state = void_state;

   if (dest->is_ssa) {
      nir_register *reg = reg_create_from_def(&dest->ssa, state);
      memset(dest, 0, sizeof *dest);
      dest->reg.reg = reg;
   }

   return true;
}

static bool
convert_from_ssa_block(nir_block *block, void *void_state)
{
   struct from_ssa_state *state = void_state;

   nir_foreach_instr_safe(block, instr) {
      if (instr->type == nir_instr_type_ssa_undef) {
         nir_ssa_undef_instr *undef = nir_instr_as_ssa_undef(instr);
         reg_create_from_def(&undef->def, state);
         exec_node_remove(&instr->node);
         ralloc_steal(state->dead_ctx, instr);
      } else {
         nir_foreach_src(instr, rewrite_ssa_src, state);
         nir_foreach_dest(instr, rewrite_ssa_dest, state);
      }
   }

   if (block->cf_node.node.next != NULL && /* check that we aren't the end node */
       !nir_cf_node_is_last(&block->cf_node) &&
       nir_cf_node_next(&block->cf_node)->type == nir_cf_node_if) {
      nir_if *if_stmt = nir_cf_node_as_if(nir_cf_node_next(&block->cf_node));
      rewrite_ssa_src(&if_stmt->condition, state);
   }

   return true;
}

static bool
remove_phi_nodes(nir_block *block, void *void_state)
{
   struct from_ssa_state *state = void_state;

   nir_foreach_instr_safe(block, instr) {
      /* Phi nodes only ever come at the start of a block */
      if (instr->type != nir_instr_type_phi)
         break;

      nir_foreach_dest(instr, rewrite_ssa_dest, state);

      nir_phi_instr *phi = nir_instr_as_phi(instr);
      foreach_list_typed(nir_phi_src, src, node, &phi->srcs) {
         assert(src->src.is_ssa);
         struct hash_entry *entry =
            _mesa_hash_table_search(state->ssa_table, src->src.ssa);
         nir_alu_instr *mov = nir_alu_instr_create(state->mem_ctx, nir_op_imov);
         mov->dest.dest = nir_dest_copy(phi->dest, state->mem_ctx);
         if (entry) {
            nir_register *reg = (nir_register *)entry->data;
            mov->src[0].src.reg.reg = reg;
            mov->dest.write_mask = (1 << reg->num_components) - 1;
         } else {
            mov->src[0].src = nir_src_copy(src->src, state->mem_ctx);
            mov->dest.write_mask = (1 << src->src.ssa->num_components) - 1;
         }

         nir_instr *block_end = nir_block_last_instr(src->pred);
         if (block_end && block_end->type == nir_instr_type_jump) {
            /* If the last instruction in the block is a jump, we want to
             * place the moves after the jump.  Otherwise, we want to place
             * them at the very end.
             */
            exec_node_insert_node_before(&block_end->node, &mov->instr.node);
         } else {
            exec_list_push_tail(&src->pred->instr_list, &mov->instr.node);
         }
      }

      exec_node_remove(&instr->node);
      ralloc_steal(state->dead_ctx, instr);
   }

   return true;
}

static void
nir_convert_from_ssa_impl(nir_function_impl *impl)
{
   struct from_ssa_state state;

   state.mem_ctx = ralloc_parent(impl);
   state.dead_ctx = ralloc_context(NULL);
   state.current_impl = impl;
   state.ssa_table = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                                             _mesa_key_pointer_equal);

   nir_foreach_block(impl, remove_phi_nodes, &state);
   nir_foreach_block(impl, convert_from_ssa_block, &state);

   /* Clean up dead instructions and the hash table */
   ralloc_free(state.dead_ctx);
   _mesa_hash_table_destroy(state.ssa_table, NULL);
}

void
nir_convert_from_ssa(nir_shader *shader)
{
   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         nir_convert_from_ssa_impl(overload->impl);
   }
}
