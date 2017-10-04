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
 *
 * Authors: Marek Olšák <maraeo@gmail.com>
 *
 */

#include "r600_pipe_common.h"
#include "r600_cs.h"

#include "util/u_memory.h"

static void r600_set_streamout_enable(struct r600_common_context *rctx, bool enable);

static struct pipe_stream_output_target *
r600_create_so_target(struct pipe_context *ctx,
		      struct pipe_resource *buffer,
		      unsigned buffer_offset,
		      unsigned buffer_size)
{
	struct r600_common_context *rctx = (struct r600_common_context *)ctx;
	struct r600_so_target *t;
	struct r600_resource *rbuffer = (struct r600_resource*)buffer;

	t = CALLOC_STRUCT(r600_so_target);
	if (!t) {
		return NULL;
	}

	u_suballocator_alloc(rctx->allocator_zeroed_memory, 4, 4,
			     &t->buf_filled_size_offset,
			     (struct pipe_resource**)&t->buf_filled_size);
	if (!t->buf_filled_size) {
		FREE(t);
		return NULL;
	}

	t->b.reference.count = 1;
	t->b.context = ctx;
	pipe_resource_reference(&t->b.buffer, buffer);
	t->b.buffer_offset = buffer_offset;
	t->b.buffer_size = buffer_size;

	util_range_add(&rbuffer->valid_buffer_range, buffer_offset,
		       buffer_offset + buffer_size);
	return &t->b;
}

static void r600_so_target_destroy(struct pipe_context *ctx,
				   struct pipe_stream_output_target *target)
{
	struct r600_so_target *t = (struct r600_so_target*)target;
	pipe_resource_reference(&t->b.buffer, NULL);
	r600_resource_reference(&t->buf_filled_size, NULL);
	FREE(t);
}

void si_streamout_buffers_dirty(struct r600_common_context *rctx)
{
	if (!rctx->streamout.enabled_mask)
		return;

	rctx->set_atom_dirty(rctx, &rctx->streamout.begin_atom, true);
	r600_set_streamout_enable(rctx, true);
}

void si_common_set_streamout_targets(struct pipe_context *ctx,
				     unsigned num_targets,
				     struct pipe_stream_output_target **targets,
				     const unsigned *offsets)
{
	struct r600_common_context *rctx = (struct r600_common_context *)ctx;
	unsigned i;
        unsigned enabled_mask = 0, append_bitmask = 0;

	/* Stop streamout. */
	if (rctx->streamout.num_targets && rctx->streamout.begin_emitted) {
		si_emit_streamout_end(rctx);
	}

	/* Set the new targets. */
	for (i = 0; i < num_targets; i++) {
		pipe_so_target_reference((struct pipe_stream_output_target**)&rctx->streamout.targets[i], targets[i]);
		if (!targets[i])
			continue;

		r600_context_add_resource_size(ctx, targets[i]->buffer);
		enabled_mask |= 1 << i;
		if (offsets[i] == ((unsigned)-1))
			append_bitmask |= 1 << i;
	}
	for (; i < rctx->streamout.num_targets; i++) {
		pipe_so_target_reference((struct pipe_stream_output_target**)&rctx->streamout.targets[i], NULL);
	}

	rctx->streamout.enabled_mask = enabled_mask;

	rctx->streamout.num_targets = num_targets;
	rctx->streamout.append_bitmask = append_bitmask;

	if (num_targets) {
		si_streamout_buffers_dirty(rctx);
	} else {
		rctx->set_atom_dirty(rctx, &rctx->streamout.begin_atom, false);
		r600_set_streamout_enable(rctx, false);
	}
}

