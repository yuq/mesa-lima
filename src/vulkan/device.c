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

VkResult anv_CreateInstance(
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

   if (result != VK_SUCCESS)
      return result;

   instance->physicalDeviceCount++;
   *pInstance = (VkInstance) instance;

   return VK_SUCCESS;
}

VkResult anv_DestroyInstance(
    VkInstance                                  _instance)
{
   struct anv_instance *instance = (struct anv_instance *) _instance;

   instance->pfnFree(instance->pAllocUserData, instance);

   return VK_SUCCESS;
}

VkResult anv_EnumeratePhysicalDevices(
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

VkResult anv_GetPhysicalDeviceInfo(
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
   VkDisplayPropertiesWSI *display_properties;
   uint64_t ns_per_tick = 80;
   
   switch ((uint32_t) infoType) {
   case VK_PHYSICAL_DEVICE_INFO_TYPE_PROPERTIES:
      properties = pData;

      *pDataSize = sizeof(*properties);
      if (pData == NULL)
         return VK_SUCCESS;

      properties->apiVersion = 1;
      properties->driverVersion = 1;
      properties->vendorId = 0x8086;
      properties->deviceId = device->chipset_id;
      properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
      strcpy(properties->deviceName, device->name);
      properties->maxInlineMemoryUpdateSize = 0;
      properties->maxBoundDescriptorSets = MAX_SETS;
      properties->maxThreadGroupSize = 512;
      properties->timestampFrequency = 1000 * 1000 * 1000 / ns_per_tick;
      properties->multiColorAttachmentClears = true;
      properties->maxDescriptorSets = 8;
      properties->maxViewports = 16;
      properties->maxColorAttachments = 8;
      return VK_SUCCESS;

   case VK_PHYSICAL_DEVICE_INFO_TYPE_PERFORMANCE:
      performance = pData;

      *pDataSize = sizeof(*performance);
      if (pData == NULL)
         return VK_SUCCESS;

      performance->maxDeviceClock = 1.0;
      performance->aluPerClock = 1.0;
      performance->texPerClock = 1.0;
      performance->primsPerClock = 1.0;
      performance->pixelsPerClock = 1.0;
      return VK_SUCCESS;
      
   case VK_PHYSICAL_DEVICE_INFO_TYPE_QUEUE_PROPERTIES:
      queue_properties = pData;

      *pDataSize = sizeof(*queue_properties);
      if (pData == NULL)
         return VK_SUCCESS;

      queue_properties->queueFlags = 0;
      queue_properties->queueCount = 1;
      queue_properties->supportsTimestamps = true;
      return VK_SUCCESS;

   case VK_PHYSICAL_DEVICE_INFO_TYPE_MEMORY_PROPERTIES:
      memory_properties = pData;

      *pDataSize = sizeof(*memory_properties);
      if (pData == NULL)
         return VK_SUCCESS;

      memory_properties->supportsMigration = false;
      memory_properties->supportsPinning = false;
      return VK_SUCCESS;

   case VK_PHYSICAL_DEVICE_INFO_TYPE_DISPLAY_PROPERTIES_WSI:
      anv_finishme("VK_PHYSICAL_DEVICE_INFO_TYPE_DISPLAY_PROPERTIES_WSI");

      *pDataSize = sizeof(*display_properties);
      if (pData == NULL)
         return VK_SUCCESS;

      display_properties = pData;
      display_properties->display = 0;
      display_properties->physicalResolution = (VkExtent2D) { 0, 0 };
      return VK_SUCCESS;

   case VK_PHYSICAL_DEVICE_INFO_TYPE_QUEUE_PRESENT_PROPERTIES_WSI:
      anv_finishme("VK_PHYSICAL_DEVICE_INFO_TYPE_QUEUE_PRESENT_PROPERTIES_WSI");
      return VK_SUCCESS;


   default:
      return VK_UNSUPPORTED;
   }

}

void * vkGetProcAddr(
    VkPhysicalDevice                            physicalDevice,
    const char*                                 pName)
{
   return anv_lookup_entrypoint(pName);
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

static VkResult
anv_queue_init(struct anv_device *device, struct anv_queue *queue)
{
   queue->device = device;
   queue->pool = &device->surface_state_pool;

   queue->completed_serial = anv_state_pool_alloc(queue->pool, 4, 4);
   if (queue->completed_serial.map == NULL)
      return vk_error(VK_ERROR_OUT_OF_DEVICE_MEMORY);

   *(uint32_t *)queue->completed_serial.map = 0;
   queue->next_serial = 1;

   return VK_SUCCESS;
}

static void
anv_queue_finish(struct anv_queue *queue)
{
#ifdef HAVE_VALGRIND
   /* This gets torn down with the device so we only need to do this if
    * valgrind is present.
    */
   anv_state_pool_free(queue->pool, queue->completed_serial);
#endif
}

static void
anv_device_init_border_colors(struct anv_device *device)
{
   float float_border_colors[][4] = {
      [VK_BORDER_COLOR_OPAQUE_WHITE]            = { 1.0, 1.0, 1.0, 1.0 },
      [VK_BORDER_COLOR_TRANSPARENT_BLACK]       = { 0.0, 0.0, 0.0, 0.0 },
      [VK_BORDER_COLOR_OPAQUE_BLACK]            = { 0.0, 0.0, 0.0, 1.0 }
   };

   uint32_t uint32_border_colors[][4] = {
      [VK_BORDER_COLOR_OPAQUE_WHITE]            = { 1, 1, 1, 1 },
      [VK_BORDER_COLOR_TRANSPARENT_BLACK]       = { 0, 0, 0, 0 },
      [VK_BORDER_COLOR_OPAQUE_BLACK]            = { 0, 0, 0, 1 }
   };

   device->float_border_colors =
      anv_state_pool_alloc(&device->dynamic_state_pool,
                           sizeof(float_border_colors), 32);
   memcpy(device->float_border_colors.map,
          float_border_colors, sizeof(float_border_colors));

   device->uint32_border_colors =
      anv_state_pool_alloc(&device->dynamic_state_pool,
                           sizeof(uint32_border_colors), 32);
   memcpy(device->uint32_border_colors.map,
          uint32_border_colors, sizeof(uint32_border_colors));

}

static const uint32_t BATCH_SIZE = 8192;

VkResult anv_CreateDevice(
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

   anv_bo_pool_init(&device->batch_bo_pool, device, BATCH_SIZE);

   anv_block_pool_init(&device->dynamic_state_block_pool, device, 2048);

   anv_state_pool_init(&device->dynamic_state_pool,
                       &device->dynamic_state_block_pool);

   anv_block_pool_init(&device->instruction_block_pool, device, 2048);
   anv_block_pool_init(&device->surface_state_block_pool, device, 2048);

   anv_state_pool_init(&device->surface_state_pool,
                       &device->surface_state_block_pool);

   anv_block_pool_init(&device->scratch_block_pool, device, 0x10000);

   device->info = *physicalDevice->info;

   device->compiler = anv_compiler_create(device);
   device->aub_writer = NULL;

   pthread_mutex_init(&device->mutex, NULL);

   anv_queue_init(device, &device->queue);

   anv_device_init_meta(device);

   anv_device_init_border_colors(device);

   *pDevice = (VkDevice) device;

   return VK_SUCCESS;

 fail_fd:
   close(device->fd);
 fail_device:
   anv_device_free(device, device);

   return vk_error(VK_ERROR_UNAVAILABLE);
}

VkResult anv_DestroyDevice(
    VkDevice                                    _device)
{
   struct anv_device *device = (struct anv_device *) _device;

   anv_compiler_destroy(device->compiler);

   anv_queue_finish(&device->queue);

   anv_device_finish_meta(device);

#ifdef HAVE_VALGRIND
   /* We only need to free these to prevent valgrind errors.  The backing
    * BO will go away in a couple of lines so we don't actually leak.
    */
   anv_state_pool_free(&device->dynamic_state_pool,
                       device->float_border_colors);
   anv_state_pool_free(&device->dynamic_state_pool,
                       device->uint32_border_colors);
#endif

   anv_bo_pool_finish(&device->batch_bo_pool);
   anv_block_pool_finish(&device->dynamic_state_block_pool);
   anv_block_pool_finish(&device->instruction_block_pool);
   anv_block_pool_finish(&device->surface_state_block_pool);

   close(device->fd);

   if (device->aub_writer)
      anv_aub_writer_destroy(device->aub_writer);

   anv_device_free(device, device);

   return VK_SUCCESS;
}

VkResult anv_GetGlobalExtensionInfo(
    VkExtensionInfoType                         infoType,
    uint32_t                                    extensionIndex,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   static const VkExtensionProperties extensions[] = {
      {
         .extName = "VK_WSI_LunarG",
         .version = 3
      }
   };
   uint32_t count = ARRAY_SIZE(extensions);

   switch (infoType) {
   case VK_EXTENSION_INFO_TYPE_COUNT:
      memcpy(pData, &count, sizeof(count));
      *pDataSize = sizeof(count);
      return VK_SUCCESS;

   case VK_EXTENSION_INFO_TYPE_PROPERTIES:
      if (extensionIndex >= count)
         return vk_error(VK_ERROR_INVALID_EXTENSION);

      memcpy(pData, &extensions[extensionIndex], sizeof(extensions[0]));
      *pDataSize = sizeof(extensions[0]);
      return VK_SUCCESS;

   default:
      return VK_UNSUPPORTED;
   }
}

VkResult anv_GetPhysicalDeviceExtensionInfo(
    VkPhysicalDevice                            physicalDevice,
    VkExtensionInfoType                         infoType,
    uint32_t                                    extensionIndex,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   uint32_t *count;

   switch (infoType) {
   case VK_EXTENSION_INFO_TYPE_COUNT:
      *pDataSize = 4;
      if (pData == NULL)
         return VK_SUCCESS;

      count = pData;
      *count = 0;
      return VK_SUCCESS;
      
   case VK_EXTENSION_INFO_TYPE_PROPERTIES:
      return vk_error(VK_ERROR_INVALID_EXTENSION);
      
   default:
      return VK_UNSUPPORTED;
   }
}

VkResult anv_EnumerateLayers(
    VkPhysicalDevice                            physicalDevice,
    size_t                                      maxStringSize,
    size_t*                                     pLayerCount,
    char* const*                                pOutLayers,
    void*                                       pReserved)
{
   *pLayerCount = 0;

   return VK_SUCCESS;
}

VkResult anv_GetDeviceQueue(
    VkDevice                                    _device,
    uint32_t                                    queueNodeIndex,
    uint32_t                                    queueIndex,
    VkQueue*                                    pQueue)
{
   struct anv_device *device = (struct anv_device *) _device;

   assert(queueIndex == 0);

   *pQueue = (VkQueue) &device->queue;

   return VK_SUCCESS;
}

VkResult
anv_reloc_list_init(struct anv_reloc_list *list, struct anv_device *device)
{
   list->num_relocs = 0;
   list->array_length = 256;
   list->relocs =
      anv_device_alloc(device, list->array_length * sizeof(*list->relocs), 8,
                       VK_SYSTEM_ALLOC_TYPE_INTERNAL);

   if (list->relocs == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   list->reloc_bos =
      anv_device_alloc(device, list->array_length * sizeof(*list->reloc_bos), 8,
                       VK_SYSTEM_ALLOC_TYPE_INTERNAL);

   if (list->relocs == NULL) {
      anv_device_free(device, list->relocs);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   return VK_SUCCESS;
}

void
anv_reloc_list_finish(struct anv_reloc_list *list, struct anv_device *device)
{
   anv_device_free(device, list->relocs);
   anv_device_free(device, list->reloc_bos);
}

static VkResult
anv_reloc_list_grow(struct anv_reloc_list *list, struct anv_device *device,
                    size_t num_additional_relocs)
{
   if (list->num_relocs + num_additional_relocs <= list->array_length)
      return VK_SUCCESS;

   size_t new_length = list->array_length * 2;
   while (new_length < list->num_relocs + num_additional_relocs)
      new_length *= 2;

   struct drm_i915_gem_relocation_entry *new_relocs =
      anv_device_alloc(device, new_length * sizeof(*list->relocs), 8,
                       VK_SYSTEM_ALLOC_TYPE_INTERNAL);
   if (new_relocs == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   struct anv_bo **new_reloc_bos =
      anv_device_alloc(device, new_length * sizeof(*list->reloc_bos), 8,
                       VK_SYSTEM_ALLOC_TYPE_INTERNAL);
   if (new_relocs == NULL) {
      anv_device_free(device, new_relocs);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   memcpy(new_relocs, list->relocs, list->num_relocs * sizeof(*list->relocs));
   memcpy(new_reloc_bos, list->reloc_bos,
          list->num_relocs * sizeof(*list->reloc_bos));

   anv_device_free(device, list->relocs);
   anv_device_free(device, list->reloc_bos);

   list->relocs = new_relocs;
   list->reloc_bos = new_reloc_bos;

   return VK_SUCCESS;
}

static VkResult
anv_batch_bo_create(struct anv_device *device, struct anv_batch_bo **bbo_out)
{
   VkResult result;

   struct anv_batch_bo *bbo =
      anv_device_alloc(device, sizeof(*bbo), 8, VK_SYSTEM_ALLOC_TYPE_INTERNAL);
   if (bbo == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   bbo->num_relocs = 0;
   bbo->prev_batch_bo = NULL;

   result = anv_bo_pool_alloc(&device->batch_bo_pool, &bbo->bo);
   if (result != VK_SUCCESS) {
      anv_device_free(device, bbo);
      return result;
   }

   *bbo_out = bbo;

   return VK_SUCCESS;
}

static void
anv_batch_bo_start(struct anv_batch_bo *bbo, struct anv_batch *batch,
                   size_t batch_padding)
{
   batch->next = batch->start = bbo->bo.map;
   batch->end = bbo->bo.map + bbo->bo.size - batch_padding;
   bbo->first_reloc = batch->relocs.num_relocs;
}

static void
anv_batch_bo_finish(struct anv_batch_bo *bbo, struct anv_batch *batch)
{
   assert(batch->start == bbo->bo.map);
   bbo->length = batch->next - batch->start;
   VG(VALGRIND_CHECK_MEM_IS_DEFINED(batch->start, bbo->length));
   bbo->num_relocs = batch->relocs.num_relocs - bbo->first_reloc;
}

static void
anv_batch_bo_destroy(struct anv_batch_bo *bbo, struct anv_device *device)
{
   anv_bo_pool_free(&device->batch_bo_pool, &bbo->bo);
   anv_device_free(device, bbo);
}

void *
anv_batch_emit_dwords(struct anv_batch *batch, int num_dwords)
{
   if (batch->next + num_dwords * 4 > batch->end)
      batch->extend_cb(batch, batch->user_data);

   void *p = batch->next;

   batch->next += num_dwords * 4;
   assert(batch->next <= batch->end);

   return p;
}

static void
anv_reloc_list_append(struct anv_reloc_list *list, struct anv_device *device,
                      struct anv_reloc_list *other, uint32_t offset)
{
   anv_reloc_list_grow(list, device, other->num_relocs);
   /* TODO: Handle failure */

   memcpy(&list->relocs[list->num_relocs], &other->relocs[0],
          other->num_relocs * sizeof(other->relocs[0]));
   memcpy(&list->reloc_bos[list->num_relocs], &other->reloc_bos[0],
          other->num_relocs * sizeof(other->reloc_bos[0]));

   for (uint32_t i = 0; i < other->num_relocs; i++)
      list->relocs[i + list->num_relocs].offset += offset;

   list->num_relocs += other->num_relocs;
}

static uint64_t
anv_reloc_list_add(struct anv_reloc_list *list, struct anv_device *device,
                   uint32_t offset, struct anv_bo *target_bo, uint32_t delta)
{
   struct drm_i915_gem_relocation_entry *entry;
   int index;

   anv_reloc_list_grow(list, device, 1);
   /* TODO: Handle failure */

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

   size = other->next - other->start;
   assert(size % 4 == 0);

   if (batch->next + size > batch->end)
      batch->extend_cb(batch, batch->user_data);

   assert(batch->next + size <= batch->end);

   memcpy(batch->next, other->start, size);

   offset = batch->next - batch->start;
   anv_reloc_list_append(&batch->relocs, batch->device,
                         &other->relocs, offset);

   batch->next += size;
}

uint64_t
anv_batch_emit_reloc(struct anv_batch *batch,
                     void *location, struct anv_bo *bo, uint32_t delta)
{
   return anv_reloc_list_add(&batch->relocs, batch->device,
                             location - batch->start, bo, delta);
}

VkResult anv_QueueSubmit(
    VkQueue                                     _queue,
    uint32_t                                    cmdBufferCount,
    const VkCmdBuffer*                          pCmdBuffers,
    VkFence                                     _fence)
{
   struct anv_queue *queue = (struct anv_queue *) _queue;
   struct anv_device *device = queue->device;
   struct anv_fence *fence = (struct anv_fence *) _fence;
   int ret;

   for (uint32_t i = 0; i < cmdBufferCount; i++) {
      struct anv_cmd_buffer *cmd_buffer =
         (struct anv_cmd_buffer *) pCmdBuffers[i];

      if (device->dump_aub)
         anv_cmd_buffer_dump(cmd_buffer);

      if (!device->no_hw) {
         ret = anv_gem_execbuffer(device, &cmd_buffer->execbuf);
         if (ret != 0)
            return vk_error(VK_ERROR_UNKNOWN);

         if (fence) {
            ret = anv_gem_execbuffer(device, &fence->execbuf);
            if (ret != 0)
               return vk_error(VK_ERROR_UNKNOWN);
         }

         for (uint32_t i = 0; i < cmd_buffer->bo_count; i++)
            cmd_buffer->exec2_bos[i]->offset = cmd_buffer->exec2_objects[i].offset;
      } else {
         *(uint32_t *)queue->completed_serial.map = cmd_buffer->serial;
      }
   }

   return VK_SUCCESS;
}

VkResult anv_QueueWaitIdle(
    VkQueue                                     _queue)
{
   struct anv_queue *queue = (struct anv_queue *) _queue;

   return vkDeviceWaitIdle((VkDevice) queue->device);
}

VkResult anv_DeviceWaitIdle(
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

   state = anv_state_pool_alloc(&device->dynamic_state_pool, 32, 32);
   bo = &device->dynamic_state_pool.block_pool->bo;
   batch.start = batch.next = state.map;
   batch.end = state.map + 32;
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

   anv_state_pool_free(&device->dynamic_state_pool, state);

   return VK_SUCCESS;

 fail:
   anv_state_pool_free(&device->dynamic_state_pool, state);

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

VkResult anv_AllocMemory(
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

VkResult anv_FreeMemory(
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

VkResult anv_MapMemory(
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

VkResult anv_UnmapMemory(
    VkDevice                                    _device,
    VkDeviceMemory                              _mem)
{
   struct anv_device_memory *mem = (struct anv_device_memory *) _mem;

   anv_gem_munmap(mem->map, mem->map_size);

   return VK_SUCCESS;
}

VkResult anv_FlushMappedMemory(
    VkDevice                                    device,
    VkDeviceMemory                              mem,
    VkDeviceSize                                offset,
    VkDeviceSize                                size)
{
   /* clflush here for !llc platforms */

   return VK_SUCCESS;
}

VkResult anv_PinSystemMemory(
    VkDevice                                    device,
    const void*                                 pSysMem,
    size_t                                      memSize,
    VkDeviceMemory*                             pMem)
{
   return VK_SUCCESS;
}

VkResult anv_DestroyObject(
    VkDevice                                    _device,
    VkObjectType                                objType,
    VkObject                                    _object)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_object *object = (struct anv_object *) _object;

   switch (objType) {
   case VK_OBJECT_TYPE_INSTANCE:
      return anv_DestroyInstance((VkInstance) _object);

   case VK_OBJECT_TYPE_PHYSICAL_DEVICE:
      /* We don't want to actually destroy physical devices */
      return VK_SUCCESS;

   case VK_OBJECT_TYPE_DEVICE:
      assert(_device == (VkDevice) _object);
      return anv_DestroyDevice((VkDevice) _object);

   case VK_OBJECT_TYPE_QUEUE:
      /* TODO */
      return VK_SUCCESS;

   case VK_OBJECT_TYPE_DEVICE_MEMORY:
      return anv_FreeMemory(_device, (VkDeviceMemory) _object);

   case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
      /* These are just dummys anyway, so we don't need to destroy them */
      return VK_SUCCESS;

   case VK_OBJECT_TYPE_BUFFER:
   case VK_OBJECT_TYPE_IMAGE:
   case VK_OBJECT_TYPE_DEPTH_STENCIL_VIEW:
   case VK_OBJECT_TYPE_SHADER:
   case VK_OBJECT_TYPE_PIPELINE_LAYOUT:
   case VK_OBJECT_TYPE_SAMPLER:
   case VK_OBJECT_TYPE_DESCRIPTOR_SET:
   case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT:
   case VK_OBJECT_TYPE_DYNAMIC_RS_STATE:
   case VK_OBJECT_TYPE_DYNAMIC_CB_STATE:
   case VK_OBJECT_TYPE_DYNAMIC_DS_STATE:
   case VK_OBJECT_TYPE_RENDER_PASS:
      /* These are trivially destroyable */
      anv_device_free(device, (void *) _object);
      return VK_SUCCESS;

   case VK_OBJECT_TYPE_COMMAND_BUFFER:
   case VK_OBJECT_TYPE_PIPELINE:
   case VK_OBJECT_TYPE_DYNAMIC_VP_STATE:
   case VK_OBJECT_TYPE_FENCE:
   case VK_OBJECT_TYPE_QUERY_POOL:
   case VK_OBJECT_TYPE_FRAMEBUFFER:
   case VK_OBJECT_TYPE_BUFFER_VIEW:
   case VK_OBJECT_TYPE_IMAGE_VIEW:
   case VK_OBJECT_TYPE_COLOR_ATTACHMENT_VIEW:
      (object->destructor)(device, object, objType);
      return VK_SUCCESS;

   case VK_OBJECT_TYPE_SEMAPHORE:
   case VK_OBJECT_TYPE_EVENT:
      stub_return(VK_UNSUPPORTED);

   default:
      unreachable("Invalid object type");
   }
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
      /* VK_MEMORY_PROPERTY_HOST_NON_COHERENT_BIT | */
      /* VK_MEMORY_PROPERTY_HOST_UNCACHED_BIT | */
      VK_MEMORY_PROPERTY_HOST_WRITE_COMBINED_BIT;

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

static uint32_t
get_allocation_count(VkObjectType objType)
{
   switch (objType) {
   case VK_OBJECT_TYPE_BUFFER:
   case VK_OBJECT_TYPE_IMAGE:
      return 1;
   default:
      return 0;
   }
}

VkResult anv_GetObjectInfo(
    VkDevice                                    _device,
    VkObjectType                                objType,
    VkObject                                    object,
    VkObjectInfoType                            infoType,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   VkMemoryRequirements memory_requirements;
   uint32_t *count;

   switch (infoType) {
   case VK_OBJECT_INFO_TYPE_MEMORY_REQUIREMENTS:
      *pDataSize = sizeof(memory_requirements);
      if (pData == NULL)
         return VK_SUCCESS;

      fill_memory_requirements(objType, object, pData);
      return VK_SUCCESS;

   case VK_OBJECT_INFO_TYPE_MEMORY_ALLOCATION_COUNT:
      *pDataSize = sizeof(count);
      if (pData == NULL)
         return VK_SUCCESS;

      count = pData;
      *count = get_allocation_count(objType);
      return VK_SUCCESS;

   default:
      return vk_error(VK_UNSUPPORTED);
   }

}

VkResult anv_QueueBindObjectMemory(
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
      buffer->bo = &mem->bo;
      buffer->offset = memOffset;
      break;
   case VK_OBJECT_TYPE_IMAGE:
      image = (struct anv_image *) object;
      image->bo = &mem->bo;
      image->offset = memOffset;
      break;
   default:
      break;
   }
   
   return VK_SUCCESS;
}

VkResult anv_QueueBindObjectMemoryRange(
    VkQueue                                     queue,
    VkObjectType                                objType,
    VkObject                                    object,
    uint32_t                                    allocationIdx,
    VkDeviceSize                                rangeOffset,
    VkDeviceSize                                rangeSize,
    VkDeviceMemory                              mem,
    VkDeviceSize                                memOffset)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_QueueBindImageMemoryRange(
    VkQueue                                     queue,
    VkImage                                     image,
    uint32_t                                    allocationIdx,
    const VkImageMemoryBindInfo*                pBindInfo,
    VkDeviceMemory                              mem,
    VkDeviceSize                                memOffset)
{
   stub_return(VK_UNSUPPORTED);
}

static void
anv_fence_destroy(struct anv_device *device,
                  struct anv_object *object,
                  VkObjectType obj_type)
{
   struct anv_fence *fence = (struct anv_fence *) object;

   assert(obj_type == VK_OBJECT_TYPE_FENCE);

   anv_gem_munmap(fence->bo.map, fence->bo.size);
   anv_gem_close(device, fence->bo.gem_handle);
   anv_device_free(device, fence);
}

VkResult anv_CreateFence(
    VkDevice                                    _device,
    const VkFenceCreateInfo*                    pCreateInfo,
    VkFence*                                    pFence)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_fence *fence;
   struct anv_batch batch;
   VkResult result;

   const uint32_t fence_size = 128;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);

   fence = anv_device_alloc(device, sizeof(*fence), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (fence == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   result = anv_bo_init_new(&fence->bo, device, fence_size);
   if (result != VK_SUCCESS)
      goto fail;

   fence->base.destructor = anv_fence_destroy;

   fence->bo.map =
      anv_gem_mmap(device, fence->bo.gem_handle, 0, fence->bo.size);
   batch.next = batch.start = fence->bo.map;
   batch.end = fence->bo.map + fence->bo.size;
   anv_batch_emit(&batch, GEN8_MI_BATCH_BUFFER_END);
   anv_batch_emit(&batch, GEN8_MI_NOOP);

   fence->exec2_objects[0].handle = fence->bo.gem_handle;
   fence->exec2_objects[0].relocation_count = 0;
   fence->exec2_objects[0].relocs_ptr = 0;
   fence->exec2_objects[0].alignment = 0;
   fence->exec2_objects[0].offset = fence->bo.offset;
   fence->exec2_objects[0].flags = 0;
   fence->exec2_objects[0].rsvd1 = 0;
   fence->exec2_objects[0].rsvd2 = 0;

   fence->execbuf.buffers_ptr = (uintptr_t) fence->exec2_objects;
   fence->execbuf.buffer_count = 1;
   fence->execbuf.batch_start_offset = 0;
   fence->execbuf.batch_len = batch.next - fence->bo.map;
   fence->execbuf.cliprects_ptr = 0;
   fence->execbuf.num_cliprects = 0;
   fence->execbuf.DR1 = 0;
   fence->execbuf.DR4 = 0;

   fence->execbuf.flags =
      I915_EXEC_HANDLE_LUT | I915_EXEC_NO_RELOC | I915_EXEC_RENDER;
   fence->execbuf.rsvd1 = device->context_id;
   fence->execbuf.rsvd2 = 0;

   *pFence = (VkFence) fence;

   return VK_SUCCESS;

 fail:
   anv_device_free(device, fence);

   return result;
}

VkResult anv_ResetFences(
    VkDevice                                    _device,
    uint32_t                                    fenceCount,
    VkFence*                                    pFences)
{
   struct anv_fence **fences = (struct anv_fence **) pFences;

   for (uint32_t i = 0; i < fenceCount; i++)
      fences[i]->ready = false;

   return VK_SUCCESS;
}

VkResult anv_GetFenceStatus(
    VkDevice                                    _device,
    VkFence                                     _fence)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_fence *fence = (struct anv_fence *) _fence;
   int64_t t = 0;
   int ret;

   if (fence->ready)
      return VK_SUCCESS;

   ret = anv_gem_wait(device, fence->bo.gem_handle, &t);
   if (ret == 0) {
      fence->ready = true;
      return VK_SUCCESS;
   }
   
   return VK_NOT_READY;
}

VkResult anv_WaitForFences(
    VkDevice                                    _device,
    uint32_t                                    fenceCount,
    const VkFence*                              pFences,
    bool32_t                                    waitAll,
    uint64_t                                    timeout)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_fence **fences = (struct anv_fence **) pFences;
   int64_t t = timeout;
   int ret;

   /* FIXME: handle !waitAll */

   for (uint32_t i = 0; i < fenceCount; i++) {
      ret = anv_gem_wait(device, fences[i]->bo.gem_handle, &t);
      if (ret == -1 && errno == ETIME)
         return VK_TIMEOUT;
      else if (ret == -1)
         return vk_error(VK_ERROR_UNKNOWN);
   }      

   return VK_SUCCESS;
}

// Queue semaphore functions

VkResult anv_CreateSemaphore(
    VkDevice                                    device,
    const VkSemaphoreCreateInfo*                pCreateInfo,
    VkSemaphore*                                pSemaphore)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_QueueSignalSemaphore(
    VkQueue                                     queue,
    VkSemaphore                                 semaphore)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_QueueWaitSemaphore(
    VkQueue                                     queue,
    VkSemaphore                                 semaphore)
{
   stub_return(VK_UNSUPPORTED);
}

// Event functions

VkResult anv_CreateEvent(
    VkDevice                                    device,
    const VkEventCreateInfo*                    pCreateInfo,
    VkEvent*                                    pEvent)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_GetEventStatus(
    VkDevice                                    device,
    VkEvent                                     event)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_SetEvent(
    VkDevice                                    device,
    VkEvent                                     event)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_ResetEvent(
    VkDevice                                    device,
    VkEvent                                     event)
{
   stub_return(VK_UNSUPPORTED);
}

// Buffer functions

VkResult anv_CreateBuffer(
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
   buffer->bo = NULL;
   buffer->offset = 0;

   *pBuffer = (VkBuffer) buffer;

   return VK_SUCCESS;
}

// Buffer view functions

static void
fill_buffer_surface_state(void *state, VkFormat format,
                          uint32_t offset, uint32_t range)
{
   const struct anv_format *info;

   info = anv_format_for_vk_format(format);
   /* This assumes RGBA float format. */
   uint32_t stride = 4;
   uint32_t num_elements = range / stride;

   struct GEN8_RENDER_SURFACE_STATE surface_state = {
      .SurfaceType = SURFTYPE_BUFFER,
      .SurfaceArray = false,
      .SurfaceFormat = info->surface_format,
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

VkResult anv_CreateBufferView(
    VkDevice                                    _device,
    const VkBufferViewCreateInfo*               pCreateInfo,
    VkBufferView*                               pView)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_buffer *buffer = (struct anv_buffer *) pCreateInfo->buffer;
   struct anv_surface_view *view;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);

   view = anv_device_alloc(device, sizeof(*view), 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (view == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   view->base.destructor = anv_surface_view_destroy;

   view->bo = buffer->bo;
   view->offset = buffer->offset + pCreateInfo->offset;
   view->surface_state =
      anv_state_pool_alloc(&device->surface_state_pool, 64, 64);
   view->format = pCreateInfo->format;
   view->range = pCreateInfo->range;

   fill_buffer_surface_state(view->surface_state.map,
                             pCreateInfo->format, view->offset, pCreateInfo->range);

   *pView = (VkBufferView) view;

   return VK_SUCCESS;
}

// Sampler functions

VkResult anv_CreateSampler(
    VkDevice                                    _device,
    const VkSamplerCreateInfo*                  pCreateInfo,
    VkSampler*                                  pSampler)
{
   struct anv_device *device = (struct anv_device *) _device;
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
         device->float_border_colors.offset +
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

   *pSampler = (VkSampler) sampler;

   return VK_SUCCESS;
}

// Descriptor set functions

VkResult anv_CreateDescriptorSetLayout(
    VkDevice                                    _device,
    const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
    VkDescriptorSetLayout*                      pSetLayout)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_descriptor_set_layout *set_layout;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);

   uint32_t sampler_count[VK_SHADER_STAGE_NUM] = { 0, };
   uint32_t surface_count[VK_SHADER_STAGE_NUM] = { 0, };
   uint32_t num_dynamic_buffers = 0;
   uint32_t count = 0;
   uint32_t stages = 0;
   uint32_t s;

   for (uint32_t i = 0; i < pCreateInfo->count; i++) {
      switch (pCreateInfo->pBinding[i].descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         for_each_bit(s, pCreateInfo->pBinding[i].stageFlags)
            sampler_count[s] += pCreateInfo->pBinding[i].arraySize;
         break;
      default:
         break;
      }

      switch (pCreateInfo->pBinding[i].descriptorType) {
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         for_each_bit(s, pCreateInfo->pBinding[i].stageFlags)
            surface_count[s] += pCreateInfo->pBinding[i].arraySize;
         break;
      default:
         break;
      }

      switch (pCreateInfo->pBinding[i].descriptorType) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         num_dynamic_buffers += pCreateInfo->pBinding[i].arraySize;
         break;
      default:
         break;
      }

      stages |= pCreateInfo->pBinding[i].stageFlags;
      count += pCreateInfo->pBinding[i].arraySize;
   }

   uint32_t sampler_total = 0;
   uint32_t surface_total = 0;
   for (uint32_t s = 0; s < VK_SHADER_STAGE_NUM; s++) {
      sampler_total += sampler_count[s];
      surface_total += surface_count[s];
   }

   size_t size = sizeof(*set_layout) +
      (sampler_total + surface_total) * sizeof(set_layout->entries[0]);
   set_layout = anv_device_alloc(device, size, 8,
                                 VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (!set_layout)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   set_layout->num_dynamic_buffers = num_dynamic_buffers;
   set_layout->count = count;
   set_layout->shader_stages = stages;

   struct anv_descriptor_slot *p = set_layout->entries;
   struct anv_descriptor_slot *sampler[VK_SHADER_STAGE_NUM];
   struct anv_descriptor_slot *surface[VK_SHADER_STAGE_NUM];
   for (uint32_t s = 0; s < VK_SHADER_STAGE_NUM; s++) {
      set_layout->stage[s].surface_count = surface_count[s];
      set_layout->stage[s].surface_start = surface[s] = p;
      p += surface_count[s];
      set_layout->stage[s].sampler_count = sampler_count[s];
      set_layout->stage[s].sampler_start = sampler[s] = p;
      p += sampler_count[s];
   }

   uint32_t descriptor = 0;
   int8_t dynamic_slot = 0;
   bool is_dynamic;
   for (uint32_t i = 0; i < pCreateInfo->count; i++) {
      switch (pCreateInfo->pBinding[i].descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         for_each_bit(s, pCreateInfo->pBinding[i].stageFlags)
            for (uint32_t j = 0; j < pCreateInfo->pBinding[i].arraySize; j++) {
               sampler[s]->index = descriptor + j;
               sampler[s]->dynamic_slot = -1;
               sampler[s]++;
            }
         break;
      default:
         break;
      }

      switch (pCreateInfo->pBinding[i].descriptorType) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         is_dynamic = true;
         break;
      default:
         is_dynamic = false;
         break;
      }

      switch (pCreateInfo->pBinding[i].descriptorType) {
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         for_each_bit(s, pCreateInfo->pBinding[i].stageFlags)
            for (uint32_t j = 0; j < pCreateInfo->pBinding[i].arraySize; j++) {
               surface[s]->index = descriptor + j;
               if (is_dynamic)
                  surface[s]->dynamic_slot = dynamic_slot + j;
               else
                  surface[s]->dynamic_slot = -1;
               surface[s]++;
            }
         break;
      default:
         break;
      }

      if (is_dynamic)
         dynamic_slot += pCreateInfo->pBinding[i].arraySize;

      descriptor += pCreateInfo->pBinding[i].arraySize;
   }

   *pSetLayout = (VkDescriptorSetLayout) set_layout;

   return VK_SUCCESS;
}

VkResult anv_CreateDescriptorPool(
    VkDevice                                    device,
    VkDescriptorPoolUsage                       poolUsage,
    uint32_t                                    maxSets,
    const VkDescriptorPoolCreateInfo*           pCreateInfo,
    VkDescriptorPool*                           pDescriptorPool)
{
   *pDescriptorPool = 1;

   return VK_SUCCESS;
}

VkResult anv_ResetDescriptorPool(
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool)
{
   return VK_SUCCESS;
}

VkResult anv_AllocDescriptorSets(
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
      size = sizeof(*set) + layout->count * sizeof(set->descriptors[0]);
      set = anv_device_alloc(device, size, 8,
                             VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
      if (!set) {
         *pCount = i;
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      /* Descriptor sets may not be 100% filled out so we need to memset to
       * ensure that we can properly detect and handle holes.
       */
      memset(set, 0, size);

      pDescriptorSets[i] = (VkDescriptorSet) set;
   }

   *pCount = count;

   return VK_SUCCESS;
}

void anv_UpdateDescriptors(
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
            set->descriptors[update_samplers->binding + j].sampler =
               (struct anv_sampler *) update_samplers->pSamplers[j];
         }
         break;

      case VK_STRUCTURE_TYPE_UPDATE_SAMPLER_TEXTURES:
         /* FIXME: Shouldn't this be *_UPDATE_SAMPLER_IMAGES? */
         update_sampler_textures = (VkUpdateSamplerTextures *) common;

         for (uint32_t j = 0; j < update_sampler_textures->count; j++) {
            set->descriptors[update_sampler_textures->binding + j].view =
               (struct anv_surface_view *)
               update_sampler_textures->pSamplerImageViews[j].pImageView->view;
            set->descriptors[update_sampler_textures->binding + j].sampler =
               (struct anv_sampler *)
               update_sampler_textures->pSamplerImageViews[j].sampler;
         }
         break;

      case VK_STRUCTURE_TYPE_UPDATE_IMAGES:
         update_images = (VkUpdateImages *) common;

         for (uint32_t j = 0; j < update_images->count; j++) {
            set->descriptors[update_images->binding + j].view =
               (struct anv_surface_view *) update_images->pImageViews[j].view;
         }
         break;

      case VK_STRUCTURE_TYPE_UPDATE_BUFFERS:
         update_buffers = (VkUpdateBuffers *) common;

         for (uint32_t j = 0; j < update_buffers->count; j++) {
            set->descriptors[update_buffers->binding + j].view =
               (struct anv_surface_view *) update_buffers->pBufferViews[j].view;
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

static void
anv_dynamic_vp_state_destroy(struct anv_device *device,
                             struct anv_object *object,
                             VkObjectType obj_type)
{
   struct anv_dynamic_vp_state *state = (void *)object;

   assert(obj_type == VK_OBJECT_TYPE_DYNAMIC_VP_STATE);

   anv_state_pool_free(&device->dynamic_state_pool, state->sf_clip_vp);
   anv_state_pool_free(&device->dynamic_state_pool, state->cc_vp);
   anv_state_pool_free(&device->dynamic_state_pool, state->scissor);

   anv_device_free(device, state);
}

VkResult anv_CreateDynamicViewportState(
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

   state->base.destructor = anv_dynamic_vp_state_destroy;

   unsigned count = pCreateInfo->viewportAndScissorCount;
   state->sf_clip_vp = anv_state_pool_alloc(&device->dynamic_state_pool,
                                            count * 64, 64);
   state->cc_vp = anv_state_pool_alloc(&device->dynamic_state_pool,
                                       count * 8, 32);
   state->scissor = anv_state_pool_alloc(&device->dynamic_state_pool,
                                         count * 32, 32);

   for (uint32_t i = 0; i < pCreateInfo->viewportAndScissorCount; i++) {
      const VkViewport *vp = &pCreateInfo->pViewports[i];
      const VkRect2D *s = &pCreateInfo->pScissors[i];

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

VkResult anv_CreateDynamicRasterState(
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

   *pState = (VkDynamicRsState) state;

   return VK_SUCCESS;
}

VkResult anv_CreateDynamicColorBlendState(
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

   struct GEN8_COLOR_CALC_STATE color_calc_state = {
      .BlendConstantColorRed = pCreateInfo->blendConst[0],
      .BlendConstantColorGreen = pCreateInfo->blendConst[1],
      .BlendConstantColorBlue = pCreateInfo->blendConst[2],
      .BlendConstantColorAlpha = pCreateInfo->blendConst[3]
   };

   GEN8_COLOR_CALC_STATE_pack(NULL, state->state_color_calc, &color_calc_state);

   *pState = (VkDynamicCbState) state;

   return VK_SUCCESS;
}

VkResult anv_CreateDynamicDepthStencilState(
    VkDevice                                    _device,
    const VkDynamicDsStateCreateInfo*           pCreateInfo,
    VkDynamicDsState*                           pState)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_dynamic_ds_state *state;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DYNAMIC_DS_STATE_CREATE_INFO);

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

   *pState = (VkDynamicDsState) state;

   return VK_SUCCESS;
}

// Command buffer functions

static void
anv_cmd_buffer_destroy(struct anv_device *device,
                       struct anv_object *object,
                       VkObjectType obj_type)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) object;

   assert(obj_type == VK_OBJECT_TYPE_COMMAND_BUFFER);

   /* Destroy all of the batch buffers */
   struct anv_batch_bo *bbo = cmd_buffer->last_batch_bo;
   while (bbo) {
      struct anv_batch_bo *prev = bbo->prev_batch_bo;
      anv_batch_bo_destroy(bbo, device);
      bbo = prev;
   }
   anv_reloc_list_finish(&cmd_buffer->batch.relocs, device);

   /* Destroy all of the surface state buffers */
   bbo = cmd_buffer->surface_batch_bo;
   while (bbo) {
      struct anv_batch_bo *prev = bbo->prev_batch_bo;
      anv_batch_bo_destroy(bbo, device);
      bbo = prev;
   }
   anv_reloc_list_finish(&cmd_buffer->surface_relocs, device);

   anv_state_stream_finish(&cmd_buffer->surface_state_stream);
   anv_state_stream_finish(&cmd_buffer->dynamic_state_stream);
   anv_device_free(device, cmd_buffer->exec2_objects);
   anv_device_free(device, cmd_buffer->exec2_bos);
   anv_device_free(device, cmd_buffer);
}

static VkResult
anv_cmd_buffer_chain_batch(struct anv_batch *batch, void *_data)
{
   struct anv_cmd_buffer *cmd_buffer = _data;

   struct anv_batch_bo *new_bbo, *old_bbo = cmd_buffer->last_batch_bo;

   VkResult result = anv_batch_bo_create(cmd_buffer->device, &new_bbo);
   if (result != VK_SUCCESS)
      return result;

   /* We set the end of the batch a little short so we would be sure we
    * have room for the chaining command.  Since we're about to emit the
    * chaining command, let's set it back where it should go.
    */
   batch->end += GEN8_MI_BATCH_BUFFER_START_length * 4;
   assert(batch->end == old_bbo->bo.map + old_bbo->bo.size);

   anv_batch_emit(batch, GEN8_MI_BATCH_BUFFER_START,
      GEN8_MI_BATCH_BUFFER_START_header,
      ._2ndLevelBatchBuffer = _1stlevelbatch,
      .AddressSpaceIndicator = ASI_PPGTT,
      .BatchBufferStartAddress = { &new_bbo->bo, 0 },
   );

   /* Pad out to a 2-dword aligned boundary with zeros */
   if ((uintptr_t)batch->next % 8 != 0) {
      *(uint32_t *)batch->next = 0;
      batch->next += 4;
   }

   anv_batch_bo_finish(cmd_buffer->last_batch_bo, batch);

   new_bbo->prev_batch_bo = old_bbo;
   cmd_buffer->last_batch_bo = new_bbo;

   anv_batch_bo_start(new_bbo, batch, GEN8_MI_BATCH_BUFFER_START_length * 4);

   return VK_SUCCESS;
}

VkResult anv_CreateCommandBuffer(
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

   cmd_buffer->base.destructor = anv_cmd_buffer_destroy;

   cmd_buffer->device = device;
   cmd_buffer->rs_state = NULL;
   cmd_buffer->vp_state = NULL;
   cmd_buffer->cb_state = NULL;
   cmd_buffer->ds_state = NULL;
   memset(&cmd_buffer->state_vf, 0, sizeof(cmd_buffer->state_vf));
   memset(&cmd_buffer->descriptors, 0, sizeof(cmd_buffer->descriptors));

   result = anv_batch_bo_create(device, &cmd_buffer->last_batch_bo);
   if (result != VK_SUCCESS)
      goto fail;

   result = anv_reloc_list_init(&cmd_buffer->batch.relocs, device);
   if (result != VK_SUCCESS)
      goto fail_batch_bo;

   cmd_buffer->batch.device = device;
   cmd_buffer->batch.extend_cb = anv_cmd_buffer_chain_batch;
   cmd_buffer->batch.user_data = cmd_buffer;

   anv_batch_bo_start(cmd_buffer->last_batch_bo, &cmd_buffer->batch,
                      GEN8_MI_BATCH_BUFFER_START_length * 4);

   result = anv_batch_bo_create(device, &cmd_buffer->surface_batch_bo);
   if (result != VK_SUCCESS)
      goto fail_batch_relocs;
   cmd_buffer->surface_batch_bo->first_reloc = 0;

   result = anv_reloc_list_init(&cmd_buffer->surface_relocs, device);
   if (result != VK_SUCCESS)
      goto fail_ss_batch_bo;

   /* Start surface_next at 1 so surface offset 0 is invalid. */
   cmd_buffer->surface_next = 1;

   cmd_buffer->exec2_objects = NULL;
   cmd_buffer->exec2_bos = NULL;
   cmd_buffer->exec2_array_length = 0;

   anv_state_stream_init(&cmd_buffer->surface_state_stream,
                         &device->surface_state_block_pool);
   anv_state_stream_init(&cmd_buffer->dynamic_state_stream,
                         &device->dynamic_state_block_pool);

   cmd_buffer->dirty = 0;
   cmd_buffer->vb_dirty = 0;
   cmd_buffer->descriptors_dirty = 0;
   cmd_buffer->pipeline = NULL;
   cmd_buffer->vp_state = NULL;
   cmd_buffer->rs_state = NULL;
   cmd_buffer->ds_state = NULL;

   *pCmdBuffer = (VkCmdBuffer) cmd_buffer;

   return VK_SUCCESS;

 fail_ss_batch_bo:
   anv_batch_bo_destroy(cmd_buffer->surface_batch_bo, device);
 fail_batch_relocs:
   anv_reloc_list_finish(&cmd_buffer->batch.relocs, device);
 fail_batch_bo:
   anv_batch_bo_destroy(cmd_buffer->last_batch_bo, device);
 fail:
   anv_device_free(device, cmd_buffer);

   return result;
}

static void
anv_cmd_buffer_emit_state_base_address(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_bo *scratch_bo = NULL;

   cmd_buffer->scratch_size = device->scratch_block_pool.size;
   if (cmd_buffer->scratch_size > 0)
      scratch_bo = &device->scratch_block_pool.bo;

   anv_batch_emit(&cmd_buffer->batch, GEN8_STATE_BASE_ADDRESS,
                  .GeneralStateBaseAddress = { scratch_bo, 0 },
                  .GeneralStateMemoryObjectControlState = GEN8_MOCS,
                  .GeneralStateBaseAddressModifyEnable = true,
                  .GeneralStateBufferSize = 0xfffff,
                  .GeneralStateBufferSizeModifyEnable = true,

                  .SurfaceStateBaseAddress = { &cmd_buffer->surface_batch_bo->bo, 0 },
                  .SurfaceStateMemoryObjectControlState = GEN8_MOCS,
                  .SurfaceStateBaseAddressModifyEnable = true,

                  .DynamicStateBaseAddress = { &device->dynamic_state_block_pool.bo, 0 },
                  .DynamicStateMemoryObjectControlState = GEN8_MOCS,
                  .DynamicStateBaseAddressModifyEnable = true,
                  .DynamicStateBufferSize = 0xfffff,
                  .DynamicStateBufferSizeModifyEnable = true,

                  .IndirectObjectBaseAddress = { NULL, 0 },
                  .IndirectObjectMemoryObjectControlState = GEN8_MOCS,
                  .IndirectObjectBaseAddressModifyEnable = true,
                  .IndirectObjectBufferSize = 0xfffff,
                  .IndirectObjectBufferSizeModifyEnable = true,

                  .InstructionBaseAddress = { &device->instruction_block_pool.bo, 0 },
                  .InstructionMemoryObjectControlState = GEN8_MOCS,
                  .InstructionBaseAddressModifyEnable = true,
                  .InstructionBufferSize = 0xfffff,
                  .InstructionBuffersizeModifyEnable = true);
}

VkResult anv_BeginCommandBuffer(
    VkCmdBuffer                                 cmdBuffer,
    const VkCmdBufferBeginInfo*                 pBeginInfo)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;

   anv_cmd_buffer_emit_state_base_address(cmd_buffer);
   cmd_buffer->current_pipeline = UINT32_MAX;

   return VK_SUCCESS;
}

static VkResult
anv_cmd_buffer_add_bo(struct anv_cmd_buffer *cmd_buffer,
                      struct anv_bo *bo,
                      struct drm_i915_gem_relocation_entry *relocs,
                      size_t num_relocs)
{
   struct drm_i915_gem_exec_object2 *obj;

   if (bo->index < cmd_buffer->bo_count &&
       cmd_buffer->exec2_bos[bo->index] == bo)
      return VK_SUCCESS;

   if (cmd_buffer->bo_count >= cmd_buffer->exec2_array_length) {
      uint32_t new_len = cmd_buffer->exec2_objects ?
                         cmd_buffer->exec2_array_length * 2 : 64;

      struct drm_i915_gem_exec_object2 *new_objects =
         anv_device_alloc(cmd_buffer->device, new_len * sizeof(*new_objects),
                          8, VK_SYSTEM_ALLOC_TYPE_INTERNAL);
      if (new_objects == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      struct anv_bo **new_bos =
         anv_device_alloc(cmd_buffer->device, new_len * sizeof(*new_bos),
                          8, VK_SYSTEM_ALLOC_TYPE_INTERNAL);
      if (new_objects == NULL) {
         anv_device_free(cmd_buffer->device, new_objects);
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      if (cmd_buffer->exec2_objects) {
         memcpy(new_objects, cmd_buffer->exec2_objects,
                cmd_buffer->bo_count * sizeof(*new_objects));
         memcpy(new_bos, cmd_buffer->exec2_bos,
                cmd_buffer->bo_count * sizeof(*new_bos));
      }

      cmd_buffer->exec2_objects = new_objects;
      cmd_buffer->exec2_bos = new_bos;
      cmd_buffer->exec2_array_length = new_len;
   }

   assert(cmd_buffer->bo_count < cmd_buffer->exec2_array_length);

   bo->index = cmd_buffer->bo_count++;
   obj = &cmd_buffer->exec2_objects[bo->index];
   cmd_buffer->exec2_bos[bo->index] = bo;

   obj->handle = bo->gem_handle;
   obj->relocation_count = 0;
   obj->relocs_ptr = 0;
   obj->alignment = 0;
   obj->offset = bo->offset;
   obj->flags = 0;
   obj->rsvd1 = 0;
   obj->rsvd2 = 0;

   if (relocs) {
      obj->relocation_count = num_relocs;
      obj->relocs_ptr = (uintptr_t) relocs;
   }

   return VK_SUCCESS;
}

static void
anv_cmd_buffer_add_validate_bos(struct anv_cmd_buffer *cmd_buffer,
                                struct anv_reloc_list *list)
{
   for (size_t i = 0; i < list->num_relocs; i++)
      anv_cmd_buffer_add_bo(cmd_buffer, list->reloc_bos[i], NULL, 0);
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

VkResult anv_EndCommandBuffer(
    VkCmdBuffer                                 cmdBuffer)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_device *device = cmd_buffer->device;
   struct anv_batch *batch = &cmd_buffer->batch;

   anv_batch_emit(batch, GEN8_MI_BATCH_BUFFER_END);

   /* Round batch up to an even number of dwords. */
   if ((batch->next - batch->start) & 4)
      anv_batch_emit(batch, GEN8_MI_NOOP);

   anv_batch_bo_finish(cmd_buffer->last_batch_bo, &cmd_buffer->batch);
   cmd_buffer->surface_batch_bo->num_relocs =
      cmd_buffer->surface_relocs.num_relocs - cmd_buffer->surface_batch_bo->first_reloc;
   cmd_buffer->surface_batch_bo->length = cmd_buffer->surface_next;

   cmd_buffer->bo_count = 0;
   cmd_buffer->need_reloc = false;

   /* Lock for access to bo->index. */
   pthread_mutex_lock(&device->mutex);

   /* Add surface state bos first so we can add them with their relocs. */
   for (struct anv_batch_bo *bbo = cmd_buffer->surface_batch_bo;
        bbo != NULL; bbo = bbo->prev_batch_bo) {
      anv_cmd_buffer_add_bo(cmd_buffer, &bbo->bo,
                            &cmd_buffer->surface_relocs.relocs[bbo->first_reloc],
                            bbo->num_relocs);
   }

   /* Add all of the BOs referenced by surface state */
   anv_cmd_buffer_add_validate_bos(cmd_buffer, &cmd_buffer->surface_relocs);

   /* Add all but the first batch BO */
   struct anv_batch_bo *batch_bo = cmd_buffer->last_batch_bo;
   while (batch_bo->prev_batch_bo) {
      anv_cmd_buffer_add_bo(cmd_buffer, &batch_bo->bo,
                            &batch->relocs.relocs[batch_bo->first_reloc],
                            batch_bo->num_relocs);
      batch_bo = batch_bo->prev_batch_bo;
   }

   /* Add everything referenced by the batches */
   anv_cmd_buffer_add_validate_bos(cmd_buffer, &batch->relocs);

   /* Add the first batch bo last */
   assert(batch_bo->prev_batch_bo == NULL && batch_bo->first_reloc == 0);
   anv_cmd_buffer_add_bo(cmd_buffer, &batch_bo->bo,
                         &batch->relocs.relocs[batch_bo->first_reloc],
                         batch_bo->num_relocs);
   assert(batch_bo->bo.index == cmd_buffer->bo_count - 1);

   anv_cmd_buffer_process_relocs(cmd_buffer, &cmd_buffer->surface_relocs);
   anv_cmd_buffer_process_relocs(cmd_buffer, &batch->relocs);

   cmd_buffer->execbuf.buffers_ptr = (uintptr_t) cmd_buffer->exec2_objects;
   cmd_buffer->execbuf.buffer_count = cmd_buffer->bo_count;
   cmd_buffer->execbuf.batch_start_offset = 0;
   cmd_buffer->execbuf.batch_len = batch->next - batch->start;
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

VkResult anv_ResetCommandBuffer(
    VkCmdBuffer                                 cmdBuffer)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;

   /* Delete all but the first batch bo */
   while (cmd_buffer->last_batch_bo->prev_batch_bo) {
      struct anv_batch_bo *prev = cmd_buffer->last_batch_bo->prev_batch_bo;
      anv_batch_bo_destroy(cmd_buffer->last_batch_bo, cmd_buffer->device);
      cmd_buffer->last_batch_bo = prev;
   }
   assert(cmd_buffer->last_batch_bo->prev_batch_bo == NULL);

   cmd_buffer->batch.relocs.num_relocs = 0;
   anv_batch_bo_start(cmd_buffer->last_batch_bo, &cmd_buffer->batch,
                      GEN8_MI_BATCH_BUFFER_START_length * 4);

   /* Delete all but the first batch bo */
   while (cmd_buffer->surface_batch_bo->prev_batch_bo) {
      struct anv_batch_bo *prev = cmd_buffer->surface_batch_bo->prev_batch_bo;
      anv_batch_bo_destroy(cmd_buffer->surface_batch_bo, cmd_buffer->device);
      cmd_buffer->surface_batch_bo = prev;
   }
   assert(cmd_buffer->surface_batch_bo->prev_batch_bo == NULL);

   cmd_buffer->surface_next = 1;
   cmd_buffer->surface_relocs.num_relocs = 0;

   cmd_buffer->rs_state = NULL;
   cmd_buffer->vp_state = NULL;
   cmd_buffer->cb_state = NULL;
   cmd_buffer->ds_state = NULL;

   return VK_SUCCESS;
}

// Command buffer building functions

void anv_CmdBindPipeline(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    VkPipeline                                  _pipeline)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_pipeline *pipeline = (struct anv_pipeline *) _pipeline;

   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      cmd_buffer->compute_pipeline = pipeline;
      cmd_buffer->compute_dirty |= ANV_CMD_BUFFER_PIPELINE_DIRTY;
      break;

   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      cmd_buffer->pipeline = pipeline;
      cmd_buffer->vb_dirty |= pipeline->vb_used;
      cmd_buffer->dirty |= ANV_CMD_BUFFER_PIPELINE_DIRTY;
      break;

   default:
      assert(!"invalid bind point");
      break;
   }
}

void anv_CmdBindDynamicStateObject(
    VkCmdBuffer                                 cmdBuffer,
    VkStateBindPoint                            stateBindPoint,
    VkDynamicStateObject                        dynamicState)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;

   switch (stateBindPoint) {
   case VK_STATE_BIND_POINT_VIEWPORT:
      cmd_buffer->vp_state = (struct anv_dynamic_vp_state *) dynamicState;
      cmd_buffer->dirty |= ANV_CMD_BUFFER_VP_DIRTY;
      break;
   case VK_STATE_BIND_POINT_RASTER:
      cmd_buffer->rs_state = (struct anv_dynamic_rs_state *) dynamicState;
      cmd_buffer->dirty |= ANV_CMD_BUFFER_RS_DIRTY;
      break;
   case VK_STATE_BIND_POINT_COLOR_BLEND:
      cmd_buffer->cb_state = (struct anv_dynamic_cb_state *) dynamicState;
      cmd_buffer->dirty |= ANV_CMD_BUFFER_CB_DIRTY;
      break;
   case VK_STATE_BIND_POINT_DEPTH_STENCIL:
      cmd_buffer->ds_state = (struct anv_dynamic_ds_state *) dynamicState;
      cmd_buffer->dirty |= ANV_CMD_BUFFER_DS_DIRTY;
      break;
   default:
      break;
   };
}

static struct anv_state
anv_cmd_buffer_alloc_surface_state(struct anv_cmd_buffer *cmd_buffer,
                                   uint32_t size, uint32_t alignment)
{
   struct anv_state state;

   state.offset = align_u32(cmd_buffer->surface_next, alignment);
   if (state.offset + size > cmd_buffer->surface_batch_bo->bo.size)
      return (struct anv_state) { 0 };

   state.map = cmd_buffer->surface_batch_bo->bo.map + state.offset;
   state.alloc_size = size;
   cmd_buffer->surface_next = state.offset + size;

   assert(state.offset + size <= cmd_buffer->surface_batch_bo->bo.size);

   return state;
}

static VkResult
anv_cmd_buffer_new_surface_state_bo(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_batch_bo *new_bbo, *old_bbo = cmd_buffer->surface_batch_bo;

   /* Finish off the old buffer */
   old_bbo->num_relocs =
      cmd_buffer->surface_relocs.num_relocs - old_bbo->first_reloc;
   old_bbo->length = cmd_buffer->surface_next;

   VkResult result = anv_batch_bo_create(cmd_buffer->device, &new_bbo);
   if (result != VK_SUCCESS)
      return result;

   new_bbo->first_reloc = cmd_buffer->surface_relocs.num_relocs;
   cmd_buffer->surface_next = 1;

   new_bbo->prev_batch_bo = old_bbo;
   cmd_buffer->surface_batch_bo = new_bbo;

   /* Re-emit state base addresses so we get the new surface state base
    * address before we start emitting binding tables etc.
    */
   anv_cmd_buffer_emit_state_base_address(cmd_buffer);

   /* It seems like just changing the state base addresses isn't enough.
    * Invalidating the cache seems to be enough to cause things to
    * propagate.  However, I'm not 100% sure what we're supposed to do.
    */
   anv_batch_emit(&cmd_buffer->batch, GEN8_PIPE_CONTROL,
                  .TextureCacheInvalidationEnable = true);

   return VK_SUCCESS;
}

void anv_CmdBindDescriptorSets(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    uint32_t                                    firstSet,
    uint32_t                                    setCount,
    const VkDescriptorSet*                      pDescriptorSets,
    uint32_t                                    dynamicOffsetCount,
    const uint32_t*                             pDynamicOffsets)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_pipeline_layout *layout;
   struct anv_descriptor_set *set;
   struct anv_descriptor_set_layout *set_layout;

   assert(firstSet + setCount < MAX_SETS);

   if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
      layout = cmd_buffer->pipeline->layout;
   else
      layout = cmd_buffer->compute_pipeline->layout;

   uint32_t dynamic_slot = 0;
   for (uint32_t i = 0; i < setCount; i++) {
      set = (struct anv_descriptor_set *) pDescriptorSets[i];
      set_layout = layout->set[firstSet + i].layout;

      cmd_buffer->descriptors[firstSet + i].set = set;

      assert(set_layout->num_dynamic_buffers <
             ARRAY_SIZE(cmd_buffer->descriptors[0].dynamic_offsets));
      memcpy(cmd_buffer->descriptors[firstSet + i].dynamic_offsets,
             pDynamicOffsets + dynamic_slot,
             set_layout->num_dynamic_buffers * sizeof(*pDynamicOffsets));

      cmd_buffer->descriptors_dirty |= set_layout->shader_stages;

      dynamic_slot += set_layout->num_dynamic_buffers;
   }
}

void anv_CmdBindIndexBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    VkIndexType                                 indexType)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_buffer *buffer = (struct anv_buffer *) _buffer;

   static const uint32_t vk_to_gen_index_type[] = {
      [VK_INDEX_TYPE_UINT16]                    = INDEX_WORD,
      [VK_INDEX_TYPE_UINT32]                    = INDEX_DWORD,
   };

   struct GEN8_3DSTATE_VF vf = {
      GEN8_3DSTATE_VF_header,
      .CutIndex = (indexType == VK_INDEX_TYPE_UINT16) ? UINT16_MAX : UINT32_MAX,
   };
   GEN8_3DSTATE_VF_pack(NULL, cmd_buffer->state_vf, &vf);

   cmd_buffer->dirty |= ANV_CMD_BUFFER_INDEX_BUFFER_DIRTY;

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_INDEX_BUFFER,
                  .IndexFormat = vk_to_gen_index_type[indexType],
                  .MemoryObjectControlState = GEN8_MOCS,
                  .BufferStartingAddress = { buffer->bo, buffer->offset + offset },
                  .BufferSize = buffer->size - offset);
}

void anv_CmdBindVertexBuffers(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    startBinding,
    uint32_t                                    bindingCount,
    const VkBuffer*                             pBuffers,
    const VkDeviceSize*                         pOffsets)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_vertex_binding *vb = cmd_buffer->vertex_bindings;

   /* We have to defer setting up vertex buffer since we need the buffer
    * stride from the pipeline. */

   assert(startBinding + bindingCount < MAX_VBS);
   for (uint32_t i = 0; i < bindingCount; i++) {
      vb[startBinding + i].buffer = (struct anv_buffer *) pBuffers[i];
      vb[startBinding + i].offset = pOffsets[i];
      cmd_buffer->vb_dirty |= 1 << (startBinding + i);
   }
}

