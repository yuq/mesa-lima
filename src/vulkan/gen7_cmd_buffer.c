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


void
gen7_cmd_buffer_emit_state_base_address(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_bo *scratch_bo = NULL;

   cmd_buffer->state.scratch_size =
      anv_block_pool_size(&device->scratch_block_pool);
   if (cmd_buffer->state.scratch_size > 0)
      scratch_bo = &device->scratch_block_pool.bo;

   anv_batch_emit(&cmd_buffer->batch, GEN7_STATE_BASE_ADDRESS,
      .GeneralStateBaseAddress                  = { scratch_bo, 0 },
      .GeneralStateMemoryObjectControlState     = GEN7_MOCS,
      .GeneralStateBaseAddressModifyEnable      = true,
      .GeneralStateAccessUpperBound             = { scratch_bo, scratch_bo->size },
      .GeneralStateAccessUpperBoundModifyEnable = true,

      .SurfaceStateBaseAddress                  = anv_cmd_buffer_surface_base_address(cmd_buffer),
      .SurfaceStateMemoryObjectControlState     = GEN7_MOCS,
      .SurfaceStateBaseAddressModifyEnable      = true,

      .DynamicStateBaseAddress                  = { &device->dynamic_state_block_pool.bo, 0 },
      .DynamicStateMemoryObjectControlState     = GEN7_MOCS,
      .DynamicStateBaseAddressModifyEnable      = true,
      .DynamicStateAccessUpperBound             = { &device->dynamic_state_block_pool.bo,
                                                    device->dynamic_state_block_pool.bo.size },
      .DynamicStateAccessUpperBoundModifyEnable = true,

      .IndirectObjectBaseAddress                = { NULL, 0 },
      .IndirectObjectMemoryObjectControlState   = GEN7_MOCS,
      .IndirectObjectBaseAddressModifyEnable    = true,

      .IndirectObjectAccessUpperBound           = { NULL, 0xffffffff },
      .IndirectObjectAccessUpperBoundModifyEnable = true,

      .InstructionBaseAddress                   = { &device->instruction_block_pool.bo, 0 },
      .InstructionMemoryObjectControlState      = GEN7_MOCS,
      .InstructionBaseAddressModifyEnable       = true,
      .InstructionAccessUpperBound              =  { &device->instruction_block_pool.bo,
                                                     device->instruction_block_pool.bo.size },
      .InstructionAccessUpperBoundModifyEnable  = true);

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
   anv_batch_emit(&cmd_buffer->batch, GEN7_PIPE_CONTROL,
                  .TextureCacheInvalidationEnable = true);
}

static const uint32_t vk_to_gen_index_type[] = {
   [VK_INDEX_TYPE_UINT16]                       = INDEX_WORD,
   [VK_INDEX_TYPE_UINT32]                       = INDEX_DWORD,
};

void gen7_CmdBindIndexBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    VkIndexType                                 indexType)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);

   cmd_buffer->state.dirty |= ANV_CMD_BUFFER_INDEX_BUFFER_DIRTY;
   cmd_buffer->state.gen7.index_buffer = buffer;
   cmd_buffer->state.gen7.index_type = vk_to_gen_index_type[indexType];
   cmd_buffer->state.gen7.index_offset = offset;
}

static VkResult
gen7_flush_compute_descriptor_set(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   struct anv_state surfaces = { 0, }, samplers = { 0, };
   VkResult result;

   result = anv_cmd_buffer_emit_samplers(cmd_buffer,
                                         VK_SHADER_STAGE_COMPUTE, &samplers);
   if (result != VK_SUCCESS)
      return result;
   result = anv_cmd_buffer_emit_binding_table(cmd_buffer,
                                               VK_SHADER_STAGE_COMPUTE, &surfaces);
   if (result != VK_SUCCESS)
      return result;

   struct GEN7_INTERFACE_DESCRIPTOR_DATA desc = {
      .KernelStartPointer = pipeline->cs_simd,
      .BindingTablePointer = surfaces.offset,
      .SamplerStatePointer = samplers.offset,
      .NumberofThreadsinGPGPUThreadGroup = 0 /* FIXME: Really? */
   };

   uint32_t size = GEN7_INTERFACE_DESCRIPTOR_DATA_length * sizeof(uint32_t);
   struct anv_state state =
      anv_state_pool_alloc(&device->dynamic_state_pool, size, 64);

   GEN7_INTERFACE_DESCRIPTOR_DATA_pack(NULL, state.map, &desc);

   anv_batch_emit(&cmd_buffer->batch, GEN7_MEDIA_INTERFACE_DESCRIPTOR_LOAD,
                  .InterfaceDescriptorTotalLength = size,
                  .InterfaceDescriptorDataStartAddress = state.offset);

   return VK_SUCCESS;
}

