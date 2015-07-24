/*
 * Copyright 2013 Advanced Micro Devices, Inc.
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
 *      Marek Olšák <marek.olsak@amd.com>
 */

/* Resource binding slots and sampler states (each described with 8 or
 * 4 dwords) are stored in lists in memory which is accessed by shaders
 * using scalar load instructions.
 *
 * This file is responsible for managing such lists. It keeps a copy of all
 * descriptors in CPU memory and re-uploads a whole list if some slots have
 * been changed.
 *
 * This code is also reponsible for updating shader pointers to those lists.
 *
 * Note that CP DMA can't be used for updating the lists, because a GPU hang
 * could leave the list in a mid-IB state and the next IB would get wrong
 * descriptors and the whole context would be unusable at that point.
 * (Note: The register shadowing can't be used due to the same reason)
 *
 * Also, uploading descriptors to newly allocated memory doesn't require
 * a KCACHE flush.
 */

#include "radeon/r600_cs.h"
#include "si_pipe.h"
#include "si_shader.h"
#include "sid.h"

#include "util/u_memory.h"
#include "util/u_upload_mgr.h"


/* NULL image and buffer descriptor.
 *
 * For images, all fields must be zero except for the swizzle, which
 * supports arbitrary combinations of 0s and 1s. The texture type must be
 * any valid type (e.g. 1D). If the texture type isn't set, the hw hangs.
 *
 * For buffers, all fields must be zero. If they are not, the hw hangs.
 *
 * This is the only reason why the buffer descriptor must be in words [4:7].
 */
static uint32_t null_descriptor[8] = {
	0,
	0,
	0,
	S_008F1C_DST_SEL_W(V_008F1C_SQ_SEL_1) |
	S_008F1C_TYPE(V_008F1C_SQ_RSRC_IMG_1D)
	/* the rest must contain zeros, which is also used by the buffer
	 * descriptor */
};

/* Set this if you want the 3D engine to wait until CP DMA is done.
 * It should be set on the last CP DMA packet. */
#define R600_CP_DMA_SYNC	(1 << 0) /* R600+ */

/* Set this if the source data was used as a destination in a previous CP DMA
 * packet. It's for preventing a read-after-write (RAW) hazard between two
 * CP DMA packets. */
#define SI_CP_DMA_RAW_WAIT	(1 << 1) /* SI+ */
#define CIK_CP_DMA_USE_L2	(1 << 2)

/* Emit a CP DMA packet to do a copy from one buffer to another.
 * The size must fit in bits [20:0].
 */
static void si_emit_cp_dma_copy_buffer(struct si_context *sctx,
				       uint64_t dst_va, uint64_t src_va,
				       unsigned size, unsigned flags)
{
	struct radeon_winsys_cs *cs = sctx->b.rings.gfx.cs;
	uint32_t sync_flag = flags & R600_CP_DMA_SYNC ? PKT3_CP_DMA_CP_SYNC : 0;
	uint32_t raw_wait = flags & SI_CP_DMA_RAW_WAIT ? PKT3_CP_DMA_CMD_RAW_WAIT : 0;
	uint32_t sel = flags & CIK_CP_DMA_USE_L2 ?
			   PKT3_CP_DMA_SRC_SEL(3) | PKT3_CP_DMA_DST_SEL(3) : 0;

	assert(size);
	assert((size & ((1<<21)-1)) == size);

	if (sctx->b.chip_class >= CIK) {
		radeon_emit(cs, PKT3(PKT3_DMA_DATA, 5, 0));
		radeon_emit(cs, sync_flag | sel);	/* CP_SYNC [31] */
		radeon_emit(cs, src_va);		/* SRC_ADDR_LO [31:0] */
		radeon_emit(cs, src_va >> 32);		/* SRC_ADDR_HI [31:0] */
		radeon_emit(cs, dst_va);		/* DST_ADDR_LO [31:0] */
		radeon_emit(cs, dst_va >> 32);		/* DST_ADDR_HI [31:0] */
		radeon_emit(cs, size | raw_wait);	/* COMMAND [29:22] | BYTE_COUNT [20:0] */
	} else {
		radeon_emit(cs, PKT3(PKT3_CP_DMA, 4, 0));
		radeon_emit(cs, src_va);			/* SRC_ADDR_LO [31:0] */
		radeon_emit(cs, sync_flag | ((src_va >> 32) & 0xffff)); /* CP_SYNC [31] | SRC_ADDR_HI [15:0] */
		radeon_emit(cs, dst_va);			/* DST_ADDR_LO [31:0] */
		radeon_emit(cs, (dst_va >> 32) & 0xffff);	/* DST_ADDR_HI [15:0] */
		radeon_emit(cs, size | raw_wait);		/* COMMAND [29:22] | BYTE_COUNT [20:0] */
	}
}

/* Emit a CP DMA packet to clear a buffer. The size must fit in bits [20:0]. */
static void si_emit_cp_dma_clear_buffer(struct si_context *sctx,
					uint64_t dst_va, unsigned size,
					uint32_t clear_value, unsigned flags)
{
	struct radeon_winsys_cs *cs = sctx->b.rings.gfx.cs;
	uint32_t sync_flag = flags & R600_CP_DMA_SYNC ? PKT3_CP_DMA_CP_SYNC : 0;
	uint32_t raw_wait = flags & SI_CP_DMA_RAW_WAIT ? PKT3_CP_DMA_CMD_RAW_WAIT : 0;
	uint32_t dst_sel = flags & CIK_CP_DMA_USE_L2 ? PKT3_CP_DMA_DST_SEL(3) : 0;

	assert(size);
	assert((size & ((1<<21)-1)) == size);

	if (sctx->b.chip_class >= CIK) {
		radeon_emit(cs, PKT3(PKT3_DMA_DATA, 5, 0));
		radeon_emit(cs, sync_flag | dst_sel | PKT3_CP_DMA_SRC_SEL(2)); /* CP_SYNC [31] | SRC_SEL[30:29] */
		radeon_emit(cs, clear_value);		/* DATA [31:0] */
		radeon_emit(cs, 0);
		radeon_emit(cs, dst_va);		/* DST_ADDR_LO [31:0] */
		radeon_emit(cs, dst_va >> 32);		/* DST_ADDR_HI [15:0] */
		radeon_emit(cs, size | raw_wait);	/* COMMAND [29:22] | BYTE_COUNT [20:0] */
	} else {
		radeon_emit(cs, PKT3(PKT3_CP_DMA, 4, 0));
		radeon_emit(cs, clear_value);		/* DATA [31:0] */
		radeon_emit(cs, sync_flag | PKT3_CP_DMA_SRC_SEL(2)); /* CP_SYNC [31] | SRC_SEL[30:29] */
		radeon_emit(cs, dst_va);			/* DST_ADDR_LO [31:0] */
		radeon_emit(cs, (dst_va >> 32) & 0xffff);	/* DST_ADDR_HI [15:0] */
		radeon_emit(cs, size | raw_wait);		/* COMMAND [29:22] | BYTE_COUNT [20:0] */
	}
}

