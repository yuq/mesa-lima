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

static const struct anv_format anv_formats[] = {
   [VK_FORMAT_UNDEFINED] = { .format = RAW },
   // [VK_FORMAT_R4G4_UNORM] = { .format = R4G4_UNORM },
   // [VK_FORMAT_R4G4_USCALED] = { .format = R4G4_USCALED },
   // [VK_FORMAT_R4G4B4A4_UNORM] = { .format = R4G4B4A4_UNORM },
   // [VK_FORMAT_R4G4B4A4_USCALED] = { .format = R4G4B4A4_USCALED },
   // [VK_FORMAT_R5G6B5_UNORM] = { .format = R5G6B5_UNORM },
   // [VK_FORMAT_R5G6B5_USCALED] = { .format = R5G6B5_USCALED },
   // [VK_FORMAT_R5G5B5A1_UNORM] = { .format = R5G5B5A1_UNORM },
   // [VK_FORMAT_R5G5B5A1_USCALED] = { .format = R5G5B5A1_USCALED },
   [VK_FORMAT_R8_UNORM] = { .format = R8_UNORM, .cpp = 1, .channels = 1 },
   [VK_FORMAT_R8_SNORM] = { .format = R8_SNORM, .cpp = 1, .channels = 1 },
   [VK_FORMAT_R8_USCALED] = { .format = R8_USCALED, .cpp = 1, .channels = 1 },
   [VK_FORMAT_R8_SSCALED] = { .format = R8_SSCALED, .cpp = 1, .channels = 1 },
   [VK_FORMAT_R8_UINT] = { .format = R8_UINT, .cpp = 1, .channels = 1 },
   [VK_FORMAT_R8_SINT] = { .format = R8_SINT, .cpp = 1, .channels = 1 },
   // [VK_FORMAT_R8_SRGB] = { .format = R8_SRGB, .cpp = 1 },
   [VK_FORMAT_R8G8_UNORM] = { .format = R8G8_UNORM, .cpp = 2, .channels = 2 },
   [VK_FORMAT_R8G8_SNORM] = { .format = R8G8_SNORM, .cpp = 2, .channels = 2 },
   [VK_FORMAT_R8G8_USCALED] = { .format = R8G8_USCALED, .cpp = 2, .channels = 2 },
   [VK_FORMAT_R8G8_SSCALED] = { .format = R8G8_SSCALED, .cpp = 2, .channels = 2 },
   [VK_FORMAT_R8G8_UINT] = { .format = R8G8_UINT, .cpp = 2, .channels = 2 },
   [VK_FORMAT_R8G8_SINT] = { .format = R8G8_SINT, .cpp = 2, .channels = 2 },
   // [VK_FORMAT_R8G8_SRGB] = { .format = R8G8_SRGB },
   [VK_FORMAT_R8G8B8_UNORM] = { .format = R8G8B8X8_UNORM, .cpp = 3, .channels = 3  },
   // [VK_FORMAT_R8G8B8_SNORM] = { .format = R8G8B8X8_SNORM, .cpp = 4 },
   [VK_FORMAT_R8G8B8_USCALED] = { .format = R8G8B8_USCALED, .cpp = 3, .channels = 3 },
   [VK_FORMAT_R8G8B8_SSCALED] = { .format = R8G8B8_SSCALED, .cpp = 3, .channels = 3 },
   [VK_FORMAT_R8G8B8_UINT] = { .format = R8G8B8_UINT, .cpp = 3, .channels = 3 },
   [VK_FORMAT_R8G8B8_SINT] = { .format = R8G8B8_SINT, .cpp = 3, .channels = 3 },
   // [VK_FORMAT_R8G8B8_SRGB] = { .format = R8G8B8_SRGB },
   [VK_FORMAT_R8G8B8A8_UNORM] = { .format = R8G8B8A8_UNORM, .cpp = 4, .channels = 4 },
   [VK_FORMAT_R8G8B8A8_SNORM] = { .format = R8G8B8A8_SNORM, .cpp = 4, .channels = 4 },
   [VK_FORMAT_R8G8B8A8_USCALED] = { .format = R8G8B8A8_USCALED, .cpp = 4, .channels = 4 },
   [VK_FORMAT_R8G8B8A8_SSCALED] = { .format = R8G8B8A8_SSCALED, .cpp = 4, .channels = 4 },
   [VK_FORMAT_R8G8B8A8_UINT] = { .format = R8G8B8A8_UINT, .cpp = 4, .channels = 4 },
   [VK_FORMAT_R8G8B8A8_SINT] = { .format = R8G8B8A8_SINT, .cpp = 4, .channels = 4 },
   // [VK_FORMAT_R8G8B8A8_SRGB] = { .format = R8G8B8A8_SRGB },
   // [VK_FORMAT_R10G10B10A2_UNORM] = { .format = R10G10B10A2_UNORM },
   // [VK_FORMAT_R10G10B10A2_SNORM] = { .format = R10G10B10A2_SNORM },
   // [VK_FORMAT_R10G10B10A2_USCALED] = { .format = R10G10B10A2_USCALED },
   // [VK_FORMAT_R10G10B10A2_SSCALED] = { .format = R10G10B10A2_SSCALED },
   // [VK_FORMAT_R10G10B10A2_UINT] = { .format = R10G10B10A2_UINT },
   // [VK_FORMAT_R10G10B10A2_SINT] = { .format = R10G10B10A2_SINT },
   // [VK_FORMAT_R16_UNORM] = { .format = R16_UNORM },
   // [VK_FORMAT_R16_SNORM] = { .format = R16_SNORM },
   // [VK_FORMAT_R16_USCALED] = { .format = R16_USCALED },
   // [VK_FORMAT_R16_SSCALED] = { .format = R16_SSCALED },
   // [VK_FORMAT_R16_UINT] = { .format = R16_UINT },
   // [VK_FORMAT_R16_SINT] = { .format = R16_SINT },
   [VK_FORMAT_R16_SFLOAT] = { .format = R16_FLOAT, .cpp = 2, .channels = 1 },
   // [VK_FORMAT_R16G16_UNORM] = { .format = R16G16_UNORM },
   // [VK_FORMAT_R16G16_SNORM] = { .format = R16G16_SNORM },
   // [VK_FORMAT_R16G16_USCALED] = { .format = R16G16_USCALED },
   // [VK_FORMAT_R16G16_SSCALED] = { .format = R16G16_SSCALED },
   // [VK_FORMAT_R16G16_UINT] = { .format = R16G16_UINT },
   // [VK_FORMAT_R16G16_SINT] = { .format = R16G16_SINT },
   [VK_FORMAT_R16G16_SFLOAT] = { .format = R16G16_FLOAT, .cpp = 4, .channels = 2 },
   // [VK_FORMAT_R16G16B16_UNORM] = { .format = R16G16B16_UNORM },
   // [VK_FORMAT_R16G16B16_SNORM] = { .format = R16G16B16_SNORM },
   // [VK_FORMAT_R16G16B16_USCALED] = { .format = R16G16B16_USCALED },
   // [VK_FORMAT_R16G16B16_SSCALED] = { .format = R16G16B16_SSCALED },
   // [VK_FORMAT_R16G16B16_UINT] = { .format = R16G16B16_UINT },
   // [VK_FORMAT_R16G16B16_SINT] = { .format = R16G16B16_SINT },
   [VK_FORMAT_R16G16B16_SFLOAT] = { .format = R16G16B16_FLOAT, .cpp = 6, .channels = 3 },
   // [VK_FORMAT_R16G16B16A16_UNORM] = { .format = R16G16B16A16_UNORM },
   // [VK_FORMAT_R16G16B16A16_SNORM] = { .format = R16G16B16A16_SNORM },
   // [VK_FORMAT_R16G16B16A16_USCALED] = { .format = R16G16B16A16_USCALED },
   // [VK_FORMAT_R16G16B16A16_SSCALED] = { .format = R16G16B16A16_SSCALED },
   // [VK_FORMAT_R16G16B16A16_UINT] = { .format = R16G16B16A16_UINT },
   // [VK_FORMAT_R16G16B16A16_SINT] = { .format = R16G16B16A16_SINT },
   [VK_FORMAT_R16G16B16A16_SFLOAT] = { .format = R16G16B16A16_FLOAT, .cpp = 8, .channels = 4 },
   // [VK_FORMAT_R32_UINT] = { .format = R32_UINT },
   // [VK_FORMAT_R32_SINT] = { .format = R32_SINT },
   [VK_FORMAT_R32_SFLOAT] = { .format = R32_FLOAT, .cpp = 4, .channels = 1 },
   // [VK_FORMAT_R32G32_UINT] = { .format = R32G32_UINT },
   // [VK_FORMAT_R32G32_SINT] = { .format = R32G32_SINT },
   [VK_FORMAT_R32G32_SFLOAT] = { .format = R32G32_FLOAT, .cpp = 8, .channels = 2 },
   // [VK_FORMAT_R32G32B32_UINT] = { .format = R32G32B32_UINT },
   // [VK_FORMAT_R32G32B32_SINT] = { .format = R32G32B32_SINT },
   [VK_FORMAT_R32G32B32_SFLOAT] = { .format = R32G32B32_FLOAT, .cpp = 12, .channels = 3 },
   // [VK_FORMAT_R32G32B32A32_UINT] = { .format = R32G32B32A32_UINT },
   // [VK_FORMAT_R32G32B32A32_SINT] = { .format = R32G32B32A32_SINT },
   [VK_FORMAT_R32G32B32A32_SFLOAT] = { .format = R32G32B32A32_FLOAT, .cpp = 16, .channels = 4 },
   [VK_FORMAT_R64_SFLOAT] = { .format = R64_FLOAT, .cpp = 8, .channels = 1 },
   [VK_FORMAT_R64G64_SFLOAT] = { .format = R64G64_FLOAT, .cpp = 16, .channels = 2 },
   [VK_FORMAT_R64G64B64_SFLOAT] = { .format = R64G64B64_FLOAT, .cpp = 24, .channels = 3 },
   [VK_FORMAT_R64G64B64A64_SFLOAT] = { .format = R64G64B64A64_FLOAT, .cpp = 32, .channels = 4 },
   // [VK_FORMAT_R11G11B10_UFLOAT] = { .format = R11G11B10_UFLOAT },
   // [VK_FORMAT_R9G9B9E5_UFLOAT] = { .format = R9G9B9E5_UFLOAT },
   // [VK_FORMAT_D16_UNORM] = { .format = D16_UNORM },
   // [VK_FORMAT_D24_UNORM] = { .format = D24_UNORM },
   // [VK_FORMAT_D32_SFLOAT] = { .format = D32_SFLOAT },
   // [VK_FORMAT_S8_UINT] = { .format = S8_UINT },
   // [VK_FORMAT_D16_UNORM_S8_UINT] = { .format = D16_UNORM },
   // [VK_FORMAT_D24_UNORM_S8_UINT] = { .format = D24_UNORM },
   // [VK_FORMAT_D32_SFLOAT_S8_UINT] = { .format = D32_SFLOAT },
   // [VK_FORMAT_BC1_RGB_UNORM] = { .format = BC1_RGB },
   // [VK_FORMAT_BC1_RGB_SRGB] = { .format = BC1_RGB },
   // [VK_FORMAT_BC1_RGBA_UNORM] = { .format = BC1_RGBA },
   // [VK_FORMAT_BC1_RGBA_SRGB] = { .format = BC1_RGBA },
   // [VK_FORMAT_BC2_UNORM] = { .format = BC2_UNORM },
   // [VK_FORMAT_BC2_SRGB] = { .format = BC2_SRGB },
   // [VK_FORMAT_BC3_UNORM] = { .format = BC3_UNORM },
   // [VK_FORMAT_BC3_SRGB] = { .format = BC3_SRGB },
   // [VK_FORMAT_BC4_UNORM] = { .format = BC4_UNORM },
   // [VK_FORMAT_BC4_SNORM] = { .format = BC4_SNORM },
   // [VK_FORMAT_BC5_UNORM] = { .format = BC5_UNORM },
   // [VK_FORMAT_BC5_SNORM] = { .format = BC5_SNORM },
   // [VK_FORMAT_BC6H_UFLOAT] = { .format = BC6H_UFLOAT },
   // [VK_FORMAT_BC6H_SFLOAT] = { .format = BC6H_SFLOAT },
   // [VK_FORMAT_BC7_UNORM] = { .format = BC7_UNORM },
   // [VK_FORMAT_BC7_SRGB] = { .format = BC7_SRGB },
   // [VK_FORMAT_ETC2_R8G8B8_UNORM] = { .format = ETC2_R8G8B8 },
   // [VK_FORMAT_ETC2_R8G8B8_SRGB] = { .format = ETC2_R8G8B8 },
   // [VK_FORMAT_ETC2_R8G8B8A1_UNORM] = { .format = ETC2_R8G8B8A1 },
   // [VK_FORMAT_ETC2_R8G8B8A1_SRGB] = { .format = ETC2_R8G8B8A1 },
   // [VK_FORMAT_ETC2_R8G8B8A8_UNORM] = { .format = ETC2_R8G8B8A8 },
   // [VK_FORMAT_ETC2_R8G8B8A8_SRGB] = { .format = ETC2_R8G8B8A8 },
   // [VK_FORMAT_EAC_R11_UNORM] = { .format = EAC_R11 },
   // [VK_FORMAT_EAC_R11_SNORM] = { .format = EAC_R11 },
   // [VK_FORMAT_EAC_R11G11_UNORM] = { .format = EAC_R11G11 },
   // [VK_FORMAT_EAC_R11G11_SNORM] = { .format = EAC_R11G11 },
   // [VK_FORMAT_ASTC_4x4_UNORM] = { .format = ASTC_4x4 },
   // [VK_FORMAT_ASTC_4x4_SRGB] = { .format = ASTC_4x4 },
   // [VK_FORMAT_ASTC_5x4_UNORM] = { .format = ASTC_5x4 },
   // [VK_FORMAT_ASTC_5x4_SRGB] = { .format = ASTC_5x4 },
   // [VK_FORMAT_ASTC_5x5_UNORM] = { .format = ASTC_5x5 },
   // [VK_FORMAT_ASTC_5x5_SRGB] = { .format = ASTC_5x5 },
   // [VK_FORMAT_ASTC_6x5_UNORM] = { .format = ASTC_6x5 },
   // [VK_FORMAT_ASTC_6x5_SRGB] = { .format = ASTC_6x5 },
   // [VK_FORMAT_ASTC_6x6_UNORM] = { .format = ASTC_6x6 },
   // [VK_FORMAT_ASTC_6x6_SRGB] = { .format = ASTC_6x6 },
   // [VK_FORMAT_ASTC_8x5_UNORM] = { .format = ASTC_8x5 },
   // [VK_FORMAT_ASTC_8x5_SRGB] = { .format = ASTC_8x5 },
   // [VK_FORMAT_ASTC_8x6_UNORM] = { .format = ASTC_8x6 },
   // [VK_FORMAT_ASTC_8x6_SRGB] = { .format = ASTC_8x6 },
   // [VK_FORMAT_ASTC_8x8_UNORM] = { .format = ASTC_8x8 },
   // [VK_FORMAT_ASTC_8x8_SRGB] = { .format = ASTC_8x8 },
   // [VK_FORMAT_ASTC_10x5_UNORM] = { .format = ASTC_10x5 },
   // [VK_FORMAT_ASTC_10x5_SRGB] = { .format = ASTC_10x5 },
   // [VK_FORMAT_ASTC_10x6_UNORM] = { .format = ASTC_10x6 },
   // [VK_FORMAT_ASTC_10x6_SRGB] = { .format = ASTC_10x6 },
   // [VK_FORMAT_ASTC_10x8_UNORM] = { .format = ASTC_10x8 },
   // [VK_FORMAT_ASTC_10x8_SRGB] = { .format = ASTC_10x8 },
   // [VK_FORMAT_ASTC_10x10_UNORM] = { .format = ASTC_10x10 },
   // [VK_FORMAT_ASTC_10x10_SRGB] = { .format = ASTC_10x10 },
   // [VK_FORMAT_ASTC_12x10_UNORM] = { .format = ASTC_12x10 },
   // [VK_FORMAT_ASTC_12x10_SRGB] = { .format = ASTC_12x10 },
   // [VK_FORMAT_ASTC_12x12_UNORM] = { .format = ASTC_12x12 },
   // [VK_FORMAT_ASTC_12x12_SRGB] = { .format = ASTC_12x12 },
   // [VK_FORMAT_B4G4R4A4_UNORM] = { .format = B4G4R4A4_UNORM },
   // [VK_FORMAT_B5G5R5A1_UNORM] = { .format = B5G5R5A1_UNORM },
   // [VK_FORMAT_B5G6R5_UNORM] = { .format = B5G6R5_UNORM },
   // [VK_FORMAT_B5G6R5_USCALED] = { .format = B5G6R5_USCALED },
   // [VK_FORMAT_B8G8R8_UNORM] = { .format = B8G8R8_UNORM },
   // [VK_FORMAT_B8G8R8_SNORM] = { .format = B8G8R8_SNORM },
   // [VK_FORMAT_B8G8R8_USCALED] = { .format = B8G8R8_USCALED },
   // [VK_FORMAT_B8G8R8_SSCALED] = { .format = B8G8R8_SSCALED },
   // [VK_FORMAT_B8G8R8_UINT] = { .format = B8G8R8_UINT },
   // [VK_FORMAT_B8G8R8_SINT] = { .format = B8G8R8_SINT },
   // [VK_FORMAT_B8G8R8_SRGB] = { .format = B8G8R8_SRGB },
   [VK_FORMAT_B8G8R8A8_UNORM] = { .format = B8G8R8A8_UNORM, .cpp = 4, .channels = 4 },
   // [VK_FORMAT_B8G8R8A8_SNORM] = { .format = B8G8R8A8_SNORM },
   // [VK_FORMAT_B8G8R8A8_USCALED] = { .format = B8G8R8A8_USCALED },
   // [VK_FORMAT_B8G8R8A8_SSCALED] = { .format = B8G8R8A8_SSCALED },
   // [VK_FORMAT_B8G8R8A8_UINT] = { .format = B8G8R8A8_UINT },
   // [VK_FORMAT_B8G8R8A8_SINT] = { .format = B8G8R8A8_SINT },
   // [VK_FORMAT_B8G8R8A8_SRGB] = { .format = B8G8R8A8_SRGB },
   // [VK_FORMAT_B10G10R10A2_UNORM] = { .format = B10G10R10A2_UNORM },
   // [VK_FORMAT_B10G10R10A2_SNORM] = { .format = B10G10R10A2_SNORM },
   // [VK_FORMAT_B10G10R10A2_USCALED] = { .format = B10G10R10A2_USCALED },
   // [VK_FORMAT_B10G10R10A2_SSCALED] = { .format = B10G10R10A2_SSCALED },
   // [VK_FORMAT_B10G10R10A2_UINT] = { .format = B10G10R10A2_UINT },
   // [VK_FORMAT_B10G10R10A2_SINT] = { .format = B10G10R10A2_SINT }
};

