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
#include "program/prog_parameter.h"
#include "nir/nir_builder.h"

struct apply_pipeline_layout_state {
   nir_shader *shader;
   nir_builder builder;

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

   gl_shader_stage stage = state->shader->stage;

   assert(binding < set_layout->binding_count);

   assert(set_layout->binding[binding].stage[stage].surface_index >= 0);

   uint32_t surface_index =
      state->layout->set[set].stage[stage].surface_start +
      set_layout->binding[binding].stage[stage].surface_index;

   assert(surface_index < state->layout->stage[stage].surface_count);

   return surface_index;
}

static uint32_t
get_sampler_index(unsigned set, unsigned binding, nir_texop tex_op,
                  struct apply_pipeline_layout_state *state)
{
   assert(set < state->layout->num_sets);
   struct anv_descriptor_set_layout *set_layout =
      state->layout->set[set].layout;

   assert(binding < set_layout->binding_count);

   gl_shader_stage stage = state->shader->stage;

   if (set_layout->binding[binding].stage[stage].sampler_index < 0) {
      assert(tex_op == nir_texop_txf);
      return 0;
   }

   uint32_t sampler_index =
      state->layout->set[set].stage[stage].sampler_start +
      set_layout->binding[binding].stage[stage].sampler_index;

   assert(sampler_index < state->layout->stage[stage].sampler_count);

   return sampler_index;
}

static uint32_t
get_image_index(unsigned set, unsigned binding,
                struct apply_pipeline_layout_state *state)
{
   assert(set < state->layout->num_sets);
   struct anv_descriptor_set_layout *set_layout =
      state->layout->set[set].layout;

   assert(binding < set_layout->binding_count);

   gl_shader_stage stage = state->shader->stage;

   assert(set_layout->binding[binding].stage[stage].image_index >= 0);

   uint32_t image_index =
      state->layout->set[set].stage[stage].image_start +
      set_layout->binding[binding].stage[stage].image_index;

   assert(image_index < state->layout->stage[stage].image_count);

   return image_index;
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
lower_tex_deref(nir_tex_instr *tex, nir_deref_var *deref,
                unsigned *const_index, nir_tex_src_type src_type,
                struct apply_pipeline_layout_state *state)
{
   if (deref->deref.child) {
      assert(deref->deref.child->deref_type == nir_deref_type_array);
      nir_deref_array *deref_array = nir_deref_as_array(deref->deref.child);

      *const_index += deref_array->base_offset;

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
         tex->src[tex->num_srcs].src_type = src_type;
         tex->num_srcs++;
         assert(deref_array->indirect.is_ssa);
         nir_instr_rewrite_src(&tex->instr, &tex->src[tex->num_srcs - 1].src,
                               deref_array->indirect);
      }
   }
}

static void
cleanup_tex_deref(nir_tex_instr *tex, nir_deref_var *deref)
{
   if (deref->deref.child == NULL)
      return;

   nir_deref_array *deref_array = nir_deref_as_array(deref->deref.child);

   if (deref_array->deref_array_type != nir_deref_array_type_indirect)
      return;

   nir_instr_rewrite_src(&tex->instr, &deref_array->indirect, NIR_SRC_INIT);
}

static void
lower_tex(nir_tex_instr *tex, struct apply_pipeline_layout_state *state)
{
   /* No one should have come by and lowered it already */
   assert(tex->sampler);

   nir_deref_var *tex_deref = tex->texture ? tex->texture : tex->sampler;
   tex->texture_index =
      get_surface_index(tex_deref->var->data.descriptor_set,
                        tex_deref->var->data.binding, state);
   lower_tex_deref(tex, tex_deref, &tex->texture_index,
                   nir_tex_src_texture_offset, state);

   tex->sampler_index =
      get_sampler_index(tex->sampler->var->data.descriptor_set,
                        tex->sampler->var->data.binding, tex->op, state);
   lower_tex_deref(tex, tex->sampler, &tex->sampler_index,
                   nir_tex_src_sampler_offset, state);

   /* The backend only ever uses this to mark used surfaces.  We don't care
    * about that little optimization so it just needs to be non-zero.
    */
   tex->texture_array_size = 1;

   if (tex->texture)
      cleanup_tex_deref(tex, tex->texture);
   cleanup_tex_deref(tex, tex->sampler);
   tex->texture = NULL;
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

static void
setup_vec4_uniform_value(const union gl_constant_value **params,
                         const union gl_constant_value *values,
                         unsigned n)
{
   static const gl_constant_value zero = { 0 };

   for (unsigned i = 0; i < n; ++i)
      params[i] = &values[i];

   for (unsigned i = n; i < 4; ++i)
      params[i] = &zero;
}

bool
anv_nir_apply_pipeline_layout(nir_shader *shader,
                              struct brw_stage_prog_data *prog_data,
                              const struct anv_pipeline_layout *layout)
{
   struct apply_pipeline_layout_state state = {
      .shader = shader,
      .layout = layout,
   };

   nir_foreach_function(shader, function) {
      if (function->impl) {
         nir_builder_init(&state.builder, function->impl);
         nir_foreach_block(function->impl, apply_pipeline_layout_block, &state);
         nir_metadata_preserve(function->impl, nir_metadata_block_index |
                                               nir_metadata_dominance);
      }
   }

   if (layout->stage[shader->stage].image_count > 0) {
      nir_foreach_variable(var, &shader->uniforms) {
         if (glsl_type_is_image(var->type) ||
             (glsl_type_is_array(var->type) &&
              glsl_type_is_image(glsl_get_array_element(var->type)))) {
            /* Images are represented as uniform push constants and the actual
             * information required for reading/writing to/from the image is
             * storred in the uniform.
             */
            unsigned image_index = get_image_index(var->data.descriptor_set,
                                                   var->data.binding, &state);

            var->data.driver_location = shader->num_uniforms +
                                        image_index * BRW_IMAGE_PARAM_SIZE * 4;
         }
      }

      struct anv_push_constants *null_data = NULL;
      const gl_constant_value **param = prog_data->param + shader->num_uniforms;
      const struct brw_image_param *image_param = null_data->images;
      for (uint32_t i = 0; i < layout->stage[shader->stage].image_count; i++) {
         setup_vec4_uniform_value(param + BRW_IMAGE_PARAM_SURFACE_IDX_OFFSET,
            (const union gl_constant_value *)&image_param->surface_idx, 1);
         setup_vec4_uniform_value(param + BRW_IMAGE_PARAM_OFFSET_OFFSET,
            (const union gl_constant_value *)image_param->offset, 2);
         setup_vec4_uniform_value(param + BRW_IMAGE_PARAM_SIZE_OFFSET,
            (const union gl_constant_value *)image_param->size, 3);
         setup_vec4_uniform_value(param + BRW_IMAGE_PARAM_STRIDE_OFFSET,
            (const union gl_constant_value *)image_param->stride, 4);
         setup_vec4_uniform_value(param + BRW_IMAGE_PARAM_TILING_OFFSET,
            (const union gl_constant_value *)image_param->tiling, 3);
         setup_vec4_uniform_value(param + BRW_IMAGE_PARAM_SWIZZLING_OFFSET,
            (const union gl_constant_value *)image_param->swizzling, 2);

         param += BRW_IMAGE_PARAM_SIZE;
         image_param ++;
      }

      shader->num_uniforms += layout->stage[shader->stage].image_count *
                              BRW_IMAGE_PARAM_SIZE * 4;
   }

   return state.progress;
}
