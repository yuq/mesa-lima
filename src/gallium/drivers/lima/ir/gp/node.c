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
#include "util/u_memory.h"
#include "gpir.h"

const gpir_op_info gpir_op_infos[] = {
   [gpir_op_mov] = {
      .name = "mov",
      .latency = 1,
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
      .latency = 1,
      .slots = (int []) { GPIR_INSTR_SLOT_MUL0, GPIR_INSTR_SLOT_MUL1, GPIR_INSTR_SLOT_END },
   },
   [gpir_op_select] = {
      .name = "select",
      .dest_neg = true,
      .latency = 1,
   },
   [gpir_op_complex1] = {
      .name = "complex1",
      .latency = 2,
   },
   [gpir_op_complex2] = {
      .name = "complex2",
      .latency = 1,
   },
   [gpir_op_add] = {
      .name = "add",
      .src_neg = {true, true, false, false},
      .latency = 1,
      .slots = (int []) { GPIR_INSTR_SLOT_ADD0, GPIR_INSTR_SLOT_ADD1, GPIR_INSTR_SLOT_END },
   },
   [gpir_op_sub] = {
      .name = "sub",
      .src_neg = {true, true, false, false},
      .latency = 1,
      .slots = (int []) { GPIR_INSTR_SLOT_ADD0, GPIR_INSTR_SLOT_ADD1, GPIR_INSTR_SLOT_END },
   },
   [gpir_op_floor] = {
      .name = "floor",
      .src_neg = {true, false, false, false},
      .latency = 1,
   },
   [gpir_op_sign] = {
      .name = "sign",
      .src_neg = {true, false, false, false},
      .latency = 1,
   },
   [gpir_op_ge] = {
      .name = "ge",
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_lt] = {
      .name = "lt",
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_min] = {
      .name = "min",
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_max] = {
      .name = "max",
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_abs] = {
      .name = "abs",
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_neg] = {
      .name = "neg",
      .latency = 1,
   },
   [gpir_op_clamp_const] = {
      .name = "clamp_const",
      .latency = 1,
   },
   [gpir_op_preexp2] = {
      .name = "preexp2",
      .latency = 1,
   },
   [gpir_op_postlog2] = {
      .name = "postlog2",
      .latency = 1,
   },
   [gpir_op_exp2_impl] = {
      .name = "exp2_impl",
      .latency = 1,
   },
   [gpir_op_log2_impl] = {
      .name = "log2_impl",
      .latency = 1,
   },
   [gpir_op_rcp_impl] = {
      .name = "rcp_impl",
      .latency = 1,
   },
   [gpir_op_rsqrt_impl] = {
      .name = "rsqrt_impl",
      .latency = 1,
   },
   [gpir_op_load_uniform] = {
      .name = "ld_unif",
      .latency = 0,
      .type = gpir_node_type_load,
   },
   [gpir_op_load_temp] = {
      .name = "ld_temp",
      .latency = 0,
      .type = gpir_node_type_load,
   },
   [gpir_op_load_attribute] = {
      .name = "ld_attr",
      .latency = 0,
      .slots = (int []) { GPIR_INSTR_SLOT_LOAD0, GPIR_INSTR_SLOT_END },
      .type = gpir_node_type_load,
   },
   [gpir_op_load_reg] = {
      .name = "ld_reg",
      .latency = 0,
      .type = gpir_node_type_load,
   },
   [gpir_op_store_temp] = {
      .name = "str_temp",
      .latency = 4,
      .type = gpir_node_type_store,
   },
   [gpir_op_store_reg] = {
      .name = "str_reg",
      .latency = 3,
      .type = gpir_node_type_store,
   },
   [gpir_op_store_varying] = {
      .name = "str_vary",
      .latency = 1,
      .slots = (int []) { GPIR_INSTR_SLOT_STORE, GPIR_INSTR_SLOT_END },
      .type = gpir_node_type_store,
   },
   [gpir_op_store_temp_load_off0] = {
      .name = "str_off0",
      .latency = 4,
      .type = gpir_node_type_store,
   },
   [gpir_op_store_temp_load_off1] = {
      .name = "str_off1",
      .latency = 4,
      .type = gpir_node_type_store,
   },
   [gpir_op_store_temp_load_off2] = {
      .name = "str_off2",
      .latency = 4,
      .type = gpir_node_type_store,
   },
   [gpir_op_branch_cond] = {
      .name = "branch_cond",
      .latency = 1,
      .type = gpir_node_type_branch,
   },
   [gpir_op_const] = {
      .name = "const",
      .latency = -1,
      .type = gpir_node_type_const,
   },
   [gpir_op_copy] = {
      .name = "copy",
      .latency = 0,
   },
   [gpir_op_exp2] = {
      .name = "exp2",
      .latency = -1,
   },
   [gpir_op_log2] = {
      .name = "log2",
      .latency = -1,
   },
   [gpir_op_rcp] = {
      .name = "rcp",
      .latency = -1,
   },
   [gpir_op_rsqrt] = {
      .name = "rsqrt",
      .latency = -1,
   },
   [gpir_op_ceil] = {
      .name = "ceil",
      .latency = -1,
   },
   [gpir_op_exp] = {
      .name = "exp",
      .latency = -1,
   },
   [gpir_op_log] = {
      .name = "log",
      .latency = -1,
   },
   [gpir_op_sin] = {
      .name = "sin",
      .latency = -1,
   },
   [gpir_op_cos] = {
      .name = "cos",
      .latency = -1,
   },
   [gpir_op_tan] = {
      .name = "tan",
      .latency = -1,
   },
   [gpir_op_branch_uncond] = {
      .name = "branch_uncond",
      .latency = -1,
      .type = gpir_node_type_branch,
   },
};

