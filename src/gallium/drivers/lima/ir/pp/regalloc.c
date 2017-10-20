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

#include "util/ralloc.h"
#include "util/register_allocate.h"

#include "ppir.h"
#include "interface.h"

#define PPIR_FULL_REG_NUM  6

#define PPIR_VEC1_REG_NUM       (PPIR_FULL_REG_NUM * 4) /* x, y, z, w */
#define PPIR_VEC2_REG_NUM       (PPIR_FULL_REG_NUM * 3) /* xy, yz, zw */
#define PPIR_VEC3_REG_NUM       (PPIR_FULL_REG_NUM * 2) /* xyz, yzw */
#define PPIR_VEC4_REG_NUM       PPIR_FULL_REG_NUM       /* xyzw */
#define PPIR_HEAD_VEC1_REG_NUM  PPIR_FULL_REG_NUM       /* x */
#define PPIR_HEAD_VEC2_REG_NUM  PPIR_FULL_REG_NUM       /* xy */
#define PPIR_HEAD_VEC3_REG_NUM  PPIR_FULL_REG_NUM       /* xyz */
#define PPIR_HEAD_VEC4_REG_NUM  PPIR_FULL_REG_NUM       /* xyzw */

#define PPIR_VEC1_REG_BASE       0
#define PPIR_VEC2_REG_BASE       (PPIR_VEC1_REG_BASE + PPIR_VEC1_REG_NUM)
#define PPIR_VEC3_REG_BASE       (PPIR_VEC2_REG_BASE + PPIR_VEC2_REG_NUM)
#define PPIR_VEC4_REG_BASE       (PPIR_VEC3_REG_BASE + PPIR_VEC3_REG_NUM)
#define PPIR_HEAD_VEC1_REG_BASE  (PPIR_VEC4_REG_BASE + PPIR_VEC4_REG_NUM)
#define PPIR_HEAD_VEC2_REG_BASE  (PPIR_HEAD_VEC1_REG_BASE + PPIR_HEAD_VEC1_REG_NUM)
#define PPIR_HEAD_VEC3_REG_BASE  (PPIR_HEAD_VEC2_REG_BASE + PPIR_HEAD_VEC2_REG_NUM)
#define PPIR_HEAD_VEC4_REG_BASE  (PPIR_HEAD_VEC3_REG_BASE + PPIR_HEAD_VEC3_REG_NUM)
#define PPIR_REG_COUNT           (PPIR_HEAD_VEC4_REG_BASE + PPIR_HEAD_VEC4_REG_NUM)

enum ppir_ra_reg_class {
   ppir_ra_reg_class_vec1,
   ppir_ra_reg_class_vec2,
   ppir_ra_reg_class_vec3,
   ppir_ra_reg_class_vec4,

   /* 4 reg class for load/store instr regs:
    * load/store instr has no swizzle field, so the (virtual) register
    * must be allocated at the beginning of a (physical) register,
    */
   ppir_ra_reg_class_head_vec1,
   ppir_ra_reg_class_head_vec2,
   ppir_ra_reg_class_head_vec3,
   ppir_ra_reg_class_head_vec4,

   ppir_ra_reg_class_num,
};

static const int ppir_ra_reg_base[ppir_ra_reg_class_num + 1] = {
   [ppir_ra_reg_class_vec1]       = PPIR_VEC1_REG_BASE,
   [ppir_ra_reg_class_vec2]       = PPIR_VEC2_REG_BASE,
   [ppir_ra_reg_class_vec3]       = PPIR_VEC3_REG_BASE,
   [ppir_ra_reg_class_vec4]       = PPIR_VEC4_REG_BASE,
   [ppir_ra_reg_class_head_vec1]  = PPIR_HEAD_VEC1_REG_BASE,
   [ppir_ra_reg_class_head_vec2]  = PPIR_HEAD_VEC2_REG_BASE,
   [ppir_ra_reg_class_head_vec3]  = PPIR_HEAD_VEC3_REG_BASE,
   [ppir_ra_reg_class_head_vec4]  = PPIR_HEAD_VEC4_REG_BASE,
   [ppir_ra_reg_class_num]        = PPIR_REG_COUNT,
};

