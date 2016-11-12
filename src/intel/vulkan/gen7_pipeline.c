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

   assert(pCreateInfo->pRasterizationState);
   emit_rs_state(pipeline, pCreateInfo->pRasterizationState,
                 pCreateInfo->pMultisampleState, pass, subpass);

   emit_ds_state(pipeline, pCreateInfo->pDepthStencilState, pass, subpass);

   emit_cb_state(pipeline, pCreateInfo->pColorBlendState,
                           pCreateInfo->pMultisampleState);

   emit_urb_setup(pipeline);

   emit_3dstate_clip(pipeline, pCreateInfo->pViewportState,
                     pCreateInfo->pRasterizationState);
   emit_3dstate_streamout(pipeline, pCreateInfo->pRasterizationState);

   emit_ms_state(pipeline, pCreateInfo->pMultisampleState);

#if 0 
   /* From gen7_vs_state.c */

   /**
    * From Graphics BSpec: 3D-Media-GPGPU Engine > 3D Pipeline Stages >
    * Geometry > Geometry Shader > State:
    *
    *     "Note: Because of corruption in IVB:GT2, software needs to flush the
    *     whole fixed function pipeline when the GS enable changes value in
    *     the 3DSTATE_GS."
    *
    * The hardware architects have clarified that in this context "flush the
    * whole fixed function pipeline" means to emit a PIPE_CONTROL with the "CS
    * Stall" bit set.
    */
   if (!brw->is_haswell && !brw->is_baytrail)
      gen7_emit_vs_workaround_flush(brw);
#endif

   emit_3dstate_vs(pipeline);
   emit_3dstate_gs(pipeline);
   emit_3dstate_sbe(pipeline);
   emit_3dstate_ps(pipeline);

   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_WM), wm) {
         wm.StatisticsEnable                    = true;
         wm.ThreadDispatchEnable                = false;
         wm.LineEndCapAntialiasingRegionWidth   = 0; /* 0.5 pixels */
         wm.LineAntialiasingRegionWidth         = 1; /* 1.0 pixels */
         wm.EarlyDepthStencilControl            = EDSC_NORMAL;
         wm.PointRasterizationRule              = RASTRULE_UPPER_RIGHT;
      }
   } else {
      const struct brw_wm_prog_data *wm_prog_data = get_wm_prog_data(pipeline);

      if (wm_prog_data->urb_setup[VARYING_SLOT_BFC0] != -1 ||
          wm_prog_data->urb_setup[VARYING_SLOT_BFC1] != -1)
         anv_finishme("two-sided color needs sbe swizzling setup");
      if (wm_prog_data->urb_setup[VARYING_SLOT_PRIMITIVE_ID] != -1)
         anv_finishme("primitive_id needs sbe swizzling setup");

      uint32_t samples = pCreateInfo->pMultisampleState ?
                         pCreateInfo->pMultisampleState->rasterizationSamples : 1;

      /* FIXME-GEN7: This needs a lot more work, cf gen7 upload_wm_state(). */
      anv_batch_emit(&pipeline->batch, GENX(3DSTATE_WM), wm) {
         wm.StatisticsEnable                    = true;
         wm.ThreadDispatchEnable                = true;
         wm.LineEndCapAntialiasingRegionWidth   = 0; /* 0.5 pixels */
         wm.LineAntialiasingRegionWidth         = 1; /* 1.0 pixels */
         wm.PointRasterizationRule              = RASTRULE_UPPER_RIGHT;
         wm.PixelShaderKillPixel                = wm_prog_data->uses_kill;
         wm.PixelShaderComputedDepthMode        = wm_prog_data->computed_depth_mode;
         wm.PixelShaderUsesSourceDepth          = wm_prog_data->uses_src_depth;
         wm.PixelShaderUsesSourceW              = wm_prog_data->uses_src_w;
         wm.PixelShaderUsesInputCoverageMask    = wm_prog_data->uses_sample_mask;

         if (wm_prog_data->early_fragment_tests) {
            wm.EarlyDepthStencilControl         = EDSC_PREPS;
         } else if (wm_prog_data->has_side_effects) {
            wm.EarlyDepthStencilControl         = EDSC_PSEXEC;
         } else {
            wm.EarlyDepthStencilControl         = EDSC_NORMAL;
         }

         wm.BarycentricInterpolationMode        = wm_prog_data->barycentric_interp_modes;

         wm.MultisampleRasterizationMode        = samples > 1 ?
                                                  MSRASTMODE_ON_PATTERN : MSRASTMODE_OFF_PIXEL;
         wm.MultisampleDispatchMode             = ((samples == 1) ||
                                                   (samples > 1 && wm_prog_data->persample_dispatch)) ?
                                                  MSDISPMODE_PERSAMPLE : MSDISPMODE_PERPIXEL;
      }
   }

   *pPipeline = anv_pipeline_to_handle(pipeline);

   return VK_SUCCESS;
}
