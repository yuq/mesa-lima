/*
 * Copyright © 2015 Intel Corporation
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
 */

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "anv_private.h"

static const uint8_t anv_halign[] = {
    [4] = HALIGN4,
    [8] = HALIGN8,
    [16] = HALIGN16,
};

static const uint8_t anv_valign[] = {
    [4] = VALIGN4,
    [8] = VALIGN8,
    [16] = VALIGN16,
};

static const uint8_t anv_surf_type_from_image_type[] = {
   [VK_IMAGE_TYPE_1D] = SURFTYPE_1D,
   [VK_IMAGE_TYPE_2D] = SURFTYPE_2D,
   [VK_IMAGE_TYPE_3D] = SURFTYPE_3D,

};

static const struct anv_image_view_info
anv_image_view_info_table[] = {
   #define INFO(s, ...) { .surface_type = s, __VA_ARGS__ }
   [VK_IMAGE_VIEW_TYPE_1D]          = INFO(SURFTYPE_1D),
   [VK_IMAGE_VIEW_TYPE_2D]          = INFO(SURFTYPE_2D),
   [VK_IMAGE_VIEW_TYPE_3D]          = INFO(SURFTYPE_3D),
   [VK_IMAGE_VIEW_TYPE_CUBE]        = INFO(SURFTYPE_CUBE,                  .is_cube = 1),
   [VK_IMAGE_VIEW_TYPE_1D_ARRAY]    = INFO(SURFTYPE_1D,     .is_array = 1),
   [VK_IMAGE_VIEW_TYPE_2D_ARRAY]    = INFO(SURFTYPE_2D,     .is_array = 1),
   [VK_IMAGE_VIEW_TYPE_CUBE_ARRAY]  = INFO(SURFTYPE_CUBE,   .is_array = 1, .is_cube = 1),
   #undef INFO
};

const struct anv_image_view_info *
anv_image_view_info_for_vk_image_view_type(VkImageViewType type)
{
   return &anv_image_view_info_table[type];
}

static const struct anv_surf_type_limits {
   int32_t width;
   int32_t height;
   int32_t depth;
} anv_surf_type_limits[] = {
   [SURFTYPE_1D]     = {16384,       0,   2048},
   [SURFTYPE_2D]     = {16384,   16384,   2048},
   [SURFTYPE_3D]     = {2048,     2048,   2048},
   [SURFTYPE_CUBE]   = {16384,   16384,    340},
   [SURFTYPE_BUFFER] = {128,     16384,     64},
   [SURFTYPE_STRBUF] = {128,     16384,     64},
};

static const struct anv_tile_info {
   uint32_t width;
   uint32_t height;

   /**
    * Alignment for RENDER_SURFACE_STATE.SurfaceBaseAddress.
    *
    * To simplify calculations, the alignments defined in the table are
    * sometimes larger than required.  For example, Skylake requires that X and
    * Y tiled buffers be aligned to 4K, but Broadwell permits smaller
    * alignment. We choose 4K to accomodate both chipsets.  The alignment of
    * a linear buffer depends on its element type and usage. Linear depth
    * buffers have the largest alignment, 64B, so we choose that for all linear
    * buffers.
    */
   uint32_t surface_alignment;
} anv_tile_info_table[] = {
   [LINEAR] = {   1,  1,   64 },
   [XMAJOR] = { 512,  8, 4096 },
   [YMAJOR] = { 128, 32, 4096 },
   [WMAJOR] = { 128, 32, 4096 },
};

/**
 * Return -1 on failure.
 */
static int8_t
anv_image_choose_tile_mode(const struct anv_image_create_info *anv_info)
{
   if (anv_info->force_tile_mode)
      return anv_info->tile_mode;

   /* The Sandybridge PRM says that the stencil buffer "is supported
    * only in Tile W memory".
    */

   switch (anv_info->vk_info->tiling) {
   case VK_IMAGE_TILING_LINEAR:
      if (unlikely(anv_info->vk_info->format == VK_FORMAT_S8_UINT)) {
         return -1;
      } else {
         return LINEAR;
      }
   case VK_IMAGE_TILING_OPTIMAL:
      if (unlikely(anv_info->vk_info->format == VK_FORMAT_S8_UINT)) {
         return WMAJOR;
      } else {
         return YMAJOR;
      }
   default:
      assert(!"bad VKImageTiling");
      return LINEAR;
   }
}


