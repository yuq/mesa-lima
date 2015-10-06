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

VkResult gen8_CreateDynamicRasterState(
    VkDevice                                    _device,
    const VkDynamicRasterStateCreateInfo*       pCreateInfo,
    VkDynamicRasterState*                       pState)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_dynamic_rs_state *state;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DYNAMIC_RASTER_STATE_CREATE_INFO);

   state = anv_device_alloc(device, sizeof(*state), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (state == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   struct GEN8_3DSTATE_SF sf = {
      GEN8_3DSTATE_SF_header,
      .LineWidth = pCreateInfo->lineWidth,
   };

   GEN8_3DSTATE_SF_pack(NULL, state->gen8.sf, &sf);

   bool enable_bias = pCreateInfo->depthBias != 0.0f ||
      pCreateInfo->slopeScaledDepthBias != 0.0f;
   struct GEN8_3DSTATE_RASTER raster = {
      .GlobalDepthOffsetEnableSolid = enable_bias,
      .GlobalDepthOffsetEnableWireframe = enable_bias,
      .GlobalDepthOffsetEnablePoint = enable_bias,
      .GlobalDepthOffsetConstant = pCreateInfo->depthBias,
      .GlobalDepthOffsetScale = pCreateInfo->slopeScaledDepthBias,
      .GlobalDepthOffsetClamp = pCreateInfo->depthBiasClamp
   };

   GEN8_3DSTATE_RASTER_pack(NULL, state->gen8.raster, &raster);

   *pState = anv_dynamic_rs_state_to_handle(state);

   return VK_SUCCESS;
}

void
gen8_fill_buffer_surface_state(void *state, const struct anv_format *format,
                               uint32_t offset, uint32_t range)
{
   /* This assumes RGBA float format. */
   uint32_t stride = 4;
   uint32_t num_elements = range / stride;

   struct GEN8_RENDER_SURFACE_STATE surface_state = {
      .SurfaceType = SURFTYPE_BUFFER,
      .SurfaceArray = false,
      .SurfaceFormat = format->surface_format,
      .SurfaceVerticalAlignment = VALIGN4,
      .SurfaceHorizontalAlignment = HALIGN4,
      .TileMode = LINEAR,
      .SamplerL2BypassModeDisable = true,
      .RenderCacheReadWriteMode = WriteOnlyCache,
      .MemoryObjectControlState = GEN8_MOCS,
      .Height = (num_elements >> 7) & 0x3fff,
      .Width = num_elements & 0x7f,
      .Depth = (num_elements >> 21) & 0x3f,
      .SurfacePitch = stride - 1,
      .NumberofMultisamples = MULTISAMPLECOUNT_1,
      .ShaderChannelSelectRed = SCS_RED,
      .ShaderChannelSelectGreen = SCS_GREEN,
      .ShaderChannelSelectBlue = SCS_BLUE,
      .ShaderChannelSelectAlpha = SCS_ALPHA,
      /* FIXME: We assume that the image must be bound at this time. */
      .SurfaceBaseAddress = { NULL, offset },
   };

   GEN8_RENDER_SURFACE_STATE_pack(NULL, state, &surface_state);
}

VkResult gen8_CreateBufferView(
    VkDevice                                    _device,
    const VkBufferViewCreateInfo*               pCreateInfo,
    VkBufferView*                               pView)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_buffer_view *bview;
   VkResult result;

   result = anv_buffer_view_create(device, pCreateInfo, &bview);
   if (result != VK_SUCCESS)
      return result;

   const struct anv_format *format =
      anv_format_for_vk_format(pCreateInfo->format);

   gen8_fill_buffer_surface_state(bview->surface_state.map, format,
                                  bview->offset, pCreateInfo->range);

   *pView = anv_buffer_view_to_handle(bview);

   return VK_SUCCESS;
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

void
gen8_image_view_init(struct anv_image_view *iview,
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

   const struct anv_format *format_info =
      anv_format_for_vk_format(pCreateInfo->format);

   const struct anv_image_view_info view_type_info =
      anv_image_view_info_for_vk_image_view_type(pCreateInfo->viewType);

   iview->bo = image->bo;
   iview->offset = image->offset + surface->offset;
   iview->format = format_info;

   iview->extent = (VkExtent3D) {
      .width = anv_minify(image->extent.width, range->baseMipLevel),
      .height = anv_minify(image->extent.height, range->baseMipLevel),
      .depth = anv_minify(image->extent.depth, range->baseMipLevel),
   };

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
      depth = range->arraySize;

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

   static const uint32_t vk_to_gen_swizzle[] = {
      [VK_CHANNEL_SWIZZLE_ZERO]                 = SCS_ZERO,
      [VK_CHANNEL_SWIZZLE_ONE]                  = SCS_ONE,
      [VK_CHANNEL_SWIZZLE_R]                    = SCS_RED,
      [VK_CHANNEL_SWIZZLE_G]                    = SCS_GREEN,
      [VK_CHANNEL_SWIZZLE_B]                    = SCS_BLUE,
      [VK_CHANNEL_SWIZZLE_A]                    = SCS_ALPHA
   };

   struct GEN8_RENDER_SURFACE_STATE surface_state = {
      .SurfaceType = view_type_info.surface_type,
      .SurfaceArray = image->array_size > 1,
      .SurfaceFormat = format_info->surface_format,
      .SurfaceVerticalAlignment = anv_valign[surface->v_align],
      .SurfaceHorizontalAlignment = anv_halign[surface->h_align],
      .TileMode = surface->tile_mode,
      .VerticalLineStride = 0,
      .VerticalLineStrideOffset = 0,
      .SamplerL2BypassModeDisable = true,
      .RenderCacheReadWriteMode = WriteOnlyCache,
      .MemoryObjectControlState = GEN8_MOCS,

      /* The driver sets BaseMipLevel in SAMPLER_STATE, not here in
       * RENDER_SURFACE_STATE. The Broadwell PRM says "it is illegal to have
       * both Base Mip Level fields nonzero".
       */
      .BaseMipLevel = 0.0,

      .SurfaceQPitch = surface->qpitch >> 2,
      .Height = image->extent.height - 1,
      .Width = image->extent.width - 1,
      .Depth = depth - 1,
      .SurfacePitch = surface->stride - 1,
      .RenderTargetViewExtent = rt_view_extent - 1,
      .MinimumArrayElement = range->baseArrayLayer,
      .NumberofMultisamples = MULTISAMPLECOUNT_1,
      .XOffset = 0,
      .YOffset = 0,

      /* For sampler surfaces, the hardware interprets field MIPCount/LOD as
       * MIPCount.  The range of levels accessible by the sampler engine is
       * [SurfaceMinLOD, SurfaceMinLOD + MIPCountLOD].
       */
      .MIPCountLOD = range->mipLevels - 1,
      .SurfaceMinLOD = range->baseMipLevel,

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
      .SurfaceBaseAddress = { NULL, iview->offset },
   };

   if (cmd_buffer) {
      iview->surface_state =
         anv_state_stream_alloc(&cmd_buffer->surface_state_stream, 64, 64);
   } else {
      iview->surface_state =
         anv_state_pool_alloc(&device->surface_state_pool, 64, 64);
   }

   GEN8_RENDER_SURFACE_STATE_pack(NULL, iview->surface_state.map,
                                  &surface_state);
}

void
gen8_color_attachment_view_init(struct anv_image_view *iview,
                                struct anv_device *device,
                                const VkAttachmentViewCreateInfo* pCreateInfo,
                                struct anv_cmd_buffer *cmd_buffer)
{
   ANV_FROM_HANDLE(anv_image, image, pCreateInfo->image);
   struct anv_surface *surface =
      anv_image_get_surface_for_color_attachment(image);
   const struct anv_format *format_info =
      anv_format_for_vk_format(pCreateInfo->format);

   uint32_t depth = 1; /* RENDER_SURFACE_STATE::Depth */
   uint32_t rt_view_extent = 1; /* RENDER_SURFACE_STATE::RenderTargetViewExtent */

   anv_assert(pCreateInfo->arraySize > 0);
   anv_assert(pCreateInfo->mipLevel < image->levels);
   anv_assert(pCreateInfo->baseArraySlice + pCreateInfo->arraySize <= image->array_size);

   iview->bo = image->bo;
   iview->offset = image->offset + surface->offset;
   iview->format = anv_format_for_vk_format(pCreateInfo->format);

   iview->extent = (VkExtent3D) {
      .width = anv_minify(image->extent.width, pCreateInfo->mipLevel),
      .height = anv_minify(image->extent.height, pCreateInfo->mipLevel),
      .depth = anv_minify(image->extent.depth, pCreateInfo->mipLevel),
   };

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
      depth = pCreateInfo->arraySize;

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

   if (cmd_buffer) {
      iview->surface_state =
         anv_state_stream_alloc(&cmd_buffer->surface_state_stream, 64, 64);
   } else {
      iview->surface_state =
         anv_state_pool_alloc(&device->surface_state_pool, 64, 64);
   }

   struct GEN8_RENDER_SURFACE_STATE surface_state = {
      .SurfaceType = image->type,
      .SurfaceArray = image->array_size > 1,
      .SurfaceFormat = format_info->surface_format,
      .SurfaceVerticalAlignment = anv_valign[surface->v_align],
      .SurfaceHorizontalAlignment = anv_halign[surface->h_align],
      .TileMode = surface->tile_mode,
      .VerticalLineStride = 0,
      .VerticalLineStrideOffset = 0,
      .SamplerL2BypassModeDisable = true,
      .RenderCacheReadWriteMode = WriteOnlyCache,
      .MemoryObjectControlState = GEN8_MOCS,

      /* The driver sets BaseMipLevel in SAMPLER_STATE, not here in
       * RENDER_SURFACE_STATE. The Broadwell PRM says "it is illegal to have
       * both Base Mip Level fields nonzero".
       */
      .BaseMipLevel = 0.0,

      .SurfaceQPitch = surface->qpitch >> 2,
      .Height = image->extent.height - 1,
      .Width = image->extent.width - 1,
      .Depth = depth - 1,
      .SurfacePitch = surface->stride - 1,
      .RenderTargetViewExtent = rt_view_extent - 1,
      .MinimumArrayElement = pCreateInfo->baseArraySlice,
      .NumberofMultisamples = MULTISAMPLECOUNT_1,
      .XOffset = 0,
      .YOffset = 0,

      /* For render target surfaces, the hardware interprets field MIPCount/LOD as
       * LOD. The Broadwell PRM says:
       *
       *    MIPCountLOD defines the LOD that will be rendered into.
       *    SurfaceMinLOD is ignored.
       */
      .SurfaceMinLOD = 0,
      .MIPCountLOD = pCreateInfo->mipLevel,

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
      .SurfaceBaseAddress = { NULL, iview->offset },
   };

   GEN8_RENDER_SURFACE_STATE_pack(NULL, iview->surface_state.map,
                                  &surface_state);
}

VkResult gen8_CreateSampler(
    VkDevice                                    _device,
    const VkSamplerCreateInfo*                  pCreateInfo,
    VkSampler*                                  pSampler)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_sampler *sampler;
   uint32_t mag_filter, min_filter, max_anisotropy;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

   sampler = anv_device_alloc(device, sizeof(*sampler), 8,
                              VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (!sampler)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   static const uint32_t vk_to_gen_tex_filter[] = {
      [VK_TEX_FILTER_NEAREST]                   = MAPFILTER_NEAREST,
      [VK_TEX_FILTER_LINEAR]                    = MAPFILTER_LINEAR
   };

   static const uint32_t vk_to_gen_mipmap_mode[] = {
      [VK_TEX_MIPMAP_MODE_BASE]                 = MIPFILTER_NONE,
      [VK_TEX_MIPMAP_MODE_NEAREST]              = MIPFILTER_NEAREST,
      [VK_TEX_MIPMAP_MODE_LINEAR]               = MIPFILTER_LINEAR
   };

   static const uint32_t vk_to_gen_tex_address[] = {
      [VK_TEX_ADDRESS_MODE_WRAP]                = TCM_WRAP,
      [VK_TEX_ADDRESS_MODE_MIRROR]              = TCM_MIRROR,
      [VK_TEX_ADDRESS_MODE_CLAMP]               = TCM_CLAMP,
      [VK_TEX_ADDRESS_MODE_MIRROR_ONCE]         = TCM_MIRROR_ONCE,
      [VK_TEX_ADDRESS_MODE_CLAMP_BORDER]        = TCM_CLAMP_BORDER,
   };

   static const uint32_t vk_to_gen_compare_op[] = {
      [VK_COMPARE_OP_NEVER]                     = PREFILTEROPNEVER,
      [VK_COMPARE_OP_LESS]                      = PREFILTEROPLESS,
      [VK_COMPARE_OP_EQUAL]                     = PREFILTEROPEQUAL,
      [VK_COMPARE_OP_LESS_EQUAL]                = PREFILTEROPLEQUAL,
      [VK_COMPARE_OP_GREATER]                   = PREFILTEROPGREATER,
      [VK_COMPARE_OP_NOT_EQUAL]                 = PREFILTEROPNOTEQUAL,
      [VK_COMPARE_OP_GREATER_EQUAL]             = PREFILTEROPGEQUAL,
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

   struct GEN8_SAMPLER_STATE sampler_state = {
      .SamplerDisable = false,
      .TextureBorderColorMode = DX10OGL,
      .LODPreClampMode = 0,
      .BaseMipLevel = 0.0,
      .MipModeFilter = vk_to_gen_mipmap_mode[pCreateInfo->mipMode],
      .MagModeFilter = mag_filter,
      .MinModeFilter = min_filter,
      .TextureLODBias = pCreateInfo->mipLodBias * 256,
      .AnisotropicAlgorithm = EWAApproximation,
      .MinLOD = pCreateInfo->minLod,
      .MaxLOD = pCreateInfo->maxLod,
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

   GEN8_SAMPLER_STATE_pack(NULL, sampler->state, &sampler_state);

   *pSampler = anv_sampler_to_handle(sampler);

   return VK_SUCCESS;
}

VkResult gen8_CreateDynamicDepthStencilState(
    VkDevice                                    _device,
    const VkDynamicDepthStencilStateCreateInfo* pCreateInfo,
    VkDynamicDepthStencilState*                 pState)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_dynamic_ds_state *state;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DYNAMIC_DEPTH_STENCIL_STATE_CREATE_INFO);

   state = anv_device_alloc(device, sizeof(*state), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (state == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   struct GEN8_3DSTATE_WM_DEPTH_STENCIL wm_depth_stencil = {
      GEN8_3DSTATE_WM_DEPTH_STENCIL_header,

      /* Is this what we need to do? */
      .StencilBufferWriteEnable = pCreateInfo->stencilWriteMask != 0,

      .StencilTestMask = pCreateInfo->stencilReadMask & 0xff,
      .StencilWriteMask = pCreateInfo->stencilWriteMask & 0xff,

      .BackfaceStencilTestMask = pCreateInfo->stencilReadMask & 0xff,
      .BackfaceStencilWriteMask = pCreateInfo->stencilWriteMask & 0xff,
   };

   GEN8_3DSTATE_WM_DEPTH_STENCIL_pack(NULL, state->gen8.wm_depth_stencil,
                                      &wm_depth_stencil);

   struct GEN8_COLOR_CALC_STATE color_calc_state = {
      .StencilReferenceValue = pCreateInfo->stencilFrontRef,
      .BackFaceStencilReferenceValue = pCreateInfo->stencilBackRef
   };

   GEN8_COLOR_CALC_STATE_pack(NULL, state->gen8.color_calc_state, &color_calc_state);

   *pState = anv_dynamic_ds_state_to_handle(state);

   return VK_SUCCESS;
}
