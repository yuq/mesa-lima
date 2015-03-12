/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright (C) 2015 Rob Clark <robclark@freedesktop.org>
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
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <stdarg.h>

#include "pipe/p_state.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "tgsi/tgsi_lowering.h"
#include "tgsi/tgsi_strings.h"

#include "nir/tgsi_to_nir.h"
#include "glsl/shader_enums.h"

#include "freedreno_util.h"

#include "ir3_compiler.h"
#include "ir3_shader.h"

#include "instr-a3xx.h"
#include "ir3.h"


static struct ir3_instruction * create_immed(struct ir3_block *block, uint32_t val);

struct ir3_compile {
	const struct tgsi_token *tokens;
	struct nir_shader *s;

	struct ir3 *ir;
	struct ir3_shader_variant *so;

	/* bitmask of which samplers are integer: */
	uint16_t integer_s;

	struct ir3_block *block;

	/* For fragment shaders, from the hw perspective the only
	 * actual input is r0.xy position register passed to bary.f.
	 * But TGSI doesn't know that, it still declares things as
	 * IN[] registers.  So we do all the input tracking normally
	 * and fix things up after compile_instructions()
	 *
	 * NOTE that frag_pos is the hardware position (possibly it
	 * is actually an index or tag or some such.. it is *not*
	 * values that can be directly used for gl_FragCoord..)
	 */
	struct ir3_instruction *frag_pos, *frag_face, *frag_coord[4];

	/* For vertex shaders, keep track of the system values sources */
	struct ir3_instruction *vertex_id, *basevertex, *instance_id;

	/* mapping from nir_register to defining instruction: */
	struct hash_table *def_ht;

	/* a common pattern for indirect addressing is to request the
	 * same address register multiple times.  To avoid generating
	 * duplicate instruction sequences (which our backend does not
	 * try to clean up, since that should be done as the NIR stage)
	 * we cache the address value generated for a given src value:
	 */
	struct hash_table *addr_ht;

	/* for calculating input/output positions/linkages: */
	unsigned next_inloc;

	/* a4xx (at least patchlevel 0) cannot seem to flat-interpolate
	 * so we need to use ldlv.u32 to load the varying directly:
	 */
	bool flat_bypass;

	/* for looking up which system value is which */
	unsigned sysval_semantics[8];

	/* list of kill instructions: */
	struct ir3_instruction *kill[16];
	unsigned int kill_count;

	/* set if we encounter something we can't handle yet, so we
	 * can bail cleanly and fallback to TGSI compiler f/e
	 */
	bool error;
};


static struct nir_shader *to_nir(const struct tgsi_token *tokens)
{
	struct nir_shader_compiler_options options = {
			.lower_fpow = true,
			.lower_fsat = true,
			.lower_scmp = true,
			.lower_flrp = true,
			.native_integers = true,
	};
	bool progress;

	struct nir_shader *s = tgsi_to_nir(tokens, &options);

	if (fd_mesa_debug & FD_DBG_OPTMSGS) {
		debug_printf("----------------------\n");
		nir_print_shader(s, stdout);
		debug_printf("----------------------\n");
	}

	nir_opt_global_to_local(s);
	nir_convert_to_ssa(s);
	nir_lower_idiv(s);

	do {
		progress = false;

		nir_lower_vars_to_ssa(s);
		nir_lower_alu_to_scalar(s);

		progress |= nir_copy_prop(s);
		progress |= nir_opt_dce(s);
		progress |= nir_opt_cse(s);
		progress |= nir_opt_peephole_select(s);
		progress |= nir_opt_algebraic(s);
		progress |= nir_opt_constant_folding(s);

	} while (progress);

	nir_remove_dead_variables(s);
	nir_validate_shader(s);

	if (fd_mesa_debug & FD_DBG_OPTMSGS) {
		debug_printf("----------------------\n");
		nir_print_shader(s, stdout);
		debug_printf("----------------------\n");
	}

	return s;
}

/* TODO nir doesn't lower everything for us yet, but ideally it would: */
static const struct tgsi_token *
lower_tgsi(const struct tgsi_token *tokens, struct ir3_shader_variant *so)
{
	struct tgsi_shader_info info;
	struct tgsi_lowering_config lconfig = {
			.color_two_side = so->key.color_two_side,
			.lower_FRC = true,
	};

	switch (so->type) {
	case SHADER_FRAGMENT:
	case SHADER_COMPUTE:
		lconfig.saturate_s = so->key.fsaturate_s;
		lconfig.saturate_t = so->key.fsaturate_t;
		lconfig.saturate_r = so->key.fsaturate_r;
		break;
	case SHADER_VERTEX:
		lconfig.saturate_s = so->key.vsaturate_s;
		lconfig.saturate_t = so->key.vsaturate_t;
		lconfig.saturate_r = so->key.vsaturate_r;
		break;
	}

	if (!so->shader) {
		/* hack for standalone compiler which does not have
		 * screen/context:
		 */
	} else if (ir3_shader_gpuid(so->shader) >= 400) {
		/* a4xx seems to have *no* sam.p */
		lconfig.lower_TXP = ~0;  /* lower all txp */
	} else {
		/* a3xx just needs to avoid sam.p for 3d tex */
		lconfig.lower_TXP = (1 << TGSI_TEXTURE_3D);
	}

	return tgsi_transform_lowering(&lconfig, tokens, &info);
}

static struct ir3_compile *
compile_init(struct ir3_shader_variant *so,
		const struct tgsi_token *tokens)
{
	struct ir3_compile *ctx = rzalloc(NULL, struct ir3_compile);
	const struct tgsi_token *lowered_tokens;

	if (!so->shader) {
		/* hack for standalone compiler which does not have
		 * screen/context:
		 */
	} else if (ir3_shader_gpuid(so->shader) >= 400) {
		/* need special handling for "flat" */
		ctx->flat_bypass = true;
	} else {
		/* no special handling for "flat" */
		ctx->flat_bypass = false;
	}

	switch (so->type) {
	case SHADER_FRAGMENT:
	case SHADER_COMPUTE:
		ctx->integer_s = so->key.finteger_s;
		break;
	case SHADER_VERTEX:
		ctx->integer_s = so->key.vinteger_s;
		break;
	}

	ctx->ir = so->ir;
	ctx->so = so;
	ctx->next_inloc = 8;
	ctx->def_ht = _mesa_hash_table_create(ctx,
			_mesa_hash_pointer, _mesa_key_pointer_equal);
	ctx->addr_ht = _mesa_hash_table_create(ctx,
			_mesa_hash_pointer, _mesa_key_pointer_equal);

	lowered_tokens = lower_tgsi(tokens, so);
	if (!lowered_tokens)
		lowered_tokens = tokens;
	ctx->s = to_nir(lowered_tokens);

	if (lowered_tokens != tokens)
		free((void *)lowered_tokens);

	so->first_immediate = ctx->s->num_uniforms;

	return ctx;
}

static void
compile_error(struct ir3_compile *ctx, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	_debug_vprintf(format, ap);
	va_end(ap);
	nir_print_shader(ctx->s, stdout);
	ctx->error = true;
}

#define compile_assert(ctx, cond) do { \
		if (!(cond)) compile_error((ctx), "failed assert: "#cond"\n"); \
	} while (0)

