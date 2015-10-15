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

static const VkSurfaceFormatKHR formats[] = {
   { .format = VK_FORMAT_B5G6R5_UNORM, },
   { .format = VK_FORMAT_B8G8R8A8_UNORM, },
   { .format = VK_FORMAT_B8G8R8A8_SRGB, },
};

static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_MAILBOX_KHR,
};

static VkResult
x11_get_window_supported(struct anv_wsi_implementation *impl,
                         struct anv_physical_device *physical_device,
                         const VkSurfaceDescriptionWindowKHR *window,
                         VkBool32 *pSupported)
{
   *pSupported = true;
   stub_return(VK_SUCCESS);
}

static VkResult
x11_get_surface_properties(struct anv_wsi_implementation *impl,
                           struct anv_device *device,
                           const VkSurfaceDescriptionWindowKHR *vk_window,
                           VkSurfacePropertiesKHR *props)
{
   VkPlatformHandleXcbKHR *vk_xcb_handle = vk_window->pPlatformHandle;
   xcb_connection_t *conn = vk_xcb_handle->connection;
   xcb_window_t win = *(xcb_window_t *)vk_window->pPlatformWindow;

   xcb_get_geometry_cookie_t cookie = xcb_get_geometry(conn, win);
   xcb_generic_error_t *err;
   xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(conn, cookie,
                                                           &err);
   if (geom) {
      free(err);
      VkExtent2D extent = { geom->width, geom->height };
      props->currentExtent = extent;
      props->minImageExtent = extent;
      props->maxImageExtent = extent;
   } else {
      /* This can happen if the client didn't wait for the configure event
       * to come back from the compositor.  In that case, we don't know the
       * size of the window so we just return valid "I don't know" stuff.
       */
      free(geom);
      props->currentExtent = (VkExtent2D) { -1, -1 };
      props->minImageExtent = (VkExtent2D) { 1, 1 };
      props->maxImageExtent = (VkExtent2D) { INT16_MAX, INT16_MAX };
   }

   props->minImageCount = 2;
   props->maxImageCount = 4;
   props->supportedTransforms = VK_SURFACE_TRANSFORM_NONE_BIT_KHR;
   props->currentTransform = VK_SURFACE_TRANSFORM_NONE_KHR;
   props->maxImageArraySize = 1;
   props->supportedUsageFlags =
      VK_IMAGE_USAGE_TRANSFER_DESTINATION_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   return VK_SUCCESS;
}

static VkResult
x11_get_surface_formats(struct anv_wsi_implementation *impl,
                        struct anv_device *device,
                        const VkSurfaceDescriptionWindowKHR *vk_window,
                        uint32_t *pCount, VkSurfaceFormatKHR *pSurfaceFormats)
{
   if (pSurfaceFormats == NULL) {
      *pCount = ARRAY_SIZE(formats);
      return VK_SUCCESS;
   }

   assert(*pCount >= ARRAY_SIZE(formats));
   typed_memcpy(pSurfaceFormats, formats, *pCount);
   *pCount = ARRAY_SIZE(formats);

   return VK_SUCCESS;
}

static VkResult
x11_get_surface_present_modes(struct anv_wsi_implementation *impl,
                              struct anv_device *device,
                              const VkSurfaceDescriptionWindowKHR *vk_window,
                              uint32_t *pCount, VkPresentModeKHR *pPresentModes)
{
   if (pPresentModes == NULL) {
      *pCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }

   assert(*pCount >= ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pCount);
   *pCount = ARRAY_SIZE(present_modes);

   return VK_SUCCESS;
}

struct x11_image {
   struct anv_image *                        image;
   struct anv_device_memory *                memory;
   xcb_pixmap_t                              pixmap;
   xcb_get_geometry_cookie_t                 geom_cookie;
   bool                                      busy;
};

struct x11_swapchain {
   struct anv_swapchain                        base;

   xcb_connection_t *                           conn;
   xcb_window_t                                 window;
   xcb_gc_t                                     gc;
   VkExtent2D                                   extent;
   uint32_t                                     image_count;
   uint32_t                                     next_image;
   struct x11_image                             images[0];
};

static VkResult
x11_get_images(struct anv_swapchain *anv_chain,
               uint32_t* pCount, VkImage *pSwapchainImages)
{
   struct x11_swapchain *chain = (struct x11_swapchain *)anv_chain;

   if (pSwapchainImages == NULL) {
      *pCount = chain->image_count;
      return VK_SUCCESS;
   }

   assert(chain->image_count <= *pCount);
   for (uint32_t i = 0; i < chain->image_count; i++)
      pSwapchainImages[i] = anv_image_to_handle(chain->images[i].image);

   *pCount = chain->image_count;

   return VK_SUCCESS;
}

