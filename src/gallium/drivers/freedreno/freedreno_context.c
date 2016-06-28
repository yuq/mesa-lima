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

#include "freedreno_context.h"
#include "freedreno_draw.h"
#include "freedreno_fence.h"
#include "freedreno_program.h"
#include "freedreno_resource.h"
#include "freedreno_texture.h"
#include "freedreno_state.h"
#include "freedreno_gmem.h"
#include "freedreno_query.h"
#include "freedreno_query_hw.h"
#include "freedreno_util.h"

static void
fd_context_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence,
		unsigned flags)
{
	struct fd_context *ctx = fd_context(pctx);
	uint32_t timestamp;

	if (!ctx->screen->reorder) {
		struct fd_batch *batch = NULL;
		fd_batch_reference(&batch, ctx->batch);
		fd_batch_flush(batch);
		timestamp = fd_ringbuffer_timestamp(batch->gmem);
		fd_batch_reference(&batch, NULL);
	} else {
		timestamp = fd_bc_flush(&ctx->screen->batch_cache, ctx);
	}

	if (fence) {
		fd_screen_fence_ref(pctx->screen, fence, NULL);
		*fence = fd_fence_create(pctx, timestamp);
	}
}

/**
 * emit marker string as payload of a no-op packet, which can be
 * decoded by cffdump.
 */
static void
fd_emit_string_marker(struct pipe_context *pctx, const char *string, int len)
{
	struct fd_context *ctx = fd_context(pctx);
	struct fd_ringbuffer *ring;
	const uint32_t *buf = (const void *)string;

	if (!ctx->batch)
		return;

	ring = ctx->batch->draw;

	/* max packet size is 0x3fff dwords: */
	len = MIN2(len, 0x3fff * 4);

	OUT_PKT3(ring, CP_NOP, align(len, 4) / 4);
	while (len >= 4) {
		OUT_RING(ring, *buf);
		buf++;
		len -= 4;
	}

	/* copy remainder bytes without reading past end of input string: */
	if (len > 0) {
		uint32_t w = 0;
		memcpy(&w, buf, len);
		OUT_RING(ring, w);
	}
}

void
fd_context_destroy(struct pipe_context *pctx)
{
	struct fd_context *ctx = fd_context(pctx);
	unsigned i;

	DBG("");

	fd_batch_reference(&ctx->batch, NULL);  /* unref current batch */
	fd_bc_invalidate_context(ctx);

	fd_prog_fini(pctx);
	fd_hw_query_fini(pctx);

	if (ctx->blitter)
		util_blitter_destroy(ctx->blitter);

	if (ctx->primconvert)
		util_primconvert_destroy(ctx->primconvert);

	util_slab_destroy(&ctx->transfer_pool);

	for (i = 0; i < ARRAY_SIZE(ctx->pipe); i++) {
		struct fd_vsc_pipe *pipe = &ctx->pipe[i];
		if (!pipe->bo)
			break;
		fd_bo_del(pipe->bo);
	}

	fd_device_del(ctx->dev);

	if (fd_mesa_debug & (FD_DBG_BSTAT | FD_DBG_MSGS)) {
		printf("batch_total=%u, batch_sysmem=%u, batch_gmem=%u, batch_restore=%u\n",
			(uint32_t)ctx->stats.batch_total, (uint32_t)ctx->stats.batch_sysmem,
			(uint32_t)ctx->stats.batch_gmem, (uint32_t)ctx->stats.batch_restore);
	}

	FREE(ctx);
}

static void
fd_set_debug_callback(struct pipe_context *pctx,
		const struct pipe_debug_callback *cb)
{
	struct fd_context *ctx = fd_context(pctx);

	if (cb)
		ctx->debug = *cb;
	else
		memset(&ctx->debug, 0, sizeof(ctx->debug));
}

struct pipe_context *
fd_context_init(struct fd_context *ctx, struct pipe_screen *pscreen,
		const uint8_t *primtypes, void *priv)
{
	struct fd_screen *screen = fd_screen(pscreen);
	struct pipe_context *pctx;
	int i;

	ctx->screen = screen;

	ctx->primtypes = primtypes;
	ctx->primtype_mask = 0;
	for (i = 0; i < PIPE_PRIM_MAX; i++)
		if (primtypes[i])
			ctx->primtype_mask |= (1 << i);

	/* need some sane default in case state tracker doesn't
	 * set some state:
	 */
	ctx->sample_mask = 0xffff;

	ctx->stage = FD_STAGE_NULL;

	pctx = &ctx->base;
	pctx->screen = pscreen;
	pctx->priv = priv;
	pctx->flush = fd_context_flush;
	pctx->emit_string_marker = fd_emit_string_marker;
	pctx->set_debug_callback = fd_set_debug_callback;

	/* TODO what about compute?  Ideally it creates it's own independent
	 * batches per compute job (since it isn't using tiling, so no point
	 * in getting involved with the re-ordering madness)..
	 */
	if (!screen->reorder)
		ctx->batch = fd_bc_alloc_batch(&screen->batch_cache, ctx);

	fd_reset_wfi(ctx);

	util_slab_create(&ctx->transfer_pool, sizeof(struct fd_transfer),
			16, UTIL_SLAB_SINGLETHREADED);

	fd_draw_init(pctx);
	fd_resource_context_init(pctx);
	fd_query_context_init(pctx);
	fd_texture_init(pctx);
	fd_state_init(pctx);
	fd_hw_query_init(pctx);

	ctx->blitter = util_blitter_create(pctx);
	if (!ctx->blitter)
		goto fail;

	ctx->primconvert = util_primconvert_create(pctx, ctx->primtype_mask);
	if (!ctx->primconvert)
		goto fail;

	return pctx;

fail:
	pctx->destroy(pctx);
	return NULL;
}
