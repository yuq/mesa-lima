/*
 * Copyright © 2016 Intel Corporation
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

#include "vtn_private.h"

/*
 * Normally, column vectors in SPIR-V correspond to a single NIR SSA
 * definition. But for matrix multiplies, we want to do one routine for
 * multiplying a matrix by a matrix and then pretend that vectors are matrices
 * with one column. So we "wrap" these things, and unwrap the result before we
 * send it off.
 */

static struct vtn_ssa_value *
wrap_matrix(struct vtn_builder *b, struct vtn_ssa_value *val)
{
   if (val == NULL)
      return NULL;

   if (glsl_type_is_matrix(val->type))
      return val;

   struct vtn_ssa_value *dest = rzalloc(b, struct vtn_ssa_value);
   dest->type = val->type;
   dest->elems = ralloc_array(b, struct vtn_ssa_value *, 1);
   dest->elems[0] = val;

   return dest;
}

static struct vtn_ssa_value *
unwrap_matrix(struct vtn_ssa_value *val)
{
   if (glsl_type_is_matrix(val->type))
         return val;

   return val->elems[0];
}

static struct vtn_ssa_value *
matrix_multiply(struct vtn_builder *b,
                struct vtn_ssa_value *_src0, struct vtn_ssa_value *_src1)
{

   struct vtn_ssa_value *src0 = wrap_matrix(b, _src0);
   struct vtn_ssa_value *src1 = wrap_matrix(b, _src1);
   struct vtn_ssa_value *src0_transpose = wrap_matrix(b, _src0->transposed);
   struct vtn_ssa_value *src1_transpose = wrap_matrix(b, _src1->transposed);

   unsigned src0_rows = glsl_get_vector_elements(src0->type);
   unsigned src0_columns = glsl_get_matrix_columns(src0->type);
   unsigned src1_columns = glsl_get_matrix_columns(src1->type);

   const struct glsl_type *dest_type;
   if (src1_columns > 1) {
      dest_type = glsl_matrix_type(glsl_get_base_type(src0->type),
                                   src0_rows, src1_columns);
   } else {
      dest_type = glsl_vector_type(glsl_get_base_type(src0->type), src0_rows);
   }
   struct vtn_ssa_value *dest = vtn_create_ssa_value(b, dest_type);

   dest = wrap_matrix(b, dest);

   bool transpose_result = false;
   if (src0_transpose && src1_transpose) {
      /* transpose(A) * transpose(B) = transpose(B * A) */
      src1 = src0_transpose;
      src0 = src1_transpose;
      src0_transpose = NULL;
      src1_transpose = NULL;
      transpose_result = true;
   }

   if (src0_transpose && !src1_transpose &&
       glsl_get_base_type(src0->type) == GLSL_TYPE_FLOAT) {
      /* We already have the rows of src0 and the columns of src1 available,
       * so we can just take the dot product of each row with each column to
       * get the result.
       */

      for (unsigned i = 0; i < src1_columns; i++) {
         nir_ssa_def *vec_src[4];
         for (unsigned j = 0; j < src0_rows; j++) {
            vec_src[j] = nir_fdot(&b->nb, src0_transpose->elems[j]->def,
                                          src1->elems[i]->def);
         }
         dest->elems[i]->def = nir_vec(&b->nb, vec_src, src0_rows);
      }
   } else {
      /* We don't handle the case where src1 is transposed but not src0, since
       * the general case only uses individual components of src1 so the
       * optimizer should chew through the transpose we emitted for src1.
       */

      for (unsigned i = 0; i < src1_columns; i++) {
         /* dest[i] = sum(src0[j] * src1[i][j] for all j) */
         dest->elems[i]->def =
            nir_fmul(&b->nb, src0->elems[0]->def,
                     nir_channel(&b->nb, src1->elems[i]->def, 0));
         for (unsigned j = 1; j < src0_columns; j++) {
            dest->elems[i]->def =
               nir_fadd(&b->nb, dest->elems[i]->def,
                        nir_fmul(&b->nb, src0->elems[j]->def,
                                 nir_channel(&b->nb, src1->elems[i]->def, j)));
         }
      }
   }

   dest = unwrap_matrix(dest);

   if (transpose_result)
      dest = vtn_ssa_transpose(b, dest);

   return dest;
}

static struct vtn_ssa_value *
mat_times_scalar(struct vtn_builder *b,
                 struct vtn_ssa_value *mat,
                 nir_ssa_def *scalar)
{
   struct vtn_ssa_value *dest = vtn_create_ssa_value(b, mat->type);
   for (unsigned i = 0; i < glsl_get_matrix_columns(mat->type); i++) {
      if (glsl_get_base_type(mat->type) == GLSL_TYPE_FLOAT)
         dest->elems[i]->def = nir_fmul(&b->nb, mat->elems[i]->def, scalar);
      else
         dest->elems[i]->def = nir_imul(&b->nb, mat->elems[i]->def, scalar);
   }

   return dest;
}

