/*
 * Copyright (C) 2016 Rob Clark <robclark@freedesktop.org>
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

#include "pipe/p_state.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_helpers.h"
#include "util/u_format.h"
#include "util/u_viewport.h"

#include "freedreno_resource.h"
#include "freedreno_query_hw.h"

#include "fd5_emit.h"
#include "fd5_blend.h"
#include "fd5_context.h"
#include "fd5_program.h"
#include "fd5_rasterizer.h"
#include "fd5_texture.h"
#include "fd5_format.h"
#include "fd5_zsa.h"

static const enum adreno_state_block sb[] = {
	[SHADER_VERTEX]   = SB_VERT_SHADER,
	[SHADER_FRAGMENT] = SB_FRAG_SHADER,
};

/* regid:          base const register
 * prsc or dwords: buffer containing constant values
 * sizedwords:     size of const value buffer
 */
static void
fd5_emit_const(struct fd_ringbuffer *ring, enum shader_t type,
		uint32_t regid, uint32_t offset, uint32_t sizedwords,
		const uint32_t *dwords, struct pipe_resource *prsc)
{
	uint32_t i, sz;
	enum adreno_state_src src;

	debug_assert((regid % 4) == 0);
	debug_assert((sizedwords % 4) == 0);

	if (prsc) {
		sz = 0;
		src = 0x2;  // TODO ??
	} else {
		sz = sizedwords;
		src = SS_DIRECT;
	}

	OUT_PKT7(ring, CP_LOAD_STATE, 3 + sz);
	OUT_RING(ring, CP_LOAD_STATE_0_DST_OFF(regid/4) |
			CP_LOAD_STATE_0_STATE_SRC(src) |
			CP_LOAD_STATE_0_STATE_BLOCK(sb[type]) |
			CP_LOAD_STATE_0_NUM_UNIT(sizedwords/4));
	if (prsc) {
		struct fd_bo *bo = fd_resource(prsc)->bo;
		OUT_RELOC(ring, bo, offset,
				CP_LOAD_STATE_1_STATE_TYPE(ST_CONSTANTS), 0);
	} else {
		OUT_RING(ring, CP_LOAD_STATE_1_EXT_SRC_ADDR(0) |
				CP_LOAD_STATE_1_STATE_TYPE(ST_CONSTANTS));
		OUT_RING(ring, CP_LOAD_STATE_2_EXT_SRC_ADDR_HI(0));
		dwords = (uint32_t *)&((uint8_t *)dwords)[offset];
	}
	for (i = 0; i < sz; i++) {
		OUT_RING(ring, dwords[i]);
	}
}

static void
fd5_emit_const_bo(struct fd_ringbuffer *ring, enum shader_t type, boolean write,
		uint32_t regid, uint32_t num, struct pipe_resource **prscs, uint32_t *offsets)
{
	uint32_t i;

	debug_assert((regid % 4) == 0);
	debug_assert((num % 4) == 0);

	OUT_PKT7(ring, CP_LOAD_STATE, 3 + num);
	OUT_RING(ring, CP_LOAD_STATE_0_DST_OFF(regid/4) |
			CP_LOAD_STATE_0_STATE_SRC(SS_DIRECT) |
			CP_LOAD_STATE_0_STATE_BLOCK(sb[type]) |
			CP_LOAD_STATE_0_NUM_UNIT(num/4));
	OUT_RING(ring, CP_LOAD_STATE_1_EXT_SRC_ADDR(0) |
			CP_LOAD_STATE_1_STATE_TYPE(ST_CONSTANTS));
	OUT_RING(ring, CP_LOAD_STATE_2_EXT_SRC_ADDR_HI(0));

	for (i = 0; i < num; i++) {
		if (prscs[i]) {
			if (write) {
				OUT_RELOCW(ring, fd_resource(prscs[i])->bo, offsets[i], 0, 0);
			} else {
				OUT_RELOC(ring, fd_resource(prscs[i])->bo, offsets[i], 0, 0);
			}
		} else {
			OUT_RING(ring, 0xbad00000 | (i << 16));
		}
	}
}

static void
emit_textures(struct fd_context *ctx, struct fd_ringbuffer *ring,
		enum adreno_state_block sb, struct fd_texture_stateobj *tex)
{
	unsigned i;

