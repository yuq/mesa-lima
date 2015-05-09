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

static int
anv_env_get_int(const char *name)
{
   const char *val = getenv(name);

   if (!val)
      return 0;

   return strtol(val, NULL, 0);
}

static VkResult
fill_physical_device(struct anv_physical_device *device,
                     struct anv_instance *instance,
                     const char *path)
{
   int fd;
   
   fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return vk_error(VK_ERROR_UNAVAILABLE);

   device->instance = instance;
   device->path = path;
   
   device->chipset_id = anv_env_get_int("INTEL_DEVID_OVERRIDE");
   device->no_hw = false;
   if (device->chipset_id) {
      /* INTEL_DEVID_OVERRIDE implies INTEL_NO_HW. */
      device->no_hw = true;
   } else {
      device->chipset_id = anv_gem_get_param(fd, I915_PARAM_CHIPSET_ID);
   }
   if (!device->chipset_id)
      goto fail;

   device->name = brw_get_device_name(device->chipset_id);
   device->info = brw_get_device_info(device->chipset_id, -1);
   if (!device->info)
      goto fail;
   
   if (!anv_gem_get_param(fd, I915_PARAM_HAS_WAIT_TIMEOUT))
      goto fail;

   if (!anv_gem_get_param(fd, I915_PARAM_HAS_EXECBUF2))
      goto fail;

   if (!anv_gem_get_param(fd, I915_PARAM_HAS_LLC))
      goto fail;

   if (!anv_gem_get_param(fd, I915_PARAM_HAS_EXEC_CONSTANTS))
      goto fail;

   close(fd);
   
   return VK_SUCCESS;
   
 fail:
   close(fd);

   return vk_error(VK_ERROR_UNAVAILABLE);
}

static void *default_alloc(
    void*                                       pUserData,
    size_t                                      size,
    size_t                                      alignment,
    VkSystemAllocType                           allocType)
{
   return malloc(size);
}

static void default_free(
    void*                                       pUserData,
    void*                                       pMem)
{
   free(pMem);
}

static const VkAllocCallbacks default_alloc_callbacks = {
   .pUserData = NULL,
   .pfnAlloc = default_alloc,
   .pfnFree = default_free
};

