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

#include "gen8_pack.h"
#include "gen9_pack.h"

#include "genX_state_util.h"

void
genX(fill_buffer_surface_state)(void *state, enum isl_format format,
                                uint32_t offset, uint32_t range, uint32_t stride)
{
   uint32_t num_elements = range / stride;

   struct GENX(RENDER_SURFACE_STATE) surface_state = {
      .SurfaceType = SURFTYPE_BUFFER,
      .SurfaceArray = false,
      .SurfaceFormat = format,
      .SurfaceVerticalAlignment = VALIGN4,
      .SurfaceHorizontalAlignment = HALIGN4,
      .TileMode = LINEAR,
      .SamplerL2BypassModeDisable = true,
      .RenderCacheReadWriteMode = WriteOnlyCache,
      .MemoryObjectControlState = GENX(MOCS),
      .Height = ((num_elements - 1) >> 7) & 0x3fff,
      .Width = (num_elements - 1) & 0x7f,
      .Depth = ((num_elements - 1) >> 21) & 0x3f,
      .SurfacePitch = stride - 1,
      .NumberofMultisamples = MULTISAMPLECOUNT_1,
      .ShaderChannelSelectRed = SCS_RED,
      .ShaderChannelSelectGreen = SCS_GREEN,
      .ShaderChannelSelectBlue = SCS_BLUE,
      .ShaderChannelSelectAlpha = SCS_ALPHA,
      /* FIXME: We assume that the image must be bound at this time. */
      .SurfaceBaseAddress = { NULL, offset },
   };

   GENX(RENDER_SURFACE_STATE_pack)(NULL, state, &surface_state);
}

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

/**
 * Get the values to pack into RENDER_SUFFACE_STATE.SurfaceHorizontalAlignment
 * and SurfaceVerticalAlignment.
 */
static void
get_halign_valign(const struct isl_surf *surf, uint32_t *halign, uint32_t *valign)
{
   #if ANV_GENx10 >= 90
      if (isl_tiling_is_std_y(surf->tiling) ||
          surf->dim_layout == ISL_DIM_LAYOUT_GEN9_1D) {
         /* The hardware ignores the alignment values. Anyway, the surface's
          * true alignment is likely outside the enum range of HALIGN* and
          * VALIGN*.
          */
         *halign = 0;
         *valign = 0;
      } else {
         /* In Skylake, RENDER_SUFFACE_STATE.SurfaceVerticalAlignment is in units
          * of surface elements (not pixels nor samples). For compressed formats,
          * a "surface element" is defined as a compression block.  For example,
          * if SurfaceVerticalAlignment is VALIGN_4 and SurfaceFormat is an ETC2
          * format (ETC2 has a block height of 4), then the vertical alignment is
          * 4 compression blocks or, equivalently, 16 pixels.
          */
         struct isl_extent3d image_align_el
            = isl_surf_get_image_alignment_el(surf);

         *halign = anv_halign[image_align_el.width];
         *valign = anv_valign[image_align_el.height];
      }
   #else
      /* Pre-Skylake, RENDER_SUFFACE_STATE.SurfaceVerticalAlignment is in
       * units of surface samples.  For example, if SurfaceVerticalAlignment
       * is VALIGN_4 and the surface is singlesampled, then for any surface
       * format (compressed or not) the vertical alignment is
       * 4 pixels.
       */
      struct isl_extent3d image_align_sa
         = isl_surf_get_image_alignment_sa(surf);

      *halign = anv_halign[image_align_sa.width];
      *valign = anv_valign[image_align_sa.height];
   #endif
}

static uint32_t
get_qpitch(const struct isl_surf *surf)
{
   switch (surf->dim) {
   default:
      unreachable(!"bad isl_surf_dim");
   case ISL_SURF_DIM_1D:
      #if ANV_GENx10 >= 90
         /* QPitch is usually expressed as rows of surface elements (where
          * a surface element is an compression block or a single surface
          * sample). Skylake 1D is an outlier.
          *
          * From the Skylake BSpec >> Memory Views >> Common Surface
          * Formats >> Surface Layout and Tiling >> 1D Surfaces:
          *
          *    Surface QPitch specifies the distance in pixels between array
          *    slices.
          */
         return isl_surf_get_array_pitch_el(surf);
      #else
         return isl_surf_get_array_pitch_el_rows(surf);
      #endif
   case ISL_SURF_DIM_2D:
   case ISL_SURF_DIM_3D:
      return isl_surf_get_array_pitch_el_rows(surf);
   }
}

