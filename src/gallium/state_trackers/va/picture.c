/**************************************************************************
 *
 * Copyright 2010 Thomas Balling Sørensen & Orasanu Lucian.
 * Copyright 2014 Advanced Micro Devices, Inc.
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

#include "pipe/p_video_codec.h"

#include "util/u_handle_table.h"
#include "util/u_video.h"

#include "vl/vl_vlc.h"
#include "vl/vl_winsys.h"

#include "va_private.h"

VAStatus
vlVaBeginPicture(VADriverContextP ctx, VAContextID context_id, VASurfaceID render_target)
{
   vlVaDriver *drv;
   vlVaContext *context;
   vlVaSurface *surf;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   if (!drv)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   context = handle_table_get(drv->htab, context_id);
   if (!context)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   surf = handle_table_get(drv->htab, render_target);
   if (!surf || !surf->buffer)
      return VA_STATUS_ERROR_INVALID_SURFACE;

   context->target = surf->buffer;
   if (!context->decoder) {
      /* VPP */
      if ((context->target->buffer_format != PIPE_FORMAT_B8G8R8A8_UNORM  &&
           context->target->buffer_format != PIPE_FORMAT_R8G8B8A8_UNORM  &&
           context->target->buffer_format != PIPE_FORMAT_B8G8R8X8_UNORM  &&
           context->target->buffer_format != PIPE_FORMAT_R8G8B8X8_UNORM) ||
           context->target->interlaced)
         return VA_STATUS_ERROR_UNIMPLEMENTED;
      return VA_STATUS_SUCCESS;
   }

   context->decoder->begin_frame(context->decoder, context->target, &context->desc.base);

   return VA_STATUS_SUCCESS;
}

void
vlVaGetReferenceFrame(vlVaDriver *drv, VASurfaceID surface_id,
                      struct pipe_video_buffer **ref_frame)
{
   vlVaSurface *surf = handle_table_get(drv->htab, surface_id);
   if (surf)
      *ref_frame = surf->buffer;
   else
      *ref_frame = NULL;
}