/**
 * The \a format argument is required and overrides any format in
 * struct anv_image_create_info.
 */
static VkResult
anv_image_make_surface(const struct anv_image_create_info *create_info,
                       const struct anv_format *format,
                       uint64_t *inout_image_size,
                       uint32_t *inout_image_alignment,
                       struct anv_surface *out_surface)
{
   /* See RENDER_SURFACE_STATE.SurfaceQPitch */
   static const uint16_t min_qpitch UNUSED = 0x4;
   static const uint16_t max_qpitch UNUSED = 0x1ffc;

   const VkExtent3D *restrict extent = &create_info->vk_info->extent;
   const uint32_t levels = create_info->vk_info->mipLevels;
   const uint32_t array_size = create_info->vk_info->arraySize;

   const int8_t tile_mode = anv_image_choose_tile_mode(create_info);
   if (tile_mode == -1)
      return vk_error(VK_ERROR_INVALID_IMAGE);

   const struct anv_tile_info *tile_info =
       &anv_tile_info_table[tile_mode];

   const uint32_t i = 4; /* FINISHME: Stop hardcoding subimage alignment */
   const uint32_t j = 4; /* FINISHME: Stop hardcoding subimage alignment */
   const uint32_t w0 = align_u32(extent->width, i);
   const uint32_t h0 = align_u32(extent->height, j);

   uint16_t qpitch;
   uint32_t mt_width;
   uint32_t mt_height;

   if (levels == 1 && array_size == 1) {
      qpitch = min_qpitch;
      mt_width = w0;
      mt_height = h0;
   } else {
      uint32_t w1 = align_u32(anv_minify(extent->width, 1), i);
      uint32_t h1 = align_u32(anv_minify(extent->height, 1), j);
      uint32_t w2 = align_u32(anv_minify(extent->width, 2), i);

      /* The QPitch equation is found in the Broadwell PRM >> Volume 5: Memory
       * Views >> Common Surface Formats >> Surface Layout >> 2D Surfaces >>
       * Surface Arrays >> For All Surface Other Than Separate Stencil Buffer:
       */
      qpitch = h0 + h1 + 11 * j;
      mt_width = MAX(w0, w1 + w2);
      mt_height = array_size * qpitch;
   }

   assert(qpitch >= min_qpitch);
   if (qpitch > max_qpitch) {
      anv_loge("image qpitch > 0x%x\n", max_qpitch);
      return vk_error(VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   /* From the Broadwell PRM, RENDER_SURFACE_STATE.SurfaceQpitch:
    *
    *   This field must be set an integer multiple of the Surface Vertical
    *   Alignment.
    */
   assert(anv_is_aligned(qpitch, j));

   uint32_t stride = align_u32(mt_width * format->cpp, tile_info->width);
   if (create_info->stride > 0)
      stride = create_info->stride;

   const uint32_t size = stride * align_u32(mt_height, tile_info->height);
   const uint32_t offset = align_u32(*inout_image_size,
                                     tile_info->surface_alignment);

   *inout_image_size = offset + size;
   *inout_image_alignment = MAX(*inout_image_alignment,
                                tile_info->surface_alignment);

   *out_surface = (struct anv_surface) {
      .offset = offset,
      .stride = stride,
      .tile_mode = tile_mode,
      .qpitch = qpitch,
      .h_align = i,
      .v_align = j,
   };

   return VK_SUCCESS;
}

VkResult
anv_image_create(VkDevice _device,
                 const struct anv_image_create_info *create_info,
                 VkImage *pImage)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   const VkImageCreateInfo *pCreateInfo = create_info->vk_info;
   const VkExtent3D *restrict extent = &pCreateInfo->extent;
   struct anv_image *image = NULL;
   VkResult r;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);

