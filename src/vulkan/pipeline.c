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

// Shader functions

VkResult VKAPI vkCreateShader(
    VkDevice                                    _device,
    const VkShaderCreateInfo*                   pCreateInfo,
    VkShader*                                   pShader)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_shader *shader;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_CREATE_INFO);

   shader = anv_device_alloc(device, sizeof(*shader) + pCreateInfo->codeSize, 8,
                               VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (shader == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   shader->size = pCreateInfo->codeSize;
   memcpy(shader->data, pCreateInfo->pCode, shader->size);

   *pShader = (VkShader) shader;

   return VK_SUCCESS;
}

// Pipeline functions

static void
emit_vertex_input(struct anv_pipeline *pipeline, VkPipelineVertexInputCreateInfo *info)
{
   const uint32_t num_dwords = 1 + info->attributeCount * 2;
   uint32_t *p;
   bool instancing_enable[32];
   
   for (uint32_t i = 0; i < info->bindingCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &info->pVertexBindingDescriptions[i];
      
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
         .VertexBufferIndex = desc->location,
         .Valid = true,
         .SourceElementFormat = format->format,
         .EdgeFlagEnable = false,
         .SourceElementOffset = desc->offsetInBytes,
         .Component0Control = VFCOMP_STORE_SRC,
         .Component1Control = format->channels >= 2 ? VFCOMP_STORE_SRC : VFCOMP_STORE_0,
         .Component2Control = format->channels >= 3 ? VFCOMP_STORE_SRC : VFCOMP_STORE_0,
         .Component3Control = format->channels >= 4 ? VFCOMP_STORE_SRC : VFCOMP_STORE_1_FP
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
emit_ia_state(struct anv_pipeline *pipeline, VkPipelineIaStateCreateInfo *info)
{
   static const uint32_t vk_to_gen_primitive_type[] = {
      [VK_PRIMITIVE_TOPOLOGY_POINT_LIST] = _3DPRIM_POINTLIST,
      [VK_PRIMITIVE_TOPOLOGY_LINE_LIST] = _3DPRIM_LINELIST,
      [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP] = _3DPRIM_LINESTRIP,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST] = _3DPRIM_TRILIST,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP] = _3DPRIM_TRISTRIP,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN] = _3DPRIM_TRIFAN,
      [VK_PRIMITIVE_TOPOLOGY_LINE_LIST_ADJ] = _3DPRIM_LINELIST_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_ADJ] = _3DPRIM_LISTSTRIP_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_ADJ] = _3DPRIM_TRILIST_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_ADJ] = _3DPRIM_TRISTRIP_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_PATCH] = _3DPRIM_PATCHLIST_1
   };

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VF,
                  .IndexedDrawCutIndexEnable = info->primitiveRestartEnable,
                  .CutIndex = info->primitiveRestartIndex);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VF_TOPOLOGY,
                  .PrimitiveTopologyType = vk_to_gen_primitive_type[info->topology]);
}

