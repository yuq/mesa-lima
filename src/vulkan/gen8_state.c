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

#include "genxml/gen8_pack.h"
#include "genxml/gen9_pack.h"

#include "genX_state_util.h"

VkResult
genX(init_device_state)(struct anv_device *device)
{
   struct anv_batch batch;

   uint32_t cmds[64];
   batch.start = batch.next = cmds;
   batch.end = (void *) cmds + sizeof(cmds);

   anv_batch_emit(&batch, GENX(PIPELINE_SELECT),
#if ANV_GEN >= 9
                  .MaskBits = 3,
#endif
                  .PipelineSelection = _3D);

   anv_batch_emit(&batch, GENX(3DSTATE_VF_STATISTICS),
                  .StatisticsEnable = true);
   anv_batch_emit(&batch, GENX(3DSTATE_HS), .Enable = false);
   anv_batch_emit(&batch, GENX(3DSTATE_TE), .TEEnable = false);
   anv_batch_emit(&batch, GENX(3DSTATE_DS), .FunctionEnable = false);
   anv_batch_emit(&batch, GENX(3DSTATE_STREAMOUT), .SOFunctionEnable = false);
   anv_batch_emit(&batch, GENX(3DSTATE_WM_CHROMAKEY),
                  .ChromaKeyKillEnable = false);
   anv_batch_emit(&batch, GENX(3DSTATE_AA_LINE_PARAMETERS));

   /* See the Vulkan 1.0 spec Table 24.1 "Standard sample locations" and
    * VkPhysicalDeviceFeatures::standardSampleLocations.
    */
   anv_batch_emit(&batch, GENX(3DSTATE_SAMPLE_PATTERN),
      ._1xSample0XOffset      = 0.5,
      ._1xSample0YOffset      = 0.5,
      ._2xSample0XOffset      = 0.25,
      ._2xSample0YOffset      = 0.25,
      ._2xSample1XOffset      = 0.75,
      ._2xSample1YOffset      = 0.75,
      ._4xSample0XOffset      = 0.375,
      ._4xSample0YOffset      = 0.125,
      ._4xSample1XOffset      = 0.875,
      ._4xSample1YOffset      = 0.375,
      ._4xSample2XOffset      = 0.125,
      ._4xSample2YOffset      = 0.625,
      ._4xSample3XOffset      = 0.625,
      ._4xSample3YOffset      = 0.875,
      ._8xSample0XOffset      = 0.5625,
      ._8xSample0YOffset      = 0.3125,
      ._8xSample1XOffset      = 0.4375,
      ._8xSample1YOffset      = 0.6875,
      ._8xSample2XOffset      = 0.8125,
      ._8xSample2YOffset      = 0.5625,
      ._8xSample3XOffset      = 0.3125,
      ._8xSample3YOffset      = 0.1875,
      ._8xSample4XOffset      = 0.1875,
      ._8xSample4YOffset      = 0.8125,
      ._8xSample5XOffset      = 0.0625,
      ._8xSample5YOffset      = 0.4375,
      ._8xSample6XOffset      = 0.6875,
      ._8xSample6YOffset      = 0.9375,
      ._8xSample7XOffset      = 0.9375,
      ._8xSample7YOffset      = 0.0625,
#if ANV_GEN >= 9
      ._16xSample0XOffset     = 0.5625,
      ._16xSample0YOffset     = 0.5625,
      ._16xSample1XOffset     = 0.4375,
      ._16xSample1YOffset     = 0.3125,
      ._16xSample2XOffset     = 0.3125,
      ._16xSample2YOffset     = 0.6250,
      ._16xSample3XOffset     = 0.7500,
      ._16xSample3YOffset     = 0.4375,
      ._16xSample4XOffset     = 0.1875,
      ._16xSample4YOffset     = 0.3750,
      ._16xSample5XOffset     = 0.6250,
      ._16xSample5YOffset     = 0.8125,
      ._16xSample6XOffset     = 0.8125,
      ._16xSample6YOffset     = 0.6875,
      ._16xSample7XOffset     = 0.6875,
      ._16xSample7YOffset     = 0.1875,
      ._16xSample8XOffset     = 0.3750,
      ._16xSample8YOffset     = 0.8750,
      ._16xSample9XOffset     = 0.5000,
      ._16xSample9YOffset     = 0.0625,
      ._16xSample10XOffset    = 0.2500,
      ._16xSample10YOffset    = 0.1250,
      ._16xSample11XOffset    = 0.1250,
      ._16xSample11YOffset    = 0.7500,
      ._16xSample12XOffset    = 0.0000,
      ._16xSample12YOffset    = 0.5000,
      ._16xSample13XOffset    = 0.9375,
      ._16xSample13YOffset    = 0.2500,
      ._16xSample14XOffset    = 0.8750,
      ._16xSample14YOffset    = 0.9375,
      ._16xSample15XOffset    = 0.0625,
      ._16xSample15YOffset    = 0.0000,
#endif
   );

   anv_batch_emit(&batch, GENX(MI_BATCH_BUFFER_END));

   assert(batch.next <= batch.end);

   return anv_device_submit_simple_batch(device, &batch);
}

