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

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

static const VkFormat formats[] = {
   VK_FORMAT_B5G6R5_UNORM,
   VK_FORMAT_B8G8R8A8_UNORM,
   VK_FORMAT_B8G8R8A8_SRGB,
};

VkResult anv_GetDisplayInfoWSI(
    VkDisplayWSI                            display,
    VkDisplayInfoTypeWSI                    infoType,
    size_t*                                 pDataSize,
    void*                                   pData)
{
   VkDisplayFormatPropertiesWSI *properties = pData;
   size_t size;

   if (pDataSize == NULL)
      return VK_ERROR_INVALID_POINTER;

   switch (infoType) {
   case VK_DISPLAY_INFO_TYPE_FORMAT_PROPERTIES_WSI:
      size = sizeof(properties[0]) * ARRAY_SIZE(formats);

      if (pData == NULL) {
         *pDataSize = size;
         return VK_SUCCESS;
      }

      if (*pDataSize < size)
         return vk_error(VK_ERROR_INVALID_VALUE);

      *pDataSize = size;

      for (uint32_t i = 0; i < ARRAY_SIZE(formats); i++)
         properties[i].swapChainFormat = formats[i];

      return VK_SUCCESS;

   default:
      return VK_UNSUPPORTED;
   }
}

struct anv_swap_chain {
   struct anv_device *                          device;
   xcb_connection_t *                           conn;
   xcb_window_t                                 window;
   xcb_gc_t                                     gc;
   VkExtent2D                                   extent;
   uint32_t                                     count;
   struct {
      struct anv_image *                        image;
      struct anv_device_memory *                memory;
      xcb_pixmap_t                              pixmap;
   }                                            images[0];
};