static void
gen7_cmd_buffer_flush_compute_state(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   VkResult result;

   assert(pipeline->active_stages == VK_SHADER_STAGE_COMPUTE_BIT);

   if (cmd_buffer->state.current_pipeline != GPGPU) {
      anv_batch_emit(&cmd_buffer->batch, GEN7_PIPELINE_SELECT,
                     .PipelineSelection = GPGPU);
      cmd_buffer->state.current_pipeline = GPGPU;
   }

   if (cmd_buffer->state.compute_dirty & ANV_CMD_BUFFER_PIPELINE_DIRTY)
      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);

   if ((cmd_buffer->state.descriptors_dirty & VK_SHADER_STAGE_COMPUTE_BIT) ||
       (cmd_buffer->state.compute_dirty & ANV_CMD_BUFFER_PIPELINE_DIRTY)) {
      /* FIXME: figure out descriptors for gen7 */
      result = gen7_flush_compute_descriptor_set(cmd_buffer);
      assert(result == VK_SUCCESS);
      cmd_buffer->state.descriptors_dirty &= ~VK_SHADER_STAGE_COMPUTE;
   }

   cmd_buffer->state.compute_dirty = 0;
}

static void
gen7_cmd_buffer_flush_state(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;
   uint32_t *p;

   uint32_t vb_emit = cmd_buffer->state.vb_dirty & pipeline->vb_used;

   assert((pipeline->active_stages & VK_SHADER_STAGE_COMPUTE_BIT) == 0);

   if (cmd_buffer->state.current_pipeline != _3D) {
      anv_batch_emit(&cmd_buffer->batch, GEN7_PIPELINE_SELECT,
                     .PipelineSelection = _3D);
      cmd_buffer->state.current_pipeline = _3D;
   }

   if (vb_emit) {
      const uint32_t num_buffers = __builtin_popcount(vb_emit);
      const uint32_t num_dwords = 1 + num_buffers * 4;

      p = anv_batch_emitn(&cmd_buffer->batch, num_dwords,
                          GEN7_3DSTATE_VERTEX_BUFFERS);
      uint32_t vb, i = 0;
      for_each_bit(vb, vb_emit) {
         struct anv_buffer *buffer = cmd_buffer->state.vertex_bindings[vb].buffer;
         uint32_t offset = cmd_buffer->state.vertex_bindings[vb].offset;

         struct GEN7_VERTEX_BUFFER_STATE state = {
            .VertexBufferIndex = vb,
            .BufferAccessType = pipeline->instancing_enable[vb] ? INSTANCEDATA : VERTEXDATA,
            .VertexBufferMemoryObjectControlState = GEN7_MOCS,
            .AddressModifyEnable = true,
            .BufferPitch = pipeline->binding_stride[vb],
            .BufferStartingAddress = { buffer->bo, buffer->offset + offset },
            .EndAddress = { buffer->bo, buffer->offset + buffer->size - 1},
            .InstanceDataStepRate = 1
         };

         GEN7_VERTEX_BUFFER_STATE_pack(&cmd_buffer->batch, &p[1 + i * 4], &state);
         i++;
      }
   }

   if (cmd_buffer->state.dirty & ANV_CMD_BUFFER_PIPELINE_DIRTY) {
      /* If somebody compiled a pipeline after starting a command buffer the
       * scratch bo may have grown since we started this cmd buffer (and
       * emitted STATE_BASE_ADDRESS).  If we're binding that pipeline now,
       * reemit STATE_BASE_ADDRESS so that we use the bigger scratch bo. */
      if (cmd_buffer->state.scratch_size < pipeline->total_scratch)
         gen7_cmd_buffer_emit_state_base_address(cmd_buffer);

      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);
   }

   if (cmd_buffer->state.descriptors_dirty)
      anv_flush_descriptor_sets(cmd_buffer);

   if (cmd_buffer->state.dirty & ANV_CMD_BUFFER_VP_DIRTY) {
      struct anv_dynamic_vp_state *vp_state = cmd_buffer->state.vp_state;
      anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_SCISSOR_STATE_POINTERS,
                     .ScissorRectPointer = vp_state->scissor.offset);
      anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_CC,
                     .CCViewportPointer = vp_state->cc_vp.offset);
      anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP,
                     .SFClipViewportPointer = vp_state->sf_clip_vp.offset);
   }

   if (cmd_buffer->state.dirty &
       (ANV_CMD_BUFFER_PIPELINE_DIRTY | ANV_CMD_BUFFER_RS_DIRTY)) {
      anv_batch_emit_merge(&cmd_buffer->batch,
                           cmd_buffer->state.rs_state->gen7.sf,
                           pipeline->gen7.sf);
   }

   if (cmd_buffer->state.dirty &
       (ANV_CMD_BUFFER_PIPELINE_DIRTY | ANV_CMD_BUFFER_DS_DIRTY)) {
      struct anv_state state;

      if (cmd_buffer->state.ds_state == NULL)
         state = anv_cmd_buffer_emit_dynamic(cmd_buffer,
                                             pipeline->gen7.depth_stencil_state,
                                             GEN7_COLOR_CALC_STATE_length, 64);
      else
         state = anv_cmd_buffer_merge_dynamic(cmd_buffer,
                                              cmd_buffer->state.ds_state->gen7.depth_stencil_state,
                                              pipeline->gen7.depth_stencil_state,
                                              GEN7_DEPTH_STENCIL_STATE_length, 64);
      anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_DEPTH_STENCIL_STATE_POINTERS,
                     .PointertoDEPTH_STENCIL_STATE = state.offset);
   }

   if (cmd_buffer->state.dirty &
       (ANV_CMD_BUFFER_CB_DIRTY | ANV_CMD_BUFFER_DS_DIRTY)) {
      struct anv_state state;
      if (cmd_buffer->state.ds_state == NULL)
         state = anv_cmd_buffer_emit_dynamic(cmd_buffer,
                                             cmd_buffer->state.cb_state->color_calc_state,
                                             GEN7_COLOR_CALC_STATE_length, 64);
      else if (cmd_buffer->state.cb_state == NULL)
         state = anv_cmd_buffer_emit_dynamic(cmd_buffer,
                                             cmd_buffer->state.ds_state->gen7.color_calc_state,
                                             GEN7_COLOR_CALC_STATE_length, 64);
      else
         state = anv_cmd_buffer_merge_dynamic(cmd_buffer,
                                              cmd_buffer->state.ds_state->gen7.color_calc_state,
                                              cmd_buffer->state.cb_state->color_calc_state,
                                              GEN7_COLOR_CALC_STATE_length, 64);

      anv_batch_emit(&cmd_buffer->batch,
                     GEN7_3DSTATE_CC_STATE_POINTERS,
                     .ColorCalcStatePointer = state.offset);
   }

   if (cmd_buffer->state.gen7.index_buffer &&
       cmd_buffer->state.dirty & (ANV_CMD_BUFFER_PIPELINE_DIRTY |
                                  ANV_CMD_BUFFER_INDEX_BUFFER_DIRTY)) {
      struct anv_buffer *buffer = cmd_buffer->state.gen7.index_buffer;
      uint32_t offset = cmd_buffer->state.gen7.index_offset;

      anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_INDEX_BUFFER,
                     .CutIndexEnable = pipeline->primitive_restart,
                     .IndexFormat = cmd_buffer->state.gen7.index_type,
                     .MemoryObjectControlState = GEN7_MOCS,
                     .BufferStartingAddress = { buffer->bo, buffer->offset + offset },
                     .BufferEndingAddress = { buffer->bo, buffer->offset + buffer->size });
   }

   cmd_buffer->state.vb_dirty &= ~vb_emit;
   cmd_buffer->state.dirty = 0;
}

