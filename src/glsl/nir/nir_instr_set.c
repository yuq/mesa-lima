/*
 * Copyright © 2014 Connor Abbott
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
 */

#include "nir_instr_set.h"

bool
nir_srcs_equal(nir_src src1, nir_src src2)
{
   if (src1.is_ssa) {
      if (src2.is_ssa) {
         return src1.ssa == src2.ssa;
      } else {
         return false;
      }
   } else {
      if (src2.is_ssa) {
         return false;
      } else {
         if ((src1.reg.indirect == NULL) != (src2.reg.indirect == NULL))
            return false;

         if (src1.reg.indirect) {
            if (!nir_srcs_equal(*src1.reg.indirect, *src2.reg.indirect))
               return false;
         }

         return src1.reg.reg == src2.reg.reg &&
                src1.reg.base_offset == src2.reg.base_offset;
      }
   }
}

static bool
nir_alu_srcs_equal(const nir_alu_instr *alu1, const nir_alu_instr *alu2,
                   unsigned src1, unsigned src2)
{
   if (alu1->src[src1].abs != alu2->src[src2].abs ||
       alu1->src[src1].negate != alu2->src[src2].negate)
      return false;

   for (unsigned i = 0; i < nir_ssa_alu_instr_src_components(alu1, src1); i++) {
      if (alu1->src[src1].swizzle[i] != alu2->src[src2].swizzle[i])
         return false;
   }

   return nir_srcs_equal(alu1->src[src1].src, alu2->src[src2].src);
}

bool
nir_instrs_equal(const nir_instr *instr1, const nir_instr *instr2)
{
   if (instr1->type != instr2->type)
      return false;

   switch (instr1->type) {
   case nir_instr_type_alu: {
      nir_alu_instr *alu1 = nir_instr_as_alu(instr1);
      nir_alu_instr *alu2 = nir_instr_as_alu(instr2);

      if (alu1->op != alu2->op)
         return false;

      /* TODO: We can probably acutally do something more inteligent such
       * as allowing different numbers and taking a maximum or something
       * here */
      if (alu1->dest.dest.ssa.num_components != alu2->dest.dest.ssa.num_components)
         return false;

      if (nir_op_infos[alu1->op].algebraic_properties & NIR_OP_IS_COMMUTATIVE) {
         assert(nir_op_infos[alu1->op].num_inputs == 2);
         return (nir_alu_srcs_equal(alu1, alu2, 0, 0) &&
                 nir_alu_srcs_equal(alu1, alu2, 1, 1)) ||
                (nir_alu_srcs_equal(alu1, alu2, 0, 1) &&
                 nir_alu_srcs_equal(alu1, alu2, 1, 0));
      } else {
         for (unsigned i = 0; i < nir_op_infos[alu1->op].num_inputs; i++) {
            if (!nir_alu_srcs_equal(alu1, alu2, i, i))
               return false;
         }
      }
      return true;
   }
   case nir_instr_type_tex: {
      nir_tex_instr *tex1 = nir_instr_as_tex(instr1);
      nir_tex_instr *tex2 = nir_instr_as_tex(instr2);

      if (tex1->op != tex2->op)
         return false;

      if (tex1->num_srcs != tex2->num_srcs)
         return false;
      for (unsigned i = 0; i < tex1->num_srcs; i++) {
         if (tex1->src[i].src_type != tex2->src[i].src_type ||
             !nir_srcs_equal(tex1->src[i].src, tex2->src[i].src)) {
            return false;
         }
      }

      if (tex1->coord_components != tex2->coord_components ||
          tex1->sampler_dim != tex2->sampler_dim ||
          tex1->is_array != tex2->is_array ||
          tex1->is_shadow != tex2->is_shadow ||
          tex1->is_new_style_shadow != tex2->is_new_style_shadow ||
          memcmp(tex1->const_offset, tex2->const_offset,
                 sizeof(tex1->const_offset)) != 0 ||
          tex1->component != tex2->component ||
         tex1->sampler_index != tex2->sampler_index ||
         tex1->sampler_array_size != tex2->sampler_array_size) {
         return false;
      }

      /* Don't support un-lowered sampler derefs currently. */
      if (tex1->sampler || tex2->sampler)
         return false;

      return true;
   }
   case nir_instr_type_load_const: {
      nir_load_const_instr *load1 = nir_instr_as_load_const(instr1);
      nir_load_const_instr *load2 = nir_instr_as_load_const(instr2);

      if (load1->def.num_components != load2->def.num_components)
         return false;

      return memcmp(load1->value.f, load2->value.f,
                    load1->def.num_components * sizeof(*load2->value.f)) == 0;
   }
   case nir_instr_type_phi: {
      nir_phi_instr *phi1 = nir_instr_as_phi(instr1);
      nir_phi_instr *phi2 = nir_instr_as_phi(instr2);

      if (phi1->instr.block != phi2->instr.block)
         return false;

      nir_foreach_phi_src(phi1, src1) {
         nir_foreach_phi_src(phi2, src2) {
            if (src1->pred == src2->pred) {
               if (!nir_srcs_equal(src1->src, src2->src))
                  return false;

               break;
            }
         }
      }

      return true;
   }
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intrinsic1 = nir_instr_as_intrinsic(instr1);
      nir_intrinsic_instr *intrinsic2 = nir_instr_as_intrinsic(instr2);
      const nir_intrinsic_info *info =
         &nir_intrinsic_infos[intrinsic1->intrinsic];

      if (intrinsic1->intrinsic != intrinsic2->intrinsic ||
          intrinsic1->num_components != intrinsic2->num_components)
         return false;

      if (info->has_dest && intrinsic1->dest.ssa.num_components !=
                            intrinsic2->dest.ssa.num_components)
         return false;

      for (unsigned i = 0; i < info->num_srcs; i++) {
         if (!nir_srcs_equal(intrinsic1->src[i], intrinsic2->src[i]))
            return false;
      }

      assert(info->num_variables == 0);

      for (unsigned i = 0; i < info->num_indices; i++) {
         if (intrinsic1->const_index[i] != intrinsic2->const_index[i])
            return false;
      }

      return true;
   }
   case nir_instr_type_call:
   case nir_instr_type_jump:
   case nir_instr_type_ssa_undef:
   case nir_instr_type_parallel_copy:
   default:
      unreachable("Invalid instruction type");
   }

   return false;
}

