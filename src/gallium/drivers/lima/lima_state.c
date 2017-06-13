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
#include "util/u_inlines.h"
#include "util/u_helpers.h"

#include "pipe/p_state.h"

#include "lima_context.h"

static void
lima_set_framebuffer_state(struct pipe_context *pctx,
                           const struct pipe_framebuffer_state *framebuffer)
{
   printf("dummy %s\n", __func__);

   printf("%s: psurf color=%p z=%p\n", __func__,
          framebuffer->cbufs[0], framebuffer->zsbuf);

   struct lima_context *ctx = lima_context(pctx);
   struct lima_context_framebuffer *fb = &ctx->framebuffer;

   if (framebuffer->nr_cbufs > 0)
      pipe_surface_reference(&fb->cbuf, framebuffer->cbufs[0]);
   else
      pipe_surface_reference(&fb->cbuf, NULL);

   pipe_surface_reference(&fb->zsbuf, framebuffer->zsbuf);

   ctx->dirty |= LIMA_CONTEXT_DIRTY_FRAMEBUFFER;
}

static void
lima_set_polygon_stipple(struct pipe_context *pctx,
                         const struct pipe_poly_stipple *stipple)
{
   printf("dummy %s\n", __func__);
}

static void *
lima_create_depth_stencil_alpha_state(struct pipe_context *pctx,
                                      const struct pipe_depth_stencil_alpha_state *cso)
{
   struct lima_depth_stencil_alpha_state *so;

   so = CALLOC_STRUCT(lima_depth_stencil_alpha_state);
   if (!so)
      return NULL;

   printf("dummy %s\n", __func__);

   return so;
}

static void
lima_bind_depth_stencil_alpha_state(struct pipe_context *pctx, void *hwcso)
{
   printf("dummy %s\n", __func__);
}

static void
lima_delete_depth_stencil_alpha_state(struct pipe_context *pctx, void *hwcso)
{
   FREE(hwcso);
}

static void *
lima_create_rasterizer_state(struct pipe_context *pctx,
                             const struct pipe_rasterizer_state *cso)
{
   struct lima_rasterizer_state *so;

   so = CALLOC_STRUCT(lima_rasterizer_state);
   if (!so)
      return NULL;

   printf("dummy %s\n", __func__);

   return so;
}

static void
lima_bind_rasterizer_state(struct pipe_context *pctx, void *hwcso)
{
   printf("dummy %s\n", __func__);
}

static void
lima_delete_rasterizer_state(struct pipe_context *pctx, void *hwcso)
{
   FREE(hwcso);
}

static void *
lima_create_blend_state(struct pipe_context *pctx,
                        const struct pipe_blend_state *cso)
{
   struct lima_blend_state *so;

   so = CALLOC_STRUCT(lima_blend_state);
   if (!so)
      return NULL;

   printf("dummy %s\n", __func__);

   return so;
}

static void
lima_bind_blend_state(struct pipe_context *pctx, void *hwcso)
{
   printf("dummy %s\n", __func__);
}

static void
lima_delete_blend_state(struct pipe_context *pctx, void *hwcso)
{
   FREE(hwcso);
}

static void *
lima_create_vertex_elements_state(struct pipe_context *pctx, unsigned num_elements,
                                  const struct pipe_vertex_element *elements)
{
   struct lima_vertex_element_state *so;

   so = CALLOC_STRUCT(lima_vertex_element_state);
   if (!so)
      return NULL;

   printf("dummy %s\n", __func__);

   memcpy(so->pipe, elements, sizeof(*elements) * num_elements);
   so->num_elements = num_elements;

   return so;
}

static void
lima_bind_vertex_elements_state(struct pipe_context *pctx, void *hwcso)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   ctx->vertex_elements = hwcso;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_VERTEX_ELEM;
}

static void
lima_delete_vertex_elements_state(struct pipe_context *pctx, void *hwcso)
{
   FREE(hwcso);
}

static void
lima_set_vertex_buffers(struct pipe_context *pctx,
                        unsigned start_slot, unsigned count,
                        const struct pipe_vertex_buffer *vb)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);
   struct lima_context_vertex_buffer *so = &ctx->vertex_buffers;

   util_set_vertex_buffers_mask(so->vb, &so->enabled_mask, vb,
                                start_slot, count);
   so->count = util_last_bit(so->enabled_mask);

   ctx->dirty |= LIMA_CONTEXT_DIRTY_VERTEX_BUFF;
}

static void
lima_set_index_buffer(struct pipe_context *pctx,
                      const struct pipe_index_buffer *ib)
{
   printf("dummy %s\n", __func__);
}

void
lima_state_init(struct lima_context *ctx)
{
   ctx->base.set_framebuffer_state = lima_set_framebuffer_state;
   ctx->base.set_polygon_stipple = lima_set_polygon_stipple;

   ctx->base.set_vertex_buffers = lima_set_vertex_buffers;
   ctx->base.set_index_buffer = lima_set_index_buffer;

   ctx->base.create_depth_stencil_alpha_state = lima_create_depth_stencil_alpha_state;
   ctx->base.bind_depth_stencil_alpha_state = lima_bind_depth_stencil_alpha_state;
   ctx->base.delete_depth_stencil_alpha_state = lima_delete_depth_stencil_alpha_state;

   ctx->base.create_rasterizer_state = lima_create_rasterizer_state;
   ctx->base.bind_rasterizer_state = lima_bind_rasterizer_state;
   ctx->base.delete_rasterizer_state = lima_delete_rasterizer_state;

   ctx->base.create_blend_state = lima_create_blend_state;
   ctx->base.bind_blend_state = lima_bind_blend_state;
   ctx->base.delete_blend_state = lima_delete_blend_state;

   ctx->base.create_vertex_elements_state = lima_create_vertex_elements_state;
   ctx->base.bind_vertex_elements_state = lima_bind_vertex_elements_state;
   ctx->base.delete_vertex_elements_state = lima_delete_vertex_elements_state;
}

void
lima_state_fini(struct lima_context *ctx)
{
   struct lima_context_vertex_buffer *so = &ctx->vertex_buffers;

   util_set_vertex_buffers_mask(so->vb, &so->enabled_mask, NULL,
                                0, ARRAY_SIZE(so->vb));

   pipe_surface_reference(&ctx->framebuffer.cbuf, NULL);
   pipe_surface_reference(&ctx->framebuffer.zsbuf, NULL);
}
