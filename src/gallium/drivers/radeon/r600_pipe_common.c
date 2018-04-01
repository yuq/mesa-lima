/*
 * Copyright 2013 Advanced Micro Devices, Inc.
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
 */

#include "r600_pipe_common.h"
#include "r600_cs.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "radeon/radeon_video.h"

/*
 * pipe_context
 */

static enum pipe_reset_status r600_get_reset_status(struct pipe_context *ctx)
{
	struct r600_common_context *rctx = (struct r600_common_context *)ctx;
	unsigned latest = rctx->ws->query_value(rctx->ws,
						RADEON_GPU_RESET_COUNTER);

	if (rctx->gpu_reset_counter == latest)
		return PIPE_NO_RESET;

	rctx->gpu_reset_counter = latest;
	return PIPE_UNKNOWN_CONTEXT_RESET;
}

static void r600_set_device_reset_callback(struct pipe_context *ctx,
					   const struct pipe_device_reset_callback *cb)
{
	struct r600_common_context *rctx = (struct r600_common_context *)ctx;

	if (cb)
		rctx->device_reset_callback = *cb;
	else
		memset(&rctx->device_reset_callback, 0,
		       sizeof(rctx->device_reset_callback));
}

bool si_check_device_reset(struct r600_common_context *rctx)
{
	enum pipe_reset_status status;

	if (!rctx->device_reset_callback.reset)
		return false;

	if (!rctx->b.get_device_reset_status)
		return false;

	status = rctx->b.get_device_reset_status(&rctx->b);
	if (status == PIPE_NO_RESET)
		return false;

	rctx->device_reset_callback.reset(rctx->device_reset_callback.data, status);
	return true;
}

static bool r600_resource_commit(struct pipe_context *pctx,
				 struct pipe_resource *resource,
				 unsigned level, struct pipe_box *box,
				 bool commit)
{
	struct r600_common_context *ctx = (struct r600_common_context *)pctx;
	struct r600_resource *res = r600_resource(resource);

	/*
	 * Since buffer commitment changes cannot be pipelined, we need to
	 * (a) flush any pending commands that refer to the buffer we're about
	 *     to change, and
	 * (b) wait for threaded submit to finish, including those that were
	 *     triggered by some other, earlier operation.
	 */
	if (radeon_emitted(ctx->gfx.cs, ctx->initial_gfx_cs_size) &&
	    ctx->ws->cs_is_buffer_referenced(ctx->gfx.cs,
					     res->buf, RADEON_USAGE_READWRITE)) {
		si_flush_gfx_cs(ctx, PIPE_FLUSH_ASYNC, NULL);
	}
	if (radeon_emitted(ctx->dma.cs, 0) &&
	    ctx->ws->cs_is_buffer_referenced(ctx->dma.cs,
					     res->buf, RADEON_USAGE_READWRITE)) {
		si_flush_dma_cs(ctx, PIPE_FLUSH_ASYNC, NULL);
	}

	ctx->ws->cs_sync_flush(ctx->dma.cs);
	ctx->ws->cs_sync_flush(ctx->gfx.cs);

	assert(resource->target == PIPE_BUFFER);

	return ctx->ws->buffer_commit(res->buf, box->x, box->width, commit);
}

