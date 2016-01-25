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

#include "genX_pipeline_util.h"

static void
emit_vertex_input(struct anv_pipeline *pipeline,
                  const VkPipelineVertexInputStateCreateInfo *info,
                  const struct anv_graphics_pipeline_create_info *extra)
{
   static_assert(ANV_GEN >= 8, "should be compiling this for gen < 8");

   uint32_t elements;
   if (extra && extra->disable_vs) {
      /* If the VS is disabled, just assume the user knows what they're
       * doing and apply the layout blindly.  This can only come from
       * meta, so this *should* be safe.
       */
      elements = 0;
      for (uint32_t i = 0; i < info->vertexAttributeDescriptionCount; i++)
         elements |= (1 << info->pVertexAttributeDescriptions[i].location);
   } else {
      /* Pull inputs_read out of the VS prog data */
      uint64_t inputs_read = pipeline->vs_prog_data.inputs_read;
      assert((inputs_read & ((1 << VERT_ATTRIB_GENERIC0) - 1)) == 0);
      elements = inputs_read >> VERT_ATTRIB_GENERIC0;
   }

   const uint32_t num_dwords = 1 + __builtin_popcount(elements) * 2;

   uint32_t *p;
   if (elements != 0) {
      p = anv_batch_emitn(&pipeline->batch, num_dwords,
                          GENX(3DSTATE_VERTEX_ELEMENTS));
      memset(p + 1, 0, (num_dwords - 1) * 4);
   }

   for (uint32_t i = 0; i < info->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *desc =
         &info->pVertexAttributeDescriptions[i];
      enum isl_format format = anv_get_isl_format(desc->format,
                                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                                  VK_IMAGE_TILING_LINEAR);

      assert(desc->binding < 32);

      if ((elements & (1 << desc->location)) == 0)
         continue; /* Binding unused */

      uint32_t slot = __builtin_popcount(elements & ((1 << desc->location) - 1));

      struct GENX(VERTEX_ELEMENT_STATE) element = {
         .VertexBufferIndex = desc->binding,
         .Valid = true,
         .SourceElementFormat = format,
         .EdgeFlagEnable = false,
         .SourceElementOffset = desc->offset,
         .Component0Control = vertex_element_comp_control(format, 0),
         .Component1Control = vertex_element_comp_control(format, 1),
         .Component2Control = vertex_element_comp_control(format, 2),
         .Component3Control = vertex_element_comp_control(format, 3),
      };
      GENX(VERTEX_ELEMENT_STATE_pack)(NULL, &p[1 + slot * 2], &element);

      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_VF_INSTANCING),
                     .InstancingEnable = pipeline->instancing_enable[desc->binding],
                     .VertexElementIndex = slot,
                     /* Vulkan so far doesn't have an instance divisor, so
                      * this is always 1 (ignored if not instancing). */
                     .InstanceDataStepRate = 1);
   }

   const uint32_t id_slot = __builtin_popcount(elements);
   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_VF_SGVS),
                  .VertexIDEnable = pipeline->vs_prog_data.uses_vertexid,
                  .VertexIDComponentNumber = 2,
                  .VertexIDElementOffset = id_slot,
                  .InstanceIDEnable = pipeline->vs_prog_data.uses_instanceid,
                  .InstanceIDComponentNumber = 3,
                  .InstanceIDElementOffset = id_slot);
}

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
              const struct anv_graphics_pipeline_create_info *extra)
{
   struct GENX(3DSTATE_SF) sf = {
      GENX(3DSTATE_SF_header),
      .ViewportTransformEnable = !(extra && extra->disable_viewport),
      .TriangleStripListProvokingVertexSelect = 0,
      .LineStripListProvokingVertexSelect = 0,
      .TriangleFanProvokingVertexSelect = 0,
      .PointWidthSource = pipeline->writes_point_size ? Vertex : State,
      .PointWidth = 1.0,
   };

   /* FINISHME: VkBool32 rasterizerDiscardEnable; */

   GENX(3DSTATE_SF_pack)(NULL, pipeline->gen8.sf, &sf);

   struct GENX(3DSTATE_RASTER) raster = {
      GENX(3DSTATE_RASTER_header),
      .FrontWinding = vk_to_gen_front_face[info->frontFace],
      .CullMode = vk_to_gen_cullmode[info->cullMode],
      .FrontFaceFillMode = vk_to_gen_fillmode[info->polygonMode],
      .BackFaceFillMode = vk_to_gen_fillmode[info->polygonMode],
      .ScissorRectangleEnable = !(extra && extra->disable_scissor),
#if ANV_GEN == 8
      .ViewportZClipTestEnable = true,
#else
      /* GEN9+ splits ViewportZClipTestEnable into near and far enable bits */
      .ViewportZFarClipTestEnable = true,
      .ViewportZNearClipTestEnable = true,
#endif
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
   uint32_t *dw = ANV_GEN == 8 ?
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
      .StencilFailOp = vk_to_gen_stencil_op[info->front.failOp],
      .StencilPassDepthPassOp = vk_to_gen_stencil_op[info->front.passOp],
      .StencilPassDepthFailOp = vk_to_gen_stencil_op[info->front.depthFailOp],
      .StencilTestFunction = vk_to_gen_compare_op[info->front.compareOp],
      .BackfaceStencilFailOp = vk_to_gen_stencil_op[info->back.failOp],
      .BackfaceStencilPassDepthPassOp = vk_to_gen_stencil_op[info->back.passOp],
      .BackfaceStencilPassDepthFailOp =vk_to_gen_stencil_op[info->back.depthFailOp],
      .BackfaceStencilTestFunction = vk_to_gen_compare_op[info->back.compareOp],
   };

   GENX(3DSTATE_WM_DEPTH_STENCIL_pack)(NULL, dw, &wm_depth_stencil);
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
   emit_rs_state(pipeline, pCreateInfo->pRasterizationState, extra);
   emit_ds_state(pipeline, pCreateInfo->pDepthStencilState);
   emit_cb_state(pipeline, pCreateInfo->pColorBlendState,
                           pCreateInfo->pMultisampleState);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_VF_STATISTICS),
                   .StatisticsEnable = true);
   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_HS), .Enable = false);
   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_TE), .TEEnable = false);
   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_DS), .FunctionEnable = false);
   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_STREAMOUT), .SOFunctionEnable = false);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PUSH_CONSTANT_ALLOC_VS),
                  .ConstantBufferOffset = 0,
                  .ConstantBufferSize = 4);
   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PUSH_CONSTANT_ALLOC_GS),
                  .ConstantBufferOffset = 4,
                  .ConstantBufferSize = 4);
   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PUSH_CONSTANT_ALLOC_PS),
                  .ConstantBufferOffset = 8,
                  .ConstantBufferSize = 4);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_WM_CHROMAKEY),
                  .ChromaKeyKillEnable = false);
   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_AA_LINE_PARAMETERS));

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_CLIP),
                  .ClipEnable = true,
                  .ViewportXYClipTestEnable = !(extra && extra->disable_viewport),
                  .MinimumPointWidth = 0.125,
                  .MaximumPointWidth = 255.875);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_WM),
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

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_MULTISAMPLE),
                  .PixelPositionOffsetEnable = enable_sampling,
                  .PixelLocation = CENTER,
                  .NumberofMultisamples = log2_samples);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_SAMPLE_MASK),
                  .SampleMask = 0xffff);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_URB_VS),
                  .VSURBStartingAddress = pipeline->urb.vs_start,
                  .VSURBEntryAllocationSize = pipeline->urb.vs_size - 1,
                  .VSNumberofURBEntries = pipeline->urb.nr_vs_entries);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_URB_GS),
                  .GSURBStartingAddress = pipeline->urb.gs_start,
                  .GSURBEntryAllocationSize = pipeline->urb.gs_size - 1,
                  .GSNumberofURBEntries = pipeline->urb.nr_gs_entries);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_URB_HS),
                  .HSURBStartingAddress = pipeline->urb.vs_start,
                  .HSURBEntryAllocationSize = 0,
                  .HSNumberofURBEntries = 0);

   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_URB_DS),
                  .DSURBStartingAddress = pipeline->urb.vs_start,
                  .DSURBEntryAllocationSize = 0,
                  .DSNumberofURBEntries = 0);

   const struct brw_gs_prog_data *gs_prog_data = &pipeline->gs_prog_data;
   offset = 1;
   length = (gs_prog_data->base.vue_map.num_slots + 1) / 2 - offset;

   if (pipeline->gs_kernel == NO_KERNEL)
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_GS), .Enable = false);
   else
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_GS),
                     .SingleProgramFlow = false,
                     .KernelStartPointer = pipeline->gs_kernel,
                     .VectorMaskEnable = Dmask,
                     .SamplerCount = 0,
                     .BindingTableEntryCount = 0,
                     .ExpectedVertexCount = pipeline->gs_vertex_count,

                     .ScratchSpaceBasePointer = pipeline->scratch_start[MESA_SHADER_GEOMETRY],
                     .PerThreadScratchSpace = ffs(gs_prog_data->base.base.total_scratch / 2048),

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

                     .ScratchSpaceBasePointer = pipeline->scratch_start[MESA_SHADER_VERTEX],
                     .PerThreadScratchSpace = ffs(vue_prog_data->base.total_scratch / 2048),

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

   const int num_thread_bias = ANV_GEN == 8 ? 2 : 1;
   if (pipeline->ps_ksp0 == NO_KERNEL) {
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS));
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS_EXTRA),
                     .PixelShaderValid = false);
   } else {
      /* TODO: We should clean this up.  Among other things, this is mostly
       * shared with other gens.
       */
      const struct brw_vue_map *fs_input_map;
      if (pipeline->gs_kernel == NO_KERNEL)
         fs_input_map = &vue_prog_data->vue_map;
      else
         fs_input_map = &gs_prog_data->base.vue_map;

      struct GENX(3DSTATE_SBE_SWIZ) swiz = {
         GENX(3DSTATE_SBE_SWIZ_header),
      };

      int max_source_attr = 0;
      for (int attr = 0; attr < VARYING_SLOT_MAX; attr++) {
         int input_index = wm_prog_data->urb_setup[attr];

         if (input_index < 0)
            continue;

         int source_attr = fs_input_map->varying_to_slot[attr];
         max_source_attr = MAX2(max_source_attr, source_attr);

         if (input_index >= 16)
            continue;

         if (source_attr == -1) {
            /* This attribute does not exist in the VUE--that means that the
             * vertex shader did not write to it.  It could be that it's a
             * regular varying read by the fragment shader but not written by
             * the vertex shader or it's gl_PrimitiveID. In the first case the
             * value is undefined, in the second it needs to be
             * gl_PrimitiveID.
             */
            swiz.Attribute[input_index].ConstantSource = PRIM_ID;
            swiz.Attribute[input_index].ComponentOverrideX = true;
            swiz.Attribute[input_index].ComponentOverrideY = true;
            swiz.Attribute[input_index].ComponentOverrideZ = true;
            swiz.Attribute[input_index].ComponentOverrideW = true;
         } else {
            /* We have to subtract two slots to accout for the URB entry output
             * read offset in the VS and GS stages.
             */
            swiz.Attribute[input_index].SourceAttribute = source_attr - 2;
         }
      }

      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_SBE),
                     .AttributeSwizzleEnable = true,
                     .ForceVertexURBEntryReadLength = false,
                     .ForceVertexURBEntryReadOffset = false,
                     .VertexURBEntryReadLength =
                        DIV_ROUND_UP(max_source_attr + 1, 2),
                     .PointSpriteTextureCoordinateOrigin = UPPERLEFT,
                     .NumberofSFOutputAttributes =
                     wm_prog_data->num_varying_inputs,