static void si_init_descriptors(struct si_descriptors *desc,
				unsigned shader_userdata_index,
				unsigned element_dw_size,
				unsigned num_elements)
{
	int i;

	assert(num_elements <= sizeof(desc->enabled_mask)*8);

	desc->list = CALLOC(num_elements, element_dw_size * 4);
	desc->element_dw_size = element_dw_size;
	desc->num_elements = num_elements;
	desc->list_dirty = true; /* upload the list before the next draw */
	desc->shader_userdata_offset = shader_userdata_index * 4;

	/* Initialize the array to NULL descriptors if the element size is 8. */
	if (element_dw_size == 8)
		for (i = 0; i < num_elements; i++)
			memcpy(desc->list + i*element_dw_size, null_descriptor,
			       sizeof(null_descriptor));
}

static void si_release_descriptors(struct si_descriptors *desc)
{
	pipe_resource_reference((struct pipe_resource**)&desc->buffer, NULL);
	FREE(desc->list);
}

static bool si_upload_descriptors(struct si_context *sctx,
				  struct si_descriptors *desc)
{
	unsigned list_size = desc->num_elements * desc->element_dw_size * 4;
	void *ptr;

	if (!desc->list_dirty)
		return true;

	u_upload_alloc(sctx->b.uploader, 0, list_size,
		       &desc->buffer_offset,
		       (struct pipe_resource**)&desc->buffer, &ptr);
	if (!desc->buffer)
		return false; /* skip the draw call */

	util_memcpy_cpu_to_le32(ptr, desc->list, list_size);

	r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx, desc->buffer,
			      RADEON_USAGE_READ, RADEON_PRIO_SHADER_DATA);

	desc->list_dirty = false;
	desc->pointer_dirty = true;
	sctx->shader_userdata.atom.dirty = true;
	return true;
}

/* SAMPLER VIEWS */

static void si_release_sampler_views(struct si_sampler_views *views)
{
	int i;

	for (i = 0; i < Elements(views->views); i++) {
		pipe_sampler_view_reference(&views->views[i], NULL);
	}
	si_release_descriptors(&views->desc);
}

static enum radeon_bo_priority si_get_resource_ro_priority(struct r600_resource *res)
{
	if (res->b.b.target == PIPE_BUFFER)
		return RADEON_PRIO_SHADER_BUFFER_RO;

	if (res->b.b.nr_samples > 1)
		return RADEON_PRIO_SHADER_TEXTURE_MSAA;

	return RADEON_PRIO_SHADER_TEXTURE_RO;
}

static void si_sampler_views_begin_new_cs(struct si_context *sctx,
					  struct si_sampler_views *views)
{
	uint64_t mask = views->desc.enabled_mask;

	/* Add relocations to the CS. */
	while (mask) {
		int i = u_bit_scan64(&mask);
		struct si_sampler_view *rview =
			(struct si_sampler_view*)views->views[i];

		if (!rview->resource)
			continue;

		r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
				      rview->resource, RADEON_USAGE_READ,
				      si_get_resource_ro_priority(rview->resource));
	}

	if (!views->desc.buffer)
		return;
	r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx, views->desc.buffer,
			      RADEON_USAGE_READWRITE, RADEON_PRIO_SHADER_DATA);
}

static void si_set_sampler_view(struct si_context *sctx, unsigned shader,
				unsigned slot, struct pipe_sampler_view *view,
				unsigned *view_desc)
{
	struct si_sampler_views *views = &sctx->samplers[shader].views;

	if (views->views[slot] == view)
		return;

	if (view) {
		struct si_sampler_view *rview =
			(struct si_sampler_view*)view;

		if (rview->resource)
			r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
				rview->resource, RADEON_USAGE_READ,
				si_get_resource_ro_priority(rview->resource));

		pipe_sampler_view_reference(&views->views[slot], view);
		memcpy(views->desc.list + slot*8, view_desc, 8*4);
		views->desc.enabled_mask |= 1llu << slot;
	} else {
		pipe_sampler_view_reference(&views->views[slot], NULL);
		memcpy(views->desc.list + slot*8, null_descriptor, 8*4);
		views->desc.enabled_mask &= ~(1llu << slot);
	}

	views->desc.list_dirty = true;
}

static void si_set_sampler_views(struct pipe_context *ctx,
				 unsigned shader, unsigned start,
                                 unsigned count,
				 struct pipe_sampler_view **views)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_textures_info *samplers = &sctx->samplers[shader];
	struct si_sampler_view **rviews = (struct si_sampler_view **)views;
	int i;

	if (!count || shader >= SI_NUM_SHADERS)
		return;

	for (i = 0; i < count; i++) {
		unsigned slot = start + i;

		if (!views || !views[i]) {
			samplers->depth_texture_mask &= ~(1 << slot);
			samplers->compressed_colortex_mask &= ~(1 << slot);
			si_set_sampler_view(sctx, shader, slot, NULL, NULL);
			si_set_sampler_view(sctx, shader, SI_FMASK_TEX_OFFSET + slot,
					    NULL, NULL);
			continue;
		}

		si_set_sampler_view(sctx, shader, slot, views[i], rviews[i]->state);

		if (views[i]->texture && views[i]->texture->target != PIPE_BUFFER) {
			struct r600_texture *rtex =
				(struct r600_texture*)views[i]->texture;

			if (rtex->is_depth && !rtex->is_flushing_texture) {
				samplers->depth_texture_mask |= 1 << slot;
			} else {
				samplers->depth_texture_mask &= ~(1 << slot);
			}
			if (rtex->cmask.size || rtex->fmask.size) {
				samplers->compressed_colortex_mask |= 1 << slot;
			} else {
				samplers->compressed_colortex_mask &= ~(1 << slot);
			}

			if (rtex->fmask.size) {
				si_set_sampler_view(sctx, shader, SI_FMASK_TEX_OFFSET + slot,
						    views[i], rviews[i]->fmask_state);
			} else {
				si_set_sampler_view(sctx, shader, SI_FMASK_TEX_OFFSET + slot,
						    NULL, NULL);
			}
		} else {
			samplers->depth_texture_mask &= ~(1 << slot);
			samplers->compressed_colortex_mask &= ~(1 << slot);
			si_set_sampler_view(sctx, shader, SI_FMASK_TEX_OFFSET + slot,
					    NULL, NULL);
		}
	}
}

