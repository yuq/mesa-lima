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

/**
 * The \a format argument is required and overrides any format found in struct
 * anv_image_create_info. Exactly one bit must be set in \a aspect.
 */
static isl_surf_usage_flags_t
choose_isl_surf_usage(const struct anv_image_create_info *info,
                      VkImageAspectFlags aspect)
{
   const VkImageCreateInfo *vk_info = info->vk_info;
   isl_surf_usage_flags_t isl_flags = 0;

   /* FINISHME: Support aux surfaces */
   isl_flags |= ISL_SURF_USAGE_DISABLE_AUX_BIT;

   if (vk_info->usage & VK_IMAGE_USAGE_SAMPLED_BIT)
      isl_flags |= ISL_SURF_USAGE_TEXTURE_BIT;

   if (vk_info->usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)
      isl_flags |= ISL_SURF_USAGE_TEXTURE_BIT;

   if (vk_info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      isl_flags |= ISL_SURF_USAGE_RENDER_TARGET_BIT;

   if (vk_info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
      isl_flags |= ISL_SURF_USAGE_CUBE_BIT;

   if (vk_info->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      switch (aspect) {
      default:
         unreachable("bad VkImageAspect");
      case VK_IMAGE_ASPECT_DEPTH_BIT:
         isl_flags |= ISL_SURF_USAGE_DEPTH_BIT;
         break;
      case VK_IMAGE_ASPECT_STENCIL_BIT:
         isl_flags |= ISL_SURF_USAGE_STENCIL_BIT;
         break;
      }
   }

   if (vk_info->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
      /* Meta implements transfers by sampling from the source image. */
      isl_flags |= ISL_SURF_USAGE_TEXTURE_BIT;
   }

   if (vk_info->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
      /* Meta implements transfers by rendering into the destination image. */
      isl_flags |= ISL_SURF_USAGE_RENDER_TARGET_BIT;
   }

   return isl_flags;
}

/**
 * Exactly one bit must be set in \a aspect.
 */
static struct anv_surface *
get_surface(struct anv_image *image, VkImageAspectFlags aspect)
{
   switch (aspect) {
   default:
      unreachable("bad VkImageAspect");
   case VK_IMAGE_ASPECT_COLOR_BIT:
      return &image->color_surface;
   case VK_IMAGE_ASPECT_DEPTH_BIT:
      return &image->depth_surface;
   case VK_IMAGE_ASPECT_STENCIL_BIT:
      return &image->stencil_surface;
   }
}

/**
 * Initialize the anv_image::*_surface selected by \a aspect. Then update the
 * image's memory requirements (that is, the image's size and alignment).
 *
 * Exactly one bit must be set in \a aspect.
 */
static VkResult
make_surface(const struct anv_device *dev,
             struct anv_image *image,
             const struct anv_image_create_info *anv_info,
             VkImageAspectFlags aspect)
{
   const VkImageCreateInfo *vk_info = anv_info->vk_info;
   bool ok UNUSED;

   static const enum isl_surf_dim vk_to_isl_surf_dim[] = {
      [VK_IMAGE_TYPE_1D] = ISL_SURF_DIM_1D,
      [VK_IMAGE_TYPE_2D] = ISL_SURF_DIM_2D,
      [VK_IMAGE_TYPE_3D] = ISL_SURF_DIM_3D,
   };

   isl_tiling_flags_t tiling_flags = anv_info->isl_tiling_flags;
   if (vk_info->tiling == VK_IMAGE_TILING_LINEAR)
      tiling_flags &= ISL_TILING_LINEAR_BIT;

   struct anv_surface *anv_surf = get_surface(image, aspect);

   ok = isl_surf_init(&dev->isl_dev, &anv_surf->isl,
      .dim = vk_to_isl_surf_dim[vk_info->imageType],
      .format = anv_get_isl_format(vk_info->format, aspect,
                                   vk_info->tiling, NULL),
      .width = vk_info->extent.width,
      .height = vk_info->extent.height,
      .depth = vk_info->extent.depth,
      .levels = vk_info->mipLevels,
      .array_len = vk_info->arrayLayers,
      .samples = vk_info->samples,
      .min_alignment = 0,
      .min_pitch = 0,
      .usage = choose_isl_surf_usage(anv_info, aspect),
      .tiling_flags = tiling_flags);

   /* isl_surf_init() will fail only if provided invalid input. Invalid input
    * is illegal in Vulkan.
    */
   assert(ok);

   anv_surf->offset = align_u32(image->size, anv_surf->isl.alignment);
   image->size = anv_surf->offset + anv_surf->isl.size;
   image->alignment = MAX(image->alignment, anv_surf->isl.alignment);

   return VK_SUCCESS;
}

static VkImageUsageFlags
anv_image_get_full_usage(const VkImageCreateInfo *info)
{
   VkImageUsageFlags usage = info->usage;

   if (info->samples > 1 &&
       (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
      /* Meta will resolve the image by binding it as a texture. */
      usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
   }

   if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
      /* Meta will transfer from the image by binding it as a texture. */
      usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
   }

   if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
      /* Meta will transfer to the image by binding it as a color attachment,
       * even if the image format is not a color format.
       */
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   }

   return usage;
}

VkResult
anv_image_create(VkDevice _device,
                 const struct anv_image_create_info *create_info,
                 const VkAllocationCallbacks* alloc,
                 VkImage *pImage)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   const VkImageCreateInfo *pCreateInfo = create_info->vk_info;
   struct anv_image *image = NULL;
   VkResult r;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);

   anv_assert(pCreateInfo->mipLevels > 0);
   anv_assert(pCreateInfo->arrayLayers > 0);
   anv_assert(pCreateInfo->samples > 0);
   anv_assert(pCreateInfo->extent.width > 0);
   anv_assert(pCreateInfo->extent.height > 0);
   anv_assert(pCreateInfo->extent.depth > 0);

   image = anv_alloc2(&device->alloc, alloc, sizeof(*image), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!image)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(image, 0, sizeof(*image));
   image->type = pCreateInfo->imageType;
   image->extent = pCreateInfo->extent;
   image->vk_format = pCreateInfo->format;
   image->format = anv_format_for_vk_format(pCreateInfo->format);
   image->levels = pCreateInfo->mipLevels;
   image->array_size = pCreateInfo->arrayLayers;
   image->samples = pCreateInfo->samples;
   image->usage = anv_image_get_full_usage(pCreateInfo);
   image->tiling = pCreateInfo->tiling;

   if (likely(anv_format_is_color(image->format))) {
      r = make_surface(device, image, create_info,
                       VK_IMAGE_ASPECT_COLOR_BIT);
      if (r != VK_SUCCESS)
         goto fail;
   } else {
      if (image->format->has_depth) {
         r = make_surface(device, image, create_info,
                          VK_IMAGE_ASPECT_DEPTH_BIT);
         if (r != VK_SUCCESS)
            goto fail;
      }

      if (image->format->has_stencil) {
         r = make_surface(device, image, create_info,
                          VK_IMAGE_ASPECT_STENCIL_BIT);
         if (r != VK_SUCCESS)
            goto fail;
      }
   }

   *pImage = anv_image_to_handle(image);

   return VK_SUCCESS;

