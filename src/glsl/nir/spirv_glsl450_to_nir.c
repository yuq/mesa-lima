/*
 * Copyright © 2015 Intel Corporation
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
 *    Jason Ekstrand (jason@jlekstrand.net)
 *
 */

#include "spirv_to_nir_private.h"
#include "spirv_glsl450.h"

static nir_ssa_def*
build_length(nir_builder *b, nir_ssa_def *vec)
{
   switch (vec->num_components) {
   case 1: return nir_fsqrt(b, nir_fmul(b, vec, vec));
   case 2: return nir_fsqrt(b, nir_fdot2(b, vec, vec));
   case 3: return nir_fsqrt(b, nir_fdot3(b, vec, vec));
   case 4: return nir_fsqrt(b, nir_fdot4(b, vec, vec));
   default:
      unreachable("Invalid number of components");
   }
}

static void
handle_glsl450_alu(struct vtn_builder *b, enum GLSLstd450 entrypoint,
                   const uint32_t *w, unsigned count)
{
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   val->ssa = rzalloc(b, struct vtn_ssa_value);
   val->ssa->type = vtn_value(b, w[1], vtn_value_type_type)->type->type;

   /* Collect the various SSA sources */
   unsigned num_inputs = count - 5;
   nir_ssa_def *src[3];
   for (unsigned i = 0; i < num_inputs; i++)
      src[i] = vtn_ssa_value(b, w[i + 5])->def;

   nir_op op;
   switch (entrypoint) {
   case GLSLstd450Round:       op = nir_op_fround_even;   break; /* TODO */
   case GLSLstd450RoundEven:   op = nir_op_fround_even;   break;
   case GLSLstd450Trunc:       op = nir_op_ftrunc;        break;
   case GLSLstd450FAbs:        op = nir_op_fabs;          break;
   case GLSLstd450FSign:       op = nir_op_fsign;         break;
   case GLSLstd450Floor:       op = nir_op_ffloor;        break;
   case GLSLstd450Ceil:        op = nir_op_fceil;         break;
   case GLSLstd450Fract:       op = nir_op_ffract;        break;
   case GLSLstd450Radians:
      val->ssa->def = nir_fmul(&b->nb, src[0], nir_imm_float(&b->nb, 0.01745329251));
      return;
   case GLSLstd450Degrees:
      val->ssa->def = nir_fmul(&b->nb, src[0], nir_imm_float(&b->nb, 57.2957795131));
      return;
   case GLSLstd450Sin:         op = nir_op_fsin;       break;
   case GLSLstd450Cos:         op = nir_op_fcos;       break;
   case GLSLstd450Tan:
      val->ssa->def = nir_fdiv(&b->nb, nir_fsin(&b->nb, src[0]),
                               nir_fcos(&b->nb, src[0]));
      return;
   case GLSLstd450Pow:         op = nir_op_fpow;       break;
   case GLSLstd450Exp2:        op = nir_op_fexp2;      break;
   case GLSLstd450Log2:        op = nir_op_flog2;      break;
   case GLSLstd450Sqrt:        op = nir_op_fsqrt;      break;
   case GLSLstd450InverseSqrt: op = nir_op_frsq;       break;

   case GLSLstd450Modf:        op = nir_op_fmod;       break;
   case GLSLstd450FMin:        op = nir_op_fmin;       break;
   case GLSLstd450FMax:        op = nir_op_fmax;       break;
   case GLSLstd450FMix:        op = nir_op_flrp;       break;
   case GLSLstd450Step:
      val->ssa->def = nir_sge(&b->nb, src[1], src[0]);
      return;

   case GLSLstd450Fma:         op = nir_op_ffma;       break;
   case GLSLstd450Ldexp:       op = nir_op_ldexp;      break;

   /* Packing/Unpacking functions */
   case GLSLstd450PackSnorm4x8:      op = nir_op_pack_snorm_4x8;      break;
   case GLSLstd450PackUnorm4x8:      op = nir_op_pack_unorm_4x8;      break;
   case GLSLstd450PackSnorm2x16:     op = nir_op_pack_snorm_2x16;     break;
   case GLSLstd450PackUnorm2x16:     op = nir_op_pack_unorm_2x16;     break;
   case GLSLstd450PackHalf2x16:      op = nir_op_pack_half_2x16;      break;
   case GLSLstd450UnpackSnorm4x8:    op = nir_op_unpack_snorm_4x8;    break;
   case GLSLstd450UnpackUnorm4x8:    op = nir_op_unpack_unorm_4x8;    break;
   case GLSLstd450UnpackSnorm2x16:   op = nir_op_unpack_snorm_2x16;   break;
   case GLSLstd450UnpackUnorm2x16:   op = nir_op_unpack_unorm_2x16;   break;
   case GLSLstd450UnpackHalf2x16:    op = nir_op_unpack_half_2x16;    break;

   case GLSLstd450Length:
      val->ssa->def = build_length(&b->nb, src[0]);
      return;
   case GLSLstd450Distance:
      val->ssa->def = build_length(&b->nb, nir_fsub(&b->nb, src[0], src[1]));
      return;
   case GLSLstd450Normalize:
      val->ssa->def = nir_fdiv(&b->nb, src[0], build_length(&b->nb, src[0]));
      return;

   case GLSLstd450Exp:
   case GLSLstd450Log:
   case GLSLstd450FClamp:
   case GLSLstd450UClamp:
   case GLSLstd450SClamp:
   case GLSLstd450Asin:
   case GLSLstd450Acos:
   case GLSLstd450Atan:
   case GLSLstd450Atan2:
   case GLSLstd450Sinh:
   case GLSLstd450Cosh:
   case GLSLstd450Tanh:
   case GLSLstd450Asinh:
   case GLSLstd450Acosh:
   case GLSLstd450Atanh:
   case GLSLstd450SmoothStep:
   case GLSLstd450Frexp:
   case GLSLstd450PackDouble2x32:
   case GLSLstd450UnpackDouble2x32:
   case GLSLstd450Cross:
   case GLSLstd450FaceForward:
   case GLSLstd450Reflect:
   case GLSLstd450Refract:
   case GLSLstd450IMix:
   default:
      unreachable("Unhandled opcode");
   }

   nir_alu_instr *instr = nir_alu_instr_create(b->shader, op);
   nir_ssa_dest_init(&instr->instr, &instr->dest.dest,
                     glsl_get_vector_elements(val->ssa->type), val->name);
   instr->dest.write_mask = (1 << instr->dest.dest.ssa.num_components) - 1;
   val->ssa->def = &instr->dest.dest.ssa;

   for (unsigned i = 0; i < nir_op_infos[op].num_inputs; i++)
      instr->src[i].src = nir_src_for_ssa(src[i]);

   nir_builder_instr_insert(&b->nb, &instr->instr);
}

bool
vtn_handle_glsl450_instruction(struct vtn_builder *b, uint32_t ext_opcode,
                               const uint32_t *words, unsigned count)
{
   switch ((enum GLSLstd450)ext_opcode) {
   case GLSLstd450Determinant:
   case GLSLstd450MatrixInverse:
   case GLSLstd450InterpolateAtCentroid:
   case GLSLstd450InterpolateAtSample:
   case GLSLstd450InterpolateAtOffset:
      unreachable("Unhandled opcode");

   default:
      handle_glsl450_alu(b, (enum GLSLstd450)ext_opcode, words, count);
   }

   return true;
}
