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

static VkResult
radv_wsi_image_create(VkDevice device_h,
		      const VkSwapchainCreateInfoKHR *pCreateInfo,
		      const VkAllocationCallbacks* pAllocator,
		      struct wsi_image *wsi_image)
{
	VkResult result = VK_SUCCESS;
	struct radeon_surf *surface;
	VkImage image_h;
	struct radv_image *image;
	int fd;
	RADV_FROM_HANDLE(radv_device, device, device_h);

	result = radv_image_create(device_h,
				   &(struct radv_image_create_info) {
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
					   },
						   .scanout = true},
				   NULL,
				   &image_h);
	if (result != VK_SUCCESS)
		return result;

	image = radv_image_from_handle(image_h);

	VkDeviceMemory memory_h;

	const VkMemoryDedicatedAllocateInfoKHR ded_alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
		.pNext = NULL,
		.buffer = VK_NULL_HANDLE,
		.image = image_h
	};

	/* Find the first VRAM memory type, or GART for PRIME images. */
	int memory_type_index = -1;
	for (int i = 0; i < device->physical_device->memory_properties.memoryTypeCount; ++i) {
		bool is_local = !!(device->physical_device->memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (is_local) {
			memory_type_index = i;
			break;
		}
	}

	/* fallback */
	if (memory_type_index == -1)
		memory_type_index = 0;

	result = radv_alloc_memory(device_h,
				     &(VkMemoryAllocateInfo) {
					     .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
					     .pNext = &ded_alloc,
					     .allocationSize = image->size,
					     .memoryTypeIndex = memory_type_index,
				     },
				     NULL /* XXX: pAllocator */,
				     RADV_MEM_IMPLICIT_SYNC,
				     &memory_h);
	if (result != VK_SUCCESS)
		goto fail_create_image;

	radv_BindImageMemory(device_h, image_h, memory_h, 0);

	RADV_FROM_HANDLE(radv_device_memory, memory, memory_h);
	if (!radv_get_memory_fd(device, memory, &fd))
		goto fail_alloc_memory;
	wsi_image->fd = fd;

	surface = &image->surface;

	wsi_image->image = image_h;
	wsi_image->memory = memory_h;
	wsi_image->size = image->size;
	wsi_image->offset = image->offset;
	if (device->physical_device->rad_info.chip_class >= GFX9)
		wsi_image->row_pitch =
			surface->u.gfx9.surf_pitch * surface->bpe;
	else
		wsi_image->row_pitch =
			surface->u.legacy.level[0].nblk_x * surface->bpe;

	return VK_SUCCESS;
 fail_alloc_memory:
	radv_FreeMemory(device_h, memory_h, pAllocator);

fail_create_image:
	radv_DestroyImage(device_h, image_h, pAllocator);

	return result;
}

static void
radv_wsi_image_free(VkDevice device,
		    const VkAllocationCallbacks* pAllocator,
		    struct wsi_image *wsi_image)
{
	radv_DestroyImage(device, wsi_image->image, pAllocator);

	radv_FreeMemory(device, wsi_image->memory, pAllocator);
}

static const struct wsi_image_fns radv_wsi_image_fns = {
   .create_wsi_image = radv_wsi_image_create,
   .free_wsi_image = radv_wsi_image_free,
};

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
						  alloc, &radv_wsi_image_fns,
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
	VkResult result = VK_SUCCESS;
	const VkPresentRegionsKHR *regions =
	         vk_find_struct_const(pPresentInfo->pNext, PRESENT_REGIONS_KHR);

	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
		RADV_FROM_HANDLE(wsi_swapchain, swapchain, pPresentInfo->pSwapchains[i]);
		struct radeon_winsys_cs *cs;
		const VkPresentRegionKHR *region = NULL;
		VkResult item_result;
		struct radv_winsys_sem_info sem_info;

		item_result = radv_alloc_sem_info(&sem_info,
						  pPresentInfo->waitSemaphoreCount,
						  pPresentInfo->pWaitSemaphores,
						  0,
						  NULL);
		if (pPresentInfo->pResults != NULL)
			pPresentInfo->pResults[i] = item_result;
		result = result == VK_SUCCESS ? item_result : result;
		if (item_result != VK_SUCCESS) {
			radv_free_sem_info(&sem_info);
			continue;
		}

		assert(radv_device_from_handle(swapchain->device) == queue->device);
		if (swapchain->fences[0] == VK_NULL_HANDLE) {
			item_result = radv_CreateFence(radv_device_to_handle(queue->device),
						  &(VkFenceCreateInfo) {
							  .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
								  .flags = 0,
								  }, &swapchain->alloc, &swapchain->fences[0]);
			if (pPresentInfo->pResults != NULL)
				pPresentInfo->pResults[i] = item_result;
			result = result == VK_SUCCESS ? item_result : result;
			if (item_result != VK_SUCCESS) {
				radv_free_sem_info(&sem_info);
				continue;
			}
		} else {
			radv_ResetFences(radv_device_to_handle(queue->device),
					 1, &swapchain->fences[0]);
		}

		cs = queue->device->empty_cs[queue->queue_family_index];
		RADV_FROM_HANDLE(radv_fence, fence, swapchain->fences[0]);
		struct radeon_winsys_fence *base_fence = fence->fence;
		struct radeon_winsys_ctx *ctx = queue->hw_ctx;

		queue->device->ws->cs_submit(ctx, queue->queue_idx,
					     &cs,
					     1, NULL, NULL,
					     &sem_info,
					     false, base_fence);
		fence->submitted = true;

		if (regions && regions->pRegions)
			region = &regions->pRegions[i];

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
		if (item_result != VK_SUCCESS) {
			radv_free_sem_info(&sem_info);
			continue;
		}

		VkFence last = swapchain->fences[2];
		swapchain->fences[2] = swapchain->fences[1];
		swapchain->fences[1] = swapchain->fences[0];
		swapchain->fences[0] = last;

		if (last != VK_NULL_HANDLE) {
			radv_WaitForFences(radv_device_to_handle(queue->device),
					   1, &last, true, 1);
		}

		radv_free_sem_info(&sem_info);
	}

	return VK_SUCCESS;
}