fail:
   if (image)
      anv_free2(&device->alloc, alloc, image);

   return r;
}

VkResult
anv_CreateImage(VkDevice device,
                const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkImage *pImage)
{
   return anv_image_create(device,
      &(struct anv_image_create_info) {
         .vk_info = pCreateInfo,
         .isl_tiling_flags = ISL_TILING_ANY_MASK,
      },
      pAllocator,
      pImage);
}

void
anv_DestroyImage(VkDevice _device, VkImage _image,
                 const VkAllocationCallbacks *pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   anv_free2(&device->alloc, pAllocator, anv_image_from_handle(_image));
}

static void
anv_surface_get_subresource_layout(struct anv_image *image,
                                   struct anv_surface *surface,
                                   const VkImageSubresource *subresource,
                                   VkSubresourceLayout *layout)
{
   /* If we are on a non-zero mip level or array slice, we need to
    * calculate a real offset.
    */
   anv_assert(subresource->mipLevel == 0);
   anv_assert(subresource->arrayLayer == 0);

   layout->offset = surface->offset;
   layout->rowPitch = surface->isl.row_pitch;
   layout->depthPitch = isl_surf_get_array_pitch(&surface->isl);
   layout->arrayPitch = isl_surf_get_array_pitch(&surface->isl);
   layout->size = surface->isl.size;
}

