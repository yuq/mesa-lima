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

#include "util/u_dynarray.h"
#include "gpir.h"

static void gpir_update_distance(gpir_node *node, int d)
{
   if (d > node->distance) {
      node->distance = d;
      for (int i = 0; i < node->num_child; i++)
         gpir_update_distance(node->children[i], d + gpir_op_infos[node->op].latency);
   }
}

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

   for (int i = 0; i < root.size / sizeof(gpir_node *); i++)
      gpir_update_distance(*util_dynarray_element(&root, gpir_node *, i), 0);

   util_dynarray_fini(&root);
   util_dynarray_fini(&leaf);
}

void gpir_schedule_prog(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      gpir_schedule_block(block);
   }
}
