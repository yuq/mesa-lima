/**************************************************************************
 *
 * Copyright 2017 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <assert.h>
#include <stdio.h>

#include "pipe/p_video_codec.h"

#include "util/u_memory.h"
#include "util/u_video.h"

#include "vl/vl_mpeg12_decoder.h"

#include "r600_pipe_common.h"
#include "radeon_video.h"
#include "radeon_vcn_dec.h"

#define FB_BUFFER_OFFSET		0x1000
#define FB_BUFFER_SIZE			2048
#define IT_SCALING_TABLE_SIZE		992
#define RDECODE_SESSION_CONTEXT_SIZE	(128 * 1024)

#define RDECODE_GPCOM_VCPU_CMD		0x2070c
#define RDECODE_GPCOM_VCPU_DATA0	0x20710
#define RDECODE_GPCOM_VCPU_DATA1	0x20714
#define RDECODE_ENGINE_CNTL		0x20718

#define NUM_BUFFERS			4
#define NUM_MPEG2_REFS			6
#define NUM_H264_REFS			17
#define NUM_VC1_REFS			5

struct radeon_decoder {
	struct pipe_video_codec		base;

	unsigned			stream_handle;
	unsigned			stream_type;
	unsigned			frame_number;

	struct pipe_screen		*screen;
	struct radeon_winsys		*ws;
	struct radeon_winsys_cs		*cs;

	void				*msg;
	uint32_t			*fb;
	uint8_t				*it;
	void				*bs_ptr;

	struct rvid_buffer		msg_fb_it_buffers[NUM_BUFFERS];
	struct rvid_buffer		bs_buffers[NUM_BUFFERS];
	struct rvid_buffer		dpb;
	struct rvid_buffer		ctx;
	struct rvid_buffer		sessionctx;

	unsigned			bs_size;
	unsigned			cur_buffer;
};

static void radeon_dec_destroy_associated_data(void *data)
{
	/* NOOP, since we only use an intptr */
}

static void rvcn_dec_message_create(struct radeon_decoder *dec)
{
	rvcn_dec_message_header_t *header = dec->msg;
	rvcn_dec_message_create_t *create = dec->msg + sizeof(rvcn_dec_message_header_t);
	unsigned sizes = sizeof(rvcn_dec_message_header_t) + sizeof(rvcn_dec_message_create_t);

	memset(dec->msg, 0, sizes);
	header->header_size = sizeof(rvcn_dec_message_header_t);
	header->total_size = sizes;
	header->num_buffers = 1;
	header->msg_type = RDECODE_MSG_CREATE;
	header->stream_handle = dec->stream_handle;
	header->status_report_feedback_number = 0;

	header->index[0].message_id = RDECODE_MESSAGE_CREATE;
	header->index[0].offset = sizeof(rvcn_dec_message_header_t);
	header->index[0].size = sizeof(rvcn_dec_message_create_t);
	header->index[0].filled = 0;

	create->stream_type = dec->stream_type;
	create->session_flags = 0;
	create->width_in_samples = dec->base.width;
	create->height_in_samples = dec->base.height;
}

static struct pb_buffer *rvcn_dec_message_decode(struct radeon_decoder *dec)
{
	/* TODO */
	return NULL;
}

static void rvcn_dec_message_destroy(struct radeon_decoder *dec)
{
	/* TODO */
}

static void rvcn_dec_message_feedback(struct radeon_decoder *dec)
{
	/* TODO */
}

/* flush IB to the hardware */
static int flush(struct radeon_decoder *dec, unsigned flags)
{
	return dec->ws->cs_flush(dec->cs, flags, NULL);
}

/* add a new set register command to the IB */
static void set_reg(struct radeon_decoder *dec, unsigned reg, uint32_t val)
{
	radeon_emit(dec->cs, RDECODE_PKT0(reg >> 2, 0));
	radeon_emit(dec->cs, val);
}

/* send a command to the VCPU through the GPCOM registers */
static void send_cmd(struct radeon_decoder *dec, unsigned cmd,
		     struct pb_buffer* buf, uint32_t off,
		     enum radeon_bo_usage usage, enum radeon_bo_domain domain)
{
	uint64_t addr;

