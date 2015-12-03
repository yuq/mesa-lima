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

VkResult anv_CreateQueryPool(
    VkDevice                                    _device,
    const VkQueryPoolCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkQueryPool*                                pQueryPool)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_query_pool *pool;
   VkResult result;
   uint32_t slot_size;
   uint64_t size;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);

   switch (pCreateInfo->queryType) {
   case VK_QUERY_TYPE_OCCLUSION:
      slot_size = sizeof(struct anv_query_pool_slot);
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   case VK_QUERY_TYPE_TIMESTAMP:
      slot_size = sizeof(uint64_t);
      break;
   default:
      assert(!"Invalid query type");
   }

   pool = anv_alloc2(&device->alloc, pAllocator, sizeof(*pool), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pool->type = pCreateInfo->queryType;
   pool->slots = pCreateInfo->entryCount;

   size = pCreateInfo->entryCount * slot_size;
   result = anv_bo_init_new(&pool->bo, device, size);
   if (result != VK_SUCCESS)
      goto fail;

   pool->bo.map = anv_gem_mmap(device, pool->bo.gem_handle, 0, size);

   *pQueryPool = anv_query_pool_to_handle(pool);

   return VK_SUCCESS;

 fail:
   anv_free2(&device->alloc, pAllocator, pool);

   return result;
}

void anv_DestroyQueryPool(
    VkDevice                                    _device,
    VkQueryPool                                 _pool,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_query_pool, pool, _pool);

   anv_gem_munmap(pool->bo.map, pool->bo.size);
   anv_gem_close(device, pool->bo.gem_handle);
   anv_free2(&device->alloc, pAllocator, pool);
}

VkResult anv_GetQueryPoolResults(
    VkDevice                                    _device,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount,
    size_t                                      dataSize,
    void*                                       pData,
    VkDeviceSize                                stride,
    VkQueryResultFlags                          flags)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_query_pool, pool, queryPool);
   int64_t timeout = INT64_MAX;
   uint64_t result;
   int ret;

   if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
      /* Where is the availabilty info supposed to go? */
      anv_finishme("VK_QUERY_RESULT_WITH_AVAILABILITY_BIT");
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   assert(pool->type == VK_QUERY_TYPE_OCCLUSION ||
          pool->type == VK_QUERY_TYPE_TIMESTAMP);

   if (pData == NULL)
      return VK_SUCCESS;

   if (flags & VK_QUERY_RESULT_WAIT_BIT) {
      ret = anv_gem_wait(device, pool->bo.gem_handle, &timeout);
      if (ret == -1) {
         /* We don't know the real error. */
         return vk_errorf(VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "gem_wait failed %m");
      }
   }

   void *data_end = pData + dataSize;

   for (uint32_t i = 0; i < queryCount; i++) {
      switch (pool->type) {
      case VK_QUERY_TYPE_OCCLUSION: {
         struct anv_query_pool_slot *slot = pool->bo.map;
         result = slot[startQuery + i].end - slot[startQuery + i].begin;
         break;
      }
      case VK_QUERY_TYPE_PIPELINE_STATISTICS:
         /* Not yet implemented */
         break;
      case VK_QUERY_TYPE_TIMESTAMP: {
         uint64_t *slot = pool->bo.map;
         result = slot[startQuery + i];
         break;
      }
      default:
         assert(!"Invalid query type");
      }

      if (flags & VK_QUERY_RESULT_64_BIT) {
         *(uint64_t *)pData = result;
      } else {
         if (result > UINT32_MAX)
            result = UINT32_MAX;
         *(uint32_t *)pData = result;
      }
      pData += stride;
      if (pData >= data_end)
         break;
   }

   return VK_SUCCESS;
}

void anv_CmdResetQueryPool(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount)
{
   stub();
}
