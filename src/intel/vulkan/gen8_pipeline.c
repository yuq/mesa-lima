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

#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"

#include "genX_pipeline_util.h"

static void
emit_ia_state(struct anv_pipeline *pipeline,
              const VkPipelineInputAssemblyStateCreateInfo *info,
              const struct anv_graphics_pipeline_create_info *extra)
{
   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_VF_TOPOLOGY),
                  .PrimitiveTopologyType = pipeline->topology);
}

static void
emit_rs_state(struct anv_pipeline *pipeline,
              const VkPipelineRasterizationStateCreateInfo *info,
              const VkPipelineMultisampleStateCreateInfo *ms_info,
              const struct anv_graphics_pipeline_create_info *extra)
{
   uint32_t samples = 1;

   if (ms_info)
      samples = ms_info->rasterizationSamples;

   struct GENX(3DSTATE_SF) sf = {
      GENX(3DSTATE_SF_header),
      .ViewportTransformEnable = !(extra && extra->disable_viewport),
      .TriangleStripListProvokingVertexSelect = 0,
      .LineStripListProvokingVertexSelect = 0,
      .TriangleFanProvokingVertexSelect = 1,
      .PointWidthSource = pipeline->writes_point_size ? Vertex : State,
      .PointWidth = 1.0,
   };

   /* FINISHME: VkBool32 rasterizerDiscardEnable; */

   GENX(3DSTATE_SF_pack)(NULL, pipeline->gen8.sf, &sf);

   struct GENX(3DSTATE_RASTER) raster = {
      GENX(3DSTATE_RASTER_header),

      /* For details on 3DSTATE_RASTER multisample state, see the BSpec table
       * "Multisample Modes State".
       */
      .DXMultisampleRasterizationEnable = samples > 1,
      .ForcedSampleCount = FSC_NUMRASTSAMPLES_0,
      .ForceMultisampling = false,

      .FrontWinding = vk_to_gen_front_face[info->frontFace],
      .CullMode = vk_to_gen_cullmode[info->cullMode],
      .FrontFaceFillMode = vk_to_gen_fillmode[info->polygonMode],
      .BackFaceFillMode = vk_to_gen_fillmode[info->polygonMode],
      .ScissorRectangleEnable = !(extra && extra->disable_scissor),
#if GEN_GEN == 8
      .ViewportZClipTestEnable = true,
#else
      /* GEN9+ splits ViewportZClipTestEnable into near and far enable bits */
      .ViewportZFarClipTestEnable = true,
      .ViewportZNearClipTestEnable = true,
#endif
      .GlobalDepthOffsetEnableSolid = info->depthBiasEnable,
      .GlobalDepthOffsetEnableWireframe = info->depthBiasEnable,
      .GlobalDepthOffsetEnablePoint = info->depthBiasEnable,
   };

   GENX(3DSTATE_RASTER_pack)(NULL, pipeline->gen8.raster, &raster);
}