	if (tex->num_samplers > 0) {
		/* output sampler state: */
		OUT_PKT7(ring, CP_LOAD_STATE, 3 + (4 * tex->num_samplers));
		OUT_RING(ring, CP_LOAD_STATE_0_DST_OFF(0) |
				CP_LOAD_STATE_0_STATE_SRC(SS_DIRECT) |
				CP_LOAD_STATE_0_STATE_BLOCK(sb) |
				CP_LOAD_STATE_0_NUM_UNIT(tex->num_samplers));
		OUT_RING(ring, CP_LOAD_STATE_1_STATE_TYPE(ST_SHADER) |
				CP_LOAD_STATE_1_EXT_SRC_ADDR(0));
		OUT_RING(ring, CP_LOAD_STATE_2_EXT_SRC_ADDR_HI(0));
		for (i = 0; i < tex->num_samplers; i++) {
			static const struct fd5_sampler_stateobj dummy_sampler = {};
			const struct fd5_sampler_stateobj *sampler = tex->samplers[i] ?
					fd5_sampler_stateobj(tex->samplers[i]) :
					&dummy_sampler;
			OUT_RING(ring, sampler->texsamp0);
			OUT_RING(ring, sampler->texsamp1);
			OUT_RING(ring, sampler->texsamp2);
			OUT_RING(ring, sampler->texsamp3);
		}
	}

	if (tex->num_textures > 0) {
		unsigned num_textures = tex->num_textures;

		/* emit texture state: */
		OUT_PKT7(ring, CP_LOAD_STATE, 3 + (12 * num_textures));
		OUT_RING(ring, CP_LOAD_STATE_0_DST_OFF(0) |
				CP_LOAD_STATE_0_STATE_SRC(SS_DIRECT) |
				CP_LOAD_STATE_0_STATE_BLOCK(sb) |
				CP_LOAD_STATE_0_NUM_UNIT(num_textures));
		OUT_RING(ring, CP_LOAD_STATE_1_STATE_TYPE(ST_CONSTANTS) |
				CP_LOAD_STATE_1_EXT_SRC_ADDR(0));
		OUT_RING(ring, CP_LOAD_STATE_2_EXT_SRC_ADDR_HI(0));
		for (i = 0; i < tex->num_textures; i++) {
			static const struct fd5_pipe_sampler_view dummy_view = {};
			const struct fd5_pipe_sampler_view *view = tex->textures[i] ?
					fd5_pipe_sampler_view(tex->textures[i]) :
					&dummy_view;

			OUT_RING(ring, view->texconst0);
			OUT_RING(ring, view->texconst1);
			OUT_RING(ring, view->texconst2);
			OUT_RING(ring, view->texconst3);
			if (view->base.texture) {
				struct fd_resource *rsc = fd_resource(view->base.texture);
				OUT_RELOC(ring, rsc->bo, view->offset,
						(uint64_t)view->texconst5 << 32, 0);
			} else {
				OUT_RING(ring, 0x00000000);
				OUT_RING(ring, view->texconst5);
			}
			OUT_RING(ring, view->texconst6);
			OUT_RING(ring, view->texconst7);
			OUT_RING(ring, view->texconst8);
			OUT_RING(ring, view->texconst9);
			OUT_RING(ring, view->texconst10);
			OUT_RING(ring, view->texconst11);
		}
	}
}

