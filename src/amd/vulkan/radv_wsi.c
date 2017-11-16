/*
 * Copyright © 2016 Red Hat
 * based on intel anv code:
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

#include "radv_private.h"
#include "radv_meta.h"
#include "wsi_common.h"
#include "vk_util.h"
#include "util/macros.h"

#define WSI_CB(x) .x = radv_##x
MAYBE_UNUSED static const struct wsi_callbacks wsi_cbs = {
	WSI_CB(GetPhysicalDeviceFormatProperties),
};

static PFN_vkVoidFunction
radv_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
	return radv_lookup_entrypoint(pName);
}

static uint32_t
radv_wsi_queue_get_family_index(VkQueue _queue)
{
	RADV_FROM_HANDLE(radv_queue, queue, _queue);
	return queue->queue_family_index;
}

VkResult
radv_init_wsi(struct radv_physical_device *physical_device)
{
	VkResult result;

	wsi_device_init(&physical_device->wsi_device,
			radv_physical_device_to_handle(physical_device),
			radv_wsi_proc_addr);

	physical_device->wsi_device.queue_get_family_index =
		radv_wsi_queue_get_family_index;

#ifdef VK_USE_PLATFORM_XCB_KHR
	result = wsi_x11_init_wsi(&physical_device->wsi_device, &physical_device->instance->alloc);
	if (result != VK_SUCCESS)
		return result;
#endif

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
	result = wsi_wl_init_wsi(&physical_device->wsi_device, &physical_device->instance->alloc,
				 radv_physical_device_to_handle(physical_device),
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
radv_finish_wsi(struct radv_physical_device *physical_device)
{
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
	wsi_wl_finish_wsi(&physical_device->wsi_device, &physical_device->instance->alloc);
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
	wsi_x11_finish_wsi(&physical_device->wsi_device, &physical_device->instance->alloc);
#endif
}

void radv_DestroySurfaceKHR(
	VkInstance                                   _instance,
	VkSurfaceKHR                                 _surface,
	const VkAllocationCallbacks*                 pAllocator)
{
	RADV_FROM_HANDLE(radv_instance, instance, _instance);
	ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);

	vk_free2(&instance->alloc, pAllocator, surface);
}

VkResult radv_GetPhysicalDeviceSurfaceSupportKHR(
	VkPhysicalDevice                            physicalDevice,
	uint32_t                                    queueFamilyIndex,
	VkSurfaceKHR                                _surface,
	VkBool32*                                   pSupported)
{
	RADV_FROM_HANDLE(radv_physical_device, device, physicalDevice);
	ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
	struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

	return iface->get_support(surface, &device->wsi_device,
				  &device->instance->alloc,
				  queueFamilyIndex, device->local_fd, true, pSupported);
}

VkResult radv_GetPhysicalDeviceSurfaceCapabilitiesKHR(
	VkPhysicalDevice                            physicalDevice,
	VkSurfaceKHR                                _surface,
	VkSurfaceCapabilitiesKHR*                   pSurfaceCapabilities)
{
	RADV_FROM_HANDLE(radv_physical_device, device, physicalDevice);
	ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
	struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

	return iface->get_capabilities(surface, pSurfaceCapabilities);
}

VkResult radv_GetPhysicalDeviceSurfaceFormatsKHR(
	VkPhysicalDevice                            physicalDevice,
	VkSurfaceKHR                                _surface,
	uint32_t*                                   pSurfaceFormatCount,
	VkSurfaceFormatKHR*                         pSurfaceFormats)
{
	RADV_FROM_HANDLE(radv_physical_device, device, physicalDevice);
	ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
	struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

	return iface->get_formats(surface, &device->wsi_device, pSurfaceFormatCount,
				  pSurfaceFormats);
}

VkResult radv_GetPhysicalDeviceSurfacePresentModesKHR(
	VkPhysicalDevice                            physicalDevice,
	VkSurfaceKHR                                _surface,
	uint32_t*                                   pPresentModeCount,
	VkPresentModeKHR*                           pPresentModes)
{
	RADV_FROM_HANDLE(radv_physical_device, device, physicalDevice);
	ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
	struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

	return iface->get_present_modes(surface, pPresentModeCount,
					pPresentModes);
}

VkResult radv_CreateSwapchainKHR(
	VkDevice                                     _device,
	const VkSwapchainCreateInfoKHR*              pCreateInfo,
	const VkAllocationCallbacks*                 pAllocator,
	VkSwapchainKHR*                              pSwapchain)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pCreateInfo->surface);
	struct wsi_interface *iface =
		device->physical_device->wsi_device.wsi[surface->platform];
	struct wsi_swapchain *swapchain;
	const VkAllocationCallbacks *alloc;
	if (pAllocator)
		alloc = pAllocator;
	else
		alloc = &device->alloc;
	VkResult result = iface->create_swapchain(surface, _device,
						  &device->physical_device->wsi_device,
						  device->physical_device->local_fd,
						  pCreateInfo,
						  alloc,
						  &swapchain);
	if (result != VK_SUCCESS)
		return result;

	if (pAllocator)
		swapchain->alloc = *pAllocator;
	else
		swapchain->alloc = device->alloc;

	for (unsigned i = 0; i < ARRAY_SIZE(swapchain->fences); i++)
		swapchain->fences[i] = VK_NULL_HANDLE;

	*pSwapchain = wsi_swapchain_to_handle(swapchain);

	return VK_SUCCESS;
}

void radv_DestroySwapchainKHR(
	VkDevice                                     _device,
	VkSwapchainKHR                               _swapchain,
	const VkAllocationCallbacks*                 pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);
	const VkAllocationCallbacks *alloc;

	if (!_swapchain)
		return;

	if (pAllocator)
		alloc = pAllocator;
	else
		alloc = &device->alloc;

	for (unsigned i = 0; i < ARRAY_SIZE(swapchain->fences); i++) {
		if (swapchain->fences[i] != VK_NULL_HANDLE)
			radv_DestroyFence(_device, swapchain->fences[i], pAllocator);
	}

	swapchain->destroy(swapchain, alloc);
}

VkResult radv_GetSwapchainImagesKHR(
	VkDevice                                     device,
	VkSwapchainKHR                               _swapchain,
	uint32_t*                                    pSwapchainImageCount,
	VkImage*                                     pSwapchainImages)
{
	RADV_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);

	return swapchain->get_images(swapchain, pSwapchainImageCount,
				     pSwapchainImages);
}

VkResult radv_AcquireNextImageKHR(
	VkDevice                                     device,
	VkSwapchainKHR                               _swapchain,
	uint64_t                                     timeout,
	VkSemaphore                                  semaphore,
	VkFence                                      _fence,
	uint32_t*                                    pImageIndex)
{
	RADV_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);
	RADV_FROM_HANDLE(radv_fence, fence, _fence);

	VkResult result = swapchain->acquire_next_image(swapchain, timeout, semaphore,
	                                                pImageIndex);

	if (fence && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) {
		fence->submitted = true;
		fence->signalled = true;
	}
	return result;
}

VkResult radv_QueuePresentKHR(
	VkQueue                                  _queue,
	const VkPresentInfoKHR*                  pPresentInfo)
{
	RADV_FROM_HANDLE(radv_queue, queue, _queue);
	return wsi_common_queue_present(&queue->device->physical_device->wsi_device,
					radv_device_to_handle(queue->device),
					_queue,
					queue->queue_family_index,
					pPresentInfo);
}
