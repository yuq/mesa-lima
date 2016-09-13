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
#include "radeon_llvm.h"

#include "gallivm/lp_bld_const.h"
#include "gallivm/lp_bld_gather.h"
#include "gallivm/lp_bld_flow.h"
#include "gallivm/lp_bld_init.h"
#include "gallivm/lp_bld_intr.h"
#include "gallivm/lp_bld_misc.h"
#include "gallivm/lp_bld_swizzle.h"
#include "tgsi/tgsi_info.h"
#include "tgsi/tgsi_parse.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_debug.h"

#include <stdio.h>
#include <llvm-c/Core.h>
#include <llvm-c/Transforms/Scalar.h>

LLVMTypeRef tgsi2llvmtype(struct lp_build_tgsi_context *bld_base,
			  enum tgsi_opcode_type type)
{
	LLVMContextRef ctx = bld_base->base.gallivm->context;

	switch (type) {
	case TGSI_TYPE_UNSIGNED:
	case TGSI_TYPE_SIGNED:
		return LLVMInt32TypeInContext(ctx);
	case TGSI_TYPE_DOUBLE:
		return LLVMDoubleTypeInContext(ctx);
	case TGSI_TYPE_UNTYPED:
	case TGSI_TYPE_FLOAT:
		return LLVMFloatTypeInContext(ctx);
	default: break;
	}
	return 0;
}

LLVMValueRef bitcast(struct lp_build_tgsi_context *bld_base,
		     enum tgsi_opcode_type type, LLVMValueRef value)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMTypeRef dst_type = tgsi2llvmtype(bld_base, type);

	if (dst_type)
		return LLVMBuildBitCast(builder, value, dst_type, "");
	else
		return value;
}

/**
 * Return a value that is equal to the given i32 \p index if it lies in [0,num)
 * or an undefined value in the same interval otherwise.
 */
LLVMValueRef radeon_llvm_bound_index(struct radeon_llvm_context *ctx,
				     LLVMValueRef index,
				     unsigned num)
{
	struct gallivm_state *gallivm = &ctx->gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef c_max = lp_build_const_int32(gallivm, num - 1);
	LLVMValueRef cc;

	if (util_is_power_of_two(num)) {
		index = LLVMBuildAnd(builder, index, c_max, "");
	} else {
		/* In theory, this MAX pattern should result in code that is
		 * as good as the bit-wise AND above.
		 *
		 * In practice, LLVM generates worse code (at the time of
		 * writing), because its value tracking is not strong enough.
		 */
		cc = LLVMBuildICmp(builder, LLVMIntULE, index, c_max, "");
		index = LLVMBuildSelect(builder, cc, index, c_max, "");
	}

	return index;
}

static struct radeon_llvm_loop *get_current_loop(struct radeon_llvm_context *ctx)
{
	return ctx->loop_depth > 0 ? ctx->loop + (ctx->loop_depth - 1) : NULL;
}

static struct radeon_llvm_branch *get_current_branch(struct radeon_llvm_context *ctx)
{
	return ctx->branch_depth > 0 ?
			ctx->branch + (ctx->branch_depth - 1) : NULL;
}

unsigned radeon_llvm_reg_index_soa(unsigned index, unsigned chan)
{
	return (index * 4) + chan;
}

static LLVMValueRef emit_swizzle(struct lp_build_tgsi_context *bld_base,
				 LLVMValueRef value,
				 unsigned swizzle_x,
				 unsigned swizzle_y,
				 unsigned swizzle_z,
				 unsigned swizzle_w)
{
	LLVMValueRef swizzles[4];
	LLVMTypeRef i32t =
		LLVMInt32TypeInContext(bld_base->base.gallivm->context);

	swizzles[0] = LLVMConstInt(i32t, swizzle_x, 0);
	swizzles[1] = LLVMConstInt(i32t, swizzle_y, 0);
	swizzles[2] = LLVMConstInt(i32t, swizzle_z, 0);
	swizzles[3] = LLVMConstInt(i32t, swizzle_w, 0);

	return LLVMBuildShuffleVector(bld_base->base.gallivm->builder,
				      value,
				      LLVMGetUndef(LLVMTypeOf(value)),
				      LLVMConstVector(swizzles, 4), "");
}

/**
 * Return the description of the array covering the given temporary register
 * index.
 */
static unsigned
get_temp_array_id(struct lp_build_tgsi_context *bld_base,
		  unsigned reg_index,
		  const struct tgsi_ind_register *reg)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	unsigned num_arrays = ctx->soa.bld_base.info->array_max[TGSI_FILE_TEMPORARY];
	unsigned i;

	if (reg && reg->ArrayID > 0 && reg->ArrayID <= num_arrays)
		return reg->ArrayID;

	for (i = 0; i < num_arrays; i++) {
		const struct tgsi_array_info *array = &ctx->temp_arrays[i];

		if (reg_index >= array->range.First && reg_index <= array->range.Last)
			return i + 1;
	}

	return 0;
}

static struct tgsi_declaration_range
get_array_range(struct lp_build_tgsi_context *bld_base,
		unsigned File, unsigned reg_index,
		const struct tgsi_ind_register *reg)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct tgsi_declaration_range range;

	if (File == TGSI_FILE_TEMPORARY) {
		unsigned array_id = get_temp_array_id(bld_base, reg_index, reg);
		if (array_id)
			return ctx->temp_arrays[array_id - 1].range;
	}

	range.First = 0;
	range.Last = bld_base->info->file_max[File];
	return range;
}

static LLVMValueRef
emit_array_index(struct lp_build_tgsi_soa_context *bld,
		 const struct tgsi_ind_register *reg,
		 unsigned offset)
{
	struct gallivm_state *gallivm = bld->bld_base.base.gallivm;

	if (!reg) {
		return lp_build_const_int32(gallivm, offset);
	}
	LLVMValueRef addr = LLVMBuildLoad(gallivm->builder, bld->addr[reg->Index][reg->Swizzle], "");
	return LLVMBuildAdd(gallivm->builder, addr, lp_build_const_int32(gallivm, offset), "");
}

/**
 * For indirect registers, construct a pointer directly to the requested
 * element using getelementptr if possible.
 *
 * Returns NULL if the insertelement/extractelement fallback for array access
 * must be used.
 */
static LLVMValueRef
get_pointer_into_array(struct radeon_llvm_context *ctx,
		       unsigned file,
		       unsigned swizzle,
		       unsigned reg_index,
		       const struct tgsi_ind_register *reg_indirect)
{
	unsigned array_id;
	struct tgsi_array_info *array;
	struct gallivm_state *gallivm = ctx->soa.bld_base.base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef idxs[2];
	LLVMValueRef index;
	LLVMValueRef alloca;

	if (file != TGSI_FILE_TEMPORARY)
		return NULL;

	array_id = get_temp_array_id(&ctx->soa.bld_base, reg_index, reg_indirect);
	if (!array_id)
		return NULL;

	alloca = ctx->temp_array_allocas[array_id - 1];
	if (!alloca)
		return NULL;

	array = &ctx->temp_arrays[array_id - 1];

	if (!(array->writemask & (1 << swizzle)))
		return ctx->undef_alloca;

	index = emit_array_index(&ctx->soa, reg_indirect,
				 reg_index - ctx->temp_arrays[array_id - 1].range.First);

	/* Ensure that the index is within a valid range, to guard against
	 * VM faults and overwriting critical data (e.g. spilled resource
	 * descriptors).
	 *
	 * TODO It should be possible to avoid the additional instructions
	 * if LLVM is changed so that it guarantuees:
	 * 1. the scratch space descriptor isolates the current wave (this
	 *    could even save the scratch offset SGPR at the cost of an
	 *    additional SALU instruction)
	 * 2. the memory for allocas must be allocated at the _end_ of the
	 *    scratch space (after spilled registers)
	 */
	index = radeon_llvm_bound_index(ctx, index, array->range.Last - array->range.First + 1);

	index = LLVMBuildMul(
		builder, index,
		lp_build_const_int32(gallivm, util_bitcount(array->writemask)),
		"");
	index = LLVMBuildAdd(
		builder, index,
		lp_build_const_int32(
			gallivm,
			util_bitcount(array->writemask & ((1 << swizzle) - 1))),
		"");
	idxs[0] = ctx->soa.bld_base.uint_bld.zero;
	idxs[1] = index;
	return LLVMBuildGEP(builder, alloca, idxs, 2, "");
}

LLVMValueRef
radeon_llvm_emit_fetch_64bit(struct lp_build_tgsi_context *bld_base,
			     enum tgsi_opcode_type type,
			     LLVMValueRef ptr,
			     LLVMValueRef ptr2)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMValueRef result;

	result = LLVMGetUndef(LLVMVectorType(LLVMIntTypeInContext(bld_base->base.gallivm->context, 32), bld_base->base.type.length * 2));

	result = LLVMBuildInsertElement(builder,
					result,
					bitcast(bld_base, TGSI_TYPE_UNSIGNED, ptr),
					bld_base->int_bld.zero, "");
	result = LLVMBuildInsertElement(builder,
					result,
					bitcast(bld_base, TGSI_TYPE_UNSIGNED, ptr2),
					bld_base->int_bld.one, "");
	return bitcast(bld_base, type, result);
}

