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
#include "nir/nir_vla.h"

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
      c->start_block = case_block;
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

      assert(block->node.link.next == NULL);
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
         list_for_each_entry(struct vtn_case, cse, &swtch->cases, link) {
            assert(cse->start_block != break_block);
            vtn_cfg_walk_blocks(b, &cse->body, cse->start_block, cse,
                                break_block, NULL, loop_cont, NULL);
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

static void
vtn_emit_branch(struct vtn_builder *b, enum vtn_branch_type branch_type,
                nir_variable *switch_fall_var, bool *has_switch_break)
{
   switch (branch_type) {
   case vtn_branch_type_switch_break:
      nir_store_var(&b->nb, switch_fall_var, nir_imm_int(&b->nb, NIR_FALSE), 1);
      *has_switch_break = true;
      break;
   case vtn_branch_type_switch_fallthrough:
      break; /* Nothing to do */
   case vtn_branch_type_loop_break:
      nir_jump(&b->nb, nir_jump_break);
      break;
   case vtn_branch_type_loop_continue:
      nir_jump(&b->nb, nir_jump_continue);
      break;
   case vtn_branch_type_return:
      nir_jump(&b->nb, nir_jump_return);
      break;
   case vtn_branch_type_discard: {
      nir_intrinsic_instr *discard =
         nir_intrinsic_instr_create(b->nb.shader, nir_intrinsic_discard);
      nir_builder_instr_insert(&b->nb, &discard->instr);
      break;
   }
   default:
      unreachable("Invalid branch type");
   }
}

static void
vtn_emit_cf_list(struct vtn_builder *b, struct list_head *cf_list,
                 nir_variable *switch_fall_var, bool *has_switch_break,
                 vtn_instruction_handler handler)
{
   list_for_each_entry(struct vtn_cf_node, node, cf_list, link) {
      switch (node->type) {
      case vtn_cf_node_type_block: {
         struct vtn_block *block = (struct vtn_block *)node;

         vtn_foreach_instruction(b, block->label,
                                 block->merge ? block->merge : block->branch,
                                 handler);

         block->end_block = nir_cursor_current_block(b->nb.cursor);

         if ((*block->branch & SpvOpCodeMask) == SpvOpReturnValue) {
            struct vtn_ssa_value *src = vtn_ssa_value(b, block->branch[1]);
            vtn_variable_store(b, src,
                               nir_deref_var_create(b, b->impl->return_var),
                               NULL);
         }

         if (block->branch_type != vtn_branch_type_none) {
            vtn_emit_branch(b, block->branch_type,
                            switch_fall_var, has_switch_break);
         }

         break;
      }

      case vtn_cf_node_type_if: {
         struct vtn_if *vtn_if = (struct vtn_if *)node;

         nir_if *if_stmt = nir_if_create(b->shader);
         if_stmt->condition =
            nir_src_for_ssa(vtn_ssa_value(b, vtn_if->condition)->def);
         nir_cf_node_insert(b->nb.cursor, &if_stmt->cf_node);

         bool sw_break = false;

         b->nb.cursor = nir_after_cf_list(&if_stmt->then_list);
         if (vtn_if->then_type == vtn_branch_type_none) {
            vtn_emit_cf_list(b, &vtn_if->then_body,
                             switch_fall_var, &sw_break, handler);
         } else {
            vtn_emit_branch(b, vtn_if->then_type, switch_fall_var, &sw_break);
         }

         b->nb.cursor = nir_after_cf_list(&if_stmt->else_list);
         if (vtn_if->else_type == vtn_branch_type_none) {
            vtn_emit_cf_list(b, &vtn_if->else_body,
                             switch_fall_var, &sw_break, handler);
         } else {
            vtn_emit_branch(b, vtn_if->else_type, switch_fall_var, &sw_break);
         }

         b->nb.cursor = nir_after_cf_node(&if_stmt->cf_node);

         /* If we encountered a switch break somewhere inside of the if,
          * then it would have been handled correctly by calling
          * emit_cf_list or emit_branch for the interrior.  However, we
          * need to predicate everything following on wether or not we're
          * still going.
          */
         if (sw_break) {
            *has_switch_break = true;

            nir_if *switch_if = nir_if_create(b->shader);
            switch_if->condition =
               nir_src_for_ssa(nir_load_var(&b->nb, switch_fall_var));
            nir_cf_node_insert(b->nb.cursor, &switch_if->cf_node);

            b->nb.cursor = nir_after_cf_list(&if_stmt->then_list);
         }
         break;
      }

      case vtn_cf_node_type_loop: {
         struct vtn_loop *vtn_loop = (struct vtn_loop *)node;

         nir_loop *loop = nir_loop_create(b->shader);
         nir_cf_node_insert(b->nb.cursor, &loop->cf_node);

         if (!list_empty(&vtn_loop->cont_body)) {
            /* If we have a non-trivial continue body then we need to put
             * it at the beginning of the loop with a flag to ensure that
             * it doesn't get executed in the first iteration.
             */
            nir_variable *do_cont =
               nir_local_variable_create(b->nb.impl, glsl_bool_type(), "cont");

            b->nb.cursor = nir_before_cf_node(&loop->cf_node);
            nir_store_var(&b->nb, do_cont, nir_imm_int(&b->nb, NIR_FALSE), 1);

            b->nb.cursor = nir_after_cf_list(&loop->body);
            nir_if *cont_if = nir_if_create(b->shader);
            cont_if->condition = nir_src_for_ssa(nir_load_var(&b->nb, do_cont));
            nir_cf_node_insert(b->nb.cursor, &cont_if->cf_node);

            b->nb.cursor = nir_after_cf_list(&cont_if->then_list);
            vtn_emit_cf_list(b, &vtn_loop->cont_body, NULL, NULL, handler);

            b->nb.cursor = nir_after_cf_node(&cont_if->cf_node);
            nir_store_var(&b->nb, do_cont, nir_imm_int(&b->nb, NIR_TRUE), 1);
         }

         b->nb.cursor = nir_after_cf_list(&loop->body);
         vtn_emit_cf_list(b, &vtn_loop->body, NULL, NULL, handler);

         b->nb.cursor = nir_after_cf_node(&loop->cf_node);
         break;
      }

      case vtn_cf_node_type_switch: {
         struct vtn_switch *vtn_switch = (struct vtn_switch *)node;

         /* First, we create a variable to keep track of whether or not the
          * switch is still going at any given point.  Any switch breaks
          * will set this variable to false.
          */
         nir_variable *fall_var =
            nir_local_variable_create(b->nb.impl, glsl_bool_type(), "fall");
         nir_store_var(&b->nb, fall_var, nir_imm_int(&b->nb, NIR_FALSE), 1);

         /* Next, we gather up all of the conditions.  We have to do this
          * up-front because we also need to build an "any" condition so
          * that we can use !any for default.
          */
         const int num_cases = list_length(&vtn_switch->cases);
         NIR_VLA(nir_ssa_def *, conditions, num_cases);

         nir_ssa_def *sel = vtn_ssa_value(b, vtn_switch->selector)->def;
         /* An accumulation of all conditions.  Used for the default */
         nir_ssa_def *any = NULL;

         int i = 0;
         list_for_each_entry(struct vtn_case, cse, &vtn_switch->cases, link) {
            if (cse->is_default) {
               conditions[i++] = NULL;
               continue;
            }

            nir_ssa_def *cond = NULL;
            nir_array_foreach(&cse->values, uint32_t, val) {
               nir_ssa_def *is_val =
                  nir_ieq(&b->nb, sel, nir_imm_int(&b->nb, *val));

               cond = cond ? nir_ior(&b->nb, cond, is_val) : is_val;
            }

            any = any ? nir_ior(&b->nb, any, cond) : cond;
            conditions[i++] = cond;
         }
         assert(i == num_cases);

         /* Now we can walk the list of cases and actually emit code */
         i = 0;
         list_for_each_entry(struct vtn_case, cse, &vtn_switch->cases, link) {
            /* Figure out the condition */
            nir_ssa_def *cond = conditions[i++];
            if (cse->is_default) {
               assert(cond == NULL);
               cond = nir_inot(&b->nb, any);
            }
            /* Take fallthrough into account */
            cond = nir_ior(&b->nb, cond, nir_load_var(&b->nb, fall_var));

            nir_if *case_if = nir_if_create(b->nb.shader);
            case_if->condition = nir_src_for_ssa(cond);
            nir_cf_node_insert(b->nb.cursor, &case_if->cf_node);

            bool has_break = false;
            b->nb.cursor = nir_after_cf_list(&case_if->then_list);
            nir_store_var(&b->nb, fall_var, nir_imm_int(&b->nb, NIR_TRUE), 1);
            vtn_emit_cf_list(b, &cse->body, fall_var, &has_break, handler);
            (void)has_break; /* We don't care */

            b->nb.cursor = nir_after_cf_node(&case_if->cf_node);
         }
         assert(i == num_cases);

         break;
      }

      default:
         unreachable("Invalid CF node type");
      }
   }
}

void
vtn_function_emit(struct vtn_builder *b, struct vtn_function *func,
                  vtn_instruction_handler instruction_handler)
{
   nir_builder_init(&b->nb, func->impl);
   b->nb.cursor = nir_after_cf_list(&func->impl->body);
   vtn_emit_cf_list(b, &func->body, NULL, NULL, instruction_handler);
}