static void
handlePictureParameterBuffer(vlVaDriver *drv, vlVaContext *context, vlVaBuffer *buf)
{
   VAPictureParameterBufferHEVC *hevc;
   unsigned int i;

   switch (u_reduce_video_profile(context->decoder->profile)) {
   case PIPE_VIDEO_FORMAT_MPEG12:
      vlVaHandlePictureParameterBufferMPEG12(drv, context, buf);
      break;

   case PIPE_VIDEO_FORMAT_MPEG4_AVC:
      vlVaHandlePictureParameterBufferH264(drv, context, buf);
      break;

   case PIPE_VIDEO_FORMAT_VC1:
      vlVaHandlePictureParameterBufferVC1(drv, context, buf);
      break;

   case PIPE_VIDEO_FORMAT_MPEG4:
      vlVaHandlePictureParameterBufferMPEG4(drv, context, buf);
      break;

  case PIPE_VIDEO_FORMAT_HEVC:
      assert(buf->size >= sizeof(VAPictureParameterBufferHEVC) && buf->num_elements == 1);
      hevc = buf->data;
      context->desc.h265.pps->sps->chroma_format_idc = hevc->pic_fields.bits.chroma_format_idc;
      context->desc.h265.pps->sps->separate_colour_plane_flag =
         hevc->pic_fields.bits.separate_colour_plane_flag;
      context->desc.h265.pps->sps->pic_width_in_luma_samples = hevc->pic_width_in_luma_samples;
      context->desc.h265.pps->sps->pic_height_in_luma_samples = hevc->pic_height_in_luma_samples;
      context->desc.h265.pps->sps->bit_depth_luma_minus8 = hevc->bit_depth_luma_minus8;
      context->desc.h265.pps->sps->bit_depth_chroma_minus8 = hevc->bit_depth_chroma_minus8;
      context->desc.h265.pps->sps->log2_max_pic_order_cnt_lsb_minus4 =
         hevc->log2_max_pic_order_cnt_lsb_minus4;
      context->desc.h265.pps->sps->sps_max_dec_pic_buffering_minus1 =
         hevc->sps_max_dec_pic_buffering_minus1;
      context->desc.h265.pps->sps->log2_min_luma_coding_block_size_minus3 =
         hevc->log2_min_luma_coding_block_size_minus3;
      context->desc.h265.pps->sps->log2_diff_max_min_luma_coding_block_size =
         hevc->log2_diff_max_min_luma_coding_block_size;
      context->desc.h265.pps->sps->log2_min_transform_block_size_minus2 =
         hevc->log2_min_transform_block_size_minus2;
      context->desc.h265.pps->sps->log2_diff_max_min_transform_block_size =
         hevc->log2_diff_max_min_transform_block_size;
      context->desc.h265.pps->sps->max_transform_hierarchy_depth_inter =
         hevc->max_transform_hierarchy_depth_inter;
      context->desc.h265.pps->sps->max_transform_hierarchy_depth_intra =
         hevc->max_transform_hierarchy_depth_intra;
      context->desc.h265.pps->sps->scaling_list_enabled_flag =
         hevc->pic_fields.bits.scaling_list_enabled_flag;
      context->desc.h265.pps->sps->amp_enabled_flag = hevc->pic_fields.bits.amp_enabled_flag;
      context->desc.h265.pps->sps->sample_adaptive_offset_enabled_flag =
         hevc->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag;
      context->desc.h265.pps->sps->pcm_enabled_flag = hevc->pic_fields.bits.pcm_enabled_flag;
      if (hevc->pic_fields.bits.pcm_enabled_flag == 1) {
         context->desc.h265.pps->sps->pcm_sample_bit_depth_luma_minus1 =
            hevc->pcm_sample_bit_depth_luma_minus1;
         context->desc.h265.pps->sps->pcm_sample_bit_depth_chroma_minus1 =
            hevc->pcm_sample_bit_depth_chroma_minus1;
         context->desc.h265.pps->sps->log2_min_pcm_luma_coding_block_size_minus3 =
            hevc->log2_min_pcm_luma_coding_block_size_minus3;
         context->desc.h265.pps->sps->log2_diff_max_min_pcm_luma_coding_block_size =
            hevc->log2_diff_max_min_pcm_luma_coding_block_size;
         context->desc.h265.pps->sps->pcm_loop_filter_disabled_flag =
            hevc->pic_fields.bits.pcm_loop_filter_disabled_flag;
      }
      context->desc.h265.pps->sps->num_short_term_ref_pic_sets = hevc->num_short_term_ref_pic_sets;
      context->desc.h265.pps->sps->long_term_ref_pics_present_flag =
         hevc->slice_parsing_fields.bits.long_term_ref_pics_present_flag;
      context->desc.h265.pps->sps->num_long_term_ref_pics_sps = hevc->num_long_term_ref_pic_sps;
      context->desc.h265.pps->sps->sps_temporal_mvp_enabled_flag =
         hevc->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag;
      context->desc.h265.pps->sps->strong_intra_smoothing_enabled_flag =
         hevc->pic_fields.bits.strong_intra_smoothing_enabled_flag;

      context->desc.h265.pps->dependent_slice_segments_enabled_flag =
         hevc->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag;
      context->desc.h265.pps->output_flag_present_flag =
         hevc->slice_parsing_fields.bits.output_flag_present_flag;
      context->desc.h265.pps->num_extra_slice_header_bits = hevc->num_extra_slice_header_bits;
      context->desc.h265.pps->sign_data_hiding_enabled_flag =
         hevc->pic_fields.bits.sign_data_hiding_enabled_flag;
      context->desc.h265.pps->cabac_init_present_flag =
         hevc->slice_parsing_fields.bits.cabac_init_present_flag;
      context->desc.h265.pps->num_ref_idx_l0_default_active_minus1 =
         hevc->num_ref_idx_l0_default_active_minus1;
      context->desc.h265.pps->num_ref_idx_l1_default_active_minus1 =
         hevc->num_ref_idx_l1_default_active_minus1;
      context->desc.h265.pps->init_qp_minus26 = hevc->init_qp_minus26;
      context->desc.h265.pps->constrained_intra_pred_flag =
         hevc->pic_fields.bits.constrained_intra_pred_flag;
      context->desc.h265.pps->transform_skip_enabled_flag =
         hevc->pic_fields.bits.transform_skip_enabled_flag;
      context->desc.h265.pps->cu_qp_delta_enabled_flag =
         hevc->pic_fields.bits.cu_qp_delta_enabled_flag;
      context->desc.h265.pps->diff_cu_qp_delta_depth = hevc->diff_cu_qp_delta_depth;
      context->desc.h265.pps->pps_cb_qp_offset = hevc->pps_cb_qp_offset;
      context->desc.h265.pps->pps_cr_qp_offset = hevc->pps_cr_qp_offset;
      context->desc.h265.pps->pps_slice_chroma_qp_offsets_present_flag =
         hevc->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag;
      context->desc.h265.pps->weighted_pred_flag = hevc->pic_fields.bits.weighted_pred_flag;
      context->desc.h265.pps->weighted_bipred_flag = hevc->pic_fields.bits.weighted_bipred_flag;
      context->desc.h265.pps->transquant_bypass_enabled_flag =
         hevc->pic_fields.bits.transquant_bypass_enabled_flag;
      context->desc.h265.pps->tiles_enabled_flag = hevc->pic_fields.bits.tiles_enabled_flag;
      context->desc.h265.pps->entropy_coding_sync_enabled_flag =
         hevc->pic_fields.bits.entropy_coding_sync_enabled_flag;
      if (hevc->pic_fields.bits.tiles_enabled_flag == 1) {
         context->desc.h265.pps->num_tile_columns_minus1 = hevc->num_tile_columns_minus1;
         context->desc.h265.pps->num_tile_rows_minus1 = hevc->num_tile_rows_minus1;
         for (i = 0 ; i < 19 ; i++)
            context->desc.h265.pps->column_width_minus1[i] = hevc->column_width_minus1[i];
         for (i = 0 ; i < 21 ; i++)
            context->desc.h265.pps->row_height_minus1[i] = hevc->row_height_minus1[i];
         context->desc.h265.pps->loop_filter_across_tiles_enabled_flag =
            hevc->pic_fields.bits.loop_filter_across_tiles_enabled_flag;
      }
      context->desc.h265.pps->pps_loop_filter_across_slices_enabled_flag =
         hevc->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag;
      context->desc.h265.pps->deblocking_filter_override_enabled_flag =
         hevc->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag;
      context->desc.h265.pps->pps_deblocking_filter_disabled_flag =
         hevc->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag;
      context->desc.h265.pps->pps_beta_offset_div2 = hevc->pps_beta_offset_div2;
      context->desc.h265.pps->pps_tc_offset_div2 = hevc->pps_tc_offset_div2;
      context->desc.h265.pps->lists_modification_present_flag =
         hevc->slice_parsing_fields.bits.lists_modification_present_flag;
      context->desc.h265.pps->log2_parallel_merge_level_minus2 =
         hevc->log2_parallel_merge_level_minus2;
      context->desc.h265.pps->slice_segment_header_extension_present_flag =
         hevc->slice_parsing_fields.bits.slice_segment_header_extension_present_flag;

      context->desc.h265.IDRPicFlag = hevc->slice_parsing_fields.bits.IdrPicFlag;
      context->desc.h265.RAPPicFlag = hevc->slice_parsing_fields.bits.RapPicFlag;

      context->desc.h265.CurrPicOrderCntVal = hevc->CurrPic.pic_order_cnt;

      for (i = 0 ; i < 8 ; i++) {
         context->desc.h265.RefPicSetStCurrBefore[i] = 0xFF;
         context->desc.h265.RefPicSetStCurrAfter[i] = 0xFF;
         context->desc.h265.RefPicSetLtCurr[i] = 0xFF;
      }
      context->desc.h265.NumPocStCurrBefore = 0;
      context->desc.h265.NumPocStCurrAfter = 0;
      context->desc.h265.NumPocLtCurr = 0;
      unsigned int iBefore = 0;
      unsigned int iAfter = 0;
      unsigned int iCurr = 0;
      for (i = 0 ; i < 15 ; i++) {
         context->desc.h265.PicOrderCntVal[i] = hevc->ReferenceFrames[i].pic_order_cnt;

         unsigned int index = hevc->ReferenceFrames[i].picture_id & 0x7F;

         if (index == 0x7F)
            continue;

         vlVaGetReferenceFrame(drv, hevc->ReferenceFrames[i].picture_id, &context->desc.h265.ref[i]);

         if ((hevc->ReferenceFrames[i].flags & VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE) && (iBefore < 8)) {
            context->desc.h265.RefPicSetStCurrBefore[iBefore++] = i;
            context->desc.h265.NumPocStCurrBefore++;
         }
         if ((hevc->ReferenceFrames[i].flags & VA_PICTURE_HEVC_RPS_ST_CURR_AFTER) && (iAfter < 8)) {
            context->desc.h265.RefPicSetStCurrAfter[iAfter++] = i;
            context->desc.h265.NumPocStCurrAfter++;
         }
         if ((hevc->ReferenceFrames[i].flags & VA_PICTURE_HEVC_RPS_LT_CURR) && (iCurr < 8)) {
            context->desc.h265.RefPicSetLtCurr[iCurr++] = i;
            context->desc.h265.NumPocLtCurr++;
         }
      }
      break;

   default:
      break;
   }
}

