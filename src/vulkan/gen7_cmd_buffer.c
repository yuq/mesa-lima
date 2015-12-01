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

#include "gen7_pack.h"
#include "gen75_pack.h"

static void
cmd_buffer_flush_push_constants(struct anv_cmd_buffer *cmd_buffer)
{
   static const uint32_t push_constant_opcodes[] = {
      [VK_SHADER_STAGE_VERTEX]                  = 21,
      [VK_SHADER_STAGE_TESS_CONTROL]            = 25, /* HS */
      [VK_SHADER_STAGE_TESS_EVALUATION]         = 26, /* DS */
      [VK_SHADER_STAGE_GEOMETRY]                = 22,
      [VK_SHADER_STAGE_FRAGMENT]                = 23,
      [VK_SHADER_STAGE_COMPUTE]                 = 0,
   };

   VkShaderStage stage;
   VkShaderStageFlags flushed = 0;

   for_each_bit(stage, cmd_buffer->state.push_constants_dirty) {
      struct anv_state state = anv_cmd_buffer_push_constants(cmd_buffer, stage);

      if (state.offset == 0)
         continue;

      anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_CONSTANT_VS,
                     ._3DCommandSubOpcode = push_constant_opcodes[stage],
                     .ConstantBody = {
                        .PointerToConstantBuffer0 = { .offset = state.offset },
                        .ConstantBuffer0ReadLength = DIV_ROUND_UP(state.alloc_size, 32),
                     });

      flushed |= 1 << stage;
   }

   cmd_buffer->state.push_constants_dirty &= ~flushed;
}

static VkResult
flush_descriptor_set(struct anv_cmd_buffer *cmd_buffer, VkShaderStage stage)
{
   struct anv_state surfaces = { 0, }, samplers = { 0, };
   VkResult result;

   result = anv_cmd_buffer_emit_samplers(cmd_buffer, stage, &samplers);
   if (result != VK_SUCCESS)
      return result;
   result = anv_cmd_buffer_emit_binding_table(cmd_buffer, stage, &surfaces);
   if (result != VK_SUCCESS)
      return result;

   static const uint32_t sampler_state_opcodes[] = {
      [VK_SHADER_STAGE_VERTEX]                  = 43,
      [VK_SHADER_STAGE_TESS_CONTROL]            = 44, /* HS */
      [VK_SHADER_STAGE_TESS_EVALUATION]         = 45, /* DS */
      [VK_SHADER_STAGE_GEOMETRY]                = 46,
      [VK_SHADER_STAGE_FRAGMENT]                = 47,
      [VK_SHADER_STAGE_COMPUTE]                 = 0,
   };

   static const uint32_t binding_table_opcodes[] = {
      [VK_SHADER_STAGE_VERTEX]                  = 38,
      [VK_SHADER_STAGE_TESS_CONTROL]            = 39,
      [VK_SHADER_STAGE_TESS_EVALUATION]         = 40,
      [VK_SHADER_STAGE_GEOMETRY]                = 41,
      [VK_SHADER_STAGE_FRAGMENT]                = 42,
      [VK_SHADER_STAGE_COMPUTE]                 = 0,
   };

   if (samplers.alloc_size > 0) {
      anv_batch_emit(&cmd_buffer->batch,
                     GEN7_3DSTATE_SAMPLER_STATE_POINTERS_VS,
                     ._3DCommandSubOpcode  = sampler_state_opcodes[stage],
                     .PointertoVSSamplerState = samplers.offset);
   }

   if (surfaces.alloc_size > 0) {
      anv_batch_emit(&cmd_buffer->batch,
                     GEN7_3DSTATE_BINDING_TABLE_POINTERS_VS,
                     ._3DCommandSubOpcode  = binding_table_opcodes[stage],
                     .PointertoVSBindingTable = surfaces.offset);
   }

   return VK_SUCCESS;
}