VkResult VKAPI vkCreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    VkInstance*                                 pInstance)
{
   struct anv_instance *instance;
   const VkAllocCallbacks *alloc_callbacks = &default_alloc_callbacks;
   void *user_data = NULL;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

   if (pCreateInfo->pAllocCb) {
      alloc_callbacks = pCreateInfo->pAllocCb;
      user_data = pCreateInfo->pAllocCb->pUserData;
   }
   instance = alloc_callbacks->pfnAlloc(user_data, sizeof(*instance), 8,
                                        VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (!instance)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   instance->pAllocUserData = alloc_callbacks->pUserData;
   instance->pfnAlloc = alloc_callbacks->pfnAlloc;
   instance->pfnFree = alloc_callbacks->pfnFree;
   instance->apiVersion = pCreateInfo->pAppInfo->apiVersion;

   instance->physicalDeviceCount = 0;
   result = fill_physical_device(&instance->physicalDevice,
                                 instance, "/dev/dri/renderD128");
   if (result == VK_SUCCESS)
      instance->physicalDeviceCount++;

   *pInstance = (VkInstance) instance;

   return VK_SUCCESS;
}

VkResult VKAPI vkDestroyInstance(
    VkInstance                                  _instance)
{
   struct anv_instance *instance = (struct anv_instance *) _instance;

   instance->pfnFree(instance->pAllocUserData, instance);

   return VK_SUCCESS;
}

VkResult VKAPI vkEnumeratePhysicalDevices(
    VkInstance                                  _instance,
    uint32_t*                                   pPhysicalDeviceCount,
    VkPhysicalDevice*                           pPhysicalDevices)
{
   struct anv_instance *instance = (struct anv_instance *) _instance;

   if (*pPhysicalDeviceCount >= 1)
      pPhysicalDevices[0] = (VkPhysicalDevice) &instance->physicalDevice;
   *pPhysicalDeviceCount = instance->physicalDeviceCount;

   return VK_SUCCESS;
}

VkResult VKAPI vkGetPhysicalDeviceInfo(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceInfoType                    infoType,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   struct anv_physical_device *device = (struct anv_physical_device *) physicalDevice;
   VkPhysicalDeviceProperties *properties;
   VkPhysicalDevicePerformance *performance;
   VkPhysicalDeviceQueueProperties *queue_properties;
   VkPhysicalDeviceMemoryProperties *memory_properties;
   uint64_t ns_per_tick = 80;
   
   switch (infoType) {
   case VK_PHYSICAL_DEVICE_INFO_TYPE_PROPERTIES:
      properties = pData;
      assert(*pDataSize >= sizeof(*properties));
      *pDataSize = sizeof(*properties); /* Assuming we have to return the size of our struct. */

      properties->apiVersion = 1;
      properties->driverVersion = 1;
      properties->vendorId = 0x8086;
      properties->deviceId = device->chipset_id;
      properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
      strcpy(properties->deviceName, device->name);
      properties->maxInlineMemoryUpdateSize = 0;
      properties->maxBoundDescriptorSets = 0;
      properties->maxThreadGroupSize = 0;
      properties->timestampFrequency = 1000 * 1000 * 1000 / ns_per_tick;
      properties->multiColorAttachmentClears = 0;
      properties->maxDescriptorSets = 2;
      properties->maxViewports = 16;
      properties->maxColorAttachments = 8;
      return VK_SUCCESS;

   case VK_PHYSICAL_DEVICE_INFO_TYPE_PERFORMANCE:
      performance = pData;
      assert(*pDataSize >= sizeof(*performance));
      *pDataSize = sizeof(*performance); /* Assuming we have to return the size of our struct. */

      performance->maxDeviceClock = 1.0;
      performance->aluPerClock = 1.0;
      performance->texPerClock = 1.0;
      performance->primsPerClock = 1.0;
      performance->pixelsPerClock = 1.0;
      return VK_SUCCESS;
      
   case VK_PHYSICAL_DEVICE_INFO_TYPE_QUEUE_PROPERTIES:
      queue_properties = pData;
      assert(*pDataSize >= sizeof(*queue_properties));
      *pDataSize = sizeof(*queue_properties);

      queue_properties->queueFlags = 0;
      queue_properties->queueCount = 1;
      queue_properties->maxAtomicCounters = 0;
      queue_properties->supportsTimestamps = 0;
      queue_properties->maxMemReferences = 0;
      return VK_SUCCESS;

   case VK_PHYSICAL_DEVICE_INFO_TYPE_MEMORY_PROPERTIES:
      memory_properties = pData;
      assert(*pDataSize >= sizeof(*memory_properties));
      *pDataSize = sizeof(*memory_properties);

      memory_properties->supportsMigration = false;
      memory_properties->supportsPinning = false;
      return VK_SUCCESS;

   default:
      return VK_UNSUPPORTED;
   }

}

void * vkGetProcAddr(
    VkPhysicalDevice                            physicalDevice,
    const char*                                 pName)
{
   return NULL;
}

static void
parse_debug_flags(struct anv_device *device)
{
   const char *debug, *p, *end;

   debug = getenv("INTEL_DEBUG");
   device->dump_aub = false;
   if (debug) {
      for (p = debug; *p; p = end + 1) {
         end = strchrnul(p, ',');
         if (end - p == 3 && memcmp(p, "aub", 3) == 0)
            device->dump_aub = true;
         if (end - p == 5 && memcmp(p, "no_hw", 5) == 0)
            device->no_hw = true;
         if (*end == '\0')
            break;
      }
   }
}

VkResult VKAPI vkCreateDevice(
    VkPhysicalDevice                            _physicalDevice,
    const VkDeviceCreateInfo*                   pCreateInfo,
    VkDevice*                                   pDevice)
{
   struct anv_physical_device *physicalDevice =
      (struct anv_physical_device *) _physicalDevice;
   struct anv_instance *instance = physicalDevice->instance;
   struct anv_device *device;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

   device = instance->pfnAlloc(instance->pAllocUserData,
                               sizeof(*device), 8,
                               VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (!device)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   device->no_hw = physicalDevice->no_hw;
   parse_debug_flags(device);

   device->instance = physicalDevice->instance;
   device->fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   if (device->fd == -1)
      goto fail_device;
      
   device->context_id = anv_gem_create_context(device);
   if (device->context_id == -1)
      goto fail_fd;

   anv_block_pool_init(&device->dyn_state_block_pool, device, 2048);

   anv_state_pool_init(&device->dyn_state_pool,
                       &device->dyn_state_block_pool);

   anv_block_pool_init(&device->instruction_block_pool, device, 2048);
   anv_block_pool_init(&device->surface_state_block_pool, device, 2048);

   anv_state_pool_init(&device->surface_state_pool,
                       &device->surface_state_block_pool);

   device->compiler = anv_compiler_create(device->fd);
   device->aub_writer = NULL;

   device->info = *physicalDevice->info;

   pthread_mutex_init(&device->mutex, NULL);

   *pDevice = (VkDevice) device;

   return VK_SUCCESS;

 fail_fd:
   close(device->fd);
 fail_device:
   anv_device_free(device, device);

   return vk_error(VK_ERROR_UNAVAILABLE);
}

VkResult VKAPI vkDestroyDevice(
    VkDevice                                    _device)
{
   struct anv_device *device = (struct anv_device *) _device;

   anv_compiler_destroy(device->compiler);

   anv_block_pool_finish(&device->dyn_state_block_pool);
   anv_block_pool_finish(&device->instruction_block_pool);
   anv_block_pool_finish(&device->surface_state_block_pool);

   close(device->fd);

   if (device->aub_writer)
      anv_aub_writer_destroy(device->aub_writer);

   anv_device_free(device, device);

   return VK_SUCCESS;
}

VkResult VKAPI vkGetGlobalExtensionInfo(
    VkExtensionInfoType                         infoType,
    uint32_t                                    extensionIndex,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   uint32_t *count;

   switch (infoType) {
   case VK_EXTENSION_INFO_TYPE_COUNT:
      count = pData;
      assert(*pDataSize == 4);
      *count = 0;
      return VK_SUCCESS;
      
   case VK_EXTENSION_INFO_TYPE_PROPERTIES:
      return vk_error(VK_ERROR_INVALID_EXTENSION);
      
   default:
      return VK_UNSUPPORTED;
   }
}

VkResult VKAPI vkGetPhysicalDeviceExtensionInfo(
    VkPhysicalDevice                            physicalDevice,
    VkExtensionInfoType                         infoType,
    uint32_t                                    extensionIndex,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   uint32_t *count;

   switch (infoType) {
   case VK_EXTENSION_INFO_TYPE_COUNT:
      count = pData;
      assert(*pDataSize == 4);
      *count = 0;
      return VK_SUCCESS;
      
   case VK_EXTENSION_INFO_TYPE_PROPERTIES:
      return vk_error(VK_ERROR_INVALID_EXTENSION);
      
   default:
      return VK_UNSUPPORTED;
   }
}

VkResult VKAPI vkEnumerateLayers(
    VkPhysicalDevice                            physicalDevice,
    size_t                                      maxStringSize,
    size_t*                                     pLayerCount,
    char* const*                                pOutLayers,
    void*                                       pReserved)
{
   *pLayerCount = 0;

   return VK_SUCCESS;
}

VkResult VKAPI vkGetDeviceQueue(
    VkDevice                                    _device,
    uint32_t                                    queueNodeIndex,
    uint32_t                                    queueIndex,
    VkQueue*                                    pQueue)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_queue *queue;

   /* FIXME: Should allocate these at device create time. */

   queue = anv_device_alloc(device, sizeof(*queue), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (queue == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   queue->device = device;
   queue->pool = &device->surface_state_pool;

   queue->completed_serial = anv_state_pool_alloc(queue->pool, 4, 4);
   *(uint32_t *)queue->completed_serial.map = 0;
   queue->next_serial = 1;

   *pQueue = (VkQueue) queue;

   return VK_SUCCESS;
}

static const uint32_t BATCH_SIZE = 8192;

VkResult
anv_batch_init(struct anv_batch *batch, struct anv_device *device)
{
   VkResult result;

   result = anv_bo_init_new(&batch->bo, device, BATCH_SIZE);
   if (result != VK_SUCCESS)
      return result;

   batch->bo.map =
      anv_gem_mmap(device, batch->bo.gem_handle, 0, BATCH_SIZE);
   if (batch->bo.map == NULL) {
      anv_gem_close(device, batch->bo.gem_handle);
      return vk_error(VK_ERROR_MEMORY_MAP_FAILED);
   }

   batch->cmd_relocs.num_relocs = 0;
   batch->surf_relocs.num_relocs = 0;
   batch->next = batch->bo.map;

   return VK_SUCCESS;
}

void
anv_batch_finish(struct anv_batch *batch, struct anv_device *device)
{
   anv_gem_munmap(batch->bo.map, BATCH_SIZE);
   anv_gem_close(device, batch->bo.gem_handle);
}

void
anv_batch_reset(struct anv_batch *batch)
{
   batch->next = batch->bo.map;
   batch->cmd_relocs.num_relocs = 0;
   batch->surf_relocs.num_relocs = 0;
}

void *
anv_batch_emit_dwords(struct anv_batch *batch, int num_dwords)
{
   void *p = batch->next;

   batch->next += num_dwords * 4;

   return p;
}

static void
anv_reloc_list_append(struct anv_reloc_list *list,
                      struct anv_reloc_list *other, uint32_t offset)
{
   uint32_t i, count;

   count = list->num_relocs;
   memcpy(&list->relocs[count], &other->relocs[0],
          other->num_relocs * sizeof(other->relocs[0]));
   memcpy(&list->reloc_bos[count], &other->reloc_bos[0],
          other->num_relocs * sizeof(other->reloc_bos[0]));
   for (i = 0; i < other->num_relocs; i++)
      list->relocs[i + count].offset += offset;

   count += other->num_relocs;
}

static uint64_t
anv_reloc_list_add(struct anv_reloc_list *list,
                   uint32_t offset,
                   struct anv_bo *target_bo, uint32_t delta)
{
   struct drm_i915_gem_relocation_entry *entry;
   int index;

   assert(list->num_relocs < ANV_BATCH_MAX_RELOCS);

   /* XXX: Can we use I915_EXEC_HANDLE_LUT? */
   index = list->num_relocs++;
   list->reloc_bos[index] = target_bo;
   entry = &list->relocs[index];
   entry->target_handle = target_bo->gem_handle;
   entry->delta = delta;
   entry->offset = offset;
   entry->presumed_offset = target_bo->offset;
   entry->read_domains = 0;
   entry->write_domain = 0;

   return target_bo->offset + delta;
}

void
anv_batch_emit_batch(struct anv_batch *batch, struct anv_batch *other)
{
   uint32_t size, offset;

   size = other->next - other->bo.map;
   memcpy(batch->next, other->bo.map, size);

   offset = batch->next - batch->bo.map;
   anv_reloc_list_append(&batch->cmd_relocs, &other->cmd_relocs, offset);
   anv_reloc_list_append(&batch->surf_relocs, &other->surf_relocs, offset);

   batch->next += size;
}

uint64_t
anv_batch_emit_reloc(struct anv_batch *batch,
                     void *location, struct anv_bo *bo, uint32_t delta)
{
   return anv_reloc_list_add(&batch->cmd_relocs,
                             location - batch->bo.map, bo, delta);
}

VkResult VKAPI vkQueueSubmit(
    VkQueue                                     _queue,
    uint32_t                                    cmdBufferCount,
    const VkCmdBuffer*                          pCmdBuffers,
    VkFence                                     fence)
{
   struct anv_queue *queue = (struct anv_queue *) _queue;
   struct anv_device *device = queue->device;
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) pCmdBuffers[0];
   int ret;

   assert(cmdBufferCount == 1);

   if (device->dump_aub)
      anv_cmd_buffer_dump(cmd_buffer);

   if (!device->no_hw) {
      ret = anv_gem_execbuffer(device, &cmd_buffer->execbuf);
      if (ret != 0)
         goto fail;

      for (uint32_t i = 0; i < cmd_buffer->bo_count; i++)
         cmd_buffer->exec2_bos[i]->offset = cmd_buffer->exec2_objects[i].offset;
   } else {
      *(uint32_t *)queue->completed_serial.map = cmd_buffer->serial;
   }

   return VK_SUCCESS;

 fail:
   pthread_mutex_unlock(&device->mutex);

   return vk_error(VK_ERROR_UNKNOWN);
}

VkResult VKAPI vkQueueAddMemReferences(
    VkQueue                                     queue,
    uint32_t                                    count,
    const VkDeviceMemory*                       pMems)
{
   return VK_SUCCESS;
}

VkResult vkQueueRemoveMemReferences(
    VkQueue                                     queue,
    uint32_t                                    count,
    const VkDeviceMemory*                       pMems)
{
   return VK_SUCCESS;
}

VkResult VKAPI vkQueueWaitIdle(
    VkQueue                                     _queue)
{
   struct anv_queue *queue = (struct anv_queue *) _queue;

   return vkDeviceWaitIdle((VkDevice) queue->device);
}

VkResult VKAPI vkDeviceWaitIdle(
    VkDevice                                    _device)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_state state;
   struct anv_batch batch;
   struct drm_i915_gem_execbuffer2 execbuf;
   struct drm_i915_gem_exec_object2 exec2_objects[1];
   struct anv_bo *bo = NULL;
   VkResult result;
   int64_t timeout;
   int ret;

   state = anv_state_pool_alloc(&device->dyn_state_pool, 32, 32);
   bo = &device->dyn_state_pool.block_pool->bo;
   batch.next = state.map;
   anv_batch_emit(&batch, GEN8_MI_BATCH_BUFFER_END);
   anv_batch_emit(&batch, GEN8_MI_NOOP);

   exec2_objects[0].handle = bo->gem_handle;
   exec2_objects[0].relocation_count = 0;
   exec2_objects[0].relocs_ptr = 0;
   exec2_objects[0].alignment = 0;
   exec2_objects[0].offset = bo->offset;
   exec2_objects[0].flags = 0;
   exec2_objects[0].rsvd1 = 0;
   exec2_objects[0].rsvd2 = 0;

   execbuf.buffers_ptr = (uintptr_t) exec2_objects;
   execbuf.buffer_count = 1;
   execbuf.batch_start_offset = state.offset;
   execbuf.batch_len = batch.next - state.map;
   execbuf.cliprects_ptr = 0;
   execbuf.num_cliprects = 0;
   execbuf.DR1 = 0;
   execbuf.DR4 = 0;

   execbuf.flags =
      I915_EXEC_HANDLE_LUT | I915_EXEC_NO_RELOC | I915_EXEC_RENDER;
   execbuf.rsvd1 = device->context_id;
   execbuf.rsvd2 = 0;

   if (!device->no_hw) {
      ret = anv_gem_execbuffer(device, &execbuf);
      if (ret != 0) {
         result = vk_error(VK_ERROR_UNKNOWN);
         goto fail;
      }

      timeout = INT64_MAX;
      ret = anv_gem_wait(device, bo->gem_handle, &timeout);
      if (ret != 0) {
         result = vk_error(VK_ERROR_UNKNOWN);
         goto fail;
      }
   }

   anv_state_pool_free(&device->dyn_state_pool, state);

   return VK_SUCCESS;

 fail:
   anv_state_pool_free(&device->dyn_state_pool, state);

   return result;
}

void *
anv_device_alloc(struct anv_device *            device,
                 size_t                         size,
                 size_t                         alignment,
                 VkSystemAllocType              allocType)
{
   return device->instance->pfnAlloc(device->instance->pAllocUserData,
                                     size,
                                     alignment,
                                     allocType);
}

void
anv_device_free(struct anv_device *             device,
                void *                          mem)
{
   return device->instance->pfnFree(device->instance->pAllocUserData,
                                    mem);
}

VkResult
anv_bo_init_new(struct anv_bo *bo, struct anv_device *device, uint64_t size)
{
   bo->gem_handle = anv_gem_create(device, size);
   if (!bo->gem_handle)
      return vk_error(VK_ERROR_OUT_OF_DEVICE_MEMORY);

   bo->map = NULL;
   bo->index = 0;
   bo->offset = 0;
   bo->size = size;

   return VK_SUCCESS;
}

VkResult VKAPI vkAllocMemory(
    VkDevice                                    _device,
    const VkMemoryAllocInfo*                    pAllocInfo,
    VkDeviceMemory*                             pMem)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_device_memory *mem;
   VkResult result;

   assert(pAllocInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO);

   mem = anv_device_alloc(device, sizeof(*mem), 8,
                          VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (mem == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   result = anv_bo_init_new(&mem->bo, device, pAllocInfo->allocationSize);
   if (result != VK_SUCCESS)
      goto fail;

   *pMem = (VkDeviceMemory) mem;

   return VK_SUCCESS;   

 fail:
   anv_device_free(device, mem);

   return result;
}

VkResult VKAPI vkFreeMemory(
    VkDevice                                    _device,
    VkDeviceMemory                              _mem)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_device_memory *mem = (struct anv_device_memory *) _mem;

   if (mem->bo.map)
      anv_gem_munmap(mem->bo.map, mem->bo.size);

   if (mem->bo.gem_handle != 0)
      anv_gem_close(device, mem->bo.gem_handle);

   anv_device_free(device, mem);

   return VK_SUCCESS;
}

VkResult VKAPI vkSetMemoryPriority(
    VkDevice                                    device,
    VkDeviceMemory                              mem,
    VkMemoryPriority                            priority)
{
   return VK_SUCCESS;
}

VkResult VKAPI vkMapMemory(
    VkDevice                                    _device,
    VkDeviceMemory                              _mem,
    VkDeviceSize                                offset,
    VkDeviceSize                                size,
    VkMemoryMapFlags                            flags,
    void**                                      ppData)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_device_memory *mem = (struct anv_device_memory *) _mem;

   /* FIXME: Is this supposed to be thread safe? Since vkUnmapMemory() only
    * takes a VkDeviceMemory pointer, it seems like only one map of the memory
    * at a time is valid. We could just mmap up front and return an offset
    * pointer here, but that may exhaust virtual memory on 32 bit
    * userspace. */

   mem->map = anv_gem_mmap(device, mem->bo.gem_handle, offset, size);
   mem->map_size = size;

   *ppData = mem->map;
   
   return VK_SUCCESS;
}