static void
vtn_handle_matrix_alu(struct vtn_builder *b, SpvOp opcode,
                      struct vtn_value *dest,
                      struct vtn_ssa_value *src0, struct vtn_ssa_value *src1)
{
   switch (opcode) {
   case SpvOpFNegate: {
      dest->ssa = vtn_create_ssa_value(b, src0->type);
      unsigned cols = glsl_get_matrix_columns(src0->type);
      for (unsigned i = 0; i < cols; i++)
         dest->ssa->elems[i]->def = nir_fneg(&b->nb, src0->elems[i]->def);
      break;
   }

   case SpvOpFAdd: {
      dest->ssa = vtn_create_ssa_value(b, src0->type);
      unsigned cols = glsl_get_matrix_columns(src0->type);
      for (unsigned i = 0; i < cols; i++)
         dest->ssa->elems[i]->def =
            nir_fadd(&b->nb, src0->elems[i]->def, src1->elems[i]->def);
      break;
   }

   case SpvOpFSub: {
      dest->ssa = vtn_create_ssa_value(b, src0->type);
      unsigned cols = glsl_get_matrix_columns(src0->type);
      for (unsigned i = 0; i < cols; i++)
         dest->ssa->elems[i]->def =
            nir_fsub(&b->nb, src0->elems[i]->def, src1->elems[i]->def);
      break;
   }

   case SpvOpTranspose:
      dest->ssa = vtn_ssa_transpose(b, src0);
      break;

   case SpvOpMatrixTimesScalar:
      if (src0->transposed) {
         dest->ssa = vtn_ssa_transpose(b, mat_times_scalar(b, src0->transposed,
                                                           src1->def));
      } else {
         dest->ssa = mat_times_scalar(b, src0, src1->def);
      }
      break;

   case SpvOpVectorTimesMatrix:
   case SpvOpMatrixTimesVector:
   case SpvOpMatrixTimesMatrix:
      if (opcode == SpvOpVectorTimesMatrix) {
         dest->ssa = matrix_multiply(b, vtn_ssa_transpose(b, src1), src0);
      } else {
         dest->ssa = matrix_multiply(b, src0, src1);
      }
      break;

   default: unreachable("unknown matrix opcode");
   }
}