#if ANV_GEN >= 9
                     .Attribute0ActiveComponentFormat = ACF_XYZW,
                     .Attribute1ActiveComponentFormat = ACF_XYZW,
                     .Attribute2ActiveComponentFormat = ACF_XYZW,
                     .Attribute3ActiveComponentFormat = ACF_XYZW,
                     .Attribute4ActiveComponentFormat = ACF_XYZW,
                     .Attribute5ActiveComponentFormat = ACF_XYZW,
                     .Attribute6ActiveComponentFormat = ACF_XYZW,
                     .Attribute7ActiveComponentFormat = ACF_XYZW,
                     .Attribute8ActiveComponentFormat = ACF_XYZW,
                     .Attribute9ActiveComponentFormat = ACF_XYZW,
                     .Attribute10ActiveComponentFormat = ACF_XYZW,
                     .Attribute11ActiveComponentFormat = ACF_XYZW,
                     .Attribute12ActiveComponentFormat = ACF_XYZW,
                     .Attribute13ActiveComponentFormat = ACF_XYZW,
                     .Attribute14ActiveComponentFormat = ACF_XYZW,
                     .Attribute15ActiveComponentFormat = ACF_XYZW,
                     /* wow, much field, very attribute */
                     .Attribute16ActiveComponentFormat = ACF_XYZW,
                     .Attribute17ActiveComponentFormat = ACF_XYZW,
                     .Attribute18ActiveComponentFormat = ACF_XYZW,
                     .Attribute19ActiveComponentFormat = ACF_XYZW,
                     .Attribute20ActiveComponentFormat = ACF_XYZW,
                     .Attribute21ActiveComponentFormat = ACF_XYZW,
                     .Attribute22ActiveComponentFormat = ACF_XYZW,
                     .Attribute23ActiveComponentFormat = ACF_XYZW,
                     .Attribute24ActiveComponentFormat = ACF_XYZW,
                     .Attribute25ActiveComponentFormat = ACF_XYZW,
                     .Attribute26ActiveComponentFormat = ACF_XYZW,
                     .Attribute27ActiveComponentFormat = ACF_XYZW,
                     .Attribute28ActiveComponentFormat = ACF_XYZW,
                     .Attribute29ActiveComponentFormat = ACF_XYZW,
                     .Attribute28ActiveComponentFormat = ACF_XYZW,
                     .Attribute29ActiveComponentFormat = ACF_XYZW,
                     .Attribute30ActiveComponentFormat = ACF_XYZW,