GENX_FUNC(GEN7, GEN7) void
genX(cmd_buffer_flush_descriptor_sets)(struct anv_cmd_buffer *cmd_buffer)
{
   VkShaderStage s;
   VkShaderStageFlags dirty = cmd_buffer->state.descriptors_dirty &
                              cmd_buffer->state.pipeline->active_stages;

   VkResult result = VK_SUCCESS;
   for_each_bit(s, dirty) {
      result = flush_descriptor_set(cmd_buffer, s);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      assert(result == VK_ERROR_OUT_OF_DEVICE_MEMORY);

      result = anv_cmd_buffer_new_binding_table_block(cmd_buffer);
      assert(result == VK_SUCCESS);

      /* Re-emit state base addresses so we get the new surface state base
       * address before we start emitting binding tables etc.
       */
      anv_cmd_buffer_emit_state_base_address(cmd_buffer);

      /* Re-emit all active binding tables */
      for_each_bit(s, cmd_buffer->state.pipeline->active_stages) {
         result = flush_descriptor_set(cmd_buffer, s);

         /* It had better succeed this time */
         assert(result == VK_SUCCESS);
      }
   }

   cmd_buffer->state.descriptors_dirty &= ~cmd_buffer->state.pipeline->active_stages;
}

static inline int64_t
clamp_int64(int64_t x, int64_t min, int64_t max)
{
   if (x < min)
      return min;
   else if (x < max)
      return x;
   else
      return max;
}

static void
emit_scissor_state(struct anv_cmd_buffer *cmd_buffer,
                   uint32_t count, const VkRect2D *scissors)
{
   struct anv_state scissor_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, count * 32, 32);

   for (uint32_t i = 0; i < count; i++) {
      const VkRect2D *s = &scissors[i];

      /* Since xmax and ymax are inclusive, we have to have xmax < xmin or
       * ymax < ymin for empty clips.  In case clip x, y, width height are all
       * 0, the clamps below produce 0 for xmin, ymin, xmax, ymax, which isn't
       * what we want. Just special case empty clips and produce a canonical
       * empty clip. */
      static const struct GEN7_SCISSOR_RECT empty_scissor = {
         .ScissorRectangleYMin = 1,
         .ScissorRectangleXMin = 1,
         .ScissorRectangleYMax = 0,
         .ScissorRectangleXMax = 0
      };

      const int max = 0xffff;
      struct GEN7_SCISSOR_RECT scissor = {
         /* Do this math using int64_t so overflow gets clamped correctly. */
         .ScissorRectangleYMin = clamp_int64(s->offset.y, 0, max),
         .ScissorRectangleXMin = clamp_int64(s->offset.x, 0, max),
         .ScissorRectangleYMax = clamp_int64((uint64_t) s->offset.y + s->extent.height - 1, 0, max),
         .ScissorRectangleXMax = clamp_int64((uint64_t) s->offset.x + s->extent.width - 1, 0, max)
      };

      if (s->extent.width <= 0 || s->extent.height <= 0) {
         GEN7_SCISSOR_RECT_pack(NULL, scissor_state.map + i * 32,
                                &empty_scissor);
      } else {
         GEN7_SCISSOR_RECT_pack(NULL, scissor_state.map + i * 32, &scissor);
      }
   }

   anv_batch_emit(&cmd_buffer->batch, GEN7_3DSTATE_SCISSOR_STATE_POINTERS,
                  .ScissorRectPointer = scissor_state.offset);
}

GENX_FUNC(GEN7, GEN7) void
genX(cmd_buffer_emit_scissor)(struct anv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->state.dynamic.scissor.count > 0) {
      emit_scissor_state(cmd_buffer, cmd_buffer->state.dynamic.scissor.count,
                         cmd_buffer->state.dynamic.scissor.scissors);
   } else {
      /* Emit a default scissor based on the currently bound framebuffer */
      emit_scissor_state(cmd_buffer, 1,
                         &(VkRect2D) {
                            .offset = { .x = 0, .y = 0, },
                            .extent = {
                               .width = cmd_buffer->state.framebuffer->width,
                               .height = cmd_buffer->state.framebuffer->height,
                            },
                         });
   }
}

static const uint32_t vk_to_gen_index_type[] = {
   [VK_INDEX_TYPE_UINT16]                       = INDEX_WORD,
   [VK_INDEX_TYPE_UINT32]                       = INDEX_DWORD,
};

static const uint32_t restart_index_for_type[] = {
   [VK_INDEX_TYPE_UINT16]                    = UINT16_MAX,
   [VK_INDEX_TYPE_UINT32]                    = UINT32_MAX,
};