	dec->ws->cs_add_buffer(dec->cs, buf, usage | RADEON_USAGE_SYNCHRONIZED,
			   domain, RADEON_PRIO_UVD);
	addr = dec->ws->buffer_get_virtual_address(buf);
	addr = addr + off;

	set_reg(dec, RDECODE_GPCOM_VCPU_DATA0, addr);
	set_reg(dec, RDECODE_GPCOM_VCPU_DATA1, addr >> 32);
	set_reg(dec, RDECODE_GPCOM_VCPU_CMD, cmd << 1);
}

/* do the codec needs an IT buffer ?*/
static bool have_it(struct radeon_decoder *dec)
{
	return dec->stream_type == RDECODE_CODEC_H264_PERF ||
		dec->stream_type == RDECODE_CODEC_H265;
}

/* map the next available message/feedback/itscaling buffer */
static void map_msg_fb_it_buf(struct radeon_decoder *dec)
{
	struct rvid_buffer* buf;
	uint8_t *ptr;

	/* grab the current message/feedback buffer */
	buf = &dec->msg_fb_it_buffers[dec->cur_buffer];

	/* and map it for CPU access */
	ptr = dec->ws->buffer_map(buf->res->buf, dec->cs, PIPE_TRANSFER_WRITE);

	/* calc buffer offsets */
	dec->msg = ptr;

	dec->fb = (uint32_t *)(ptr + FB_BUFFER_OFFSET);
	if (have_it(dec))
		dec->it = (uint8_t *)(ptr + FB_BUFFER_OFFSET + FB_BUFFER_SIZE);
}

/* unmap and send a message command to the VCPU */
static void send_msg_buf(struct radeon_decoder *dec)
{
	struct rvid_buffer* buf;

	/* ignore the request if message/feedback buffer isn't mapped */
	if (!dec->msg || !dec->fb)
		return;

	/* grab the current message buffer */
	buf = &dec->msg_fb_it_buffers[dec->cur_buffer];

	/* unmap the buffer */
	dec->ws->buffer_unmap(buf->res->buf);
	dec->msg = NULL;
	dec->fb = NULL;
	dec->it = NULL;

	if (dec->sessionctx.res)
		send_cmd(dec, RDECODE_CMD_SESSION_CONTEXT_BUFFER,
			 dec->sessionctx.res->buf, 0, RADEON_USAGE_READWRITE,
			 RADEON_DOMAIN_VRAM);

	/* and send it to the hardware */
	send_cmd(dec, RDECODE_CMD_MSG_BUFFER, buf->res->buf, 0,
		 RADEON_USAGE_READ, RADEON_DOMAIN_GTT);
}

/* cycle to the next set of buffers */
static void next_buffer(struct radeon_decoder *dec)
{
	++dec->cur_buffer;
	dec->cur_buffer %= NUM_BUFFERS;
}

static unsigned calc_ctx_size_h264_perf(struct radeon_decoder *dec)
{
	unsigned width_in_mb, height_in_mb, ctx_size;
	unsigned width = align(dec->base.width, VL_MACROBLOCK_WIDTH);
	unsigned height = align(dec->base.height, VL_MACROBLOCK_HEIGHT);

	unsigned max_references = dec->base.max_references + 1;

	// picture width & height in 16 pixel units
	width_in_mb = width / VL_MACROBLOCK_WIDTH;
	height_in_mb = align(height / VL_MACROBLOCK_HEIGHT, 2);

	unsigned fs_in_mb = width_in_mb * height_in_mb;
	unsigned num_dpb_buffer;
	switch(dec->base.level) {
	case 30:
		num_dpb_buffer = 8100 / fs_in_mb;
		break;
	case 31:
		num_dpb_buffer = 18000 / fs_in_mb;
		break;
	case 32:
		num_dpb_buffer = 20480 / fs_in_mb;
		break;
	case 41:
		num_dpb_buffer = 32768 / fs_in_mb;
		break;
	case 42:
		num_dpb_buffer = 34816 / fs_in_mb;
		break;
	case 50:
		num_dpb_buffer = 110400 / fs_in_mb;
		break;
	case 51:
		num_dpb_buffer = 184320 / fs_in_mb;
		break;
	default:
		num_dpb_buffer = 184320 / fs_in_mb;
		break;
	}
	num_dpb_buffer++;
	max_references = MAX2(MIN2(NUM_H264_REFS, num_dpb_buffer), max_references);
	ctx_size = max_references * align(width_in_mb * height_in_mb  * 192, 256);

	return ctx_size;
}

