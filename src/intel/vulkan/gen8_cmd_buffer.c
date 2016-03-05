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

static uint32_t
cmd_buffer_flush_push_constants(struct anv_cmd_buffer *cmd_buffer)
{
   static const uint32_t push_constant_opcodes[] = {
      [MESA_SHADER_VERTEX]                      = 21,
      [MESA_SHADER_TESS_CTRL]                   = 25, /* HS */
      [MESA_SHADER_TESS_EVAL]                   = 26, /* DS */
      [MESA_SHADER_GEOMETRY]                    = 22,
      [MESA_SHADER_FRAGMENT]                    = 23,
      [MESA_SHADER_COMPUTE]                     = 0,
   };

   VkShaderStageFlags flushed = 0;

   anv_foreach_stage(stage, cmd_buffer->state.push_constants_dirty) {
      if (stage == MESA_SHADER_COMPUTE)
         continue;

      struct anv_state state = anv_cmd_buffer_push_constants(cmd_buffer, stage);

      if (state.offset == 0) {
         anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_CONSTANT_VS),
                        ._3DCommandSubOpcode = push_constant_opcodes[stage]);
      } else {
         anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_CONSTANT_VS),
                        ._3DCommandSubOpcode = push_constant_opcodes[stage],
                        .ConstantBody = {
                           .PointerToConstantBuffer2 = { &cmd_buffer->device->dynamic_state_block_pool.bo, state.offset },
                           .ConstantBuffer2ReadLength = DIV_ROUND_UP(state.alloc_size, 32),
                        });
      }

      flushed |= mesa_to_vk_shader_stage(stage);
   }

   cmd_buffer->state.push_constants_dirty &= ~flushed;

   return flushed;
}

#if GEN_GEN == 8
static void
emit_viewport_state(struct anv_cmd_buffer *cmd_buffer,
                    uint32_t count, const VkViewport *viewports)
{
   struct anv_state sf_clip_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, count * 64, 64);
   struct anv_state cc_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, count * 8, 32);

   for (uint32_t i = 0; i < count; i++) {
      const VkViewport *vp = &viewports[i];

      /* The gen7 state struct has just the matrix and guardband fields, the
       * gen8 struct adds the min/max viewport fields. */
      struct GENX(SF_CLIP_VIEWPORT) sf_clip_viewport = {
         .ViewportMatrixElementm00 = vp->width / 2,
         .ViewportMatrixElementm11 = vp->height / 2,
         .ViewportMatrixElementm22 = 1.0,
         .ViewportMatrixElementm30 = vp->x + vp->width / 2,
         .ViewportMatrixElementm31 = vp->y + vp->height / 2,
         .ViewportMatrixElementm32 = 0.0,
         .XMinClipGuardband = -1.0f,
         .XMaxClipGuardband = 1.0f,
         .YMinClipGuardband = -1.0f,
         .YMaxClipGuardband = 1.0f,
         .XMinViewPort = vp->x,
         .XMaxViewPort = vp->x + vp->width - 1,
         .YMinViewPort = vp->y,
         .YMaxViewPort = vp->y + vp->height - 1,
      };

      struct GENX(CC_VIEWPORT) cc_viewport = {
         .MinimumDepth = vp->minDepth,
         .MaximumDepth = vp->maxDepth
      };

      GENX(SF_CLIP_VIEWPORT_pack)(NULL, sf_clip_state.map + i * 64,
                                 &sf_clip_viewport);
      GENX(CC_VIEWPORT_pack)(NULL, cc_state.map + i * 8, &cc_viewport);
   }

   if (!cmd_buffer->device->info.has_llc) {
      anv_state_clflush(sf_clip_state);
      anv_state_clflush(cc_state);
   }

   anv_batch_emit(&cmd_buffer->batch,
                  GENX(3DSTATE_VIEWPORT_STATE_POINTERS_CC),
                  .CCViewportPointer = cc_state.offset);
   anv_batch_emit(&cmd_buffer->batch,
                  GENX(3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP),
                  .SFClipViewportPointer = sf_clip_state.offset);
}