void gen7_CmdDraw(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    firstVertex,
    uint32_t                                    vertexCount,
    uint32_t                                    firstInstance,
    uint32_t                                    instanceCount)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;

   gen7_cmd_buffer_flush_state(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GEN7_3DPRIMITIVE,
                  .VertexAccessType = SEQUENTIAL,
                  .PrimitiveTopologyType = pipeline->topology,
                  .VertexCountPerInstance = vertexCount,
                  .StartVertexLocation = firstVertex,
                  .InstanceCount = instanceCount,
                  .StartInstanceLocation = firstInstance,
                  .BaseVertexLocation = 0);
}

void gen7_CmdDrawIndexed(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    firstIndex,
    uint32_t                                    indexCount,
    int32_t                                     vertexOffset,
    uint32_t                                    firstInstance,
    uint32_t                                    instanceCount)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;

   gen7_cmd_buffer_flush_state(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GEN7_3DPRIMITIVE,
                  .VertexAccessType = RANDOM,
                  .PrimitiveTopologyType = pipeline->topology,
                  .VertexCountPerInstance = indexCount,
                  .StartVertexLocation = firstIndex,
                  .InstanceCount = instanceCount,
                  .StartInstanceLocation = firstInstance,
                  .BaseVertexLocation = vertexOffset);
}

