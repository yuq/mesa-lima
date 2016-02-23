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

#include "genX_state_util.h"

VkResult
genX(init_device_state)(struct anv_device *device)
{
   GENX(MEMORY_OBJECT_CONTROL_STATE_pack)(NULL, &device->default_mocs,
                                          &GENX(MOCS));

   struct anv_batch batch;

   uint32_t cmds[64];
   batch.start = batch.next = cmds;
   batch.end = (void *) cmds + sizeof(cmds);

   anv_batch_emit(&batch, GENX(PIPELINE_SELECT),
                  .PipelineSelection = _3D);

   anv_batch_emit(&batch, GENX(3DSTATE_VF_STATISTICS),
                  .StatisticsEnable = true);
   anv_batch_emit(&batch, GENX(3DSTATE_HS), .Enable = false);
   anv_batch_emit(&batch, GENX(3DSTATE_TE), .TEEnable = false);
   anv_batch_emit(&batch, GENX(3DSTATE_DS), .DSFunctionEnable = false);
   anv_batch_emit(&batch, GENX(3DSTATE_STREAMOUT), .SOFunctionEnable = false);
   anv_batch_emit(&batch, GENX(3DSTATE_AA_LINE_PARAMETERS));
   anv_batch_emit(&batch, GENX(MI_BATCH_BUFFER_END));

   assert(batch.next <= batch.end);

   return anv_device_submit_simple_batch(device, &batch);
}

VkResult genX(CreateSampler)(
    VkDevice                                    _device,
    const VkSamplerCreateInfo*                  pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSampler*                                  pSampler)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_sampler *sampler;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

   sampler = anv_alloc2(&device->alloc, pAllocator, sizeof(*sampler), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!sampler)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   struct GENX(SAMPLER_STATE) sampler_state = {
      .SamplerDisable = false,
      .TextureBorderColorMode = DX10OGL,
      .LODPreClampEnable = CLAMP_ENABLE_OGL,
      .BaseMipLevel = 0.0,
      .MipModeFilter = vk_to_gen_mipmap_mode[pCreateInfo->mipmapMode],
      .MagModeFilter = vk_to_gen_tex_filter(pCreateInfo->magFilter,
                                            pCreateInfo->anisotropyEnable),
      .MinModeFilter = vk_to_gen_tex_filter(pCreateInfo->minFilter,
                                            pCreateInfo->anisotropyEnable),
      .TextureLODBias = pCreateInfo->mipLodBias * 256,
      .AnisotropicAlgorithm = EWAApproximation,
      .MinLOD = pCreateInfo->minLod,
      .MaxLOD = pCreateInfo->maxLod,
      .ChromaKeyEnable = 0,
      .ChromaKeyIndex = 0,
      .ChromaKeyMode = 0,
      .ShadowFunction = vk_to_gen_compare_op[pCreateInfo->compareOp],
      .CubeSurfaceControlMode = OVERRIDE,

      .BorderColorPointer =
         device->border_colors.offset +
         pCreateInfo->borderColor * sizeof(float) * 4,

      .MaximumAnisotropy = vk_to_gen_max_anisotropy(pCreateInfo->maxAnisotropy),
      .RAddressMinFilterRoundingEnable = 0,
      .RAddressMagFilterRoundingEnable = 0,
      .VAddressMinFilterRoundingEnable = 0,
      .VAddressMagFilterRoundingEnable = 0,
      .UAddressMinFilterRoundingEnable = 0,
      .UAddressMagFilterRoundingEnable = 0,
      .TrilinearFilterQuality = 0,
      .NonnormalizedCoordinateEnable = pCreateInfo->unnormalizedCoordinates,
      .TCXAddressControlMode = vk_to_gen_tex_address[pCreateInfo->addressModeU],
      .TCYAddressControlMode = vk_to_gen_tex_address[pCreateInfo->addressModeV],
      .TCZAddressControlMode = vk_to_gen_tex_address[pCreateInfo->addressModeW],
   };

   GENX(SAMPLER_STATE_pack)(NULL, sampler->state, &sampler_state);

   *pSampler = anv_sampler_to_handle(sampler);

   return VK_SUCCESS;
}
