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
vtn_cfg_walk_blocks(struct vtn_builder *b, struct list_head *cf_list,
                    struct vtn_block *start, struct vtn_block *break_block,
                    struct vtn_block *cont_block, struct vtn_block *end_block)
{
   struct vtn_block *block = start;
   while (block != end_block) {
      if (block->merge && (*block->merge & SpvOpCodeMask) == SpvOpLoopMerge &&
          !block->loop) {
         struct vtn_loop *loop = ralloc(b, struct vtn_loop);

         loop->node.type = vtn_cf_node_type_loop;
         list_inithead(&loop->body);
         list_inithead(&loop->cont_body);
         loop->control = block->merge[3];

         list_addtail(&loop->node.link, cf_list);
         block->loop = loop;

         struct vtn_block *loop_break =
            vtn_value(b, block->merge[1], vtn_value_type_block)->block;
         struct vtn_block *loop_cont =
            vtn_value(b, block->merge[2], vtn_value_type_block)->block;

         /* Note: This recursive call will start with the current block as
          * its start block.  If we weren't careful, we would get here
          * again and end up in infinite recursion.  This is why we set
          * block->loop above and check for it before creating one.  This
          * way, we only create the loop once and the second call that
          * tries to handle this loop goes to the cases below and gets
          * handled as a regular block.
          */
         vtn_cfg_walk_blocks(b, &loop->body, block,
                             loop_break, loop_cont, NULL );
         vtn_cfg_walk_blocks(b, &loop->cont_body, loop_cont, NULL, NULL, block);

         block = loop_break;
         continue;
      }

      list_addtail(&block->node.link, cf_list);

      switch (*block->branch & SpvOpCodeMask) {
      case SpvOpBranch: {
         struct vtn_block *branch_block =
            vtn_value(b, block->branch[1], vtn_value_type_block)->block;

         if (branch_block == break_block) {
            block->branch_type = vtn_branch_type_break;
            return;
         } else if (branch_block == cont_block) {
            block->branch_type = vtn_branch_type_continue;
            return;
         } else if (branch_block == end_block) {
            block->branch_type = vtn_branch_type_none;
            return;
         } else {
            /* If it's not one of the above, then we must be jumping to the
             * next block in the current CF list.  Just keep going.
             */
            block->branch_type = vtn_branch_type_none;
            block = branch_block;
            continue;
         }
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

         struct vtn_block *next_block = NULL;
         if (then_block == break_block) {
            if_stmt->then_type = vtn_branch_type_break;
         } else if (then_block == cont_block) {
            if_stmt->then_type = vtn_branch_type_continue;
         } else {
            if_stmt->then_type = vtn_branch_type_none;
            next_block = then_block;
         }

         if (else_block == break_block) {
            if_stmt->else_type = vtn_branch_type_break;
         } else if (else_block == cont_block) {
            if_stmt->else_type = vtn_branch_type_continue;
         } else {
            if_stmt->else_type = vtn_branch_type_none;
            next_block = else_block;
         }

         if (if_stmt->then_type == vtn_branch_type_none &&
             if_stmt->else_type == vtn_branch_type_none) {
            /* Neither side of the if is something we can short-circuit. */
            assert((*block->merge & SpvOpCodeMask) == SpvOpSelectionMerge);
            struct vtn_block *merge_block =
               vtn_value(b, block->merge[1], vtn_value_type_block)->block;

            vtn_cfg_walk_blocks(b, &if_stmt->then_body, then_block,
                                break_block, cont_block, merge_block);
            vtn_cfg_walk_blocks(b, &if_stmt->else_body, else_block,
                                break_block, cont_block, merge_block);

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
            block = next_block;
            continue;
         }
         unreachable("Should have returned or continued");
      }

      case SpvOpSwitch:
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

   foreach_list_typed(struct vtn_function, func, node, &b->functions)
      vtn_cfg_walk_blocks(b, &func->body, func->start_block, NULL, NULL, NULL);
}
