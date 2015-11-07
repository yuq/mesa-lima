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

   nir_builder *b = &state->builder;

   nir_foreach_instr_safe(block, instr) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

      unsigned block_idx_src;
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_ubo:
      case nir_intrinsic_load_ubo_indirect:
      case nir_intrinsic_load_ssbo:
      case nir_intrinsic_load_ssbo_indirect:
         block_idx_src = 0;
         break;
      case nir_intrinsic_store_ssbo:
      case nir_intrinsic_store_ssbo_indirect:
         block_idx_src = 1;
         break;
      default:
         continue; /* the loop */
      }

      nir_instr *res_instr = intrin->src[block_idx_src].ssa->parent_instr;
      assert(res_instr->type == nir_instr_type_intrinsic);
      nir_intrinsic_instr *res_intrin = nir_instr_as_intrinsic(res_instr);
      assert(res_intrin->intrinsic == nir_intrinsic_vulkan_resource_index);

      unsigned set = res_intrin->const_index[0];
      unsigned binding = res_intrin->const_index[1];

      set_layout = state->layout->set[set].layout;
      if (set_layout->binding[binding].dynamic_offset_index < 0)
         continue;

      b->cursor = nir_before_instr(&intrin->instr);

      int indirect_src;
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_ubo_indirect:
      case nir_intrinsic_load_ssbo_indirect:
         indirect_src = 1;
         break;
      case nir_intrinsic_store_ssbo_indirect:
         indirect_src = 2;
         break;
      default:
         indirect_src = -1;
         break;
      }

      /* First, we need to generate the uniform load for the buffer offset */
      uint32_t index = state->layout->set[set].dynamic_offset_start +
                       set_layout->binding[binding].dynamic_offset_index;

      nir_const_value *const_arr_idx =
         nir_src_as_const_value(res_intrin->src[0]);

      nir_intrinsic_op offset_load_op;
      if (const_arr_idx)
         offset_load_op = nir_intrinsic_load_uniform;
      else
         offset_load_op = nir_intrinsic_load_uniform_indirect;

      nir_intrinsic_instr *offset_load =
         nir_intrinsic_instr_create(state->shader, offset_load_op);
      offset_load->num_components = 2;
      offset_load->const_index[0] = state->indices_start + index * 2;

      if (const_arr_idx) {
         offset_load->const_index[1] = const_arr_idx->u[0] * 2;
      } else {
         offset_load->const_index[1] = 0;
         offset_load->src[0] = nir_src_for_ssa(
            nir_imul(b, nir_ssa_for_src(b, res_intrin->src[0], 1),
                     nir_imm_int(b, 2)));
      }

      nir_ssa_dest_init(&offset_load->instr, &offset_load->dest, 2, NULL);
      nir_builder_instr_insert(b, &offset_load->instr);

      /* We calculate the full offset and don't bother with the base
       * offset.  We need the full offset for the predicate anyway.
       */
      nir_ssa_def *rel_offset = nir_imm_int(b, intrin->const_index[0]);
      if (indirect_src >= 0) {
         assert(intrin->src[indirect_src].is_ssa);
         rel_offset = nir_iadd(b, intrin->src[indirect_src].ssa, rel_offset);
      }
      nir_ssa_def *global_offset = nir_iadd(b, rel_offset,
                                            &offset_load->dest.ssa);

      /* Now we replace the load/store intrinsic */

      nir_intrinsic_op indirect_op;
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_ubo:
         indirect_op = nir_intrinsic_load_ubo_indirect;
         break;
      case nir_intrinsic_load_ssbo:
         indirect_op = nir_intrinsic_load_ssbo_indirect;
         break;
      case nir_intrinsic_store_ssbo:
         indirect_op = nir_intrinsic_store_ssbo_indirect;
         break;
      default:
         unreachable("Invalid direct load/store intrinsic");
      }

      nir_intrinsic_instr *copy =
         nir_intrinsic_instr_create(state->shader, indirect_op);
      copy->num_components = intrin->num_components;

      /* The indirect is always the last source */
      indirect_src = nir_intrinsic_infos[indirect_op].num_srcs - 1;

      for (unsigned i = 0; i < (unsigned)indirect_src; i++)
         nir_src_copy(&copy->src[i], &intrin->src[i], &copy->instr);

      copy->src[indirect_src] = nir_src_for_ssa(global_offset);
      nir_ssa_dest_init(&copy->instr, &copy->dest,
                        intrin->dest.ssa.num_components,
                        intrin->dest.ssa.name);

      /* In order to avoid out-of-bounds access, we predicate */
      nir_ssa_def *pred = nir_fge(b, nir_channel(b, &offset_load->dest.ssa, 1),
                                  rel_offset);
      nir_if *if_stmt = nir_if_create(b->shader);
      if_stmt->condition = nir_src_for_ssa(pred);
      nir_cf_node_insert(b->cursor, &if_stmt->cf_node);

      nir_instr_insert_after_cf_list(&if_stmt->then_list, &copy->instr);

      if (indirect_op != nir_intrinsic_store_ssbo) {
         /* It's a load, we need a phi node */
         nir_phi_instr *phi = nir_phi_instr_create(b->shader);
         nir_ssa_dest_init(&phi->instr, &phi->dest,
                           intrin->num_components, NULL);

         nir_phi_src *src1 = ralloc(phi, nir_phi_src);
         struct exec_node *tnode = exec_list_get_tail(&if_stmt->then_list);
         src1->pred = exec_node_data(nir_block, tnode, cf_node.node);
         src1->src = nir_src_for_ssa(&copy->dest.ssa);
         exec_list_push_tail(&phi->srcs, &src1->node);

         b->cursor = nir_after_cf_list(&if_stmt->else_list);
         nir_ssa_def *zero = nir_build_imm(b, intrin->num_components,
            (nir_const_value) { .u = { 0, 0, 0, 0 } });

         nir_phi_src *src2 = ralloc(phi, nir_phi_src);
         struct exec_node *enode = exec_list_get_tail(&if_stmt->else_list);
         src2->pred = exec_node_data(nir_block, enode, cf_node.node);
         src2->src = nir_src_for_ssa(zero);
         exec_list_push_tail(&phi->srcs, &src2->node);

         nir_instr_insert_after_cf(&if_stmt->cf_node, &phi->instr);

         assert(intrin->dest.is_ssa);
         nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                                  nir_src_for_ssa(&phi->dest.ssa));
      }

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
   for (unsigned i = 0; i < MAX_DYNAMIC_BUFFERS; i++) {
      prog_data->param[i * 2 + shader->num_uniforms] =
         (const gl_constant_value *)&null_data->dynamic[i].offset;
      prog_data->param[i * 2 + 1 + shader->num_uniforms] =
         (const gl_constant_value *)&null_data->dynamic[i].range;
   }

   shader->num_uniforms += MAX_DYNAMIC_BUFFERS * 2;
}