static void
compile_free(struct ir3_compile *ctx)
{
	ralloc_free(ctx);
}

/* allocate a n element value array (to be populated by caller) and
 * insert in def_ht
 */
static struct ir3_instruction **
__get_dst(struct ir3_compile *ctx, void *key, unsigned n)
{
	struct ir3_instruction **value =
		ralloc_array(ctx->def_ht, struct ir3_instruction *, n);
	_mesa_hash_table_insert(ctx->def_ht, key, value);
	return value;
}

static struct ir3_instruction **
get_dst(struct ir3_compile *ctx, nir_dest *dst, unsigned n)
{
	if (dst->is_ssa) {
		return __get_dst(ctx, &dst->ssa, n);
	} else {
		return __get_dst(ctx, dst->reg.reg, n);
	}
}

static struct ir3_instruction **
get_dst_ssa(struct ir3_compile *ctx, nir_ssa_def *dst, unsigned n)
{
	return __get_dst(ctx, dst, n);
}

static struct ir3_instruction **
get_src(struct ir3_compile *ctx, nir_src *src)
{
	struct hash_entry *entry;
	if (src->is_ssa) {
		entry = _mesa_hash_table_search(ctx->def_ht, src->ssa);
	} else {
		entry = _mesa_hash_table_search(ctx->def_ht, src->reg.reg);
	}
	compile_assert(ctx, entry);
	return entry->data;
}

static struct ir3_instruction *
create_immed(struct ir3_block *block, uint32_t val)
{
	struct ir3_instruction *mov;

	mov = ir3_instr_create(block, 1, 0);
	mov->cat1.src_type = TYPE_U32;
	mov->cat1.dst_type = TYPE_U32;
	ir3_reg_create(mov, 0, 0);
	ir3_reg_create(mov, 0, IR3_REG_IMMED)->uim_val = val;

	return mov;
}

static struct ir3_instruction *
create_addr(struct ir3_block *block, struct ir3_instruction *src)
{
	struct ir3_instruction *instr, *immed;

	/* TODO in at least some cases, the backend could probably be
	 * made clever enough to propagate IR3_REG_HALF..
	 */
	instr = ir3_COV(block, src, TYPE_U32, TYPE_S16);
	instr->regs[0]->flags |= IR3_REG_HALF;

	immed = create_immed(block, 2);
	immed->regs[0]->flags |= IR3_REG_HALF;

	instr = ir3_SHL_B(block, instr, 0, immed, 0);
	instr->regs[0]->flags |= IR3_REG_HALF;
	instr->regs[1]->flags |= IR3_REG_HALF;

	instr = ir3_MOV(block, instr, TYPE_S16);
	instr->regs[0]->flags |= IR3_REG_ADDR | IR3_REG_HALF;
	instr->regs[1]->flags |= IR3_REG_HALF;

	return instr;
}

/* caches addr values to avoid generating multiple cov/shl/mova
 * sequences for each use of a given NIR level src as address
 */
static struct ir3_instruction *
get_addr(struct ir3_compile *ctx, struct ir3_instruction *src)
{
	struct ir3_instruction *addr;
	struct hash_entry *entry;
	entry = _mesa_hash_table_search(ctx->addr_ht, src);
	if (entry)
		return entry->data;

	/* TODO do we need to cache per block? */
	addr = create_addr(ctx->block, src);
	_mesa_hash_table_insert(ctx->addr_ht, src, addr);

	return addr;
}

static struct ir3_instruction *
create_uniform(struct ir3_block *block, unsigned n)
{
	struct ir3_instruction *mov;

	mov = ir3_instr_create(block, 1, 0);
	/* TODO get types right? */
	mov->cat1.src_type = TYPE_F32;
	mov->cat1.dst_type = TYPE_F32;
	ir3_reg_create(mov, 0, 0);
	ir3_reg_create(mov, n, IR3_REG_CONST);

	return mov;
}

static struct ir3_instruction *
create_uniform_indirect(struct ir3_block *block, unsigned n,
		struct ir3_instruction *address)
{
	struct ir3_instruction *mov;

	mov = ir3_instr_create(block, 1, 0);
	mov->cat1.src_type = TYPE_U32;
	mov->cat1.dst_type = TYPE_U32;
	ir3_reg_create(mov, 0, 0);
	ir3_reg_create(mov, n, IR3_REG_CONST | IR3_REG_RELATIV);
	mov->address = address;

	return mov;
}

static struct ir3_instruction *
create_indirect(struct ir3_block *block, struct ir3_instruction **arr,
		unsigned arrsz, unsigned n, struct ir3_instruction *address)
{
	struct ir3_instruction *mov, *collect;
	struct ir3_register *src;

	collect = ir3_instr_create2(block, -1, OPC_META_FI, 1 + arrsz);
	ir3_reg_create(collect, 0, 0);
	for (unsigned i = 0; i < arrsz; i++)
		ir3_reg_create(collect, 0, IR3_REG_SSA)->instr = arr[i];

	mov = ir3_instr_create(block, 1, 0);
	mov->cat1.src_type = TYPE_U32;
	mov->cat1.dst_type = TYPE_U32;
	ir3_reg_create(mov, 0, 0);
	src = ir3_reg_create(mov, 0, IR3_REG_SSA | IR3_REG_RELATIV);
	src->instr = collect;
	src->size  = arrsz;
	mov->address = address;

	return mov;
}

static struct ir3_instruction *
create_input(struct ir3_block *block, struct ir3_instruction *instr,
		unsigned n)
{
	struct ir3_instruction *in;

	in = ir3_instr_create(block, -1, OPC_META_INPUT);
	in->inout.block = block;
	ir3_reg_create(in, n, 0);
	if (instr)
		ir3_reg_create(in, 0, IR3_REG_SSA)->instr = instr;

	return in;
}

static struct ir3_instruction *
create_frag_input(struct ir3_compile *ctx, unsigned n, bool use_ldlv)
{
	struct ir3_block *block = ctx->block;
	struct ir3_instruction *instr;
	struct ir3_instruction *inloc = create_immed(block, n);

	if (use_ldlv) {
		instr = ir3_LDLV(block, inloc, 0, create_immed(block, 1), 0);
		instr->cat6.type = TYPE_U32;
		instr->cat6.iim_val = 1;
	} else {
		instr = ir3_BARY_F(block, inloc, 0, ctx->frag_pos, 0);
		instr->regs[2]->wrmask = 0x3;
	}

	return instr;
}

static struct ir3_instruction *
create_frag_coord(struct ir3_compile *ctx, unsigned comp)
{
	struct ir3_block *block = ctx->block;
	struct ir3_instruction *instr;

	compile_assert(ctx, !ctx->frag_coord[comp]);

	ctx->frag_coord[comp] = create_input(ctx->block, NULL, 0);

	switch (comp) {
	case 0: /* .x */
	case 1: /* .y */
		/* for frag_coord, we get unsigned values.. we need
		 * to subtract (integer) 8 and divide by 16 (right-
		 * shift by 4) then convert to float:
		 *
		 *    add.s tmp, src, -8
		 *    shr.b tmp, tmp, 4
		 *    mov.u32f32 dst, tmp
		 *
		 */
		instr = ir3_ADD_S(block, ctx->frag_coord[comp], 0,
				create_immed(block, -8), 0);
		instr = ir3_SHR_B(block, instr, 0,
				create_immed(block, 4), 0);
		instr = ir3_COV(block, instr, TYPE_U32, TYPE_F32);

		return instr;
	case 2: /* .z */
	case 3: /* .w */
	default:
		/* seems that we can use these as-is: */
		return ctx->frag_coord[comp];
	}
}

