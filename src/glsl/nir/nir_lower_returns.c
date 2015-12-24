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
#include "nir_builder.h"
#include "nir_control_flow.h"

struct lower_returns_state {
   nir_builder builder;
   struct exec_list *parent_cf_list;
   struct exec_list *cf_list;
   nir_loop *loop;
   nir_if *if_stmt;
   nir_variable *return_flag;
};

static bool lower_returns_in_cf_list(struct exec_list *cf_list,
                                     struct lower_returns_state *state);

static bool
lower_returns_in_loop(nir_loop *loop, struct lower_returns_state *state)
{
   nir_loop *parent = state->loop;
   state->loop = loop;
   bool progress = lower_returns_in_cf_list(&loop->body, state);
   state->loop = parent;

   /* Nothing interesting */
   if (!progress)
      return false;

   /* In this case, there was a return somewhere inside of the loop.  That
    * return would have been turned into a write to the return_flag
    * variable and a break.  We need to insert a predicated return right
    * after the loop ends.
    */

   assert(state->return_flag);

   nir_intrinsic_instr *load =
      nir_intrinsic_instr_create(state->builder.shader, nir_intrinsic_load_var);
   load->num_components = 1;
   load->variables[0] = nir_deref_var_create(load, state->return_flag);
   nir_ssa_dest_init(&load->instr, &load->dest, 1, "return");
   nir_instr_insert(nir_after_cf_node(&loop->cf_node), &load->instr);

   nir_if *if_stmt = nir_if_create(state->builder.shader);
   if_stmt->condition = nir_src_for_ssa(&load->dest.ssa);
   nir_cf_node_insert(nir_after_instr(&load->instr), &if_stmt->cf_node);

   nir_jump_instr *ret =
      nir_jump_instr_create(state->builder.shader, nir_jump_return);
   nir_instr_insert(nir_before_cf_list(&if_stmt->then_list), &ret->instr);

   return true;
}

static bool
lower_returns_in_if(nir_if *if_stmt, struct lower_returns_state *state)
{
   bool progress;

   nir_if *parent = state->if_stmt;
   state->if_stmt = if_stmt;
   progress = lower_returns_in_cf_list(&if_stmt->then_list, state);
   progress = lower_returns_in_cf_list(&if_stmt->else_list, state) || progress;
   state->if_stmt = parent;

   return progress;
}

static bool
lower_returns_in_block(nir_block *block, struct lower_returns_state *state)
{
   if (block->predecessors->entries == 0 &&
       block != nir_start_block(state->builder.impl)) {
      /* This block is unreachable.  Delete it and everything after it. */
      nir_cf_list list;
      nir_cf_extract(&list, nir_before_cf_node(&block->cf_node),
                            nir_after_cf_list(state->cf_list));

      if (exec_list_is_empty(&list.list)) {
         /* There's nothing here, which also means there's nothing in this
          * block so we have nothing to do.
          */
         return false;
      } else {
         nir_cf_delete(&list);
         return true;
      }
   }

   nir_instr *last_instr = nir_block_last_instr(block);
   if (last_instr == NULL)
      return false;

   if (last_instr->type != nir_instr_type_jump)
      return false;

   nir_jump_instr *jump = nir_instr_as_jump(last_instr);
   if (jump->type != nir_jump_return)
      return false;

   if (state->loop) {
      /* We're in a loop.  Just set the return flag to true and break.
       * lower_returns_in_loop will do the rest.
       */
      nir_builder *b = &state->builder;
      b->cursor = nir_before_instr(&jump->instr);

      if (state->return_flag == NULL) {
         state->return_flag =
            nir_local_variable_create(b->impl, glsl_bool_type(), "return");

         /* Set a default value of false */
         state->return_flag->constant_initializer =
            rzalloc(state->return_flag, nir_constant);
      }

      nir_store_var(b, state->return_flag, nir_imm_int(b, NIR_TRUE));
      jump->type = nir_jump_return;
   } else if (state->if_stmt) {
      /* If we're not in a loop but in an if, just move the rest of the CF
       * list into the the other case of the if.
       */
      nir_cf_list list;
      nir_cf_extract(&list, nir_after_cf_node(&state->if_stmt->cf_node),
                            nir_after_cf_list(state->parent_cf_list));

      nir_instr_remove(&jump->instr);

      if (state->cf_list == &state->if_stmt->then_list) {
         nir_cf_reinsert(&list,
                         nir_after_cf_list(&state->if_stmt->else_list));
      } else if (state->cf_list == &state->if_stmt->else_list) {
         nir_cf_reinsert(&list,
                         nir_after_cf_list(&state->if_stmt->then_list));
      } else {
         unreachable("Invalid CF list");
      }
   } else {
      nir_instr_remove(&jump->instr);

      /* No if, no nothing.  Just delete the return and whatever follows. */
      nir_cf_list list;
      nir_cf_extract(&list, nir_after_cf_node(&block->cf_node),
                            nir_after_cf_list(state->parent_cf_list));
      nir_cf_delete(&list);
   }

   return true;
}

static bool
lower_returns_in_cf_list(struct exec_list *cf_list,
                         struct lower_returns_state *state)
{
   bool progress = false;

   struct exec_list *prev_parent_list = state->parent_cf_list;
   state->parent_cf_list = state->cf_list;
   state->cf_list = cf_list;

   foreach_list_typed_reverse_safe(nir_cf_node, node, node, cf_list) {
      switch (node->type) {
      case nir_cf_node_block:
         if (lower_returns_in_block(nir_cf_node_as_block(node), state))
            progress = true;
         break;

      case nir_cf_node_if:
         if (lower_returns_in_if(nir_cf_node_as_if(node), state))
            progress = true;
         break;

      case nir_cf_node_loop:
         if (lower_returns_in_loop(nir_cf_node_as_loop(node), state))
            progress = true;
         break;

      default:
         unreachable("Invalid inner CF node type");
      }
   }

   state->cf_list = state->parent_cf_list;
   state->parent_cf_list = prev_parent_list;

   return progress;
}

bool
nir_lower_returns_impl(nir_function_impl *impl)
{
   struct lower_returns_state state;

   state.parent_cf_list = NULL;
   state.cf_list = &impl->body;
   state.loop = NULL;
   state.if_stmt = NULL;
   state.return_flag = NULL;
   nir_builder_init(&state.builder, impl);

   bool progress = lower_returns_in_cf_list(&impl->body, &state);

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_none);

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