/* SAMPLER STATES */

static void si_sampler_states_begin_new_cs(struct si_context *sctx,
					   struct si_sampler_states *states)
{
	if (!states->desc.buffer)
		return;
	r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx, states->desc.buffer,
			      RADEON_USAGE_READWRITE, RADEON_PRIO_SHADER_DATA);
}

void si_set_sampler_descriptors(struct si_context *sctx, unsigned shader,
				unsigned start, unsigned count, void **states)
{
	struct si_sampler_states *samplers = &sctx->samplers[shader].states;
	struct si_sampler_state **sstates = (struct si_sampler_state**)states;
	int i;

	if (start == 0)
		samplers->saved_states[0] = states[0];
	if (start == 1)
		samplers->saved_states[1] = states[0];
	else if (start == 0 && count >= 2)
		samplers->saved_states[1] = states[1];

	for (i = 0; i < count; i++) {
		unsigned slot = start + i;

		if (!sstates[i])
			continue;

		memcpy(samplers->desc.list + slot*4, sstates[i]->val, 4*4);
		samplers->desc.list_dirty = true;
	}
}

/* BUFFER RESOURCES */

static void si_init_buffer_resources(struct si_buffer_resources *buffers,
				     unsigned num_buffers,
				     unsigned shader_userdata_index,
				     enum radeon_bo_usage shader_usage,
				     enum radeon_bo_priority priority)
{
	buffers->shader_usage = shader_usage;
	buffers->priority = priority;
	buffers->buffers = CALLOC(num_buffers, sizeof(struct pipe_resource*));

	si_init_descriptors(&buffers->desc, shader_userdata_index, 4,
			    num_buffers);
}

static void si_release_buffer_resources(struct si_buffer_resources *buffers)
{
	int i;

	for (i = 0; i < buffers->desc.num_elements; i++) {
		pipe_resource_reference(&buffers->buffers[i], NULL);
	}

	FREE(buffers->buffers);
	si_release_descriptors(&buffers->desc);
}

static void si_buffer_resources_begin_new_cs(struct si_context *sctx,
					     struct si_buffer_resources *buffers)
{
	uint64_t mask = buffers->desc.enabled_mask;

	/* Add relocations to the CS. */
	while (mask) {
		int i = u_bit_scan64(&mask);

		r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
				      (struct r600_resource*)buffers->buffers[i],
				      buffers->shader_usage, buffers->priority);
	}

	if (!buffers->desc.buffer)
		return;
	r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
			      buffers->desc.buffer, RADEON_USAGE_READWRITE,
			      RADEON_PRIO_SHADER_DATA);
}

/* VERTEX BUFFERS */

static void si_vertex_buffers_begin_new_cs(struct si_context *sctx)
{
	struct si_descriptors *desc = &sctx->vertex_buffers;
	int count = sctx->vertex_elements ? sctx->vertex_elements->count : 0;
	int i;

	for (i = 0; i < count; i++) {
		int vb = sctx->vertex_elements->elements[i].vertex_buffer_index;

		if (vb >= Elements(sctx->vertex_buffer))
			continue;
		if (!sctx->vertex_buffer[vb].buffer)
			continue;

		r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
				      (struct r600_resource*)sctx->vertex_buffer[vb].buffer,
				      RADEON_USAGE_READ, RADEON_PRIO_SHADER_BUFFER_RO);
	}

	if (!desc->buffer)
		return;
	r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
			      desc->buffer, RADEON_USAGE_READ,
			      RADEON_PRIO_SHADER_DATA);
}

static bool si_upload_vertex_buffer_descriptors(struct si_context *sctx)
{
	struct si_descriptors *desc = &sctx->vertex_buffers;
	bool bound[SI_NUM_VERTEX_BUFFERS] = {};
	unsigned i, count = sctx->vertex_elements->count;
	uint64_t va;
	uint32_t *ptr;

	if (!sctx->vertex_buffers_dirty)
		return true;
	if (!count || !sctx->vertex_elements)
		return true;

	/* Vertex buffer descriptors are the only ones which are uploaded
	 * directly through a staging buffer and don't go through
	 * the fine-grained upload path.
	 */
	u_upload_alloc(sctx->b.uploader, 0, count * 16, &desc->buffer_offset,
		       (struct pipe_resource**)&desc->buffer, (void**)&ptr);
	if (!desc->buffer)
		return false;

	r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
			      desc->buffer, RADEON_USAGE_READ,
			      RADEON_PRIO_SHADER_DATA);

	assert(count <= SI_NUM_VERTEX_BUFFERS);

	for (i = 0; i < count; i++) {
		struct pipe_vertex_element *ve = &sctx->vertex_elements->elements[i];
		struct pipe_vertex_buffer *vb;
		struct r600_resource *rbuffer;
		unsigned offset;
		uint32_t *desc = &ptr[i*4];

		if (ve->vertex_buffer_index >= Elements(sctx->vertex_buffer)) {
			memset(desc, 0, 16);
			continue;
		}

		vb = &sctx->vertex_buffer[ve->vertex_buffer_index];
		rbuffer = (struct r600_resource*)vb->buffer;
		if (rbuffer == NULL) {
			memset(desc, 0, 16);
			continue;
		}

		offset = vb->buffer_offset + ve->src_offset;
		va = rbuffer->gpu_address + offset;

		/* Fill in T# buffer resource description */
		desc[0] = va & 0xFFFFFFFF;
		desc[1] = S_008F04_BASE_ADDRESS_HI(va >> 32) |
			  S_008F04_STRIDE(vb->stride);
		if (vb->stride)
			/* Round up by rounding down and adding 1 */
			desc[2] = (vb->buffer->width0 - offset -
				   sctx->vertex_elements->format_size[i]) /
				  vb->stride + 1;
		else
			desc[2] = vb->buffer->width0 - offset;

		desc[3] = sctx->vertex_elements->rsrc_word3[i];

		if (!bound[ve->vertex_buffer_index]) {
			r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
					      (struct r600_resource*)vb->buffer,
					      RADEON_USAGE_READ, RADEON_PRIO_SHADER_BUFFER_RO);
			bound[ve->vertex_buffer_index] = true;
		}
	}

	/* Don't flush the const cache. It would have a very negative effect
	 * on performance (confirmed by testing). New descriptors are always
	 * uploaded to a fresh new buffer, so I don't think flushing the const
	 * cache is needed. */
	desc->pointer_dirty = true;
	sctx->shader_userdata.atom.dirty = true;
	sctx->vertex_buffers_dirty = false;
	return true;
}