static VkResult
cmd_buffer_emit_binding_table(struct anv_cmd_buffer *cmd_buffer,
                              unsigned stage, struct anv_state *bt_state)
{
   struct anv_pipeline_layout *layout;
   uint32_t color_attachments, bias, size;

   if (stage == VK_SHADER_STAGE_COMPUTE)
      layout = cmd_buffer->compute_pipeline->layout;
   else
      layout = cmd_buffer->pipeline->layout;

   if (stage == VK_SHADER_STAGE_FRAGMENT) {
      bias = MAX_RTS;
      color_attachments = cmd_buffer->framebuffer->color_attachment_count;
   } else {
      bias = 0;
      color_attachments = 0;
   }

   /* This is a little awkward: layout can be NULL but we still have to
    * allocate and set a binding table for the PS stage for render
    * targets. */
   uint32_t surface_count = layout ? layout->stage[stage].surface_count : 0;

   if (color_attachments + surface_count == 0)
      return VK_SUCCESS;

   size = (bias + surface_count) * sizeof(uint32_t);
   *bt_state = anv_cmd_buffer_alloc_surface_state(cmd_buffer, size, 32);
   uint32_t *bt_map = bt_state->map;

   if (bt_state->map == NULL)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   for (uint32_t ca = 0; ca < color_attachments; ca++) {
      const struct anv_surface_view *view =
         cmd_buffer->framebuffer->color_attachments[ca];

      struct anv_state state =
         anv_cmd_buffer_alloc_surface_state(cmd_buffer, 64, 64);

      if (state.map == NULL)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;

      memcpy(state.map, view->surface_state.map, 64);

      /* The address goes in dwords 8 and 9 of the SURFACE_STATE */
      *(uint64_t *)(state.map + 8 * 4) =
         anv_reloc_list_add(&cmd_buffer->surface_relocs,
                            cmd_buffer->device,
                            state.offset + 8 * 4,
                            view->bo, view->offset);

      bt_map[ca] = state.offset;
   }

   if (layout == NULL)
      return VK_SUCCESS;

   for (uint32_t set = 0; set < layout->num_sets; set++) {
      struct anv_descriptor_set_binding *d = &cmd_buffer->descriptors[set];
      struct anv_descriptor_set_layout *set_layout = layout->set[set].layout;
      struct anv_descriptor_slot *surface_slots =
         set_layout->stage[stage].surface_start;

      uint32_t start = bias + layout->set[set].surface_start[stage];

      for (uint32_t b = 0; b < set_layout->stage[stage].surface_count; b++) {
         struct anv_surface_view *view =
            d->set->descriptors[surface_slots[b].index].view;

         if (!view)
            continue;

         struct anv_state state =
            anv_cmd_buffer_alloc_surface_state(cmd_buffer, 64, 64);

         if (state.map == NULL)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;

         uint32_t offset;
         if (surface_slots[b].dynamic_slot >= 0) {
            uint32_t dynamic_offset =
               d->dynamic_offsets[surface_slots[b].dynamic_slot];

            offset = view->offset + dynamic_offset;
            fill_buffer_surface_state(state.map, view->format, offset,
                                      view->range - dynamic_offset);
         } else {
            offset = view->offset;
            memcpy(state.map, view->surface_state.map, 64);
         }

         /* The address goes in dwords 8 and 9 of the SURFACE_STATE */
         *(uint64_t *)(state.map + 8 * 4) =
            anv_reloc_list_add(&cmd_buffer->surface_relocs,
                               cmd_buffer->device,
                               state.offset + 8 * 4,
                               view->bo, offset);

         bt_map[start + b] = state.offset;
      }
   }

   return VK_SUCCESS;
}

