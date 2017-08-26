/*
 * Copyright (c) 2017 Lima Project
 * Copyright (c) 2013 Connor Abbott
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
#include "util/u_dynarray.h"
#include "util/set.h"

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

typedef enum {
   gpir_node_type_alu,
   gpir_node_type_const,
   gpir_node_type_load,
   gpir_node_type_store,
   gpir_node_type_branch,
} gpir_node_type;

typedef struct {
   char *name;
   bool dest_neg;
   bool src_neg[4];
   int *slots;
   gpir_node_type type;
} gpir_op_info;

extern const gpir_op_info gpir_op_infos[];

struct gpir_node;
typedef struct gpir_node gpir_node;

/* structure for storing information about a given dependency
 * Combined with info about instruction placement, should be enough to
 * allow the scheduler to determine if the placement is legal.
 */
typedef struct {
   /* predecessor - node which must be excecuted first */
   gpir_node *pred;
   /* successor - node which must be excecuted last */
   gpir_node *succ;

   /* true - is a dependency between a child and parent node
    * false - is a read/write ordering dependency
    */
   bool is_child_dep;

   /* For temp stores, tells us whether this is an input or an offset.
    * We need to know this because offsets and inputs must be scheduled
    * differently.
    */
   bool is_offset;
} gpir_dep_info;

typedef struct gpir_node {
   struct list_head list;
   gpir_op op;
   gpir_node_type type;
   int index;
   char name[16];
   bool printed;

   /* for scheduler */
   struct set *preds, *succs;
   int sched_dist;
   int sched_instr, sched_pos;
   bool scheduled;
   struct list_head ready;
} gpir_node;

typedef struct {
   gpir_node node;

   gpir_node *children[3];
   bool children_negate[3];
   int num_child;

   bool dest_negate;
} gpir_alu_node;

typedef struct {
   gpir_node node;
   union fi value;
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

   unsigned index;
   gpir_node *child;
   unsigned component;
} gpir_store_node;

enum gpir_instr_slot {
   GPIR_INSTR_SLOT_MUL0,
   GPIR_INSTR_SLOT_MUL1,
   GPIR_INSTR_SLOT_ADD0,
   GPIR_INSTR_SLOT_ADD1,
   GPIR_INSTR_SLOT_COMPLEX,
   GPIR_INSTR_SLOT_PASS,
   GPIR_INSTR_SLOT_BRANCH,
   GPIR_INSTR_SLOT_REG0_LOAD0,
   GPIR_INSTR_SLOT_REG0_LOAD1,
   GPIR_INSTR_SLOT_REG0_LOAD2,
   GPIR_INSTR_SLOT_REG0_LOAD3,
   GPIR_INSTR_SLOT_REG1_LOAD0,
   GPIR_INSTR_SLOT_REG1_LOAD1,
   GPIR_INSTR_SLOT_REG1_LOAD2,
   GPIR_INSTR_SLOT_REG1_LOAD3,
   GPIR_INSTR_SLOT_MEM_LOAD0,
   GPIR_INSTR_SLOT_MEM_LOAD1,
   GPIR_INSTR_SLOT_MEM_LOAD2,
   GPIR_INSTR_SLOT_MEM_LOAD3,
   GPIR_INSTR_SLOT_STORE0,
   GPIR_INSTR_SLOT_STORE1,
   GPIR_INSTR_SLOT_STORE2,
   GPIR_INSTR_SLOT_STORE3,
   GPIR_INSTR_SLOT_NUM,
   GPIR_INSTR_SLOT_END,
};

typedef struct {
   gpir_node *slots[GPIR_INSTR_SLOT_NUM];

   int alu_num_slot_free;
   int alu_num_slot_needed_by_store;

   bool reg0_is_used;
   bool reg0_is_attr;
   int reg0_index;

   bool mem_is_used;
   bool mem_is_temp;
   int mem_index;

   bool store_is_used[2];
   bool store_is_temp;
   bool store_is_reg[2];
   int store_index[2];
} gpir_instr;

typedef struct gpir_block {
   struct list_head list;
   struct list_head node_list;
   struct util_dynarray instrs;
   struct gpir_compiler *comp;
} gpir_block;

#define gpir_instr_array_n(buf) ((buf)->size / sizeof(gpir_instr))
#define gpir_instr_array_e(buf, idx) (util_dynarray_element(buf, gpir_instr, idx))

typedef struct {
   gpir_node node;
   gpir_block *dest;
} gpir_branch_node;

typedef struct gpir_compiler {
   struct list_head block_list;
   int cur_index;
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

void *gpir_node_create(gpir_compiler *comp, gpir_op op, int index);
void gpir_node_add_child(gpir_node *parent, gpir_node *child);
void gpir_node_add_read_after_write_dep(gpir_node *read, gpir_node *write);
void gpir_node_remove_entry(struct set_entry *entry);
void gpir_node_replace_succ(gpir_node *dst, gpir_node *src);
void gpir_node_merge_pred(gpir_node *dst, gpir_node *src);
void gpir_node_replace_child(gpir_node *parent, gpir_node *old_child, gpir_node *new_child);
void gpir_node_delete(gpir_node *node);
void gpir_node_print_prog(gpir_compiler *comp);

static inline bool gpir_node_is_root(gpir_node *node)
{
   return !node->succs->entries;
}

static inline bool gpir_node_is_leaf(gpir_node *node)
{
   return !node->preds->entries;
}

#define gpir_dep_from_entry(entry) ((gpir_dep_info *)(entry->key))
#define gpir_node_from_entry(entry, direction) (gpir_dep_from_entry(entry)->direction)

#define gpir_node_foreach_pred(node, entry)                                \
   for (struct set_entry *entry = _mesa_set_next_entry(node->preds, NULL); \
        entry != NULL;                                                     \
        entry = _mesa_set_next_entry(node->preds, entry))

#define gpir_node_foreach_succ(node, entry)                                \
   for (struct set_entry *entry = _mesa_set_next_entry(node->succs, NULL); \
        entry != NULL;                                                     \
        entry = _mesa_set_next_entry(node->succs, entry))

#define gpir_node_to_alu(node) ((gpir_alu_node *)(node))
#define gpir_node_to_const(node) ((gpir_const_node *)(node))
#define gpir_node_to_load(node) ((gpir_load_node *)(node))
#define gpir_node_to_store(node) ((gpir_store_node *)(node))

void gpir_instr_init(gpir_instr *instr);
bool gpir_instr_try_insert_node(gpir_instr *instr, gpir_node *node);
void gpir_instr_print_prog(gpir_compiler *comp);

void gpir_lower_prog(gpir_compiler *comp);
bool gpir_schedule_prog(gpir_compiler *comp);

#endif
