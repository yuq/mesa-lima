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

struct apply_dynamic_offsets_state {
   nir_shader *shader;
   nir_builder builder;

   VkShaderStage stage;
   struct anv_pipeline_layout *layout;

   uint32_t indices_start;
};

static bool
apply_dynamic_offsets_block(nir_block *block, void *void_state)
{
   struct apply_dynamic_offsets_state *state = void_state;
   struct anv_descriptor_set_layout *set_layout;
   const struct anv_descriptor_slot *slot;

   nir_foreach_instr_safe(block, instr) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

      bool has_indirect = false;
      uint32_t set, binding;
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_ubo_indirect:
         has_indirect = true;
         /* fallthrough */
      case nir_intrinsic_load_ubo: {
         set = intrin->const_index[0];

         nir_const_value *const_binding = nir_src_as_const_value(intrin->src[0]);
         if (const_binding) {
            binding = const_binding->u[0];
         } else {
            assert(0 && "need more info from the ir for this.");
         }
         break;
      }
      default:
         continue; /* the loop */
      }

      set_layout = state->layout->set[set].layout;
      slot = &set_layout->stage[state->stage].surface_start[binding];
      if (slot->dynamic_slot < 0)
         continue;

      uint32_t dynamic_index = state->layout->set[set].dynamic_offset_start +
                               slot->dynamic_slot;

      state->builder.cursor = nir_before_instr(&intrin->instr);

      nir_intrinsic_instr *offset_load =
         nir_intrinsic_instr_create(state->shader, nir_intrinsic_load_uniform);
      offset_load->num_components = 1;
      offset_load->const_index[0] = state->indices_start + dynamic_index;
      offset_load->const_index[1] = 0;
      nir_ssa_dest_init(&offset_load->instr, &offset_load->dest, 1, NULL);
      nir_builder_instr_insert(&state->builder, &offset_load->instr);

      nir_ssa_def *offset = &offset_load->dest.ssa;
      if (has_indirect) {
         assert(intrin->src[1].is_ssa);
         offset = nir_iadd(&state->builder, intrin->src[1].ssa, offset);
      }

      assert(intrin->dest.is_ssa);

      nir_intrinsic_instr *new_load =
         nir_intrinsic_instr_create(state->shader,
                                    nir_intrinsic_load_ubo_indirect);
      new_load->num_components = intrin->num_components;
      new_load->const_index[0] = intrin->const_index[0];
      new_load->const_index[1] = intrin->const_index[1];
      nir_src_copy(&new_load->src[0], &intrin->src[0], &new_load->instr);
      new_load->src[1] = nir_src_for_ssa(offset);
      nir_ssa_dest_init(&new_load->instr, &new_load->dest,
                        intrin->dest.ssa.num_components,
                        intrin->dest.ssa.name);
      nir_builder_instr_insert(&state->builder, &new_load->instr);

      nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                               nir_src_for_ssa(&new_load->dest.ssa),
                               state->shader);

      nir_instr_remove(&intrin->instr);
   }

   return true;
}

void
anv_nir_apply_dynamic_offsets(struct anv_pipeline *pipeline,
                              nir_shader *shader,
                              struct brw_stage_prog_data *prog_data)
{
   struct apply_dynamic_offsets_state state = {
      .shader = shader,
      .stage = anv_vk_shader_stage_for_mesa_stage(shader->stage),
      .layout = pipeline->layout,
      .indices_start = shader->num_uniforms,
   };

   if (!state.layout || !state.layout->stage[state.stage].has_dynamic_offsets)
      return;

   nir_foreach_overload(shader, overload) {
      if (overload->impl) {
         nir_builder_init(&state.builder, overload->impl);
         nir_foreach_block(overload->impl, apply_dynamic_offsets_block, &state);
         nir_metadata_preserve(overload->impl, nir_metadata_block_index |
                                               nir_metadata_dominance);
      }
   }

   struct anv_push_constants *null_data = NULL;
   for (unsigned i = 0; i < MAX_DYNAMIC_BUFFERS; i++)
      prog_data->param[i + shader->num_uniforms] =
         (const gl_constant_value *)&null_data->dynamic_offsets[i];

   shader->num_uniforms += MAX_DYNAMIC_BUFFERS;
}
