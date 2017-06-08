/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "si_shader.h"
#include "si_shader_internal.h"

#include "ac_nir_to_llvm.h"

#include "tgsi/tgsi_from_mesa.h"

#include "compiler/nir/nir.h"
#include "compiler/nir_types.h"


static void scan_instruction(struct tgsi_shader_info *info,
			     nir_instr *instr)
{
	if (instr->type == nir_instr_type_alu) {
		nir_alu_instr *alu = nir_instr_as_alu(instr);

		switch (alu->op) {
		case nir_op_fddx:
		case nir_op_fddy:
		case nir_op_fddx_fine:
		case nir_op_fddy_fine:
		case nir_op_fddx_coarse:
		case nir_op_fddy_coarse:
			info->uses_derivatives = true;
			break;
		default:
			break;
		}
	} else if (instr->type == nir_instr_type_tex) {
		nir_tex_instr *tex = nir_instr_as_tex(instr);

		switch (tex->op) {
		case nir_texop_tex:
		case nir_texop_txb:
		case nir_texop_lod:
			info->uses_derivatives = true;
			break;
		default:
			break;
		}
	} else if (instr->type == nir_instr_type_intrinsic) {
		nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

		switch (intr->intrinsic) {
		case nir_intrinsic_load_front_face:
			info->uses_frontface = 1;
			break;
		case nir_intrinsic_load_instance_id:
			info->uses_instanceid = 1;
			break;
		case nir_intrinsic_load_vertex_id:
			info->uses_vertexid = 1;
			break;
		case nir_intrinsic_load_vertex_id_zero_base:
			info->uses_vertexid_nobase = 1;
			break;
		case nir_intrinsic_load_base_vertex:
			info->uses_basevertex = 1;
			break;
		case nir_intrinsic_load_primitive_id:
			info->uses_primid = 1;
			break;
		case nir_intrinsic_image_store:
		case nir_intrinsic_image_atomic_add:
		case nir_intrinsic_image_atomic_min:
		case nir_intrinsic_image_atomic_max:
		case nir_intrinsic_image_atomic_and:
		case nir_intrinsic_image_atomic_or:
		case nir_intrinsic_image_atomic_xor:
		case nir_intrinsic_image_atomic_exchange:
		case nir_intrinsic_image_atomic_comp_swap:
		case nir_intrinsic_store_ssbo:
		case nir_intrinsic_ssbo_atomic_add:
		case nir_intrinsic_ssbo_atomic_imin:
		case nir_intrinsic_ssbo_atomic_umin:
		case nir_intrinsic_ssbo_atomic_imax:
		case nir_intrinsic_ssbo_atomic_umax:
		case nir_intrinsic_ssbo_atomic_and:
		case nir_intrinsic_ssbo_atomic_or:
		case nir_intrinsic_ssbo_atomic_xor:
		case nir_intrinsic_ssbo_atomic_exchange:
		case nir_intrinsic_ssbo_atomic_comp_swap:
			info->writes_memory = true;
			break;
		default:
			break;
		}
	}
}

void si_nir_scan_shader(const struct nir_shader *nir,
			struct tgsi_shader_info *info)
{
	nir_function *func;
	unsigned i;

	assert(nir->stage == MESA_SHADER_VERTEX ||
	       nir->stage == MESA_SHADER_FRAGMENT);

	info->processor = pipe_shader_type_from_mesa(nir->stage);
	info->num_tokens = 2; /* indicate that the shader is non-empty */
	info->num_instructions = 2;

	info->num_inputs = nir->num_inputs;
	info->num_outputs = nir->num_outputs;