/* CONSTANT BUFFERS */

void si_upload_const_buffer(struct si_context *sctx, struct r600_resource **rbuffer,
			    const uint8_t *ptr, unsigned size, uint32_t *const_offset)
{
	void *tmp;

	u_upload_alloc(sctx->b.uploader, 0, size, const_offset,
		       (struct pipe_resource**)rbuffer, &tmp);
	util_memcpy_cpu_to_le32(tmp, ptr, size);
}

static void si_set_constant_buffer(struct pipe_context *ctx, uint shader, uint slot,
				   struct pipe_constant_buffer *input)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_buffer_resources *buffers = &sctx->const_buffers[shader];

	if (shader >= SI_NUM_SHADERS)
		return;

	assert(slot < buffers->desc.num_elements);
	pipe_resource_reference(&buffers->buffers[slot], NULL);

	/* CIK cannot unbind a constant buffer (S_BUFFER_LOAD is buggy
	 * with a NULL buffer). We need to use a dummy buffer instead. */
	if (sctx->b.chip_class == CIK &&
	    (!input || (!input->buffer && !input->user_buffer)))
		input = &sctx->null_const_buf;

	if (input && (input->buffer || input->user_buffer)) {
		struct pipe_resource *buffer = NULL;
		uint64_t va;

		/* Upload the user buffer if needed. */
		if (input->user_buffer) {
			unsigned buffer_offset;

			si_upload_const_buffer(sctx,
					       (struct r600_resource**)&buffer, input->user_buffer,
					       input->buffer_size, &buffer_offset);
			va = r600_resource(buffer)->gpu_address + buffer_offset;
		} else {
			pipe_resource_reference(&buffer, input->buffer);
			va = r600_resource(buffer)->gpu_address + input->buffer_offset;
		}

		/* Set the descriptor. */
		uint32_t *desc = buffers->desc.list + slot*4;
		desc[0] = va;
		desc[1] = S_008F04_BASE_ADDRESS_HI(va >> 32) |
			  S_008F04_STRIDE(0);
		desc[2] = input->buffer_size;
		desc[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
			  S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
			  S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
			  S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
			  S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
			  S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32);

		buffers->buffers[slot] = buffer;
		r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
				      (struct r600_resource*)buffer,
				      buffers->shader_usage, buffers->priority);
		buffers->desc.enabled_mask |= 1llu << slot;
	} else {
		/* Clear the descriptor. */
		memset(buffers->desc.list + slot*4, 0, sizeof(uint32_t) * 4);
		buffers->desc.enabled_mask &= ~(1llu << slot);
	}

	buffers->desc.list_dirty = true;
}

/* RING BUFFERS */

void si_set_ring_buffer(struct pipe_context *ctx, uint shader, uint slot,
			struct pipe_resource *buffer,
			unsigned stride, unsigned num_records,
			bool add_tid, bool swizzle,
			unsigned element_size, unsigned index_stride, uint64_t offset)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_buffer_resources *buffers = &sctx->rw_buffers[shader];

	if (shader >= SI_NUM_SHADERS)
		return;

	/* The stride field in the resource descriptor has 14 bits */
	assert(stride < (1 << 14));

	assert(slot < buffers->desc.num_elements);
	pipe_resource_reference(&buffers->buffers[slot], NULL);

	if (buffer) {
		uint64_t va;

		va = r600_resource(buffer)->gpu_address + offset;

		switch (element_size) {
		default:
			assert(!"Unsupported ring buffer element size");
		case 0:
		case 2:
			element_size = 0;
			break;
		case 4:
			element_size = 1;
			break;
		case 8:
			element_size = 2;
			break;
		case 16:
			element_size = 3;
			break;
		}

		switch (index_stride) {
		default:
			assert(!"Unsupported ring buffer index stride");
		case 0:
		case 8:
			index_stride = 0;
			break;
		case 16:
			index_stride = 1;
			break;
		case 32:
			index_stride = 2;
			break;
		case 64:
			index_stride = 3;
			break;
		}

		/* Set the descriptor. */
		uint32_t *desc = buffers->desc.list + slot*4;
		desc[0] = va;
		desc[1] = S_008F04_BASE_ADDRESS_HI(va >> 32) |
			  S_008F04_STRIDE(stride) |
			  S_008F04_SWIZZLE_ENABLE(swizzle);
		desc[2] = num_records;
		desc[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
			  S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
			  S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
			  S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
			  S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
			  S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32) |
			  S_008F0C_ELEMENT_SIZE(element_size) |
			  S_008F0C_INDEX_STRIDE(index_stride) |
			  S_008F0C_ADD_TID_ENABLE(add_tid);

		pipe_resource_reference(&buffers->buffers[slot], buffer);
		r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
				      (struct r600_resource*)buffer,
				      buffers->shader_usage, buffers->priority);
		buffers->desc.enabled_mask |= 1llu << slot;
	} else {
		/* Clear the descriptor. */
		memset(buffers->desc.list + slot*4, 0, sizeof(uint32_t) * 4);
		buffers->desc.enabled_mask &= ~(1llu << slot);
	}

	buffers->desc.list_dirty = true;
}

