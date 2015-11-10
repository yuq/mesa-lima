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
 */

#include "anv_nir.h"

struct lower_push_constants_state {
   nir_shader *shader;
   bool is_scalar;
};

static bool
lower_push_constants_block(nir_block *block, void *void_state)
{
   struct lower_push_constants_state *state = void_state;

   nir_foreach_instr(block, instr) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

      /* TODO: Handle indirect push constants */
      if (intrin->intrinsic != nir_intrinsic_load_push_constant)
         continue;

      assert(intrin->const_index[0] % 4 == 0);
      unsigned dword_offset = intrin->const_index[0] / 4;

      /* We just turn them into uniform loads with the appropreate offset */
      intrin->intrinsic = nir_intrinsic_load_uniform;
      intrin->const_index[0] = 0;
      if (state->is_scalar) {
         intrin->const_index[1] = dword_offset;
      } else {
         unsigned shift = dword_offset % 4;
         /* Can't cross the vec4 boundary */
         assert(shift + intrin->num_components <= 4);

         /* vec4 shifts are in units of vec4's */
         intrin->const_index[1] = dword_offset / 4;

         if (shift) {
            /* If there's a non-zero shift then we need to load a whole vec4
             * and use a move to swizzle it into place.
             */
            assert(intrin->dest.is_ssa);
            nir_alu_instr *mov = nir_alu_instr_create(state->shader,
                                                      nir_op_imov);
            mov->src[0].src = nir_src_for_ssa(&intrin->dest.ssa);
            for (unsigned i = 0; i < intrin->num_components; i++)
               mov->src[0].swizzle[i] = i + shift;
            mov->dest.write_mask = (1 << intrin->num_components) - 1;
            nir_ssa_dest_init(&mov->instr, &mov->dest.dest,
                              intrin->num_components, NULL);

            nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                                     nir_src_for_ssa(&mov->dest.dest.ssa));
            nir_instr_insert_after(&intrin->instr, &mov->instr);

            /* Stomp the number of components to 4 */
            intrin->num_components = 4;
            intrin->dest.ssa.num_components = 4;
         }
      }
   }

   return true;
}

void
anv_nir_lower_push_constants(nir_shader *shader, bool is_scalar)
{
   struct lower_push_constants_state state = {
      .shader = shader,
      .is_scalar = is_scalar,
   };

   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         nir_foreach_block(overload->impl, lower_push_constants_block, &state);
   }

   assert(shader->num_uniforms % 4 == 0);
   if (is_scalar)
      shader->num_uniforms /= 4;
   else
      shader->num_uniforms = DIV_ROUND_UP(shader->num_uniforms, 16);
}
