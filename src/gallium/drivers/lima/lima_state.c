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
#include "util/u_debug.h"

#include "pipe/p_state.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_resource.h"

static void
lima_set_framebuffer_state(struct pipe_context *pctx,
                           const struct pipe_framebuffer_state *framebuffer)
{
   debug_checkpoint();

   debug_printf("%s: psurf color=%p z=%p\n", __func__,
                framebuffer->cbufs[0], framebuffer->zsbuf);

   struct lima_context *ctx = lima_context(pctx);
   struct lima_context_framebuffer *fb = &ctx->framebuffer;

   pipe_surface_reference(&fb->cbuf, framebuffer->cbufs[0]);
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

      debug_printf("fb dim change tiled=%d/%d block=%d/%d shift=%d/%d\n",
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
   debug_checkpoint();
}

static void *
lima_create_depth_stencil_alpha_state(struct pipe_context *pctx,
                                      const struct pipe_depth_stencil_alpha_state *cso)
{
   debug_checkpoint();

   struct lima_depth_stencil_alpha_state *so;

   so = CALLOC_STRUCT(lima_depth_stencil_alpha_state);
   if (!so)
      return NULL;

   debug_printf("depth enable=%d min_b=%f max_b=%f\n",
                cso->depth.enabled, cso->depth.bounds_min, cso->depth.bounds_max);

   so->base = *cso;

   return so;
}

static void
lima_bind_depth_stencil_alpha_state(struct pipe_context *pctx, void *hwcso)
{
   debug_checkpoint();

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
   debug_checkpoint();

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
   debug_checkpoint();

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
   debug_checkpoint();

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
   debug_checkpoint();

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

   debug_checkpoint();

   memcpy(so->pipe, elements, sizeof(*elements) * num_elements);
   so->num_elements = num_elements;

   return so;
}

static void
lima_bind_vertex_elements_state(struct pipe_context *pctx, void *hwcso)
{
   debug_checkpoint();

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
   debug_checkpoint();

   struct lima_context *ctx = lima_context(pctx);
   struct lima_context_vertex_buffer *so = &ctx->vertex_buffers;

   util_set_vertex_buffers_mask(so->vb + start_slot, &so->enabled_mask,
                                vb, start_slot, count);
   so->count = util_last_bit(so->enabled_mask);

   ctx->dirty |= LIMA_CONTEXT_DIRTY_VERTEX_BUFF;
}

static void
lima_set_viewport_states(struct pipe_context *pctx,
                         unsigned start_slot,
                         unsigned num_viewports,
                         const struct pipe_viewport_state *viewport)
{
   debug_checkpoint();

   struct lima_context *ctx = lima_context(pctx);

   /* reverse calculate the parameter of glViewport */
   ctx->viewport.x = viewport->translate[0] - viewport->scale[0];
   ctx->viewport.y = fabsf(viewport->translate[1] - fabsf(viewport->scale[1]));
   ctx->viewport.width = viewport->scale[0] * 2;
   ctx->viewport.height = fabsf(viewport->scale[1] * 2);

   /* reverse calculate the parameter of glDepthRange */
   ctx->viewport.near = viewport->translate[2] - viewport->scale[2];
   ctx->viewport.far = viewport->translate[2] + viewport->scale[2];

   debug_printf("viewport scale=%f/%f/%f translate=%f/%f/%f\n",
                viewport->scale[0], viewport->scale[1], viewport->scale[2],
                viewport->translate[0], viewport->translate[1],
                viewport->translate[2]);
   debug_printf("glViewport x/y/w/h = %f/%f/%f/%f\n",
                ctx->viewport.x, ctx->viewport.y, ctx->viewport.width,
                ctx->viewport.height);
   debug_printf("glDepthRange n/f = %f/%f\n",
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
   debug_checkpoint();

   struct lima_context *ctx = lima_context(pctx);

   debug_printf("scissor min=%d/%d max=%d/%d\n",
                scissor->minx, scissor->miny,
                scissor->maxx, scissor->maxy);

   ctx->scissor = *scissor;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_SCISSOR;
}

static void
lima_set_blend_color(struct pipe_context *pctx,
                     const struct pipe_blend_color *blend_color)
{
   debug_checkpoint();

   struct lima_context *ctx = lima_context(pctx);

   ctx->blend_color = *blend_color;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_BLEND_COLOR;
}

static void
lima_set_stencil_ref(struct pipe_context *pctx,
                     const struct pipe_stencil_ref *stencil_ref)
{
   debug_checkpoint();

   struct lima_context *ctx = lima_context(pctx);

   ctx->stencil_ref = *stencil_ref;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_STENCIL_REF;
}

static void
lima_set_constant_buffer(struct pipe_context *pctx,
                         enum pipe_shader_type shader, uint index,
                         const struct pipe_constant_buffer *cb)
{
   debug_checkpoint();

   struct lima_context *ctx = lima_context(pctx);
   struct lima_context_constant_buffer *so = ctx->const_buffer + shader;

   assert(index == 0);

   if (unlikely(!cb)) {
      so->buffer = NULL;
      so->size = 0;
      debug_printf("shader %d index %u cb is null\n",
                   shader, index);
   } else {
      assert(!cb->buffer);

      so->buffer = cb->user_buffer + cb->buffer_offset;
      so->size = cb->buffer_size;
      debug_printf("shader %d index %u cb buffer %p offset %x size %x\n",
                   shader, index, cb->buffer, cb->buffer_offset, cb->buffer_size);
   }

   so->dirty = true;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_CONST_BUFF;

}

static void *
lima_create_sampler_state(struct pipe_context *pctx,
                         const struct pipe_sampler_state *cso)
{
   struct lima_sampler_state *so = CALLOC_STRUCT(lima_sampler_state);
   if (!so)
      return NULL;

   memcpy(so, cso, sizeof(*cso));

   return so;
}

static void
lima_sampler_state_delete(struct pipe_context *pctx, void *sstate)
{
   free(sstate);
}

static void
lima_sampler_states_bind(struct pipe_context *pctx,
                        enum pipe_shader_type shader, unsigned start,
                        unsigned nr, void **hwcso)
{
   struct lima_context *ctx = lima_context(pctx);
   struct lima_texture_stateobj *lima_tex = &ctx->tex_stateobj;
   unsigned i;
   unsigned new_nr = 0;

   assert(start == 0);

   for (i = 0; i < nr; i++) {
      if (hwcso[i])
         new_nr = i + 1;
      lima_tex->samplers[i] = hwcso[i];
   }

   for (; i < lima_tex->num_samplers; i++) {
      lima_tex->samplers[i] = NULL;
   }

   lima_tex->num_samplers = new_nr;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_TEXTURES;
}

static struct pipe_sampler_view *
lima_create_sampler_view(struct pipe_context *pctx, struct pipe_resource *prsc,
                        const struct pipe_sampler_view *cso)
{
   struct lima_sampler_view *so = CALLOC_STRUCT(lima_sampler_view);

   if (!so)
      return NULL;

   so->base = *cso;

   pipe_reference(NULL, &prsc->reference);
   so->base.texture = prsc;
   so->base.reference.count = 1;
   so->base.context = pctx;

   return &so->base;
}

static void
lima_sampler_view_destroy(struct pipe_context *pctx,
                         struct pipe_sampler_view *pview)
{
   struct lima_sampler_view *view = lima_sampler_view(pview);

   pipe_resource_reference(&pview->texture, NULL);

   free(view);
}

static void
lima_set_sampler_views(struct pipe_context *pctx,
                      enum pipe_shader_type shader,
                      unsigned start, unsigned nr,
                      struct pipe_sampler_view **views)
{
   struct lima_context *ctx = lima_context(pctx);
   struct lima_texture_stateobj *lima_tex = &ctx->tex_stateobj;
   int i;
   unsigned new_nr = 0;

   assert(start == 0);

   for (i = 0; i < nr; i++) {
      if (views[i])
         new_nr = i + 1;
      pipe_sampler_view_reference(&lima_tex->textures[i], views[i]);
   }

   for (; i < lima_tex->num_textures; i++) {
      pipe_sampler_view_reference(&lima_tex->textures[i], NULL);
   }

   lima_tex->num_textures = new_nr;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_TEXTURES;
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

   ctx->base.create_sampler_state = lima_create_sampler_state;
   ctx->base.delete_sampler_state = lima_sampler_state_delete;
   ctx->base.bind_sampler_states = lima_sampler_states_bind;

   ctx->base.create_sampler_view = lima_create_sampler_view;
   ctx->base.sampler_view_destroy = lima_sampler_view_destroy;
   ctx->base.set_sampler_views = lima_set_sampler_views;
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
