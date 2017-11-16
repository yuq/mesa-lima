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
#ifndef WSI_COMMON_H
#define WSI_COMMON_H

#include <stdint.h>
#include <stdbool.h>

#include "vk_alloc.h"
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

/* This is guaranteed to not collide with anything because it's in the
 * VK_KHR_swapchain namespace but not actually used by the extension.
 */
#define VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA (VkStructureType)1000001002
#define VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA (VkStructureType)1000001003

struct wsi_image_create_info {
    VkStructureType sType;
    const void *pNext;
    bool scanout;
};

struct wsi_memory_allocate_info {
    VkStructureType sType;
    const void *pNext;
    bool implicit_sync;
};

struct wsi_device;

struct wsi_swapchain {
   const struct wsi_device *wsi;

   VkDevice device;
   VkAllocationCallbacks alloc;
   VkFence fences[3];
   VkPresentModeKHR present_mode;
   uint32_t image_count;

   /* Command pools, one per queue family */
   VkCommandPool *cmd_pools;

   VkResult (*destroy)(struct wsi_swapchain *swapchain,
                       const VkAllocationCallbacks *pAllocator);
   VkResult (*get_images)(struct wsi_swapchain *swapchain,
                          uint32_t *pCount, VkImage *pSwapchainImages);
   VkResult (*acquire_next_image)(struct wsi_swapchain *swap_chain,
                                  uint64_t timeout, VkSemaphore semaphore,
                                  uint32_t *image_index);
   VkResult (*queue_present)(struct wsi_swapchain *swap_chain,
                             VkQueue queue,
                             uint32_t waitSemaphoreCount,
                             const VkSemaphore *pWaitSemaphores,
                             uint32_t image_index,
                             const VkPresentRegionKHR *damage);
};

struct wsi_interface {
   VkResult (*get_support)(VkIcdSurfaceBase *surface,
                           struct wsi_device *wsi_device,
                           const VkAllocationCallbacks *alloc,
                           uint32_t queueFamilyIndex,
                           int local_fd,
                           bool can_handle_different_gpu,
                           VkBool32* pSupported);
   VkResult (*get_capabilities)(VkIcdSurfaceBase *surface,
                                VkSurfaceCapabilitiesKHR* pSurfaceCapabilities);
   VkResult (*get_capabilities2)(VkIcdSurfaceBase *surface,
                                 const void *info_next,
                                 VkSurfaceCapabilities2KHR* pSurfaceCapabilities);
   VkResult (*get_formats)(VkIcdSurfaceBase *surface,
                           struct wsi_device *wsi_device,
                           uint32_t* pSurfaceFormatCount,
                           VkSurfaceFormatKHR* pSurfaceFormats);
   VkResult (*get_formats2)(VkIcdSurfaceBase *surface,
                            struct wsi_device *wsi_device,
                            const void *info_next,
                            uint32_t* pSurfaceFormatCount,
                            VkSurfaceFormat2KHR* pSurfaceFormats);
   VkResult (*get_present_modes)(VkIcdSurfaceBase *surface,
                                 uint32_t* pPresentModeCount,
                                 VkPresentModeKHR* pPresentModes);
   VkResult (*create_swapchain)(VkIcdSurfaceBase *surface,
                                VkDevice device,
                                struct wsi_device *wsi_device,
                                int local_fd,
                                const VkSwapchainCreateInfoKHR* pCreateInfo,
                                const VkAllocationCallbacks* pAllocator,
                                struct wsi_swapchain **swapchain);
};

#define VK_ICD_WSI_PLATFORM_MAX 5

struct wsi_device {
   VkPhysicalDeviceMemoryProperties memory_props;
   uint32_t queue_family_count;

