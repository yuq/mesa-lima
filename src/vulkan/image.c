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

#include "private.h"

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

static const uint8_t anv_surf_type_from_image_view_type[] = {
   [VK_IMAGE_VIEW_TYPE_1D]    = SURFTYPE_1D,
   [VK_IMAGE_VIEW_TYPE_2D]    = SURFTYPE_2D,
   [VK_IMAGE_VIEW_TYPE_3D]    = SURFTYPE_3D,
   [VK_IMAGE_VIEW_TYPE_CUBE]  = SURFTYPE_CUBE,
};

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

static uint32_t
anv_image_choose_tile_mode(const struct anv_image_create_info *anv_info)
{
   if (anv_info->force_tile_mode)
      return anv_info->tile_mode;

   switch (anv_info->vk_info->tiling) {
   case VK_IMAGE_TILING_LINEAR:
      return LINEAR;
   case VK_IMAGE_TILING_OPTIMAL:
      return YMAJOR;
   default:
      assert(!"bad VKImageTiling");
      return LINEAR;
   }
}

VkResult anv_image_create(
    VkDevice                                    _device,
    const struct anv_image_create_info *        create_info,
    VkImage*                                    pImage)
{
   struct anv_device *device = (struct anv_device *) _device;
   const VkImageCreateInfo *pCreateInfo = create_info->vk_info;
   const VkExtent3D *restrict extent = &pCreateInfo->extent;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);

   /* XXX: We don't handle any of these */
   anv_assert(pCreateInfo->imageType == VK_IMAGE_TYPE_2D);
   anv_assert(pCreateInfo->mipLevels == 1);
   anv_assert(pCreateInfo->arraySize == 1);
   anv_assert(pCreateInfo->samples == 1);
   anv_assert(pCreateInfo->extent.width > 0);
   anv_assert(pCreateInfo->extent.height > 0);
   anv_assert(pCreateInfo->extent.depth == 1);

   const uint32_t tile_mode = anv_image_choose_tile_mode(create_info);

   /* TODO(chadv): How should we validate inputs? */
   const uint8_t surf_type =
      anv_surf_type_from_image_type[pCreateInfo->imageType];

   const struct anv_surf_type_limits *limits =
      &anv_surf_type_limits[surf_type];

   const struct anv_tile_info *tile_info =
       &anv_tile_info_table[tile_mode];

   if (extent->width > limits->width ||
       extent->height > limits->height ||
       extent->depth > limits->depth) {
      /* TODO(chadv): What is the correct error? */
      anv_loge("image extent is too large");
      return vk_error(VK_ERROR_INVALID_MEMORY_SIZE);
   }

   const struct anv_format *format_info =
      anv_format_for_vk_format(pCreateInfo->format);

   uint32_t image_stride = 0;
   uint32_t image_size = 0;
   uint32_t stencil_offset = 0;
   uint32_t stencil_stride = 0;

   if (!format_info->has_stencil || format_info->depth_format) {
      /* The format has a color or depth component. Calculate space for it. */
      uint32_t aligned_height;

      image_stride = align_i32(extent->width * format_info->cpp,
                               tile_info->width);
      aligned_height = align_i32(extent->height, tile_info->height);
      image_size = image_stride * aligned_height;
   }

   if (format_info->has_stencil) {
      /* From the GPU's perspective, the depth buffer and stencil buffer are
       * separate buffers.  From Vulkan's perspective, though, depth and
       * stencil reside in the same image.  To satisfy Vulkan and the GPU, we
       * place the depth and stencil buffers in the same bo.
       */
      const struct anv_tile_info *w_info = &anv_tile_info_table[WMAJOR];
      uint32_t aligned_height;
      uint32_t stencil_size;

      stencil_offset = align_u32(image_size, w_info->surface_alignment);
      stencil_stride = align_i32(extent->width, w_info->width);
      aligned_height = align_i32(extent->height, w_info->height);
      stencil_size = stencil_stride * aligned_height;
      image_size = stencil_offset + stencil_size;
   }

   struct anv_image *image = anv_device_alloc(device, sizeof(*image), 8,
                                              VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (!image)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   /* To eliminate the risk of using unitialized struct members above, fill the
    * image struct here at the function bottom instead of piecemeal throughout
    * the function body.
    */
   *image = (struct anv_image) {
      .type = pCreateInfo->imageType,
      .extent = pCreateInfo->extent,
      .format = pCreateInfo->format,

      .size = image_size,
      .alignment = tile_info->surface_alignment,
      .stride = image_stride,

      .bo = NULL,
      .offset = 0,

      .stencil_offset = stencil_offset,
      .stencil_stride = stencil_stride,

      .tile_mode = tile_mode,
      .surf_type = surf_type,

      /* FINISHME: Stop hardcoding miptree image alignment */
      .h_align = 4,
      .v_align = 4,

      .swap_chain = NULL,
   };

   *pImage = (VkImage) image;

   return VK_SUCCESS;
}