   /* XXX: We don't handle any of these */
   anv_assert(pCreateInfo->imageType == VK_IMAGE_TYPE_2D);
   anv_assert(pCreateInfo->mipLevels > 0);
   anv_assert(pCreateInfo->arraySize > 0);
   anv_assert(pCreateInfo->samples == 1);
   anv_assert(pCreateInfo->extent.width > 0);
   anv_assert(pCreateInfo->extent.height > 0);
   anv_assert(pCreateInfo->extent.depth > 0);

   /* TODO(chadv): How should we validate inputs? */
   const uint8_t surf_type =
      anv_surf_type_from_image_type[pCreateInfo->imageType];

   const struct anv_surf_type_limits *limits =
      &anv_surf_type_limits[surf_type];

   if (extent->width > limits->width ||
       extent->height > limits->height ||
       extent->depth > limits->depth) {
      /* TODO(chadv): What is the correct error? */
      return vk_errorf(VK_ERROR_INVALID_MEMORY_SIZE, "image extent is too large");
   }

   image = anv_device_alloc(device, sizeof(*image), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (!image)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(image, 0, sizeof(*image));
   image->type = pCreateInfo->imageType;
   image->extent = pCreateInfo->extent;
   image->format = anv_format_for_vk_format(pCreateInfo->format);
   image->levels = pCreateInfo->mipLevels;
   image->array_size = pCreateInfo->arraySize;
   image->surf_type = surf_type;

   if (likely(anv_format_is_color(image->format))) {
      r = anv_image_make_surface(create_info, image->format,
                                 &image->size, &image->alignment,
                                 &image->color_surface);
      if (r != VK_SUCCESS)
         goto fail;
   } else {
      if (image->format->depth_format) {
         r = anv_image_make_surface(create_info, image->format,
                                    &image->size, &image->alignment,
                                    &image->depth_surface);
         if (r != VK_SUCCESS)
            goto fail;
      }

      if (image->format->has_stencil) {
         r = anv_image_make_surface(create_info, anv_format_s8_uint,
                                    &image->size, &image->alignment,
                                    &image->stencil_surface);
         if (r != VK_SUCCESS)
            goto fail;
      }
   }

   *pImage = anv_image_to_handle(image);

   return VK_SUCCESS;

fail:
   if (image)
      anv_device_free(device, image);

   return r;
}

VkResult
anv_CreateImage(VkDevice device,
                const VkImageCreateInfo *pCreateInfo,
                VkImage *pImage)
{
   return anv_image_create(device,
      &(struct anv_image_create_info) {
         .vk_info = pCreateInfo,
      },
      pImage);
}

VkResult
anv_DestroyImage(VkDevice _device, VkImage _image)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   anv_device_free(device, anv_image_from_handle(_image));

   return VK_SUCCESS;
}

VkResult anv_GetImageSubresourceLayout(
    VkDevice                                    device,
    VkImage                                     image,
    const VkImageSubresource*                   pSubresource,
    VkSubresourceLayout*                        pLayout)
{
   stub_return(VK_UNSUPPORTED);
}

void
anv_surface_view_fini(struct anv_device *device,
                      struct anv_surface_view *view)
{
   anv_state_pool_free(&device->surface_state_pool, view->surface_state);
}