static LLVMValueRef
emit_array_fetch(struct lp_build_tgsi_context *bld_base,
		 unsigned File, enum tgsi_opcode_type type,
		 struct tgsi_declaration_range range,
		 unsigned swizzle)
{
	struct lp_build_tgsi_soa_context *bld = lp_soa_context(bld_base);
	struct gallivm_state *gallivm = bld->bld_base.base.gallivm;
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;

	unsigned i, size = range.Last - range.First + 1;
	LLVMTypeRef vec = LLVMVectorType(tgsi2llvmtype(bld_base, type), size);
	LLVMValueRef result = LLVMGetUndef(vec);

	struct tgsi_full_src_register tmp_reg = {};
	tmp_reg.Register.File = File;

	for (i = 0; i < size; ++i) {
		tmp_reg.Register.Index = i + range.First;
		LLVMValueRef temp = radeon_llvm_emit_fetch(bld_base, &tmp_reg, type, swizzle);
		result = LLVMBuildInsertElement(builder, result, temp,
			lp_build_const_int32(gallivm, i), "array_vector");
	}
	return result;
}

static LLVMValueRef
load_value_from_array(struct lp_build_tgsi_context *bld_base,
		      unsigned file,
		      enum tgsi_opcode_type type,
		      unsigned swizzle,
		      unsigned reg_index,
		      const struct tgsi_ind_register *reg_indirect)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct lp_build_tgsi_soa_context *bld = lp_soa_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef ptr;

	ptr = get_pointer_into_array(ctx, file, swizzle, reg_index, reg_indirect);
	if (ptr) {
		LLVMValueRef val = LLVMBuildLoad(builder, ptr, "");
		if (tgsi_type_is_64bit(type)) {
			LLVMValueRef ptr_hi, val_hi;
			ptr_hi = LLVMBuildGEP(builder, ptr, &bld_base->uint_bld.one, 1, "");
			val_hi = LLVMBuildLoad(builder, ptr_hi, "");
			val = radeon_llvm_emit_fetch_64bit(bld_base, type, val, val_hi);
		}

		return val;
	} else {
		struct tgsi_declaration_range range =
			get_array_range(bld_base, file, reg_index, reg_indirect);
		LLVMValueRef index =
			emit_array_index(bld, reg_indirect, reg_index - range.First);
		LLVMValueRef array =
			emit_array_fetch(bld_base, file, type, range, swizzle);
		return LLVMBuildExtractElement(builder, array, index, "");
	}
}

static void
store_value_to_array(struct lp_build_tgsi_context *bld_base,
		     LLVMValueRef value,
		     unsigned file,
		     unsigned chan_index,
		     unsigned reg_index,
		     const struct tgsi_ind_register *reg_indirect)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct lp_build_tgsi_soa_context *bld = lp_soa_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef ptr;

	ptr = get_pointer_into_array(ctx, file, chan_index, reg_index, reg_indirect);
	if (ptr) {
		LLVMBuildStore(builder, value, ptr);
	} else {
		unsigned i, size;
		struct tgsi_declaration_range range = get_array_range(bld_base, file, reg_index, reg_indirect);
		LLVMValueRef index = emit_array_index(bld, reg_indirect, reg_index - range.First);
		LLVMValueRef array =
			emit_array_fetch(bld_base, file, TGSI_TYPE_FLOAT, range, chan_index);
		LLVMValueRef temp_ptr;

		array = LLVMBuildInsertElement(builder, array, value, index, "");

		size = range.Last - range.First + 1;
		for (i = 0; i < size; ++i) {
			switch(file) {
			case TGSI_FILE_OUTPUT:
				temp_ptr = bld->outputs[i + range.First][chan_index];
				break;

			case TGSI_FILE_TEMPORARY:
				if (range.First + i >= ctx->temps_count)
					continue;
				temp_ptr = ctx->temps[(i + range.First) * TGSI_NUM_CHANNELS + chan_index];
				break;

			default:
				continue;
			}
			value = LLVMBuildExtractElement(builder, array,
				lp_build_const_int32(gallivm, i), "");
			LLVMBuildStore(builder, value, temp_ptr);
		}
	}
}

LLVMValueRef radeon_llvm_emit_fetch(struct lp_build_tgsi_context *bld_base,
				    const struct tgsi_full_src_register *reg,
				    enum tgsi_opcode_type type,
				    unsigned swizzle)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct lp_build_tgsi_soa_context *bld = lp_soa_context(bld_base);
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMValueRef result = NULL, ptr, ptr2;

	if (swizzle == ~0) {
		LLVMValueRef values[TGSI_NUM_CHANNELS];
		unsigned chan;
		for (chan = 0; chan < TGSI_NUM_CHANNELS; chan++) {
			values[chan] = radeon_llvm_emit_fetch(bld_base, reg, type, chan);
		}
		return lp_build_gather_values(bld_base->base.gallivm, values,
					      TGSI_NUM_CHANNELS);
	}

	if (reg->Register.Indirect) {
		LLVMValueRef load = load_value_from_array(bld_base, reg->Register.File, type,
				swizzle, reg->Register.Index, &reg->Indirect);
		return bitcast(bld_base, type, load);
	}

	switch(reg->Register.File) {
	case TGSI_FILE_IMMEDIATE: {
		LLVMTypeRef ctype = tgsi2llvmtype(bld_base, type);
		if (tgsi_type_is_64bit(type)) {
			result = LLVMGetUndef(LLVMVectorType(LLVMIntTypeInContext(bld_base->base.gallivm->context, 32), bld_base->base.type.length * 2));
			result = LLVMConstInsertElement(result,
							bld->immediates[reg->Register.Index][swizzle],
							bld_base->int_bld.zero);
			result = LLVMConstInsertElement(result,
							bld->immediates[reg->Register.Index][swizzle + 1],
							bld_base->int_bld.one);
			return LLVMConstBitCast(result, ctype);
		} else {
			return LLVMConstBitCast(bld->immediates[reg->Register.Index][swizzle], ctype);
		}
	}

	case TGSI_FILE_INPUT: {
		unsigned index = reg->Register.Index;
		LLVMValueRef input[4];

		/* I don't think doing this for vertex shaders is beneficial.
		 * For those, we want to make sure the VMEM loads are executed
		 * only once. Fragment shaders don't care much, because
		 * v_interp instructions are much cheaper than VMEM loads.
		 */
		if (ctx->soa.bld_base.info->processor == PIPE_SHADER_FRAGMENT)
			ctx->load_input(ctx, index, &ctx->input_decls[index], input);
		else
			memcpy(input, &ctx->inputs[index * 4], sizeof(input));

		result = input[swizzle];

		if (tgsi_type_is_64bit(type)) {
			ptr = result;
			ptr2 = input[swizzle + 1];
			return radeon_llvm_emit_fetch_64bit(bld_base, type, ptr, ptr2);
		}
		break;
	}

	case TGSI_FILE_TEMPORARY:
		if (reg->Register.Index >= ctx->temps_count)
			return LLVMGetUndef(tgsi2llvmtype(bld_base, type));
		ptr = ctx->temps[reg->Register.Index * TGSI_NUM_CHANNELS + swizzle];
		if (tgsi_type_is_64bit(type)) {
			ptr2 = ctx->temps[reg->Register.Index * TGSI_NUM_CHANNELS + swizzle + 1];
			return radeon_llvm_emit_fetch_64bit(bld_base, type,
						 LLVMBuildLoad(builder, ptr, ""),
						 LLVMBuildLoad(builder, ptr2, ""));
		}
		result = LLVMBuildLoad(builder, ptr, "");
		break;

	case TGSI_FILE_OUTPUT:
		ptr = lp_get_output_ptr(bld, reg->Register.Index, swizzle);
		if (tgsi_type_is_64bit(type)) {
			ptr2 = lp_get_output_ptr(bld, reg->Register.Index, swizzle + 1);
			return radeon_llvm_emit_fetch_64bit(bld_base, type,
						 LLVMBuildLoad(builder, ptr, ""),
						 LLVMBuildLoad(builder, ptr2, ""));
		}
		result = LLVMBuildLoad(builder, ptr, "");
		break;

	default:
		return LLVMGetUndef(tgsi2llvmtype(bld_base, type));
	}

	return bitcast(bld_base, type, result);
}

static LLVMValueRef fetch_system_value(struct lp_build_tgsi_context *bld_base,
				       const struct tgsi_full_src_register *reg,
				       enum tgsi_opcode_type type,
				       unsigned swizzle)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;

	LLVMValueRef cval = ctx->system_values[reg->Register.Index];
	if (LLVMGetTypeKind(LLVMTypeOf(cval)) == LLVMVectorTypeKind) {
		cval = LLVMBuildExtractElement(gallivm->builder, cval,
					       lp_build_const_int32(gallivm, swizzle), "");
	}
	return bitcast(bld_base, type, cval);
}