   uint32_t (*queue_get_family_index)(VkQueue queue);

#define WSI_CB(cb) PFN_vk##cb cb
   WSI_CB(AllocateMemory);
   WSI_CB(AllocateCommandBuffers);
   WSI_CB(BindBufferMemory);
   WSI_CB(BindImageMemory);
   WSI_CB(BeginCommandBuffer);
   WSI_CB(CmdCopyImageToBuffer);
   WSI_CB(CreateBuffer);
   WSI_CB(CreateCommandPool);
   WSI_CB(CreateFence);
   WSI_CB(CreateImage);
   WSI_CB(DestroyBuffer);
   WSI_CB(DestroyCommandPool);
   WSI_CB(DestroyFence);
   WSI_CB(DestroyImage);
   WSI_CB(EndCommandBuffer);
   WSI_CB(FreeMemory);
   WSI_CB(FreeCommandBuffers);
   WSI_CB(GetBufferMemoryRequirements);
   WSI_CB(GetImageMemoryRequirements);
   WSI_CB(GetImageSubresourceLayout);
   WSI_CB(GetMemoryFdKHR);
   WSI_CB(ResetFences);
   WSI_CB(QueueSubmit);
   WSI_CB(WaitForFences);
#undef WSI_CB

    struct wsi_interface *                  wsi[VK_ICD_WSI_PLATFORM_MAX];
};

typedef PFN_vkVoidFunction (VKAPI_PTR *WSI_FN_GetPhysicalDeviceProcAddr)(VkPhysicalDevice physicalDevice, const char* pName);

void
wsi_device_init(struct wsi_device *wsi,
                VkPhysicalDevice pdevice,
                WSI_FN_GetPhysicalDeviceProcAddr proc_addr);

#define WSI_CB(cb) PFN_vk##cb cb
struct wsi_callbacks {
   VkPhysicalDevice (*device_get_physical)(VkDevice);

   WSI_CB(GetDeviceProcAddr);
   WSI_CB(GetPhysicalDeviceFormatProperties);
   WSI_CB(GetPhysicalDeviceMemoryProperties);
   WSI_CB(GetPhysicalDeviceQueueFamilyProperties);
};
#undef WSI_CB

#define WSI_DEFINE_NONDISP_HANDLE_CASTS(__wsi_type, __VkType)              \
                                                                           \
   static inline struct __wsi_type *                                       \
   __wsi_type ## _from_handle(__VkType _handle)                            \
   {                                                                       \
      return (struct __wsi_type *)(uintptr_t) _handle;                     \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __wsi_type ## _to_handle(struct __wsi_type *_obj)                       \
   {                                                                       \
      return (__VkType)(uintptr_t) _obj;                                   \
   }

#define WSI_FROM_HANDLE(__wsi_type, __name, __handle) \
   struct __wsi_type *__name = __wsi_type ## _from_handle(__handle)

WSI_DEFINE_NONDISP_HANDLE_CASTS(wsi_swapchain, VkSwapchainKHR)

#define ICD_DEFINE_NONDISP_HANDLE_CASTS(__VkIcdType, __VkType)             \
                                                                           \
   static inline __VkIcdType *                                             \
   __VkIcdType ## _from_handle(__VkType _handle)                           \
   {                                                                       \
      return (__VkIcdType *)(uintptr_t) _handle;                           \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __VkIcdType ## _to_handle(__VkIcdType *_obj)                            \
   {                                                                       \
      return (__VkType)(uintptr_t) _obj;                                   \
   }

#define ICD_FROM_HANDLE(__VkIcdType, __name, __handle) \
   __VkIcdType *__name = __VkIcdType ## _from_handle(__handle)

ICD_DEFINE_NONDISP_HANDLE_CASTS(VkIcdSurfaceBase, VkSurfaceKHR)

VkResult wsi_x11_init_wsi(struct wsi_device *wsi_device,
                          const VkAllocationCallbacks *alloc);
void wsi_x11_finish_wsi(struct wsi_device *wsi_device,
                        const VkAllocationCallbacks *alloc);
VkResult wsi_wl_init_wsi(struct wsi_device *wsi_device,
                         const VkAllocationCallbacks *alloc,
                         VkPhysicalDevice physical_device,
                         const struct wsi_callbacks *cbs);
void wsi_wl_finish_wsi(struct wsi_device *wsi_device,
                       const VkAllocationCallbacks *alloc);

VkResult
wsi_common_queue_present(const struct wsi_device *wsi,
                         VkDevice device_h,
                         VkQueue queue_h,
                         int queue_family_index,
                         const VkPresentInfoKHR *pPresentInfo);

#endif