static struct ir3_instruction *
create_frag_face(struct ir3_compile *ctx, unsigned comp)
{
	struct ir3_block *block = ctx->block;
	struct ir3_instruction *instr;

	switch (comp) {
	case 0: /* .x */
		compile_assert(ctx, !ctx->frag_face);

		ctx->frag_face = create_input(block, NULL, 0);

		/* for faceness, we always get -1 or 0 (int).. but TGSI expects
		 * positive vs negative float.. and piglit further seems to
		 * expect -1.0 or 1.0:
		 *
		 *    mul.s tmp, hr0.x, 2
		 *    add.s tmp, tmp, 1
		 *    mov.s32f32, dst, tmp
		 *
		 */
		instr = ir3_MUL_S(block, ctx->frag_face, 0,
				create_immed(block, 2), 0);
		instr = ir3_ADD_S(block, instr, 0,
				create_immed(block, 1), 0);
		instr = ir3_COV(block, instr, TYPE_S32, TYPE_F32);

		return instr;
	case 1: /* .y */
	case 2: /* .z */
		return create_immed(block, fui(0.0));
	default:
	case 3: /* .w */
		return create_immed(block, fui(1.0));
	}
}

/*
 * Adreno uses uint rather than having dedicated bool type,
 * which (potentially) requires some conversion, in particular
 * when using output of an bool instr to int input, or visa
 * versa.
 *
 *         | Adreno  |  NIR  |
 *  -------+---------+-------+-
 *   true  |    1    |  ~0   |
 *   false |    0    |   0   |
 *
 * To convert from an adreno bool (uint) to nir, use:
 *
 *    absneg.s dst, (neg)src
 *
 * To convert back in the other direction:
 *
 *    absneg.s dst, (abs)arc
 *
 * The CP step can clean up the absneg.s that cancel each other
 * out, and with a slight bit of extra cleverness (to recognize
 * the instructions which produce either a 0 or 1) can eliminate
 * the absneg.s's completely when an instruction that wants
 * 0/1 consumes the result.  For example, when a nir 'bcsel'
 * consumes the result of 'feq'.  So we should be able to get by
 * without a boolean resolve step, and without incuring any
 * extra penalty in instruction count.
 */

/* NIR bool -> native (adreno): */
static struct ir3_instruction *
ir3_b2n(struct ir3_block *block, struct ir3_instruction *instr)
{
	return ir3_ABSNEG_S(block, instr, IR3_REG_SABS);
}

/* native (adreno) -> NIR bool: */
static struct ir3_instruction *
ir3_n2b(struct ir3_block *block, struct ir3_instruction *instr)
{
	return ir3_ABSNEG_S(block, instr, IR3_REG_SNEG);
}

/*
 * alu/sfu instructions:
 */

