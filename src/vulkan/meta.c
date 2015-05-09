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

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "private.h"

void VKAPI vkCmdCopyBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{
}

void VKAPI vkCmdCopyImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageCopy*                          pRegions)
{
}

void VKAPI vkCmdBlitImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageBlit*                          pRegions)
{
}

void VKAPI vkCmdCopyBufferToImage(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    srcBuffer,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
}

void VKAPI vkCmdCopyImageToBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
}

void VKAPI vkCmdCloneImageData(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout)
{
}

void VKAPI vkCmdUpdateBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                dataSize,
    const uint32_t*                             pData)
{
}

void VKAPI vkCmdFillBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                fillSize,
    uint32_t                                    data)
{
}

void VKAPI vkCmdClearColorImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     image,
    VkImageLayout                               imageLayout,
    const VkClearColor*                         color,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
}

void VKAPI vkCmdClearDepthStencil(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     image,
    VkImageLayout                               imageLayout,
    float                                       depth,
    uint32_t                                    stencil,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
}

void VKAPI vkCmdResolveImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageResolve*                       pRegions)
{
}