void genX(CmdBindIndexBuffer)(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    VkIndexType                                 indexType)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);

   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_INDEX_BUFFER;
   if (ANV_IS_HASWELL)
      cmd_buffer->state.restart_index = restart_index_for_type[indexType];
   cmd_buffer->state.gen7.index_buffer = buffer;
   cmd_buffer->state.gen7.index_type = vk_to_gen_index_type[indexType];
   cmd_buffer->state.gen7.index_offset = offset;
}

static VkResult
flush_compute_descriptor_set(struct anv_cmd_buffer *cmd_buffer)
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
cmd_buffer_flush_compute_state(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   VkResult result;

   assert(pipeline->active_stages == VK_SHADER_STAGE_COMPUTE_BIT);

   if (cmd_buffer->state.current_pipeline != GPGPU) {
      anv_batch_emit(&cmd_buffer->batch, GEN7_PIPELINE_SELECT,
                     .PipelineSelection = GPGPU);
      cmd_buffer->state.current_pipeline = GPGPU;
   }

   if (cmd_buffer->state.compute_dirty & ANV_CMD_DIRTY_PIPELINE)
      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);

   if ((cmd_buffer->state.descriptors_dirty & VK_SHADER_STAGE_COMPUTE_BIT) ||
       (cmd_buffer->state.compute_dirty & ANV_CMD_DIRTY_PIPELINE)) {
      /* FIXME: figure out descriptors for gen7 */
      result = flush_compute_descriptor_set(cmd_buffer);
      assert(result == VK_SUCCESS);
      cmd_buffer->state.descriptors_dirty &= ~VK_SHADER_STAGE_COMPUTE;
   }

   cmd_buffer->state.compute_dirty = 0;
}

