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

#include "private.h"

#define UNSUPPORTED 0xffff

#define fmt(__vk_fmt, ...) \
   [VK_FORMAT_##__vk_fmt] = { __VA_ARGS__ }

static const struct anv_format anv_formats[] = {
   fmt(UNDEFINED, .format = RAW, .cpp = 1, .channels = 1),
   fmt(R4G4_UNORM, .format = UNSUPPORTED),
   fmt(R4G4_USCALED, .format = UNSUPPORTED),
   fmt(R4G4B4A4_UNORM, .format = UNSUPPORTED),
   fmt(R4G4B4A4_USCALED, .format = UNSUPPORTED),
   fmt(R5G6B5_UNORM, .format = UNSUPPORTED),
   fmt(R5G6B5_USCALED, .format = UNSUPPORTED),
   fmt(R5G5B5A1_UNORM, .format = UNSUPPORTED),
   fmt(R5G5B5A1_USCALED, .format = UNSUPPORTED),
   fmt(R8_UNORM, .format = R8_UNORM, .cpp = 1, .channels = 1),
   fmt(R8_SNORM, .format = R8_SNORM, .cpp = 1, .channels = 1,),
   fmt(R8_USCALED, .format = R8_USCALED, .cpp = 1, .channels = 1),
   fmt(R8_SSCALED, .format = R8_SSCALED, .cpp = 1, .channels = 1),
   fmt(R8_UINT, .format = R8_UINT, .cpp = 1, .channels = 1),
   fmt(R8_SINT, .format = R8_SINT, .cpp = 1, .channels = 1),
   fmt(R8_SRGB, .format = UNSUPPORTED),
   fmt(R8G8_UNORM, .format = R8G8_UNORM, .cpp = 2, .channels = 2),
   fmt(R8G8_SNORM, .format = R8G8_SNORM, .cpp = 2, .channels = 2),
   fmt(R8G8_USCALED, .format = R8G8_USCALED, .cpp = 2, .channels = 2),
   fmt(R8G8_SSCALED, .format = R8G8_SSCALED, .cpp = 2, .channels = 2),
   fmt(R8G8_UINT, .format = R8G8_UINT, .cpp = 2, .channels = 2),
   fmt(R8G8_SINT, .format = R8G8_SINT, .cpp = 2, .channels = 2),
   fmt(R8G8_SRGB, .format = UNSUPPORTED), /* L8A8_UNORM_SRGB */
   fmt(R8G8B8_UNORM, .format = R8G8B8X8_UNORM, .cpp = 3, .channels = 3),
   fmt(R8G8B8_SNORM, .format = R8G8B8_SNORM, .cpp = 4),
   fmt(R8G8B8_USCALED, .format = R8G8B8_USCALED, .cpp = 3, .channels = 3),
   fmt(R8G8B8_SSCALED, .format = R8G8B8_SSCALED, .cpp = 3, .channels = 3),
   fmt(R8G8B8_UINT, .format = R8G8B8_UINT, .cpp = 3, .channels = 3),
   fmt(R8G8B8_SINT, .format = R8G8B8_SINT, .cpp = 3, .channels = 3),
   fmt(R8G8B8_SRGB, .format = UNSUPPORTED), /* B8G8R8A8_UNORM_SRGB */
   fmt(R8G8B8A8_UNORM, .format = R8G8B8A8_UNORM, .cpp = 4, .channels = 4),
   fmt(R8G8B8A8_SNORM, .format = R8G8B8A8_SNORM, .cpp = 4, .channels = 4),
   fmt(R8G8B8A8_USCALED, .format = R8G8B8A8_USCALED, .cpp = 4, .channels = 4),
   fmt(R8G8B8A8_SSCALED, .format = R8G8B8A8_SSCALED, .cpp = 4, .channels = 4),
   fmt(R8G8B8A8_UINT, .format = R8G8B8A8_UINT, .cpp = 4, .channels = 4),
   fmt(R8G8B8A8_SINT, .format = R8G8B8A8_SINT, .cpp = 4, .channels = 4),
   fmt(R8G8B8A8_SRGB, .format = R8G8B8A8_UNORM_SRGB, .cpp = 4, .channels = 4),
   fmt(R10G10B10A2_UNORM, .format = R10G10B10A2_UNORM, .cpp = 4, .channels = 4),
   fmt(R10G10B10A2_SNORM, .format = R10G10B10A2_SNORM, .cpp = 4, .channels = 4),
   fmt(R10G10B10A2_USCALED, .format = R10G10B10A2_USCALED, .cpp = 4, .channels = 4),
   fmt(R10G10B10A2_SSCALED, .format = R10G10B10A2_SSCALED, .cpp = 4, .channels = 4),
   fmt(R10G10B10A2_UINT, .format = R10G10B10A2_UINT, .cpp = 4, .channels = 4),
   fmt(R10G10B10A2_SINT, .format = R10G10B10A2_SINT, .cpp = 4, .channels = 4),
   fmt(R16_UNORM, .format = R16_UNORM, .cpp = 2, .channels = 1),
   fmt(R16_SNORM, .format = R16_SNORM, .cpp = 2, .channels = 1),
   fmt(R16_USCALED, .format = R16_USCALED, .cpp = 2, .channels = 1),
   fmt(R16_SSCALED, .format = R16_SSCALED, .cpp = 2, .channels = 1),
   fmt(R16_UINT, .format = R16_UINT, .cpp = 2, .channels = 1),
   fmt(R16_SINT, .format = R16_SINT, .cpp = 2, .channels = 1),
   fmt(R16_SFLOAT, .format = R16_FLOAT, .cpp = 2, .channels = 1),
   fmt(R16G16_UNORM, .format = R16G16_UNORM, .cpp = 4, .channels = 2),
   fmt(R16G16_SNORM, .format = R16G16_SNORM, .cpp = 4, .channels = 2),
   fmt(R16G16_USCALED, .format = R16G16_USCALED, .cpp = 4, .channels = 2),
   fmt(R16G16_SSCALED, .format = R16G16_SSCALED, .cpp = 4, .channels = 2),
   fmt(R16G16_UINT, .format = R16G16_UINT, .cpp = 4, .channels = 2),
   fmt(R16G16_SINT, .format = R16G16_SINT, .cpp = 4, .channels = 2),
   fmt(R16G16_SFLOAT, .format = R16G16_FLOAT, .cpp = 4, .channels = 2),
   fmt(R16G16B16_UNORM, .format = R16G16B16_UNORM, .cpp = 6, .channels = 3),
   fmt(R16G16B16_SNORM, .format = R16G16B16_SNORM, .cpp = 6, .channels = 3),
   fmt(R16G16B16_USCALED, .format = R16G16B16_USCALED, .cpp = 6, .channels = 3),
   fmt(R16G16B16_SSCALED, .format = R16G16B16_SSCALED, .cpp = 6, .channels = 3),
   fmt(R16G16B16_UINT, .format = R16G16B16_UINT, .cpp = 6, .channels = 3),
   fmt(R16G16B16_SINT, .format = R16G16B16_SINT, .cpp = 6, .channels = 3),
   fmt(R16G16B16_SFLOAT, .format = R16G16B16_FLOAT, .cpp = 6, .channels = 3),
   fmt(R16G16B16A16_UNORM, .format = R16G16B16A16_UNORM, .cpp = 8, .channels = 4),
   fmt(R16G16B16A16_SNORM, .format = R16G16B16A16_SNORM, .cpp = 8, .channels = 4),
   fmt(R16G16B16A16_USCALED, .format = R16G16B16A16_USCALED, .cpp = 8, .channels = 4),
   fmt(R16G16B16A16_SSCALED, .format = R16G16B16A16_SSCALED, .cpp = 8, .channels = 4),
   fmt(R16G16B16A16_UINT, .format = R16G16B16A16_UINT, .cpp = 8, .channels = 4),
   fmt(R16G16B16A16_SINT, .format = R16G16B16A16_SINT, .cpp = 8, .channels = 4),
   fmt(R16G16B16A16_SFLOAT, .format = R16G16B16A16_FLOAT, .cpp = 8, .channels = 4),
   fmt(R32_UINT, .format = R32_UINT, .cpp = 4, .channels = 1,),
   fmt(R32_SINT, .format = R32_SINT, .cpp = 4, .channels = 1,),
   fmt(R32_SFLOAT, .format = R32_FLOAT, .cpp = 4, .channels = 1,),
   fmt(R32G32_UINT, .format = R32G32_UINT, .cpp = 8, .channels = 2,),
   fmt(R32G32_SINT, .format = R32G32_SINT, .cpp = 8, .channels = 2,),
   fmt(R32G32_SFLOAT, .format = R32G32_FLOAT, .cpp = 8, .channels = 2,),
   fmt(R32G32B32_UINT, .format = R32G32B32_UINT, .cpp = 12, .channels = 3,),
   fmt(R32G32B32_SINT, .format = R32G32B32_SINT, .cpp = 12, .channels = 3,),
   fmt(R32G32B32_SFLOAT, .format = R32G32B32_FLOAT, .cpp = 12, .channels = 3,),
   fmt(R32G32B32A32_UINT, .format = R32G32B32A32_UINT, .cpp = 16, .channels = 4,),
   fmt(R32G32B32A32_SINT, .format = R32G32B32A32_SINT, .cpp = 16, .channels = 4,),
   fmt(R32G32B32A32_SFLOAT, .format = R32G32B32A32_FLOAT, .cpp = 16, .channels = 4,),
   fmt(R64_SFLOAT, .format = R64_FLOAT, .cpp = 8, .channels = 1),
   fmt(R64G64_SFLOAT, .format = R64G64_FLOAT, .cpp = 16, .channels = 2),
   fmt(R64G64B64_SFLOAT, .format = R64G64B64_FLOAT, .cpp = 24, .channels = 3),
   fmt(R64G64B64A64_SFLOAT, .format = R64G64B64A64_FLOAT, .cpp = 32, .channels = 4),
   fmt(R11G11B10_UFLOAT, .format = R11G11B10_FLOAT, .cpp = 4, .channels = 3),
   fmt(R9G9B9E5_UFLOAT, .format = R9G9B9E5_SHAREDEXP, .cpp = 4, .channels = 3),

   /* For depth/stencil formats, the .format and .cpp fields describe the
    * depth format. The field .has_stencil indicates whether or not there's a
    * stencil buffer.
    */
   fmt(D16_UNORM, .format = D16_UNORM, .cpp = 2, .channels = 1),
   fmt(D24_UNORM, .format = D24_UNORM_X8_UINT, .cpp = 4, .channels = 1),
   fmt(D32_SFLOAT, .format = D32_FLOAT, .cpp = 4, .channels = 1),
   fmt(S8_UINT, .format = UNSUPPORTED, .cpp = 0, .channels = 1, .has_stencil = true),
   fmt(D16_UNORM_S8_UINT, .format = D16_UNORM, .cpp = 2, .channels = 2, .has_stencil = true),
   fmt(D24_UNORM_S8_UINT, .format = D24_UNORM_X8_UINT, .cpp = 4, .channels = 2, .has_stencil = true),
   fmt(D32_SFLOAT_S8_UINT, .format = D32_FLOAT, .cpp = 4, .channels = 2, .has_stencil = true),

   fmt(BC1_RGB_UNORM, .format = UNSUPPORTED),
   fmt(BC1_RGB_SRGB, .format = UNSUPPORTED),
   fmt(BC1_RGBA_UNORM, .format = UNSUPPORTED),
   fmt(BC1_RGBA_SRGB, .format = UNSUPPORTED),
   fmt(BC2_UNORM, .format = UNSUPPORTED),
   fmt(BC2_SRGB, .format = UNSUPPORTED),
   fmt(BC3_UNORM, .format = UNSUPPORTED),
   fmt(BC3_SRGB, .format = UNSUPPORTED),
   fmt(BC4_UNORM, .format = UNSUPPORTED),
   fmt(BC4_SNORM, .format = UNSUPPORTED),
   fmt(BC5_UNORM, .format = UNSUPPORTED),
   fmt(BC5_SNORM, .format = UNSUPPORTED),
   fmt(BC6H_UFLOAT, .format = UNSUPPORTED),
   fmt(BC6H_SFLOAT, .format = UNSUPPORTED),
   fmt(BC7_UNORM, .format = UNSUPPORTED),
   fmt(BC7_SRGB, .format = UNSUPPORTED),
   fmt(ETC2_R8G8B8_UNORM, .format = UNSUPPORTED),
   fmt(ETC2_R8G8B8_SRGB, .format = UNSUPPORTED),
   fmt(ETC2_R8G8B8A1_UNORM, .format = UNSUPPORTED),
   fmt(ETC2_R8G8B8A1_SRGB, .format = UNSUPPORTED),
   fmt(ETC2_R8G8B8A8_UNORM, .format = UNSUPPORTED),
   fmt(ETC2_R8G8B8A8_SRGB, .format = UNSUPPORTED),
   fmt(EAC_R11_UNORM, .format = UNSUPPORTED),
   fmt(EAC_R11_SNORM, .format = UNSUPPORTED),
   fmt(EAC_R11G11_UNORM, .format = UNSUPPORTED),
   fmt(EAC_R11G11_SNORM, .format = UNSUPPORTED),
   fmt(ASTC_4x4_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_4x4_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_5x4_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_5x4_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_5x5_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_5x5_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_6x5_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_6x5_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_6x6_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_6x6_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_8x5_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_8x5_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_8x6_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_8x6_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_8x8_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_8x8_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_10x5_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_10x5_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_10x6_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_10x6_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_10x8_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_10x8_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_10x10_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_10x10_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_12x10_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_12x10_SRGB, .format = UNSUPPORTED),
   fmt(ASTC_12x12_UNORM, .format = UNSUPPORTED),
   fmt(ASTC_12x12_SRGB, .format = UNSUPPORTED),
   fmt(B4G4R4A4_UNORM, .format = B4G4R4A4_UNORM, .cpp = 2, .channels = 4),
   fmt(B5G5R5A1_UNORM, .format = B5G5R5A1_UNORM, .cpp = 2, .channels = 4),
   fmt(B5G6R5_UNORM, .format = B5G6R5_UNORM, .cpp = 2, .channels = 3),
   fmt(B5G6R5_USCALED, .format = UNSUPPORTED),
   fmt(B8G8R8_UNORM, .format = UNSUPPORTED),
   fmt(B8G8R8_SNORM, .format = UNSUPPORTED),
   fmt(B8G8R8_USCALED, .format = UNSUPPORTED),
   fmt(B8G8R8_SSCALED, .format = UNSUPPORTED),
   fmt(B8G8R8_UINT, .format = UNSUPPORTED),
   fmt(B8G8R8_SINT, .format = UNSUPPORTED),
   fmt(B8G8R8_SRGB, .format = UNSUPPORTED),
   fmt(B8G8R8A8_UNORM, .format = B8G8R8A8_UNORM, .cpp = 4, .channels = 4),
   fmt(B8G8R8A8_SNORM, .format = UNSUPPORTED),
   fmt(B8G8R8A8_USCALED, .format = UNSUPPORTED),
   fmt(B8G8R8A8_SSCALED, .format = UNSUPPORTED),
   fmt(B8G8R8A8_UINT, .format = UNSUPPORTED),
   fmt(B8G8R8A8_SINT, .format = UNSUPPORTED),
   fmt(B8G8R8A8_SRGB, .format = B8G8R8A8_UNORM_SRGB, .cpp = 4, .channels = 4),
   fmt(B10G10R10A2_UNORM, .format = B10G10R10A2_UNORM, .cpp = 4, .channels = 4),
   fmt(B10G10R10A2_SNORM, .format = B10G10R10A2_SNORM, .cpp = 4, .channels = 4),
   fmt(B10G10R10A2_USCALED, .format = B10G10R10A2_USCALED, .cpp = 4, .channels = 4),
   fmt(B10G10R10A2_SSCALED, .format = B10G10R10A2_SSCALED, .cpp = 4, .channels = 4),
   fmt(B10G10R10A2_UINT, .format = B10G10R10A2_UINT, .cpp = 4, .channels = 4),
   fmt(B10G10R10A2_SINT, .format = B10G10R10A2_SINT, .cpp = 4, .channels = 4)
};

const struct anv_format *
anv_format_for_vk_format(VkFormat format)
{
   return &anv_formats[format];
}

// Format capabilities

struct surface_format_info {
   bool exists;
   int sampling;
   int filtering;
   int shadow_compare;
   int chroma_key;
   int render_target;
   int alpha_blend;
   int input_vb;
   int streamed_output_vb;
   int color_processing;
};

extern const struct surface_format_info surface_formats[];

VkResult anv_GetFormatInfo(
    VkDevice                                    _device,
    VkFormat                                    _format,
    VkFormatInfoType                            infoType,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   struct anv_device *device = (struct anv_device *) _device;
   VkFormatProperties *properties;
   const struct anv_format *format;
   const struct surface_format_info *info;
   int gen;

   gen = device->info.gen * 10;
   if (device->info.is_haswell)
      gen += 5;

   format = anv_format_for_vk_format(_format);
   if (format == 0)
      return vk_error(VK_ERROR_INVALID_VALUE);
   if (format->format == UNSUPPORTED)
      return VK_UNSUPPORTED;
   info = &surface_formats[format->format];
   if (!info->exists)
      return VK_UNSUPPORTED;

   uint32_t linear = 0, tiled = 0;
   if (info->sampling <= gen) {
      linear |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      tiled |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
   }
   if (info->render_target <= gen) {
      linear |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
      tiled |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
   }
   if (info->alpha_blend <= gen) {
      linear |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
      tiled |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
   }
   if (info->input_vb <= gen) {
      linear |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
   }

   switch (infoType) {
   case VK_FORMAT_INFO_TYPE_PROPERTIES:
      properties = pData;

      *pDataSize = sizeof(*properties);
      if (pData == NULL)
         return VK_SUCCESS;

      properties->linearTilingFeatures = linear;
      properties->optimalTilingFeatures = tiled;
      return VK_SUCCESS;

   default:
      return VK_ERROR_INVALID_VALUE;
   }
}
