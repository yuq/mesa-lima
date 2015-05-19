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

// Image functions

static const struct anv_tile_mode_info {
   int32_t tile_width;
   int32_t tile_height;
} tile_mode_info[] = {
   [LINEAR] = {   1,  1 },
   [XMAJOR] = { 512,  8 },
   [YMAJOR] = { 128, 32 },
   [WMAJOR] = { 128, 32 }
};

VkResult anv_CreateImage(
    VkDevice                                    _device,
    const VkImageCreateInfo*                    pCreateInfo,
    VkImage*                                    pImage)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_image *image;
   const struct anv_format *format;
   int32_t aligned_height;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);

   image = anv_device_alloc(device, sizeof(*image), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (image == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   image->bo = NULL;
   image->offset = 0;
   image->type = pCreateInfo->imageType;
   image->format = pCreateInfo->format;
   image->extent = pCreateInfo->extent;

   assert(image->extent.width > 0);
   assert(image->extent.height > 0);
   assert(image->extent.depth > 0);

   switch (pCreateInfo->tiling) {
   case VK_IMAGE_TILING_LINEAR:
      image->tile_mode = LINEAR;
      /* Linear depth buffers must be 64 byte aligned, which is the strictest
       * requirement for all kinds of linear surfaces.
       */
      image->alignment = 64;
      break;
   case VK_IMAGE_TILING_OPTIMAL:
      image->tile_mode = YMAJOR;
      image->alignment = 4096;
      break;
   default:
      break;
   }
   
   format = anv_format_for_vk_format(pCreateInfo->format);
   image->stride = ALIGN_I32(image->extent.width * format->cpp,
                             tile_mode_info[image->tile_mode].tile_width);
   aligned_height = ALIGN_I32(image->extent.height,
                              tile_mode_info[image->tile_mode].tile_height);
   image->size = image->stride * aligned_height;

   *pImage = (VkImage) image;

   return VK_SUCCESS;
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

// Image view functions

static struct anv_state
create_surface_state(struct anv_device *device,
                     struct anv_image *image, const struct anv_format *format,
                     struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_state state;
   if (cmd_buffer)
      state = anv_state_stream_alloc(&cmd_buffer->surface_state_stream, 64, 64);
   else
      state = anv_state_pool_alloc(&device->surface_state_pool, 64, 64);

   struct GEN8_RENDER_SURFACE_STATE surface_state = {
      .SurfaceType = SURFTYPE_2D,
      .SurfaceArray = false,
      .SurfaceFormat = format->format,
      .SurfaceVerticalAlignment = VALIGN4,
      .SurfaceHorizontalAlignment = HALIGN4,
      .TileMode = image->tile_mode,
      .VerticalLineStride = 0,
      .VerticalLineStrideOffset = 0,
      .SamplerL2BypassModeDisable = true,
      .RenderCacheReadWriteMode = WriteOnlyCache,
      .MemoryObjectControlState = 0, /* FIXME: MOCS */
      .BaseMipLevel = 0,
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
      .ResourceMinLOD = 0,
      /* FIXME: We assume that the image must be bound at this time. */
      .SurfaceBaseAddress = { NULL, image->offset },
   };

   GEN8_RENDER_SURFACE_STATE_pack(NULL, state.map, &surface_state);

   return state;
}

void
anv_image_view_init(struct anv_surface_view *view,
                    struct anv_device *device,
                    const VkImageViewCreateInfo* pCreateInfo,
                    struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_image *image = (struct anv_image *) pCreateInfo->image;
   const struct anv_format *format =
      anv_format_for_vk_format(pCreateInfo->format);

   view->bo = image->bo;
   view->offset = image->offset;
   view->surface_state = create_surface_state(device, image, format,
                                              cmd_buffer);
   view->format = pCreateInfo->format;

   /* TODO: Miplevels */
   view->extent = image->extent;
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

   view->bo = image->bo;
   view->offset = image->offset;
   view->surface_state = create_surface_state(device, image, format,
                                              cmd_buffer);
   view->extent = image->extent;
   view->format = pCreateInfo->format;
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

   *pView = (VkColorAttachmentView) view;

   return VK_SUCCESS;
}

VkResult anv_CreateDepthStencilView(
    VkDevice                                    device,
    const VkDepthStencilViewCreateInfo*         pCreateInfo,
    VkDepthStencilView*                         pView)
{
   stub_return(VK_UNSUPPORTED);
}
