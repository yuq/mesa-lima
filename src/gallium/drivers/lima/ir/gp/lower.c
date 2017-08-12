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

#include "gpir.h"

static void gpir_lower_const(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (node->op == gpir_op_const) {
            gpir_const_node *c = (gpir_const_node *)node;
            unsigned num_parent = node->num_parent;

            for (int i = 0; i < num_parent; i++) {
               gpir_node *p = node->parents[i];

               if (p->op == gpir_op_load_attribute ||
                   (p->op == gpir_op_store_varying && p->children[1] == node)) {
                  assert(c->num_components == 1);
                  assert(c->value[0].ui == 0);

                  if (p->op == gpir_op_load_attribute)
                     p->children[0] = NULL;
                  else
                     p->children[1] = NULL;
                  p->num_child--;

                  node->parents[i] = NULL;
                  node->num_parent--;
               }
            }

            if (node->num_parent)
               gpir_node_remove_parent_cleanup(node);
            else
               gpir_node_delete(node);
         }
      }
   }
}

static void gpir_lower_copy(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (node->op == gpir_op_copy) {
            gpir_alu_node *copy = (gpir_alu_node *)node;

            /* update all copy node parents to use copy node children directly */
            for (int i = 0; i < node->num_parent; i++) {
               gpir_node *parent = node->parents[i];
               if (parent->type == gpir_node_type_alu) {
                  assert(node->num_child == 1);
                  gpir_alu_node *alu = (gpir_alu_node *)parent;

                  for (int j = 0; j < parent->num_child; j++) {
                     if (parent->children[j] == node) {
                        parent->children[j] = node->children[0];
                        alu->children_component[j] = copy->children_component[0];
                     }
                  }
               }
               else {
                  assert(parent->type == gpir_node_type_store);
                  gpir_store_node *store = (gpir_store_node *)parent;
                  for (int j = 0; j < node->num_child; j++) {
                     parent->children[j] = node->children[j];
                     store->children_component[j] = copy->children_component[j];
                  }
                  parent->num_child = node->num_child;
               }
            }

            /* add all copy node parents to copy node children's parents */
            for (int i = 0; i < node->num_child; i++)
               gpir_node_replace_parent(node->children[i], node);

            gpir_node_delete(node);
         }
      }
   }
}

void gpir_lower_prog(gpir_compiler *comp)
{
   gpir_lower_const(comp);
   gpir_lower_copy(comp);
}
