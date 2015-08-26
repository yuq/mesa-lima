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

// Shader functions

VkResult anv_CreateShaderModule(
    VkDevice                                    _device,
    const VkShaderModuleCreateInfo*             pCreateInfo,
    VkShaderModule*                             pShaderModule)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_shader_module *module;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
   assert(pCreateInfo->flags == 0);

   module = anv_device_alloc(device, sizeof(*module) + pCreateInfo->codeSize, 8,
                             VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (module == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   module->size = pCreateInfo->codeSize;
   memcpy(module->data, pCreateInfo->pCode, module->size);

   *pShaderModule = anv_shader_module_to_handle(module);

   return VK_SUCCESS;
}

VkResult anv_DestroyShaderModule(
    VkDevice                                    _device,
    VkShaderModule                              _module)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_shader_module, module, _module);

   anv_device_free(device, module);

   return VK_SUCCESS;
}

VkResult anv_CreateShader(
    VkDevice                                    _device,
    const VkShaderCreateInfo*                   pCreateInfo,
    VkShader*                                   pShader)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_shader_module, module, pCreateInfo->module);
   struct anv_shader *shader;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_CREATE_INFO);
   assert(pCreateInfo->flags == 0);

   const char *name = pCreateInfo->pName ? pCreateInfo->pName : "main";
   size_t name_len = strlen(name);

   if (strcmp(name, "main") != 0) {
      anv_finishme("Multiple shaders per module not really supported");
   }

   shader = anv_device_alloc(device, sizeof(*shader) + name_len + 1, 8,
                             VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (shader == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   shader->module = module;
   memcpy(shader->entrypoint, name, name_len + 1);

   *pShader = anv_shader_to_handle(shader);

   return VK_SUCCESS;
}

VkResult anv_DestroyShader(
    VkDevice                                    _device,
    VkShader                                    _shader)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_shader, shader, _shader);

   anv_device_free(device, shader);

   return VK_SUCCESS;
}


VkResult anv_CreatePipelineCache(
    VkDevice                                    device,
    const VkPipelineCacheCreateInfo*            pCreateInfo,
    VkPipelineCache*                            pPipelineCache)
{
   pPipelineCache->handle = 1;

   stub_return(VK_SUCCESS);
}

VkResult anv_DestroyPipelineCache(
    VkDevice                                    _device,
    VkPipelineCache                             _cache)
{
   /* VkPipelineCache is a dummy object. */
   return VK_SUCCESS;
}

size_t anv_GetPipelineCacheSize(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache)
{
   stub_return(0);
}

VkResult anv_GetPipelineCacheData(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache,
    void*                                       pData)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_MergePipelineCaches(
    VkDevice                                    device,
    VkPipelineCache                             destCache,
    uint32_t                                    srcCacheCount,
    const VkPipelineCache*                      pSrcCaches)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_DestroyPipeline(
    VkDevice                                    _device,
    VkPipeline                                  _pipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_pipeline, pipeline, _pipeline);

   anv_compiler_free(pipeline);
   anv_reloc_list_finish(&pipeline->batch_relocs, pipeline->device);
   anv_state_stream_finish(&pipeline->program_stream);
   anv_state_pool_free(&device->dynamic_state_pool, pipeline->blend_state);
   anv_device_free(pipeline->device, pipeline);

   return VK_SUCCESS;
}

static const uint32_t vk_to_gen_primitive_type[] = {
   [VK_PRIMITIVE_TOPOLOGY_POINT_LIST]           = _3DPRIM_POINTLIST,
   [VK_PRIMITIVE_TOPOLOGY_LINE_LIST]            = _3DPRIM_LINELIST,
   [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP]           = _3DPRIM_LINESTRIP,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST]        = _3DPRIM_TRILIST,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP]       = _3DPRIM_TRISTRIP,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN]         = _3DPRIM_TRIFAN,
   [VK_PRIMITIVE_TOPOLOGY_LINE_LIST_ADJ]        = _3DPRIM_LINELIST_ADJ,
   [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_ADJ]       = _3DPRIM_LINESTRIP_ADJ,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_ADJ]    = _3DPRIM_TRILIST_ADJ,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_ADJ]   = _3DPRIM_TRISTRIP_ADJ,
   [VK_PRIMITIVE_TOPOLOGY_PATCH]                = _3DPRIM_PATCHLIST_1
};

VkResult
anv_pipeline_init(struct anv_pipeline *pipeline, struct anv_device *device,
                  const VkGraphicsPipelineCreateInfo *pCreateInfo,
                  const struct anv_graphics_pipeline_create_info *extra)
{
   VkResult result;

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

   const struct brw_wm_prog_data *wm_prog_data = &pipeline->wm_prog_data;

   pipeline->ps_ksp2 = 0;
   pipeline->ps_grf_start2 = 0;
   if (pipeline->ps_simd8 != NO_KERNEL) {
      pipeline->ps_ksp0 = pipeline->ps_simd8;
      pipeline->ps_grf_start0 = wm_prog_data->base.dispatch_grf_start_reg;
      if (pipeline->ps_simd16 != NO_KERNEL) {
         pipeline->ps_ksp2 = pipeline->ps_simd16;
         pipeline->ps_grf_start2 = wm_prog_data->dispatch_grf_start_reg_16;
      }
   } else if (pipeline->ps_simd16 != NO_KERNEL) {
      pipeline->ps_ksp0 = pipeline->ps_simd16;
      pipeline->ps_grf_start0 = wm_prog_data->dispatch_grf_start_reg_16;
   } else {
      unreachable("no ps shader");
   }

   const VkPipelineVertexInputStateCreateInfo *vi_info =
      pCreateInfo->pVertexInputState;
   pipeline->vb_used = 0;
   for (uint32_t i = 0; i < vi_info->bindingCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &vi_info->pVertexBindingDescriptions[i];

      pipeline->vb_used |= 1 << desc->binding;
      pipeline->binding_stride[desc->binding] = desc->strideInBytes;

      /* Step rate is programmed per vertex element (attribute), not
       * binding. Set up a map of which bindings step per instance, for
       * reference by vertex element setup. */
      switch (desc->stepRate) {
      default:
      case VK_VERTEX_INPUT_STEP_RATE_VERTEX:
         pipeline->instancing_enable[desc->binding] = false;
         break;
      case VK_VERTEX_INPUT_STEP_RATE_INSTANCE:
         pipeline->instancing_enable[desc->binding] = true;
         break;
      }
   }

   const VkPipelineInputAssemblyStateCreateInfo *ia_info =
      pCreateInfo->pInputAssemblyState;
   pipeline->primitive_restart = ia_info->primitiveRestartEnable;
   pipeline->topology = vk_to_gen_primitive_type[ia_info->topology];

   if (extra && extra->use_rectlist)
      pipeline->topology = _3DPRIM_RECTLIST;

   return VK_SUCCESS;
}