VkResult anv_CreateImage(
    VkDevice                                    device,
    const VkImageCreateInfo*                    pCreateInfo,
    VkImage*                                    pImage)
{
   return anv_image_create(device,
      &(struct anv_image_create_info) {
         .vk_info = pCreateInfo,
      },
      pImage);
}

VkResult anv_GetImageSubresourceInfo(
    VkDevice                                    device,
    VkImage                                     image,
    const VkImageSubresource*                   pSubresource,
    VkSubresourceInfoType                       infoType,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   stub_return(VK_UNSUPPORTED);
}

void
anv_surface_view_destroy(struct anv_device *device,
                         struct anv_object *obj, VkObjectType obj_type)
{
   struct anv_surface_view *view = (struct anv_surface_view *)obj;

   assert(obj_type == VK_OBJECT_TYPE_BUFFER_VIEW ||
          obj_type == VK_OBJECT_TYPE_IMAGE_VIEW ||
          obj_type == VK_OBJECT_TYPE_COLOR_ATTACHMENT_VIEW);

   anv_state_pool_free(&device->surface_state_pool, view->surface_state);

   anv_device_free(device, view);
}

void
anv_image_view_init(struct anv_surface_view *view,
                    struct anv_device *device,
                    const VkImageViewCreateInfo* pCreateInfo,
                    struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_image *image = (struct anv_image *) pCreateInfo->image;

   const struct anv_format *format_info =
      anv_format_for_vk_format(pCreateInfo->format);

   /* XXX: We don't handle any of these */
   anv_assert(pCreateInfo->viewType == VK_IMAGE_VIEW_TYPE_2D);
   anv_assert(pCreateInfo->subresourceRange.baseMipLevel == 0);
   anv_assert(pCreateInfo->subresourceRange.mipLevels == 1);
   anv_assert(pCreateInfo->subresourceRange.baseArraySlice == 0);
   anv_assert(pCreateInfo->subresourceRange.arraySize == 1);

   view->bo = image->bo;

   switch (pCreateInfo->subresourceRange.aspect) {
   case VK_IMAGE_ASPECT_STENCIL:
      anv_finishme("stencil image views");
      abort();
      break;
   case VK_IMAGE_ASPECT_DEPTH:
   case VK_IMAGE_ASPECT_COLOR:
      view->offset = image->offset;
      break;
   default:
      unreachable("");
      break;
   }

   /* TODO: Miplevels */
   view->extent = image->extent;

   static const uint32_t vk_to_gen_swizzle[] = {
      [VK_CHANNEL_SWIZZLE_ZERO]                 = SCS_ZERO,
      [VK_CHANNEL_SWIZZLE_ONE]                  = SCS_ONE,
      [VK_CHANNEL_SWIZZLE_R]                    = SCS_RED,
      [VK_CHANNEL_SWIZZLE_G]                    = SCS_GREEN,
      [VK_CHANNEL_SWIZZLE_B]                    = SCS_BLUE,
      [VK_CHANNEL_SWIZZLE_A]                    = SCS_ALPHA
   };

   struct GEN8_RENDER_SURFACE_STATE surface_state = {
      .SurfaceType = anv_surf_type_from_image_view_type[pCreateInfo->viewType],
      .SurfaceArray = false,
      .SurfaceFormat = format_info->surface_format,
      .SurfaceVerticalAlignment = anv_valign[image->v_align],
      .SurfaceHorizontalAlignment = anv_halign[image->h_align],
      .TileMode = image->tile_mode,
      .VerticalLineStride = 0,
      .VerticalLineStrideOffset = 0,
      .SamplerL2BypassModeDisable = true,
      .RenderCacheReadWriteMode = WriteOnlyCache,
      .MemoryObjectControlState = GEN8_MOCS,
      .BaseMipLevel = 0.0,
      .SurfaceQPitch = 0,
      .Height = image->extent.height - 1,
      .Width = image->extent.width - 1,
      .Depth = image->extent.depth - 1,
      .SurfacePitch = image->stride - 1,
      .MinimumArrayElement = 0,
      .NumberofMultisamples = MULTISAMPLECOUNT_1,
      .XOffset = 0,
      .YOffset = 0,
      .SurfaceMinLOD = 0,
      .MIPCountLOD = 0,
      .AuxiliarySurfaceMode = AUX_NONE,
      .RedClearColor = 0,
      .GreenClearColor = 0,
      .BlueClearColor = 0,
      .AlphaClearColor = 0,
      .ShaderChannelSelectRed = vk_to_gen_swizzle[pCreateInfo->channels.r],
      .ShaderChannelSelectGreen = vk_to_gen_swizzle[pCreateInfo->channels.g],
      .ShaderChannelSelectBlue = vk_to_gen_swizzle[pCreateInfo->channels.b],
      .ShaderChannelSelectAlpha = vk_to_gen_swizzle[pCreateInfo->channels.a],
      .ResourceMinLOD = 0.0,
      .SurfaceBaseAddress = { NULL, view->offset },
   };

   if (cmd_buffer)
      view->surface_state =
         anv_state_stream_alloc(&cmd_buffer->surface_state_stream, 64, 64);
   else
      view->surface_state =
         anv_state_pool_alloc(&device->surface_state_pool, 64, 64);

   GEN8_RENDER_SURFACE_STATE_pack(NULL, view->surface_state.map, &surface_state);
}

