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

   GEN8_3DSTATE_SF_pack(NULL, state->state_sf, &sf);

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

   GEN8_3DSTATE_RASTER_pack(NULL, state->state_raster, &raster);

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
      .VerticalLineStride = 0,
      .VerticalLineStrideOffset = 0,
      .SamplerL2BypassModeDisable = true,
      .RenderCacheReadWriteMode = WriteOnlyCache,
      .MemoryObjectControlState = GEN8_MOCS,
      .BaseMipLevel = 0.0,
      .SurfaceQPitch = 0,
      .Height = (num_elements >> 7) & 0x3fff,
      .Width = num_elements & 0x7f,
      .Depth = (num_elements >> 21) & 0x3f,
      .SurfacePitch = stride - 1,
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
   struct anv_buffer_view *view;
   VkResult result;

   result = anv_buffer_view_create(device, pCreateInfo, &view);
   if (result != VK_SUCCESS)
      return result;

   const struct anv_format *format = 
      anv_format_for_vk_format(pCreateInfo->format);

   gen8_fill_buffer_surface_state(view->view.surface_state.map, format,
                                  view->view.offset, pCreateInfo->range);

   *pView = anv_buffer_view_to_handle(view);

   return VK_SUCCESS;
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
      [VK_TEX_ADDRESS_WRAP]                     = TCM_WRAP,
      [VK_TEX_ADDRESS_MIRROR]                   = TCM_MIRROR,
      [VK_TEX_ADDRESS_CLAMP]                    = TCM_CLAMP,
      [VK_TEX_ADDRESS_MIRROR_ONCE]              = TCM_MIRROR_ONCE,
      [VK_TEX_ADDRESS_CLAMP_BORDER]             = TCM_CLAMP_BORDER,
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
      .NonnormalizedCoordinateEnable = 0,
      .TCXAddressControlMode = vk_to_gen_tex_address[pCreateInfo->addressU],
      .TCYAddressControlMode = vk_to_gen_tex_address[pCreateInfo->addressV],
      .TCZAddressControlMode = vk_to_gen_tex_address[pCreateInfo->addressW],
   };

   GEN8_SAMPLER_STATE_pack(NULL, sampler->state, &sampler_state);

   *pSampler = anv_sampler_to_handle(sampler);

   return VK_SUCCESS;
}

static void
emit_vertex_input(struct anv_pipeline *pipeline,
                  const VkPipelineVertexInputStateCreateInfo *info)
{
   const uint32_t num_dwords = 1 + info->attributeCount * 2;
   uint32_t *p;
   bool instancing_enable[32];

   pipeline->vb_used = 0;
   for (uint32_t i = 0; i < info->bindingCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &info->pVertexBindingDescriptions[i];

      pipeline->vb_used |= 1 << desc->binding;
      pipeline->binding_stride[desc->binding] = desc->strideInBytes;

      /* Step rate is programmed per vertex element (attribute), not
       * binding. Set up a map of which bindings step per instance, for
       * reference by vertex element setup. */
      switch (desc->stepRate) {
      default:
      case VK_VERTEX_INPUT_STEP_RATE_VERTEX:
         instancing_enable[desc->binding] = false;
         break;
      case VK_VERTEX_INPUT_STEP_RATE_INSTANCE:
         instancing_enable[desc->binding] = true;
         break;
      }
   }

   p = anv_batch_emitn(&pipeline->batch, num_dwords,
                       GEN8_3DSTATE_VERTEX_ELEMENTS);

   for (uint32_t i = 0; i < info->attributeCount; i++) {
      const VkVertexInputAttributeDescription *desc =
         &info->pVertexAttributeDescriptions[i];
      const struct anv_format *format = anv_format_for_vk_format(desc->format);

      struct GEN8_VERTEX_ELEMENT_STATE element = {
         .VertexBufferIndex = desc->binding,
         .Valid = true,
         .SourceElementFormat = format->surface_format,
         .EdgeFlagEnable = false,
         .SourceElementOffset = desc->offsetInBytes,
         .Component0Control = VFCOMP_STORE_SRC,
         .Component1Control = format->num_channels >= 2 ? VFCOMP_STORE_SRC : VFCOMP_STORE_0,
         .Component2Control = format->num_channels >= 3 ? VFCOMP_STORE_SRC : VFCOMP_STORE_0,
         .Component3Control = format->num_channels >= 4 ? VFCOMP_STORE_SRC : VFCOMP_STORE_1_FP
      };
      GEN8_VERTEX_ELEMENT_STATE_pack(NULL, &p[1 + i * 2], &element);

      anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VF_INSTANCING,
                     .InstancingEnable = instancing_enable[desc->binding],
                     .VertexElementIndex = i,
                     /* Vulkan so far doesn't have an instance divisor, so
                      * this is always 1 (ignored if not instancing). */
                     .InstanceDataStepRate = 1);
   }

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VF_SGVS,
                  .VertexIDEnable = pipeline->vs_prog_data.uses_vertexid,
                  .VertexIDComponentNumber = 2,
                  .VertexIDElementOffset = info->bindingCount,
                  .InstanceIDEnable = pipeline->vs_prog_data.uses_instanceid,
                  .InstanceIDComponentNumber = 3,
                  .InstanceIDElementOffset = info->bindingCount);
}

