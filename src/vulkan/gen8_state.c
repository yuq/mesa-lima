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
genX(image_view_init)(struct anv_image_view *iview,
                      struct anv_device *device,
                      const VkImageViewCreateInfo* pCreateInfo,
                      struct anv_cmd_buffer *cmd_buffer)
{
   ANV_FROM_HANDLE(anv_image, image, pCreateInfo->image);

   const VkImageSubresourceRange *range = &pCreateInfo->subresourceRange;

   struct anv_surface *surface =
      anv_image_get_surface_for_aspect_mask(image, range->aspectMask);

   uint32_t depth = 1; /* RENDER_SURFACE_STATE::Depth */
   uint32_t rt_view_extent = 1; /* RENDER_SURFACE_STATE::RenderTargetViewExtent */

   switch (image->type) {
   case VK_IMAGE_TYPE_1D:
   case VK_IMAGE_TYPE_2D:
      /* From the Broadwell PRM >> RENDER_SURFACE_STATE::Depth:
       *
       *    For SURFTYPE_1D, 2D, and CUBE: The range of this field is reduced
       *    by one for each increase from zero of Minimum Array Element. For
       *    example, if Minimum Array Element is set to 1024 on a 2D surface,
       *    the range of this field is reduced to [0,1023].
       */
      depth = range->layerCount;

      /* From the Broadwell PRM >> RENDER_SURFACE_STATE::RenderTargetViewExtent:
       *
       *    For Render Target and Typed Dataport 1D and 2D Surfaces:
       *    This field must be set to the same value as the Depth field.
       */
      rt_view_extent = depth;
      break;
   case VK_IMAGE_TYPE_3D:
      /* From the Broadwell PRM >> RENDER_SURFACE_STATE::Depth:
       *
       *    If the volume texture is MIP-mapped, this field specifies the
       *    depth of the base MIP level.
       */
      depth = image->extent.depth;

      /* From the Broadwell PRM >> RENDER_SURFACE_STATE::RenderTargetViewExtent:
       *
       *    For Render Target and Typed Dataport 3D Surfaces: This field
       *    indicates the extent of the accessible 'R' coordinates minus 1 on
       *    the LOD currently being rendered to.
       */
      rt_view_extent = iview->extent.depth;
      break;
   default:
      unreachable(!"bad VkImageType");
   }

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

   struct GENX(RENDER_SURFACE_STATE) surface_state = {
      .SurfaceType = anv_surftype(image, pCreateInfo->viewType, false),
      .SurfaceArray = image->array_size > 1,
      .SurfaceFormat = iview->format,
      .SurfaceVerticalAlignment = valign,
      .SurfaceHorizontalAlignment = halign,
      .TileMode = isl_to_gen_tiling[surface->isl.tiling],
      .VerticalLineStride = 0,
      .VerticalLineStrideOffset = 0,
      .SamplerL2BypassModeDisable = true,
      .RenderCacheReadWriteMode = WriteOnlyCache,
      .MemoryObjectControlState = GENX(MOCS),

      /* The driver sets BaseMipLevel in SAMPLER_STATE, not here in
       * RENDER_SURFACE_STATE. The Broadwell PRM says "it is illegal to have
       * both Base Mip Level fields nonzero".
       */
      .BaseMipLevel = 0.0,

      .SurfaceQPitch = get_qpitch(&surface->isl) >> 2,
      .Height = image->extent.height - 1,
      .Width = image->extent.width - 1,
      .Depth = depth - 1,
      .SurfacePitch = surface->isl.row_pitch - 1,
      .RenderTargetViewExtent = rt_view_extent - 1,
      .MinimumArrayElement = range->baseArrayLayer,
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

   if (image->needs_nonrt_surface_state) {
      iview->nonrt_surface_state =
         alloc_surface_state(device, cmd_buffer);

      /* For non render target surfaces, the hardware interprets field
       * MIPCount/LOD as MIPCount.  The range of levels accessible by the
       * sampler engine is [SurfaceMinLOD, SurfaceMinLOD + MIPCountLOD].
       */
      surface_state.SurfaceMinLOD = range->baseMipLevel;
      surface_state.MIPCountLOD = MAX2(range->levelCount, 1) - 1;

      GENX(RENDER_SURFACE_STATE_pack)(NULL, iview->nonrt_surface_state.map,
                                      &surface_state);
      if (!device->info.has_llc)
         anv_state_clflush(iview->nonrt_surface_state);
   }

   if (image->needs_color_rt_surface_state) {
      iview->color_rt_surface_state =
         alloc_surface_state(device, cmd_buffer);

      /* For render target surfaces, the hardware interprets field
       * MIPCount/LOD as LOD. The Broadwell PRM says:
       *
       *    MIPCountLOD defines the LOD that will be rendered into.
       *    SurfaceMinLOD is ignored.
       */
      surface_state.MIPCountLOD = range->baseMipLevel;
      surface_state.SurfaceMinLOD = 0;

      GENX(RENDER_SURFACE_STATE_pack)(NULL, iview->color_rt_surface_state.map,
                                      &surface_state);
      if (!device->info.has_llc)
         anv_state_clflush(iview->color_rt_surface_state);
   }