static void
cmd_buffer_flush_state(struct anv_cmd_buffer *cmd_buffer)
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

   if (cmd_buffer->state.dirty & ANV_CMD_DIRTY_PIPELINE) {
      /* If somebody compiled a pipeline after starting a command buffer the
       * scratch bo may have grown since we started this cmd buffer (and
       * emitted STATE_BASE_ADDRESS).  If we're binding that pipeline now,
       * reemit STATE_BASE_ADDRESS so that we use the bigger scratch bo. */
      if (cmd_buffer->state.scratch_size < pipeline->total_scratch)
         gen7_cmd_buffer_emit_state_base_address(cmd_buffer);

      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);
   }

   if (cmd_buffer->state.descriptors_dirty & VK_SHADER_STAGE_VERTEX_BIT ||
       cmd_buffer->state.push_constants_dirty & VK_SHADER_STAGE_VERTEX_BIT) {
      /* From the IVB PRM Vol. 2, Part 1, Section 3.2.1:
       *
       *    "A PIPE_CONTROL with Post-Sync Operation set to 1h and a depth
       *    stall needs to be sent just prior to any 3DSTATE_VS,
       *    3DSTATE_URB_VS, 3DSTATE_CONSTANT_VS,
       *    3DSTATE_BINDING_TABLE_POINTER_VS,
       *    3DSTATE_SAMPLER_STATE_POINTER_VS command.  Only one
       *    PIPE_CONTROL needs to be sent before any combination of VS
       *    associated 3DSTATE."
       */
      anv_batch_emit(&cmd_buffer->batch, GEN7_PIPE_CONTROL,
                     .DepthStallEnable = true,
                     .PostSyncOperation = WriteImmediateData,
                     .Address = { &cmd_buffer->device->workaround_bo, 0 });
   }

   if (cmd_buffer->state.descriptors_dirty)
      gen7_cmd_buffer_flush_descriptor_sets(cmd_buffer);

   if (cmd_buffer->state.push_constants_dirty)
      cmd_buffer_flush_push_constants(cmd_buffer);

   /* We use the gen8 state here because it only contains the additional
    * min/max fields and, since they occur at the end of the packet and
    * don't change the stride, they work on gen7 too.
    */
   if (cmd_buffer->state.dirty & ANV_CMD_DIRTY_DYNAMIC_VIEWPORT)
      gen8_cmd_buffer_emit_viewport(cmd_buffer);

   if (cmd_buffer->state.dirty & ANV_CMD_DIRTY_DYNAMIC_SCISSOR)
      gen7_cmd_buffer_emit_scissor(cmd_buffer);

   if (cmd_buffer->state.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                  ANV_CMD_DIRTY_DYNAMIC_LINE_WIDTH |
                                  ANV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS)) {

      bool enable_bias = cmd_buffer->state.dynamic.depth_bias.bias != 0.0f ||
         cmd_buffer->state.dynamic.depth_bias.slope != 0.0f;

      uint32_t sf_dw[GEN7_3DSTATE_SF_length];
      struct GEN7_3DSTATE_SF sf = {
         GEN7_3DSTATE_SF_header,
         .LineWidth = cmd_buffer->state.dynamic.line_width,
         .GlobalDepthOffsetEnableSolid = enable_bias,
         .GlobalDepthOffsetEnableWireframe = enable_bias,
         .GlobalDepthOffsetEnablePoint = enable_bias,
         .GlobalDepthOffsetConstant = cmd_buffer->state.dynamic.depth_bias.bias,
         .GlobalDepthOffsetScale = cmd_buffer->state.dynamic.depth_bias.slope,
         .GlobalDepthOffsetClamp = cmd_buffer->state.dynamic.depth_bias.clamp
      };
      GEN7_3DSTATE_SF_pack(NULL, sf_dw, &sf);

      anv_batch_emit_merge(&cmd_buffer->batch, sf_dw, pipeline->gen7.sf);
   }

   if (cmd_buffer->state.dirty & (ANV_CMD_DIRTY_DYNAMIC_BLEND_CONSTANTS |
                                  ANV_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE)) {
      struct anv_state cc_state =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer,
                                            GEN7_COLOR_CALC_STATE_length, 64);
      struct GEN7_COLOR_CALC_STATE cc = {
         .BlendConstantColorRed = cmd_buffer->state.dynamic.blend_constants[0],
         .BlendConstantColorGreen = cmd_buffer->state.dynamic.blend_constants[1],
         .BlendConstantColorBlue = cmd_buffer->state.dynamic.blend_constants[2],
         .BlendConstantColorAlpha = cmd_buffer->state.dynamic.blend_constants[3],
         .StencilReferenceValue =
            cmd_buffer->state.dynamic.stencil_reference.front,
         .BackFaceStencilReferenceValue =
            cmd_buffer->state.dynamic.stencil_reference.back,
      };
      GEN7_COLOR_CALC_STATE_pack(NULL, cc_state.map, &cc);

      anv_batch_emit(&cmd_buffer->batch,
                     GEN7_3DSTATE_CC_STATE_POINTERS,
                     .ColorCalcStatePointer = cc_state.offset);
   }

   if (cmd_buffer->state.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                  ANV_CMD_DIRTY_RENDER_TARGETS |
                                  ANV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK |
                                  ANV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK)) {
      uint32_t depth_stencil_dw[GEN7_DEPTH_STENCIL_STATE_length];

      const struct anv_image_view *iview =
         anv_cmd_buffer_get_depth_stencil_view(cmd_buffer);

      struct GEN7_DEPTH_STENCIL_STATE depth_stencil = {
         .StencilBufferWriteEnable = iview && iview->format->has_stencil,

         .StencilTestMask =
            cmd_buffer->state.dynamic.stencil_compare_mask.front & 0xff,
         .StencilWriteMask =
            cmd_buffer->state.dynamic.stencil_write_mask.front & 0xff,

         .BackfaceStencilTestMask =
            cmd_buffer->state.dynamic.stencil_compare_mask.back & 0xff,
         .BackfaceStencilWriteMask =
            cmd_buffer->state.dynamic.stencil_write_mask.back & 0xff,
      };
      GEN7_DEPTH_STENCIL_STATE_pack(NULL, depth_stencil_dw, &depth_stencil);

      struct anv_state ds_state =
         anv_cmd_buffer_merge_dynamic(cmd_buffer, depth_stencil_dw,
                                      pipeline->gen7.depth_stencil_state,
                                      GEN7_DEPTH_STENCIL_STATE_length, 64);

      anv_batch_emit(&cmd_buffer->batch,
                     GEN7_3DSTATE_DEPTH_STENCIL_STATE_POINTERS,
                     .PointertoDEPTH_STENCIL_STATE = ds_state.offset);
   }

   if (cmd_buffer->state.gen7.index_buffer &&
       cmd_buffer->state.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                  ANV_CMD_DIRTY_INDEX_BUFFER)) {
      struct anv_buffer *buffer = cmd_buffer->state.gen7.index_buffer;
      uint32_t offset = cmd_buffer->state.gen7.index_offset;

      if (ANV_IS_HASWELL) {
         anv_batch_emit(&cmd_buffer->batch, GEN75_3DSTATE_VF,
                        .IndexedDrawCutIndexEnable = pipeline->primitive_restart,
                        .CutIndex = cmd_buffer->state.restart_index);
      }

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

void genX(CmdDraw)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    vertexCount,
    uint32_t                                    instanceCount,
    uint32_t                                    firstVertex,
    uint32_t                                    firstInstance)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;

   cmd_buffer_flush_state(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GEN7_3DPRIMITIVE,
                  .VertexAccessType = SEQUENTIAL,
                  .PrimitiveTopologyType = pipeline->topology,
                  .VertexCountPerInstance = vertexCount,
                  .StartVertexLocation = firstVertex,
                  .InstanceCount = instanceCount,
                  .StartInstanceLocation = firstInstance,
                  .BaseVertexLocation = 0);
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

   cmd_buffer_flush_state(cmd_buffer);

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

   cmd_buffer_flush_state(cmd_buffer);

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

   cmd_buffer_flush_state(cmd_buffer);

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

