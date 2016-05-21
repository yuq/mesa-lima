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

#ifndef FREEDRENO_BATCH_H_
#define FREEDRENO_BATCH_H_

#include "util/u_inlines.h"

#include "freedreno_util.h"

struct fd_context;
struct fd_resource;
enum fd_resource_status;

/* A batch tracks everything about a cmdstream batch/submit, including the
 * ringbuffers used for binning, draw, and gmem cmds, list of associated
 * fd_resource-s, etc.
 */
struct fd_batch {
	struct pipe_reference reference;
	unsigned seqno;
	struct fd_context *ctx;

	/** draw pass cmdstream: */
	struct fd_ringbuffer *draw;
	/** binning pass cmdstream: */
	struct fd_ringbuffer *binning;
	/** tiling/gmem (IB0) cmdstream: */
	struct fd_ringbuffer *gmem;

	/** list of resources used by currently-unsubmitted batch */
	struct list_head used_resources;
};

struct fd_batch * fd_batch_create(struct fd_context *ctx);

void fd_batch_flush(struct fd_batch *batch);
void fd_batch_resource_used(struct fd_batch *batch, struct fd_resource *rsc,
		enum fd_resource_status status);
void fd_batch_check_size(struct fd_batch *batch);

/* not called directly: */
void __fd_batch_describe(char* buf, const struct fd_batch *batch);
void __fd_batch_destroy(struct fd_batch *batch);

static inline void
fd_batch_reference(struct fd_batch **ptr, struct fd_batch *batch)
{
	struct fd_batch *old_batch = *ptr;
	if (pipe_reference_described(&(*ptr)->reference, &batch->reference,
			(debug_reference_descriptor)__fd_batch_describe))
		__fd_batch_destroy(old_batch);
	*ptr = batch;
}

#endif /* FREEDRENO_BATCH_H_ */
