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

#include "lima_context.h"

union lima_float_uint_type {
   uint32_t u;
   float f;
};

static void
lima_clear(struct pipe_context *pctx, unsigned buffers,
           const union pipe_color_union *color, double depth, unsigned stencil)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);
   struct lima_context_clear *clear = &ctx->clear;

   clear->buffers = buffers;

   if (buffers & PIPE_CLEAR_COLOR0) {
      clear->color[0] = color->ui[0];
      clear->color[1] = color->ui[1];
      clear->color[2] = color->ui[2];
      clear->color[3] = color->ui[3];
   }

   if (buffers & PIPE_CLEAR_DEPTH) {
      union lima_float_uint_type d;
      d.f = depth;
      clear->depth = d.u;
   }

   if (buffers & PIPE_CLEAR_STENCIL)
      clear->stencil = stencil;

   ctx->dirty |= LIMA_CONTEXT_DIRTY_CLEAR;
}

static void
lima_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info)
{
   printf("dummy %s\n", __func__);
}

void
lima_draw_init(struct lima_context *ctx)
{
   ctx->base.clear = lima_clear;
   ctx->base.draw_vbo = lima_draw_vbo;
}