static void emit_declaration(struct lp_build_tgsi_context *bld_base,
			     const struct tgsi_full_declaration *decl)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	unsigned first, last, i;
	switch(decl->Declaration.File) {
	case TGSI_FILE_ADDRESS:
	{
		 unsigned idx;
		for (idx = decl->Range.First; idx <= decl->Range.Last; idx++) {
			unsigned chan;
			for (chan = 0; chan < TGSI_NUM_CHANNELS; chan++) {
				 ctx->soa.addr[idx][chan] = lp_build_alloca_undef(
					&ctx->gallivm,
					ctx->soa.bld_base.uint_bld.elem_type, "");
			}
		}
		break;
	}

	case TGSI_FILE_TEMPORARY:
	{
		char name[16] = "";
		LLVMValueRef array_alloca = NULL;
		unsigned decl_size;
		unsigned writemask = decl->Declaration.UsageMask;
		first = decl->Range.First;
		last = decl->Range.Last;
		decl_size = 4 * ((last - first) + 1);

		if (decl->Declaration.Array) {
			unsigned id = decl->Array.ArrayID - 1;
			unsigned array_size;

			writemask &= ctx->temp_arrays[id].writemask;
			ctx->temp_arrays[id].writemask = writemask;
			array_size = ((last - first) + 1) * util_bitcount(writemask);

			/* If the array has more than 16 elements, store it
			 * in memory using an alloca that spans the entire
			 * array.
			 *
			 * Otherwise, store each array element individually.
			 * We will then generate vectors (per-channel, up to
			 * <16 x float> if the usagemask is a single bit) for
			 * indirect addressing.
			 *
			 * Note that 16 is the number of vector elements that
			 * LLVM will store in a register, so theoretically an
			 * array with up to 4 * 16 = 64 elements could be
			 * handled this way, but whether that's a good idea
			 * depends on VGPR register pressure elsewhere.
			 *
			 * FIXME: We shouldn't need to have the non-alloca
			 * code path for arrays. LLVM should be smart enough to
			 * promote allocas into registers when profitable.
			 *
			 * LLVM 3.8 crashes with this.
			 */
			if (HAVE_LLVM >= 0x0309 && array_size > 16) {
				array_alloca = LLVMBuildAlloca(builder,
					LLVMArrayType(bld_base->base.vec_type,
						      array_size), "array");
				ctx->temp_array_allocas[id] = array_alloca;
			}
		}

		if (!ctx->temps_count) {
			ctx->temps_count = bld_base->info->file_max[TGSI_FILE_TEMPORARY] + 1;
			ctx->temps = MALLOC(TGSI_NUM_CHANNELS * ctx->temps_count * sizeof(LLVMValueRef));
		}
		if (!array_alloca) {
			for (i = 0; i < decl_size; ++i) {
#ifdef DEBUG
				snprintf(name, sizeof(name), "TEMP%d.%c",
					 first + i / 4, "xyzw"[i % 4]);
#endif
				ctx->temps[first * TGSI_NUM_CHANNELS + i] =
					lp_build_alloca_undef(bld_base->base.gallivm,
							      bld_base->base.vec_type,
							      name);
			}
		} else {
			LLVMValueRef idxs[2] = {
				bld_base->uint_bld.zero,
				NULL
			};
			unsigned j = 0;

			if (writemask != TGSI_WRITEMASK_XYZW &&
			    !ctx->undef_alloca) {
				/* Create a dummy alloca. We use it so that we
				 * have a pointer that is safe to load from if
				 * a shader ever reads from a channel that
				 * it never writes to.
				 */
				ctx->undef_alloca = lp_build_alloca_undef(
					bld_base->base.gallivm,
					bld_base->base.vec_type, "undef");
			}

			for (i = 0; i < decl_size; ++i) {
				LLVMValueRef ptr;
				if (writemask & (1 << (i % 4))) {
#ifdef DEBUG
					snprintf(name, sizeof(name), "TEMP%d.%c",
						 first + i / 4, "xyzw"[i % 4]);
#endif
					idxs[1] = lp_build_const_int32(bld_base->base.gallivm, j);
					ptr = LLVMBuildGEP(builder, array_alloca, idxs, 2, name);
					j++;
				} else {
					ptr = ctx->undef_alloca;
				}
				ctx->temps[first * TGSI_NUM_CHANNELS + i] = ptr;
			}
		}
		break;
	}
	case TGSI_FILE_INPUT:
	{
		unsigned idx;
		for (idx = decl->Range.First; idx <= decl->Range.Last; idx++) {
			if (ctx->load_input) {
				ctx->input_decls[idx] = *decl;

				if (bld_base->info->processor != PIPE_SHADER_FRAGMENT)
					ctx->load_input(ctx, idx, decl,
							&ctx->inputs[idx * 4]);
			}
		}
	}
	break;

	case TGSI_FILE_SYSTEM_VALUE:
	{
		unsigned idx;
		for (idx = decl->Range.First; idx <= decl->Range.Last; idx++) {
			ctx->load_system_value(ctx, idx, decl);
		}
	}
	break;

	case TGSI_FILE_OUTPUT:
	{
		unsigned idx;
		for (idx = decl->Range.First; idx <= decl->Range.Last; idx++) {
			unsigned chan;
			assert(idx < RADEON_LLVM_MAX_OUTPUTS);
			for (chan = 0; chan < TGSI_NUM_CHANNELS; chan++) {
				ctx->soa.outputs[idx][chan] = lp_build_alloca_undef(
					&ctx->gallivm,
					ctx->soa.bld_base.base.elem_type, "");
			}
		}
		break;
	}

	case TGSI_FILE_MEMORY:
		ctx->declare_memory_region(ctx, decl);
		break;

	default:
		break;
	}
}

LLVMValueRef radeon_llvm_saturate(struct lp_build_tgsi_context *bld_base,
                                  LLVMValueRef value)
{
	struct lp_build_emit_data clamp_emit_data;

	memset(&clamp_emit_data, 0, sizeof(clamp_emit_data));
	clamp_emit_data.arg_count = 3;
	clamp_emit_data.args[0] = value;
	clamp_emit_data.args[2] = bld_base->base.one;
	clamp_emit_data.args[1] = bld_base->base.zero;

	return lp_build_emit_llvm(bld_base, TGSI_OPCODE_CLAMP,
				  &clamp_emit_data);
}

void radeon_llvm_emit_store(struct lp_build_tgsi_context *bld_base,
			    const struct tgsi_full_instruction *inst,
			    const struct tgsi_opcode_info *info,
			    LLVMValueRef dst[4])
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct lp_build_tgsi_soa_context *bld = lp_soa_context(bld_base);
	struct gallivm_state *gallivm = bld->bld_base.base.gallivm;
	const struct tgsi_full_dst_register *reg = &inst->Dst[0];
	LLVMBuilderRef builder = bld->bld_base.base.gallivm->builder;
	LLVMValueRef temp_ptr, temp_ptr2 = NULL;
	unsigned chan, chan_index;
	bool is_vec_store = false;
	enum tgsi_opcode_type dtype = tgsi_opcode_infer_dst_type(inst->Instruction.Opcode);

	if (dst[0]) {
		LLVMTypeKind k = LLVMGetTypeKind(LLVMTypeOf(dst[0]));
		is_vec_store = (k == LLVMVectorTypeKind);
	}

	if (is_vec_store) {
		LLVMValueRef values[4] = {};
		TGSI_FOR_EACH_DST0_ENABLED_CHANNEL(inst, chan) {
			LLVMValueRef index = lp_build_const_int32(gallivm, chan);
			values[chan]  = LLVMBuildExtractElement(gallivm->builder,
							dst[0], index, "");
		}
		bld_base->emit_store(bld_base, inst, info, values);
		return;
	}

	TGSI_FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
		LLVMValueRef value = dst[chan_index];

		if (tgsi_type_is_64bit(dtype) && (chan_index == 1 || chan_index == 3))
			continue;
		if (inst->Instruction.Saturate)
			value = radeon_llvm_saturate(bld_base, value);

		if (reg->Register.File == TGSI_FILE_ADDRESS) {
			temp_ptr = bld->addr[reg->Register.Index][chan_index];
			LLVMBuildStore(builder, value, temp_ptr);
			continue;
		}

		if (!tgsi_type_is_64bit(dtype))
			value = bitcast(bld_base, TGSI_TYPE_FLOAT, value);

		if (reg->Register.Indirect) {
			unsigned file = reg->Register.File;
			unsigned reg_index = reg->Register.Index;
			store_value_to_array(bld_base, value, file, chan_index,
					     reg_index, &reg->Indirect);
		} else {
			switch(reg->Register.File) {
			case TGSI_FILE_OUTPUT:
				temp_ptr = bld->outputs[reg->Register.Index][chan_index];
				if (tgsi_type_is_64bit(dtype))
					temp_ptr2 = bld->outputs[reg->Register.Index][chan_index + 1];
				break;

			case TGSI_FILE_TEMPORARY:
			{
				if (reg->Register.Index >= ctx->temps_count)
					continue;

				temp_ptr = ctx->temps[ TGSI_NUM_CHANNELS * reg->Register.Index + chan_index];
				if (tgsi_type_is_64bit(dtype))
					temp_ptr2 = ctx->temps[ TGSI_NUM_CHANNELS * reg->Register.Index + chan_index + 1];

				break;
			}
			default:
				return;
			}
			if (!tgsi_type_is_64bit(dtype))
				LLVMBuildStore(builder, value, temp_ptr);
			else {
				LLVMValueRef ptr = LLVMBuildBitCast(builder, value,
								    LLVMVectorType(LLVMIntTypeInContext(bld_base->base.gallivm->context, 32), 2), "");
				LLVMValueRef val2;
				value = LLVMBuildExtractElement(builder, ptr,
								bld_base->uint_bld.zero, "");
				val2 = LLVMBuildExtractElement(builder, ptr,
								bld_base->uint_bld.one, "");

				LLVMBuildStore(builder, bitcast(bld_base, TGSI_TYPE_FLOAT, value), temp_ptr);
				LLVMBuildStore(builder, bitcast(bld_base, TGSI_TYPE_FLOAT, val2), temp_ptr2);
			}
		}
	}
}

static void bgnloop_emit(const struct lp_build_tgsi_action *action,
			 struct lp_build_tgsi_context *bld_base,
			 struct lp_build_emit_data *emit_data)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBasicBlockRef loop_block;
	LLVMBasicBlockRef endloop_block;
	endloop_block = LLVMAppendBasicBlockInContext(gallivm->context,
						ctx->main_fn, "ENDLOOP");
	loop_block = LLVMInsertBasicBlockInContext(gallivm->context,
						endloop_block, "LOOP");
	LLVMBuildBr(gallivm->builder, loop_block);
	LLVMPositionBuilderAtEnd(gallivm->builder, loop_block);

	if (++ctx->loop_depth > ctx->loop_depth_max) {
		unsigned new_max = ctx->loop_depth_max << 1;

		if (!new_max)
			new_max = RADEON_LLVM_INITIAL_CF_DEPTH;

		ctx->loop = REALLOC(ctx->loop, ctx->loop_depth_max *
				    sizeof(ctx->loop[0]),
				    new_max * sizeof(ctx->loop[0]));
		ctx->loop_depth_max = new_max;
	}

	ctx->loop[ctx->loop_depth - 1].loop_block = loop_block;
	ctx->loop[ctx->loop_depth - 1].endloop_block = endloop_block;
}

