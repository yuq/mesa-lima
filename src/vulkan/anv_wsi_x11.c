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

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

#include "anv_wsi.h"

static const VkFormat formats[] = {
   VK_FORMAT_B5G6R5_UNORM,
   VK_FORMAT_B8G8R8A8_UNORM,
   VK_FORMAT_B8G8R8A8_SRGB,
};

static const VkSurfacePresentModePropertiesWSI present_modes[] = {
   { VK_PRESENT_MODE_MAILBOX_WSI },
};

static VkResult
x11_get_window_supported(struct anv_wsi_implementation *impl,
                         struct anv_physical_device *physical_device,
                         const VkSurfaceDescriptionWindowWSI *window,
                         VkBool32 *pSupported)
{
   *pSupported = true;
   stub_return(VK_SUCCESS);
}

static VkResult
x11_get_surface_info(struct anv_wsi_implementation *impl,
                     struct anv_device *device,
                     VkSurfaceDescriptionWindowWSI *vk_window,
                     VkSurfaceInfoTypeWSI infoType,
                     size_t* pDataSize, void* pData)
{
   if (pDataSize == NULL)
      return vk_error(VK_ERROR_INVALID_POINTER);

   switch (infoType) {
   case VK_SURFACE_INFO_TYPE_PROPERTIES_WSI: {
      VkSurfacePropertiesWSI *props = pData;

      if (pData == NULL) {
         *pDataSize = sizeof(*props);
         return VK_SUCCESS;
      }

      assert(*pDataSize >= sizeof(*props));

      VkPlatformHandleXcbWSI *vk_xcb_handle = vk_window->pPlatformHandle;
      xcb_connection_t *conn = vk_xcb_handle->connection;
      xcb_window_t win = (xcb_window_t)(uintptr_t)vk_window->pPlatformWindow;

      xcb_get_geometry_cookie_t cookie = xcb_get_geometry(conn, win);
      xcb_generic_error_t *err;
      xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(conn, cookie,
                                                              &err);
      if (!geom) {
         if (err->error_code == XCB_DRAWABLE) {
            return vk_error(VK_ERROR_INVALID_HANDLE);
         } else {
            return vk_error(VK_ERROR_UNKNOWN);
         }
         free(err);
      }

      VkExtent2D extent = { geom->width, geom->height };
      free(geom);

      props->minImageCount = 2;
      props->maxImageCount = 4;
      props->currentExtent = extent;
      props->minImageExtent = extent;
      props->maxImageExtent = extent;
      props->supportedTransforms = VK_SURFACE_TRANSFORM_NONE_BIT_WSI;
      props->currentTransform = VK_SURFACE_TRANSFORM_NONE_WSI;
      props->maxImageArraySize = 1;
      props->supportedUsageFlags =
         VK_IMAGE_USAGE_TRANSFER_DESTINATION_BIT |
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

      return VK_SUCCESS;
   }

   case VK_SURFACE_INFO_TYPE_FORMATS_WSI:
      if (pData == NULL) {
         *pDataSize = sizeof(formats);
         return VK_SUCCESS;
      }

      assert(*pDataSize >= sizeof(formats));
      memcpy(pData, formats, *pDataSize);

      return VK_SUCCESS;

   case VK_SURFACE_INFO_TYPE_PRESENT_MODES_WSI:
      if (pData == NULL) {
         *pDataSize = sizeof(present_modes);
         return VK_SUCCESS;
      }

      assert(*pDataSize >= sizeof(present_modes));
      memcpy(pData, present_modes, *pDataSize);

      return VK_SUCCESS;
   default:
      return vk_error(VK_ERROR_INVALID_VALUE);
   }
}

struct x11_image {
   struct anv_image *                        image;
   struct anv_device_memory *                memory;
   xcb_pixmap_t                              pixmap;
   xcb_get_geometry_cookie_t                 geom_cookie;
   bool                                      busy;
};

struct x11_swap_chain {
   struct anv_swap_chain                        base;

   xcb_connection_t *                           conn;
   xcb_window_t                                 window;
   xcb_gc_t                                     gc;
   VkExtent2D                                   extent;
   uint32_t                                     image_count;
   uint32_t                                     next_image;
   struct x11_image                             images[0];
};

static VkResult
x11_get_swap_chain_info(struct anv_swap_chain *anv_chain,
                        VkSwapChainInfoTypeWSI infoType,
                        size_t* pDataSize, void* pData)
{
   struct x11_swap_chain *chain = (struct x11_swap_chain *)anv_chain;
   size_t size;

   switch (infoType) {
   case VK_SWAP_CHAIN_INFO_TYPE_IMAGES_WSI: {
      VkSwapChainImagePropertiesWSI *images = pData;

      size = chain->image_count * sizeof(*images);

      if (pData == NULL) {
         *pDataSize = size;
         return VK_SUCCESS;
      }

      assert(size <= *pDataSize);
      for (uint32_t i = 0; i < chain->image_count; i++)
         images[i].image = anv_image_to_handle(chain->images[i].image);

      *pDataSize = size;

      return VK_SUCCESS;
   }

   default:
      return vk_error(VK_ERROR_INVALID_VALUE);
   }
}

