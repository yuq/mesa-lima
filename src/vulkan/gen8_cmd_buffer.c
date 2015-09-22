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

static void
gen8_cmd_buffer_flush_push_constants(struct anv_cmd_buffer *cmd_buffer)
{
   uint32_t stage;

   static const uint32_t push_constant_opcodes[] = {
      [VK_SHADER_STAGE_VERTEX]                  = 21,
      [VK_SHADER_STAGE_TESS_CONTROL]            = 25, /* HS */
      [VK_SHADER_STAGE_TESS_EVALUATION]         = 26, /* DS */
      [VK_SHADER_STAGE_GEOMETRY]                = 22,
      [VK_SHADER_STAGE_FRAGMENT]                = 23,
      [VK_SHADER_STAGE_COMPUTE]                 = 0,
   };

   uint32_t flushed = 0;

   for_each_bit(stage, cmd_buffer->state.push_constants_dirty) {
      struct anv_state state = anv_cmd_buffer_push_constants(cmd_buffer, stage);

      if (state.offset == 0)
         continue;

      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_CONSTANT_VS,
                     ._3DCommandSubOpcode = push_constant_opcodes[stage],
                     .ConstantBody = {
                        .PointerToConstantBuffer0 = { .offset = state.offset },
                        .ConstantBuffer0ReadLength = DIV_ROUND_UP(state.alloc_size, 32),
                     });

      flushed |= 1 << stage;
   }

   cmd_buffer->state.push_constants_dirty &= ~flushed;
}