static VkResult
cmd_buffer_emit_samplers(struct anv_cmd_buffer *cmd_buffer,
                         unsigned stage, struct anv_state *state)
{
   struct anv_pipeline_layout *layout;
   uint32_t sampler_count;

   if (stage == VK_SHADER_STAGE_COMPUTE)
      layout = cmd_buffer->compute_pipeline->layout;
   else
      layout = cmd_buffer->pipeline->layout;

   sampler_count = layout ? layout->stage[stage].sampler_count : 0;
   if (sampler_count == 0)
      return VK_SUCCESS;

   uint32_t size = sampler_count * 16;
   *state = anv_state_stream_alloc(&cmd_buffer->dynamic_state_stream, size, 32);

   if (state->map == NULL)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   for (uint32_t set = 0; set < layout->num_sets; set++) {
      struct anv_descriptor_set_binding *d = &cmd_buffer->descriptors[set];
      struct anv_descriptor_set_layout *set_layout = layout->set[set].layout;
      struct anv_descriptor_slot *sampler_slots =
         set_layout->stage[stage].sampler_start;

      uint32_t start = layout->set[set].sampler_start[stage];

      for (uint32_t b = 0; b < set_layout->stage[stage].sampler_count; b++) {
         struct anv_sampler *sampler =
            d->set->descriptors[sampler_slots[b].index].sampler;

         if (!sampler)
            continue;

         memcpy(state->map + (start + b) * 16,
                sampler->state, sizeof(sampler->state));
      }
   }

   return VK_SUCCESS;
}

