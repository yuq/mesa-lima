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

#include "ppir.h"

static bool create_new_instr(ppir_block *block, ppir_node *node)
{
   ppir_instr *instr = ppir_instr_create(block);
   if (!instr)
      return false;

   if (!ppir_instr_insert_node(instr, node))
      return false;

   node->instr = instr;
   return true;
}

static bool ppir_schedule_create_instr_from_node(ppir_compiler *comp)
{
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_node, node, &block->node_list, list) {
         switch (node->type) {
         case ppir_node_type_load:
            if (node->op != ppir_op_load_varying)
               break;
         case ppir_node_type_alu:
            if (!create_new_instr(block, node))
               return false;
            break;
         case ppir_node_type_store:
         {
            /* Only the store color node should appear here.
             * Currently we always insert a move node as the end instr.
             * But it should only be done when:
             *   1. store a const node
             *   2. store a load node
             *   3. store a reg assigned in another block like loop/if
             */
            ppir_node *move = ppir_node_create(comp, ppir_op_mov, -1, 0);
            if (!move)
               return false;

            ppir_node_foreach_pred(node, entry) {
               ppir_node *pred = ppir_node_from_entry(entry, pred);
               ppir_node_remove_entry(entry);
               ppir_node_add_child(move, pred);
            }

            ppir_node_add_child(node, move);
            list_addtail(&move->list, &node->list);

            ppir_alu_node *alu = ppir_node_to_alu(move);
            ppir_store_node *store = ppir_node_to_store(node);
            alu->src[0] = store->src;
            alu->num_src = 1;

            alu->dest.type = ppir_target_ssa;
            alu->dest.ssa.node = move;
            alu->dest.ssa.num_components = 4;
            alu->dest.write_mask = 0xf;

            store->src.type = ppir_target_ssa;
            store->src.ssa = &alu->dest.ssa;

            if (!create_new_instr(block, move))
               return false;

            move->instr->is_end = true;
            break;
         }
         default:
            break;
         }
      }
   }

   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_node, node, &block->node_list, list) {
         switch (node->type) {
         case ppir_node_type_const:
            ppir_node_foreach_succ(node, entry) {
               ppir_node *succ = ppir_node_from_entry(entry, succ);
               if (!ppir_instr_insert_node(succ->instr, node))
                  return false;
            }
            break;
         case ppir_node_type_load:
         {
            if (node->op == ppir_op_load_varying)
               break;

            struct list_head move_list;
            list_inithead(&move_list);

            ppir_node_foreach_succ(node, entry) {
               ppir_node *succ = ppir_node_from_entry(entry, succ);
               assert(succ->type == ppir_node_type_alu);

               if (!ppir_instr_insert_node(succ->instr, node)) {
                  /* each instr can only have one load node with the same type
                   * create a move node to insert instead when fail, we can choose:
                   *   1. one move for all failed node (less move but more reg pressure)
                   *   2. one move for one failed node
                   */
                  ppir_node *move = ppir_node_create(comp, ppir_op_mov, -1, 0);
                  if (!move)
                     return false;

                  ppir_load_node *load = ppir_node_to_load(node);
                  assert(load->dest.type == ppir_target_ssa);

                  ppir_alu_node *alu = ppir_node_to_alu(move);
                  alu->dest = load->dest;
                  alu->num_src = 1;
                  ppir_node_target_assign(alu->src, &load->dest);
                  for (int i = 0; i < 4; i++)
                     alu->src->swizzle[i] = i;

                  ppir_dep_info *dep = ppir_dep_from_entry(entry);
                  dep->pred = move;
                  _mesa_set_add_pre_hashed(move->succs, entry->hash, dep);
                  _mesa_set_remove(node->succs, entry);
                  ppir_node_replace_child(succ, node, move);

                  if (!create_new_instr(block, move) ||
                      !ppir_instr_insert_node(move->instr, node))
                     return false;

                  /* can't add move to node succs here in a set loop */
                  list_addtail(&move->list, &move_list);
               }
            }

            list_for_each_entry(ppir_node, move, &move_list, list) {
               ppir_node_add_child(move, node);
            }
            list_splicetail(&move_list, &node->list);
            break;
         }
         default:
            break;
         }
      }
   }

   return true;
}

static void ppir_schedule_build_instr_dependency(ppir_compiler *comp)
{
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_instr, instr, &block->instr_list, list) {
         for (int i = 0; i < PPIR_INSTR_SLOT_NUM; i++) {
            ppir_node *node = instr->slots[i];
            if (node) {
               ppir_node_foreach_pred(node, entry) {
                  ppir_node *pred = ppir_node_from_entry(entry, pred);
                  if (pred->instr && pred->instr != instr)
                     ppir_instr_add_depend(instr, pred->instr);
               }
            }
         }
      }
   }
}

static void ppir_schedule_calc_reg_pressure(ppir_instr *instr)
{
   int n = 0;

   /* update all children's reg pressure */
   ppir_instr_foreach_pred(instr, entry) {
      ppir_instr *pred = ppir_instr_from_entry(entry);
      if (pred->reg_pressure < 0)
         ppir_schedule_calc_reg_pressure(pred);
      n++;
   }

   /* leaf instr */
   if (!n) {
      instr->reg_pressure = 0;
      return;
   }

   int i = 0, reg[n];
   ppir_instr_foreach_pred(instr, entry) {
      ppir_instr *pred = ppir_instr_from_entry(entry);
      reg[i++] = pred->reg_pressure;
   }

   /* sort */
   for (i = 0; i < n - 1; i++) {
      for (int j = 0; j < n - i - 1; j++) {
         if (reg[j] > reg[j + 1]) {
            int tmp = reg[j + 1];
            reg[j + 1] = reg[j];
            reg[j] = tmp;
         }
      }
   }

   for (i = 0; i < n; i++) {
      int pressure = reg[i] + n - (i + 1);
      if (pressure > instr->reg_pressure)
         instr->reg_pressure = pressure;
   }
}

/* Register sensitive schedule algorithm from paper:
 * "Register-Sensitive Selection, Duplication, and Sequencing of Instructions"
 * Author: Vivek Sarkar,  Mauricio J. Serrano,  Barbara B. Simons
 */
static void ppir_schedule_block(ppir_block *block)
{
   /* step 2 */
   list_for_each_entry(ppir_instr, instr, &block->instr_list, list) {
      if (ppir_instr_is_root(instr))
         ppir_schedule_calc_reg_pressure(instr);
   }
}

static void _ppir_schedule_prog(ppir_compiler *comp)
{
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      ppir_schedule_block(block);
   }
}

bool ppir_schedule_prog(ppir_compiler *comp)
{
   if (!ppir_schedule_create_instr_from_node(comp))
      return false;
   ppir_instr_print_list(comp);

   ppir_schedule_build_instr_dependency(comp);
   ppir_instr_print_depend(comp);

   _ppir_schedule_prog(comp);

   return true;
}
