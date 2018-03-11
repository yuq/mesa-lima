/*
 * Copyright (c) 2017 Lima Project
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <fcntl.h>
#include <unistd.h>

#include "rockchip_drm_public.h"
#include "lima/drm/lima_drm_public.h"
#include "xf86drm.h"

#include "pipe/p_screen.h"
#include "renderonly/renderonly.h"

#if defined(GALLIUM_LIMA)
static struct pipe_screen *rockchip_screen_create_lima(int fd)
{
   struct renderonly ro = {
      /* Passes the lima-allocated BO through to the rockchip DRM device using
       * PRIME buffer sharing.  The lima BO must be linear, which the SCANOUT
       * flag on allocation will have ensured.
       */
      .create_for_resource = renderonly_create_kms_dumb_buffer_for_resource,
      .kms_fd = fd,
      .gpu_fd = drmOpenWithType("lima", NULL, DRM_NODE_RENDER),
   };

   if (ro.gpu_fd < 0)
      return NULL;

   struct pipe_screen *screen = lima_drm_screen_create_renderonly(&ro);
   if (!screen)
      close(ro.gpu_fd);

   return screen;
}
#else
static struct pipe_screen *rockchip_screen_create_lima(int fd)
{
   return NULL;
}
#endif

/*
 * Rockchip SoCs use a plethora of 3D GPUs combined with the same
 * KMS device. Malis from the Utgard and Midgard branches as well
 * as PowerVR and Vivante cores.
 * So to we try to find a suitable GPU by trying to talk to each
 * of them one after the other.
 */
struct pipe_screen *rockchip_screen_create(int fd)
{
   struct pipe_screen *screen;

   screen = rockchip_screen_create_lima(fd);
   if (screen)
      return screen;

   return NULL;
}
