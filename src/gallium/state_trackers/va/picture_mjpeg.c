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

#include "va_private.h"

void vlVaHandlePictureParameterBufferMJPEG(vlVaDriver *drv, vlVaContext *context, vlVaBuffer *buf)
{
   VAPictureParameterBufferJPEGBaseline *mjpeg = buf->data;
   int i;

   assert(buf->size >= sizeof(VAPictureParameterBufferJPEGBaseline) && buf->num_elements == 1);

   context->desc.mjpeg.picture_parameter.picture_width = mjpeg->picture_width;
   context->desc.mjpeg.picture_parameter.picture_height = mjpeg->picture_height;

   for (i = 0; i < mjpeg->num_components; ++i) {
      context->desc.mjpeg.picture_parameter.components[i].component_id =
         mjpeg->components[i].component_id;
      context->desc.mjpeg.picture_parameter.components[i].h_sampling_factor =
         mjpeg->components[i].h_sampling_factor;
      context->desc.mjpeg.picture_parameter.components[i].v_sampling_factor =
         mjpeg->components[i].v_sampling_factor;
      context->desc.mjpeg.picture_parameter.components[i].quantiser_table_selector =
         mjpeg->components[i].quantiser_table_selector;
   }

   context->desc.mjpeg.picture_parameter.num_components = mjpeg->num_components;
}

void vlVaHandleIQMatrixBufferMJPEG(vlVaContext *context, vlVaBuffer *buf)
{
   VAIQMatrixBufferJPEGBaseline *mjpeg = buf->data;

   assert(buf->size >= sizeof(VAIQMatrixBufferJPEGBaseline) && buf->num_elements == 1);

   memcpy(&context->desc.mjpeg.quantization_table.load_quantiser_table, mjpeg->load_quantiser_table, 4);
   memcpy(&context->desc.mjpeg.quantization_table.quantiser_table, mjpeg->quantiser_table, 4 * 64);
}

void vlVaHandleHuffmanTableBufferType(vlVaContext *context, vlVaBuffer *buf)
{
   /* TODO */
}

void vlVaHandleSliceParameterBufferMJPEG(vlVaContext *context, vlVaBuffer *buf)
{
   /* TODO */
}