static void
emit_alu(struct ir3_compile *ctx, nir_alu_instr *alu)
{
	const nir_op_info *info = &nir_op_infos[alu->op];
	struct ir3_instruction **dst, *src[info->num_inputs];
	struct ir3_block *b = ctx->block;

	dst = get_dst(ctx, &alu->dest.dest, MAX2(info->output_size, 1));

	/* Vectors are special in that they have non-scalarized writemasks,
	 * and just take the first swizzle channel for each argument in
	 * order into each writemask channel.
	 */
	if ((alu->op == nir_op_vec2) ||
			(alu->op == nir_op_vec3) ||
			(alu->op == nir_op_vec4)) {

		for (int i = 0; i < info->num_inputs; i++) {
			nir_alu_src *asrc = &alu->src[i];

			compile_assert(ctx, !asrc->abs);
			compile_assert(ctx, !asrc->negate);

			src[i] = get_src(ctx, &asrc->src)[asrc->swizzle[0]];
			dst[i] = ir3_MOV(b, src[i], TYPE_U32);
		}

		return;
	}

	/* General case: We can just grab the one used channel per src. */
	for (int i = 0; i < info->num_inputs; i++) {
		unsigned chan = ffs(alu->dest.write_mask) - 1;
		nir_alu_src *asrc = &alu->src[i];

		compile_assert(ctx, !asrc->abs);
		compile_assert(ctx, !asrc->negate);

		src[i] = get_src(ctx, &asrc->src)[asrc->swizzle[chan]];
	}

	switch (alu->op) {
	case nir_op_f2i:
		dst[0] = ir3_COV(b, src[0], TYPE_F32, TYPE_S32);
		break;
	case nir_op_f2u:
		dst[0] = ir3_COV(b, src[0], TYPE_F32, TYPE_U32);
		break;
	case nir_op_i2f:
		dst[0] = ir3_COV(b, src[0], TYPE_S32, TYPE_F32);
		break;
	case nir_op_u2f:
		dst[0] = ir3_COV(b, src[0], TYPE_U32, TYPE_F32);
		break;
	case nir_op_imov:
		dst[0] = ir3_MOV(b, src[0], TYPE_S32);
		break;
	case nir_op_f2b:
		dst[0] = ir3_CMPS_F(b, src[0], 0, create_immed(b, fui(0.0)), 0);
		dst[0]->cat2.condition = IR3_COND_NE;
		dst[0] = ir3_n2b(b, dst[0]);
		break;
	case nir_op_b2f:
		dst[0] = ir3_COV(b, ir3_b2n(b, src[0]), TYPE_U32, TYPE_F32);
		break;
	case nir_op_b2i:
		dst[0] = ir3_b2n(b, src[0]);
		break;
	case nir_op_i2b:
		dst[0] = ir3_CMPS_S(b, src[0], 0, create_immed(b, 0), 0);
		dst[0]->cat2.condition = IR3_COND_NE;
		dst[0] = ir3_n2b(b, dst[0]);
		break;

	case nir_op_fneg:
		dst[0] = ir3_ABSNEG_F(b, src[0], IR3_REG_FNEG);
		break;
	case nir_op_fabs:
		dst[0] = ir3_ABSNEG_F(b, src[0], IR3_REG_FABS);
		break;
	case nir_op_fmax:
		dst[0] = ir3_MAX_F(b, src[0], 0, src[1], 0);
		break;
	case nir_op_fmin:
		dst[0] = ir3_MIN_F(b, src[0], 0, src[1], 0);
		break;
	case nir_op_fmul:
		dst[0] = ir3_MUL_F(b, src[0], 0, src[1], 0);
		break;
	case nir_op_fadd:
		dst[0] = ir3_ADD_F(b, src[0], 0, src[1], 0);
		break;
	case nir_op_fsub:
		dst[0] = ir3_ADD_F(b, src[0], 0, src[1], IR3_REG_FNEG);
		break;
	case nir_op_ffma:
		dst[0] = ir3_MAD_F32(b, src[0], 0, src[1], 0, src[2], 0);
		break;
	case nir_op_fddx:
		dst[0] = ir3_DSX(b, src[0], 0);
		dst[0]->cat5.type = TYPE_F32;
		break;
	case nir_op_fddy:
		dst[0] = ir3_DSY(b, src[0], 0);
		dst[0]->cat5.type = TYPE_F32;
		break;
		break;
	case nir_op_flt:
		dst[0] = ir3_CMPS_F(b, src[0], 0, src[1], 0);
		dst[0]->cat2.condition = IR3_COND_LT;
		dst[0] = ir3_n2b(b, dst[0]);
		break;
	case nir_op_fge:
		dst[0] = ir3_CMPS_F(b, src[0], 0, src[1], 0);
		dst[0]->cat2.condition = IR3_COND_GE;
		dst[0] = ir3_n2b(b, dst[0]);
		break;
	case nir_op_feq:
		dst[0] = ir3_CMPS_F(b, src[0], 0, src[1], 0);
		dst[0]->cat2.condition = IR3_COND_EQ;
		dst[0] = ir3_n2b(b, dst[0]);
		break;
	case nir_op_fne:
		dst[0] = ir3_CMPS_F(b, src[0], 0, src[1], 0);
		dst[0]->cat2.condition = IR3_COND_NE;
		dst[0] = ir3_n2b(b, dst[0]);
		break;
	case nir_op_fceil:
		dst[0] = ir3_CEIL_F(b, src[0], 0);
		break;
	case nir_op_ffloor:
		dst[0] = ir3_FLOOR_F(b, src[0], 0);
		break;
	case nir_op_ftrunc:
		dst[0] = ir3_TRUNC_F(b, src[0], 0);
		break;
	case nir_op_fround_even:
		dst[0] = ir3_RNDNE_F(b, src[0], 0);
		break;
	case nir_op_fsign:
		dst[0] = ir3_SIGN_F(b, src[0], 0);
		break;

	case nir_op_fsin:
		dst[0] = ir3_SIN(b, src[0], 0);
		break;
	case nir_op_fcos:
		dst[0] = ir3_COS(b, src[0], 0);
		break;
	case nir_op_frsq:
		dst[0] = ir3_RSQ(b, src[0], 0);
		break;
	case nir_op_frcp:
		dst[0] = ir3_RCP(b, src[0], 0);
		break;
	case nir_op_flog2:
		dst[0] = ir3_LOG2(b, src[0], 0);
		break;
	case nir_op_fexp2:
		dst[0] = ir3_EXP2(b, src[0], 0);
		break;
	case nir_op_fsqrt:
		dst[0] = ir3_SQRT(b, src[0], 0);
		break;

	case nir_op_iabs:
		dst[0] = ir3_ABSNEG_S(b, src[0], IR3_REG_SABS);
		break;
	case nir_op_iadd:
		dst[0] = ir3_ADD_U(b, src[0], 0, src[1], 0);
		break;
	case nir_op_iand:
		dst[0] = ir3_AND_B(b, src[0], 0, src[1], 0);
		break;
	case nir_op_imax:
		dst[0] = ir3_MAX_S(b, src[0], 0, src[1], 0);
		break;
	case nir_op_imin:
		dst[0] = ir3_MIN_S(b, src[0], 0, src[1], 0);
		break;
	case nir_op_imul:
		/*
		 * dst = (al * bl) + (ah * bl << 16) + (al * bh << 16)
		 *   mull.u tmp0, a, b           ; mul low, i.e. al * bl
		 *   madsh.m16 tmp1, a, b, tmp0  ; mul-add shift high mix, i.e. ah * bl << 16
		 *   madsh.m16 dst, b, a, tmp1   ; i.e. al * bh << 16
		 */
		dst[0] = ir3_MADSH_M16(b, src[1], 0, src[0], 0,
					ir3_MADSH_M16(b, src[0], 0, src[1], 0,
						ir3_MULL_U(b, src[0], 0, src[1], 0), 0), 0);
		break;
	case nir_op_ineg:
		dst[0] = ir3_ABSNEG_S(b, src[0], IR3_REG_SNEG);
		break;
	case nir_op_inot:
		dst[0] = ir3_NOT_B(b, src[0], 0);
		break;
	case nir_op_ior:
		dst[0] = ir3_OR_B(b, src[0], 0, src[1], 0);
		break;
	case nir_op_ishl:
		dst[0] = ir3_SHL_B(b, src[0], 0, src[1], 0);
		break;
	case nir_op_ishr:
		dst[0] = ir3_ASHR_B(b, src[0], 0, src[1], 0);
		break;
	case nir_op_isign: {
		/* maybe this would be sane to lower in nir.. */
		struct ir3_instruction *neg, *pos;

		neg = ir3_CMPS_S(b, src[0], 0, create_immed(b, 0), 0);
		neg->cat2.condition = IR3_COND_LT;

		pos = ir3_CMPS_S(b, src[0], 0, create_immed(b, 0), 0);
		pos->cat2.condition = IR3_COND_GT;

		dst[0] = ir3_SUB_U(b, pos, 0, neg, 0);

		break;
	}
	case nir_op_isub:
		dst[0] = ir3_SUB_U(b, src[0], 0, src[1], 0);
		break;
	case nir_op_ixor:
		dst[0] = ir3_XOR_B(b, src[0], 0, src[1], 0);
		break;
	case nir_op_ushr:
		dst[0] = ir3_SHR_B(b, src[0], 0, src[1], 0);
		break;
	case nir_op_ilt:
		dst[0] = ir3_CMPS_S(b, src[0], 0, src[1], 0);
		dst[0]->cat2.condition = IR3_COND_LT;
		dst[0] = ir3_n2b(b, dst[0]);
		break;
	case nir_op_ige:
		dst[0] = ir3_CMPS_S(b, src[0], 0, src[1], 0);
		dst[0]->cat2.condition = IR3_COND_GE;
		dst[0] = ir3_n2b(b, dst[0]);
		break;
	case nir_op_ieq:
		dst[0] = ir3_CMPS_S(b, src[0], 0, src[1], 0);
		dst[0]->cat2.condition = IR3_COND_EQ;
		dst[0] = ir3_n2b(b, dst[0]);
		break;
	case nir_op_ine:
		dst[0] = ir3_CMPS_S(b, src[0], 0, src[1], 0);
		dst[0]->cat2.condition = IR3_COND_NE;
		dst[0] = ir3_n2b(b, dst[0]);
		break;
	case nir_op_ult:
		dst[0] = ir3_CMPS_U(b, src[0], 0, src[1], 0);
		dst[0]->cat2.condition = IR3_COND_LT;
		dst[0] = ir3_n2b(b, dst[0]);
		break;
	case nir_op_uge:
		dst[0] = ir3_CMPS_U(b, src[0], 0, src[1], 0);
		dst[0]->cat2.condition = IR3_COND_GE;
		dst[0] = ir3_n2b(b, dst[0]);
		break;

	case nir_op_bcsel:
		dst[0] = ir3_SEL_B32(b, src[1], 0, ir3_b2n(b, src[0]), 0, src[2], 0);
		break;

	default:
		compile_error(ctx, "Unhandled ALU op: %s\n",
				nir_op_infos[alu->op].name);
		break;
	}
}

