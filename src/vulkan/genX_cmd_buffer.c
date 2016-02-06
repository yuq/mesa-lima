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
#if (ANV_GEN >= 8)
   /* Emit a render target cache flush.
    *
    * This isn't documented anywhere in the PRM.  However, it seems to be
    * necessary prior to changing the surface state base adress.  Without
    * this, we get GPU hangs when using multi-level command buffers which
    * clear depth, reset state base address, and then go render stuff.
    */
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL),
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
    VkCommandBuffer                             commandBuffer,
    VkPipelineStageFlags                        srcStageMask,
    VkPipelineStageFlags                        destStageMask,
    VkBool32                                    byRegion,
    uint32_t                                    memoryBarrierCount,
    const VkMemoryBarrier*                      pMemoryBarriers,
    uint32_t                                    bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
    uint32_t                                    imageMemoryBarrierCount,
    const VkImageMemoryBarrier*                 pImageMemoryBarriers)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
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
   VkAccessFlags src_flags = 0;
   VkAccessFlags dst_flags = 0;

   for (uint32_t i = 0; i < memoryBarrierCount; i++) {
      src_flags |= pMemoryBarriers[i].srcAccessMask;
      dst_flags |= pMemoryBarriers[i].dstAccessMask;
   }

   for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++) {
      src_flags |= pBufferMemoryBarriers[i].srcAccessMask;
      dst_flags |= pBufferMemoryBarriers[i].dstAccessMask;
   }

   for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
      src_flags |= pImageMemoryBarriers[i].srcAccessMask;
      dst_flags |= pImageMemoryBarriers[i].dstAccessMask;
   }

   /* The src flags represent how things were used previously.  This is
    * what we use for doing flushes.
    */
   for_each_bit(b, src_flags) {
      switch ((VkAccessFlagBits)(1 << b)) {
      case VK_ACCESS_SHADER_WRITE_BIT:
         cmd.DCFlushEnable = true;
         break;
      case VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:
         cmd.RenderTargetCacheFlushEnable = true;
         break;
      case VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:
         cmd.DepthCacheFlushEnable = true;
         break;
      case VK_ACCESS_TRANSFER_WRITE_BIT:
         cmd.RenderTargetCacheFlushEnable = true;
         cmd.DepthCacheFlushEnable = true;
         break;
      default:
         /* Doesn't require a flush */
         break;
      }
   }

   /* The dst flags represent how things will be used in the fugure.  This
    * is what we use for doing cache invalidations.
    */
   for_each_bit(b, dst_flags) {
      switch ((VkAccessFlagBits)(1 << b)) {
      case VK_ACCESS_INDIRECT_COMMAND_READ_BIT:
      case VK_ACCESS_INDEX_READ_BIT:
      case VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT:
         cmd.VFCacheInvalidationEnable = true;
         break;
      case VK_ACCESS_UNIFORM_READ_BIT:
         cmd.ConstantCacheInvalidationEnable = true;
         /* fallthrough */
      case VK_ACCESS_SHADER_READ_BIT:
         cmd.TextureCacheInvalidationEnable = true;
         break;
      case VK_ACCESS_COLOR_ATTACHMENT_READ_BIT:
         cmd.TextureCacheInvalidationEnable = true;
         break;
      case VK_ACCESS_TRANSFER_READ_BIT:
         cmd.TextureCacheInvalidationEnable = true;
         break;
      case VK_ACCESS_MEMORY_READ_BIT:
         break; /* XXX: What is this? */
      default:
         /* Doesn't require a flush */
         break;
      }
   }

   dw = anv_batch_emit_dwords(&cmd_buffer->batch, GENX(PIPE_CONTROL_length));
   GENX(PIPE_CONTROL_pack)(&cmd_buffer->batch, dw, &cmd);
}

static void
emit_base_vertex_instance_bo(struct anv_cmd_buffer *cmd_buffer,
                             struct anv_bo *bo, uint32_t offset)
{
   uint32_t *p = anv_batch_emitn(&cmd_buffer->batch, 5,
                                 GENX(3DSTATE_VERTEX_BUFFERS));

   GENX(VERTEX_BUFFER_STATE_pack)(&cmd_buffer->batch, p + 1,
      &(struct GENX(VERTEX_BUFFER_STATE)) {
         .VertexBufferIndex = 32, /* Reserved for this */
         .AddressModifyEnable = true,
         .BufferPitch = 0,
#if (ANV_GEN >= 8)
         .MemoryObjectControlState = GENX(MOCS),
         .BufferStartingAddress = { bo, offset },
         .BufferSize = 8
#else
         .VertexBufferMemoryObjectControlState = GENX(MOCS),
         .BufferStartingAddress = { bo, offset },
         .EndAddress = { bo, offset + 8 },
#endif
      });
}

static void
emit_base_vertex_instance(struct anv_cmd_buffer *cmd_buffer,
                          uint32_t base_vertex, uint32_t base_instance)
{
   struct anv_state id_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, 8, 4);

   ((uint32_t *)id_state.map)[0] = base_vertex;
   ((uint32_t *)id_state.map)[1] = base_instance;

   if (!cmd_buffer->device->info.has_llc)
      anv_state_clflush(id_state);

   emit_base_vertex_instance_bo(cmd_buffer,
      &cmd_buffer->device->dynamic_state_block_pool.bo, id_state.offset);
}

