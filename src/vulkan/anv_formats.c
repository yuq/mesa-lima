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

#include "anv_private.h"

#define UNSUPPORTED 0xffff

#define fmt(__vk_fmt, ...) \
   [__vk_fmt] = { .vk_format = __vk_fmt, .name = #__vk_fmt, __VA_ARGS__ }

static const struct anv_format anv_formats[] = {
   fmt(VK_FORMAT_UNDEFINED,               RAW,                    .cpp = 1,   .num_channels = 1),
   fmt(VK_FORMAT_R4G4_UNORM,              UNSUPPORTED),
   fmt(VK_FORMAT_R4G4_USCALED,            UNSUPPORTED),
   fmt(VK_FORMAT_R4G4B4A4_UNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_R4G4B4A4_USCALED,        UNSUPPORTED),
   fmt(VK_FORMAT_R5G6B5_UNORM,            UNSUPPORTED),
   fmt(VK_FORMAT_R5G6B5_USCALED,          UNSUPPORTED),
   fmt(VK_FORMAT_R5G5B5A1_UNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_R5G5B5A1_USCALED,        UNSUPPORTED),
   fmt(VK_FORMAT_R8_UNORM,                R8_UNORM,               .cpp = 1,   .num_channels = 1),
   fmt(VK_FORMAT_R8_SNORM,                R8_SNORM,               .cpp = 1,   .num_channels = 1,),
   fmt(VK_FORMAT_R8_USCALED,              R8_USCALED,             .cpp = 1,   .num_channels = 1),
   fmt(VK_FORMAT_R8_SSCALED,              R8_SSCALED,             .cpp = 1,   .num_channels = 1),
   fmt(VK_FORMAT_R8_UINT,                 R8_UINT,                .cpp = 1,   .num_channels = 1),
   fmt(VK_FORMAT_R8_SINT,                 R8_SINT,                .cpp = 1,   .num_channels = 1),
   fmt(VK_FORMAT_R8_SRGB,                 UNSUPPORTED),
   fmt(VK_FORMAT_R8G8_UNORM,              R8G8_UNORM,             .cpp = 2,   .num_channels = 2),
   fmt(VK_FORMAT_R8G8_SNORM,              R8G8_SNORM,             .cpp = 2,   .num_channels = 2),
   fmt(VK_FORMAT_R8G8_USCALED,            R8G8_USCALED,           .cpp = 2,   .num_channels = 2),
   fmt(VK_FORMAT_R8G8_SSCALED,            R8G8_SSCALED,           .cpp = 2,   .num_channels = 2),
   fmt(VK_FORMAT_R8G8_UINT,               R8G8_UINT,              .cpp = 2,   .num_channels = 2),
   fmt(VK_FORMAT_R8G8_SINT,               R8G8_SINT,              .cpp = 2,   .num_channels = 2),
   fmt(VK_FORMAT_R8G8_SRGB,               UNSUPPORTED), /* L8A8_UNORM_SRGB */
   fmt(VK_FORMAT_R8G8B8_UNORM,            R8G8B8X8_UNORM,         .cpp = 3,   .num_channels = 3),
   fmt(VK_FORMAT_R8G8B8_SNORM,            R8G8B8_SNORM,           .cpp = 3,   .num_channels = 3),
   fmt(VK_FORMAT_R8G8B8_USCALED,          R8G8B8_USCALED,         .cpp = 3,   .num_channels = 3),
   fmt(VK_FORMAT_R8G8B8_SSCALED,          R8G8B8_SSCALED,         .cpp = 3,   .num_channels = 3),
   fmt(VK_FORMAT_R8G8B8_UINT,             R8G8B8_UINT,            .cpp = 3,   .num_channels = 3),
   fmt(VK_FORMAT_R8G8B8_SINT,             R8G8B8_SINT,            .cpp = 3,   .num_channels = 3),
   fmt(VK_FORMAT_R8G8B8_SRGB,             UNSUPPORTED), /* B8G8R8A8_UNORM_SRGB */
   fmt(VK_FORMAT_R8G8B8A8_UNORM,          R8G8B8A8_UNORM,         .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R8G8B8A8_SNORM,          R8G8B8A8_SNORM,         .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R8G8B8A8_USCALED,        R8G8B8A8_USCALED,       .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R8G8B8A8_SSCALED,        R8G8B8A8_SSCALED,       .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R8G8B8A8_UINT,           R8G8B8A8_UINT,          .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R8G8B8A8_SINT,           R8G8B8A8_SINT,          .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R8G8B8A8_SRGB,           R8G8B8A8_UNORM_SRGB,    .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R10G10B10A2_UNORM,       R10G10B10A2_UNORM,      .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R10G10B10A2_SNORM,       R10G10B10A2_SNORM,      .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R10G10B10A2_USCALED,     R10G10B10A2_USCALED,    .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R10G10B10A2_SSCALED,     R10G10B10A2_SSCALED,    .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R10G10B10A2_UINT,        R10G10B10A2_UINT,       .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R10G10B10A2_SINT,        R10G10B10A2_SINT,       .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_R16_UNORM,               R16_UNORM,              .cpp = 2,   .num_channels = 1),
   fmt(VK_FORMAT_R16_SNORM,               R16_SNORM,              .cpp = 2,   .num_channels = 1),
   fmt(VK_FORMAT_R16_USCALED,             R16_USCALED,            .cpp = 2,   .num_channels = 1),
   fmt(VK_FORMAT_R16_SSCALED,             R16_SSCALED,            .cpp = 2,   .num_channels = 1),
   fmt(VK_FORMAT_R16_UINT,                R16_UINT,               .cpp = 2,   .num_channels = 1),
   fmt(VK_FORMAT_R16_SINT,                R16_SINT,               .cpp = 2,   .num_channels = 1),
   fmt(VK_FORMAT_R16_SFLOAT,              R16_FLOAT,              .cpp = 2,   .num_channels = 1),
   fmt(VK_FORMAT_R16G16_UNORM,            R16G16_UNORM,           .cpp = 4,   .num_channels = 2),
   fmt(VK_FORMAT_R16G16_SNORM,            R16G16_SNORM,           .cpp = 4,   .num_channels = 2),
   fmt(VK_FORMAT_R16G16_USCALED,          R16G16_USCALED,         .cpp = 4,   .num_channels = 2),
   fmt(VK_FORMAT_R16G16_SSCALED,          R16G16_SSCALED,         .cpp = 4,   .num_channels = 2),
   fmt(VK_FORMAT_R16G16_UINT,             R16G16_UINT,            .cpp = 4,   .num_channels = 2),
   fmt(VK_FORMAT_R16G16_SINT,             R16G16_SINT,            .cpp = 4,   .num_channels = 2),
   fmt(VK_FORMAT_R16G16_SFLOAT,           R16G16_FLOAT,           .cpp = 4,   .num_channels = 2),
   fmt(VK_FORMAT_R16G16B16_UNORM,         R16G16B16_UNORM,        .cpp = 6,   .num_channels = 3),
   fmt(VK_FORMAT_R16G16B16_SNORM,         R16G16B16_SNORM,        .cpp = 6,   .num_channels = 3),
   fmt(VK_FORMAT_R16G16B16_USCALED,       R16G16B16_USCALED,      .cpp = 6,   .num_channels = 3),
   fmt(VK_FORMAT_R16G16B16_SSCALED,       R16G16B16_SSCALED,      .cpp = 6,   .num_channels = 3),
   fmt(VK_FORMAT_R16G16B16_UINT,          R16G16B16_UINT,         .cpp = 6,   .num_channels = 3),
   fmt(VK_FORMAT_R16G16B16_SINT,          R16G16B16_SINT,         .cpp = 6,   .num_channels = 3),
   fmt(VK_FORMAT_R16G16B16_SFLOAT,        R16G16B16_FLOAT,        .cpp = 6,   .num_channels = 3),
   fmt(VK_FORMAT_R16G16B16A16_UNORM,      R16G16B16A16_UNORM,     .cpp = 8,   .num_channels = 4),
   fmt(VK_FORMAT_R16G16B16A16_SNORM,      R16G16B16A16_SNORM,     .cpp = 8,   .num_channels = 4),
   fmt(VK_FORMAT_R16G16B16A16_USCALED,    R16G16B16A16_USCALED,   .cpp = 8,   .num_channels = 4),
   fmt(VK_FORMAT_R16G16B16A16_SSCALED,    R16G16B16A16_SSCALED,   .cpp = 8,   .num_channels = 4),
   fmt(VK_FORMAT_R16G16B16A16_UINT,       R16G16B16A16_UINT,      .cpp = 8,   .num_channels = 4),
   fmt(VK_FORMAT_R16G16B16A16_SINT,       R16G16B16A16_SINT,      .cpp = 8,   .num_channels = 4),
   fmt(VK_FORMAT_R16G16B16A16_SFLOAT,     R16G16B16A16_FLOAT,     .cpp = 8,   .num_channels = 4),
   fmt(VK_FORMAT_R32_UINT,                R32_UINT,               .cpp = 4,   .num_channels = 1,),
   fmt(VK_FORMAT_R32_SINT,                R32_SINT,               .cpp = 4,   .num_channels = 1,),
   fmt(VK_FORMAT_R32_SFLOAT,              R32_FLOAT,              .cpp = 4,   .num_channels = 1,),
   fmt(VK_FORMAT_R32G32_UINT,             R32G32_UINT,            .cpp = 8,   .num_channels = 2,),
   fmt(VK_FORMAT_R32G32_SINT,             R32G32_SINT,            .cpp = 8,   .num_channels = 2,),
   fmt(VK_FORMAT_R32G32_SFLOAT,           R32G32_FLOAT,           .cpp = 8,   .num_channels = 2,),
   fmt(VK_FORMAT_R32G32B32_UINT,          R32G32B32_UINT,         .cpp = 12,  .num_channels = 3,),
   fmt(VK_FORMAT_R32G32B32_SINT,          R32G32B32_SINT,         .cpp = 12,  .num_channels = 3,),
   fmt(VK_FORMAT_R32G32B32_SFLOAT,        R32G32B32_FLOAT,        .cpp = 12,  .num_channels = 3,),
   fmt(VK_FORMAT_R32G32B32A32_UINT,       R32G32B32A32_UINT,      .cpp = 16,  .num_channels = 4,),
   fmt(VK_FORMAT_R32G32B32A32_SINT,       R32G32B32A32_SINT,      .cpp = 16,  .num_channels = 4,),
   fmt(VK_FORMAT_R32G32B32A32_SFLOAT,     R32G32B32A32_FLOAT,     .cpp = 16,  .num_channels = 4,),
   fmt(VK_FORMAT_R64_SFLOAT,              R64_FLOAT,              .cpp = 8,   .num_channels = 1),
   fmt(VK_FORMAT_R64G64_SFLOAT,           R64G64_FLOAT,           .cpp = 16,  .num_channels = 2),
   fmt(VK_FORMAT_R64G64B64_SFLOAT,        R64G64B64_FLOAT,        .cpp = 24,  .num_channels = 3),
   fmt(VK_FORMAT_R64G64B64A64_SFLOAT,     R64G64B64A64_FLOAT,     .cpp = 32,  .num_channels = 4),
   fmt(VK_FORMAT_R11G11B10_UFLOAT,        R11G11B10_FLOAT,        .cpp = 4,   .num_channels = 3),
   fmt(VK_FORMAT_R9G9B9E5_UFLOAT,         R9G9B9E5_SHAREDEXP,     .cpp = 4,   .num_channels = 3),

   fmt(VK_FORMAT_D16_UNORM,               R16_UNORM,              .cpp = 2,   .num_channels = 1, .depth_format = D16_UNORM),
   fmt(VK_FORMAT_D24_UNORM,               R24_UNORM_X8_TYPELESS,  .cpp = 4,   .num_channels = 1, .depth_format = D24_UNORM_X8_UINT),
   fmt(VK_FORMAT_D32_SFLOAT,              R32_FLOAT,              .cpp = 4,   .num_channels = 1, .depth_format = D32_FLOAT),
   fmt(VK_FORMAT_S8_UINT,                 R8_UINT,                .cpp = 1,   .num_channels = 1,                                       .has_stencil = true),
   fmt(VK_FORMAT_D16_UNORM_S8_UINT,       R16_UNORM,              .cpp = 2,   .num_channels = 2, .depth_format = D16_UNORM,            .has_stencil = true),
   fmt(VK_FORMAT_D24_UNORM_S8_UINT,       R24_UNORM_X8_TYPELESS,  .cpp = 4,   .num_channels = 2, .depth_format = D24_UNORM_X8_UINT,    .has_stencil = true),
   fmt(VK_FORMAT_D32_SFLOAT_S8_UINT,      R32_FLOAT,              .cpp = 4,   .num_channels = 2, .depth_format = D32_FLOAT,            .has_stencil = true),

   fmt(VK_FORMAT_BC1_RGB_UNORM,           UNSUPPORTED),
   fmt(VK_FORMAT_BC1_RGB_SRGB,            UNSUPPORTED),
   fmt(VK_FORMAT_BC1_RGBA_UNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_BC1_RGBA_SRGB,           UNSUPPORTED),
   fmt(VK_FORMAT_BC2_UNORM,               UNSUPPORTED),
   fmt(VK_FORMAT_BC2_SRGB,                UNSUPPORTED),
   fmt(VK_FORMAT_BC3_UNORM,               UNSUPPORTED),
   fmt(VK_FORMAT_BC3_SRGB,                UNSUPPORTED),
   fmt(VK_FORMAT_BC4_UNORM,               UNSUPPORTED),
   fmt(VK_FORMAT_BC4_SNORM,               UNSUPPORTED),
   fmt(VK_FORMAT_BC5_UNORM,               UNSUPPORTED),
   fmt(VK_FORMAT_BC5_SNORM,               UNSUPPORTED),
   fmt(VK_FORMAT_BC6H_UFLOAT,             UNSUPPORTED),
   fmt(VK_FORMAT_BC6H_SFLOAT,             UNSUPPORTED),
   fmt(VK_FORMAT_BC7_UNORM,               UNSUPPORTED),
   fmt(VK_FORMAT_BC7_SRGB,                UNSUPPORTED),
   fmt(VK_FORMAT_ETC2_R8G8B8_UNORM,       UNSUPPORTED),
   fmt(VK_FORMAT_ETC2_R8G8B8_SRGB,        UNSUPPORTED),
   fmt(VK_FORMAT_ETC2_R8G8B8A1_UNORM,     UNSUPPORTED),
   fmt(VK_FORMAT_ETC2_R8G8B8A1_SRGB,      UNSUPPORTED),
   fmt(VK_FORMAT_ETC2_R8G8B8A8_UNORM,     UNSUPPORTED),
   fmt(VK_FORMAT_ETC2_R8G8B8A8_SRGB,      UNSUPPORTED),
   fmt(VK_FORMAT_EAC_R11_UNORM,           UNSUPPORTED),
   fmt(VK_FORMAT_EAC_R11_SNORM,           UNSUPPORTED),
   fmt(VK_FORMAT_EAC_R11G11_UNORM,        UNSUPPORTED),
   fmt(VK_FORMAT_EAC_R11G11_SNORM,        UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_4x4_UNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_4x4_SRGB,           UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_5x4_UNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_5x4_SRGB,           UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_5x5_UNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_5x5_SRGB,           UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_6x5_UNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_6x5_SRGB,           UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_6x6_UNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_6x6_SRGB,           UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_8x5_UNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_8x5_SRGB,           UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_8x6_UNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_8x6_SRGB,           UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_8x8_UNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_8x8_SRGB,           UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_10x5_UNORM,         UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_10x5_SRGB,          UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_10x6_UNORM,         UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_10x6_SRGB,          UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_10x8_UNORM,         UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_10x8_SRGB,          UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_10x10_UNORM,        UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_10x10_SRGB,         UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_12x10_UNORM,        UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_12x10_SRGB,         UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_12x12_UNORM,        UNSUPPORTED),
   fmt(VK_FORMAT_ASTC_12x12_SRGB,         UNSUPPORTED),
   fmt(VK_FORMAT_B4G4R4A4_UNORM,          B4G4R4A4_UNORM,         .cpp = 2,   .num_channels = 4),
   fmt(VK_FORMAT_B5G5R5A1_UNORM,          B5G5R5A1_UNORM,         .cpp = 2,   .num_channels = 4),
   fmt(VK_FORMAT_B5G6R5_UNORM,            B5G6R5_UNORM,           .cpp = 2,   .num_channels = 3),
   fmt(VK_FORMAT_B5G6R5_USCALED,          UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8_UNORM,            UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8_SNORM,            UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8_USCALED,          UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8_SSCALED,          UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8_UINT,             UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8_SINT,             UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8_SRGB,             UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8A8_UNORM,          B8G8R8A8_UNORM,         .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_B8G8R8A8_SNORM,          UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8A8_USCALED,        UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8A8_SSCALED,        UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8A8_UINT,           UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8A8_SINT,           UNSUPPORTED),
   fmt(VK_FORMAT_B8G8R8A8_SRGB,           B8G8R8A8_UNORM_SRGB,    .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_B10G10R10A2_UNORM,       B10G10R10A2_UNORM,      .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_B10G10R10A2_SNORM,       B10G10R10A2_SNORM,      .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_B10G10R10A2_USCALED,     B10G10R10A2_USCALED,    .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_B10G10R10A2_SSCALED,     B10G10R10A2_SSCALED,    .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_B10G10R10A2_UINT,        B10G10R10A2_UINT,       .cpp = 4,   .num_channels = 4),
   fmt(VK_FORMAT_B10G10R10A2_SINT,        B10G10R10A2_SINT,       .cpp = 4,   .num_channels = 4)
};

#undef fmt

const struct anv_format *const
anv_format_s8_uint = &anv_formats[VK_FORMAT_S8_UINT];

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

VkResult anv_validate_GetPhysicalDeviceFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    _format,
    VkFormatProperties*                         pFormatProperties)
{
   const struct anv_format *format = anv_format_for_vk_format(_format);
   fprintf(stderr, "vkGetFormatProperties(%s)\n", format->name);
   return anv_GetPhysicalDeviceFormatProperties(physicalDevice, _format, pFormatProperties);
}