VkResult VKAPI vkUnmapMemory(
    VkDevice                                    _device,
    VkDeviceMemory                              _mem)
{
   struct anv_device_memory *mem = (struct anv_device_memory *) _mem;

   anv_gem_munmap(mem->map, mem->map_size);

   return VK_SUCCESS;
}

VkResult VKAPI vkFlushMappedMemory(
    VkDevice                                    device,
    VkDeviceMemory                              mem,
    VkDeviceSize                                offset,
    VkDeviceSize                                size)
{
   /* clflush here for !llc platforms */

   return VK_SUCCESS;
}

VkResult VKAPI vkPinSystemMemory(
    VkDevice                                    device,
    const void*                                 pSysMem,
    size_t                                      memSize,
    VkDeviceMemory*                             pMem)
{
   return VK_SUCCESS;
}

VkResult VKAPI vkGetMultiDeviceCompatibility(
    VkPhysicalDevice                            physicalDevice0,
    VkPhysicalDevice                            physicalDevice1,
    VkPhysicalDeviceCompatibilityInfo*          pInfo)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkOpenSharedMemory(
    VkDevice                                    device,
    const VkMemoryOpenInfo*                     pOpenInfo,
    VkDeviceMemory*                             pMem)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkOpenSharedSemaphore(
    VkDevice                                    device,
    const VkSemaphoreOpenInfo*                  pOpenInfo,
    VkSemaphore*                                pSemaphore)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkOpenPeerMemory(
    VkDevice                                    device,
    const VkPeerMemoryOpenInfo*                 pOpenInfo,
    VkDeviceMemory*                             pMem)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkOpenPeerImage(
    VkDevice                                    device,
    const VkPeerImageOpenInfo*                  pOpenInfo,
    VkImage*                                    pImage,
    VkDeviceMemory*                             pMem)
{
   return VK_UNSUPPORTED;
}

static VkResult
anv_instance_destructor(struct anv_device *     device,
                        VkObject                object)
{
   return vkDestroyInstance(object);
}

static VkResult
anv_noop_destructor(struct anv_device *         device,
                    VkObject                    object)
{
   return VK_SUCCESS;
}

static VkResult
anv_device_destructor(struct anv_device *       device,
                      VkObject                  object)
{
   return vkDestroyDevice(object);
}

static VkResult
anv_cmd_buffer_destructor(struct anv_device *   device,
                          VkObject              object)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) object;
   
   anv_state_stream_finish(&cmd_buffer->surface_state_stream);
   anv_batch_finish(&cmd_buffer->batch, device);
   anv_device_free(device, cmd_buffer->exec2_objects);
   anv_device_free(device, cmd_buffer->exec2_bos);
   anv_device_free(device, cmd_buffer);

   return VK_SUCCESS;
}

static VkResult
anv_pipeline_destructor(struct anv_device *   device,
                        VkObject              object)
{
   struct anv_pipeline *pipeline = (struct anv_pipeline *) object;

   return anv_pipeline_destroy(pipeline);
}

static VkResult
anv_free_destructor(struct anv_device *         device,
                    VkObject                    object)
{
   anv_device_free(device, (void *) object);

   return VK_SUCCESS;
}

static VkResult (*anv_object_destructors[])(struct anv_device *device,
                                            VkObject object) = {
   [VK_OBJECT_TYPE_INSTANCE] =        anv_instance_destructor,
   [VK_OBJECT_TYPE_PHYSICAL_DEVICE] = anv_noop_destructor,
   [VK_OBJECT_TYPE_DEVICE] =          anv_device_destructor,
   [VK_OBJECT_TYPE_QUEUE] =           anv_noop_destructor,
   [VK_OBJECT_TYPE_COMMAND_BUFFER] =  anv_cmd_buffer_destructor,
   [VK_OBJECT_TYPE_PIPELINE] =        anv_pipeline_destructor,
   [VK_OBJECT_TYPE_SHADER] =          anv_free_destructor,
   [VK_OBJECT_TYPE_BUFFER] =          anv_free_destructor,
   [VK_OBJECT_TYPE_IMAGE] =           anv_free_destructor,
   [VK_OBJECT_TYPE_RENDER_PASS] =     anv_free_destructor
};

VkResult VKAPI vkDestroyObject(
    VkDevice                                    _device,
    VkObjectType                                objType,
    VkObject                                    object)
{
   struct anv_device *device = (struct anv_device *) _device;

   assert(objType < ARRAY_SIZE(anv_object_destructors) &&
          anv_object_destructors[objType] != NULL);
      
   return anv_object_destructors[objType](device, object);
}

static void
fill_memory_requirements(
    VkObjectType                                objType,
    VkObject                                    object,
    VkMemoryRequirements *                      memory_requirements)
{
   struct anv_buffer *buffer;
   struct anv_image *image;

   memory_requirements->memPropsAllowed =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_DEVICE_COHERENT_BIT |
      /* VK_MEMORY_PROPERTY_HOST_UNCACHED_BIT | */
      VK_MEMORY_PROPERTY_HOST_WRITE_COMBINED_BIT |
      VK_MEMORY_PROPERTY_PREFER_HOST_LOCAL |
      VK_MEMORY_PROPERTY_SHAREABLE_BIT;

   memory_requirements->memPropsRequired = 0;

   switch (objType) {
   case VK_OBJECT_TYPE_BUFFER:
      buffer = (struct anv_buffer *) object;
      memory_requirements->size = buffer->size;
      memory_requirements->alignment = 16;
      break;
   case VK_OBJECT_TYPE_IMAGE:
      image = (struct anv_image *) object;
      memory_requirements->size = image->size;
      memory_requirements->alignment = image->alignment;
      break;
   default:
      memory_requirements->size = 0;
      break;
   }
}

VkResult VKAPI vkGetObjectInfo(
    VkDevice                                    _device,
    VkObjectType                                objType,
    VkObject                                    object,
    VkObjectInfoType                            infoType,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   VkMemoryRequirements memory_requirements;

   switch (infoType) {
   case VK_OBJECT_INFO_TYPE_MEMORY_REQUIREMENTS:
      fill_memory_requirements(objType, object, &memory_requirements);
      memcpy(pData, &memory_requirements,
             MIN2(*pDataSize, sizeof(memory_requirements)));
      *pDataSize = sizeof(memory_requirements);
      return VK_SUCCESS;

   case VK_OBJECT_INFO_TYPE_MEMORY_ALLOCATION_COUNT:
   default:
      return VK_UNSUPPORTED;
   }

}

VkResult VKAPI vkQueueBindObjectMemory(
    VkQueue                                     queue,
    VkObjectType                                objType,
    VkObject                                    object,
    uint32_t                                    allocationIdx,
    VkDeviceMemory                              _mem,
    VkDeviceSize                                memOffset)
{
   struct anv_buffer *buffer;
   struct anv_image *image;
   struct anv_device_memory *mem = (struct anv_device_memory *) _mem;

   switch (objType) {
   case VK_OBJECT_TYPE_BUFFER:
      buffer = (struct anv_buffer *) object;
      buffer->mem = mem;
      buffer->offset = memOffset;
      break;
   case VK_OBJECT_TYPE_IMAGE:
      image = (struct anv_image *) object;
      image->mem = mem;
      image->offset = memOffset;
      break;
   default:
      break;
   }
   
   return VK_SUCCESS;
}

