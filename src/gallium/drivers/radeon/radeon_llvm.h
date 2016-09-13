/*
 * Copyright 2011 Advanced Micro Devices, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors: Tom Stellard <thomas.stellard@amd.com>
 *
 */

#ifndef RADEON_LLVM_H
#define RADEON_LLVM_H

#include <llvm-c/Core.h>
#include "gallivm/lp_bld_init.h"
#include "gallivm/lp_bld_tgsi.h"
#include "tgsi/tgsi_parse.h"

#define RADEON_LLVM_MAX_INPUT_SLOTS 32
#define RADEON_LLVM_MAX_INPUTS 32 * 4
#define RADEON_LLVM_MAX_OUTPUTS 32 * 4

#define RADEON_LLVM_INITIAL_CF_DEPTH 4

#define RADEON_LLVM_MAX_SYSTEM_VALUES 4

struct radeon_llvm_branch {
	LLVMBasicBlockRef endif_block;
	LLVMBasicBlockRef if_block;
	LLVMBasicBlockRef else_block;
	unsigned has_else;
};

struct radeon_llvm_loop {
	LLVMBasicBlockRef loop_block;
	LLVMBasicBlockRef endloop_block;
};

struct radeon_llvm_context {
	struct lp_build_tgsi_soa_context soa;

	/*=== Front end configuration ===*/

	/* Instructions that are not described by any of the TGSI opcodes. */

	/** This function is responsible for initilizing the inputs array and will be
	  * called once for each input declared in the TGSI shader.
	  */
	void (*load_input)(struct radeon_llvm_context *,
			   unsigned input_index,
			   const struct tgsi_full_declaration *decl,
			   LLVMValueRef out[4]);

	void (*load_system_value)(struct radeon_llvm_context *,
				  unsigned index,
				  const struct tgsi_full_declaration *decl);

	void (*declare_memory_region)(struct radeon_llvm_context *,
				      const struct tgsi_full_declaration *decl);

	/** This array contains the input values for the shader.  Typically these
	  * values will be in the form of a target intrinsic that will inform the
	  * backend how to load the actual inputs to the shader. 
	  */
	struct tgsi_full_declaration input_decls[RADEON_LLVM_MAX_INPUT_SLOTS];
	LLVMValueRef inputs[RADEON_LLVM_MAX_INPUTS];
	LLVMValueRef outputs[RADEON_LLVM_MAX_OUTPUTS][TGSI_NUM_CHANNELS];

	/** This pointer is used to contain the temporary values.
	  * The amount of temporary used in tgsi can't be bound to a max value and
	  * thus we must allocate this array at runtime.
	  */
	LLVMValueRef *temps;
	unsigned temps_count;
	LLVMValueRef system_values[RADEON_LLVM_MAX_SYSTEM_VALUES];

	/*=== Private Members ===*/

	struct radeon_llvm_branch *branch;
	struct radeon_llvm_loop *loop;

	unsigned branch_depth;
	unsigned branch_depth_max;
	unsigned loop_depth;
	unsigned loop_depth_max;

	struct tgsi_array_info *temp_arrays;
	LLVMValueRef *temp_array_allocas;

	LLVMValueRef undef_alloca;

	LLVMValueRef main_fn;
	LLVMTypeRef return_type;

	unsigned fpmath_md_kind;
	LLVMValueRef fpmath_md_2p5_ulp;

	struct gallivm_state gallivm;
};

LLVMTypeRef tgsi2llvmtype(struct lp_build_tgsi_context *bld_base,
			  enum tgsi_opcode_type type);

LLVMValueRef bitcast(struct lp_build_tgsi_context *bld_base,
		     enum tgsi_opcode_type type, LLVMValueRef value);

LLVMValueRef radeon_llvm_bound_index(struct radeon_llvm_context *ctx,
				     LLVMValueRef index,
				     unsigned num);

void radeon_llvm_emit_prepare_cube_coords(struct lp_build_tgsi_context *bld_base,
					  struct lp_build_emit_data *emit_data,
					  LLVMValueRef *coords_arg,
					  LLVMValueRef *derivs_arg);

void radeon_llvm_context_init(struct radeon_llvm_context *ctx,
                              const char *triple,
			      const struct tgsi_shader_info *info,
			      const struct tgsi_token *tokens);

void radeon_llvm_create_func(struct radeon_llvm_context *ctx,
			     LLVMTypeRef *return_types, unsigned num_return_elems,
			     LLVMTypeRef *ParamTypes, unsigned ParamCount);

void radeon_llvm_dispose(struct radeon_llvm_context *ctx);

unsigned radeon_llvm_reg_index_soa(unsigned index, unsigned chan);

void radeon_llvm_finalize_module(struct radeon_llvm_context *ctx);

void build_tgsi_intrinsic_nomem(const struct lp_build_tgsi_action *action,
				struct lp_build_tgsi_context *bld_base,
				struct lp_build_emit_data *emit_data);

LLVMValueRef radeon_llvm_emit_fetch_64bit(struct lp_build_tgsi_context *bld_base,
					  enum tgsi_opcode_type type,
					  LLVMValueRef ptr,
					  LLVMValueRef ptr2);

LLVMValueRef radeon_llvm_saturate(struct lp_build_tgsi_context *bld_base,
                                  LLVMValueRef value);

LLVMValueRef radeon_llvm_emit_fetch(struct lp_build_tgsi_context *bld_base,
				    const struct tgsi_full_src_register *reg,
				    enum tgsi_opcode_type type,
				    unsigned swizzle);

void radeon_llvm_emit_store(struct lp_build_tgsi_context *bld_base,
			    const struct tgsi_full_instruction *inst,
			    const struct tgsi_opcode_info *info,
			    LLVMValueRef dst[4]);

static inline struct radeon_llvm_context *
radeon_llvm_context(struct lp_build_tgsi_context *bld_base)
{
	return (struct radeon_llvm_context*)bld_base;
}

#endif /* RADEON_LLVM_H */