VkResult
anv_validate_CreateImageView(VkDevice _device,
                             const VkImageViewCreateInfo *pCreateInfo,
                             VkImageView *pView)
{
   ANV_FROM_HANDLE(anv_image, image, pCreateInfo->image);
   const VkImageSubresourceRange *subresource;
   const struct anv_image_view_info *view_info;
   const struct anv_format *view_format_info;

   /* Validate structure type before dereferencing it. */
   assert(pCreateInfo);
   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
   subresource = &pCreateInfo->subresourceRange;

   /* Validate viewType is in range before using it. */
   assert(pCreateInfo->viewType >= VK_IMAGE_VIEW_TYPE_BEGIN_RANGE);
   assert(pCreateInfo->viewType <= VK_IMAGE_VIEW_TYPE_END_RANGE);
   view_info = &anv_image_view_info_table[pCreateInfo->viewType];

   /* Validate format is in range before using it. */
   assert(pCreateInfo->format >= VK_FORMAT_BEGIN_RANGE);
   assert(pCreateInfo->format <= VK_FORMAT_END_RANGE);
   view_format_info = anv_format_for_vk_format(pCreateInfo->format);

   /* Validate channel swizzles. */
   assert(pCreateInfo->channels.r >= VK_CHANNEL_SWIZZLE_BEGIN_RANGE);
   assert(pCreateInfo->channels.r <= VK_CHANNEL_SWIZZLE_END_RANGE);
   assert(pCreateInfo->channels.g >= VK_CHANNEL_SWIZZLE_BEGIN_RANGE);
   assert(pCreateInfo->channels.g <= VK_CHANNEL_SWIZZLE_END_RANGE);
   assert(pCreateInfo->channels.b >= VK_CHANNEL_SWIZZLE_BEGIN_RANGE);
   assert(pCreateInfo->channels.b <= VK_CHANNEL_SWIZZLE_END_RANGE);
   assert(pCreateInfo->channels.a >= VK_CHANNEL_SWIZZLE_BEGIN_RANGE);
   assert(pCreateInfo->channels.a <= VK_CHANNEL_SWIZZLE_END_RANGE);

   /* Validate subresource. */
   assert(subresource->aspect >= VK_IMAGE_ASPECT_BEGIN_RANGE);
   assert(subresource->aspect <= VK_IMAGE_ASPECT_END_RANGE);
   assert(subresource->mipLevels > 0);
   assert(subresource->arraySize > 0);
   assert(subresource->baseMipLevel < image->levels);
   assert(subresource->baseMipLevel + subresource->mipLevels <= image->levels);
   assert(subresource->baseArraySlice < image->array_size);
   assert(subresource->baseArraySlice + subresource->arraySize <= image->array_size);
   assert(pView);

   if (view_info->is_cube) {
      assert(subresource->baseArraySlice % 6 == 0);
      assert(subresource->arraySize % 6 == 0);
   }

   /* Validate format. */
   switch (subresource->aspect) {
   case VK_IMAGE_ASPECT_COLOR:
      assert(!image->format->depth_format);
      assert(!image->format->has_stencil);
      assert(!view_format_info->depth_format);
      assert(!view_format_info->has_stencil);
      assert(view_format_info->cpp == image->format->cpp);
      break;
   case VK_IMAGE_ASPECT_DEPTH:
      assert(image->format->depth_format);
      assert(view_format_info->depth_format);
      assert(view_format_info->cpp == image->format->cpp);
      break;
   case VK_IMAGE_ASPECT_STENCIL:
      /* FINISHME: Is it legal to have an R8 view of S8? */
      assert(image->format->has_stencil);
      assert(view_format_info->has_stencil);
      break;
   default:
      assert(!"bad VkImageAspect");
      break;
   }

   return anv_CreateImageView(_device, pCreateInfo, pView);
}

void
anv_image_view_init(struct anv_image_view *iview,
                    struct anv_device *device,
                    const VkImageViewCreateInfo* pCreateInfo,
                    struct anv_cmd_buffer *cmd_buffer)
{
   switch (device->info.gen) {
   case 7:
      gen7_image_view_init(iview, device, pCreateInfo, cmd_buffer);
      break;
   case 8:
      gen8_image_view_init(iview, device, pCreateInfo, cmd_buffer);
      break;
   default:
      unreachable("unsupported gen\n");
   }
}

VkResult
anv_CreateImageView(VkDevice _device,
                    const VkImageViewCreateInfo *pCreateInfo,
                    VkImageView *pView)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_image_view *view;

   view = anv_device_alloc(device, sizeof(*view), 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (view == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   anv_image_view_init(view, device, pCreateInfo, NULL);

   *pView = anv_image_view_to_handle(view);

   return VK_SUCCESS;
}

VkResult
anv_DestroyImageView(VkDevice _device, VkImageView _iview)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_image_view, iview, _iview);

   anv_surface_view_fini(device, &iview->view);
   anv_device_free(device, iview);

   return VK_SUCCESS;
}