static unsigned int *
ppir_ra_reg_q_values[ppir_ra_reg_class_num] = {
   (unsigned int []) {1, 2, 3, 4, 1, 2, 3, 4},
   (unsigned int []) {2, 3, 3, 3, 1, 2, 3, 3},
   (unsigned int []) {2, 2, 2, 2, 1, 2, 2, 2},
   (unsigned int []) {1, 1, 1, 1, 1, 1, 1, 1},
   (unsigned int []) {1, 1, 1, 1, 1, 1, 1, 1},
   (unsigned int []) {1, 1, 1, 1, 1, 1, 1, 1},
   (unsigned int []) {1, 1, 1, 1, 1, 1, 1, 1},
   (unsigned int []) {1, 1, 1, 1, 1, 1, 1, 1},
};

struct ra_regs *ppir_regalloc_init(void *mem_ctx)
{
   struct ra_regs *ret = ra_alloc_reg_set(mem_ctx, PPIR_REG_COUNT, false);
   if (!ret)
      return NULL;

   /* (x, y, z, w) (xy, yz, zw) (xyz, yzw) (xyzw) (x) (xy) (xyz) (xyzw) */
   static const int class_reg_num[ppir_ra_reg_class_num] = {
      4, 3, 2, 1, 1, 1, 1, 1,
   };
   /* base reg (x, y, z, w) confliction with other regs */
   for (int h = 0; h < 4; h++) {
      int base_reg_mask = 1 << h;
      for (int i = 1; i < ppir_ra_reg_class_num; i++) {
         int class_reg_base_mask = (1 << ((i % 4) + 1)) - 1;
         for (int j = 0; j < class_reg_num[i]; j++) {
            if (base_reg_mask & (class_reg_base_mask << j)) {
               for (int k = 0; k < PPIR_FULL_REG_NUM; k++) {
                  ra_add_reg_conflict(ret, k * 4 + h,
                     ppir_ra_reg_base[i] + k * class_reg_num[i] + j);
               }
            }
         }
      }
   }
   /* build all other confliction by the base reg confliction */
   for (int i = 0; i < PPIR_VEC1_REG_NUM; i++)
      ra_make_reg_conflicts_transitive(ret, i);

   for (int i = 0; i < ppir_ra_reg_class_num; i++)
      ra_alloc_reg_class(ret);

   int reg_index = 0;
   for (int i = 0; i < ppir_ra_reg_class_num; i++) {
      while (reg_index < ppir_ra_reg_base[i + 1])
         ra_class_add_reg(ret, i, reg_index++);
   }

   ra_set_finalize(ret, ppir_ra_reg_q_values);
   return ret;
}

static ppir_reg *get_src_reg(ppir_src *src)
{
   switch (src->type) {
   case ppir_target_ssa:
      return src->ssa;
   case ppir_target_register:
      return src->reg;
   default:
      return NULL;
   }
}

static ppir_reg *ppir_regalloc_build_liveness_info(ppir_compiler *comp)
{
   ppir_reg *ret = NULL;

   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_node, node, &block->node_list, list) {
         if (node->op == ppir_op_store_color) {
            ppir_store_node *store = ppir_node_to_store(node);
            if (store->src.type == ppir_target_ssa)
               ret = store->src.ssa;
            else
               ret = store->src.reg;
            ret->live_out = INT_MAX;
            continue;
         }

         if (!node->instr)
            continue;

         /* update reg live_in from node dest (write) */
         ppir_dest *dest = ppir_node_get_dest(node);
         if (dest) {
            ppir_reg *reg = NULL;

            if (dest->type == ppir_target_ssa) {
               reg = &dest->ssa;
               list_addtail(&reg->list, &comp->reg_list);
            }
            else if (dest->type == ppir_target_register)
               reg = dest->reg;

            if (reg && node->instr->seq < reg->live_in)
               reg->live_in = node->instr->seq;
         }

         /* update reg live_out from node src (read) */
         if (node->type == ppir_node_type_alu) {
            ppir_alu_node *alu = ppir_node_to_alu(node);
            for (int i = 0; i < alu->num_src; i++) {
               ppir_reg *reg = get_src_reg(alu->src + i);
               if (reg && node->instr->seq > reg->live_out)
                  reg->live_out = node->instr->seq;
            }
         }
         else if (node->type == ppir_node_type_store) {
            ppir_store_node *store = ppir_node_to_store(node);
            ppir_reg *reg = get_src_reg(&store->src);
            if (reg && node->instr->seq > reg->live_out)
               reg->live_out = node->instr->seq;
         }
      }
   }

   return ret;
}

