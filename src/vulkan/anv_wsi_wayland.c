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

#include <wayland-client.h>
#include <wayland-drm-client-protocol.h>

#include "anv_wsi.h"

#include <util/hash_table.h>

#define MIN_NUM_IMAGES 2

struct wsi_wl_display {
   struct wl_display *                          display;
   struct wl_drm *                              drm;

   /* Vector of VkFormats supported */
   struct anv_vector                            formats;

   uint32_t                                     capabilities;
};

struct wsi_wayland {
   struct anv_wsi_implementation                base;

   struct anv_instance *                        instance;

    pthread_mutex_t                             mutex;
    /* Hash table of wl_display -> wsi_wl_display mappings */
    struct hash_table *                         displays;
};

static void
wsi_wl_display_add_vk_format(struct wsi_wl_display *display, VkFormat format)
{
   /* Don't add a format that's already in the list */
   VkFormat *f;
   anv_vector_foreach(f, &display->formats)
      if (*f == format)
         return;

   /* Don't add formats which aren't supported by the driver */
   if (anv_format_for_vk_format(format)->cpp == 0)
      return;

   f = anv_vector_add(&display->formats);
   if (f)
      *f = format;
}

static void
drm_handle_device(void *data, struct wl_drm *drm, const char *name)
{
   fprintf(stderr, "wl_drm.device(%s)\n", name);
}

static uint32_t
wl_drm_format_for_vk_format(VkFormat vk_format, bool alpha)
{
   switch (vk_format) {
   case VK_FORMAT_R4G4B4A4_UNORM:
      return alpha ? WL_DRM_FORMAT_ABGR4444 : WL_DRM_FORMAT_XBGR4444;
   case VK_FORMAT_R5G6B5_UNORM:
      return WL_DRM_FORMAT_BGR565;
   case VK_FORMAT_R5G5B5A1_UNORM:
      return alpha ? WL_DRM_FORMAT_ABGR1555 : WL_DRM_FORMAT_XBGR1555;
   case VK_FORMAT_R8G8B8_UNORM:
      return WL_DRM_FORMAT_XBGR8888;
   case VK_FORMAT_R8G8B8A8_UNORM:
      return alpha ? WL_DRM_FORMAT_ABGR8888 : WL_DRM_FORMAT_XBGR8888;
   case VK_FORMAT_R10G10B10A2_UNORM:
      return alpha ? WL_DRM_FORMAT_ABGR2101010 : WL_DRM_FORMAT_XBGR2101010;
   case VK_FORMAT_B4G4R4A4_UNORM:
      return alpha ? WL_DRM_FORMAT_ARGB4444 : WL_DRM_FORMAT_XRGB4444;
   case VK_FORMAT_B5G6R5_UNORM:
      return WL_DRM_FORMAT_RGB565;
   case VK_FORMAT_B5G5R5A1_UNORM:
      return alpha ? WL_DRM_FORMAT_XRGB1555 : WL_DRM_FORMAT_XRGB1555;
   case VK_FORMAT_B8G8R8_UNORM:
      return WL_DRM_FORMAT_BGRX8888;
   case VK_FORMAT_B8G8R8A8_UNORM:
      return alpha ? WL_DRM_FORMAT_ARGB8888 : WL_DRM_FORMAT_XRGB8888;
   case VK_FORMAT_B10G10R10A2_UNORM:
      return alpha ? WL_DRM_FORMAT_ARGB2101010 : WL_DRM_FORMAT_XRGB2101010;

   default:
      assert("!Unsupported Vulkan format");
      return 0;
   }
}

