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

#if GEN_GEN == 7 && !GEN_IS_HASWELL
void
gen7_cmd_buffer_emit_descriptor_pointers(struct anv_cmd_buffer *cmd_buffer,
                                         uint32_t stages)
{
   static const uint32_t sampler_state_opcodes[] = {
      [MESA_SHADER_VERTEX]                      = 43,
      [MESA_SHADER_TESS_CTRL]                   = 44, /* HS */
      [MESA_SHADER_TESS_EVAL]                   = 45, /* DS */
      [MESA_SHADER_GEOMETRY]                    = 46,
      [MESA_SHADER_FRAGMENT]                    = 47,
      [MESA_SHADER_COMPUTE]                     = 0,
   };

   static const uint32_t binding_table_opcodes[] = {
      [MESA_SHADER_VERTEX]                      = 38,
      [MESA_SHADER_TESS_CTRL]                   = 39,
      [MESA_SHADER_TESS_EVAL]                   = 40,
      [MESA_SHADER_GEOMETRY]                    = 41,
      [MESA_SHADER_FRAGMENT]                    = 42,
      [MESA_SHADER_COMPUTE]                     = 0,
   };

   anv_foreach_stage(s, stages) {
      if (cmd_buffer->state.samplers[s].alloc_size > 0) {
         anv_batch_emit(&cmd_buffer->batch,
                        GENX(3DSTATE_SAMPLER_STATE_POINTERS_VS), ssp) {
            ssp._3DCommandSubOpcode = sampler_state_opcodes[s];
            ssp.PointertoVSSamplerState = cmd_buffer->state.samplers[s].offset;
         }
      }

      /* Always emit binding table pointers if we're asked to, since on SKL
       * this is what flushes push constants. */
      anv_batch_emit(&cmd_buffer->batch,
                     GENX(3DSTATE_BINDING_TABLE_POINTERS_VS), btp) {
         btp._3DCommandSubOpcode = binding_table_opcodes[s];
         btp.PointertoVSBindingTable = cmd_buffer->state.binding_tables[s].offset;
      }
   }
}

uint32_t
gen7_cmd_buffer_flush_descriptor_sets(struct anv_cmd_buffer *cmd_buffer)
{
   VkShaderStageFlags dirty = cmd_buffer->state.descriptors_dirty &
                              cmd_buffer->state.pipeline->active_stages;

   VkResult result = VK_SUCCESS;
   anv_foreach_stage(s, dirty) {
      result = anv_cmd_buffer_emit_samplers(cmd_buffer, s,
                                            &cmd_buffer->state.samplers[s]);
      if (result != VK_SUCCESS)
         break;
      result = anv_cmd_buffer_emit_binding_table(cmd_buffer, s,
                                                 &cmd_buffer->state.binding_tables[s]);
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
      dirty |= cmd_buffer->state.pipeline->active_stages;
      anv_foreach_stage(s, dirty) {
         result = anv_cmd_buffer_emit_samplers(cmd_buffer, s,
                                               &cmd_buffer->state.samplers[s]);
         if (result != VK_SUCCESS)
            return result;
         result = anv_cmd_buffer_emit_binding_table(cmd_buffer, s,
                                                    &cmd_buffer->state.binding_tables[s]);
         if (result != VK_SUCCESS)
            return result;
      }
   }

   cmd_buffer->state.descriptors_dirty &= ~dirty;

   return dirty;
}
#endif /* GEN_GEN == 7 && !GEN_IS_HASWELL */

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

