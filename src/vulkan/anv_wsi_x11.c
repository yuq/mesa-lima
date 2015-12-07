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

struct x11_surface {
   struct anv_wsi_surface base;

   xcb_connection_t *connection;
   xcb_window_t window;
};

static const VkSurfaceFormatKHR formats[] = {
   { .format = VK_FORMAT_B8G8R8A8_UNORM, },
};

static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_MAILBOX_KHR,
};

VkBool32 anv_GetPhysicalDeviceXcbPresentationSupportKHR(
    VkPhysicalDevice                            physicalDevice,
    uint32_t                                    queueFamilyIndex,
    xcb_connection_t*                           connection,
    xcb_visualid_t                              visual_id)
{
   anv_finishme("Check that we actually have DRI3");
   stub_return(true);
}

static VkResult
x11_surface_get_capabilities(struct anv_wsi_surface *wsi_surface,
                             struct anv_physical_device *device,
                             VkSurfaceCapabilitiesKHR *caps)
{
   struct x11_surface *surface = (struct x11_surface *)wsi_surface;

   xcb_get_geometry_cookie_t cookie = xcb_get_geometry(surface->connection,
                                                       surface->window);
   xcb_generic_error_t *err;
   xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(surface->connection,
                                                           cookie, &err);
   if (geom) {
      VkExtent2D extent = { geom->width, geom->height };
      caps->currentExtent = extent;
      caps->minImageExtent = extent;
      caps->maxImageExtent = extent;
   } else {
      /* This can happen if the client didn't wait for the configure event
       * to come back from the compositor.  In that case, we don't know the
       * size of the window so we just return valid "I don't know" stuff.
       */
      caps->currentExtent = (VkExtent2D) { -1, -1 };
      caps->minImageExtent = (VkExtent2D) { 1, 1 };
      caps->maxImageExtent = (VkExtent2D) { INT16_MAX, INT16_MAX };
   }
   free(err);
   free(geom);

   caps->minImageCount = 2;
   caps->maxImageCount = 4;
   caps->supportedTransforms = VK_SURFACE_TRANSFORM_NONE_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_NONE_BIT_KHR;
   caps->maxImageArrayLayers = 1;
   caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
   caps->supportedUsageFlags =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   return VK_SUCCESS;
}

static VkResult
x11_surface_get_formats(struct anv_wsi_surface *surface,
                        struct anv_physical_device *device,
                        uint32_t *pSurfaceFormatCount,
                        VkSurfaceFormatKHR *pSurfaceFormats)
{
   if (pSurfaceFormats == NULL) {
      *pSurfaceFormatCount = ARRAY_SIZE(formats);
      return VK_SUCCESS;
   }

   assert(*pSurfaceFormatCount >= ARRAY_SIZE(formats));
   typed_memcpy(pSurfaceFormats, formats, *pSurfaceFormatCount);
   *pSurfaceFormatCount = ARRAY_SIZE(formats);

   return VK_SUCCESS;
}

static VkResult
x11_surface_get_present_modes(struct anv_wsi_surface *surface,
                              struct anv_physical_device *device,
                              uint32_t *pPresentModeCount,
                              VkPresentModeKHR *pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }

   assert(*pPresentModeCount >= ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pPresentModeCount);
   *pPresentModeCount = ARRAY_SIZE(present_modes);

   return VK_SUCCESS;
}

static void
x11_surface_destroy(struct anv_wsi_surface *surface,
                    const VkAllocationCallbacks *pAllocator)
{
   anv_free2(&surface->instance->alloc, pAllocator, surface);
}

static VkResult
x11_surface_create_swapchain(struct anv_wsi_surface *surface,
                             struct anv_device *device,
                             const VkSwapchainCreateInfoKHR* pCreateInfo,
                             const VkAllocationCallbacks* pAllocator,
                             struct anv_swapchain **swapchain);

VkResult anv_CreateXcbSurfaceKHR(
    VkInstance                                  _instance,
    xcb_connection_t*                           connection,
    xcb_window_t                                window,
    const VkAllocationCallbacks*                pAllocator,
    VkSurfaceKHR*                               pSurface)
{
   ANV_FROM_HANDLE(anv_instance, instance, _instance);
   struct x11_surface *surface;

   surface = anv_alloc2(&instance->alloc, pAllocator, sizeof *surface, 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   surface->connection = connection;
   surface->window = window;

   surface->base.instance = instance;
   surface->base.destroy = x11_surface_destroy;
   surface->base.get_capabilities = x11_surface_get_capabilities;
   surface->base.get_formats = x11_surface_get_formats;
   surface->base.get_present_modes = x11_surface_get_present_modes;
   surface->base.create_swapchain = x11_surface_create_swapchain;

   *pSurface = anv_wsi_surface_to_handle(&surface->base);

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
x11_swapchain_destroy(struct anv_swapchain *anv_chain,
                      const VkAllocationCallbacks *pAllocator)
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

   anv_free(NULL /* XXX: pAllocator */, chain);

   return VK_SUCCESS;
}

static VkResult
x11_surface_create_swapchain(struct anv_wsi_surface *wsi_surface,
                             struct anv_device *device,
                             const VkSwapchainCreateInfoKHR *pCreateInfo,
                             const VkAllocationCallbacks* pAllocator,
                             struct anv_swapchain **swapchain_out)
{
   struct x11_surface *surface = (struct x11_surface *)wsi_surface;
   struct x11_swapchain *chain;
   xcb_void_cookie_t cookie;
   VkResult result;

   int num_images = pCreateInfo->minImageCount;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);
   chain = anv_alloc2(&device->alloc, pAllocator, size, 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   chain->base.device = device;
   chain->base.destroy = x11_swapchain_destroy;
   chain->base.get_images = x11_get_images;
   chain->base.acquire_next_image = x11_acquire_next_image;
   chain->base.queue_present = x11_queue_present;

   chain->conn = surface->connection;
   chain->window = surface->window;
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
            .isl_tiling_flags = ISL_TILING_X_BIT,
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
            .arrayLayers = 1,
            .samples = 1,
            /* FIXME: Need a way to use X tiling to allow scanout */
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .flags = 0,
         }},
         NULL,
         &image_h);

      image = anv_image_from_handle(image_h);
      assert(anv_format_is_color(image->format));

      surface = &image->color_surface;

      anv_AllocateMemory(anv_device_to_handle(device),
         &(VkMemoryAllocateInfo) {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = image->size,
            .memoryTypeIndex = 0,
         },
         NULL /* XXX: pAllocator */,
         &memory_h);

      memory = anv_device_memory_from_handle(memory_h);

      anv_BindImageMemory(VK_NULL_HANDLE, anv_image_to_handle(image),
                          memory_h, 0);

      int ret = anv_gem_set_tiling(device, memory->bo.gem_handle,
                                   surface->isl.row_pitch, I915_TILING_X);
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
                                             surface->isl.row_pitch,
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
   return VK_SUCCESS;
}

void
anv_x11_finish_wsi(struct anv_instance *instance)
{ }
