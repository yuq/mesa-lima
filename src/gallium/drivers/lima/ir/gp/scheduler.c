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

#define gpir_instr_array_n(buf) ((buf)->size / sizeof(gpir_instr))
#define gpir_instr_array_e(buf, idx) (util_dynarray_element(buf, gpir_instr, idx))

static gpir_instr *gpir_instr_array_grow(struct util_dynarray *instrs, int pos)
{
   int n = gpir_instr_array_n(instrs);
   if (n <= pos) {
      int size = (pos + 1 - n) * sizeof(gpir_instr);
      util_dynarray_grow(instrs, size);
      memset(gpir_instr_array_e(instrs, n), 0, size);
   }

   return gpir_instr_array_e(instrs, pos);
}

static void gpir_schedule_node(gpir_block *block, gpir_node *node)
{
   node->scheduled = true;

   const gpir_op_info *info = gpir_op_infos + node->op;
   int *slots = info->slots;
   /* not schedule node without instr slot or no latency
    * like load node which can be schduled with child */
   if (!slots || !info->latency)
      return;

   int i, start = 0;

   /* find the fist legal instr contrained by child */
   for (i = 0; i < node->num_child; i++) {
      gpir_node *child = node->children[i];
      int next = child->instr_index + gpir_op_infos[child->op].latency;
      if (next > start)
         start = next;
   }

   while (1) {
      gpir_instr *instr = gpir_instr_array_grow(&block->instrs, start);

      /* find an idle slot to insert the node */
      for (i = 0; slots[i] != GPIR_INSTR_SLOT_END; i++) {
         if (!instr->slots[slots[i]]) {
            instr->slots[slots[i]] = node;
            break;
         }
      }

      if (slots[i] != GPIR_INSTR_SLOT_END) {
         node->instr_index = start;
         break;
      }

      start++;
   }
}

static void gpir_schedule_ready_list(gpir_block *block, struct list_head *ready_list)
{
   if (list_empty(ready_list))
      return;

   gpir_node *node = list_first_entry(ready_list, gpir_node, ready);
   list_del(&node->ready);

   gpir_schedule_node(block, node);

   for (int i = 0; i < node->num_parent; i++) {
      gpir_node *parent = node->parents[i];
      bool ready = true;

      /* after all children has been scheduled */
      for (int j = 0; j < parent->num_child; j++) {
         if (!parent->children[j]->scheduled) {
            ready = false;
            break;
         }
      }

      if (ready)
         gpir_insert_ready_list(ready_list, parent);
   }

   gpir_schedule_ready_list(block, ready_list);
}

static void gpir_schedule_block(gpir_block *block)
{
   /* calculate distance start from root nodes */
   list_for_each_entry(gpir_node, node, &block->node_list, list) {
      if (!node->num_parent)
         gpir_update_distance(node, 0);
   }

   struct list_head ready_list;
   list_inithead(&ready_list);

   /* construct the ready list from leaf nodes */
   list_for_each_entry(gpir_node, node, &block->node_list, list) {
      if (!node->num_child)
         gpir_insert_ready_list(&ready_list, node);
   }

   gpir_schedule_ready_list(block, &ready_list);
}

void gpir_schedule_prog(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      gpir_schedule_block(block);
   }
}