static void
emit_intrinisic(struct ir3_compile *ctx, nir_intrinsic_instr *intr)
{
	const nir_intrinsic_info *info = &nir_intrinsic_infos[intr->intrinsic];
	struct ir3_instruction **dst, **src;
	struct ir3_block *b = ctx->block;
	unsigned idx = intr->const_index[0];

	if (info->has_dest) {
		dst = get_dst(ctx, &intr->dest, intr->num_components);
	}

	switch (intr->intrinsic) {
	case nir_intrinsic_load_uniform:
		compile_assert(ctx, intr->const_index[1] == 1);
		for (int i = 0; i < intr->num_components; i++) {
			unsigned n = idx * 4 + i;
			dst[i] = create_uniform(b, n);
		}
		break;
	case nir_intrinsic_load_uniform_indirect:
		compile_assert(ctx, intr->const_index[1] == 1);
		src = get_src(ctx, &intr->src[0]);
		for (int i = 0; i < intr->num_components; i++) {
			unsigned n = idx * 4 + i;
			dst[i] = create_uniform_indirect(b, n,
					get_addr(ctx, src[0]));
		}
		break;
	case nir_intrinsic_load_input:
		compile_assert(ctx, intr->const_index[1] == 1);
		for (int i = 0; i < intr->num_components; i++) {
			unsigned n = idx * 4 + i;
			dst[i] = b->inputs[n];
		}
		break;
	case nir_intrinsic_load_input_indirect:
		compile_assert(ctx, intr->const_index[1] == 1);
		src = get_src(ctx, &intr->src[0]);
		for (int i = 0; i < intr->num_components; i++) {
			unsigned n = idx * 4 + i;
			dst[i] = create_indirect(b, b->inputs, b->ninputs, n,
					get_addr(ctx, src[i]));
		}
		break;
	case nir_intrinsic_store_output:
		compile_assert(ctx, intr->const_index[1] == 1);
		src = get_src(ctx, &intr->src[0]);
		for (int i = 0; i < intr->num_components; i++) {
			unsigned n = idx * 4 + i;
			b->outputs[n] = src[i];
		}
		break;
	case nir_intrinsic_discard_if:
	case nir_intrinsic_discard: {
		struct ir3_instruction *cond, *kill;

		if (intr->intrinsic == nir_intrinsic_discard_if) {
			/* conditional discard: */
			src = get_src(ctx, &intr->src[0]);
			cond = ir3_b2n(b, src[0]);
		} else {
			/* unconditional discard: */
			cond = create_immed(b, 1);
		}

		cond = ir3_CMPS_S(b, cond, 0, create_immed(b, 0), 0);
		cond->cat2.condition = IR3_COND_NE;

		/* condition always goes in predicate register: */
		cond->regs[0]->num = regid(REG_P0, 0);

		kill = ir3_KILL(b, cond, 0);

		ctx->kill[ctx->kill_count++] = kill;
		ctx->so->has_kill = true;

		break;
	}
	default:
		compile_error(ctx, "Unhandled intrinsic type: %s\n",
				nir_intrinsic_infos[intr->intrinsic].name);
		break;
	}
}

static void
emit_load_const(struct ir3_compile *ctx, nir_load_const_instr *instr)
{
	struct ir3_instruction **dst = get_dst_ssa(ctx, &instr->def,
			instr->def.num_components);
	for (int i = 0; i < instr->def.num_components; i++)
		dst[i] = create_immed(ctx->block, instr->value.u[i]);
}

static void
emit_undef(struct ir3_compile *ctx, nir_ssa_undef_instr *undef)
{
	struct ir3_instruction **dst = get_dst_ssa(ctx, &undef->def,
			undef->def.num_components);
	/* backend doesn't want undefined instructions, so just plug
	 * in 0.0..
	 */
	for (int i = 0; i < undef->def.num_components; i++)
		dst[i] = create_immed(ctx->block, fui(0.0));
}

/*
 * texture fetch/sample instructions:
 */

static void
emit_tex(struct ir3_compile *ctx, nir_tex_instr *tex)
{
	struct ir3_block *b = ctx->block;
	struct ir3_instruction **dst, *src0, *src1, *sam;
	struct ir3_instruction **coord, *lod, *compare, *proj, **off, **ddx, **ddy;
	struct ir3_register *reg;
	bool has_bias = false, has_lod = false, has_proj = false, has_off = false;
	unsigned i, coords, flags = 0;
	opc_t opc;

	/* TODO: might just be one component for gathers? */
	dst = get_dst(ctx, &tex->dest, 4);

	for (unsigned i = 0; i < tex->num_srcs; i++) {
		switch (tex->src[i].src_type) {
		case nir_tex_src_coord:
			coord = get_src(ctx, &tex->src[i].src);
			break;
		case nir_tex_src_bias:
			lod = get_src(ctx, &tex->src[i].src)[0];
			has_bias = true;
			break;
		case nir_tex_src_lod:
			lod = get_src(ctx, &tex->src[i].src)[0];
			has_lod = true;
			break;
		case nir_tex_src_comparitor: /* shadow comparator */
			compare = get_src(ctx, &tex->src[i].src)[0];
			break;
		case nir_tex_src_projector:
			proj = get_src(ctx, &tex->src[i].src)[0];
			has_proj = true;
			break;
		case nir_tex_src_offset:
			off = get_src(ctx, &tex->src[i].src);
			has_off = true;
			break;
		case nir_tex_src_ddx:
			ddx = get_src(ctx, &tex->src[i].src);
			break;
		case nir_tex_src_ddy:
			ddy = get_src(ctx, &tex->src[i].src);
			break;
		default:
			compile_error(ctx, "Unhandled NIR tex serc type: %d\n",
					tex->src[i].src_type);
			return;
		}
	}

	/*
	 * lay out the first argument in the proper order:
	 *  - actual coordinates first
	 *  - shadow reference
	 *  - array index
	 *  - projection w
	 *  - starting at offset 4, dpdx.xy, dpdy.xy
	 *
	 * bias/lod go into the second arg
	 */

	src0 = ir3_instr_create2(b, -1, OPC_META_FI, 12);
	ir3_reg_create(src0, 0, 0);

	coords = tex->coord_components;
	if (tex->is_array)       /* array idx goes after shadow ref */
		coords--;

	/* insert tex coords: */
	for (i = 0; i < coords; i++)
		ir3_reg_create(src0, 0, IR3_REG_SSA)->instr = coord[i];

	if (coords == 1) {
		/* hw doesn't do 1d, so we treat it as 2d with
		 * height of 1, and patch up the y coord.
		 * TODO: y coord should be (int)0 in some cases..
		 */
		ir3_reg_create(src0, 0, IR3_REG_SSA)->instr =
				create_immed(b, fui(0.5));
	}

	if (tex->is_shadow) {
		ir3_reg_create(src0, 0, IR3_REG_SSA)->instr = compare;
		flags |= IR3_INSTR_S;
	}

	if (tex->is_array) {
		ir3_reg_create(src0, 0, IR3_REG_SSA)->instr = coord[coords];
		flags |= IR3_INSTR_A;
	}

	if (has_proj) {
		ir3_reg_create(src0, 0, IR3_REG_SSA)->instr = proj;
		flags |= IR3_INSTR_P;
	}

	/* pad to 4, then ddx/ddy: */
	if (tex->op == nir_texop_txd) {
		while (src0->regs_count < 5) {
			ir3_reg_create(src0, 0, IR3_REG_SSA)->instr =
					create_immed(b, fui(0.0));
		}
		for (i = 0; i < coords; i++) {
			ir3_reg_create(src0, 0, IR3_REG_SSA)->instr = ddx[i];
		}
		if (coords < 2) {
			ir3_reg_create(src0, 0, IR3_REG_SSA)->instr =
					create_immed(b, fui(0.0));
		}
		for (i = 0; i < coords; i++) {
			ir3_reg_create(src0, 0, IR3_REG_SSA)->instr = ddy[i];
		}
		if (coords < 2) {
			ir3_reg_create(src0, 0, IR3_REG_SSA)->instr =
					create_immed(b, fui(0.0));
		}
	}

	/*
	 * second argument (if applicable):
	 *  - offsets
	 *  - lod
	 *  - bias
	 */
	if (has_off | has_lod | has_bias) {
		src1 = ir3_instr_create2(b, -1, OPC_META_FI, 5);
		ir3_reg_create(src1, 0, 0);

		if (has_off) {
			for (i = 0; i < coords; i++) {
				ir3_reg_create(src0, 0, IR3_REG_SSA)->instr = off[i];
			}
			if (coords < 2) {
				ir3_reg_create(src0, 0, IR3_REG_SSA)->instr =
						create_immed(b, fui(0.0));
			}
			flags |= IR3_INSTR_O;
		}

		if (has_lod | has_bias) {
			ir3_reg_create(src1, 0, IR3_REG_SSA)->instr = lod;
		}
	} else {
		src1 = NULL;
	}

	switch (tex->op) {
	case nir_texop_tex:      opc = OPC_SAM;      break;
	case nir_texop_txb:      opc = OPC_SAMB;     break;
	case nir_texop_txl:      opc = OPC_SAML;     break;
	case nir_texop_txd:      opc = OPC_SAMGQ;    break;
	case nir_texop_txf:      opc = OPC_ISAML;    break;
	case nir_texop_txf_ms:
	case nir_texop_txs:
	case nir_texop_lod:
	case nir_texop_tg4:
	case nir_texop_query_levels:
		compile_error(ctx, "Unhandled NIR tex type: %d\n", tex->op);
		return;
	}

	sam = ir3_instr_create(b, 5, opc);
	sam->flags |= flags;
	ir3_reg_create(sam, 0, 0)->wrmask = 0xf;  // TODO proper wrmask??
	reg = ir3_reg_create(sam, 0, IR3_REG_SSA);
	reg->wrmask = (1 << (src0->regs_count - 1)) - 1;
	reg->instr = src0;
	if (src1) {
		reg = ir3_reg_create(sam, 0, IR3_REG_SSA);
		reg->instr = src1;
		reg->wrmask = (1 << (src1->regs_count - 1)) - 1;
	}
	sam->cat5.samp = tex->sampler_index;
	sam->cat5.tex  = tex->sampler_index;

	switch (tex->dest_type) {
	case nir_type_invalid:
	case nir_type_float:
		sam->cat5.type = TYPE_F32;
		break;
	case nir_type_int:
		sam->cat5.type = TYPE_S32;
		break;
	case nir_type_unsigned:
	case nir_type_bool:
		sam->cat5.type = TYPE_U32;
	}

	// TODO maybe split this out into a helper, for other cases that
	// write multiple?
	struct ir3_instruction *prev = NULL;
	for (int i = 0; i < 4; i++) {
		struct ir3_instruction *split =
				ir3_instr_create(b, -1, OPC_META_FO);
		ir3_reg_create(split, 0, IR3_REG_SSA);
		ir3_reg_create(split, 0, IR3_REG_SSA)->instr = sam;
		split->fo.off = i;

		if (prev) {
			split->cp.left = prev;
			split->cp.left_cnt++;
			prev->cp.right = split;
			prev->cp.right_cnt++;
		}
		prev = split;

		dst[i] = split;
	}
}