static void brk_emit(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	struct radeon_llvm_loop *current_loop = get_current_loop(ctx);

	LLVMBuildBr(gallivm->builder, current_loop->endloop_block);
}

static void cont_emit(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	struct radeon_llvm_loop *current_loop = get_current_loop(ctx);

	LLVMBuildBr(gallivm->builder, current_loop->loop_block);
}

static void else_emit(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	struct radeon_llvm_branch *current_branch = get_current_branch(ctx);
	LLVMBasicBlockRef current_block = LLVMGetInsertBlock(gallivm->builder);

	/* We need to add a terminator to the current block if the previous
	 * instruction was an ENDIF.Example:
	 * IF
	 *   [code]
	 *   IF
	 *     [code]
	 *   ELSE
	 *    [code]
	 *   ENDIF <--
	 * ELSE<--
	 *   [code]
	 * ENDIF
	 */

	if (current_block != current_branch->if_block) {
		LLVMBuildBr(gallivm->builder, current_branch->endif_block);
	}
	if (!LLVMGetBasicBlockTerminator(current_branch->if_block)) {
		LLVMBuildBr(gallivm->builder, current_branch->endif_block);
	}
	current_branch->has_else = 1;
	LLVMPositionBuilderAtEnd(gallivm->builder, current_branch->else_block);
}

static void endif_emit(const struct lp_build_tgsi_action *action,
		       struct lp_build_tgsi_context *bld_base,
		       struct lp_build_emit_data *emit_data)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	struct radeon_llvm_branch *current_branch = get_current_branch(ctx);
	LLVMBasicBlockRef current_block = LLVMGetInsertBlock(gallivm->builder);

	/* If we have consecutive ENDIF instructions, then the first ENDIF
	 * will not have a terminator, so we need to add one. */
	if (current_block != current_branch->if_block
			&& current_block != current_branch->else_block
			&& !LLVMGetBasicBlockTerminator(current_block)) {

		 LLVMBuildBr(gallivm->builder, current_branch->endif_block);
	}
	if (!LLVMGetBasicBlockTerminator(current_branch->else_block)) {
		LLVMPositionBuilderAtEnd(gallivm->builder, current_branch->else_block);
		LLVMBuildBr(gallivm->builder, current_branch->endif_block);
	}

	if (!LLVMGetBasicBlockTerminator(current_branch->if_block)) {
		LLVMPositionBuilderAtEnd(gallivm->builder, current_branch->if_block);
		LLVMBuildBr(gallivm->builder, current_branch->endif_block);
	}

	LLVMPositionBuilderAtEnd(gallivm->builder, current_branch->endif_block);
	ctx->branch_depth--;
}

static void endloop_emit(const struct lp_build_tgsi_action *action,
			 struct lp_build_tgsi_context *bld_base,
			 struct lp_build_emit_data *emit_data)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	struct radeon_llvm_loop *current_loop = get_current_loop(ctx);

	if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(gallivm->builder))) {
		 LLVMBuildBr(gallivm->builder, current_loop->loop_block);
	}

	LLVMPositionBuilderAtEnd(gallivm->builder, current_loop->endloop_block);
	ctx->loop_depth--;
}

static void if_cond_emit(const struct lp_build_tgsi_action *action,
			 struct lp_build_tgsi_context *bld_base,
			 struct lp_build_emit_data *emit_data,
			 LLVMValueRef cond)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBasicBlockRef if_block, else_block, endif_block;

	endif_block = LLVMAppendBasicBlockInContext(gallivm->context,
						ctx->main_fn, "ENDIF");
	if_block = LLVMInsertBasicBlockInContext(gallivm->context,
						endif_block, "IF");
	else_block = LLVMInsertBasicBlockInContext(gallivm->context,
						endif_block, "ELSE");
	LLVMBuildCondBr(gallivm->builder, cond, if_block, else_block);
	LLVMPositionBuilderAtEnd(gallivm->builder, if_block);

	if (++ctx->branch_depth > ctx->branch_depth_max) {
		unsigned new_max = ctx->branch_depth_max << 1;

		if (!new_max)
			new_max = RADEON_LLVM_INITIAL_CF_DEPTH;

		ctx->branch = REALLOC(ctx->branch, ctx->branch_depth_max *
				      sizeof(ctx->branch[0]),
				      new_max * sizeof(ctx->branch[0]));
		ctx->branch_depth_max = new_max;
	}

	ctx->branch[ctx->branch_depth - 1].endif_block = endif_block;
	ctx->branch[ctx->branch_depth - 1].if_block = if_block;
	ctx->branch[ctx->branch_depth - 1].else_block = else_block;
	ctx->branch[ctx->branch_depth - 1].has_else = 0;
}

static void if_emit(const struct lp_build_tgsi_action *action,
		    struct lp_build_tgsi_context *bld_base,
		    struct lp_build_emit_data *emit_data)
{
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMValueRef cond;

	cond = LLVMBuildFCmp(gallivm->builder, LLVMRealUNE,
			emit_data->args[0],
			bld_base->base.zero, "");

	if_cond_emit(action, bld_base, emit_data, cond);
}

static void uif_emit(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMValueRef cond;

	cond = LLVMBuildICmp(gallivm->builder, LLVMIntNE,
	        bitcast(bld_base, TGSI_TYPE_UNSIGNED, emit_data->args[0]),
			bld_base->int_bld.zero, "");

	if_cond_emit(action, bld_base, emit_data, cond);
}

static void kill_if_fetch_args(struct lp_build_tgsi_context *bld_base,
			       struct lp_build_emit_data *emit_data)
{
	const struct tgsi_full_instruction *inst = emit_data->inst;
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	unsigned i;
	LLVMValueRef conds[TGSI_NUM_CHANNELS];

	for (i = 0; i < TGSI_NUM_CHANNELS; i++) {
		LLVMValueRef value = lp_build_emit_fetch(bld_base, inst, 0, i);
		conds[i] = LLVMBuildFCmp(builder, LLVMRealOLT, value,
					bld_base->base.zero, "");
	}

	/* Or the conditions together */
	for (i = TGSI_NUM_CHANNELS - 1; i > 0; i--) {
		conds[i - 1] = LLVMBuildOr(builder, conds[i], conds[i - 1], "");
	}

	emit_data->dst_type = LLVMVoidTypeInContext(gallivm->context);
	emit_data->arg_count = 1;
	emit_data->args[0] = LLVMBuildSelect(builder, conds[0],
					lp_build_const_float(gallivm, -1.0f),
					bld_base->base.zero, "");
}

static void kil_emit(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	unsigned i;
	for (i = 0; i < emit_data->arg_count; i++) {
		emit_data->output[i] = lp_build_intrinsic_unary(
			bld_base->base.gallivm->builder,
			action->intr_name,
			emit_data->dst_type, emit_data->args[i]);
	}
}

static LLVMValueRef build_cube_intrinsic(struct gallivm_state *gallivm,
					 LLVMValueRef in[3])
{
	if (HAVE_LLVM >= 0x0309) {
		LLVMTypeRef f32 = LLVMTypeOf(in[0]);
		LLVMValueRef out[4];

		out[0] = lp_build_intrinsic(gallivm->builder, "llvm.amdgcn.cubetc",
					    f32, in, 3, LLVMReadNoneAttribute);
		out[1] = lp_build_intrinsic(gallivm->builder, "llvm.amdgcn.cubesc",
					    f32, in, 3, LLVMReadNoneAttribute);
		out[2] = lp_build_intrinsic(gallivm->builder, "llvm.amdgcn.cubema",
					    f32, in, 3, LLVMReadNoneAttribute);
		out[3] = lp_build_intrinsic(gallivm->builder, "llvm.amdgcn.cubeid",
					    f32, in, 3, LLVMReadNoneAttribute);

		return lp_build_gather_values(gallivm, out, 4);
	} else {
		LLVMValueRef c[4] = {
			in[0],
			in[1],
			in[2],
			LLVMGetUndef(LLVMTypeOf(in[0]))
		};
		LLVMValueRef vec = lp_build_gather_values(gallivm, c, 4);

		return lp_build_intrinsic(gallivm->builder, "llvm.AMDGPU.cube",
					  LLVMTypeOf(vec), &vec, 1,
					  LLVMReadNoneAttribute);
	}
}

static void radeon_llvm_cube_to_2d_coords(struct lp_build_tgsi_context *bld_base,
					  LLVMValueRef *in, LLVMValueRef *out)
{
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMTypeRef type = bld_base->base.elem_type;
	LLVMValueRef coords[4];
	LLVMValueRef mad_args[3];
	LLVMValueRef v;
	unsigned i;

	v = build_cube_intrinsic(gallivm, in);

	for (i = 0; i < 4; ++i)
		coords[i] = LLVMBuildExtractElement(builder, v,
						    lp_build_const_int32(gallivm, i), "");

	coords[2] = lp_build_intrinsic(builder, "llvm.fabs.f32",
			type, &coords[2], 1, LLVMReadNoneAttribute);
	coords[2] = lp_build_emit_llvm_unary(bld_base, TGSI_OPCODE_RCP, coords[2]);

	mad_args[1] = coords[2];
	mad_args[2] = LLVMConstReal(type, 1.5);

	mad_args[0] = coords[0];
	coords[0] = lp_build_emit_llvm_ternary(bld_base, TGSI_OPCODE_MAD,
			mad_args[0], mad_args[1], mad_args[2]);

	mad_args[0] = coords[1];
	coords[1] = lp_build_emit_llvm_ternary(bld_base, TGSI_OPCODE_MAD,
			mad_args[0], mad_args[1], mad_args[2]);

	/* apply xyz = yxw swizzle to cooords */
	out[0] = coords[1];
	out[1] = coords[0];
	out[2] = coords[3];
}

