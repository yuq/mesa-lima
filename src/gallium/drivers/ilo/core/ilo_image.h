/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2014 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Chia-I Wu <olv@lunarg.com>
 */

#ifndef ILO_IMAGE_H
#define ILO_IMAGE_H

#include "genhw/genhw.h"
#include "intel_winsys.h"

#include "ilo_core.h"
#include "ilo_dev.h"

struct pipe_resource;

enum ilo_image_walk_type {
   /*
    * Array layers of an LOD are packed together vertically.  This maps to
    * ARYSPC_LOD0 for non-mipmapped 2D textures, and is extended to support
    * mipmapped stencil textures and HiZ on GEN6.
    */
   ILO_IMAGE_WALK_LOD,

   /*
    * LODs of an array layer are packed together.  This maps to ARYSPC_FULL
    * and is used for mipmapped 2D textures.
    */
   ILO_IMAGE_WALK_LAYER,

   /*
    * 3D slices of an LOD are packed together, horizontally with wrapping.
    * Used for 3D textures.
    */
   ILO_IMAGE_WALK_3D,
};

enum ilo_image_aux_type {
   ILO_IMAGE_AUX_NONE,
   ILO_IMAGE_AUX_HIZ,
   ILO_IMAGE_AUX_MCS,
};

struct ilo_image_lod {
   /* physical position */
   unsigned x;
   unsigned y;

   /*
    * Physical size of an LOD slice.  There may be multiple slices when the
    * walk type is not ILO_IMAGE_WALK_LAYER.
    */
   unsigned slice_width;
   unsigned slice_height;
};

/**
 * Texture layout.
 */
struct ilo_image {
   enum ilo_image_aux_type aux;

   /* physical width0, height0, and format */
   unsigned width0;
   unsigned height0;
   enum pipe_format format;
   bool separate_stencil;

   /*
    * width, height, and size of pixel blocks, for conversion between 2D
    * coordinates and memory offsets
    */
   unsigned block_width;
   unsigned block_height;
   unsigned block_size;

   enum ilo_image_walk_type walk;
   bool interleaved_samples;

   /* bitmask of valid tiling modes */
   unsigned valid_tilings;
   enum gen_surface_tiling tiling;

   /* mipmap alignments */
   unsigned align_i;
   unsigned align_j;

   struct ilo_image_lod lods[PIPE_MAX_TEXTURE_LEVELS];

   /* physical height of layers for ILO_IMAGE_WALK_LAYER */
   unsigned layer_height;

   /* distance in bytes between two pixel block rows */
   unsigned bo_stride;
   /* number of pixel block rows */
   unsigned bo_height;

   /* bitmask of levels that can use aux */
   unsigned aux_enables;
   unsigned aux_offsets[PIPE_MAX_TEXTURE_LEVELS];
   unsigned aux_layer_height;
   unsigned aux_stride;
   unsigned aux_height;

   struct intel_bo *bo;
   struct intel_bo *aux_bo;
};

void
ilo_image_init(struct ilo_image *img,
               const struct ilo_dev *dev,
               const struct pipe_resource *templ);

bool
ilo_image_update_for_imported_bo(struct ilo_image *img,
                                 enum gen_surface_tiling tiling,
                                 unsigned bo_stride);

static inline void
ilo_image_cleanup(struct ilo_image *img)
{
   intel_bo_unref(img->bo);
   intel_bo_unref(img->aux_bo);
}

static inline void
ilo_image_set_bo(struct ilo_image *img, struct intel_bo *bo)
{
   intel_bo_unref(img->bo);
   img->bo = intel_bo_ref(bo);
}

static inline void
ilo_image_set_aux_bo(struct ilo_image *img, struct intel_bo *bo)
{
   intel_bo_unref(img->aux_bo);
   img->aux_bo = intel_bo_ref(bo);
}

/**
 * Convert from pixel position to 2D memory offset.
 */