static VkResult
flush_descriptor_set(struct anv_cmd_buffer *cmd_buffer, uint32_t stage)
{
   struct anv_state surfaces = { 0, }, samplers = { 0, };
   VkResult result;

   result = cmd_buffer_emit_samplers(cmd_buffer, stage, &samplers);
   if (result != VK_SUCCESS)
      return result;
   result = cmd_buffer_emit_binding_table(cmd_buffer, stage, &surfaces);
   if (result != VK_SUCCESS)
      return result;

   static const uint32_t sampler_state_opcodes[] = {
      [VK_SHADER_STAGE_VERTEX]                  = 43,
      [VK_SHADER_STAGE_TESS_CONTROL]            = 44, /* HS */
      [VK_SHADER_STAGE_TESS_EVALUATION]         = 45, /* DS */
      [VK_SHADER_STAGE_GEOMETRY]                = 46,
      [VK_SHADER_STAGE_FRAGMENT]                = 47,
      [VK_SHADER_STAGE_COMPUTE]                 = 0,
   };

   static const uint32_t binding_table_opcodes[] = {
      [VK_SHADER_STAGE_VERTEX]                  = 38,
      [VK_SHADER_STAGE_TESS_CONTROL]            = 39,
      [VK_SHADER_STAGE_TESS_EVALUATION]         = 40,
      [VK_SHADER_STAGE_GEOMETRY]                = 41,
      [VK_SHADER_STAGE_FRAGMENT]                = 42,
      [VK_SHADER_STAGE_COMPUTE]                 = 0,
   };

   if (samplers.alloc_size > 0) {
      anv_batch_emit(&cmd_buffer->batch,
                     GEN8_3DSTATE_SAMPLER_STATE_POINTERS_VS,
                     ._3DCommandSubOpcode  = sampler_state_opcodes[stage],
                     .PointertoVSSamplerState = samplers.offset);
   }

   if (surfaces.alloc_size > 0) {
      anv_batch_emit(&cmd_buffer->batch,
                     GEN8_3DSTATE_BINDING_TABLE_POINTERS_VS,
                     ._3DCommandSubOpcode  = binding_table_opcodes[stage],
                     .PointertoVSBindingTable = surfaces.offset);
   }

   return VK_SUCCESS;
}

