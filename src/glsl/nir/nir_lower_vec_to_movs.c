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
 * Implements a simple pass that lowers vecN instructions to a series of
 * moves with partial writes.
 */

static bool
lower_vec_to_movs_block(nir_block *block, void *mem_ctx)
{
   nir_foreach_instr_safe(block, instr) {
      if (instr->type != nir_instr_type_alu)
         continue;

      nir_alu_instr *vec = (nir_alu_instr *)instr;

      switch (vec->op) {
      case nir_op_vec2:
      case nir_op_vec3:
      case nir_op_vec4:
         break;
      default:
         continue; /* The loop */
      }

      for (unsigned i = 0, src_idx = 0; i < 4; i++) {
         if (!(vec->dest.write_mask & (1 << i)))
            continue;

         assert(src_idx < nir_op_infos[vec->op].num_inputs);

         nir_alu_instr *mov = nir_alu_instr_create(mem_ctx, nir_op_imov);
         mov->src[0].src = nir_src_copy(vec->src[src_idx].src, mem_ctx);
         mov->src[0].negate = vec->src[src_idx].negate;
         mov->src[0].abs = vec->src[src_idx].abs;

         /* We only care about the one swizzle */
         mov->src[0].swizzle[i] = vec->src[src_idx].swizzle[0];

         mov->dest.dest = nir_dest_copy(vec->dest.dest, mem_ctx);
         mov->dest.saturate = vec->dest.saturate;
         mov->dest.write_mask = (1u << i);

         nir_instr_insert_before(&vec->instr, &mov->instr);

         src_idx++;
      }

      nir_instr_remove(&vec->instr);
      ralloc_free(vec);
   }

   return true;
}

static void
nir_lower_vec_to_movs_impl(nir_function_impl *impl)
{
   nir_foreach_block(impl, lower_vec_to_movs_block, ralloc_parent(impl));
}

void
nir_lower_vec_to_movs(nir_shader *shader)
{
   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         nir_lower_vec_to_movs_impl(overload->impl);
   }
}