static void
gen7_batch_lrm(struct anv_batch *batch,
              uint32_t reg, struct anv_bo *bo, uint32_t offset)
{
   anv_batch_emit(batch, GEN7_MI_LOAD_REGISTER_MEM,
                  .RegisterAddress = reg,
                  .MemoryAddress = { bo, offset });
}

static void
gen7_batch_lri(struct anv_batch *batch, uint32_t reg, uint32_t imm)
{
   anv_batch_emit(batch, GEN7_MI_LOAD_REGISTER_IMM,
                  .RegisterOffset = reg,
                  .DataDWord = imm);
}

/* Auto-Draw / Indirect Registers */
#define GEN7_3DPRIM_END_OFFSET          0x2420
#define GEN7_3DPRIM_START_VERTEX        0x2430
#define GEN7_3DPRIM_VERTEX_COUNT        0x2434
#define GEN7_3DPRIM_INSTANCE_COUNT      0x2438
#define GEN7_3DPRIM_START_INSTANCE      0x243C
#define GEN7_3DPRIM_BASE_VERTEX         0x2440

void gen7_CmdDrawIndirect(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    count,
    uint32_t                                    stride)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;
   struct anv_bo *bo = buffer->bo;
   uint32_t bo_offset = buffer->offset + offset;

   gen7_cmd_buffer_flush_state(cmd_buffer);

   gen7_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_VERTEX_COUNT, bo, bo_offset);
   gen7_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_INSTANCE_COUNT, bo, bo_offset + 4);
   gen7_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_VERTEX, bo, bo_offset + 8);
   gen7_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_INSTANCE, bo, bo_offset + 12);
   gen7_batch_lri(&cmd_buffer->batch, GEN7_3DPRIM_BASE_VERTEX, 0);

   anv_batch_emit(&cmd_buffer->batch, GEN7_3DPRIMITIVE,
                  .IndirectParameterEnable = true,
                  .VertexAccessType = SEQUENTIAL,
                  .PrimitiveTopologyType = pipeline->topology);
}

void gen7_CmdDrawIndexedIndirect(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    count,
    uint32_t                                    stride)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;
   struct anv_bo *bo = buffer->bo;
   uint32_t bo_offset = buffer->offset + offset;

   gen7_cmd_buffer_flush_state(cmd_buffer);

   gen7_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_VERTEX_COUNT, bo, bo_offset);
   gen7_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_INSTANCE_COUNT, bo, bo_offset + 4);
   gen7_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_VERTEX, bo, bo_offset + 8);
   gen7_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_BASE_VERTEX, bo, bo_offset + 12);
   gen7_batch_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_INSTANCE, bo, bo_offset + 16);

   anv_batch_emit(&cmd_buffer->batch, GEN7_3DPRIMITIVE,
                  .IndirectParameterEnable = true,
                  .VertexAccessType = RANDOM,
                  .PrimitiveTopologyType = pipeline->topology);
}