static void r600_flush_vgt_streamout(struct r600_common_context *rctx)
{
	struct radeon_winsys_cs *cs = rctx->gfx.cs;
	unsigned reg_strmout_cntl;

	/* The register is at different places on different ASICs. */
	if (rctx->chip_class >= CIK) {
		reg_strmout_cntl = R_0300FC_CP_STRMOUT_CNTL;
		radeon_set_uconfig_reg(cs, reg_strmout_cntl, 0);
	} else {
		reg_strmout_cntl = R_0084FC_CP_STRMOUT_CNTL;
		radeon_set_config_reg(cs, reg_strmout_cntl, 0);
	}

	radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
	radeon_emit(cs, EVENT_TYPE(EVENT_TYPE_SO_VGTSTREAMOUT_FLUSH) | EVENT_INDEX(0));

	radeon_emit(cs, PKT3(PKT3_WAIT_REG_MEM, 5, 0));
	radeon_emit(cs, WAIT_REG_MEM_EQUAL); /* wait until the register is equal to the reference value */
	radeon_emit(cs, reg_strmout_cntl >> 2);  /* register */
	radeon_emit(cs, 0);
	radeon_emit(cs, S_008490_OFFSET_UPDATE_DONE(1)); /* reference value */
	radeon_emit(cs, S_008490_OFFSET_UPDATE_DONE(1)); /* mask */
	radeon_emit(cs, 4); /* poll interval */
}

static void r600_emit_streamout_begin(struct r600_common_context *rctx, struct r600_atom *atom)
{
	struct radeon_winsys_cs *cs = rctx->gfx.cs;
	struct r600_so_target **t = rctx->streamout.targets;
	uint16_t *stride_in_dw = rctx->streamout.stride_in_dw;
	unsigned i;

	r600_flush_vgt_streamout(rctx);

	for (i = 0; i < rctx->streamout.num_targets; i++) {
		if (!t[i])
			continue;

		t[i]->stride_in_dw = stride_in_dw[i];

		/* SI binds streamout buffers as shader resources.
		 * VGT only counts primitives and tells the shader
		 * through SGPRs what to do. */
		radeon_set_context_reg_seq(cs, R_028AD0_VGT_STRMOUT_BUFFER_SIZE_0 + 16*i, 2);
		radeon_emit(cs, (t[i]->b.buffer_offset +
				 t[i]->b.buffer_size) >> 2);	/* BUFFER_SIZE (in DW) */
		radeon_emit(cs, stride_in_dw[i]);		/* VTX_STRIDE (in DW) */

		if (rctx->streamout.append_bitmask & (1 << i) && t[i]->buf_filled_size_valid) {
			uint64_t va = t[i]->buf_filled_size->gpu_address +
				      t[i]->buf_filled_size_offset;

			/* Append. */
			radeon_emit(cs, PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
			radeon_emit(cs, STRMOUT_SELECT_BUFFER(i) |
				    STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_FROM_MEM)); /* control */
			radeon_emit(cs, 0); /* unused */
			radeon_emit(cs, 0); /* unused */
			radeon_emit(cs, va); /* src address lo */
			radeon_emit(cs, va >> 32); /* src address hi */

			r600_emit_reloc(rctx,  &rctx->gfx, t[i]->buf_filled_size,
					RADEON_USAGE_READ, RADEON_PRIO_SO_FILLED_SIZE);
		} else {
			/* Start from the beginning. */
			radeon_emit(cs, PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
			radeon_emit(cs, STRMOUT_SELECT_BUFFER(i) |
				    STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_FROM_PACKET)); /* control */
			radeon_emit(cs, 0); /* unused */
			radeon_emit(cs, 0); /* unused */
			radeon_emit(cs, t[i]->b.buffer_offset >> 2); /* buffer offset in DW */
			radeon_emit(cs, 0); /* unused */
		}
	}

	rctx->streamout.begin_emitted = true;
}