VkResult VKAPI vkQueueBindObjectMemoryRange(
    VkQueue                                     queue,
    VkObjectType                                objType,
    VkObject                                    object,
    uint32_t                                    allocationIdx,
    VkDeviceSize                                rangeOffset,
    VkDeviceSize                                rangeSize,
    VkDeviceMemory                              mem,
    VkDeviceSize                                memOffset)
{
   return VK_UNSUPPORTED;
}

VkResult vkQueueBindImageMemoryRange(
    VkQueue                                     queue,
    VkImage                                     image,
    uint32_t                                    allocationIdx,
    const VkImageMemoryBindInfo*                pBindInfo,
    VkDeviceMemory                              mem,
    VkDeviceSize                                memOffset)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkCreateFence(
    VkDevice                                    device,
    const VkFenceCreateInfo*                    pCreateInfo,
    VkFence*                                    pFence)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkResetFences(
    VkDevice                                    device,
    uint32_t                                    fenceCount,
    VkFence*                                    pFences)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkGetFenceStatus(
    VkDevice                                    device,
    VkFence                                     fence)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkWaitForFences(
    VkDevice                                    device,
    uint32_t                                    fenceCount,
    const VkFence*                              pFences,
    bool32_t                                    waitAll,
    uint64_t                                    timeout)
{
   return VK_UNSUPPORTED;
}

// Queue semaphore functions

VkResult VKAPI vkCreateSemaphore(
    VkDevice                                    device,
    const VkSemaphoreCreateInfo*                pCreateInfo,
    VkSemaphore*                                pSemaphore)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkQueueSignalSemaphore(
    VkQueue                                     queue,
    VkSemaphore                                 semaphore)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkQueueWaitSemaphore(
    VkQueue                                     queue,
    VkSemaphore                                 semaphore)
{
   return VK_UNSUPPORTED;
}

// Event functions

VkResult VKAPI vkCreateEvent(
    VkDevice                                    device,
    const VkEventCreateInfo*                    pCreateInfo,
    VkEvent*                                    pEvent)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkGetEventStatus(
    VkDevice                                    device,
    VkEvent                                     event)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkSetEvent(
    VkDevice                                    device,
    VkEvent                                     event)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkResetEvent(
    VkDevice                                    device,
    VkEvent                                     event)
{
   return VK_UNSUPPORTED;
}

// Query functions

struct anv_query_pool {
   VkQueryType                                 type;
   uint32_t                                    slots;
   struct anv_bo bo;
};

VkResult VKAPI vkCreateQueryPool(
    VkDevice                                    _device,
    const VkQueryPoolCreateInfo*                pCreateInfo,
    VkQueryPool*                                pQueryPool)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_query_pool *pool;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
   
   pool = anv_device_alloc(device, sizeof(*pool), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pool == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pool->type = pCreateInfo->queryType;
   result = anv_bo_init_new(&pool->bo, device, pCreateInfo->slots * 16);
   if (result != VK_SUCCESS)
      goto fail;

   *pQueryPool = (VkQueryPool) pool;

   return VK_SUCCESS;

 fail:
   anv_device_free(device, pool);

   return result;
}

VkResult VKAPI vkGetQueryPoolResults(
    VkDevice                                    device,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount,
    size_t*                                     pDataSize,
    void*                                       pData,
    VkQueryResultFlags                          flags)
{
   return VK_UNSUPPORTED;
}

// Format capabilities

VkResult VKAPI vkGetFormatInfo(
    VkDevice                                    device,
    VkFormat                                    format,
    VkFormatInfoType                            infoType,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   return VK_UNSUPPORTED;
}

// Buffer functions

VkResult VKAPI vkCreateBuffer(
    VkDevice                                    _device,
    const VkBufferCreateInfo*                   pCreateInfo,
    VkBuffer*                                   pBuffer)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_buffer *buffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);

   buffer = anv_device_alloc(device, sizeof(*buffer), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (buffer == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   buffer->size = pCreateInfo->size;
   buffer->mem = NULL;
   buffer->offset = 0;

   *pBuffer = (VkBuffer) buffer;

   return VK_SUCCESS;
}

// Buffer view functions

VkResult VKAPI vkCreateBufferView(
    VkDevice                                    _device,
    const VkBufferViewCreateInfo*               pCreateInfo,
    VkBufferView*                               pView)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_buffer_view *view;
   const struct anv_format *format;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);

   view = anv_device_alloc(device, sizeof(*view), 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (view == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   view->buffer = (struct anv_buffer *) pCreateInfo->buffer;
   view->offset = pCreateInfo->offset;
   view->surface_state =
      anv_state_pool_alloc(&device->surface_state_pool, 64, 64);

   format = anv_format_for_vk_format(pCreateInfo->format);
   /* This assumes RGBA float format. */
   uint32_t stride = 4;
   uint32_t num_elements = pCreateInfo->range / stride;
   struct GEN8_RENDER_SURFACE_STATE surface_state = {
      .SurfaceType = SURFTYPE_BUFFER,
      .SurfaceArray = false,
      .SurfaceFormat = format->format,
      .SurfaceVerticalAlignment = VALIGN4,
      .SurfaceHorizontalAlignment = HALIGN4,
      .TileMode = LINEAR,
      .VerticalLineStride = 0,
      .VerticalLineStrideOffset = 0,
      .SamplerL2BypassModeDisable = true,
      .RenderCacheReadWriteMode = WriteOnlyCache,
      .MemoryObjectControlState = 0, /* FIXME: MOCS */
      .BaseMipLevel = 0,
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
      .ResourceMinLOD = 0,
      /* FIXME: We assume that the image must be bound at this time. */
      .SurfaceBaseAddress = { NULL, view->buffer->offset + view->offset },
   };

   GEN8_RENDER_SURFACE_STATE_pack(NULL, view->surface_state.map, &surface_state);

   *pView = (VkImageView) view;

   return VK_SUCCESS;
}

// Sampler functions

struct anv_sampler {
   uint32_t state[4];
};

VkResult VKAPI vkCreateSampler(
    VkDevice                                    _device,
    const VkSamplerCreateInfo*                  pCreateInfo,
    VkSampler*                                  pSampler)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_sampler *sampler;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);

   sampler = anv_device_alloc(device, sizeof(*sampler), 8,
                              VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (!sampler)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   struct GEN8_SAMPLER_STATE sampler_state = {
      .SamplerDisable = 0,
      .TextureBorderColorMode = 0,
      .LODPreClampMode = 0,
      .BaseMipLevel = 0,
      .MipModeFilter = 0,
      .MagModeFilter = 0,
      .MinModeFilter = 0,
      .TextureLODBias = 0,
      .AnisotropicAlgorithm = 0,
      .MinLOD = 0,
      .MaxLOD = 0,
      .ChromaKeyEnable = 0,
      .ChromaKeyIndex = 0,
      .ChromaKeyMode = 0,
      .ShadowFunction = 0,
      .CubeSurfaceControlMode = 0,
      .IndirectStatePointer = 0,
      .LODClampMagnificationMode = 0,
      .MaximumAnisotropy = 0,
      .RAddressMinFilterRoundingEnable = 0,
      .RAddressMagFilterRoundingEnable = 0,
      .VAddressMinFilterRoundingEnable = 0,
      .VAddressMagFilterRoundingEnable = 0,
      .UAddressMinFilterRoundingEnable = 0,
      .UAddressMagFilterRoundingEnable = 0,
      .TrilinearFilterQuality = 0,
      .NonnormalizedCoordinateEnable = 0,
      .TCXAddressControlMode = 0,
      .TCYAddressControlMode = 0,
      .TCZAddressControlMode = 0,
   };

   GEN8_SAMPLER_STATE_pack(NULL, sampler->state, &sampler_state);

   *pSampler = (VkSampler) sampler;

   return VK_SUCCESS;
}

// Descriptor set functions

VkResult VKAPI vkCreateDescriptorSetLayout(
    VkDevice                                    _device,
    const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
    VkDescriptorSetLayout*                      pSetLayout)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_descriptor_set_layout *set_layout;
   uint32_t count, k;
   size_t size, total;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);

   count = 0;
   for (uint32_t i = 0; i < pCreateInfo->count; i++)
      count += pCreateInfo->pBinding[i].count;

   size = sizeof(*set_layout) +
      count * sizeof(set_layout->bindings[0]);
   set_layout = anv_device_alloc(device, size, 8,
                                 VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (!set_layout)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   k = 0;
   total = 0;
   for (uint32_t i = 0; i < pCreateInfo->count; i++) {
      for (uint32_t j = 0; j < pCreateInfo->pBinding[i].count; j++) {
         set_layout->bindings[k].mask = pCreateInfo->pBinding[i].stageFlags;
         set_layout->bindings[k].type = pCreateInfo->pBinding[i].descriptorType;
         k++;
      }

      total += pCreateInfo->pBinding[i].count *
         __builtin_popcount(pCreateInfo->pBinding[i].stageFlags);
   }

   set_layout->total = total;
   set_layout->count = count;

   *pSetLayout = (VkDescriptorSetLayout) set_layout;

   return VK_SUCCESS;
}

VkResult VKAPI vkBeginDescriptorPoolUpdate(
    VkDevice                                    device,
    VkDescriptorUpdateMode                      updateMode)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkEndDescriptorPoolUpdate(
    VkDevice                                    device,
    VkCmdBuffer                                 cmd)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkCreateDescriptorPool(
    VkDevice                                    device,
    VkDescriptorPoolUsage                       poolUsage,
    uint32_t                                    maxSets,
    const VkDescriptorPoolCreateInfo*           pCreateInfo,
    VkDescriptorPool*                           pDescriptorPool)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkResetDescriptorPool(
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool)
{
   return VK_UNSUPPORTED;
}