static void
emit_cb_state(struct anv_pipeline *pipeline,
              const VkPipelineColorBlendStateCreateInfo *info,
              const VkPipelineMultisampleStateCreateInfo *ms_info)
{
   struct anv_device *device = pipeline->device;

   uint32_t num_dwords = GENX(BLEND_STATE_length);
   pipeline->blend_state =
      anv_state_pool_alloc(&device->dynamic_state_pool, num_dwords * 4, 64);

   struct GENX(BLEND_STATE) blend_state = {
      .AlphaToCoverageEnable = ms_info && ms_info->alphaToCoverageEnable,
      .AlphaToOneEnable = ms_info && ms_info->alphaToOneEnable,
   };

   bool has_writeable_rt = false;
   for (uint32_t i = 0; i < info->attachmentCount; i++) {
      const VkPipelineColorBlendAttachmentState *a = &info->pAttachments[i];

      if (a->srcColorBlendFactor != a->srcAlphaBlendFactor ||
          a->dstColorBlendFactor != a->dstAlphaBlendFactor ||
          a->colorBlendOp != a->alphaBlendOp) {
         blend_state.IndependentAlphaBlendEnable = true;
      }

      blend_state.Entry[i] = (struct GENX(BLEND_STATE_ENTRY)) {
         .LogicOpEnable = info->logicOpEnable,
         .LogicOpFunction = vk_to_gen_logic_op[info->logicOp],
         .ColorBufferBlendEnable = a->blendEnable,
         .PreBlendSourceOnlyClampEnable = false,
         .ColorClampRange = COLORCLAMP_RTFORMAT,
         .PreBlendColorClampEnable = true,
         .PostBlendColorClampEnable = true,
         .SourceBlendFactor = vk_to_gen_blend[a->srcColorBlendFactor],
         .DestinationBlendFactor = vk_to_gen_blend[a->dstColorBlendFactor],
         .ColorBlendFunction = vk_to_gen_blend_op[a->colorBlendOp],
         .SourceAlphaBlendFactor = vk_to_gen_blend[a->srcAlphaBlendFactor],
         .DestinationAlphaBlendFactor = vk_to_gen_blend[a->dstAlphaBlendFactor],
         .AlphaBlendFunction = vk_to_gen_blend_op[a->alphaBlendOp],
         .WriteDisableAlpha = !(a->colorWriteMask & VK_COLOR_COMPONENT_A_BIT),
         .WriteDisableRed = !(a->colorWriteMask & VK_COLOR_COMPONENT_R_BIT),
         .WriteDisableGreen = !(a->colorWriteMask & VK_COLOR_COMPONENT_G_BIT),
         .WriteDisableBlue = !(a->colorWriteMask & VK_COLOR_COMPONENT_B_BIT),
      };

      if (a->colorWriteMask != 0)
         has_writeable_rt = true;

      /* Our hardware applies the blend factor prior to the blend function
       * regardless of what function is used.  Technically, this means the
       * hardware can do MORE than GL or Vulkan specify.  However, it also
       * means that, for MIN and MAX, we have to stomp the blend factor to
       * ONE to make it a no-op.
       */
      if (a->colorBlendOp == VK_BLEND_OP_MIN ||
          a->colorBlendOp == VK_BLEND_OP_MAX) {
         blend_state.Entry[i].SourceBlendFactor = BLENDFACTOR_ONE;
         blend_state.Entry[i].DestinationBlendFactor = BLENDFACTOR_ONE;
      }
      if (a->alphaBlendOp == VK_BLEND_OP_MIN ||
          a->alphaBlendOp == VK_BLEND_OP_MAX) {
         blend_state.Entry[i].SourceAlphaBlendFactor = BLENDFACTOR_ONE;
         blend_state.Entry[i].DestinationAlphaBlendFactor = BLENDFACTOR_ONE;
      }
   }

   for (uint32_t i = info->attachmentCount; i < 8; i++) {
      blend_state.Entry[i].WriteDisableAlpha = true;
      blend_state.Entry[i].WriteDisableRed = true;
      blend_state.Entry[i].WriteDisableGreen = true;
      blend_state.Entry[i].WriteDisableBlue = true;
   }

   if (info->attachmentCount > 0) {
      struct GENX(BLEND_STATE_ENTRY) *bs = &blend_state.Entry[0];

      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS_BLEND),
                     .AlphaToCoverageEnable = blend_state.AlphaToCoverageEnable,
                     .HasWriteableRT = has_writeable_rt,
                     .ColorBufferBlendEnable = bs->ColorBufferBlendEnable,
                     .SourceAlphaBlendFactor = bs->SourceAlphaBlendFactor,
                     .DestinationAlphaBlendFactor =
                        bs->DestinationAlphaBlendFactor,
                     .SourceBlendFactor = bs->SourceBlendFactor,
                     .DestinationBlendFactor = bs->DestinationBlendFactor,
                     .AlphaTestEnable = false,
                     .IndependentAlphaBlendEnable =
                        blend_state.IndependentAlphaBlendEnable);
   } else {
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS_BLEND));
   }

   GENX(BLEND_STATE_pack)(NULL, pipeline->blend_state.map, &blend_state);
   if (!device->info.has_llc)
      anv_state_clflush(pipeline->blend_state);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_BLEND_STATE_POINTERS),
                  .BlendStatePointer = pipeline->blend_state.offset,
                  .BlendStatePointerValid = true);
}

