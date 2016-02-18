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

/*
 * Descriptor set layouts.
 */

VkResult anv_CreateDescriptorSetLayout(
    VkDevice                                    _device,
    const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDescriptorSetLayout*                      pSetLayout)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_descriptor_set_layout *set_layout;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);

   uint32_t max_binding = 0;
   uint32_t immutable_sampler_count = 0;
   for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
      max_binding = MAX2(max_binding, pCreateInfo->pBindings[j].binding);
      if (pCreateInfo->pBindings[j].pImmutableSamplers)
         immutable_sampler_count += pCreateInfo->pBindings[j].descriptorCount;
   }

   size_t size = sizeof(struct anv_descriptor_set_layout) +
                 (max_binding + 1) * sizeof(set_layout->binding[0]) +
                 immutable_sampler_count * sizeof(struct anv_sampler *);

   set_layout = anv_alloc2(&device->alloc, pAllocator, size, 8,
                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!set_layout)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   /* We just allocate all the samplers at the end of the struct */
   struct anv_sampler **samplers =
      (struct anv_sampler **)&set_layout->binding[max_binding + 1];

   set_layout->binding_count = max_binding + 1;
   set_layout->shader_stages = 0;
   set_layout->size = 0;

   for (uint32_t b = 0; b <= max_binding; b++) {
      /* Initialize all binding_layout entries to -1 */
      memset(&set_layout->binding[b], -1, sizeof(set_layout->binding[b]));

      set_layout->binding[b].immutable_samplers = NULL;
   }

   /* Initialize all samplers to 0 */
   memset(samplers, 0, immutable_sampler_count * sizeof(*samplers));

   uint32_t sampler_count[MESA_SHADER_STAGES] = { 0, };
   uint32_t surface_count[MESA_SHADER_STAGES] = { 0, };
   uint32_t image_count[MESA_SHADER_STAGES] = { 0, };
   uint32_t buffer_count = 0;
   uint32_t dynamic_offset_count = 0;

   for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
      const VkDescriptorSetLayoutBinding *binding = &pCreateInfo->pBindings[j];
      uint32_t b = binding->binding;

      assert(binding->descriptorCount > 0);
      set_layout->binding[b].array_size = binding->descriptorCount;
      set_layout->binding[b].descriptor_index = set_layout->size;
      set_layout->size += binding->descriptorCount;

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         anv_foreach_stage(s, binding->stageFlags) {
            set_layout->binding[b].stage[s].sampler_index = sampler_count[s];
            sampler_count[s] += binding->descriptorCount;
         }
         break;
      default:
         break;
      }

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         set_layout->binding[b].buffer_index = buffer_count;
         buffer_count += binding->descriptorCount;
         /* fall through */

      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         anv_foreach_stage(s, binding->stageFlags) {
            set_layout->binding[b].stage[s].surface_index = surface_count[s];
            surface_count[s] += binding->descriptorCount;
         }
         break;
      default:
         break;
      }

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         set_layout->binding[b].dynamic_offset_index = dynamic_offset_count;
         dynamic_offset_count += binding->descriptorCount;
         break;
      default:
         break;
      }

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         anv_foreach_stage(s, binding->stageFlags) {
            set_layout->binding[b].stage[s].image_index = image_count[s];
            image_count[s] += binding->descriptorCount;
         }
         break;
      default:
         break;
      }

      if (binding->pImmutableSamplers) {
         set_layout->binding[b].immutable_samplers = samplers;
         samplers += binding->descriptorCount;

         for (uint32_t i = 0; i < binding->descriptorCount; i++)
            set_layout->binding[b].immutable_samplers[i] =
               anv_sampler_from_handle(binding->pImmutableSamplers[i]);
      } else {
         set_layout->binding[b].immutable_samplers = NULL;
      }

      set_layout->shader_stages |= binding->stageFlags;
   }

   set_layout->buffer_count = buffer_count;
   set_layout->dynamic_offset_count = dynamic_offset_count;

   *pSetLayout = anv_descriptor_set_layout_to_handle(set_layout);

   return VK_SUCCESS;
}

void anv_DestroyDescriptorSetLayout(
    VkDevice                                    _device,
    VkDescriptorSetLayout                       _set_layout,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_descriptor_set_layout, set_layout, _set_layout);

   anv_free2(&device->alloc, pAllocator, set_layout);
}

/*
 * Pipeline layouts.  These have nothing to do with the pipeline.  They are
 * just muttiple descriptor set layouts pasted together
 */