VkResult VKAPI vkAllocDescriptorSets(
    VkDevice                                    _device,
    VkDescriptorPool                            descriptorPool,
    VkDescriptorSetUsage                        setUsage,
    uint32_t                                    count,
    const VkDescriptorSetLayout*                pSetLayouts,
    VkDescriptorSet*                            pDescriptorSets,
    uint32_t*                                   pCount)
{
   struct anv_device *device = (struct anv_device *) _device;
   const struct anv_descriptor_set_layout *layout;
   struct anv_descriptor_set *set;
   size_t size;

   for (uint32_t i = 0; i < count; i++) {
      layout = (struct anv_descriptor_set_layout *) pSetLayouts[i];
      size = sizeof(*set) + layout->total * sizeof(set->descriptors[0]);
      set = anv_device_alloc(device, size, 8,
                             VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
      if (!set) {
         *pCount = i;
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      pDescriptorSets[i] = (VkDescriptorSet) set;
   }

   *pCount = count;

   return VK_UNSUPPORTED;
}

void VKAPI vkClearDescriptorSets(
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool,
    uint32_t                                    count,
    const VkDescriptorSet*                      pDescriptorSets)
{
}

void VKAPI vkUpdateDescriptors(
    VkDevice                                    _device,
    VkDescriptorSet                             descriptorSet,
    uint32_t                                    updateCount,
    const void**                                ppUpdateArray)
{
   struct anv_descriptor_set *set = (struct anv_descriptor_set *) descriptorSet;
   VkUpdateSamplers *update_samplers;
   VkUpdateSamplerTextures *update_sampler_textures;
   VkUpdateImages *update_images;
   VkUpdateBuffers *update_buffers;
   VkUpdateAsCopy *update_as_copy;

   for (uint32_t i = 0; i < updateCount; i++) {
      const struct anv_common *common = ppUpdateArray[i];

      switch (common->sType) {
      case VK_STRUCTURE_TYPE_UPDATE_SAMPLERS:
         update_samplers = (VkUpdateSamplers *) common;

         for (uint32_t j = 0; j < update_samplers->count; j++) {
            set->descriptors[update_samplers->binding + j] =
               (void *) update_samplers->pSamplers[j];
         }
         break;

      case VK_STRUCTURE_TYPE_UPDATE_SAMPLER_TEXTURES:
         /* FIXME: Shouldn't this be *_UPDATE_SAMPLER_IMAGES? */
         update_sampler_textures = (VkUpdateSamplerTextures *) common;

         for (uint32_t j = 0; j < update_sampler_textures->count; j++) {
            set->descriptors[update_sampler_textures->binding + j] =
               (void *) update_sampler_textures->pSamplerImageViews[j].pImageView->view;
         }
         break;

      case VK_STRUCTURE_TYPE_UPDATE_IMAGES:
         update_images = (VkUpdateImages *) common;

         for (uint32_t j = 0; j < update_images->count; j++) {
            set->descriptors[update_images->binding + j] =
               (void *) update_images->pImageViews[j].view;
         }
         break;

      case VK_STRUCTURE_TYPE_UPDATE_BUFFERS:
         update_buffers = (VkUpdateBuffers *) common;

         for (uint32_t j = 0; j < update_buffers->count; j++) {
            set->descriptors[update_buffers->binding + j] =
               (void *) update_buffers->pBufferViews[j].view;
         }
         /* FIXME: descriptor arrays? */
         break;

      case VK_STRUCTURE_TYPE_UPDATE_AS_COPY:
         update_as_copy = (VkUpdateAsCopy *) common;
         (void) update_as_copy;
         break;

      default:
         break;
      }
   }
}

// State object functions

static inline int64_t
clamp_int64(int64_t x, int64_t min, int64_t max)
{
   if (x < min)
      return min;
   else if (x < max)
      return x;
   else
      return max;
}

VkResult VKAPI vkCreateDynamicViewportState(
    VkDevice                                    _device,
    const VkDynamicVpStateCreateInfo*           pCreateInfo,
    VkDynamicVpState*                           pState)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_dynamic_vp_state *state;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DYNAMIC_VP_STATE_CREATE_INFO);

   state = anv_device_alloc(device, sizeof(*state), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (state == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   unsigned count = pCreateInfo->viewportAndScissorCount;
   state->sf_clip_vp = anv_state_pool_alloc(&device->dyn_state_pool,
                                            count * 64, 64);
   state->cc_vp = anv_state_pool_alloc(&device->dyn_state_pool,
                                       count * 8, 32);
   state->scissor = anv_state_pool_alloc(&device->dyn_state_pool,
                                         count * 32, 32);

   for (uint32_t i = 0; i < pCreateInfo->viewportAndScissorCount; i++) {
      const VkViewport *vp = &pCreateInfo->pViewports[i];
      const VkRect *s = &pCreateInfo->pScissors[i];

      struct GEN8_SF_CLIP_VIEWPORT sf_clip_viewport = {
         .ViewportMatrixElementm00 = vp->width / 2,
         .ViewportMatrixElementm11 = vp->height / 2,
         .ViewportMatrixElementm22 = (vp->maxDepth - vp->minDepth) / 2,
         .ViewportMatrixElementm30 = vp->originX + vp->width / 2,
         .ViewportMatrixElementm31 = vp->originY + vp->height / 2,
         .ViewportMatrixElementm32 = (vp->maxDepth + vp->minDepth) / 2,
         .XMinClipGuardband = -1.0f,
         .XMaxClipGuardband = 1.0f,
         .YMinClipGuardband = -1.0f,
         .YMaxClipGuardband = 1.0f,
         .XMinViewPort = vp->originX,
         .XMaxViewPort = vp->originX + vp->width - 1,
         .YMinViewPort = vp->originY,
         .YMaxViewPort = vp->originY + vp->height - 1,
      };

      struct GEN8_CC_VIEWPORT cc_viewport = {
         .MinimumDepth = vp->minDepth,
         .MaximumDepth = vp->maxDepth
      };

      /* Since xmax and ymax are inclusive, we have to have xmax < xmin or
       * ymax < ymin for empty clips.  In case clip x, y, width height are all
       * 0, the clamps below produce 0 for xmin, ymin, xmax, ymax, which isn't
       * what we want. Just special case empty clips and produce a canonical
       * empty clip. */
      static const struct GEN8_SCISSOR_RECT empty_scissor = {
         .ScissorRectangleYMin = 1,
         .ScissorRectangleXMin = 1,
         .ScissorRectangleYMax = 0,
         .ScissorRectangleXMax = 0
      };

      const int max = 0xffff;
      struct GEN8_SCISSOR_RECT scissor = {
         /* Do this math using int64_t so overflow gets clamped correctly. */
         .ScissorRectangleYMin = clamp_int64(s->offset.y, 0, max),
         .ScissorRectangleXMin = clamp_int64(s->offset.x, 0, max),
         .ScissorRectangleYMax = clamp_int64((uint64_t) s->offset.y + s->extent.height - 1, 0, max),
         .ScissorRectangleXMax = clamp_int64((uint64_t) s->offset.x + s->extent.width - 1, 0, max)
      };

      GEN8_SF_CLIP_VIEWPORT_pack(NULL, state->sf_clip_vp.map + i * 64, &sf_clip_viewport);
      GEN8_CC_VIEWPORT_pack(NULL, state->cc_vp.map + i * 32, &cc_viewport);

      if (s->extent.width <= 0 || s->extent.height <= 0) {
         GEN8_SCISSOR_RECT_pack(NULL, state->scissor.map + i * 32, &empty_scissor);
      } else {
         GEN8_SCISSOR_RECT_pack(NULL, state->scissor.map + i * 32, &scissor);
      }
   }

   *pState = (VkDynamicVpState) state;

   return VK_SUCCESS;
}

VkResult VKAPI vkCreateDynamicRasterState(
    VkDevice                                    _device,
    const VkDynamicRsStateCreateInfo*           pCreateInfo,
    VkDynamicRsState*                           pState)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_dynamic_rs_state *state;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DYNAMIC_RS_STATE_CREATE_INFO);

   state = anv_device_alloc(device, sizeof(*state), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (state == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Missing these:
    * float                                       depthBias;
    * float                                       depthBiasClamp;
    * float                                       slopeScaledDepthBias;
    * float                                       pointFadeThreshold;
    *                            // optional (GL45) - Size of point fade threshold
    */

   struct GEN8_3DSTATE_SF sf = {
      GEN8_3DSTATE_SF_header,
      .LineWidth = pCreateInfo->lineWidth,
      .PointWidth = pCreateInfo->pointSize,
   };

   GEN8_3DSTATE_SF_pack(NULL, state->state_sf, &sf);

   *pState = (VkDynamicRsState) state;

   return VK_SUCCESS;
}

VkResult VKAPI vkCreateDynamicColorBlendState(
    VkDevice                                    _device,
    const VkDynamicCbStateCreateInfo*           pCreateInfo,
    VkDynamicCbState*                           pState)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_dynamic_cb_state *state;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DYNAMIC_CB_STATE_CREATE_INFO);

   state = anv_device_alloc(device, sizeof(*state), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (state == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   *pState = (VkDynamicCbState) state;

   return VK_SUCCESS;
}

VkResult VKAPI vkCreateDynamicDepthStencilState(
    VkDevice                                    device,
    const VkDynamicDsStateCreateInfo*           pCreateInfo,
    VkDynamicDsState*                           pState)
{
   return VK_UNSUPPORTED;
}

// Command buffer functions

VkResult VKAPI vkCreateCommandBuffer(
    VkDevice                                    _device,
    const VkCmdBufferCreateInfo*                pCreateInfo,
    VkCmdBuffer*                                pCmdBuffer)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_cmd_buffer *cmd_buffer;
   VkResult result;

   cmd_buffer = anv_device_alloc(device, sizeof(*cmd_buffer), 8,
                                 VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   cmd_buffer->device = device;
   
   result = anv_batch_init(&cmd_buffer->batch, device);
   if (result != VK_SUCCESS)
      goto fail;

   cmd_buffer->exec2_objects =
      anv_device_alloc(device, 8192 * sizeof(cmd_buffer->exec2_objects[0]), 8,
                       VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (cmd_buffer->exec2_objects == NULL) {
      result = vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_batch;
   }

   cmd_buffer->exec2_bos =
      anv_device_alloc(device, 8192 * sizeof(cmd_buffer->exec2_bos[0]), 8,
                       VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (cmd_buffer->exec2_bos == NULL) {
      result = vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_exec2_objects;
   }

   anv_state_stream_init(&cmd_buffer->surface_state_stream,
                         &device->surface_state_block_pool);

   cmd_buffer->dirty = 0;
   cmd_buffer->vb_dirty = 0;

   *pCmdBuffer = (VkCmdBuffer) cmd_buffer;

   return VK_SUCCESS;

 fail_exec2_objects:
   anv_device_free(device, cmd_buffer->exec2_objects);
 fail_batch:
   anv_batch_finish(&cmd_buffer->batch, device);
 fail:
   anv_device_free(device, cmd_buffer);

   return result;
}

VkResult VKAPI vkBeginCommandBuffer(
    VkCmdBuffer                                 cmdBuffer,
    const VkCmdBufferBeginInfo*                 pBeginInfo)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_device *device = cmd_buffer->device;

   anv_batch_emit(&cmd_buffer->batch, GEN8_PIPELINE_SELECT,
                  .PipelineSelection = _3D);
   anv_batch_emit(&cmd_buffer->batch, GEN8_STATE_SIP);

   anv_batch_emit(&cmd_buffer->batch, GEN8_STATE_BASE_ADDRESS,
                  .GeneralStateBaseAddress = { NULL, 0 },
                  .GeneralStateBaseAddressModifyEnable = true,
                  .GeneralStateBufferSize = 0xfffff,
                  .GeneralStateBufferSizeModifyEnable = true,

                  .SurfaceStateBaseAddress = { &device->surface_state_block_pool.bo, 0 },
                  .SurfaceStateMemoryObjectControlState = 0, /* FIXME: MOCS */
                  .SurfaceStateBaseAddressModifyEnable = true,

                  .DynamicStateBaseAddress = { &device->dyn_state_block_pool.bo, 0 },
                  .DynamicStateBaseAddressModifyEnable = true,
                  .DynamicStateBufferSize = 0xfffff,
                  .DynamicStateBufferSizeModifyEnable = true,

                  .IndirectObjectBaseAddress = { NULL, 0 },
                  .IndirectObjectBaseAddressModifyEnable = true,
                  .IndirectObjectBufferSize = 0xfffff,
                  .IndirectObjectBufferSizeModifyEnable = true,
                  
                  .InstructionBaseAddress = { &device->instruction_block_pool.bo, 0 },
                  .InstructionBaseAddressModifyEnable = true,
                  .InstructionBufferSize = 0xfffff,
                  .InstructionBuffersizeModifyEnable = true);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_VF_STATISTICS,
                   .StatisticsEnable = true);
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_HS, .Enable = false);
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_TE, .TEEnable = false);
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_DS, .FunctionEnable = false);
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_STREAMOUT, .SOFunctionEnable = false);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_VS,
                  .ConstantBufferOffset = 0,
                  .ConstantBufferSize = 4);
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_GS,
                  .ConstantBufferOffset = 4,
                  .ConstantBufferSize = 4);
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_PS,
                  .ConstantBufferOffset = 8,
                  .ConstantBufferSize = 4);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_CLIP,
                  .ClipEnable = true,
                  .ViewportXYClipTestEnable = true);
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_WM_CHROMAKEY,
                  .ChromaKeyKillEnable = false);
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_SBE_SWIZ);
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_AA_LINE_PARAMETERS);

   /* Hardcoded state: */
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_DEPTH_BUFFER,
                  .SurfaceType = SURFTYPE_2D,
                  .Width = 1,
                  .Height = 1,
                  .SurfaceFormat = D16_UNORM,
                  .SurfaceBaseAddress = { NULL, 0 },
                  .HierarchicalDepthBufferEnable = 0);
   
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_WM_DEPTH_STENCIL,
                  .DepthTestEnable = false,
                  .DepthBufferWriteEnable = false);

   return VK_SUCCESS;
}