static void
handleIQMatrixBuffer(vlVaContext *context, vlVaBuffer *buf)
{
   VAIQMatrixBufferHEVC *h265;

   switch (u_reduce_video_profile(context->decoder->profile)) {
   case PIPE_VIDEO_FORMAT_MPEG12:
      vlVaHandleIQMatrixBufferMPEG12(context, buf);
      break;

   case PIPE_VIDEO_FORMAT_MPEG4_AVC:
      vlVaHandleIQMatrixBufferH264(context, buf);
      break;

   case PIPE_VIDEO_FORMAT_MPEG4:
      vlVaHandleIQMatrixBufferMPEG4(context, buf);

   case PIPE_VIDEO_FORMAT_HEVC:
      assert(buf->size >= sizeof(VAIQMatrixBufferH264) && buf->num_elements == 1);
      h265 = buf->data;
      memcpy(&context->desc.h265.pps->sps->ScalingList4x4, h265->ScalingList4x4, 6 * 16);
      memcpy(&context->desc.h265.pps->sps->ScalingList8x8, h265->ScalingList8x8, 6 * 64);
      memcpy(&context->desc.h265.pps->sps->ScalingList16x16, h265->ScalingList16x16, 6 * 64);
      memcpy(&context->desc.h265.pps->sps->ScalingList32x32, h265->ScalingList32x32, 2 * 64);
      memcpy(&context->desc.h265.pps->sps->ScalingListDCCoeff16x16, h265->ScalingListDC16x16, 6);
      memcpy(&context->desc.h265.pps->sps->ScalingListDCCoeff32x32, h265->ScalingListDC32x32, 2);
      break;

   default:
      break;
   }
}

