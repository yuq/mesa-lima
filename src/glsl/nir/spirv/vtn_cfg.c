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

#include "vtn_private.h"

static bool
vtn_cfg_handle_prepass_instruction(struct vtn_builder *b, SpvOp opcode,
                                   const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpFunction: {
      assert(b->func == NULL);
      b->func = rzalloc(b, struct vtn_function);

      list_inithead(&b->func->body);
      b->func->control = w[3];

      const struct glsl_type *result_type =
         vtn_value(b, w[1], vtn_value_type_type)->type->type;
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_function);
      val->func = b->func;

      const struct glsl_type *func_type =
         vtn_value(b, w[4], vtn_value_type_type)->type->type;

      assert(glsl_get_function_return_type(func_type) == result_type);

      nir_function *func =
         nir_function_create(b->shader, ralloc_strdup(b->shader, val->name));

      func->num_params = glsl_get_length(func_type);
      func->params = ralloc_array(b->shader, nir_parameter, func->num_params);
      for (unsigned i = 0; i < func->num_params; i++) {
         const struct glsl_function_param *param =
            glsl_get_function_param(func_type, i);
         func->params[i].type = param->type;
         if (param->in) {
            if (param->out) {
               func->params[i].param_type = nir_parameter_inout;
            } else {
               func->params[i].param_type = nir_parameter_in;
            }
         } else {
            if (param->out) {
               func->params[i].param_type = nir_parameter_out;
            } else {
               assert(!"Parameter is neither in nor out");
            }
         }
      }

      func->return_type = glsl_get_function_return_type(func_type);

      b->func->impl = nir_function_impl_create(func);
      if (!glsl_type_is_void(func->return_type)) {
         b->func->impl->return_var =
            nir_local_variable_create(b->func->impl, func->return_type, "ret");
      }

      b->func_param_idx = 0;
      break;
   }

   case SpvOpFunctionEnd:
      b->func->end = w;
      b->func = NULL;
      break;

   case SpvOpFunctionParameter: {
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_deref);

      assert(b->func_param_idx < b->func->impl->num_params);
      unsigned idx = b->func_param_idx++;

      nir_variable *param =
         nir_local_variable_create(b->func->impl,
                                   b->func->impl->function->params[idx].type,
                                   val->name);

      b->func->impl->params[idx] = param;
      val->deref = nir_deref_var_create(b, param);
      val->deref_type = vtn_value(b, w[1], vtn_value_type_type)->type;
      break;
   }

   case SpvOpLabel: {
      assert(b->block == NULL);
      b->block = rzalloc(b, struct vtn_block);
      b->block->node.type = vtn_cf_node_type_block;
      b->block->label = w;
      vtn_push_value(b, w[1], vtn_value_type_block)->block = b->block;

      if (b->func->start_block == NULL) {
         /* This is the first block encountered for this function.  In this
          * case, we set the start block and add it to the list of
          * implemented functions that we'll walk later.
          */
         b->func->start_block = b->block;
         exec_list_push_tail(&b->functions, &b->func->node);
      }
      break;
   }

   case SpvOpSelectionMerge:
   case SpvOpLoopMerge:
      assert(b->block && b->block->merge == NULL);
      b->block->merge = w;
      break;

   case SpvOpBranch:
   case SpvOpBranchConditional:
   case SpvOpSwitch:
   case SpvOpKill:
   case SpvOpReturn:
   case SpvOpReturnValue:
   case SpvOpUnreachable:
      assert(b->block && b->block->branch == NULL);
      b->block->branch = w;
      b->block = NULL;
      break;

   default:
      /* Continue on as per normal */
      return true;
   }

   return true;
}

static void
vtn_add_case(struct vtn_builder *b, struct vtn_switch *swtch,
             struct vtn_block *break_block,
             uint32_t block_id, uint32_t val, bool is_default)
{
   struct vtn_block *case_block =
      vtn_value(b, block_id, vtn_value_type_block)->block;

   /* Don't create dummy cases that just break */
   if (case_block == break_block)
      return;

   if (case_block->switch_case == NULL) {
      struct vtn_case *c = ralloc(b, struct vtn_case);

      list_inithead(&c->body);
      c->fallthrough = NULL;
      nir_array_init(&c->values, b);
      c->is_default = false;
      c->visited = false;

      list_addtail(&c->link, &swtch->cases);

      case_block->switch_case = c;
   }

   if (is_default) {
      case_block->switch_case->is_default = true;
   } else {
      nir_array_add(&case_block->switch_case->values, uint32_t, val);
   }
}

/* This function performs a depth-first search of the cases and puts them
 * in fall-through order.
 */
