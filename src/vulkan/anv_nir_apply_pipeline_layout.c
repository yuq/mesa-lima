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
#include "glsl/nir/nir_builder.h"

struct apply_pipeline_layout_state {
   nir_shader *shader;
   nir_builder builder;

   VkShaderStage stage;
   const struct anv_pipeline_layout *layout;

   bool progress;
};

static uint32_t
get_surface_index(unsigned set, unsigned binding,
                  struct apply_pipeline_layout_state *state)
{
   assert(set < state->layout->num_sets);
   struct anv_descriptor_set_layout *set_layout =
      state->layout->set[set].layout;

   assert(binding < set_layout->binding_count);

   assert(set_layout->binding[binding].stage[state->stage].surface_index >= 0);

   uint32_t surface_index =
      state->layout->set[set].stage[state->stage].surface_start +
      set_layout->binding[binding].stage[state->stage].surface_index;

   assert(surface_index < state->layout->stage[state->stage].surface_count);

   return surface_index;
}

static void
lower_res_index_intrinsic(nir_intrinsic_instr *intrin,
                          struct apply_pipeline_layout_state *state)
{
   nir_builder *b = &state->builder;

   b->cursor = nir_before_instr(&intrin->instr);

   uint32_t set = intrin->const_index[0];
   uint32_t binding = intrin->const_index[1];

   uint32_t surface_index = get_surface_index(set, binding, state);

   nir_const_value *const_block_idx =
      nir_src_as_const_value(intrin->src[0]);

   nir_ssa_def *block_index;
   if (const_block_idx) {
      block_index = nir_imm_int(b, surface_index + const_block_idx->u[0]);
   } else {
      block_index = nir_iadd(b, nir_imm_int(b, surface_index),
                             nir_ssa_for_src(b, intrin->src[0], 1));
   }

   assert(intrin->dest.is_ssa);
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, nir_src_for_ssa(block_index));
   nir_instr_remove(&intrin->instr);
}

static void
lower_tex(nir_tex_instr *tex, struct apply_pipeline_layout_state *state)
{
   /* No one should have come by and lowered it already */
   assert(tex->sampler);

   unsigned set = tex->sampler->var->data.descriptor_set;
   unsigned binding = tex->sampler->var->data.binding;

   tex->sampler_index = get_surface_index(set, binding, state);

   if (tex->sampler->deref.child) {
      assert(tex->sampler->deref.child->deref_type == nir_deref_type_array);
      nir_deref_array *deref_array =
         nir_deref_as_array(tex->sampler->deref.child);

      tex->sampler_index += deref_array->base_offset;

      if (deref_array->deref_array_type == nir_deref_array_type_indirect) {
         nir_tex_src *new_srcs = rzalloc_array(tex, nir_tex_src,
                                               tex->num_srcs + 1);

         for (unsigned i = 0; i < tex->num_srcs; i++) {
            new_srcs[i].src_type = tex->src[i].src_type;
            nir_instr_move_src(&tex->instr, &new_srcs[i].src, &tex->src[i].src);
         }

         ralloc_free(tex->src);
         tex->src = new_srcs;

         /* Now we can go ahead and move the source over to being a
          * first-class texture source.
          */
         tex->src[tex->num_srcs].src_type = nir_tex_src_sampler_offset;
         tex->num_srcs++;
         nir_instr_move_src(&tex->instr, &tex->src[tex->num_srcs - 1].src,
                            &deref_array->indirect);
      }
   }

   tex->sampler = NULL;
}

static bool
apply_pipeline_layout_block(nir_block *block, void *void_state)
{
   struct apply_pipeline_layout_state *state = void_state;

   nir_foreach_instr_safe(block, instr) {
      switch (instr->type) {
      case nir_instr_type_intrinsic: {
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic == nir_intrinsic_vulkan_resource_index) {
            lower_res_index_intrinsic(intrin, state);
            state->progress = true;
         }
         break;
      }
      case nir_instr_type_tex:
         lower_tex(nir_instr_as_tex(instr), state);
         /* All texture instructions need lowering */
         state->progress = true;
         break;
      default:
         continue;
      }
   }

   return true;
}

bool
anv_nir_apply_pipeline_layout(nir_shader *shader,
                              const struct anv_pipeline_layout *layout)
{
   struct apply_pipeline_layout_state state = {
      .shader = shader,
      .stage = anv_vk_shader_stage_for_mesa_stage(shader->stage),
      .layout = layout,
   };

   nir_foreach_overload(shader, overload) {
      if (overload->impl) {
         nir_builder_init(&state.builder, overload->impl);
         nir_foreach_block(overload->impl, apply_pipeline_layout_block, &state);
         nir_metadata_preserve(overload->impl, nir_metadata_block_index |
                                               nir_metadata_dominance);
      }
   }

   return state.progress;
}
