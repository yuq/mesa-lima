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

#include "anv_private.h"

#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"

#include "genX_pipeline_util.h"

static void
emit_ia_state(struct anv_pipeline *pipeline,
              const VkPipelineInputAssemblyStateCreateInfo *info)
{
   anv_batch_emit(&pipeline->batch, GENX(3DSTATE_VF_TOPOLOGY), vft) {
      vft.PrimitiveTopologyType = pipeline->topology;
   }
}

VkResult
genX(graphics_pipeline_create)(
    VkDevice                                    _device,
    struct anv_pipeline_cache *                 cache,
    const VkGraphicsPipelineCreateInfo*         pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_render_pass, pass, pCreateInfo->renderPass);
   struct anv_subpass *subpass = &pass->subpasses[pCreateInfo->subpass];
   struct anv_pipeline *pipeline;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);

   pipeline = vk_alloc2(&device->alloc, pAllocator, sizeof(*pipeline), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   result = anv_pipeline_init(pipeline, device, cache,
                              pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&device->alloc, pAllocator, pipeline);
      return result;
   }

   assert(pCreateInfo->pVertexInputState);
   emit_vertex_input(pipeline, pCreateInfo->pVertexInputState);
   assert(pCreateInfo->pInputAssemblyState);
   emit_ia_state(pipeline, pCreateInfo->pInputAssemblyState);
   assert(pCreateInfo->pRasterizationState);
   emit_rs_state(pipeline, pCreateInfo->pRasterizationState,
                 pCreateInfo->pMultisampleState, pass, subpass);
   emit_ms_state(pipeline, pCreateInfo->pMultisampleState);
   emit_ds_state(pipeline, pCreateInfo->pDepthStencilState, pass, subpass);
   emit_cb_state(pipeline, pCreateInfo->pColorBlendState,
                           pCreateInfo->pMultisampleState);

   emit_urb_setup(pipeline);

   emit_3dstate_clip(pipeline, pCreateInfo->pViewportState,
                     pCreateInfo->pRasterizationState);
   emit_3dstate_streamout(pipeline, pCreateInfo->pRasterizationState);

   const struct brw_wm_prog_data *wm_prog_data = get_wm_prog_data(pipeline);

   emit_3dstate_wm(pipeline, pCreateInfo->pMultisampleState);
   emit_3dstate_gs(pipeline);
   emit_3dstate_vs(pipeline);
   emit_3dstate_sbe(pipeline);
   emit_3dstate_ps(pipeline);

   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS_EXTRA), extra) {
         extra.PixelShaderValid = false;
      }
   } else {

      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_PS_EXTRA), ps) {
         ps.PixelShaderValid              = true;
         ps.PixelShaderKillsPixel         = wm_prog_data->uses_kill;
         ps.PixelShaderComputedDepthMode  = wm_prog_data->computed_depth_mode;
         ps.AttributeEnable               = wm_prog_data->num_varying_inputs > 0;
         ps.oMaskPresenttoRenderTarget    = wm_prog_data->uses_omask;
         ps.PixelShaderIsPerSample        = wm_prog_data->persample_dispatch;
         ps.PixelShaderUsesSourceDepth    = wm_prog_data->uses_src_depth;
         ps.PixelShaderUsesSourceW        = wm_prog_data->uses_src_w;
#if GEN_GEN >= 9
         ps.PixelShaderPullsBary    = wm_prog_data->pulls_bary;
         ps.InputCoverageMaskState  = wm_prog_data->uses_sample_mask ?
            ICMS_INNER_CONSERVATIVE : ICMS_NONE;
#else
         ps.PixelShaderUsesInputCoverageMask = wm_prog_data->uses_sample_mask;
#endif
      }
   }

   *pPipeline = anv_pipeline_to_handle(pipeline);

   return VK_SUCCESS;
}