static void
flush_descriptor_sets(struct anv_cmd_buffer *cmd_buffer)
{
   uint32_t s, dirty = cmd_buffer->descriptors_dirty &
                       cmd_buffer->pipeline->active_stages;

   VkResult result;
   for_each_bit(s, dirty) {
      result = flush_descriptor_set(cmd_buffer, s);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      assert(result == VK_ERROR_OUT_OF_DEVICE_MEMORY);

      result = anv_cmd_buffer_new_surface_state_bo(cmd_buffer);
      assert(result == VK_SUCCESS);

      /* Re-emit all active binding tables */
      for_each_bit(s, cmd_buffer->pipeline->active_stages) {
         result = flush_descriptor_set(cmd_buffer, s);

         /* It had better succeed this time */
         assert(result == VK_SUCCESS);
      }
   }

   cmd_buffer->descriptors_dirty &= ~cmd_buffer->pipeline->active_stages;
}

static struct anv_state
anv_cmd_buffer_emit_dynamic(struct anv_cmd_buffer *cmd_buffer,
                             uint32_t *a, uint32_t dwords, uint32_t alignment)
{
   struct anv_state state;

   state = anv_state_stream_alloc(&cmd_buffer->dynamic_state_stream,
                                  dwords * 4, alignment);
   memcpy(state.map, a, dwords * 4);

   VG(VALGRIND_CHECK_MEM_IS_DEFINED(state.map, dwords * 4));

   return state;
}