void anv_GetImageSubresourceLayout(
    VkDevice                                    device,
    VkImage                                     _image,
    const VkImageSubresource*                   pSubresource,
    VkSubresourceLayout*                        pLayout)
{
   ANV_FROM_HANDLE(anv_image, image, _image);

   assert(__builtin_popcount(pSubresource->aspectMask) == 1);

   switch (pSubresource->aspectMask) {
   case VK_IMAGE_ASPECT_COLOR_BIT:
      anv_surface_get_subresource_layout(image, &image->color_surface,
                                         pSubresource, pLayout);
      break;
   case VK_IMAGE_ASPECT_DEPTH_BIT:
      anv_surface_get_subresource_layout(image, &image->depth_surface,
                                         pSubresource, pLayout);
      break;
   case VK_IMAGE_ASPECT_STENCIL_BIT:
      anv_surface_get_subresource_layout(image, &image->stencil_surface,
                                         pSubresource, pLayout);
      break;
   default:
      assert(!"Invalid image aspect");
   }
}

VkResult
anv_validate_CreateImageView(VkDevice _device,
                             const VkImageViewCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkImageView *pView)
{
   ANV_FROM_HANDLE(anv_image, image, pCreateInfo->image);
   const VkImageSubresourceRange *subresource;
   const struct anv_format *view_format_info;

   /* Validate structure type before dereferencing it. */
   assert(pCreateInfo);
   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
   subresource = &pCreateInfo->subresourceRange;

   /* Validate viewType is in range before using it. */
   assert(pCreateInfo->viewType >= VK_IMAGE_VIEW_TYPE_BEGIN_RANGE);
   assert(pCreateInfo->viewType <= VK_IMAGE_VIEW_TYPE_END_RANGE);

   /* Validate format is in range before using it. */
   assert(pCreateInfo->format >= VK_FORMAT_BEGIN_RANGE);
   assert(pCreateInfo->format <= VK_FORMAT_END_RANGE);
   view_format_info = anv_format_for_vk_format(pCreateInfo->format);

   /* Validate channel swizzles. */
   assert(pCreateInfo->components.r >= VK_COMPONENT_SWIZZLE_BEGIN_RANGE);
   assert(pCreateInfo->components.r <= VK_COMPONENT_SWIZZLE_END_RANGE);
   assert(pCreateInfo->components.g >= VK_COMPONENT_SWIZZLE_BEGIN_RANGE);
   assert(pCreateInfo->components.g <= VK_COMPONENT_SWIZZLE_END_RANGE);
   assert(pCreateInfo->components.b >= VK_COMPONENT_SWIZZLE_BEGIN_RANGE);
   assert(pCreateInfo->components.b <= VK_COMPONENT_SWIZZLE_END_RANGE);
   assert(pCreateInfo->components.a >= VK_COMPONENT_SWIZZLE_BEGIN_RANGE);
   assert(pCreateInfo->components.a <= VK_COMPONENT_SWIZZLE_END_RANGE);

   /* Validate subresource. */
   assert(subresource->aspectMask != 0);
   assert(subresource->levelCount > 0);
   assert(subresource->layerCount > 0);
   assert(subresource->baseMipLevel < image->levels);
   assert(subresource->baseMipLevel + subresource->levelCount <= image->levels);
   assert(subresource->baseArrayLayer < image->array_size);
   assert(subresource->baseArrayLayer + subresource->layerCount <= image->array_size);
   assert(pView);

   const VkImageAspectFlags ds_flags = VK_IMAGE_ASPECT_DEPTH_BIT
                                     | VK_IMAGE_ASPECT_STENCIL_BIT;

   /* Validate format. */
   if (subresource->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
      assert(subresource->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
      assert(!image->format->has_depth);
      assert(!image->format->has_stencil);
      assert(!view_format_info->has_depth);
      assert(!view_format_info->has_stencil);
      assert(view_format_info->isl_layout->bs ==
             image->format->isl_layout->bs);
   } else if (subresource->aspectMask & ds_flags) {
      assert((subresource->aspectMask & ~ds_flags) == 0);

      if (subresource->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
         assert(image->format->has_depth);
         assert(view_format_info->has_depth);
         assert(view_format_info->isl_layout->bs ==
                image->format->isl_layout->bs);
      }

      if (subresource->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
         /* FINISHME: Is it legal to have an R8 view of S8? */
         assert(image->format->has_stencil);
         assert(view_format_info->has_stencil);
      }
   } else {
      assert(!"bad VkImageSubresourceRange::aspectFlags");
   }

   return anv_CreateImageView(_device, pCreateInfo, pAllocator, pView);
}

void
anv_fill_image_surface_state(struct anv_device *device, struct anv_state state,
                             struct anv_image_view *iview,
                             const VkImageViewCreateInfo *pCreateInfo,
                             VkImageUsageFlagBits usage)
{
   switch (device->info.gen) {
   case 7:
      if (device->info.is_haswell)
         gen75_fill_image_surface_state(device, state.map, iview,
                                        pCreateInfo, usage);
      else
         gen7_fill_image_surface_state(device, state.map, iview,
                                       pCreateInfo, usage);
      break;
   case 8:
      gen8_fill_image_surface_state(device, state.map, iview,
                                    pCreateInfo, usage);
      break;
   case 9:
      gen9_fill_image_surface_state(device, state.map, iview,
                                    pCreateInfo, usage);
      break;
   default:
      unreachable("unsupported gen\n");
   }

   if (!device->info.has_llc)
      anv_state_clflush(state);
}

static struct anv_state
alloc_surface_state(struct anv_device *device,
                    struct anv_cmd_buffer *cmd_buffer)
{
      if (cmd_buffer) {
         return anv_cmd_buffer_alloc_surface_state(cmd_buffer);
      } else {
         return anv_state_pool_alloc(&device->surface_state_pool, 64, 64);
      }
}

static bool
has_matching_storage_typed_format(const struct anv_device *device,
                                  enum isl_format format)
{
   return (isl_format_get_layout(format)->bs <= 4 ||
           (isl_format_get_layout(format)->bs <= 8 &&
            (device->info.gen >= 8 || device->info.is_haswell)) ||
           device->info.gen >= 9);
}

static VkComponentSwizzle
remap_swizzle(VkComponentSwizzle swizzle, VkComponentSwizzle component,
              struct anv_format_swizzle format_swizzle)
{
   if (swizzle == VK_COMPONENT_SWIZZLE_IDENTITY)
      swizzle = component;

   switch (swizzle) {
   case VK_COMPONENT_SWIZZLE_ZERO:
      return VK_COMPONENT_SWIZZLE_ZERO;
   case VK_COMPONENT_SWIZZLE_ONE:
      return VK_COMPONENT_SWIZZLE_ONE;
   case VK_COMPONENT_SWIZZLE_R:
      return VK_COMPONENT_SWIZZLE_R + format_swizzle.r;
   case VK_COMPONENT_SWIZZLE_G:
      return VK_COMPONENT_SWIZZLE_R + format_swizzle.g;
   case VK_COMPONENT_SWIZZLE_B:
      return VK_COMPONENT_SWIZZLE_R + format_swizzle.b;
   case VK_COMPONENT_SWIZZLE_A:
      return VK_COMPONENT_SWIZZLE_R + format_swizzle.a;
   default:
      unreachable("Invalid swizzle");
   }
}

void
anv_image_view_init(struct anv_image_view *iview,
                    struct anv_device *device,
                    const VkImageViewCreateInfo* pCreateInfo,
                    struct anv_cmd_buffer *cmd_buffer,
                    uint32_t offset)
{
   ANV_FROM_HANDLE(anv_image, image, pCreateInfo->image);
   const VkImageSubresourceRange *range = &pCreateInfo->subresourceRange;
   VkImageViewCreateInfo mCreateInfo;
   memcpy(&mCreateInfo, pCreateInfo, sizeof(VkImageViewCreateInfo));

   assert(range->layerCount > 0);
   assert(range->baseMipLevel < image->levels);
   assert(image->usage & (VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));

   switch (image->type) {
   default:
      unreachable("bad VkImageType");
   case VK_IMAGE_TYPE_1D:
   case VK_IMAGE_TYPE_2D:
      assert(range->baseArrayLayer + range->layerCount - 1 <= image->array_size);
      break;
   case VK_IMAGE_TYPE_3D:
      assert(range->baseArrayLayer + range->layerCount - 1
             <= anv_minify(image->extent.depth, range->baseMipLevel));
      break;
   }

   struct anv_surface *surface =
      anv_image_get_surface_for_aspect_mask(image, range->aspectMask);

   iview->image = image;
   iview->bo = image->bo;
   iview->offset = image->offset + surface->offset + offset;

   iview->aspect_mask = pCreateInfo->subresourceRange.aspectMask;
   iview->vk_format = pCreateInfo->format;

   struct anv_format_swizzle swizzle;
   iview->format = anv_get_isl_format(pCreateInfo->format, iview->aspect_mask,
                                      image->tiling, &swizzle);
   iview->swizzle.r = remap_swizzle(pCreateInfo->components.r,
                                    VK_COMPONENT_SWIZZLE_R, swizzle);
   iview->swizzle.g = remap_swizzle(pCreateInfo->components.g,
                                    VK_COMPONENT_SWIZZLE_G, swizzle);
   iview->swizzle.b = remap_swizzle(pCreateInfo->components.b,
                                    VK_COMPONENT_SWIZZLE_B, swizzle);
   iview->swizzle.a = remap_swizzle(pCreateInfo->components.a,
                                    VK_COMPONENT_SWIZZLE_A, swizzle);

   iview->base_layer = range->baseArrayLayer;
   iview->base_mip = range->baseMipLevel;

   if (!isl_format_is_compressed(iview->format) &&
       isl_format_is_compressed(image->format->isl_format)) {
      /* Scale the ImageView extent by the backing Image. This is used
       * internally when an uncompressed ImageView is created on a
       * compressed Image. The ImageView can therefore be used for copying
       * data from a source Image to a destination Image.
       */
      const struct isl_format_layout * isl_layout = image->format->isl_layout;

      iview->level_0_extent.depth  = anv_minify(image->extent.depth, range->baseMipLevel);
      iview->level_0_extent.depth  = DIV_ROUND_UP(iview->level_0_extent.depth, isl_layout->bd);

      iview->level_0_extent.height = isl_surf_get_array_pitch_el_rows(&surface->isl) * image->array_size;
      iview->level_0_extent.width  = isl_surf_get_row_pitch_el(&surface->isl);
      mCreateInfo.subresourceRange.baseMipLevel = 0;
      mCreateInfo.subresourceRange.baseArrayLayer = 0;
   } else {
      iview->level_0_extent.width  = image->extent.width;
      iview->level_0_extent.height = image->extent.height;
      iview->level_0_extent.depth  = image->extent.depth;
   }

   iview->extent = (VkExtent3D) {
      .width  = anv_minify(iview->level_0_extent.width , range->baseMipLevel),
      .height = anv_minify(iview->level_0_extent.height, range->baseMipLevel),
      .depth  = anv_minify(iview->level_0_extent.depth , range->baseMipLevel),
   };

   if (image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
      iview->sampler_surface_state = alloc_surface_state(device, cmd_buffer);

      anv_fill_image_surface_state(device, iview->sampler_surface_state,
                                   iview, &mCreateInfo,
                                   VK_IMAGE_USAGE_SAMPLED_BIT);
   } else {
      iview->sampler_surface_state.alloc_size = 0;
   }

   if (image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      iview->color_rt_surface_state = alloc_surface_state(device, cmd_buffer);

      anv_fill_image_surface_state(device, iview->color_rt_surface_state,
                                   iview, &mCreateInfo,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
   } else {
      iview->color_rt_surface_state.alloc_size = 0;
   }

   if (image->usage & VK_IMAGE_USAGE_STORAGE_BIT) {
      iview->storage_surface_state = alloc_surface_state(device, cmd_buffer);

      if (has_matching_storage_typed_format(device, iview->format))
         anv_fill_image_surface_state(device, iview->storage_surface_state,
                                      iview, &mCreateInfo,
                                      VK_IMAGE_USAGE_STORAGE_BIT);
      else
         anv_fill_buffer_surface_state(device, iview->storage_surface_state,
                                       ISL_FORMAT_RAW,
                                       iview->offset,
                                       iview->bo->size - iview->offset, 1);

   } else {
      iview->storage_surface_state.alloc_size = 0;
   }
}

VkResult
anv_CreateImageView(VkDevice _device,
                    const VkImageViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkImageView *pView)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_image_view *view;

   view = anv_alloc2(&device->alloc, pAllocator, sizeof(*view), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (view == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   anv_image_view_init(view, device, pCreateInfo, NULL, 0);

   *pView = anv_image_view_to_handle(view);

   return VK_SUCCESS;
}

void
anv_DestroyImageView(VkDevice _device, VkImageView _iview,
                     const VkAllocationCallbacks *pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_image_view, iview, _iview);

   if (iview->color_rt_surface_state.alloc_size > 0) {
      anv_state_pool_free(&device->surface_state_pool,
                          iview->color_rt_surface_state);
   }

   if (iview->sampler_surface_state.alloc_size > 0) {
      anv_state_pool_free(&device->surface_state_pool,
                          iview->sampler_surface_state);
   }

   if (iview->storage_surface_state.alloc_size > 0) {
      anv_state_pool_free(&device->surface_state_pool,
                          iview->storage_surface_state);
   }

   anv_free2(&device->alloc, pAllocator, iview);
}

VkResult
anv_CreateBufferView(VkDevice _device,
                     const VkBufferViewCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkBufferView *pView)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_buffer, buffer, pCreateInfo->buffer);
   struct anv_buffer_view *view;

   view = anv_alloc2(&device->alloc, pAllocator, sizeof(*view), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!view)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   const struct anv_format *format =
      anv_format_for_vk_format(pCreateInfo->format);

   view->format = format->isl_format;
   view->bo = buffer->bo;
   view->offset = buffer->offset + pCreateInfo->offset;
   view->range = pCreateInfo->range == VK_WHOLE_SIZE ?
                 buffer->size - view->offset : pCreateInfo->range;

   if (buffer->usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT) {
      view->surface_state =
         anv_state_pool_alloc(&device->surface_state_pool, 64, 64);

      anv_fill_buffer_surface_state(device, view->surface_state,
                                    view->format,
                                    view->offset, view->range,
                                    format->isl_layout->bs);
   } else {
      view->surface_state = (struct anv_state){ 0 };
   }

   if (buffer->usage & VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT) {
      view->storage_surface_state =
         anv_state_pool_alloc(&device->surface_state_pool, 64, 64);

      enum isl_format storage_format =
         has_matching_storage_typed_format(device, view->format) ?
         isl_lower_storage_image_format(&device->isl_dev, view->format) :
         ISL_FORMAT_RAW;

      anv_fill_buffer_surface_state(device, view->storage_surface_state,
                                    storage_format,
                                    view->offset, view->range,
                                    (storage_format == ISL_FORMAT_RAW ? 1 :
                                     format->isl_layout->bs));

   } else {
      view->storage_surface_state = (struct anv_state){ 0 };
   }

   *pView = anv_buffer_view_to_handle(view);

   return VK_SUCCESS;
}