static const uint32_t
isl_to_gen_multisample_layout[] = {
   [ISL_MSAA_LAYOUT_NONE]           = MSS,
   [ISL_MSAA_LAYOUT_INTERLEAVED]    = DEPTH_STENCIL,
   [ISL_MSAA_LAYOUT_ARRAY]          = MSS,
};

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
      #if ANV_GEN >= 9
         return isl_surf_get_array_pitch_el_rows(surf);
      #else
         /* From the Broadwell PRM for RENDER_SURFACE_STATE.QPitch
          *
          *    "This field must be set to an integer multiple of the Surface
          *    Vertical Alignment. For compressed textures (BC*, FXT1,
          *    ETC*, and EAC* Surface Formats), this field is in units of
          *    rows in the uncompressed surface, and must be set to an
          *    integer multiple of the vertical alignment parameter "j"
          *    defined in the Common Surface Formats section."
          */
         return isl_surf_get_array_pitch_sa_rows(surf);
      #endif
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
   bool is_storage = (usage == VK_IMAGE_USAGE_STORAGE_BIT);
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
      .SurfaceType = anv_surftype(image, pCreateInfo->viewType, is_storage),
      .SurfaceArray = image->array_size > 1,
      .SurfaceFormat = anv_surface_format(device, iview->format, is_storage),
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
      .Height = iview->level_0_extent.height - 1,
      .Width  = iview->level_0_extent.width  - 1,
      .Depth = 0, /* TEMPLATE */
      .SurfacePitch = surface->isl.row_pitch - 1,
      .RenderTargetViewExtent = 0, /* TEMPLATE */
      .MinimumArrayElement = 0, /* TEMPLATE */
      .MultisampledSurfaceStorageFormat =
         isl_to_gen_multisample_layout[surface->isl.msaa_layout],
      .NumberofMultisamples = ffs(surface->isl.samples) - 1,
      .MultisamplePositionPaletteIndex = 0, /* UNUSED */
      .XOffset = 0,
      .YOffset = 0,

      .MIPCountLOD = 0, /* TEMPLATE */
      .SurfaceMinLOD = 0, /* TEMPLATE */

      .AuxiliarySurfaceMode = AUX_NONE,
      .RedClearColor = 0,
      .GreenClearColor = 0,
      .BlueClearColor = 0,
      .AlphaClearColor = 0,
      .ShaderChannelSelectRed = vk_to_gen_swizzle[iview->swizzle.r],
      .ShaderChannelSelectGreen = vk_to_gen_swizzle[iview->swizzle.g],
      .ShaderChannelSelectBlue = vk_to_gen_swizzle[iview->swizzle.b],
      .ShaderChannelSelectAlpha = vk_to_gen_swizzle[iview->swizzle.a],
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
      template.MinimumArrayElement = range->baseArrayLayer;
      /* Same as SURFTYPE_2D, but divided by 6 */
      template.Depth = range->layerCount / 6 - 1;
      template.RenderTargetViewExtent = template.Depth;
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

   uint32_t border_color_offset = device->border_colors.offset +
                                  pCreateInfo->borderColor * 64;

   struct GENX(SAMPLER_STATE) sampler_state = {
      .SamplerDisable = false,
      .TextureBorderColorMode = DX10OGL,
      .LODPreClampMode = CLAMP_MODE_OGL,
#if ANV_GEN == 8
      .BaseMipLevel = 0.0,
#endif
      .MipModeFilter = vk_to_gen_mipmap_mode[pCreateInfo->mipmapMode],
      .MagModeFilter = vk_to_gen_tex_filter(pCreateInfo->magFilter, pCreateInfo->anisotropyEnable),
      .MinModeFilter = vk_to_gen_tex_filter(pCreateInfo->minFilter, pCreateInfo->anisotropyEnable),
      .TextureLODBias = anv_clamp_f(pCreateInfo->mipLodBias, -16, 15.996),
      .AnisotropicAlgorithm = EWAApproximation,
      .MinLOD = anv_clamp_f(pCreateInfo->minLod, 0, 14),
      .MaxLOD = anv_clamp_f(pCreateInfo->maxLod, 0, 14),
      .ChromaKeyEnable = 0,
      .ChromaKeyIndex = 0,
      .ChromaKeyMode = 0,
      .ShadowFunction = vk_to_gen_compare_op[pCreateInfo->compareOp],
      .CubeSurfaceControlMode = OVERRIDE,

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