void radeon_llvm_emit_prepare_cube_coords(struct lp_build_tgsi_context *bld_base,
					  struct lp_build_emit_data *emit_data,
					  LLVMValueRef *coords_arg,
					  LLVMValueRef *derivs_arg)
{

	unsigned target = emit_data->inst->Texture.Texture;
	unsigned opcode = emit_data->inst->Instruction.Opcode;
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef coords[4];
	unsigned i;

	radeon_llvm_cube_to_2d_coords(bld_base, coords_arg, coords);

	if (opcode == TGSI_OPCODE_TXD && derivs_arg) {
		LLVMValueRef derivs[4];
		int axis;

		/* Convert cube derivatives to 2D derivatives. */
		for (axis = 0; axis < 2; axis++) {
			LLVMValueRef shifted_cube_coords[4], shifted_coords[4];

			/* Shift the cube coordinates by the derivatives to get
			 * the cube coordinates of the "neighboring pixel".
			 */
			for (i = 0; i < 3; i++)
				shifted_cube_coords[i] =
					LLVMBuildFAdd(builder, coords_arg[i],
						      derivs_arg[axis*3+i], "");
			shifted_cube_coords[3] = LLVMGetUndef(bld_base->base.elem_type);

			/* Project the shifted cube coordinates onto the face. */
			radeon_llvm_cube_to_2d_coords(bld_base, shifted_cube_coords,
						      shifted_coords);

			/* Subtract both sets of 2D coordinates to get 2D derivatives.
			 * This won't work if the shifted coordinates ended up
			 * in a different face.
			 */
			for (i = 0; i < 2; i++)
				derivs[axis * 2 + i] =
					LLVMBuildFSub(builder, shifted_coords[i],
						      coords[i], "");
		}

		memcpy(derivs_arg, derivs, sizeof(derivs));
	}

	if (target == TGSI_TEXTURE_CUBE_ARRAY ||
	    target == TGSI_TEXTURE_SHADOWCUBE_ARRAY) {
		/* for cube arrays coord.z = coord.w(array_index) * 8 + face */
		/* coords_arg.w component - array_index for cube arrays */
		coords[2] = lp_build_emit_llvm_ternary(bld_base, TGSI_OPCODE_MAD,
						       coords_arg[3], lp_build_const_float(gallivm, 8.0), coords[2]);
	}

	/* Preserve compare/lod/bias. Put it in coords.w. */
	if (opcode == TGSI_OPCODE_TEX2 ||
	    opcode == TGSI_OPCODE_TXB2 ||
	    opcode == TGSI_OPCODE_TXL2) {
		coords[3] = coords_arg[4];
	} else if (opcode == TGSI_OPCODE_TXB ||
		   opcode == TGSI_OPCODE_TXL ||
		   target == TGSI_TEXTURE_SHADOWCUBE) {
		coords[3] = coords_arg[3];
	}

	memcpy(coords_arg, coords, sizeof(coords));
}

static void emit_icmp(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	unsigned pred;
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMContextRef context = bld_base->base.gallivm->context;

	switch (emit_data->inst->Instruction.Opcode) {
	case TGSI_OPCODE_USEQ: pred = LLVMIntEQ; break;
	case TGSI_OPCODE_USNE: pred = LLVMIntNE; break;
	case TGSI_OPCODE_USGE: pred = LLVMIntUGE; break;
	case TGSI_OPCODE_USLT: pred = LLVMIntULT; break;
	case TGSI_OPCODE_ISGE: pred = LLVMIntSGE; break;
	case TGSI_OPCODE_ISLT: pred = LLVMIntSLT; break;
	default:
		assert(!"unknown instruction");
		pred = 0;
		break;
	}

	LLVMValueRef v = LLVMBuildICmp(builder, pred,
			emit_data->args[0], emit_data->args[1],"");

	v = LLVMBuildSExtOrBitCast(builder, v,
			LLVMInt32TypeInContext(context), "");

	emit_data->output[emit_data->chan] = v;
}

static void emit_ucmp(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;

	LLVMValueRef arg0 = LLVMBuildBitCast(builder, emit_data->args[0],
					     bld_base->uint_bld.elem_type, "");

	LLVMValueRef v = LLVMBuildICmp(builder, LLVMIntNE, arg0,
				       bld_base->uint_bld.zero, "");

	emit_data->output[emit_data->chan] =
		LLVMBuildSelect(builder, v, emit_data->args[1], emit_data->args[2], "");
}

static void emit_cmp(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMValueRef cond, *args = emit_data->args;

	cond = LLVMBuildFCmp(builder, LLVMRealOLT, args[0],
			     bld_base->base.zero, "");

	emit_data->output[emit_data->chan] =
		LLVMBuildSelect(builder, cond, args[1], args[2], "");
}

static void emit_set_cond(const struct lp_build_tgsi_action *action,
			  struct lp_build_tgsi_context *bld_base,
			  struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMRealPredicate pred;
	LLVMValueRef cond;

	/* Use ordered for everything but NE (which is usual for
	 * float comparisons)
	 */
	switch (emit_data->inst->Instruction.Opcode) {
	case TGSI_OPCODE_SGE: pred = LLVMRealOGE; break;
	case TGSI_OPCODE_SEQ: pred = LLVMRealOEQ; break;
	case TGSI_OPCODE_SLE: pred = LLVMRealOLE; break;
	case TGSI_OPCODE_SLT: pred = LLVMRealOLT; break;
	case TGSI_OPCODE_SNE: pred = LLVMRealUNE; break;
	case TGSI_OPCODE_SGT: pred = LLVMRealOGT; break;
	default: assert(!"unknown instruction"); pred = 0; break;
	}

	cond = LLVMBuildFCmp(builder,
		pred, emit_data->args[0], emit_data->args[1], "");

	emit_data->output[emit_data->chan] = LLVMBuildSelect(builder,
		cond, bld_base->base.one, bld_base->base.zero, "");
}

static void emit_fcmp(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMContextRef context = bld_base->base.gallivm->context;
	LLVMRealPredicate pred;

	/* Use ordered for everything but NE (which is usual for
	 * float comparisons)
	 */
	switch (emit_data->inst->Instruction.Opcode) {
	case TGSI_OPCODE_FSEQ: pred = LLVMRealOEQ; break;
	case TGSI_OPCODE_FSGE: pred = LLVMRealOGE; break;
	case TGSI_OPCODE_FSLT: pred = LLVMRealOLT; break;
	case TGSI_OPCODE_FSNE: pred = LLVMRealUNE; break;
	default: assert(!"unknown instruction"); pred = 0; break;
	}

	LLVMValueRef v = LLVMBuildFCmp(builder, pred,
			emit_data->args[0], emit_data->args[1],"");

	v = LLVMBuildSExtOrBitCast(builder, v,
			LLVMInt32TypeInContext(context), "");

	emit_data->output[emit_data->chan] = v;
}

static void emit_dcmp(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMContextRef context = bld_base->base.gallivm->context;
	LLVMRealPredicate pred;

	/* Use ordered for everything but NE (which is usual for
	 * float comparisons)
	 */
	switch (emit_data->inst->Instruction.Opcode) {
	case TGSI_OPCODE_DSEQ: pred = LLVMRealOEQ; break;
	case TGSI_OPCODE_DSGE: pred = LLVMRealOGE; break;
	case TGSI_OPCODE_DSLT: pred = LLVMRealOLT; break;
	case TGSI_OPCODE_DSNE: pred = LLVMRealUNE; break;
	default: assert(!"unknown instruction"); pred = 0; break;
	}

	LLVMValueRef v = LLVMBuildFCmp(builder, pred,
			emit_data->args[0], emit_data->args[1],"");

	v = LLVMBuildSExtOrBitCast(builder, v,
			LLVMInt32TypeInContext(context), "");

	emit_data->output[emit_data->chan] = v;
}

static void emit_not(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMValueRef v = bitcast(bld_base, TGSI_TYPE_UNSIGNED,
			emit_data->args[0]);
	emit_data->output[emit_data->chan] = LLVMBuildNot(builder, v, "");
}

static void emit_arl(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMValueRef floor_index =  lp_build_emit_llvm_unary(bld_base, TGSI_OPCODE_FLR, emit_data->args[0]);
	emit_data->output[emit_data->chan] = LLVMBuildFPToSI(builder,
			floor_index, bld_base->base.int_elem_type , "");
}

