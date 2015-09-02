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
anv_GetPhysicalDeviceSurfaceSupportWSI(
    VkPhysicalDevice                        physicalDevice,
    uint32_t                                queueFamilyIndex,
    const VkSurfaceDescriptionWSI*          pSurfaceDescription,
    VkBool32*                               pSupported)
{
   assert(pSurfaceDescription->sType ==
          VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_WSI);

   VkSurfaceDescriptionWindowWSI *window = (void *)pSurfaceDescription;

   *pSupported = window->platform == VK_PLATFORM_XCB_WSI;

   return VK_SUCCESS;
}

VkResult
anv_GetSurfaceInfoWSI(
    VkDevice                                 _device,
    const VkSurfaceDescriptionWSI*           pSurfaceDescription,
    VkSurfaceInfoTypeWSI                     infoType,
    size_t*                                  pDataSize,
    void*                                    pData)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   assert(pSurfaceDescription->sType ==
          VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_WSI);
   VkSurfaceDescriptionWindowWSI *window =
      (VkSurfaceDescriptionWindowWSI *)pSurfaceDescription;

   switch (window->platform) {
   case VK_PLATFORM_XCB_WSI:
      return anv_x11_get_surface_info(device, window, infoType,
                                      pDataSize, pData);
   default:
      return vk_error(VK_ERROR_INVALID_VALUE);
   }
}

VkResult
anv_CreateSwapChainWSI(
    VkDevice                                 _device,
    const VkSwapChainCreateInfoWSI*          pCreateInfo,
    VkSwapChainWSI*                          pSwapChain)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_swap_chain *swap_chain;
   VkResult result;

   assert(pCreateInfo->pSurfaceDescription->sType ==
          VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_WSI);
   VkSurfaceDescriptionWindowWSI *window =
      (VkSurfaceDescriptionWindowWSI *)pCreateInfo->pSurfaceDescription;

   switch (window->platform) {
   case VK_PLATFORM_XCB_WSI:
      result = anv_x11_create_swap_chain(device, pCreateInfo,
                                         (void *)&swap_chain);
      break;
   default:
      return vk_error(VK_ERROR_INVALID_VALUE);
   }

   if (result == VK_SUCCESS)
      *pSwapChain = anv_swap_chain_to_handle(swap_chain);

   return result;
}

VkResult
anv_DestroySwapChainWSI(
    VkDevice                                 device,
    VkSwapChainWSI                           swapChain)
{
   ANV_FROM_HANDLE(anv_swap_chain, swap_chain, swapChain);

   assert(swap_chain->device == anv_device_from_handle(device));

   switch (swap_chain->type) {
   case ANV_SWAP_CHAIN_TYPE_X11:
      return anv_x11_destroy_swap_chain((void *)swap_chain);
   default:
      return vk_error(VK_ERROR_INVALID_VALUE);
   }
}

VkResult
anv_GetSwapChainInfoWSI(
    VkDevice                                 device,
    VkSwapChainWSI                           swapChain,
    VkSwapChainInfoTypeWSI                   infoType,
    size_t*                                  pDataSize,
    void*                                    pData)
{
   ANV_FROM_HANDLE(anv_swap_chain, swap_chain, swapChain);

   assert(swap_chain->device == anv_device_from_handle(device));

   switch (swap_chain->type) {
   case ANV_SWAP_CHAIN_TYPE_X11:
      return anv_x11_get_swap_chain_info((void *)swap_chain,
                                         infoType, pDataSize, pData);
   default:
      return vk_error(VK_ERROR_INVALID_VALUE);
   }
}

VkResult
anv_AcquireNextImageWSI(
    VkDevice                                 device,
    VkSwapChainWSI                           swapChain,
    uint64_t                                 timeout,
    VkSemaphore                              semaphore,
    uint32_t*                                pImageIndex)
{
   ANV_FROM_HANDLE(anv_swap_chain, swap_chain, swapChain);

   assert(swap_chain->device == anv_device_from_handle(device));

   switch (swap_chain->type) {
   case ANV_SWAP_CHAIN_TYPE_X11:
      return anv_x11_acquire_next_image((void *)swap_chain,
                                        timeout, semaphore, pImageIndex);
   default:
      return vk_error(VK_ERROR_INVALID_VALUE);
   }
}

VkResult
anv_QueuePresentWSI(
    VkQueue                                  _queue,
    VkPresentInfoWSI*                        pPresentInfo)
{
   ANV_FROM_HANDLE(anv_queue, queue, _queue);
   VkResult result;

   for (uint32_t i = 0; i < pPresentInfo->swapChainCount; i++) {
      ANV_FROM_HANDLE(anv_swap_chain, swap_chain, pPresentInfo->swapChains[i]);

      assert(swap_chain->device == queue->device);

      switch (swap_chain->type) {
      case ANV_SWAP_CHAIN_TYPE_X11:
         result = anv_x11_queue_present(queue, (void *)swap_chain,
                                        pPresentInfo->imageIndices[i]);
         /* TODO: What if one of them returns OUT_OF_DATE? */
         if (result != VK_SUCCESS)
            return result;
         else
            continue;

      default:
         return vk_error(VK_ERROR_INVALID_VALUE);
      }
   }

   return VK_SUCCESS;
}
