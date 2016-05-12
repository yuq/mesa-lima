/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "brw_context.h"
#include "intel_batchbuffer.h"
#include "intel_fbo.h"
#include "brw_meta_util.h"

#include "main/blit.h"
#include "main/buffers.h"
#include "main/enums.h"
#include "main/fbobject.h"
#include "main/framebuffer.h"
#include "main/renderbuffer.h"

#include "drivers/common/meta.h"

/**
 * @file brw_meta_updownsample.c
 *
 * Implements upsampling and downsampling of miptrees for window system
 * framebuffers.
 */

/**
 * Implementation of up or downsampling for window-system MSAA miptrees.
 */
void
brw_meta_updownsample(struct brw_context *brw,
                      struct intel_mipmap_tree *src_mt,
                      struct intel_mipmap_tree *dst_mt)
{
   struct gl_context *ctx = &brw->ctx;
   struct gl_framebuffer *src_fb;
   struct gl_framebuffer *dst_fb;
   struct gl_renderbuffer *src_rb;
   struct gl_renderbuffer *dst_rb;
   GLenum drawbuffer;
   GLbitfield attachment, blit_bit;

   if (_mesa_get_format_base_format(src_mt->format) == GL_DEPTH_COMPONENT ||
       _mesa_get_format_base_format(src_mt->format) == GL_DEPTH_STENCIL) {
      attachment = GL_DEPTH_ATTACHMENT;
      drawbuffer = GL_NONE;
      blit_bit = GL_DEPTH_BUFFER_BIT;
   } else {
      attachment = GL_COLOR_ATTACHMENT0;
      drawbuffer = GL_COLOR_ATTACHMENT0;
      blit_bit = GL_COLOR_BUFFER_BIT;
   }

   brw_emit_mi_flush(brw);

   _mesa_meta_begin(ctx, MESA_META_ALL);
   src_rb = brw_get_rb_for_slice(brw, src_mt, 0, 0, false);
   dst_rb = brw_get_rb_for_slice(brw, dst_mt, 0, 0, false);
   src_fb = ctx->Driver.NewFramebuffer(ctx, 0xDEADBEEF);
   dst_fb = ctx->Driver.NewFramebuffer(ctx, 0xDEADBEEF);

   if (src_fb == NULL || dst_fb == NULL || src_rb == NULL || dst_rb == NULL) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "in %s", __func__);
      goto error;
   }

   _mesa_bind_framebuffers(ctx, dst_fb, src_fb);
   _mesa_framebuffer_renderbuffer(ctx, ctx->ReadBuffer, attachment, src_rb);
   _mesa_ReadBuffer(drawbuffer);

   _mesa_framebuffer_renderbuffer(ctx, ctx->DrawBuffer, attachment, dst_rb);
   _mesa_DrawBuffer(drawbuffer);

   _mesa_BlitFramebuffer(0, 0,
                         src_mt->logical_width0, src_mt->logical_height0,
                         0, 0,
                         dst_mt->logical_width0, dst_mt->logical_height0,
                         blit_bit, GL_NEAREST);

error:
   _mesa_reference_renderbuffer(&src_rb, NULL);
   _mesa_reference_renderbuffer(&dst_rb, NULL);
   _mesa_reference_framebuffer(&src_fb, NULL);
   _mesa_reference_framebuffer(&dst_fb, NULL);

   _mesa_meta_end(ctx);

   brw_emit_mi_flush(brw);
}