void si_emit_streamout_end(struct r600_common_context *rctx)
{
	struct radeon_winsys_cs *cs = rctx->gfx.cs;
	struct r600_so_target **t = rctx->streamout.targets;
	unsigned i;
	uint64_t va;

	r600_flush_vgt_streamout(rctx);

	for (i = 0; i < rctx->streamout.num_targets; i++) {
		if (!t[i])
			continue;

		va = t[i]->buf_filled_size->gpu_address + t[i]->buf_filled_size_offset;
		radeon_emit(cs, PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
		radeon_emit(cs, STRMOUT_SELECT_BUFFER(i) |
			    STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_NONE) |
			    STRMOUT_STORE_BUFFER_FILLED_SIZE); /* control */
		radeon_emit(cs, va);     /* dst address lo */
		radeon_emit(cs, va >> 32); /* dst address hi */
		radeon_emit(cs, 0); /* unused */
		radeon_emit(cs, 0); /* unused */

		r600_emit_reloc(rctx,  &rctx->gfx, t[i]->buf_filled_size,
				RADEON_USAGE_WRITE, RADEON_PRIO_SO_FILLED_SIZE);

		/* Zero the buffer size. The counters (primitives generated,
		 * primitives emitted) may be enabled even if there is not
		 * buffer bound. This ensures that the primitives-emitted query
		 * won't increment. */
		radeon_set_context_reg(cs, R_028AD0_VGT_STRMOUT_BUFFER_SIZE_0 + 16*i, 0);

		t[i]->buf_filled_size_valid = true;
	}

	rctx->streamout.begin_emitted = false;
	rctx->flags |= R600_CONTEXT_STREAMOUT_FLUSH;
}

/* STREAMOUT CONFIG DERIVED STATE
 *
 * Streamout must be enabled for the PRIMITIVES_GENERATED query to work.
 * The buffer mask is an independent state, so no writes occur if there
 * are no buffers bound.
 */

static void r600_emit_streamout_enable(struct r600_common_context *rctx,
				       struct r600_atom *atom)
{
	radeon_set_context_reg_seq(rctx->gfx.cs, R_028B94_VGT_STRMOUT_CONFIG, 2);
	radeon_emit(rctx->gfx.cs,
		    S_028B94_STREAMOUT_0_EN(r600_get_strmout_en(rctx)) |
		    S_028B94_RAST_STREAM(0) |
		    S_028B94_STREAMOUT_1_EN(r600_get_strmout_en(rctx)) |
		    S_028B94_STREAMOUT_2_EN(r600_get_strmout_en(rctx)) |
		    S_028B94_STREAMOUT_3_EN(r600_get_strmout_en(rctx)));
	radeon_emit(rctx->gfx.cs,
		    rctx->streamout.hw_enabled_mask &
		    rctx->streamout.enabled_stream_buffers_mask);
}

static void r600_set_streamout_enable(struct r600_common_context *rctx, bool enable)
{
	bool old_strmout_en = r600_get_strmout_en(rctx);
	unsigned old_hw_enabled_mask = rctx->streamout.hw_enabled_mask;

	rctx->streamout.streamout_enabled = enable;

	rctx->streamout.hw_enabled_mask = rctx->streamout.enabled_mask |
					  (rctx->streamout.enabled_mask << 4) |
					  (rctx->streamout.enabled_mask << 8) |
					  (rctx->streamout.enabled_mask << 12);

	if ((old_strmout_en != r600_get_strmout_en(rctx)) ||
            (old_hw_enabled_mask != rctx->streamout.hw_enabled_mask)) {
		rctx->set_atom_dirty(rctx, &rctx->streamout.enable_atom, true);
	}
}

void si_update_prims_generated_query_state(struct r600_common_context *rctx,
					   unsigned type, int diff)
{
	if (type == PIPE_QUERY_PRIMITIVES_GENERATED) {
		bool old_strmout_en = r600_get_strmout_en(rctx);

		rctx->streamout.num_prims_gen_queries += diff;
		assert(rctx->streamout.num_prims_gen_queries >= 0);

		rctx->streamout.prims_gen_query_enabled =
			rctx->streamout.num_prims_gen_queries != 0;

		if (old_strmout_en != r600_get_strmout_en(rctx)) {
			rctx->set_atom_dirty(rctx, &rctx->streamout.enable_atom, true);
		}
	}
}

void si_streamout_init(struct r600_common_context *rctx)
{
	rctx->b.create_stream_output_target = r600_create_so_target;
	rctx->b.stream_output_target_destroy = r600_so_target_destroy;
	rctx->streamout.begin_atom.emit = r600_emit_streamout_begin;
	rctx->streamout.enable_atom.emit = r600_emit_streamout_enable;
}