static void
emit_ia_state(struct anv_pipeline *pipeline,
              const VkPipelineInputAssemblyStateCreateInfo *info,
              const struct anv_graphics_pipeline_create_info *extra)
{
   static const uint32_t vk_to_gen_primitive_type[] = {
      [VK_PRIMITIVE_TOPOLOGY_POINT_LIST]        = _3DPRIM_POINTLIST,
      [VK_PRIMITIVE_TOPOLOGY_LINE_LIST]         = _3DPRIM_LINELIST,
      [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP]        = _3DPRIM_LINESTRIP,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST]     = _3DPRIM_TRILIST,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP]    = _3DPRIM_TRISTRIP,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN]      = _3DPRIM_TRIFAN,
      [VK_PRIMITIVE_TOPOLOGY_LINE_LIST_ADJ]     = _3DPRIM_LINELIST_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_ADJ]    = _3DPRIM_LINESTRIP_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_ADJ] = _3DPRIM_TRILIST_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_ADJ] = _3DPRIM_TRISTRIP_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_PATCH]             = _3DPRIM_PATCHLIST_1
   };
   uint32_t topology = vk_to_gen_primitive_type[info->topology];

   if (extra && extra->use_rectlist)
      topology = _3DPRIM_RECTLIST;

   struct GEN8_3DSTATE_VF vf = {
      GEN8_3DSTATE_VF_header,
      .IndexedDrawCutIndexEnable = info->primitiveRestartEnable,
   };
   GEN8_3DSTATE_VF_pack(NULL, pipeline->state_vf, &vf);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VF_TOPOLOGY,
                  .PrimitiveTopologyType = topology);
}

static void
emit_rs_state(struct anv_pipeline *pipeline,
              const VkPipelineRasterStateCreateInfo *info,
              const struct anv_graphics_pipeline_create_info *extra)
{
   static const uint32_t vk_to_gen_cullmode[] = {
      [VK_CULL_MODE_NONE]                       = CULLMODE_NONE,
      [VK_CULL_MODE_FRONT]                      = CULLMODE_FRONT,
      [VK_CULL_MODE_BACK]                       = CULLMODE_BACK,
      [VK_CULL_MODE_FRONT_AND_BACK]             = CULLMODE_BOTH
   };

   static const uint32_t vk_to_gen_fillmode[] = {
      [VK_FILL_MODE_POINTS]                     = RASTER_POINT,
      [VK_FILL_MODE_WIREFRAME]                  = RASTER_WIREFRAME,
      [VK_FILL_MODE_SOLID]                      = RASTER_SOLID
   };

   static const uint32_t vk_to_gen_front_face[] = {
      [VK_FRONT_FACE_CCW]                       = CounterClockwise,
      [VK_FRONT_FACE_CW]                        = Clockwise
   };

   struct GEN8_3DSTATE_SF sf = {
      GEN8_3DSTATE_SF_header,
      .ViewportTransformEnable = !(extra && extra->disable_viewport),
      .TriangleStripListProvokingVertexSelect = 0,
      .LineStripListProvokingVertexSelect = 0,
      .TriangleFanProvokingVertexSelect = 0,
      .PointWidthSource = pipeline->writes_point_size ? Vertex : State,
      .PointWidth = 1.0,
   };

   /* FINISHME: VkBool32 rasterizerDiscardEnable; */

   GEN8_3DSTATE_SF_pack(NULL, pipeline->state_sf, &sf);

   struct GEN8_3DSTATE_RASTER raster = {
      GEN8_3DSTATE_RASTER_header,
      .FrontWinding = vk_to_gen_front_face[info->frontFace],
      .CullMode = vk_to_gen_cullmode[info->cullMode],
      .FrontFaceFillMode = vk_to_gen_fillmode[info->fillMode],
      .BackFaceFillMode = vk_to_gen_fillmode[info->fillMode],
      .ScissorRectangleEnable = !(extra && extra->disable_scissor),
      .ViewportZClipTestEnable = info->depthClipEnable
   };

   GEN8_3DSTATE_RASTER_pack(NULL, pipeline->state_raster, &raster);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_SBE,
                  .ForceVertexURBEntryReadLength = false,
                  .ForceVertexURBEntryReadOffset = false,
                  .PointSpriteTextureCoordinateOrigin = UPPERLEFT,
                  .NumberofSFOutputAttributes =
                     pipeline->wm_prog_data.num_varying_inputs);

}

