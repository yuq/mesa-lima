/****************************************************************************
 * Copyright (C) 2015 Intel Corporation.   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 ***************************************************************************/

#ifndef SWR_RESOURCE_H
#define SWR_RESOURCE_H

#include "pipe/p_state.h"
#include "api.h"

struct sw_displaytarget;

struct swr_resource {
   struct pipe_resource base;

   bool has_depth;
   bool has_stencil;

   UINT alignedWidth;
   UINT alignedHeight;

   SWR_SURFACE_STATE swr;
   SWR_SURFACE_STATE secondary; // for faking depth/stencil merged formats

   struct sw_displaytarget *display_target;

   unsigned row_stride[PIPE_MAX_TEXTURE_LEVELS];
   unsigned img_stride[PIPE_MAX_TEXTURE_LEVELS];
   unsigned mip_offsets[PIPE_MAX_TEXTURE_LEVELS];

   /* Opaque pointer to swr_context to mark resource in use */
   void *bound_to_context;
};


static INLINE struct swr_resource *
swr_resource(struct pipe_resource *resource)
{
   return (struct swr_resource *)resource;
}

static INLINE boolean
swr_resource_is_texture(const struct pipe_resource *resource)
{
   switch (resource->target) {
   case PIPE_BUFFER:
      return FALSE;
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_1D_ARRAY:
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_3D:
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_CUBE_ARRAY:
      return TRUE;
   default:
      assert(0);
      return FALSE;
   }
}


static INLINE void *
swr_resource_data(struct pipe_resource *resource)
{
   struct swr_resource *swr_r = swr_resource(resource);

   assert(!swr_resource_is_texture(resource));

   return swr_r->swr.pBaseAddress;
}


void swr_store_render_target(struct swr_context *ctx,
                             uint32_t attachment,
                             enum SWR_TILE_STATE post_tile_state);
#endif
