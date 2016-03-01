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

#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"

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
#if (GEN_GEN >= 8)
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

#  if (GEN_GEN >= 8)
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

   /* Mask out the Source access flags we care about */
   const uint32_t src_mask =
      VK_ACCESS_SHADER_WRITE_BIT |
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_TRANSFER_WRITE_BIT;

   src_flags = src_flags & src_mask;

   /* Mask out the destination access flags we care about */
   const uint32_t dst_mask =
      VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
      VK_ACCESS_INDEX_READ_BIT |
      VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
      VK_ACCESS_UNIFORM_READ_BIT |
      VK_ACCESS_SHADER_READ_BIT |
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
      VK_ACCESS_TRANSFER_READ_BIT;

   dst_flags = dst_flags & dst_mask;

   /* The src flags represent how things were used previously.  This is
    * what we use for doing flushes.
    */
   struct GENX(PIPE_CONTROL) flush_cmd = {
      GENX(PIPE_CONTROL_header),
      .PostSyncOperation = NoWrite,
   };

   for_each_bit(b, src_flags) {
      switch ((VkAccessFlagBits)(1 << b)) {
      case VK_ACCESS_SHADER_WRITE_BIT:
         flush_cmd.DCFlushEnable = true;
         break;
      case VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:
         flush_cmd.RenderTargetCacheFlushEnable = true;
         break;
      case VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:
         flush_cmd.DepthCacheFlushEnable = true;
         break;
      case VK_ACCESS_TRANSFER_WRITE_BIT:
         flush_cmd.RenderTargetCacheFlushEnable = true;
         flush_cmd.DepthCacheFlushEnable = true;
         break;
      default:
         unreachable("should've masked this out by now");
      }
   }

   /* If we end up doing two PIPE_CONTROLs, the first, flusing one also has to
    * stall and wait for the flushing to finish, so we don't re-dirty the
    * caches with in-flight rendering after the second PIPE_CONTROL
    * invalidates.
    */

   if (dst_flags)
      flush_cmd.CommandStreamerStallEnable = true;

   if (src_flags && dst_flags) {
      dw = anv_batch_emit_dwords(&cmd_buffer->batch, GENX(PIPE_CONTROL_length));
      GENX(PIPE_CONTROL_pack)(&cmd_buffer->batch, dw, &flush_cmd);
   }

   /* The dst flags represent how things will be used in the future.  This
    * is what we use for doing cache invalidations.
    */
   struct GENX(PIPE_CONTROL) invalidate_cmd = {
      GENX(PIPE_CONTROL_header),
      .PostSyncOperation = NoWrite,
   };

   for_each_bit(b, dst_flags) {
      switch ((VkAccessFlagBits)(1 << b)) {
      case VK_ACCESS_INDIRECT_COMMAND_READ_BIT:
      case VK_ACCESS_INDEX_READ_BIT:
      case VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT:
         invalidate_cmd.VFCacheInvalidationEnable = true;
         break;
      case VK_ACCESS_UNIFORM_READ_BIT:
         invalidate_cmd.ConstantCacheInvalidationEnable = true;
         /* fallthrough */
      case VK_ACCESS_SHADER_READ_BIT:
         invalidate_cmd.TextureCacheInvalidationEnable = true;
         break;
      case VK_ACCESS_COLOR_ATTACHMENT_READ_BIT:
         invalidate_cmd.TextureCacheInvalidationEnable = true;
         break;
      case VK_ACCESS_TRANSFER_READ_BIT:
         invalidate_cmd.TextureCacheInvalidationEnable = true;
         break;
      default:
         unreachable("should've masked this out by now");
      }
   }

   if (dst_flags) {
      dw = anv_batch_emit_dwords(&cmd_buffer->batch, GENX(PIPE_CONTROL_length));
      GENX(PIPE_CONTROL_pack)(&cmd_buffer->batch, dw, &invalidate_cmd);
   }
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
#if (GEN_GEN >= 8)
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