void genX(CmdDraw)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    vertexCount,
    uint32_t                                    instanceCount,
    uint32_t                                    firstVertex,
    uint32_t                                    firstInstance)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;

   genX(cmd_buffer_flush_state)(cmd_buffer);

   if (cmd_buffer->state.pipeline->vs_prog_data.uses_basevertex ||
       cmd_buffer->state.pipeline->vs_prog_data.uses_baseinstance)
      emit_base_vertex_instance(cmd_buffer, firstVertex, firstInstance);

   anv_batch_emit(&cmd_buffer->batch, GENX(3DPRIMITIVE),
      .VertexAccessType                         = SEQUENTIAL,
      .PrimitiveTopologyType                    = pipeline->topology,
      .VertexCountPerInstance                   = vertexCount,
      .StartVertexLocation                      = firstVertex,
      .InstanceCount                            = instanceCount,
      .StartInstanceLocation                    = firstInstance,
      .BaseVertexLocation                       = 0);
}

void genX(CmdDrawIndexed)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    indexCount,
    uint32_t                                    instanceCount,
    uint32_t                                    firstIndex,
    int32_t                                     vertexOffset,
    uint32_t                                    firstInstance)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;

   genX(cmd_buffer_flush_state)(cmd_buffer);

   if (cmd_buffer->state.pipeline->vs_prog_data.uses_basevertex ||
       cmd_buffer->state.pipeline->vs_prog_data.uses_baseinstance)
      emit_base_vertex_instance(cmd_buffer, vertexOffset, firstInstance);

   anv_batch_emit(&cmd_buffer->batch, GENX(3DPRIMITIVE),
      .VertexAccessType                         = RANDOM,
      .PrimitiveTopologyType                    = pipeline->topology,
      .VertexCountPerInstance                   = indexCount,
      .StartVertexLocation                      = firstIndex,
      .InstanceCount                            = instanceCount,
      .StartInstanceLocation                    = firstInstance,
      .BaseVertexLocation                       = vertexOffset);
}

/* Auto-Draw / Indirect Registers */
#define GEN7_3DPRIM_END_OFFSET          0x2420
#define GEN7_3DPRIM_START_VERTEX        0x2430
#define GEN7_3DPRIM_VERTEX_COUNT        0x2434
#define GEN7_3DPRIM_INSTANCE_COUNT      0x2438
#define GEN7_3DPRIM_START_INSTANCE      0x243C
#define GEN7_3DPRIM_BASE_VERTEX         0x2440

static void
emit_lrm(struct anv_batch *batch,
         uint32_t reg, struct anv_bo *bo, uint32_t offset)
{
   anv_batch_emit(batch, GENX(MI_LOAD_REGISTER_MEM),
                  .RegisterAddress = reg,
                  .MemoryAddress = { bo, offset });
}

static void
emit_lri(struct anv_batch *batch, uint32_t reg, uint32_t imm)
{
   anv_batch_emit(batch, GENX(MI_LOAD_REGISTER_IMM),
                  .RegisterOffset = reg,
                  .DataDWord = imm);
}

void genX(CmdDrawIndirect)(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    drawCount,
    uint32_t                                    stride)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;
   struct anv_bo *bo = buffer->bo;
   uint32_t bo_offset = buffer->offset + offset;

   genX(cmd_buffer_flush_state)(cmd_buffer);

   if (cmd_buffer->state.pipeline->vs_prog_data.uses_basevertex ||
       cmd_buffer->state.pipeline->vs_prog_data.uses_baseinstance)
      emit_base_vertex_instance_bo(cmd_buffer, bo, bo_offset + 8);

   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_VERTEX_COUNT, bo, bo_offset);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_INSTANCE_COUNT, bo, bo_offset + 4);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_VERTEX, bo, bo_offset + 8);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_INSTANCE, bo, bo_offset + 12);
   emit_lri(&cmd_buffer->batch, GEN7_3DPRIM_BASE_VERTEX, 0);

   anv_batch_emit(&cmd_buffer->batch, GENX(3DPRIMITIVE),
      .IndirectParameterEnable                  = true,
      .VertexAccessType                         = SEQUENTIAL,
      .PrimitiveTopologyType                    = pipeline->topology);
}

void genX(CmdDrawIndexedIndirect)(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    drawCount,
    uint32_t                                    stride)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;
   struct anv_bo *bo = buffer->bo;
   uint32_t bo_offset = buffer->offset + offset;

   genX(cmd_buffer_flush_state)(cmd_buffer);

   /* TODO: We need to stomp base vertex to 0 somehow */
   if (cmd_buffer->state.pipeline->vs_prog_data.uses_basevertex ||
       cmd_buffer->state.pipeline->vs_prog_data.uses_baseinstance)
      emit_base_vertex_instance_bo(cmd_buffer, bo, bo_offset + 12);

   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_VERTEX_COUNT, bo, bo_offset);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_INSTANCE_COUNT, bo, bo_offset + 4);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_VERTEX, bo, bo_offset + 8);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_BASE_VERTEX, bo, bo_offset + 12);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_INSTANCE, bo, bo_offset + 16);

   anv_batch_emit(&cmd_buffer->batch, GENX(3DPRIMITIVE),
      .IndirectParameterEnable                  = true,
      .VertexAccessType                         = RANDOM,
      .PrimitiveTopologyType                    = pipeline->topology);
}