#if GEN_GEN == 7 && !GEN_IS_HASWELL
void
gen7_cmd_buffer_emit_scissor(struct anv_cmd_buffer *cmd_buffer)
{
   uint32_t count = cmd_buffer->state.dynamic.scissor.count;
   const VkRect2D *scissors =  cmd_buffer->state.dynamic.scissor.scissors;
   struct anv_state scissor_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, count * 8, 32);

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
         GEN7_SCISSOR_RECT_pack(NULL, scissor_state.map + i * 8,
                                &empty_scissor);
      } else {
         GEN7_SCISSOR_RECT_pack(NULL, scissor_state.map + i * 8, &scissor);
      }
   }

   anv_batch_emit(&cmd_buffer->batch,
                  GEN7_3DSTATE_SCISSOR_STATE_POINTERS, ssp) {
      ssp.ScissorRectPointer = scissor_state.offset;
   }

   if (!cmd_buffer->device->info.has_llc)
      anv_state_clflush(scissor_state);
}
#endif

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
   if (GEN_IS_HASWELL)
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
                                         MESA_SHADER_COMPUTE, &samplers);
   if (result != VK_SUCCESS)
      return result;
   result = anv_cmd_buffer_emit_binding_table(cmd_buffer,
                                              MESA_SHADER_COMPUTE, &surfaces);
   if (result != VK_SUCCESS)
      return result;

   struct anv_state push_state = anv_cmd_buffer_cs_push_constants(cmd_buffer);

   const struct brw_cs_prog_data *cs_prog_data = get_cs_prog_data(pipeline);
   const struct brw_stage_prog_data *prog_data = &cs_prog_data->base;

   unsigned local_id_dwords = cs_prog_data->local_invocation_id_regs * 8;
   unsigned push_constant_data_size =
      (prog_data->nr_params + local_id_dwords) * 4;
   unsigned reg_aligned_constant_size = ALIGN(push_constant_data_size, 32);
   unsigned push_constant_regs = reg_aligned_constant_size / 32;

   if (push_state.alloc_size) {
      anv_batch_emit(&cmd_buffer->batch, GENX(MEDIA_CURBE_LOAD), curbe) {
         curbe.CURBETotalDataLength    = push_state.alloc_size;
         curbe.CURBEDataStartAddress   = push_state.offset;
      }
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
                          .BindingTablePointer = surfaces.offset,
                          .SamplerStatePointer = samplers.offset,
                          .ConstantURBEntryReadLength =
                             push_constant_regs,
#if !GEN_IS_HASWELL
                          .ConstantURBEntryReadOffset = 0,
#endif
                          .BarrierEnable = cs_prog_data->uses_barrier,
                          .SharedLocalMemorySize = slm_size,
                          .NumberofThreadsinGPGPUThreadGroup =
                             pipeline->cs_thread_width_max);

   const uint32_t size = GENX(INTERFACE_DESCRIPTOR_DATA_length) * sizeof(uint32_t);
   anv_batch_emit(&cmd_buffer->batch,
                  GENX(MEDIA_INTERFACE_DESCRIPTOR_LOAD), idl) {
      idl.InterfaceDescriptorTotalLength        = size;
      idl.InterfaceDescriptorDataStartAddress = state.offset;
   }

   return VK_SUCCESS;
}