#endif
         );

      uint32_t *dw = anv_batch_emit_dwords(&pipeline->batch,
                                           GENX(3DSTATE_SBE_SWIZ_length));
      GENX(3DSTATE_SBE_SWIZ_pack)(&pipeline->batch, dw, &swiz);

      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS),
                     .KernelStartPointer0 = pipeline->ps_ksp0,

                     .SingleProgramFlow = false,
                     .VectorMaskEnable = true,
                     .SamplerCount = 1,

                     .ScratchSpaceBasePointer = pipeline->scratch_start[MESA_SHADER_FRAGMENT],
                     .PerThreadScratchSpace = ffs(wm_prog_data->base.total_scratch / 2048),

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

      bool per_sample_ps = false;
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS_EXTRA),
                     .PixelShaderValid = true,
                     .PixelShaderKillsPixel = wm_prog_data->uses_kill,
                     .PixelShaderComputedDepthMode = wm_prog_data->computed_depth_mode,
                     .AttributeEnable = wm_prog_data->num_varying_inputs > 0,
                     .oMaskPresenttoRenderTarget = wm_prog_data->uses_omask,
                     .PixelShaderIsPerSample = per_sample_ps,
#if ANV_GEN >= 9
                     .PixelShaderPullsBary = wm_prog_data->pulls_bary,
                     .InputCoverageMaskState = ICMS_NONE
