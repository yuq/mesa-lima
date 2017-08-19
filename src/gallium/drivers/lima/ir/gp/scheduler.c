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

#include <limits.h>
#include <stdio.h>

#include "gpir.h"

static int gpir_min_dist_alu(gpir_dep_info *dep)
{
   switch (dep->pred->op) {
   case gpir_op_load_attribute:
      return 0;

   default:
      return 1;
   }
}

static int gpir_get_min_dist(gpir_dep_info *dep)
{
   if (dep->is_child_dep) {
      switch (dep->succ->op) {
      case gpir_op_store_varying:
         return 0;

      case gpir_op_mov:
      case gpir_op_mul:
      case gpir_op_add:
      case gpir_op_sub:
         return gpir_min_dist_alu(dep);

      default:
         return 0;
      }
   }

   return 0;
}

static int gpir_max_dist_alu(gpir_dep_info *dep)
{
   switch (dep->pred->op) {
   case gpir_op_load_attribute:
      return 1;
   default:
      break;
   }

   return 2;
}

static int gpir_get_max_dist(gpir_dep_info *dep)
{
   if (dep->is_child_dep) {
      switch (dep->succ->op) {
      case gpir_op_store_varying:
         return 0;

      case gpir_op_mov:
      case gpir_op_mul:
      case gpir_op_add:
      case gpir_op_sub:
         return gpir_max_dist_alu(dep);

      default:
         return 0;
      }
   }

   return INT_MAX >> 2; /* Don't want to overflow... */
}

static void gpir_update_distance(gpir_node *node, int d)
{
   if (d > node->sched_dist) {
      node->sched_dist = d;
      gpir_node_foreach_succ(node, entry) {
         gpir_node *succ = gpir_node_from_entry(entry, succ);
         gpir_update_distance(succ, d + gpir_get_min_dist(gpir_dep_from_entry(entry)));
      }
   }
}

static void gpir_insert_ready_list(struct list_head *ready_list, gpir_node *insert_node)
{
   struct list_head *insert_pos = ready_list;

   list_for_each_entry(gpir_node, node, ready_list, ready) {
      if (insert_node->sched_dist > node->sched_dist) {
         insert_pos = &node->ready;
         break;
      }
   }

   list_addtail(&insert_node->ready, insert_pos);
}

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
   /* not schedule node without instr slot */
   if (!slots)
      return;

   int i, start = 0, end = INT_MAX;

   /* find legal instr range contrained by successors */
   gpir_node_foreach_succ(node, entry) {
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      int min = succ->sched_instr + gpir_get_min_dist(dep);
      int max = succ->sched_instr + gpir_get_max_dist(dep);

      if (min > end || max < start)
         goto err_out;

      if (min > start)
         start = min;
      if (max < end)
         end = max;
   }

   while (start <= end) {
      gpir_instr *instr = gpir_instr_array_grow(&block->instrs, start);

      /* find an idle slot to insert the node */
      for (i = 0; slots[i] != GPIR_INSTR_SLOT_END; i++) {
         if (!instr->slots[slots[i]]) {
            instr->slots[slots[i]] = node;
            break;
         }
      }

      if (slots[i] != GPIR_INSTR_SLOT_END) {
         node->sched_instr = start;
         node->sched_pos = slots[i];
         return;
      }

      start++;
   }

err_out:
   fprintf(stderr, "gpir: fail to schedule node %s %d\n",
           gpir_op_infos[node->op].name, node->index);
}

static void gpir_schedule_ready_list(gpir_block *block, struct list_head *ready_list)
{
   if (list_empty(ready_list))
      return;

   gpir_node *node = list_first_entry(ready_list, gpir_node, ready);
   list_del(&node->ready);

   gpir_schedule_node(block, node);

   gpir_node_foreach_pred(node, entry) {
      gpir_node *pred = gpir_node_from_entry(entry, pred);
      bool ready = true;

      /* after all successor has been scheduled */
      gpir_node_foreach_succ(pred, _entry) {
         gpir_node *succ = gpir_node_from_entry(_entry, succ);
         if (!succ->scheduled) {
            ready = false;
            break;
         }
      }

      if (ready)
         gpir_insert_ready_list(ready_list, pred);
   }

   gpir_schedule_ready_list(block, ready_list);
}

static void gpir_schedule_block(gpir_block *block)
{
   /* schedule node start from root to leaf (backwork schedule)
    * we can also use forword schedule from leaf to root which more
    * suites the instruction execution sequence and human mind,
    * but backword schedule bring us some convenience for inserting
    * move and load nodes.
    */

   /* calculate distance start from leaf nodes */
   list_for_each_entry(gpir_node, node, &block->node_list, list) {
      if (gpir_node_is_leaf(node))
         gpir_update_distance(node, 0);
   }

   struct list_head ready_list;
   list_inithead(&ready_list);

   /* construct the ready list from root nodes */
   list_for_each_entry(gpir_node, node, &block->node_list, list) {
      if (gpir_node_is_root(node))
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
