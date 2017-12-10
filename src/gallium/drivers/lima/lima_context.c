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
#include "util/u_debug.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_resource.h"

#include <lima_drm.h>

static void
lima_context_destroy(struct pipe_context *pctx)
{
   struct lima_context *ctx = lima_context(pctx);

   lima_state_fini(ctx);

   if (ctx->uploader)
      u_upload_destroy(ctx->uploader);

   slab_destroy_child(&ctx->transfer_pool);

   if (ctx->gp_submit)
      lima_submit_delete(ctx->gp_submit);

   if (ctx->pp_submit)
      lima_submit_delete(ctx->pp_submit);

   if (ctx->share_buffer)
      lima_buffer_free(ctx->share_buffer);

   if (ctx->gp_buffer)
      lima_buffer_free(ctx->gp_buffer);

   if (ctx->pp_buffer)
      lima_buffer_free(ctx->pp_buffer);

   FREE(ctx);
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

   lima_resource_context_init(ctx);
   lima_state_init(ctx);
   lima_draw_init(ctx);
   lima_program_init(ctx);
   lima_query_init(ctx);

   ctx->uploader = u_upload_create_default(&ctx->base);
   ctx->base.stream_uploader = ctx->uploader;
   ctx->base.const_uploader = ctx->uploader;

   slab_create_child(&ctx->transfer_pool, &screen->transfer_pool);

   ctx->share_buffer =
      lima_buffer_alloc(screen, sh_buffer_size, LIMA_BUFFER_ALLOC_VA);
   if (!ctx->share_buffer)
      goto err_out;

   ctx->gp_buffer = lima_buffer_alloc(
      screen, gp_buffer_size, LIMA_BUFFER_ALLOC_MAP | LIMA_BUFFER_ALLOC_VA);
   if (!ctx->gp_buffer)
      goto err_out;

   /* plb address stream for plbu is static for any framebuffer */
   int max_plb = 512, block_size = 0x200;
   uint32_t *plbu_stream = ctx->gp_buffer->map + gp_plbu_plb_offset;
   for (int i = 0; i < max_plb; i++)
      plbu_stream[i] = ctx->share_buffer->va + sh_plb_offset + block_size * i;

   if (lima_submit_create(screen->dev, LIMA_PIPE_GP, &ctx->gp_submit))
      goto err_out;

   if (lima_submit_add_bo(ctx->gp_submit, ctx->share_buffer->bo,
                          LIMA_SUBMIT_BO_FLAG_WRITE))
      goto err_out;

   if (lima_submit_add_bo(ctx->gp_submit, ctx->gp_buffer->bo,
                          LIMA_SUBMIT_BO_FLAG_READ|LIMA_SUBMIT_BO_FLAG_WRITE))
      goto err_out;

   ctx->pp_buffer = lima_buffer_alloc(
      screen, pp_buffer_size, LIMA_BUFFER_ALLOC_MAP | LIMA_BUFFER_ALLOC_VA);
   if (!ctx->pp_buffer)
      goto err_out;

   /* fs program for clear buffer? */
   static uint32_t pp_program[] = {
      0x00020425, 0x0000000c, 0x01e007cf, 0xb0000000, /* 0x00000000 */
      0x000005f5, 0x00000000, 0x00000000, 0x00000000, /* 0x00000010 */
   };
   memcpy(ctx->pp_buffer->map + pp_clear_program_offset, pp_program, sizeof(pp_program));

   /* is pp frame render state static? */
   uint32_t *pp_frame_rsw = ctx->pp_buffer->map + pp_frame_rsw_offset;
   memset(pp_frame_rsw, 0, 0x40);
   pp_frame_rsw[8] = 0x0000f008;
   pp_frame_rsw[9] = ctx->pp_buffer->va + pp_clear_program_offset;
   pp_frame_rsw[13] = 0x00000100;

   if (lima_submit_create(screen->dev, LIMA_PIPE_PP, &ctx->pp_submit))
      goto err_out;

   if (lima_submit_add_bo(ctx->pp_submit, ctx->share_buffer->bo,
                          LIMA_SUBMIT_BO_FLAG_READ))
      goto err_out;

   if (lima_submit_add_bo(ctx->pp_submit, ctx->pp_buffer->bo,
                          LIMA_SUBMIT_BO_FLAG_READ | LIMA_SUBMIT_BO_FLAG_WRITE))
      goto err_out;

   return &ctx->base;

err_out:
   lima_context_destroy(&ctx->base);
   return NULL;
}