void gen7_CmdDispatch(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    x,
    uint32_t                                    y,
    uint32_t                                    z)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;

   gen7_cmd_buffer_flush_compute_state(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GEN7_GPGPU_WALKER,
                  .SIMDSize = prog_data->simd_size / 16,
                  .ThreadDepthCounterMaximum = 0,
                  .ThreadHeightCounterMaximum = 0,
                  .ThreadWidthCounterMaximum = pipeline->cs_thread_width_max,
                  .ThreadGroupIDXDimension = x,
                  .ThreadGroupIDYDimension = y,
                  .ThreadGroupIDZDimension = z,
                  .RightExecutionMask = pipeline->cs_right_mask,
                  .BottomExecutionMask = 0xffffffff);

   anv_batch_emit(&cmd_buffer->batch, GEN7_MEDIA_STATE_FLUSH);
}

#define GPGPU_DISPATCHDIMX 0x2500
#define GPGPU_DISPATCHDIMY 0x2504
#define GPGPU_DISPATCHDIMZ 0x2508

void gen7_CmdDispatchIndirect(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;
   struct anv_bo *bo = buffer->bo;
   uint32_t bo_offset = buffer->offset + offset;

   gen7_cmd_buffer_flush_compute_state(cmd_buffer);

   gen7_batch_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMX, bo, bo_offset);
   gen7_batch_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMY, bo, bo_offset + 4);
   gen7_batch_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMZ, bo, bo_offset + 8);

   anv_batch_emit(&cmd_buffer->batch, GEN7_GPGPU_WALKER,
                  .IndirectParameterEnable = true,
                  .SIMDSize = prog_data->simd_size / 16,
                  .ThreadDepthCounterMaximum = 0,
                  .ThreadHeightCounterMaximum = 0,
                  .ThreadWidthCounterMaximum = pipeline->cs_thread_width_max,
                  .RightExecutionMask = pipeline->cs_right_mask,
                  .BottomExecutionMask = 0xffffffff);

   anv_batch_emit(&cmd_buffer->batch, GEN7_MEDIA_STATE_FLUSH);
}

void gen7_CmdPipelineBarrier(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineStageFlags                        srcStageMask,
    VkPipelineStageFlags                        destStageMask,
    VkBool32                                    byRegion,
    uint32_t                                    memBarrierCount,
    const void* const*                          ppMemBarriers)
{
   stub();
}

static void
gen7_cmd_buffer_emit_depth_stencil(struct anv_cmd_buffer *cmd_buffer)
{
   const struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   const struct anv_depth_stencil_view *view =
      anv_cmd_buffer_get_depth_stencil_view(cmd_buffer);
   const struct anv_image *image = view ? view->image : NULL;
   const bool has_depth = view && view->format->depth_format;
   const bool has_stencil = view && view->format->has_stencil;

   /* Emit 3DSTATE_DEPTH_BUFFER */
   if (has_depth) {
      anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_DEPTH_BUFFER,
         .SurfaceType = SURFTYPE_2D,
         .DepthWriteEnable = view->format->depth_format,
         .StencilWriteEnable = has_stencil,
         .HierarchicalDepthBufferEnable = false,
         .SurfaceFormat = view->format->depth_format,
         .SurfacePitch = image->depth_surface.stride - 1,
         .SurfaceBaseAddress = {
            .bo = image->bo,
            .offset = image->depth_surface.offset,
         },
         .Height = fb->height - 1,
         .Width = fb->width - 1,
         .LOD = 0,
         .Depth = 1 - 1,
         .MinimumArrayElement = 0,
         .DepthBufferObjectControlState = GEN7_MOCS,
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
      anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_DEPTH_BUFFER,
         .SurfaceType = SURFTYPE_2D,
         .SurfaceFormat = D16_UNORM,
         .Width = fb->width - 1,
         .Height = fb->height - 1,
         .StencilWriteEnable = has_stencil);
   }

   /* Emit 3DSTATE_STENCIL_BUFFER */
   if (has_stencil) {
      anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_STENCIL_BUFFER,
         .StencilBufferObjectControlState = GEN7_MOCS,

         /* Stencil buffers have strange pitch. The PRM says:
          *
          *    The pitch must be set to 2x the value computed based on width,
          *    as the stencil buffer is stored with two rows interleaved.
          */
         .SurfacePitch = 2 * image->stencil_surface.stride - 1,

         .SurfaceBaseAddress = {
            .bo = image->bo,
            .offset = image->offset + image->stencil_surface.offset,
         });
   } else {
      anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_STENCIL_BUFFER);
   }

   /* Disable hierarchial depth buffers. */
   anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_HIER_DEPTH_BUFFER);

   /* Clear the clear params. */
   anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_CLEAR_PARAMS);
}