void genX(CmdDispatch)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    x,
    uint32_t                                    y,
    uint32_t                                    z)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;

   if (prog_data->uses_num_work_groups) {
      struct anv_state state =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, 12, 4);
      uint32_t *sizes = state.map;
      sizes[0] = x;
      sizes[1] = y;
      sizes[2] = z;
      if (!cmd_buffer->device->info.has_llc)
         anv_state_clflush(state);
      cmd_buffer->state.num_workgroups_offset = state.offset;
      cmd_buffer->state.num_workgroups_bo =
         &cmd_buffer->device->dynamic_state_block_pool.bo;
   }

   genX(cmd_buffer_flush_compute_state)(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GENX(GPGPU_WALKER),
                  .SIMDSize = prog_data->simd_size / 16,
                  .ThreadDepthCounterMaximum = 0,
                  .ThreadHeightCounterMaximum = 0,
                  .ThreadWidthCounterMaximum = pipeline->cs_thread_width_max - 1,
                  .ThreadGroupIDXDimension = x,
                  .ThreadGroupIDYDimension = y,
                  .ThreadGroupIDZDimension = z,
                  .RightExecutionMask = pipeline->cs_right_mask,
                  .BottomExecutionMask = 0xffffffff);

   anv_batch_emit(&cmd_buffer->batch, GENX(MEDIA_STATE_FLUSH));
}

#define GPGPU_DISPATCHDIMX 0x2500
#define GPGPU_DISPATCHDIMY 0x2504
#define GPGPU_DISPATCHDIMZ 0x2508

void genX(CmdDispatchIndirect)(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;
   struct anv_bo *bo = buffer->bo;
   uint32_t bo_offset = buffer->offset + offset;

   if (prog_data->uses_num_work_groups) {
      cmd_buffer->state.num_workgroups_offset = bo_offset;
      cmd_buffer->state.num_workgroups_bo = bo;
   }

   genX(cmd_buffer_flush_compute_state)(cmd_buffer);

   emit_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMX, bo, bo_offset);
   emit_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMY, bo, bo_offset + 4);
   emit_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMZ, bo, bo_offset + 8);

   anv_batch_emit(&cmd_buffer->batch, GENX(GPGPU_WALKER),
                  .IndirectParameterEnable = true,
                  .SIMDSize = prog_data->simd_size / 16,
                  .ThreadDepthCounterMaximum = 0,
                  .ThreadHeightCounterMaximum = 0,
                  .ThreadWidthCounterMaximum = pipeline->cs_thread_width_max - 1,
                  .RightExecutionMask = pipeline->cs_right_mask,
                  .BottomExecutionMask = 0xffffffff);

   anv_batch_emit(&cmd_buffer->batch, GENX(MEDIA_STATE_FLUSH));
}

void
genX(flush_pipeline_select_3d)(struct anv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->state.current_pipeline != _3D) {
      anv_batch_emit(&cmd_buffer->batch, GENX(PIPELINE_SELECT),
#if GEN_GEN >= 9
                     .MaskBits = 3,
#endif
                     .PipelineSelection = _3D);
      cmd_buffer->state.current_pipeline = _3D;
   }
}

static void
cmd_buffer_emit_depth_stencil(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   const struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   const struct anv_image_view *iview =
      anv_cmd_buffer_get_depth_stencil_view(cmd_buffer);
   const struct anv_image *image = iview ? iview->image : NULL;
   const struct anv_format *anv_format =
      iview ? anv_format_for_vk_format(iview->vk_format) : NULL;
   const bool has_depth = iview && anv_format->has_depth;
   const bool has_stencil = iview && anv_format->has_stencil;

   /* FIXME: Implement the PMA stall W/A */
   /* FIXME: Width and Height are wrong */

   /* Emit 3DSTATE_DEPTH_BUFFER */
   if (has_depth) {
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_DEPTH_BUFFER),
         .SurfaceType = SURFTYPE_2D,
         .DepthWriteEnable = true,
         .StencilWriteEnable = has_stencil,
         .HierarchicalDepthBufferEnable = false,
         .SurfaceFormat = isl_surf_get_depth_format(&device->isl_dev,
                                                    &image->depth_surface.isl),
         .SurfacePitch = image->depth_surface.isl.row_pitch - 1,
         .SurfaceBaseAddress = {
            .bo = image->bo,
            .offset = image->offset + image->depth_surface.offset,
         },
         .Height = fb->height - 1,
         .Width = fb->width - 1,
         .LOD = 0,
         .Depth = 1 - 1,
         .MinimumArrayElement = 0,
         .DepthBufferObjectControlState = GENX(MOCS),
#if GEN_GEN >= 8
         .SurfaceQPitch = isl_surf_get_array_pitch_el_rows(&image->depth_surface.isl) >> 2,
#endif
         .RenderTargetViewExtent = 1 - 1);
   } else {
      /* Even when no depth buffer is present, the hardware requires that
       * 3DSTATE_DEPTH_BUFFER be programmed correctly. The Broadwell PRM says:
       *
       *    If a null depth buffer is bound, the driver must instead bind depth as:
       *       3DSTATE_DEPTH.SurfaceType = SURFTYPE_2D
       *       3DSTATE_DEPTH.Width = 1
       *       3DSTATE_DEPTH.Height = 1
       *       3DSTATE_DEPTH.SuraceFormat = D16_UNORM
       *       3DSTATE_DEPTH.SurfaceBaseAddress = 0
       *       3DSTATE_DEPTH.HierarchicalDepthBufferEnable = 0
       *       3DSTATE_WM_DEPTH_STENCIL.DepthTestEnable = 0
       *       3DSTATE_WM_DEPTH_STENCIL.DepthBufferWriteEnable = 0
       *
       * The PRM is wrong, though. The width and height must be programmed to
       * actual framebuffer's width and height, even when neither depth buffer
       * nor stencil buffer is present.
       */
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_DEPTH_BUFFER),
         .SurfaceType = SURFTYPE_2D,
         .SurfaceFormat = D16_UNORM,
         .Width = fb->width - 1,
         .Height = fb->height - 1,
         .StencilWriteEnable = has_stencil);
   }

   /* Emit 3DSTATE_STENCIL_BUFFER */
   if (has_stencil) {
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_STENCIL_BUFFER),
#if GEN_GEN >= 8 || GEN_IS_HASWELL
         .StencilBufferEnable = true,
