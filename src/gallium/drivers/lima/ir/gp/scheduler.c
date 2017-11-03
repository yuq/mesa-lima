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

#include "util/bitscan.h"

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
    * gpir_op_load_reg
    * gpir_op_mov
    *
    * so these two nodes as pred must call this function with sched_pos set first
    */
   if (dep->pred->op == gpir_op_load_reg ||
       dep->pred->op == gpir_op_mov)
      assert(dep->pred->sched_instr >= 0);

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

   /* We must schedule complex2 & impl nodes right after the complex1,
    * otherwise the sapce reserved in gpir_try_place_node() may be used
    * by other node.
    *
    * complex2 & impl node will be called here right after the complex1
    * is scheduled because they're the children of complex1 and have no
    * other successor. And no other node will be called with this function
    * before schedule complex2 & impl, because complex1 only have there
    * children, the last one is also the child of the complex2 & impl, it
    * won't be schedulable until complex2 & impl have been scheduled.
    *
    * Unsure: also schedule load node early to avoid spill to reg */
   if (insert_node->op == gpir_op_complex2 ||
       insert_node->op == gpir_op_rcp_impl ||
       insert_node->type == gpir_node_type_load) {
      list_add(&insert_node->ready, ready_list);
      return;
   }

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

      gpir_instr *ia = gpir_instr_array(instrs);
      memset(ia + n, 0, size);
      for (int i = n; i <= pos; i++)
         gpir_instr_init(ia + i);
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

static int gpir_get_max_end(gpir_node *node)
{
   if (gpir_node_is_root(node))
      return INT_MAX;

   int max_end = 0;
   /* find the max end instr constrainted by all successors */
   gpir_node_foreach_succ(node, entry) {
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      int end = succ->sched_instr + gpir_get_max_dist(dep);

      if (end > max_end)
         max_end = end;
   }

   return max_end;
}

static bool gpir_try_place_node(gpir_block *block, gpir_node *node, int start, int end)
{
   int *slots = gpir_op_infos[node->op].slots;

   /* try to insert node begin from max_start */
   for (node->sched_instr = start; node->sched_instr < end; node->sched_instr++) {
      gpir_instr *instr = gpir_instr_array_grow(&block->instrs, node->sched_instr);

      /* complex1 need to make sure the instr before 'instr' has
       * slot for complex2 and impl */
      if (node->op == gpir_op_complex1) {
         gpir_instr *prev = instr + 1;
         if (prev < (gpir_instr *)util_dynarray_end(&block->instrs)) {
            if (prev->slots[GPIR_INSTR_SLOT_MUL0] ||
                prev->slots[GPIR_INSTR_SLOT_COMPLEX])
               continue;
         }
      }

      /* for node to choose a slot which can satisfy one successor first.
       * i.e. if insert the load reg node in the same instr as its successor,
       * we prefer load_reg1 than load_reg0 slot because load_reg0 is shared
       * with load attr; but if insert one instr before its successor,
       * we prefer load_reg0 than load_reg1 slot because load_reg1 need
       * another move node to satisfy the successor.
       */
      for (int i = 0; slots[i] != GPIR_INSTR_SLOT_END; i++) {
         node->sched_pos = slots[i];
         if (gpir_get_max_end(node) >= node->sched_instr &&
             gpir_instr_try_insert_node(instr, node))
            return true;
      }

      for (int i = 0; slots[i] != GPIR_INSTR_SLOT_END; i++) {
         node->sched_pos = slots[i];
         if (gpir_instr_try_insert_node(instr, node))
            return true;
      }
   }

   node->sched_instr = -1;
   node->sched_pos = -1;
   return false;
}