static VkResult
anv_physical_device_get_format_properties(struct anv_physical_device *physical_device,
                                          const struct anv_format *format,
                                          VkFormatProperties *out_properties)
{
   const struct surface_format_info *info;
   int gen;
   VkFormatFeatureFlags flags;

   if (format == NULL)
      return VK_ERROR_INVALID_VALUE;

   gen = physical_device->info->gen * 10;
   if (physical_device->info->is_haswell)
      gen += 5;

   if (format->surface_format == UNSUPPORTED)
      goto unsupported;

   uint32_t linear = 0, tiled = 0;
   if (anv_format_is_depth_or_stencil(format)) {
      tiled |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      tiled |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
      tiled |= VK_FORMAT_FEATURE_BLIT_SOURCE_BIT;
      if (format->depth_format) {
         tiled |= VK_FORMAT_FEATURE_BLIT_DESTINATION_BIT;
      }
   } else {
      /* The surface_formats table only contains color formats */
      info = &surface_formats[format->surface_format];
      if (!info->exists)
         goto unsupported;

      if (info->sampling <= gen) {
         flags = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                 VK_FORMAT_FEATURE_BLIT_SOURCE_BIT;
         linear |= flags;
         tiled |= flags;
      }
      if (info->render_target <= gen) {
         flags = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                 VK_FORMAT_FEATURE_BLIT_DESTINATION_BIT;
         linear |= flags;
         tiled |= flags;
      }
      if (info->alpha_blend <= gen) {
         linear |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
         tiled |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
      }
      if (info->input_vb <= gen) {
         linear |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
      }
   }

   out_properties->linearTilingFeatures = linear;
   out_properties->optimalTilingFeatures = tiled;
   out_properties->bufferFeatures = 0; /* FINISHME */

   return VK_SUCCESS;

 unsupported:
   out_properties->linearTilingFeatures = 0;
   out_properties->optimalTilingFeatures = 0;

   return VK_SUCCESS;
}


