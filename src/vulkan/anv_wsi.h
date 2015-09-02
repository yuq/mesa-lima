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

enum anv_swap_chain_type {
   ANV_SWAP_CHAIN_TYPE_X11 = 11,
};

struct anv_swap_chain {
   enum anv_swap_chain_type                     type;

   struct anv_device *                          device;
};

struct anv_x11_swap_chain;

ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_swap_chain, VkSwapChainWSI)

VkResult anv_x11_get_surface_info(struct anv_device *device,
                                  VkSurfaceDescriptionWindowWSI *window,
                                  VkSurfaceInfoTypeWSI infoType,
                                  size_t* pDataSize, void* pData);
VkResult anv_x11_create_swap_chain(struct anv_device *device,
                                   const VkSwapChainCreateInfoWSI *pCreateInfo,
                                   struct anv_x11_swap_chain **swap_chain);
VkResult anv_x11_destroy_swap_chain(struct anv_x11_swap_chain *swap_chain);
VkResult anv_x11_get_swap_chain_info(struct anv_x11_swap_chain *swap_chain,
                                     VkSwapChainInfoTypeWSI infoType,
                                     size_t* pDataSize, void* pData);
VkResult anv_x11_acquire_next_image(struct anv_x11_swap_chain *swap_chain,
                                    uint64_t timeout,
                                    VkSemaphore semaphore,
                                    uint32_t *image_index);
VkResult anv_x11_queue_present(struct anv_queue *queue,
                               struct anv_x11_swap_chain *swap_chain,
                               uint32_t image_index);
