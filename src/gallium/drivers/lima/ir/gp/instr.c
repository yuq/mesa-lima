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

#include "gpir.h"

bool gpir_instr_try_insert_node(gpir_instr *instr, gpir_node *node)
{
   if (!instr->slots[node->sched_pos]) {
      instr->slots[node->sched_pos] = node;
      return true;
   }

   return false;
}

void gpir_instr_print_prog(gpir_compiler *comp)
{
   struct {
      int len;
      char *name;
   } fields[] = {
      [GPIR_INSTR_SLOT_MUL0] = { 8, "mul0" },
      [GPIR_INSTR_SLOT_MUL1] = { 8, "mul1" },
      [GPIR_INSTR_SLOT_ADD0] = { 8, "add0" },
      [GPIR_INSTR_SLOT_ADD1] = { 8, "add1" },
      [GPIR_INSTR_SLOT_LOAD0] = { 10, "load0" },
      [GPIR_INSTR_SLOT_LOAD1] = { 10, "load1" },
      [GPIR_INSTR_SLOT_LOAD2] = { 10, "load2" },
      [GPIR_INSTR_SLOT_BRANCH] = { 8, "branch" },
      [GPIR_INSTR_SLOT_STORE] = { 10, "store" },
      [GPIR_INSTR_SLOT_COMPLEX] = { 10, "complex" },
      [GPIR_INSTR_SLOT_PASS] = { 8, "pass" },
   };

   printf("========prog instr========\n");
   printf("     ");
   for (int i = 0; i < GPIR_INSTR_SLOT_NUM; i++)
      printf("%-*s ", fields[i].len, fields[i].name);
   printf("\n");

   int index = 0;
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      printf("-------block instr------\n");
      for (int i = gpir_instr_array_n(&block->instrs) - 1; i >= 0; i--) {
         printf("%03d: ", index++);
         gpir_instr *instr = gpir_instr_array_e(&block->instrs, i);
         for (int j = 0; j < GPIR_INSTR_SLOT_NUM; j++) {
            gpir_node *node = instr->slots[j];
            char buff[32] = "null";
            if (node)
               snprintf(buff, sizeof(buff), "%d|%s", node->index, gpir_op_infos[node->op].name);
            printf("%-*s ", fields[j].len, buff);
         }
         printf("\n");
      }
   }
   printf("==========================\n");
}
