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

struct anv_swap_chain {
   struct anv_device *                          device;

   VkResult (*destroy)(struct anv_swap_chain *swap_chain);
   VkResult (*get_swap_chain_info)(struct anv_swap_chain *swap_chain,
                                   VkSwapChainInfoTypeWSI infoType,
                                   size_t *pDataSize, void *pData);
   VkResult (*acquire_next_image)(struct anv_swap_chain *swap_chain,
                                  uint64_t timeout, VkSemaphore semaphore,
                                  uint32_t *image_index);
   VkResult (*queue_present)(struct anv_swap_chain *swap_chain,
                             struct anv_queue *queue,
                             uint32_t image_index);
};

ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_swap_chain, VkSwapChainWSI)

struct anv_wsi_implementation {
   VkResult (*get_window_supported)(struct anv_wsi_implementation *impl,
                                    struct anv_physical_device *physical_device,
                                    const VkSurfaceDescriptionWindowWSI *window,
                                    VkBool32 *pSupported);
   VkResult (*get_surface_info)(struct anv_wsi_implementation *impl,
                                struct anv_device *device,
                                VkSurfaceDescriptionWindowWSI *window,
                                VkSurfaceInfoTypeWSI infoType,
                                size_t* pDataSize, void* pData);
   VkResult (*create_swap_chain)(struct anv_wsi_implementation *impl,
                                 struct anv_device *device,
                                 const VkSwapChainCreateInfoWSI *pCreateInfo,
                                 struct anv_swap_chain **swap_chain);
};

VkResult anv_x11_init_wsi(struct anv_instance *instance);
void anv_x11_finish_wsi(struct anv_instance *instance);
VkResult anv_wl_init_wsi(struct anv_instance *instance);
void anv_wl_finish_wsi(struct anv_instance *instance);