static void
emit_ds_state(struct anv_pipeline *pipeline,
              const VkPipelineDepthStencilStateCreateInfo *info)
{
   uint32_t *dw = GEN_GEN == 8 ?
      pipeline->gen8.wm_depth_stencil : pipeline->gen9.wm_depth_stencil;

   if (info == NULL) {
      /* We're going to OR this together with the dynamic state.  We need
       * to make sure it's initialized to something useful.
       */
      memset(pipeline->gen8.wm_depth_stencil, 0,
             sizeof(pipeline->gen8.wm_depth_stencil));
      memset(pipeline->gen9.wm_depth_stencil, 0,
             sizeof(pipeline->gen9.wm_depth_stencil));
      return;
   }

   /* VkBool32 depthBoundsTestEnable; // optional (depth_bounds_test) */

   struct GENX(3DSTATE_WM_DEPTH_STENCIL) wm_depth_stencil = {
      .DepthTestEnable = info->depthTestEnable,
      .DepthBufferWriteEnable = info->depthWriteEnable,
      .DepthTestFunction = vk_to_gen_compare_op[info->depthCompareOp],
      .DoubleSidedStencilEnable = true,

      .StencilTestEnable = info->stencilTestEnable,
      .StencilBufferWriteEnable = info->stencilTestEnable,
      .StencilFailOp = vk_to_gen_stencil_op[info->front.failOp],
      .StencilPassDepthPassOp = vk_to_gen_stencil_op[info->front.passOp],
      .StencilPassDepthFailOp = vk_to_gen_stencil_op[info->front.depthFailOp],
      .StencilTestFunction = vk_to_gen_compare_op[info->front.compareOp],
      .BackfaceStencilFailOp = vk_to_gen_stencil_op[info->back.failOp],
      .BackfaceStencilPassDepthPassOp = vk_to_gen_stencil_op[info->back.passOp],
      .BackfaceStencilPassDepthFailOp =vk_to_gen_stencil_op[info->back.depthFailOp],
      .BackfaceStencilTestFunction = vk_to_gen_compare_op[info->back.compareOp],
   };

   /* From the Broadwell PRM:
    *
    *    "If Depth_Test_Enable = 1 AND Depth_Test_func = EQUAL, the
    *    Depth_Write_Enable must be set to 0."
    */
   if (info->depthTestEnable && info->depthCompareOp == VK_COMPARE_OP_EQUAL)
      wm_depth_stencil.DepthBufferWriteEnable = false;

   GENX(3DSTATE_WM_DEPTH_STENCIL_pack)(NULL, dw, &wm_depth_stencil);
}

static void
emit_ms_state(struct anv_pipeline *pipeline,
              const VkPipelineMultisampleStateCreateInfo *info)
{
   uint32_t samples = 1;
   uint32_t log2_samples = 0;

   /* From the Vulkan 1.0 spec:
    *    If pSampleMask is NULL, it is treated as if the mask has all bits
    *    enabled, i.e. no coverage is removed from fragments.
    *
    * 3DSTATE_SAMPLE_MASK.SampleMask is 16 bits.
    */
   uint32_t sample_mask = 0xffff;

   if (info) {
      samples = info->rasterizationSamples;
      log2_samples = __builtin_ffs(samples) - 1;
   }

   if (info && info->pSampleMask)
      sample_mask &= info->pSampleMask[0];

   if (info && info->sampleShadingEnable)
      anv_finishme("VkPipelineMultisampleStateCreateInfo::sampleShadingEnable");

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_MULTISAMPLE),

      /* The PRM says that this bit is valid only for DX9:
       *
       *    SW can choose to set this bit only for DX9 API. DX10/OGL API's
       *    should not have any effect by setting or not setting this bit.
       */
      .PixelPositionOffsetEnable = false,

      .PixelLocation = CENTER,
      .NumberofMultisamples = log2_samples);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_SAMPLE_MASK),
      .SampleMask = sample_mask);
}

