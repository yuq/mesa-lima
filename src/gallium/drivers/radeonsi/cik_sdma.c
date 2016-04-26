/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 * Copyright 2014,2015 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *      Jerome Glisse
 */

#include "sid.h"
#include "si_pipe.h"
#include "radeon/r600_cs.h"

#include "util/u_format.h"

static void cik_sdma_do_copy_buffer(struct si_context *ctx,
				    struct pipe_resource *dst,
				    struct pipe_resource *src,
				    uint64_t dst_offset,
				    uint64_t src_offset,
				    uint64_t size)
{
	struct radeon_winsys_cs *cs = ctx->b.dma.cs;
	unsigned i, ncopy, csize;
	struct r600_resource *rdst = (struct r600_resource*)dst;
	struct r600_resource *rsrc = (struct r600_resource*)src;

	dst_offset += r600_resource(dst)->gpu_address;
	src_offset += r600_resource(src)->gpu_address;

	ncopy = DIV_ROUND_UP(size, CIK_SDMA_COPY_MAX_SIZE);
	r600_need_dma_space(&ctx->b, ncopy * 7);

	radeon_add_to_buffer_list(&ctx->b, &ctx->b.dma, rsrc, RADEON_USAGE_READ,
			      RADEON_PRIO_SDMA_BUFFER);
	radeon_add_to_buffer_list(&ctx->b, &ctx->b.dma, rdst, RADEON_USAGE_WRITE,
			      RADEON_PRIO_SDMA_BUFFER);

	for (i = 0; i < ncopy; i++) {
		csize = MIN2(size, CIK_SDMA_COPY_MAX_SIZE);
		cs->buf[cs->cdw++] = CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
						     CIK_SDMA_COPY_SUB_OPCODE_LINEAR,
						     0);
		cs->buf[cs->cdw++] = csize;
		cs->buf[cs->cdw++] = 0; /* src/dst endian swap */
		cs->buf[cs->cdw++] = src_offset;
		cs->buf[cs->cdw++] = src_offset >> 32;
		cs->buf[cs->cdw++] = dst_offset;
		cs->buf[cs->cdw++] = dst_offset >> 32;
		dst_offset += csize;
		src_offset += csize;
		size -= csize;
	}
}

static void cik_sdma_copy_buffer(struct si_context *ctx,
				 struct pipe_resource *dst,
				 struct pipe_resource *src,
				 uint64_t dst_offset,
				 uint64_t src_offset,
				 uint64_t size)
{
	struct r600_resource *rdst = (struct r600_resource*)dst;

	/* Mark the buffer range of destination as valid (initialized),
	 * so that transfer_map knows it should wait for the GPU when mapping
	 * that range. */
	util_range_add(&rdst->valid_buffer_range, dst_offset,
		       dst_offset + size);

	cik_sdma_do_copy_buffer(ctx, dst, src, dst_offset, src_offset, size);
}

static void cik_sdma_copy(struct pipe_context *ctx,
			  struct pipe_resource *dst,
			  unsigned dst_level,
			  unsigned dstx, unsigned dsty, unsigned dstz,
			  struct pipe_resource *src,
			  unsigned src_level,
			  const struct pipe_box *src_box)
{
	struct si_context *sctx = (struct si_context *)ctx;

	if (!sctx->b.dma.cs)
		goto fallback;

	if (dst->target == PIPE_BUFFER && src->target == PIPE_BUFFER) {
		cik_sdma_copy_buffer(sctx, dst, src, dstx, src_box->x, src_box->width);
		return;
	}

fallback:
	si_resource_copy_region(ctx, dst, dst_level, dstx, dsty, dstz,
				src, src_level, src_box);
}

void cik_init_sdma_functions(struct si_context *sctx)
{
	sctx->b.dma_copy = cik_sdma_copy;
}
