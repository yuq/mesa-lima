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

static void gpir_lower_copy(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (node->op == gpir_op_copy) {
            gpir_alu_node *copy = gpir_node_to_alu(node);
            assert(copy->num_child == 1);

            /* add copy node succ to copy node pred's succ */
            gpir_node_foreach_pred(node, entry) {
               gpir_node *pred = gpir_node_from_entry(entry, pred);
               gpir_node_merge_succ(pred, node);
               gpir_node_remove_entry(entry);
            }

            /* update copy node succ to use copy node pred */
            gpir_node_foreach_succ(node, entry) {
               gpir_node *succ = gpir_node_from_entry(entry, succ);

               if (succ->type == gpir_node_type_alu) {
                  gpir_alu_node *alu = gpir_node_to_alu(succ);
                  assert(alu->num_child == 1);

                  for (int i = 0; i < alu->num_child; i++) {
                     if (alu->children[i] == node) {
                        alu->children[i] = copy->children[0];
                        alu->children_component[i] = copy->children_component[0];
                        break;
                     }
                  }
               }
               else {
                  assert(succ->type == gpir_node_type_store);
                  gpir_store_node *store = gpir_node_to_store(succ);
                  store->child = copy->children[0];
                  store->component = copy->children_component[0];
               }

               gpir_node_remove_entry(entry);
            }

            gpir_node_delete(node);
         }
      }
   }
}

static void gpir_lower_store(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (node->type == gpir_node_type_store) {
            gpir_store_node *s = gpir_node_to_store(node);
            gpir_node *child = s->child;

            /* store node can only accept alu child, so insert a move node
             * between load node and store node
             */
            if (child->type == gpir_node_type_load) {
               gpir_node *move = gpir_node_create(comp, gpir_op_mov, -1);

               fprintf(stderr, "gpir: lower store create move %d for %s %d and %s %d\n",
                       move->index, gpir_op_infos[node->op].name, node->index,
                       gpir_op_infos[child->op].name, child->index);

               gpir_alu_node *m = gpir_node_to_alu(move);
               m->children[0] = child;
               m->num_child = 1;
               gpir_node_add_child(move, child);

               s->child = move;

               gpir_node_foreach_succ(child, entry) {
                  gpir_dep_info *dep = gpir_dep_from_entry(entry);
                  gpir_node *succ = gpir_node_from_entry(entry, succ);

                  if (succ == node) {
                     dep->pred = move;
                     _mesa_set_add_pre_hashed(move->succs, entry->hash, dep);
                     _mesa_set_remove(child->succs, entry);
                     break;
                  }
               }

               list_addtail(&move->list, &block->node_list);
            }
         }
      }
   }
}

void gpir_lower_prog(gpir_compiler *comp)
{
   gpir_lower_const(comp);
   gpir_lower_copy(comp);
   gpir_lower_store(comp);
}