void
genX(fill_image_surface_state)(struct anv_device *device, void *state_map,
                               struct anv_image_view *iview,
                               const VkImageViewCreateInfo *pCreateInfo,
                               VkImageUsageFlagBits usage)
{
   assert(usage & (VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_STORAGE_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
   assert(util_is_power_of_two(usage));

   ANV_FROM_HANDLE(anv_image, image, pCreateInfo->image);
   const VkImageSubresourceRange *range = &pCreateInfo->subresourceRange;
   struct anv_surface *surface =
      anv_image_get_surface_for_aspect_mask(image, range->aspectMask);

   static const uint8_t isl_to_gen_tiling[] = {
      [ISL_TILING_LINEAR]  = LINEAR,
      [ISL_TILING_X]       = XMAJOR,
      [ISL_TILING_Y0]      = YMAJOR,
      [ISL_TILING_Yf]      = YMAJOR,
      [ISL_TILING_Ys]      = YMAJOR,
      [ISL_TILING_W]       = WMAJOR,
   };

   uint32_t halign, valign;
   get_halign_valign(&surface->isl, &halign, &valign);

   struct GENX(RENDER_SURFACE_STATE) template = {
      .SurfaceType = anv_surftype(image, pCreateInfo->viewType,
                                  usage == VK_IMAGE_USAGE_STORAGE_BIT),
      .SurfaceArray = image->array_size > 1,
      .SurfaceFormat = (usage != VK_IMAGE_USAGE_STORAGE_BIT ? iview->format :
                        isl_lower_storage_image_format(
                           &device->isl_dev, iview->format)),
      .SurfaceVerticalAlignment = valign,
      .SurfaceHorizontalAlignment = halign,
      .TileMode = isl_to_gen_tiling[surface->isl.tiling],
      .VerticalLineStride = 0,
      .VerticalLineStrideOffset = 0,
      .SamplerL2BypassModeDisable = true,
      .RenderCacheReadWriteMode = WriteOnlyCache,
      .CubeFaceEnablePositiveZ = 1,
      .CubeFaceEnableNegativeZ = 1,
      .CubeFaceEnablePositiveY = 1,
      .CubeFaceEnableNegativeY = 1,
      .CubeFaceEnablePositiveX = 1,
      .CubeFaceEnableNegativeX = 1,
      .MemoryObjectControlState = GENX(MOCS),

      /* The driver sets BaseMipLevel in SAMPLER_STATE, not here in
       * RENDER_SURFACE_STATE. The Broadwell PRM says "it is illegal to have
       * both Base Mip Level fields nonzero".
       */
      .BaseMipLevel = 0.0,

      .SurfaceQPitch = get_qpitch(&surface->isl) >> 2,
      .Height = image->extent.height - 1,
      .Width = image->extent.width - 1,
      .Depth = 0, /* TEMPLATE */
      .SurfacePitch = surface->isl.row_pitch - 1,
      .RenderTargetViewExtent = 0, /* TEMPLATE */
      .MinimumArrayElement = 0, /* TEMPLATE */
      .NumberofMultisamples = MULTISAMPLECOUNT_1,
      .XOffset = 0,
      .YOffset = 0,

      .MIPCountLOD = 0, /* TEMPLATE */
      .SurfaceMinLOD = 0, /* TEMPLATE */

      .AuxiliarySurfaceMode = AUX_NONE,
      .RedClearColor = 0,
      .GreenClearColor = 0,
      .BlueClearColor = 0,
      .AlphaClearColor = 0,
      .ShaderChannelSelectRed = vk_to_gen_swizzle(pCreateInfo->components.r,
                                                  VK_COMPONENT_SWIZZLE_R),
      .ShaderChannelSelectGreen = vk_to_gen_swizzle(pCreateInfo->components.g,
                                                    VK_COMPONENT_SWIZZLE_G),
      .ShaderChannelSelectBlue = vk_to_gen_swizzle(pCreateInfo->components.b,
                                                   VK_COMPONENT_SWIZZLE_B),
      .ShaderChannelSelectAlpha = vk_to_gen_swizzle(pCreateInfo->components.a,
                                                    VK_COMPONENT_SWIZZLE_A),
      .ResourceMinLOD = 0.0,
      .SurfaceBaseAddress = { NULL, iview->offset },
   };

   switch (template.SurfaceType) {
   case SURFTYPE_1D:
   case SURFTYPE_2D:
      template.MinimumArrayElement = range->baseArrayLayer;

      /* From the Broadwell PRM >> RENDER_SURFACE_STATE::Depth:
       *
       *    For SURFTYPE_1D, 2D, and CUBE: The range of this field is reduced
       *    by one for each increase from zero of Minimum Array Element. For
       *    example, if Minimum Array Element is set to 1024 on a 2D surface,
       *    the range of this field is reduced to [0,1023].
       *
       * In other words, 'Depth' is the number of array layers.
       */
      template.Depth = range->layerCount - 1;

      /* From the Broadwell PRM >> RENDER_SURFACE_STATE::RenderTargetViewExtent:
       *
       *    For Render Target and Typed Dataport 1D and 2D Surfaces:
       *    This field must be set to the same value as the Depth field.
       */
      template.RenderTargetViewExtent = template.Depth;
      break;
   case SURFTYPE_CUBE:
      #if ANV_GENx10 >= 90
         /* Like SURFTYPE_2D, but divided by 6. */
         template.MinimumArrayElement = range->baseArrayLayer / 6;
         template.Depth = range->layerCount / 6 - 1;
         template.RenderTargetViewExtent = template.Depth;
      #else
         /* Same as SURFTYPE_2D */
         template.MinimumArrayElement = range->baseArrayLayer;
         template.Depth = range->layerCount - 1;
         template.RenderTargetViewExtent = template.Depth;
      #endif
      break;
   case SURFTYPE_3D:
      template.MinimumArrayElement = range->baseArrayLayer;

      /* From the Broadwell PRM >> RENDER_SURFACE_STATE::Depth:
       *
       *    If the volume texture is MIP-mapped, this field specifies the
       *    depth of the base MIP level.
       */
      template.Depth = image->extent.depth - 1;

      /* From the Broadwell PRM >> RENDER_SURFACE_STATE::RenderTargetViewExtent:
       *
       *    For Render Target and Typed Dataport 3D Surfaces: This field
       *    indicates the extent of the accessible 'R' coordinates minus 1 on
       *    the LOD currently being rendered to.
       */
      template.RenderTargetViewExtent = iview->extent.depth - 1;
      break;
   default:
      unreachable(!"bad SurfaceType");
   }

   if (usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      /* For render target surfaces, the hardware interprets field
       * MIPCount/LOD as LOD. The Broadwell PRM says:
       *
       *    MIPCountLOD defines the LOD that will be rendered into.
       *    SurfaceMinLOD is ignored.
       */
      template.MIPCountLOD = range->baseMipLevel;
      template.SurfaceMinLOD = 0;
   } else {
      /* For non render target surfaces, the hardware interprets field
       * MIPCount/LOD as MIPCount.  The range of levels accessible by the
       * sampler engine is [SurfaceMinLOD, SurfaceMinLOD + MIPCountLOD].
       */
      template.SurfaceMinLOD = range->baseMipLevel;
      template.MIPCountLOD = MAX2(range->levelCount, 1) - 1;
   }

   GENX(RENDER_SURFACE_STATE_pack)(NULL, state_map, &template);
}

VkResult genX(CreateSampler)(
    VkDevice                                    _device,
    const VkSamplerCreateInfo*                  pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSampler*                                  pSampler)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_sampler *sampler;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

   sampler = anv_alloc2(&device->alloc, pAllocator, sizeof(*sampler), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!sampler)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   uint32_t filter = vk_to_gen_tex_filter(pCreateInfo->magFilter,
                                          pCreateInfo->anisotropyEnable);

   uint32_t border_color_offset = device->border_colors.offset +
                                  pCreateInfo->borderColor * 64;

   struct GENX(SAMPLER_STATE) sampler_state = {
      .SamplerDisable = false,
      .TextureBorderColorMode = DX10OGL,
      .LODPreClampMode = CLAMP_OGL,
#if ANV_GEN == 8
      .BaseMipLevel = 0.0,
#endif
      .MipModeFilter = vk_to_gen_mipmap_mode[pCreateInfo->mipmapMode],
      .MagModeFilter = filter,
      .MinModeFilter = filter,
      .TextureLODBias = anv_clamp_f(pCreateInfo->mipLodBias, -16, 15.996),
      .AnisotropicAlgorithm = EWAApproximation,
      .MinLOD = anv_clamp_f(pCreateInfo->minLod, 0, 14),
      .MaxLOD = anv_clamp_f(pCreateInfo->maxLod, 0, 14),
      .ChromaKeyEnable = 0,
      .ChromaKeyIndex = 0,
      .ChromaKeyMode = 0,
      .ShadowFunction = vk_to_gen_compare_op[pCreateInfo->compareOp],
      .CubeSurfaceControlMode = 0,

      .IndirectStatePointer = border_color_offset >> 6,

      .LODClampMagnificationMode = MIPNONE,
      .MaximumAnisotropy = vk_to_gen_max_anisotropy(pCreateInfo->maxAnisotropy),
      .RAddressMinFilterRoundingEnable = 0,
      .RAddressMagFilterRoundingEnable = 0,
      .VAddressMinFilterRoundingEnable = 0,
      .VAddressMagFilterRoundingEnable = 0,
      .UAddressMinFilterRoundingEnable = 0,
      .UAddressMagFilterRoundingEnable = 0,
      .TrilinearFilterQuality = 0,
      .NonnormalizedCoordinateEnable = pCreateInfo->unnormalizedCoordinates,
      .TCXAddressControlMode = vk_to_gen_tex_address[pCreateInfo->addressModeU],
      .TCYAddressControlMode = vk_to_gen_tex_address[pCreateInfo->addressModeV],
      .TCZAddressControlMode = vk_to_gen_tex_address[pCreateInfo->addressModeW],
   };

   GENX(SAMPLER_STATE_pack)(NULL, sampler->state, &sampler_state);

   *pSampler = anv_sampler_to_handle(sampler);

   return VK_SUCCESS;
}
