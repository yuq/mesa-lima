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

#define RADEON_ENC_CS(value) (enc->cs->current.buf[enc->cs->current.cdw++] = (value))
#define RADEON_ENC_BEGIN(cmd) { \
	uint32_t *begin = &enc->cs->current.buf[enc->cs->current.cdw++]; \
RADEON_ENC_CS(cmd)
#define RADEON_ENC_READ(buf, domain, off) radeon_enc_add_buffer(enc, (buf), RADEON_USAGE_READ, (domain), (off))
#define RADEON_ENC_WRITE(buf, domain, off) radeon_enc_add_buffer(enc, (buf), RADEON_USAGE_WRITE, (domain), (off))
#define RADEON_ENC_READWRITE(buf, domain, off) radeon_enc_add_buffer(enc, (buf), RADEON_USAGE_READWRITE, (domain), (off))
#define RADEON_ENC_END() *begin = (&enc->cs->current.buf[enc->cs->current.cdw] - begin) * 4; \
	enc->total_task_size += *begin;}

static const unsigned profiles[7] = { 66, 77, 88, 100, 110, 122, 244 };

static void radeon_enc_add_buffer(struct radeon_encoder *enc, struct pb_buffer *buf,
								  enum radeon_bo_usage usage, enum radeon_bo_domain domain,
								  signed offset)
{
	enc->ws->cs_add_buffer(enc->cs, buf, usage | RADEON_USAGE_SYNCHRONIZED,
									   domain, RADEON_PRIO_VCE);
	uint64_t addr;
	addr = enc->ws->buffer_get_virtual_address(buf);
	addr = addr + offset;
	RADEON_ENC_CS(addr >> 32);
	RADEON_ENC_CS(addr);
}

static void radeon_enc_session_info(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_task_info(struct radeon_encoder *enc, bool need_feedback)
{
	/* TODO*/
}

static void radeon_enc_session_init(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_layer_control(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_layer_select(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_slice_control(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_spec_misc(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_rc_session_init(struct radeon_encoder *enc, struct pipe_h264_enc_picture_desc *pic)
{
	/* TODO*/
}

static void radeon_enc_rc_layer_init(struct radeon_encoder *enc, struct pipe_h264_enc_picture_desc *pic)
{
	/* TODO*/
}

static void radeon_enc_deblocking_filter_h264(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_quality_params(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_ctx(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_bitstream(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_feedback(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_intra_refresh(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_rc_per_pic(struct radeon_encoder *enc, struct pipe_h264_enc_picture_desc *pic)
{
	/* TODO*/
}

static void radeon_enc_encode_params(struct radeon_encoder *enc)
{
	/* TODO*/
}
static void radeon_enc_encode_params_h264(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_op_init(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_op_close(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_op_enc(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_op_init_rc(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_op_init_rc_vbv(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void radeon_enc_op_speed(struct radeon_encoder *enc)
{
	/* TODO*/
}

static void begin(struct radeon_encoder *enc, struct pipe_h264_enc_picture_desc *pic)
{
	radeon_enc_session_info(enc);
	enc->total_task_size = 0;
	radeon_enc_task_info(enc, enc->need_feedback);
	radeon_enc_op_init(enc);
	radeon_enc_session_init(enc);
	radeon_enc_layer_control(enc);
	radeon_enc_slice_control(enc);
	radeon_enc_spec_misc(enc);
	radeon_enc_rc_session_init(enc, pic);
	radeon_enc_deblocking_filter_h264(enc);
	radeon_enc_quality_params(enc);
	radeon_enc_layer_select(enc);
	radeon_enc_rc_layer_init(enc, pic);
	radeon_enc_layer_select(enc);
	radeon_enc_rc_per_pic(enc, pic);
	radeon_enc_op_init_rc(enc);
	radeon_enc_op_init_rc_vbv(enc);
	*enc->p_task_size = (enc->total_task_size);
}

static void encode(struct radeon_encoder *enc)
{
	radeon_enc_session_info(enc);
	enc->total_task_size = 0;
	radeon_enc_task_info(enc, enc->need_feedback);
	radeon_enc_ctx(enc);
	radeon_enc_bitstream(enc);
	radeon_enc_feedback(enc);
	radeon_enc_intra_refresh(enc);
	radeon_enc_encode_params(enc);
	radeon_enc_encode_params_h264(enc);
	radeon_enc_op_speed(enc);
	radeon_enc_op_enc(enc);
	*enc->p_task_size = (enc->total_task_size);
}

static void destroy(struct radeon_encoder *enc)
{
	radeon_enc_session_info(enc);
	enc->total_task_size = 0;
	radeon_enc_task_info(enc, enc->need_feedback);
	radeon_enc_op_close(enc);
	*enc->p_task_size = (enc->total_task_size);
}

void radeon_enc_1_2_init(struct radeon_encoder *enc)
{
	enc->begin = begin;
	enc->encode = encode;
	enc->destroy = destroy;
}
