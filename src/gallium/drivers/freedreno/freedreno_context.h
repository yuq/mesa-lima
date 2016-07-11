/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright (C) 2012 Rob Clark <robclark@freedesktop.org>
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

#ifndef FREEDRENO_CONTEXT_H_
#define FREEDRENO_CONTEXT_H_

#include "pipe/p_context.h"
#include "indices/u_primconvert.h"
#include "util/u_blitter.h"
#include "util/list.h"
#include "util/u_slab.h"
#include "util/u_string.h"

#include "freedreno_batch.h"
#include "freedreno_screen.h"
#include "freedreno_gmem.h"
#include "freedreno_util.h"

#define BORDER_COLOR_UPLOAD_SIZE (2 * PIPE_MAX_SAMPLERS * BORDERCOLOR_SIZE)

struct fd_vertex_stateobj;

struct fd_texture_stateobj {
	struct pipe_sampler_view *textures[PIPE_MAX_SAMPLERS];
	unsigned num_textures;
	unsigned valid_textures;
	struct pipe_sampler_state *samplers[PIPE_MAX_SAMPLERS];
	unsigned num_samplers;
	unsigned valid_samplers;
};

struct fd_program_stateobj {
	void *vp, *fp;

	/* rest only used by fd2.. split out: */
	uint8_t num_exports;
	/* Indexed by semantic name or TGSI_SEMANTIC_COUNT + semantic index
	 * for TGSI_SEMANTIC_GENERIC.  Special vs exports (position and point-
	 * size) are not included in this
	 */
	uint8_t export_linkage[63];
};

struct fd_constbuf_stateobj {
	struct pipe_constant_buffer cb[PIPE_MAX_CONSTANT_BUFFERS];
	uint32_t enabled_mask;
	uint32_t dirty_mask;
};

struct fd_vertexbuf_stateobj {
	struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
	unsigned count;
	uint32_t enabled_mask;
	uint32_t dirty_mask;
};

struct fd_vertex_stateobj {
	struct pipe_vertex_element pipe[PIPE_MAX_ATTRIBS];
	unsigned num_elements;
};

struct fd_streamout_stateobj {
	struct pipe_stream_output_target *targets[PIPE_MAX_SO_BUFFERS];
	unsigned num_targets;
	/* Track offset from vtxcnt for streamout data.  This counter
	 * is just incremented by # of vertices on each draw until
	 * reset or new streamout buffer bound.
	 *
	 * When we eventually have GS, the CPU won't actually know the
	 * number of vertices per draw, so I think we'll have to do
	 * something more clever.
	 */
	unsigned offsets[PIPE_MAX_SO_BUFFERS];
};

/* group together the vertex and vertexbuf state.. for ease of passing
 * around, and because various internal operations (gmem<->mem, etc)
 * need their own vertex state:
 */
struct fd_vertex_state {
	struct fd_vertex_stateobj *vtx;
	struct fd_vertexbuf_stateobj vertexbuf;
};


struct fd_context {
	struct pipe_context base;

	struct fd_device *dev;
	struct fd_screen *screen;

	struct blitter_context *blitter;
	struct primconvert_context *primconvert;

	/* slab for pipe_transfer allocations: */
	struct util_slab_mempool transfer_pool;

	/* slabs for fd_hw_sample and fd_hw_sample_period allocations: */
	struct util_slab_mempool sample_pool;
	struct util_slab_mempool sample_period_pool;

	/* sample-providers for hw queries: */
	const struct fd_hw_sample_provider *sample_providers[MAX_HW_SAMPLE_PROVIDERS];

	/* list of active queries: */
	struct list_head active_queries;

	/* table with PIPE_PRIM_MAX entries mapping PIPE_PRIM_x to
	 * DI_PT_x value to use for draw initiator.  There are some
	 * slight differences between generation:
	 */
	const uint8_t *primtypes;
	uint32_t primtype_mask;

	/* shaders used by clear, and gmem->mem blits: */
	struct fd_program_stateobj solid_prog; // TODO move to screen?