/* STREAMOUT BUFFERS */

static void si_set_streamout_targets(struct pipe_context *ctx,
				     unsigned num_targets,
				     struct pipe_stream_output_target **targets,
				     const unsigned *offsets)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_buffer_resources *buffers = &sctx->rw_buffers[PIPE_SHADER_VERTEX];
	unsigned old_num_targets = sctx->b.streamout.num_targets;
	unsigned i, bufidx;

	/* We are going to unbind the buffers. Mark which caches need to be flushed. */
	if (sctx->b.streamout.num_targets && sctx->b.streamout.begin_emitted) {
		/* Since streamout uses vector writes which go through TC L2
		 * and most other clients can use TC L2 as well, we don't need
		 * to flush it.
		 *
		 * The only case which requires flushing it is VGT DMA index
		 * fetching, which is a rare case. Thus, flag the TC L2
		 * dirtiness in the resource and handle it when index fetching
		 * is used.
		 */
		for (i = 0; i < sctx->b.streamout.num_targets; i++)
			if (sctx->b.streamout.targets[i])
				r600_resource(sctx->b.streamout.targets[i]->b.buffer)->TC_L2_dirty = true;

		/* Invalidate the scalar cache in case a streamout buffer is
		 * going to be used as a constant buffer.
		 *
		 * Invalidate TC L1, because streamout bypasses it (done by
		 * setting GLC=1 in the store instruction), but it can contain
		 * outdated data of streamout buffers.
		 *
		 * VS_PARTIAL_FLUSH is required if the buffers are going to be
		 * used as an input immediately.
		 */
		sctx->b.flags |= SI_CONTEXT_INV_KCACHE |
				 SI_CONTEXT_INV_TC_L1 |
				 SI_CONTEXT_VS_PARTIAL_FLUSH;
	}

	/* Streamout buffers must be bound in 2 places:
	 * 1) in VGT by setting the VGT_STRMOUT registers
	 * 2) as shader resources
	 */

	/* Set the VGT regs. */
	r600_set_streamout_targets(ctx, num_targets, targets, offsets);

	/* Set the shader resources.*/
	for (i = 0; i < num_targets; i++) {
		bufidx = SI_SO_BUF_OFFSET + i;

		if (targets[i]) {
			struct pipe_resource *buffer = targets[i]->buffer;
			uint64_t va = r600_resource(buffer)->gpu_address;

			/* Set the descriptor. */
			uint32_t *desc = buffers->desc.list + bufidx*4;
			desc[0] = va;
			desc[1] = S_008F04_BASE_ADDRESS_HI(va >> 32);
			desc[2] = 0xffffffff;
			desc[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
				  S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
				  S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
				  S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W);

			/* Set the resource. */
			pipe_resource_reference(&buffers->buffers[bufidx],
						buffer);
			r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
					      (struct r600_resource*)buffer,
					      buffers->shader_usage, buffers->priority);
			buffers->desc.enabled_mask |= 1llu << bufidx;
		} else {
			/* Clear the descriptor and unset the resource. */
			memset(buffers->desc.list + bufidx*4, 0,
			       sizeof(uint32_t) * 4);
			pipe_resource_reference(&buffers->buffers[bufidx],
						NULL);
			buffers->desc.enabled_mask &= ~(1llu << bufidx);
		}
	}
	for (; i < old_num_targets; i++) {
		bufidx = SI_SO_BUF_OFFSET + i;
		/* Clear the descriptor and unset the resource. */
		memset(buffers->desc.list + bufidx*4, 0, sizeof(uint32_t) * 4);
		pipe_resource_reference(&buffers->buffers[bufidx], NULL);
		buffers->desc.enabled_mask &= ~(1llu << bufidx);
	}

	buffers->desc.list_dirty = true;
}

static void si_desc_reset_buffer_offset(struct pipe_context *ctx,
					uint32_t *desc, uint64_t old_buf_va,
					struct pipe_resource *new_buf)
{
	/* Retrieve the buffer offset from the descriptor. */
	uint64_t old_desc_va =
		desc[0] | ((uint64_t)G_008F04_BASE_ADDRESS_HI(desc[1]) << 32);

	assert(old_buf_va <= old_desc_va);
	uint64_t offset_within_buffer = old_desc_va - old_buf_va;

	/* Update the descriptor. */
	uint64_t va = r600_resource(new_buf)->gpu_address + offset_within_buffer;

	desc[0] = va;
	desc[1] = (desc[1] & C_008F04_BASE_ADDRESS_HI) |
		  S_008F04_BASE_ADDRESS_HI(va >> 32);
}

/* BUFFER DISCARD/INVALIDATION */

/* Reallocate a buffer a update all resource bindings where the buffer is
 * bound.
 *
 * This is used to avoid CPU-GPU synchronizations, because it makes the buffer
 * idle by discarding its contents. Apps usually tell us when to do this using
 * map_buffer flags, for example.
 */