static void
emit_rs_state(struct anv_pipeline *pipeline, VkPipelineRsStateCreateInfo *info)
{
   static const uint32_t vk_to_gen_cullmode[] = {
      [VK_CULL_MODE_NONE] = CULLMODE_NONE,
      [VK_CULL_MODE_FRONT] = CULLMODE_FRONT,
      [VK_CULL_MODE_BACK] = CULLMODE_BACK,
      [VK_CULL_MODE_FRONT_AND_BACK] = CULLMODE_BOTH
   };

   static const uint32_t vk_to_gen_fillmode[] = {
      [VK_FILL_MODE_POINTS] = RASTER_POINT,
      [VK_FILL_MODE_WIREFRAME] = RASTER_WIREFRAME,
      [VK_FILL_MODE_SOLID] = RASTER_SOLID
   };

   static const uint32_t vk_to_gen_front_face[] = {
      [VK_FRONT_FACE_CCW] = CounterClockwise,
      [VK_FRONT_FACE_CW] = Clockwise
   };
   
   static const uint32_t vk_to_gen_coordinate_origin[] = {
      [VK_COORDINATE_ORIGIN_UPPER_LEFT] = UPPERLEFT,
      [VK_COORDINATE_ORIGIN_LOWER_LEFT] = LOWERLEFT
   };

   struct GEN8_3DSTATE_SF sf = {
      GEN8_3DSTATE_SF_header,
      .ViewportTransformEnable = true,
      .TriangleStripListProvokingVertexSelect =
         info->provokingVertex == VK_PROVOKING_VERTEX_FIRST ? 0 : 2,
      .LineStripListProvokingVertexSelect =
         info->provokingVertex == VK_PROVOKING_VERTEX_FIRST ? 0 : 1,
      .TriangleFanProvokingVertexSelect =
         info->provokingVertex == VK_PROVOKING_VERTEX_FIRST ? 0 : 2,
      .PointWidthSource = info->programPointSize ? Vertex : State,
   };

   /* bool32_t                                    rasterizerDiscardEnable; */


   GEN8_3DSTATE_SF_pack(NULL, pipeline->state_sf, &sf);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_RASTER,
                  .FrontWinding = vk_to_gen_front_face[info->frontFace],
                  .CullMode = vk_to_gen_cullmode[info->cullMode],
                  .FrontFaceFillMode = vk_to_gen_fillmode[info->fillMode],
                  .BackFaceFillMode = vk_to_gen_fillmode[info->fillMode],
                  .ScissorRectangleEnable = true,
                  .ViewportZClipTestEnable = info->depthClipEnable);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_SBE,
                  .ForceVertexURBEntryReadLength = false,
                  .ForceVertexURBEntryReadOffset = false,
                  .PointSpriteTextureCoordinateOrigin =
                     vk_to_gen_coordinate_origin[info->pointOrigin],
                  .NumberofSFOutputAttributes =
                     pipeline->wm_prog_data.num_varying_inputs);

}

VkResult VKAPI vkCreateGraphicsPipeline(
    VkDevice                                    _device,
    const VkGraphicsPipelineCreateInfo*         pCreateInfo,
    VkPipeline*                                 pPipeline)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_pipeline *pipeline;
   const struct anv_common *common;
   VkPipelineShaderStageCreateInfo *shader_create_info;
   VkPipelineIaStateCreateInfo *ia_info;
   VkPipelineRsStateCreateInfo *rs_info;
   VkPipelineVertexInputCreateInfo *vi_info;
   VkResult result;
   uint32_t offset, length;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
   
   pipeline = anv_device_alloc(device, sizeof(*pipeline), 8,
                               VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pipeline == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->device = device;
   pipeline->layout = (struct anv_pipeline_layout *) pCreateInfo->layout;
   memset(pipeline->shaders, 0, sizeof(pipeline->shaders));
   result = anv_batch_init(&pipeline->batch, device);
   if (result != VK_SUCCESS)
      goto fail;

   for (common = pCreateInfo->pNext; common; common = common->pNext) {
      switch (common->sType) {
      case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_CREATE_INFO:
         vi_info = (VkPipelineVertexInputCreateInfo *) common;
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_IA_STATE_CREATE_INFO:
         ia_info = (VkPipelineIaStateCreateInfo *) common;
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_TESS_STATE_CREATE_INFO:
      case VK_STRUCTURE_TYPE_PIPELINE_VP_STATE_CREATE_INFO:
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_RS_STATE_CREATE_INFO:
         rs_info = (VkPipelineRsStateCreateInfo *) common;
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_MS_STATE_CREATE_INFO:
      case VK_STRUCTURE_TYPE_PIPELINE_CB_STATE_CREATE_INFO:
      case VK_STRUCTURE_TYPE_PIPELINE_DS_STATE_CREATE_INFO:
      case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO:
         shader_create_info = (VkPipelineShaderStageCreateInfo *) common;
         pipeline->shaders[shader_create_info->shader.stage] =
            (struct anv_shader *) shader_create_info->shader.shader;
         break;
      default:
         break;
      }
   }

   pipeline->use_repclear = false;

   anv_compiler_run(device->compiler, pipeline);

   emit_vertex_input(pipeline, vi_info);
   emit_ia_state(pipeline, ia_info);
   emit_rs_state(pipeline, rs_info);

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
                        
                     .PerThreadScratchSpace = 0,
                     .ScratchSpaceBasePointer = 0,

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

   //trp_generate_blend_hw_cmds(batch, pipeline);

   const struct brw_vue_prog_data *vue_prog_data = &pipeline->vs_prog_data.base;
   /* Skip the VUE header and position slots */
   offset = 1;
   length = (vue_prog_data->vue_map.num_slots + 1) / 2 - offset;

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

                  /* FIXME: pointer needs to be assigned outside as it aliases
                   * PerThreadScratchSpace.
                   */
                  .ScratchSpaceBasePointer = 0,
                  .PerThreadScratchSpace = 0,

                  .DispatchGRFStartRegisterForURBData =
                     vue_prog_data->base.dispatch_grf_start_reg,
                  .VertexURBEntryReadLength = vue_prog_data->urb_read_length,
                  .VertexURBEntryReadOffset = 0,

                  .MaximumNumberofThreads = device->info.max_vs_threads - 1,
                  .StatisticsEnable = false,
                  .SIMD8DispatchEnable = true,
                  .VertexCacheDisable = ia_info->disableVertexReuse,
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
                  .SamplerCount = 0,

                  .ScratchSpaceBasePointer = 0,
                  .PerThreadScratchSpace = 0,
                  
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

   *pPipeline = (VkPipeline) pipeline;

   return VK_SUCCESS;

 fail:
   anv_device_free(device, pipeline);
   
   return result;
}