static void
anv_cmd_buffer_add_bo(struct anv_cmd_buffer *cmd_buffer,
                      struct anv_bo *bo, struct anv_reloc_list *list)
{
   struct drm_i915_gem_exec_object2 *obj;

   bo->index = cmd_buffer->bo_count;
   obj = &cmd_buffer->exec2_objects[bo->index];
   cmd_buffer->exec2_bos[bo->index] = bo;
   cmd_buffer->bo_count++;

   obj->handle = bo->gem_handle;
   obj->relocation_count = 0;
   obj->relocs_ptr = 0;
   obj->alignment = 0;
   obj->offset = bo->offset;
   obj->flags = 0;
   obj->rsvd1 = 0;
   obj->rsvd2 = 0;

   if (list) {
      obj->relocation_count = list->num_relocs;
      obj->relocs_ptr = (uintptr_t) list->relocs;
   }
}

static void
anv_cmd_buffer_add_validate_bos(struct anv_cmd_buffer *cmd_buffer,
                                struct anv_reloc_list *list)
{
   struct anv_bo *bo, *batch_bo;

   batch_bo = &cmd_buffer->batch.bo;
   for (size_t i = 0; i < list->num_relocs; i++) {
      bo = list->reloc_bos[i];
      /* Skip any relocations targeting the batch bo. We need to make sure
       * it's the last in the list so we'll add it manually later.
       */
      if (bo == batch_bo)
         continue;
      if (bo->index < cmd_buffer->bo_count && cmd_buffer->exec2_bos[bo->index] == bo)
         continue;

      anv_cmd_buffer_add_bo(cmd_buffer, bo, NULL);
   }
}

static void
anv_cmd_buffer_process_relocs(struct anv_cmd_buffer *cmd_buffer,
                              struct anv_reloc_list *list)
{
   struct anv_bo *bo;

   /* If the kernel supports I915_EXEC_NO_RELOC, it will compare offset in
    * struct drm_i915_gem_exec_object2 against the bos current offset and if
    * all bos haven't moved it will skip relocation processing alltogether.
    * If I915_EXEC_NO_RELOC is not supported, the kernel ignores the incoming
    * value of offset so we can set it either way.  For that to work we need
    * to make sure all relocs use the same presumed offset.
    */

   for (size_t i = 0; i < list->num_relocs; i++) {
      bo = list->reloc_bos[i];
      if (bo->offset != list->relocs[i].presumed_offset)
         cmd_buffer->need_reloc = true;

      list->relocs[i].target_handle = bo->index;
   }
}

VkResult VKAPI vkEndCommandBuffer(
    VkCmdBuffer                                 cmdBuffer)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_device *device = cmd_buffer->device;
   struct anv_batch *batch = &cmd_buffer->batch;

   anv_batch_emit(batch, GEN8_MI_BATCH_BUFFER_END);

   /* Round batch up to an even number of dwords. */
   if ((batch->next - batch->bo.map) & 4)
      anv_batch_emit(batch, GEN8_MI_NOOP);

   cmd_buffer->bo_count = 0;
   cmd_buffer->need_reloc = false;

   /* Lock for access to bo->index. */
   pthread_mutex_lock(&device->mutex);

   /* Add block pool bos first so we can add them with their relocs. */
   anv_cmd_buffer_add_bo(cmd_buffer, &device->surface_state_block_pool.bo,
                         &batch->surf_relocs);

   anv_cmd_buffer_add_validate_bos(cmd_buffer, &batch->surf_relocs);
   anv_cmd_buffer_add_validate_bos(cmd_buffer, &batch->cmd_relocs);
   anv_cmd_buffer_add_bo(cmd_buffer, &batch->bo, &batch->cmd_relocs);
   anv_cmd_buffer_process_relocs(cmd_buffer, &batch->surf_relocs);
   anv_cmd_buffer_process_relocs(cmd_buffer, &batch->cmd_relocs);

   cmd_buffer->execbuf.buffers_ptr = (uintptr_t) cmd_buffer->exec2_objects;
   cmd_buffer->execbuf.buffer_count = cmd_buffer->bo_count;
   cmd_buffer->execbuf.batch_start_offset = 0;
   cmd_buffer->execbuf.batch_len = batch->next - batch->bo.map;
   cmd_buffer->execbuf.cliprects_ptr = 0;
   cmd_buffer->execbuf.num_cliprects = 0;
   cmd_buffer->execbuf.DR1 = 0;
   cmd_buffer->execbuf.DR4 = 0;

   cmd_buffer->execbuf.flags = I915_EXEC_HANDLE_LUT;
   if (!cmd_buffer->need_reloc)
      cmd_buffer->execbuf.flags |= I915_EXEC_NO_RELOC;
   cmd_buffer->execbuf.flags |= I915_EXEC_RENDER;
   cmd_buffer->execbuf.rsvd1 = device->context_id;
   cmd_buffer->execbuf.rsvd2 = 0;

   pthread_mutex_unlock(&device->mutex);

   return VK_SUCCESS;
}

VkResult VKAPI vkResetCommandBuffer(
    VkCmdBuffer                                 cmdBuffer)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;

   anv_batch_reset(&cmd_buffer->batch);

   return VK_SUCCESS;
}

// Command buffer building functions

void VKAPI vkCmdBindPipeline(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    VkPipeline                                  _pipeline)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;

   cmd_buffer->pipeline = (struct anv_pipeline *) _pipeline;
   cmd_buffer->dirty |= ANV_CMD_BUFFER_PIPELINE_DIRTY;
}