static bool gpir_try_place_move_node(gpir_block *block, gpir_node *node)
{
   int start = gpir_get_max_start(node);

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

static int gpir_get_unsatisfied_max_start(gpir_node *node, gpir_node *load)
{
   int start = -1;

   gpir_node_foreach_succ(node, entry) {
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      int max = succ->sched_instr + gpir_get_max_dist(dep);

      if (max < node->sched_instr) {
         gpir_dep_info tmp = {
            .pred = load,
            .succ = succ,
            .is_child_dep = dep->is_child_dep,
            .is_offset = dep->is_offset,
         };

         int min = succ->sched_instr + gpir_get_min_dist(&tmp);
         if (min > start)
            start = min;
      }
   }

   return start;
}

static int gpir_get_unsatisfied_max_end(gpir_node *node)
{
   int end = -1;

   gpir_node_foreach_succ(node, entry) {
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      int max = succ->sched_instr + gpir_get_max_dist(dep);

      if (max < node->sched_instr) {
         if (succ->sched_instr > end)
            end = succ->sched_instr;
      }
   }

   return end;
}

static gpir_node *_gpir_create_from_node(gpir_block *block, gpir_node *node, gpir_node *load)
{
   gpir_node *ret = gpir_node_create(block->comp, load ? load->op : gpir_op_mov, -1);
   if (!ret)
      return NULL;

   gpir_debug("scheduler create node %s %d from node %s %d\n",
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

static void gpir_move_unsatistied_node(gpir_node *dst, gpir_node *src)
{
   /* get remain unsatisfied nodes */
   gpir_node_foreach_succ(src, entry) {
      gpir_node *succ = gpir_node_from_entry(entry, succ);

      /* dst maybe already successor of src */
      if (succ == dst)
         continue;

      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      int max = succ->sched_instr + gpir_get_max_dist(dep);
      if (max < src->sched_instr)
         gpir_node_replace_pred(entry, dst);
   }
}

static gpir_node *gpir_create_from_node(gpir_block *block, gpir_node *node, gpir_node *load)
{
    gpir_node *ret = _gpir_create_from_node(block, node, load);
    if (!ret)
       return NULL;

    gpir_move_unsatistied_node(ret, node);
    return ret;
}

static inline void instr_remove_node(gpir_block *block, gpir_node *node)
{
   gpir_instr *instr = gpir_instr_array_e(&block->instrs, node->sched_instr);
   gpir_instr_remove_node(instr, node);
   node->sched_instr = -1;
   node->sched_pos = -1;
}

static void gpir_remove_created_node(gpir_block *block, gpir_node *created,
                                     gpir_node *node)
{
   gpir_node_foreach_succ(created, entry) {
      gpir_node_replace_pred(entry, node);
   }

   if (created->sched_instr >= 0)
      instr_remove_node(block, created);

   gpir_debug("remove created node %d back to %d\n",
              created->index, node->index);

   gpir_node_delete(created);
}

static void gpir_remove_all_created_node(gpir_block *block, gpir_node *node)
{
   bool done = true;

   while (done) {
      done = false;

      gpir_node_foreach_succ(node, entry) {
         gpir_node *succ = gpir_node_from_entry(entry, succ);

         if (succ->index >= block->comp->save_index) {
            gpir_remove_created_node(block, succ, node);
            done = true;
            break;
         }
      }
   }
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
         gpir_node *succ = gpir_node_from_entry(entry, succ);

         /* move is already successor of load node */
         if (succ == move)
            continue;

         gpir_dep_info tmp = {
            .pred = move,
            .succ = succ,
            .is_child_dep = true,
            .is_offset = false,
         };
         int min = succ->sched_instr + gpir_get_min_dist(&tmp);
         int max = succ->sched_instr + gpir_get_max_dist(&tmp);

         if (move->sched_instr >= min && move->sched_instr <= max)
            gpir_node_replace_pred(entry, move);
      }
   }

   return true;
}

static gpir_node *get_spill_node(gpir_node *node)
{
   gpir_node *ret = NULL;

   gpir_node_foreach_succ(node, entry) {
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      int max = succ->sched_instr + gpir_get_max_dist(dep);

      if (max < node->sched_instr) {
         if (!ret || succ->sched_instr > ret->sched_instr)
            ret = succ;
      }
   }

   return ret;
}

static int gpir_get_max_start_for_load(gpir_node *node, gpir_node *load)
{
   int max_start = 0;

   /* find the max start instr constrainted by all successors */
   gpir_node_foreach_succ(node, entry) {
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      gpir_dep_info *dep = gpir_dep_from_entry(entry);

      gpir_dep_info tmp = {
         .pred = load,
         .succ = succ,
         .is_child_dep = dep->is_child_dep,
         .is_offset = dep->is_offset,
      };
      int start = succ->sched_instr + gpir_get_min_dist(&tmp);

      if (start > max_start)
         max_start = start;
   }

   return max_start;
}

static int gpir_has_satisfied_succ(gpir_node *node)
{
   gpir_node_foreach_succ(node, entry) {
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      int max = succ->sched_instr + gpir_get_max_dist(dep);

      if (max >= node->sched_instr)
         return true;
   }

   return false;
}

static int gpir_try_insert_load_reg(gpir_block *block, gpir_node *node);

static int insert_load_reg(gpir_block *block, gpir_node *node, int end,
                           gpir_node *load, gpir_node *move)
{
   /* TODO: we may use already scheduled load node */
   if (!load) {
      load = _gpir_create_from_node(block, NULL, node);
      if (!load)
         return -2;
   }

   gpir_alu_node *alu = gpir_node_to_alu(move);
   alu->children[0] = load;
   alu->num_child = 1;
   gpir_node_add_child(move, load);

   int start = gpir_get_max_start_for_load(move, load);

   while (true) {
      MAYBE_UNUSED bool place_result =
         gpir_try_place_node(block, load, start, end);
      assert(place_result);

      if (!gpir_try_place_move_node(block, move)) {
         start = load->sched_instr + 1;
         instr_remove_node(block, load);
         continue;
      }

      int err = gpir_try_insert_load_reg(block, move);
      if (err == 0)
         return 0;
      else if (err == -2) {
         /* TODO: handle target fail case */
         return -1;
      }
      else if (err < -2)
         return -2;

      /* TODO: for load attr, reg_move may have another instr for try */
      instr_remove_node(block, move);
      start = load->sched_instr + 1;
      instr_remove_node(block, load);
   }

   return -2;
}

static void gpir_print_unsatisfied_succ(gpir_node *node)
{
   if (!lima_shader_debug_gp)
      return;

   printf("unsatisfied successors");
   gpir_node_foreach_succ(node, entry) {
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      int max = succ->sched_instr + gpir_get_max_dist(dep);

      if (max < node->sched_instr)
         printf(" %d", succ->index);
   }
   printf("\n");
}

/* insert an empty instruction and try again */
static bool gpir_try_relax_insert_move(gpir_block *block, gpir_node *node,
                                       gpir_node *move)
{
   struct set_entry *entry = _mesa_set_next_entry(move->preds, NULL);
   gpir_dep_info *dep = gpir_dep_from_entry(entry);
   const int pos = node->sched_instr - gpir_get_max_dist(dep);

   gpir_debug("relax insert move %d for node %d at new instr %d\n",
              move->index, node->index, pos);

   gpir_instr *instrs = gpir_instr_array(&block->instrs);
   int n = gpir_instr_array_n(&block->instrs);
   int dist_one_nodes = 0, dist_two_nodes = 0;
   int dist_one_slots = 1, dist_two_slots = 5;

   /* keep complex1 node in new instr if pos instr has it */
   gpir_node *nd = instrs[pos].slots[GPIR_INSTR_SLOT_MUL0];
   if (nd && nd->op == gpir_op_complex1) {
      dist_two_slots -= 2;
      /* for insert move node, can use the empty slot in pos
       * instr made by moving complex1 nodes out, so just need
       * a dist one slot in pos+1 instr */
      dist_one_slots -= 1;
   }
   else {
      /* for insert move node, must use dist two slot to make
       * any progress (exceed pos instr) */
      dist_two_slots -= 1;
   }

   /* two instr after pos, the two dist slot output may be used
    * by pos instr slots, but only need new instr's one dist slot
    */
   if (pos + 2 < n) {
      gpir_instr *instr = instrs + pos + 2;

      for (int i = GPIR_INSTR_SLOT_DIST_TWO_BEGIN;
           i <= GPIR_INSTR_SLOT_DIST_TWO_END; i++) {
         if (!(nd = instr->slots[i]))
            continue;

         if (nd->op == gpir_op_complex1 && i == GPIR_INSTR_SLOT_MUL1)
            continue;

         /* if nd is used by any pos instr slot node */
         gpir_node_foreach_succ(nd, entry) {
            gpir_node *succ = gpir_node_from_entry(entry, succ);
            if (succ->sched_instr == pos) {
               dist_one_nodes++;
               break;
            }
         }
      }
   }

   /* one instr after pos, the two dist slot output may be used
    * by pos-1 instr slots, and need new instr's two dist slot
    */
   gpir_instr *instr = instrs + pos + 1;
   for (int i = GPIR_INSTR_SLOT_DIST_TWO_BEGIN;
        i <= GPIR_INSTR_SLOT_DIST_TWO_END; i++) {
      if (!(nd = instr->slots[i]))
         continue;

      if (nd->op == gpir_op_complex1 && i == GPIR_INSTR_SLOT_MUL1)
         continue;

      /* if nd is used by any pos-1 instr slot node */
      gpir_node_foreach_succ(nd, entry) {
         gpir_node *succ = gpir_node_from_entry(entry, succ);
         if (succ->sched_instr == pos - 1) {
            dist_two_nodes++;
            break;
         }
      }
   }

   /* the one dist slot output may be used by pos instr slots,
    * need new instr's one dist slot */
   if ((nd = instr->slots[GPIR_INSTR_SLOT_COMPLEX])) {
      /* if nd is used by any pos instr slot node */
      gpir_node_foreach_succ(nd, entry) {
         gpir_node *succ = gpir_node_from_entry(entry, succ);
         /* complex impl node */
         if (succ->op == gpir_op_complex1)
            break;

         if (succ->sched_instr == pos) {
            dist_one_nodes++;
            break;
         }
      }
   }

   gpir_debug("relax insert move %d check d2n/d2s=%d/%d d1n/d1s=%d/%d\n",
              move->index, dist_two_nodes, dist_two_slots,
              dist_one_nodes, dist_one_slots);

   /* check if we can do instr insert */
   if (dist_two_nodes > dist_two_slots ||
       dist_one_nodes + dist_two_nodes > dist_one_slots + dist_two_slots) {
      gpir_debug("fail relax insert move %d\n", move->index);
      return false;
   }

   /* really do new instr insert */

   /* create a new instr at last and move all instrs after pos down */
   instr = gpir_instr_array_grow(&block->instrs, n);
   instrs = gpir_instr_array(&block->instrs);
   for ( ; instr - 1 != instrs + pos; instr--) {
      *instr = *(instr - 1);
      for (int i = 0; i < GPIR_INSTR_SLOT_NUM; i++) {
         if ((nd = instr->slots[i]) &&
             !(nd->op == gpir_op_complex1 && i == GPIR_INSTR_SLOT_MUL1))
            nd->sched_instr++;
      }
   }

   /* reset new inserted instr */
   gpir_instr *new_instr = instr;
   memset(new_instr, 0, sizeof(*new_instr));
   gpir_instr_init(new_instr);
   new_instr->reg_status = instrs[pos].reg_status;

   /* keep complex1 node */
   MAYBE_UNUSED bool insert_result;
   nd = instrs[pos].slots[GPIR_INSTR_SLOT_MUL0];
   if (nd && nd->op == gpir_op_complex1) {
      gpir_instr_remove_node(instrs + pos, nd);

      nd->sched_instr = pos + 1;
      insert_result = gpir_instr_try_insert_node(new_instr, nd);
      assert(insert_result);

      gpir_node_foreach_succ(nd, entry) {
         gpir_node *succ = gpir_node_from_entry(entry, succ);

         /* need insert a move node at pos */
         if (succ->sched_instr == pos - 1) {
            gpir_node *move = gpir_create_from_node(block, nd, NULL);
            move->sched_instr = pos;
            move->sched_pos = GPIR_INSTR_SLOT_MUL1;
            insert_result = gpir_instr_try_insert_node(instrs + pos, move);
            assert(insert_result);
            break;
         }
      }
   }

   /* first insert moves for nodes need dist two slot */
   instr = new_instr + 1;
   for (int i = GPIR_INSTR_SLOT_DIST_TWO_BEGIN;
        i <= GPIR_INSTR_SLOT_DIST_TWO_END; i++) {
      if (!(nd = instr->slots[i]))
         continue;

      if (nd->op == gpir_op_complex1 && i == GPIR_INSTR_SLOT_MUL1)
         continue;

      /* if nd is used by any pos-1 instr slot node */
      gpir_node_foreach_succ(nd, entry) {
         gpir_node *succ = gpir_node_from_entry(entry, succ);
         if (succ->sched_instr == pos - 1) {
            gpir_node *move = gpir_create_from_node(block, nd, NULL);
            insert_result = gpir_try_place_node(block, move, pos + 1, pos + 2);
            assert(insert_result);
            break;
         }
      }
   }

   /* insert the move node here for better dist two slot if avaliable */
   insert_result = gpir_try_place_node(block, move, pos + 1, pos + 2);
   assert(insert_result);

   /* insert moves for other nodes need dist one slot */

   n = gpir_instr_array_n(&block->instrs);
   if (pos + 3 < n) {
      gpir_instr *instr = instrs + pos + 3;

      for (int i = GPIR_INSTR_SLOT_DIST_TWO_BEGIN;
           i <= GPIR_INSTR_SLOT_DIST_TWO_END; i++) {
         if (!(nd = instr->slots[i]))
            continue;

         if (nd->op == gpir_op_complex1 && i == GPIR_INSTR_SLOT_MUL1)
            continue;

         /* if nd is used by any pos instr slot node */
         gpir_node_foreach_succ(nd, entry) {
            gpir_node *succ = gpir_node_from_entry(entry, succ);
            if (succ->sched_instr == pos) {
               gpir_node *move = gpir_create_from_node(block, nd, NULL);
               insert_result = gpir_try_place_node(block, move, pos + 1, pos + 2);
               assert(insert_result);
               break;
            }
         }
      }
   }

   if ((nd = instr->slots[GPIR_INSTR_SLOT_COMPLEX])) {
      /* if nd is used by any pos instr slot node */
      gpir_node_foreach_succ(nd, entry) {
         gpir_node *succ = gpir_node_from_entry(entry, succ);
         /* complex impl node */
         if (succ->op == gpir_op_complex1)
            break;

         if (succ->sched_instr == pos) {
            gpir_node *move = gpir_create_from_node(block, nd, NULL);
            insert_result = gpir_try_place_node(block, move, pos + 1, pos + 2);
            assert(insert_result);
            break;
         }
      }
   }

   for (int i = GPIR_INSTR_SLOT_REG0_LOAD0;
        i <= GPIR_INSTR_SLOT_REG0_LOAD3; i++) {
      if (!(nd = instr->slots[i]))
            continue;

      /* check if load node can be just moved to next instr */
      bool move_load = true;
      gpir_node_foreach_succ(nd, entry) {
         gpir_node *succ = gpir_node_from_entry(entry, succ);
         if (succ->sched_instr != pos) {
            move_load = false;
            break;
         }
      }

      bool reset_reg = false;
      if (move_load) {
         gpir_instr_remove_node(instr, nd);
         insert_result = gpir_try_place_node(block, nd, pos + 1, pos + 2);
         assert(insert_result);
         reset_reg = true;
      }
      else {
         gpir_node *move = gpir_create_from_node(block, nd, NULL);
         insert_result = gpir_try_place_node(block, move, pos + 1, pos + 2);
         if (!insert_result) {
            gpir_remove_created_node(block, move, nd);
            gpir_node *load = gpir_create_from_node(block, nd, nd);
            insert_result = gpir_try_place_node(block, load, pos + 1, pos + 2);
            assert(insert_result);
            reset_reg = true;
         }
      }

      if (reset_reg && nd->op == gpir_op_load_reg) {
         gpir_load_node *ln = gpir_node_to_load(nd);
         int reg = (ln->index << 2) + ln->component;
         instr->reg_status &= ~(1ull << reg);
      }
   }

   gpir_debug("success relax insert move %d\n", move->index);
   return true;
}

/*
 * Return:
 * >=0 - success, the last inserted load instr index
 *  -1 - recoverable fail
 *  -2 - unrecoverable fail
 */
static int gpir_try_insert_load(gpir_block *block, gpir_node *node, int orig_end)
{
   gpir_node *reg_load = NULL, *reg_move = NULL;

   /* save reg0 load slot for reg usage */
   if (node->op == gpir_op_load_attribute && node->succs->entries > 3) {
      reg_load = node;

      reg_move = gpir_node_create(block->comp, gpir_op_mov, -1);
      if (!reg_move)
         return -2;
      list_addtail(&reg_move->list, &block->node_list);

      gpir_debug("create move %d for load %d to store reg\n",
                 reg_move->index, node->index);

      gpir_node_foreach_succ(node, entry) {
         gpir_node_replace_pred(entry, reg_move);
      }

      gpir_debug("schedule attr load %d with reg load\n", node->index);

      return insert_load_reg(block, node, orig_end, reg_load, reg_move);
   }

   int start = gpir_get_max_start(node), end = orig_end;
   gpir_node *load = node, *current = NULL;
   gpir_node *last_load = NULL, *last_current = NULL;

   while (true) {
      if (gpir_try_place_node(block, load, start, end)) {
         last_current = current;
         last_load = load;
         current = load;
      }
      else {
         if (!current) {
            gpir_print_unsatisfied_succ(load);
            return -1;
         }

         gpir_remove_created_node(block, load, current);
         load = last_load;
      }

      while (true) {
         int new_start = gpir_get_unsatisfied_max_start(current, node);

         /* all constraints are satisfied */
         if (new_start < 0) {
            if (reg_move)
               return insert_load_reg(block, node, orig_end, reg_load, reg_move);
            else
               return load->sched_instr;
         }

         /* part of constraints are satisfied and new instr position avaliable */
         if (new_start < start) {
            end = start;
            start = new_start;
            break;
         }

         int max_end = gpir_get_unsatisfied_max_end(current);
         int max_dist = current->type == gpir_node_type_load ? 3 : 5;
         if (node->op == gpir_op_load_reg ||
             current->sched_instr - max_end < max_dist) {
            gpir_node *move = gpir_create_from_node(block, current, NULL);
            if (!move)
               return -2;

            if (gpir_try_place_move_node(block, move) ||
                gpir_try_relax_insert_move(block, current, move)) {
               current = move;
               continue;
            }

            gpir_remove_created_node(block, move, current);
         }

         /* load reg can't use reg again */
         if (node->op == gpir_op_load_reg) {
            gpir_print_unsatisfied_succ(current);
            return -1;
         }

         /* revert unused created nodes */
         while (true) {
            if (gpir_has_satisfied_succ(current) || current == node)
               break;

            if (current->type == gpir_node_type_load) {
               /* current != node && current->type == load, so must
                * "another load round" */
               assert(last_current);
               gpir_remove_created_node(block, current, last_current);
               current = last_current;
               break;
            }
            else {
               /* must be move node */
               gpir_node *child = gpir_node_to_alu(current)->children[0];
               gpir_remove_created_node(block, current, child);
               current = child;
            }
         }

         /* Spill unsatisfied successor and continue to use original
          * load to satisfy the remaining successors. Use reg load to
          * satisfy the spilled node. */

         /* create reg_move to hold all spilled nodes */
         if (!reg_move) {
            reg_move = gpir_node_create(block->comp, gpir_op_mov, -1);
            if (!reg_move)
               return -2;
            list_addtail(&reg_move->list, &block->node_list);

            gpir_debug("create move %d for load %d to store reg\n",
                       reg_move->index, node->index);
         }

         /* spill */
         gpir_node *spill = get_spill_node(current);
         assert(spill);
         gpir_node_foreach_succ(current, entry) {
            gpir_node *succ = gpir_node_from_entry(entry, succ);
            if (succ == spill) {
               gpir_node_replace_pred(entry, reg_move);
               break;
            }
         }

         gpir_debug("spill node %d for load %d\n", spill->index, node->index);

         /* handle current == node case */
         if (current == node) {
            /* all successors spilled */
            if (gpir_node_is_root(node)) {
               instr_remove_node(block, node);
               reg_load = node;
               return insert_load_reg(block, node, orig_end, reg_load, reg_move);
            }

            /* node need re-place (original placement satisfy no successor) */
            if (!gpir_has_satisfied_succ(node)) {
               instr_remove_node(block, node);

               start = gpir_get_max_start(node);
               end = orig_end;

               MAYBE_UNUSED bool place_result =
                  gpir_try_place_node(block, node, start, end);
               /* for none-reg load, orig_end is INT_MAX, so must success */
               assert(place_result);
            }
         }
      }

      load = gpir_create_from_node(block, current, node);
      if (!load)
         return -2;
   }

   return -2;
}

static uint64_t gpir_get_instr_permitted_regs(gpir_instr *instr)
{
   uint64_t ret = ~0ull;

   for (int i = 0; i < 2; i++) {
      if (instr->store_content[i] != GPIR_INSTR_STORE_NONE)
         ret &= 0xCCCCCCCCCCCCCCCCull >> (i * 2);

      if (instr->store_content[i] == GPIR_INSTR_STORE_REG) {
         for (int j = 0; j < 2; j++) {
            int component = (i << 1) + j;
            if (!instr->slots[GPIR_INSTR_SLOT_STORE0 + component])
               ret |= 1ull << ((instr->store_index[i] << 2) + component);
         }
      }
   }

   return ret;
}

static uint64_t gpir_get_free_regs(gpir_instr *instrs, int start, int end)
{
   /* current instr may have store node already which will constraint
    * free regs that can be chosen */
   uint64_t permitted_regs =
      gpir_get_instr_permitted_regs(instrs + end);

   if (!permitted_regs)
      return 0;

   uint64_t ret = ~0ull;
   /* store will have 3 instr delay, so end-2 */
   for (int i = start; i < end - 2; i++)
      ret &= instrs[i].reg_status;

   /* TODO: support spill to memory */
   assert(ret);

   return ret & permitted_regs;
}

static int gpir_alloc_reg(gpir_instr *instrs, gpir_node *load,
                          gpir_node **store_alu)
{
   /* find nearest & farest succ */
   int start = INT_MAX, end = 0;
   gpir_node_foreach_succ(load, entry) {
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      if (succ->sched_instr < start)
         start = succ->sched_instr;
      if (succ->sched_instr > end)
         end = succ->sched_instr;
   }

   gpir_node *current = *store_alu;
   /* current need to be far enough from nearest succ for a reg
    * store lantency */
   while (current->sched_instr < end + 3) {
      if (current->op == gpir_op_mov) {
         current = gpir_node_to_alu(current)->children[0];
         /* assert current has been inserted to instr before */
         assert(current->sched_instr >= 0);
      }
      else
         return -1;
   }

   uint64_t free_regs;
   while (!(free_regs = gpir_get_free_regs(
               instrs, start, current->sched_instr))) {
      if (current->op == gpir_op_mov) {
         current = gpir_node_to_alu(current)->children[0];
         /* assert current has been inserted to instr before */
         assert(current->sched_instr >= 0);
      }
      else {
         /* TODO: we may create and insert some move node here
          * based on all the nodes we've tried above and use it
          * as store alu */
         return -1;
      }
   }

   int reg = ffsll(free_regs) - 1;

   /* find a free reg, prefer in the same slot as already
    * used load of the succ instr */
   gpir_node_foreach_succ(load, entry) {
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      gpir_instr *instr = instrs + succ->sched_instr;

      if (instr->reg1_use_count) {
         uint64_t reg1 = (0xfull << (instr->reg1_index * 4)) & free_regs;
         if (reg1) {
            reg = ffsll(reg1) - 1;
            break;
         }
      }
      else
         continue;

      if (instr->reg0_use_count && !instr->reg0_is_attr) {
         uint64_t reg0 = (0xfull << (instr->reg0_index * 4)) & free_regs;
         if (reg0) {
            reg = ffsll(reg0) - 1;
            break;
         }
      }
      else
         continue;

      instr++;
      if (instr->reg0_use_count && !instr->reg0_is_attr) {
         uint64_t reg0 = (0xfull << (instr->reg0_index * 4)) & free_regs;
         if (reg0) {
            reg = ffsll(reg0) - 1;
            break;
         }
      }
   }

   /* record alu node used for creating a store node */
   *store_alu = current;

   gpir_debug("alloc reg %d for node %d\n", reg, load->index);
   return reg;
}

/*
 * Return:
 *   0 - success
 *  -1 - fail due to source node
 *  -2 - fail due to target node
 *  -3 - unrecoverable fail
 */
static int gpir_try_insert_load_reg(gpir_block *block, gpir_node *node)
{
   gpir_node *load = gpir_node_create(block->comp, gpir_op_load_reg, -1);
   if (!block)
      return -3;
   list_addtail(&load->list, &block->node_list);

   gpir_debug("create load reg %d for node %d\n", load->index, node->index);

   gpir_move_unsatistied_node(load, node);

   gpir_instr *instrs = gpir_instr_array(&block->instrs);

   gpir_node *store_alu = node;
   int reg = gpir_alloc_reg(instrs, load, &store_alu);
   if (reg < 0) {
      gpir_debug("alloc reg for node %d fail\n", load->index);
      gpir_remove_created_node(block, load, node);
      return -1;
   }

   gpir_load_node *ln = gpir_node_to_load(load);
   ln->index = reg >> 2;
   ln->component = reg & 0x3;

   /* create store reg node */
   gpir_node *store = gpir_node_create(block->comp, gpir_op_store_reg, -1);
   if (!store)
      return -3;
   list_addtail(&store->list, &block->node_list);

   gpir_debug("create store reg %d for %d\n", store->index, node->index);

   gpir_node_add_child(store, store_alu);
   gpir_dep_info *dep = gpir_node_add_read_after_write_dep(load, store);

   gpir_store_node *sn = gpir_node_to_store(store);
   sn->index = reg >> 2;
   sn->component = reg & 0x3;
   sn->child = store_alu;

   store->sched_instr = store_alu->sched_instr;
   store->sched_pos = GPIR_INSTR_SLOT_STORE0 + sn->component;
   bool MAYBE_UNUSED store_result =
      gpir_instr_try_insert_node(instrs + store_alu->sched_instr, store);
   /* ensured by the reg alloc process */
   assert(store_result);

   if (!gpir_insert_move_for_store_load(block, load))
      return -3;

   /* insert the load node */
   int load_end = store->sched_instr - gpir_get_min_dist(dep) + 1;
   int load_start = gpir_try_insert_load(block, load, load_end);
   if (load_start < 0) {
      gpir_error("insert load reg fail %d for node %d\n",
                 load_start, node->index);
      return load_start == -1 ? -2 : -3;
   }

   /* update reg status of instr between load/store */
   for (int i = load_start; i < load_end; i++)
      instrs[i].reg_status &= ~(1ull << reg);

   return 0;
}

static bool gpir_try_schedule_node(gpir_block *block, gpir_node *node)
{
   if (node->type == gpir_node_type_load) {
      /* store node can only accept alu child, so insert a move node
       * between load node and store node. The reason do this here
       * instead of pre-schedule is only at this point can we know
       * if two store node can share a single move node
       */
      if (!gpir_insert_move_for_store_load(block, node))
         return false;

      if (gpir_try_insert_load(block, node, INT_MAX) < 0)
         return false;
   }
   else {
      int start = gpir_get_max_start(node);
      gpir_try_place_node(block, node, start, INT_MAX);

      gpir_node *current = node;
      for (int i = 0; true; i++) {
         int end = gpir_get_unsatisfied_max_end(current);

         /* all constraints are satisfied */
         if (end < 0) {
            if (i > 0)
               gpir_debug("add %d moves for node %s %d\n",
                          i, gpir_op_infos[node->op].name, node->index);
            return true;
         }

         /* if next nearest succ is close enough, use move node to
          * satisfy, otherwise use reg directly */
         if (current->sched_instr - end < 5) {
            gpir_node *move = gpir_create_from_node(block, current, NULL);
            if (!move)
               return false;

            if (gpir_try_place_move_node(block, move) ||
                gpir_try_relax_insert_move(block, current, move)) {
               current = move;
               continue;
            }

            gpir_remove_created_node(block, move, current);
         }

         /* revert unused created move */
         while (current != node) {
            if (gpir_has_satisfied_succ(current))
               break;

            gpir_node *child = gpir_node_to_alu(current)->children[0];
            gpir_remove_created_node(block, current, child);
            current = child;
         }

         /* load reg function will handle min dist and reuse move case */
         int err = gpir_try_insert_load_reg(block, current);
         if (err == 0) {
            gpir_debug("add reg load and %d moves for node %s %d\n",
                       i, gpir_op_infos[node->op].name, node->index);
            return true;
         }
         else if (err == -2) {
            /* TODO: handle the target fail case to reschedule the target
             * node and all its preceeds */
            return false;
         }
         else if (err < -2)
            return false;

         /* fail to reg schedule current, we need to reschedule node */
         gpir_debug("reschedule node %d\n", node->index);

         /* remove all created move node and merge all successor back to node */
         gpir_remove_all_created_node(block, node);

         /* re-insert node one instr after current scheduled place */
         int reschedule_start = node->sched_instr + 1;
         instr_remove_node(block, node);
         gpir_try_place_node(block, node, reschedule_start, INT_MAX);

         current = node;
         i = 0;
      }
   }

   return true;
}

static void gpir_print_pre_schedule_node(gpir_node *node)
{
   printf("gpir: pre schedule node %d successors", node->index);
   gpir_node_foreach_succ(node, entry) {
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      printf(" %d", succ->index);
   }
   printf("\n");
}

static bool gpir_schedule_node(gpir_block *block, gpir_node *node)
{
   node->scheduled = true;

   const gpir_op_info *info = gpir_op_infos + node->op;
   int *slots = info->slots;
   /* not schedule node without instr slot */
   if (!slots)
      return true;

   if (lima_shader_debug_gp)
      gpir_print_pre_schedule_node(node);

   if (gpir_try_schedule_node(block, node))
      return true;

   gpir_error("fail to schedule node %s %d\n",
              gpir_op_infos[node->op].name, node->index);

   gpir_instr_print_prog(block->comp);
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

static void print_statistic(gpir_compiler *comp)
{
   int num_nodes[gpir_op_num] = {0};
   int num_created_nodes[gpir_op_num] = {0};

   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry(gpir_node, node, &block->node_list, list) {
         num_nodes[node->op]++;
         if (node->index >= comp->save_index)
            num_created_nodes[node->op]++;
      }
   }

   printf("====== gpir scheduler statistic ======\n");
   printf("---- how many nodes are scheduled ----\n");
   int n = 0, l = 0;
   for (int i = 0; i < gpir_op_num; i++) {
      if (num_nodes[i]) {
         printf("%10s:%-6d", gpir_op_infos[i].name, num_nodes[i]);
         n += num_nodes[i];
         if (!(++l % 4))
            printf("\n");
      }
   }
   if (l % 4)
      printf("\n");
   printf("\ntotal: %d\n", n);

   printf("---- how many nodes are created ----\n");
   n = l = 0;
   for (int i = 0; i < gpir_op_num; i++) {
      if (num_created_nodes[i]) {
         printf("%10s:%-6d", gpir_op_infos[i].name, num_created_nodes[i]);
         n += num_created_nodes[i];
         if (!(++l % 4))
            printf("\n");
      }
   }
   if (l % 4)
      printf("\n");
   printf("\ntotal: %d\n", n);
   printf("------------------------------------\n");
}

bool gpir_schedule_prog(gpir_compiler *comp)
{
   comp->save_index = comp->cur_index;

   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      if (!gpir_schedule_block(block))
         return false;
   }

   if (lima_shader_debug_gp) {
      print_statistic(comp);
      gpir_instr_print_prog(comp);
   }

   return true;
}
