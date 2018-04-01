/*
 * Copyright 2013 Advanced Micro Devices, Inc.
 * All Rights Reserved.
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

static enum pipe_reset_status si_get_reset_status(struct pipe_context *ctx)
{
	struct si_context *sctx = (struct si_context *)ctx;
	unsigned latest = sctx->b.ws->query_value(sctx->b.ws,
						  RADEON_GPU_RESET_COUNTER);

	if (sctx->b.gpu_reset_counter == latest)
		return PIPE_NO_RESET;

	sctx->b.gpu_reset_counter = latest;
	return PIPE_UNKNOWN_CONTEXT_RESET;
}

static void si_set_device_reset_callback(struct pipe_context *ctx,
					   const struct pipe_device_reset_callback *cb)
{
	struct si_context *sctx = (struct si_context *)ctx;

	if (cb)
		sctx->b.device_reset_callback = *cb;
	else
		memset(&sctx->b.device_reset_callback, 0,
		       sizeof(sctx->b.device_reset_callback));
}

bool si_check_device_reset(struct si_context *sctx)
{
	enum pipe_reset_status status;

	if (!sctx->b.device_reset_callback.reset)
		return false;

	if (!sctx->b.b.get_device_reset_status)
		return false;

	status = sctx->b.b.get_device_reset_status(&sctx->b.b);
	if (status == PIPE_NO_RESET)
		return false;

	sctx->b.device_reset_callback.reset(sctx->b.device_reset_callback.data, status);
	return true;
}

static bool si_resource_commit(struct pipe_context *pctx,
			       struct pipe_resource *resource,
			       unsigned level, struct pipe_box *box,
			       bool commit)
{
	struct si_context *ctx = (struct si_context *)pctx;
	struct r600_resource *res = r600_resource(resource);

	/*
	 * Since buffer commitment changes cannot be pipelined, we need to
	 * (a) flush any pending commands that refer to the buffer we're about
	 *     to change, and
	 * (b) wait for threaded submit to finish, including those that were
	 *     triggered by some other, earlier operation.
	 */
	if (radeon_emitted(ctx->b.gfx_cs, ctx->b.initial_gfx_cs_size) &&
	    ctx->b.ws->cs_is_buffer_referenced(ctx->b.gfx_cs,
					       res->buf, RADEON_USAGE_READWRITE)) {
		si_flush_gfx_cs(ctx, PIPE_FLUSH_ASYNC, NULL);
	}
	if (radeon_emitted(ctx->b.dma_cs, 0) &&
	    ctx->b.ws->cs_is_buffer_referenced(ctx->b.dma_cs,
					       res->buf, RADEON_USAGE_READWRITE)) {
		si_flush_dma_cs(ctx, PIPE_FLUSH_ASYNC, NULL);
	}

	ctx->b.ws->cs_sync_flush(ctx->b.dma_cs);
	ctx->b.ws->cs_sync_flush(ctx->b.gfx_cs);

	assert(resource->target == PIPE_BUFFER);

	return ctx->b.ws->buffer_commit(res->buf, box->x, box->width, commit);
}