VkResult anv_CreatePipelineLayout(
    VkDevice                                    _device,
    const VkPipelineLayoutCreateInfo*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkPipelineLayout*                           pPipelineLayout)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_pipeline_layout *layout;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);

   layout = anv_alloc2(&device->alloc, pAllocator, sizeof(*layout), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (layout == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   layout->num_sets = pCreateInfo->setLayoutCount;

   unsigned dynamic_offset_count = 0;

   memset(layout->stage, 0, sizeof(layout->stage));
   for (uint32_t set = 0; set < pCreateInfo->setLayoutCount; set++) {
      ANV_FROM_HANDLE(anv_descriptor_set_layout, set_layout,
                      pCreateInfo->pSetLayouts[set]);
      layout->set[set].layout = set_layout;

      layout->set[set].dynamic_offset_start = dynamic_offset_count;
      for (uint32_t b = 0; b < set_layout->binding_count; b++) {
         if (set_layout->binding[b].dynamic_offset_index >= 0)
            dynamic_offset_count += set_layout->binding[b].array_size;
         for (gl_shader_stage s = 0; s < MESA_SHADER_STAGES; s++) {
            if (set_layout->binding[b].stage[s].surface_index >= 0)
               layout->stage[s].has_dynamic_offsets = true;
         }
      }
   }

   *pPipelineLayout = anv_pipeline_layout_to_handle(layout);

   return VK_SUCCESS;
}

void anv_DestroyPipelineLayout(
    VkDevice                                    _device,
    VkPipelineLayout                            _pipelineLayout,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_pipeline_layout, pipeline_layout, _pipelineLayout);

   anv_free2(&device->alloc, pAllocator, pipeline_layout);
}

/*
 * Descriptor pools.  These are a no-op for now.
 */

VkResult anv_CreateDescriptorPool(
    VkDevice                                    device,
    const VkDescriptorPoolCreateInfo*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDescriptorPool*                           pDescriptorPool)
{
   anv_finishme("VkDescriptorPool is a stub");
   *pDescriptorPool = (VkDescriptorPool)1;
   return VK_SUCCESS;
}

void anv_DestroyDescriptorPool(
    VkDevice                                    _device,
    VkDescriptorPool                            _pool,
    const VkAllocationCallbacks*                pAllocator)
{
   anv_finishme("VkDescriptorPool is a stub: free the pool's descriptor sets");
}

VkResult anv_ResetDescriptorPool(
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool,
    VkDescriptorPoolResetFlags                  flags)
{
   anv_finishme("VkDescriptorPool is a stub: free the pool's descriptor sets");
   return VK_SUCCESS;
}

VkResult
anv_descriptor_set_create(struct anv_device *device,
                          const struct anv_descriptor_set_layout *layout,
                          struct anv_descriptor_set **out_set)
{
   struct anv_descriptor_set *set;
   size_t size = sizeof(*set) + layout->size * sizeof(set->descriptors[0]);

   set = anv_alloc(&device->alloc /* XXX: Use the pool */, size, 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!set)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   /* A descriptor set may not be 100% filled. Clear the set so we can can
    * later detect holes in it.
    */
   memset(set, 0, size);

   set->layout = layout;

   /* Go through and fill out immutable samplers if we have any */
   struct anv_descriptor *desc = set->descriptors;
   for (uint32_t b = 0; b < layout->binding_count; b++) {
      if (layout->binding[b].immutable_samplers) {
         for (uint32_t i = 0; i < layout->binding[b].array_size; i++)
            desc[i].sampler = layout->binding[b].immutable_samplers[i];
      }
      desc += layout->binding[b].array_size;
   }

   /* XXX: Use the pool */
   set->buffer_views =
      anv_alloc(&device->alloc,
                sizeof(set->buffer_views[0]) * layout->buffer_count, 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!set->buffer_views) {
      anv_free(&device->alloc, set);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   for (uint32_t b = 0; b < layout->buffer_count; b++) {
      set->buffer_views[b].surface_state =
         anv_state_pool_alloc(&device->surface_state_pool, 64, 64);
   }
   set->buffer_count = layout->buffer_count;
   *out_set = set;

   return VK_SUCCESS;
}

void
anv_descriptor_set_destroy(struct anv_device *device,
                           struct anv_descriptor_set *set)
{
   /* XXX: Use the pool */
   for (uint32_t b = 0; b < set->buffer_count; b++)
      anv_state_pool_free(&device->surface_state_pool,
                          set->buffer_views[b].surface_state);

   anv_free(&device->alloc, set->buffer_views);
   anv_free(&device->alloc, set);
}

VkResult anv_AllocateDescriptorSets(
    VkDevice                                    _device,
    const VkDescriptorSetAllocateInfo*          pAllocateInfo,
    VkDescriptorSet*                            pDescriptorSets)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   VkResult result = VK_SUCCESS;
   struct anv_descriptor_set *set;
   uint32_t i;

   for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      ANV_FROM_HANDLE(anv_descriptor_set_layout, layout,
                      pAllocateInfo->pSetLayouts[i]);

      result = anv_descriptor_set_create(device, layout, &set);
      if (result != VK_SUCCESS)
         break;

      pDescriptorSets[i] = anv_descriptor_set_to_handle(set);
   }

   if (result != VK_SUCCESS)
      anv_FreeDescriptorSets(_device, pAllocateInfo->descriptorPool,
                             i, pDescriptorSets);

   return result;
}

