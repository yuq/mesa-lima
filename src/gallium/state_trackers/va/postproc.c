/**************************************************************************
 *
 * Copyright 2015 Advanced Micro Devices, Inc.
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

//#include "pipe/p_video_codec.h"

#include "util/u_handle_table.h"
//#include "util/u_video.h"

//#include "vl/vl_vlc.h"
//#include "vl/vl_winsys.h"

#include "va_private.h"

VAStatus
vlVaHandleVAProcPipelineParameterBufferType(vlVaDriver *drv, vlVaContext *context, vlVaBuffer *buf)
{
   struct u_rect src_rect;
   struct u_rect dst_rect;
   vlVaSurface *src_surface;
   VAProcPipelineParameterBuffer *pipeline_param;
   struct pipe_surface **surfaces;
   struct pipe_screen *screen;
   struct pipe_surface *psurf;

   if (!drv || !context)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   if (!buf || !buf->data)
      return VA_STATUS_ERROR_INVALID_BUFFER;

   if (!context->target)
      return VA_STATUS_ERROR_INVALID_SURFACE;

   pipeline_param = (VAProcPipelineParameterBuffer *)buf->data;

   src_surface = handle_table_get(drv->htab, pipeline_param->surface);
   if (!src_surface || !src_surface->buffer)
      return VA_STATUS_ERROR_INVALID_SURFACE;

   surfaces = context->target->get_surfaces(context->target);

   if (!surfaces || !surfaces[0])
      return VA_STATUS_ERROR_INVALID_SURFACE;

   screen = drv->pipe->screen;

   psurf = surfaces[0];

   src_rect.x0 = pipeline_param->surface_region->x;
   src_rect.y0 = pipeline_param->surface_region->y;
   src_rect.x1 = pipeline_param->surface_region->x + pipeline_param->surface_region->width;
   src_rect.y1 = pipeline_param->surface_region->y + pipeline_param->surface_region->height;

   dst_rect.x0 = pipeline_param->output_region->x;
   dst_rect.y0 = pipeline_param->output_region->y;
   dst_rect.x1 = pipeline_param->output_region->x + pipeline_param->output_region->width;
   dst_rect.y1 = pipeline_param->output_region->y + pipeline_param->output_region->height;

   vl_compositor_clear_layers(&drv->cstate);
   vl_compositor_set_buffer_layer(&drv->cstate, &drv->compositor, 0, src_surface->buffer, &src_rect, NULL, VL_COMPOSITOR_WEAVE);
   vl_compositor_set_layer_dst_area(&drv->cstate, 0, &dst_rect);
   vl_compositor_render(&drv->cstate, &drv->compositor, psurf, NULL, false);

   screen->fence_reference(screen, &src_surface->fence, NULL);
   drv->pipe->flush(drv->pipe, &src_surface->fence, 0);

   return VA_STATUS_SUCCESS;
}


