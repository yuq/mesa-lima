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

#include <vulkan/vulkan_intel.h>

VkResult VKAPI vkCreateDmaBufImageINTEL(
    VkDevice                                    _device,
    const VkDmaBufImageCreateInfo*              pCreateInfo,
    VkDeviceMemory*                             pMem,
    VkImage*                                    pImage)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_device_memory *mem;
   struct anv_image *image;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DMA_BUF_IMAGE_CREATE_INFO_INTEL);

   mem = anv_device_alloc(device, sizeof(*mem), 8,
                          VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (mem == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   mem->bo.gem_handle = anv_gem_fd_to_handle(device, pCreateInfo->fd);
   if (!mem->bo.gem_handle) {
      result = vk_error(VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto fail;
   }

   mem->bo.map = NULL;
   mem->bo.index = 0;
   mem->bo.offset = 0;
   mem->bo.size = pCreateInfo->strideInBytes * pCreateInfo->extent.height;

   image = anv_device_alloc(device, sizeof(*image), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (image == NULL) {
      result = vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_mem;
   }

   image->mem = mem;
   image->offset = 0;
   image->type = VK_IMAGE_TYPE_2D;
   image->extent = pCreateInfo->extent;
   image->tile_mode = XMAJOR;
   image->stride = pCreateInfo->strideInBytes;
   image->size = mem->bo.size;

   assert(image->extent.width > 0);
   assert(image->extent.height > 0);
   assert(image->extent.depth == 1);

   *pMem = (VkDeviceMemory) mem;
   *pImage = (VkImage) image;

   return VK_SUCCESS;

 fail_mem:
   anv_gem_close(device, mem->bo.gem_handle);
 fail:
   anv_device_free(device, mem);

   return result;
}