void
anv_DestroyBufferView(VkDevice _device, VkBufferView bufferView,
                      const VkAllocationCallbacks *pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_buffer_view, view, bufferView);

   if (view->surface_state.alloc_size > 0)
      anv_state_pool_free(&device->surface_state_pool,
                          view->surface_state);

   if (view->storage_surface_state.alloc_size > 0)
      anv_state_pool_free(&device->surface_state_pool,
                          view->storage_surface_state);

   anv_free2(&device->alloc, pAllocator, view);
}

struct anv_surface *
anv_image_get_surface_for_aspect_mask(struct anv_image *image, VkImageAspectFlags aspect_mask)
{
   switch (aspect_mask) {
   case VK_IMAGE_ASPECT_COLOR_BIT:
      /* Dragons will eat you.
       *
       * Meta attaches all destination surfaces as color render targets. Guess
       * what surface the Meta Dragons really want.
       */
      if (image->format->has_depth && image->format->has_stencil) {
         return &image->depth_surface;
      } else if (image->format->has_depth) {
         return &image->depth_surface;
      } else if (image->format->has_stencil) {
         return &image->stencil_surface;
      } else {
         return &image->color_surface;
      }
      break;
   case VK_IMAGE_ASPECT_DEPTH_BIT:
      assert(image->format->has_depth);
      return &image->depth_surface;
   case VK_IMAGE_ASPECT_STENCIL_BIT:
      assert(image->format->has_stencil);
      return &image->stencil_surface;
   case VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT:
      if (image->format->has_depth && image->format->has_stencil) {
         /* FINISHME: The Vulkan spec (git a511ba2) requires support for
          * combined depth stencil formats. Specifically, it states:
          *
          *    At least one of ename:VK_FORMAT_D24_UNORM_S8_UINT or
          *    ename:VK_FORMAT_D32_SFLOAT_S8_UINT must be supported.
          *
          * Image views with both depth and stencil aspects are only valid for
          * render target attachments, in which case
          * cmd_buffer_emit_depth_stencil() will pick out both the depth and
          * stencil surfaces from the underlying surface.
          */
         return &image->depth_surface;
      } else if (image->format->has_depth) {
         return &image->depth_surface;
      } else if (image->format->has_stencil) {
         return &image->stencil_surface;
      }
      /* fallthrough */
    default:
       unreachable("image does not have aspect");
       return NULL;
   }
}