void genX(CmdDispatch)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    x,
    uint32_t                                    y,
    uint32_t                                    z)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;

   cmd_buffer_flush_compute_state(cmd_buffer);

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

   cmd_buffer_flush_compute_state(cmd_buffer);

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

static void
cmd_buffer_emit_depth_stencil(struct anv_cmd_buffer *cmd_buffer)
{
   const struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   const struct anv_image_view *iview =
      anv_cmd_buffer_get_depth_stencil_view(cmd_buffer);
   const struct anv_image *image = iview ? iview->image : NULL;
   const bool has_depth = iview && iview->format->depth_format;
   const bool has_stencil = iview && iview->format->has_stencil;

   /* Emit 3DSTATE_DEPTH_BUFFER */
   if (has_depth) {
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_DEPTH_BUFFER),
         .SurfaceType = SURFTYPE_2D,
         .DepthWriteEnable = iview->format->depth_format,
         .StencilWriteEnable = has_stencil,
         .HierarchicalDepthBufferEnable = false,
         .SurfaceFormat = iview->format->depth_format,
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
         .DepthBufferObjectControlState = GENX(MOCS),
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
#     if (ANV_IS_HASWELL)
         .StencilBufferEnable = true,
#     endif
         .StencilBufferObjectControlState = GENX(MOCS),

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

GENX_FUNC(GEN7, GEN7) void
genX(cmd_buffer_begin_subpass)(struct anv_cmd_buffer *cmd_buffer,
                               struct anv_subpass *subpass)
{
   cmd_buffer->state.subpass = subpass;
   cmd_buffer->state.descriptors_dirty |= VK_SHADER_STAGE_FRAGMENT_BIT;
   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_RENDER_TARGETS;

   cmd_buffer_emit_depth_stencil(cmd_buffer);
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

void genX(CmdBeginRenderPass)(
    VkCommandBuffer                             commandBuffer,
    const VkRenderPassBeginInfo*                pRenderPassBegin,
    VkSubpassContents                           contents)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_render_pass, pass, pRenderPassBegin->renderPass);

   begin_render_pass(cmd_buffer, pRenderPassBegin);

   gen7_cmd_buffer_begin_subpass(cmd_buffer, pass->subpasses);
}

void genX(CmdNextSubpass)(
    VkCommandBuffer                             commandBuffer,
    VkSubpassContents                           contents)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   assert(cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);

   gen7_cmd_buffer_begin_subpass(cmd_buffer, cmd_buffer->state.subpass + 1);
}

void genX(CmdEndRenderPass)(
    VkCommandBuffer                             commandBuffer)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

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
