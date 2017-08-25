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
   case gpir_op_load_uniform:
   case gpir_op_load_temp:
   case gpir_op_load_reg:
   case gpir_op_load_attribute:
      return 0;

   case gpir_op_complex1:
      return 2;

   default:
      return 1;
   }
}

static int gpir_get_min_dist(gpir_dep_info *dep)
{
   if (dep->is_child_dep) {
      switch (dep->succ->op) {
      case gpir_op_store_temp:
         if (dep->is_offset)
            return gpir_min_dist_alu(dep);
         else
            return 0;

      case gpir_op_store_reg:
      case gpir_op_store_varying:
         return 0;

      case gpir_op_mov:
      case gpir_op_mul:
      case gpir_op_add:
      case gpir_op_select:
      case gpir_op_complex1:
      case gpir_op_complex2:
      case gpir_op_floor:
      case gpir_op_sign:
      case gpir_op_ge:
      case gpir_op_lt:
      case gpir_op_min:
      case gpir_op_max:
      case gpir_op_neg:
      case gpir_op_clamp_const:
      case gpir_op_preexp2:
      case gpir_op_postlog2:
      case gpir_op_exp2_impl:
      case gpir_op_log2_impl:
      case gpir_op_rcp_impl:
      case gpir_op_rsqrt_impl:
      case gpir_op_branch_cond:
      case gpir_op_store_temp_load_off0:
      case gpir_op_store_temp_load_off1:
      case gpir_op_store_temp_load_off2:
         return gpir_min_dist_alu(dep);

      default:
         assert(0);
      }
   }
   else {
      if (dep->pred->op == gpir_op_store_temp && dep->succ->op == gpir_op_load_temp)
         return 4;
      else if (dep->pred->op == gpir_op_store_reg && dep->succ->op == gpir_op_load_reg)
         return 3;
      else if ((dep->pred->op == gpir_op_store_temp_load_off0 ||
                dep->pred->op == gpir_op_store_temp_load_off1 ||
                dep->pred->op == gpir_op_store_temp_load_off2) &&
               dep->succ->op == gpir_op_load_uniform)
         return 4;
      else
         return 1;
   }

   return 0;
}

static bool gpir_is_sched_complex(gpir_node *node)
{
   switch (node->op) {
   case gpir_op_exp2_impl:
   case gpir_op_log2_impl:
   case gpir_op_rcp_impl:
   case gpir_op_rsqrt_impl:
   case gpir_op_store_temp_load_off0:
   case gpir_op_store_temp_load_off1:
   case gpir_op_store_temp_load_off2:
      return true;
   case gpir_op_mov:
      if (node->sched_pos == GPIR_INSTR_SLOT_COMPLEX)
         return true;
      break;
   default:
      break;
   }

   return false;
}

static int gpir_max_dist_alu(gpir_dep_info *dep)
{
   switch (dep->pred->op) {
   case gpir_op_load_uniform:
   case gpir_op_load_temp:
      return 0;
   case gpir_op_load_attribute:
      return 1;
   case gpir_op_load_reg:
      if (dep->pred->sched_pos >= GPIR_INSTR_SLOT_REG0_LOAD0 &&
          dep->pred->sched_pos <= GPIR_INSTR_SLOT_REG0_LOAD3)
         return 1;
      else
         return 0;
   default:
      break;
   }

   if (dep->succ->op == gpir_op_complex1)
      return 1;

   if (gpir_is_sched_complex(dep->pred))
      return 1;

   return 2;
}

