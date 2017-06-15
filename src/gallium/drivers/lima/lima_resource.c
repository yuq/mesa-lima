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
#include "util/u_format.h"
#include "util/u_inlines.h"
#include "util/u_math.h"

#include "state_tracker/drm_driver.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_resource.h"


void
lima_buffer_free(struct lima_buffer *buffer)
{
   if (buffer->bo)
      lima_bo_free(buffer->bo);

   if (buffer->va)
      lima_va_range_free(buffer->screen->dev, buffer->size, buffer->va);

   FREE(buffer);
}

struct lima_buffer *
lima_buffer_alloc(struct lima_screen *screen, uint32_t size,
                     enum lima_buffer_alloc_flag flags)
{
   struct lima_buffer *buffer = CALLOC_STRUCT(lima_buffer);

   if (!buffer)
      return NULL;

   buffer->size = size;
   buffer->screen = screen;

   struct lima_bo_create_request req = {
      .size = size,
      .flags = 0,
   };
   if (lima_bo_create(screen->dev, &req, &buffer->bo))
      goto err_out;

   if (flags & LIMA_BUFFER_ALLOC_MAP) {
      buffer->map = lima_bo_map(buffer->bo);
      if (!buffer->map)
         goto err_out;
   }

   if (flags & LIMA_BUFFER_ALLOC_VA) {
      if (lima_va_range_alloc(screen->dev, size, &buffer->va) ||
          lima_bo_va_map(buffer->bo, buffer->va, 0))
          goto err_out;
   }

   return buffer;

err_out:
   lima_buffer_free(buffer);
   return NULL;
}

int
lima_buffer_update(struct lima_buffer *buffer,
                   enum lima_buffer_alloc_flag flags)
{
   if ((flags & LIMA_BUFFER_ALLOC_MAP) && !buffer->map) {
      buffer->map = lima_bo_map(buffer->bo);
      if (!buffer->map)
         return -1;
   }

   if ((flags & LIMA_BUFFER_ALLOC_VA) && !buffer->va) {
      uint32_t va;
      int err;

      err = lima_va_range_alloc(buffer->screen->dev, buffer->size, &va);
      if (err)
         return err;

      err = lima_bo_va_map(buffer->bo, va, 0);
      if (err) {
         lima_va_range_free(buffer->screen->dev, buffer->size, va);
         return err;
      }

      buffer->va = va;
   }

   return 0;
}

static struct pipe_resource *
lima_resource_create(struct pipe_screen *pscreen,
                     const struct pipe_resource *templat)
{
   struct lima_screen *screen = lima_screen(pscreen);
   struct lima_resource *res;

   res = CALLOC_STRUCT(lima_resource);
   if (!res)
      return NULL;

   res->base = *templat;
   res->base.screen = pscreen;
   pipe_reference_init(&res->base.reference, 1);

   /* TODO: mipmap, padding */
   struct pipe_resource *pres = &res->base;
   res->stride = util_format_get_stride(pres->format, pres->width0);

   uint32_t size = res->stride *
      util_format_get_nblocksy(pres->format, pres->height0) *
      pres->array_size * pres->depth0;

   res->buffer = lima_buffer_alloc(screen, align(size, 0x1000), 0);
   if (!res->buffer) {
      FREE(res);
      return NULL;
   }

   printf("%s: pres=%p width=%u height=%u\n", __func__, &res->base,
          pres->width0, pres->height0);

   return pres;
}

static void
lima_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *pres)
{
   struct lima_resource *res = lima_resource(pres);

   if (res->buffer)
      lima_buffer_free(res->buffer);

   FREE(res);
}

static boolean
lima_resource_get_handle(struct pipe_screen *pscreen,
                         struct pipe_context *pctx,
                         struct pipe_resource *pres,
                         struct winsys_handle *handle, unsigned usage)
{
   struct lima_resource *res = lima_resource(pres);
   lima_bo_handle bo = res->buffer->bo;
   int err;

   switch (handle->type) {
   case DRM_API_HANDLE_TYPE_SHARED:
      err = lima_bo_export(bo, lima_bo_handle_type_gem_flink_name, &handle->handle);
      if (err)
         return FALSE;
      break;
   case DRM_API_HANDLE_TYPE_KMS:
      err = lima_bo_export(bo, lima_bo_handle_type_kms, &handle->handle);
      if (err)
         return FALSE;
   default:
      return FALSE;
   }

   handle->stride = res->stride;
   return TRUE;
}

