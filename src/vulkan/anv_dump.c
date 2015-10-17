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

#include "anv_private.h"

/* This file contains utility functions for help debugging.  They can be
 * called from GDB or similar to help inspect images and buffers.
 */

void
anv_dump_image_to_ppm(struct anv_device *device,
                      struct anv_image *image, unsigned miplevel,
                      unsigned array_layer, const char *filename)
{
   VkDevice vk_device = anv_device_to_handle(device);
   VkResult result;

   VkExtent2D extent = { image->extent.width, image->extent.height };
   for (unsigned i = 0; i < miplevel; i++) {
      extent.width = MAX2(1, extent.width / 2);
      extent.height = MAX2(1, extent.height / 2);
   }

   VkImage copy_image;
   result = anv_CreateImage(vk_device,
      &(VkImageCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = (VkExtent3D) { extent.width, extent.height, 1 },
         .mipLevels = 1,
         .arraySize = 1,
         .samples = 1,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = VK_IMAGE_USAGE_TRANSFER_DESTINATION_BIT,
         .flags = 0,
      }, &copy_image);
   assert(result == VK_SUCCESS);

   VkMemoryRequirements reqs;
   result = anv_GetImageMemoryRequirements(vk_device, copy_image, &reqs);

   VkDeviceMemory memory;
   result = anv_AllocMemory(vk_device,
      &(VkMemoryAllocInfo) {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO,
         .allocationSize = reqs.size,
         .memoryTypeIndex = 0,
      }, &memory);
   assert(result == VK_SUCCESS);

   result = anv_BindImageMemory(vk_device, copy_image, memory, 0);
   assert(result == VK_SUCCESS);

   VkCmdPool cmdPool;
   result = anv_CreateCommandPool(vk_device,
      &(VkCmdPoolCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_CMD_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
         .flags = 0,
      }, &cmdPool);
   assert(result == VK_SUCCESS);

   VkCmdBuffer cmd;
   result = anv_CreateCommandBuffer(vk_device,
      &(VkCmdBufferCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_CMD_BUFFER_CREATE_INFO,
         .cmdPool = cmdPool,
         .level = VK_CMD_BUFFER_LEVEL_PRIMARY,
         .flags = 0,
      }, &cmd);
   assert(result == VK_SUCCESS);

   result = anv_BeginCommandBuffer(cmd,
      &(VkCmdBufferBeginInfo) {
         .sType = VK_STRUCTURE_TYPE_CMD_BUFFER_BEGIN_INFO,
         .flags = VK_CMD_BUFFER_OPTIMIZE_ONE_TIME_SUBMIT_BIT,
      });
   assert(result == VK_SUCCESS);

   anv_CmdBlitImage(cmd,
      anv_image_to_handle(image), VK_IMAGE_LAYOUT_GENERAL,
      copy_image, VK_IMAGE_LAYOUT_GENERAL, 1,
      &(VkImageBlit) {
         .srcSubresource = {
            .aspect = VK_IMAGE_ASPECT_COLOR,
            .mipLevel = miplevel,
            .arrayLayer = array_layer,
            .arraySize = 1,
         },
         .srcOffset = (VkOffset3D) { 0, 0, 0 },
         .srcExtent = (VkExtent3D) {
            extent.width,
            extent.height,
            1
         },
         .destSubresource = {
            .aspect = VK_IMAGE_ASPECT_COLOR,
            .mipLevel = 0,
            .arrayLayer = 0,
            .arraySize = 1,
         },
         .destOffset = (VkOffset3D) { 0, 0, 0 },
         .destExtent = (VkExtent3D) {
            extent.width,
            extent.height,
            1
         },
      }, VK_TEX_FILTER_NEAREST);

   ANV_CALL(CmdPipelineBarrier)(cmd,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      true, 1,
      (const void * []) { &(VkImageMemoryBarrier) {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .outputMask = VK_MEMORY_OUTPUT_TRANSFER_BIT,
         .inputMask = VK_MEMORY_INPUT_HOST_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = 0,
         .destQueueFamilyIndex = 0,
         .image = copy_image,
         .subresourceRange = (VkImageSubresourceRange) {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .mipLevels = 1,
            .baseArrayLayer = 0,
            .arraySize = 1,
         },
      }});

   result = anv_EndCommandBuffer(cmd);
   assert(result == VK_SUCCESS);

   VkFence fence;
   result = anv_CreateFence(vk_device,
      &(VkFenceCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
         .flags = 0,
      }, &fence);
   assert(result == VK_SUCCESS);

   result = anv_QueueSubmit(anv_queue_to_handle(&device->queue),
                            1, &cmd, fence);
   assert(result == VK_SUCCESS);

   result = anv_WaitForFences(vk_device, 1, &fence, true, UINT64_MAX);
   assert(result == VK_SUCCESS);

   anv_DestroyFence(vk_device, fence);
   anv_DestroyCommandPool(vk_device, cmdPool);

   uint8_t *map;
   result = anv_MapMemory(vk_device, memory, 0, reqs.size, 0, (void **)&map);
   assert(result == VK_SUCCESS);

   VkSubresourceLayout layout;
   result = anv_GetImageSubresourceLayout(vk_device, copy_image,
      &(VkImageSubresource) {
         .aspect = VK_IMAGE_ASPECT_COLOR,
         .mipLevel = 0,
         .arrayLayer = 0,
      }, &layout);
   assert(result == VK_SUCCESS);

   map += layout.offset;

   /* Now we can finally write the PPM file */
   FILE *file = fopen(filename, "wb");
   assert(file);

   fprintf(file, "P6\n%d %d\n255\n", extent.width, extent.height);
   for (unsigned y = 0; y < extent.height; y++) {
      uint8_t row[extent.width * 3];
      for (unsigned x = 0; x < extent.width; x++) {
         row[x * 3 + 0] = map[x * 4 + 0];
         row[x * 3 + 1] = map[x * 4 + 1];
         row[x * 3 + 2] = map[x * 4 + 2];
      }
      fwrite(row, 3, extent.width, file);

      map += layout.rowPitch;
   }
   fclose(file);

   anv_UnmapMemory(vk_device, memory);
   anv_DestroyImage(vk_device, copy_image);
   anv_FreeMemory(vk_device, memory);
}