void *gpir_node_create(gpir_compiler *comp, gpir_op op, int index, int max_parent)
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
   gpir_node *node = CALLOC(1, size);
   if (!node)
      return NULL;

   if (max_parent) {
      max_parent = util_next_power_of_two(max_parent);
      node->parents = CALLOC(1, max_parent * sizeof(gpir_node *));
      if (!node->parents)
         goto err_out;
   }
   node->max_parent = max_parent;

   if (index >= 0)
      comp->var_nodes[index] = node;

   node->op = op;
   node->type = type;
   node->distance = -1;

   return node;

err_out:
   FREE(node);
   return NULL;
}

static void gpir_node_add_parent(gpir_node *parent, gpir_node *child)
{
   for (int i = 0; i < child->num_parent; i++) {
      if (child->parents[i] == parent)
         return;
   }

   child->parents[child->num_parent++] = parent;
   assert(child->num_parent <= child->max_parent);
}

void gpir_node_add_child(gpir_node *parent, gpir_node *child)
{
   parent->children[parent->num_child++] = child;
   gpir_node_add_parent(parent, child);
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

void gpir_node_replace_parent(gpir_node *child, gpir_node *parent)
{
   for (int i = 0; i < child->num_parent; i++) {
      if (child->parents[i] == parent) {
         child->parents[i] = parent->parents[0];
         break;
      }
   }

   int n = parent->num_parent - 1;
   if (n) {
      int max_parent = child->num_parent + n;
      if (child->max_parent < max_parent) {
         max_parent = util_next_power_of_two(max_parent);
         child->parents = REALLOC(child->parents,
                                  child->max_parent * sizeof(gpir_node *),
                                  max_parent * sizeof(gpir_node *));
         child->max_parent = max_parent;
      }
      for (int i = 0; i < n; i++)
         gpir_node_add_parent(parent->parents[i + 1], child);
   }
}

void gpir_node_delete(gpir_node *node)
{
   list_del(&node->list);
   if (node->max_parent)
      FREE(node->parents);
   FREE(node);
}

static void gpir_node_print_node(gpir_node *node, int space)
{
   for (int i = 0; i < space; i++)
      printf(" ");
   printf("%s\n", gpir_op_infos[node->op].name);

   for (int i = 0; i < node->num_child; i++)
      gpir_node_print_node(node->children[i], space + 2);
}

void gpir_node_print_prog(gpir_compiler *comp)
{
   printf("========prog========\n");
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      printf("-------block------\n");
      list_for_each_entry(gpir_node, node, &block->node_list, list) {
         if (!node->num_parent)
            gpir_node_print_node(node, 0);
      }
   }
   printf("====================\n");
}
