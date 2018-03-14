/**************************************************************************
 *
 * Copyright 2018 Advanced Micro Devices, Inc.
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

#include "va_private.h"

void vlVaHandlePictureParameterBufferVP9(vlVaDriver *drv, vlVaContext *context, vlVaBuffer *buf)
{
   VADecPictureParameterBufferVP9 *vp9 = buf->data;
   int i;

   assert(buf->size >= sizeof(VADecPictureParameterBufferVP9) && buf->num_elements == 1);

   context->desc.vp9.picture_parameter.frame_width = vp9->frame_width;
   context->desc.vp9.picture_parameter.frame_height = vp9->frame_height;

   context->desc.vp9.picture_parameter.pic_fields.subsampling_x = vp9->pic_fields.bits.subsampling_x;
   context->desc.vp9.picture_parameter.pic_fields.subsampling_y = vp9->pic_fields.bits.subsampling_y;
   context->desc.vp9.picture_parameter.pic_fields.frame_type = vp9->pic_fields.bits.frame_type;
   context->desc.vp9.picture_parameter.pic_fields.show_frame = vp9->pic_fields.bits.show_frame;
   context->desc.vp9.picture_parameter.pic_fields.error_resilient_mode = vp9->pic_fields.bits.error_resilient_mode;
   context->desc.vp9.picture_parameter.pic_fields.intra_only = vp9->pic_fields.bits.intra_only;
   context->desc.vp9.picture_parameter.pic_fields.allow_high_precision_mv = vp9->pic_fields.bits.allow_high_precision_mv;
   context->desc.vp9.picture_parameter.pic_fields.mcomp_filter_type = vp9->pic_fields.bits.mcomp_filter_type;
   context->desc.vp9.picture_parameter.pic_fields.frame_parallel_decoding_mode = vp9->pic_fields.bits.frame_parallel_decoding_mode;
   context->desc.vp9.picture_parameter.pic_fields.reset_frame_context = vp9->pic_fields.bits.reset_frame_context;
   context->desc.vp9.picture_parameter.pic_fields.refresh_frame_context = vp9->pic_fields.bits.refresh_frame_context;
   context->desc.vp9.picture_parameter.pic_fields.frame_context_idx = vp9->pic_fields.bits.frame_context_idx;
   context->desc.vp9.picture_parameter.pic_fields.segmentation_enabled = vp9->pic_fields.bits.segmentation_enabled;
   context->desc.vp9.picture_parameter.pic_fields.segmentation_temporal_update = vp9->pic_fields.bits.segmentation_temporal_update;
   context->desc.vp9.picture_parameter.pic_fields.segmentation_update_map = vp9->pic_fields.bits.segmentation_update_map;
   context->desc.vp9.picture_parameter.pic_fields.last_ref_frame = vp9->pic_fields.bits.last_ref_frame;
   context->desc.vp9.picture_parameter.pic_fields.last_ref_frame_sign_bias = vp9->pic_fields.bits.last_ref_frame_sign_bias;
   context->desc.vp9.picture_parameter.pic_fields.golden_ref_frame = vp9->pic_fields.bits.golden_ref_frame;
   context->desc.vp9.picture_parameter.pic_fields.golden_ref_frame_sign_bias = vp9->pic_fields.bits.golden_ref_frame_sign_bias;
   context->desc.vp9.picture_parameter.pic_fields.alt_ref_frame = vp9->pic_fields.bits.alt_ref_frame;
   context->desc.vp9.picture_parameter.pic_fields.alt_ref_frame_sign_bias = vp9->pic_fields.bits.alt_ref_frame_sign_bias;
   context->desc.vp9.picture_parameter.pic_fields.lossless_flag = vp9->pic_fields.bits.lossless_flag;

   context->desc.vp9.picture_parameter.filter_level = vp9->filter_level;
   context->desc.vp9.picture_parameter.sharpness_level = vp9->sharpness_level;

   context->desc.vp9.picture_parameter.log2_tile_rows = vp9->log2_tile_rows;
   context->desc.vp9.picture_parameter.log2_tile_columns = vp9->log2_tile_columns;

   context->desc.vp9.picture_parameter.frame_header_length_in_bytes = vp9->frame_header_length_in_bytes;
   context->desc.vp9.picture_parameter.first_partition_size = vp9->first_partition_size;

   for (i = 0; i < 7; ++i)
      context->desc.vp9.picture_parameter.mb_segment_tree_probs[i] = vp9->mb_segment_tree_probs[i];
   for (i = 0; i < 3; ++i)
      context->desc.vp9.picture_parameter.segment_pred_probs[i] = vp9->segment_pred_probs[i];

   context->desc.vp9.picture_parameter.profile = vp9->profile;

   context->desc.vp9.picture_parameter.bit_depth = vp9->bit_depth;

   for (i = 0 ; i < 8 ; i++)
      vlVaGetReferenceFrame(drv, vp9->reference_frames[i], &context->desc.vp9.ref[i]);
}

void vlVaHandleSliceParameterBufferVP9(vlVaContext *context, vlVaBuffer *buf)
{
   /* TODO */
}