static void
image_param_defaults(struct brw_image_param *param)
{
   memset(param, 0, sizeof *param);
   /* Set the swizzling shifts to all-ones to effectively disable swizzling --
    * See emit_address_calculation() in brw_fs_surface_builder.cpp for a more
    * detailed explanation of these parameters.
    */
   param->swizzling[0] = 0xff;
   param->swizzling[1] = 0xff;
}

void
anv_image_view_fill_image_param(struct anv_device *device,
                                struct anv_image_view *view,
                                struct brw_image_param *param)
{
   image_param_defaults(param);

   const struct isl_surf *surf = &view->image->color_surface.isl;
   const int cpp = isl_format_get_layout(surf->format)->bs;
   const struct isl_extent3d image_align_sa =
      isl_surf_get_image_alignment_sa(surf);

   param->size[0] = view->extent.width;
   param->size[1] = view->extent.height;
   if (surf->dim == ISL_SURF_DIM_3D) {
      param->size[2] = view->extent.depth;
   } else {
      param->size[2] = surf->logical_level0_px.array_len - view->base_layer;
   }

   isl_surf_get_image_offset_el(surf, view->base_mip, view->base_layer, 0,
                                &param->offset[0],  &param->offset[1]);

   param->stride[0] = cpp;
   param->stride[1] = surf->row_pitch / cpp;

