/*
 * Copyright (c) 2017 Lima Project
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

#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "util/u_inlines.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_resource.h"

static void
lima_context_destroy(struct pipe_context *pctx)
{
   struct lima_context *ctx = lima_context(pctx);

   lima_state_fini(ctx);

   pipe_surface_reference(&ctx->framebuffer.cbuf, NULL);
   pipe_surface_reference(&ctx->framebuffer.zsbuf, NULL);

   if (ctx->uploader)
      u_upload_destroy(ctx->uploader);

   slab_destroy_child(&ctx->transfer_pool);

   FREE(ctx);
}

static void
lima_pipe_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence,
                unsigned flags)
{
   printf("dummy %s\n", __func__);
}

struct pipe_context *
lima_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct lima_screen *screen = lima_screen(pscreen);
   struct lima_context *ctx;

   ctx = CALLOC_STRUCT(lima_context);
   if (!ctx)
      return NULL;

   ctx->base.screen = pscreen;
   ctx->base.destroy = lima_context_destroy;
   ctx->base.flush = lima_pipe_flush;

   lima_resource_context_init(ctx);
   lima_state_init(ctx);
   lima_draw_init(ctx);
   lima_program_init(ctx);

   ctx->uploader = u_upload_create_default(&ctx->base);
   ctx->base.stream_uploader = ctx->uploader;
   ctx->base.const_uploader = ctx->uploader;

   slab_create_child(&ctx->transfer_pool, &screen->transfer_pool);

   return &ctx->base;
}