void
fd5_emit_vertex_bufs(struct fd_ringbuffer *ring, struct fd5_emit *emit)
{
	int32_t i, j;
	const struct fd_vertex_state *vtx = emit->vtx;
	const struct ir3_shader_variant *vp = fd5_emit_get_vp(emit);

	for (i = 0, j = 0; i <= vp->inputs_count; i++) {
		if (vp->inputs[i].sysval)
			continue;
		if (vp->inputs[i].compmask) {
			struct pipe_vertex_element *elem = &vtx->vtx->pipe[i];
			const struct pipe_vertex_buffer *vb =
					&vtx->vertexbuf.vb[elem->vertex_buffer_index];
			struct fd_resource *rsc = fd_resource(vb->buffer);
			enum pipe_format pfmt = elem->src_format;
			enum a5xx_vtx_fmt fmt = fd5_pipe2vtx(pfmt);
			uint32_t off = vb->buffer_offset + elem->src_offset;
			uint32_t size = fd_bo_size(rsc->bo) - off;
			debug_assert(fmt != ~0);

			OUT_PKT4(ring, REG_A5XX_VFD_FETCH(j), 4);
			OUT_RELOC(ring, rsc->bo, off, 0, 0);
			OUT_RING(ring, size);           /* VFD_FETCH[j].SIZE */
			OUT_RING(ring, vb->stride);     /* VFD_FETCH[j].STRIDE */

			OUT_PKT4(ring, REG_A5XX_VFD_DECODE(j), 2);
			OUT_RING(ring, A5XX_VFD_DECODE_INSTR_IDX(j) |
					A5XX_VFD_DECODE_INSTR_FORMAT(fmt) |
					A5XX_VFD_DECODE_INSTR_SWAP(fd5_pipe2swap(pfmt)));
			OUT_RING(ring, MAX2(1, elem->instance_divisor)); /* VFD_DECODE[j].STEP_RATE */

			OUT_PKT4(ring, REG_A5XX_VFD_DEST_CNTL(j), 1);
			OUT_RING(ring, A5XX_VFD_DEST_CNTL_INSTR_WRITEMASK(vp->inputs[i].compmask) |
					A5XX_VFD_DEST_CNTL_INSTR_REGID(vp->inputs[i].regid));

			j++;
		}
	}

	OUT_PKT4(ring, REG_A5XX_VFD_CONTROL_0, 1);
	OUT_RING(ring, A5XX_VFD_CONTROL_0_VTXCNT(j));
}

void
fd5_emit_state(struct fd_context *ctx, struct fd_ringbuffer *ring,
		struct fd5_emit *emit)
{
	const struct ir3_shader_variant *vp = fd5_emit_get_vp(emit);
	const struct ir3_shader_variant *fp = fd5_emit_get_fp(emit);
	uint32_t dirty = emit->dirty;

	emit_marker5(ring, 5);

	if ((dirty & FD_DIRTY_FRAMEBUFFER) && !emit->key.binning_pass) {
		struct pipe_framebuffer_state *pfb = &ctx->batch->framebuffer;
		unsigned char mrt_comp[A5XX_MAX_RENDER_TARGETS] = {0};

		for (unsigned i = 0; i < A5XX_MAX_RENDER_TARGETS; i++) {
			mrt_comp[i] = ((i < pfb->nr_cbufs) && pfb->cbufs[i]) ? 0xf : 0;
		}

		OUT_PKT4(ring, REG_A5XX_RB_RENDER_COMPONENTS, 1);
		OUT_RING(ring, A5XX_RB_RENDER_COMPONENTS_RT0(mrt_comp[0]) |
				A5XX_RB_RENDER_COMPONENTS_RT1(mrt_comp[1]) |
				A5XX_RB_RENDER_COMPONENTS_RT2(mrt_comp[2]) |
				A5XX_RB_RENDER_COMPONENTS_RT3(mrt_comp[3]) |
				A5XX_RB_RENDER_COMPONENTS_RT4(mrt_comp[4]) |
				A5XX_RB_RENDER_COMPONENTS_RT5(mrt_comp[5]) |
				A5XX_RB_RENDER_COMPONENTS_RT6(mrt_comp[6]) |
				A5XX_RB_RENDER_COMPONENTS_RT7(mrt_comp[7]));
	}

	if (dirty & (FD_DIRTY_ZSA | FD_DIRTY_FRAMEBUFFER)) {
		struct fd5_zsa_stateobj *zsa = fd5_zsa_stateobj(ctx->zsa);
		struct pipe_framebuffer_state *pfb = &ctx->batch->framebuffer;
		uint32_t rb_alpha_control = zsa->rb_alpha_control;

		if (util_format_is_pure_integer(pipe_surface_format(pfb->cbufs[0])))
			rb_alpha_control &= ~A5XX_RB_ALPHA_CONTROL_ALPHA_TEST;

		OUT_PKT4(ring, REG_A5XX_RB_ALPHA_CONTROL, 1);
		OUT_RING(ring, rb_alpha_control);

		OUT_PKT4(ring, REG_A5XX_RB_STENCIL_CONTROL, 1);
		OUT_RING(ring, zsa->rb_stencil_control);
	}

