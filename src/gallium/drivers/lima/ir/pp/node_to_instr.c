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

static bool ppir_create_instr_from_node(ppir_compiler *comp)
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
            ppir_node *move = ppir_node_create(block, ppir_op_mov, -1, 0);
            if (!move)
               return false;

            ppir_node_foreach_pred_safe(node, dep) {
               ppir_node_remove_dep(dep);
               ppir_node_add_dep(move, dep->pred);
            }

            ppir_node_add_dep(node, move);
            list_addtail(&move->list, &node->list);

            ppir_alu_node *alu = ppir_node_to_alu(move);
            ppir_store_node *store = ppir_node_to_store(node);
            alu->src[0] = store->src;
            alu->num_src = 1;

            alu->dest.type = ppir_target_ssa;
            alu->dest.ssa.num_components = 4;
            alu->dest.ssa.live_in = INT_MAX;
            alu->dest.ssa.live_out = 0;
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
            ppir_node_foreach_succ(node, dep) {
               ppir_node *succ = dep->succ;
               if (!ppir_instr_insert_node(succ->instr, node))
                  return false;
            }
            break;
         case ppir_node_type_load:
         {
            if (node->op == ppir_op_load_varying)
               break;

            ppir_load_node *load = ppir_node_to_load(node);
            assert(load->dest.type == ppir_target_ssa);

            struct list_head move_list;
            list_inithead(&move_list);

            ppir_node_foreach_succ_safe(node, dep) {
               ppir_node *succ = dep->succ;
               assert(succ->type == ppir_node_type_alu);

               if (!ppir_instr_insert_node(succ->instr, node)) {
                  /* each instr can only have one load node with the same type
                   * create a move node to insert instead when fail, we can choose:
                   *   1. one move for all failed node (less move but more reg pressure)
                   *   2. one move for one failed node
                   */
                  ppir_node *move = ppir_node_create(block, ppir_op_mov, -1, 0);
                  if (!move)
                     return false;

                  ppir_alu_node *alu = ppir_node_to_alu(move);
                  alu->dest = load->dest;
                  alu->num_src = 1;
                  ppir_node_target_assign(alu->src, &load->dest);
                  for (int i = 0; i < 4; i++)
                     alu->src->swizzle[i] = i;

                  ppir_node_replace_pred(dep, move);
                  ppir_node_replace_child(succ, node, move);

                  if (!create_new_instr(block, move) ||
                      !ppir_instr_insert_node(move->instr, node))
                     return false;

                  /* can't add move to node succs here in a set loop */
                  list_addtail(&move->list, &move_list);
               }
            }

            if (node->op == ppir_op_load_uniform) {
               load->dest.type = ppir_target_pipeline;
               load->dest.pipeline = ppir_pipeline_reg_uniform;
            }

            list_for_each_entry(ppir_node, move, &move_list, list) {
               ppir_node_add_dep(move, node);
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

static void ppir_build_instr_dependency(ppir_compiler *comp)
{
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_instr, instr, &block->instr_list, list) {
         for (int i = 0; i < PPIR_INSTR_SLOT_NUM; i++) {
            ppir_node *node = instr->slots[i];
            if (node) {
               ppir_node_foreach_pred(node, dep) {
                  ppir_node *pred = dep->pred;
                  if (pred->instr && pred->instr != instr)
                     ppir_instr_add_dep(instr, pred->instr);
               }
            }
         }
      }
   }
}

bool ppir_node_to_instr(ppir_compiler *comp)
{
   if (!ppir_create_instr_from_node(comp))
      return false;
   ppir_instr_print_list(comp);

   ppir_build_instr_dependency(comp);
   ppir_instr_print_dep(comp);

   return true;
}