void
gen8_cmd_buffer_emit_viewport(struct anv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->state.dynamic.viewport.count > 0) {
      emit_viewport_state(cmd_buffer, cmd_buffer->state.dynamic.viewport.count,
                          cmd_buffer->state.dynamic.viewport.viewports);
   } else {
      /* If viewport count is 0, this is taken to mean "use the default" */
      emit_viewport_state(cmd_buffer, 1,
                          &(VkViewport) {
                             .x = 0.0f,
                             .y = 0.0f,
                             .width = cmd_buffer->state.framebuffer->width,
                             .height = cmd_buffer->state.framebuffer->height,
                             .minDepth = 0.0f,
                             .maxDepth = 1.0f,
                          });
   }
}
#endif

static void
emit_lri(struct anv_batch *batch, uint32_t reg, uint32_t imm)
{
   anv_batch_emit(batch, GENX(MI_LOAD_REGISTER_IMM),
                  .RegisterOffset = reg,
                  .DataDWord = imm);
}

#define GEN8_L3CNTLREG                  0x7034

static void
config_l3(struct anv_cmd_buffer *cmd_buffer, bool enable_slm)
{
   /* References for GL state:
    *
    * - commits e307cfa..228d5a3
    * - src/mesa/drivers/dri/i965/gen7_l3_state.c
    */

   uint32_t val = enable_slm ?
      /* All = 48 ways; URB = 16 ways; DC and RO = 0, SLM = 1 */
      0x60000021 :
      /* All = 48 ways; URB = 48 ways; DC, RO and SLM = 0 */
      0x60000060;
   bool changed = cmd_buffer->state.current_l3_config != val;

   if (changed) {
      /* According to the hardware docs, the L3 partitioning can only be changed
       * while the pipeline is completely drained and the caches are flushed,
       * which involves a first PIPE_CONTROL flush which stalls the pipeline and
       * initiates invalidation of the relevant caches...
       */
      anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL),
                     .TextureCacheInvalidationEnable = true,
                     .ConstantCacheInvalidationEnable = true,
                     .InstructionCacheInvalidateEnable = true,
                     .DCFlushEnable = true,
                     .PostSyncOperation = NoWrite,
                     .CommandStreamerStallEnable = true);

      /* ...followed by a second stalling flush which guarantees that
       * invalidation is complete when the L3 configuration registers are
       * modified.
       */
      anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL),
                     .DCFlushEnable = true,
                     .PostSyncOperation = NoWrite,
                     .CommandStreamerStallEnable = true);

      emit_lri(&cmd_buffer->batch, GEN8_L3CNTLREG, val);
      cmd_buffer->state.current_l3_config = val;
   }
}

static void
__emit_genx_sf_state(struct anv_cmd_buffer *cmd_buffer)
{
      uint32_t sf_dw[GENX(3DSTATE_SF_length)];
      struct GENX(3DSTATE_SF) sf = {
         GENX(3DSTATE_SF_header),
         .LineWidth = cmd_buffer->state.dynamic.line_width,
      };
      GENX(3DSTATE_SF_pack)(NULL, sf_dw, &sf);
      /* FIXME: gen9.fs */
      anv_batch_emit_merge(&cmd_buffer->batch, sf_dw,
                           cmd_buffer->state.pipeline->gen8.sf);
}

#include "genxml/gen9_pack.h"
static void
__emit_gen9_sf_state(struct anv_cmd_buffer *cmd_buffer)
{
      uint32_t sf_dw[GENX(3DSTATE_SF_length)];
      struct GEN9_3DSTATE_SF sf = {
         GEN9_3DSTATE_SF_header,
         .LineWidth = cmd_buffer->state.dynamic.line_width,
      };
      GEN9_3DSTATE_SF_pack(NULL, sf_dw, &sf);
      /* FIXME: gen9.fs */
      anv_batch_emit_merge(&cmd_buffer->batch, sf_dw,
                           cmd_buffer->state.pipeline->gen8.sf);
}

