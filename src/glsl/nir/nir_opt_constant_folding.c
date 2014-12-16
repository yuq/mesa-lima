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
 *    Jason Ekstrand (jason@jlekstrand.net)
 *
 */

#include "nir.h"
#include <math.h>

/*
 * Implements SSA-based constant folding.
 */

struct constant_fold_state {
   void *mem_ctx;
   nir_function_impl *impl;
   bool progress;
};

#define SRC_COMP(T, IDX, CMP) src[IDX]->value.T[instr->src[IDX].swizzle[CMP]]
#define SRC(T, IDX) SRC_COMP(T, IDX, i)
#define DEST_COMP(T, CMP) dest->value.T[CMP]
#define DEST(T) DEST_COMP(T, i)

#define FOLD_PER_COMP(EXPR) \
   for (unsigned i = 0; i < instr->dest.dest.ssa.num_components; i++) { \
      EXPR; \
   } \

static bool
constant_fold_alu_instr(nir_alu_instr *instr, void *mem_ctx)
{
   nir_load_const_instr *src[4], *dest;

   if (!instr->dest.dest.is_ssa)
      return false;

   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
      if (!instr->src[i].src.is_ssa)
         return false;

      if (instr->src[i].src.ssa->parent_instr->type != nir_instr_type_load_const)
         return false;

      /* We shouldn't have any source modifiers in the optimization loop. */
      assert(!instr->src[i].abs && !instr->src[i].negate);

      src[i] = nir_instr_as_load_const(instr->src[i].src.ssa->parent_instr);
   }

   /* We shouldn't have any saturate modifiers in the optimization loop. */
   assert(!instr->dest.saturate);

   dest = nir_load_const_instr_create(mem_ctx,
                                      instr->dest.dest.ssa.num_components);

   switch (instr->op) {
   case nir_op_ineg:
      FOLD_PER_COMP(DEST(i) = -SRC(i, 0));
      break;
   case nir_op_fneg:
      FOLD_PER_COMP(DEST(f) = -SRC(f, 0));
      break;
   case nir_op_inot:
      FOLD_PER_COMP(DEST(i) = ~SRC(i, 0));
      break;
   case nir_op_fnot:
      FOLD_PER_COMP(DEST(f) = (SRC(f, 0) == 0.0f) ? 1.0f : 0.0f);
      break;
   case nir_op_frcp:
      FOLD_PER_COMP(DEST(f) = 1.0f / SRC(f, 0));
      break;
   case nir_op_frsq:
      FOLD_PER_COMP(DEST(f) = 1.0f / sqrt(SRC(f, 0)));
      break;
   case nir_op_fsqrt:
      FOLD_PER_COMP(DEST(f) = sqrtf(SRC(f, 0)));
      break;
   case nir_op_fexp:
      FOLD_PER_COMP(DEST(f) = expf(SRC(f, 0)));
      break;
   case nir_op_flog:
      FOLD_PER_COMP(DEST(f) = logf(SRC(f, 0)));
      break;
   case nir_op_fexp2:
      FOLD_PER_COMP(DEST(f) = exp2f(SRC(f, 0)));
      break;
   case nir_op_flog2:
      FOLD_PER_COMP(DEST(f) = log2f(SRC(f, 0)));
      break;
   case nir_op_f2i:
      FOLD_PER_COMP(DEST(i) = SRC(f, 0));
      break;
   case nir_op_f2u:
      FOLD_PER_COMP(DEST(u) = SRC(f, 0));
      break;
   case nir_op_i2f:
      FOLD_PER_COMP(DEST(f) = SRC(i, 0));
      break;
   case nir_op_f2b:
      FOLD_PER_COMP(DEST(u) = (SRC(i, 0) == 0.0f) ? NIR_FALSE : NIR_TRUE);
      break;
   case nir_op_b2f:
      FOLD_PER_COMP(DEST(f) = SRC(u, 0) ? 1.0f : 0.0f);
      break;
   case nir_op_i2b:
      FOLD_PER_COMP(DEST(u) = SRC(i, 0) ? NIR_TRUE : NIR_FALSE);
      break;
   case nir_op_u2f:
      FOLD_PER_COMP(DEST(f) = SRC(u, 0));
      break;
   case nir_op_bany2:
      DEST_COMP(u, 0) = (SRC_COMP(u, 0, 0) || SRC_COMP(u, 0, 1)) ?
                        NIR_TRUE : NIR_FALSE;
      break;
   case nir_op_fadd:
      FOLD_PER_COMP(DEST(f) = SRC(f, 0) + SRC(f, 1));
      break;
   case nir_op_iadd:
      FOLD_PER_COMP(DEST(i) = SRC(i, 0) + SRC(i, 1));
      break;
   case nir_op_fsub:
      FOLD_PER_COMP(DEST(f) = SRC(f, 0) - SRC(f, 1));
      break;
   case nir_op_isub:
      FOLD_PER_COMP(DEST(i) = SRC(i, 0) - SRC(i, 1));
      break;
   case nir_op_fmul:
      FOLD_PER_COMP(DEST(f) = SRC(f, 0) * SRC(f, 1));
      break;
   case nir_op_imul:
      FOLD_PER_COMP(DEST(i) = SRC(i, 0) * SRC(i, 1));
      break;
   case nir_op_fdiv:
      FOLD_PER_COMP(DEST(f) = SRC(f, 0) / SRC(f, 1));
      break;
   case nir_op_idiv:
      FOLD_PER_COMP(DEST(i) = SRC(i, 0) / SRC(i, 1));
      break;
   case nir_op_udiv:
      FOLD_PER_COMP(DEST(u) = SRC(u, 0) / SRC(u, 1));
      break;
   case nir_op_flt:
      FOLD_PER_COMP(DEST(u) = (SRC(f, 0) < SRC(f, 1)) ? NIR_TRUE : NIR_FALSE);
      break;
   case nir_op_fge:
      FOLD_PER_COMP(DEST(u) = (SRC(f, 0) >= SRC(f, 1)) ? NIR_TRUE : NIR_FALSE);
      break;
   case nir_op_feq:
      FOLD_PER_COMP(DEST(u) = (SRC(f, 0) == SRC(f, 1)) ? NIR_TRUE : NIR_FALSE);
      break;
   case nir_op_fne:
      FOLD_PER_COMP(DEST(u) = (SRC(f, 0) != SRC(f, 1)) ? NIR_TRUE : NIR_FALSE);
      break;
   case nir_op_ilt:
      FOLD_PER_COMP(DEST(u) = (SRC(i, 0) < SRC(i, 1)) ? NIR_TRUE : NIR_FALSE);
      break;
   case nir_op_ige:
      FOLD_PER_COMP(DEST(u) = (SRC(i, 0) >= SRC(i, 1)) ? NIR_TRUE : NIR_FALSE);
      break;
   case nir_op_ieq:
      FOLD_PER_COMP(DEST(u) = (SRC(i, 0) == SRC(i, 1)) ? NIR_TRUE : NIR_FALSE);
      break;
   case nir_op_ine:
      FOLD_PER_COMP(DEST(u) = (SRC(i, 0) != SRC(i, 1)) ? NIR_TRUE : NIR_FALSE);
      break;
   case nir_op_ult:
      FOLD_PER_COMP(DEST(u) = (SRC(u, 0) < SRC(u, 1)) ? NIR_TRUE : NIR_FALSE);
      break;
   case nir_op_uge:
      FOLD_PER_COMP(DEST(u) = (SRC(u, 0) >= SRC(u, 1)) ? NIR_TRUE : NIR_FALSE);
      break;
   case nir_op_ishl:
      FOLD_PER_COMP(DEST(i) = SRC(i, 0) << SRC(i, 1));
      break;
   case nir_op_ishr:
      FOLD_PER_COMP(DEST(i) = SRC(i, 0) >> SRC(i, 1));
      break;
   case nir_op_ushr:
      FOLD_PER_COMP(DEST(u) = SRC(u, 0) >> SRC(u, 1));
      break;
   case nir_op_iand:
      FOLD_PER_COMP(DEST(i) = SRC(i, 0) & SRC(i, 1));
      break;
   case nir_op_ior:
      FOLD_PER_COMP(DEST(i) = SRC(i, 0) | SRC(i, 1));
      break;
   case nir_op_ixor:
      FOLD_PER_COMP(DEST(i) = SRC(i, 0) ^ SRC(i, 1));
      break;
   default:
      ralloc_free(dest);
      return false;
   }

   nir_instr_insert_before(&instr->instr, &dest->instr);

   nir_src new_src = {
      .is_ssa = true,
      .ssa = &dest->def,
   };

   nir_ssa_def_rewrite_uses(&instr->dest.dest.ssa, new_src, mem_ctx);

   nir_instr_remove(&instr->instr);
   ralloc_free(instr);

   return true;
}