VkResult
genX(graphics_pipeline_create)(
    VkDevice                                    _device,
    struct anv_pipeline_cache *                 cache,
    const VkGraphicsPipelineCreateInfo*         pCreateInfo,
    const struct anv_graphics_pipeline_create_info *extra,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_pipeline *pipeline;
   VkResult result;
   uint32_t offset, length;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);

   pipeline = anv_alloc2(&device->alloc, pAllocator, sizeof(*pipeline), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   result = anv_pipeline_init(pipeline, device, cache,
                              pCreateInfo, extra, pAllocator);
   if (result != VK_SUCCESS) {
      anv_free2(&device->alloc, pAllocator, pipeline);
      return result;
   }

   assert(pCreateInfo->pVertexInputState);
   emit_vertex_input(pipeline, pCreateInfo->pVertexInputState, extra);
   assert(pCreateInfo->pInputAssemblyState);
   emit_ia_state(pipeline, pCreateInfo->pInputAssemblyState, extra);
   assert(pCreateInfo->pRasterizationState);
   emit_rs_state(pipeline, pCreateInfo->pRasterizationState,
                 pCreateInfo->pMultisampleState, extra);
   emit_ms_state(pipeline, pCreateInfo->pMultisampleState);
   emit_ds_state(pipeline, pCreateInfo->pDepthStencilState);
   emit_cb_state(pipeline, pCreateInfo->pColorBlendState,
                           pCreateInfo->pMultisampleState);

   emit_urb_setup(pipeline);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_CLIP),
                  .ClipEnable = true,
                  .EarlyCullEnable = true,
                  .APIMode = 1, /* D3D */
                  .ViewportXYClipTestEnable = !(extra && extra->disable_viewport),

                  .ClipMode =
                     pCreateInfo->pRasterizationState->rasterizerDiscardEnable ?
                     REJECT_ALL : NORMAL,

                  .NonPerspectiveBarycentricEnable =
                     (pipeline->wm_prog_data.barycentric_interp_modes & 0x38) != 0,

                  .TriangleStripListProvokingVertexSelect = 0,
                  .LineStripListProvokingVertexSelect = 0,
                  .TriangleFanProvokingVertexSelect = 1,

                  .MinimumPointWidth = 0.125,
                  .MaximumPointWidth = 255.875,
                  .MaximumVPIndex = pCreateInfo->pViewportState->viewportCount - 1);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_WM),
                  .StatisticsEnable = true,
                  .LineEndCapAntialiasingRegionWidth = _05pixels,
                  .LineAntialiasingRegionWidth = _10pixels,
                  .EarlyDepthStencilControl = NORMAL,
                  .ForceThreadDispatchEnable = NORMAL,
                  .PointRasterizationRule = RASTRULE_UPPER_RIGHT,
                  .BarycentricInterpolationMode =
                     pipeline->ps_ksp0 == NO_KERNEL ?
                     0 : pipeline->wm_prog_data.barycentric_interp_modes);

   const struct brw_gs_prog_data *gs_prog_data = &pipeline->gs_prog_data;
   offset = 1;
   length = (gs_prog_data->base.vue_map.num_slots + 1) / 2 - offset;

   if (pipeline->gs_kernel == NO_KERNEL)
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_GS), .Enable = false);
   else
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_GS),
                     .SingleProgramFlow = false,
                     .KernelStartPointer = pipeline->gs_kernel,
                     .VectorMaskEnable = false,
                     .SamplerCount = 0,
                     .BindingTableEntryCount = 0,
                     .ExpectedVertexCount = gs_prog_data->vertices_in,

                     .ScratchSpaceBasePointer = pipeline->scratch_start[MESA_SHADER_GEOMETRY],
                     .PerThreadScratchSpace = scratch_space(&gs_prog_data->base.base),

                     .OutputVertexSize = gs_prog_data->output_vertex_size_hwords * 2 - 1,
                     .OutputTopology = gs_prog_data->output_topology,
                     .VertexURBEntryReadLength = gs_prog_data->base.urb_read_length,
                     .IncludeVertexHandles = gs_prog_data->base.include_vue_handles,
                     .DispatchGRFStartRegisterForURBData =
                        gs_prog_data->base.base.dispatch_grf_start_reg,

                     .MaximumNumberofThreads = device->info.max_gs_threads / 2 - 1,
                     .ControlDataHeaderSize = gs_prog_data->control_data_header_size_hwords,
                     .DispatchMode = gs_prog_data->base.dispatch_mode,
                     .StatisticsEnable = true,
                     .IncludePrimitiveID = gs_prog_data->include_primitive_id,
                     .ReorderMode = TRAILING,
                     .Enable = true,

                     .ControlDataFormat = gs_prog_data->control_data_format,

                     .StaticOutput = gs_prog_data->static_vertex_count >= 0,
                     .StaticOutputVertexCount =
                        gs_prog_data->static_vertex_count >= 0 ?
                        gs_prog_data->static_vertex_count : 0,

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

   uint32_t vs_start = pipeline->vs_simd8 != NO_KERNEL ? pipeline->vs_simd8 :
                                                         pipeline->vs_vec4;

   if (vs_start == NO_KERNEL || (extra && extra->disable_vs))
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_VS),
                     .FunctionEnable = false,
                     /* Even if VS is disabled, SBE still gets the amount of
                      * vertex data to read from this field. */
                     .VertexURBEntryOutputReadOffset = offset,
                     .VertexURBEntryOutputLength = length);
   else
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_VS),
                     .KernelStartPointer = vs_start,
                     .SingleVertexDispatch = false,
                     .VectorMaskEnable = false,
                     .SamplerCount = 0,
                     .BindingTableEntryCount =
                     vue_prog_data->base.binding_table.size_bytes / 4,
                     .ThreadDispatchPriority = false,
                     .FloatingPointMode = IEEE754,
                     .IllegalOpcodeExceptionEnable = false,
                     .AccessesUAV = false,
                     .SoftwareExceptionEnable = false,

                     .ScratchSpaceBasePointer = pipeline->scratch_start[MESA_SHADER_VERTEX],
                     .PerThreadScratchSpace = scratch_space(&vue_prog_data->base),

                     .DispatchGRFStartRegisterForURBData =
                     vue_prog_data->base.dispatch_grf_start_reg,
                     .VertexURBEntryReadLength = vue_prog_data->urb_read_length,
                     .VertexURBEntryReadOffset = 0,

                     .MaximumNumberofThreads = device->info.max_vs_threads - 1,
                     .StatisticsEnable = false,
                     .SIMD8DispatchEnable = pipeline->vs_simd8 != NO_KERNEL,
                     .VertexCacheDisable = false,
                     .FunctionEnable = true,

                     .VertexURBEntryOutputReadOffset = offset,
                     .VertexURBEntryOutputLength = length,
                     .UserClipDistanceClipTestEnableBitmask = 0,
                     .UserClipDistanceCullTestEnableBitmask = 0);

   const struct brw_wm_prog_data *wm_prog_data = &pipeline->wm_prog_data;

   const int num_thread_bias = GEN_GEN == 8 ? 2 : 1;
   if (pipeline->ps_ksp0 == NO_KERNEL) {
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS));
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS_EXTRA),
                     .PixelShaderValid = false);
   } else {
      emit_3dstate_sbe(pipeline);

      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS),
                     .KernelStartPointer0 = pipeline->ps_ksp0,

                     .SingleProgramFlow = false,
                     .VectorMaskEnable = true,
                     .SamplerCount = 1,

                     .ScratchSpaceBasePointer = pipeline->scratch_start[MESA_SHADER_FRAGMENT],
                     .PerThreadScratchSpace = scratch_space(&wm_prog_data->base),

                     .MaximumNumberofThreadsPerPSD = 64 - num_thread_bias,
                     .PositionXYOffsetSelect = wm_prog_data->uses_pos_offset ?
                        POSOFFSET_SAMPLE: POSOFFSET_NONE,
                     .PushConstantEnable = wm_prog_data->base.nr_params > 0,
                     ._8PixelDispatchEnable = pipeline->ps_simd8 != NO_KERNEL,
                     ._16PixelDispatchEnable = pipeline->ps_simd16 != NO_KERNEL,
                     ._32PixelDispatchEnable = false,

                     .DispatchGRFStartRegisterForConstantSetupData0 = pipeline->ps_grf_start0,
                     .DispatchGRFStartRegisterForConstantSetupData1 = 0,
                     .DispatchGRFStartRegisterForConstantSetupData2 = pipeline->ps_grf_start2,

                     .KernelStartPointer1 = 0,
                     .KernelStartPointer2 = pipeline->ps_ksp2);

      bool per_sample_ps = pCreateInfo->pMultisampleState &&
                           pCreateInfo->pMultisampleState->sampleShadingEnable;

      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS_EXTRA),
                     .PixelShaderValid = true,
                     .PixelShaderKillsPixel = wm_prog_data->uses_kill,
                     .PixelShaderComputedDepthMode = wm_prog_data->computed_depth_mode,
                     .AttributeEnable = wm_prog_data->num_varying_inputs > 0,
                     .oMaskPresenttoRenderTarget = wm_prog_data->uses_omask,
                     .PixelShaderIsPerSample = per_sample_ps,
                     .PixelShaderUsesSourceDepth = wm_prog_data->uses_src_depth,
                     .PixelShaderUsesSourceW = wm_prog_data->uses_src_w,
#if GEN_GEN >= 9
                     .PixelShaderPullsBary = wm_prog_data->pulls_bary,
                     .InputCoverageMaskState = wm_prog_data->uses_sample_mask ?
                        ICMS_INNER_CONSERVATIVE : ICMS_NONE,
#else
                     .PixelShaderUsesInputCoverageMask =
                        wm_prog_data->uses_sample_mask,
#endif
         );
   }

   *pPipeline = anv_pipeline_to_handle(pipeline);

   return VK_SUCCESS;
}
