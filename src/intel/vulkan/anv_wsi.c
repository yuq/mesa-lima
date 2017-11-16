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
    VkSurfaceKHR                                _surface,
    VkBool32*                                   pSupported)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

   return iface->get_support(surface, &device->wsi_device,
                             &device->instance->alloc,
                             queueFamilyIndex, device->local_fd, false, pSupported);
}

VkResult anv_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                _surface,
    VkSurfaceCapabilitiesKHR*                   pSurfaceCapabilities)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

   return iface->get_capabilities(surface, pSurfaceCapabilities);
}

VkResult anv_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice                            physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR*      pSurfaceInfo,
    VkSurfaceCapabilities2KHR*                  pSurfaceCapabilities)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pSurfaceInfo->surface);
   struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

   return iface->get_capabilities2(surface, pSurfaceInfo->pNext,
                                   pSurfaceCapabilities);
}

VkResult anv_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                _surface,
    uint32_t*                                   pSurfaceFormatCount,
    VkSurfaceFormatKHR*                         pSurfaceFormats)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

   return iface->get_formats(surface, &device->wsi_device, pSurfaceFormatCount,
                             pSurfaceFormats);
}

VkResult anv_GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice                            physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR*      pSurfaceInfo,
    uint32_t*                                   pSurfaceFormatCount,
    VkSurfaceFormat2KHR*                        pSurfaceFormats)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pSurfaceInfo->surface);
   struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

   return iface->get_formats2(surface, &device->wsi_device, pSurfaceInfo->pNext,
                              pSurfaceFormatCount, pSurfaceFormats);
}

VkResult anv_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                _surface,
    uint32_t*                                   pPresentModeCount,
    VkPresentModeKHR*                           pPresentModes)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

   return iface->get_present_modes(surface, pPresentModeCount,
                                   pPresentModes);
}

VkResult anv_CreateSwapchainKHR(
    VkDevice                                     _device,
    const VkSwapchainCreateInfoKHR*              pCreateInfo,
    const VkAllocationCallbacks*                 pAllocator,
    VkSwapchainKHR*                              pSwapchain)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pCreateInfo->surface);
   struct wsi_interface *iface =
      device->instance->physicalDevice.wsi_device.wsi[surface->platform];
   struct wsi_swapchain *swapchain;
   const VkAllocationCallbacks *alloc;

   if (pAllocator)
     alloc = pAllocator;
   else
     alloc = &device->alloc;
   VkResult result = iface->create_swapchain(surface, _device,
                                             &device->instance->physicalDevice.wsi_device,
                                             device->instance->physicalDevice.local_fd,
                                             pCreateInfo,
                                             alloc,
                                             &swapchain);
   if (result != VK_SUCCESS)
      return result;

   swapchain->alloc = *alloc;

   for (unsigned i = 0; i < ARRAY_SIZE(swapchain->fences); i++)
      swapchain->fences[i] = VK_NULL_HANDLE;

   *pSwapchain = wsi_swapchain_to_handle(swapchain);

   return VK_SUCCESS;
}

void anv_DestroySwapchainKHR(
    VkDevice                                     _device,
    VkSwapchainKHR                               _swapchain,
    const VkAllocationCallbacks*                 pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);
   const VkAllocationCallbacks *alloc;

   if (!swapchain)
      return;

   if (pAllocator)
     alloc = pAllocator;
   else
     alloc = &device->alloc;
   for (unsigned i = 0; i < ARRAY_SIZE(swapchain->fences); i++) {
      if (swapchain->fences[i] != VK_NULL_HANDLE)
         anv_DestroyFence(_device, swapchain->fences[i], pAllocator);
   }

   swapchain->destroy(swapchain, alloc);
}

VkResult anv_GetSwapchainImagesKHR(
    VkDevice                                     device,
    VkSwapchainKHR                               _swapchain,
    uint32_t*                                    pSwapchainImageCount,
    VkImage*                                     pSwapchainImages)
{
   ANV_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);

   return swapchain->get_images(swapchain, pSwapchainImageCount,
                                pSwapchainImages);
}

VkResult anv_AcquireNextImageKHR(
    VkDevice                                     _device,
    VkSwapchainKHR                               _swapchain,
    uint64_t                                     timeout,
    VkSemaphore                                  semaphore,
    VkFence                                      _fence,
    uint32_t*                                    pImageIndex)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);
   ANV_FROM_HANDLE(anv_fence, fence, _fence);

   VkResult result = swapchain->acquire_next_image(swapchain, timeout,
                                                   semaphore, pImageIndex);

   /* Thanks to implicit sync, the image is ready immediately.  However, we
    * should wait for the current GPU state to finish.
    */
   if (fence)
      anv_QueueSubmit(anv_queue_to_handle(&device->queue), 0, NULL, _fence);

   return result;
}

VkResult anv_QueuePresentKHR(
    VkQueue                                  _queue,
    const VkPresentInfoKHR*                  pPresentInfo)
{
   ANV_FROM_HANDLE(anv_queue, queue, _queue);
   VkResult result = VK_SUCCESS;

   const VkPresentRegionsKHR *regions =
      vk_find_struct_const(pPresentInfo->pNext, PRESENT_REGIONS_KHR);

   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      ANV_FROM_HANDLE(wsi_swapchain, swapchain, pPresentInfo->pSwapchains[i]);
      VkResult item_result;

      const VkPresentRegionKHR *region = NULL;
      if (regions && regions->pRegions)
         region = &regions->pRegions[i];

      assert(anv_device_from_handle(swapchain->device) == queue->device);

      if (swapchain->fences[0] == VK_NULL_HANDLE) {
         item_result = anv_CreateFence(anv_device_to_handle(queue->device),
            &(VkFenceCreateInfo) {
               .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
               .flags = 0,
            }, &swapchain->alloc, &swapchain->fences[0]);
         if (pPresentInfo->pResults != NULL)
            pPresentInfo->pResults[i] = item_result;
         result = result == VK_SUCCESS ? item_result : result;
         if (item_result != VK_SUCCESS)
            continue;
      } else {
         anv_ResetFences(anv_device_to_handle(queue->device),
                         1, &swapchain->fences[0]);
      }

      anv_QueueSubmit(_queue, 0, NULL, swapchain->fences[0]);

      item_result = swapchain->queue_present(swapchain,
                                             _queue,
                                             pPresentInfo->waitSemaphoreCount,
                                             pPresentInfo->pWaitSemaphores,
                                             pPresentInfo->pImageIndices[i],
                                             region);
      /* TODO: What if one of them returns OUT_OF_DATE? */
      if (pPresentInfo->pResults != NULL)
         pPresentInfo->pResults[i] = item_result;
      result = result == VK_SUCCESS ? item_result : result;
      if (item_result != VK_SUCCESS)
            continue;

      VkFence last = swapchain->fences[2];
      swapchain->fences[2] = swapchain->fences[1];
      swapchain->fences[1] = swapchain->fences[0];
      swapchain->fences[0] = last;

      if (last != VK_NULL_HANDLE) {
         anv_WaitForFences(anv_device_to_handle(queue->device),
                           1, &last, true, 1);
      }
   }

   return VK_SUCCESS;
}