#endif
         .StencilBufferObjectControlState = GENX(MOCS),

         /* Stencil buffers have strange pitch. The PRM says:
          *
          *    The pitch must be set to 2x the value computed based on width,
          *    as the stencil buffer is stored with two rows interleaved.
          */
         .SurfacePitch = 2 * image->stencil_surface.isl.row_pitch - 1,

#if GEN_GEN >= 8
         .SurfaceQPitch = isl_surf_get_array_pitch_el_rows(&image->stencil_surface.isl) >> 2,
#endif
         .SurfaceBaseAddress = {
            .bo = image->bo,
            .offset = image->offset + image->stencil_surface.offset,
         });
   } else {
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_STENCIL_BUFFER));
   }

   /* Disable hierarchial depth buffers. */
   anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_HIER_DEPTH_BUFFER));

   /* Clear the clear params. */
   anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_CLEAR_PARAMS));
}

/**
 * @see anv_cmd_buffer_set_subpass()
 */
void
genX(cmd_buffer_set_subpass)(struct anv_cmd_buffer *cmd_buffer,
                             struct anv_subpass *subpass)
{
   cmd_buffer->state.subpass = subpass;

   cmd_buffer->state.descriptors_dirty |= VK_SHADER_STAGE_FRAGMENT_BIT;

   cmd_buffer_emit_depth_stencil(cmd_buffer);
}

void genX(CmdBeginRenderPass)(
    VkCommandBuffer                             commandBuffer,
    const VkRenderPassBeginInfo*                pRenderPassBegin,
    VkSubpassContents                           contents)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_render_pass, pass, pRenderPassBegin->renderPass);
   ANV_FROM_HANDLE(anv_framebuffer, framebuffer, pRenderPassBegin->framebuffer);

   cmd_buffer->state.framebuffer = framebuffer;
   cmd_buffer->state.pass = pass;
   anv_cmd_state_setup_attachments(cmd_buffer, pRenderPassBegin);

   genX(flush_pipeline_select_3d)(cmd_buffer);

   const VkRect2D *render_area = &pRenderPassBegin->renderArea;

   anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_DRAWING_RECTANGLE),
                  .ClippedDrawingRectangleYMin = render_area->offset.y,
                  .ClippedDrawingRectangleXMin = render_area->offset.x,
                  .ClippedDrawingRectangleYMax =
                     render_area->offset.y + render_area->extent.height - 1,
                  .ClippedDrawingRectangleXMax =
                     render_area->offset.x + render_area->extent.width - 1,
                  .DrawingRectangleOriginY = 0,
                  .DrawingRectangleOriginX = 0);

   genX(cmd_buffer_set_subpass)(cmd_buffer, pass->subpasses);
   anv_cmd_buffer_clear_subpass(cmd_buffer);
}

void genX(CmdNextSubpass)(
    VkCommandBuffer                             commandBuffer,
    VkSubpassContents                           contents)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   assert(cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);

   anv_cmd_buffer_resolve_subpass(cmd_buffer);
   genX(cmd_buffer_set_subpass)(cmd_buffer, cmd_buffer->state.subpass + 1);
   anv_cmd_buffer_clear_subpass(cmd_buffer);
}

void genX(CmdEndRenderPass)(
    VkCommandBuffer                             commandBuffer)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   anv_cmd_buffer_resolve_subpass(cmd_buffer);
}