static struct anv_state
anv_cmd_buffer_merge_dynamic(struct anv_cmd_buffer *cmd_buffer,
                             uint32_t *a, uint32_t *b,
                             uint32_t dwords, uint32_t alignment)
{
   struct anv_state state;
   uint32_t *p;

   state = anv_state_stream_alloc(&cmd_buffer->dynamic_state_stream,
                                  dwords * 4, alignment);
   p = state.map;
   for (uint32_t i = 0; i < dwords; i++)
      p[i] = a[i] | b[i];

   VG(VALGRIND_CHECK_MEM_IS_DEFINED(p, dwords * 4));

   return state;
}

static VkResult
flush_compute_descriptor_set(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_pipeline *pipeline = cmd_buffer->compute_pipeline;
   struct anv_state surfaces = { 0, }, samplers = { 0, };
   VkResult result;

   result = cmd_buffer_emit_samplers(cmd_buffer,
                                     VK_SHADER_STAGE_COMPUTE, &samplers);
   if (result != VK_SUCCESS)
      return result;
   result = cmd_buffer_emit_binding_table(cmd_buffer,
                                          VK_SHADER_STAGE_COMPUTE, &surfaces);
   if (result != VK_SUCCESS)
      return result;

   struct GEN8_INTERFACE_DESCRIPTOR_DATA desc = {
      .KernelStartPointer = pipeline->cs_simd,
      .KernelStartPointerHigh = 0,
      .BindingTablePointer = surfaces.offset,
      .BindingTableEntryCount = 0,
      .SamplerStatePointer = samplers.offset,
      .SamplerCount = 0,
      .NumberofThreadsinGPGPUThreadGroup = 0 /* FIXME: Really? */
   };

   uint32_t size = GEN8_INTERFACE_DESCRIPTOR_DATA_length * sizeof(uint32_t);
   struct anv_state state =
      anv_state_pool_alloc(&device->dynamic_state_pool, size, 64);

   GEN8_INTERFACE_DESCRIPTOR_DATA_pack(NULL, state.map, &desc);

   anv_batch_emit(&cmd_buffer->batch, GEN8_MEDIA_INTERFACE_DESCRIPTOR_LOAD,
                  .InterfaceDescriptorTotalLength = size,
                  .InterfaceDescriptorDataStartAddress = state.offset);

   return VK_SUCCESS;
}

