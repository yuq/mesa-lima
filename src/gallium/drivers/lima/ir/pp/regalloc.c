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

bool ppir_regalloc_prog(ppir_compiler *comp)
{
   return true;
}
