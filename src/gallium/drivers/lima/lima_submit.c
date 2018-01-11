/*
 * Copyright (C) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "lima_drm.h"

#include "lima_screen.h"
#include "lima_submit.h"
#include "lima_bo.h"
#include "lima_util.h"

#define VOID2U64(x) ((uint64_t)(unsigned long)(x))

struct lima_submit *lima_submit_create(struct lima_screen *screen, uint32_t pipe)
{
   struct lima_submit *s;

   s = calloc(1, sizeof(*s));
   if (!s)
      return NULL;

   s->screen = screen;
   s->pipe = pipe;
   return s;
}

void lima_submit_delete(struct lima_submit *submit)
{
   if (submit->bos)
      free(submit->bos);
   free(submit);
}

bool lima_submit_add_bo(struct lima_submit *submit, struct lima_bo *bo, uint32_t flags)
{
   uint32_t i, new_bos = 8;

   for (i = 0; i < submit->nr_bos; i++) {
      if (submit->bos[i] == bo)
         return true;
   }

   if (submit->bos && submit->max_bos == submit->nr_bos)
      new_bos = submit->max_bos * 2;

   if (new_bos > submit->max_bos) {
      void *bos = realloc(submit->bos,
         (sizeof(*submit->bos) + sizeof(*submit->gem_bos)) * new_bos);
      if (!bos)
         return false;
      submit->max_bos = new_bos;
      submit->bos = bos;
      submit->gem_bos = bos + sizeof(*submit->bos) * new_bos;
   }

   /* prevent bo from being freed when submit start */
   lima_bo_reference(bo);

   submit->bos[submit->nr_bos] = bo;
   submit->gem_bos[submit->nr_bos].handle = bo->handle;
   submit->gem_bos[submit->nr_bos].flags = flags;
   submit->nr_bos++;
   return true;
}

bool lima_submit_start(struct lima_submit *submit)
{
   struct drm_lima_gem_submit req = {
      .fence = 0,
      .pipe = submit->pipe,
      .nr_bos = submit->nr_bos,
      .bos = VOID2U64(submit->gem_bos),
      .frame = VOID2U64(submit->frame),
      .frame_size = submit->frame_size,
   };

   if (drmIoctl(submit->screen->fd, DRM_IOCTL_LIMA_GEM_SUBMIT, &req))
      return false;

   for (int i = 0; i < submit->nr_bos; i++)
      lima_bo_free(submit->bos[i]);

   submit->nr_bos = 0;
   submit->fence = req.fence;
   return true;
}

bool lima_submit_wait(struct lima_submit *submit, uint64_t timeout_ns, bool relative)
{
   struct drm_lima_wait_fence req = {
      .pipe = submit->pipe,
      .fence = submit->fence,
      .timeout_ns = timeout_ns,
   };

   if (lima_get_absolute_timeout(&req.timeout_ns, relative))
      return false;

   return drmIoctl(submit->screen->fd, DRM_IOCTL_LIMA_WAIT_FENCE, &req) == 0;
}