static void
gen8_cmd_buffer_flush_state(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;
   uint32_t *p;

   uint32_t vb_emit = cmd_buffer->state.vb_dirty & pipeline->vb_used;

   assert((pipeline->active_stages & VK_SHADER_STAGE_COMPUTE_BIT) == 0);

   if (cmd_buffer->state.current_pipeline != _3D) {
      anv_batch_emit(&cmd_buffer->batch, GEN8_PIPELINE_SELECT,
                     .PipelineSelection = _3D);
      cmd_buffer->state.current_pipeline = _3D;
   }

   if (vb_emit) {
      const uint32_t num_buffers = __builtin_popcount(vb_emit);
      const uint32_t num_dwords = 1 + num_buffers * 4;

      p = anv_batch_emitn(&cmd_buffer->batch, num_dwords,
                          GEN8_3DSTATE_VERTEX_BUFFERS);
      uint32_t vb, i = 0;
      for_each_bit(vb, vb_emit) {
         struct anv_buffer *buffer = cmd_buffer->state.vertex_bindings[vb].buffer;
         uint32_t offset = cmd_buffer->state.vertex_bindings[vb].offset;

         struct GEN8_VERTEX_BUFFER_STATE state = {
            .VertexBufferIndex = vb,
            .MemoryObjectControlState = GEN8_MOCS,
            .AddressModifyEnable = true,
            .BufferPitch = pipeline->binding_stride[vb],
            .BufferStartingAddress = { buffer->bo, buffer->offset + offset },
            .BufferSize = buffer->size - offset
         };

         GEN8_VERTEX_BUFFER_STATE_pack(&cmd_buffer->batch, &p[1 + i * 4], &state);
         i++;
      }
   }

   if (cmd_buffer->state.dirty & ANV_CMD_BUFFER_PIPELINE_DIRTY) {
      /* If somebody compiled a pipeline after starting a command buffer the
       * scratch bo may have grown since we started this cmd buffer (and
       * emitted STATE_BASE_ADDRESS).  If we're binding that pipeline now,
       * reemit STATE_BASE_ADDRESS so that we use the bigger scratch bo. */
      if (cmd_buffer->state.scratch_size < pipeline->total_scratch)
         anv_cmd_buffer_emit_state_base_address(cmd_buffer);

      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);
   }

   if (cmd_buffer->state.descriptors_dirty)
      anv_flush_descriptor_sets(cmd_buffer);

   if (cmd_buffer->state.push_constants_dirty)
      gen8_cmd_buffer_flush_push_constants(cmd_buffer);

   if (cmd_buffer->state.dirty & ANV_CMD_BUFFER_VP_DIRTY) {
      struct anv_dynamic_vp_state *vp_state = cmd_buffer->state.vp_state;
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_SCISSOR_STATE_POINTERS,
                     .ScissorRectPointer = vp_state->scissor.offset);
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_CC,
                     .CCViewportPointer = vp_state->cc_vp.offset);
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP,
                     .SFClipViewportPointer = vp_state->sf_clip_vp.offset);
   }

   if (cmd_buffer->state.dirty & (ANV_CMD_BUFFER_PIPELINE_DIRTY |
                                  ANV_CMD_BUFFER_RS_DIRTY)) {
      anv_batch_emit_merge(&cmd_buffer->batch,
                           cmd_buffer->state.rs_state->gen8.sf,
                           pipeline->gen8.sf);
      anv_batch_emit_merge(&cmd_buffer->batch,
                           cmd_buffer->state.rs_state->gen8.raster,
                           pipeline->gen8.raster);
   }

   if (cmd_buffer->state.ds_state &&
       (cmd_buffer->state.dirty & (ANV_CMD_BUFFER_PIPELINE_DIRTY |
                                   ANV_CMD_BUFFER_DS_DIRTY))) {
      anv_batch_emit_merge(&cmd_buffer->batch,
                           cmd_buffer->state.ds_state->gen8.wm_depth_stencil,
                           pipeline->gen8.wm_depth_stencil);
   }

   if (cmd_buffer->state.dirty & (ANV_CMD_BUFFER_CB_DIRTY |
                                  ANV_CMD_BUFFER_DS_DIRTY)) {
      struct anv_state state;
      if (cmd_buffer->state.ds_state == NULL)
         state = anv_cmd_buffer_emit_dynamic(cmd_buffer,
                                             cmd_buffer->state.cb_state->color_calc_state,
                                             GEN8_COLOR_CALC_STATE_length, 64);
      else if (cmd_buffer->state.cb_state == NULL)
         state = anv_cmd_buffer_emit_dynamic(cmd_buffer,
                                             cmd_buffer->state.ds_state->gen8.color_calc_state,
                                             GEN8_COLOR_CALC_STATE_length, 64);
      else
         state = anv_cmd_buffer_merge_dynamic(cmd_buffer,
                                              cmd_buffer->state.ds_state->gen8.color_calc_state,
                                              cmd_buffer->state.cb_state->color_calc_state,
                                              GEN8_COLOR_CALC_STATE_length, 64);

      anv_batch_emit(&cmd_buffer->batch,
                     GEN8_3DSTATE_CC_STATE_POINTERS,
                     .ColorCalcStatePointer = state.offset,
                     .ColorCalcStatePointerValid = true);
   }

   if (cmd_buffer->state.dirty & (ANV_CMD_BUFFER_PIPELINE_DIRTY |
                                  ANV_CMD_BUFFER_INDEX_BUFFER_DIRTY)) {
      anv_batch_emit_merge(&cmd_buffer->batch,
                           cmd_buffer->state.state_vf, pipeline->gen8.vf);
   }

   cmd_buffer->state.vb_dirty &= ~vb_emit;
   cmd_buffer->state.dirty = 0;
}

void gen8_CmdDraw(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    firstVertex,
    uint32_t                                    vertexCount,
    uint32_t                                    firstInstance,
    uint32_t                                    instanceCount)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   gen8_cmd_buffer_flush_state(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DPRIMITIVE,
                  .VertexAccessType = SEQUENTIAL,
                  .VertexCountPerInstance = vertexCount,
                  .StartVertexLocation = firstVertex,
                  .InstanceCount = instanceCount,
                  .StartInstanceLocation = firstInstance,
                  .BaseVertexLocation = 0);
}