	if (dirty & (FD_DIRTY_ZSA | FD_DIRTY_STENCIL_REF)) {
		struct fd5_zsa_stateobj *zsa = fd5_zsa_stateobj(ctx->zsa);
		struct pipe_stencil_ref *sr = &ctx->stencil_ref;

		OUT_PKT4(ring, REG_A5XX_RB_STENCILREFMASK, 1);
		OUT_RING(ring, zsa->rb_stencilrefmask |
				A5XX_RB_STENCILREFMASK_STENCILREF(sr->ref_value[0]));
	}

	if (dirty & (FD_DIRTY_ZSA | FD_DIRTY_RASTERIZER | FD_DIRTY_PROG)) {
		struct fd5_zsa_stateobj *zsa = fd5_zsa_stateobj(ctx->zsa);
		bool fragz = fp->has_kill | fp->writes_pos;

		OUT_PKT4(ring, REG_A5XX_RB_DEPTH_CNTL, 1);
		OUT_RING(ring, zsa->rb_depth_cntl);

		OUT_PKT4(ring, REG_A5XX_RB_DEPTH_PLANE_CNTL, 1);
		OUT_RING(ring, COND(fragz, A5XX_RB_DEPTH_PLANE_CNTL_FRAG_WRITES_Z));

		OUT_PKT4(ring, REG_A5XX_GRAS_SU_DEPTH_PLANE_CNTL, 1);
		OUT_RING(ring, COND(fragz, A5XX_GRAS_SU_DEPTH_PLANE_CNTL_FRAG_WRITES_Z));
	}

	if (dirty & FD_DIRTY_RASTERIZER) {
		struct fd5_rasterizer_stateobj *rasterizer =
				fd5_rasterizer_stateobj(ctx->rasterizer);

		OUT_PKT4(ring, REG_A5XX_GRAS_SU_CNTL, 1);
		OUT_RING(ring, rasterizer->gras_su_cntl);

		OUT_PKT4(ring, REG_A5XX_GRAS_SU_POINT_MINMAX, 2);
		OUT_RING(ring, rasterizer->gras_su_point_minmax);
		OUT_RING(ring, rasterizer->gras_su_point_size);

		OUT_PKT4(ring, REG_A5XX_GRAS_SU_POLY_OFFSET_SCALE, 3);
		OUT_RING(ring, rasterizer->gras_su_poly_offset_scale);
		OUT_RING(ring, rasterizer->gras_su_poly_offset_offset);
		OUT_RING(ring, rasterizer->gras_su_poly_offset_clamp);
	}

	/* NOTE: since primitive_restart is not actually part of any
	 * state object, we need to make sure that we always emit
	 * PRIM_VTX_CNTL.. either that or be more clever and detect
	 * when it changes.
	 */
	if (emit->info) {
		struct fd5_rasterizer_stateobj *rast =
			fd5_rasterizer_stateobj(ctx->rasterizer);
		uint32_t val = rast->pc_prim_vtx_cntl;

		val |= COND(vp->writes_psize, A5XX_PC_PRIM_VTX_CNTL_PSIZE);

		OUT_PKT4(ring, REG_A5XX_PC_PRIM_VTX_CNTL, 1);
		OUT_RING(ring, val);
	}

	if (dirty & FD_DIRTY_SCISSOR) {
		struct pipe_scissor_state *scissor = fd_context_get_scissor(ctx);

		OUT_PKT4(ring, REG_A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0, 2);
		OUT_RING(ring, A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(scissor->minx) |
				A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(scissor->miny));
		OUT_RING(ring, A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(scissor->maxx - 1) |
				A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(scissor->maxy - 1));

		OUT_PKT4(ring, REG_A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0, 2);
		OUT_RING(ring, A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(scissor->minx) |
				A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(scissor->miny));
		OUT_RING(ring, A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(scissor->maxx - 1) |
				A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(scissor->maxy - 1));

		ctx->batch->max_scissor.minx = MIN2(ctx->batch->max_scissor.minx, scissor->minx);
		ctx->batch->max_scissor.miny = MIN2(ctx->batch->max_scissor.miny, scissor->miny);
		ctx->batch->max_scissor.maxx = MAX2(ctx->batch->max_scissor.maxx, scissor->maxx);
		ctx->batch->max_scissor.maxy = MAX2(ctx->batch->max_scissor.maxy, scissor->maxy);
	}