static void
__emit_sf_state(struct anv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->device->info.is_cherryview)
      __emit_gen9_sf_state(cmd_buffer);
   else
      __emit_genx_sf_state(cmd_buffer);
}

void
genX(cmd_buffer_flush_state)(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;
   uint32_t *p;

   uint32_t vb_emit = cmd_buffer->state.vb_dirty & pipeline->vb_used;

   assert((pipeline->active_stages & VK_SHADER_STAGE_COMPUTE_BIT) == 0);

   config_l3(cmd_buffer, false);

   genX(flush_pipeline_select_3d)(cmd_buffer);

   if (vb_emit) {
      const uint32_t num_buffers = __builtin_popcount(vb_emit);
      const uint32_t num_dwords = 1 + num_buffers * 4;

      p = anv_batch_emitn(&cmd_buffer->batch, num_dwords,
                          GENX(3DSTATE_VERTEX_BUFFERS));
      uint32_t vb, i = 0;
      for_each_bit(vb, vb_emit) {
         struct anv_buffer *buffer = cmd_buffer->state.vertex_bindings[vb].buffer;
         uint32_t offset = cmd_buffer->state.vertex_bindings[vb].offset;

         struct GENX(VERTEX_BUFFER_STATE) state = {
            .VertexBufferIndex = vb,
            .MemoryObjectControlState = GENX(MOCS),
            .AddressModifyEnable = true,
            .BufferPitch = pipeline->binding_stride[vb],
            .BufferStartingAddress = { buffer->bo, buffer->offset + offset },
            .BufferSize = buffer->size - offset
         };

         GENX(VERTEX_BUFFER_STATE_pack)(&cmd_buffer->batch, &p[1 + i * 4], &state);
         i++;
      }
   }

   if (cmd_buffer->state.dirty & ANV_CMD_DIRTY_PIPELINE) {
      /* If somebody compiled a pipeline after starting a command buffer the
       * scratch bo may have grown since we started this cmd buffer (and
       * emitted STATE_BASE_ADDRESS).  If we're binding that pipeline now,
       * reemit STATE_BASE_ADDRESS so that we use the bigger scratch bo. */
      if (cmd_buffer->state.scratch_size < pipeline->total_scratch)
         anv_cmd_buffer_emit_state_base_address(cmd_buffer);

      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);

      /* From the BDW PRM for 3DSTATE_PUSH_CONSTANT_ALLOC_VS:
       *
       *    "The 3DSTATE_CONSTANT_VS must be reprogrammed prior to
       *    the next 3DPRIMITIVE command after programming the
       *    3DSTATE_PUSH_CONSTANT_ALLOC_VS"
       *
       * Since 3DSTATE_PUSH_CONSTANT_ALLOC_VS is programmed as part of
       * pipeline setup, we need to dirty push constants.
       */
      cmd_buffer->state.push_constants_dirty |= VK_SHADER_STAGE_ALL_GRAPHICS;
   }

   /* We emit the binding tables and sampler tables first, then emit push
    * constants and then finally emit binding table and sampler table
    * pointers.  It has to happen in this order, since emitting the binding
    * tables may change the push constants (in case of storage images). After
    * emitting push constants, on SKL+ we have to emit the corresponding
    * 3DSTATE_BINDING_TABLE_POINTER_* for the push constants to take effect.
    */
   uint32_t dirty = 0;
   if (cmd_buffer->state.descriptors_dirty)
      dirty = gen7_cmd_buffer_flush_descriptor_sets(cmd_buffer);

   if (cmd_buffer->state.push_constants_dirty)
      dirty |= cmd_buffer_flush_push_constants(cmd_buffer);

   if (dirty)
      gen7_cmd_buffer_emit_descriptor_pointers(cmd_buffer, dirty);

   if (cmd_buffer->state.dirty & ANV_CMD_DIRTY_DYNAMIC_VIEWPORT)
      gen8_cmd_buffer_emit_viewport(cmd_buffer);

   if (cmd_buffer->state.dirty & ANV_CMD_DIRTY_DYNAMIC_SCISSOR)
      gen7_cmd_buffer_emit_scissor(cmd_buffer);

   if (cmd_buffer->state.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                  ANV_CMD_DIRTY_DYNAMIC_LINE_WIDTH)) {
      __emit_sf_state(cmd_buffer);
   }

   if (cmd_buffer->state.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                  ANV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS)){
      uint32_t raster_dw[GENX(3DSTATE_RASTER_length)];
      struct GENX(3DSTATE_RASTER) raster = {
         GENX(3DSTATE_RASTER_header),
         .GlobalDepthOffsetConstant = cmd_buffer->state.dynamic.depth_bias.bias,
         .GlobalDepthOffsetScale = cmd_buffer->state.dynamic.depth_bias.slope,
         .GlobalDepthOffsetClamp = cmd_buffer->state.dynamic.depth_bias.clamp
      };
      GENX(3DSTATE_RASTER_pack)(NULL, raster_dw, &raster);
      anv_batch_emit_merge(&cmd_buffer->batch, raster_dw,
                           pipeline->gen8.raster);
   }

   /* Stencil reference values moved from COLOR_CALC_STATE in gen8 to
    * 3DSTATE_WM_DEPTH_STENCIL in gen9. That means the dirty bits gets split
    * across different state packets for gen8 and gen9. We handle that by
    * using a big old #if switch here.
    */