void gen8_CmdDrawIndexed(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    firstIndex,
    uint32_t                                    indexCount,
    int32_t                                     vertexOffset,
    uint32_t                                    firstInstance,
    uint32_t                                    instanceCount)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   gen8_cmd_buffer_flush_state(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DPRIMITIVE,
                  .VertexAccessType = RANDOM,
                  .VertexCountPerInstance = indexCount,
                  .StartVertexLocation = firstIndex,
                  .InstanceCount = instanceCount,
                  .StartInstanceLocation = firstInstance,
                  .BaseVertexLocation = vertexOffset);
}

static void
emit_lrm(struct anv_batch *batch,
         uint32_t reg, struct anv_bo *bo, uint32_t offset)
{
   anv_batch_emit(batch, GEN8_MI_LOAD_REGISTER_MEM,
                  .RegisterAddress = reg,
                  .MemoryAddress = { bo, offset });
}

static void
emit_lri(struct anv_batch *batch, uint32_t reg, uint32_t imm)
{
   anv_batch_emit(batch, GEN8_MI_LOAD_REGISTER_IMM,
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

void gen8_CmdDrawIndirect(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    count,
    uint32_t                                    stride)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_bo *bo = buffer->bo;
   uint32_t bo_offset = buffer->offset + offset;

   gen8_cmd_buffer_flush_state(cmd_buffer);

   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_VERTEX_COUNT, bo, bo_offset);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_INSTANCE_COUNT, bo, bo_offset + 4);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_VERTEX, bo, bo_offset + 8);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_INSTANCE, bo, bo_offset + 12);
   emit_lri(&cmd_buffer->batch, GEN7_3DPRIM_BASE_VERTEX, 0);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DPRIMITIVE,
                  .IndirectParameterEnable = true,
                  .VertexAccessType = SEQUENTIAL);
}

void gen8_CmdBindIndexBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    VkIndexType                                 indexType)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);

   static const uint32_t vk_to_gen_index_type[] = {
      [VK_INDEX_TYPE_UINT16]                    = INDEX_WORD,
      [VK_INDEX_TYPE_UINT32]                    = INDEX_DWORD,
   };

   struct GEN8_3DSTATE_VF vf = {
      GEN8_3DSTATE_VF_header,
      .CutIndex = (indexType == VK_INDEX_TYPE_UINT16) ? UINT16_MAX : UINT32_MAX,
   };
   GEN8_3DSTATE_VF_pack(NULL, cmd_buffer->state.state_vf, &vf);

   cmd_buffer->state.dirty |= ANV_CMD_BUFFER_INDEX_BUFFER_DIRTY;

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_INDEX_BUFFER,
                  .IndexFormat = vk_to_gen_index_type[indexType],
                  .MemoryObjectControlState = GEN8_MOCS,
                  .BufferStartingAddress = { buffer->bo, buffer->offset + offset },
                  .BufferSize = buffer->size - offset);
}

static VkResult
gen8_flush_compute_descriptor_set(struct anv_cmd_buffer *cmd_buffer)
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

   struct GEN8_INTERFACE_DESCRIPTOR_DATA desc = {
      .KernelStartPointer = pipeline->cs_simd,
      .KernelStartPointerHigh = 0,
      .BindingTablePointer = surfaces.offset,
      .BindingTableEntryCount = 0,
      .SamplerStatePointer = samplers.offset,
      .SamplerCount = 0,
      .NumberofThreadsinGPGPUThreadGroup = 0 /* FIXME: Really? */
   };

   uint32_t size = GEN8_INTERFACE_DESCRIPTOR_DATA_length * sizeof(uint32_t);
   struct anv_state state =
      anv_state_pool_alloc(&device->dynamic_state_pool, size, 64);

   GEN8_INTERFACE_DESCRIPTOR_DATA_pack(NULL, state.map, &desc);

   anv_batch_emit(&cmd_buffer->batch, GEN8_MEDIA_INTERFACE_DESCRIPTOR_LOAD,
                  .InterfaceDescriptorTotalLength = size,
                  .InterfaceDescriptorDataStartAddress = state.offset);

   return VK_SUCCESS;
}

