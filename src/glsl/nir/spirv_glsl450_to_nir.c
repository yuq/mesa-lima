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

enum GLSL450Entrypoint {
    Round = 0,
    RoundEven = 1,
    Trunc = 2,
    Abs = 3,
    Sign = 4,
    Floor = 5,
    Ceil = 6,
    Fract = 7,

    Radians = 8,
    Degrees = 9,
    Sin = 10,
    Cos = 11,
    Tan = 12,
    Asin = 13,
    Acos = 14,
    Atan = 15,
    Sinh = 16,
    Cosh = 17,
    Tanh = 18,
    Asinh = 19,
    Acosh = 20,
    Atanh = 21,
    Atan2 = 22,

    Pow = 23,
    Exp = 24,
    Log = 25,
    Exp2 = 26,
    Log2 = 27,
    Sqrt = 28,
    InverseSqrt = 29,

    Determinant = 30,
    MatrixInverse = 31,

    Modf = 32,            // second argument needs the OpVariable = , not an OpLoad
    Min = 33,
    Max = 34,
    Clamp = 35,
    Mix = 36,
    Step = 37,
    SmoothStep = 38,

    FloatBitsToInt = 39,
    FloatBitsToUint = 40,
    IntBitsToFloat = 41,
    UintBitsToFloat = 42,

    Fma = 43,
    Frexp = 44,
    Ldexp = 45,

    PackSnorm4x8 = 46,
    PackUnorm4x8 = 47,
    PackSnorm2x16 = 48,
    PackUnorm2x16 = 49,
    PackHalf2x16 = 50,
    PackDouble2x32 = 51,
    UnpackSnorm2x16 = 52,
    UnpackUnorm2x16 = 53,
    UnpackHalf2x16 = 54,
    UnpackSnorm4x8 = 55,
    UnpackUnorm4x8 = 56,
    UnpackDouble2x32 = 57,

    Length = 58,
    Distance = 59,
    Cross = 60,
    Normalize = 61,
    Ftransform = 62,
    FaceForward = 63,
    Reflect = 64,
    Refract = 65,

    UaddCarry = 66,
    UsubBorrow = 67,
    UmulExtended = 68,
    ImulExtended = 69,
    BitfieldExtract = 70,
    BitfieldInsert = 71,
    BitfieldReverse = 72,
    BitCount = 73,
    FindLSB = 74,
    FindMSB = 75,

    InterpolateAtCentroid = 76,
    InterpolateAtSample = 77,
    InterpolateAtOffset = 78,