#if GEN_GEN == 8
   if (cmd_buffer->state.dirty & (ANV_CMD_DIRTY_DYNAMIC_BLEND_CONSTANTS |
                                  ANV_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE)) {
      struct anv_dynamic_state *d = &cmd_buffer->state.dynamic;
      struct anv_state cc_state =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer,
                                            GENX(COLOR_CALC_STATE_length) * 4,
                                            64);
      struct GENX(COLOR_CALC_STATE) cc = {
         .BlendConstantColorRed = cmd_buffer->state.dynamic.blend_constants[0],
         .BlendConstantColorGreen = cmd_buffer->state.dynamic.blend_constants[1],
         .BlendConstantColorBlue = cmd_buffer->state.dynamic.blend_constants[2],
         .BlendConstantColorAlpha = cmd_buffer->state.dynamic.blend_constants[3],
         .StencilReferenceValue = d->stencil_reference.front & 0xff,
         .BackFaceStencilReferenceValue = d->stencil_reference.back & 0xff,
      };
      GENX(COLOR_CALC_STATE_pack)(NULL, cc_state.map, &cc);

      if (!cmd_buffer->device->info.has_llc)
         anv_state_clflush(cc_state);

      anv_batch_emit(&cmd_buffer->batch,
                     GENX(3DSTATE_CC_STATE_POINTERS),
                     .ColorCalcStatePointer = cc_state.offset,
                     .ColorCalcStatePointerValid = true);
   }

   if (cmd_buffer->state.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                  ANV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK |
                                  ANV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK)) {
      uint32_t wm_depth_stencil_dw[GENX(3DSTATE_WM_DEPTH_STENCIL_length)];
      struct anv_dynamic_state *d = &cmd_buffer->state.dynamic;

      struct GENX(3DSTATE_WM_DEPTH_STENCIL wm_depth_stencil) = {
         GENX(3DSTATE_WM_DEPTH_STENCIL_header),

         .StencilTestMask = d->stencil_compare_mask.front & 0xff,
         .StencilWriteMask = d->stencil_write_mask.front & 0xff,

         .BackfaceStencilTestMask = d->stencil_compare_mask.back & 0xff,
         .BackfaceStencilWriteMask = d->stencil_write_mask.back & 0xff,
      };
      GENX(3DSTATE_WM_DEPTH_STENCIL_pack)(NULL, wm_depth_stencil_dw,
                                          &wm_depth_stencil);

      anv_batch_emit_merge(&cmd_buffer->batch, wm_depth_stencil_dw,
                           pipeline->gen8.wm_depth_stencil);
   }
