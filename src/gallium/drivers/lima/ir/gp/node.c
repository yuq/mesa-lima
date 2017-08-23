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

#include "util/u_math.h"
#include "util/ralloc.h"
#include "util/hash_table.h"

#include "gpir.h"

const gpir_op_info gpir_op_infos[] = {
   [gpir_op_mov] = {
      .name = "mov",
      .slots = (int []) {
         GPIR_INSTR_SLOT_MUL0, GPIR_INSTR_SLOT_MUL1,
         GPIR_INSTR_SLOT_ADD0, GPIR_INSTR_SLOT_ADD1,
         GPIR_INSTR_SLOT_COMPLEX, GPIR_INSTR_SLOT_PASS,
         GPIR_INSTR_SLOT_END
      },
   },
   [gpir_op_mul] = {
      .name = "mul",
      .dest_neg = true,
      .slots = (int []) { GPIR_INSTR_SLOT_MUL0, GPIR_INSTR_SLOT_MUL1, GPIR_INSTR_SLOT_END },
   },
   [gpir_op_select] = {
      .name = "select",
      .dest_neg = true,
   },
   [gpir_op_complex1] = {
      .name = "complex1",
   },
   [gpir_op_complex2] = {
      .name = "complex2",
   },
   [gpir_op_add] = {
      .name = "add",
      .src_neg = {true, true, false, false},
      .slots = (int []) { GPIR_INSTR_SLOT_ADD0, GPIR_INSTR_SLOT_ADD1, GPIR_INSTR_SLOT_END },
   },
   [gpir_op_sub] = {
      .name = "sub",
      .src_neg = {true, true, false, false},
      .slots = (int []) { GPIR_INSTR_SLOT_ADD0, GPIR_INSTR_SLOT_ADD1, GPIR_INSTR_SLOT_END },
   },
   [gpir_op_floor] = {
      .name = "floor",
      .src_neg = {true, false, false, false},
   },
   [gpir_op_sign] = {
      .name = "sign",
      .src_neg = {true, false, false, false},
   },
   [gpir_op_ge] = {
      .name = "ge",
      .src_neg = {true, true, false, false},
   },
   [gpir_op_lt] = {
      .name = "lt",
      .src_neg = {true, true, false, false},
   },
   [gpir_op_min] = {
      .name = "min",
      .src_neg = {true, true, false, false},
   },
   [gpir_op_max] = {
      .name = "max",
      .src_neg = {true, true, false, false},
   },
   [gpir_op_abs] = {
      .name = "abs",
      .src_neg = {true, true, false, false},
   },
   [gpir_op_neg] = {
      .name = "neg",
   },
   [gpir_op_clamp_const] = {
      .name = "clamp_const",
   },
   [gpir_op_preexp2] = {
      .name = "preexp2",
   },
   [gpir_op_postlog2] = {
      .name = "postlog2",
   },
   [gpir_op_exp2_impl] = {
      .name = "exp2_impl",
   },
   [gpir_op_log2_impl] = {
      .name = "log2_impl",
   },
   [gpir_op_rcp_impl] = {
      .name = "rcp_impl",
   },
   [gpir_op_rsqrt_impl] = {
      .name = "rsqrt_impl",
   },
   [gpir_op_load_uniform] = {
      .name = "ld_uni",
      .type = gpir_node_type_load,
   },
   [gpir_op_load_temp] = {
      .name = "ld_tmp",
      .type = gpir_node_type_load,
   },
   [gpir_op_load_attribute] = {
      .name = "ld_att",
      .slots = (int []) {
         GPIR_INSTR_SLOT_REG0_LOAD0, GPIR_INSTR_SLOT_REG0_LOAD1,
         GPIR_INSTR_SLOT_REG0_LOAD2, GPIR_INSTR_SLOT_REG0_LOAD3,
         GPIR_INSTR_SLOT_END
      },
      .type = gpir_node_type_load,
   },
   [gpir_op_load_reg] = {
      .name = "ld_reg",
      .type = gpir_node_type_load,
   },
   [gpir_op_store_temp] = {
      .name = "st_tmp",
      .type = gpir_node_type_store,
   },
   [gpir_op_store_reg] = {
      .name = "st_reg",
      .type = gpir_node_type_store,
   },
   [gpir_op_store_varying] = {
      .name = "st_var",
      .slots = (int []) {
         GPIR_INSTR_SLOT_STORE0, GPIR_INSTR_SLOT_STORE1,
         GPIR_INSTR_SLOT_STORE2, GPIR_INSTR_SLOT_STORE3,
         GPIR_INSTR_SLOT_END
      },
      .type = gpir_node_type_store,
   },
   [gpir_op_store_temp_load_off0] = {
      .name = "st_of0",
      .type = gpir_node_type_store,
   },
   [gpir_op_store_temp_load_off1] = {
      .name = "st_of1",
      .type = gpir_node_type_store,
   },
   [gpir_op_store_temp_load_off2] = {
      .name = "st_of2",
      .type = gpir_node_type_store,
   },
   [gpir_op_branch_cond] = {
      .name = "branch_cond",
      .type = gpir_node_type_branch,
   },
   [gpir_op_const] = {
      .name = "const",
      .type = gpir_node_type_const,
   },
   [gpir_op_copy] = {
      .name = "copy",
   },
   [gpir_op_exp2] = {
      .name = "exp2",
   },
   [gpir_op_log2] = {
      .name = "log2",
   },
   [gpir_op_rcp] = {
      .name = "rcp",
   },
   [gpir_op_rsqrt] = {
      .name = "rsqrt",
   },
   [gpir_op_ceil] = {
      .name = "ceil",
   },
   [gpir_op_exp] = {
      .name = "exp",
   },
   [gpir_op_log] = {
      .name = "log",
   },
   [gpir_op_sin] = {
      .name = "sin",
   },
   [gpir_op_cos] = {
      .name = "cos",
   },
   [gpir_op_tan] = {
      .name = "tan",
   },
   [gpir_op_branch_uncond] = {
      .name = "branch_uncond",
      .type = gpir_node_type_branch,
   },
};