	if (dirty & FD_DIRTY_VIEWPORT) {
		fd_wfi(ctx->batch, ring);
		OUT_PKT4(ring, REG_A5XX_GRAS_CL_VPORT_XOFFSET_0, 6);
		OUT_RING(ring, A5XX_GRAS_CL_VPORT_XOFFSET_0(ctx->viewport.translate[0]));
		OUT_RING(ring, A5XX_GRAS_CL_VPORT_XSCALE_0(ctx->viewport.scale[0]));
		OUT_RING(ring, A5XX_GRAS_CL_VPORT_YOFFSET_0(ctx->viewport.translate[1]));
		OUT_RING(ring, A5XX_GRAS_CL_VPORT_YSCALE_0(ctx->viewport.scale[1]));
		OUT_RING(ring, A5XX_GRAS_CL_VPORT_ZOFFSET_0(ctx->viewport.translate[2]));
		OUT_RING(ring, A5XX_GRAS_CL_VPORT_ZSCALE_0(ctx->viewport.scale[2]));
	}

	if (dirty & (FD_DIRTY_PROG | FD_DIRTY_FRAMEBUFFER)) {
		struct pipe_framebuffer_state *pfb = &ctx->batch->framebuffer;
		unsigned n = pfb->nr_cbufs;
		/* if we have depth/stencil, we need at least on MRT: */
		if (pfb->zsbuf)
			n = MAX2(1, n);
		fd5_program_emit(ring, emit, n, pfb->cbufs);
	}

	if (emit->prog == &ctx->prog) { /* evil hack to deal sanely with clear path */
		ir3_emit_consts(vp, ring, ctx, emit->info, dirty);
		if (!emit->key.binning_pass)
			ir3_emit_consts(fp, ring, ctx, emit->info, dirty);
	}

	if ((dirty & FD_DIRTY_BLEND)) {
		struct fd5_blend_stateobj *blend = fd5_blend_stateobj(ctx->blend);
		uint32_t i;

		for (i = 0; i < A5XX_MAX_RENDER_TARGETS; i++) {
			enum pipe_format format = pipe_surface_format(
					ctx->batch->framebuffer.cbufs[i]);
			bool is_int = util_format_is_pure_integer(format);
			bool has_alpha = util_format_has_alpha(format);
			uint32_t control = blend->rb_mrt[i].control;
			uint32_t blend_control = blend->rb_mrt[i].blend_control_alpha;

			if (is_int) {
				control &= A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE__MASK;
//				control |= A5XX_RB_MRT_CONTROL_ROP_CODE(ROP_COPY);
			}

			if (has_alpha) {
				blend_control |= blend->rb_mrt[i].blend_control_rgb;
			} else {
				blend_control |= blend->rb_mrt[i].blend_control_no_alpha_rgb;
				control &= ~A5XX_RB_MRT_CONTROL_BLEND2;
			}

			OUT_PKT4(ring, REG_A5XX_RB_MRT_CONTROL(i), 1);
			OUT_RING(ring, control);

			OUT_PKT4(ring, REG_A5XX_RB_MRT_BLEND_CONTROL(i), 1);
			OUT_RING(ring, blend_control);
		}

		OUT_PKT4(ring, REG_A5XX_RB_BLEND_CNTL, 1);
		OUT_RING(ring, blend->rb_blend_cntl |
				A5XX_RB_BLEND_CNTL_SAMPLE_MASK(0xffff));

		OUT_PKT4(ring, REG_A5XX_SP_BLEND_CNTL, 1);
		OUT_RING(ring, 0x00000100);
	}