static void
anv_cmd_buffer_flush_compute_state(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->compute_pipeline;
   VkResult result;

   assert(pipeline->active_stages == VK_SHADER_STAGE_COMPUTE_BIT);

   if (cmd_buffer->current_pipeline != GPGPU) {
      anv_batch_emit(&cmd_buffer->batch, GEN8_PIPELINE_SELECT,
                     .PipelineSelection = GPGPU);
      cmd_buffer->current_pipeline = GPGPU;
   }

   if (cmd_buffer->compute_dirty & ANV_CMD_BUFFER_PIPELINE_DIRTY)
      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);

   if ((cmd_buffer->descriptors_dirty & VK_SHADER_STAGE_COMPUTE_BIT) ||
       (cmd_buffer->compute_dirty & ANV_CMD_BUFFER_PIPELINE_DIRTY)) {
      result = flush_compute_descriptor_set(cmd_buffer);
      if (result != VK_SUCCESS) {
         result = anv_cmd_buffer_new_surface_state_bo(cmd_buffer);
         assert(result == VK_SUCCESS);
         result = flush_compute_descriptor_set(cmd_buffer);
         assert(result == VK_SUCCESS);
      }
      cmd_buffer->descriptors_dirty &= ~VK_SHADER_STAGE_COMPUTE;
   }

   cmd_buffer->compute_dirty = 0;
}

static void
anv_cmd_buffer_flush_state(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->pipeline;
   uint32_t *p;

   uint32_t vb_emit = cmd_buffer->vb_dirty & pipeline->vb_used;

   assert((pipeline->active_stages & VK_SHADER_STAGE_COMPUTE_BIT) == 0);

   if (cmd_buffer->current_pipeline != _3D) {
      anv_batch_emit(&cmd_buffer->batch, GEN8_PIPELINE_SELECT,
                     .PipelineSelection = _3D);
      cmd_buffer->current_pipeline = _3D;
   }

   if (vb_emit) {
      const uint32_t num_buffers = __builtin_popcount(vb_emit);
      const uint32_t num_dwords = 1 + num_buffers * 4;

      p = anv_batch_emitn(&cmd_buffer->batch, num_dwords,
                          GEN8_3DSTATE_VERTEX_BUFFERS);
      uint32_t vb, i = 0;
      for_each_bit(vb, vb_emit) {
         struct anv_buffer *buffer = cmd_buffer->vertex_bindings[vb].buffer;
         uint32_t offset = cmd_buffer->vertex_bindings[vb].offset;

         struct GEN8_VERTEX_BUFFER_STATE state = {
            .VertexBufferIndex = vb,
            .MemoryObjectControlState = GEN8_MOCS,
            .AddressModifyEnable = true,
            .BufferPitch = pipeline->binding_stride[vb],
            .BufferStartingAddress = { buffer->bo, buffer->offset + offset },
            .BufferSize = buffer->size - offset
         };

         GEN8_VERTEX_BUFFER_STATE_pack(&cmd_buffer->batch, &p[1 + i * 4], &state);
         i++;
      }
   }

   if (cmd_buffer->dirty & ANV_CMD_BUFFER_PIPELINE_DIRTY) {
      /* If somebody compiled a pipeline after starting a command buffer the
       * scratch bo may have grown since we started this cmd buffer (and
       * emitted STATE_BASE_ADDRESS).  If we're binding that pipeline now,
       * reemit STATE_BASE_ADDRESS so that we use the bigger scratch bo. */
      if (cmd_buffer->scratch_size < pipeline->total_scratch)
         anv_cmd_buffer_emit_state_base_address(cmd_buffer);

      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);
   }

   if (cmd_buffer->descriptors_dirty)
      flush_descriptor_sets(cmd_buffer);

   if (cmd_buffer->dirty & ANV_CMD_BUFFER_VP_DIRTY) {
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_SCISSOR_STATE_POINTERS,
                     .ScissorRectPointer = cmd_buffer->vp_state->scissor.offset);
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_CC,
                     .CCViewportPointer = cmd_buffer->vp_state->cc_vp.offset);
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP,
                     .SFClipViewportPointer = cmd_buffer->vp_state->sf_clip_vp.offset);
   }

   if (cmd_buffer->dirty & (ANV_CMD_BUFFER_PIPELINE_DIRTY | ANV_CMD_BUFFER_RS_DIRTY)) {
      anv_batch_emit_merge(&cmd_buffer->batch,
                           cmd_buffer->rs_state->state_sf, pipeline->state_sf);
      anv_batch_emit_merge(&cmd_buffer->batch,
                           cmd_buffer->rs_state->state_raster, pipeline->state_raster);
   }

   if (cmd_buffer->ds_state &&
       (cmd_buffer->dirty & (ANV_CMD_BUFFER_PIPELINE_DIRTY | ANV_CMD_BUFFER_DS_DIRTY)))
      anv_batch_emit_merge(&cmd_buffer->batch,
                           cmd_buffer->ds_state->state_wm_depth_stencil,
                           pipeline->state_wm_depth_stencil);

   if (cmd_buffer->dirty & (ANV_CMD_BUFFER_CB_DIRTY | ANV_CMD_BUFFER_DS_DIRTY)) {
      struct anv_state state;
      if (cmd_buffer->ds_state == NULL)
         state = anv_cmd_buffer_emit_dynamic(cmd_buffer,
                                             cmd_buffer->cb_state->state_color_calc,
                                             GEN8_COLOR_CALC_STATE_length, 64);
      else if (cmd_buffer->cb_state == NULL)
         state = anv_cmd_buffer_emit_dynamic(cmd_buffer,
                                             cmd_buffer->ds_state->state_color_calc,
                                             GEN8_COLOR_CALC_STATE_length, 64);
      else
         state = anv_cmd_buffer_merge_dynamic(cmd_buffer,
                                              cmd_buffer->ds_state->state_color_calc,
                                              cmd_buffer->cb_state->state_color_calc,
                                              GEN8_COLOR_CALC_STATE_length, 64);

      anv_batch_emit(&cmd_buffer->batch,
                     GEN8_3DSTATE_CC_STATE_POINTERS,
                     .ColorCalcStatePointer = state.offset,
                     .ColorCalcStatePointerValid = true);
   }

   if (cmd_buffer->dirty & (ANV_CMD_BUFFER_PIPELINE_DIRTY | ANV_CMD_BUFFER_INDEX_BUFFER_DIRTY)) {
      anv_batch_emit_merge(&cmd_buffer->batch,
                           cmd_buffer->state_vf, pipeline->state_vf);
   }

   cmd_buffer->vb_dirty &= ~vb_emit;
   cmd_buffer->dirty = 0;
}

void anv_CmdDraw(
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

void anv_CmdDrawIndexed(
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
                  .BaseVertexLocation = vertexOffset);
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

void anv_CmdDrawIndirect(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    count,
    uint32_t                                    stride)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_buffer *buffer = (struct anv_buffer *) _buffer;
   struct anv_bo *bo = buffer->bo;
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

void anv_CmdDrawIndexedIndirect(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    count,
    uint32_t                                    stride)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_buffer *buffer = (struct anv_buffer *) _buffer;
   struct anv_bo *bo = buffer->bo;
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

void anv_CmdDispatch(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    x,
    uint32_t                                    y,
    uint32_t                                    z)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_pipeline *pipeline = cmd_buffer->compute_pipeline;
   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;

   anv_cmd_buffer_flush_compute_state(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GEN8_GPGPU_WALKER,
                  .SIMDSize = prog_data->simd_size / 16,
                  .ThreadDepthCounterMaximum = 0,
                  .ThreadHeightCounterMaximum = 0,
                  .ThreadWidthCounterMaximum = pipeline->cs_thread_width_max,
                  .ThreadGroupIDXDimension = x,
                  .ThreadGroupIDYDimension = y,
                  .ThreadGroupIDZDimension = z,
                  .RightExecutionMask = pipeline->cs_right_mask,
                  .BottomExecutionMask = 0xffffffff);

   anv_batch_emit(&cmd_buffer->batch, GEN8_MEDIA_STATE_FLUSH);
}

#define GPGPU_DISPATCHDIMX 0x2500
#define GPGPU_DISPATCHDIMY 0x2504
#define GPGPU_DISPATCHDIMZ 0x2508

void anv_CmdDispatchIndirect(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_pipeline *pipeline = cmd_buffer->compute_pipeline;
   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;
   struct anv_buffer *buffer = (struct anv_buffer *) _buffer;
   struct anv_bo *bo = buffer->bo;
   uint32_t bo_offset = buffer->offset + offset;

   anv_cmd_buffer_flush_compute_state(cmd_buffer);

   anv_batch_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMX, bo, bo_offset);
   anv_batch_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMY, bo, bo_offset + 4);
   anv_batch_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMZ, bo, bo_offset + 8);

   anv_batch_emit(&cmd_buffer->batch, GEN8_GPGPU_WALKER,
                  .IndirectParameterEnable = true,
                  .SIMDSize = prog_data->simd_size / 16,
                  .ThreadDepthCounterMaximum = 0,
                  .ThreadHeightCounterMaximum = 0,
                  .ThreadWidthCounterMaximum = pipeline->cs_thread_width_max,
                  .RightExecutionMask = pipeline->cs_right_mask,
                  .BottomExecutionMask = 0xffffffff);

   anv_batch_emit(&cmd_buffer->batch, GEN8_MEDIA_STATE_FLUSH);
}

void anv_CmdSetEvent(
    VkCmdBuffer                                 cmdBuffer,
    VkEvent                                     event,
    VkPipeEvent                                 pipeEvent)
{
   stub();
}

void anv_CmdResetEvent(
    VkCmdBuffer                                 cmdBuffer,
    VkEvent                                     event,
    VkPipeEvent                                 pipeEvent)
{
   stub();
}

void anv_CmdWaitEvents(
    VkCmdBuffer                                 cmdBuffer,
    VkWaitEvent                                 waitEvent,
    uint32_t                                    eventCount,
    const VkEvent*                              pEvents,
    VkPipeEventFlags                            pipeEventMask,
    uint32_t                                    memBarrierCount,
    const void* const*                          ppMemBarriers)
{
   stub();
}

