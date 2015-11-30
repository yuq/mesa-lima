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

#include "anv_private.h"

#if (ANV_GEN == 9)
#  include "gen9_pack.h"
#elif (ANV_GEN == 8)
#  include "gen8_pack.h"
#elif (ANV_IS_HASWELL)
#  include "gen75_pack.h"
#elif (ANV_GEN == 7)
#  include "gen7_pack.h"
#endif

void
genX(cmd_buffer_emit_state_base_address)(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_bo *scratch_bo = NULL;

   cmd_buffer->state.scratch_size =
      anv_block_pool_size(&device->scratch_block_pool);
   if (cmd_buffer->state.scratch_size > 0)
      scratch_bo = &device->scratch_block_pool.bo;

/* XXX: Do we need this on more than just BDW? */
#if (ANV_GEN == 8)
   /* Emit a render target cache flush.
    *
    * This isn't documented anywhere in the PRM.  However, it seems to be
    * necessary prior to changing the surface state base adress.  Without
    * this, we get GPU hangs when using multi-level command buffers which
    * clear depth, reset state base address, and then go render stuff.
    */
   anv_batch_emit(&cmd_buffer->batch, GEN8_PIPE_CONTROL,
                  .RenderTargetCacheFlushEnable = true);
#endif

   anv_batch_emit(&cmd_buffer->batch, GENX(STATE_BASE_ADDRESS),
      .GeneralStateBaseAddress = { scratch_bo, 0 },
      .GeneralStateMemoryObjectControlState = GENX(MOCS),
      .GeneralStateBaseAddressModifyEnable = true,

      .SurfaceStateBaseAddress = anv_cmd_buffer_surface_base_address(cmd_buffer),
      .SurfaceStateMemoryObjectControlState = GENX(MOCS),
      .SurfaceStateBaseAddressModifyEnable = true,

      .DynamicStateBaseAddress = { &device->dynamic_state_block_pool.bo, 0 },
      .DynamicStateMemoryObjectControlState = GENX(MOCS),
      .DynamicStateBaseAddressModifyEnable = true,

      .IndirectObjectBaseAddress = { NULL, 0 },
      .IndirectObjectMemoryObjectControlState = GENX(MOCS),
      .IndirectObjectBaseAddressModifyEnable = true,

      .InstructionBaseAddress = { &device->instruction_block_pool.bo, 0 },
      .InstructionMemoryObjectControlState = GENX(MOCS),
      .InstructionBaseAddressModifyEnable = true,

#  if (ANV_GEN >= 8)
      /* Broadwell requires that we specify a buffer size for a bunch of
       * these fields.  However, since we will be growing the BO's live, we
       * just set them all to the maximum.
       */
      .GeneralStateBufferSize = 0xfffff,
      .GeneralStateBufferSizeModifyEnable = true,
      .DynamicStateBufferSize = 0xfffff,
      .DynamicStateBufferSizeModifyEnable = true,
      .IndirectObjectBufferSize = 0xfffff,
      .IndirectObjectBufferSizeModifyEnable = true,
      .InstructionBufferSize = 0xfffff,
      .InstructionBuffersizeModifyEnable = true,
#  endif
   );

   /* After re-setting the surface state base address, we have to do some
    * cache flusing so that the sampler engine will pick up the new
    * SURFACE_STATE objects and binding tables. From the Broadwell PRM,
    * Shared Function > 3D Sampler > State > State Caching (page 96):
    *
    *    Coherency with system memory in the state cache, like the texture
    *    cache is handled partially by software. It is expected that the
    *    command stream or shader will issue Cache Flush operation or
    *    Cache_Flush sampler message to ensure that the L1 cache remains
    *    coherent with system memory.
    *
    *    [...]
    *
    *    Whenever the value of the Dynamic_State_Base_Addr,
    *    Surface_State_Base_Addr are altered, the L1 state cache must be
    *    invalidated to ensure the new surface or sampler state is fetched
    *    from system memory.
    *
    * The PIPE_CONTROL command has a "State Cache Invalidation Enable" bit
    * which, according the PIPE_CONTROL instruction documentation in the
    * Broadwell PRM:
    *
    *    Setting this bit is independent of any other bit in this packet.
    *    This bit controls the invalidation of the L1 and L2 state caches
    *    at the top of the pipe i.e. at the parsing time.
    *
    * Unfortunately, experimentation seems to indicate that state cache
    * invalidation through a PIPE_CONTROL does nothing whatsoever in
    * regards to surface state and binding tables.  In stead, it seems that
    * invalidating the texture cache is what is actually needed.
    *
    * XXX:  As far as we have been able to determine through
    * experimentation, shows that flush the texture cache appears to be
    * sufficient.  The theory here is that all of the sampling/rendering
    * units cache the binding table in the texture cache.  However, we have
    * yet to be able to actually confirm this.
    */
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL),
                  .TextureCacheInvalidationEnable = true);
}