static void
emit_cb_state(struct anv_pipeline *pipeline,
              const VkPipelineColorBlendStateCreateInfo *info)
{
   struct anv_device *device = pipeline->device;

   static const uint32_t vk_to_gen_logic_op[] = {
      [VK_LOGIC_OP_COPY]                        = LOGICOP_COPY,
      [VK_LOGIC_OP_CLEAR]                       = LOGICOP_CLEAR,
      [VK_LOGIC_OP_AND]                         = LOGICOP_AND,
      [VK_LOGIC_OP_AND_REVERSE]                 = LOGICOP_AND_REVERSE,
      [VK_LOGIC_OP_AND_INVERTED]                = LOGICOP_AND_INVERTED,
      [VK_LOGIC_OP_NOOP]                        = LOGICOP_NOOP,
      [VK_LOGIC_OP_XOR]                         = LOGICOP_XOR,
      [VK_LOGIC_OP_OR]                          = LOGICOP_OR,
      [VK_LOGIC_OP_NOR]                         = LOGICOP_NOR,
      [VK_LOGIC_OP_EQUIV]                       = LOGICOP_EQUIV,
      [VK_LOGIC_OP_INVERT]                      = LOGICOP_INVERT,
      [VK_LOGIC_OP_OR_REVERSE]                  = LOGICOP_OR_REVERSE,
      [VK_LOGIC_OP_COPY_INVERTED]               = LOGICOP_COPY_INVERTED,
      [VK_LOGIC_OP_OR_INVERTED]                 = LOGICOP_OR_INVERTED,
      [VK_LOGIC_OP_NAND]                        = LOGICOP_NAND,
      [VK_LOGIC_OP_SET]                         = LOGICOP_SET,
   };

   static const uint32_t vk_to_gen_blend[] = {
      [VK_BLEND_ZERO]                           = BLENDFACTOR_ZERO,
      [VK_BLEND_ONE]                            = BLENDFACTOR_ONE,
      [VK_BLEND_SRC_COLOR]                      = BLENDFACTOR_SRC_COLOR,
      [VK_BLEND_ONE_MINUS_SRC_COLOR]            = BLENDFACTOR_INV_SRC_COLOR,
      [VK_BLEND_DEST_COLOR]                     = BLENDFACTOR_DST_COLOR,
      [VK_BLEND_ONE_MINUS_DEST_COLOR]           = BLENDFACTOR_INV_DST_COLOR,
      [VK_BLEND_SRC_ALPHA]                      = BLENDFACTOR_SRC_ALPHA,
      [VK_BLEND_ONE_MINUS_SRC_ALPHA]            = BLENDFACTOR_INV_SRC_ALPHA,
      [VK_BLEND_DEST_ALPHA]                     = BLENDFACTOR_DST_ALPHA,
      [VK_BLEND_ONE_MINUS_DEST_ALPHA]           = BLENDFACTOR_INV_DST_ALPHA,
      [VK_BLEND_CONSTANT_COLOR]                 = BLENDFACTOR_CONST_COLOR,
      [VK_BLEND_ONE_MINUS_CONSTANT_COLOR]       = BLENDFACTOR_INV_CONST_COLOR,
      [VK_BLEND_CONSTANT_ALPHA]                 = BLENDFACTOR_CONST_ALPHA,
      [VK_BLEND_ONE_MINUS_CONSTANT_ALPHA]       = BLENDFACTOR_INV_CONST_ALPHA,
      [VK_BLEND_SRC_ALPHA_SATURATE]             = BLENDFACTOR_SRC_ALPHA_SATURATE,
      [VK_BLEND_SRC1_COLOR]                     = BLENDFACTOR_SRC1_COLOR,
      [VK_BLEND_ONE_MINUS_SRC1_COLOR]           = BLENDFACTOR_INV_SRC1_COLOR,
      [VK_BLEND_SRC1_ALPHA]                     = BLENDFACTOR_SRC1_ALPHA,
      [VK_BLEND_ONE_MINUS_SRC1_ALPHA]           = BLENDFACTOR_INV_SRC1_ALPHA,
   };

   static const uint32_t vk_to_gen_blend_op[] = {
      [VK_BLEND_OP_ADD]                         = BLENDFUNCTION_ADD,
      [VK_BLEND_OP_SUBTRACT]                    = BLENDFUNCTION_SUBTRACT,
      [VK_BLEND_OP_REVERSE_SUBTRACT]            = BLENDFUNCTION_REVERSE_SUBTRACT,
      [VK_BLEND_OP_MIN]                         = BLENDFUNCTION_MIN,
      [VK_BLEND_OP_MAX]                         = BLENDFUNCTION_MAX,
   };

   uint32_t num_dwords = GEN8_BLEND_STATE_length;
   pipeline->blend_state =
      anv_state_pool_alloc(&device->dynamic_state_pool, num_dwords * 4, 64);

   struct GEN8_BLEND_STATE blend_state = {
      .AlphaToCoverageEnable = info->alphaToCoverageEnable,
   };

   for (uint32_t i = 0; i < info->attachmentCount; i++) {
      const VkPipelineColorBlendAttachmentState *a = &info->pAttachments[i];

      blend_state.Entry[i] = (struct GEN8_BLEND_STATE_ENTRY) {
         .LogicOpEnable = info->logicOpEnable,
         .LogicOpFunction = vk_to_gen_logic_op[info->logicOp],
         .ColorBufferBlendEnable = a->blendEnable,
         .PreBlendSourceOnlyClampEnable = false,
         .PreBlendColorClampEnable = false,
         .PostBlendColorClampEnable = false,
         .SourceBlendFactor = vk_to_gen_blend[a->srcBlendColor],
         .DestinationBlendFactor = vk_to_gen_blend[a->destBlendColor],
         .ColorBlendFunction = vk_to_gen_blend_op[a->blendOpColor],
         .SourceAlphaBlendFactor = vk_to_gen_blend[a->srcBlendAlpha],
         .DestinationAlphaBlendFactor = vk_to_gen_blend[a->destBlendAlpha],
         .AlphaBlendFunction = vk_to_gen_blend_op[a->blendOpAlpha],
         .WriteDisableAlpha = !(a->channelWriteMask & VK_CHANNEL_A_BIT),
         .WriteDisableRed = !(a->channelWriteMask & VK_CHANNEL_R_BIT),
         .WriteDisableGreen = !(a->channelWriteMask & VK_CHANNEL_G_BIT),
         .WriteDisableBlue = !(a->channelWriteMask & VK_CHANNEL_B_BIT),
      };
   }

   GEN8_BLEND_STATE_pack(NULL, pipeline->blend_state.map, &blend_state);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_BLEND_STATE_POINTERS,
                  .BlendStatePointer = pipeline->blend_state.offset,
                  .BlendStatePointerValid = true);
}