bool si_common_context_init(struct si_context *sctx,
			    struct si_screen *sscreen,
			    unsigned context_flags)
{

	slab_create_child(&sctx->b.pool_transfers, &sscreen->pool_transfers);
	slab_create_child(&sctx->b.pool_transfers_unsync, &sscreen->pool_transfers);

	sctx->b.screen = sscreen;
	sctx->b.ws = sscreen->ws;
	sctx->b.family = sscreen->info.family;
	sctx->b.chip_class = sscreen->info.chip_class;

	sctx->b.b.resource_commit = si_resource_commit;

	if (sscreen->info.drm_major == 2 && sscreen->info.drm_minor >= 43) {
		sctx->b.b.get_device_reset_status = si_get_reset_status;
		sctx->b.gpu_reset_counter =
				sctx->b.ws->query_value(sctx->b.ws,
							RADEON_GPU_RESET_COUNTER);
	}

	sctx->b.b.set_device_reset_callback = si_set_device_reset_callback;

	si_init_context_texture_functions(sctx);
	si_init_query_functions(sctx);

	if (sctx->b.chip_class == CIK ||
	    sctx->b.chip_class == VI ||
	    sctx->b.chip_class == GFX9) {
		sctx->b.eop_bug_scratch = (struct r600_resource*)
					  pipe_buffer_create(&sscreen->b, 0, PIPE_USAGE_DEFAULT,
							     16 * sscreen->info.num_render_backends);
		if (!sctx->b.eop_bug_scratch)
			return false;
	}

	sctx->b.allocator_zeroed_memory =
			u_suballocator_create(&sctx->b.b, sscreen->info.gart_page_size,
					      0, PIPE_USAGE_DEFAULT, 0, true);
	if (!sctx->b.allocator_zeroed_memory)
		return false;

	sctx->b.b.stream_uploader = u_upload_create(&sctx->b.b, 1024 * 1024,
						    0, PIPE_USAGE_STREAM,
						    R600_RESOURCE_FLAG_READ_ONLY);
	if (!sctx->b.b.stream_uploader)
		return false;

	sctx->b.b.const_uploader = u_upload_create(&sctx->b.b, 128 * 1024,
						   0, PIPE_USAGE_DEFAULT,
						   R600_RESOURCE_FLAG_32BIT |
						   (sscreen->cpdma_prefetch_writes_memory ?
							    0 : R600_RESOURCE_FLAG_READ_ONLY));
	if (!sctx->b.b.const_uploader)
		return false;

	sctx->b.cached_gtt_allocator = u_upload_create(&sctx->b.b, 16 * 1024,
						       0, PIPE_USAGE_STAGING, 0);
	if (!sctx->b.cached_gtt_allocator)
		return false;

	sctx->b.ctx = sctx->b.ws->ctx_create(sctx->b.ws);
	if (!sctx->b.ctx)
		return false;

	if (sscreen->info.num_sdma_rings && !(sscreen->debug_flags & DBG(NO_ASYNC_DMA))) {
		sctx->b.dma_cs = sctx->b.ws->cs_create(sctx->b.ctx, RING_DMA,
						       (void*)si_flush_dma_cs,
						       sctx);
	}

	return true;
}

void si_common_context_cleanup(struct si_context *sctx)
{
	unsigned i,j;

	/* Release DCC stats. */
	for (i = 0; i < ARRAY_SIZE(sctx->b.dcc_stats); i++) {
		assert(!sctx->b.dcc_stats[i].query_active);

		for (j = 0; j < ARRAY_SIZE(sctx->b.dcc_stats[i].ps_stats); j++)
			if (sctx->b.dcc_stats[i].ps_stats[j])
				sctx->b.b.destroy_query(&sctx->b.b,
							sctx->b.dcc_stats[i].ps_stats[j]);

		r600_texture_reference(&sctx->b.dcc_stats[i].tex, NULL);
	}

	if (sctx->b.query_result_shader)
		sctx->b.b.delete_compute_state(&sctx->b.b, sctx->b.query_result_shader);

	if (sctx->b.gfx_cs)
		sctx->b.ws->cs_destroy(sctx->b.gfx_cs);
	if (sctx->b.dma_cs)
		sctx->b.ws->cs_destroy(sctx->b.dma_cs);
	if (sctx->b.ctx)
		sctx->b.ws->ctx_destroy(sctx->b.ctx);

	if (sctx->b.b.stream_uploader)
		u_upload_destroy(sctx->b.b.stream_uploader);
	if (sctx->b.b.const_uploader)
		u_upload_destroy(sctx->b.b.const_uploader);
	if (sctx->b.cached_gtt_allocator)
		u_upload_destroy(sctx->b.cached_gtt_allocator);

	slab_destroy_child(&sctx->b.pool_transfers);
	slab_destroy_child(&sctx->b.pool_transfers_unsync);

	if (sctx->b.allocator_zeroed_memory) {
		u_suballocator_destroy(sctx->b.allocator_zeroed_memory);
	}
	sctx->b.ws->fence_reference(&sctx->b.last_gfx_fence, NULL);
	sctx->b.ws->fence_reference(&sctx->b.last_sdma_fence, NULL);
	r600_resource_reference(&sctx->b.eop_bug_scratch, NULL);
}