void genX(CmdPipelineBarrier)(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineStageFlags                        srcStageMask,
    VkPipelineStageFlags                        destStageMask,
    VkBool32                                    byRegion,
    uint32_t                                    memBarrierCount,
    const void* const*                          ppMemBarriers)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   uint32_t b, *dw;

   struct GENX(PIPE_CONTROL) cmd = {
      GENX(PIPE_CONTROL_header),
      .PostSyncOperation = NoWrite,
   };

   /* XXX: I think waitEvent is a no-op on our HW.  We should verify that. */

   if (anv_clear_mask(&srcStageMask, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)) {
      /* This is just what PIPE_CONTROL does */
   }

   if (anv_clear_mask(&srcStageMask,
                      VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                      VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                      VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                      VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)) {
      cmd.StallAtPixelScoreboard = true;
   }

   if (anv_clear_mask(&srcStageMask,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_TRANSFER_BIT)) {
      cmd.CommandStreamerStallEnable = true;
   }

   if (anv_clear_mask(&srcStageMask, VK_PIPELINE_STAGE_HOST_BIT)) {
      anv_finishme("VK_PIPE_EVENT_CPU_SIGNAL_BIT");
   }

   /* On our hardware, all stages will wait for execution as needed. */
   (void)destStageMask;

   /* We checked all known VkPipeEventFlags. */
   anv_assert(srcStageMask == 0);

   /* XXX: Right now, we're really dumb and just flush whatever categories
    * the app asks for.  One of these days we may make this a bit better
    * but right now that's all the hardware allows for in most areas.
    */
   VkMemoryOutputFlags out_flags = 0;
   VkMemoryInputFlags in_flags = 0;

   for (uint32_t i = 0; i < memBarrierCount; i++) {
      const struct anv_common *common = ppMemBarriers[i];
      switch (common->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_BARRIER: {
         ANV_COMMON_TO_STRUCT(VkMemoryBarrier, barrier, common);
         out_flags |= barrier->outputMask;
         in_flags |= barrier->inputMask;
         break;
      }
      case VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER: {
         ANV_COMMON_TO_STRUCT(VkBufferMemoryBarrier, barrier, common);
         out_flags |= barrier->outputMask;
         in_flags |= barrier->inputMask;
         break;
      }
      case VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER: {
         ANV_COMMON_TO_STRUCT(VkImageMemoryBarrier, barrier, common);
         out_flags |= barrier->outputMask;
         in_flags |= barrier->inputMask;
         break;
      }
      default:
         unreachable("Invalid memory barrier type");
      }
   }

   for_each_bit(b, out_flags) {
      switch ((VkMemoryOutputFlags)(1 << b)) {
      case VK_MEMORY_OUTPUT_HOST_WRITE_BIT:
         break; /* FIXME: Little-core systems */
      case VK_MEMORY_OUTPUT_SHADER_WRITE_BIT:
         cmd.DCFlushEnable = true;
         break;
      case VK_MEMORY_OUTPUT_COLOR_ATTACHMENT_BIT:
         cmd.RenderTargetCacheFlushEnable = true;
         break;
      case VK_MEMORY_OUTPUT_DEPTH_STENCIL_ATTACHMENT_BIT:
         cmd.DepthCacheFlushEnable = true;
         break;
      case VK_MEMORY_OUTPUT_TRANSFER_BIT:
         cmd.RenderTargetCacheFlushEnable = true;
         cmd.DepthCacheFlushEnable = true;
         break;
      default:
         unreachable("Invalid memory output flag");
      }
   }

   for_each_bit(b, out_flags) {
      switch ((VkMemoryInputFlags)(1 << b)) {
      case VK_MEMORY_INPUT_HOST_READ_BIT:
         break; /* FIXME: Little-core systems */
      case VK_MEMORY_INPUT_INDIRECT_COMMAND_BIT:
      case VK_MEMORY_INPUT_INDEX_FETCH_BIT:
      case VK_MEMORY_INPUT_VERTEX_ATTRIBUTE_FETCH_BIT:
         cmd.VFCacheInvalidationEnable = true;
         break;
      case VK_MEMORY_INPUT_UNIFORM_READ_BIT:
         cmd.ConstantCacheInvalidationEnable = true;
         /* fallthrough */
      case VK_MEMORY_INPUT_SHADER_READ_BIT:
         cmd.DCFlushEnable = true;
         cmd.TextureCacheInvalidationEnable = true;
         break;
      case VK_MEMORY_INPUT_COLOR_ATTACHMENT_BIT:
      case VK_MEMORY_INPUT_DEPTH_STENCIL_ATTACHMENT_BIT:
         break; /* XXX: Hunh? */
      case VK_MEMORY_INPUT_TRANSFER_BIT:
         cmd.TextureCacheInvalidationEnable = true;
         break;
      }
   }

   dw = anv_batch_emit_dwords(&cmd_buffer->batch, GENX(PIPE_CONTROL_length));
   GENX(PIPE_CONTROL_pack)(&cmd_buffer->batch, dw, &cmd);
}