static void emit_and(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildAnd(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_or(const struct lp_build_tgsi_action *action,
		    struct lp_build_tgsi_context *bld_base,
		    struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildOr(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_uadd(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildAdd(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_udiv(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildUDiv(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_idiv(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildSDiv(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_mod(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildSRem(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_umod(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildURem(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_shl(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildShl(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_ushr(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildLShr(builder,
			emit_data->args[0], emit_data->args[1], "");
}
static void emit_ishr(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildAShr(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_xor(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildXor(builder,
			emit_data->args[0], emit_data->args[1], "");
}

static void emit_ssg(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;

	LLVMValueRef cmp, val;

	if (emit_data->inst->Instruction.Opcode == TGSI_OPCODE_ISSG) {
		cmp = LLVMBuildICmp(builder, LLVMIntSGT, emit_data->args[0], bld_base->int_bld.zero, "");
		val = LLVMBuildSelect(builder, cmp, bld_base->int_bld.one, emit_data->args[0], "");
		cmp = LLVMBuildICmp(builder, LLVMIntSGE, val, bld_base->int_bld.zero, "");
		val = LLVMBuildSelect(builder, cmp, val, LLVMConstInt(bld_base->int_bld.elem_type, -1, true), "");
	} else { // float SSG
		cmp = LLVMBuildFCmp(builder, LLVMRealOGT, emit_data->args[0], bld_base->base.zero, "");
		val = LLVMBuildSelect(builder, cmp, bld_base->base.one, emit_data->args[0], "");
		cmp = LLVMBuildFCmp(builder, LLVMRealOGE, val, bld_base->base.zero, "");
		val = LLVMBuildSelect(builder, cmp, val, LLVMConstReal(bld_base->base.elem_type, -1), "");
	}

	emit_data->output[emit_data->chan] = val;
}

static void emit_ineg(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildNeg(builder,
			emit_data->args[0], "");
}

static void emit_dneg(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildFNeg(builder,
			emit_data->args[0], "");
}

static void emit_frac(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	char *intr;

	if (emit_data->info->opcode == TGSI_OPCODE_FRC)
		intr = "llvm.floor.f32";
	else if (emit_data->info->opcode == TGSI_OPCODE_DFRAC)
		intr = "llvm.floor.f64";
	else {
		assert(0);
		return;
	}

	LLVMValueRef floor = lp_build_intrinsic(builder, intr, emit_data->dst_type,
						&emit_data->args[0], 1,
						LLVMReadNoneAttribute);
	emit_data->output[emit_data->chan] = LLVMBuildFSub(builder,
			emit_data->args[0], floor, "");
}

static void emit_f2i(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildFPToSI(builder,
			emit_data->args[0], bld_base->int_bld.elem_type, "");
}

static void emit_f2u(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildFPToUI(builder,
			emit_data->args[0], bld_base->uint_bld.elem_type, "");
}

static void emit_i2f(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildSIToFP(builder,
			emit_data->args[0], bld_base->base.elem_type, "");
}

static void emit_u2f(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	emit_data->output[emit_data->chan] = LLVMBuildUIToFP(builder,
			emit_data->args[0], bld_base->base.elem_type, "");
}

static void emit_immediate(struct lp_build_tgsi_context *bld_base,
			   const struct tgsi_full_immediate *imm)
{
	unsigned i;
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);

	for (i = 0; i < 4; ++i) {
		ctx->soa.immediates[ctx->soa.num_immediates][i] =
				LLVMConstInt(bld_base->uint_bld.elem_type, imm->u[i].Uint, false   );
	}

	ctx->soa.num_immediates++;
}

void
build_tgsi_intrinsic_nomem(const struct lp_build_tgsi_action *action,
			   struct lp_build_tgsi_context *bld_base,
			   struct lp_build_emit_data *emit_data)
{
	struct lp_build_context *base = &bld_base->base;
	emit_data->output[emit_data->chan] =
		lp_build_intrinsic(base->gallivm->builder, action->intr_name,
				   emit_data->dst_type, emit_data->args,
				   emit_data->arg_count, LLVMReadNoneAttribute);
}

static void emit_bfi(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef bfi_args[3];

	// Calculate the bitmask: (((1 << src3) - 1) << src2
	bfi_args[0] = LLVMBuildShl(builder,
				   LLVMBuildSub(builder,
						LLVMBuildShl(builder,
							     bld_base->int_bld.one,
							     emit_data->args[3], ""),
						bld_base->int_bld.one, ""),
				   emit_data->args[2], "");

	bfi_args[1] = LLVMBuildShl(builder, emit_data->args[1],
				   emit_data->args[2], "");

	bfi_args[2] = emit_data->args[0];

	/* Calculate:
	 *   (arg0 & arg1) | (~arg0 & arg2) = arg2 ^ (arg0 & (arg1 ^ arg2)
	 * Use the right-hand side, which the LLVM backend can convert to V_BFI.
	 */
	emit_data->output[emit_data->chan] =
		LLVMBuildXor(builder, bfi_args[2],
			LLVMBuildAnd(builder, bfi_args[0],
				LLVMBuildXor(builder, bfi_args[1], bfi_args[2],
					     ""), ""), "");
}

/* this is ffs in C */
static void emit_lsb(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMValueRef args[2] = {
		emit_data->args[0],

		/* The value of 1 means that ffs(x=0) = undef, so LLVM won't
		 * add special code to check for x=0. The reason is that
		 * the LLVM behavior for x=0 is different from what we
		 * need here.
		 *
		 * The hardware already implements the correct behavior.
		 */
		lp_build_const_int32(gallivm, 1)
	};

	emit_data->output[emit_data->chan] =
		lp_build_intrinsic(gallivm->builder, "llvm.cttz.i32",
				emit_data->dst_type, args, ARRAY_SIZE(args),
				LLVMReadNoneAttribute);
}

/* Find the last bit set. */
static void emit_umsb(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef args[2] = {
		emit_data->args[0],
		/* Don't generate code for handling zero: */
		lp_build_const_int32(gallivm, 1)
	};

	LLVMValueRef msb =
		lp_build_intrinsic(builder, "llvm.ctlz.i32",
				emit_data->dst_type, args, ARRAY_SIZE(args),
				LLVMReadNoneAttribute);

	/* The HW returns the last bit index from MSB, but TGSI wants
	 * the index from LSB. Invert it by doing "31 - msb". */
	msb = LLVMBuildSub(builder, lp_build_const_int32(gallivm, 31),
			   msb, "");

	/* Check for zero: */
	emit_data->output[emit_data->chan] =
		LLVMBuildSelect(builder,
				LLVMBuildICmp(builder, LLVMIntEQ, args[0],
					      bld_base->uint_bld.zero, ""),
				lp_build_const_int32(gallivm, -1), msb, "");
}

/* Find the last bit opposite of the sign bit. */
static void emit_imsb(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	struct gallivm_state *gallivm = bld_base->base.gallivm;
	LLVMBuilderRef builder = gallivm->builder;
	LLVMValueRef arg = emit_data->args[0];

	LLVMValueRef msb =
		lp_build_intrinsic(builder, "llvm.AMDGPU.flbit.i32",
				emit_data->dst_type, &arg, 1,
				LLVMReadNoneAttribute);

	/* The HW returns the last bit index from MSB, but TGSI wants
	 * the index from LSB. Invert it by doing "31 - msb". */
	msb = LLVMBuildSub(builder, lp_build_const_int32(gallivm, 31),
			   msb, "");

	/* If arg == 0 || arg == -1 (0xffffffff), return -1. */
	LLVMValueRef all_ones = lp_build_const_int32(gallivm, -1);

	LLVMValueRef cond =
		LLVMBuildOr(builder,
			    LLVMBuildICmp(builder, LLVMIntEQ, arg,
					  bld_base->uint_bld.zero, ""),
			    LLVMBuildICmp(builder, LLVMIntEQ, arg,
					  all_ones, ""), "");

	emit_data->output[emit_data->chan] =
		LLVMBuildSelect(builder, cond, all_ones, msb, "");
}

static void emit_iabs(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;

	emit_data->output[emit_data->chan] =
		lp_build_emit_llvm_binary(bld_base, TGSI_OPCODE_IMAX,
					  emit_data->args[0],
					  LLVMBuildNeg(builder,
						       emit_data->args[0], ""));
}

static void emit_minmax_int(const struct lp_build_tgsi_action *action,
			    struct lp_build_tgsi_context *bld_base,
			    struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMIntPredicate op;

	switch (emit_data->info->opcode) {
	default:
		assert(0);
	case TGSI_OPCODE_IMAX:
		op = LLVMIntSGT;
		break;
	case TGSI_OPCODE_IMIN:
		op = LLVMIntSLT;
		break;
	case TGSI_OPCODE_UMAX:
		op = LLVMIntUGT;
		break;
	case TGSI_OPCODE_UMIN:
		op = LLVMIntULT;
		break;
	}

	emit_data->output[emit_data->chan] =
		LLVMBuildSelect(builder,
				LLVMBuildICmp(builder, op, emit_data->args[0],
					      emit_data->args[1], ""),
				emit_data->args[0],
				emit_data->args[1], "");
}

static void pk2h_fetch_args(struct lp_build_tgsi_context *bld_base,
			    struct lp_build_emit_data *emit_data)
{
	emit_data->args[0] = lp_build_emit_fetch(bld_base, emit_data->inst,
						 0, TGSI_CHAN_X);
	emit_data->args[1] = lp_build_emit_fetch(bld_base, emit_data->inst,
						 0, TGSI_CHAN_Y);
}

static void emit_pk2h(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMContextRef context = bld_base->base.gallivm->context;
	struct lp_build_context *uint_bld = &bld_base->uint_bld;
	LLVMTypeRef fp16, i16;
	LLVMValueRef const16, comp[2];
	unsigned i;

	fp16 = LLVMHalfTypeInContext(context);
	i16 = LLVMInt16TypeInContext(context);
	const16 = lp_build_const_int32(uint_bld->gallivm, 16);

	for (i = 0; i < 2; i++) {
		comp[i] = LLVMBuildFPTrunc(builder, emit_data->args[i], fp16, "");
		comp[i] = LLVMBuildBitCast(builder, comp[i], i16, "");
		comp[i] = LLVMBuildZExt(builder, comp[i], uint_bld->elem_type, "");
	}

	comp[1] = LLVMBuildShl(builder, comp[1], const16, "");
	comp[0] = LLVMBuildOr(builder, comp[0], comp[1], "");

	emit_data->output[emit_data->chan] = comp[0];
}

static void up2h_fetch_args(struct lp_build_tgsi_context *bld_base,
			    struct lp_build_emit_data *emit_data)
{
	emit_data->args[0] = lp_build_emit_fetch(bld_base, emit_data->inst,
						 0, TGSI_CHAN_X);
}

static void emit_up2h(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	LLVMBuilderRef builder = bld_base->base.gallivm->builder;
	LLVMContextRef context = bld_base->base.gallivm->context;
	struct lp_build_context *uint_bld = &bld_base->uint_bld;
	LLVMTypeRef fp16, i16;
	LLVMValueRef const16, input, val;
	unsigned i;

	fp16 = LLVMHalfTypeInContext(context);
	i16 = LLVMInt16TypeInContext(context);
	const16 = lp_build_const_int32(uint_bld->gallivm, 16);
	input = emit_data->args[0];

	for (i = 0; i < 2; i++) {
		val = i == 1 ? LLVMBuildLShr(builder, input, const16, "") : input;
		val = LLVMBuildTrunc(builder, val, i16, "");
		val = LLVMBuildBitCast(builder, val, fp16, "");
		emit_data->output[i] =
			LLVMBuildFPExt(builder, val, bld_base->base.elem_type, "");
	}
}

static void emit_fdiv(const struct lp_build_tgsi_action *action,
		      struct lp_build_tgsi_context *bld_base,
		      struct lp_build_emit_data *emit_data)
{
	struct radeon_llvm_context *ctx = radeon_llvm_context(bld_base);

	emit_data->output[emit_data->chan] =
		LLVMBuildFDiv(bld_base->base.gallivm->builder,
			      emit_data->args[0], emit_data->args[1], "");

	/* Use v_rcp_f32 instead of precise division. */
	if (HAVE_LLVM >= 0x0309 &&
	    !LLVMIsConstant(emit_data->output[emit_data->chan]))
		LLVMSetMetadata(emit_data->output[emit_data->chan],
				ctx->fpmath_md_kind, ctx->fpmath_md_2p5_ulp);
}

/* 1/sqrt is translated to rsq for f32 if fp32 denormals are not enabled in
 * the target machine. f64 needs global unsafe math flags to get rsq. */
static void emit_rsq(const struct lp_build_tgsi_action *action,
		     struct lp_build_tgsi_context *bld_base,
		     struct lp_build_emit_data *emit_data)
{
	LLVMValueRef sqrt =
		lp_build_emit_llvm_unary(bld_base, TGSI_OPCODE_SQRT,
					 emit_data->args[0]);

	emit_data->output[emit_data->chan] =
		lp_build_emit_llvm_binary(bld_base, TGSI_OPCODE_DIV,
					  bld_base->base.one, sqrt);
}

void radeon_llvm_context_init(struct radeon_llvm_context *ctx, const char *triple,
			      const struct tgsi_shader_info *info,
			      const struct tgsi_token *tokens)
{
	struct lp_type type;

	/* Initialize the gallivm object:
	 * We are only using the module, context, and builder fields of this struct.
	 * This should be enough for us to be able to pass our gallivm struct to the
	 * helper functions in the gallivm module.
	 */
	memset(&ctx->gallivm, 0, sizeof (ctx->gallivm));
	memset(&ctx->soa, 0, sizeof(ctx->soa));
	ctx->gallivm.context = LLVMContextCreate();
	ctx->gallivm.module = LLVMModuleCreateWithNameInContext("tgsi",
						ctx->gallivm.context);
	LLVMSetTarget(ctx->gallivm.module, triple);
	ctx->gallivm.builder = LLVMCreateBuilderInContext(ctx->gallivm.context);

	struct lp_build_tgsi_context *bld_base = &ctx->soa.bld_base;

	bld_base->info = info;

	if (info && info->array_max[TGSI_FILE_TEMPORARY] > 0) {
		int size = info->array_max[TGSI_FILE_TEMPORARY];

		ctx->temp_arrays = CALLOC(size, sizeof(ctx->temp_arrays[0]));
		ctx->temp_array_allocas = CALLOC(size, sizeof(ctx->temp_array_allocas[0]));

		if (tokens)
			tgsi_scan_arrays(tokens, TGSI_FILE_TEMPORARY, size,
					 ctx->temp_arrays);
	}

	type.floating = true;
	type.fixed = false;
	type.sign = true;
	type.norm = false;
	type.width = 32;
	type.length = 1;

	lp_build_context_init(&bld_base->base, &ctx->gallivm, type);
	lp_build_context_init(&ctx->soa.bld_base.uint_bld, &ctx->gallivm, lp_uint_type(type));
	lp_build_context_init(&ctx->soa.bld_base.int_bld, &ctx->gallivm, lp_int_type(type));
	{
		struct lp_type dbl_type;
		dbl_type = type;
		dbl_type.width *= 2;
		lp_build_context_init(&ctx->soa.bld_base.dbl_bld, &ctx->gallivm, dbl_type);
	}

	bld_base->soa = 1;
	bld_base->emit_store = radeon_llvm_emit_store;
	bld_base->emit_swizzle = emit_swizzle;
	bld_base->emit_declaration = emit_declaration;
	bld_base->emit_immediate = emit_immediate;

	bld_base->emit_fetch_funcs[TGSI_FILE_IMMEDIATE] = radeon_llvm_emit_fetch;
	bld_base->emit_fetch_funcs[TGSI_FILE_INPUT] = radeon_llvm_emit_fetch;
	bld_base->emit_fetch_funcs[TGSI_FILE_TEMPORARY] = radeon_llvm_emit_fetch;
	bld_base->emit_fetch_funcs[TGSI_FILE_OUTPUT] = radeon_llvm_emit_fetch;
	bld_base->emit_fetch_funcs[TGSI_FILE_SYSTEM_VALUE] = fetch_system_value;

	/* metadata allowing 2.5 ULP */
	ctx->fpmath_md_kind = LLVMGetMDKindIDInContext(ctx->gallivm.context,
						       "fpmath", 6);
	LLVMValueRef arg = lp_build_const_float(&ctx->gallivm, 2.5);
	ctx->fpmath_md_2p5_ulp = LLVMMDNodeInContext(ctx->gallivm.context,
						     &arg, 1);

	/* Allocate outputs */
	ctx->soa.outputs = ctx->outputs;

	lp_set_default_actions(bld_base);

	bld_base->op_actions[TGSI_OPCODE_ABS].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_ABS].intr_name = "llvm.fabs.f32";
	bld_base->op_actions[TGSI_OPCODE_AND].emit = emit_and;
	bld_base->op_actions[TGSI_OPCODE_ARL].emit = emit_arl;
	bld_base->op_actions[TGSI_OPCODE_BFI].emit = emit_bfi;
	bld_base->op_actions[TGSI_OPCODE_BGNLOOP].emit = bgnloop_emit;
	bld_base->op_actions[TGSI_OPCODE_BREV].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_BREV].intr_name =
		HAVE_LLVM >= 0x0308 ? "llvm.bitreverse.i32" : "llvm.AMDGPU.brev";
	bld_base->op_actions[TGSI_OPCODE_BRK].emit = brk_emit;
	bld_base->op_actions[TGSI_OPCODE_CEIL].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_CEIL].intr_name = "llvm.ceil.f32";
	bld_base->op_actions[TGSI_OPCODE_CLAMP].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_CLAMP].intr_name =
		HAVE_LLVM >= 0x0308 ? "llvm.AMDGPU.clamp." : "llvm.AMDIL.clamp.";
	bld_base->op_actions[TGSI_OPCODE_CMP].emit = emit_cmp;
	bld_base->op_actions[TGSI_OPCODE_CONT].emit = cont_emit;
	bld_base->op_actions[TGSI_OPCODE_COS].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_COS].intr_name = "llvm.cos.f32";
	bld_base->op_actions[TGSI_OPCODE_DABS].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_DABS].intr_name = "llvm.fabs.f64";
	bld_base->op_actions[TGSI_OPCODE_DFMA].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_DFMA].intr_name = "llvm.fma.f64";
	bld_base->op_actions[TGSI_OPCODE_DFRAC].emit = emit_frac;
	bld_base->op_actions[TGSI_OPCODE_DIV].emit = emit_fdiv;
	bld_base->op_actions[TGSI_OPCODE_DNEG].emit = emit_dneg;
	bld_base->op_actions[TGSI_OPCODE_DSEQ].emit = emit_dcmp;
	bld_base->op_actions[TGSI_OPCODE_DSGE].emit = emit_dcmp;
	bld_base->op_actions[TGSI_OPCODE_DSLT].emit = emit_dcmp;
	bld_base->op_actions[TGSI_OPCODE_DSNE].emit = emit_dcmp;
	bld_base->op_actions[TGSI_OPCODE_DRSQ].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_DRSQ].intr_name =
		HAVE_LLVM >= 0x0309 ? "llvm.amdgcn.rsq.f64" : "llvm.AMDGPU.rsq.f64";
	bld_base->op_actions[TGSI_OPCODE_DSQRT].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_DSQRT].intr_name = "llvm.sqrt.f64";
	bld_base->op_actions[TGSI_OPCODE_ELSE].emit = else_emit;
	bld_base->op_actions[TGSI_OPCODE_ENDIF].emit = endif_emit;
	bld_base->op_actions[TGSI_OPCODE_ENDLOOP].emit = endloop_emit;
	bld_base->op_actions[TGSI_OPCODE_EX2].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_EX2].intr_name =
		HAVE_LLVM >= 0x0308 ? "llvm.exp2.f32" : "llvm.AMDIL.exp.";
	bld_base->op_actions[TGSI_OPCODE_FLR].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_FLR].intr_name = "llvm.floor.f32";
	bld_base->op_actions[TGSI_OPCODE_FMA].emit =
		bld_base->op_actions[TGSI_OPCODE_MAD].emit;
	bld_base->op_actions[TGSI_OPCODE_FRC].emit = emit_frac;
	bld_base->op_actions[TGSI_OPCODE_F2I].emit = emit_f2i;
	bld_base->op_actions[TGSI_OPCODE_F2U].emit = emit_f2u;
	bld_base->op_actions[TGSI_OPCODE_FSEQ].emit = emit_fcmp;
	bld_base->op_actions[TGSI_OPCODE_FSGE].emit = emit_fcmp;
	bld_base->op_actions[TGSI_OPCODE_FSLT].emit = emit_fcmp;
	bld_base->op_actions[TGSI_OPCODE_FSNE].emit = emit_fcmp;
	bld_base->op_actions[TGSI_OPCODE_IABS].emit = emit_iabs;
	bld_base->op_actions[TGSI_OPCODE_IBFE].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_IBFE].intr_name = "llvm.AMDGPU.bfe.i32";
	bld_base->op_actions[TGSI_OPCODE_IDIV].emit = emit_idiv;
	bld_base->op_actions[TGSI_OPCODE_IF].emit = if_emit;
	bld_base->op_actions[TGSI_OPCODE_UIF].emit = uif_emit;
	bld_base->op_actions[TGSI_OPCODE_IMAX].emit = emit_minmax_int;
	bld_base->op_actions[TGSI_OPCODE_IMIN].emit = emit_minmax_int;
	bld_base->op_actions[TGSI_OPCODE_IMSB].emit = emit_imsb;
	bld_base->op_actions[TGSI_OPCODE_INEG].emit = emit_ineg;
	bld_base->op_actions[TGSI_OPCODE_ISHR].emit = emit_ishr;
	bld_base->op_actions[TGSI_OPCODE_ISGE].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_ISLT].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_ISSG].emit = emit_ssg;
	bld_base->op_actions[TGSI_OPCODE_I2F].emit = emit_i2f;
	bld_base->op_actions[TGSI_OPCODE_KILL_IF].fetch_args = kill_if_fetch_args;
	bld_base->op_actions[TGSI_OPCODE_KILL_IF].emit = kil_emit;
	bld_base->op_actions[TGSI_OPCODE_KILL_IF].intr_name = "llvm.AMDGPU.kill";
	bld_base->op_actions[TGSI_OPCODE_KILL].emit = lp_build_tgsi_intrinsic;
	bld_base->op_actions[TGSI_OPCODE_KILL].intr_name = "llvm.AMDGPU.kilp";
	bld_base->op_actions[TGSI_OPCODE_LSB].emit = emit_lsb;
	bld_base->op_actions[TGSI_OPCODE_LG2].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_LG2].intr_name = "llvm.log2.f32";
	bld_base->op_actions[TGSI_OPCODE_MOD].emit = emit_mod;
	bld_base->op_actions[TGSI_OPCODE_UMSB].emit = emit_umsb;
	bld_base->op_actions[TGSI_OPCODE_NOT].emit = emit_not;
	bld_base->op_actions[TGSI_OPCODE_OR].emit = emit_or;
	bld_base->op_actions[TGSI_OPCODE_PK2H].fetch_args = pk2h_fetch_args;
	bld_base->op_actions[TGSI_OPCODE_PK2H].emit = emit_pk2h;
	bld_base->op_actions[TGSI_OPCODE_POPC].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_POPC].intr_name = "llvm.ctpop.i32";
	bld_base->op_actions[TGSI_OPCODE_POW].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_POW].intr_name = "llvm.pow.f32";
	bld_base->op_actions[TGSI_OPCODE_ROUND].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_ROUND].intr_name = "llvm.rint.f32";
	bld_base->op_actions[TGSI_OPCODE_RSQ].emit = emit_rsq;
	bld_base->op_actions[TGSI_OPCODE_SGE].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SEQ].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SHL].emit = emit_shl;
	bld_base->op_actions[TGSI_OPCODE_SLE].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SLT].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SNE].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SGT].emit = emit_set_cond;
	bld_base->op_actions[TGSI_OPCODE_SIN].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_SIN].intr_name = "llvm.sin.f32";
	bld_base->op_actions[TGSI_OPCODE_SQRT].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_SQRT].intr_name = "llvm.sqrt.f32";
	bld_base->op_actions[TGSI_OPCODE_SSG].emit = emit_ssg;
	bld_base->op_actions[TGSI_OPCODE_TRUNC].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_TRUNC].intr_name = "llvm.trunc.f32";
	bld_base->op_actions[TGSI_OPCODE_UADD].emit = emit_uadd;
	bld_base->op_actions[TGSI_OPCODE_UBFE].emit = build_tgsi_intrinsic_nomem;
	bld_base->op_actions[TGSI_OPCODE_UBFE].intr_name = "llvm.AMDGPU.bfe.u32";
	bld_base->op_actions[TGSI_OPCODE_UDIV].emit = emit_udiv;
	bld_base->op_actions[TGSI_OPCODE_UMAX].emit = emit_minmax_int;
	bld_base->op_actions[TGSI_OPCODE_UMIN].emit = emit_minmax_int;
	bld_base->op_actions[TGSI_OPCODE_UMOD].emit = emit_umod;
	bld_base->op_actions[TGSI_OPCODE_USEQ].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_USGE].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_USHR].emit = emit_ushr;
	bld_base->op_actions[TGSI_OPCODE_USLT].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_USNE].emit = emit_icmp;
	bld_base->op_actions[TGSI_OPCODE_U2F].emit = emit_u2f;
	bld_base->op_actions[TGSI_OPCODE_XOR].emit = emit_xor;
	bld_base->op_actions[TGSI_OPCODE_UCMP].emit = emit_ucmp;
	bld_base->op_actions[TGSI_OPCODE_UP2H].fetch_args = up2h_fetch_args;
	bld_base->op_actions[TGSI_OPCODE_UP2H].emit = emit_up2h;
}