void
lima_resource_screen_init(struct lima_screen *screen)
{
   screen->base.resource_create = lima_resource_create;
   screen->base.resource_destroy = lima_resource_destroy;
   screen->base.resource_get_handle = lima_resource_get_handle;
}

static struct pipe_surface *
lima_surface_create(struct pipe_context *pctx,
                    struct pipe_resource *pres,
                    const struct pipe_surface *surf_tmpl)
{
   struct lima_surface *surf = CALLOC_STRUCT(lima_surface);

   if (!surf)
      return NULL;

   assert(surf_tmpl->u.tex.first_layer == surf_tmpl->u.tex.last_layer);

   struct pipe_surface *psurf = &surf->base;
   unsigned level = surf_tmpl->u.tex.level;

   pipe_reference_init(&psurf->reference, 1);
   pipe_resource_reference(&psurf->texture, pres);

   psurf->context = pctx;
   psurf->format = surf_tmpl->format;
   psurf->width = u_minify(pres->width0, level);
   psurf->height = u_minify(pres->height0, level);
   psurf->u.tex.level = level;
   psurf->u.tex.first_layer = surf_tmpl->u.tex.first_layer;
   psurf->u.tex.last_layer = surf_tmpl->u.tex.last_layer;

   printf("%s: pres=%p psurf=%p\n", __func__, pres, psurf);

   return &surf->base;
}

static void
lima_surface_destroy(struct pipe_context *pctx, struct pipe_surface *psurf)
{
   struct lima_surface *surf = lima_surface(psurf);

   pipe_resource_reference(&psurf->texture, NULL);
   FREE(surf);
}

static void *
lima_transfer_map(struct pipe_context *pctx,
                  struct pipe_resource *pres,
                  unsigned level,
                  unsigned usage,
                  const struct pipe_box *box,
                  struct pipe_transfer **pptrans)
{
   struct lima_context *ctx = lima_context(pctx);
   struct lima_resource *res = lima_resource(pres);
   struct lima_buffer *buffer = res->buffer;
   struct lima_transfer *trans;
   struct pipe_transfer *ptrans;
   unsigned bo_op = 0;

   printf("%s: pres=%p\n", __func__, pres);

   if (usage & PIPE_TRANSFER_READ)
      bo_op |= LIMA_BO_WAIT_FLAG_READ;
   if (usage & PIPE_TRANSFER_WRITE)
      bo_op |= LIMA_BO_WAIT_FLAG_WRITE;

   if (lima_bo_wait(buffer->bo, bo_op, 1000000000, true))
      return NULL;

   if (!buffer->map) {
      buffer->map = lima_bo_map(buffer->bo);
      if (!buffer->map)
         return NULL;
   }

   trans = slab_alloc(&ctx->transfer_pool);
   if (!trans)
      return NULL;

   memset(trans, 0, sizeof(*trans));
   ptrans = &trans->base;

   pipe_resource_reference(&ptrans->resource, pres);
   ptrans->level = level;
   ptrans->usage = usage;
   ptrans->box = *box;
   ptrans->stride = util_format_get_stride(pres->format, pres->width0);
   ptrans->layer_stride = ptrans->stride * util_format_get_nblocksy(pres->format, pres->height0);

   *pptrans = ptrans;

   return buffer->map + box->z * ptrans->layer_stride +
      box->y / util_format_get_blockheight(pres->format) * ptrans->stride +
      box->x / util_format_get_blockwidth(pres->format) *
      util_format_get_blocksize(pres->format);
}

static void
lima_transfer_flush_region(struct pipe_context *pctx,
                           struct pipe_transfer *ptrans,
                           const struct pipe_box *box)
{
   printf("dummy %s\n", __func__);
}

static void
lima_transfer_unmap(struct pipe_context *pctx,
                    struct pipe_transfer *ptrans)
{
   struct lima_context *ctx = lima_context(pctx);
   struct lima_transfer *trans = lima_transfer(ptrans);

   pipe_resource_reference(&ptrans->resource, NULL);
   slab_free(&ctx->transfer_pool, trans);
}

static void
lima_flush_resource(struct pipe_context *pctx, struct pipe_resource *resource)
{
   printf("dummy %s\n", __func__);
}

void
lima_resource_context_init(struct lima_context *ctx)
{
   ctx->base.create_surface = lima_surface_create;
   ctx->base.surface_destroy = lima_surface_destroy;

   ctx->base.transfer_map = lima_transfer_map;
   ctx->base.transfer_flush_region = lima_transfer_flush_region;
   ctx->base.transfer_unmap = lima_transfer_unmap;

   ctx->base.flush_resource = lima_flush_resource;
}