void
gen7_cmd_buffer_begin_subpass(struct anv_cmd_buffer *cmd_buffer,
                             struct anv_subpass *subpass)
{
   cmd_buffer->state.subpass = subpass;
   cmd_buffer->state.descriptors_dirty |= VK_SHADER_STAGE_FRAGMENT_BIT;

   gen7_cmd_buffer_emit_depth_stencil(cmd_buffer);
}

static void
begin_render_pass(struct anv_cmd_buffer *cmd_buffer,
                  const VkRenderPassBeginInfo* pRenderPassBegin)
{
   ANV_FROM_HANDLE(anv_render_pass, pass, pRenderPassBegin->renderPass);
   ANV_FROM_HANDLE(anv_framebuffer, framebuffer, pRenderPassBegin->framebuffer);

   cmd_buffer->state.framebuffer = framebuffer;
   cmd_buffer->state.pass = pass;

   const VkRect2D *render_area = &pRenderPassBegin->renderArea;

   anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_DRAWING_RECTANGLE,
                  .ClippedDrawingRectangleYMin = render_area->offset.y,
                  .ClippedDrawingRectangleXMin = render_area->offset.x,
                  .ClippedDrawingRectangleYMax =
                     render_area->offset.y + render_area->extent.height - 1,
                  .ClippedDrawingRectangleXMax =
                     render_area->offset.x + render_area->extent.width - 1,
                  .DrawingRectangleOriginY = 0,
                  .DrawingRectangleOriginX = 0);

   anv_cmd_buffer_clear_attachments(cmd_buffer, pass,
                                    pRenderPassBegin->pClearValues);
}

void gen7_CmdBeginRenderPass(
    VkCmdBuffer                                 cmdBuffer,
    const VkRenderPassBeginInfo*                pRenderPassBegin,
    VkRenderPassContents                        contents)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_render_pass, pass, pRenderPassBegin->renderPass);

   begin_render_pass(cmd_buffer, pRenderPassBegin);

   gen7_cmd_buffer_begin_subpass(cmd_buffer, pass->subpasses);
}

void gen7_CmdNextSubpass(
    VkCmdBuffer                                 cmdBuffer,
    VkRenderPassContents                        contents)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   assert(cmd_buffer->level == VK_CMD_BUFFER_LEVEL_PRIMARY);

   gen7_cmd_buffer_begin_subpass(cmd_buffer, cmd_buffer->state.subpass + 1);
}

void gen7_CmdEndRenderPass(
    VkCmdBuffer                                 cmdBuffer)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   /* Emit a flushing pipe control at the end of a pass.  This is kind of a
    * hack but it ensures that render targets always actually get written.
    * Eventually, we should do flushing based on image format transitions
    * or something of that nature.
    */
   anv_batch_emit(&cmd_buffer->batch, GEN7_PIPE_CONTROL,
                  .PostSyncOperation = NoWrite,
                  .RenderTargetCacheFlushEnable = true,
                  .InstructionCacheInvalidateEnable = true,
                  .DepthCacheFlushEnable = true,
                  .VFCacheInvalidationEnable = true,
                  .TextureCacheInvalidationEnable = true,
                  .CommandStreamerStallEnable = true);
}
