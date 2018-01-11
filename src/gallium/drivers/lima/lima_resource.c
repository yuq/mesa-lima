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
#include "util/u_debug.h"
#include "util/u_transfer.h"
#include "renderonly/renderonly.h"

#include "state_tracker/drm_driver.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_resource.h"
#include "lima_bo.h"
#include "lima_util.h"
#include "lima_drm.h"

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

   /* TODO: mipmap */
   struct pipe_resource *pres = &res->base;
   unsigned width, height;
   if (pres->bind & PIPE_BIND_RENDER_TARGET) {
      width = align(pres->width0, 16);
      height = align(pres->height0, 16);
   }
   else {
      width = pres->width0;
      height = pres->height0;
   }

   res->stride = util_format_get_stride(pres->format, width);

   uint32_t size = res->stride *
      util_format_get_nblocksy(pres->format, height) *
      pres->array_size * pres->depth0;

   res->bo = lima_bo_create(screen, align(size, LIMA_PAGE_SIZE), 0, false, false);
   if (!res->bo)
      goto err_out0;

   if (screen->ro && templat->bind & PIPE_BIND_SCANOUT) {
      res->scanout =
         renderonly_scanout_for_resource(pres, screen->ro, NULL);
      if (!res->scanout)
         goto err_out1;
   }

   debug_printf("%s: pres=%p width=%u height=%u depth=%u target=%d bind=%x\n",
                __func__, &res->base, pres->width0, pres->height0, pres->depth0,
                pres->target, pres->bind);

   return pres;

err_out1:
   lima_bo_free(res->bo);
err_out0:
   FREE(res);
   return NULL;
}

static void
lima_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *pres)
{
   struct lima_screen *screen = lima_screen(pscreen);
   struct lima_resource *res = lima_resource(pres);

   if (res->bo)
      lima_bo_free(res->bo);

   if (res->scanout)
      renderonly_scanout_destroy(res->scanout, screen->ro);

   FREE(res);
}

static struct pipe_resource *
lima_resource_from_handle(struct pipe_screen *pscreen,
        const struct pipe_resource *templat,
        struct winsys_handle *handle, unsigned usage)
{
   struct lima_resource *res;
   struct lima_screen *screen = lima_screen(pscreen);

   res = CALLOC_STRUCT(lima_resource);
   if (!res)
      return NULL;

   struct pipe_resource *pres = &res->base;
   *pres = *templat;
   pres->screen = pscreen;
   pipe_reference_init(&pres->reference, 1);
   res->stride = handle->stride;

   res->bo = lima_bo_import(screen, handle);
   if (!res->bo) {
      FREE(res);
      return NULL;
   }

   /* check alignment for the buffer */
   if (pres->bind & PIPE_BIND_RENDER_TARGET) {
      unsigned width, height, stride, size;

      width = align(pres->width0, 16);
      height = align(pres->height0, 16);
      stride = util_format_get_stride(pres->format, width);
      size = util_format_get_2d_size(pres->format, stride, height);

      if (res->stride != stride || res->bo->size < size) {
         debug_error("import buffer not properly aligned\n");
         lima_resource_destroy(pscreen, pres);
         return NULL;
      }
   }

   return pres;
}

static boolean
lima_resource_get_handle(struct pipe_screen *pscreen,
                         struct pipe_context *pctx,
                         struct pipe_resource *pres,
                         struct winsys_handle *handle, unsigned usage)
{
   struct lima_screen *screen = lima_screen(pscreen);
   struct lima_resource *res = lima_resource(pres);

   if (handle->type == DRM_API_HANDLE_TYPE_KMS && screen->ro &&
       renderonly_get_handle(res->scanout, handle))
      return TRUE;

   if (!lima_bo_export(res->bo, handle))
      return FALSE;

   handle->stride = res->stride;
   return TRUE;
}

void
lima_resource_screen_init(struct lima_screen *screen)
{
   screen->base.resource_create = lima_resource_create;
   screen->base.resource_from_handle = lima_resource_from_handle;
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

   debug_printf("%s: pres=%p psurf=%p\n", __func__, pres, psurf);

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
   struct lima_bo *bo = res->bo;
   struct lima_transfer *trans;
   struct pipe_transfer *ptrans;
   unsigned bo_op = 0;

   debug_printf("%s: pres=%p\n", __func__, pres);

   if (usage & PIPE_TRANSFER_READ)
      bo_op |= LIMA_GEM_WAIT_READ;
   if (usage & PIPE_TRANSFER_WRITE)
      bo_op |= LIMA_GEM_WAIT_WRITE;

   if (!lima_bo_wait(bo, bo_op, 1000000000, true))
      return NULL;

   if (!lima_bo_update(bo, true, false))
      return NULL;

   trans = slab_alloc(&ctx->transfer_pool);
   if (!trans)
      return NULL;

   memset(trans, 0, sizeof(*trans));
   ptrans = &trans->base;

   pipe_resource_reference(&ptrans->resource, pres);
   ptrans->level = level;
   ptrans->usage = usage;
   ptrans->box = *box;
   ptrans->stride = res->stride;

   *pptrans = ptrans;

   return bo->map + box->z * ptrans->layer_stride +
      box->y / util_format_get_blockheight(pres->format) * ptrans->stride +
      box->x / util_format_get_blockwidth(pres->format) *
      util_format_get_blocksize(pres->format);
}

static void
lima_transfer_flush_region(struct pipe_context *pctx,
                           struct pipe_transfer *ptrans,
                           const struct pipe_box *box)
{
   debug_checkpoint();
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
   debug_checkpoint();
}

void
lima_resource_context_init(struct lima_context *ctx)
{
   ctx->base.create_surface = lima_surface_create;
   ctx->base.surface_destroy = lima_surface_destroy;

   ctx->base.buffer_subdata = u_default_buffer_subdata;

   ctx->base.transfer_map = lima_transfer_map;
   ctx->base.transfer_flush_region = lima_transfer_flush_region;
   ctx->base.transfer_unmap = lima_transfer_unmap;

   ctx->base.flush_resource = lima_flush_resource;
}