static void
vtn_order_case(struct vtn_switch *swtch, struct vtn_case *cse)
{
   if (cse->visited)
      return;

   cse->visited = true;

   list_del(&cse->link);

   if (cse->fallthrough) {
      vtn_order_case(swtch, cse->fallthrough);

      /* If we have a fall-through, place this case right before the case it
       * falls through to.  This ensures that fallthroughs come one after
       * the other.  These two can never get separated because that would
       * imply something else falling through to the same case.  Also, this
       * can't break ordering because the DFS ensures that this case is
       * visited before anything that falls through to it.
       */
      list_addtail(&cse->link, &cse->fallthrough->link);
   } else {
      list_add(&cse->link, &swtch->cases);
   }
}

static enum vtn_branch_type
vtn_get_branch_type(struct vtn_block *block,
                    struct vtn_case *swcase, struct vtn_block *switch_break,
                    struct vtn_block *loop_break, struct vtn_block *loop_cont)
{
   if (block->switch_case) {
      /* This branch is actually a fallthrough */
      assert(swcase->fallthrough == NULL ||
             swcase->fallthrough == block->switch_case);
      swcase->fallthrough = block->switch_case;
      return vtn_branch_type_switch_fallthrough;
   } else if (block == switch_break) {
      return vtn_branch_type_switch_break;
   } else if (block == loop_break) {
      return vtn_branch_type_loop_break;
   } else if (block == loop_cont) {
      return vtn_branch_type_loop_continue;
   } else {
      return vtn_branch_type_none;
   }
}