static bool
constant_fold_deref(nir_instr *instr, nir_deref_var *deref)
{
   bool progress = false;

   for (nir_deref *tail = deref->deref.child; tail; tail = tail->child) {
      if (tail->deref_type != nir_deref_type_array)
         continue;

      nir_deref_array *arr = nir_deref_as_array(tail);

      if (arr->deref_array_type == nir_deref_array_type_indirect &&
          arr->indirect.is_ssa &&
          arr->indirect.ssa->parent_instr->type == nir_instr_type_load_const) {
         nir_load_const_instr *indirect =
            nir_instr_as_load_const(arr->indirect.ssa->parent_instr);

         arr->base_offset += indirect->value.u[0];

         nir_src empty = {
            .is_ssa = true,
            .ssa = NULL,
         };

         nir_instr_rewrite_src(instr, &arr->indirect, empty);

         arr->deref_array_type = nir_deref_array_type_direct;

         progress = true;
      }
   }

   return progress;
}

static bool
constant_fold_intrinsic_instr(nir_intrinsic_instr *instr)
{
   bool progress = false;

   unsigned num_vars = nir_intrinsic_infos[instr->intrinsic].num_variables;
   for (unsigned i = 0; i < num_vars; i++) {
      progress |= constant_fold_deref(&instr->instr, instr->variables[i]);
   }

   return progress;
}

