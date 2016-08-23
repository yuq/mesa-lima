/*
 * Copyright © 2016 Intel Corporation
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

#include "anv_private.h"
#include "genX_multisample.h"

/* These are defined in anv_private.h and blorp_genX_exec.h */
#undef __gen_address_type
#undef __gen_user_data
#undef __gen_combine_address

#include "common/gen_l3_config.h"
#include "blorp/blorp_genX_exec.h"

static void *
blorp_emit_dwords(struct blorp_batch *batch, unsigned n)
{
   struct anv_cmd_buffer *cmd_buffer = batch->driver_batch;
   return anv_batch_emit_dwords(&cmd_buffer->batch, n);
}

static uint64_t
blorp_emit_reloc(struct blorp_batch *batch,
                 void *location, struct blorp_address address, uint32_t delta)
{
   struct anv_cmd_buffer *cmd_buffer = batch->driver_batch;
   assert(cmd_buffer->batch.start <= location &&
          location < cmd_buffer->batch.end);
   return anv_batch_emit_reloc(&cmd_buffer->batch, location,
                               address.buffer, address.offset + delta);
}

static void
blorp_surface_reloc(struct blorp_batch *batch, uint32_t ss_offset,
                    struct blorp_address address, uint32_t delta)
{
   struct anv_cmd_buffer *cmd_buffer = batch->driver_batch;
   anv_reloc_list_add(&cmd_buffer->surface_relocs, &cmd_buffer->pool->alloc,
                      ss_offset, address.buffer, address.offset + delta);
}

static void *
blorp_alloc_dynamic_state(struct blorp_batch *batch,
                          enum aub_state_struct_type type,
                          uint32_t size,
                          uint32_t alignment,
                          uint32_t *offset)
{
   struct anv_cmd_buffer *cmd_buffer = batch->driver_batch;

   struct anv_state state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, size, alignment);

   *offset = state.offset;
   return state.map;
}

static void
blorp_alloc_binding_table(struct blorp_batch *batch, unsigned num_entries,
                          unsigned state_size, unsigned state_alignment,
                          uint32_t *bt_offset,
                          uint32_t *surface_offsets, void **surface_maps)
{
   struct anv_cmd_buffer *cmd_buffer = batch->driver_batch;

   uint32_t state_offset;
   struct anv_state bt_state =
      anv_cmd_buffer_alloc_binding_table(cmd_buffer, num_entries,
                                         &state_offset);
   if (bt_state.map == NULL) {
      /* We ran out of space.  Grab a new binding table block. */
      VkResult result = anv_cmd_buffer_new_binding_table_block(cmd_buffer);
      assert(result == VK_SUCCESS);

      /* Re-emit state base addresses so we get the new surface state base
       * address before we start emitting binding tables etc.
       */
      anv_cmd_buffer_emit_state_base_address(cmd_buffer);

      bt_state = anv_cmd_buffer_alloc_binding_table(cmd_buffer, num_entries,
                                                    &state_offset);
      assert(bt_state.map != NULL);
   }

   uint32_t *bt_map = bt_state.map;
   *bt_offset = bt_state.offset;

   for (unsigned i = 0; i < num_entries; i++) {
      struct anv_state surface_state =
         anv_cmd_buffer_alloc_surface_state(cmd_buffer);
      bt_map[i] = surface_state.offset + state_offset;
      surface_offsets[i] = surface_state.offset;
      surface_maps[i] = surface_state.map;
   }
}

static void *
blorp_alloc_vertex_buffer(struct blorp_batch *batch, uint32_t size,
                          struct blorp_address *addr)
{
   struct anv_cmd_buffer *cmd_buffer = batch->driver_batch;
   struct anv_state vb_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, size, 16);

   *addr = (struct blorp_address) {
      .buffer = &cmd_buffer->device->dynamic_state_block_pool.bo,
      .offset = vb_state.offset,
   };

   return vb_state.map;
}

static void
blorp_emit_urb_config(struct blorp_batch *batch, unsigned vs_entry_size)
{
   struct anv_device *device = batch->blorp->driver_ctx;
   struct anv_cmd_buffer *cmd_buffer = batch->driver_batch;

   genX(emit_urb_setup)(device, &cmd_buffer->batch,
                        VK_SHADER_STAGE_VERTEX_BIT |
                        VK_SHADER_STAGE_FRAGMENT_BIT,
                        vs_entry_size, 0,
                        cmd_buffer->state.current_l3_config);
}

