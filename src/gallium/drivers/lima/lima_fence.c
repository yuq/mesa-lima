/*
 * Copyright (c) 2018 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include <libsync.h>

#include <util/u_memory.h>
#include <util/u_inlines.h>

#include "lima_drm.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_fence.h"
#include "lima_submit.h"

struct pipe_fence_handle {
   struct pipe_reference reference;
   struct lima_context *ctx;
   uint32_t seqno;
   int sync_fd;
};

static void
lima_create_fence_fd(struct pipe_context *pctx,
                     struct pipe_fence_handle **fence,
                     int fd, enum pipe_fd_type type)
{
   debug_printf("%s: fd=%d\n", __FUNCTION__, fd);
   assert(type == PIPE_FD_TYPE_NATIVE_SYNC);

   struct lima_context *ctx = lima_context(pctx);
   *fence = lima_fence_create(ctx, dup(fd));
}

static void
lima_fence_server_sync(struct pipe_context *pctx,
                       struct pipe_fence_handle *fence)
{
   debug_checkpoint();

   struct lima_context *ctx = lima_context(pctx);
   union drm_lima_gem_submit_dep dep;

   if (fence->sync_fd >= 0) {
      dep.type = LIMA_SUBMIT_DEP_SYNC_FD;
      dep.sync_fd.fd = fence->sync_fd;
      debug_printf("add sync fd dep %d\n", fence->sync_fd);
   }
   else {
      dep.type = LIMA_SUBMIT_DEP_FENCE;
      dep.fence.ctx = ctx->id;
      dep.fence.pipe = LIMA_PIPE_PP;
      dep.fence.seq = fence->seqno;
      debug_printf("add native fence %u\n", fence->seqno);
   }

   lima_submit_add_dep(ctx->gp_submit, &dep);
}

void lima_fence_context_init(struct lima_context *ctx)
{
   ctx->base.create_fence_fd = lima_create_fence_fd;
   ctx->base.fence_server_sync = lima_fence_server_sync;
}

struct pipe_fence_handle *
lima_fence_create(struct lima_context *ctx, int sync_fd)
{
   debug_printf("%s: sync_fd=%d\n", __FUNCTION__, sync_fd);

   struct pipe_fence_handle *fence;

   fence = CALLOC_STRUCT(pipe_fence_handle);
   if (!fence)
      return NULL;

   pipe_reference_init(&fence->reference, 1);
   fence->ctx = ctx;
   fence->sync_fd = sync_fd;
   if (sync_fd < 0) {
      if (!lima_submit_get_fence(ctx->pp_submit, &fence->seqno)) {
         FREE(fence);
         return NULL;
      }
   }

   return fence;
}

static int
lima_fence_get_fd(struct pipe_screen *pscreen,
                  struct pipe_fence_handle *fence)
{
   debug_checkpoint();

   assert(fence->sync_fd >= 0);
   return dup(fence->sync_fd);
}

static void
lima_fence_destroy(struct pipe_fence_handle *fence)
{
   if (fence->sync_fd >= 0)
      close(fence->sync_fd);
   FREE(fence);
}

static void
lima_fence_reference(struct pipe_screen *pscreen,
                     struct pipe_fence_handle **ptr,
                     struct pipe_fence_handle *fence)
{
   debug_checkpoint();

   if (pipe_reference(&(*ptr)->reference, &fence->reference))
      lima_fence_destroy(*ptr);
   *ptr = fence;
}

static boolean
lima_fence_finish(struct pipe_screen *pscreen, struct pipe_context *pctx,
                  struct pipe_fence_handle *fence, uint64_t timeout)
{
   debug_checkpoint();

   if (fence->sync_fd >= 0) {
      debug_printf("wait sync fd %d\n", fence->sync_fd);
      return !sync_wait(fence->sync_fd, timeout / 1000000);
   }

   debug_printf("wait native fence %u\n", fence->seqno);
   return lima_submit_wait_fence(fence->ctx->pp_submit, fence->seqno, timeout);
}

void
lima_fence_screen_init(struct lima_screen *screen)
{
   screen->base.fence_reference = lima_fence_reference;
   screen->base.fence_finish = lima_fence_finish;
   screen->base.fence_get_fd = lima_fence_get_fd;
}
