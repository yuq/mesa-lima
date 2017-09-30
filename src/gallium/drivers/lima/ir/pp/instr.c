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
#include "util/hash_table.h"

#include "ppir.h"

ppir_instr *ppir_instr_create(ppir_block *block)
{
   ppir_instr *instr = rzalloc(block, ppir_instr);
   if (!instr)
      return NULL;

   instr->preds = _mesa_set_create(instr, _mesa_hash_pointer, _mesa_key_pointer_equal);
   if (!instr->preds)
      goto err_out;
   instr->succs = _mesa_set_create(instr, _mesa_hash_pointer, _mesa_key_pointer_equal);
   if (!instr->succs)
      goto err_out;

   list_addtail(&instr->list, &block->instr_list);
   return instr;

err_out:
   ralloc_free(instr);
   return NULL;
}

void ppir_instr_add_depend(ppir_instr *succ, ppir_instr *pred)
{
   /* don't add duplicated instr */
   ppir_instr_foreach_pred(succ, entry) {
      ppir_instr *instr = ppir_instr_from_entry(entry);
      if (instr == pred)
         return;
   }

   _mesa_set_add(succ->preds, pred);
   _mesa_set_add(pred->succs, succ);
}

/* check whether a const slot fix into another const slot */
static bool ppir_instr_insert_const(ppir_const *dst, const ppir_const *src,
                                    uint8_t *swizzle)
{
   int i, j;

   for (i = 0; i < src->num; i++) {
      for (j = 0; j < dst->num; j++) {
         if (src->value[i].ui == dst->value[j].ui)
            break;
      }

      if (j == dst->num) {
         if (dst->num == 4)
            return false;
         dst->value[dst->num++] = src->value[i];
      }

      swizzle[i] = j;
   }

   return true;
}

/* make alu node src reflact the pipeline reg */
static void ppir_instr_update_src_pipeline(ppir_instr *instr, ppir_pipeline pipeline,
                                           ppir_dest *dest, uint8_t *swizzle)
{
   for (int i = PPIR_INSTR_SLOT_ALU_START; i <= PPIR_INSTR_SLOT_ALU_END; i++) {
      if (!instr->slots[i])
         continue;

      ppir_alu_node *alu = ppir_node_to_alu(instr->slots[i]);
      for (int j = 0; j < alu->num_src; j++) {
         ppir_src *src = alu->src + j;
         if (ppir_node_target_equal(src, dest)) {
            src->type = ppir_target_pipeline;
            src->pipeline = pipeline;

            if (swizzle) {
               for (int k = 0; k < 4; k++)
                  src->swizzle[k] = swizzle[src->swizzle[k]];
            }
         }
      }
   }
}

bool ppir_instr_insert_node(ppir_instr *instr, ppir_node *node)
{
   switch (node->op) {
   case ppir_op_mov:
   case ppir_op_mul:
      instr->slots[PPIR_INSTR_SLOT_ALU_VEC_MUL] = node;
      break;

   case ppir_op_add:
      instr->slots[PPIR_INSTR_SLOT_ALU_VEC_ADD] = node;
      break;

   case ppir_op_const:
   {
      int i;
      ppir_const_node *c = ppir_node_to_const(node);
      const ppir_const *nc = &c->constant;

      for (i = 0; i < 2; i++) {
         ppir_const ic = instr->constant[i];
         uint8_t swizzle[4] = {0};

         if (ppir_instr_insert_const(&ic, nc, swizzle)) {
            instr->constant[i] = ic;
            ppir_instr_update_src_pipeline(
               instr, ppir_pipeline_reg_const0 + i, &c->dest, swizzle);
            break;
         }
      }

      /* no const slot can insert */
      if (i == 2)
         return false;

      break;
   }
   case ppir_op_load_varying:
      if (instr->slots[PPIR_INSTR_SLOT_VARYING])
         return false;
      instr->slots[PPIR_INSTR_SLOT_VARYING] = node;
      break;

   case ppir_op_load_uniform:
   {
      if (instr->slots[PPIR_INSTR_SLOT_UNIFORM])
         return false;
      instr->slots[PPIR_INSTR_SLOT_UNIFORM] = node;

      ppir_load_node *l = ppir_node_to_load(node);
      ppir_instr_update_src_pipeline(
         instr, ppir_pipeline_reg_uniform, &l->dest, NULL);
      break;
   }
   default:
      return false;
   }

   return true;
}

static struct {
   int len;
   char *name;
} ppir_instr_fields[] = {
   [PPIR_INSTR_SLOT_VARYING] = { 4, "vary" },
   [PPIR_INSTR_SLOT_TEXLD] = { 4, "texl"},
   [PPIR_INSTR_SLOT_UNIFORM] = { 4, "unif" },
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
   printf("const0|1\n");

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
         for (int i = 0; i < 2; i++) {
            if (i)
               printf("| ");

            for (int j = 0; j < instr->constant[i].num; j++)
               printf("%f ", instr->constant[i].value[j].f);
         }
         printf("\n");
      }
   }
   printf("===================================\n");
}