static void
emit_instr(struct ir3_compile *ctx, nir_instr *instr)
{
	switch (instr->type) {
	case nir_instr_type_alu:
		emit_alu(ctx, nir_instr_as_alu(instr));
		break;
	case nir_instr_type_intrinsic:
		emit_intrinisic(ctx, nir_instr_as_intrinsic(instr));
		break;
	case nir_instr_type_load_const:
		emit_load_const(ctx, nir_instr_as_load_const(instr));
		break;
	case nir_instr_type_ssa_undef:
		emit_undef(ctx, nir_instr_as_ssa_undef(instr));
		break;
	case nir_instr_type_tex:
		emit_tex(ctx, nir_instr_as_tex(instr));
		break;

	case nir_instr_type_call:
	case nir_instr_type_jump:
	case nir_instr_type_phi:
	case nir_instr_type_parallel_copy:
		compile_error(ctx, "Unhandled NIR instruction type: %d\n", instr->type);
		break;
	}
}

static void
emit_block(struct ir3_compile *ctx, nir_block *block)
{
	nir_foreach_instr(block, instr) {
		emit_instr(ctx, instr);
		if (ctx->error)
			return;
	}
}

static void
emit_function(struct ir3_compile *ctx, nir_function_impl *impl)
{
	foreach_list_typed(nir_cf_node, node, node, &impl->body) {
		switch (node->type) {
		case nir_cf_node_block:
			emit_block(ctx, nir_cf_node_as_block(node));
			break;
		case nir_cf_node_if:
		case nir_cf_node_loop:
		case nir_cf_node_function:
			compile_error(ctx, "TODO\n");
			break;
		}
		if (ctx->error)
			return;
	}
}

static void
setup_input(struct ir3_compile *ctx, nir_variable *in)
{
	struct ir3_shader_variant *so = ctx->so;
	unsigned array_len = MAX2(glsl_get_length(in->type), 1);
	unsigned ncomp = glsl_get_components(in->type);
	/* XXX: map loc slots to semantics */
	unsigned semantic_name = in->data.location;
	unsigned semantic_index = in->data.index;
	unsigned n = in->data.driver_location;

	DBG("; in: %u:%u, len=%ux%u, loc=%u\n",
			semantic_name, semantic_index, array_len,
			ncomp, n);

	so->inputs[n].semantic =
			ir3_semantic_name(semantic_name, semantic_index);
	so->inputs[n].compmask = (1 << ncomp) - 1;
	so->inputs[n].inloc = ctx->next_inloc;
	so->inputs[n].interpolate = 0;
	so->inputs_count = MAX2(so->inputs_count, n + 1);

	/* the fdN_program_emit() code expects tgsi consts here, so map
	 * things back to tgsi for now:
	 */
	switch (in->data.interpolation) {
	case INTERP_QUALIFIER_FLAT:
		so->inputs[n].interpolate = TGSI_INTERPOLATE_CONSTANT;
		break;
	case INTERP_QUALIFIER_NOPERSPECTIVE:
		so->inputs[n].interpolate = TGSI_INTERPOLATE_LINEAR;
		break;
	case INTERP_QUALIFIER_SMOOTH:
		so->inputs[n].interpolate = TGSI_INTERPOLATE_PERSPECTIVE;
		break;
	}

	for (int i = 0; i < ncomp; i++) {
		struct ir3_instruction *instr = NULL;
		unsigned idx = (n * 4) + i;

		if (ctx->so->type == SHADER_FRAGMENT) {
			if (semantic_name == TGSI_SEMANTIC_POSITION) {
				so->inputs[n].bary = false;
				so->frag_coord = true;
				instr = create_frag_coord(ctx, i);
			} else if (semantic_name == TGSI_SEMANTIC_FACE) {
				so->inputs[n].bary = false;
				so->frag_face = true;
				instr = create_frag_face(ctx, i);
			} else {
				bool use_ldlv = false;

				/* with NIR, we need to infer TGSI_INTERPOLATE_COLOR
				 * from the semantic name:
				 */
				if (semantic_name == TGSI_SEMANTIC_COLOR)
					so->inputs[n].interpolate = TGSI_INTERPOLATE_COLOR;

				if (ctx->flat_bypass) {
					/* with NIR, we need to infer TGSI_INTERPOLATE_COLOR
					 * from the semantic name:
					 */
					switch (so->inputs[n].interpolate) {
					case TGSI_INTERPOLATE_COLOR:
						if (!ctx->so->key.rasterflat)
							break;
						/* fallthrough */
					case TGSI_INTERPOLATE_CONSTANT:
						use_ldlv = true;
						break;
					}
				}

				so->inputs[n].bary = true;

				instr = create_frag_input(ctx, idx, use_ldlv);
			}
		} else {
			instr = create_input(ctx->block, NULL, idx);
		}

		ctx->block->inputs[idx] = instr;
	}

	if (so->inputs[n].bary || (ctx->so->type == SHADER_VERTEX)) {
		ctx->next_inloc += ncomp;
		so->total_in += ncomp;
	}
}

