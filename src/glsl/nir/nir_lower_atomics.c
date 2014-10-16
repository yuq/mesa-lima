/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Connor Abbott (cwabbott0@gmail.com)
 *
 */

#include "nir.h"
#include <assert.h>

/*
 * replace atomic counter intrinsics that use a variable with intrinsics
 * that directly store the buffer index and byte offset
 */

static void
lower_instr(nir_intrinsic_instr *instr, nir_function_impl *impl)
{
   nir_intrinsic_op op;
   switch (instr->intrinsic) {
   case nir_intrinsic_atomic_counter_read_var:
      op = nir_intrinsic_atomic_counter_read;
      break;

   case nir_intrinsic_atomic_counter_inc_var:
      op = nir_intrinsic_atomic_counter_inc;
      break;

   case nir_intrinsic_atomic_counter_dec_var:
      op = nir_intrinsic_atomic_counter_dec;
      break;

   default:
      return;
   }

   if (instr->variables[0]->var->data.mode != nir_var_uniform)
      return; /* atomics passed as function arguments can't be lowered */

   void *mem_ctx = ralloc_parent(instr);

   /* TODO support SSA */
   assert(!instr->dest.is_ssa);

   nir_intrinsic_instr *new_instr = nir_intrinsic_instr_create(mem_ctx, op);
   new_instr->dest = nir_dest_copy(instr->dest, mem_ctx);
   new_instr->const_index[0] =
      (int) instr->variables[0]->var->data.atomic.buffer_index;

   nir_load_const_instr *offset_const = nir_load_const_instr_create(mem_ctx);
   offset_const->num_components = 1;
   offset_const->value.u[0] = instr->variables[0]->var->data.atomic.offset;
   offset_const->dest.reg.reg = nir_local_reg_create(impl);
   offset_const->dest.reg.reg->num_components = 1;

   nir_instr_insert_before(&instr->instr, &offset_const->instr);

   nir_register *offset_reg = offset_const->dest.reg.reg;

   if (instr->variables[0]->deref.child != NULL) {
      assert(instr->variables[0]->deref.child->deref_type ==
             nir_deref_type_array);
      nir_deref_array *deref_array =
         nir_deref_as_array(instr->variables[0]->deref.child);
      assert(deref_array->deref.child == NULL);

      offset_const->value.u[0] += deref_array->base_offset;

      if (deref_array->has_indirect) {
         nir_load_const_instr *atomic_counter_size =
               nir_load_const_instr_create(mem_ctx);
         atomic_counter_size->num_components = 1;
         atomic_counter_size->value.u[0] = ATOMIC_COUNTER_SIZE;
         atomic_counter_size->dest.reg.reg = nir_local_reg_create(impl);
         atomic_counter_size->dest.reg.reg->num_components = 1;
         nir_instr_insert_before(&instr->instr, &atomic_counter_size->instr);

         nir_alu_instr *mul = nir_alu_instr_create(mem_ctx, nir_op_imul);
         mul->dest.dest.reg.reg = nir_local_reg_create(impl);
         mul->dest.dest.reg.reg->num_components = 1;
         mul->dest.write_mask = 0x1;
         mul->src[0].src = nir_src_copy(deref_array->indirect, mem_ctx);
         mul->src[1].src.reg.reg = atomic_counter_size->dest.reg.reg;
         nir_instr_insert_before(&instr->instr, &mul->instr);

         nir_alu_instr *add = nir_alu_instr_create(mem_ctx, nir_op_iadd);
         add->dest.dest.reg.reg = nir_local_reg_create(impl);
         add->dest.dest.reg.reg->num_components = 1;
         add->dest.write_mask = 0x1;
         add->src[0].src.reg.reg = mul->dest.dest.reg.reg;
         add->src[1].src.reg.reg = offset_const->dest.reg.reg;
         nir_instr_insert_before(&instr->instr, &add->instr);

         offset_reg = add->dest.dest.reg.reg;
      }
   }

   new_instr->src[0].reg.reg = offset_reg;

   nir_instr_insert_before(&instr->instr, &new_instr->instr);
   nir_instr_remove(&instr->instr);
}

static bool
lower_block(nir_block *block, void *state)
{
   nir_foreach_instr_safe(block, instr) {
      if (instr->type == nir_instr_type_intrinsic)
         lower_instr(nir_instr_as_intrinsic(instr),
                     (nir_function_impl *) state);
   }

   return true;
}

void
nir_lower_atomics(nir_shader *shader)
{
   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         nir_foreach_block(overload->impl, lower_block, overload->impl);
   }
}