static bool
constant_fold_tex_instr(nir_tex_instr *instr)
{
   if (instr->sampler)
      return constant_fold_deref(&instr->instr, instr->sampler);
   else
      return false;
}

static bool
constant_fold_block(nir_block *block, void *void_state)
{
   struct constant_fold_state *state = void_state;

   nir_foreach_instr_safe(block, instr) {
      switch (instr->type) {
      case nir_instr_type_alu:
         state->progress |= constant_fold_alu_instr(nir_instr_as_alu(instr),
                                                    state->mem_ctx);
         break;
      case nir_instr_type_intrinsic:
         state->progress |=
            constant_fold_intrinsic_instr(nir_instr_as_intrinsic(instr));
         break;
      case nir_instr_type_tex:
         state->progress |= constant_fold_tex_instr(nir_instr_as_tex(instr));
         break;
      default:
         /* Don't know how to constant fold */
         break;
      }
   }

   return true;
}

static bool
nir_opt_constant_folding_impl(nir_function_impl *impl)
{
   struct constant_fold_state state;

   state.mem_ctx = ralloc_parent(impl);
   state.impl = impl;
   state.progress = false;

   nir_foreach_block(impl, constant_fold_block, &state);

   if (state.progress)
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);

   return state.progress;
}

bool
nir_opt_constant_folding(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         progress |= nir_opt_constant_folding_impl(overload->impl);
   }

   return progress;
}
