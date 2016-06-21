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

#include "util/list.h"
#include "util/u_string.h"

#include "freedreno_batch.h"
#include "freedreno_context.h"
#include "freedreno_resource.h"

struct fd_batch *
fd_batch_create(struct fd_context *ctx)
{
	struct fd_batch *batch = CALLOC_STRUCT(fd_batch);
	static unsigned seqno = 0;
	unsigned size = 0;

	if (!batch)
		return NULL;

	pipe_reference_init(&batch->reference, 1);
	batch->seqno = ++seqno;
	batch->ctx = ctx;

	/* if kernel is too old to support unlimited # of cmd buffers, we
	 * have no option but to allocate large worst-case sizes so that
	 * we don't need to grow the ringbuffer.  Performance is likely to
	 * suffer, but there is no good alternative.
	 */
	if (fd_device_version(ctx->screen->dev) < FD_VERSION_UNLIMITED_CMDS) {
		size = 0x100000;
	}

	batch->draw    = fd_ringbuffer_new(ctx->screen->pipe, size);
	batch->binning = fd_ringbuffer_new(ctx->screen->pipe, size);
	batch->gmem    = fd_ringbuffer_new(ctx->screen->pipe, size);

	fd_ringbuffer_set_parent(batch->gmem, NULL);
	fd_ringbuffer_set_parent(batch->draw, batch->gmem);
	fd_ringbuffer_set_parent(batch->binning, batch->gmem);

	list_inithead(&batch->used_resources);

	return batch;
}

void
__fd_batch_destroy(struct fd_batch *batch)
{
	fd_ringbuffer_del(batch->draw);
	fd_ringbuffer_del(batch->binning);
	fd_ringbuffer_del(batch->gmem);

	free(batch);
}

void
__fd_batch_describe(char* buf, const struct fd_batch *batch)
{
	util_sprintf(buf, "fd_batch<%u>", batch->seqno);
}

void
fd_batch_flush(struct fd_batch *batch)
{
	struct fd_resource *rsc, *rsc_tmp;

	fd_gmem_render_tiles(batch->ctx);

	/* go through all the used resources and clear their reading flag */
	LIST_FOR_EACH_ENTRY_SAFE(rsc, rsc_tmp, &batch->used_resources, list) {
		debug_assert(rsc->pending_batch == batch);
		debug_assert(rsc->status != 0);
		rsc->status = 0;
		fd_batch_reference(&rsc->pending_batch, NULL);
		list_delinit(&rsc->list);
	}

	assert(LIST_IS_EMPTY(&batch->used_resources));
}

void
fd_batch_resource_used(struct fd_batch *batch, struct fd_resource *rsc,
		enum fd_resource_status status)
{
	rsc->status |= status;

	if (rsc->stencil)
		rsc->stencil->status |= status;

	/* TODO resources can actually be shared across contexts,
	 * so I'm not sure a single list-head will do the trick?
	 */
	debug_assert((rsc->pending_batch == batch) || !rsc->pending_batch);
	list_delinit(&rsc->list);
	list_addtail(&rsc->list, &batch->used_resources);
	fd_batch_reference(&rsc->pending_batch, batch);
}

void
fd_batch_check_size(struct fd_batch *batch)
{
	if (fd_device_version(batch->ctx->screen->dev) >= FD_VERSION_UNLIMITED_CMDS)
		return;

	struct fd_ringbuffer *ring = batch->draw;
	if (((ring->cur - ring->start) > (ring->size/4 - 0x1000)) ||
			(fd_mesa_debug & FD_DBG_FLUSH))
		fd_context_render(&batch->ctx->base);
}