#else
   if (cmd_buffer->state.dirty & ANV_CMD_DIRTY_DYNAMIC_BLEND_CONSTANTS) {
      struct anv_state cc_state =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer,
                                            GEN9_COLOR_CALC_STATE_length * 4,
                                            64);
      struct GEN9_COLOR_CALC_STATE cc = {
         .BlendConstantColorRed = cmd_buffer->state.dynamic.blend_constants[0],
         .BlendConstantColorGreen = cmd_buffer->state.dynamic.blend_constants[1],
         .BlendConstantColorBlue = cmd_buffer->state.dynamic.blend_constants[2],
         .BlendConstantColorAlpha = cmd_buffer->state.dynamic.blend_constants[3],
      };
      GEN9_COLOR_CALC_STATE_pack(NULL, cc_state.map, &cc);

      if (!cmd_buffer->device->info.has_llc)
         anv_state_clflush(cc_state);

      anv_batch_emit(&cmd_buffer->batch,
                     GEN9_3DSTATE_CC_STATE_POINTERS,
                     .ColorCalcStatePointer = cc_state.offset,
                     .ColorCalcStatePointerValid = true);
   }

   if (cmd_buffer->state.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                  ANV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK |
                                  ANV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK |
                                  ANV_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE)) {
      uint32_t dwords[GEN9_3DSTATE_WM_DEPTH_STENCIL_length];
      struct anv_dynamic_state *d = &cmd_buffer->state.dynamic;
      struct GEN9_3DSTATE_WM_DEPTH_STENCIL wm_depth_stencil = {
         GEN9_3DSTATE_WM_DEPTH_STENCIL_header,

         .StencilTestMask = d->stencil_compare_mask.front & 0xff,
         .StencilWriteMask = d->stencil_write_mask.front & 0xff,

         .BackfaceStencilTestMask = d->stencil_compare_mask.back & 0xff,
         .BackfaceStencilWriteMask = d->stencil_write_mask.back & 0xff,

         .StencilReferenceValue = d->stencil_reference.front & 0xff,
         .BackfaceStencilReferenceValue = d->stencil_reference.back & 0xff,
      };
      GEN9_3DSTATE_WM_DEPTH_STENCIL_pack(NULL, dwords, &wm_depth_stencil);

      anv_batch_emit_merge(&cmd_buffer->batch, dwords,
                           pipeline->gen9.wm_depth_stencil);
   }
#endif

   if (cmd_buffer->state.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                  ANV_CMD_DIRTY_INDEX_BUFFER)) {
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_VF),
         .IndexedDrawCutIndexEnable = pipeline->primitive_restart,
         .CutIndex = cmd_buffer->state.restart_index,
      );
   }

   cmd_buffer->state.vb_dirty &= ~vb_emit;
   cmd_buffer->state.dirty = 0;
}

void genX(CmdBindIndexBuffer)(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    VkIndexType                                 indexType)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);

   static const uint32_t vk_to_gen_index_type[] = {
      [VK_INDEX_TYPE_UINT16]                    = INDEX_WORD,
      [VK_INDEX_TYPE_UINT32]                    = INDEX_DWORD,
   };

   static const uint32_t restart_index_for_type[] = {
      [VK_INDEX_TYPE_UINT16]                    = UINT16_MAX,
      [VK_INDEX_TYPE_UINT32]                    = UINT32_MAX,
   };

   cmd_buffer->state.restart_index = restart_index_for_type[indexType];

   anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_INDEX_BUFFER),
                  .IndexFormat = vk_to_gen_index_type[indexType],
                  .MemoryObjectControlState = GENX(MOCS),
                  .BufferStartingAddress = { buffer->bo, buffer->offset + offset },
                  .BufferSize = buffer->size - offset);

   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_INDEX_BUFFER;
}