static void
setup_output(struct ir3_compile *ctx, nir_variable *out)
{
	struct ir3_shader_variant *so = ctx->so;
	unsigned array_len = MAX2(glsl_get_length(out->type), 1);
	unsigned ncomp = glsl_get_components(out->type);
	/* XXX: map loc slots to semantics */
	unsigned semantic_name = out->data.location;
	unsigned semantic_index = out->data.index;
	unsigned n = out->data.driver_location;
	unsigned comp = 0;

	DBG("; out: %u:%u, len=%ux%u, loc=%u\n",
			semantic_name, semantic_index, array_len,
			ncomp, n);

	if (ctx->so->type == SHADER_VERTEX) {
		switch (semantic_name) {
		case TGSI_SEMANTIC_POSITION:
			so->writes_pos = true;
			break;
		case TGSI_SEMANTIC_PSIZE:
			so->writes_psize = true;
			break;
		case TGSI_SEMANTIC_COLOR:
		case TGSI_SEMANTIC_BCOLOR:
		case TGSI_SEMANTIC_GENERIC:
		case TGSI_SEMANTIC_FOG:
		case TGSI_SEMANTIC_TEXCOORD:
			break;
		default:
			compile_error(ctx, "unknown VS semantic name: %s\n",
					tgsi_semantic_names[semantic_name]);
		}
	} else {
		switch (semantic_name) {
		case TGSI_SEMANTIC_POSITION:
			comp = 2;  /* tgsi will write to .z component */
			so->writes_pos = true;
			break;
		case TGSI_SEMANTIC_COLOR:
			break;
		default:
			compile_error(ctx, "unknown FS semantic name: %s\n",
					tgsi_semantic_names[semantic_name]);
		}
	}

	compile_assert(ctx, n < ARRAY_SIZE(so->outputs));

	so->outputs[n].semantic =
			ir3_semantic_name(semantic_name, semantic_index);
	so->outputs[n].regid = regid(n, comp);
	so->outputs_count = MAX2(so->outputs_count, n + 1);

	for (int i = 0; i < ncomp; i++) {
		unsigned idx = (n * 4) + i;

		ctx->block->outputs[idx] = create_immed(ctx->block, fui(0.0));
	}
}

static void
emit_instructions(struct ir3_compile *ctx)
{
	unsigned ninputs  = exec_list_length(&ctx->s->inputs) * 4;
	unsigned noutputs = exec_list_length(&ctx->s->outputs) * 4;

	/* we need to allocate big enough outputs array so that
	 * we can stuff the kill's at the end:
	 */
	if (ctx->so->type == SHADER_FRAGMENT)
		noutputs += ARRAY_SIZE(ctx->kill);

	ctx->block = ir3_block_create(ctx->ir, 0, ninputs, noutputs);

	if (ctx->so->type == SHADER_FRAGMENT)
		ctx->block->noutputs -= ARRAY_SIZE(ctx->kill);


	/* for fragment shader, we have a single input register (usually
	 * r0.xy) which is used as the base for bary.f varying fetch instrs:
	 */
	if (ctx->so->type == SHADER_FRAGMENT) {
		// TODO maybe a helper for fi since we need it a few places..
		struct ir3_instruction *instr;
		instr = ir3_instr_create(ctx->block, -1, OPC_META_FI);
		ir3_reg_create(instr, 0, 0);
		ir3_reg_create(instr, 0, IR3_REG_SSA);    /* r0.x */
		ir3_reg_create(instr, 0, IR3_REG_SSA);    /* r0.y */
		ctx->frag_pos = instr;
	}

	/* Setup inputs: */
	foreach_list_typed(nir_variable, var, node, &ctx->s->inputs) {
		setup_input(ctx, var);
		if (ctx->error)
			return;
	}

	/* Setup outputs: */
	foreach_list_typed(nir_variable, var, node, &ctx->s->outputs) {
		setup_output(ctx, var);
		if (ctx->error)
			return;
	}

	/* Find the main function and emit the body: */
	nir_foreach_overload(ctx->s, overload) {
		compile_assert(ctx, strcmp(overload->function->name, "main") == 0);
		compile_assert(ctx, overload->impl);
		emit_function(ctx, overload->impl);
		if (ctx->error)
			return;
	}
}

/* from NIR perspective, we actually have inputs.  But most of the "inputs"
 * for a fragment shader are just bary.f instructions.  The *actual* inputs
 * from the hw perspective are the frag_pos and optionally frag_coord and
 * frag_face.
 */
static void
fixup_frag_inputs(struct ir3_compile *ctx)
{
	struct ir3_shader_variant *so = ctx->so;
	struct ir3_block *block = ctx->block;
	struct ir3_instruction **inputs;
	struct ir3_instruction *instr;
	int n, regid = 0;

	block->ninputs = 0;

	n  = 4;  /* always have frag_pos */
	n += COND(so->frag_face, 4);
	n += COND(so->frag_coord, 4);

	inputs = ir3_alloc(ctx->ir, n * (sizeof(struct ir3_instruction *)));

	if (so->frag_face) {
		/* this ultimately gets assigned to hr0.x so doesn't conflict
		 * with frag_coord/frag_pos..
		 */
		inputs[block->ninputs++] = ctx->frag_face;
		ctx->frag_face->regs[0]->num = 0;

		/* remaining channels not used, but let's avoid confusing
		 * other parts that expect inputs to come in groups of vec4
		 */
		inputs[block->ninputs++] = NULL;
		inputs[block->ninputs++] = NULL;
		inputs[block->ninputs++] = NULL;
	}

	/* since we don't know where to set the regid for frag_coord,
	 * we have to use r0.x for it.  But we don't want to *always*
	 * use r1.x for frag_pos as that could increase the register
	 * footprint on simple shaders:
	 */
	if (so->frag_coord) {
		ctx->frag_coord[0]->regs[0]->num = regid++;
		ctx->frag_coord[1]->regs[0]->num = regid++;
		ctx->frag_coord[2]->regs[0]->num = regid++;
		ctx->frag_coord[3]->regs[0]->num = regid++;

		inputs[block->ninputs++] = ctx->frag_coord[0];
		inputs[block->ninputs++] = ctx->frag_coord[1];
		inputs[block->ninputs++] = ctx->frag_coord[2];
		inputs[block->ninputs++] = ctx->frag_coord[3];
	}

	/* we always have frag_pos: */
	so->pos_regid = regid;

	/* r0.x */
	instr = create_input(block, NULL, block->ninputs);
	instr->regs[0]->num = regid++;
	inputs[block->ninputs++] = instr;
	ctx->frag_pos->regs[1]->instr = instr;

	/* r0.y */
	instr = create_input(block, NULL, block->ninputs);
	instr->regs[0]->num = regid++;
	inputs[block->ninputs++] = instr;
	ctx->frag_pos->regs[2]->instr = instr;

	block->inputs = inputs;
}

