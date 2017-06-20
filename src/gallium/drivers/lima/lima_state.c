/*
 * Copyright (c) 2011-2013 Luc Verhaegen <libv@skynet.be>
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
#include "lima_resource.h"

static void
lima_update_resource(lima_submit_handle submit, struct pipe_resource *dst,
                     struct pipe_resource *src, unsigned flags)
{
   if (dst) {
      struct lima_resource *res = lima_resource(dst);
      lima_submit_remove_bo(submit, res->buffer->bo);
   }

   if (src) {
      struct lima_resource *res = lima_resource(src);
      lima_submit_add_bo(submit, res->buffer->bo, flags);
   }
}

static void
lima_set_framebuffer_state(struct pipe_context *pctx,
                           const struct pipe_framebuffer_state *framebuffer)
{
   printf("dummy %s\n", __func__);

   printf("%s: psurf color=%p z=%p\n", __func__,
          framebuffer->cbufs[0], framebuffer->zsbuf);

   struct lima_context *ctx = lima_context(pctx);
   struct lima_context_framebuffer *fb = &ctx->framebuffer;

   lima_update_resource(ctx->pp_submit, fb->cbuf ? fb->cbuf->texture : NULL,
                        framebuffer->cbufs[0] ? framebuffer->cbufs[0]->texture : NULL,
                        LIMA_SUBMIT_BO_FLAG_WRITE);
   pipe_surface_reference(&fb->cbuf, framebuffer->cbufs[0]);

   lima_update_resource(ctx->pp_submit, fb->zsbuf ? fb->zsbuf->texture : NULL,
                        framebuffer->zsbuf ? framebuffer->zsbuf->texture : NULL,
                        LIMA_SUBMIT_BO_FLAG_WRITE);
   pipe_surface_reference(&fb->zsbuf, framebuffer->zsbuf);

   /* need align here? */
   fb->width = framebuffer->width;
   fb->height = framebuffer->height;

   int width = align(framebuffer->width, 16) >> 4;
   int height = align(framebuffer->height, 16) >> 4;
   if (fb->tiled_w != width || fb->tiled_h != height) {
      fb->tiled_w = width;
      fb->tiled_h = height;

      /* max 512, not sure if set to 512 will affect performance */
      int limit = 500;
      while ((width * height) > limit) {
         if (width >= height) {
            width = (width + 1) >> 1;
            fb->shift_w++;
         } else {
            height = (height + 1) >> 1;
            fb->shift_h++;
         }
      }

      fb->block_w = width;
      fb->block_h = height;

      int max;
      if (fb->shift_h > fb->shift_w)
         max = fb->shift_h;
      else
         max = fb->shift_w;

      if (max > 2)
         fb->shift_max = 2;
      else if (max)
         fb->shift_max = 1;

      printf("fb dim change tiled=%d/%d block=%d/%d shift=%d/%d\n",
             fb->tiled_w, fb->tiled_h, fb->block_w, fb->block_h,
             fb->shift_w, fb->shift_h);

      fb->dirty_dim = true;
   }

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
   printf("dummy %s\n", __func__);

   struct lima_depth_stencil_alpha_state *so;

   so = CALLOC_STRUCT(lima_depth_stencil_alpha_state);
   if (!so)
      return NULL;

   printf("depth enable=%d min_b=%f max_b=%f\n",
          cso->depth.enabled, cso->depth.bounds_min, cso->depth.bounds_max);

   so->base = *cso;

   return so;
}

static void
lima_bind_depth_stencil_alpha_state(struct pipe_context *pctx, void *hwcso)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   ctx->zsa = hwcso;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_ZSA;
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
   printf("dummy %s\n", __func__);

   struct lima_rasterizer_state *so;

   so = CALLOC_STRUCT(lima_rasterizer_state);
   if (!so)
      return NULL;

   so->base = *cso;

   return so;
}

static void
lima_bind_rasterizer_state(struct pipe_context *pctx, void *hwcso)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   ctx->rasterizer = hwcso;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_RASTERIZER;
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
   printf("dummy %s\n", __func__);

   struct lima_blend_state *so;

   so = CALLOC_STRUCT(lima_blend_state);
   if (!so)
      return NULL;

   so->base = *cso;

   return so;
}