	if (dirty & FD_DIRTY_BLEND_COLOR) {
		struct pipe_blend_color *bcolor = &ctx->blend_color;

		OUT_PKT4(ring, REG_A5XX_RB_BLEND_RED, 8);
		OUT_RING(ring, A5XX_RB_BLEND_RED_FLOAT(bcolor->color[0]) |
				A5XX_RB_BLEND_RED_UINT(bcolor->color[0] * 0xff) |
				A5XX_RB_BLEND_RED_SINT(bcolor->color[0] * 0x7f));
		OUT_RING(ring, A5XX_RB_BLEND_RED_F32(bcolor->color[0]));
		OUT_RING(ring, A5XX_RB_BLEND_GREEN_FLOAT(bcolor->color[1]) |
				A5XX_RB_BLEND_GREEN_UINT(bcolor->color[1] * 0xff) |
				A5XX_RB_BLEND_GREEN_SINT(bcolor->color[1] * 0x7f));
		OUT_RING(ring, A5XX_RB_BLEND_RED_F32(bcolor->color[1]));
		OUT_RING(ring, A5XX_RB_BLEND_BLUE_FLOAT(bcolor->color[2]) |
				A5XX_RB_BLEND_BLUE_UINT(bcolor->color[2] * 0xff) |
				A5XX_RB_BLEND_BLUE_SINT(bcolor->color[2] * 0x7f));
		OUT_RING(ring, A5XX_RB_BLEND_BLUE_F32(bcolor->color[2]));
		OUT_RING(ring, A5XX_RB_BLEND_ALPHA_FLOAT(bcolor->color[3]) |
				A5XX_RB_BLEND_ALPHA_UINT(bcolor->color[3] * 0xff) |
				A5XX_RB_BLEND_ALPHA_SINT(bcolor->color[3] * 0x7f));
		OUT_RING(ring, A5XX_RB_BLEND_ALPHA_F32(bcolor->color[3]));
	}

	if (dirty & FD_DIRTY_VERTTEX) {
		if (vp->has_samp) {
			emit_textures(ctx, ring, SB_VERT_TEX, &ctx->verttex);
			OUT_PKT4(ring, REG_A5XX_TPL1_VS_TEX_COUNT, 1);
			OUT_RING(ring, ctx->verttex.num_textures);
		} else {
			dirty &= ~FD_DIRTY_VERTTEX;
		}
	}

	if (dirty & FD_DIRTY_FRAGTEX) {
		if (fp->has_samp) {
			emit_textures(ctx, ring, SB_FRAG_TEX, &ctx->fragtex);
			OUT_PKT4(ring, REG_A5XX_TPL1_FS_TEX_COUNT, 1);
			OUT_RING(ring, ctx->fragtex.num_textures);
		} else {
			dirty &= ~FD_DIRTY_FRAGTEX;
		}
	}

	ctx->dirty &= ~dirty;
}

/* emit setup at begin of new cmdstream buffer (don't rely on previous
 * state, there could have been a context switch between ioctls):
 */