	i = 0;
	nir_foreach_variable(variable, &nir->inputs) {
		unsigned semantic_name, semantic_index;
		unsigned attrib_count = glsl_count_attribute_slots(variable->type,
								   nir->stage == MESA_SHADER_VERTEX);

		assert(attrib_count == 1 && "not implemented");

		/* Vertex shader inputs don't have semantics. The state
		 * tracker has already mapped them to attributes via
		 * variable->data.driver_location.
		 */
		if (nir->stage == MESA_SHADER_VERTEX)
			continue;

		/* Fragment shader position is a system value. */
		if (nir->stage == MESA_SHADER_FRAGMENT &&
		    variable->data.location == VARYING_SLOT_POS) {
			if (variable->data.pixel_center_integer)
				info->properties[TGSI_PROPERTY_FS_COORD_PIXEL_CENTER] =
					TGSI_FS_COORD_PIXEL_CENTER_INTEGER;
			continue;
		}

		tgsi_get_gl_varying_semantic(variable->data.location, true,
					     &semantic_name, &semantic_index);

		info->input_semantic_name[i] = semantic_name;
		info->input_semantic_index[i] = semantic_index;

		if (variable->data.sample)
			info->input_interpolate_loc[i] = TGSI_INTERPOLATE_LOC_SAMPLE;
		else if (variable->data.centroid)
			info->input_interpolate_loc[i] = TGSI_INTERPOLATE_LOC_CENTROID;
		else
			info->input_interpolate_loc[i] = TGSI_INTERPOLATE_LOC_CENTER;

		enum glsl_base_type base_type =
			glsl_get_base_type(glsl_without_array(variable->type));

		switch (variable->data.interpolation) {
		case INTERP_MODE_NONE:
			if (glsl_base_type_is_integer(base_type)) {
				info->input_interpolate[i] = TGSI_INTERPOLATE_CONSTANT;
				break;
			}

			if (semantic_name == TGSI_SEMANTIC_COLOR) {
				info->input_interpolate[i] = TGSI_INTERPOLATE_COLOR;
				goto persp_locations;
			}
			/* fall-through */
		case INTERP_MODE_SMOOTH:
			assert(!glsl_base_type_is_integer(base_type));

			info->input_interpolate[i] = TGSI_INTERPOLATE_PERSPECTIVE;

		persp_locations:
			if (variable->data.sample)
				info->uses_persp_sample = true;
			else if (variable->data.centroid)
				info->uses_persp_centroid = true;
			else
				info->uses_persp_center = true;
			break;

		case INTERP_MODE_NOPERSPECTIVE:
			assert(!glsl_base_type_is_integer(base_type));

			info->input_interpolate[i] = TGSI_INTERPOLATE_LINEAR;

			if (variable->data.sample)
				info->uses_linear_sample = true;
			else if (variable->data.centroid)
				info->uses_linear_centroid = true;
			else
				info->uses_linear_center = true;
			break;

		case INTERP_MODE_FLAT:
			info->input_interpolate[i] = TGSI_INTERPOLATE_CONSTANT;
			break;
		}

		/* TODO make this more precise */
		if (variable->data.location == VARYING_SLOT_COL0)
			info->colors_read |= 0x0f;
		else if (variable->data.location == VARYING_SLOT_COL1)
			info->colors_read |= 0xf0;

		i++;
	}

	i = 0;
	nir_foreach_variable(variable, &nir->outputs) {
		unsigned semantic_name, semantic_index;

		if (nir->stage == MESA_SHADER_FRAGMENT) {
			tgsi_get_gl_frag_result_semantic(variable->data.location,
				&semantic_name, &semantic_index);
		} else {
			tgsi_get_gl_varying_semantic(variable->data.location, true,
						     &semantic_name, &semantic_index);
		}

		info->output_semantic_name[i] = semantic_name;
		info->output_semantic_index[i] = semantic_index;
		info->output_usagemask[i] = TGSI_WRITEMASK_XYZW;

		switch (semantic_name) {
		case TGSI_SEMANTIC_PRIMID:
			info->writes_primid = true;
			break;
		case TGSI_SEMANTIC_VIEWPORT_INDEX:
			info->writes_viewport_index = true;
			break;
		case TGSI_SEMANTIC_LAYER:
			info->writes_layer = true;
			break;
		case TGSI_SEMANTIC_PSIZE:
			info->writes_psize = true;
			break;
		case TGSI_SEMANTIC_CLIPVERTEX:
			info->writes_clipvertex = true;
			break;
		case TGSI_SEMANTIC_COLOR:
			info->colors_written |= 1 << semantic_index;
			break;
		case TGSI_SEMANTIC_STENCIL:
			info->writes_stencil = true;
			break;
		case TGSI_SEMANTIC_SAMPLEMASK:
			info->writes_samplemask = true;
			break;
		case TGSI_SEMANTIC_EDGEFLAG:
			info->writes_edgeflag = true;
			break;
		case TGSI_SEMANTIC_POSITION:
			if (info->processor == PIPE_SHADER_FRAGMENT)
				info->writes_z = true;
			else
				info->writes_position = true;
			break;
		}

		i++;
	}

