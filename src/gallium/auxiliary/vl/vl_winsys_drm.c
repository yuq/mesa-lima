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
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <assert.h>

#include "pipe/p_screen.h"
#include "pipe-loader/pipe_loader.h"
#include "state_tracker/drm_driver.h"

#include "util/u_memory.h"
#include "vl/vl_winsys.h"

struct vl_screen*
vl_drm_screen_create(int fd)
{
   struct vl_screen *vscreen;

   vscreen = CALLOC_STRUCT(vl_screen);
   if (!vscreen)
      return NULL;

#if GALLIUM_STATIC_TARGETS
   vscreen->pscreen = dd_create_screen(fd);
#else
   if (pipe_loader_drm_probe_fd(&vscreen->dev, dup(fd))) {
      vscreen->pscreen =
         pipe_loader_create_screen(vscreen->dev, PIPE_SEARCH_DIR);
      if (!vscreen->pscreen)
         pipe_loader_release(&vscreen->dev, 1);
   }
#endif

   if (!vscreen->pscreen) {
      FREE(vscreen);
      return NULL;
   }

   return vscreen;
}

void
vl_drm_screen_destroy(struct vl_screen *vscreen)
{
   assert(vscreen);

   vscreen->pscreen->destroy(vscreen->pscreen);

#if !GALLIUM_STATIC_TARGETS
   pipe_loader_release(&vscreen->dev, 1);
#endif

   FREE(vscreen);
}