static void
drm_handle_format(void *data, struct wl_drm *drm, uint32_t wl_format)
{
   struct wsi_wl_display *display = data;

   switch (wl_format) {
   case WL_DRM_FORMAT_ABGR4444:
   case WL_DRM_FORMAT_XBGR4444:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_R4G4B4A4_UNORM);
      break;
   case WL_DRM_FORMAT_BGR565:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_R5G6B5_UNORM);
      break;
   case WL_DRM_FORMAT_ABGR1555:
   case WL_DRM_FORMAT_XBGR1555:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_R5G5B5A1_UNORM);
      break;
   case WL_DRM_FORMAT_XBGR8888:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_R8G8B8_UNORM);
      /* fallthrough */
   case WL_DRM_FORMAT_ABGR8888:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_R8G8B8A8_UNORM);
      break;
   case WL_DRM_FORMAT_ABGR2101010:
   case WL_DRM_FORMAT_XBGR2101010:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_R10G10B10A2_UNORM);
      break;
   case WL_DRM_FORMAT_ARGB4444:
   case WL_DRM_FORMAT_XRGB4444:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_B4G4R4A4_UNORM);
      break;
   case WL_DRM_FORMAT_RGB565:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_B5G6R5_UNORM);
      break;
   case WL_DRM_FORMAT_ARGB1555:
   case WL_DRM_FORMAT_XRGB1555:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_B5G5R5A1_UNORM);
      break;
   case WL_DRM_FORMAT_XRGB8888:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_B8G8R8_UNORM);
      /* fallthrough */
   case WL_DRM_FORMAT_ARGB8888:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_B8G8R8A8_UNORM);
      break;
   case WL_DRM_FORMAT_ARGB2101010:
   case WL_DRM_FORMAT_XRGB2101010:
      wsi_wl_display_add_vk_format(display, VK_FORMAT_B10G10R10A2_UNORM);
      break;
   }
}

static void
drm_handle_authenticated(void *data, struct wl_drm *drm)
{
}

static void
drm_handle_capabilities(void *data, struct wl_drm *drm, uint32_t capabilities)
{
   struct wsi_wl_display *display = data;

   display->capabilities = capabilities;
}

static const struct wl_drm_listener drm_listener = {
   drm_handle_device,
   drm_handle_format,
   drm_handle_authenticated,
   drm_handle_capabilities,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
                       uint32_t name, const char *interface, uint32_t version)
{
   struct wsi_wl_display *display = data;

   if (strcmp(interface, "wl_drm") == 0) {
      assert(display->drm == NULL);

      assert(version >= 2);
      display->drm = wl_registry_bind(registry, name, &wl_drm_interface, 2);

      if (display->drm)
         wl_drm_add_listener(display->drm, &drm_listener, display);
   }
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
                              uint32_t name)
{ /* No-op */ }

static const struct wl_registry_listener registry_listener = {
   registry_handle_global,
   registry_handle_global_remove
};

static void
wsi_wl_display_destroy(struct wsi_wayland *wsi, struct wsi_wl_display *display)
{
   anv_vector_finish(&display->formats);
   if (display->drm)
      wl_drm_destroy(display->drm);
   anv_instance_free(wsi->instance, display);
}

static struct wsi_wl_display *
wsi_wl_display_create(struct wsi_wayland *wsi, struct wl_display *wl_display)
{
   struct wsi_wl_display *display =
      anv_instance_alloc(wsi->instance, sizeof(*display), 8,
                         VK_SYSTEM_ALLOC_TYPE_INTERNAL);
   if (!display)
      return NULL;

   memset(display, 0, sizeof(*display));

   display->display = wl_display;

   if (!anv_vector_init(&display->formats, sizeof(VkFormat), 8))
      goto fail;

   struct wl_registry *registry = wl_display_get_registry(wl_display);
   if (!registry)
      return NULL;

   wl_registry_add_listener(registry, &registry_listener, display);

   /* Round-rip to get the wl_drm global */
   wl_display_roundtrip(wl_display);

   if (!display->drm)
      goto fail;

   /* Round-rip to get wl_drm formats and capabilities */
   wl_display_roundtrip(wl_display);

   /* We need prime support */
   if (!(display->capabilities & WL_DRM_CAPABILITY_PRIME))
      goto fail;

   /* We don't need this anymore */
   wl_registry_destroy(registry);

   return display;

fail:
   if (registry)
      wl_registry_destroy(registry);

   wsi_wl_display_destroy(wsi, display);
   return NULL;
}