   if (image->needs_storage_surface_state) {
      iview->storage_surface_state =
         alloc_surface_state(device, cmd_buffer);

      surface_state.SurfaceType =
         anv_surftype(image, pCreateInfo->viewType, true),

      surface_state.SurfaceFormat =
         isl_lower_storage_image_format(&device->isl_dev, iview->format);

      surface_state.SurfaceMinLOD = range->baseMipLevel;
      surface_state.MIPCountLOD = MAX2(range->levelCount, 1) - 1;

      GENX(RENDER_SURFACE_STATE_pack)(NULL, iview->storage_surface_state.map,
                                      &surface_state);
   }
}

VkResult genX(CreateSampler)(
    VkDevice                                    _device,
    const VkSamplerCreateInfo*                  pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSampler*                                  pSampler)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_sampler *sampler;
   uint32_t mag_filter, min_filter, max_anisotropy;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

   sampler = anv_alloc2(&device->alloc, pAllocator, sizeof(*sampler), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!sampler)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   static const uint32_t vk_to_gen_tex_filter[] = {
      [VK_FILTER_NEAREST]                       = MAPFILTER_NEAREST,
      [VK_FILTER_LINEAR]                        = MAPFILTER_LINEAR
   };

   static const uint32_t vk_to_gen_mipmap_mode[] = {
      [VK_SAMPLER_MIPMAP_MODE_BASE]             = MIPFILTER_NONE,
      [VK_SAMPLER_MIPMAP_MODE_NEAREST]          = MIPFILTER_NEAREST,
      [VK_SAMPLER_MIPMAP_MODE_LINEAR]           = MIPFILTER_LINEAR
   };

   static const uint32_t vk_to_gen_tex_address[] = {
      [VK_SAMPLER_ADDRESS_MODE_REPEAT]          = TCM_WRAP,
      [VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT] = TCM_MIRROR,
      [VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE]   = TCM_CLAMP,
      [VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE] = TCM_MIRROR_ONCE,
      [VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER] = TCM_CLAMP_BORDER,
   };

   static const uint32_t vk_to_gen_compare_op[] = {
      [VK_COMPARE_OP_NEVER]                     = PREFILTEROPNEVER,
      [VK_COMPARE_OP_LESS]                      = PREFILTEROPLESS,
      [VK_COMPARE_OP_EQUAL]                     = PREFILTEROPEQUAL,
      [VK_COMPARE_OP_LESS_OR_EQUAL]             = PREFILTEROPLEQUAL,
      [VK_COMPARE_OP_GREATER]                   = PREFILTEROPGREATER,
      [VK_COMPARE_OP_NOT_EQUAL]                 = PREFILTEROPNOTEQUAL,
      [VK_COMPARE_OP_GREATER_OR_EQUAL]          = PREFILTEROPGEQUAL,
      [VK_COMPARE_OP_ALWAYS]                    = PREFILTEROPALWAYS,
   };

   if (pCreateInfo->maxAnisotropy > 1) {
      mag_filter = MAPFILTER_ANISOTROPIC;
      min_filter = MAPFILTER_ANISOTROPIC;
      max_anisotropy = (pCreateInfo->maxAnisotropy - 2) / 2;
   } else {
      mag_filter = vk_to_gen_tex_filter[pCreateInfo->magFilter];
      min_filter = vk_to_gen_tex_filter[pCreateInfo->minFilter];
      max_anisotropy = RATIO21;
   }

   struct GENX(SAMPLER_STATE) sampler_state = {
      .SamplerDisable = false,
      .TextureBorderColorMode = DX10OGL,
      .LODPreClampMode = 0,
#if ANV_GEN == 8
      .BaseMipLevel = 0.0,
#endif
      .MipModeFilter = vk_to_gen_mipmap_mode[pCreateInfo->mipmapMode],
      .MagModeFilter = mag_filter,
      .MinModeFilter = min_filter,
      .TextureLODBias = anv_clamp_f(pCreateInfo->mipLodBias, -16, 15.996),
      .AnisotropicAlgorithm = EWAApproximation,
      .MinLOD = anv_clamp_f(pCreateInfo->minLod, 0, 14),
      .MaxLOD = anv_clamp_f(pCreateInfo->maxLod, 0, 14),
      .ChromaKeyEnable = 0,
      .ChromaKeyIndex = 0,
      .ChromaKeyMode = 0,
      .ShadowFunction = vk_to_gen_compare_op[pCreateInfo->compareOp],
      .CubeSurfaceControlMode = 0,

      .IndirectStatePointer =
         device->border_colors.offset +
         pCreateInfo->borderColor * sizeof(float) * 4,

      .LODClampMagnificationMode = MIPNONE,
      .MaximumAnisotropy = max_anisotropy,
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