static void
handleSliceParameterBuffer(vlVaContext *context, vlVaBuffer *buf)
{
   VASliceParameterBufferHEVC *h265;

   switch (u_reduce_video_profile(context->decoder->profile)) {
   case PIPE_VIDEO_FORMAT_MPEG4_AVC:
      vlVaHandleSliceParameterBufferH264(context, buf);
      break;

   case PIPE_VIDEO_FORMAT_MPEG4:
      vlVaHandleSliceParameterBufferMPEG4(context, buf);
      break;

   case PIPE_VIDEO_FORMAT_HEVC:
      assert(buf->size >= sizeof(VASliceParameterBufferHEVC) && buf->num_elements == 1);
      h265 = buf->data;
      for (int i = 0 ; i < 2 ; i++) {
         for (int j = 0 ; j < 15 ; j++)
            context->desc.h265.RefPicList[i][j] = h265->RefPicList[i][j];
      }
      context->desc.h265.UseRefPicList = true;
      break;
   default:
      break;
   }
}

static unsigned int
bufHasStartcode(vlVaBuffer *buf, unsigned int code, unsigned int bits)
{
   struct vl_vlc vlc = {0};
   int i;

   /* search the first 64 bytes for a startcode */
   vl_vlc_init(&vlc, 1, (const void * const*)&buf->data, &buf->size);
   for (i = 0; i < 64 && vl_vlc_bits_left(&vlc) >= bits; ++i) {
      if (vl_vlc_peekbits(&vlc, bits) == code)
         return 1;
      vl_vlc_eatbits(&vlc, 8);
      vl_vlc_fillbits(&vlc);
   }

   return 0;
}

