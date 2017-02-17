/*
 * Copyright 2016 Advanced Micro Devices, Inc.
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

#ifndef SI_SHADER_PRIVATE_H
#define SI_SHADER_PRIVATE_H

#include "si_shader.h"
#include "gallivm/lp_bld_init.h"
#include "gallivm/lp_bld_tgsi.h"
#include "tgsi/tgsi_parse.h"
#include "ac_llvm_util.h"
#include "ac_llvm_build.h"

#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>

struct pipe_debug_callback;
struct ac_shader_binary;

#define RADEON_LLVM_MAX_INPUT_SLOTS 32
#define RADEON_LLVM_MAX_INPUTS 32 * 4
#define RADEON_LLVM_MAX_OUTPUTS 32 * 4

#define RADEON_LLVM_INITIAL_CF_DEPTH 4

#define RADEON_LLVM_MAX_SYSTEM_VALUES 11
#define RADEON_LLVM_MAX_ADDRS 16

struct si_llvm_flow;

struct si_shader_context {
	struct lp_build_tgsi_context bld_base;
	struct gallivm_state gallivm;
	struct ac_llvm_context ac;
	struct si_shader *shader;
	struct si_screen *screen;

	unsigned type; /* PIPE_SHADER_* specifies the type of shader. */

	/* Whether the prolog will be compiled separately. */
	bool separate_prolog;

	/** This function is responsible for initilizing the inputs array and will be
	  * called once for each input declared in the TGSI shader.
	  */
	void (*load_input)(struct si_shader_context *,
			   unsigned input_index,
			   const struct tgsi_full_declaration *decl,
			   LLVMValueRef out[4]);

	void (*load_system_value)(struct si_shader_context *,
				  unsigned index,
				  const struct tgsi_full_declaration *decl);

	void (*declare_memory_region)(struct si_shader_context *,
				      const struct tgsi_full_declaration *decl);

	/** This array contains the input values for the shader.  Typically these
	  * values will be in the form of a target intrinsic that will inform the
	  * backend how to load the actual inputs to the shader.
	  */
	struct tgsi_full_declaration input_decls[RADEON_LLVM_MAX_INPUT_SLOTS];
	LLVMValueRef inputs[RADEON_LLVM_MAX_INPUTS];
	LLVMValueRef outputs[RADEON_LLVM_MAX_OUTPUTS][TGSI_NUM_CHANNELS];
	LLVMValueRef addrs[RADEON_LLVM_MAX_ADDRS][TGSI_NUM_CHANNELS];

	/** This pointer is used to contain the temporary values.
	  * The amount of temporary used in tgsi can't be bound to a max value and
	  * thus we must allocate this array at runtime.
	  */
	LLVMValueRef *temps;
	unsigned temps_count;
	LLVMValueRef system_values[RADEON_LLVM_MAX_SYSTEM_VALUES];

	LLVMValueRef *imms;
	unsigned imms_num;

	struct si_llvm_flow *flow;
	unsigned flow_depth;
	unsigned flow_depth_max;

	struct tgsi_array_info *temp_arrays;
	LLVMValueRef *temp_array_allocas;

	LLVMValueRef undef_alloca;

	LLVMValueRef main_fn;
	LLVMTypeRef return_type;

	/* Parameter indices for LLVMGetParam. */
	int param_rw_buffers;
	int param_const_buffers;
	int param_samplers;
	int param_images;
	int param_shader_buffers;
	/* API VS */
	int param_vertex_buffers;
	int param_base_vertex;
	int param_start_instance;
	int param_draw_id;
	int param_vertex_id;
	int param_rel_auto_id;
	int param_vs_prim_id;
	int param_instance_id;
	int param_vertex_index0;
	/* VS states and layout of LS outputs / TCS inputs at the end
	 *   [0] = clamp vertex color
	 *   [1] = indexed
	 *   [8:20] = stride between patches in DW = num_inputs * num_vertices * 4
	 *            max = 32*32*4
	 *   [24:31] = stride between vertices in DW = num_inputs * 4
	 *             max = 32*4
	 */
	int param_vs_state_bits;
	/* HW VS */
	int param_streamout_config;
	int param_streamout_write_index;
	int param_streamout_offset[4];

	/* API TCS & TES */
	/* Layout of TCS outputs in the offchip buffer
	 *   [0:8] = the number of patches per threadgroup.
	 *   [9:15] = the number of output vertices per patch.
	 *   [16:31] = the offset of per patch attributes in the buffer in bytes. */
	int param_tcs_offchip_layout;

	/* API TCS */
	/* Offsets where TCS outputs and TCS patch outputs live in LDS:
	 *   [0:15] = TCS output patch0 offset / 16, max = NUM_PATCHES * 32 * 32
	 *   [16:31] = TCS output patch0 offset for per-patch / 16
	 *             max = NUM_PATCHES*32*32* + 32*32
	 */
	int param_tcs_out_lds_offsets;
	/* Layout of TCS outputs / TES inputs:
	 *   [0:12] = stride between output patches in DW, num_outputs * num_vertices * 4
	 *            max = 32*32*4
	 *   [13:20] = stride between output vertices in DW = num_inputs * 4
	 *             max = 32*4
	 *   [26:31] = gl_PatchVerticesIn, max = 32
	 */
	int param_tcs_out_lds_layout;
	int param_tcs_offchip_offset;
	int param_tcs_factor_offset;
	int param_tcs_patch_id;
	int param_tcs_rel_ids;

	/* API TES */
	int param_tes_u;
	int param_tes_v;
	int param_tes_rel_patch_id;
	int param_tes_patch_id;
	/* HW ES */
	int param_es2gs_offset;
	/* API GS */
	int param_gs2vs_offset;
	int param_gs_wave_id;
	int param_gs_vtx0_offset;
	int param_gs_vtx1_offset;
	int param_gs_prim_id;
	int param_gs_vtx2_offset;
	int param_gs_vtx3_offset;
	int param_gs_vtx4_offset;
	int param_gs_vtx5_offset;
	int param_gs_instance_id;

	LLVMTargetMachineRef tm;

	unsigned range_md_kind;
	unsigned fpmath_md_kind;
	LLVMValueRef fpmath_md_2p5_ulp;

	/* Preloaded descriptors. */
	LLVMValueRef esgs_ring;
	LLVMValueRef gsvs_ring[4];

	LLVMValueRef lds;
	LLVMValueRef gs_next_vertex[4];
	LLVMValueRef return_value;

	LLVMTypeRef voidt;
	LLVMTypeRef i1;
	LLVMTypeRef i8;
	LLVMTypeRef i32;
	LLVMTypeRef i64;
	LLVMTypeRef i128;
	LLVMTypeRef f32;
	LLVMTypeRef v16i8;
	LLVMTypeRef v2i32;
	LLVMTypeRef v4i32;
	LLVMTypeRef v4f32;
	LLVMTypeRef v8i32;

	LLVMValueRef i32_0;
	LLVMValueRef i32_1;

	LLVMValueRef shared_memory;
};