/* calculate size of reference picture buffer */
static unsigned calc_dpb_size(struct radeon_decoder *dec)
{
	unsigned width_in_mb, height_in_mb, image_size, dpb_size;

	// always align them to MB size for dpb calculation
	unsigned width = align(dec->base.width, VL_MACROBLOCK_WIDTH);
	unsigned height = align(dec->base.height, VL_MACROBLOCK_HEIGHT);

	// always one more for currently decoded picture
	unsigned max_references = dec->base.max_references + 1;

	// aligned size of a single frame
	image_size = align(width, 32) * height;
	image_size += image_size / 2;
	image_size = align(image_size, 1024);

	// picture width & height in 16 pixel units
	width_in_mb = width / VL_MACROBLOCK_WIDTH;
	height_in_mb = align(height / VL_MACROBLOCK_HEIGHT, 2);

	switch (u_reduce_video_profile(dec->base.profile)) {
	case PIPE_VIDEO_FORMAT_MPEG4_AVC: {
		unsigned fs_in_mb = width_in_mb * height_in_mb;
		unsigned num_dpb_buffer;

		switch(dec->base.level) {
		case 30:
			num_dpb_buffer = 8100 / fs_in_mb;
			break;
		case 31:
			num_dpb_buffer = 18000 / fs_in_mb;
			break;
		case 32:
			num_dpb_buffer = 20480 / fs_in_mb;
			break;
		case 41:
			num_dpb_buffer = 32768 / fs_in_mb;
			break;
		case 42:
			num_dpb_buffer = 34816 / fs_in_mb;
			break;
		case 50:
			num_dpb_buffer = 110400 / fs_in_mb;
			break;
		case 51:
			num_dpb_buffer = 184320 / fs_in_mb;
			break;
		default:
			num_dpb_buffer = 184320 / fs_in_mb;
			break;
		}
		num_dpb_buffer++;
		max_references = MAX2(MIN2(NUM_H264_REFS, num_dpb_buffer), max_references);
		dpb_size = image_size * max_references;
		break;
	}

	case PIPE_VIDEO_FORMAT_HEVC:
		if (dec->base.width * dec->base.height >= 4096*2000)
			max_references = MAX2(max_references, 8);
		else
			max_references = MAX2(max_references, 17);

		width = align (width, 16);
		height = align (height, 16);
		if (dec->base.profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10)
			dpb_size = align((align(width, 32) * height * 9) / 4, 256) * max_references;
		else
			dpb_size = align((align(width, 32) * height * 3) / 2, 256) * max_references;
		break;

	case PIPE_VIDEO_FORMAT_VC1:
		// the firmware seems to allways assume a minimum of ref frames
		max_references = MAX2(NUM_VC1_REFS, max_references);

		// reference picture buffer
		dpb_size = image_size * max_references;

		// CONTEXT_BUFFER
		dpb_size += width_in_mb * height_in_mb * 128;

		// IT surface buffer
		dpb_size += width_in_mb * 64;

		// DB surface buffer
		dpb_size += width_in_mb * 128;

		// BP
		dpb_size += align(MAX2(width_in_mb, height_in_mb) * 7 * 16, 64);
		break;

	case PIPE_VIDEO_FORMAT_MPEG12:
		// reference picture buffer, must be big enough for all frames
		dpb_size = image_size * NUM_MPEG2_REFS;
		break;

	case PIPE_VIDEO_FORMAT_MPEG4:
		// reference picture buffer
		dpb_size = image_size * max_references;

		// CM
		dpb_size += width_in_mb * height_in_mb * 64;

		// IT surface buffer
		dpb_size += align(width_in_mb * height_in_mb * 32, 64);

		dpb_size = MAX2(dpb_size, 30 * 1024 * 1024);
		break;

	default:
		// something is missing here
		assert(0);

		// at least use a sane default value
		dpb_size = 32 * 1024 * 1024;
		break;
	}
	return dpb_size;
}