static const uint32_t vk_to_gen_compare_op[] = {
   [VK_COMPARE_OP_NEVER]                        = COMPAREFUNCTION_NEVER,
   [VK_COMPARE_OP_LESS]                         = COMPAREFUNCTION_LESS,
   [VK_COMPARE_OP_EQUAL]                        = COMPAREFUNCTION_EQUAL,
   [VK_COMPARE_OP_LESS_EQUAL]                   = COMPAREFUNCTION_LEQUAL,
   [VK_COMPARE_OP_GREATER]                      = COMPAREFUNCTION_GREATER,
   [VK_COMPARE_OP_NOT_EQUAL]                    = COMPAREFUNCTION_NOTEQUAL,
   [VK_COMPARE_OP_GREATER_EQUAL]                = COMPAREFUNCTION_GEQUAL,
   [VK_COMPARE_OP_ALWAYS]                       = COMPAREFUNCTION_ALWAYS,
};

static const uint32_t vk_to_gen_stencil_op[] = {
   [VK_STENCIL_OP_KEEP]                         = 0,
   [VK_STENCIL_OP_ZERO]                         = 0,
   [VK_STENCIL_OP_REPLACE]                      = 0,
   [VK_STENCIL_OP_INC_CLAMP]                    = 0,
   [VK_STENCIL_OP_DEC_CLAMP]                    = 0,
   [VK_STENCIL_OP_INVERT]                       = 0,
   [VK_STENCIL_OP_INC_WRAP]                     = 0,
   [VK_STENCIL_OP_DEC_WRAP]                     = 0
};