#endif
         );
   }

   *pPipeline = anv_pipeline_to_handle(pipeline);

   return VK_SUCCESS;
}

VkResult genX(compute_pipeline_create)(
    VkDevice                                    _device,
    struct anv_pipeline_cache *                 cache,
    const VkComputePipelineCreateInfo*          pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_pipeline *pipeline;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);

   pipeline = anv_alloc2(&device->alloc, pAllocator, sizeof(*pipeline), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->device = device;
   pipeline->layout = anv_pipeline_layout_from_handle(pCreateInfo->layout);

   pipeline->blend_state.map = NULL;

   result = anv_reloc_list_init(&pipeline->batch_relocs,
                                pAllocator ? pAllocator : &device->alloc);
   if (result != VK_SUCCESS) {
      anv_free2(&device->alloc, pAllocator, pipeline);
      return result;
   }
   pipeline->batch.next = pipeline->batch.start = pipeline->batch_data;
   pipeline->batch.end = pipeline->batch.start + sizeof(pipeline->batch_data);
   pipeline->batch.relocs = &pipeline->batch_relocs;

   /* When we free the pipeline, we detect stages based on the NULL status
    * of various prog_data pointers.  Make them NULL by default.
    */
   memset(pipeline->prog_data, 0, sizeof(pipeline->prog_data));
   memset(pipeline->scratch_start, 0, sizeof(pipeline->scratch_start));

   pipeline->vs_simd8 = NO_KERNEL;
   pipeline->vs_vec4 = NO_KERNEL;
   pipeline->gs_kernel = NO_KERNEL;

   pipeline->active_stages = 0;
   pipeline->total_scratch = 0;

   assert(pCreateInfo->stage.stage == VK_SHADER_STAGE_COMPUTE_BIT);
   ANV_FROM_HANDLE(anv_shader_module, module,  pCreateInfo->stage.module);
   anv_pipeline_compile_cs(pipeline, cache, pCreateInfo, module,
                           pCreateInfo->stage.pName,
                           pCreateInfo->stage.pSpecializationInfo);

   pipeline->use_repclear = false;

   const struct brw_cs_prog_data *cs_prog_data = &pipeline->cs_prog_data;

   anv_batch_emit(&pipeline->batch, GENX(MEDIA_VFE_STATE),
                  .ScratchSpaceBasePointer = pipeline->scratch_start[MESA_SHADER_COMPUTE],
                  .PerThreadScratchSpace = ffs(cs_prog_data->base.total_scratch / 2048),
                  .ScratchSpaceBasePointerHigh = 0,
                  .StackSize = 0,

                  .MaximumNumberofThreads = device->info.max_cs_threads - 1,
                  .NumberofURBEntries = 2,
                  .ResetGatewayTimer = true,
#if ANV_GEN == 8
                  .BypassGatewayControl = true,
#endif
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
