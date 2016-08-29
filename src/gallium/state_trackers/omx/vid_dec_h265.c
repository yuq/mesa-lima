/**************************************************************************
 *
 * Copyright 2016 Advanced Micro Devices, Inc.
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
#include "util/u_memory.h"
#include "util/u_video.h"
#include "vl/vl_rbsp.h"

#include "entrypoint.h"
#include "vid_dec.h"

#define DPB_MAX_SIZE 16
#define MAX_NUM_REF_PICS 16

enum {
   NAL_UNIT_TYPE_TRAIL_N = 0,
   NAL_UNIT_TYPE_TRAIL_R = 1,
   NAL_UNIT_TYPE_TSA_N = 2,
   NAL_UNIT_TYPE_TSA_R = 3,
   NAL_UNIT_TYPE_STSA_N = 4,
   NAL_UNIT_TYPE_STSA_R = 5,
   NAL_UNIT_TYPE_RADL_N = 6,
   NAL_UNIT_TYPE_RADL_R = 7,
   NAL_UNIT_TYPE_RASL_N = 8,
   NAL_UNIT_TYPE_RASL_R = 9,
   NAL_UNIT_TYPE_BLA_W_LP = 16,
   NAL_UNIT_TYPE_BLA_W_RADL = 17,
   NAL_UNIT_TYPE_BLA_N_LP =  18,
   NAL_UNIT_TYPE_IDR_W_RADL = 19,
   NAL_UNIT_TYPE_IDR_N_LP = 20,
   NAL_UNIT_TYPE_CRA = 21,
   NAL_UNIT_TYPE_SPS = 33,
   NAL_UNIT_TYPE_PPS = 34,
};

struct dpb_list {
   struct list_head list;
   struct pipe_video_buffer *buffer;
   unsigned poc;
};

struct ref_pic_set {
  unsigned  num_pics;
  unsigned  num_neg_pics;
  unsigned  num_pos_pics;
  unsigned  num_delta_poc;
  int  delta_poc[MAX_NUM_REF_PICS];
  bool used[MAX_NUM_REF_PICS];
};

/* random access point picture */
static bool is_rap_picture(unsigned nal_unit_type)
{
   return (nal_unit_type >= NAL_UNIT_TYPE_BLA_W_LP &&
           nal_unit_type <= NAL_UNIT_TYPE_CRA);
}

static bool is_slice_picture(unsigned nal_unit_type)
{
   return (nal_unit_type <= NAL_UNIT_TYPE_RASL_R ||
           is_rap_picture(nal_unit_type));
}

static void profile_tier(struct vl_rbsp *rbsp)
{
   int i;

   /* general_profile_space */
   vl_rbsp_u(rbsp, 2);

   /* general_tier_flag */
   vl_rbsp_u(rbsp, 1);

   /* general_profile_idc */
   vl_rbsp_u(rbsp, 5);

   /* general_profile_compatibility_flag */
   for(i = 0; i < 32; ++i)
      vl_rbsp_u(rbsp, 1);

   /* general_progressive_source_flag */
   vl_rbsp_u(rbsp, 1);

   /* general_interlaced_source_flag */
   vl_rbsp_u(rbsp, 1);

   /* general_non_packed_constraint_flag */
   vl_rbsp_u(rbsp, 1);

   /* general_frame_only_constraint_flag */
   vl_rbsp_u(rbsp, 1);

   /* general_reserved_zero_44bits */
   vl_rbsp_u(rbsp, 16);
   vl_rbsp_u(rbsp, 16);
   vl_rbsp_u(rbsp, 12);
}

static unsigned profile_tier_level(struct vl_rbsp *rbsp,
                                   int max_sublayers_minus1)
{
   bool sub_layer_profile_present_flag[6];
   bool sub_layer_level_present_flag[6];
   unsigned level_idc;
   int i;

   profile_tier(rbsp);

   /* general_level_idc */
   level_idc = vl_rbsp_u(rbsp, 8);

   for (i = 0; i < max_sublayers_minus1; ++i) {
      sub_layer_profile_present_flag[i] = vl_rbsp_u(rbsp, 1);
      sub_layer_level_present_flag[i] = vl_rbsp_u(rbsp, 1);
   }

   if (max_sublayers_minus1 > 0)
      for (i = max_sublayers_minus1; i < 8; ++i)
         /* reserved_zero_2bits */
         vl_rbsp_u(rbsp, 2);

   for (i = 0; i < max_sublayers_minus1; ++i) {
      if (sub_layer_profile_present_flag[i])
         profile_tier(rbsp);

      if (sub_layer_level_present_flag[i])
         /* sub_layer_level_idc */
         vl_rbsp_u(rbsp, 8);
   }

   return level_idc;
}

