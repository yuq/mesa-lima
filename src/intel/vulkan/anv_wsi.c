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
#include "wsi_common.h"
#include "vk_format_info.h"
#include "vk_util.h"

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
#define WSI_CB(x) .x = anv_##x
static const struct wsi_callbacks wsi_cbs = {
   WSI_CB(GetPhysicalDeviceFormatProperties),
};
#endif

static PFN_vkVoidFunction
anv_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   ANV_FROM_HANDLE(anv_physical_device, physical_device, physicalDevice);
   return anv_lookup_entrypoint(&physical_device->info, pName);
}

static uint32_t
anv_wsi_queue_get_family_index(VkQueue queue)
{
   return 0;
}

VkResult
anv_init_wsi(struct anv_physical_device *physical_device)
{
   VkResult result;

   wsi_device_init(&physical_device->wsi_device,
                   anv_physical_device_to_handle(physical_device),
                   anv_wsi_proc_addr);

   physical_device->wsi_device.queue_get_family_index =
      anv_wsi_queue_get_family_index;

#ifdef VK_USE_PLATFORM_XCB_KHR
   result = wsi_x11_init_wsi(&physical_device->wsi_device, &physical_device->instance->alloc);
   if (result != VK_SUCCESS)
      return result;
#endif

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   result = wsi_wl_init_wsi(&physical_device->wsi_device, &physical_device->instance->alloc,
                            anv_physical_device_to_handle(physical_device),
                            &wsi_cbs);
   if (result != VK_SUCCESS) {
#ifdef VK_USE_PLATFORM_XCB_KHR
      wsi_x11_finish_wsi(&physical_device->wsi_device, &physical_device->instance->alloc);
#endif
      return result;
   }
#endif

   return VK_SUCCESS;
}

void
anv_finish_wsi(struct anv_physical_device *physical_device)
{
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   wsi_wl_finish_wsi(&physical_device->wsi_device, &physical_device->instance->alloc);
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
   wsi_x11_finish_wsi(&physical_device->wsi_device, &physical_device->instance->alloc);
#endif
}

void anv_DestroySurfaceKHR(
    VkInstance                                   _instance,
    VkSurfaceKHR                                 _surface,
    const VkAllocationCallbacks*                 pAllocator)
{
   ANV_FROM_HANDLE(anv_instance, instance, _instance);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);

   if (!surface)
      return;

   vk_free2(&instance->alloc, pAllocator, surface);
}

VkResult anv_GetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice                            physicalDevice,
    uint32_t                                    queueFamilyIndex,
    VkSurfaceKHR                                surface,
    VkBool32*                                   pSupported)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);

   return wsi_common_get_surface_support(&device->wsi_device,
                                         device->local_fd,
                                         queueFamilyIndex,
                                         surface,
                                         &device->instance->alloc,
                                         pSupported);
}

VkResult anv_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                surface,
    VkSurfaceCapabilitiesKHR*                   pSurfaceCapabilities)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);

   return wsi_common_get_surface_capabilities(&device->wsi_device,
                                              surface,
                                              pSurfaceCapabilities);
}

VkResult anv_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice                            physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR*      pSurfaceInfo,
    VkSurfaceCapabilities2KHR*                  pSurfaceCapabilities)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);

   return wsi_common_get_surface_capabilities2(&device->wsi_device,
                                               pSurfaceInfo,
                                               pSurfaceCapabilities);
}

VkResult anv_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                surface,
    uint32_t*                                   pSurfaceFormatCount,
    VkSurfaceFormatKHR*                         pSurfaceFormats)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);

   return wsi_common_get_surface_formats(&device->wsi_device, surface,
                                         pSurfaceFormatCount, pSurfaceFormats);
}

VkResult anv_GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice                            physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR*      pSurfaceInfo,
    uint32_t*                                   pSurfaceFormatCount,
    VkSurfaceFormat2KHR*                        pSurfaceFormats)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);

   return wsi_common_get_surface_formats2(&device->wsi_device, pSurfaceInfo,
                                          pSurfaceFormatCount, pSurfaceFormats);
}

VkResult anv_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                surface,
    uint32_t*                                   pPresentModeCount,
    VkPresentModeKHR*                           pPresentModes)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);

   return wsi_common_get_surface_present_modes(&device->wsi_device, surface,
                                               pPresentModeCount,
                                               pPresentModes);
}

VkResult anv_CreateSwapchainKHR(
    VkDevice                                     _device,
    const VkSwapchainCreateInfoKHR*              pCreateInfo,
    const VkAllocationCallbacks*                 pAllocator,
    VkSwapchainKHR*                              pSwapchain)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct wsi_device *wsi_device = &device->instance->physicalDevice.wsi_device;
   const VkAllocationCallbacks *alloc;

   if (pAllocator)
     alloc = pAllocator;
   else
     alloc = &device->alloc;

   return wsi_common_create_swapchain(wsi_device, _device, device->fd,
                                      pCreateInfo, alloc, pSwapchain);
}

void anv_DestroySwapchainKHR(
    VkDevice                                     _device,
    VkSwapchainKHR                               swapchain,
    const VkAllocationCallbacks*                 pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   const VkAllocationCallbacks *alloc;

   if (pAllocator)
     alloc = pAllocator;
   else
     alloc = &device->alloc;

   wsi_common_destroy_swapchain(_device, swapchain, alloc);
}

VkResult anv_GetSwapchainImagesKHR(
    VkDevice                                     device,
    VkSwapchainKHR                               swapchain,
    uint32_t*                                    pSwapchainImageCount,
    VkImage*                                     pSwapchainImages)
{
   return wsi_common_get_images(swapchain,
                                pSwapchainImageCount,
                                pSwapchainImages);
}

VkResult anv_AcquireNextImageKHR(
    VkDevice                                     _device,
    VkSwapchainKHR                               swapchain,
    uint64_t                                     timeout,
    VkSemaphore                                  semaphore,
    VkFence                                      fence,
    uint32_t*                                    pImageIndex)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_physical_device *pdevice = &device->instance->physicalDevice;

   VkResult result = wsi_common_acquire_next_image(&pdevice->wsi_device,
                                                   _device,
                                                   swapchain,
                                                   timeout,
                                                   semaphore,
                                                   pImageIndex);

   /* Thanks to implicit sync, the image is ready immediately.  However, we
    * should wait for the current GPU state to finish.
    */
   if (fence != VK_NULL_HANDLE)
      anv_QueueSubmit(anv_queue_to_handle(&device->queue), 0, NULL, fence);

   return result;
}

VkResult anv_QueuePresentKHR(
    VkQueue                                  _queue,
    const VkPresentInfoKHR*                  pPresentInfo)
{
   ANV_FROM_HANDLE(anv_queue, queue, _queue);
   struct anv_physical_device *pdevice =
      &queue->device->instance->physicalDevice;

   return wsi_common_queue_present(&pdevice->wsi_device,
                                   anv_device_to_handle(queue->device),
                                   _queue, 0,
                                   pPresentInfo);
}