static void
gen8_cmd_buffer_flush_compute_state(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   VkResult result;

   assert(pipeline->active_stages == VK_SHADER_STAGE_COMPUTE_BIT);

   if (cmd_buffer->state.current_pipeline != GPGPU) {
      anv_batch_emit(&cmd_buffer->batch, GEN8_PIPELINE_SELECT,
                     .PipelineSelection = GPGPU);
      cmd_buffer->state.current_pipeline = GPGPU;
   }

   if (cmd_buffer->state.compute_dirty & ANV_CMD_BUFFER_PIPELINE_DIRTY)
      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);

   if ((cmd_buffer->state.descriptors_dirty & VK_SHADER_STAGE_COMPUTE_BIT) ||
       (cmd_buffer->state.compute_dirty & ANV_CMD_BUFFER_PIPELINE_DIRTY)) {
      result = gen8_flush_compute_descriptor_set(cmd_buffer);
      assert(result == VK_SUCCESS);
      cmd_buffer->state.descriptors_dirty &= ~VK_SHADER_STAGE_COMPUTE;
   }

   cmd_buffer->state.compute_dirty = 0;
}

void gen8_CmdDrawIndexedIndirect(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    count,
    uint32_t                                    stride)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_bo *bo = buffer->bo;
   uint32_t bo_offset = buffer->offset + offset;

   gen8_cmd_buffer_flush_state(cmd_buffer);

   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_VERTEX_COUNT, bo, bo_offset);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_INSTANCE_COUNT, bo, bo_offset + 4);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_VERTEX, bo, bo_offset + 8);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_BASE_VERTEX, bo, bo_offset + 12);
   emit_lrm(&cmd_buffer->batch, GEN7_3DPRIM_START_INSTANCE, bo, bo_offset + 16);

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DPRIMITIVE,
                  .IndirectParameterEnable = true,
                  .VertexAccessType = RANDOM);
}

void gen8_CmdDispatch(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    x,
    uint32_t                                    y,
    uint32_t                                    z)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;

   gen8_cmd_buffer_flush_compute_state(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GEN8_GPGPU_WALKER,
                  .SIMDSize = prog_data->simd_size / 16,
                  .ThreadDepthCounterMaximum = 0,
                  .ThreadHeightCounterMaximum = 0,
                  .ThreadWidthCounterMaximum = pipeline->cs_thread_width_max,
                  .ThreadGroupIDXDimension = x,
                  .ThreadGroupIDYDimension = y,
                  .ThreadGroupIDZDimension = z,
                  .RightExecutionMask = pipeline->cs_right_mask,
                  .BottomExecutionMask = 0xffffffff);

   anv_batch_emit(&cmd_buffer->batch, GEN8_MEDIA_STATE_FLUSH);
}

#define GPGPU_DISPATCHDIMX 0x2500
#define GPGPU_DISPATCHDIMY 0x2504
#define GPGPU_DISPATCHDIMZ 0x2508

void gen8_CmdDispatchIndirect(
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

   gen8_cmd_buffer_flush_compute_state(cmd_buffer);

   emit_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMX, bo, bo_offset);
   emit_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMY, bo, bo_offset + 4);
   emit_lrm(&cmd_buffer->batch, GPGPU_DISPATCHDIMZ, bo, bo_offset + 8);

   anv_batch_emit(&cmd_buffer->batch, GEN8_GPGPU_WALKER,
                  .IndirectParameterEnable = true,
                  .SIMDSize = prog_data->simd_size / 16,
                  .ThreadDepthCounterMaximum = 0,
                  .ThreadHeightCounterMaximum = 0,
                  .ThreadWidthCounterMaximum = pipeline->cs_thread_width_max,
                  .RightExecutionMask = pipeline->cs_right_mask,
                  .BottomExecutionMask = 0xffffffff);

   anv_batch_emit(&cmd_buffer->batch, GEN8_MEDIA_STATE_FLUSH);
}