VkResult anv_CreateImageView(
    VkDevice                                    _device,
    const VkImageViewCreateInfo*                pCreateInfo,
    VkImageView*                                pView)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_surface_view *view;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);

   view = anv_device_alloc(device, sizeof(*view), 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (view == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   anv_image_view_init(view, device, pCreateInfo, NULL);

   view->base.destructor = anv_surface_view_destroy;

   *pView = (VkImageView) view;

   return VK_SUCCESS;
}

void
anv_color_attachment_view_init(struct anv_surface_view *view,
                               struct anv_device *device,
                               const VkColorAttachmentViewCreateInfo* pCreateInfo,
                               struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_image *image = (struct anv_image *) pCreateInfo->image;
   const struct anv_format *format =
      anv_format_for_vk_format(pCreateInfo->format);

   /* XXX: We don't handle any of these */
   anv_assert(pCreateInfo->mipLevel == 0);
   anv_assert(pCreateInfo->baseArraySlice == 0);
   anv_assert(pCreateInfo->arraySize == 1);
   anv_assert(pCreateInfo->msaaResolveImage == 0);

   view->bo = image->bo;
   view->offset = image->offset;
   view->extent = image->extent;
   view->format = pCreateInfo->format;

   if (cmd_buffer)
      view->surface_state =
         anv_state_stream_alloc(&cmd_buffer->surface_state_stream, 64, 64);
   else
      view->surface_state =
         anv_state_pool_alloc(&device->surface_state_pool, 64, 64);

   struct GEN8_RENDER_SURFACE_STATE surface_state = {
      .SurfaceType = SURFTYPE_2D,
      .SurfaceArray = false,
      .SurfaceFormat = format->surface_format,
      .SurfaceVerticalAlignment = anv_valign[image->v_align],
      .SurfaceHorizontalAlignment = anv_halign[image->h_align],
      .TileMode = image->tile_mode,
      .VerticalLineStride = 0,
      .VerticalLineStrideOffset = 0,
      .SamplerL2BypassModeDisable = true,
      .RenderCacheReadWriteMode = WriteOnlyCache,
      .MemoryObjectControlState = GEN8_MOCS,
      .BaseMipLevel = 0.0,
      .SurfaceQPitch = 0,
      .Height = image->extent.height - 1,
      .Width = image->extent.width - 1,
      .Depth = image->extent.depth - 1,
      .SurfacePitch = image->stride - 1,
      .MinimumArrayElement = 0,
      .NumberofMultisamples = MULTISAMPLECOUNT_1,
      .XOffset = 0,
      .YOffset = 0,
      .SurfaceMinLOD = 0,
      .MIPCountLOD = 0,
      .AuxiliarySurfaceMode = AUX_NONE,
      .RedClearColor = 0,
      .GreenClearColor = 0,
      .BlueClearColor = 0,
      .AlphaClearColor = 0,
      .ShaderChannelSelectRed = SCS_RED,
      .ShaderChannelSelectGreen = SCS_GREEN,
      .ShaderChannelSelectBlue = SCS_BLUE,
      .ShaderChannelSelectAlpha = SCS_ALPHA,
      .ResourceMinLOD = 0.0,
      .SurfaceBaseAddress = { NULL, view->offset },
   };

   GEN8_RENDER_SURFACE_STATE_pack(NULL, view->surface_state.map, &surface_state);
}

VkResult anv_CreateColorAttachmentView(
    VkDevice                                    _device,
    const VkColorAttachmentViewCreateInfo*      pCreateInfo,
    VkColorAttachmentView*                      pView)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_surface_view *view;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_COLOR_ATTACHMENT_VIEW_CREATE_INFO);

   view = anv_device_alloc(device, sizeof(*view), 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (view == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   anv_color_attachment_view_init(view, device, pCreateInfo, NULL);

   view->base.destructor = anv_surface_view_destroy;

   *pView = (VkColorAttachmentView) view;

   return VK_SUCCESS;
}

VkResult anv_CreateDepthStencilView(
    VkDevice                                    _device,
    const VkDepthStencilViewCreateInfo*         pCreateInfo,
    VkDepthStencilView*                         pView)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_depth_stencil_view *view;
   struct anv_image *image = (struct anv_image *) pCreateInfo->image;
   const struct anv_format *format =
      anv_format_for_vk_format(image->format);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DEPTH_STENCIL_VIEW_CREATE_INFO);

   view = anv_device_alloc(device, sizeof(*view), 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (view == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   /* XXX: We don't handle any of these */
   anv_assert(pCreateInfo->mipLevel == 0);
   anv_assert(pCreateInfo->baseArraySlice == 0);
   anv_assert(pCreateInfo->arraySize == 1);
   anv_assert(pCreateInfo->msaaResolveImage == 0);

   view->bo = image->bo;

   view->depth_stride = image->stride;
   view->depth_offset = image->offset;
   view->depth_format = format->depth_format;
   view->depth_qpitch = 0; /* FINISHME: QPitch */

   view->stencil_stride = image->stencil_stride;
   view->stencil_offset = image->offset + image->stencil_offset;
   view->stencil_qpitch = 0; /* FINISHME: QPitch */

   *pView = (VkDepthStencilView) view;

   return VK_SUCCESS;
}
