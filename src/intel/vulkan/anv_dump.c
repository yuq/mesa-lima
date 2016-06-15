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

struct dump_image {
   const char *filename;

   VkExtent2D extent;
   VkImage image;
   VkDeviceMemory memory;
};

static void
dump_image_init(struct anv_device *device, struct dump_image *image,
                uint32_t width, uint32_t height, const char *filename)
{
   VkDevice vk_device = anv_device_to_handle(device);
   MAYBE_UNUSED VkResult result;

   image->filename = filename;
   image->extent = (VkExtent2D) { width, height };

   result = anv_CreateImage(vk_device,
      &(VkImageCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = (VkExtent3D) { width, height, 1 },
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = 1,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .flags = 0,
      }, NULL, &image->image);
   assert(result == VK_SUCCESS);

   VkMemoryRequirements reqs;
   anv_GetImageMemoryRequirements(vk_device, image->image, &reqs);

   result = anv_AllocateMemory(vk_device,
      &(VkMemoryAllocateInfo) {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = reqs.size,
         .memoryTypeIndex = 0,
      }, NULL, &image->memory);
   assert(result == VK_SUCCESS);

   result = anv_BindImageMemory(vk_device, image->image, image->memory, 0);
   assert(result == VK_SUCCESS);
}

static void
dump_image_finish(struct anv_device *device, struct dump_image *image)
{
   VkDevice vk_device = anv_device_to_handle(device);

   anv_DestroyImage(vk_device, image->image, NULL);
   anv_FreeMemory(vk_device, image->memory, NULL);
}

static void
dump_image_do_blit(struct anv_device *device, struct dump_image *image,
                   struct anv_cmd_buffer *cmd_buffer, struct anv_image *src,
                   VkImageAspectFlagBits aspect,
                   unsigned miplevel, unsigned array_layer)
{
   /* We need to do a blit so the image needs to be declared as sampled.  The
    * only thing these are used for is making sure we create the correct
    * views, so it should be find to just stomp it and set it back.
    */
   VkImageUsageFlags old_usage = src->usage;
   src->usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

   anv_CmdBlitImage(anv_cmd_buffer_to_handle(cmd_buffer),
      anv_image_to_handle(src), VK_IMAGE_LAYOUT_GENERAL,
      image->image, VK_IMAGE_LAYOUT_GENERAL, 1,
      &(VkImageBlit) {
         .srcSubresource = {
            .aspectMask = aspect,
            .mipLevel = miplevel,
            .baseArrayLayer = array_layer,
            .layerCount = 1,
         },
         .srcOffsets = {
            { 0, 0, 0 },
            { image->extent.width, image->extent.height, 1 },
         },
         .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
         .dstOffsets = {
            { 0, 0, 0 },
            { image->extent.width, image->extent.height, 1 },
         },
      }, VK_FILTER_NEAREST);

   src->usage = old_usage;

   ANV_CALL(CmdPipelineBarrier)(anv_cmd_buffer_to_handle(cmd_buffer),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      true, 0, NULL, 0, NULL, 1,
      &(VkImageMemoryBarrier) {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_HOST_READ_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = 0,
         .dstQueueFamilyIndex = 0,
         .image = image->image,
         .subresourceRange = (VkImageSubresourceRange) {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
      });
}

static void
dump_image_write_to_ppm(struct anv_device *device, struct dump_image *image)
{
   VkDevice vk_device = anv_device_to_handle(device);
   MAYBE_UNUSED VkResult result;

   VkMemoryRequirements reqs;
   anv_GetImageMemoryRequirements(vk_device, image->image, &reqs);

   uint8_t *map;
   result = anv_MapMemory(vk_device, image->memory, 0, reqs.size, 0, (void **)&map);
   assert(result == VK_SUCCESS);

   VkSubresourceLayout layout;
   anv_GetImageSubresourceLayout(vk_device, image->image,
      &(VkImageSubresource) {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .mipLevel = 0,
         .arrayLayer = 0,
      }, &layout);

   map += layout.offset;

   FILE *file = fopen(image->filename, "wb");
   assert(file);

   uint8_t *row = malloc(image->extent.width * 3);
   assert(row);

   fprintf(file, "P6\n%d %d\n255\n", image->extent.width, image->extent.height);
   for (unsigned y = 0; y < image->extent.height; y++) {
      for (unsigned x = 0; x < image->extent.width; x++) {
         row[x * 3 + 0] = map[x * 4 + 0];
         row[x * 3 + 1] = map[x * 4 + 1];
         row[x * 3 + 2] = map[x * 4 + 2];
      }
      fwrite(row, 3, image->extent.width, file);

      map += layout.rowPitch;
   }
   free(row);
   fclose(file);

   anv_UnmapMemory(vk_device, image->memory);
}

void
anv_dump_image_to_ppm(struct anv_device *device,
                      struct anv_image *image, unsigned miplevel,
                      unsigned array_layer, VkImageAspectFlagBits aspect,
                      const char *filename)
{
   VkDevice vk_device = anv_device_to_handle(device);
   MAYBE_UNUSED VkResult result;

   const uint32_t width = anv_minify(image->extent.width, miplevel);
   const uint32_t height = anv_minify(image->extent.height, miplevel);

   struct dump_image dump;
   dump_image_init(device, &dump, width, height, filename);

   VkCommandPool commandPool;
   result = anv_CreateCommandPool(vk_device,
      &(VkCommandPoolCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
         .flags = 0,
      }, NULL, &commandPool);
   assert(result == VK_SUCCESS);

   VkCommandBuffer cmd;
   result = anv_AllocateCommandBuffers(vk_device,
      &(VkCommandBufferAllocateInfo) {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = commandPool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      }, &cmd);
   assert(result == VK_SUCCESS);

   result = anv_BeginCommandBuffer(cmd,
      &(VkCommandBufferBeginInfo) {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      });
   assert(result == VK_SUCCESS);

   dump_image_do_blit(device, &dump, anv_cmd_buffer_from_handle(cmd), image,
                      aspect, miplevel, array_layer);

   result = anv_EndCommandBuffer(cmd);
   assert(result == VK_SUCCESS);

   VkFence fence;
   result = anv_CreateFence(vk_device,
      &(VkFenceCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
         .flags = 0,
      }, NULL, &fence);
   assert(result == VK_SUCCESS);

   result = anv_QueueSubmit(anv_queue_to_handle(&device->queue), 1,
      &(VkSubmitInfo) {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmd,
      }, fence);
   assert(result == VK_SUCCESS);

   result = anv_WaitForFences(vk_device, 1, &fence, true, UINT64_MAX);
   assert(result == VK_SUCCESS);

   anv_DestroyFence(vk_device, fence, NULL);
   anv_DestroyCommandPool(vk_device, commandPool, NULL);

   dump_image_write_to_ppm(device, &dump);
   dump_image_finish(device, &dump);
}