VkResult anv_GetPhysicalDeviceFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    format,
    VkFormatProperties*                         pFormatProperties)
{
   ANV_FROM_HANDLE(anv_physical_device, physical_device, physicalDevice);
   VkResult result;

   result = anv_physical_device_get_format_properties(
               physical_device,
               anv_format_for_vk_format(format),
               pFormatProperties);
   if (result != VK_SUCCESS)
      return vk_error(result);

   return VK_SUCCESS;
}

VkResult anv_GetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    _format,
    VkImageType                                 type,
    VkImageTiling                               tiling,
    VkImageUsageFlags                           usage,
    VkImageCreateFlags                          flags,
    VkImageFormatProperties*                    pImageFormatProperties)
{
   ANV_FROM_HANDLE(anv_physical_device, physical_device, physicalDevice);
   const struct anv_format *format = anv_format_for_vk_format(_format);
   VkFormatProperties format_props;
   VkFormatFeatureFlags format_feature_flags;
   VkResult result;

   result = anv_physical_device_get_format_properties(physical_device, format,
                                                      &format_props);
   if (result != VK_SUCCESS)
      return vk_error(result);

   /* Extract the VkFormatFeatureFlags that are relevant for the queried
    * tiling.
    */
   if (tiling == VK_IMAGE_TILING_LINEAR) {
      format_feature_flags = format_props.linearTilingFeatures;
   } else if (tiling == VK_IMAGE_TILING_OPTIMAL) {
      format_feature_flags = format_props.optimalTilingFeatures;
   } else {
      unreachable("bad VkImageTiling");
   }

   if (usage & VK_IMAGE_USAGE_TRANSFER_SOURCE_BIT) {
      /* Meta implements transfers by sampling from the source image. */
      if (!(format_feature_flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
         goto unsupported;
      }
   }

   if (usage & VK_IMAGE_USAGE_TRANSFER_DESTINATION_BIT) {
      if (format->has_stencil) {
         /* Not yet implemented because copying to a W-tiled surface is crazy
          * hard.
          */
         anv_finishme("support VK_IMAGE_USAGE_TRANSFER_DESTINATION_BIT for "
                      "stencil format");
         goto unsupported;
      }
   }

   if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
         goto unsupported;
      }
   }

   if (usage & VK_IMAGE_USAGE_STORAGE_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
         goto unsupported;
      }
   }

   if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
         goto unsupported;
      }
   }

   if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
         goto unsupported;
      }
   }

   if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
      /* Nothing to check. */
   }

   if (usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) {
      /* Ignore this flag because it was removed from the
       * provisional_I_20150910 header.
       */
   }

   *pImageFormatProperties = (VkImageFormatProperties) {
      /* FINISHME: Support multisampling */
      .maxSamples = 1,

      /* FINISHME: Accurately calculate
       * VkImageFormatProperties::maxResourceSize.
       */
      .maxResourceSize = UINT32_MAX,
   };

    return VK_SUCCESS;

unsupported:
   *pImageFormatProperties = (VkImageFormatProperties) {
      .maxSamples = 0,
      .maxResourceSize = 0,
   };

   return VK_SUCCESS;
}

VkResult anv_GetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    format,
    VkImageType                                 type,
    uint32_t                                    samples,
    VkImageUsageFlags                           usage,
    VkImageTiling                               tiling,
    uint32_t*                                   pNumProperties,
    VkSparseImageFormatProperties*              pProperties)
{
   /* Sparse images are not yet supported. */
   *pNumProperties = 0;

   return VK_SUCCESS;
}
