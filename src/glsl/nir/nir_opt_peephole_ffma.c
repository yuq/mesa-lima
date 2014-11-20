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

/*
 * Implements a small peephole optimization that looks for a multiply that
 * is only ever used in an add and replaces both with an fma.
 */

struct peephole_ffma_state {
   void *mem_ctx;
   bool progress;
};

static inline nir_alu_instr *
get_mul_for_src(nir_alu_instr *add, unsigned idx)
{
   if (!add->src[idx].src.is_ssa)
      return NULL;

   /* We can't handle these in between the operations */
   if (add->src[idx].negate || add->src[idx].abs)
      return NULL;

   nir_instr *instr = add->src[idx].src.ssa->parent_instr;
   if (instr->type != nir_instr_type_alu)
      return NULL;

   nir_alu_instr *mul = nir_instr_as_alu(instr);
   if (mul->op != nir_op_fmul)
      return NULL;

   /* Can't handle a saturate in between */
   if (mul->dest.saturate)
      return NULL;

   /* We already know that the same source is not used twice in the add and
    * we will assume valid use-def information, so this check is sufficient
    */
   if (mul->dest.dest.ssa.uses->entries > 1)
      return NULL; /* Not the only use */

   return mul;
}

/* Copies (and maybe swizzles) the given ALU source */
static inline void
copy_alu_src(void *mem_ctx, nir_alu_src *new_src, nir_alu_src old_src,
             uint8_t *swizzle)
{
   new_src->src = nir_src_copy(old_src.src, mem_ctx);
   new_src->abs = old_src.abs;
   new_src->negate = old_src.negate;

   if (swizzle == NULL) {
      memcpy(new_src->swizzle, old_src.swizzle, sizeof old_src.swizzle);
   } else {
      for (int i = 0; i < 4; ++i) {
         if (swizzle[i] < 4)
            new_src->swizzle[i] = old_src.swizzle[swizzle[i]];
      }
   }
}

static bool
nir_opt_peephole_ffma_block(nir_block *block, void *void_state)
{
   struct peephole_ffma_state *state = void_state;

   nir_foreach_instr_safe(block, instr) {
      if (instr->type != nir_instr_type_alu)
         continue;

      nir_alu_instr *add = nir_instr_as_alu(instr);
      if (add->op != nir_op_fadd)
         continue;

      /* TODO: Maybe bail if this expression is considered "precise"? */

      /* This, is the case a + a.  We would rather handle this with an
       * algebraic reduction than fuse it.  Also, we want to only fuse
       * things where the multiply is used only once and, in this case,
       * it would be used twice by the same instruction.
       */
      if (add->src[0].src.is_ssa && add->src[1].src.is_ssa &&
          add->src[0].src.ssa == add->src[1].src.ssa)
         continue;

      nir_alu_instr *mul = get_mul_for_src(add, 0);
      unsigned mul_src = 0;

      if (mul == NULL) {
         mul = get_mul_for_src(add, 1);
         mul_src = 1;
      }

      if (mul == NULL)
         continue;

      nir_alu_instr *ffma = nir_alu_instr_create(state->mem_ctx, nir_op_ffma);
      ffma->dest.saturate = add->dest.saturate;
      ffma->dest.write_mask = add->dest.write_mask;

      copy_alu_src(state->mem_ctx, &ffma->src[0], mul->src[0],
                   add->src[mul_src].swizzle);
      copy_alu_src(state->mem_ctx, &ffma->src[1], mul->src[1],
                   add->src[mul_src].swizzle);
      copy_alu_src(state->mem_ctx, &ffma->src[2], add->src[1 - mul_src], NULL);

      if (add->dest.dest.is_ssa) {
         ffma->dest.dest.is_ssa = true;
         nir_ssa_def_init(&ffma->instr, &ffma->dest.dest.ssa,
                          add->dest.dest.ssa.num_components,
                          add->dest.dest.ssa.name);

         nir_src ffma_dest_src = {
            .is_ssa = true,
            .ssa = &ffma->dest.dest.ssa,
         };
         nir_ssa_def_rewrite_uses(&add->dest.dest.ssa, ffma_dest_src,
                                  state->mem_ctx);
      } else {
         ffma->dest.dest = nir_dest_copy(add->dest.dest, state->mem_ctx);
      }

      nir_instr_insert_before(&add->instr, &ffma->instr);
      nir_instr_remove(&add->instr);
      nir_instr_remove(&mul->instr);

      state->progress = true;
   }

   return true;
}

static bool
nir_opt_peephole_ffma_impl(nir_function_impl *impl)
{
   struct peephole_ffma_state state;

   state.mem_ctx = ralloc_parent(impl);
   state.progress = false;

   nir_foreach_block(impl, nir_opt_peephole_ffma_block, &state);

   if (state.progress)
      nir_metadata_dirty(impl, nir_metadata_block_index |
                               nir_metadata_dominance);

   return state.progress;
}

bool
nir_opt_peephole_ffma(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         progress |= nir_opt_peephole_ffma_impl(overload->impl);
   }

   return progress;
}