static void
emit_ds_state(struct anv_pipeline *pipeline,
              const VkPipelineDepthStencilStateCreateInfo *info)
{
   if (info == NULL) {
      /* We're going to OR this together with the dynamic state.  We need
       * to make sure it's initialized to something useful.
       */
      memset(pipeline->state_wm_depth_stencil, 0,
             sizeof(pipeline->state_wm_depth_stencil));
      return;
   }

   /* VkBool32 depthBoundsEnable;          // optional (depth_bounds_test) */

   struct GEN8_3DSTATE_WM_DEPTH_STENCIL wm_depth_stencil = {
      .DepthTestEnable = info->depthTestEnable,
      .DepthBufferWriteEnable = info->depthWriteEnable,
      .DepthTestFunction = vk_to_gen_compare_op[info->depthCompareOp],
      .DoubleSidedStencilEnable = true,

      .StencilTestEnable = info->stencilTestEnable,
      .StencilFailOp = vk_to_gen_stencil_op[info->front.stencilFailOp],
      .StencilPassDepthPassOp = vk_to_gen_stencil_op[info->front.stencilPassOp],
      .StencilPassDepthFailOp = vk_to_gen_stencil_op[info->front.stencilDepthFailOp],
      .StencilTestFunction = vk_to_gen_compare_op[info->front.stencilCompareOp],
      .BackfaceStencilFailOp = vk_to_gen_stencil_op[info->back.stencilFailOp],
      .BackfaceStencilPassDepthPassOp = vk_to_gen_stencil_op[info->back.stencilPassOp],
      .BackfaceStencilPassDepthFailOp =vk_to_gen_stencil_op[info->back.stencilDepthFailOp],
      .BackfaceStencilTestFunction = vk_to_gen_compare_op[info->back.stencilCompareOp],
   };

   GEN8_3DSTATE_WM_DEPTH_STENCIL_pack(NULL, pipeline->state_wm_depth_stencil, &wm_depth_stencil);
}