/**
 * destroy this video decoder
 */
static void radeon_dec_destroy(struct pipe_video_codec *decoder)
{
	struct radeon_decoder *dec = (struct radeon_decoder*)decoder;
	unsigned i;

	assert(decoder);

	map_msg_fb_it_buf(dec);
	rvcn_dec_message_destroy(dec);
	send_msg_buf(dec);

	flush(dec, 0);

	dec->ws->cs_destroy(dec->cs);

	for (i = 0; i < NUM_BUFFERS; ++i) {
		rvid_destroy_buffer(&dec->msg_fb_it_buffers[i]);
		rvid_destroy_buffer(&dec->bs_buffers[i]);
	}

	rvid_destroy_buffer(&dec->dpb);
	rvid_destroy_buffer(&dec->ctx);
	rvid_destroy_buffer(&dec->sessionctx);

	FREE(dec);
}

/**
 * start decoding of a new frame
 */
static void radeon_dec_begin_frame(struct pipe_video_codec *decoder,
			     struct pipe_video_buffer *target,
			     struct pipe_picture_desc *picture)
{
	struct radeon_decoder *dec = (struct radeon_decoder*)decoder;
	uintptr_t frame;

	assert(decoder);

	frame = ++dec->frame_number;
	vl_video_buffer_set_associated_data(target, decoder, (void *)frame,
					    &radeon_dec_destroy_associated_data);

	dec->bs_size = 0;
	dec->bs_ptr = dec->ws->buffer_map(
		dec->bs_buffers[dec->cur_buffer].res->buf,
		dec->cs, PIPE_TRANSFER_WRITE);
}

/**
 * decode a macroblock
 */
static void radeon_dec_decode_macroblock(struct pipe_video_codec *decoder,
				   struct pipe_video_buffer *target,
				   struct pipe_picture_desc *picture,
				   const struct pipe_macroblock *macroblocks,
				   unsigned num_macroblocks)
{
	/* not supported (yet) */
	assert(0);
}

/**
 * decode a bitstream
 */
static void radeon_dec_decode_bitstream(struct pipe_video_codec *decoder,
				  struct pipe_video_buffer *target,
				  struct pipe_picture_desc *picture,
				  unsigned num_buffers,
				  const void * const *buffers,
				  const unsigned *sizes)
{
	struct radeon_decoder *dec = (struct radeon_decoder*)decoder;
	unsigned i;

	assert(decoder);

	if (!dec->bs_ptr)
		return;

	for (i = 0; i < num_buffers; ++i) {
		struct rvid_buffer *buf = &dec->bs_buffers[dec->cur_buffer];
		unsigned new_size = dec->bs_size + sizes[i];

		if (new_size > buf->res->buf->size) {
			dec->ws->buffer_unmap(buf->res->buf);
			if (!rvid_resize_buffer(dec->screen, dec->cs, buf, new_size)) {
				RVID_ERR("Can't resize bitstream buffer!");
				return;
			}

			dec->bs_ptr = dec->ws->buffer_map(buf->res->buf, dec->cs,
							  PIPE_TRANSFER_WRITE);
			if (!dec->bs_ptr)
				return;

			dec->bs_ptr += dec->bs_size;
		}

		memcpy(dec->bs_ptr, buffers[i], sizes[i]);
		dec->bs_size += sizes[i];
		dec->bs_ptr += sizes[i];
	}
}

/**
 * end decoding of the current frame
 */