static void
handleVASliceDataBufferType(vlVaContext *context, vlVaBuffer *buf)
{
   enum pipe_video_format format;
   unsigned num_buffers = 0;
   void * const *buffers[2];
   unsigned sizes[2];
   static const uint8_t start_code_h264[] = { 0x00, 0x00, 0x01 };
   static const uint8_t start_code_h265[] = { 0x00, 0x00, 0x01 };
   static const uint8_t start_code_vc1[] = { 0x00, 0x00, 0x01, 0x0d };

   format = u_reduce_video_profile(context->decoder->profile);
   switch (format) {
   case PIPE_VIDEO_FORMAT_MPEG4_AVC:
         if (bufHasStartcode(buf, 0x000001, 24))
            break;

         buffers[num_buffers] = (void *const)&start_code_h264;
         sizes[num_buffers++] = sizeof(start_code_h264);
      break;
   case PIPE_VIDEO_FORMAT_HEVC:
      if (bufHasStartcode(buf, 0x000001, 24))
         break;

         buffers[num_buffers] = (void *const)&start_code_h265;
         sizes[num_buffers++] = sizeof(start_code_h265);
         break;
   case PIPE_VIDEO_FORMAT_VC1:
      if (bufHasStartcode(buf, 0x0000010d, 32) ||
          bufHasStartcode(buf, 0x0000010c, 32) ||
          bufHasStartcode(buf, 0x0000010b, 32))
         break;

      if (context->decoder->profile == PIPE_VIDEO_PROFILE_VC1_ADVANCED) {
         buffers[num_buffers] = (void *const)&start_code_vc1;
         sizes[num_buffers++] = sizeof(start_code_vc1);
      }
      break;
   case PIPE_VIDEO_FORMAT_MPEG4:
      if (bufHasStartcode(buf, 0x000001, 24))
         break;

      vlVaDecoderFixMPEG4Startcode(context);
      buffers[num_buffers] = (void *)context->mpeg4.start_code;
      sizes[num_buffers++] = context->mpeg4.start_code_size;
   default:
      break;
   }

   buffers[num_buffers] = buf->data;
   sizes[num_buffers] = buf->size;
   ++num_buffers;
   context->decoder->decode_bitstream(context->decoder, context->target, &context->desc.base,
      num_buffers, (const void * const*)buffers, sizes);
}

VAStatus
vlVaRenderPicture(VADriverContextP ctx, VAContextID context_id, VABufferID *buffers, int num_buffers)
{
   vlVaDriver *drv;
   vlVaContext *context;
   VAStatus vaStatus = VA_STATUS_SUCCESS;

   unsigned i;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   if (!drv)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   context = handle_table_get(drv->htab, context_id);
   if (!context)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   for (i = 0; i < num_buffers; ++i) {
      vlVaBuffer *buf = handle_table_get(drv->htab, buffers[i]);
      if (!buf)
         return VA_STATUS_ERROR_INVALID_BUFFER;

      switch (buf->type) {
      case VAPictureParameterBufferType:
         handlePictureParameterBuffer(drv, context, buf);
         break;

      case VAIQMatrixBufferType:
         handleIQMatrixBuffer(context, buf);
         break;

      case VASliceParameterBufferType:
         handleSliceParameterBuffer(context, buf);
         break;

      case VASliceDataBufferType:
         handleVASliceDataBufferType(context, buf);
         break;
      case VAProcPipelineParameterBufferType:
         vaStatus = vlVaHandleVAProcPipelineParameterBufferType(drv, context, buf);
         break;

      default:
         break;
      }
   }

   return vaStatus;
}

VAStatus
vlVaEndPicture(VADriverContextP ctx, VAContextID context_id)
{
   vlVaDriver *drv;
   vlVaContext *context;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   if (!drv)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   context = handle_table_get(drv->htab, context_id);
   if (!context)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   if (!context->decoder) {
      /* VPP */
      return VA_STATUS_SUCCESS;
   }

   context->mpeg4.frame_num++;
   context->decoder->end_frame(context->decoder, context->target, &context->desc.base);

   return VA_STATUS_SUCCESS;
}