static VkResult
x11_acquire_next_image(struct anv_swap_chain *anv_chain,
                       uint64_t timeout,
                       VkSemaphore semaphore,
                       uint32_t *image_index)
{
   struct x11_swap_chain *chain = (struct x11_swap_chain *)anv_chain;
   struct x11_image *image = &chain->images[chain->next_image];

   if (image->busy) {
      xcb_generic_error_t *err;
      xcb_get_geometry_reply_t *geom =
         xcb_get_geometry_reply(chain->conn, image->geom_cookie, &err);
      if (!geom) {
         if (err->error_code == XCB_DRAWABLE) {
            /* Probably the best thing to do if our drawable goes away */
            return vk_error(VK_ERROR_OUT_OF_DATE_WSI);
         } else {
            return vk_error(VK_ERROR_UNKNOWN);
         }
         free(err);
      }

      if (geom->width != chain->extent.width ||
          geom->height != chain->extent.height) {
         free(geom);
         return vk_error(VK_ERROR_OUT_OF_DATE_WSI);
      }
      free(geom);

      image->busy = false;
   }

   *image_index = chain->next_image;
   chain->next_image = (chain->next_image + 1) % chain->image_count;
   return VK_SUCCESS;
}

static VkResult
x11_queue_present(struct anv_swap_chain *anv_chain,
                  struct anv_queue *queue,
                  uint32_t image_index)
{
   struct x11_swap_chain *chain = (struct x11_swap_chain *)anv_chain;
   struct x11_image *image = &chain->images[image_index];

   assert(image_index < chain->image_count);

   xcb_void_cookie_t cookie;

   cookie = xcb_copy_area(chain->conn,
                          image->pixmap,
                          chain->window,
                          chain->gc,
                          0, 0,
                          0, 0,
                          chain->extent.width,
                          chain->extent.height);
   xcb_discard_reply(chain->conn, cookie.sequence);

   image->geom_cookie = xcb_get_geometry(chain->conn, chain->window);
   image->busy = true;

   xcb_flush(chain->conn);

   return VK_SUCCESS;
}

static VkResult
x11_destroy_swap_chain(struct anv_swap_chain *anv_chain)
{
   struct x11_swap_chain *chain = (struct x11_swap_chain *)anv_chain;
   xcb_void_cookie_t cookie;

   for (uint32_t i = 0; i < chain->image_count; i++) {
      struct x11_image *image = &chain->images[i];

      if (image->busy)
         xcb_discard_reply(chain->conn, image->geom_cookie.sequence);

      cookie = xcb_free_pixmap(chain->conn, image->pixmap);
      xcb_discard_reply(chain->conn, cookie.sequence);

      /* TODO: Delete images and free memory */
   }

   anv_device_free(chain->base.device, chain);

   return VK_SUCCESS;
}

static VkResult
x11_create_swap_chain(struct anv_wsi_implementation *impl,
                      struct anv_device *device,
                      const VkSwapChainCreateInfoWSI *pCreateInfo,
                      struct anv_swap_chain **swap_chain_out)
{
   struct x11_swap_chain *chain;
   xcb_void_cookie_t cookie;
   VkResult result;

   assert(pCreateInfo->pSurfaceDescription->sType ==
          VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_WSI);
   VkSurfaceDescriptionWindowWSI *vk_window =
      (VkSurfaceDescriptionWindowWSI *)pCreateInfo->pSurfaceDescription;
   assert(vk_window->platform == VK_PLATFORM_XCB_WSI);

   int num_images = pCreateInfo->minImageCount;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAP_CHAIN_CREATE_INFO_WSI);

   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);
   chain = anv_device_alloc(device, size, 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (chain == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   chain->base.device = device;
   chain->base.destroy = x11_destroy_swap_chain;
   chain->base.get_swap_chain_info = x11_get_swap_chain_info;
   chain->base.acquire_next_image = x11_acquire_next_image;
   chain->base.queue_present = x11_queue_present;

   VkPlatformHandleXcbWSI *vk_xcb_handle = vk_window->pPlatformHandle;

   chain->conn = (xcb_connection_t *) vk_xcb_handle->connection;
   chain->window = (xcb_window_t) (uintptr_t)vk_window->pPlatformWindow;
   chain->extent = pCreateInfo->imageExtent;
   chain->image_count = num_images;
   chain->next_image = 0;

   for (uint32_t i = 0; i < chain->image_count; i++) {
      VkDeviceMemory memory_h;
      VkImage image_h;
      struct anv_image *image;
      struct anv_surface *surface;
      struct anv_device_memory *memory;

      anv_image_create(anv_device_to_handle(device),
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

      anv_AllocMemory(anv_device_to_handle(device),
                      &(VkMemoryAllocInfo) {
                         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO,
                         .allocationSize = image->size,
                         .memoryTypeIndex = 0,
                      },
                      &memory_h);

      memory = anv_device_memory_from_handle(memory_h);

      anv_BindImageMemory(VK_NULL_HANDLE, anv_image_to_handle(image),
                          memory_h, 0);

      int ret = anv_gem_set_tiling(device, memory->bo.gem_handle,
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
      chain->images[i].busy = false;

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

   *swap_chain_out = &chain->base;

   return VK_SUCCESS;

 fail:
   return result;
}

VkResult
anv_x11_init_wsi(struct anv_instance *instance)
{
   struct anv_wsi_implementation *impl;

   impl = anv_instance_alloc(instance, sizeof(*impl), 8,
                             VK_SYSTEM_ALLOC_TYPE_INTERNAL);
   if (!impl)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   impl->get_window_supported = x11_get_window_supported;
   impl->get_surface_info = x11_get_surface_info;
   impl->create_swap_chain = x11_create_swap_chain;

   instance->wsi_impl[VK_PLATFORM_XCB_WSI] = impl;

   return VK_SUCCESS;
}

void
anv_x11_finish_wsi(struct anv_instance *instance)
{
   anv_instance_free(instance, instance->wsi_impl[VK_PLATFORM_XCB_WSI]);
}