VkResult
anv_pipeline_destroy(struct anv_pipeline *pipeline)
{
   anv_compiler_free(pipeline);
   anv_batch_finish(&pipeline->batch, pipeline->device);
   anv_device_free(pipeline->device, pipeline);

   return VK_SUCCESS;
}

VkResult VKAPI vkCreateGraphicsPipelineDerivative(
    VkDevice                                    device,
    const VkGraphicsPipelineCreateInfo*         pCreateInfo,
    VkPipeline                                  basePipeline,
    VkPipeline*                                 pPipeline)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkCreateComputePipeline(
    VkDevice                                    device,
    const VkComputePipelineCreateInfo*          pCreateInfo,
    VkPipeline*                                 pPipeline)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkStorePipeline(
    VkDevice                                    device,
    VkPipeline                                  pipeline,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkLoadPipeline(
    VkDevice                                    device,
    size_t                                      dataSize,
    const void*                                 pData,
    VkPipeline*                                 pPipeline)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkLoadPipelineDerivative(
    VkDevice                                    device,
    size_t                                      dataSize,
    const void*                                 pData,
    VkPipeline                                  basePipeline,
    VkPipeline*                                 pPipeline)
{
   return VK_UNSUPPORTED;
}

// Pipeline layout functions

VkResult VKAPI vkCreatePipelineLayout(
    VkDevice                                    _device,
    const VkPipelineLayoutCreateInfo*           pCreateInfo,
    VkPipelineLayout*                           pPipelineLayout)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_pipeline_layout *layout;
   struct anv_pipeline_layout_entry *entry;
   uint32_t total;
   size_t size;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
   
   total = 0;
   for (uint32_t i = 0; i < pCreateInfo->descriptorSetCount; i++) {
      struct anv_descriptor_set_layout *set_layout =
         (struct anv_descriptor_set_layout *) pCreateInfo->pSetLayouts[i];
      for (uint32_t j = 0; j < set_layout->count; j++)
         total += set_layout->total;
   }

   size = sizeof(*layout) + total * sizeof(layout->entries[0]);
   layout = anv_device_alloc(device, size, 8,
                             VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (layout == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   entry = layout->entries;
   for (uint32_t s = 0; s < VK_NUM_SHADER_STAGE; s++) {
      layout->stage[s].entries = entry;

      for (uint32_t i = 0; i < pCreateInfo->descriptorSetCount; i++) {
         struct anv_descriptor_set_layout *set_layout =
            (struct anv_descriptor_set_layout *) pCreateInfo->pSetLayouts[i];
         for (uint32_t j = 0; j < set_layout->count; j++)
            if (set_layout->bindings[j].mask & (1 << s)) {
               entry->type = set_layout->bindings[j].type;
               entry->set = i;
               entry->index = j;
               entry++;
            }
      }

      layout->stage[s].count = entry - layout->stage[s].entries;
   }

   *pPipelineLayout = (VkPipelineLayout) layout;

   return VK_SUCCESS;
}
