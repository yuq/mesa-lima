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

static void gpir_update_distance(gpir_node *node, int d)
{
   if (d > node->distance) {
      node->distance = d;
      for (int i = 0; i < node->num_child; i++)
         gpir_update_distance(node->children[i], d + gpir_op_infos[node->op].latency);
   }
}

static void gpir_insert_ready_list(struct list_head *ready_list, gpir_node *insert_node)
{
   struct list_head *insert_pos = ready_list;

   list_for_each_entry(gpir_node, node, ready_list, ready) {
      if (insert_node->distance > node->distance ||
          (insert_node->distance == node->distance &&
           insert_node->num_parent > node->num_parent)) {
         insert_pos = &node->ready;
         break;
      }
   }

   list_addtail(&insert_node->ready, insert_pos);
}

static void gpir_schedule_ready_list(gpir_block *block, struct list_head *ready_list)
{
   
}

#define gpir_node_array_n(buf) ((buf)->size / sizeof(gpir_node *))
#define gpir_node_array_e(buf, idx) (*util_dynarray_element(buf, gpir_node *, idx))

static void gpir_schedule_block(gpir_block *block)
{
   struct util_dynarray root, leaf;

   util_dynarray_init(&root);
   util_dynarray_init(&leaf);

   /* collect root and leaf nodes */
   list_for_each_entry(gpir_node, node, &block->node_list, list) {
      if (!node->num_child)
         util_dynarray_append(&leaf, gpir_node *, node);
      if (!node->num_parent)
         util_dynarray_append(&root, gpir_node *, node);
   }

   for (int i = 0; i < gpir_node_array_n(&root); i++)
      gpir_update_distance(gpir_node_array_e(&root, i), 0);
   util_dynarray_fini(&root);

   struct list_head ready_list;
   list_inithead(&ready_list);

   /* construct the ready list initially from leaf nodes */
   for (int i = 0; i < gpir_node_array_n(&leaf); i++) {
      gpir_node *insert_node = gpir_node_array_e(&leaf, i);
      gpir_insert_ready_list(&ready_list, insert_node);
   }
   util_dynarray_fini(&leaf);

   gpir_schedule_ready_list(block, &ready_list);
}

void gpir_schedule_prog(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      gpir_schedule_block(block);
   }
}