static void si_invalidate_buffer(struct pipe_context *ctx, struct pipe_resource *buf)
{
	struct si_context *sctx = (struct si_context*)ctx;
	struct r600_resource *rbuffer = r600_resource(buf);
	unsigned i, shader, alignment = rbuffer->buf->alignment;
	uint64_t old_va = rbuffer->gpu_address;
	unsigned num_elems = sctx->vertex_elements ?
				       sctx->vertex_elements->count : 0;
	struct si_sampler_view *view;

	/* Reallocate the buffer in the same pipe_resource. */
	r600_init_resource(&sctx->screen->b, rbuffer, rbuffer->b.b.width0,
			   alignment, TRUE);

	/* We changed the buffer, now we need to bind it where the old one
	 * was bound. This consists of 2 things:
	 *   1) Updating the resource descriptor and dirtying it.
	 *   2) Adding a relocation to the CS, so that it's usable.
	 */

	/* Vertex buffers. */
	for (i = 0; i < num_elems; i++) {
		int vb = sctx->vertex_elements->elements[i].vertex_buffer_index;

		if (vb >= Elements(sctx->vertex_buffer))
			continue;
		if (!sctx->vertex_buffer[vb].buffer)
			continue;

		if (sctx->vertex_buffer[vb].buffer == buf) {
			sctx->vertex_buffers_dirty = true;
			break;
		}
	}

	/* Read/Write buffers. */
	for (shader = 0; shader < SI_NUM_SHADERS; shader++) {
		struct si_buffer_resources *buffers = &sctx->rw_buffers[shader];
		uint64_t mask = buffers->desc.enabled_mask;

		while (mask) {
			i = u_bit_scan64(&mask);
			if (buffers->buffers[i] == buf) {
				si_desc_reset_buffer_offset(ctx, buffers->desc.list + i*4,
							    old_va, buf);
				buffers->desc.list_dirty = true;

				r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
						      rbuffer, buffers->shader_usage,
						      buffers->priority);

				if (i >= SI_SO_BUF_OFFSET && shader == PIPE_SHADER_VERTEX) {
					/* Update the streamout state. */
					if (sctx->b.streamout.begin_emitted) {
						r600_emit_streamout_end(&sctx->b);
					}
					sctx->b.streamout.append_bitmask =
						sctx->b.streamout.enabled_mask;
					r600_streamout_buffers_dirty(&sctx->b);
				}
			}
		}
	}

	/* Constant buffers. */
	for (shader = 0; shader < SI_NUM_SHADERS; shader++) {
		struct si_buffer_resources *buffers = &sctx->const_buffers[shader];
		uint64_t mask = buffers->desc.enabled_mask;

		while (mask) {
			unsigned i = u_bit_scan64(&mask);
			if (buffers->buffers[i] == buf) {
				si_desc_reset_buffer_offset(ctx, buffers->desc.list + i*4,
							    old_va, buf);
				buffers->desc.list_dirty = true;

				r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
						      rbuffer, buffers->shader_usage,
						      buffers->priority);
			}
		}
	}

	/* Texture buffers - update virtual addresses in sampler view descriptors. */
	LIST_FOR_EACH_ENTRY(view, &sctx->b.texture_buffers, list) {
		if (view->base.texture == buf) {
			si_desc_reset_buffer_offset(ctx, &view->state[4], old_va, buf);
		}
	}
	/* Texture buffers - update bindings. */
	for (shader = 0; shader < SI_NUM_SHADERS; shader++) {
		struct si_sampler_views *views = &sctx->samplers[shader].views;
		uint64_t mask = views->desc.enabled_mask;

		while (mask) {
			unsigned i = u_bit_scan64(&mask);
			if (views->views[i]->texture == buf) {
				si_desc_reset_buffer_offset(ctx, views->desc.list + i*8+4,
							    old_va, buf);
				views->desc.list_dirty = true;

				r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
						      rbuffer, RADEON_USAGE_READ,
						      RADEON_PRIO_SHADER_BUFFER_RO);
			}
		}
	}
}

/* CP DMA */

/* The max number of bytes to copy per packet. */
#define CP_DMA_MAX_BYTE_COUNT ((1 << 21) - 8)

static void si_clear_buffer(struct pipe_context *ctx, struct pipe_resource *dst,
			    unsigned offset, unsigned size, unsigned value,
			    bool is_framebuffer)
{
	struct si_context *sctx = (struct si_context*)ctx;
	unsigned flush_flags, tc_l2_flag;

	if (!size)
		return;

	/* Mark the buffer range of destination as valid (initialized),
	 * so that transfer_map knows it should wait for the GPU when mapping
	 * that range. */
	util_range_add(&r600_resource(dst)->valid_buffer_range, offset,
		       offset + size);

	/* Fallback for unaligned clears. */
	if (offset % 4 != 0 || size % 4 != 0) {
		uint32_t *map = sctx->b.ws->buffer_map(r600_resource(dst)->cs_buf,
						       sctx->b.rings.gfx.cs,
						       PIPE_TRANSFER_WRITE);
		size /= 4;
		for (unsigned i = 0; i < size; i++)
			*map++ = value;
		return;
	}

	uint64_t va = r600_resource(dst)->gpu_address + offset;

	/* Flush the caches where the resource is bound. */
	if (is_framebuffer) {
		flush_flags = SI_CONTEXT_FLUSH_AND_INV_FRAMEBUFFER;
		tc_l2_flag = 0;
	} else {
		flush_flags = SI_CONTEXT_INV_TC_L1 |
			      (sctx->b.chip_class == SI ? SI_CONTEXT_INV_TC_L2 : 0) |
			      SI_CONTEXT_INV_KCACHE;
		tc_l2_flag = sctx->b.chip_class == SI ? 0 : CIK_CP_DMA_USE_L2;
	}

	sctx->b.flags |= SI_CONTEXT_PS_PARTIAL_FLUSH |
			 flush_flags;

	while (size) {
		unsigned byte_count = MIN2(size, CP_DMA_MAX_BYTE_COUNT);
		unsigned dma_flags = tc_l2_flag;

		si_need_cs_space(sctx, 7 + (sctx->b.flags ? sctx->cache_flush.num_dw : 0),
				 FALSE);

		/* This must be done after need_cs_space. */
		r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx,
				      (struct r600_resource*)dst, RADEON_USAGE_WRITE,
				      RADEON_PRIO_MIN);

		/* Flush the caches for the first copy only.
		 * Also wait for the previous CP DMA operations. */
		if (sctx->b.flags) {
			si_emit_cache_flush(&sctx->b, NULL);
			dma_flags |= SI_CP_DMA_RAW_WAIT; /* same as WAIT_UNTIL=CP_DMA_IDLE */
		}

		/* Do the synchronization after the last copy, so that all data is written to memory. */
		if (size == byte_count)
			dma_flags |= R600_CP_DMA_SYNC;

		/* Emit the clear packet. */
		si_emit_cp_dma_clear_buffer(sctx, va, byte_count, value, dma_flags);

		size -= byte_count;
		va += byte_count;
	}

	/* Flush the caches again in case the 3D engine has been prefetching
	 * the resource. */
	sctx->b.flags |= flush_flags;

	if (tc_l2_flag)
		r600_resource(dst)->TC_L2_dirty = true;
}