static void
vtn_cfg_walk_blocks(struct vtn_builder *b, struct list_head *cf_list,
                    struct vtn_block *start, struct vtn_case *switch_case,
                    struct vtn_block *switch_break,
                    struct vtn_block *loop_break, struct vtn_block *loop_cont,
                    struct vtn_block *end)
{
   struct vtn_block *block = start;
   while (block != end) {
      if (block->merge && (*block->merge & SpvOpCodeMask) == SpvOpLoopMerge &&
          !block->loop) {
         struct vtn_loop *loop = ralloc(b, struct vtn_loop);

         loop->node.type = vtn_cf_node_type_loop;
         list_inithead(&loop->body);
         list_inithead(&loop->cont_body);
         loop->control = block->merge[3];

         list_addtail(&loop->node.link, cf_list);
         block->loop = loop;

         struct vtn_block *new_loop_break =
            vtn_value(b, block->merge[1], vtn_value_type_block)->block;
         struct vtn_block *new_loop_cont =
            vtn_value(b, block->merge[2], vtn_value_type_block)->block;

         /* Note: This recursive call will start with the current block as
          * its start block.  If we weren't careful, we would get here
          * again and end up in infinite recursion.  This is why we set
          * block->loop above and check for it before creating one.  This
          * way, we only create the loop once and the second call that
          * tries to handle this loop goes to the cases below and gets
          * handled as a regular block.
          *
          * Note: When we make the recursive walk calls, we pass NULL for
          * the switch break since you have to break out of the loop first.
          * We do, however, still pass the current switch case because it's
          * possible that the merge block for the loop is the start of
          * another case.
          */
         vtn_cfg_walk_blocks(b, &loop->body, block, switch_case, NULL,
                             new_loop_break, new_loop_cont, NULL );
         vtn_cfg_walk_blocks(b, &loop->cont_body, new_loop_cont, NULL, NULL,
                             NULL, NULL, block);

         block = new_loop_break;
         continue;
      }

      list_addtail(&block->node.link, cf_list);

      switch (*block->branch & SpvOpCodeMask) {
      case SpvOpBranch: {
         struct vtn_block *branch_block =
            vtn_value(b, block->branch[1], vtn_value_type_block)->block;

         block->branch_type = vtn_get_branch_type(branch_block,
                                                  switch_case, switch_break,
                                                  loop_break, loop_cont);

         if (block->branch_type != vtn_branch_type_none)
            return;

         block = branch_block;
         continue;
      }

      case SpvOpReturn:
      case SpvOpReturnValue:
         block->branch_type = vtn_branch_type_return;
         return;

      case SpvOpKill:
         block->branch_type = vtn_branch_type_discard;
         return;

      case SpvOpBranchConditional: {
         struct vtn_block *then_block =
            vtn_value(b, block->branch[2], vtn_value_type_block)->block;
         struct vtn_block *else_block =
            vtn_value(b, block->branch[3], vtn_value_type_block)->block;

         struct vtn_if *if_stmt = ralloc(b, struct vtn_if);

         if_stmt->node.type = vtn_cf_node_type_if;
         if_stmt->condition = block->branch[1];
         list_inithead(&if_stmt->then_body);
         list_inithead(&if_stmt->else_body);

         list_addtail(&if_stmt->node.link, cf_list);

         /* OpBranchConditional must be at the end of a block with either
          * an OpSelectionMerge or an OpLoopMerge.
          */
         assert(block->merge);
         if ((*block->merge & SpvOpCodeMask) == SpvOpSelectionMerge)
            if_stmt->control = block->merge[2];

         if_stmt->then_type = vtn_get_branch_type(then_block,
                                                  switch_case, switch_break,
                                                  loop_break, loop_cont);
         if_stmt->else_type = vtn_get_branch_type(else_block,
                                                  switch_case, switch_break,
                                                  loop_break, loop_cont);

         if (if_stmt->then_type == vtn_branch_type_none &&
             if_stmt->else_type == vtn_branch_type_none) {
            /* Neither side of the if is something we can short-circuit. */
            assert((*block->merge & SpvOpCodeMask) == SpvOpSelectionMerge);
            struct vtn_block *merge_block =
               vtn_value(b, block->merge[1], vtn_value_type_block)->block;

            vtn_cfg_walk_blocks(b, &if_stmt->then_body, then_block,
                                switch_case, switch_break,
                                loop_break, loop_cont, merge_block);
            vtn_cfg_walk_blocks(b, &if_stmt->else_body, else_block,
                                switch_case, switch_break,
                                loop_break, loop_cont, merge_block);

            block = merge_block;
            continue;
         } else if (if_stmt->then_type != vtn_branch_type_none &&
                    if_stmt->else_type != vtn_branch_type_none) {
            /* Both sides were short-circuited.  We're done here. */
            return;
         } else {
            /* Exeactly one side of the branch could be short-circuited.
             * We set the branch up as a predicated break/continue and we
             * continue on with the other side as if it were what comes
             * after the if.
             */
            if (if_stmt->then_type == vtn_branch_type_none) {
               block = then_block;
            } else {
               block = else_block;
            }
            continue;
         }
         unreachable("Should have returned or continued");
      }

      case SpvOpSwitch: {
         assert((*block->merge & SpvOpCodeMask) == SpvOpSelectionMerge);
         struct vtn_block *break_block =
            vtn_value(b, block->merge[1], vtn_value_type_block)->block;

         struct vtn_switch *swtch = ralloc(b, struct vtn_switch);

         swtch->node.type = vtn_cf_node_type_switch;
         swtch->selector = block->branch[1];
         list_inithead(&swtch->cases);

         list_addtail(&swtch->node.link, cf_list);

         /* First, we go through and record all of the cases. */
         const uint32_t *branch_end =
            block->branch + (block->branch[0] >> SpvWordCountShift);

         vtn_add_case(b, swtch, break_block, block->branch[2], 0, true);
         for (const uint32_t *w = block->branch + 3; w < branch_end; w += 2)
            vtn_add_case(b, swtch, break_block, w[1], w[0], false);

         /* Now, we go through and walk the blocks.  While we walk through
          * the blocks, we also gather the much-needed fall-through
          * information.
          */
         for (const uint32_t *w = block->branch + 2; w < branch_end; w += 2) {
            struct vtn_block *case_block =
               vtn_value(b, *w, vtn_value_type_block)->block;

            if (case_block == break_block)
               continue;

            assert(case_block->switch_case);

            vtn_cfg_walk_blocks(b, &case_block->switch_case->body, case_block,
                                case_block->switch_case, break_block,
                                NULL, loop_cont, NULL);
         }

         /* Finally, we walk over all of the cases one more time and put
          * them in fall-through order.
          */
         for (const uint32_t *w = block->branch + 2; w < branch_end; w += 2) {
            struct vtn_block *case_block =
               vtn_value(b, *w, vtn_value_type_block)->block;

            if (case_block == break_block)
               continue;

            assert(case_block->switch_case);

            vtn_order_case(swtch, case_block->switch_case);
         }

         block = break_block;
         continue;
      }

      case SpvOpUnreachable:
      default:
         unreachable("Unhandled opcode");
      }
   }
}

void
vtn_build_cfg(struct vtn_builder *b, const uint32_t *words, const uint32_t *end)
{
   vtn_foreach_instruction(b, words, end,
                           vtn_cfg_handle_prepass_instruction);

   foreach_list_typed(struct vtn_function, func, node, &b->functions) {
      vtn_cfg_walk_blocks(b, &func->body, func->start_block,
                          NULL, NULL, NULL, NULL, NULL);
   }
}