VkResult anv_FreeDescriptorSets(
    VkDevice                                    _device,
    VkDescriptorPool                            descriptorPool,
    uint32_t                                    count,
    const VkDescriptorSet*                      pDescriptorSets)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   for (uint32_t i = 0; i < count; i++) {
      ANV_FROM_HANDLE(anv_descriptor_set, set, pDescriptorSets[i]);

      anv_descriptor_set_destroy(device, set);
   }

   return VK_SUCCESS;
}

void anv_UpdateDescriptorSets(
    VkDevice                                    _device,
    uint32_t                                    descriptorWriteCount,
    const VkWriteDescriptorSet*                 pDescriptorWrites,
    uint32_t                                    descriptorCopyCount,
    const VkCopyDescriptorSet*                  pDescriptorCopies)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   for (uint32_t i = 0; i < descriptorWriteCount; i++) {
      const VkWriteDescriptorSet *write = &pDescriptorWrites[i];
      ANV_FROM_HANDLE(anv_descriptor_set, set, write->dstSet);
      const struct anv_descriptor_set_binding_layout *bind_layout =
         &set->layout->binding[write->dstBinding];
      struct anv_descriptor *desc =
         &set->descriptors[bind_layout->descriptor_index];
      desc += write->dstArrayElement;

      switch (write->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            ANV_FROM_HANDLE(anv_sampler, sampler,
                            write->pImageInfo[j].sampler);

            desc[j] = (struct anv_descriptor) {
               .type = VK_DESCRIPTOR_TYPE_SAMPLER,
               .sampler = sampler,
            };
         }
         break;

      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            ANV_FROM_HANDLE(anv_image_view, iview,
                            write->pImageInfo[j].imageView);
            ANV_FROM_HANDLE(anv_sampler, sampler,
                            write->pImageInfo[j].sampler);

            desc[j].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            desc[j].image_view = iview;

            /* If this descriptor has an immutable sampler, we don't want
             * to stomp on it.
             */
            if (sampler)
               desc[j].sampler = sampler;
         }
         break;

      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            ANV_FROM_HANDLE(anv_image_view, iview,
                            write->pImageInfo[j].imageView);

            desc[j] = (struct anv_descriptor) {
               .type = write->descriptorType,
               .image_view = iview,
            };
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            ANV_FROM_HANDLE(anv_buffer_view, bview,
                            write->pTexelBufferView[j]);

            desc[j] = (struct anv_descriptor) {
               .type = write->descriptorType,
               .buffer_view = bview,
            };
         }
         break;

      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         anv_finishme("input attachments not implemented");
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            assert(write->pBufferInfo[j].buffer);
            ANV_FROM_HANDLE(anv_buffer, buffer, write->pBufferInfo[j].buffer);
            assert(buffer);

            struct anv_buffer_view *view =
               &set->buffer_views[bind_layout->buffer_index];
            view += write->dstArrayElement + j;

            const struct anv_format *format =
               anv_format_for_descriptor_type(write->descriptorType);

            view->format = format->isl_format;
            view->bo = buffer->bo;
            view->offset = buffer->offset + write->pBufferInfo[j].offset;

            /* For buffers with dynamic offsets, we use the full possible
             * range in the surface state and do the actual range-checking
             * in the shader.
             */
            if (bind_layout->dynamic_offset_index >= 0 ||
                write->pBufferInfo[j].range == VK_WHOLE_SIZE)
               view->range = buffer->size - write->pBufferInfo[j].offset;
            else
               view->range = write->pBufferInfo[j].range;

            anv_fill_buffer_surface_state(device, view->surface_state,
                                          view->format,
                                          view->offset, view->range, 1);

            desc[j] = (struct anv_descriptor) {
               .type = write->descriptorType,
               .buffer_view = view,
            };

         }

      default:
         break;
      }
   }

   for (uint32_t i = 0; i < descriptorCopyCount; i++) {
      const VkCopyDescriptorSet *copy = &pDescriptorCopies[i];
      ANV_FROM_HANDLE(anv_descriptor_set, src, copy->dstSet);
      ANV_FROM_HANDLE(anv_descriptor_set, dst, copy->dstSet);

      const struct anv_descriptor_set_binding_layout *src_layout =
         &src->layout->binding[copy->srcBinding];
      struct anv_descriptor *src_desc =
         &src->descriptors[src_layout->descriptor_index];
      src_desc += copy->srcArrayElement;

      const struct anv_descriptor_set_binding_layout *dst_layout =
         &dst->layout->binding[copy->dstBinding];
      struct anv_descriptor *dst_desc =
         &dst->descriptors[dst_layout->descriptor_index];
      dst_desc += copy->dstArrayElement;

      for (uint32_t j = 0; j < copy->descriptorCount; j++)
         dst_desc[j] = src_desc[j];
   }
}
