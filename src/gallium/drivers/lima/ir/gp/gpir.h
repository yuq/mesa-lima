/*
 * Copyright (c) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#ifndef LIMA_IR_GP_GPIR_H
#define LIMA_IR_GP_GPIR_H

#include "util/list.h"
#include "util/u_math.h"

/* list of operations that a node can do. */
typedef enum {
   gpir_op_mov,

   /* mul ops */
   gpir_op_mul,
   gpir_op_select,
   gpir_op_complex1,
   gpir_op_complex2,

   /* add ops */
   gpir_op_add,
   gpir_op_sub,
   gpir_op_floor,
   gpir_op_sign,
   gpir_op_ge,
   gpir_op_lt,
   gpir_op_min,
   gpir_op_max,
   gpir_op_abs,

   /* mul/add ops */
   gpir_op_neg,

   /* passthrough ops */
   gpir_op_clamp_const,
   gpir_op_preexp2,
   gpir_op_postlog2,

   /* complex ops */
   gpir_op_exp2_impl,
   gpir_op_log2_impl,
   gpir_op_rcp_impl,
   gpir_op_rsqrt_impl,

   /* load/store ops */
   gpir_op_load_uniform,
   gpir_op_load_temp,
   gpir_op_load_attribute,
   gpir_op_load_reg,
   gpir_op_store_temp,
   gpir_op_store_reg,
   gpir_op_store_varying,
   gpir_op_store_temp_load_off0,
   gpir_op_store_temp_load_off1,
   gpir_op_store_temp_load_off2,

   /* branch */
   gpir_op_branch_cond,

   /* const (emulated) */
   gpir_op_const,

   /* copy (emulated) */
   gpir_op_copy,

   /* emulated ops */
   gpir_op_exp2,
   gpir_op_log2,
   gpir_op_rcp,
   gpir_op_rsqrt,
   gpir_op_ceil,
   gpir_op_exp,
   gpir_op_log,
   gpir_op_sin,
   gpir_op_cos,
   gpir_op_tan,
   gpir_op_branch_uncond,
} gpir_op;

typedef struct gpir_node {
   struct list_head list;
   gpir_op op;

   struct gpir_node *children[5];
   unsigned num_child;

   /* point to the end of the sub-struct */
   struct gpir_node **parents;
   unsigned num_parent;
   unsigned max_parent;
} gpir_node;

typedef struct {
   gpir_node node;
   bool dest_negate;
   bool children_negate[4];
   uint8_t children_component[4];
} gpir_alu_node;

typedef struct {
   gpir_node node;
   union fi constant;
} gpir_const_node;

typedef struct {
   gpir_node node;
   unsigned index;
   unsigned component;

   /* for uniforms/temporaries only */
   bool offset;
   unsigned off_reg;
} gpir_load_node;

typedef struct {
   gpir_node node;
   unsigned index; /* must be 0 when storing temporaries */
   bool write_mask[4]; /* which components to store to */
   uint8_t children_component[4];
} gpir_store_node;

typedef struct gpir_block {
   struct list_head list;
   struct list_head node_list;
} gpir_block;

typedef struct {
   gpir_node node;
   gpir_block *dest;
} gpir_branch_node;

typedef struct gpir_compiler {
   struct list_head block_list;
   /* array for searching ssa/reg node */
   gpir_node **var_nodes;
   unsigned reg_base;
} gpir_compiler;

typedef struct gpir_prog {
   void *prog;
   unsigned prog_size;
} gpir_prog;

typedef struct nir_shader nir_shader;

gpir_prog *gpir_compile_nir(nir_shader *nir);

#endif
