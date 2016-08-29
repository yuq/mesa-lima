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

struct dpb_list {
   struct list_head list;
   struct pipe_video_buffer *buffer;
   unsigned poc;
};

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