VkResult anv_CreateSwapChainWSI(
    VkDevice                                _device,
    const VkSwapChainCreateInfoWSI*         pCreateInfo,
    VkSwapChainWSI*                         pSwapChain)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   struct anv_swap_chain *chain;
   xcb_void_cookie_t cookie;
   VkResult result;
   size_t size;
   int ret;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAP_CHAIN_CREATE_INFO_WSI);

   size = sizeof(*chain) + pCreateInfo->imageCount * sizeof(chain->images[0]);
   chain = anv_device_alloc(device, size, 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (chain == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   chain->device = device;
   chain->conn = (xcb_connection_t *) pCreateInfo->pNativeWindowSystemHandle;
   chain->window = (xcb_window_t) (uintptr_t) pCreateInfo->pNativeWindowHandle;
   chain->count = pCreateInfo->imageCount;
   chain->extent = pCreateInfo->imageExtent;

   for (uint32_t i = 0; i < chain->count; i++) {
      VkDeviceMemory memory_h;
      VkImage image_h;
      struct anv_image *image;
      struct anv_surface *surface;
      struct anv_device_memory *memory;

      anv_image_create(_device,
         &(struct anv_image_create_info) {
            .force_tile_mode = true,
            .tile_mode = XMAJOR,
            .stride = 0,
            .vk_info =
         &(VkImageCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = pCreateInfo->imageFormat,
            .extent = {
               .width = pCreateInfo->imageExtent.width,
               .height = pCreateInfo->imageExtent.height,
               .depth = 1
            },
            .mipLevels = 1,
            .arraySize = 1,
            .samples = 1,
            /* FIXME: Need a way to use X tiling to allow scanout */
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .flags = 0,
         }},
         &image_h);

      image = anv_image_from_handle(image_h);
      assert(anv_format_is_color(image->format));

      surface = &image->color_surface;

      anv_AllocMemory(_device,
                      &(VkMemoryAllocInfo) {
                         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO,
                         .allocationSize = image->size,
                         .memoryTypeIndex = 0,
                      },
                      &memory_h);

      memory = anv_device_memory_from_handle(memory_h);

      anv_BindImageMemory(VK_NULL_HANDLE, anv_image_to_handle(image),
                          memory_h, 0);

      ret = anv_gem_set_tiling(device, memory->bo.gem_handle,
                               surface->stride, I915_TILING_X);
      if (ret) {
         result = vk_errorf(VK_ERROR_UNKNOWN, "set_tiling failed: %m");
         goto fail;
      }

      int fd = anv_gem_handle_to_fd(device, memory->bo.gem_handle);
      if (fd == -1) {
         result = vk_errorf(VK_ERROR_UNKNOWN, "handle_to_fd failed: %m");
         goto fail;
      }

      uint32_t bpp = 32;
      uint32_t depth = 24;
      xcb_pixmap_t pixmap = xcb_generate_id(chain->conn);

      cookie =
         xcb_dri3_pixmap_from_buffer_checked(chain->conn,
                                             pixmap,
                                             chain->window,
                                             image->size,
                                             pCreateInfo->imageExtent.width,
                                             pCreateInfo->imageExtent.height,
                                             surface->stride,
                                             depth, bpp, fd);

      chain->images[i].image = image;
      chain->images[i].memory = memory;
      chain->images[i].pixmap = pixmap;
      image->swap_chain = chain;

      xcb_discard_reply(chain->conn, cookie.sequence);
   }

   chain->gc = xcb_generate_id(chain->conn);
   if (!chain->gc) {
      result = vk_error(VK_ERROR_UNKNOWN);
      goto fail;
   }

   cookie = xcb_create_gc(chain->conn,
                          chain->gc,
                          chain->window,
                          XCB_GC_GRAPHICS_EXPOSURES,
                          (uint32_t []) { 0 });
   xcb_discard_reply(chain->conn, cookie.sequence);

   *pSwapChain = anv_swap_chain_to_handle(chain);

   return VK_SUCCESS;

 fail:
   return result;
}

VkResult anv_DestroySwapChainWSI(
    VkSwapChainWSI                          _chain)
{
   ANV_FROM_HANDLE(anv_swap_chain, chain, _chain);

   anv_device_free(chain->device, chain);

   return VK_SUCCESS;
}

VkResult anv_GetSwapChainInfoWSI(
    VkSwapChainWSI                          _chain,
    VkSwapChainInfoTypeWSI                  infoType,
    size_t*                                 pDataSize,
    void*                                   pData)
{
   ANV_FROM_HANDLE(anv_swap_chain, chain, _chain);

   VkSwapChainImageInfoWSI *images;
   size_t size;

   switch (infoType) {
   case VK_SWAP_CHAIN_INFO_TYPE_PERSISTENT_IMAGES_WSI:
      size = sizeof(*images) * chain->count;
      if (pData && *pDataSize < size)
         return VK_ERROR_INVALID_VALUE;

      *pDataSize = size;
      if (!pData)
         return VK_SUCCESS;

      images = pData;
      for (uint32_t i = 0; i < chain->count; i++) {
         images[i].image = anv_image_to_handle(chain->images[i].image);
         images[i].memory = anv_device_memory_to_handle(chain->images[i].memory);
      }

      return VK_SUCCESS;

   default:
      return VK_UNSUPPORTED;
   }
}

VkResult anv_QueuePresentWSI(
    VkQueue                                 queue_,
    const VkPresentInfoWSI*                 pPresentInfo)
{
   ANV_FROM_HANDLE(anv_image, image, pPresentInfo->image);

   struct anv_swap_chain *chain = image->swap_chain;
   xcb_void_cookie_t cookie;
   xcb_pixmap_t pixmap;

   assert(pPresentInfo->sType == VK_STRUCTURE_TYPE_PRESENT_INFO_WSI);

   if (chain == NULL)
      return vk_error(VK_ERROR_INVALID_VALUE);

   pixmap = XCB_NONE;
   for (uint32_t i = 0; i < chain->count; i++) {
      if (image == chain->images[i].image) {
         pixmap = chain->images[i].pixmap;
         break;
      }
   }

   if (pixmap == XCB_NONE)
      return vk_error(VK_ERROR_INVALID_VALUE);

   cookie = xcb_copy_area(chain->conn,
                          pixmap,
                          chain->window,
                          chain->gc,
                          0, 0,
                          0, 0,
                          chain->extent.width,
                          chain->extent.height);
   xcb_discard_reply(chain->conn, cookie.sequence);

   xcb_flush(chain->conn);

   return VK_SUCCESS;
}
