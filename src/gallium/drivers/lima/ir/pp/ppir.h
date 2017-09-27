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

#ifndef LIMA_IR_PP_PPIR_H
#define LIMA_IR_PP_PPIR_H

#include "util/u_math.h"
#include "util/list.h"
#include "util/set.h"

typedef enum {
   ppir_op_mov,

   ppir_op_neg,
   ppir_op_add,
   ppir_op_sub,

   ppir_op_ddx,
   ppir_op_ddy,

   ppir_op_mul,
   ppir_op_rcp,
   ppir_op_div,

   ppir_op_sin_lut,
   ppir_op_cos_lut,

   ppir_op_sum3,
   ppir_op_sum4,

   ppir_op_normalize2,
   ppir_op_normalize3,
   ppir_op_normalize4,

   ppir_op_select,

   ppir_op_sin,
   ppir_op_cos,
   ppir_op_tan,
   ppir_op_asin,
   ppir_op_acos,

   ppir_op_atan,
   ppir_op_atan2,
   ppir_op_atan_pt1,
   ppir_op_atan2_pt1,
   ppir_op_atan_pt2,

   ppir_op_pow,
   ppir_op_exp,
   ppir_op_log,
   ppir_op_exp2,
   ppir_op_log2,
   ppir_op_sqrt,
   ppir_op_rsqrt,

   ppir_op_abs,
   ppir_op_sign,
   ppir_op_floor,
   ppir_op_ceil,
   ppir_op_fract,
   ppir_op_mod,
   ppir_op_min,
   ppir_op_max,

   ppir_op_dot2,
   ppir_op_dot3,
   ppir_op_dot4,

   ppir_op_gt,
   ppir_op_ge,
   ppir_op_eq,
   ppir_op_ne,
   ppir_op_not,

   ppir_op_copy,

   ppir_op_load_uniform,
   ppir_op_load_varying,
   ppir_op_load_texture,

   ppir_op_store_temp,
   ppir_op_store_color,

   ppir_op_const,

   ppir_op_num,
} ppir_op;

typedef enum {
   ppir_node_type_alu,
   ppir_node_type_const,
   ppir_node_type_load,
   ppir_node_type_store,
} ppir_node_type;

typedef struct {
   char *name;
   ppir_node_type type;
} ppir_op_info;

extern const ppir_op_info ppir_op_infos[];

typedef struct ppir_node {
   struct list_head list;
   ppir_op op;
   ppir_node_type type;
   int index;
   char name[16];
   bool printed;
   struct ppir_instr *instr;

   /* for scheduler */
   struct set *preds, *succs;
} ppir_node;

typedef enum {
   ppir_pipeline_reg_const0,
   ppir_pipeline_reg_const1,
   ppir_pipeline_reg_sampler,
   ppir_pipeline_reg_uniform,
   ppir_pipeline_reg_vmul,
   ppir_pipeline_reg_fmul,
   ppir_pipeline_reg_discard, /* varying load */
} ppir_pipeline_reg;

typedef struct ppir_reg {
   struct list_head list;

   int index;
   int num_components;
} ppir_reg;

typedef struct ppir_ssa {
   int num_components;
} ppir_ssa;

typedef struct ppir_src {
   enum {
      ppir_src_ssa,
      ppir_src_pipeline,
      ppir_src_register,
   } type;

   union {
      ppir_reg *reg;
      ppir_pipeline_reg pipeline_reg;
      ppir_ssa *ssa;
   } reg;

   uint8_t swizzle[4];
   bool absolute, negate;
} ppir_src;

typedef enum {
   ppir_outmod_none,
   ppir_outmod_clamp_fraction,
   ppir_outmod_clamp_positive,
   ppir_outmod_round,
} ppir_outmod;

typedef struct ppir_dest {
   enum {
      ppir_dest_ssa,
      ppir_dest_pipeline,
      ppir_dest_register,
   } type;

   union {
      ppir_reg *reg;
      ppir_pipeline_reg pipeline_reg;
      ppir_ssa ssa;
   } reg;

   ppir_outmod modifier;
   unsigned write_mask : 4;
} ppir_dest;

typedef struct {
   ppir_node node;
   ppir_node *children[4];
   int num_child;

   ppir_dest dest;
   ppir_src src[4];
} ppir_alu_node;