VkResult
gen8_graphics_pipeline_create(
    VkDevice                                    _device,
    const VkGraphicsPipelineCreateInfo*         pCreateInfo,
    const struct anv_graphics_pipeline_create_info *extra,
    VkPipeline*                                 pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_pipeline *pipeline;
   VkResult result;
   uint32_t offset, length;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);

   pipeline = anv_device_alloc(device, sizeof(*pipeline), 8,
                               VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pipeline == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->device = device;
   pipeline->layout = anv_pipeline_layout_from_handle(pCreateInfo->layout);
   memset(pipeline->shaders, 0, sizeof(pipeline->shaders));

   result = anv_reloc_list_init(&pipeline->batch_relocs, device);
   if (result != VK_SUCCESS) {
      anv_device_free(device, pipeline);
      return result;
   }
   pipeline->batch.next = pipeline->batch.start = pipeline->batch_data;
   pipeline->batch.end = pipeline->batch.start + sizeof(pipeline->batch_data);
   pipeline->batch.relocs = &pipeline->batch_relocs;

   anv_state_stream_init(&pipeline->program_stream,
                         &device->instruction_block_pool);

   for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
      pipeline->shaders[pCreateInfo->pStages[i].stage] =
         anv_shader_from_handle(pCreateInfo->pStages[i].shader);
   }

   if (pCreateInfo->pTessellationState)
      anv_finishme("VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO");
   if (pCreateInfo->pViewportState)
      anv_finishme("VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO");
   if (pCreateInfo->pMultisampleState)
      anv_finishme("VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO");

   pipeline->use_repclear = extra && extra->use_repclear;

   anv_compiler_run(device->compiler, pipeline);

   /* FIXME: The compiler dead-codes FS inputs when we don't have a VS, so we
    * hard code this to num_attributes - 2. This is because the attributes
    * include VUE header and position, which aren't counted as varying
    * inputs. */
   if (pipeline->vs_simd8 == NO_KERNEL) {
      pipeline->wm_prog_data.num_varying_inputs =
         pCreateInfo->pVertexInputState->attributeCount - 2;
   }

   assert(pCreateInfo->pVertexInputState);
   emit_vertex_input(pipeline, pCreateInfo->pVertexInputState);
   assert(pCreateInfo->pInputAssemblyState);
   emit_ia_state(pipeline, pCreateInfo->pInputAssemblyState, extra);
   assert(pCreateInfo->pRasterState);
   emit_rs_state(pipeline, pCreateInfo->pRasterState, extra);
   emit_ds_state(pipeline, pCreateInfo->pDepthStencilState);
   emit_cb_state(pipeline, pCreateInfo->pColorBlendState);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VF_STATISTICS,
                   .StatisticsEnable = true);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_HS, .Enable = false);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_TE, .TEEnable = false);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_DS, .FunctionEnable = false);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_STREAMOUT, .SOFunctionEnable = false);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_VS,
                  .ConstantBufferOffset = 0,
                  .ConstantBufferSize = 4);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_GS,
                  .ConstantBufferOffset = 4,
                  .ConstantBufferSize = 4);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_PS,
                  .ConstantBufferOffset = 8,
                  .ConstantBufferSize = 4);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_WM_CHROMAKEY,
                  .ChromaKeyKillEnable = false);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_SBE_SWIZ);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_AA_LINE_PARAMETERS);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_CLIP,
                  .ClipEnable = true,
                  .ViewportXYClipTestEnable = !(extra && extra->disable_viewport),
                  .MinimumPointWidth = 0.125,
                  .MaximumPointWidth = 255.875);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_WM,
                  .StatisticsEnable = true,
                  .LineEndCapAntialiasingRegionWidth = _05pixels,
                  .LineAntialiasingRegionWidth = _10pixels,
                  .EarlyDepthStencilControl = NORMAL,
                  .ForceThreadDispatchEnable = NORMAL,
                  .PointRasterizationRule = RASTRULE_UPPER_RIGHT,
                  .BarycentricInterpolationMode =
                     pipeline->wm_prog_data.barycentric_interp_modes);

   uint32_t samples = 1;
   uint32_t log2_samples = __builtin_ffs(samples) - 1;
   bool enable_sampling = samples > 1 ? true : false;

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_MULTISAMPLE,
                  .PixelPositionOffsetEnable = enable_sampling,
                  .PixelLocation = CENTER,
                  .NumberofMultisamples = log2_samples);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_SAMPLE_MASK,
                  .SampleMask = 0xffff);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_URB_VS,
                  .VSURBStartingAddress = pipeline->urb.vs_start,
                  .VSURBEntryAllocationSize = pipeline->urb.vs_size - 1,
                  .VSNumberofURBEntries = pipeline->urb.nr_vs_entries);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_URB_GS,
                  .GSURBStartingAddress = pipeline->urb.gs_start,
                  .GSURBEntryAllocationSize = pipeline->urb.gs_size - 1,
                  .GSNumberofURBEntries = pipeline->urb.nr_gs_entries);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_URB_HS,
                  .HSURBStartingAddress = pipeline->urb.vs_start,
                  .HSURBEntryAllocationSize = 0,
                  .HSNumberofURBEntries = 0);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_URB_DS,
                  .DSURBStartingAddress = pipeline->urb.vs_start,
                  .DSURBEntryAllocationSize = 0,
                  .DSNumberofURBEntries = 0);

   const struct brw_gs_prog_data *gs_prog_data = &pipeline->gs_prog_data;
   offset = 1;
   length = (gs_prog_data->base.vue_map.num_slots + 1) / 2 - offset;

   if (pipeline->gs_vec4 == NO_KERNEL)
      anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_GS, .Enable = false);
   else
      anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_GS,
                     .SingleProgramFlow = false,
                     .KernelStartPointer = pipeline->gs_vec4,
                     .VectorMaskEnable = Vmask,
                     .SamplerCount = 0,
                     .BindingTableEntryCount = 0,
                     .ExpectedVertexCount = pipeline->gs_vertex_count,

                     .ScratchSpaceBasePointer = pipeline->scratch_start[VK_SHADER_STAGE_GEOMETRY],
                     .PerThreadScratchSpace = ffs(gs_prog_data->base.base.total_scratch / 2048),

                     .OutputVertexSize = gs_prog_data->output_vertex_size_hwords * 2 - 1,
                     .OutputTopology = gs_prog_data->output_topology,
                     .VertexURBEntryReadLength = gs_prog_data->base.urb_read_length,
                     .DispatchGRFStartRegisterForURBData =
                        gs_prog_data->base.base.dispatch_grf_start_reg,

                     .MaximumNumberofThreads = device->info.max_gs_threads,
                     .ControlDataHeaderSize = gs_prog_data->control_data_header_size_hwords,
                     //pipeline->gs_prog_data.dispatch_mode |
                     .StatisticsEnable = true,
                     .IncludePrimitiveID = gs_prog_data->include_primitive_id,
                     .ReorderMode = TRAILING,
                     .Enable = true,

                     .ControlDataFormat = gs_prog_data->control_data_format,

                     /* FIXME: mesa sets this based on ctx->Transform.ClipPlanesEnabled:
                      * UserClipDistanceClipTestEnableBitmask_3DSTATE_GS(v)
                      * UserClipDistanceCullTestEnableBitmask(v)
                      */

                     .VertexURBEntryOutputReadOffset = offset,
                     .VertexURBEntryOutputLength = length);

   const struct brw_vue_prog_data *vue_prog_data = &pipeline->vs_prog_data.base;
   /* Skip the VUE header and position slots */
   offset = 1;
   length = (vue_prog_data->vue_map.num_slots + 1) / 2 - offset;

   if (pipeline->vs_simd8 == NO_KERNEL || (extra && extra->disable_vs))
      anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VS,
                     .FunctionEnable = false,
                     .VertexURBEntryOutputReadOffset = 1,
                     /* Even if VS is disabled, SBE still gets the amount of
                      * vertex data to read from this field. We use attribute
                      * count - 1, as we don't count the VUE header here. */
                     .VertexURBEntryOutputLength =
                        DIV_ROUND_UP(pCreateInfo->pVertexInputState->attributeCount - 1, 2));
   else
      anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VS,
                     .KernelStartPointer = pipeline->vs_simd8,
                     .SingleVertexDispatch = Multiple,
                     .VectorMaskEnable = Dmask,
                     .SamplerCount = 0,
                     .BindingTableEntryCount =
                     vue_prog_data->base.binding_table.size_bytes / 4,
                     .ThreadDispatchPriority = Normal,
                     .FloatingPointMode = IEEE754,
                     .IllegalOpcodeExceptionEnable = false,
                     .AccessesUAV = false,
                     .SoftwareExceptionEnable = false,

                     .ScratchSpaceBasePointer = pipeline->scratch_start[VK_SHADER_STAGE_VERTEX],
                     .PerThreadScratchSpace = ffs(vue_prog_data->base.total_scratch / 2048),

                     .DispatchGRFStartRegisterForURBData =
                     vue_prog_data->base.dispatch_grf_start_reg,
                     .VertexURBEntryReadLength = vue_prog_data->urb_read_length,
                     .VertexURBEntryReadOffset = 0,

                     .MaximumNumberofThreads = device->info.max_vs_threads - 1,
                     .StatisticsEnable = false,
                     .SIMD8DispatchEnable = true,
                     .VertexCacheDisable = false,
                     .FunctionEnable = true,

                     .VertexURBEntryOutputReadOffset = offset,
                     .VertexURBEntryOutputLength = length,
                     .UserClipDistanceClipTestEnableBitmask = 0,
                     .UserClipDistanceCullTestEnableBitmask = 0);

   const struct brw_wm_prog_data *wm_prog_data = &pipeline->wm_prog_data;
   uint32_t ksp0, ksp2, grf_start0, grf_start2;

   ksp2 = 0;
   grf_start2 = 0;
   if (pipeline->ps_simd8 != NO_KERNEL) {
      ksp0 = pipeline->ps_simd8;
      grf_start0 = wm_prog_data->base.dispatch_grf_start_reg;
      if (pipeline->ps_simd16 != NO_KERNEL) {
         ksp2 = pipeline->ps_simd16;
         grf_start2 = wm_prog_data->dispatch_grf_start_reg_16;
      }
   } else if (pipeline->ps_simd16 != NO_KERNEL) {
      ksp0 = pipeline->ps_simd16;
      grf_start0 = wm_prog_data->dispatch_grf_start_reg_16;
   } else {
      unreachable("no ps shader");
   }

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_PS,
                  .KernelStartPointer0 = ksp0,

                  .SingleProgramFlow = false,
                  .VectorMaskEnable = true,
                  .SamplerCount = 1,

                  .ScratchSpaceBasePointer = pipeline->scratch_start[VK_SHADER_STAGE_FRAGMENT],
                  .PerThreadScratchSpace = ffs(wm_prog_data->base.total_scratch / 2048),

                  .MaximumNumberofThreadsPerPSD = 64 - 2,
                  .PositionXYOffsetSelect = wm_prog_data->uses_pos_offset ?
                     POSOFFSET_SAMPLE: POSOFFSET_NONE,
                  .PushConstantEnable = wm_prog_data->base.nr_params > 0,
                  ._8PixelDispatchEnable = pipeline->ps_simd8 != NO_KERNEL,
                  ._16PixelDispatchEnable = pipeline->ps_simd16 != NO_KERNEL,
                  ._32PixelDispatchEnable = false,

                  .DispatchGRFStartRegisterForConstantSetupData0 = grf_start0,
                  .DispatchGRFStartRegisterForConstantSetupData1 = 0,
                  .DispatchGRFStartRegisterForConstantSetupData2 = grf_start2,

                  .KernelStartPointer1 = 0,
                  .KernelStartPointer2 = ksp2);

   bool per_sample_ps = false;
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_PS_EXTRA,
                  .PixelShaderValid = true,
                  .PixelShaderKillsPixel = wm_prog_data->uses_kill,
                  .PixelShaderComputedDepthMode = wm_prog_data->computed_depth_mode,
                  .AttributeEnable = wm_prog_data->num_varying_inputs > 0,
                  .oMaskPresenttoRenderTarget = wm_prog_data->uses_omask,
                  .PixelShaderIsPerSample = per_sample_ps);

   *pPipeline = anv_pipeline_to_handle(pipeline);

   return VK_SUCCESS;
}