void VKAPI vkCmdBindDynamicStateObject(
    VkCmdBuffer                                 cmdBuffer,
    VkStateBindPoint                            stateBindPoint,
    VkDynamicStateObject                        dynamicState)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_dynamic_vp_state *vp_state;

   switch (stateBindPoint) {
   case VK_STATE_BIND_POINT_VIEWPORT:
      vp_state = (struct anv_dynamic_vp_state *) dynamicState;

      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_SCISSOR_STATE_POINTERS,
                     .ScissorRectPointer = vp_state->scissor.offset);
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_CC,
                     .CCViewportPointer = vp_state->cc_vp.offset);
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP,
                     .SFClipViewportPointer = vp_state->sf_clip_vp.offset);
      break;
   case VK_STATE_BIND_POINT_RASTER:
      cmd_buffer->rs_state = (struct anv_dynamic_rs_state *) dynamicState;
      cmd_buffer->dirty |= ANV_CMD_BUFFER_RS_DIRTY;
      break;
   case VK_STATE_BIND_POINT_COLOR_BLEND:
   case VK_STATE_BIND_POINT_DEPTH_STENCIL:
      break;
   default:
      break;
   };
}

void VKAPI vkCmdBindDescriptorSets(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    uint32_t                                    firstSet,
    uint32_t                                    setCount,
    const VkDescriptorSet*                      pDescriptorSets,
    uint32_t                                    dynamicOffsetCount,
    const uint32_t*                             pDynamicOffsets)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;

   /* What are the semantics for setting descriptor sets? Assuming that
    * setting preserves lower sets and invalidate higher sets. This means that
    * we can set the number of active sets to firstSet + setCount.
    */

   for (uint32_t i = 0; i < setCount; i++)
      cmd_buffer->descriptor_sets[firstSet + i] =
         (struct anv_descriptor_set *) pDescriptorSets[i];

   cmd_buffer->num_descriptor_sets = firstSet + setCount;
   cmd_buffer->dirty |= ANV_CMD_BUFFER_DESCRIPTOR_SET_DIRTY;
}

void VKAPI vkCmdBindIndexBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    VkIndexType                                 indexType)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_buffer *buffer = (struct anv_buffer *) _buffer;

   static const uint32_t vk_to_gen_index_type[] = {
      [VK_INDEX_TYPE_UINT8] = INDEX_BYTE,
      [VK_INDEX_TYPE_UINT16] = INDEX_WORD,
      [VK_INDEX_TYPE_UINT32] = INDEX_DWORD,
   };

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_INDEX_BUFFER,
                  .IndexFormat = vk_to_gen_index_type[indexType],
                  .MemoryObjectControlState = 0,
                  .BufferStartingAddress = { &buffer->mem->bo, buffer->offset + offset },
                  .BufferSize = buffer->size - offset);
}

void VKAPI vkCmdBindVertexBuffers(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    startBinding,
    uint32_t                                    bindingCount,
    const VkBuffer*                             pBuffers,
    const VkDeviceSize*                         pOffsets)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;

   /* We have to defer setting up vertex buffer since we need the buffer
    * stride from the pipeline. */

   for (uint32_t i = 0; i < bindingCount; i++) {
      cmd_buffer->vb[startBinding + i].buffer = (struct anv_buffer *) pBuffers[i];
      cmd_buffer->vb[startBinding + i].offset = pOffsets[i];
      cmd_buffer->vb_dirty |= 1 << (startBinding + i);
   }
}

static void
flush_descriptor_sets(struct anv_cmd_buffer *cmd_buffer)
{
   static const uint32_t opcodes[] = {
      [VK_SHADER_STAGE_VERTEX] = 38,
      [VK_SHADER_STAGE_TESS_CONTROL] = 39,
      [VK_SHADER_STAGE_TESS_EVALUATION] = 40,
      [VK_SHADER_STAGE_GEOMETRY] = 41,
      [VK_SHADER_STAGE_FRAGMENT] = 42,
      [VK_SHADER_STAGE_COMPUTE] = 0,
   };

   struct anv_pipeline_layout *layout = cmd_buffer->pipeline->layout;
   struct anv_framebuffer *framebuffer = cmd_buffer->framebuffer;

   for (uint32_t s = 0; s < VK_NUM_SHADER_STAGE; s++) {

      uint32_t bias = s == VK_SHADER_STAGE_FRAGMENT ? MAX_RTS : 0;
      uint32_t count, *table;
      struct anv_state table_state;

      if (layout)
         count = layout->stage[s].count + bias;
      else if (s == VK_SHADER_STAGE_FRAGMENT)
         count = framebuffer->color_attachment_count;
      else
         count = 0;
      
      if (count == 0)
         continue;

      table_state = anv_state_stream_alloc(&cmd_buffer->surface_state_stream,
                                           count * 4, 32);
      table = table_state.map;

      if (s == VK_SHADER_STAGE_FRAGMENT) {
         for (uint32_t i = 0; i < framebuffer->color_attachment_count; i++) {
            struct anv_color_attachment_view *view = framebuffer->color_attachments[i];
            table[i] = view->surface_state.offset;

            /* Don't write the reloc back to the surface state. We do that at
             * submit time. Surface address is dwords 8-9. */
            anv_reloc_list_add(&cmd_buffer->batch.surf_relocs,
                               view->surface_state.offset + 8 * sizeof(int32_t),
                               &view->image->mem->bo, view->image->offset);
         }
      }

      if (layout) {
         for (uint32_t i = 0; i < layout->stage[s].count; i++) {
            struct anv_pipeline_layout_entry *e = &layout->stage[s].entries[i];
            struct anv_image_view *image_view;
            struct anv_buffer_view *buffer_view;
            void *d = cmd_buffer->descriptor_sets[e->set]->descriptors[e->index];

            switch (e->type) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
               break;
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
               image_view = d;
               table[bias + i] = image_view->surface_state.offset;
               anv_reloc_list_add(&cmd_buffer->batch.surf_relocs,
                                  image_view->surface_state.offset + 8 * sizeof(int32_t),
                                  &image_view->image->mem->bo,
                                  image_view->image->offset);
               break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
               /* FIXME: What are these? TBOs? */
               break;

            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
               buffer_view = d;
               table[bias + i] = buffer_view->surface_state.offset;
               anv_reloc_list_add(&cmd_buffer->batch.surf_relocs,
                                  buffer_view->surface_state.offset + 8 * sizeof(int32_t),
                                  &buffer_view->buffer->mem->bo,
                                  buffer_view->buffer->offset + buffer_view->offset);
               break;

            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
               break;
            default:
               break;
            }
         }
      }

      /* FIXME: Samplers */

      /* The binding table pointer commands all have the same structure, only
       * the opcode differs.
       */
      anv_batch_emit(&cmd_buffer->batch,
                     GEN8_3DSTATE_BINDING_TABLE_POINTERS_VS,
                     ._3DCommandSubOpcode  = opcodes[s],
                     .PointertoVSBindingTable = table_state.offset);
   }
}

static void
anv_cmd_buffer_flush_state(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->pipeline;
   const uint32_t num_buffers = __builtin_popcount(cmd_buffer->vb_dirty);
   const uint32_t num_dwords = 1 + num_buffers * 4;
   uint32_t *p;

   if (cmd_buffer->vb_dirty) {
      p = anv_batch_emitn(&cmd_buffer->batch, num_dwords,
                          GEN8_3DSTATE_VERTEX_BUFFERS);
      uint32_t vb, i = 0;
      for_each_bit(vb, cmd_buffer->vb_dirty) {
         struct anv_buffer *buffer = cmd_buffer->vb[vb].buffer;
         uint32_t offset = cmd_buffer->vb[vb].offset;
      
         struct GEN8_VERTEX_BUFFER_STATE state = {
            .VertexBufferIndex = vb,
            .MemoryObjectControlState = 0,
            .AddressModifyEnable = true,
            .BufferPitch = pipeline->binding_stride[vb],
            .BufferStartingAddress = { &buffer->mem->bo, buffer->offset + offset },
            .BufferSize = buffer->size - offset
         };

         GEN8_VERTEX_BUFFER_STATE_pack(&cmd_buffer->batch, &p[1 + i * 4], &state);
         i++;
      }
   }

   if (cmd_buffer->dirty & ANV_CMD_BUFFER_PIPELINE_DIRTY)
      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);

   if (cmd_buffer->dirty & ANV_CMD_BUFFER_DESCRIPTOR_SET_DIRTY)
      flush_descriptor_sets(cmd_buffer);

   if (cmd_buffer->dirty & (ANV_CMD_BUFFER_PIPELINE_DIRTY | ANV_CMD_BUFFER_RS_DIRTY)) {
      /* maybe: anv_batch_merge(batch, GEN8_3DSTATE_SF, a, b) */
      uint32_t *dw;

      dw = anv_batch_emit_dwords(&cmd_buffer->batch, GEN8_3DSTATE_SF_length);
      for (uint32_t i = 0; i < GEN8_3DSTATE_SF_length; i++)
         dw[i] = cmd_buffer->rs_state->state_sf[i] | pipeline->state_sf[i];
   }

   cmd_buffer->vb_dirty = 0;
   cmd_buffer->dirty = 0;
}

void VKAPI vkCmdDraw(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    firstVertex,
    uint32_t                                    vertexCount,
    uint32_t                                    firstInstance,
    uint32_t                                    instanceCount)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;

   anv_cmd_buffer_flush_state(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DPRIMITIVE,
                  .VertexAccessType = SEQUENTIAL,
                  .VertexCountPerInstance = vertexCount,
                  .StartVertexLocation = firstVertex,
                  .InstanceCount = instanceCount,
                  .StartInstanceLocation = firstInstance,
                  .BaseVertexLocation = 0);
}

void VKAPI vkCmdDrawIndexed(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    firstIndex,
    uint32_t                                    indexCount,
    int32_t                                     vertexOffset,
    uint32_t                                    firstInstance,
    uint32_t                                    instanceCount)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;

   anv_cmd_buffer_flush_state(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DPRIMITIVE,
                  .VertexAccessType = RANDOM,
                  .VertexCountPerInstance = indexCount,
                  .StartVertexLocation = firstIndex,
                  .InstanceCount = instanceCount,
                  .StartInstanceLocation = firstInstance,
                  .BaseVertexLocation = 0);
}