void
vtn_handle_alu(struct vtn_builder *b, SpvOp opcode,
               const uint32_t *w, unsigned count)
{
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   const struct glsl_type *type =
      vtn_value(b, w[1], vtn_value_type_type)->type->type;

   /* Collect the various SSA sources */
   const unsigned num_inputs = count - 3;
   struct vtn_ssa_value *vtn_src[4] = { NULL, };
   for (unsigned i = 0; i < num_inputs; i++)
      vtn_src[i] = vtn_ssa_value(b, w[i + 3]);

   if (glsl_type_is_matrix(vtn_src[0]->type) ||
       (num_inputs >= 2 && glsl_type_is_matrix(vtn_src[1]->type))) {
      vtn_handle_matrix_alu(b, opcode, val, vtn_src[0], vtn_src[1]);
      return;
   }

   val->ssa = vtn_create_ssa_value(b, type);
   nir_ssa_def *src[4] = { NULL, };
   for (unsigned i = 0; i < num_inputs; i++) {
      assert(glsl_type_is_vector_or_scalar(vtn_src[i]->type));
      src[i] = vtn_src[i]->def;
   }

   /* Indicates that the first two arguments should be swapped.  This is
    * used for implementing greater-than and less-than-or-equal.
    */
   bool swap = false;

   nir_op op;
   switch (opcode) {
   /* Basic ALU operations */
   case SpvOpSNegate:               op = nir_op_ineg;    break;
   case SpvOpFNegate:               op = nir_op_fneg;    break;
   case SpvOpNot:                   op = nir_op_inot;    break;

   case SpvOpAny:
      if (src[0]->num_components == 1) {
         op = nir_op_imov;
      } else {
         switch (src[0]->num_components) {
         case 2:  op = nir_op_bany_inequal2; break;
         case 3:  op = nir_op_bany_inequal3; break;
         case 4:  op = nir_op_bany_inequal4; break;
         }
         src[1] = nir_imm_int(&b->nb, NIR_FALSE);
      }
      break;

   case SpvOpAll:
      if (src[0]->num_components == 1) {
         op = nir_op_imov;
      } else {
         switch (src[0]->num_components) {
         case 2:  op = nir_op_ball_iequal2;  break;
         case 3:  op = nir_op_ball_iequal3;  break;
         case 4:  op = nir_op_ball_iequal4;  break;
         }
         src[1] = nir_imm_int(&b->nb, NIR_TRUE);
      }
      break;

   case SpvOpIAdd:                  op = nir_op_iadd;    break;
   case SpvOpFAdd:                  op = nir_op_fadd;    break;
   case SpvOpISub:                  op = nir_op_isub;    break;
   case SpvOpFSub:                  op = nir_op_fsub;    break;
   case SpvOpIMul:                  op = nir_op_imul;    break;
   case SpvOpFMul:                  op = nir_op_fmul;    break;
   case SpvOpUDiv:                  op = nir_op_udiv;    break;
   case SpvOpSDiv:                  op = nir_op_idiv;    break;
   case SpvOpFDiv:                  op = nir_op_fdiv;    break;
   case SpvOpUMod:                  op = nir_op_umod;    break;
   case SpvOpSMod:                  op = nir_op_umod;    break; /* FIXME? */
   case SpvOpFMod:                  op = nir_op_fmod;    break;

   case SpvOpOuterProduct: {
      for (unsigned i = 0; i < src[1]->num_components; i++) {
         val->ssa->elems[i]->def =
            nir_fmul(&b->nb, src[0], nir_channel(&b->nb, src[1], i));
      }
      return;
   }

   case SpvOpDot:
      assert(src[0]->num_components == src[1]->num_components);
      switch (src[0]->num_components) {
      case 1:  op = nir_op_fmul;    break;
      case 2:  op = nir_op_fdot2;   break;
      case 3:  op = nir_op_fdot3;   break;
      case 4:  op = nir_op_fdot4;   break;
      }
      break;

   case SpvOpIAddCarry:
      assert(glsl_type_is_struct(val->ssa->type));
      val->ssa->elems[0]->def = nir_iadd(&b->nb, src[0], src[1]);
      val->ssa->elems[1]->def = nir_uadd_carry(&b->nb, src[0], src[1]);
      return;

   case SpvOpISubBorrow:
      assert(glsl_type_is_struct(val->ssa->type));
      val->ssa->elems[0]->def = nir_isub(&b->nb, src[0], src[1]);
      val->ssa->elems[1]->def = nir_usub_borrow(&b->nb, src[0], src[1]);
      return;

   case SpvOpUMulExtended:
      assert(glsl_type_is_struct(val->ssa->type));
      val->ssa->elems[0]->def = nir_imul(&b->nb, src[0], src[1]);
      val->ssa->elems[1]->def = nir_umul_high(&b->nb, src[0], src[1]);
      return;

   case SpvOpSMulExtended:
      assert(glsl_type_is_struct(val->ssa->type));
      val->ssa->elems[0]->def = nir_imul(&b->nb, src[0], src[1]);
      val->ssa->elems[1]->def = nir_imul_high(&b->nb, src[0], src[1]);
      return;

   case SpvOpShiftRightLogical:     op = nir_op_ushr;    break;
   case SpvOpShiftRightArithmetic:  op = nir_op_ishr;    break;
   case SpvOpShiftLeftLogical:      op = nir_op_ishl;    break;
   case SpvOpLogicalOr:             op = nir_op_ior;     break;
   case SpvOpLogicalEqual:          op = nir_op_ieq;     break;
   case SpvOpLogicalNotEqual:       op = nir_op_ine;     break;
   case SpvOpLogicalAnd:            op = nir_op_iand;    break;
   case SpvOpLogicalNot:            op = nir_op_inot;    break;
   case SpvOpBitwiseOr:             op = nir_op_ior;     break;
   case SpvOpBitwiseXor:            op = nir_op_ixor;    break;
   case SpvOpBitwiseAnd:            op = nir_op_iand;    break;
   case SpvOpSelect:                op = nir_op_bcsel;   break;
   case SpvOpIEqual:                op = nir_op_ieq;     break;

   case SpvOpBitFieldInsert:        op = nir_op_bitfield_insert;     break;
   case SpvOpBitFieldSExtract:      op = nir_op_ibitfield_extract;   break;
   case SpvOpBitFieldUExtract:      op = nir_op_ubitfield_extract;   break;
   case SpvOpBitReverse:            op = nir_op_bitfield_reverse;    break;
   case SpvOpBitCount:              op = nir_op_bit_count;           break;

   /* Comparisons: (TODO: How do we want to handled ordered/unordered?) */
   case SpvOpFOrdEqual:             op = nir_op_feq;     break;
   case SpvOpFUnordEqual:           op = nir_op_feq;     break;
   case SpvOpINotEqual:             op = nir_op_ine;     break;
   case SpvOpFOrdNotEqual:          op = nir_op_fne;     break;
   case SpvOpFUnordNotEqual:        op = nir_op_fne;     break;
   case SpvOpULessThan:             op = nir_op_ult;     break;
   case SpvOpSLessThan:             op = nir_op_ilt;     break;
   case SpvOpFOrdLessThan:          op = nir_op_flt;     break;
   case SpvOpFUnordLessThan:        op = nir_op_flt;     break;
   case SpvOpUGreaterThan:          op = nir_op_ult;  swap = true;   break;
   case SpvOpSGreaterThan:          op = nir_op_ilt;  swap = true;   break;
   case SpvOpFOrdGreaterThan:       op = nir_op_flt;  swap = true;   break;
   case SpvOpFUnordGreaterThan:     op = nir_op_flt;  swap = true;   break;
   case SpvOpULessThanEqual:        op = nir_op_uge;  swap = true;   break;
   case SpvOpSLessThanEqual:        op = nir_op_ige;  swap = true;   break;
   case SpvOpFOrdLessThanEqual:     op = nir_op_fge;  swap = true;   break;
   case SpvOpFUnordLessThanEqual:   op = nir_op_fge;  swap = true;   break;
   case SpvOpUGreaterThanEqual:     op = nir_op_uge;     break;
   case SpvOpSGreaterThanEqual:     op = nir_op_ige;     break;
   case SpvOpFOrdGreaterThanEqual:  op = nir_op_fge;     break;
   case SpvOpFUnordGreaterThanEqual:op = nir_op_fge;     break;

   /* Conversions: */
   case SpvOpConvertFToU:           op = nir_op_f2u;     break;
   case SpvOpConvertFToS:           op = nir_op_f2i;     break;
   case SpvOpConvertSToF:           op = nir_op_i2f;     break;
   case SpvOpConvertUToF:           op = nir_op_u2f;     break;
   case SpvOpBitcast:               op = nir_op_imov;    break;
   case SpvOpUConvert:
   case SpvOpSConvert:
      op = nir_op_imov; /* TODO: NIR is 32-bit only; these are no-ops. */
      break;
   case SpvOpFConvert:
      op = nir_op_fmov;
      break;

   case SpvOpQuantizeToF16:
      op = nir_op_fquantize2f16;
      break;

   /* Derivatives: */
   case SpvOpDPdx:         op = nir_op_fddx;          break;
   case SpvOpDPdy:         op = nir_op_fddy;          break;
   case SpvOpDPdxFine:     op = nir_op_fddx_fine;     break;
   case SpvOpDPdyFine:     op = nir_op_fddy_fine;     break;
   case SpvOpDPdxCoarse:   op = nir_op_fddx_coarse;   break;
   case SpvOpDPdyCoarse:   op = nir_op_fddy_coarse;   break;
   case SpvOpFwidth:
      val->ssa->def = nir_fadd(&b->nb,
                               nir_fabs(&b->nb, nir_fddx(&b->nb, src[0])),
                               nir_fabs(&b->nb, nir_fddx(&b->nb, src[1])));
      return;
   case SpvOpFwidthFine:
      val->ssa->def = nir_fadd(&b->nb,
                               nir_fabs(&b->nb, nir_fddx_fine(&b->nb, src[0])),
                               nir_fabs(&b->nb, nir_fddx_fine(&b->nb, src[1])));
      return;
   case SpvOpFwidthCoarse:
      val->ssa->def = nir_fadd(&b->nb,
                               nir_fabs(&b->nb, nir_fddx_coarse(&b->nb, src[0])),
                               nir_fabs(&b->nb, nir_fddx_coarse(&b->nb, src[1])));
      return;

   case SpvOpVectorTimesScalar:
      /* The builder will take care of splatting for us. */
      val->ssa->def = nir_fmul(&b->nb, src[0], src[1]);
      return;

   case SpvOpSRem:
   case SpvOpFRem:
      unreachable("No NIR equivalent");

   case SpvOpIsNan:
      val->ssa->def = nir_fne(&b->nb, src[0], src[0]);
      return;

   case SpvOpIsInf:
      val->ssa->def = nir_feq(&b->nb, nir_fabs(&b->nb, src[0]),
                                      nir_imm_float(&b->nb, INFINITY));
      return;

   case SpvOpIsFinite:
   case SpvOpIsNormal:
   case SpvOpSignBitSet:
   case SpvOpLessOrGreater:
   case SpvOpOrdered:
   case SpvOpUnordered:
   default:
      unreachable("Unhandled opcode");
   }

   if (swap) {
      nir_ssa_def *tmp = src[0];
      src[0] = src[1];
      src[1] = tmp;
   }

   val->ssa->def = nir_build_alu(&b->nb, op, src[0], src[1], src[2], src[3]);
}