static VkResult
flush_compute_descriptor_set(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   struct anv_state surfaces = { 0, }, samplers = { 0, };
   VkResult result;

   result = anv_cmd_buffer_emit_samplers(cmd_buffer,
                                         MESA_SHADER_COMPUTE, &samplers);
   if (result != VK_SUCCESS)
      return result;
   result = anv_cmd_buffer_emit_binding_table(cmd_buffer,
                                              MESA_SHADER_COMPUTE, &surfaces);
   if (result != VK_SUCCESS)
      return result;

   struct anv_state push_state = anv_cmd_buffer_cs_push_constants(cmd_buffer);

   const struct brw_cs_prog_data *cs_prog_data = &pipeline->cs_prog_data;
   const struct brw_stage_prog_data *prog_data = &cs_prog_data->base;

   unsigned local_id_dwords = cs_prog_data->local_invocation_id_regs * 8;
   unsigned push_constant_data_size =
      (prog_data->nr_params + local_id_dwords) * 4;
   unsigned reg_aligned_constant_size = ALIGN(push_constant_data_size, 32);
   unsigned push_constant_regs = reg_aligned_constant_size / 32;

   if (push_state.alloc_size) {
      anv_batch_emit(&cmd_buffer->batch, GENX(MEDIA_CURBE_LOAD),
                     .CURBETotalDataLength = push_state.alloc_size,
                     .CURBEDataStartAddress = push_state.offset);
   }

   assert(prog_data->total_shared <= 64 * 1024);
   uint32_t slm_size = 0;
   if (prog_data->total_shared > 0) {
      /* slm_size is in 4k increments, but must be a power of 2. */
      slm_size = 4 * 1024;
      while (slm_size < prog_data->total_shared)
         slm_size <<= 1;
      slm_size /= 4 * 1024;
   }

   struct anv_state state =
      anv_state_pool_emit(&device->dynamic_state_pool,
                          GENX(INTERFACE_DESCRIPTOR_DATA), 64,
                          .KernelStartPointer = pipeline->cs_simd,
                          .KernelStartPointerHigh = 0,
                          .BindingTablePointer = surfaces.offset,
                          .BindingTableEntryCount = 0,
                          .SamplerStatePointer = samplers.offset,
                          .SamplerCount = 0,
                          .ConstantIndirectURBEntryReadLength = push_constant_regs,
                          .ConstantURBEntryReadOffset = 0,
                          .BarrierEnable = cs_prog_data->uses_barrier,
                          .SharedLocalMemorySize = slm_size,
                          .NumberofThreadsinGPGPUThreadGroup =
                             pipeline->cs_thread_width_max);

   uint32_t size = GENX(INTERFACE_DESCRIPTOR_DATA_length) * sizeof(uint32_t);
   anv_batch_emit(&cmd_buffer->batch, GENX(MEDIA_INTERFACE_DESCRIPTOR_LOAD),
                  .InterfaceDescriptorTotalLength = size,
                  .InterfaceDescriptorDataStartAddress = state.offset);

   return VK_SUCCESS;
}