void radeon_llvm_create_func(struct radeon_llvm_context *ctx,
			     LLVMTypeRef *return_types, unsigned num_return_elems,
			     LLVMTypeRef *ParamTypes, unsigned ParamCount)
{
	LLVMTypeRef main_fn_type, ret_type;
	LLVMBasicBlockRef main_fn_body;

	if (num_return_elems)
		ret_type = LLVMStructTypeInContext(ctx->gallivm.context,
						   return_types,
						   num_return_elems, true);
	else
		ret_type = LLVMVoidTypeInContext(ctx->gallivm.context);

	/* Setup the function */
	ctx->return_type = ret_type;
	main_fn_type = LLVMFunctionType(ret_type, ParamTypes, ParamCount, 0);
	ctx->main_fn = LLVMAddFunction(ctx->gallivm.module, "main", main_fn_type);
	main_fn_body = LLVMAppendBasicBlockInContext(ctx->gallivm.context,
			ctx->main_fn, "main_body");
	LLVMPositionBuilderAtEnd(ctx->gallivm.builder, main_fn_body);
}

void radeon_llvm_finalize_module(struct radeon_llvm_context *ctx)
{
	struct gallivm_state *gallivm = ctx->soa.bld_base.base.gallivm;
	const char *triple = LLVMGetTarget(gallivm->module);
	LLVMTargetLibraryInfoRef target_library_info;

	/* Create the pass manager */
	gallivm->passmgr = LLVMCreateFunctionPassManagerForModule(
							gallivm->module);

	target_library_info = gallivm_create_target_library_info(triple);
	LLVMAddTargetLibraryInfo(target_library_info, gallivm->passmgr);

	/* This pass should eliminate all the load and store instructions */
	LLVMAddPromoteMemoryToRegisterPass(gallivm->passmgr);

	/* Add some optimization passes */
	LLVMAddScalarReplAggregatesPass(gallivm->passmgr);
	LLVMAddLICMPass(gallivm->passmgr);
	LLVMAddAggressiveDCEPass(gallivm->passmgr);
	LLVMAddCFGSimplificationPass(gallivm->passmgr);
	LLVMAddInstructionCombiningPass(gallivm->passmgr);

	/* Run the pass */
	LLVMInitializeFunctionPassManager(gallivm->passmgr);
	LLVMRunFunctionPassManager(gallivm->passmgr, ctx->main_fn);
	LLVMFinalizeFunctionPassManager(gallivm->passmgr);

	LLVMDisposeBuilder(gallivm->builder);
	LLVMDisposePassManager(gallivm->passmgr);
	gallivm_dispose_target_library_info(target_library_info);
}

void radeon_llvm_dispose(struct radeon_llvm_context *ctx)
{
	LLVMDisposeModule(ctx->soa.bld_base.base.gallivm->module);
	LLVMContextDispose(ctx->soa.bld_base.base.gallivm->context);
	FREE(ctx->temp_arrays);
	ctx->temp_arrays = NULL;
	FREE(ctx->temp_array_allocas);
	ctx->temp_array_allocas = NULL;
	FREE(ctx->temps);
	ctx->temps = NULL;
	ctx->temps_count = 0;
	FREE(ctx->loop);
	ctx->loop = NULL;
	ctx->loop_depth_max = 0;
	FREE(ctx->branch);
	ctx->branch = NULL;
	ctx->branch_depth_max = 0;
}
