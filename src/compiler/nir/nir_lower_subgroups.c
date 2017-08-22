/*
 * Copyright © 2017 Intel Corporation
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

#include "nir.h"
#include "nir_builder.h"

/**
 * \file nir_opt_intrinsics.c
 */

static nir_ssa_def *
lower_read_invocation_to_scalar(nir_builder *b, nir_intrinsic_instr *intrin)
{
   /* This is safe to call on scalar things but it would be silly */
   assert(intrin->dest.ssa.num_components > 1);

   nir_ssa_def *value = nir_ssa_for_src(b, intrin->src[0],
                                           intrin->num_components);
   nir_ssa_def *reads[4];

   for (unsigned i = 0; i < intrin->num_components; i++) {
      nir_intrinsic_instr *chan_intrin =
         nir_intrinsic_instr_create(b->shader, intrin->intrinsic);
      nir_ssa_dest_init(&chan_intrin->instr, &chan_intrin->dest,
                        1, intrin->dest.ssa.bit_size, NULL);
      chan_intrin->num_components = 1;

      /* value */
      chan_intrin->src[0] = nir_src_for_ssa(nir_channel(b, value, i));
      /* invocation */
      if (intrin->intrinsic == nir_intrinsic_read_invocation)
         nir_src_copy(&chan_intrin->src[1], &intrin->src[1], chan_intrin);

      nir_builder_instr_insert(b, &chan_intrin->instr);

      reads[i] = &chan_intrin->dest.ssa;
   }

   return nir_vec(b, reads, intrin->num_components);
}

static nir_ssa_def *
high_subgroup_mask(nir_builder *b,
                   nir_ssa_def *count,
                   uint64_t base_mask)
{
   /* group_mask could probably be calculated more efficiently but we want to
    * be sure not to shift by 64 if the subgroup size is 64 because the GLSL
    * shift operator is undefined in that case. In any case if we were worried
    * about efficency this should probably be done further down because the
    * subgroup size is likely to be known at compile time.
    */
   nir_ssa_def *subgroup_size = nir_load_subgroup_size(b);
   nir_ssa_def *all_bits = nir_imm_int64(b, ~0ull);
   nir_ssa_def *shift = nir_isub(b, nir_imm_int(b, 64), subgroup_size);
   nir_ssa_def *group_mask = nir_ushr(b, all_bits, shift);
   nir_ssa_def *higher_bits = nir_ishl(b, nir_imm_int64(b, base_mask), count);

   return nir_iand(b, higher_bits, group_mask);
}

static nir_ssa_def *
lower_subgroups_intrin(nir_builder *b, nir_intrinsic_instr *intrin,
                       const nir_lower_subgroups_options *options)
{
   switch (intrin->intrinsic) {
   case nir_intrinsic_vote_any:
   case nir_intrinsic_vote_all:
      if (options->lower_vote_trivial)
         return nir_ssa_for_src(b, intrin->src[0], 1);
      break;

   case nir_intrinsic_vote_eq:
      if (options->lower_vote_trivial)
         return nir_imm_int(b, NIR_TRUE);
      break;

   case nir_intrinsic_read_invocation:
   case nir_intrinsic_read_first_invocation:
      if (options->lower_to_scalar && intrin->num_components > 1)
         return lower_read_invocation_to_scalar(b, intrin);
      break;

   case nir_intrinsic_load_subgroup_eq_mask:
   case nir_intrinsic_load_subgroup_ge_mask:
   case nir_intrinsic_load_subgroup_gt_mask:
   case nir_intrinsic_load_subgroup_le_mask:
   case nir_intrinsic_load_subgroup_lt_mask: {
      if (!options->lower_subgroup_masks)
         return NULL;

      nir_ssa_def *count = nir_load_subgroup_invocation(b);

      switch (intrin->intrinsic) {
      case nir_intrinsic_load_subgroup_eq_mask:
         return nir_ishl(b, nir_imm_int64(b, 1ull), count);
      case nir_intrinsic_load_subgroup_ge_mask:
         return high_subgroup_mask(b, count, ~0ull);
      case nir_intrinsic_load_subgroup_gt_mask:
         return high_subgroup_mask(b, count, ~1ull);
      case nir_intrinsic_load_subgroup_le_mask:
         return nir_inot(b, nir_ishl(b, nir_imm_int64(b, ~1ull), count));
      case nir_intrinsic_load_subgroup_lt_mask:
         return nir_inot(b, nir_ishl(b, nir_imm_int64(b, ~0ull), count));
      default:
         unreachable("you seriously can't tell this is unreachable?");
      }
      break;
   }
   default:
      break;
   }

   return NULL;
}

static bool
lower_subgroups_impl(nir_function_impl *impl,
                     const nir_lower_subgroups_options *options)
{
   nir_builder b;
   nir_builder_init(&b, impl);
   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         b.cursor = nir_before_instr(instr);

         nir_ssa_def *lower = lower_subgroups_intrin(&b, intrin, options);
         if (!lower)
            continue;

         nir_ssa_def_rewrite_uses(&intrin->dest.ssa, nir_src_for_ssa(lower));
         nir_instr_remove(instr);
         progress = true;
      }
   }

   return progress;
}

bool
nir_lower_subgroups(nir_shader *shader,
                    const nir_lower_subgroups_options *options)
{
   bool progress = false;

   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;

      if (lower_subgroups_impl(function->impl, options)) {
         progress = true;
         nir_metadata_preserve(function->impl, nir_metadata_block_index |
                                               nir_metadata_dominance);
      }
   }

   return progress;
}