	nir_foreach_variable(variable, &nir->uniforms) {
		const struct glsl_type *type = variable->type;
		enum glsl_base_type base_type =
			glsl_get_base_type(glsl_without_array(type));
		unsigned aoa_size = MAX2(1, glsl_get_aoa_size(type));

		/* We rely on the fact that nir_lower_samplers_as_deref has
		 * eliminated struct dereferences.
		 */
		if (base_type == GLSL_TYPE_SAMPLER)
			info->samplers_declared |=
				u_bit_consecutive(variable->data.binding, aoa_size);
		else if (base_type == GLSL_TYPE_IMAGE)
			info->images_declared |=
				u_bit_consecutive(variable->data.binding, aoa_size);
	}

	info->num_written_clipdistance = nir->info.clip_distance_array_size;
	info->num_written_culldistance = nir->info.cull_distance_array_size;
	info->clipdist_writemask = u_bit_consecutive(0, info->num_written_clipdistance);
	info->culldist_writemask = u_bit_consecutive(info->num_written_clipdistance,
						     info->num_written_culldistance);

	if (info->processor == PIPE_SHADER_FRAGMENT)
		info->uses_kill = nir->info.fs.uses_discard;

	/* TODO make this more accurate */
	info->const_buffers_declared = u_bit_consecutive(0, SI_NUM_CONST_BUFFERS);
	info->shader_buffers_declared = u_bit_consecutive(0, SI_NUM_SHADER_BUFFERS);

	func = (struct nir_function *)exec_list_get_head_const(&nir->functions);
	nir_foreach_block(block, func->impl) {
		nir_foreach_instr(instr, block)
			scan_instruction(info, instr);
	}
}

static void declare_nir_input_vs(struct si_shader_context *ctx,
				 struct nir_variable *variable, unsigned rel,
				 LLVMValueRef out[4])
{
	si_llvm_load_input_vs(ctx, variable->data.driver_location / 4 + rel, out);
}

static void declare_nir_input_fs(struct si_shader_context *ctx,
				 struct nir_variable *variable, unsigned rel,
				 unsigned *fs_attr_idx,
				 LLVMValueRef out[4])
{
	unsigned slot = variable->data.location + rel;

	assert(variable->data.location >= VARYING_SLOT_VAR0 || rel == 0);

	if (slot == VARYING_SLOT_POS) {
		out[0] = LLVMGetParam(ctx->main_fn, SI_PARAM_POS_X_FLOAT);
		out[1] = LLVMGetParam(ctx->main_fn, SI_PARAM_POS_Y_FLOAT);
		out[2] = LLVMGetParam(ctx->main_fn, SI_PARAM_POS_Z_FLOAT);
		out[3] = ac_build_fdiv(&ctx->ac, ctx->ac.f32_1,
				LLVMGetParam(ctx->main_fn, SI_PARAM_POS_W_FLOAT));
		return;
	}

	si_llvm_load_input_fs(ctx, *fs_attr_idx, out);
	(*fs_attr_idx)++;
}

bool si_nir_build_llvm(struct si_shader_context *ctx, struct nir_shader *nir)
{
	unsigned fs_attr_idx = 0;
	nir_foreach_variable(variable, &nir->inputs) {
		unsigned attrib_count = glsl_count_attribute_slots(variable->type,
								   nir->stage == MESA_SHADER_VERTEX);
		unsigned input_idx = variable->data.driver_location;

		for (unsigned i = 0; i < attrib_count; ++i) {
			LLVMValueRef data[4];

			if (nir->stage == MESA_SHADER_VERTEX)
				declare_nir_input_vs(ctx, variable, i, data);
			else if (nir->stage == MESA_SHADER_FRAGMENT)
				declare_nir_input_fs(ctx, variable, i, &fs_attr_idx, data);

			for (unsigned chan = 0; chan < 4; chan++) {
				ctx->inputs[input_idx + chan] =
					LLVMBuildBitCast(ctx->ac.builder, data[chan], ctx->ac.i32, "");
			}
		}
	}

	ctx->abi.inputs = &ctx->inputs[0];

	ac_nir_translate(&ctx->ac, &ctx->abi, nir, NULL);

	return true;
}
