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

#include "util/u_memory.h"
#include "gpir.h"

void *gpir_node_create(gpir_compiler *comp, int size, int index, int max_parent)
{
   gpir_node *node = CALLOC(1, size + max_parent * sizeof(gpir_node *));
   if (!node)
      return NULL;

   node->max_parent = max_parent;
   if (max_parent)
      node->parents = (void *)node + size;

   if (index >= 0)
      comp->var_nodes[index] = node;

   return node;
}

void gpir_node_add_child(gpir_node *parent, gpir_node *child)
{
   parent->children[parent->num_child++] = child;
   for (int i = 0; i < child->num_parent; i++) {
      if (child->parents[i] == parent)
         return;
   }

   child->parents[child->num_parent++] = parent;
   assert(child->num_parent <= child->max_parent);
}

void gpir_node_remove_parent_cleanup(gpir_node *node)
{
   int i, j;

   /* remove holes in parents array */
   for (i = 0; i < node->num_parent; i++) {
      for (j = i; !node->parents[j]; j++);
      if (j != i) {
         node->parents[i] = node->parents[j];
         node->parents[j] = NULL;
      }
   }
}

void gpir_node_delete(gpir_node *node)
{
   assert(node->num_parent == 0);
   assert(node->num_child == 0);
   list_del(&node->list);
   FREE(node);
}