void
fd5_emit_restore(struct fd_batch *batch, struct fd_ringbuffer *ring)
{
	struct fd_context *ctx = batch->ctx;

	fd5_set_render_mode(ctx, ring, BYPASS);
	fd5_cache_flush(batch, ring);

	OUT_PKT4(ring, REG_A5XX_HLSQ_UPDATE_CNTL, 1);
	OUT_RING(ring, 0xfffff);

/*
t7              opcode: CP_PERFCOUNTER_ACTION (50) (4 dwords)
0000000500024048:               70d08003 00000000 001c5000 00000005
t7              opcode: CP_PERFCOUNTER_ACTION (50) (4 dwords)
0000000500024058:               70d08003 00000010 001c7000 00000005

t7              opcode: CP_WAIT_FOR_IDLE (26) (1 dwords)
0000000500024068:               70268000
*/

	OUT_PKT4(ring, REG_A5XX_PC_RESTART_INDEX, 1);
	OUT_RING(ring, 0xffffffff);

	OUT_PKT4(ring, REG_A5XX_PC_RASTER_CNTL, 1);
	OUT_RING(ring, 0x00000012);

	OUT_PKT4(ring, REG_A5XX_GRAS_LRZ_CNTL, 1);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_GRAS_SU_POINT_MINMAX, 2);
	OUT_RING(ring, A5XX_GRAS_SU_POINT_MINMAX_MIN(1.0) |
			A5XX_GRAS_SU_POINT_MINMAX_MAX(4092.0));
	OUT_RING(ring, A5XX_GRAS_SU_POINT_SIZE(0.5));

	OUT_PKT4(ring, REG_A5XX_GRAS_SU_CONSERVATIVE_RAS_CNTL, 1);
	OUT_RING(ring, 0x00000000);   /* GRAS_SU_CONSERVATIVE_RAS_CNTL */

	OUT_PKT4(ring, REG_A5XX_GRAS_SC_SCREEN_SCISSOR_CNTL, 1);
	OUT_RING(ring, 0x00000000);   /* GRAS_SC_SCREEN_SCISSOR_CNTL */

	OUT_PKT4(ring, REG_A5XX_SP_VS_CONFIG_MAX_CONST, 1);
	OUT_RING(ring, 0);            /* SP_VS_CONFIG_MAX_CONST */

	OUT_PKT4(ring, REG_A5XX_SP_FS_CONFIG_MAX_CONST, 1);
	OUT_RING(ring, 0);            /* SP_FS_CONFIG_MAX_CONST */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E292, 2);
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E292 */
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E293 */

	OUT_PKT4(ring, REG_A5XX_RB_MODE_CNTL, 1);
	OUT_RING(ring, 0x00000044);   /* RB_MODE_CNTL */

	OUT_PKT4(ring, REG_A5XX_RB_DBG_ECO_CNTL, 1);
	OUT_RING(ring, 0x00100000);   /* RB_DBG_ECO_CNTL */

	OUT_PKT4(ring, REG_A5XX_VFD_MODE_CNTL, 1);
	OUT_RING(ring, 0x00000000);   /* VFD_MODE_CNTL */

	OUT_PKT4(ring, REG_A5XX_PC_MODE_CNTL, 1);
	OUT_RING(ring, 0x0000001f);   /* PC_MODE_CNTL */

	OUT_PKT4(ring, REG_A5XX_SP_MODE_CNTL, 1);
	OUT_RING(ring, 0x0000001e);   /* SP_MODE_CNTL */

	OUT_PKT4(ring, REG_A5XX_SP_DBG_ECO_CNTL, 1);
	OUT_RING(ring, 0x40000800);   /* SP_DBG_ECO_CNTL */

	OUT_PKT4(ring, REG_A5XX_TPL1_MODE_CNTL, 1);
	OUT_RING(ring, 0x00000544);   /* TPL1_MODE_CNTL */

	OUT_PKT4(ring, REG_A5XX_HLSQ_TIMEOUT_THRESHOLD_0, 2);
	OUT_RING(ring, 0x00000080);   /* HLSQ_TIMEOUT_THRESHOLD_0 */
	OUT_RING(ring, 0x00000000);   /* HLSQ_TIMEOUT_THRESHOLD_1 */

	OUT_PKT4(ring, REG_A5XX_VPC_DBG_ECO_CNTL, 1);
	OUT_RING(ring, 0x00000400);   /* VPC_DBG_ECO_CNTL */

	OUT_PKT4(ring, REG_A5XX_HLSQ_MODE_CNTL, 1);
	OUT_RING(ring, 0x00000001);   /* HLSQ_MODE_CNTL */

	OUT_PKT4(ring, REG_A5XX_VPC_MODE_CNTL, 1);
	OUT_RING(ring, 0x00000000);   /* VPC_MODE_CNTL */

	/* we don't use this yet.. probably best to disable.. */
	OUT_PKT7(ring, CP_SET_DRAW_STATE, 3);
	OUT_RING(ring, CP_SET_DRAW_STATE__0_COUNT(0) |
			CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS |
			CP_SET_DRAW_STATE__0_GROUP_ID(0));
	OUT_RING(ring, CP_SET_DRAW_STATE__1_ADDR_LO(0));
	OUT_RING(ring, CP_SET_DRAW_STATE__2_ADDR_HI(0));

	/* other regs not used (yet?) and always seem to have same value: */
	OUT_PKT4(ring, REG_A5XX_GRAS_CL_CNTL, 1);
	OUT_RING(ring, 0x00000080);   /* GRAS_CL_CNTL */

	OUT_PKT4(ring, REG_A5XX_GRAS_SU_CONSERVATIVE_RAS_CNTL, 1);
	OUT_RING(ring, 0x00000000);   /* GRAS_SU_CONSERVATIVE_RAS_CNTL */

	OUT_PKT4(ring, REG_A5XX_GRAS_SC_BIN_CNTL, 1);
	OUT_RING(ring, 0x00000000);   /* GRAS_SC_BIN_CNTL */

	OUT_PKT4(ring, REG_A5XX_GRAS_SC_BIN_CNTL, 1);
	OUT_RING(ring, 0x00000000);   /* GRAS_SC_BIN_CNTL */

	OUT_PKT4(ring, REG_A5XX_VPC_FS_PRIMITIVEID_CNTL, 1);
	OUT_RING(ring, 0x000000ff);   /* VPC_FS_PRIMITIVEID_CNTL */

	OUT_PKT4(ring, REG_A5XX_VPC_SO_OVERRIDE, 1);
	OUT_RING(ring, 0x00000001);   /* VPC_SO_OVERRIDE */

	OUT_PKT4(ring, REG_A5XX_VPC_SO_BUFFER_BASE_LO_0, 3);
	OUT_RING(ring, 0x00000000);   /* VPC_SO_BUFFER_BASE_LO_0 */
	OUT_RING(ring, 0x00000000);   /* VPC_SO_BUFFER_BASE_HI_0 */
	OUT_RING(ring, 0x00000000);   /* VPC_SO_BUFFER_SIZE_0 */

	OUT_PKT4(ring, REG_A5XX_VPC_SO_FLUSH_BASE_LO_0, 2);
	OUT_RING(ring, 0x00000000);   /* VPC_SO_FLUSH_BASE_LO_0 */
	OUT_RING(ring, 0x00000000);   /* VPC_SO_FLUSH_BASE_HI_0 */

	OUT_PKT4(ring, REG_A5XX_PC_GS_PARAM, 1);
	OUT_RING(ring, 0x00000000);   /* PC_GS_PARAM */

	OUT_PKT4(ring, REG_A5XX_PC_HS_PARAM, 1);
	OUT_RING(ring, 0x00000000);   /* PC_HS_PARAM */

	OUT_PKT4(ring, REG_A5XX_TPL1_TP_FS_ROTATION_CNTL, 1);
	OUT_RING(ring, 0x00000000);   /* TPL1_TP_FS_ROTATION_CNTL */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E001, 1);
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E001 */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E004, 1);
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E004 */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E093, 1);
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E093 */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E1C7, 1);
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E1C7 */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E29A, 1);
	OUT_RING(ring, 0x00ffff00);   /* UNKNOWN_E29A */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E2A1, 1);
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E2A1 */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E2AB, 1);
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E2AB */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E389, 1);
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E389 */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E38D, 1);
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E38D */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E5AB, 1);
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E5AB */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E5C2, 1);
	OUT_RING(ring, 0x00000000);   /* UNKNOWN_E5C2 */

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E2AE, 3);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E2B2, 6);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E2B9, 6);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E2C0, 3);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E5DB, 1);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E600, 1);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E640, 1);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_TPL1_VS_TEX_COUNT, 4);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_TPL1_FS_TEX_COUNT, 2);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E7C0, 3);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E7C5, 3);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E7CA, 3);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E7CF, 3);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E7D4, 3);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_UNKNOWN_E7D9, 3);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000000);

	// TODO hacks.. these should not be hardcoded:
	OUT_PKT4(ring, REG_A5XX_GRAS_SC_CNTL, 1);
	OUT_RING(ring, 0x00000008);   /* GRAS_SC_CNTL */

	fd_hw_query_enable(batch, ring);
}

static void
fd5_emit_ib(struct fd_ringbuffer *ring, struct fd_ringbuffer *target)
{
	__OUT_IB5(ring, target);
}

void
fd5_emit_init(struct pipe_context *pctx)
{
	struct fd_context *ctx = fd_context(pctx);
	ctx->emit_const = fd5_emit_const;
	ctx->emit_const_bo = fd5_emit_const_bo;
	ctx->emit_ib = fd5_emit_ib;
}