void *gpir_node_create(gpir_compiler *comp, gpir_op op, int index)
{
   static const int node_size[] = {
      [gpir_node_type_alu] = sizeof(gpir_alu_node),
      [gpir_node_type_const] = sizeof(gpir_const_node),
      [gpir_node_type_load] = sizeof(gpir_load_node),
      [gpir_node_type_store] = sizeof(gpir_store_node),
      [gpir_node_type_branch] = sizeof(gpir_branch_node),
   };

   gpir_node_type type = gpir_op_infos[op].type;
   int size = node_size[type];
   gpir_node *node = rzalloc_size(NULL, size);
   if (!node)
      return NULL;

   node->preds = _mesa_set_create(node, _mesa_hash_pointer, _mesa_key_pointer_equal);
   if (!node->preds)
      goto err_out;
   node->succs = _mesa_set_create(node, _mesa_hash_pointer, _mesa_key_pointer_equal);
   if (!node->succs)
      goto err_out;

   if (index >= 0)
      comp->var_nodes[index] = node;

   node->op = op;
   node->type = type;
   node->index = comp->cur_index++;
   node->sched_dist = -1;

   return node;

err_out:
   ralloc_free(node);
   return NULL;
}

static void gpir_node_create_dep(gpir_node *succ, gpir_node *pred,
                                 bool is_child_dep, bool is_offset)
{
   /* don't add duplicated dep */
   gpir_node_foreach_pred(succ, entry) {
      gpir_node *node = gpir_node_from_entry(entry, pred);
      if (node == pred)
         return;
   }

   gpir_dep_info *dep = ralloc(succ, gpir_dep_info);

   dep->pred = pred;
   dep->succ = succ;
   dep->is_child_dep = is_child_dep;
   dep->is_offset = is_offset;

   _mesa_set_add(succ->preds, dep);
   _mesa_set_add(pred->succs, dep);
}

void gpir_node_add_child(gpir_node *parent, gpir_node *child)
{
   gpir_node_create_dep(parent, child, true, false);
}

void gpir_node_remove_entry(struct set_entry *entry)
{
   uint32_t hash = entry->hash;
   gpir_dep_info *dep = gpir_dep_from_entry(entry);

   struct set *set = dep->pred->succs;
   _mesa_set_remove(set, _mesa_set_search_pre_hashed(set, hash, dep));

   set = dep->succ->preds;
   _mesa_set_remove(set, _mesa_set_search_pre_hashed(set, hash, dep));

   ralloc_free(dep);
}

void gpir_node_merge_succ(gpir_node *dst, gpir_node *src)
{
   gpir_node_foreach_succ(src, entry) {
      gpir_node *succ = gpir_node_from_entry(entry, succ);
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node_create_dep(succ, dst, dep->is_child_dep, dep->is_offset);
   }
}

void gpir_node_merge_pred(gpir_node *dst, gpir_node *src)
{
   gpir_node_foreach_pred(src, entry) {
      gpir_node *pred = gpir_node_from_entry(entry, pred);
      gpir_dep_info *dep = gpir_dep_from_entry(entry);
      gpir_node_create_dep(pred, dst, dep->is_child_dep, dep->is_offset);
   }
}

void gpir_node_replace_child(gpir_node *parent, gpir_node *old_child, gpir_node *new_child)
{
   if (parent->type == gpir_node_type_alu) {
      gpir_alu_node *alu = gpir_node_to_alu(parent);
      for (int i = 0; i < alu->num_child; i++) {
         if (alu->children[i] == old_child)
            alu->children[i] = new_child;
      }
   }
   else if (parent->type == gpir_node_type_store) {
      gpir_store_node *store = gpir_node_to_store(parent);
      if (store->child == old_child)
         store->child = new_child;
   }
}

void gpir_node_delete(gpir_node *node)
{
   list_del(&node->list);
   ralloc_free(node);
}

static void gpir_node_print_node(gpir_node *node, int space)
{
   for (int i = 0; i < space; i++)
      printf(" ");
   printf("%s %d\n", gpir_op_infos[node->op].name, node->index);

   gpir_node_foreach_pred(node, entry) {
      gpir_node *pred = gpir_node_from_entry(entry, pred);
      gpir_node_print_node(pred, space + 2);
   }
}

void gpir_node_print_prog(gpir_compiler *comp)
{
   printf("========prog========\n");
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      printf("-------block------\n");
      list_for_each_entry(gpir_node, node, &block->node_list, list) {
         if (gpir_node_is_root(node))
            gpir_node_print_node(node, 0);
      }
   }
   printf("====================\n");
}