static void
compile_dump(struct ir3_compile *ctx)
{
	const char *name = (ctx->so->type == SHADER_VERTEX) ? "vert" : "frag";
	static unsigned n = 0;
	char fname[16];
	FILE *f;
	snprintf(fname, sizeof(fname), "%s-%04u.dot", name, n++);
	f = fopen(fname, "w");
	if (!f)
		return;
	ir3_block_depth(ctx->block);
	ir3_dump(ctx->ir, name, ctx->block, f);
	fclose(f);
}

int
ir3_compile_shader_nir(struct ir3_shader_variant *so,
		const struct tgsi_token *tokens, struct ir3_shader_key key)
{
	struct ir3_compile *ctx;
	struct ir3_block *block;
	struct ir3_instruction **inputs;
	unsigned i, j, actual_in;
	int ret = 0, max_bary;

	assert(!so->ir);

	so->ir = ir3_create();

	assert(so->ir);

	ctx = compile_init(so, tokens);
	if (!ctx) {
		DBG("INIT failed!");
		ret = -1;
		goto out;
	}

	emit_instructions(ctx);

	if (ctx->error) {
		DBG("EMIT failed!");
		ret = -1;
		goto out;
	}

	block = ctx->block;
	so->ir->block = block;

	/* keep track of the inputs from TGSI perspective.. */
	inputs = block->inputs;

	/* but fixup actual inputs for frag shader: */
	if (so->type == SHADER_FRAGMENT)
		fixup_frag_inputs(ctx);

	/* at this point, for binning pass, throw away unneeded outputs: */
	if (key.binning_pass) {
		for (i = 0, j = 0; i < so->outputs_count; i++) {
			unsigned name = sem2name(so->outputs[i].semantic);
			unsigned idx = sem2idx(so->outputs[i].semantic);

			/* throw away everything but first position/psize */
			if ((idx == 0) && ((name == TGSI_SEMANTIC_POSITION) ||
					(name == TGSI_SEMANTIC_PSIZE))) {
				if (i != j) {
					so->outputs[j] = so->outputs[i];
					block->outputs[(j*4)+0] = block->outputs[(i*4)+0];
					block->outputs[(j*4)+1] = block->outputs[(i*4)+1];
					block->outputs[(j*4)+2] = block->outputs[(i*4)+2];
					block->outputs[(j*4)+3] = block->outputs[(i*4)+3];
				}
				j++;
			}
		}
		so->outputs_count = j;
		block->noutputs = j * 4;
	}

	/* if we want half-precision outputs, mark the output registers
	 * as half:
	 */
	if (key.half_precision) {
		for (i = 0; i < block->noutputs; i++) {
			if (!block->outputs[i])
				continue;
			block->outputs[i]->regs[0]->flags |= IR3_REG_HALF;
		}
	}

	/* at this point, we want the kill's in the outputs array too,
	 * so that they get scheduled (since they have no dst).. we've
	 * already ensured that the array is big enough in push_block():
	 */
	if (so->type == SHADER_FRAGMENT) {
		for (i = 0; i < ctx->kill_count; i++)
			block->outputs[block->noutputs++] = ctx->kill[i];
	}

	if (fd_mesa_debug & FD_DBG_OPTDUMP)
		compile_dump(ctx);

	if (fd_mesa_debug & FD_DBG_OPTMSGS) {
		printf("BEFORE CP:\n");
		ir3_dump_instr_list(block->head);
	}

	ir3_block_depth(block);

	ir3_block_cp(block);

	if (fd_mesa_debug & FD_DBG_OPTMSGS) {
		printf("BEFORE GROUPING:\n");
		ir3_dump_instr_list(block->head);
	}

	/* Group left/right neighbors, inserting mov's where needed to
	 * solve conflicts:
	 */
	ir3_block_group(block);

	if (fd_mesa_debug & FD_DBG_OPTDUMP)
		compile_dump(ctx);

	ir3_block_depth(block);

	if (fd_mesa_debug & FD_DBG_OPTMSGS) {
		printf("AFTER DEPTH:\n");
		ir3_dump_instr_list(block->head);
	}

	ret = ir3_block_sched(block);
	if (ret) {
		DBG("SCHED failed!");
		goto out;
	}

	if (fd_mesa_debug & FD_DBG_OPTMSGS) {
		printf("AFTER SCHED:\n");
		ir3_dump_instr_list(block->head);
	}

	ret = ir3_block_ra(block, so->type, so->frag_coord, so->frag_face);
	if (ret) {
		DBG("RA failed!");
		goto out;
	}

	if (fd_mesa_debug & FD_DBG_OPTMSGS) {
		printf("AFTER RA:\n");
		ir3_dump_instr_list(block->head);
	}

	ir3_block_legalize(block, &so->has_samp, &max_bary);

	/* fixup input/outputs: */
	for (i = 0; i < so->outputs_count; i++) {
		so->outputs[i].regid = block->outputs[i*4]->regs[0]->num;
		/* preserve hack for depth output.. tgsi writes depth to .z,
		 * but what we give the hw is the scalar register:
		 */
		if ((so->type == SHADER_FRAGMENT) &&
			(sem2name(so->outputs[i].semantic) == TGSI_SEMANTIC_POSITION))
			so->outputs[i].regid += 2;
	}

	/* Note that some or all channels of an input may be unused: */
	actual_in = 0;
	for (i = 0; i < so->inputs_count; i++) {
		unsigned j, regid = ~0, compmask = 0;
		so->inputs[i].ncomp = 0;
		for (j = 0; j < 4; j++) {
			struct ir3_instruction *in = inputs[(i*4) + j];
			if (in) {
				compmask |= (1 << j);
				regid = in->regs[0]->num - j;
				actual_in++;
				so->inputs[i].ncomp++;
			}
		}
		so->inputs[i].regid = regid;
		so->inputs[i].compmask = compmask;
	}

	/* fragment shader always gets full vec4's even if it doesn't
	 * fetch all components, but vertex shader we need to update
	 * with the actual number of components fetch, otherwise thing
	 * will hang due to mismaptch between VFD_DECODE's and
	 * TOTALATTRTOVS
	 */
	if (so->type == SHADER_VERTEX)
		so->total_in = actual_in;
	else
		so->total_in = align(max_bary + 1, 4);

out:
	if (ret) {
		ir3_destroy(so->ir);
		so->ir = NULL;
	}
	compile_free(ctx);

	return ret;
}