static inline struct si_shader_context *
si_shader_context(struct lp_build_tgsi_context *bld_base)
{
	return (struct si_shader_context*)bld_base;
}

void si_llvm_add_attribute(LLVMValueRef F, const char *name, int value);
void si_llvm_shader_type(LLVMValueRef F, unsigned type);

LLVMTargetRef si_llvm_get_amdgpu_target(const char *triple);

unsigned si_llvm_compile(LLVMModuleRef M, struct ac_shader_binary *binary,
			 LLVMTargetMachineRef tm,
			 struct pipe_debug_callback *debug);

LLVMTypeRef tgsi2llvmtype(struct lp_build_tgsi_context *bld_base,
			  enum tgsi_opcode_type type);

LLVMValueRef bitcast(struct lp_build_tgsi_context *bld_base,
		     enum tgsi_opcode_type type, LLVMValueRef value);

LLVMValueRef si_llvm_bound_index(struct si_shader_context *ctx,
				 LLVMValueRef index,
				 unsigned num);

void si_llvm_context_init(struct si_shader_context *ctx,
			  struct si_screen *sscreen,
			  LLVMTargetMachineRef tm);
void si_llvm_context_set_tgsi(struct si_shader_context *ctx,
			      struct si_shader *shader);

void si_llvm_create_func(struct si_shader_context *ctx,
			 const char *name,
			 LLVMTypeRef *return_types, unsigned num_return_elems,
			 LLVMTypeRef *ParamTypes, unsigned ParamCount);

void si_llvm_dispose(struct si_shader_context *ctx);

void si_llvm_finalize_module(struct si_shader_context *ctx,
			     bool run_verifier);

LLVMValueRef si_llvm_emit_fetch_64bit(struct lp_build_tgsi_context *bld_base,
				      enum tgsi_opcode_type type,
				      LLVMValueRef ptr,
				      LLVMValueRef ptr2);

LLVMValueRef si_llvm_emit_fetch(struct lp_build_tgsi_context *bld_base,
				const struct tgsi_full_src_register *reg,
				enum tgsi_opcode_type type,
				unsigned swizzle);

void si_llvm_emit_store(struct lp_build_tgsi_context *bld_base,
			const struct tgsi_full_instruction *inst,
			const struct tgsi_opcode_info *info,
			LLVMValueRef dst[4]);

void si_shader_context_init_alu(struct lp_build_tgsi_context *bld_base);

#endif