VkResult gen8_compute_pipeline_create(
    VkDevice                                    _device,
    const VkComputePipelineCreateInfo*          pCreateInfo,
    VkPipeline*                                 pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_pipeline *pipeline;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);

   pipeline = anv_device_alloc(device, sizeof(*pipeline), 8,
                               VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pipeline == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->device = device;
   pipeline->layout = anv_pipeline_layout_from_handle(pCreateInfo->layout);

   result = anv_reloc_list_init(&pipeline->batch_relocs, device);
   if (result != VK_SUCCESS) {
      anv_device_free(device, pipeline);
      return result;
   }
   pipeline->batch.next = pipeline->batch.start = pipeline->batch_data;
   pipeline->batch.end = pipeline->batch.start + sizeof(pipeline->batch_data);
   pipeline->batch.relocs = &pipeline->batch_relocs;

   anv_state_stream_init(&pipeline->program_stream,
                         &device->instruction_block_pool);

   memset(pipeline->shaders, 0, sizeof(pipeline->shaders));

   pipeline->shaders[VK_SHADER_STAGE_COMPUTE] =
      anv_shader_from_handle(pCreateInfo->cs.shader);

   pipeline->use_repclear = false;

   anv_compiler_run(device->compiler, pipeline);

   const struct brw_cs_prog_data *cs_prog_data = &pipeline->cs_prog_data;

   anv_batch_emit(&pipeline->batch, GEN8_MEDIA_VFE_STATE,
                  .ScratchSpaceBasePointer = pipeline->scratch_start[VK_SHADER_STAGE_FRAGMENT],
                  .PerThreadScratchSpace = ffs(cs_prog_data->base.total_scratch / 2048),
                  .ScratchSpaceBasePointerHigh = 0,
                  .StackSize = 0,

                  .MaximumNumberofThreads = device->info.max_cs_threads - 1,
                  .NumberofURBEntries = 2,
                  .ResetGatewayTimer = true,
                  .BypassGatewayControl = true,
                  .URBEntryAllocationSize = 2,
                  .CURBEAllocationSize = 0);

   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;
   uint32_t group_size = prog_data->local_size[0] *
      prog_data->local_size[1] * prog_data->local_size[2];
   pipeline->cs_thread_width_max = DIV_ROUND_UP(group_size, prog_data->simd_size);
   uint32_t remainder = group_size & (prog_data->simd_size - 1);

   if (remainder > 0)
      pipeline->cs_right_mask = ~0u >> (32 - remainder);
   else
      pipeline->cs_right_mask = ~0u >> (32 - prog_data->simd_size);


   *pPipeline = anv_pipeline_to_handle(pipeline);

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

   GEN8_3DSTATE_WM_DEPTH_STENCIL_pack(NULL, state->state_wm_depth_stencil,
                                      &wm_depth_stencil);

   struct GEN8_COLOR_CALC_STATE color_calc_state = {
      .StencilReferenceValue = pCreateInfo->stencilFrontRef,
      .BackFaceStencilReferenceValue = pCreateInfo->stencilBackRef
   };

   GEN8_COLOR_CALC_STATE_pack(NULL, state->state_color_calc, &color_calc_state);

   *pState = anv_dynamic_ds_state_to_handle(state);

   return VK_SUCCESS;
}