static inline void
ilo_image_pos_to_mem(const struct ilo_image *img,
                     unsigned pos_x, unsigned pos_y,
                     unsigned *mem_x, unsigned *mem_y)
{
   assert(pos_x % img->block_width == 0);
   assert(pos_y % img->block_height == 0);

   *mem_x = pos_x / img->block_width * img->block_size;
   *mem_y = pos_y / img->block_height;
}

/**
 * Convert from 2D memory offset to linear offset.
 */
static inline unsigned
ilo_image_mem_to_linear(const struct ilo_image *img,
                        unsigned mem_x, unsigned mem_y)
{
   return mem_y * img->bo_stride + mem_x;
}

/**
 * Convert from 2D memory offset to raw offset.
 */
static inline unsigned
ilo_image_mem_to_raw(const struct ilo_image *img,
                     unsigned mem_x, unsigned mem_y)
{
   unsigned tile_w, tile_h;

   switch (img->tiling) {
   case GEN6_TILING_NONE:
      tile_w = 1;
      tile_h = 1;
      break;
   case GEN6_TILING_X:
      tile_w = 512;
      tile_h = 8;
      break;
   case GEN6_TILING_Y:
      tile_w = 128;
      tile_h = 32;
      break;
   case GEN8_TILING_W:
      tile_w = 64;
      tile_h = 64;
      break;
   default:
      assert(!"unknown tiling");
      tile_w = 1;
      tile_h = 1;
      break;
   }

   assert(mem_x % tile_w == 0);
   assert(mem_y % tile_h == 0);

   return mem_y * img->bo_stride + mem_x * tile_h;
}

/**
 * Return the stride, in bytes, between slices within a level.
 */
static inline unsigned
ilo_image_get_slice_stride(const struct ilo_image *img, unsigned level)
{
   unsigned h;

   switch (img->walk) {
   case ILO_IMAGE_WALK_LOD:
      h = img->lods[level].slice_height;
      break;
   case ILO_IMAGE_WALK_LAYER:
      h = img->layer_height;
      break;
   case ILO_IMAGE_WALK_3D:
      if (level == 0) {
         h = img->lods[0].slice_height;
         break;
      }
      /* fall through */
   default:
      assert(!"no single stride to walk across slices");
      h = 0;
      break;
   }

   assert(h % img->block_height == 0);

   return (h / img->block_height) * img->bo_stride;
}

/**
 * Return the physical size, in bytes, of a slice in a level.
 */
static inline unsigned
ilo_image_get_slice_size(const struct ilo_image *img, unsigned level)
{
   const unsigned w = img->lods[level].slice_width;
   const unsigned h = img->lods[level].slice_height;

   assert(w % img->block_width == 0);
   assert(h % img->block_height == 0);

   return (w / img->block_width * img->block_size) *
      (h / img->block_height);
}

/**
 * Return the pixel position of a slice.
 */
static inline void
ilo_image_get_slice_pos(const struct ilo_image *img,
                        unsigned level, unsigned slice,
                        unsigned *x, unsigned *y)
{
   switch (img->walk) {
   case ILO_IMAGE_WALK_LOD:
      *x = img->lods[level].x;
      *y = img->lods[level].y + img->lods[level].slice_height * slice;
      break;
   case ILO_IMAGE_WALK_LAYER:
      *x = img->lods[level].x;
      *y = img->lods[level].y + img->layer_height * slice;
      break;
   case ILO_IMAGE_WALK_3D:
      {
         /* slices are packed horizontally with wrapping */
         const unsigned sx = slice & ((1 << level) - 1);
         const unsigned sy = slice >> level;

         *x = img->lods[level].x + img->lods[level].slice_width * sx;
         *y = img->lods[level].y + img->lods[level].slice_height * sy;

         /* should not overlap with the next level */
         if (level + 1 < Elements(img->lods) &&
             img->lods[level + 1].y) {
            assert(*y + img->lods[level].slice_height <=
                  img->lods[level + 1].y);
         }
         break;
      }
   default:
      assert(!"unknown img walk type");
      *x = 0;
      *y = 0;
      break;
   }

   /* should not exceed the bo size */
   assert(*y + img->lods[level].slice_height <=
         img->bo_height * img->block_height);
}

#endif /* ILO_IMAGE_H */