VkResult
anv_graphics_pipeline_create(
   VkDevice _device,
   const VkGraphicsPipelineCreateInfo *pCreateInfo,
   const struct anv_graphics_pipeline_create_info *extra,
   VkPipeline *pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   switch (device->info.gen) {
   case 7:
      return gen7_graphics_pipeline_create(_device, pCreateInfo, extra, pPipeline);
   case 8:
      return gen8_graphics_pipeline_create(_device, pCreateInfo, extra, pPipeline);
   default:
      unreachable("unsupported gen\n");
   }
}

VkResult anv_CreateGraphicsPipelines(
    VkDevice                                    _device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    count,
    const VkGraphicsPipelineCreateInfo*         pCreateInfos,
    VkPipeline*                                 pPipelines)
{
   VkResult result = VK_SUCCESS;

   unsigned i = 0;
   for (; i < count; i++) {
      result = anv_graphics_pipeline_create(_device, &pCreateInfos[i],
                                            NULL, &pPipelines[i]);
      if (result != VK_SUCCESS) {
         for (unsigned j = 0; j < i; j++) {
            anv_DestroyPipeline(_device, pPipelines[j]);
         }

         return result;
      }
   }

   return VK_SUCCESS;
}

static VkResult anv_compute_pipeline_create(
    VkDevice                                    _device,
    const VkComputePipelineCreateInfo*          pCreateInfo,
    VkPipeline*                                 pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   switch (device->info.gen) {
   case 7:
      return gen7_compute_pipeline_create(_device, pCreateInfo, pPipeline);
   case 8:
      return gen8_compute_pipeline_create(_device, pCreateInfo, pPipeline);
   default:
      unreachable("unsupported gen\n");
   }
}

VkResult anv_CreateComputePipelines(
    VkDevice                                    _device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    count,
    const VkComputePipelineCreateInfo*          pCreateInfos,
    VkPipeline*                                 pPipelines)
{
   VkResult result = VK_SUCCESS;

   unsigned i = 0;
   for (; i < count; i++) {
      result = anv_compute_pipeline_create(_device, &pCreateInfos[i],
                                           &pPipelines[i]);
      if (result != VK_SUCCESS) {
         for (unsigned j = 0; j < i; j++) {
            anv_DestroyPipeline(_device, pPipelines[j]);
         }

         return result;
      }
   }

   return VK_SUCCESS;
}

// Pipeline layout functions

VkResult anv_CreatePipelineLayout(
    VkDevice                                    _device,
    const VkPipelineLayoutCreateInfo*           pCreateInfo,
    VkPipelineLayout*                           pPipelineLayout)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_pipeline_layout *layout;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);

   layout = anv_device_alloc(device, sizeof(*layout), 8,
                             VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (layout == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   layout->num_sets = pCreateInfo->descriptorSetCount;

   uint32_t surface_start[VK_SHADER_STAGE_NUM] = { 0, };
   uint32_t sampler_start[VK_SHADER_STAGE_NUM] = { 0, };

   for (uint32_t s = 0; s < VK_SHADER_STAGE_NUM; s++) {
      layout->stage[s].surface_count = 0;
      layout->stage[s].sampler_count = 0;
   }

   for (uint32_t i = 0; i < pCreateInfo->descriptorSetCount; i++) {
      ANV_FROM_HANDLE(anv_descriptor_set_layout, set_layout,
                      pCreateInfo->pSetLayouts[i]);

      layout->set[i].layout = set_layout;
      for (uint32_t s = 0; s < VK_SHADER_STAGE_NUM; s++) {
         layout->set[i].surface_start[s] = surface_start[s];
         surface_start[s] += set_layout->stage[s].surface_count;
         layout->set[i].sampler_start[s] = sampler_start[s];
         sampler_start[s] += set_layout->stage[s].sampler_count;

         layout->stage[s].surface_count += set_layout->stage[s].surface_count;
         layout->stage[s].sampler_count += set_layout->stage[s].sampler_count;
      }
   }

   *pPipelineLayout = anv_pipeline_layout_to_handle(layout);

   return VK_SUCCESS;
}

VkResult anv_DestroyPipelineLayout(
    VkDevice                                    _device,
    VkPipelineLayout                            _pipelineLayout)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_pipeline_layout, pipeline_layout, _pipelineLayout);

   anv_device_free(device, pipeline_layout);

   return VK_SUCCESS;
}