    Count
};

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
handle_glsl450_alu(struct vtn_builder *b, enum GLSL450Entrypoint entrypoint,
                   const uint32_t *w, unsigned count)
{
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   val->type = vtn_value(b, w[1], vtn_value_type_type)->type;

   /* Collect the various SSA sources */
   unsigned num_inputs = count - 5;
   nir_ssa_def *src[3];
   for (unsigned i = 0; i < num_inputs; i++)
      src[i] = vtn_ssa_value(b, w[i + 5]);

   nir_op op;
   switch (entrypoint) {
   case Round:       op = nir_op_fround_even;   break; /* TODO */
   case RoundEven:   op = nir_op_fround_even;   break;
   case Trunc:       op = nir_op_ftrunc;        break;
   case Abs:         op = nir_op_fabs;          break;
   case Sign:        op = nir_op_fsign;         break;
   case Floor:       op = nir_op_ffloor;        break;
   case Ceil:        op = nir_op_fceil;         break;
   case Fract:       op = nir_op_ffract;        break;
   case Radians:
      val->ssa = nir_fmul(&b->nb, src[0], nir_imm_float(&b->nb, 0.01745329251));
      return;
   case Degrees:
      val->ssa = nir_fmul(&b->nb, src[0], nir_imm_float(&b->nb, 57.2957795131));
      return;
   case Sin:         op = nir_op_fsin;       break;
   case Cos:         op = nir_op_fcos;       break;
   case Tan:
      val->ssa = nir_fdiv(&b->nb, nir_fsin(&b->nb, src[0]),
                                  nir_fcos(&b->nb, src[0]));
      return;
   case Pow:         op = nir_op_fpow;       break;
   case Exp:         op = nir_op_fexp;       break;
   case Log:         op = nir_op_flog;       break;
   case Exp2:        op = nir_op_fexp2;      break;
   case Log2:        op = nir_op_flog2;      break;
   case Sqrt:        op = nir_op_fsqrt;      break;
   case InverseSqrt: op = nir_op_frsq;       break;

   case Modf:        op = nir_op_fmod;       break;
   case Min:         op = nir_op_fmin;       break;
   case Max:         op = nir_op_fmax;       break;
   case Mix:         op = nir_op_flrp;       break;
   case Step:
      val->ssa = nir_sge(&b->nb, src[1], src[0]);
      return;

   case FloatBitsToInt:
   case FloatBitsToUint:
   case IntBitsToFloat:
   case UintBitsToFloat:
      /* Probably going to be removed from the final version of the spec. */
      val->ssa = src[0];
      return;

   case Fma:         op = nir_op_ffma;       break;
   case Ldexp:       op = nir_op_ldexp;      break;

   /* Packing/Unpacking functions */
   case PackSnorm4x8:      op = nir_op_pack_snorm_4x8;      break;
   case PackUnorm4x8:      op = nir_op_pack_unorm_4x8;      break;
   case PackSnorm2x16:     op = nir_op_pack_snorm_2x16;     break;
   case PackUnorm2x16:     op = nir_op_pack_unorm_2x16;     break;
   case PackHalf2x16:      op = nir_op_pack_half_2x16;      break;
   case UnpackSnorm4x8:    op = nir_op_unpack_snorm_4x8;    break;
   case UnpackUnorm4x8:    op = nir_op_unpack_unorm_4x8;    break;
   case UnpackSnorm2x16:   op = nir_op_unpack_snorm_2x16;   break;
   case UnpackUnorm2x16:   op = nir_op_unpack_unorm_2x16;   break;
   case UnpackHalf2x16:    op = nir_op_unpack_half_2x16;    break;

   case Length:
      val->ssa = build_length(&b->nb, src[0]);
      return;
   case Distance:
      val->ssa = build_length(&b->nb, nir_fsub(&b->nb, src[0], src[1]));
      return;
   case Normalize:
      val->ssa = nir_fdiv(&b->nb, src[0], build_length(&b->nb, src[0]));
      return;

   case UaddCarry:         op = nir_op_uadd_carry;          break;
   case UsubBorrow:        op = nir_op_usub_borrow;         break;
   case BitfieldExtract:   op = nir_op_ubitfield_extract;   break; /* TODO */
   case BitfieldInsert:    op = nir_op_bitfield_insert;     break;
   case BitfieldReverse:   op = nir_op_bitfield_reverse;    break;
   case BitCount:          op = nir_op_bit_count;           break;
   case FindLSB:           op = nir_op_find_lsb;            break;
   case FindMSB:           op = nir_op_ufind_msb;           break; /* TODO */

   case Clamp:
   case Asin:
   case Acos:
   case Atan:
   case Atan2:
   case Sinh:
   case Cosh:
   case Tanh:
   case Asinh:
   case Acosh:
   case Atanh:
   case SmoothStep:
   case Frexp:
   case PackDouble2x32:
   case UnpackDouble2x32:
   case Cross:
   case Ftransform:
   case FaceForward:
   case Reflect:
   case Refract:
   case UmulExtended:
   case ImulExtended:
   default:
      unreachable("Unhandled opcode");
   }

   nir_alu_instr *instr = nir_alu_instr_create(b->shader, op);
   nir_ssa_dest_init(&instr->instr, &instr->dest.dest,
                     glsl_get_vector_elements(val->type), val->name);
   val->ssa = &instr->dest.dest.ssa;

   for (unsigned i = 0; i < nir_op_infos[op].num_inputs; i++)
      instr->src[i].src = nir_src_for_ssa(src[i]);

   nir_builder_instr_insert(&b->nb, &instr->instr);
}

bool
vtn_handle_glsl450_instruction(struct vtn_builder *b, uint32_t ext_opcode,
                               const uint32_t *words, unsigned count)
{
   switch ((enum GLSL450Entrypoint)ext_opcode) {
   case Determinant:
   case MatrixInverse:
   case InterpolateAtCentroid:
   case InterpolateAtSample:
   case InterpolateAtOffset:
      unreachable("Unhandled opcode");

   default:
      handle_glsl450_alu(b, (enum GLSL450Entrypoint)ext_opcode, words, count);
   }

   return true;
}