static struct wsi_wl_display *
wsi_wl_get_display(struct wsi_wayland *wsi, struct wl_display *wl_display)
{
   pthread_mutex_lock(&wsi->mutex);

   struct hash_entry *entry = _mesa_hash_table_search(wsi->displays,
                                                      wl_display);
   if (!entry) {
      /* We're about to make a bunch of blocking calls.  Let's drop the
       * mutex for now so we don't block up too badly.
       */
      pthread_mutex_unlock(&wsi->mutex);

      struct wsi_wl_display *display = wsi_wl_display_create(wsi, wl_display);

      pthread_mutex_lock(&wsi->mutex);

      entry = _mesa_hash_table_search(wsi->displays, wl_display);
      if (entry) {
         /* Oops, someone raced us to it */
         wsi_wl_display_destroy(wsi, display);
      } else {
         entry = _mesa_hash_table_insert(wsi->displays, wl_display, display);
      }
   }

   pthread_mutex_unlock(&wsi->mutex);

   return entry->data;
}

static VkResult
wsi_wl_get_window_supported(struct anv_wsi_implementation *impl,
                            struct anv_physical_device *physical_device,
                            const VkSurfaceDescriptionWindowWSI *window,
                            VkBool32 *pSupported)
{
   struct wsi_wayland *wsi = (struct wsi_wayland *)impl;

   *pSupported = wsi_wl_get_display(wsi, window->pPlatformHandle) != NULL;

   return VK_SUCCESS;
}

static const VkSurfacePresentModePropertiesWSI present_modes[] = {
   { VK_PRESENT_MODE_MAILBOX_WSI },
   { VK_PRESENT_MODE_FIFO_WSI },
};

static VkResult
wsi_wl_get_surface_info(struct anv_wsi_implementation *impl,
                        struct anv_device *device,
                        VkSurfaceDescriptionWindowWSI *window,
                        VkSurfaceInfoTypeWSI infoType,
                        size_t* pDataSize, void* pData)
{
   struct wsi_wayland *wsi = (struct wsi_wayland *)impl;

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

      props->minImageCount = MIN_NUM_IMAGES;
      props->maxImageCount = 4;
      props->currentExtent = (VkExtent2D) { -1, -1 };
      props->minImageExtent = (VkExtent2D) { 1, 1 };
      props->maxImageExtent = (VkExtent2D) { INT16_MAX, INT16_MAX };
      props->supportedTransforms = VK_SURFACE_TRANSFORM_NONE_BIT_WSI;
      props->currentTransform = VK_SURFACE_TRANSFORM_NONE_WSI;
      props->maxImageArraySize = 1;
      props->supportedUsageFlags =
         VK_IMAGE_USAGE_TRANSFER_DESTINATION_BIT |
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

      return VK_SUCCESS;
   }

   case VK_SURFACE_INFO_TYPE_FORMATS_WSI: {
      VkSurfaceFormatPropertiesWSI *formats = pData;

      struct wsi_wl_display *display =
         wsi_wl_get_display(wsi, window->pPlatformHandle);

      uint32_t size = anv_vector_length(&display->formats) * sizeof(*formats);

      if (pData == NULL) {
         *pDataSize = size;
         return VK_SUCCESS;
      }

      assert(*pDataSize >= size);
      *pDataSize = size;

      VkFormat *f;
      anv_vector_foreach(f, &display->formats)
         (formats++)->format = *f;

      return VK_SUCCESS;
   }

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

struct wsi_wl_image {
   struct anv_image *                           image;
   struct anv_device_memory *                   memory;
   struct wl_buffer *                           buffer;
   bool                                         busy;
};

struct wsi_wl_swap_chain {
   struct anv_swap_chain                        base;

