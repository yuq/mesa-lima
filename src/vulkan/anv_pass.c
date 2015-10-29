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

VkResult anv_CreateRenderPass(
    VkDevice                                    _device,
    const VkRenderPassCreateInfo*               pCreateInfo,
    VkRenderPass*                               pRenderPass)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_render_pass *pass;
   size_t size;
   size_t attachments_offset;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);

   size = sizeof(*pass);
   size += pCreateInfo->subpassCount * sizeof(pass->subpasses[0]);
   attachments_offset = size;
   size += pCreateInfo->attachmentCount * sizeof(pass->attachments[0]);

   pass = anv_device_alloc(device, size, 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pass == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Clear the subpasses along with the parent pass. This required because
    * each array member of anv_subpass must be a valid pointer if not NULL.
    */
   memset(pass, 0, size);
   pass->attachment_count = pCreateInfo->attachmentCount;
   pass->subpass_count = pCreateInfo->subpassCount;
   pass->attachments = (void *) pass + attachments_offset;

   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      struct anv_render_pass_attachment *att = &pass->attachments[i];

      att->format = anv_format_for_vk_format(pCreateInfo->pAttachments[i].format);
      att->samples = pCreateInfo->pAttachments[i].samples;
      att->load_op = pCreateInfo->pAttachments[i].loadOp;
      att->stencil_load_op = pCreateInfo->pAttachments[i].stencilLoadOp;
      // att->store_op = pCreateInfo->pAttachments[i].storeOp;
      // att->stencil_store_op = pCreateInfo->pAttachments[i].stencilStoreOp;

      if (anv_format_is_color(att->format)) {
         if (att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            ++pass->num_color_clear_attachments;
         }
      } else {
         if (att->format->depth_format &&
             att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            pass->has_depth_clear_attachment = true;
         }

         if (att->format->has_stencil &&
             att->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            assert(att->format->has_stencil);
            pass->has_stencil_clear_attachment = true;
         }
      }
   }

   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription *desc = &pCreateInfo->pSubpasses[i];
      struct anv_subpass *subpass = &pass->subpasses[i];

      subpass->input_count = desc->inputCount;
      subpass->color_count = desc->colorCount;

      if (desc->inputCount > 0) {
         subpass->input_attachments =
            anv_device_alloc(device, desc->inputCount * sizeof(uint32_t),
                             8, VK_SYSTEM_ALLOC_TYPE_API_OBJECT);

         for (uint32_t j = 0; j < desc->inputCount; j++) {
            subpass->input_attachments[j]
               = desc->pInputAttachments[j].attachment;
         }
      }

      if (desc->colorCount > 0) {
         subpass->color_attachments =
            anv_device_alloc(device, desc->colorCount * sizeof(uint32_t),
                             8, VK_SYSTEM_ALLOC_TYPE_API_OBJECT);

         for (uint32_t j = 0; j < desc->colorCount; j++) {
            subpass->color_attachments[j]
               = desc->pColorAttachments[j].attachment;
         }
      }

      if (desc->pResolveAttachments) {
         subpass->resolve_attachments =
            anv_device_alloc(device, desc->colorCount * sizeof(uint32_t),
                             8, VK_SYSTEM_ALLOC_TYPE_API_OBJECT);

         for (uint32_t j = 0; j < desc->colorCount; j++) {
            subpass->resolve_attachments[j]
               = desc->pResolveAttachments[j].attachment;
         }
      }

      subpass->depth_stencil_attachment = desc->depthStencilAttachment.attachment;
   }

   *pRenderPass = anv_render_pass_to_handle(pass);

   return VK_SUCCESS;
}

void anv_DestroyRenderPass(
    VkDevice                                    _device,
    VkRenderPass                                _pass)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_render_pass, pass, _pass);

   for (uint32_t i = 0; i < pass->subpass_count; i++) {
      /* In VkSubpassCreateInfo, each of the attachment arrays may be null.
       * Don't free the null arrays.
       */
      struct anv_subpass *subpass = &pass->subpasses[i];

      anv_device_free(device, subpass->input_attachments);
      anv_device_free(device, subpass->color_attachments);
      anv_device_free(device, subpass->resolve_attachments);
   }

   anv_device_free(device, pass);
}

VkResult anv_GetRenderAreaGranularity(
    VkDevice                                    device,
    VkRenderPass                                renderPass,
    VkExtent2D*                                 pGranularity)
{
   *pGranularity = (VkExtent2D) { 1, 1 };

   return VK_SUCCESS;
}