	/* shaders used by mem->gmem blits: */
	struct fd_program_stateobj blit_prog[MAX_RENDER_TARGETS]; // TODO move to screen?
	struct fd_program_stateobj blit_z, blit_zs;

	/* Stats/counters:
	 */
	struct {
		uint64_t prims_emitted;
		uint64_t prims_generated;
		uint64_t draw_calls;
		uint64_t batch_total, batch_sysmem, batch_gmem, batch_restore;
	} stats;

	/* Current batch.. the rule here is that you can deref ctx->batch
	 * in codepaths from pipe_context entrypoints.  But not in code-
	 * paths from fd_batch_flush() (basically, the stuff that gets
	 * called from GMEM code), since in those code-paths the batch
	 * you care about is not necessarily the same as ctx->batch.
	 */
	struct fd_batch *batch;

	/* Keep track if WAIT_FOR_IDLE is needed for registers we need
	 * to update via RMW:
	 */
	bool needs_wfi;

	/* Do we need to re-emit RB_FRAME_BUFFER_DIMENSION?  At least on a3xx
	 * it is not a banked context register, so it needs a WFI to update.
	 * Keep track if it has actually changed, to avoid unneeded WFI.
	 * */
	bool needs_rb_fbd;

	struct pipe_scissor_state scissor;

	/* we don't have a disable/enable bit for scissor, so instead we keep
	 * a disabled-scissor state which matches the entire bound framebuffer
	 * and use that when scissor is not enabled.
	 */
	struct pipe_scissor_state disabled_scissor;

	/* Current gmem/tiling configuration.. gets updated on render_tiles()
	 * if out of date with current maximal-scissor/cpp:
	 *
	 * (NOTE: this is kind of related to the batch, but moving it there
	 * means we'd always have to recalc tiles ever batch)
	 */
	struct fd_gmem_stateobj gmem;
	struct fd_vsc_pipe      pipe[8];
	struct fd_tile          tile[512];

	/* which state objects need to be re-emit'd: */
	enum {
		FD_DIRTY_BLEND       = (1 <<  0),
		FD_DIRTY_RASTERIZER  = (1 <<  1),
		FD_DIRTY_ZSA         = (1 <<  2),
		FD_DIRTY_FRAGTEX     = (1 <<  3),
		FD_DIRTY_VERTTEX     = (1 <<  4),
		FD_DIRTY_TEXSTATE    = (1 <<  5),

		FD_SHADER_DIRTY_VP   = (1 <<  6),
		FD_SHADER_DIRTY_FP   = (1 <<  7),
		/* skip geom/tcs/tes/compute */
		FD_DIRTY_PROG        = FD_SHADER_DIRTY_FP | FD_SHADER_DIRTY_VP,

		FD_DIRTY_BLEND_COLOR = (1 << 12),
		FD_DIRTY_STENCIL_REF = (1 << 13),
		FD_DIRTY_SAMPLE_MASK = (1 << 14),
		FD_DIRTY_FRAMEBUFFER = (1 << 15),
		FD_DIRTY_STIPPLE     = (1 << 16),
		FD_DIRTY_VIEWPORT    = (1 << 17),
		FD_DIRTY_CONSTBUF    = (1 << 18),
		FD_DIRTY_VTXSTATE    = (1 << 19),
		FD_DIRTY_VTXBUF      = (1 << 20),
		FD_DIRTY_INDEXBUF    = (1 << 21),
		FD_DIRTY_SCISSOR     = (1 << 22),
		FD_DIRTY_STREAMOUT   = (1 << 23),
		FD_DIRTY_UCP         = (1 << 24),
		FD_DIRTY_BLEND_DUAL  = (1 << 25),
	} dirty;

	struct pipe_blend_state *blend;
	struct pipe_rasterizer_state *rasterizer;
	struct pipe_depth_stencil_alpha_state *zsa;

	struct fd_texture_stateobj verttex, fragtex;

	struct fd_program_stateobj prog;

	struct fd_vertex_state vtx;