static void
gen8_cmd_buffer_emit_depth_stencil(struct anv_cmd_buffer *cmd_buffer)
{
   const struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   const struct anv_depth_stencil_view *view =
      anv_cmd_buffer_get_depth_stencil_view(cmd_buffer);
   const struct anv_image *image = view ? view->image : NULL;
   const bool has_depth = view && view->format->depth_format;
   const bool has_stencil = view && view->format->has_stencil;

   /* FIXME: Implement the PMA stall W/A */
   /* FIXME: Width and Height are wrong */

   /* Emit 3DSTATE_DEPTH_BUFFER */
   if (has_depth) {
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_DEPTH_BUFFER,
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
         .DepthBufferObjectControlState = GEN8_MOCS,
         .RenderTargetViewExtent = 1 - 1,
         .SurfaceQPitch = image->depth_surface.qpitch >> 2);
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
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_DEPTH_BUFFER,
         .SurfaceType = SURFTYPE_2D,
         .SurfaceFormat = D16_UNORM,
         .Width = fb->width - 1,
         .Height = fb->height - 1,
         .StencilWriteEnable = has_stencil);
   }

   /* Emit 3DSTATE_STENCIL_BUFFER */
   if (has_stencil) {
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_STENCIL_BUFFER,
         .StencilBufferEnable = true,
         .StencilBufferObjectControlState = GEN8_MOCS,

         /* Stencil buffers have strange pitch. The PRM says:
          *
          *    The pitch must be set to 2x the value computed based on width,
          *    as the stencil buffer is stored with two rows interleaved.
          */
         .SurfacePitch = 2 * image->stencil_surface.stride - 1,

         .SurfaceBaseAddress = {
            .bo = image->bo,
            .offset = image->offset + image->stencil_surface.offset,
         },
         .SurfaceQPitch = image->stencil_surface.stride >> 2);
   } else {
      anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_STENCIL_BUFFER);
   }

   /* Disable hierarchial depth buffers. */
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_HIER_DEPTH_BUFFER);

   /* Clear the clear params. */
   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_CLEAR_PARAMS);
}

void
gen8_cmd_buffer_begin_subpass(struct anv_cmd_buffer *cmd_buffer,
                             struct anv_subpass *subpass)
{
   cmd_buffer->state.subpass = subpass;

   cmd_buffer->state.descriptors_dirty |= VK_SHADER_STAGE_FRAGMENT_BIT;

   gen8_cmd_buffer_emit_depth_stencil(cmd_buffer);
}

void gen8_CmdBeginRenderPass(
    VkCmdBuffer                                 cmdBuffer,
    const VkRenderPassBeginInfo*                pRenderPassBegin,
    VkRenderPassContents                        contents)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_render_pass, pass, pRenderPassBegin->renderPass);
   ANV_FROM_HANDLE(anv_framebuffer, framebuffer, pRenderPassBegin->framebuffer);

   cmd_buffer->state.framebuffer = framebuffer;
   cmd_buffer->state.pass = pass;

   const VkRect2D *render_area = &pRenderPassBegin->renderArea;

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_DRAWING_RECTANGLE,
                  .ClippedDrawingRectangleYMin = render_area->offset.y,
                  .ClippedDrawingRectangleXMin = render_area->offset.x,
                  .ClippedDrawingRectangleYMax =
                     render_area->offset.y + render_area->extent.height - 1,
                  .ClippedDrawingRectangleXMax =
                     render_area->offset.x + render_area->extent.width - 1,
                  .DrawingRectangleOriginY = 0,
                  .DrawingRectangleOriginX = 0);

   anv_cmd_buffer_clear_attachments(cmd_buffer, pass,
                                    pRenderPassBegin->pAttachmentClearValues);

   gen8_cmd_buffer_begin_subpass(cmd_buffer, pass->subpasses);
}

