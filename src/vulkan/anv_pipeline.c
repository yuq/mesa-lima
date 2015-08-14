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

VkResult
anv_graphics_pipeline_create(
   VkDevice _device,
   const VkGraphicsPipelineCreateInfo *pCreateInfo,
   const struct anv_graphics_pipeline_create_info *extra,
   VkPipeline *pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   switch (device->info.gen) {
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