static int get_phy_reg_index(int reg)
{
   int i;

   for (i = 0; i < ppir_ra_reg_class_num; i++) {
      if (reg < ppir_ra_reg_base[i + 1]) {
         reg -= ppir_ra_reg_base[i];
         break;
      }
   }

   if (i < ppir_ra_reg_class_head_vec1)
      return reg / (4 - i) * 4 + reg % (4 - i);
   else
      return reg * 4;
}

static void ppir_regalloc_print_result(ppir_compiler *comp)
{
#ifdef DEBUG
   printf("======ppir regalloc result======\n");
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_instr, instr, &block->instr_list, list) {
         printf("%03d:", instr->index);
         for (int i = 0; i < PPIR_INSTR_SLOT_NUM; i++) {
            ppir_node *node = instr->slots[i];
            if (!node)
               continue;

            printf(" (%d|", node->index);

            ppir_dest *dest = ppir_node_get_dest(node);
            if (dest)
               printf("%d", ppir_target_get_dest_reg_index(dest));

            printf("|");

            if (node->type == ppir_node_type_alu) {
               ppir_alu_node *alu = ppir_node_to_alu(node);
               for (int j = 0; j < alu->num_src; j++) {
                  if (j)
                     printf(" ");

                  printf("%d", ppir_target_get_src_reg_index(alu->src + j));
               }
            }
            else if (node->type == ppir_node_type_store) {
               ppir_store_node *store = ppir_node_to_store(node);
               printf("%d", ppir_target_get_src_reg_index(&store->src));
            }

            printf(")");
         }
         printf("\n");
      }
   }
   printf("--------------------------\n");
#endif
}

bool ppir_regalloc_prog(ppir_compiler *comp)
{
   ppir_reg *end_reg = ppir_regalloc_build_liveness_info(comp);

   struct ra_graph *g = ra_alloc_interference_graph(
      comp->ra, list_length(&comp->reg_list));

   int n = 0, end_reg_index = 0;
   list_for_each_entry(ppir_reg, reg, &comp->reg_list, list) {
      int c = ppir_ra_reg_class_vec1 + (reg->num_components - 1);
      if (reg->is_head)
         c += 4;
      if (reg == end_reg)
         end_reg_index = n;
      ra_set_node_class(g, n++, c);
   }

   int n1 = 0;
   list_for_each_entry(ppir_reg, reg1, &comp->reg_list, list) {
      int n2 = n1 + 1;
      list_for_each_entry_from(ppir_reg, reg2, reg1->list.next,
                               &comp->reg_list, list) {
         bool interference = false;
         if (reg1->live_in < reg2->live_in) {
            if (reg1->live_out > reg2->live_in)
               interference = true;
         }
         else if (reg1->live_in > reg2->live_in) {
            if (reg2->live_out > reg1->live_in)
               interference = true;
         }
         else
            interference = true;

         if (interference)
            ra_add_node_interference(g, n1, n2);

         n2++;
      }
      n1++;
   }

   ra_set_node_reg(g, end_reg_index, ppir_ra_reg_base[ppir_ra_reg_class_vec4]);

   if (!ra_allocate(g)) {
      fprintf(stderr, "ppir: regalloc fail\n");
      goto err_out;
   }

   n = 0;
   list_for_each_entry(ppir_reg, reg, &comp->reg_list, list) {
      int reg_index = ra_get_node_reg(g, n++);
      reg->index = get_phy_reg_index(reg_index);
   }

   ralloc_free(g);

   ppir_regalloc_print_result(comp);
   return true;

err_out:
   ralloc_free(g);
   return false;
}