   struct wsi_wl_display *                      display;
   struct wl_event_queue *                      queue;
   struct wl_surface *                          surface;

   VkExtent2D                                   extent;
   VkFormat                                     vk_format;
   uint32_t                                     drm_format;

   VkPresentModeWSI                             present_mode;
   bool                                         fifo_ready;

   uint32_t                                     image_count;
   struct wsi_wl_image                          images[0];
};

static VkResult
wsi_wl_get_swap_chain_info(struct anv_swap_chain *anv_chain,
                           VkSwapChainInfoTypeWSI infoType,
                           size_t* pDataSize, void* pData)
{
   struct wsi_wl_swap_chain *chain = (struct wsi_wl_swap_chain *)anv_chain;
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
wsi_wl_acquire_next_image(struct anv_swap_chain *anv_chain,
                          uint64_t timeout,
                          VkSemaphore semaphore,
                          uint32_t *image_index)
{
   struct wsi_wl_swap_chain *chain = (struct wsi_wl_swap_chain *)anv_chain;

   int ret = wl_display_dispatch_queue_pending(chain->display->display,
                                               chain->queue);
   /* XXX: I'm not sure if out-of-date is the right error here.  If
    * wl_display_dispatch_queue_pending fails it most likely means we got
    * kicked by the server so this seems more-or-less correct.
    */
   if (ret < 0)
      return vk_error(VK_ERROR_OUT_OF_DATE_WSI);

   while (1) {
      for (uint32_t i = 0; i < chain->image_count; i++) {
         if (!chain->images[i].busy) {
            /* We found a non-busy image */
            *image_index = i;
            return VK_SUCCESS;
         }
      }

      /* This time we do a blocking dispatch because we can't go
       * anywhere until we get an event.
       */
      int ret = wl_display_dispatch_queue(chain->display->display,
                                          chain->queue);
      if (ret < 0)
         return vk_error(VK_ERROR_OUT_OF_DATE_WSI);
   }
}

static void
frame_handle_done(void *data, struct wl_callback *callback, uint32_t serial)
{
   struct wsi_wl_swap_chain *chain = data;

   chain->fifo_ready = true;

   wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {
   frame_handle_done,
};

static VkResult
wsi_wl_queue_present(struct anv_swap_chain *anv_chain,
                     struct anv_queue *queue,
                     uint32_t image_index)
{
   struct wsi_wl_swap_chain *chain = (struct wsi_wl_swap_chain *)anv_chain;

   if (chain->present_mode == VK_PRESENT_MODE_FIFO_WSI) {
      while (!chain->fifo_ready) {
         int ret = wl_display_dispatch_queue(chain->display->display,
                                             chain->queue);
         if (ret < 0)
            return vk_error(VK_ERROR_OUT_OF_DATE_WSI);
      }
   }

   assert(image_index < chain->image_count);
   wl_surface_attach(chain->surface, chain->images[image_index].buffer, 0, 0);
   wl_surface_damage(chain->surface, 0, 0, INT32_MAX, INT32_MAX);

   if (chain->present_mode == VK_PRESENT_MODE_FIFO_WSI) {
      struct wl_callback *frame = wl_surface_frame(chain->surface);
      wl_proxy_set_queue((struct wl_proxy *)frame, chain->queue);
      wl_callback_add_listener(frame, &frame_listener, chain);
      chain->fifo_ready = false;
   }

   wl_surface_commit(chain->surface);
   wl_display_flush(chain->display->display);

   return VK_SUCCESS;
}

static void
wsi_wl_image_finish(struct wsi_wl_swap_chain *chain, struct wsi_wl_image *image)
{
   VkDevice vk_device = anv_device_to_handle(chain->base.device);
   anv_FreeMemory(vk_device, anv_device_memory_to_handle(image->memory));
   anv_DestroyImage(vk_device, anv_image_to_handle(image->image));
}

static void
buffer_handle_release(void *data, struct wl_buffer *buffer)
{
   struct wsi_wl_image *image = data;

   assert(image->buffer == buffer);

   image->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
   buffer_handle_release,
};

static VkResult
wsi_wl_image_init(struct wsi_wl_swap_chain *chain, struct wsi_wl_image *image)
{
   VkDevice vk_device = anv_device_to_handle(chain->base.device);
   VkResult result;

   VkImage vk_image;
   result = anv_image_create(vk_device,
      &(struct anv_image_create_info) {
         .force_tile_mode = true,
         .tile_mode = XMAJOR,
         .stride = 0,
         .vk_info =
      &(VkImageCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = chain->vk_format,
         .extent = {
            .width = chain->extent.width,
            .height = chain->extent.height,
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
      &vk_image);

   if (result != VK_SUCCESS)
      return result;

   image->image = anv_image_from_handle(vk_image);
   assert(anv_format_is_color(image->image->format));

   struct anv_surface *surface = &image->image->color_surface;

   VkDeviceMemory vk_memory;
   result = anv_AllocMemory(vk_device,
      &(VkMemoryAllocInfo) {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO,
         .allocationSize = image->image->size,
         .memoryTypeIndex = 0,
      },
      &vk_memory);

   if (result != VK_SUCCESS)
      goto fail_image;

   image->memory = anv_device_memory_from_handle(vk_memory);

   result = anv_BindImageMemory(vk_device, vk_image, vk_memory, 0);

   if (result != VK_SUCCESS)
      goto fail_mem;

   int ret = anv_gem_set_tiling(chain->base.device,
                                image->memory->bo.gem_handle,
                                surface->stride, I915_TILING_X);
   if (ret) {
      result = vk_error(VK_ERROR_UNKNOWN);
      goto fail_mem;
   }

   int fd = anv_gem_handle_to_fd(chain->base.device,
                                 image->memory->bo.gem_handle);
   if (fd == -1) {
      result = vk_error(VK_ERROR_UNKNOWN);
      goto fail_mem;
   }

   image->buffer = wl_drm_create_prime_buffer(chain->display->drm,
                                              fd, /* name */
                                              chain->extent.width,
                                              chain->extent.height,
                                              chain->drm_format,
                                              surface->offset,
                                              surface->stride,
                                              0, 0, 0, 0 /* unused */);
   wl_display_roundtrip(chain->display->display);
   close(fd);

   wl_proxy_set_queue((struct wl_proxy *)image->buffer, chain->queue);
   wl_buffer_add_listener(image->buffer, &buffer_listener, image);

   return VK_SUCCESS;

fail_mem:
   anv_FreeMemory(vk_device, vk_memory);
fail_image:
   anv_DestroyImage(vk_device, vk_image);

   return result;
}

static VkResult
wsi_wl_destroy_swap_chain(struct anv_swap_chain *anv_chain)
{
   struct wsi_wl_swap_chain *chain = (struct wsi_wl_swap_chain *)anv_chain;

   for (uint32_t i = 0; i < chain->image_count; i++) {
      if (chain->images[i].buffer)
         wsi_wl_image_finish(chain, &chain->images[i]);
   }

   anv_device_free(chain->base.device, chain);

   return VK_SUCCESS;
}

static VkResult
wsi_wl_create_swap_chain(struct anv_wsi_implementation *impl,
                         struct anv_device *device,
                         const VkSwapChainCreateInfoWSI *pCreateInfo,
                         struct anv_swap_chain **swap_chain_out)
{
   struct wsi_wayland *wsi = (struct wsi_wayland *)impl;
   struct wsi_wl_swap_chain *chain;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAP_CHAIN_CREATE_INFO_WSI);

   assert(pCreateInfo->pSurfaceDescription->sType ==
          VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_WSI);
   VkSurfaceDescriptionWindowWSI *vk_window =
      (VkSurfaceDescriptionWindowWSI *)pCreateInfo->pSurfaceDescription;
   assert(vk_window->platform == VK_PLATFORM_WAYLAND_WSI);

   int num_images = pCreateInfo->minImageCount;

   assert(num_images >= MIN_NUM_IMAGES);

   /* For true mailbox mode, we need at least 4 images:
    *  1) One to scan out from
    *  2) One to have queued for scan-out
    *  3) One to be currently held by the Wayland compositor
    *  4) One to render to
    */
   if (pCreateInfo->presentMode == VK_PRESENT_MODE_MAILBOX_WSI)
      num_images = MAX2(num_images, 4);

   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);
   chain = anv_device_alloc(device, size, 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (chain == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   chain->base.device = device;
   chain->base.destroy = wsi_wl_destroy_swap_chain;
   chain->base.get_swap_chain_info = wsi_wl_get_swap_chain_info;
   chain->base.acquire_next_image = wsi_wl_acquire_next_image;
   chain->base.queue_present = wsi_wl_queue_present;

   chain->surface = vk_window->pPlatformWindow;
   chain->extent = pCreateInfo->imageExtent;
   chain->vk_format = pCreateInfo->imageFormat;
   chain->drm_format = wl_drm_format_for_vk_format(chain->vk_format, false);

   chain->present_mode = pCreateInfo->presentMode;
   chain->fifo_ready = true;

   chain->image_count = num_images;

   /* Mark a bunch of stuff as NULL.  This way we can just call
    * destroy_swapchain for cleanup.
    */
   for (uint32_t i = 0; i < chain->image_count; i++)
      chain->images[i].buffer = NULL;
   chain->queue = NULL;

   chain->display = wsi_wl_get_display(wsi, vk_window->pPlatformHandle);
   if (!chain->display)
      goto fail;

   chain->queue = wl_display_create_queue(chain->display->display);
   if (!chain->queue)
      goto fail;

   for (uint32_t i = 0; i < chain->image_count; i++) {
      result = wsi_wl_image_init(chain, &chain->images[i]);
      if (result != VK_SUCCESS)
         goto fail;
      chain->images[i].busy = false;
   }

   *swap_chain_out = &chain->base;

   return VK_SUCCESS;

fail:
   wsi_wl_destroy_swap_chain(&chain->base);

   return result;
}

VkResult
anv_wl_init_wsi(struct anv_instance *instance)
{
   struct wsi_wayland *wsi;
   VkResult result;

   wsi = anv_instance_alloc(instance, sizeof(*wsi), 8,
                            VK_SYSTEM_ALLOC_TYPE_INTERNAL);
   if (!wsi)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   wsi->base.get_window_supported = wsi_wl_get_window_supported;
   wsi->base.get_surface_info = wsi_wl_get_surface_info;
   wsi->base.create_swap_chain = wsi_wl_create_swap_chain;

   wsi->instance = instance;

   int ret = pthread_mutex_init(&wsi->mutex, NULL);
   if (ret != 0) {
      result = (ret == ENOMEM) ? VK_ERROR_OUT_OF_HOST_MEMORY :
                                 VK_ERROR_UNKNOWN;
      goto fail_alloc;
   }

   wsi->displays = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                                           _mesa_key_pointer_equal);
   if (!wsi->displays) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_mutex;
   }

   instance->wsi_impl[VK_PLATFORM_WAYLAND_WSI] = &wsi->base;

   return VK_SUCCESS;

fail_mutex:
   pthread_mutex_destroy(&wsi->mutex);

fail_alloc:
   anv_instance_free(instance, wsi);

   return result;
}

void
anv_wl_finish_wsi(struct anv_instance *instance)
{
   struct wsi_wayland *wsi =
      (struct wsi_wayland *)instance->wsi_impl[VK_PLATFORM_WAYLAND_WSI];

   _mesa_hash_table_destroy(wsi->displays, NULL);

   pthread_mutex_destroy(&wsi->mutex);

   anv_instance_free(instance, wsi);
}