void gen8_CmdNextSubpass(
    VkCmdBuffer                                 cmdBuffer,
    VkRenderPassContents                        contents)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   assert(cmd_buffer->level == VK_CMD_BUFFER_LEVEL_PRIMARY);

   gen8_cmd_buffer_begin_subpass(cmd_buffer, cmd_buffer->state.subpass + 1);
}

void gen8_CmdEndRenderPass(
    VkCmdBuffer                                 cmdBuffer)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   /* Emit a flushing pipe control at the end of a pass.  This is kind of a
    * hack but it ensures that render targets always actually get written.
    * Eventually, we should do flushing based on image format transitions
    * or something of that nature.
    */
   anv_batch_emit(&cmd_buffer->batch, GEN8_PIPE_CONTROL,
                  .PostSyncOperation = NoWrite,
                  .RenderTargetCacheFlushEnable = true,
                  .InstructionCacheInvalidateEnable = true,
                  .DepthCacheFlushEnable = true,
                  .VFCacheInvalidationEnable = true,
                  .TextureCacheInvalidationEnable = true,
                  .CommandStreamerStallEnable = true);
}

static void
emit_ps_depth_count(struct anv_batch *batch,
                    struct anv_bo *bo, uint32_t offset)
{
   anv_batch_emit(batch, GEN8_PIPE_CONTROL,
                  .DestinationAddressType = DAT_PPGTT,
                  .PostSyncOperation = WritePSDepthCount,
                  .Address = { bo, offset });  /* FIXME: This is only lower 32 bits */
}

void gen8_CmdBeginQuery(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    slot,
    VkQueryControlFlags                         flags)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_query_pool, pool, queryPool);

   switch (pool->type) {
   case VK_QUERY_TYPE_OCCLUSION:
      emit_ps_depth_count(&cmd_buffer->batch, &pool->bo,
                                    slot * sizeof(struct anv_query_pool_slot));
      break;

   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
   default:
      unreachable("");
   }
}

void gen8_CmdEndQuery(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    slot)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_query_pool, pool, queryPool);

   switch (pool->type) {
   case VK_QUERY_TYPE_OCCLUSION:
      emit_ps_depth_count(&cmd_buffer->batch, &pool->bo,
                          slot * sizeof(struct anv_query_pool_slot) + 8);
      break;

   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
   default:
      unreachable("");
   }
}

#define TIMESTAMP 0x2358

void gen8_CmdWriteTimestamp(
    VkCmdBuffer                                 cmdBuffer,
    VkTimestampType                             timestampType,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, destBuffer);
   struct anv_bo *bo = buffer->bo;

   switch (timestampType) {
   case VK_TIMESTAMP_TYPE_TOP:
      anv_batch_emit(&cmd_buffer->batch, GEN8_MI_STORE_REGISTER_MEM,
                     .RegisterAddress = TIMESTAMP,
                     .MemoryAddress = { bo, buffer->offset + destOffset });
      anv_batch_emit(&cmd_buffer->batch, GEN8_MI_STORE_REGISTER_MEM,
                     .RegisterAddress = TIMESTAMP + 4,
                     .MemoryAddress = { bo, buffer->offset + destOffset + 4 });
      break;

   case VK_TIMESTAMP_TYPE_BOTTOM:
      anv_batch_emit(&cmd_buffer->batch, GEN8_PIPE_CONTROL,
                     .DestinationAddressType = DAT_PPGTT,
                     .PostSyncOperation = WriteTimestamp,
                     .Address = /* FIXME: This is only lower 32 bits */
                        { bo, buffer->offset + destOffset });
      break;

   default:
      break;
   }
}