static void radeon_dec_end_frame(struct pipe_video_codec *decoder,
			   struct pipe_video_buffer *target,
			   struct pipe_picture_desc *picture)
{
	struct radeon_decoder *dec = (struct radeon_decoder*)decoder;
	struct pb_buffer *dt;
	struct rvid_buffer *msg_fb_it_buf, *bs_buf;

	assert(decoder);

	if (!dec->bs_ptr)
		return;

	msg_fb_it_buf = &dec->msg_fb_it_buffers[dec->cur_buffer];
	bs_buf = &dec->bs_buffers[dec->cur_buffer];

	memset(dec->bs_ptr, 0, align(dec->bs_size, 128) - dec->bs_size);
	dec->ws->buffer_unmap(bs_buf->res->buf);

	map_msg_fb_it_buf(dec);
	dt = rvcn_dec_message_decode(dec);
	rvcn_dec_message_feedback(dec);
	send_msg_buf(dec);

	send_cmd(dec, RDECODE_CMD_DPB_BUFFER, dec->dpb.res->buf, 0,
		 RADEON_USAGE_READWRITE, RADEON_DOMAIN_VRAM);
	if (dec->ctx.res)
		send_cmd(dec, RDECODE_CMD_CONTEXT_BUFFER, dec->ctx.res->buf, 0,
			RADEON_USAGE_READWRITE, RADEON_DOMAIN_VRAM);
	send_cmd(dec, RDECODE_CMD_BITSTREAM_BUFFER, bs_buf->res->buf,
		 0, RADEON_USAGE_READ, RADEON_DOMAIN_GTT);
	send_cmd(dec, RDECODE_CMD_DECODING_TARGET_BUFFER, dt, 0,
		 RADEON_USAGE_WRITE, RADEON_DOMAIN_VRAM);
	send_cmd(dec, RDECODE_CMD_FEEDBACK_BUFFER, msg_fb_it_buf->res->buf,
		 FB_BUFFER_OFFSET, RADEON_USAGE_WRITE, RADEON_DOMAIN_GTT);
	if (have_it(dec))
		send_cmd(dec, RDECODE_CMD_IT_SCALING_TABLE_BUFFER, msg_fb_it_buf->res->buf,
			 FB_BUFFER_OFFSET + FB_BUFFER_SIZE, RADEON_USAGE_READ, RADEON_DOMAIN_GTT);
	set_reg(dec, RDECODE_ENGINE_CNTL, 1);

	flush(dec, RADEON_FLUSH_ASYNC);
	next_buffer(dec);
}

/**
 * flush any outstanding command buffers to the hardware
 */
static void radeon_dec_flush(struct pipe_video_codec *decoder)
{
}

/**
 * create and HW decoder
 */