static VkResult
x11_acquire_next_image(struct anv_swapchain *anv_chain,
                       uint64_t timeout,
                       VkSemaphore semaphore,
                       uint32_t *image_index)
{
   struct x11_swapchain *chain = (struct x11_swapchain *)anv_chain;
   struct x11_image *image = &chain->images[chain->next_image];

   if (image->busy) {
      xcb_generic_error_t *err;
      xcb_get_geometry_reply_t *geom =
         xcb_get_geometry_reply(chain->conn, image->geom_cookie, &err);
      if (!geom) {
         free(err);
         return vk_error(VK_ERROR_OUT_OF_DATE_KHR);
      }

      if (geom->width != chain->extent.width ||
          geom->height != chain->extent.height) {
         free(geom);
         return vk_error(VK_ERROR_OUT_OF_DATE_KHR);
      }
      free(geom);

      image->busy = false;
   }

   *image_index = chain->next_image;
   chain->next_image = (chain->next_image + 1) % chain->image_count;
   return VK_SUCCESS;
}

static VkResult
x11_queue_present(struct anv_swapchain *anv_chain,
                  struct anv_queue *queue,
                  uint32_t image_index)
{
   struct x11_swapchain *chain = (struct x11_swapchain *)anv_chain;
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
x11_destroy_swapchain(struct anv_swapchain *anv_chain)
{
   struct x11_swapchain *chain = (struct x11_swapchain *)anv_chain;
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
x11_create_swapchain(struct anv_wsi_implementation *impl,
                      struct anv_device *device,
                      const VkSwapchainCreateInfoKHR *pCreateInfo,
                      struct anv_swapchain **swapchain_out)
{
   struct x11_swapchain *chain;
   xcb_void_cookie_t cookie;
   VkResult result;

   assert(pCreateInfo->pSurfaceDescription->sType ==
          VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_KHR);
   VkSurfaceDescriptionWindowKHR *vk_window =
      (VkSurfaceDescriptionWindowKHR *)pCreateInfo->pSurfaceDescription;
   assert(vk_window->platform == VK_PLATFORM_XCB_KHR);

   int num_images = pCreateInfo->minImageCount;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);
   chain = anv_device_alloc(device, size, 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (chain == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   chain->base.device = device;
   chain->base.destroy = x11_destroy_swapchain;
   chain->base.get_images = x11_get_images;
   chain->base.acquire_next_image = x11_acquire_next_image;
   chain->base.queue_present = x11_queue_present;

   VkPlatformHandleXcbKHR *vk_xcb_handle = vk_window->pPlatformHandle;

   chain->conn = (xcb_connection_t *) vk_xcb_handle->connection;
   chain->window = *(xcb_window_t *)vk_window->pPlatformWindow;
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
         /* FINISHME: Choose a better error. */
         result = vk_errorf(VK_ERROR_OUT_OF_DEVICE_MEMORY,
                            "set_tiling failed: %m");
         goto fail;
      }

      int fd = anv_gem_handle_to_fd(device, memory->bo.gem_handle);
      if (fd == -1) {
         /* FINISHME: Choose a better error. */
         result = vk_errorf(VK_ERROR_OUT_OF_DEVICE_MEMORY,
                            "handle_to_fd failed: %m");
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
      /* FINISHME: Choose a better error. */
      result = vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail;
   }

   cookie = xcb_create_gc(chain->conn,
                          chain->gc,
                          chain->window,
                          XCB_GC_GRAPHICS_EXPOSURES,
                          (uint32_t []) { 0 });
   xcb_discard_reply(chain->conn, cookie.sequence);

   *swapchain_out = &chain->base;

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
   impl->get_surface_properties = x11_get_surface_properties;
   impl->get_surface_formats = x11_get_surface_formats;
   impl->get_surface_present_modes = x11_get_surface_present_modes;
   impl->create_swapchain = x11_create_swapchain;

   instance->wsi_impl[VK_PLATFORM_XCB_KHR] = impl;

   return VK_SUCCESS;
}

void
anv_x11_finish_wsi(struct anv_instance *instance)
{
   anv_instance_free(instance, instance->wsi_impl[VK_PLATFORM_XCB_KHR]);
}
