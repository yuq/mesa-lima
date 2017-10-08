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
#include "codegen.h"
#include "lima_context.h"

static void ppir_codegen_encode_varying(ppir_node *node, void *code)
{
   
}

static void ppir_codegen_encode_texld(ppir_node *node, void *code)
{
   
}

static void ppir_codegen_encode_uniform(ppir_node *node, void *code)
{
   
}

static unsigned shift_to_op(int shift)
{
   assert(shift >= -3 && shift <= 3);
   return shift < 0 ? shift + 8 : shift;
}

static unsigned encode_swizzle(uint8_t *swizzle, int shift)
{
   unsigned ret = 0;
   for (int i = 0; i < 4; i++)
      ret |= ((swizzle[i] + shift) & 0x3) << (i * 2);
   return ret;
}

static void ppir_codegen_encode_vec_mul(ppir_node *node, void *code)
{
   ppir_codegen_field_vec4_mul *f = code;
   ppir_alu_node *alu = ppir_node_to_alu(node);

   ppir_dest *dest = &alu->dest;
   int index = ppir_target_get_dest_reg_index(dest);
   f->dest = index >> 2;
   f->mask = dest->write_mask << (index & 0x3);
   f->dest_modifier = dest->modifier;

   switch (node->op) {
   case ppir_op_mul:
      f->op = shift_to_op(alu->shift);
      break;
   case ppir_op_mov:
      f->op = ppir_codegen_vec4_mul_op_mov;
      break;
   default:
      break;
   }

   ppir_src *src = alu->src;
   index = ppir_target_get_src_reg_index(src);
   f->arg0_source = index >> 2;
   f->arg0_swizzle = encode_swizzle(src->swizzle, index & 0x3);
   f->arg0_absolute = src->absolute;
   f->arg0_negate = src->negate;

   if (alu->num_src == 2) {
      src = alu->src + 1;
      index = ppir_target_get_src_reg_index(src);
      f->arg1_source = index >> 2;
      f->arg1_swizzle = encode_swizzle(src->swizzle, index & 0x3);
      f->arg1_absolute = src->absolute;
      f->arg1_negate = src->negate;
   }
}

static void ppir_codegen_encode_scl_mul(ppir_node *node, void *code)
{
   
}

static void ppir_codegen_encode_vec_add(ppir_node *node, void *code)
{
   
}

static void ppir_codegen_encode_scl_add(ppir_node *node, void *code)
{
   
}

static void ppir_codegen_encode_combine(ppir_node *node, void *code)
{
   
}

static void ppir_codegen_encode_store_temp(ppir_node *node, void *code)
{
   
}

static void ppir_codegen_encode_const(ppir_const *constant, void *code)
{
   
}

typedef void (*ppir_codegen_instr_slot_encode_func)(ppir_node *, void *);

static const ppir_codegen_instr_slot_encode_func
ppir_codegen_encode_slot[PPIR_INSTR_SLOT_NUM] = {
   [PPIR_INSTR_SLOT_VARYING] = ppir_codegen_encode_varying,
   [PPIR_INSTR_SLOT_TEXLD] = ppir_codegen_encode_texld,
   [PPIR_INSTR_SLOT_UNIFORM] = ppir_codegen_encode_uniform,
   [PPIR_INSTR_SLOT_ALU_VEC_MUL] = ppir_codegen_encode_vec_mul,
   [PPIR_INSTR_SLOT_ALU_SCL_MUL] = ppir_codegen_encode_scl_mul,
   [PPIR_INSTR_SLOT_ALU_VEC_ADD] = ppir_codegen_encode_vec_add,
   [PPIR_INSTR_SLOT_ALU_SCL_ADD] = ppir_codegen_encode_scl_add,
   [PPIR_INSTR_SLOT_ALU_COMBINE] = ppir_codegen_encode_combine,
   [PPIR_INSTR_SLOT_STORE_TEMP] = ppir_codegen_encode_store_temp,
};

static const int ppir_codegen_field_size[] = {
   34, 62, 41, 43, 30, 44, 31, 30, 41, 73
};

static inline int align_to_word(int size)
{
   return ((size + 0x1f) >> 5);
}

static int get_instr_encode_size(ppir_instr *instr)
{
   int size = 0;

   for (int i = 0; i < PPIR_INSTR_SLOT_NUM; i++) {
      if (instr->slots[i])
         size += ppir_codegen_field_size[i];
   }

   for (int i = 0; i < 2; i++) {
      if (instr->constant[i].num)
         size += 64;
   }

   return align_to_word(size) + 1;
}

static void bitcopy(void *dst, int dst_offset, void *src, int src_size)
{
   
}

static int encode_instr(ppir_instr *instr, void *code, void *last_code)
{
   int size = 0;
   ppir_codegen_ctrl *ctrl = code;

   for (int i = 0; i < PPIR_INSTR_SLOT_NUM; i++) {
      if (instr->slots[i]) {
         /* max field size (73) */
         uint8_t output[10] = {0};

         ppir_codegen_encode_slot[i](instr->slots[i], output);
         bitcopy(ctrl + 1, size, output, ppir_codegen_field_size[i]);

         size += ppir_codegen_field_size[i];
         ctrl->fields |= 1 << i;
      }
   }

   for (int i = 0; i < 2; i++) {
      if (instr->constant[i].num) {
         uint16_t output[4] = {0};

         ppir_codegen_encode_const(instr->constant + i, output);
         bitcopy(ctrl + 1, size, output, instr->constant[i].num * 16);

         size += 64;
         ctrl->fields |= 1 << (ppir_codegen_field_shift_vec4_const_0 + i);
      }
   }

   size = align_to_word(size) + 1;

   ctrl->count = size;
   if (instr->is_end)
      ctrl->stop = true;

   if (last_code) {
      ppir_codegen_ctrl *last_ctrl = last_code;
      last_ctrl->next_count = size;
      last_ctrl->prefetch = true;
   }

   return size;
}

bool ppir_codegen_prog(ppir_compiler *comp)
{
   int size = 0;
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_instr, instr, &block->instr_list, list) {
         size += get_instr_encode_size(instr);
      }
   }

   uint32_t *prog = rzalloc_size(comp->prog, size * sizeof(uint32_t));
   if (!prog)
      return false;

   uint32_t *code = prog, *last_code = NULL;
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_instr, instr, &block->instr_list, list) {
         int offset = encode_instr(instr, code, last_code);
         last_code = code;
         code += offset;
      }
   }

   comp->prog->shader = prog;
   comp->prog->shader_size = size;

   return true;
}
