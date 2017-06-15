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
#include "util/u_math.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_resource.h"

static void
lima_context_destroy(struct pipe_context *pctx)
{
   struct lima_context *ctx = lima_context(pctx);

   lima_state_fini(ctx);

   if (ctx->uploader)
      u_upload_destroy(ctx->uploader);

   slab_destroy_child(&ctx->transfer_pool);

   if (ctx->plb)
      lima_buffer_free(ctx->plb);

   if (ctx->gp_buffer)
      lima_buffer_free(ctx->gp_buffer);

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

   /* assume max fb size 4096x4096  */
   int block_size = 0x200;
   int max_plb = 512;
   int plb_size = block_size * max_plb;
   int plbu_size = max_plb * sizeof(uint32_t);
   int pp_end_mark = 2 * sizeof(uint32_t) * 4;
   /* max possible plb pp stream */
   int pp_size = (4096 >> 4) * (4096 >> 4) * 4 * sizeof(uint32_t) + pp_end_mark;

   ctx->plb = lima_buffer_alloc(screen, align(plb_size + plbu_size + pp_size, 0x1000),
                                LIMA_BUFFER_ALLOC_MAP | LIMA_BUFFER_ALLOC_VA);
   if (!ctx->plb)
      goto err_out;

   ctx->plb_plbu_offset = 0;
   ctx->plb_offset = ctx->plb_plbu_offset + plbu_size;

   /* plb address stream for pp depends on framebuffer dimension */
   int i;
   int per_pp_size = align(pp_size, 0x1000) / screen->info.num_pp;
   for (i = 0; i < screen->info.num_pp; i++)
      ctx->plb_pp_offset[i] = ctx->plb_offset + plb_size + i * per_pp_size;

   /* plb address stream for plbu is static for any framebuffer */
   for (i = 0; i < max_plb; i++)
      ((uint32_t *)(ctx->plb->map + ctx->plb_plbu_offset))[i] =
         ctx->plb->va + ctx->plb_offset + block_size * i;

   ctx->gp_buffer = lima_buffer_alloc(
      screen, gp_buffer_size, LIMA_BUFFER_ALLOC_MAP | LIMA_BUFFER_ALLOC_VA);
   if (!ctx->gp_buffer)
      goto err_out;

   return &ctx->base;

err_out:
   lima_context_destroy(&ctx->base);
   return NULL;
}