void
genX(cmd_buffer_config_l3)(struct anv_cmd_buffer *cmd_buffer, bool enable_slm)
{
   /* References for GL state:
    *
    * - commits e307cfa..228d5a3
    * - src/mesa/drivers/dri/i965/gen7_l3_state.c
    */

   uint32_t l3cr2_slm, l3cr2_noslm;
   anv_pack_struct(&l3cr2_noslm, GENX(L3CNTLREG2),
                   .URBAllocation = 24,
                   .ROAllocation = 0,
                   .DCAllocation = 16);
   anv_pack_struct(&l3cr2_slm, GENX(L3CNTLREG2),
                   .SLMEnable = 1,
                   .URBAllocation = 16,
                   .URBLowBandwidth = 1,
                   .ROAllocation = 0,
                   .DCAllocation = 8);
   const uint32_t l3cr2_val = enable_slm ? l3cr2_slm : l3cr2_noslm;
   bool changed = cmd_buffer->state.current_l3_config != l3cr2_val;

   if (changed) {
      /* According to the hardware docs, the L3 partitioning can only be
       * changed while the pipeline is completely drained and the caches are
       * flushed, which involves a first PIPE_CONTROL flush which stalls the
       * pipeline...
       */
      anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
         pc.DCFlushEnable              = true;
         pc.CommandStreamerStallEnable = true;
         pc.PostSyncOperation          = NoWrite;
      }

      /* ...followed by a second pipelined PIPE_CONTROL that initiates
       * invalidation of the relevant caches. Note that because RO
       * invalidation happens at the top of the pipeline (i.e. right away as
       * the PIPE_CONTROL command is processed by the CS) we cannot combine it
       * with the previous stalling flush as the hardware documentation
       * suggests, because that would cause the CS to stall on previous
       * rendering *after* RO invalidation and wouldn't prevent the RO caches
       * from being polluted by concurrent rendering before the stall
       * completes. This intentionally doesn't implement the SKL+ hardware
       * workaround suggesting to enable CS stall on PIPE_CONTROLs with the
       * texture cache invalidation bit set for GPGPU workloads because the
       * previous and subsequent PIPE_CONTROLs already guarantee that there is
       * no concurrent GPGPU kernel execution (see SKL HSD 2132585).
       */
      anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
         pc.TextureCacheInvalidationEnable   = true;
         pc.ConstantCacheInvalidationEnable  = true;
         pc.InstructionCacheInvalidateEnable = true;
         pc.StateCacheInvalidationEnable     = true;
         pc.PostSyncOperation                = NoWrite;
      }

      /* Now send a third stalling flush to make sure that invalidation is
       * complete when the L3 configuration registers are modified.
       */
      anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
         pc.DCFlushEnable              = true;
         pc.CommandStreamerStallEnable = true;
         pc.PostSyncOperation          = NoWrite;
      }

      anv_finishme("write GEN7_L3SQCREG1");
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_LOAD_REGISTER_IMM), lri) {
         lri.RegisterOffset   = GENX(L3CNTLREG2_num);
         lri.DataDWord        = l3cr2_val;
      }

      uint32_t l3cr3_slm, l3cr3_noslm;
      anv_pack_struct(&l3cr3_noslm, GENX(L3CNTLREG3),
                      .ISAllocation = 8,
                      .CAllocation = 4,
                      .TAllocation = 8);
      anv_pack_struct(&l3cr3_slm, GENX(L3CNTLREG3),
                      .ISAllocation = 8,
                      .CAllocation = 8,
                      .TAllocation = 8);
      const uint32_t l3cr3_val = enable_slm ? l3cr3_slm : l3cr3_noslm;
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_LOAD_REGISTER_IMM), lri) {
         lri.RegisterOffset   = GENX(L3CNTLREG3_num);
         lri.DataDWord        = l3cr3_val;
      }

      cmd_buffer->state.current_l3_config = l3cr2_val;
   }
}

void
genX(cmd_buffer_flush_compute_state)(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   const struct brw_cs_prog_data *cs_prog_data = get_cs_prog_data(pipeline);
   MAYBE_UNUSED VkResult result;

   assert(pipeline->active_stages == VK_SHADER_STAGE_COMPUTE_BIT);

   bool needs_slm = cs_prog_data->base.total_shared > 0;
   genX(cmd_buffer_config_l3)(cmd_buffer, needs_slm);

   genX(flush_pipeline_select_gpgpu)(cmd_buffer);

   if (cmd_buffer->state.compute_dirty & ANV_CMD_DIRTY_PIPELINE)
      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);

   if ((cmd_buffer->state.descriptors_dirty & VK_SHADER_STAGE_COMPUTE_BIT) ||
       (cmd_buffer->state.compute_dirty & ANV_CMD_DIRTY_PIPELINE)) {
      /* FIXME: figure out descriptors for gen7 */
      result = flush_compute_descriptor_set(cmd_buffer);
      assert(result == VK_SUCCESS);
      cmd_buffer->state.descriptors_dirty &= ~VK_SHADER_STAGE_COMPUTE_BIT;
   }

   cmd_buffer->state.compute_dirty = 0;
}