static void
lima_bind_blend_state(struct pipe_context *pctx, void *hwcso)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   ctx->blend = hwcso;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_BLEND;
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
   unsigned i;

   for (i = 0; i < count; i++)
      lima_update_resource(ctx->gp_submit, so->vb[start_slot + i].buffer,
                           vb ? vb[i].buffer : NULL,
                           LIMA_SUBMIT_BO_FLAG_READ);

   util_set_vertex_buffers_mask(so->vb + start_slot, &so->enabled_mask,
                                vb, start_slot, count);
   so->count = util_last_bit(so->enabled_mask);

   ctx->dirty |= LIMA_CONTEXT_DIRTY_VERTEX_BUFF;
}

static void
lima_set_index_buffer(struct pipe_context *pctx,
                      const struct pipe_index_buffer *ib)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   if (ib) {
      pipe_resource_reference(&ctx->index_buffer.buffer, ib->buffer);
      ctx->index_buffer.index_size = ib->index_size;
      ctx->index_buffer.offset = ib->offset;
      ctx->index_buffer.user_buffer = ib->user_buffer;
   } else {
      pipe_resource_reference(&ctx->index_buffer.buffer, NULL);
   }

   ctx->dirty |= LIMA_CONTEXT_DIRTY_INDEX_BUFF;
}

static void
lima_set_viewport_states(struct pipe_context *pctx,
                         unsigned start_slot,
                         unsigned num_viewports,
                         const struct pipe_viewport_state *viewport)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   /* reverse calculate the parameter of glViewport */
   ctx->viewport.x = viewport->translate[0] - viewport->scale[0];
   ctx->viewport.y = fabsf(viewport->translate[1] - fabsf(viewport->scale[1]));
   ctx->viewport.width = viewport->scale[0] * 2;
   ctx->viewport.height = fabsf(viewport->scale[1] * 2);

   /* reverse calculate the parameter of glDepthRange */
   ctx->viewport.near = viewport->translate[2] - viewport->scale[2];
   ctx->viewport.far = viewport->translate[2] + viewport->scale[2];

   printf("viewport scale=%f/%f/%f translate=%f/%f/%f\n",
          viewport->scale[0], viewport->scale[1], viewport->scale[2],
          viewport->translate[0], viewport->translate[1], viewport->translate[2]);
   printf("glViewport x/y/w/h = %f/%f/%f/%f\n",
          ctx->viewport.x, ctx->viewport.y, ctx->viewport.width, ctx->viewport.height);
   printf("glDepthRange n/f = %f/%f\n",
          ctx->viewport.near, ctx->viewport.far);

   ctx->viewport.transform = *viewport;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_VIEWPORT;
}

static void
lima_set_scissor_states(struct pipe_context *pctx,
                        unsigned start_slot,
                        unsigned num_scissors,
                        const struct pipe_scissor_state *scissor)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   printf("scissor min=%d/%d max=%d/%d\n",
          scissor->minx, scissor->miny,
          scissor->maxx, scissor->maxy);

   ctx->scissor = *scissor;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_SCISSOR;
}

static void
lima_set_blend_color(struct pipe_context *pctx,
                     const struct pipe_blend_color *blend_color)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   ctx->blend_color = *blend_color;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_BLEND_COLOR;
}

static void
lima_set_stencil_ref(struct pipe_context *pctx,
                     const struct pipe_stencil_ref *stencil_ref)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   ctx->stencil_ref = *stencil_ref;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_STENCIL_REF;
}

static void
lima_set_constant_buffer(struct pipe_context *pctx,
                         enum pipe_shader_type shader, uint index,
                         const struct pipe_constant_buffer *cb)
{
   printf("dummy %s\n", __func__);

   printf("shader %d index %u cb buffer %p offset %x size %x\n",
          shader, index, cb->buffer, cb->buffer_offset, cb->buffer_size);
}

void
lima_state_init(struct lima_context *ctx)
{
   ctx->base.set_framebuffer_state = lima_set_framebuffer_state;
   ctx->base.set_polygon_stipple = lima_set_polygon_stipple;
   ctx->base.set_viewport_states = lima_set_viewport_states;
   ctx->base.set_scissor_states = lima_set_scissor_states;
   ctx->base.set_blend_color = lima_set_blend_color;
   ctx->base.set_stencil_ref = lima_set_stencil_ref;

   ctx->base.set_vertex_buffers = lima_set_vertex_buffers;
   ctx->base.set_index_buffer = lima_set_index_buffer;
   ctx->base.set_constant_buffer = lima_set_constant_buffer;

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

   pipe_resource_reference(&ctx->index_buffer.buffer, NULL);
}