void si_copy_buffer(struct si_context *sctx,
		    struct pipe_resource *dst, struct pipe_resource *src,
		    uint64_t dst_offset, uint64_t src_offset, unsigned size,
		    bool is_framebuffer)
{
	unsigned flush_flags, tc_l2_flag;

	if (!size)
		return;

	/* Mark the buffer range of destination as valid (initialized),
	 * so that transfer_map knows it should wait for the GPU when mapping
	 * that range. */
	util_range_add(&r600_resource(dst)->valid_buffer_range, dst_offset,
		       dst_offset + size);

	dst_offset += r600_resource(dst)->gpu_address;
	src_offset += r600_resource(src)->gpu_address;

	/* Flush the caches where the resource is bound. */
	if (is_framebuffer) {
		flush_flags = SI_CONTEXT_FLUSH_AND_INV_FRAMEBUFFER;
		tc_l2_flag = 0;
	} else {
		flush_flags = SI_CONTEXT_INV_TC_L1 |
			      (sctx->b.chip_class == SI ? SI_CONTEXT_INV_TC_L2 : 0) |
			      SI_CONTEXT_INV_KCACHE;
		tc_l2_flag = sctx->b.chip_class == SI ? 0 : CIK_CP_DMA_USE_L2;
	}

	sctx->b.flags |= SI_CONTEXT_PS_PARTIAL_FLUSH |
			 flush_flags;

	while (size) {
		unsigned sync_flags = tc_l2_flag;
		unsigned byte_count = MIN2(size, CP_DMA_MAX_BYTE_COUNT);

		si_need_cs_space(sctx, 7 + (sctx->b.flags ? sctx->cache_flush.num_dw : 0), FALSE);

		/* Flush the caches for the first copy only. Also wait for old CP DMA packets to complete. */
		if (sctx->b.flags) {
			si_emit_cache_flush(&sctx->b, NULL);
			sync_flags |= SI_CP_DMA_RAW_WAIT;
		}

		/* Do the synchronization after the last copy, so that all data is written to memory. */
		if (size == byte_count) {
			sync_flags |= R600_CP_DMA_SYNC;
		}

		/* This must be done after r600_need_cs_space. */
		r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx, (struct r600_resource*)src,
				      RADEON_USAGE_READ, RADEON_PRIO_MIN);
		r600_context_bo_reloc(&sctx->b, &sctx->b.rings.gfx, (struct r600_resource*)dst,
				      RADEON_USAGE_WRITE, RADEON_PRIO_MIN);

		si_emit_cp_dma_copy_buffer(sctx, dst_offset, src_offset, byte_count, sync_flags);

		size -= byte_count;
		src_offset += byte_count;
		dst_offset += byte_count;
	}

	/* Flush the caches again in case the 3D engine has been prefetching
	 * the resource. */
	sctx->b.flags |= flush_flags;

	if (tc_l2_flag)
		r600_resource(dst)->TC_L2_dirty = true;
}

/* SHADER USER DATA */

static void si_mark_shader_pointers_dirty(struct si_context *sctx,
					  unsigned shader)
{
	sctx->const_buffers[shader].desc.pointer_dirty = true;
	sctx->rw_buffers[shader].desc.pointer_dirty = true;
	sctx->samplers[shader].views.desc.pointer_dirty = true;
	sctx->samplers[shader].states.desc.pointer_dirty = true;

	if (shader == PIPE_SHADER_VERTEX)
		sctx->vertex_buffers.pointer_dirty = true;

	sctx->shader_userdata.atom.dirty = true;
}

static void si_shader_userdata_begin_new_cs(struct si_context *sctx)
{
	int i;

	for (i = 0; i < SI_NUM_SHADERS; i++) {
		si_mark_shader_pointers_dirty(sctx, i);
	}
}

/* Set a base register address for user data constants in the given shader.
 * This assigns a mapping from PIPE_SHADER_* to SPI_SHADER_USER_DATA_*.
 */
static void si_set_user_data_base(struct si_context *sctx,
				  unsigned shader, uint32_t new_base)
{
	uint32_t *base = &sctx->shader_userdata.sh_base[shader];

	if (*base != new_base) {
		*base = new_base;

		if (new_base)
			si_mark_shader_pointers_dirty(sctx, shader);
	}
}

/* This must be called when these shaders are changed from non-NULL to NULL
 * and vice versa:
 * - geometry shader
 * - tessellation control shader
 * - tessellation evaluation shader
 */
void si_shader_change_notify(struct si_context *sctx)
{
	/* VS can be bound as VS, ES, or LS. */
	if (sctx->tes_shader)
		si_set_user_data_base(sctx, PIPE_SHADER_VERTEX,
				      R_00B530_SPI_SHADER_USER_DATA_LS_0);
	else if (sctx->gs_shader)
		si_set_user_data_base(sctx, PIPE_SHADER_VERTEX,
				      R_00B330_SPI_SHADER_USER_DATA_ES_0);
	else
		si_set_user_data_base(sctx, PIPE_SHADER_VERTEX,
				      R_00B130_SPI_SHADER_USER_DATA_VS_0);

	/* TES can be bound as ES, VS, or not bound. */
	if (sctx->tes_shader) {
		if (sctx->gs_shader)
			si_set_user_data_base(sctx, PIPE_SHADER_TESS_EVAL,
					      R_00B330_SPI_SHADER_USER_DATA_ES_0);
		else
			si_set_user_data_base(sctx, PIPE_SHADER_TESS_EVAL,
					      R_00B130_SPI_SHADER_USER_DATA_VS_0);
	} else {
		si_set_user_data_base(sctx, PIPE_SHADER_TESS_EVAL, 0);
	}
}

static void si_emit_shader_pointer(struct si_context *sctx,
				   struct si_descriptors *desc,
				   unsigned sh_base, bool keep_dirty)
{
	struct radeon_winsys_cs *cs = sctx->b.rings.gfx.cs;
	uint64_t va;

	if (!desc->pointer_dirty || !desc->buffer)
		return;

	va = desc->buffer->gpu_address +
	     desc->buffer_offset;

	radeon_emit(cs, PKT3(PKT3_SET_SH_REG, 2, 0));
	radeon_emit(cs, (sh_base + desc->shader_userdata_offset - SI_SH_REG_OFFSET) >> 2);
	radeon_emit(cs, va);
	radeon_emit(cs, va >> 32);

	desc->pointer_dirty = keep_dirty;
}

static void si_emit_shader_userdata(struct si_context *sctx,
				    struct r600_atom *atom)
{
	unsigned i;
	uint32_t *sh_base = sctx->shader_userdata.sh_base;