#define alu_opcode(v)   __gen_field((v),  20, 31)
#define alu_operand1(v) __gen_field((v),  10, 19)
#define alu_operand2(v) __gen_field((v),   0,  9)
#define alu(opcode, operand1, operand2) \
   alu_opcode(opcode) | alu_operand1(operand1) | alu_operand2(operand2)

#define OPCODE_NOOP      0x000
#define OPCODE_LOAD      0x080
#define OPCODE_LOADINV   0x480
#define OPCODE_LOAD0     0x081
#define OPCODE_LOAD1     0x481
#define OPCODE_ADD       0x100
#define OPCODE_SUB       0x101
#define OPCODE_AND       0x102
#define OPCODE_OR        0x103
#define OPCODE_XOR       0x104
#define OPCODE_STORE     0x180
#define OPCODE_STOREINV  0x580

#define OPERAND_R0   0x00
#define OPERAND_R1   0x01
#define OPERAND_R2   0x02
#define OPERAND_R3   0x03
#define OPERAND_R4   0x04
#define OPERAND_SRCA 0x20
#define OPERAND_SRCB 0x21
#define OPERAND_ACCU 0x31
#define OPERAND_ZF   0x32
#define OPERAND_CF   0x33

#define CS_GPR(n) (0x2600 + (n) * 8)

static void
emit_load_alu_reg_u64(struct anv_batch *batch, uint32_t reg,
                      struct anv_bo *bo, uint32_t offset)
{
   anv_batch_emit(batch, GEN8_MI_LOAD_REGISTER_MEM,
                  .RegisterAddress = reg,
                  .MemoryAddress = { bo, offset });
   anv_batch_emit(batch, GEN8_MI_LOAD_REGISTER_MEM,
                  .RegisterAddress = reg + 4,
                  .MemoryAddress = { bo, offset + 4 });
}

void gen8_CmdCopyQueryPoolResults(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                destStride,
    VkQueryResultFlags                          flags)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_query_pool, pool, queryPool);
   ANV_FROM_HANDLE(anv_buffer, buffer, destBuffer);
   uint32_t slot_offset, dst_offset;

   if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
      /* Where is the availabilty info supposed to go? */
      anv_finishme("VK_QUERY_RESULT_WITH_AVAILABILITY_BIT");
      return;
   }

   assert(pool->type == VK_QUERY_TYPE_OCCLUSION);

   /* FIXME: If we're not waiting, should we just do this on the CPU? */
   if (flags & VK_QUERY_RESULT_WAIT_BIT)
      anv_batch_emit(&cmd_buffer->batch, GEN8_PIPE_CONTROL,
                     .CommandStreamerStallEnable = true,
                     .StallAtPixelScoreboard = true);

   dst_offset = buffer->offset + destOffset;
   for (uint32_t i = 0; i < queryCount; i++) {

      slot_offset = (startQuery + i) * sizeof(struct anv_query_pool_slot);

      emit_load_alu_reg_u64(&cmd_buffer->batch, CS_GPR(0), &pool->bo, slot_offset);
      emit_load_alu_reg_u64(&cmd_buffer->batch, CS_GPR(1), &pool->bo, slot_offset + 8);

      /* FIXME: We need to clamp the result for 32 bit. */

      uint32_t *dw = anv_batch_emitn(&cmd_buffer->batch, 5, GEN8_MI_MATH);
      dw[1] = alu(OPCODE_LOAD, OPERAND_SRCA, OPERAND_R1);
      dw[2] = alu(OPCODE_LOAD, OPERAND_SRCB, OPERAND_R0);
      dw[3] = alu(OPCODE_SUB, 0, 0);
      dw[4] = alu(OPCODE_STORE, OPERAND_R2, OPERAND_ACCU);

      anv_batch_emit(&cmd_buffer->batch, GEN8_MI_STORE_REGISTER_MEM,
                     .RegisterAddress = CS_GPR(2),
                     /* FIXME: This is only lower 32 bits */
                     .MemoryAddress = { buffer->bo, dst_offset });

      if (flags & VK_QUERY_RESULT_64_BIT)
         anv_batch_emit(&cmd_buffer->batch, GEN8_MI_STORE_REGISTER_MEM,
                        .RegisterAddress = CS_GPR(2) + 4,
                        /* FIXME: This is only lower 32 bits */
                        .MemoryAddress = { buffer->bo, dst_offset + 4 });

      dst_offset += destStride;
   }
}

