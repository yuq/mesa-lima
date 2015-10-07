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

#include "anv_wsi.h"

VkResult
anv_init_wsi(struct anv_instance *instance)
{
   VkResult result;

   memset(instance->wsi_impl, 0, sizeof(instance->wsi_impl));

   result = anv_x11_init_wsi(instance);
   if (result != VK_SUCCESS)
      return result;

#ifdef HAVE_WAYLAND_PLATFORM
   result = anv_wl_init_wsi(instance);
   if (result != VK_SUCCESS) {
      anv_x11_finish_wsi(instance);
      return result;
   }
#endif

   return VK_SUCCESS;
}

void
anv_finish_wsi(struct anv_instance *instance)
{
#ifdef HAVE_WAYLAND_PLATFORM
   anv_wl_finish_wsi(instance);
#endif
   anv_x11_finish_wsi(instance);
}

VkResult
anv_GetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice                        physicalDevice,
    uint32_t                                queueFamilyIndex,
    const VkSurfaceDescriptionKHR*          pSurfaceDescription,
    VkBool32*                               pSupported)
{
   ANV_FROM_HANDLE(anv_physical_device, physical_device, physicalDevice);

   assert(pSurfaceDescription->sType ==
          VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_KHR);

   VkSurfaceDescriptionWindowKHR *window = (void *)pSurfaceDescription;

   struct anv_wsi_implementation *impl =
      physical_device->instance->wsi_impl[window->platform];

   if (impl) {
      return impl->get_window_supported(impl, physical_device,
                                        window, pSupported);
   } else {
      *pSupported = false;
      return VK_SUCCESS;
   }
}

VkResult
anv_GetSurfacePropertiesKHR(
    VkDevice                                 _device,
    const VkSurfaceDescriptionKHR*           pSurfaceDescription,
    VkSurfacePropertiesKHR*                  pSurfaceProperties)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   assert(pSurfaceDescription->sType ==
          VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_KHR);
   VkSurfaceDescriptionWindowKHR *window =
      (VkSurfaceDescriptionWindowKHR *)pSurfaceDescription;

   struct anv_wsi_implementation *impl =
      device->instance->wsi_impl[window->platform];

   assert(impl);

   return impl->get_surface_properties(impl, device, window,
                                       pSurfaceProperties);
}

VkResult
anv_GetSurfaceFormatsKHR(
    VkDevice                                 _device,
    const VkSurfaceDescriptionKHR*           pSurfaceDescription,
    uint32_t*                                pCount,
    VkSurfaceFormatKHR*                      pSurfaceFormats)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   assert(pSurfaceDescription->sType ==
          VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_KHR);
   VkSurfaceDescriptionWindowKHR *window =
      (VkSurfaceDescriptionWindowKHR *)pSurfaceDescription;

   struct anv_wsi_implementation *impl =
      device->instance->wsi_impl[window->platform];

   assert(impl);

   return impl->get_surface_formats(impl, device, window,
                                    pCount, pSurfaceFormats);
}

VkResult
anv_CreateSwapchainKHR(
    VkDevice                                 _device,
    const VkSwapchainCreateInfoKHR*          pCreateInfo,
    VkSwapchainKHR*                          pSwapchain)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_swapchain *swapchain;
   VkResult result;

   assert(pCreateInfo->pSurfaceDescription->sType ==
          VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_KHR);
   VkSurfaceDescriptionWindowKHR *window =
      (VkSurfaceDescriptionWindowKHR *)pCreateInfo->pSurfaceDescription;

   struct anv_wsi_implementation *impl =
      device->instance->wsi_impl[window->platform];

   assert(impl);

   result = impl->create_swapchain(impl, device, pCreateInfo, &swapchain);

   if (result == VK_SUCCESS)
      *pSwapchain = anv_swapchain_to_handle(swapchain);

   return result;
}

VkResult
anv_DestroySwapchainKHR(
    VkDevice                                 device,
    VkSwapchainKHR                           swapChain)
{
   ANV_FROM_HANDLE(anv_swapchain, swapchain, swapChain);

   assert(swapchain->device == anv_device_from_handle(device));

   return swapchain->destroy(swapchain);
}

VkResult
anv_GetSwapchainImagesKHR(
    VkDevice                                 device,
    VkSwapchainKHR                           _swapchain,
    uint32_t*                                pCount,
    VkImage*                                 pSwapchainImages)
{
   ANV_FROM_HANDLE(anv_swapchain, swapchain, _swapchain);

   assert(swapchain->device == anv_device_from_handle(device));

   return swapchain->get_images(swapchain, pCount, pSwapchainImages);
}

VkResult
anv_AcquireNextImageKHR(
    VkDevice                                 device,
    VkSwapchainKHR                           _swapchain,
    uint64_t                                 timeout,
    VkSemaphore                              semaphore,
    uint32_t*                                pImageIndex)
{
   ANV_FROM_HANDLE(anv_swapchain, swapchain, _swapchain);

   assert(swapchain->device == anv_device_from_handle(device));

   return swapchain->acquire_next_image(swapchain,
                                        timeout, semaphore, pImageIndex);
}

VkResult
anv_QueuePresentKHR(
    VkQueue                                  _queue,
    VkPresentInfoKHR*                        pPresentInfo)
{
   ANV_FROM_HANDLE(anv_queue, queue, _queue);
   VkResult result;

   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      ANV_FROM_HANDLE(anv_swapchain, swapchain, pPresentInfo->swapchains[i]);

      assert(swapchain->device == queue->device);

      result = swapchain->queue_present(swapchain, queue,
                                        pPresentInfo->imageIndices[i]);
      /* TODO: What if one of them returns OUT_OF_DATE? */
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}