	if (sctx->gs_shader) {
		/* The VS copy shader needs these for clipping, streamout, and rings. */
		unsigned vs_base = R_00B130_SPI_SHADER_USER_DATA_VS_0;
		unsigned i = PIPE_SHADER_VERTEX;

		si_emit_shader_pointer(sctx, &sctx->const_buffers[i].desc, vs_base, true);
		si_emit_shader_pointer(sctx, &sctx->rw_buffers[i].desc, vs_base, true);

		/* The TESSEVAL shader needs this for the ESGS ring buffer. */
		si_emit_shader_pointer(sctx, &sctx->rw_buffers[i].desc,
				       R_00B330_SPI_SHADER_USER_DATA_ES_0, true);
	} else if (sctx->tes_shader) {
		/* The TESSEVAL shader needs this for streamout. */
		si_emit_shader_pointer(sctx, &sctx->rw_buffers[PIPE_SHADER_VERTEX].desc,
				       R_00B130_SPI_SHADER_USER_DATA_VS_0, true);
	}

	for (i = 0; i < SI_NUM_SHADERS; i++) {
		unsigned base = sh_base[i];

		if (!base)
			continue;

		if (i != PIPE_SHADER_TESS_EVAL)
			si_emit_shader_pointer(sctx, &sctx->rw_buffers[i].desc, base, false);

		si_emit_shader_pointer(sctx, &sctx->const_buffers[i].desc, base, false);
		si_emit_shader_pointer(sctx, &sctx->samplers[i].views.desc, base, false);
		si_emit_shader_pointer(sctx, &sctx->samplers[i].states.desc, base, false);
	}
	si_emit_shader_pointer(sctx, &sctx->vertex_buffers, sh_base[PIPE_SHADER_VERTEX], false);
}

/* INIT/DEINIT/UPLOAD */

void si_init_all_descriptors(struct si_context *sctx)
{
	int i;

	for (i = 0; i < SI_NUM_SHADERS; i++) {
		si_init_buffer_resources(&sctx->const_buffers[i],
					 SI_NUM_CONST_BUFFERS, SI_SGPR_CONST,
					 RADEON_USAGE_READ, RADEON_PRIO_SHADER_BUFFER_RO);
		si_init_buffer_resources(&sctx->rw_buffers[i],
					 SI_NUM_RW_BUFFERS, SI_SGPR_RW_BUFFERS,
					 RADEON_USAGE_READWRITE, RADEON_PRIO_SHADER_RESOURCE_RW);

		si_init_descriptors(&sctx->samplers[i].views.desc,
				    SI_SGPR_RESOURCE, 8, SI_NUM_SAMPLER_VIEWS);
		si_init_descriptors(&sctx->samplers[i].states.desc,
				    SI_SGPR_SAMPLER, 4, SI_NUM_SAMPLER_STATES);
	}

	si_init_descriptors(&sctx->vertex_buffers, SI_SGPR_VERTEX_BUFFER,
			    4, SI_NUM_VERTEX_BUFFERS);

	/* Set pipe_context functions. */
	sctx->b.b.set_constant_buffer = si_set_constant_buffer;
	sctx->b.b.set_sampler_views = si_set_sampler_views;
	sctx->b.b.set_stream_output_targets = si_set_streamout_targets;
	sctx->b.clear_buffer = si_clear_buffer;
	sctx->b.invalidate_buffer = si_invalidate_buffer;

	/* Shader user data. */
	sctx->atoms.s.shader_userdata = &sctx->shader_userdata.atom;
	sctx->shader_userdata.atom.emit = (void*)si_emit_shader_userdata;

	/* Upper bound, 4 pointers per shader, +1 for vertex buffers, +2 for the VS copy shader. */
	sctx->shader_userdata.atom.num_dw = (SI_NUM_SHADERS * 4 + 1 + 2) * 4;

	/* Set default and immutable mappings. */
	si_set_user_data_base(sctx, PIPE_SHADER_VERTEX, R_00B130_SPI_SHADER_USER_DATA_VS_0);
	si_set_user_data_base(sctx, PIPE_SHADER_TESS_CTRL, R_00B430_SPI_SHADER_USER_DATA_HS_0);
	si_set_user_data_base(sctx, PIPE_SHADER_GEOMETRY, R_00B230_SPI_SHADER_USER_DATA_GS_0);
	si_set_user_data_base(sctx, PIPE_SHADER_FRAGMENT, R_00B030_SPI_SHADER_USER_DATA_PS_0);
}

bool si_upload_shader_descriptors(struct si_context *sctx)
{
	int i;

	for (i = 0; i < SI_NUM_SHADERS; i++) {
		if (!si_upload_descriptors(sctx, &sctx->const_buffers[i].desc) ||
		    !si_upload_descriptors(sctx, &sctx->rw_buffers[i].desc) ||
		    !si_upload_descriptors(sctx, &sctx->samplers[i].views.desc) ||
		    !si_upload_descriptors(sctx, &sctx->samplers[i].states.desc))
			return false;
	}
	return si_upload_vertex_buffer_descriptors(sctx);
}

void si_release_all_descriptors(struct si_context *sctx)
{
	int i;

	for (i = 0; i < SI_NUM_SHADERS; i++) {
		si_release_buffer_resources(&sctx->const_buffers[i]);
		si_release_buffer_resources(&sctx->rw_buffers[i]);
		si_release_sampler_views(&sctx->samplers[i].views);
		si_release_descriptors(&sctx->samplers[i].states.desc);
	}
	si_release_descriptors(&sctx->vertex_buffers);
}

void si_all_descriptors_begin_new_cs(struct si_context *sctx)
{
	int i;

	for (i = 0; i < SI_NUM_SHADERS; i++) {
		si_buffer_resources_begin_new_cs(sctx, &sctx->const_buffers[i]);
		si_buffer_resources_begin_new_cs(sctx, &sctx->rw_buffers[i]);
		si_sampler_views_begin_new_cs(sctx, &sctx->samplers[i].views);
		si_sampler_states_begin_new_cs(sctx, &sctx->samplers[i].states);
	}
	si_vertex_buffers_begin_new_cs(sctx);
	si_shader_userdata_begin_new_cs(sctx);
}