bool si_common_context_init(struct r600_common_context *rctx,
			    struct si_screen *sscreen,
			    unsigned context_flags)
{
	slab_create_child(&rctx->pool_transfers, &sscreen->pool_transfers);
	slab_create_child(&rctx->pool_transfers_unsync, &sscreen->pool_transfers);

	rctx->screen = sscreen;
	rctx->ws = sscreen->ws;
	rctx->family = sscreen->info.family;
	rctx->chip_class = sscreen->info.chip_class;

	rctx->b.resource_commit = r600_resource_commit;

	if (sscreen->info.drm_major == 2 && sscreen->info.drm_minor >= 43) {
		rctx->b.get_device_reset_status = r600_get_reset_status;
		rctx->gpu_reset_counter =
			rctx->ws->query_value(rctx->ws,
					      RADEON_GPU_RESET_COUNTER);
	}

	rctx->b.set_device_reset_callback = r600_set_device_reset_callback;

	si_init_context_texture_functions(rctx);
	si_init_query_functions(rctx);

	if (rctx->chip_class == CIK ||
	    rctx->chip_class == VI ||
	    rctx->chip_class == GFX9) {
		rctx->eop_bug_scratch = (struct r600_resource*)
			pipe_buffer_create(&sscreen->b, 0, PIPE_USAGE_DEFAULT,
					   16 * sscreen->info.num_render_backends);
		if (!rctx->eop_bug_scratch)
			return false;
	}

	rctx->allocator_zeroed_memory =
		u_suballocator_create(&rctx->b, sscreen->info.gart_page_size,
				      0, PIPE_USAGE_DEFAULT, 0, true);
	if (!rctx->allocator_zeroed_memory)
		return false;

	rctx->b.stream_uploader = u_upload_create(&rctx->b, 1024 * 1024,
						  0, PIPE_USAGE_STREAM,
						  R600_RESOURCE_FLAG_READ_ONLY);
	if (!rctx->b.stream_uploader)
		return false;

	rctx->b.const_uploader = u_upload_create(&rctx->b, 128 * 1024,
						 0, PIPE_USAGE_DEFAULT,
						 R600_RESOURCE_FLAG_32BIT |
						 (sscreen->cpdma_prefetch_writes_memory ?
							0 : R600_RESOURCE_FLAG_READ_ONLY));
	if (!rctx->b.const_uploader)
		return false;

	rctx->cached_gtt_allocator = u_upload_create(&rctx->b, 16 * 1024,
						     0, PIPE_USAGE_STAGING, 0);
	if (!rctx->cached_gtt_allocator)
		return false;

	rctx->ctx = rctx->ws->ctx_create(rctx->ws);
	if (!rctx->ctx)
		return false;

	if (sscreen->info.num_sdma_rings && !(sscreen->debug_flags & DBG(NO_ASYNC_DMA))) {
		rctx->dma.cs = rctx->ws->cs_create(rctx->ctx, RING_DMA,
						   si_flush_dma_cs,
						   rctx);
		rctx->dma.flush = si_flush_dma_cs;
	}

	return true;
}

void si_common_context_cleanup(struct r600_common_context *rctx)
{
	unsigned i,j;

	/* Release DCC stats. */
	for (i = 0; i < ARRAY_SIZE(rctx->dcc_stats); i++) {
		assert(!rctx->dcc_stats[i].query_active);

		for (j = 0; j < ARRAY_SIZE(rctx->dcc_stats[i].ps_stats); j++)
			if (rctx->dcc_stats[i].ps_stats[j])
				rctx->b.destroy_query(&rctx->b,
						      rctx->dcc_stats[i].ps_stats[j]);

		r600_texture_reference(&rctx->dcc_stats[i].tex, NULL);
	}

	if (rctx->query_result_shader)
		rctx->b.delete_compute_state(&rctx->b, rctx->query_result_shader);

	if (rctx->gfx.cs)
		rctx->ws->cs_destroy(rctx->gfx.cs);
	if (rctx->dma.cs)
		rctx->ws->cs_destroy(rctx->dma.cs);
	if (rctx->ctx)
		rctx->ws->ctx_destroy(rctx->ctx);

	if (rctx->b.stream_uploader)
		u_upload_destroy(rctx->b.stream_uploader);
	if (rctx->b.const_uploader)
		u_upload_destroy(rctx->b.const_uploader);
	if (rctx->cached_gtt_allocator)
		u_upload_destroy(rctx->cached_gtt_allocator);

	slab_destroy_child(&rctx->pool_transfers);
	slab_destroy_child(&rctx->pool_transfers_unsync);

	if (rctx->allocator_zeroed_memory) {
		u_suballocator_destroy(rctx->allocator_zeroed_memory);
	}
	rctx->ws->fence_reference(&rctx->last_gfx_fence, NULL);
	rctx->ws->fence_reference(&rctx->last_sdma_fence, NULL);
	r600_resource_reference(&rctx->eop_bug_scratch, NULL);
}