   if (device->info.gen < 9 && surf->dim == ISL_SURF_DIM_3D) {
      param->stride[2] = util_align_npot(param->size[0], image_align_sa.w);
      param->stride[3] = util_align_npot(param->size[1], image_align_sa.h);
   } else {
      param->stride[2] = 0;
      param->stride[3] = isl_surf_get_array_pitch_el_rows(surf);
   }

   switch (surf->tiling) {
   case ISL_TILING_LINEAR:
      /* image_param_defaults is good enough */
      break;

   case ISL_TILING_X:
      /* An X tile is a rectangular block of 512x8 bytes. */
      param->tiling[0] = util_logbase2(512 / cpp);
      param->tiling[1] = util_logbase2(8);

      if (device->isl_dev.has_bit6_swizzling) {
         /* Right shifts required to swizzle bits 9 and 10 of the memory
          * address with bit 6.
          */
         param->swizzling[0] = 3;
         param->swizzling[1] = 4;
      }
      break;

   case ISL_TILING_Y0:
      /* The layout of a Y-tiled surface in memory isn't really fundamentally
       * different to the layout of an X-tiled surface, we simply pretend that
       * the surface is broken up in a number of smaller 16Bx32 tiles, each
       * one arranged in X-major order just like is the case for X-tiling.
       */
      param->tiling[0] = util_logbase2(16 / cpp);
      param->tiling[1] = util_logbase2(32);

      if (device->isl_dev.has_bit6_swizzling) {
         /* Right shift required to swizzle bit 9 of the memory address with
          * bit 6.
          */
         param->swizzling[0] = 3;
         param->swizzling[1] = 0xff;
      }
      break;

   default:
      assert(!"Unhandled storage image tiling");
   }

   /* 3D textures are arranged in 2D in memory with 2^lod slices per row.  The
    * address calculation algorithm (emit_address_calculation() in
    * brw_fs_surface_builder.cpp) handles this as a sort of tiling with
    * modulus equal to the LOD.
    */
   param->tiling[2] = (device->info.gen < 9 && surf->dim == ISL_SURF_DIM_3D ?
                       view->base_mip : 0);
}

void
anv_buffer_view_fill_image_param(struct anv_device *device,
                                 struct anv_buffer_view *view,
                                 struct brw_image_param *param)
{
   image_param_defaults(param);

   param->stride[0] = isl_format_layouts[view->format].bs;
   param->size[0] = view->range / param->stride[0];
}