void
genX(cmd_buffer_flush_dynamic_state)(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.pipeline;

   if (cmd_buffer->state.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                  ANV_CMD_DIRTY_RENDER_TARGETS |
                                  ANV_CMD_DIRTY_DYNAMIC_LINE_WIDTH |
                                  ANV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS)) {

      const struct anv_image_view *iview =
         anv_cmd_buffer_get_depth_stencil_view(cmd_buffer);
      const struct anv_image *image = iview ? iview->image : NULL;
      const struct anv_format *anv_format =
         iview ? anv_format_for_vk_format(iview->vk_format) : NULL;
      const bool has_depth = iview && anv_format->has_depth;
      const uint32_t depth_format = has_depth ?
         isl_surf_get_depth_format(&cmd_buffer->device->isl_dev,
                                   &image->depth_surface.isl) : D16_UNORM;

      uint32_t sf_dw[GENX(3DSTATE_SF_length)];
      struct GENX(3DSTATE_SF) sf = {
         GENX(3DSTATE_SF_header),
         .DepthBufferSurfaceFormat = depth_format,
         .LineWidth = cmd_buffer->state.dynamic.line_width,
         .GlobalDepthOffsetConstant = cmd_buffer->state.dynamic.depth_bias.bias,
         .GlobalDepthOffsetScale = cmd_buffer->state.dynamic.depth_bias.slope,
         .GlobalDepthOffsetClamp = cmd_buffer->state.dynamic.depth_bias.clamp
      };
      GENX(3DSTATE_SF_pack)(NULL, sf_dw, &sf);

      anv_batch_emit_merge(&cmd_buffer->batch, sf_dw, pipeline->gen7.sf);
   }

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

      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_CC_STATE_POINTERS), ccp) {
         ccp.ColorCalcStatePointer = cc_state.offset;
      }
   }

   if (cmd_buffer->state.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                  ANV_CMD_DIRTY_RENDER_TARGETS |
                                  ANV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK |
                                  ANV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK)) {
      uint32_t depth_stencil_dw[GENX(DEPTH_STENCIL_STATE_length)];
      struct anv_dynamic_state *d = &cmd_buffer->state.dynamic;

      struct GENX(DEPTH_STENCIL_STATE) depth_stencil = {
         .StencilTestMask = d->stencil_compare_mask.front & 0xff,
         .StencilWriteMask = d->stencil_write_mask.front & 0xff,

         .BackfaceStencilTestMask = d->stencil_compare_mask.back & 0xff,
         .BackfaceStencilWriteMask = d->stencil_write_mask.back & 0xff,
      };
      GENX(DEPTH_STENCIL_STATE_pack)(NULL, depth_stencil_dw, &depth_stencil);

      struct anv_state ds_state =
         anv_cmd_buffer_merge_dynamic(cmd_buffer, depth_stencil_dw,
                                      pipeline->gen7.depth_stencil_state,
                                      GENX(DEPTH_STENCIL_STATE_length), 64);

      anv_batch_emit(&cmd_buffer->batch,
                     GENX(3DSTATE_DEPTH_STENCIL_STATE_POINTERS), dsp) {
         dsp.PointertoDEPTH_STENCIL_STATE = ds_state.offset;
      }
   }

   if (cmd_buffer->state.gen7.index_buffer &&
       cmd_buffer->state.dirty & (ANV_CMD_DIRTY_PIPELINE |
                                  ANV_CMD_DIRTY_INDEX_BUFFER)) {
      struct anv_buffer *buffer = cmd_buffer->state.gen7.index_buffer;
      uint32_t offset = cmd_buffer->state.gen7.index_offset;

#if GEN_IS_HASWELL
      anv_batch_emit(&cmd_buffer->batch, GEN75_3DSTATE_VF, vf) {
         vf.IndexedDrawCutIndexEnable  = pipeline->primitive_restart;
         vf.CutIndex                   = cmd_buffer->state.restart_index;
      }
#endif

      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_INDEX_BUFFER), ib) {
#if !GEN_IS_HASWELL
         ib.CutIndexEnable             = pipeline->primitive_restart;
#endif
         ib.IndexFormat                = cmd_buffer->state.gen7.index_type;
         ib.MemoryObjectControlState   = GENX(MOCS);

         ib.BufferStartingAddress =
            (struct anv_address) { buffer->bo, buffer->offset + offset };
         ib.BufferEndingAddress =
            (struct anv_address) { buffer->bo, buffer->offset + buffer->size };
      }
   }

   cmd_buffer->state.dirty = 0;
}

void genX(CmdSetEvent)(
    VkCommandBuffer                             commandBuffer,
    VkEvent                                     event,
    VkPipelineStageFlags                        stageMask)
{
   stub();
}

void genX(CmdResetEvent)(
    VkCommandBuffer                             commandBuffer,
    VkEvent                                     event,
    VkPipelineStageFlags                        stageMask)
{
   stub();
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
   stub();
}