static void
blorp_emit_3dstate_multisample(struct blorp_batch *batch, unsigned samples)
{
   blorp_emit(batch, GENX(3DSTATE_MULTISAMPLE), ms) {
      ms.NumberofMultisamples       = __builtin_ffs(samples) - 1;

#if GEN_GEN >= 8
      /* The PRM says that this bit is valid only for DX9:
       *
       *    SW can choose to set this bit only for DX9 API. DX10/OGL API's
       *    should not have any effect by setting or not setting this bit.
       */
      ms.PixelPositionOffsetEnable  = false;
      ms.PixelLocation              = CENTER;
#else
      ms.PixelLocation              = PIXLOC_CENTER;

      switch (samples) {
      case 1:
         SAMPLE_POS_1X(ms.Sample);
         break;
      case 2:
         SAMPLE_POS_2X(ms.Sample);
         break;
      case 4:
         SAMPLE_POS_4X(ms.Sample);
         break;
      case 8:
         SAMPLE_POS_8X(ms.Sample);
         break;
      default:
         break;
      }
#endif
   }
}

void genX(blorp_exec)(struct blorp_batch *batch,
                      const struct blorp_params *params);

void
genX(blorp_exec)(struct blorp_batch *batch,
                 const struct blorp_params *params)
{
   struct anv_cmd_buffer *cmd_buffer = batch->driver_batch;

   if (!cmd_buffer->state.current_l3_config) {
      const struct gen_l3_config *cfg =
         gen_get_default_l3_config(&cmd_buffer->device->info);
      genX(cmd_buffer_config_l3)(cmd_buffer, cfg);
   }

   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);

   if (cmd_buffer->state.current_pipeline != _3D) {
#if GEN_GEN <= 7
      /* From "BXML » GT » MI » vol1a GPU Overview » [Instruction]
       * PIPELINE_SELECT [DevBWR+]":
       *
       *   Project: DEVSNB+
       *
       *   Software must ensure all the write caches are flushed through a
       *   stalling PIPE_CONTROL command followed by another PIPE_CONTROL
       *   command to invalidate read only caches prior to programming
       *   MI_PIPELINE_SELECT command to change the Pipeline Select Mode.
       */
      blorp_emit(batch, GENX(PIPE_CONTROL), pc) {
         pc.RenderTargetCacheFlushEnable  = true;
         pc.DepthCacheFlushEnable         = true;
         pc.DCFlushEnable                 = true;
         pc.PostSyncOperation             = NoWrite;
         pc.CommandStreamerStallEnable    = true;
      }

      blorp_emit(batch, GENX(PIPE_CONTROL), pc) {
         pc.TextureCacheInvalidationEnable   = true;
         pc.ConstantCacheInvalidationEnable  = true;
         pc.StateCacheInvalidationEnable     = true;
         pc.InstructionCacheInvalidateEnable = true;
         pc.PostSyncOperation                = NoWrite;
      }
#endif

      blorp_emit(batch, GENX(PIPELINE_SELECT), ps) {
#if GEN_GEN >= 9
         ps.MaskBits = 3;
#endif
         ps.PipelineSelection = _3D;
      }

      cmd_buffer->state.current_pipeline = _3D;
   }

   blorp_exec(batch, params);

   /* BLORP sets DRAWING_RECTANGLE but we always want it set to the maximum.
    * Since we set it once at driver init and never again, we have to set it
    * back after invoking blorp.
    *
    * TODO: BLORP should assume a max drawing rectangle
    */
   blorp_emit(batch, GENX(3DSTATE_DRAWING_RECTANGLE), rect) {
      rect.ClippedDrawingRectangleYMin = 0;
      rect.ClippedDrawingRectangleXMin = 0;
      rect.ClippedDrawingRectangleYMax = UINT16_MAX;
      rect.ClippedDrawingRectangleXMax = UINT16_MAX;
      rect.DrawingRectangleOriginY = 0;
      rect.DrawingRectangleOriginX = 0;
   }

   cmd_buffer->state.vb_dirty = ~0;
   cmd_buffer->state.dirty = ~0;
   cmd_buffer->state.push_constants_dirty = ~0;
}
