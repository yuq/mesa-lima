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

#include "util/ralloc.h"
#include "gpir.h"
#include "lima_context.h"

static bool gpir_lower_const(gpir_compiler *comp)
{
   int num_constant = 0;
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (node->op == gpir_op_const && !gpir_node_is_root(node))
            num_constant++;
      }
   }

   if (num_constant) {
      union fi *constant = ralloc_array(comp->prog, union fi, num_constant);
      if (!constant)
         return false;

      comp->prog->constant = constant;
      comp->prog->constant_size = num_constant * sizeof(union fi);

      int index = 0;
      list_for_each_entry(gpir_block, block, &comp->block_list, list) {
         list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
            if (node->op == gpir_op_const) {
               gpir_const_node *c = gpir_node_to_const(node);

               if (!gpir_node_is_root(node)) {
                  gpir_load_node *load = gpir_node_create(comp, gpir_op_load_uniform, -1);
                  if (!load)
                     return false;

                  load->index = comp->constant_base + (index >> 2);
                  load->component = index % 4;
                  constant[index++] = c->value;
                  gpir_node_replace_succ(&load->node, node);
                  list_addtail(&load->node.list, &node->list);

                  fprintf(stderr, "gpir: lower const create uniform %d for const %d\n",
                          load->node.index, node->index);
               }

               gpir_node_delete(node);
            }
         }
      }
   }

   return true;
}

static bool gpir_lower_neg(gpir_block *block, gpir_node *node)
{
   gpir_alu_node *neg = gpir_node_to_alu(node);
   gpir_node *child = neg->children[0];

   /* check if child can dest negate */
   if (child->type == gpir_node_type_alu) {
      /* negate must be its only successor */
      bool only = true;
      gpir_node_foreach_succ(child, entry) {
         gpir_node *succ = gpir_node_from_entry(entry, succ);
         if (succ != node) {
            only = false;
            break;
         }
      }

      if (only && gpir_op_infos[child->op].dest_neg) {
         gpir_alu_node *alu = gpir_node_to_alu(child);
         alu->dest_negate = !alu->dest_negate;

         gpir_node_replace_succ(child, node);
         gpir_node_delete(node);
         return true;
      }
   }

   /* check if child can src negate */
   gpir_node_foreach_succ(node, entry) {
      gpir_node *succ = gpir_node_from_entry(entry, succ);

      if (succ->type != gpir_node_type_alu)
         continue;

      bool success = true;
      gpir_alu_node *alu = gpir_node_to_alu(succ);
      for (int i = 0; i < alu->num_child; i++) {
         if (alu->children[i] == node) {
            if (gpir_op_infos[succ->op].src_neg[i]) {
               alu->children_negate[i] = !alu->children_negate[i];
               alu->children[i] = child;
            }
            else
               success = false;
         }
      }

      if (success) {
         gpir_node_remove_entry(entry);
         gpir_node_add_child(succ, child);
      }
   }

   if (gpir_node_is_root(node))
      gpir_node_delete(node);

   return true;
}

static bool gpir_lower_rcp(gpir_block *block, gpir_node *node)
{
   gpir_alu_node *alu = gpir_node_to_alu(node);
   gpir_node *child = alu->children[0];

   gpir_alu_node *complex2 = gpir_node_create(block->comp, gpir_op_complex2, -1);
   if (!complex2)
      return false;
   complex2->children[0] = child;
   complex2->num_child = 1;
   gpir_node_add_child(&complex2->node, child);
   list_addtail(&complex2->node.list, &node->list);

   gpir_alu_node *impl = gpir_node_create(block->comp, gpir_op_rcp_impl, -1);
   if (!impl)
      return false;
   impl->children[0] = child;
   impl->num_child = 1;
   gpir_node_add_child(&impl->node, child);
   list_addtail(&impl->node.list, &node->list);

   /* complex1 node */
   node->op = gpir_op_complex1;
   alu->children[0] = &impl->node;
   alu->children[1] = &complex2->node;
   alu->children[2] = child;
   alu->num_child = 3;
   gpir_node_add_child(node, &impl->node);
   gpir_node_add_child(node, &complex2->node);

   return true;
}

static bool (*gpir_lower_funcs[gpir_op_num])(gpir_block *, gpir_node *) = {
   [gpir_op_neg] = gpir_lower_neg,
   [gpir_op_rcp] = gpir_lower_rcp,
};

bool gpir_lower_prog(gpir_compiler *comp)
{
   if (!gpir_lower_const(comp))
      return false;

   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (gpir_lower_funcs[node->op] &&
             !gpir_lower_funcs[node->op](block, node))
            return false;
      }
   }

   gpir_node_print_prog(comp);
   return true;
}
