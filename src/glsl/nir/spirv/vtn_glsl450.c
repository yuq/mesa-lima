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

#include "vtn_private.h"
#include "GLSL.std.450.h"

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

static inline nir_ssa_def *
build_fclamp(nir_builder *b,
             nir_ssa_def *x, nir_ssa_def *min_val, nir_ssa_def *max_val)
{
   return nir_fmin(b, nir_fmax(b, x, min_val), max_val);
}

/**
 * Return e^x.
 */
static nir_ssa_def *
build_exp(nir_builder *b, nir_ssa_def *x)
{
   return nir_fexp2(b, nir_fmul(b, x, nir_imm_float(b, M_LOG2E)));
}

/**
 * Return ln(x) - the natural logarithm of x.
 */
static nir_ssa_def *
build_log(nir_builder *b, nir_ssa_def *x)
{
   return nir_fmul(b, nir_flog2(b, x), nir_imm_float(b, 1.0 / M_LOG2E));
}

static void
handle_glsl450_alu(struct vtn_builder *b, enum GLSLstd450 entrypoint,
                   const uint32_t *w, unsigned count)
{
   struct nir_builder *nb = &b->nb;
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
   case GLSLstd450SAbs:        op = nir_op_iabs;          break;
   case GLSLstd450FSign:       op = nir_op_fsign;         break;
   case GLSLstd450SSign:       op = nir_op_isign;         break;
   case GLSLstd450Floor:       op = nir_op_ffloor;        break;
   case GLSLstd450Ceil:        op = nir_op_fceil;         break;
   case GLSLstd450Fract:       op = nir_op_ffract;        break;
   case GLSLstd450Radians:
      val->ssa->def = nir_fmul(nb, src[0], nir_imm_float(nb, 0.01745329251));
      return;
   case GLSLstd450Degrees:
      val->ssa->def = nir_fmul(nb, src[0], nir_imm_float(nb, 57.2957795131));
      return;
   case GLSLstd450Sin:         op = nir_op_fsin;       break;
   case GLSLstd450Cos:         op = nir_op_fcos;       break;
   case GLSLstd450Tan:
      val->ssa->def = nir_fdiv(nb, nir_fsin(nb, src[0]),
                               nir_fcos(nb, src[0]));
      return;
   case GLSLstd450Pow:         op = nir_op_fpow;       break;
   case GLSLstd450Exp2:        op = nir_op_fexp2;      break;
   case GLSLstd450Log2:        op = nir_op_flog2;      break;
   case GLSLstd450Sqrt:        op = nir_op_fsqrt;      break;
   case GLSLstd450InverseSqrt: op = nir_op_frsq;       break;

   case GLSLstd450Modf: {
      val->ssa->def = nir_ffract(nb, src[0]);
      nir_deref_var *out = vtn_value(b, w[6], vtn_value_type_deref)->deref;
      nir_store_deref_var(nb, out, nir_ffloor(nb, src[0]), 0xf);
      return;
   }

   op = nir_op_fmod;       break;
   case GLSLstd450FMin:        op = nir_op_fmin;       break;
   case GLSLstd450UMin:        op = nir_op_umin;       break;
   case GLSLstd450SMin:        op = nir_op_imin;       break;
   case GLSLstd450FMax:        op = nir_op_fmax;       break;
   case GLSLstd450UMax:        op = nir_op_umax;       break;
   case GLSLstd450SMax:        op = nir_op_imax;       break;
   case GLSLstd450FMix:        op = nir_op_flrp;       break;
   case GLSLstd450Step:
      val->ssa->def = nir_sge(nb, src[1], src[0]);
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
      val->ssa->def = build_length(nb, src[0]);
      return;
   case GLSLstd450Distance:
      val->ssa->def = build_length(nb, nir_fsub(nb, src[0], src[1]));
      return;
   case GLSLstd450Normalize:
      val->ssa->def = nir_fdiv(nb, src[0], build_length(nb, src[0]));
      return;

   case GLSLstd450Exp:
      val->ssa->def = build_exp(nb, src[0]);
      return;

   case GLSLstd450Log:
      val->ssa->def = build_log(nb, src[0]);
      return;

   case GLSLstd450FClamp:
      val->ssa->def = build_fclamp(nb, src[0], src[1], src[2]);
      return;
   case GLSLstd450UClamp:
      val->ssa->def = nir_umin(nb, nir_umax(nb, src[0], src[1]), src[2]);
      return;
   case GLSLstd450SClamp:
      val->ssa->def = nir_imin(nb, nir_imax(nb, src[0], src[1]), src[2]);
      return;

   case GLSLstd450Cross: {
      unsigned yzx[4] = { 1, 2, 0, 0 };
      unsigned zxy[4] = { 2, 0, 1, 0 };
      val->ssa->def =
         nir_fsub(nb, nir_fmul(nb, nir_swizzle(nb, src[0], yzx, 3, true),
                                   nir_swizzle(nb, src[1], zxy, 3, true)),
                      nir_fmul(nb, nir_swizzle(nb, src[0], zxy, 3, true),
                                   nir_swizzle(nb, src[1], yzx, 3, true)));
      return;
   }

   case GLSLstd450SmoothStep: {
      /* t = clamp((x - edge0) / (edge1 - edge0), 0, 1) */
      nir_ssa_def *t =
         build_fclamp(nb, nir_fdiv(nb, nir_fsub(nb, src[2], src[0]),
                                       nir_fsub(nb, src[1], src[0])),
                          nir_imm_float(nb, 0.0), nir_imm_float(nb, 1.0));
      /* result = t * t * (3 - 2 * t) */
      val->ssa->def =
         nir_fmul(nb, t, nir_fmul(nb, t,
            nir_fsub(nb, nir_imm_float(nb, 3.0),
                         nir_fmul(nb, nir_imm_float(nb, 2.0), t))));
      return;
   }

   case GLSLstd450FaceForward:
      val->ssa->def =
         nir_bcsel(nb, nir_flt(nb, nir_fdot(nb, src[2], src[1]),
                                   nir_imm_float(nb, 0.0)),
                       src[0], nir_fneg(nb, src[0]));
      return;

   case GLSLstd450Reflect:
      /* I - 2 * dot(N, I) * N */
      val->ssa->def =
         nir_fsub(nb, src[0], nir_fmul(nb, nir_imm_float(nb, 2.0),
                              nir_fmul(nb, nir_fdot(nb, src[0], src[1]),
                                           src[1])));
      return;

   case GLSLstd450Refract: {
      nir_ssa_def *I = src[0];
      nir_ssa_def *N = src[1];
      nir_ssa_def *eta = src[2];
      nir_ssa_def *n_dot_i = nir_fdot(nb, N, I);
      nir_ssa_def *one = nir_imm_float(nb, 1.0);
      nir_ssa_def *zero = nir_imm_float(nb, 0.0);
      /* k = 1.0 - eta * eta * (1.0 - dot(N, I) * dot(N, I)) */
      nir_ssa_def *k =
         nir_fsub(nb, one, nir_fmul(nb, eta, nir_fmul(nb, eta,
                      nir_fsub(nb, one, nir_fmul(nb, n_dot_i, n_dot_i)))));
      nir_ssa_def *result =
         nir_fsub(nb, nir_fmul(nb, eta, I),
                      nir_fmul(nb, nir_fadd(nb, nir_fmul(nb, eta, n_dot_i),
                                                nir_fsqrt(nb, k)), N));
      /* XXX: bcsel, or if statement? */
      val->ssa->def = nir_bcsel(nb, nir_flt(nb, k, zero), zero, result);
      return;
   }

   case GLSLstd450Sinh:
      /* 0.5 * (e^x - e^(-x)) */
      val->ssa->def =
         nir_fmul(nb, nir_imm_float(nb, 0.5f),
                      nir_fsub(nb, build_exp(nb, src[0]),
                                   build_exp(nb, nir_fneg(nb, src[0]))));
      return;

   case GLSLstd450Cosh:
      /* 0.5 * (e^x + e^(-x)) */
      val->ssa->def =
         nir_fmul(nb, nir_imm_float(nb, 0.5f),
                      nir_fadd(nb, build_exp(nb, src[0]),
                                   build_exp(nb, nir_fneg(nb, src[0]))));
      return;

   case GLSLstd450Tanh:
      /* (e^x - e^(-x)) / (e^x + e^(-x)) */
      val->ssa->def =
         nir_fdiv(nb, nir_fsub(nb, build_exp(nb, src[0]),
                                   build_exp(nb, nir_fneg(nb, src[0]))),
                      nir_fadd(nb, build_exp(nb, src[0]),
                                   build_exp(nb, nir_fneg(nb, src[0]))));
      return;

   case GLSLstd450Asinh:
      val->ssa->def = nir_fmul(nb, nir_fsign(nb, src[0]),
         build_log(nb, nir_fadd(nb, nir_fabs(nb, src[0]),
                       nir_fsqrt(nb, nir_fadd(nb, nir_fmul(nb, src[0], src[0]),
                                                  nir_imm_float(nb, 1.0f))))));
      return;
   case GLSLstd450Acosh:
      val->ssa->def = build_log(nb, nir_fadd(nb, src[0],
         nir_fsqrt(nb, nir_fsub(nb, nir_fmul(nb, src[0], src[0]),
                                    nir_imm_float(nb, 1.0f)))));
      return;
   case GLSLstd450Atanh: {
      nir_ssa_def *one = nir_imm_float(nb, 1.0);
      val->ssa->def = nir_fmul(nb, nir_imm_float(nb, 0.5f),
         build_log(nb, nir_fdiv(nb, nir_fadd(nb, one, src[0]),
                                    nir_fsub(nb, one, src[0]))));
      return;
   }

   case GLSLstd450FindILsb:   op = nir_op_find_lsb;   break;
   case GLSLstd450FindSMsb:   op = nir_op_ifind_msb;  break;
   case GLSLstd450FindUMsb:   op = nir_op_ufind_msb;  break;

   case GLSLstd450Asin:
   case GLSLstd450Acos:
   case GLSLstd450Atan:
   case GLSLstd450Atan2:
   case GLSLstd450ModfStruct:
   case GLSLstd450Frexp:
   case GLSLstd450FrexpStruct:
   case GLSLstd450PackDouble2x32:
   case GLSLstd450UnpackDouble2x32:
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

   nir_builder_instr_insert(nb, &instr->instr);
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
