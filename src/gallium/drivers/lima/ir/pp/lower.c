/*
 * Copyright (c) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdio.h>

#include "util/bitscan.h"

#include "ppir.h"

static bool ppir_lower_const(ppir_block *block, ppir_node *node)
{
   if (ppir_node_is_root(node))
      ppir_node_delete(node);
   return true;
}

/* lower dot to mul+sum */
static bool ppir_lower_dot(ppir_block *block, ppir_node *node)
{
   ppir_alu_node *mul = ppir_node_create(block->comp, ppir_op_mul, -1, 0);
   if (!mul)
      return false;
   list_addtail(&mul->node.list, &node->list);

   ppir_alu_node *dot = ppir_node_to_alu(node);
   mul->src[0] = dot->src[0];
   mul->src[1] = dot->src[1];
   mul->num_src = 2;

   int num_components = node->op - ppir_op_dot2 + 2;
   ppir_dest *dest = &mul->dest;
   dest->type = ppir_target_ssa;
   dest->ssa.num_components = num_components;
   dest->ssa.live_in = INT_MAX;
   dest->ssa.live_out = 0;
   dest->write_mask = u_bit_consecutive(0, num_components);

   ppir_node_foreach_pred(node, entry) {
      ppir_node *pred = ppir_node_from_entry(entry, pred);
      ppir_node_remove_entry(entry);
      ppir_node_add_child(&mul->node, pred);
   }
   ppir_node_add_child(node, &mul->node);

   if (node->op == ppir_op_dot2) {
      node->op = ppir_op_add;

      ppir_node_target_assign(dot->src, dest);
      dot->src[0].swizzle[0] = 0;
      dot->src[0].absolute = false;
      dot->src[0].negate = false;

      ppir_node_target_assign(dot->src + 1, dest);
      dot->src[1].swizzle[0] = 1;
      dot->src[1].absolute = false;
      dot->src[1].negate = false;
   }
   else {
      node->op = node->op == ppir_op_dot3 ? ppir_op_sum3 : ppir_op_sum4;

      ppir_node_target_assign(dot->src, dest);
      for (int i = 0; i < 4; i++)
         dot->src[0].swizzle[i] = i;
      dot->src[0].absolute = false;
      dot->src[0].negate = false;

      dot->num_src = 1;
   }

   return true;
}

static void merge_src(ppir_src *dst, ppir_src *src)
{
   dst->type = src->type;
   dst->ssa = src->ssa;

   for (int i = 0; i < 4; i++)
      dst->swizzle[i] = src->swizzle[dst->swizzle[i]];

   dst->absolute = dst->absolute || src->absolute;
   dst->negate = dst->negate ^ src->negate;
}

static bool ppir_lower_neg(ppir_block *block, ppir_node *node)
{
   ppir_alu_node *neg = ppir_node_to_alu(node);
   ppir_dest *dest = &neg->dest;

   ppir_node_foreach_succ(node, entry) {
      ppir_node *succ = ppir_node_from_entry(entry, succ);

      if (succ->type != ppir_node_type_alu)
         continue;

      ppir_alu_node *alu = ppir_node_to_alu(succ);
      for (int i = 0; i < alu->num_src; i++) {
         ppir_src *src = alu->src + i;
         if (ppir_node_target_equal(src, dest)) {
            merge_src(src, neg->src);
            src->negate = !src->negate;
         }
      }

      ppir_node_remove_entry(entry);

      /* TODO: may not depend on all preceeds when reg */
      ppir_node_foreach_pred(node, entry) {
         ppir_node *pred = ppir_node_from_entry(entry, pred);
         ppir_node_add_child(succ, pred);
      }
   }

   if (ppir_node_is_root(node))
      ppir_node_delete(node);
   else {
      node->op = ppir_op_mov;
      neg->src->negate = !neg->src->negate;
   }

   return true;
}

static bool (*ppir_lower_funcs[ppir_op_num])(ppir_block *, ppir_node *) = {
   [ppir_op_const] = ppir_lower_const,
   [ppir_op_dot2] = ppir_lower_dot,
   [ppir_op_dot3] = ppir_lower_dot,
   [ppir_op_dot4] = ppir_lower_dot,
   [ppir_op_neg] = ppir_lower_neg,
};

bool ppir_lower_prog(ppir_compiler *comp)
{
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(ppir_node, node, &block->node_list, list) {
         if (ppir_lower_funcs[node->op] &&
             !ppir_lower_funcs[node->op](block, node))
            return false;
      }
   }

   ppir_node_print_prog(comp);
   return true;
}