static void
anv_depth_stencil_view_init(struct anv_depth_stencil_view *view,
                            const VkAttachmentViewCreateInfo *pCreateInfo)
{
   ANV_FROM_HANDLE(anv_image, image, pCreateInfo->image);

   view->base.attachment_type = ANV_ATTACHMENT_VIEW_TYPE_DEPTH_STENCIL;

   /* XXX: We don't handle any of these */
   anv_assert(pCreateInfo->mipLevel == 0);
   anv_assert(pCreateInfo->baseArraySlice == 0);
   anv_assert(pCreateInfo->arraySize == 1);

   view->image = image;
   view->format = anv_format_for_vk_format(pCreateInfo->format);

   assert(anv_format_is_depth_or_stencil(image->format));
   assert(anv_format_is_depth_or_stencil(view->format));
}

struct anv_surface *
anv_image_get_surface_for_aspect(struct anv_image *image, VkImageAspect aspect)
{
   switch (aspect) {
   case VK_IMAGE_ASPECT_COLOR:
      assert(anv_format_is_color(image->format));
      return &image->color_surface;
   case VK_IMAGE_ASPECT_DEPTH:
      assert(image->format->depth_format);
      return &image->depth_surface;
   case VK_IMAGE_ASPECT_STENCIL:
      assert(image->format->has_stencil);
      anv_finishme("stencil image views");
      return &image->stencil_surface;
    default:
       unreachable("image does not have aspect");
       return NULL;
   }
}

/** The attachment may be a color view into a non-color image.  */
struct anv_surface *
anv_image_get_surface_for_color_attachment(struct anv_image *image)
{
   if (anv_format_is_color(image->format)) {
      return &image->color_surface;
   } else if (image->format->depth_format) {
      return &image->depth_surface;
   } else if (image->format->has_stencil) {
      return &image->stencil_surface;
   } else {
      unreachable("image has bad format");
      return NULL;
   }
}

void
anv_color_attachment_view_init(struct anv_color_attachment_view *aview,
                               struct anv_device *device,
                               const VkAttachmentViewCreateInfo* pCreateInfo,
                               struct anv_cmd_buffer *cmd_buffer)
{
   switch (device->info.gen) {
   case 7:
      gen7_color_attachment_view_init(aview, device, pCreateInfo, cmd_buffer);
      break;
   case 8:
      gen8_color_attachment_view_init(aview, device, pCreateInfo, cmd_buffer);
      break;
   default:
      unreachable("unsupported gen\n");
   }
}

VkResult
anv_CreateAttachmentView(VkDevice _device,
                         const VkAttachmentViewCreateInfo *pCreateInfo,
                         VkAttachmentView *pView)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_ATTACHMENT_VIEW_CREATE_INFO);

   const struct anv_format *format =
      anv_format_for_vk_format(pCreateInfo->format);

   if (anv_format_is_depth_or_stencil(format)) {
      struct anv_depth_stencil_view *view =
         anv_device_alloc(device, sizeof(*view), 8,
                          VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
      if (view == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      anv_depth_stencil_view_init(view, pCreateInfo);

      *pView = anv_attachment_view_to_handle(&view->base);
   } else {
      struct anv_color_attachment_view *view =
         anv_device_alloc(device, sizeof(*view), 8,
                          VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
      if (view == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      anv_color_attachment_view_init(view, device, pCreateInfo, NULL);

      *pView = anv_attachment_view_to_handle(&view->base);
   }

   return VK_SUCCESS;
}

VkResult
anv_DestroyAttachmentView(VkDevice _device, VkAttachmentView _view)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_attachment_view, view, _view);

   if (view->attachment_type == ANV_ATTACHMENT_VIEW_TYPE_COLOR) {
      struct anv_color_attachment_view *aview =
         (struct anv_color_attachment_view *)view;

      anv_surface_view_fini(device, &aview->view);
   }

   anv_device_free(device, view);

   return VK_SUCCESS;
}
