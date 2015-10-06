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
    VkQueryPool*                                pQueryPool)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_query_pool *pool;
   VkResult result;
   size_t size;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);

   switch (pCreateInfo->queryType) {
   case VK_QUERY_TYPE_OCCLUSION:
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      return VK_UNSUPPORTED;
   default:
      unreachable("");
   }

   pool = anv_device_alloc(device, sizeof(*pool), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pool == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   size = pCreateInfo->slots * sizeof(struct anv_query_pool_slot);
   result = anv_bo_init_new(&pool->bo, device, size);
   if (result != VK_SUCCESS)
      goto fail;

   pool->bo.map = anv_gem_mmap(device, pool->bo.gem_handle, 0, size);

   *pQueryPool = anv_query_pool_to_handle(pool);

   return VK_SUCCESS;

 fail:
   anv_device_free(device, pool);

   return result;
}

void anv_DestroyQueryPool(
    VkDevice                                    _device,
    VkQueryPool                                 _pool)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_query_pool, pool, _pool);

   anv_gem_munmap(pool->bo.map, pool->bo.size);
   anv_gem_close(device, pool->bo.gem_handle);
   anv_device_free(device, pool);
}

VkResult anv_GetQueryPoolResults(
    VkDevice                                    _device,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount,
    size_t*                                     pDataSize,
    void*                                       pData,
    VkQueryResultFlags                          flags)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_query_pool, pool, queryPool);
   struct anv_query_pool_slot *slot = pool->bo.map;
   int64_t timeout = INT64_MAX;
   uint32_t *dst32 = pData;
   uint64_t *dst64 = pData;
   uint64_t result;
   int ret;

   if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
      /* Where is the availabilty info supposed to go? */
      anv_finishme("VK_QUERY_RESULT_WITH_AVAILABILITY_BIT");
      return VK_UNSUPPORTED;
   }

   assert(pool->type == VK_QUERY_TYPE_OCCLUSION);

   if (flags & VK_QUERY_RESULT_64_BIT)
      *pDataSize = queryCount * sizeof(uint64_t);
   else
      *pDataSize = queryCount * sizeof(uint32_t);

   if (pData == NULL)
      return VK_SUCCESS;

   if (flags & VK_QUERY_RESULT_WAIT_BIT) {
      ret = anv_gem_wait(device, pool->bo.gem_handle, &timeout);
      if (ret == -1)
         return vk_errorf(VK_ERROR_UNKNOWN, "gem_wait failed %m");
   }

   for (uint32_t i = 0; i < queryCount; i++) {
      result = slot[startQuery + i].end - slot[startQuery + i].begin;
      if (flags & VK_QUERY_RESULT_64_BIT) {
         *dst64++ = result;
      } else {
         if (result > UINT32_MAX)
            result = UINT32_MAX;
         *dst32++ = result;
      }
   }

   return VK_SUCCESS;
}

void anv_CmdResetQueryPool(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount)
{
   stub();
}