const struct anv_format *
anv_format_for_vk_format(VkFormat format)
{
   return &anv_formats[format];
}

static const struct anv_tile_mode_info {
   int32_t tile_width;
   int32_t tile_height;
} tile_mode_info[] = {
   [LINEAR] = {   1,  1 },
   [XMAJOR] = { 512,  8 },
   [YMAJOR] = { 128, 32 },
   [WMAJOR] = { 128, 32 }
};

VkResult VKAPI vkCreateImage(
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

   image->mem = NULL;
   image->offset = 0;
   image->type = pCreateInfo->imageType;
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

VkResult VKAPI vkGetImageSubresourceInfo(
    VkDevice                                    device,
    VkImage                                     image,
    const VkImageSubresource*                   pSubresource,
    VkSubresourceInfoType                       infoType,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   return VK_UNSUPPORTED;
}

// Image view functions

static struct anv_state
create_surface_state(struct anv_device *device,
                     struct anv_image *image, const struct anv_format *format)
{
   struct anv_state state =
      anv_state_pool_alloc(&device->surface_state_pool, 64, 64);

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

VkResult VKAPI vkCreateImageView(
    VkDevice                                    _device,
    const VkImageViewCreateInfo*                pCreateInfo,
    VkImageView*                                pView)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_image_view *view;
   const struct anv_format *format =
      anv_format_for_vk_format(pCreateInfo->format);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);

   view = anv_device_alloc(device, sizeof(*view), 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (view == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   view->image = (struct anv_image *) pCreateInfo->image;

   view->surface_state = create_surface_state(device, view->image, format);

   *pView = (VkImageView) view;

   return VK_SUCCESS;
}

VkResult VKAPI vkCreateColorAttachmentView(
    VkDevice                                    _device,
    const VkColorAttachmentViewCreateInfo*      pCreateInfo,
    VkColorAttachmentView*                      pView)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_color_attachment_view *view;
   struct anv_image *image;
   const struct anv_format *format =
      anv_format_for_vk_format(pCreateInfo->format);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_COLOR_ATTACHMENT_VIEW_CREATE_INFO);

   view = anv_device_alloc(device, sizeof(*view), 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (view == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   view->image = (struct anv_image *) pCreateInfo->image;
   image = view->image;

   view->surface_state = create_surface_state(device, image, format);

   *pView = (VkColorAttachmentView) view;

   return VK_SUCCESS;
}

VkResult VKAPI vkCreateDepthStencilView(
    VkDevice                                    device,
    const VkDepthStencilViewCreateInfo*         pCreateInfo,
    VkDepthStencilView*                         pView)
{
   return VK_UNSUPPORTED;
}
