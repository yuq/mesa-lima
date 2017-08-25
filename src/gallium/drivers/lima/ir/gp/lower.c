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

#include "gpir.h"

static void gpir_lower_const(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (node->op == gpir_op_const) {
            gpir_const_node *c = gpir_node_to_const(node);

            if (gpir_node_is_root(node))
               gpir_node_delete(node);

            fprintf(stderr, "gpir: const lower not implemented node %d value %x\n",
                    node->index, c->value.ui);
         }
      }
   }
}

static void gpir_lower_negate(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (node->op == gpir_op_neg) {
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
                  continue;
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
         }
      }
   }
}

void gpir_lower_prog(gpir_compiler *comp)
{
   gpir_lower_negate(comp);
   gpir_lower_const(comp);
}