void
genX(cmd_buffer_flush_compute_state)(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   VkResult result;

   assert(pipeline->active_stages == VK_SHADER_STAGE_COMPUTE_BIT);

   bool needs_slm = pipeline->cs_prog_data.base.total_shared > 0;
   config_l3(cmd_buffer, needs_slm);

   if (cmd_buffer->state.current_pipeline != GPGPU) {
#if GEN_GEN < 10
      /* From the Broadwell PRM, Volume 2a: Instructions, PIPELINE_SELECT:
       *
       *   Software must clear the COLOR_CALC_STATE Valid field in
       *   3DSTATE_CC_STATE_POINTERS command prior to send a PIPELINE_SELECT
       *   with Pipeline Select set to GPGPU.
       *
       * The internal hardware docs recommend the same workaround for Gen9
       * hardware too.
       */
      anv_batch_emit(&cmd_buffer->batch,
                     GENX(3DSTATE_CC_STATE_POINTERS));
#endif

      anv_batch_emit(&cmd_buffer->batch, GENX(PIPELINE_SELECT),
#if GEN_GEN >= 9
                     .MaskBits = 3,
#endif
                     .PipelineSelection = GPGPU);
      cmd_buffer->state.current_pipeline = GPGPU;
   }

   if (cmd_buffer->state.compute_dirty & ANV_CMD_DIRTY_PIPELINE)
      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);

   if ((cmd_buffer->state.descriptors_dirty & VK_SHADER_STAGE_COMPUTE_BIT) ||
       (cmd_buffer->state.compute_dirty & ANV_CMD_DIRTY_PIPELINE)) {
      result = flush_compute_descriptor_set(cmd_buffer);
      assert(result == VK_SUCCESS);
      cmd_buffer->state.descriptors_dirty &= ~VK_SHADER_STAGE_COMPUTE_BIT;
   }

   cmd_buffer->state.compute_dirty = 0;
}

void genX(CmdSetEvent)(
    VkCommandBuffer                             commandBuffer,
    VkEvent                                     _event,
    VkPipelineStageFlags                        stageMask)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_event, event, _event);

   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL),
                  .DestinationAddressType = DAT_PPGTT,
                  .PostSyncOperation = WriteImmediateData,
                  .Address = {
                     &cmd_buffer->device->dynamic_state_block_pool.bo,
                     event->state.offset
                   },
                  .ImmediateData = VK_EVENT_SET);
}

void genX(CmdResetEvent)(
    VkCommandBuffer                             commandBuffer,
    VkEvent                                     _event,
    VkPipelineStageFlags                        stageMask)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_event, event, _event);

   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL),
                  .DestinationAddressType = DAT_PPGTT,
                  .PostSyncOperation = WriteImmediateData,
                  .Address = {
                     &cmd_buffer->device->dynamic_state_block_pool.bo,
                     event->state.offset
                   },
                  .ImmediateData = VK_EVENT_RESET);
}

void genX(CmdWaitEvents)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    eventCount,
    const VkEvent*                              pEvents,
    VkPipelineStageFlags                        srcStageMask,
    VkPipelineStageFlags                        destStageMask,
    uint32_t                                    memoryBarrierCount,
    const VkMemoryBarrier*                      pMemoryBarriers,
    uint32_t                                    bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
    uint32_t                                    imageMemoryBarrierCount,
    const VkImageMemoryBarrier*                 pImageMemoryBarriers)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   for (uint32_t i = 0; i < eventCount; i++) {
      ANV_FROM_HANDLE(anv_event, event, pEvents[i]);

      anv_batch_emit(&cmd_buffer->batch, GENX(MI_SEMAPHORE_WAIT),
                     .WaitMode = PollingMode,
                     .CompareOperation = COMPARE_SAD_EQUAL_SDD,
                     .SemaphoreDataDword = VK_EVENT_SET,
                     .SemaphoreAddress = {
                        &cmd_buffer->device->dynamic_state_block_pool.bo,
                        event->state.offset
                     });
   }

   genX(CmdPipelineBarrier)(commandBuffer, srcStageMask, destStageMask,
                            false, /* byRegion */
                            memoryBarrierCount, pMemoryBarriers,
                            bufferMemoryBarrierCount, pBufferMemoryBarriers,
                            imageMemoryBarrierCount, pImageMemoryBarriers);
}
