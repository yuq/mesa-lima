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

#include "ppir.h"


bool ppir_instr_insert_node(ppir_block *block, ppir_node *node)
{
   if (node->op == ppir_op_const)
      return true;

   ppir_instr *instr = rzalloc(block->comp, ppir_instr);
   if (!instr)
      return false;

   switch (node->op) {
   case ppir_op_store_color:
   {
      ppir_store_node *store = ppir_node_to_store(node);
      ppir_node *move = ppir_node_insert_move(block->comp, store->child);
      if (!move)
         return false;

      move->instr = instr;
      list_addtail(&move->list, &node->list);

      instr->slots[PPIR_INSTR_SLOT_ALU_VEC_MUL] = node;
      instr->is_end = true;
      break;
   }

   default:
      return false;
   }

   list_addtail(&instr->list, &block->instr_list);
   return true;
}

void ppir_instr_insert_const(ppir_node *node)
{
   ppir_node_foreach_succ(node, entry) {
      ppir_node *succ = ppir_node_from_entry(entry, succ);
      ppir_instr *instr = succ->instr;

      if (instr->slots[PPIR_INSTR_SLOT_CONST0])
         instr->slots[PPIR_INSTR_SLOT_CONST1] = node;
      else
         instr->slots[PPIR_INSTR_SLOT_CONST0] = node;
   }
}

static struct {
   int len;
   char *name;
} ppir_instr_fields[] = {
   [PPIR_INSTR_SLOT_VARYING] = { 4, "vary" },
   [PPIR_INSTR_SLOT_TEXLD] = { 4, "texl"},
   [PPIR_INSTR_SLOT_UNIFORM] = { 4, "unif" },
   [PPIR_INSTR_SLOT_CONST0] = { 4, "con0" },
   [PPIR_INSTR_SLOT_CONST1] = { 4, "con1" },
   [PPIR_INSTR_SLOT_ALU_VEC_MUL] = { 4, "vmul" },
   [PPIR_INSTR_SLOT_ALU_SCL_MUL] = { 4, "smul" },
   [PPIR_INSTR_SLOT_ALU_VEC_ADD] = { 4, "vadd" },
   [PPIR_INSTR_SLOT_ALU_SCL_ADD] = { 4, "sadd" },
   [PPIR_INSTR_SLOT_ALU_COMBINE] = { 4, "comb" },
   [PPIR_INSTR_SLOT_STORE_TEMP] = { 4, "stor" },
};

void ppir_instr_print_pre_schedule(ppir_compiler *comp)
{
   printf("======ppir pre-schedule instr======\n");
   printf("      ");
   for (int i = 0; i < PPIR_INSTR_SLOT_NUM; i++)
      printf("%-*s ", ppir_instr_fields[i].len, ppir_instr_fields[i].name);
   printf("\n");

   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      int index = 0;
      printf("-----------block instr----------\n");
      list_for_each_entry(ppir_instr, instr, &block->instr_list, list) {
         printf("%c%03d: ", instr->is_end ? '*' : ' ', index++);
         for (int i = 0; i < PPIR_INSTR_SLOT_NUM; i++) {
            ppir_node *node = instr->slots[i];
            if (node)
               printf("%-*d ", ppir_instr_fields[i].len, node->index);
            else
               printf("%-*s ", ppir_instr_fields[i].len, "null");
         }
         printf("\n");
      }
   }
   printf("===================================\n");
}
