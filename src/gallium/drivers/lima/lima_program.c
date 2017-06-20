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
#include "tgsi/tgsi_dump.h"

#include "pipe/p_state.h"

#include "lima_context.h"

static void *
lima_create_fs_state(struct pipe_context *pctx,
                     const struct pipe_shader_state *cso)
{
   struct lima_fs_shader_state *so = CALLOC_STRUCT(lima_fs_shader_state);

   if (!so)
      return NULL;

   printf("dummy %s\n", __func__);

   assert(cso->type == PIPE_SHADER_IR_TGSI);
   tgsi_dump(cso->tokens, 0);

   static uint32_t fs[] = {
      0x00021025, 0x0000014c, 0x03c007cf, 0x00000000, /* 0x00000000 */
   };

   so->shader = fs;
   so->shader_size = sizeof(fs);
   so->first_inst_size = 5;

   return so;
}

static void
lima_bind_fs_state(struct pipe_context *pctx, void *hwcso)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   ctx->fs = hwcso;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_SHADER_FRAG;
}

static void
lima_delete_fs_state(struct pipe_context *pctx, void *hwcso)
{
   struct lima_fs_shader_state *so = hwcso;

   FREE(so);
}

static void *
lima_create_vs_state(struct pipe_context *pctx,
                     const struct pipe_shader_state *cso)
{
   struct lima_vs_shader_state *so = CALLOC_STRUCT(lima_vs_shader_state);

   if (!so)
      return NULL;

   printf("dummy %s\n", __func__);

   assert(cso->type == PIPE_SHADER_IR_TGSI);
   tgsi_dump(cso->tokens, 0);

   static uint32_t vs[] = {
      0xad4ad6b5, 0x0380a2cc, 0x0007ff80, 0x000ad500, /* 0x00000000 */
      0xad4685c2, 0x438002b5, 0x0007ff80, 0x000ad500, /* 0x00000010 */
      0xad4cc980, 0x438022d9, 0x0007ff80, 0x000ad500, /* 0x00000020 */
      0xad48ca3b, 0x038041d3, 0x0007ff80, 0x000ad500, /* 0x00000030 */
      0x6c8b66b5, 0x03804193, 0x4243c080, 0x000ac508, /* 0x00000040 */
   };

   so->shader = vs;
   so->shader_size = sizeof(vs);

   return so;
}

static void
lima_bind_vs_state(struct pipe_context *pctx, void *hwcso)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   ctx->vs = hwcso;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_SHADER_VERT;
}

static void
lima_delete_vs_state(struct pipe_context *pctx, void *hwcso)
{
   struct lima_vs_shader_state *so = hwcso;

   FREE(so);
}

void
lima_program_init(struct lima_context *ctx)
{
   ctx->base.create_fs_state = lima_create_fs_state;
   ctx->base.bind_fs_state = lima_bind_fs_state;
   ctx->base.delete_fs_state = lima_delete_fs_state;

   ctx->base.create_vs_state = lima_create_vs_state;
   ctx->base.bind_vs_state = lima_bind_vs_state;
   ctx->base.delete_vs_state = lima_delete_vs_state;
}