void
gen8_cmd_buffer_emit_state_base_address(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_bo *scratch_bo = NULL;

   cmd_buffer->state.scratch_size =
      anv_block_pool_size(&device->scratch_block_pool);
   if (cmd_buffer->state.scratch_size > 0)
      scratch_bo = &device->scratch_block_pool.bo;

   anv_batch_emit(&cmd_buffer->batch, GEN8_STATE_BASE_ADDRESS,
                  .GeneralStateBaseAddress = { scratch_bo, 0 },
                  .GeneralStateMemoryObjectControlState = GEN8_MOCS,
                  .GeneralStateBaseAddressModifyEnable = true,
                  .GeneralStateBufferSize = 0xfffff,
                  .GeneralStateBufferSizeModifyEnable = true,

                  .SurfaceStateBaseAddress = anv_cmd_buffer_surface_base_address(cmd_buffer),
                  .SurfaceStateMemoryObjectControlState = GEN8_MOCS,
                  .SurfaceStateBaseAddressModifyEnable = true,

                  .DynamicStateBaseAddress = { &device->dynamic_state_block_pool.bo, 0 },
                  .DynamicStateMemoryObjectControlState = GEN8_MOCS,
                  .DynamicStateBaseAddressModifyEnable = true,
                  .DynamicStateBufferSize = 0xfffff,
                  .DynamicStateBufferSizeModifyEnable = true,

                  .IndirectObjectBaseAddress = { NULL, 0 },
                  .IndirectObjectMemoryObjectControlState = GEN8_MOCS,
                  .IndirectObjectBaseAddressModifyEnable = true,
                  .IndirectObjectBufferSize = 0xfffff,
                  .IndirectObjectBufferSizeModifyEnable = true,

                  .InstructionBaseAddress = { &device->instruction_block_pool.bo, 0 },
                  .InstructionMemoryObjectControlState = GEN8_MOCS,
                  .InstructionBaseAddressModifyEnable = true,
                  .InstructionBufferSize = 0xfffff,
                  .InstructionBuffersizeModifyEnable = true);

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
   anv_batch_emit(&cmd_buffer->batch, GEN8_PIPE_CONTROL,
                  .TextureCacheInvalidationEnable = true);
}

void gen8_CmdPipelineBarrier(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineStageFlags                        srcStageMask,
    VkPipelineStageFlags                        destStageMask,
    VkBool32                                    byRegion,
    uint32_t                                    memBarrierCount,
    const void* const*                          ppMemBarriers)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   uint32_t b, *dw;

   struct GEN8_PIPE_CONTROL cmd = {
      GEN8_PIPE_CONTROL_header,
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
                      VK_PIPELINE_STAGE_TESS_CONTROL_SHADER_BIT |
                      VK_PIPELINE_STAGE_TESS_EVALUATION_SHADER_BIT |
                      VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)) {
      cmd.StallAtPixelScoreboard = true;
   }


   if (anv_clear_mask(&srcStageMask,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_TRANSFER_BIT |
                      VK_PIPELINE_STAGE_TRANSITION_BIT)) {
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

   dw = anv_batch_emit_dwords(&cmd_buffer->batch, GEN8_PIPE_CONTROL_length);
   GEN8_PIPE_CONTROL_pack(&cmd_buffer->batch, dw, &cmd);
}
