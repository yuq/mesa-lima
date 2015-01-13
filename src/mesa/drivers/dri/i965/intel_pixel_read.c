/**************************************************************************
 *
 * Copyright 2003 VMware, Inc.
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
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "main/glheader.h"
#include "main/enums.h"
#include "main/mtypes.h"
#include "main/macros.h"
#include "main/fbobject.h"
#include "main/image.h"
#include "main/bufferobj.h"
#include "main/readpix.h"
#include "main/state.h"
#include "main/glformats.h"
#include "drivers/common/meta.h"

#include "brw_context.h"
#include "intel_screen.h"
#include "intel_blit.h"
#include "intel_buffers.h"
#include "intel_fbo.h"
#include "intel_mipmap_tree.h"
#include "intel_pixel.h"
#include "intel_buffer_objects.h"

#define FILE_DEBUG_FLAG DEBUG_PIXEL

void
intelReadPixels(struct gl_context * ctx,
                GLint x, GLint y, GLsizei width, GLsizei height,
                GLenum format, GLenum type,
                const struct gl_pixelstore_attrib *pack, GLvoid * pixels)
{
   struct brw_context *brw = brw_context(ctx);
   bool dirty;

   DBG("%s\n", __FUNCTION__);

   if (_mesa_is_bufferobj(pack->BufferObj)) {
      if (_mesa_meta_pbo_GetTexSubImage(ctx, 2, NULL, x, y, 0, width, height, 1,
                                        format, type, pixels, pack))
         return;

      perf_debug("%s: fallback to CPU mapping in PBO case\n", __FUNCTION__);
   }

   /* glReadPixels() wont dirty the front buffer, so reset the dirty
    * flag after calling intel_prepare_render(). */
   dirty = brw->front_buffer_dirty;
   intel_prepare_render(brw);
   brw->front_buffer_dirty = dirty;

   /* Update Mesa state before calling _mesa_readpixels().
    * XXX this may not be needed since ReadPixels no longer uses the
    * span code.
    */

   if (ctx->NewState)
      _mesa_update_state(ctx);

   _mesa_readpixels(ctx, x, y, width, height, format, type, pack, pixels);

   /* There's an intel_prepare_render() call in intelSpanRenderStart(). */
   brw->front_buffer_dirty = dirty;
}