static void scaling_list_data(void)
{
   /* TODO */
   assert(0);
}

static void st_ref_pic_set(void)
{
   /* TODO */
}

static struct pipe_h265_sps *seq_parameter_set_id(vid_dec_PrivateType *priv,
                                                  struct vl_rbsp *rbsp)
{
   unsigned id = vl_rbsp_ue(rbsp);

   if (id >= ARRAY_SIZE(priv->codec_data.h265.sps))
      return NULL;

   return &priv->codec_data.h265.sps[id];
}

static void seq_parameter_set(vid_dec_PrivateType *priv, struct vl_rbsp *rbsp)
{
   struct pipe_h265_sps *sps;
   int sps_max_sub_layers_minus1;
   unsigned i;

   /* sps_video_parameter_set_id */
   vl_rbsp_u(rbsp, 4);

   /* sps_max_sub_layers_minus1 */
   sps_max_sub_layers_minus1 = vl_rbsp_u(rbsp, 3);

   assert(sps_max_sub_layers_minus1 <= 6);

   /* sps_temporal_id_nesting_flag */
   vl_rbsp_u(rbsp, 1);

   priv->codec_data.h265.level_idc =
      profile_tier_level(rbsp, sps_max_sub_layers_minus1);

   sps = seq_parameter_set_id(priv, rbsp);
   if (!sps)
      return;

   memset(sps, 0, sizeof(*sps));

   sps->chroma_format_idc = vl_rbsp_ue(rbsp);

   if (sps->chroma_format_idc == 3)
      sps->separate_colour_plane_flag = vl_rbsp_u(rbsp, 1);

   sps->pic_width_in_luma_samples = vl_rbsp_ue(rbsp);

   sps->pic_height_in_luma_samples = vl_rbsp_ue(rbsp);

   /* conformance_window_flag */
   if (vl_rbsp_u(rbsp, 1)) {
      /* conf_win_left_offset */
      vl_rbsp_ue(rbsp);
      /* conf_win_right_offset */
      vl_rbsp_ue(rbsp);
      /* conf_win_top_offset */
      vl_rbsp_ue(rbsp);
      /* conf_win_bottom_offset */
      vl_rbsp_ue(rbsp);
   }

   sps->bit_depth_luma_minus8 = vl_rbsp_ue(rbsp);
   sps->bit_depth_chroma_minus8 = vl_rbsp_ue(rbsp);
   sps->log2_max_pic_order_cnt_lsb_minus4 = vl_rbsp_ue(rbsp);

   /* sps_sub_layer_ordering_info_present_flag */
   i  = vl_rbsp_u(rbsp, 1) ? 0 : sps_max_sub_layers_minus1;
   for (; i <= sps_max_sub_layers_minus1; ++i) {
      sps->sps_max_dec_pic_buffering_minus1 = vl_rbsp_ue(rbsp);
      /* sps_max_num_reorder_pics */
      vl_rbsp_ue(rbsp);
      /* sps_max_latency_increase_plus */
      vl_rbsp_ue(rbsp);
   }

   sps->log2_min_luma_coding_block_size_minus3 = vl_rbsp_ue(rbsp);
   sps->log2_diff_max_min_luma_coding_block_size = vl_rbsp_ue(rbsp);
   sps->log2_min_transform_block_size_minus2 = vl_rbsp_ue(rbsp);
   sps->log2_diff_max_min_transform_block_size = vl_rbsp_ue(rbsp);
   sps->max_transform_hierarchy_depth_inter = vl_rbsp_ue(rbsp);
   sps->max_transform_hierarchy_depth_intra = vl_rbsp_ue(rbsp);

   sps->scaling_list_enabled_flag = vl_rbsp_u(rbsp, 1);
   if (sps->scaling_list_enabled_flag)
      /* sps_scaling_list_data_present_flag */
      if (vl_rbsp_u(rbsp, 1))
         scaling_list_data();

   sps->amp_enabled_flag = vl_rbsp_u(rbsp, 1);
   sps->sample_adaptive_offset_enabled_flag = vl_rbsp_u(rbsp, 1);
   sps->pcm_enabled_flag = vl_rbsp_u(rbsp, 1);
   if (sps->pcm_enabled_flag) {
      sps->pcm_sample_bit_depth_luma_minus1 = vl_rbsp_u(rbsp, 4);
      sps->pcm_sample_bit_depth_chroma_minus1 = vl_rbsp_u(rbsp, 4);
      sps->log2_min_pcm_luma_coding_block_size_minus3 = vl_rbsp_ue(rbsp);
      sps->log2_diff_max_min_pcm_luma_coding_block_size = vl_rbsp_ue(rbsp);
      sps->pcm_loop_filter_disabled_flag = vl_rbsp_u(rbsp, 1);
   }

   sps->num_short_term_ref_pic_sets = vl_rbsp_ue(rbsp);

   for (i = 0; i < sps->num_short_term_ref_pic_sets; ++i) {
      struct ref_pic_set *rps;

      rps = (struct ref_pic_set *)
         priv->codec_data.h265.ref_pic_set_list + i;
      st_ref_pic_set();
   }

   sps->long_term_ref_pics_present_flag = vl_rbsp_u(rbsp, 1);
   if (sps->long_term_ref_pics_present_flag) {
      sps->num_long_term_ref_pics_sps = vl_rbsp_ue(rbsp);
      for (i = 0; i < sps->num_long_term_ref_pics_sps; ++i) {
         /* lt_ref_pic_poc_lsb_sps */
         vl_rbsp_u(rbsp, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
         /* used_by_curr_pic_lt_sps_flag */
         vl_rbsp_u(rbsp, 1);
      }
   }

   sps->sps_temporal_mvp_enabled_flag = vl_rbsp_u(rbsp, 1);
   sps->strong_intra_smoothing_enabled_flag = vl_rbsp_u(rbsp, 1);
}

static struct pipe_h265_pps *pic_parameter_set_id(vid_dec_PrivateType *priv,
                                                  struct vl_rbsp *rbsp)
{
   unsigned id = vl_rbsp_ue(rbsp);

   if (id >= ARRAY_SIZE(priv->codec_data.h265.pps))
      return NULL;

   return &priv->codec_data.h265.pps[id];
}

static void picture_parameter_set(vid_dec_PrivateType *priv,
                                  struct vl_rbsp *rbsp)
{
   struct pipe_h265_sps *sps;
   struct pipe_h265_pps *pps;
   int i;

   pps = pic_parameter_set_id(priv, rbsp);
   if (!pps)
      return;

   memset(pps, 0, sizeof(*pps));
   sps = pps->sps = seq_parameter_set_id(priv, rbsp);
   if (!sps)
      return;

   pps->dependent_slice_segments_enabled_flag = vl_rbsp_u(rbsp, 1);
   pps->output_flag_present_flag = vl_rbsp_u(rbsp, 1);
   pps->num_extra_slice_header_bits = vl_rbsp_u(rbsp, 3);
   pps->sign_data_hiding_enabled_flag = vl_rbsp_u(rbsp, 1);
   pps->cabac_init_present_flag = vl_rbsp_u(rbsp, 1);

   pps->num_ref_idx_l0_default_active_minus1 = vl_rbsp_ue(rbsp);
   pps->num_ref_idx_l1_default_active_minus1 = vl_rbsp_ue(rbsp);
   pps->init_qp_minus26 = vl_rbsp_se(rbsp);
   pps->constrained_intra_pred_flag = vl_rbsp_u(rbsp, 1);
   pps->transform_skip_enabled_flag = vl_rbsp_u(rbsp, 1);

   pps->cu_qp_delta_enabled_flag = vl_rbsp_u(rbsp, 1);
   if (pps->cu_qp_delta_enabled_flag)
      pps->diff_cu_qp_delta_depth = vl_rbsp_ue(rbsp);

   pps->pps_cb_qp_offset = vl_rbsp_se(rbsp);
   pps->pps_cr_qp_offset = vl_rbsp_se(rbsp);
   pps->pps_slice_chroma_qp_offsets_present_flag = vl_rbsp_u(rbsp, 1);

   pps->weighted_pred_flag = vl_rbsp_u(rbsp, 1);
   pps->weighted_bipred_flag = vl_rbsp_u(rbsp, 1);

   pps->transquant_bypass_enabled_flag = vl_rbsp_u(rbsp, 1);
   pps->tiles_enabled_flag = vl_rbsp_u(rbsp, 1);
   pps->entropy_coding_sync_enabled_flag = vl_rbsp_u(rbsp, 1);

   if (pps->tiles_enabled_flag) {
      pps->num_tile_columns_minus1 = vl_rbsp_ue(rbsp);
      pps->num_tile_rows_minus1 = vl_rbsp_ue(rbsp);

      pps->uniform_spacing_flag = vl_rbsp_u(rbsp, 1);
      if (!pps->uniform_spacing_flag) {
         for (i = 0; i < pps->num_tile_columns_minus1; ++i)
            pps->column_width_minus1[i] = vl_rbsp_ue(rbsp);

         for (i = 0; i < pps->num_tile_rows_minus1; ++i)
            pps->row_height_minus1[i] = vl_rbsp_ue(rbsp);
      }

      if (!pps->num_tile_columns_minus1 || !pps->num_tile_rows_minus1)
         pps->loop_filter_across_tiles_enabled_flag = vl_rbsp_u(rbsp, 1);
   }

   pps->pps_loop_filter_across_slices_enabled_flag = vl_rbsp_u(rbsp, 1);

   pps->deblocking_filter_control_present_flag = vl_rbsp_u(rbsp, 1);
   if (pps->deblocking_filter_control_present_flag) {
      pps->deblocking_filter_override_enabled_flag = vl_rbsp_u(rbsp, 1);
      pps->pps_deblocking_filter_disabled_flag = vl_rbsp_u(rbsp, 1);
      if (!pps->pps_deblocking_filter_disabled_flag) {
         pps->pps_beta_offset_div2 = vl_rbsp_se(rbsp);
         pps->pps_tc_offset_div2 = vl_rbsp_se(rbsp);
      }
   }

   /* pps_scaling_list_data_present_flag */
   if (vl_rbsp_u(rbsp, 1))
      scaling_list_data();

   pps->lists_modification_present_flag = vl_rbsp_u(rbsp, 1);
   pps->log2_parallel_merge_level_minus2 = vl_rbsp_ue(rbsp);
   pps->slice_segment_header_extension_present_flag = vl_rbsp_u(rbsp, 1);
}

static void vid_dec_h265_BeginFrame(vid_dec_PrivateType *priv)
{
   if (priv->frame_started)
      return;

   vid_dec_NeedTarget(priv);

   if (!priv->codec) {
      struct pipe_video_codec templat = {};
      omx_base_video_PortType *port;

      port = (omx_base_video_PortType *)
         priv->ports[OMX_BASE_FILTER_INPUTPORT_INDEX];
      templat.profile = priv->profile;
      templat.entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
      templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
      templat.expect_chunked_decode = true;
      templat.width = align(port->sPortParam.format.video.nFrameWidth, 4);
      templat.height = align(port->sPortParam.format.video.nFrameHeight, 4);
      templat.level =  priv->codec_data.h265.level_idc;
      priv->codec = priv->pipe->create_video_codec(priv->pipe, &templat);
   }
   priv->codec->begin_frame(priv->codec, priv->target, &priv->picture.base);
   priv->frame_started = true;
}

static struct pipe_video_buffer *vid_dec_h265_Flush(vid_dec_PrivateType *priv,
                                                    OMX_TICKS *timestamp)
{
   struct dpb_list *entry, *result = NULL;
   struct pipe_video_buffer *buf;

   /* search for the lowest poc and break on zeros */
   LIST_FOR_EACH_ENTRY(entry, &priv->codec_data.h265.dpb_list, list) {

      if (result && entry->poc == 0)
         break;

      if (!result || entry->poc < result->poc)
         result = entry;
   }

   if (!result)
      return NULL;

   buf = result->buffer;

   --priv->codec_data.h265.dpb_num;
   LIST_DEL(&result->list);
   FREE(result);

   return buf;
}

static void vid_dec_h265_EndFrame(vid_dec_PrivateType *priv)
{
   struct dpb_list *entry = NULL;
   struct pipe_video_buffer *tmp;

   if (!priv->frame_started)
      return;

   priv->codec->end_frame(priv->codec, priv->target, &priv->picture.base);
   priv->frame_started = false;

   /* add the decoded picture to the dpb list */
   entry = CALLOC_STRUCT(dpb_list);
   if (!entry)
      return;

   entry->buffer = priv->target;

   LIST_ADDTAIL(&entry->list, &priv->codec_data.h265.dpb_list);
   ++priv->codec_data.h265.dpb_num;
   priv->target = NULL;

   if (priv->codec_data.h265.dpb_num <= DPB_MAX_SIZE)
      return;

   tmp = priv->in_buffers[0]->pInputPortPrivate;
   priv->in_buffers[0]->pInputPortPrivate = vid_dec_h265_Flush(priv, NULL);
   priv->target = tmp;
   priv->frame_finished = priv->in_buffers[0]->pInputPortPrivate != NULL;
}

static void vid_dec_h265_Decode(vid_dec_PrivateType *priv,
                                struct vl_vlc *vlc,
                                unsigned min_bits_left)
{
   unsigned nal_unit_type;
   unsigned nuh_layer_id;
   unsigned nuh_temporal_id_plus1;

   if (!vl_vlc_search_byte(vlc, vl_vlc_bits_left(vlc) - min_bits_left, 0x00))
      return;

   if (vl_vlc_peekbits(vlc, 24) != 0x000001) {
      vl_vlc_eatbits(vlc, 8);
      return;
   }

   if (priv->slice) {
      unsigned bytes = priv->bytes_left - (vl_vlc_bits_left(vlc) / 8);

      priv->codec->decode_bitstream(priv->codec, priv->target,
                                    &priv->picture.base, 1,
                                    &priv->slice, &bytes);
      priv->slice = NULL;
   }

   vl_vlc_eatbits(vlc, 24);

   /* forbidden_zero_bit */
   vl_vlc_eatbits(vlc, 1);

   if (vl_vlc_valid_bits(vlc) < 15)
      vl_vlc_fillbits(vlc);

   nal_unit_type = vl_vlc_get_uimsbf(vlc, 6);

   /* nuh_layer_id */
   nuh_layer_id = vl_vlc_get_uimsbf(vlc, 6);

   /* nuh_temporal_id_plus1 */
   nuh_temporal_id_plus1 = vl_vlc_get_uimsbf(vlc, 3);
   priv->codec_data.h265.temporal_id = nuh_temporal_id_plus1 - 1;

   if (!is_slice_picture(nal_unit_type))
      vid_dec_h265_EndFrame(priv);

   if (nal_unit_type == NAL_UNIT_TYPE_SPS) {
      struct vl_rbsp rbsp;

      vl_rbsp_init(&rbsp, vlc, ~0);
      seq_parameter_set(priv, &rbsp);

   } else if (nal_unit_type == NAL_UNIT_TYPE_PPS) {
      struct vl_rbsp rbsp;

      vl_rbsp_init(&rbsp, vlc, ~0);
      picture_parameter_set(priv, &rbsp);

   }

   /* TODO */

   /* resync to byte boundary */
   vl_vlc_eatbits(vlc, vl_vlc_valid_bits(vlc) % 8);
}

void vid_dec_h265_Init(vid_dec_PrivateType *priv)
{
   priv->picture.base.profile = PIPE_VIDEO_PROFILE_HEVC_MAIN;

   LIST_INITHEAD(&priv->codec_data.h265.dpb_list);

   priv->Decode = vid_dec_h265_Decode;
   priv->EndFrame = vid_dec_h265_EndFrame;
   priv->Flush = vid_dec_h265_Flush;
}