typedef struct {
   ppir_node node;
   union fi value[4];
   int num_components;
} ppir_const_node;

typedef struct {
   ppir_node node;
   int index;

   ppir_dest dest;
} ppir_load_node;

typedef struct {
   ppir_node node;
   int index;
   ppir_node *child;
} ppir_store_node;

typedef struct {
   ppir_node *pred, *succ;
   bool is_child_dep;
   bool is_offset;
} ppir_dep_info;

enum ppir_instr_slot {
   PPIR_INSTR_SLOT_VARYING,
   PPIR_INSTR_SLOT_TEXLD,
   PPIR_INSTR_SLOT_UNIFORM,
   PPIR_INSTR_SLOT_CONST0,
   PPIR_INSTR_SLOT_CONST1,
   PPIR_INSTR_SLOT_ALU_VEC_MUL,
   PPIR_INSTR_SLOT_ALU_SCL_MUL,
   PPIR_INSTR_SLOT_ALU_VEC_ADD,
   PPIR_INSTR_SLOT_ALU_SCL_ADD,
   PPIR_INSTR_SLOT_ALU_COMBINE,
   PPIR_INSTR_SLOT_STORE_TEMP,
   PPIR_INSTR_SLOT_NUM,
};

typedef struct ppir_instr {
   struct list_head list;
   ppir_node *slots[PPIR_INSTR_SLOT_NUM];
   bool is_end;
} ppir_instr;

typedef struct ppir_block {
   struct list_head list;
   struct list_head node_list;
   struct list_head instr_list;
   struct ppir_compiler *comp;
} ppir_block;

struct lima_fs_shader_state;

typedef struct ppir_compiler {
   struct list_head block_list;
   int cur_index;

   struct list_head reg_list;
   int cur_reg_index;

   /* array for searching ssa/reg node */
   ppir_node **var_nodes;
   unsigned reg_base;

   struct lima_fs_shader_state *prog;
} ppir_compiler;

void *ppir_node_create(ppir_compiler *comp, ppir_op op, int index);
void ppir_node_add_child(ppir_node *parent, ppir_node *child);
void ppir_node_remove_entry(struct set_entry *entry);
void ppir_node_delete(ppir_node *node);
void ppir_node_print_prog(ppir_compiler *comp);
void ppir_node_replace_child(ppir_node *parent, ppir_node *old_child, ppir_node *new_child);
void ppir_node_replace_succ(ppir_node *dst, ppir_node *src);
ppir_node *ppir_node_insert_move(ppir_compiler *comp, ppir_node *node);

static inline bool ppir_node_is_root(ppir_node *node)
{
   return !node->succs->entries;
}

static inline bool ppir_node_is_leaf(ppir_node *node)
{
   return !node->preds->entries;
}

#define ppir_dep_from_entry(entry) ((ppir_dep_info *)(entry->key))
#define ppir_node_from_entry(entry, direction) (ppir_dep_from_entry(entry)->direction)

#define ppir_node_foreach_pred(node, entry)                                \
   for (struct set_entry *entry = _mesa_set_next_entry(node->preds, NULL); \
        entry != NULL;                                                     \
        entry = _mesa_set_next_entry(node->preds, entry))

#define ppir_node_foreach_succ(node, entry)                                \
   for (struct set_entry *entry = _mesa_set_next_entry(node->succs, NULL); \
        entry != NULL;                                                     \
        entry = _mesa_set_next_entry(node->succs, entry))

#define ppir_node_to_alu(node) ((ppir_alu_node *)(node))
#define ppir_node_to_const(node) ((ppir_const_node *)(node))
#define ppir_node_to_load(node) ((ppir_load_node *)(node))
#define ppir_node_to_store(node) ((ppir_store_node *)(node))

static inline ppir_dest *ppir_node_get_dest(ppir_node *node)
{
   switch (node->type) {
   case ppir_node_type_alu:
      return &ppir_node_to_alu(node)->dest;
   case ppir_node_type_load:
      return &ppir_node_to_load(node)->dest;
   default:
      return NULL;
   }
}

bool ppir_instr_insert_node(ppir_block *block, ppir_node *node);
void ppir_instr_insert_const(ppir_node *node);
void ppir_instr_print_pre_schedule(ppir_compiler *comp);

bool ppir_lower_prog(ppir_compiler *comp);
bool ppir_schedule_prog(ppir_compiler *comp);

#endif