static void
anv_batch_lrm(struct anv_batch *batch,
              uint32_t reg, struct anv_bo *bo, uint32_t offset)
{
   anv_batch_emit(batch, GEN8_MI_LOAD_REGISTER_MEM,
                  .RegisterAddress = reg,
                  .MemoryAddress = { bo, offset });
}

static void
anv_batch_lri(struct anv_batch *batch, uint32_t reg, uint32_t imm)
{
   anv_batch_emit(batch, GEN8_MI_LOAD_REGISTER_IMM,
                  .RegisterOffset = reg,
                  .DataDWord = imm);
}

/* Auto-Draw / Indirect Registers */
#define GEN7_3DPRIM_END_OFFSET          0x2420
#define GEN7_3DPRIM_START_VERTEX        0x2430
#define GEN7_3DPRIM_VERTEX_COUNT        0x2434
#define GEN7_3DPRIM_INSTANCE_COUNT      0x2438
#define GEN7_3DPRIM_START_INSTANCE      0x243C
#define GEN7_3DPRIM_BASE_VERTEX         0x2440

void VKAPI vkCmdDrawIndirect(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    count,
    uint32_t                                    stride)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_buffer *buffer = (struct anv_buffer *) _buffer;
   struct anv_bo *bo = &buffer->mem->bo;
   uint32_t bo_offset = buffer->offset + offset;

   anv_cmd_buffer_flush_state(cmd_buffer);

   anv_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_VERTEX_COUNT, bo, bo_offset);
   anv_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_INSTANCE_COUNT, bo, bo_offset + 4);
   anv_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_VERTEX, bo, bo_offset + 8);
   anv_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_INSTANCE, bo, bo_offset + 12);
   anv_batch_lri(&cmd_buffer->batch, GEN7_3DPRIM_BASE_VERTEX, 0);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DPRIMITIVE,
                  .IndirectParameterEnable = true,
                  .VertexAccessType = SEQUENTIAL);
}

void VKAPI vkCmdDrawIndexedIndirect(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    count,
    uint32_t                                    stride)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_buffer *buffer = (struct anv_buffer *) _buffer;
   struct anv_bo *bo = &buffer->mem->bo;
   uint32_t bo_offset = buffer->offset + offset;

   anv_cmd_buffer_flush_state(cmd_buffer);

   anv_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_VERTEX_COUNT, bo, bo_offset);
   anv_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_INSTANCE_COUNT, bo, bo_offset + 4);
   anv_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_VERTEX, bo, bo_offset + 8);
   anv_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_BASE_VERTEX, bo, bo_offset + 12);
   anv_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_INSTANCE, bo, bo_offset + 16);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DPRIMITIVE,
                  .IndirectParameterEnable = true,
                  .VertexAccessType = RANDOM);
}

void VKAPI vkCmdDispatch(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    x,
    uint32_t                                    y,
    uint32_t                                    z)
{
}

void VKAPI vkCmdDispatchIndirect(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    buffer,
    VkDeviceSize                                offset)
{
}

void VKAPI vkCmdSetEvent(
    VkCmdBuffer                                 cmdBuffer,
    VkEvent                                     event,
    VkPipeEvent                                 pipeEvent)
{
}

void VKAPI vkCmdResetEvent(
    VkCmdBuffer                                 cmdBuffer,
    VkEvent                                     event,
    VkPipeEvent                                 pipeEvent)
{
}

void VKAPI vkCmdWaitEvents(
    VkCmdBuffer                                 cmdBuffer,
    VkWaitEvent                                 waitEvent,
    uint32_t                                    eventCount,
    const VkEvent*                              pEvents,
    uint32_t                                    memBarrierCount,
    const void**                                ppMemBarriers)
{
}

void VKAPI vkCmdPipelineBarrier(
    VkCmdBuffer                                 cmdBuffer,
    VkWaitEvent                                 waitEvent,
    uint32_t                                    pipeEventCount,
    const VkPipeEvent*                          pPipeEvents,
    uint32_t                                    memBarrierCount,
    const void**                                ppMemBarriers)
{
}

static void
anv_batch_emit_ps_depth_count(struct anv_batch *batch,
                              struct anv_bo *bo, uint32_t offset)
{
   anv_batch_emit(batch, GEN8_PIPE_CONTROL,
                  .DestinationAddressType = DAT_PPGTT,
                  .PostSyncOperation = WritePSDepthCount,
                  .Address = { bo, offset });  /* FIXME: This is only lower 32 bits */
}

void VKAPI vkCmdBeginQuery(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    slot,
    VkQueryControlFlags                         flags)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_query_pool *pool = (struct anv_query_pool *) queryPool;

   switch (pool->type) {
   case VK_QUERY_TYPE_OCCLUSION:
      anv_batch_emit_ps_depth_count(&cmd_buffer->batch, &pool->bo, slot * 16);
      break;

   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      break;

   default:
      break;
   }
}

void VKAPI vkCmdEndQuery(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    slot)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_query_pool *pool = (struct anv_query_pool *) queryPool;

   switch (pool->type) {
   case VK_QUERY_TYPE_OCCLUSION:
      anv_batch_emit_ps_depth_count(&cmd_buffer->batch, &pool->bo, slot * 16 + 8);
      break;

   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      break;

   default:
      break;
   }
}

void VKAPI vkCmdResetQueryPool(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount)
{
}

#define TIMESTAMP 0x44070

void VKAPI vkCmdWriteTimestamp(
    VkCmdBuffer                                 cmdBuffer,
    VkTimestampType                             timestampType,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_buffer *buffer = (struct anv_buffer *) destBuffer;
   struct anv_bo *bo = &buffer->mem->bo;

   switch (timestampType) {
   case VK_TIMESTAMP_TYPE_TOP:
      anv_batch_emit(&cmd_buffer->batch, GEN8_MI_STORE_REGISTER_MEM,
                     .RegisterAddress = TIMESTAMP,
                     .MemoryAddress = { bo, buffer->offset + destOffset });
      break;

   case VK_TIMESTAMP_TYPE_BOTTOM:
      anv_batch_emit(&cmd_buffer->batch, GEN8_PIPE_CONTROL,
                     .DestinationAddressType = DAT_PPGTT,
                     .PostSyncOperation = WriteTimestamp,
                     .Address = /* FIXME: This is only lower 32 bits */
                        { bo, buffer->offset + destOffset });
      break;

   default:
      break;
   }
}

void VKAPI vkCmdCopyQueryPoolResults(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                destStride,
    VkQueryResultFlags                          flags)
{
}

void VKAPI vkCmdInitAtomicCounters(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    uint32_t                                    startCounter,
    uint32_t                                    counterCount,
    const uint32_t*                             pData)
{
}

void VKAPI vkCmdLoadAtomicCounters(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    uint32_t                                    startCounter,
    uint32_t                                    counterCount,
    VkBuffer                                    srcBuffer,
    VkDeviceSize                                srcOffset)
{
}

void VKAPI vkCmdSaveAtomicCounters(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    uint32_t                                    startCounter,
    uint32_t                                    counterCount,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset)
{
}

VkResult VKAPI vkCreateFramebuffer(
    VkDevice                                    _device,
    const VkFramebufferCreateInfo*              pCreateInfo,
    VkFramebuffer*                              pFramebuffer)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_framebuffer *framebuffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);

   framebuffer = anv_device_alloc(device, sizeof(*framebuffer), 8,
                                  VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (framebuffer == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   framebuffer->color_attachment_count = pCreateInfo->colorAttachmentCount;
   for (uint32_t i = 0; i < pCreateInfo->colorAttachmentCount; i++) {
      framebuffer->color_attachments[i] =
         (struct anv_color_attachment_view *) pCreateInfo->pColorAttachments[i].view;
   }

   if (pCreateInfo->pDepthStencilAttachment) {
      framebuffer->depth_stencil =
         (struct anv_depth_stencil_view *) pCreateInfo->pDepthStencilAttachment->view;
   }

   framebuffer->sample_count = pCreateInfo->sampleCount;
   framebuffer->width = pCreateInfo->width;
   framebuffer->height = pCreateInfo->height;
   framebuffer->layers = pCreateInfo->layers;

   *pFramebuffer = (VkFramebuffer) framebuffer;

   return VK_SUCCESS;
}

VkResult VKAPI vkCreateRenderPass(
    VkDevice                                    _device,
    const VkRenderPassCreateInfo*               pCreateInfo,
    VkRenderPass*                               pRenderPass)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_render_pass *pass;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);

   pass = anv_device_alloc(device, sizeof(*pass), 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pass == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pass->render_area = pCreateInfo->renderArea;

   *pRenderPass = (VkRenderPass) pass;

   return VK_SUCCESS;
}

void VKAPI vkCmdBeginRenderPass(
    VkCmdBuffer                                 cmdBuffer,
    const VkRenderPassBegin*                    pRenderPassBegin)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_render_pass *pass = (struct anv_render_pass *) pRenderPassBegin->renderPass;

   cmd_buffer->framebuffer = (struct anv_framebuffer *) pRenderPassBegin->framebuffer;
   cmd_buffer->dirty |= ANV_CMD_BUFFER_DESCRIPTOR_SET_DIRTY;

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_DRAWING_RECTANGLE,
                  .ClippedDrawingRectangleYMin = pass->render_area.offset.y,
                  .ClippedDrawingRectangleXMin = pass->render_area.offset.x,
                  .ClippedDrawingRectangleYMax =
                     pass->render_area.offset.y + pass->render_area.extent.height - 1,
                  .ClippedDrawingRectangleXMax =
                     pass->render_area.offset.x + pass->render_area.extent.width - 1,
                  .DrawingRectangleOriginY = 0,
                  .DrawingRectangleOriginX = 0);
}

void VKAPI vkCmdEndRenderPass(
    VkCmdBuffer                                 cmdBuffer,
    VkRenderPass                                renderPass)
{
}
