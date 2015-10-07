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

#pragma once

#include "anv_private.h"

struct anv_swapchain {
   struct anv_device *                          device;

   VkResult (*destroy)(struct anv_swapchain *swapchain);
   VkResult (*get_images)(struct anv_swapchain *swapchain,
                          uint32_t *pCount, VkImage *pSwapchainImages);
   VkResult (*acquire_next_image)(struct anv_swapchain *swap_chain,
                                  uint64_t timeout, VkSemaphore semaphore,
                                  uint32_t *image_index);
   VkResult (*queue_present)(struct anv_swapchain *swap_chain,
                             struct anv_queue *queue,
                             uint32_t image_index);
};

ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_swapchain, VkSwapchainKHR)

struct anv_wsi_implementation {
   VkResult (*get_window_supported)(struct anv_wsi_implementation *impl,
                                    struct anv_physical_device *physical_device,
                                    const VkSurfaceDescriptionWindowKHR *window,
                                    VkBool32 *pSupported);
   VkResult (*get_surface_properties)(struct anv_wsi_implementation *impl,
                                      struct anv_device *device,
                                      const VkSurfaceDescriptionWindowKHR *window,
                                      VkSurfacePropertiesKHR *properties);
   VkResult (*get_surface_formats)(struct anv_wsi_implementation *impl,
                                   struct anv_device *device,
                                   const VkSurfaceDescriptionWindowKHR *window,
                                   uint32_t *pCount,
                                   VkSurfaceFormatKHR *pSurfaceFormats);
   VkResult (*get_surface_present_modes)(struct anv_wsi_implementation *impl,
                                         struct anv_device *device,
                                         const VkSurfaceDescriptionWindowKHR *window,
                                         uint32_t *pCount,
                                         VkPresentModeKHR *pPresentModes);
   VkResult (*create_swapchain)(struct anv_wsi_implementation *impl,
                                struct anv_device *device,
                                const VkSwapchainCreateInfoKHR *pCreateInfo,
                                struct anv_swapchain **swapchain);
};

VkResult anv_x11_init_wsi(struct anv_instance *instance);
void anv_x11_finish_wsi(struct anv_instance *instance);
VkResult anv_wl_init_wsi(struct anv_instance *instance);
void anv_wl_finish_wsi(struct anv_instance *instance);
