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

const gpir_op_info gpir_op_infos[] = {
   [gpir_op_mov] = {
      .name = "mov",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_mul] = {
      .name = "mul",
      .dest_neg = true,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_select] = {
      .name = "select",
      .dest_neg = true,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_complex1] = {
      .name = "complex1",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 2,
   },
   [gpir_op_complex2] = {
      .name = "complex2",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_add] = {
      .name = "add",
      .dest_neg = false,
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_sub] = {
      .name = "sub",
      .dest_neg = false,
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_floor] = {
      .name = "floor",
      .dest_neg = false,
      .src_neg = {true, false, false, false},
      .latency = 1,
   },
   [gpir_op_sign] = {
      .name = "sign",
      .dest_neg = false,
      .src_neg = {true, false, false, false},
      .latency = 1,
   },
   [gpir_op_ge] = {
      .name = "ge",
      .dest_neg = false,
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_lt] = {
      .name = "lt",
      .dest_neg = false,
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_min] = {
      .name = "min",
      .dest_neg = false,
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_max] = {
      .name = "max",
      .dest_neg = false,
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_abs] = {
      .name = "abs",
      .dest_neg = false,
      .src_neg = {true, true, false, false},
      .latency = 1,
   },
   [gpir_op_neg] = {
      .name = "neg",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_clamp_const] = {
      .name = "clamp_const",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_preexp2] = {
      .name = "preexp2",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_postlog2] = {
      .name = "postlog2",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_exp2_impl] = {
      .name = "exp2_impl",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_log2_impl] = {
      .name = "log2_impl",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_rcp_impl] = {
      .name = "rcp_impl",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_rsqrt_impl] = {
      .name = "rsqrt_impl",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_load_uniform] = {
      .name = "load_uniform",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 0,
   },
   [gpir_op_load_temp] = {
      .name = "load_temp",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 0,
   },
   [gpir_op_load_attribute] = {
      .name = "load_attribute",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 0,
   },
   [gpir_op_load_reg] = {
      .name = "load_reg",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 0,
   },
   [gpir_op_store_temp] = {
      .name = "store_temp",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 4,
   },
   [gpir_op_store_reg] = {
      .name = "store_reg",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 3,
   },
   [gpir_op_store_varying] = {
      .name = "store_varying",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_store_temp_load_off0] = {
      .name = "store_off0",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 4,
   },
   [gpir_op_store_temp_load_off1] = {
      .name = "store_off1",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 4,
   },
   [gpir_op_store_temp_load_off2] = {
      .name = "store_off2",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 4,
   },
   [gpir_op_branch_cond] = {
      .name = "branch_cond",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 1,
   },
   [gpir_op_const] = {
      .name = "const",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
   [gpir_op_copy] = {
      .name = "copy",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = 0,
   },
   [gpir_op_exp2] = {
      .name = "exp2",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
   [gpir_op_log2] = {
      .name = "log2",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
   [gpir_op_rcp] = {
      .name = "rcp",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
   [gpir_op_rsqrt] = {
      .name = "rsqrt",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
   [gpir_op_ceil] = {
      .name = "ceil",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
   [gpir_op_exp] = {
      .name = "exp",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
   [gpir_op_log] = {
      .name = "log",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
   [gpir_op_sin] = {
      .name = "sin",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
   [gpir_op_cos] = {
      .name = "cos",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
   [gpir_op_tan] = {
      .name = "tan",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
   [gpir_op_branch_uncond] = {
      .name = "branch_uncond",
      .dest_neg = false,
      .src_neg = {false, false, false, false},
      .latency = -1,
   },
};

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

   node->distance = -1;

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