void anv_CmdPipelineBarrier(
    VkCmdBuffer                                 cmdBuffer,
    VkWaitEvent                                 waitEvent,
    VkPipeEventFlags                            pipeEventMask,
    uint32_t                                    memBarrierCount,
    const void* const*                          ppMemBarriers)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *)cmdBuffer;
   uint32_t b, *dw;

   struct GEN8_PIPE_CONTROL cmd = {
      GEN8_PIPE_CONTROL_header,
      .PostSyncOperation = NoWrite,
   };

   /* XXX: I think waitEvent is a no-op on our HW.  We should verify that. */

   if (anv_clear_mask(&pipeEventMask, VK_PIPE_EVENT_TOP_OF_PIPE_BIT)) {
      /* This is just what PIPE_CONTROL does */
   }

   if (anv_clear_mask(&pipeEventMask,
                      VK_PIPE_EVENT_VERTEX_PROCESSING_COMPLETE_BIT |
                      VK_PIPE_EVENT_LOCAL_FRAGMENT_PROCESSING_COMPLETE_BIT |
                      VK_PIPE_EVENT_FRAGMENT_PROCESSING_COMPLETE_BIT)) {
      cmd.StallAtPixelScoreboard = true;
   }


   if (anv_clear_mask(&pipeEventMask,
                      VK_PIPE_EVENT_GRAPHICS_PIPELINE_COMPLETE_BIT |
                      VK_PIPE_EVENT_COMPUTE_PIPELINE_COMPLETE_BIT |
                      VK_PIPE_EVENT_TRANSFER_COMPLETE_BIT |
                      VK_PIPE_EVENT_COMMANDS_COMPLETE_BIT)) {
      cmd.CommandStreamerStallEnable = true;
   }

   if (anv_clear_mask(&pipeEventMask, VK_PIPE_EVENT_CPU_SIGNAL_BIT)) {
      anv_finishme("VK_PIPE_EVENT_CPU_SIGNAL_BIT");
   }

   /* We checked all known VkPipeEventFlags. */
   anv_assert(pipeEventMask == 0);

   /* XXX: Right now, we're really dumb and just flush whatever categories
    * the app asks for.  One of these days we may make this a bit better
    * but right now that's all the hardware allows for in most areas.
    */
   VkMemoryOutputFlags out_flags = 0;
   VkMemoryInputFlags in_flags = 0;

   for (uint32_t i = 0; i < memBarrierCount; i++) {
      const struct anv_common *common = ppMemBarriers[i];
      switch (common->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_BARRIER: {
         const VkMemoryBarrier *barrier = (VkMemoryBarrier *)common;
         out_flags |= barrier->outputMask;
         in_flags |= barrier->inputMask;
         break;
      }
      case VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER: {
         const VkBufferMemoryBarrier *barrier = (VkBufferMemoryBarrier *)common;
         out_flags |= barrier->outputMask;
         in_flags |= barrier->inputMask;
         break;
      }
      case VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER: {
         const VkImageMemoryBarrier *barrier = (VkImageMemoryBarrier *)common;
         out_flags |= barrier->outputMask;
         in_flags |= barrier->inputMask;
         break;
      }
      default:
         unreachable("Invalid memory barrier type");
      }
   }

   for_each_bit(b, out_flags) {
      switch ((VkMemoryOutputFlags)(1 << b)) {
      case VK_MEMORY_OUTPUT_HOST_WRITE_BIT:
         break; /* FIXME: Little-core systems */
      case VK_MEMORY_OUTPUT_SHADER_WRITE_BIT:
         cmd.DCFlushEnable = true;
         break;
      case VK_MEMORY_OUTPUT_COLOR_ATTACHMENT_BIT:
         cmd.RenderTargetCacheFlushEnable = true;
         break;
      case VK_MEMORY_OUTPUT_DEPTH_STENCIL_ATTACHMENT_BIT:
         cmd.DepthCacheFlushEnable = true;
         break;
      case VK_MEMORY_OUTPUT_TRANSFER_BIT:
         cmd.RenderTargetCacheFlushEnable = true;
         cmd.DepthCacheFlushEnable = true;
         break;
      default:
         unreachable("Invalid memory output flag");
      }
   }

   for_each_bit(b, out_flags) {
      switch ((VkMemoryInputFlags)(1 << b)) {
      case VK_MEMORY_INPUT_HOST_READ_BIT:
         break; /* FIXME: Little-core systems */
      case VK_MEMORY_INPUT_INDIRECT_COMMAND_BIT:
      case VK_MEMORY_INPUT_INDEX_FETCH_BIT:
      case VK_MEMORY_INPUT_VERTEX_ATTRIBUTE_FETCH_BIT:
         cmd.VFCacheInvalidationEnable = true;
         break;
      case VK_MEMORY_INPUT_UNIFORM_READ_BIT:
         cmd.ConstantCacheInvalidationEnable = true;
         /* fallthrough */
      case VK_MEMORY_INPUT_SHADER_READ_BIT:
         cmd.DCFlushEnable = true;
         cmd.TextureCacheInvalidationEnable = true;
         break;
      case VK_MEMORY_INPUT_COLOR_ATTACHMENT_BIT:
      case VK_MEMORY_INPUT_DEPTH_STENCIL_ATTACHMENT_BIT:
         break; /* XXX: Hunh? */
      case VK_MEMORY_INPUT_TRANSFER_BIT:
         cmd.TextureCacheInvalidationEnable = true;
         break;
      }
   }

   dw = anv_batch_emit_dwords(&cmd_buffer->batch, GEN8_PIPE_CONTROL_length);
   GEN8_PIPE_CONTROL_pack(&cmd_buffer->batch, dw, &cmd);
}

static void
anv_framebuffer_destroy(struct anv_device *device,
                        struct anv_object *object,
                        VkObjectType obj_type)
{
   struct anv_framebuffer *fb = (struct anv_framebuffer *)object;

   assert(obj_type == VK_OBJECT_TYPE_FRAMEBUFFER);

   anv_DestroyObject((VkDevice) device,
                     VK_OBJECT_TYPE_DYNAMIC_VP_STATE,
                     fb->vp_state);

   anv_device_free(device, fb);
}

VkResult anv_CreateFramebuffer(
    VkDevice                                    _device,
    const VkFramebufferCreateInfo*              pCreateInfo,
    VkFramebuffer*                              pFramebuffer)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_framebuffer *framebuffer;

   static const struct anv_depth_stencil_view null_view =
      { .depth_format = D16_UNORM, .depth_stride = 0, .stencil_stride = 0 };

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);

   framebuffer = anv_device_alloc(device, sizeof(*framebuffer), 8,
                                  VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (framebuffer == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   framebuffer->base.destructor = anv_framebuffer_destroy;

   framebuffer->color_attachment_count = pCreateInfo->colorAttachmentCount;
   for (uint32_t i = 0; i < pCreateInfo->colorAttachmentCount; i++) {
      framebuffer->color_attachments[i] =
         (struct anv_surface_view *) pCreateInfo->pColorAttachments[i].view;
   }

   if (pCreateInfo->pDepthStencilAttachment) {
      framebuffer->depth_stencil =
         (struct anv_depth_stencil_view *) pCreateInfo->pDepthStencilAttachment->view;
   } else {
      framebuffer->depth_stencil = &null_view;
   }

   framebuffer->sample_count = pCreateInfo->sampleCount;
   framebuffer->width = pCreateInfo->width;
   framebuffer->height = pCreateInfo->height;
   framebuffer->layers = pCreateInfo->layers;

   anv_CreateDynamicViewportState((VkDevice) device,
      &(VkDynamicVpStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_DYNAMIC_VP_STATE_CREATE_INFO,
         .viewportAndScissorCount = 1,
         .pViewports = (VkViewport[]) {
            {
               .originX = 0,
               .originY = 0,
               .width = pCreateInfo->width,
               .height = pCreateInfo->height,
               .minDepth = 0,
               .maxDepth = 1
            },
         },
         .pScissors = (VkRect2D[]) {
            { {  0,  0 },
              { pCreateInfo->width, pCreateInfo->height } },
         }
      },
      &framebuffer->vp_state);

   *pFramebuffer = (VkFramebuffer) framebuffer;

   return VK_SUCCESS;
}

VkResult anv_CreateRenderPass(
    VkDevice                                    _device,
    const VkRenderPassCreateInfo*               pCreateInfo,
    VkRenderPass*                               pRenderPass)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_render_pass *pass;
   size_t size;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);

   size = sizeof(*pass) +
      pCreateInfo->layers * sizeof(struct anv_render_pass_layer);
   pass = anv_device_alloc(device, size, 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pass == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pass->render_area = pCreateInfo->renderArea;

   pass->num_layers = pCreateInfo->layers;

   pass->num_clear_layers = 0;
   for (uint32_t i = 0; i < pCreateInfo->layers; i++) {
      pass->layers[i].color_load_op = pCreateInfo->pColorLoadOps[i];
      pass->layers[i].clear_color = pCreateInfo->pColorLoadClearValues[i];
      if (pass->layers[i].color_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
         pass->num_clear_layers++;
   }

   *pRenderPass = (VkRenderPass) pass;

   return VK_SUCCESS;
}

static void
anv_cmd_buffer_emit_depth_stencil(struct anv_cmd_buffer *cmd_buffer,
                                  struct anv_render_pass *pass)
{
   const struct anv_depth_stencil_view *view =
      cmd_buffer->framebuffer->depth_stencil;

   /* FIXME: Implement the PMA stall W/A */
   /* FIXME: Width and Height are wrong */

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_DEPTH_BUFFER,
                  .SurfaceType = SURFTYPE_2D,
                  .DepthWriteEnable = view->depth_stride > 0,
                  .StencilWriteEnable = view->stencil_stride > 0,
                  .HierarchicalDepthBufferEnable = false,
                  .SurfaceFormat = view->depth_format,
                  .SurfacePitch = view->depth_stride > 0 ? view->depth_stride - 1 : 0,
                  .SurfaceBaseAddress = { view->bo,  view->depth_offset },
                  .Height = pass->render_area.extent.height - 1,
                  .Width = pass->render_area.extent.width - 1,
                  .LOD = 0,
                  .Depth = 1 - 1,
                  .MinimumArrayElement = 0,
                  .DepthBufferObjectControlState = GEN8_MOCS,
                  .RenderTargetViewExtent = 1 - 1,
                  .SurfaceQPitch = view->depth_qpitch >> 2);

   /* Disable hierarchial depth buffers. */
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_HIER_DEPTH_BUFFER);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_STENCIL_BUFFER,
                  .StencilBufferEnable = view->stencil_stride > 0,
                  .StencilBufferObjectControlState = GEN8_MOCS,
                  .SurfacePitch = view->stencil_stride > 0 ? view->stencil_stride - 1 : 0,
                  .SurfaceBaseAddress = { view->bo, view->stencil_offset },
                  .SurfaceQPitch = view->stencil_qpitch >> 2);

   /* Clear the clear params. */
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_CLEAR_PARAMS);
}

void anv_CmdPushConstants(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineLayout                            layout,
    VkShaderStageFlags                          stageFlags,
    uint32_t                                    start,
    uint32_t                                    length,
    const void*                                 values)
{
   stub();
}

void anv_CmdBeginRenderPass(
    VkCmdBuffer                                 cmdBuffer,
    const VkRenderPassBegin*                    pRenderPassBegin)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_render_pass *pass = (struct anv_render_pass *) pRenderPassBegin->renderPass;
   struct anv_framebuffer *framebuffer =
      (struct anv_framebuffer *) pRenderPassBegin->framebuffer;

   cmd_buffer->framebuffer = framebuffer;

   cmd_buffer->descriptors_dirty |= VK_SHADER_STAGE_FRAGMENT_BIT;

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_DRAWING_RECTANGLE,
                  .ClippedDrawingRectangleYMin = pass->render_area.offset.y,
                  .ClippedDrawingRectangleXMin = pass->render_area.offset.x,
                  .ClippedDrawingRectangleYMax =
                     pass->render_area.offset.y + pass->render_area.extent.height - 1,
                  .ClippedDrawingRectangleXMax =
                     pass->render_area.offset.x + pass->render_area.extent.width - 1,
                  .DrawingRectangleOriginY = 0,
                  .DrawingRectangleOriginX = 0);

   anv_cmd_buffer_emit_depth_stencil(cmd_buffer, pass);

   anv_cmd_buffer_clear(cmd_buffer, pass);
}

void anv_CmdEndRenderPass(
    VkCmdBuffer                                 cmdBuffer)
{
   /* Emit a flushing pipe control at the end of a pass.  This is kind of a
    * hack but it ensures that render targets always actually get written.
    * Eventually, we should do flushing based on image format transitions
    * or something of that nature.
    */
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *)cmdBuffer;
   anv_batch_emit(&cmd_buffer->batch, GEN8_PIPE_CONTROL,
                  .PostSyncOperation = NoWrite,
                  .RenderTargetCacheFlushEnable = true,
                  .InstructionCacheInvalidateEnable = true,
                  .DepthCacheFlushEnable = true,
                  .VFCacheInvalidationEnable = true,
                  .TextureCacheInvalidationEnable = true,
                  .CommandStreamerStallEnable = true);
}

void anv_CmdExecuteCommands(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    cmdBuffersCount,
    const VkCmdBuffer*                          pCmdBuffers)
{
   stub();
}

void vkCmdDbgMarkerBegin(
    VkCmdBuffer                              cmdBuffer,
    const char*                                 pMarker)
   __attribute__ ((visibility ("default")));

void vkCmdDbgMarkerEnd(
   VkCmdBuffer                              cmdBuffer)
   __attribute__ ((visibility ("default")));

VkResult vkDbgSetObjectTag(
    VkDevice                                   device,
    VkObject                                   object,
    size_t                                     tagSize,
    const void*                                pTag)
   __attribute__ ((visibility ("default")));


void vkCmdDbgMarkerBegin(
    VkCmdBuffer                              cmdBuffer,
    const char*                                 pMarker)
{
}

void vkCmdDbgMarkerEnd(
    VkCmdBuffer                              cmdBuffer)
{
}

VkResult vkDbgSetObjectTag(
    VkDevice                                   device,
    VkObject                                   object,
    size_t                                     tagSize,
    const void*                                pTag)
{
    return VK_SUCCESS;
}
