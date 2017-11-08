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

#include <stdio.h>

#include "pipe/p_video_codec.h"

#include "util/u_video.h"
#include "util/u_memory.h"

#include "vl/vl_video_buffer.h"

#include "r600_pipe_common.h"
#include "radeon_video.h"
#include "radeon_vcn_enc.h"

static void radeon_vcn_enc_get_param(struct radeon_encoder *enc, struct pipe_h264_enc_picture_desc *pic)
{
	enc->enc_pic.picture_type = pic->picture_type;
	enc->enc_pic.frame_num = pic->frame_num;
	enc->enc_pic.pic_order_cnt = pic->pic_order_cnt;
	enc->enc_pic.pic_order_cnt_type = pic->pic_order_cnt_type;
	enc->enc_pic.ref_idx_l0 = pic->ref_idx_l0;
	enc->enc_pic.ref_idx_l1 = pic->ref_idx_l1;
	enc->enc_pic.not_referenced = pic->not_referenced;
	enc->enc_pic.is_idr = pic->is_idr;
	enc->enc_pic.crop_left = 0;
	enc->enc_pic.crop_right = (align(enc->base.width, 16) - enc->base.width) / 2;
	enc->enc_pic.crop_top = 0;
	enc->enc_pic.crop_bottom = (align(enc->base.height, 16) - enc->base.height) / 2;
}

static void flush(struct radeon_encoder *enc)
{
	enc->ws->cs_flush(enc->cs, RADEON_FLUSH_ASYNC, NULL);
}

static void radeon_enc_flush(struct pipe_video_codec *encoder)
{
	struct radeon_encoder *enc = (struct radeon_encoder*)encoder;
	flush(enc);
}

static void radeon_enc_cs_flush(void *ctx, unsigned flags,
								struct pipe_fence_handle **fence)
{
	// just ignored
}

static unsigned get_cpb_num(struct radeon_encoder *enc)
{
	unsigned w = align(enc->base.width, 16) / 16;
	unsigned h = align(enc->base.height, 16) / 16;
	unsigned dpb;

	switch (enc->base.level) {
	case 10:
		dpb = 396;
		break;
	case 11:
		dpb = 900;
		break;
	case 12:
	case 13:
	case 20:
		dpb = 2376;
		break;
	case 21:
		dpb = 4752;
		break;
	case 22:
	case 30:
		dpb = 8100;
		break;
	case 31:
		dpb = 18000;
		break;
	case 32:
		dpb = 20480;
		break;
	case 40:
	case 41:
		dpb = 32768;
		break;
	case 42:
		dpb = 34816;
		break;
	case 50:
		dpb = 110400;
		break;
	default:
	case 51:
	case 52:
		dpb = 184320;
		break;
	}

	return MIN2(dpb / (w * h), 16);
}

static void radeon_enc_begin_frame(struct pipe_video_codec *encoder,
							 struct pipe_video_buffer *source,
							 struct pipe_picture_desc *picture)
{
	/* TODO*/
}

static void radeon_enc_encode_bitstream(struct pipe_video_codec *encoder,
								  struct pipe_video_buffer *source,
								  struct pipe_resource *destination,
								  void **fb)
{
	/* TODO*/
}

static void radeon_enc_end_frame(struct pipe_video_codec *encoder,
						   struct pipe_video_buffer *source,
						   struct pipe_picture_desc *picture)
{
	/* TODO*/
}

static void radeon_enc_destroy(struct pipe_video_codec *encoder)
{
	/* TODO*/
}

static void radeon_enc_get_feedback(struct pipe_video_codec *encoder,
							  void *feedback, unsigned *size)
{
	/* TODO*/
}

struct pipe_video_codec *radeon_create_encoder(struct pipe_context *context,
		const struct pipe_video_codec *templ,
		struct radeon_winsys* ws,
		radeon_enc_get_buffer get_buffer)
{
	/* TODO*/
}