struct pipe_video_codec *radeon_create_decoder(struct pipe_context *context,
					     const struct pipe_video_codec *templ)
{
	struct radeon_winsys* ws = ((struct r600_common_context *)context)->ws;
	struct r600_common_context *rctx = (struct r600_common_context*)context;
	unsigned width = templ->width, height = templ->height;
	unsigned dpb_size, bs_buf_size, stream_type = 0;
	struct radeon_decoder *dec;
	int r, i;

	switch(u_reduce_video_profile(templ->profile)) {
	case PIPE_VIDEO_FORMAT_MPEG12:
		if (templ->entrypoint > PIPE_VIDEO_ENTRYPOINT_BITSTREAM)
			return vl_create_mpeg12_decoder(context, templ);
		stream_type = RDECODE_CODEC_MPEG2_VLD;
		break;
	case PIPE_VIDEO_FORMAT_MPEG4:
		width = align(width, VL_MACROBLOCK_WIDTH);
		height = align(height, VL_MACROBLOCK_HEIGHT);
		stream_type = RDECODE_CODEC_MPEG4;
		break;
	case PIPE_VIDEO_FORMAT_VC1:
		stream_type = RDECODE_CODEC_VC1;
		break;
	case PIPE_VIDEO_FORMAT_MPEG4_AVC:
		width = align(width, VL_MACROBLOCK_WIDTH);
		height = align(height, VL_MACROBLOCK_HEIGHT);
		stream_type = RDECODE_CODEC_H264_PERF;
		break;
	case PIPE_VIDEO_FORMAT_HEVC:
		stream_type = RDECODE_CODEC_H265;
		break;
	default:
		assert(0);
		break;
	}

	dec = CALLOC_STRUCT(radeon_decoder);

	if (!dec)
		return NULL;

	dec->base = *templ;
	dec->base.context = context;
	dec->base.width = width;
	dec->base.height = height;

	dec->base.destroy = radeon_dec_destroy;
	dec->base.begin_frame = radeon_dec_begin_frame;
	dec->base.decode_macroblock = radeon_dec_decode_macroblock;
	dec->base.decode_bitstream = radeon_dec_decode_bitstream;
	dec->base.end_frame = radeon_dec_end_frame;
	dec->base.flush = radeon_dec_flush;

	dec->stream_type = stream_type;
	dec->stream_handle = rvid_alloc_stream_handle();
	dec->screen = context->screen;
	dec->ws = ws;
	dec->cs = ws->cs_create(rctx->ctx, RING_VCN_DEC, NULL, NULL);
	if (!dec->cs) {
		RVID_ERR("Can't get command submission context.\n");
		goto error;
	}

	bs_buf_size = width * height * (512 / (16 * 16));
	for (i = 0; i < NUM_BUFFERS; ++i) {
		unsigned msg_fb_it_size = FB_BUFFER_OFFSET + FB_BUFFER_SIZE;
		if (have_it(dec))
			msg_fb_it_size += IT_SCALING_TABLE_SIZE;
		if (!rvid_create_buffer(dec->screen, &dec->msg_fb_it_buffers[i],
					msg_fb_it_size, PIPE_USAGE_STAGING)) {
			RVID_ERR("Can't allocated message buffers.\n");
			goto error;
		}

		if (!rvid_create_buffer(dec->screen, &dec->bs_buffers[i],
					bs_buf_size, PIPE_USAGE_STAGING)) {
			RVID_ERR("Can't allocated bitstream buffers.\n");
			goto error;
		}

		rvid_clear_buffer(context, &dec->msg_fb_it_buffers[i]);
		rvid_clear_buffer(context, &dec->bs_buffers[i]);
	}

	dpb_size = calc_dpb_size(dec);

	if (!rvid_create_buffer(dec->screen, &dec->dpb, dpb_size, PIPE_USAGE_DEFAULT)) {
		RVID_ERR("Can't allocated dpb.\n");
		goto error;
	}

	rvid_clear_buffer(context, &dec->dpb);

	if (dec->stream_type == RDECODE_CODEC_H264_PERF) {
		unsigned ctx_size = calc_ctx_size_h264_perf(dec);
		if (!rvid_create_buffer(dec->screen, &dec->ctx, ctx_size, PIPE_USAGE_DEFAULT)) {
			RVID_ERR("Can't allocated context buffer.\n");
			goto error;
		}
		rvid_clear_buffer(context, &dec->ctx);
	}

	if (!rvid_create_buffer(dec->screen, &dec->sessionctx,
				RDECODE_SESSION_CONTEXT_SIZE,
				PIPE_USAGE_DEFAULT)) {
		RVID_ERR("Can't allocated session ctx.\n");
		goto error;
	}
	rvid_clear_buffer(context, &dec->sessionctx);

	map_msg_fb_it_buf(dec);
	rvcn_dec_message_create(dec);
	send_msg_buf(dec);
	r = flush(dec, 0);
	if (r)
		goto error;

	next_buffer(dec);

	return &dec->base;

error:
	if (dec->cs) dec->ws->cs_destroy(dec->cs);

	for (i = 0; i < NUM_BUFFERS; ++i) {
		rvid_destroy_buffer(&dec->msg_fb_it_buffers[i]);
		rvid_destroy_buffer(&dec->bs_buffers[i]);
	}

	rvid_destroy_buffer(&dec->dpb);
	rvid_destroy_buffer(&dec->ctx);
	rvid_destroy_buffer(&dec->sessionctx);

	FREE(dec);

	return NULL;
}