static int gpir_get_max_dist(gpir_dep_info *dep)
{
   /* Note: two instr's max are affected by pred's sched_pos:
    * lima_gp_ir_op_load_reg
    * lima_gp_ir_op_mov
    *
    * so these two nodes as pred must call this function with sched_pos set first
    */
   if (dep->is_child_dep) {
      switch (dep->succ->op) {
      case gpir_op_store_temp:
         if (dep->is_offset)
            return gpir_max_dist_alu(dep);
         else
            return 0;

      case gpir_op_store_reg:
      case gpir_op_store_varying:
         return 0;

      case gpir_op_mov:
      case gpir_op_mul:
      case gpir_op_add:
      case gpir_op_select:
      case gpir_op_complex1:
      case gpir_op_complex2:
      case gpir_op_floor:
      case gpir_op_sign:
      case gpir_op_ge:
      case gpir_op_lt:
      case gpir_op_min:
      case gpir_op_max:
      case gpir_op_neg:
      case gpir_op_clamp_const:
      case gpir_op_preexp2:
      case gpir_op_postlog2:
      case gpir_op_exp2_impl:
      case gpir_op_log2_impl:
      case gpir_op_rcp_impl:
      case gpir_op_rsqrt_impl:
      case gpir_op_branch_cond:
      case gpir_op_store_temp_load_off0:
      case gpir_op_store_temp_load_off1:
      case gpir_op_store_temp_load_off2:
         return gpir_max_dist_alu(dep);

      default:
         assert(0);
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
      for (int i = n; i <= pos; i++)
         gpir_instr_init(gpir_instr_array_e(instrs, i));
   }

   return gpir_instr_array_e(instrs, pos);
}

static int gpir_get_max_start(gpir_node *node)
{
   int max_start = 0;

   /* find the max start instr constrainted by all successors */
   gpir_node_foreach_succ(node, entry) {
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      int start = succ->sched_instr + gpir_get_min_dist(dep);

      if (start > max_start)
         max_start = start;
   }

   return max_start;
}

static bool gpir_try_place_node(gpir_block *block, gpir_node *node, int start, int end)
{
   int *slots = gpir_op_infos[node->op].slots;

   /* try to insert node begin from max_start */
   for (node->sched_instr = start; node->sched_instr < end; node->sched_instr++) {
      gpir_instr *instr = gpir_instr_array_grow(&block->instrs, node->sched_instr);

      for (int i = 0; slots[i] != GPIR_INSTR_SLOT_END; i++) {
         node->sched_pos = slots[i];
         if (gpir_instr_try_insert_node(instr, node))
            return true;
      }
   }

   return false;
}

static bool gpir_try_place_move_node(gpir_block *block, gpir_node *node, int start)
{
   struct set_entry *entry = _mesa_set_next_entry(node->preds, NULL);
   gpir_dep_info *dep = gpir_dep_from_entry(entry);
   gpir_node *pred = gpir_node_from_entry(entry, pred);
   int min = pred->sched_instr - gpir_get_max_dist(dep);
   int max = pred->sched_instr - gpir_get_min_dist(dep);

   if (start > max)
      return false;

   if (min < start)
      min = start;

   return gpir_try_place_node(block, node, min, max + 1);
}

static int gpir_get_new_start(gpir_node *node)
{
   int start = -1;

   gpir_node_foreach_succ(node, entry) {
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      int min = succ->sched_instr + gpir_get_min_dist(dep);
      int max = succ->sched_instr + gpir_get_max_dist(dep);

      if (max < node->sched_instr) {
         if (min > start)
            start = min;
      }
   }

   return start;
}

static gpir_node *_gpir_create_from_node(gpir_block *block, gpir_node *node, gpir_node *load)
{
   gpir_node *ret = gpir_node_create(block->comp, load ? load->op : gpir_op_mov, -1);
   if (!ret)
      return NULL;

   fprintf(stderr, "gpir: scheduler create node %s %d from node %s %d\n",
          gpir_op_infos[ret->op].name, ret->index,
          load ? gpir_op_infos[load->op].name : gpir_op_infos[node->op].name,
          load ? load->index : node->index);

   if (load) {
      /* copy load node */
      gpir_load_node *dst = gpir_node_to_load(ret);
      gpir_load_node *src = gpir_node_to_load(load);

      dst->index = src->index;
      dst->component = src->component;
      dst->offset = src->offset;
      dst->off_reg = src->off_reg;
      gpir_node_merge_pred(ret, load);
   }
   else {
      gpir_alu_node *alu = gpir_node_to_alu(ret);
      alu->children[0] = node;
      alu->num_child = 1;
      gpir_node_add_child(ret, node);
   }

   list_addtail(&ret->list, &block->node_list);
   return ret;
}

static bool gpir_create_from_node(gpir_block *block, gpir_node *node, gpir_node *load,
                                  gpir_node **output)
{
   gpir_node *ret = NULL;

   /* get remain unsatisfied nodes */
   gpir_node_foreach_succ(node, entry) {
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      int max = succ->sched_instr + gpir_get_max_dist(dep);

      if (max < node->sched_instr) {
         if (!ret && !(ret = _gpir_create_from_node(block, node, load)))
            return false;

         dep->pred = ret;
         _mesa_set_add_pre_hashed(ret->succs, entry->hash, dep);
         _mesa_set_remove(node->succs, entry);
         gpir_node_replace_child(succ, node, ret);
      }
   }

   *output = ret;
   return true;
}

static void gpir_remove_load_node(gpir_node *load, gpir_node *node)
{
   gpir_node_foreach_succ(load, entry) {
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node *succ = gpir_node_from_entry(entry, succ);

      dep->pred = node;
      _mesa_set_add_pre_hashed(node->succs, entry->hash, dep);
      _mesa_set_remove(load->succs, entry);
      gpir_node_replace_child(succ, load, node);
   }

   gpir_node_foreach_pred(load, entry)
      gpir_node_remove_entry(entry);

   gpir_node_delete(load);
}

static bool gpir_insert_move_for_store_load(gpir_block *block, gpir_node *node)
{
   struct list_head store_list;
   list_inithead(&store_list);

   /* find all success store node of load node */
   gpir_node_foreach_succ(node, entry) {
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      if (succ->type == gpir_node_type_store) {
         gpir_store_node *store = gpir_node_to_store(succ);
         if (store->child == node) {
            /* sort the store_list for the following move node insertion order */
            struct list_head *insert_pos = &store_list;
            list_for_each_entry(gpir_node, s, &store_list, ready) {
               if (s->sched_instr > succ->sched_instr) {
                  insert_pos = &s->ready;
                  break;
               }
            }
            list_addtail(&succ->ready, insert_pos);
            _mesa_set_remove(node->succs, entry);
         }
      }
   }

   /* create move node and replace store node child to it */
   while (!list_empty(&store_list)) {
      int instr = -1;
      gpir_node *move = _gpir_create_from_node(block, node, NULL);
      if (!move)
         return false;

      /* insert store node in the same instr to this move */
      list_for_each_entry_safe(gpir_node, store, &store_list, ready) {
         if (instr < 0)
            instr = store->sched_instr;

         if (store->sched_instr == instr) {
            gpir_store_node *s = gpir_node_to_store(store);
            s->child = move;

            gpir_node_foreach_pred(store, entry) {
               gpir_dep_info *dep = gpir_dep_from_entry(entry);
               gpir_node *pred = gpir_node_from_entry(entry, pred);

               if (pred == node) {
                  dep->pred = move;
                  _mesa_set_add_pre_hashed(move->succs, entry->hash, dep);
                  break;
               }
            }

            list_del(&store->ready);
         }
         else
            break;
      }

      /* insert alu node for store node must succeed */
      if (!gpir_try_place_node(block, move, instr, instr + 1))
         assert(0);

      /* check load node successors for using this move node
       * including previous inserted move node, so the check order should
       * be from small to big (ensured by the sorting of store node)
       */
      gpir_node_foreach_succ(node, entry) {
         gpir_dep_info *dep = gpir_dep_from_entry(entry);
         gpir_node *succ = gpir_node_from_entry(entry, succ);

         /* move is already successor of load node */
         if (succ != move) {
            gpir_dep_info tmp = {
               .pred = move,
               .succ = succ,
               .is_child_dep = true,
               .is_offset = false,
            };
            int min = succ->sched_instr + gpir_get_min_dist(&tmp);
            int max = succ->sched_instr + gpir_get_max_dist(&tmp);

            if (move->sched_instr >= min && move->sched_instr <= max) {
               dep->pred = move;
               _mesa_set_add_pre_hashed(move->succs, entry->hash, dep);
               _mesa_set_remove(node->succs, entry);
               gpir_node_replace_child(succ, node, move);
            }
         }
      }
   }

   return true;
}

static gpir_node *gpir_move_get_start_node(gpir_node *node)
{
   gpir_node *move = NULL;

   /* find existing move node to reuse */
   gpir_node_foreach_succ(node, entry) {
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      int max = succ->sched_instr + gpir_get_max_dist(dep);

      /* node may have multi move successors, but can only satisfy one at a time */
      if (max >= node->sched_instr && succ->op == gpir_op_mov) {
         /* move node may have only one move successor, find the deepest */
         move = succ;
         while (true) {
            gpir_node *new_move = NULL;

            gpir_node_foreach_succ(move, _entry) {
               gpir_node *_succ = gpir_node_from_entry(_entry, succ);
               if (_succ->op == gpir_op_mov) {
                  new_move = _succ;
                  break;
               }
            }

            if (new_move)
               move = new_move;
            else
               break;
         }
      }
   }

   if (!move)
      return node;

   /* move un-satisfied successors of node to move node */
   gpir_node_foreach_succ(node, entry) {
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      int max = succ->sched_instr + gpir_get_max_dist(dep);

      if (max < node->sched_instr) {
         assert(max < move->sched_instr);

         dep->pred = move;
         _mesa_set_add_pre_hashed(move->succs, entry->hash, dep);
         _mesa_set_remove(node->succs, entry);
         gpir_node_replace_child(succ, node, move);
      }
   }

   fprintf(stderr, "gpir: scheduler reuse move node %d for node %d\n",
           move->index, node->index);

   return move;
}

static bool gpir_try_schedule_node(gpir_block *block, gpir_node *node)
{
   int start = gpir_get_max_start(node), end = INT_MAX;

   if (node->type == gpir_node_type_load) {
      /* store node can only accept alu child, so insert a move node
       * between load node and store node. The reason do this here
       * instead of pre-schedule is only at this point can we know
       * if two store node can share a single move node
       */
      if (!gpir_insert_move_for_store_load(block, node))
         return false;

      gpir_node *load = node, *current = NULL;
      while (true) {
         /* first time this must be true because end is INT_MAX */
         if (gpir_try_place_node(block, load, start, end))
            current = load;
         else
            gpir_remove_load_node(load, current);

         while (true) {
            int new_start = gpir_get_new_start(current);

            /* all constraints are satisfied */
            if (new_start < 0)
               return true;

            /* part of constraints are satisfied and new instr position avaliable */
            if (new_start < start) {
               end = start;
               start = new_start;
               break;
            }

            gpir_node *move, *start_node = gpir_move_get_start_node(current);
            if (!gpir_create_from_node(block, start_node, NULL, &move))
               return false;

            if (!gpir_try_place_move_node(block, move, start))
               return false;

            current = move;
         }

         if (!gpir_create_from_node(block, current, node, &load))
            return false;
      }
   }
   else {
      gpir_try_place_node(block, node, start, end);

      /* TODO: we may use the distance of node and all its successors to
       * predicate if using a reg instead of so many moves
       */
      gpir_node *current = node;
      for (int i = 0; true; i++) {
         int start = gpir_get_new_start(current);

         /* all constraints are satisfied */
         if (start < 0) {
            if (i > 0)
               fprintf(stderr, "gpir: add %d moves for node %s %d\n",
                       i, gpir_op_infos[node->op].name, node->index);
            return true;
         }

         gpir_node *move;
         if (!gpir_create_from_node(block, current, NULL, &move))
            return false;

         if (!gpir_try_place_move_node(block, move, start))
            return false;

         current = move;
      }
   }

   return false;
}

static bool gpir_schedule_node(gpir_block *block, gpir_node *node)
{
   node->scheduled = true;

   const gpir_op_info *info = gpir_op_infos + node->op;
   int *slots = info->slots;
   /* not schedule node without instr slot */
   if (!slots)
      return true;

   if (gpir_try_schedule_node(block, node))
      return true;

   fprintf(stderr, "gpir: fail to schedule node %s %d\n",
           gpir_op_infos[node->op].name, node->index);
   return false;
}

static bool gpir_schedule_ready_list(gpir_block *block, struct list_head *ready_list)
{
   if (list_empty(ready_list))
      return true;

   gpir_node *node = list_first_entry(ready_list, gpir_node, ready);
   list_del(&node->ready);

   if (!gpir_schedule_node(block, node))
      return false;

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

   return gpir_schedule_ready_list(block, ready_list);
}

static bool gpir_schedule_block(gpir_block *block)
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

   return gpir_schedule_ready_list(block, &ready_list);
}

bool gpir_schedule_prog(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      if (!gpir_schedule_block(block))
         return false;
   }
   return true;
}