	struct pipe_blend_color blend_color;
	struct pipe_stencil_ref stencil_ref;
	unsigned sample_mask;
	struct pipe_poly_stipple stipple;
	struct pipe_viewport_state viewport;
	struct fd_constbuf_stateobj constbuf[PIPE_SHADER_TYPES];
	struct pipe_index_buffer indexbuf;
	struct fd_streamout_stateobj streamout;
	struct pipe_clip_state ucp;

	struct pipe_query *cond_query;
	bool cond_cond; /* inverted rendering condition */
	uint cond_mode;

	/* Are we in process of shadowing a resource? Used to detect recursion
	 * in transfer_map, and skip unneeded synchronization.
	 */
	bool in_shadow;

	/* Ie. in blit situation where we no longer care about previous framebuffer
	 * contents.  Main point is to eliminate blits from fd_try_shadow_resource().
	 * For example, in case of texture upload + gen-mipmaps.
	 */
	bool discard;

	struct pipe_debug_callback debug;

	/* GMEM/tile handling fxns: */
	void (*emit_tile_init)(struct fd_batch *batch);
	void (*emit_tile_prep)(struct fd_batch *batch, struct fd_tile *tile);
	void (*emit_tile_mem2gmem)(struct fd_batch *batch, struct fd_tile *tile);
	void (*emit_tile_renderprep)(struct fd_batch *batch, struct fd_tile *tile);
	void (*emit_tile_gmem2mem)(struct fd_batch *batch, struct fd_tile *tile);

	/* optional, for GMEM bypass: */
	void (*emit_sysmem_prep)(struct fd_batch *batch);

	/* draw: */
	bool (*draw_vbo)(struct fd_context *ctx, const struct pipe_draw_info *info);
	void (*clear)(struct fd_context *ctx, unsigned buffers,
			const union pipe_color_union *color, double depth, unsigned stencil);

	/* constant emit:  (note currently not used/needed for a2xx) */
	void (*emit_const)(struct fd_ringbuffer *ring, enum shader_t type,
			uint32_t regid, uint32_t offset, uint32_t sizedwords,
			const uint32_t *dwords, struct pipe_resource *prsc);
	/* emit bo addresses as constant: */
	void (*emit_const_bo)(struct fd_ringbuffer *ring, enum shader_t type, boolean write,
			uint32_t regid, uint32_t num, struct pipe_resource **prscs, uint32_t *offsets);

	/* indirect-branch emit: */
	void (*emit_ib)(struct fd_ringbuffer *ring, struct fd_ringbuffer *target);
};

static inline struct fd_context *
fd_context(struct pipe_context *pctx)
{
	return (struct fd_context *)pctx;
}

static inline struct pipe_scissor_state *
fd_context_get_scissor(struct fd_context *ctx)
{
	if (ctx->rasterizer && ctx->rasterizer->scissor)
		return &ctx->scissor;
	return &ctx->disabled_scissor;
}

static inline bool
fd_supported_prim(struct fd_context *ctx, unsigned prim)
{
	return (1 << prim) & ctx->primtype_mask;
}

static inline void
fd_reset_wfi(struct fd_context *ctx)
{
	ctx->needs_wfi = true;
}

/* emit a WAIT_FOR_IDLE only if needed, ie. if there has not already
 * been one since last draw:
 */
static inline void
fd_wfi(struct fd_context *ctx, struct fd_ringbuffer *ring)
{
	if (ctx->needs_wfi) {
		OUT_WFI(ring);
		ctx->needs_wfi = false;
	}
}

/* emit a CP_EVENT_WRITE:
 */
static inline void
fd_event_write(struct fd_context *ctx, struct fd_ringbuffer *ring,
		enum vgt_event_type evt)
{
	OUT_PKT3(ring, CP_EVENT_WRITE, 1);
	OUT_RING(ring, evt);
	fd_reset_wfi(ctx);
}

struct pipe_context * fd_context_init(struct fd_context *ctx,
		struct pipe_screen *pscreen, const uint8_t *primtypes,
		void *priv);

void fd_context_destroy(struct pipe_context *pctx);

#endif /* FREEDRENO_CONTEXT_H_ */
