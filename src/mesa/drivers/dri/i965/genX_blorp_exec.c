/*
 * Copyright © 2011 Intel Corporation
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

#include "intel_batchbuffer.h"
#include "intel_mipmap_tree.h"

#include "brw_context.h"
#include "brw_state.h"

#include "blorp_priv.h"

#include "genxml/gen_macros.h"

static void *
blorp_emit_dwords(struct brw_context *brw, unsigned n)
{
   intel_batchbuffer_begin(brw, n, RENDER_RING);
   uint32_t *map = brw->batch.map_next;
   brw->batch.map_next += n;
   intel_batchbuffer_advance(brw);
   return map;
}

struct blorp_address {
   drm_intel_bo *buffer;
   uint32_t read_domains;
   uint32_t write_domain;
   uint32_t offset;
};

static uint64_t
blorp_emit_reloc(struct brw_context *brw, void *location,
                 struct blorp_address address, uint32_t delta)
{
   uint32_t offset = (char *)location - (char *)brw->batch.map;
   if (brw->gen >= 8) {
      return intel_batchbuffer_reloc64(brw, address.buffer, offset,
                                       address.read_domains,
                                       address.write_domain,
                                       address.offset + delta);
   } else {
      return intel_batchbuffer_reloc(brw, address.buffer, offset,
                                     address.read_domains,
                                     address.write_domain,
                                     address.offset + delta);
   }
}

#define __gen_address_type struct blorp_address
#define __gen_user_data struct brw_context

static uint64_t
__gen_combine_address(struct brw_context *brw, void *location,
                      struct blorp_address address, uint32_t delta)
{
   if (address.buffer == NULL) {
      return address.offset + delta;
   } else {
      return blorp_emit_reloc(brw, location, address, delta);
   }
}

#include "genxml/genX_pack.h"

#define _blorp_cmd_length(cmd) cmd ## _length
#define _blorp_cmd_header(cmd) cmd ## _header
#define _blorp_cmd_pack(cmd) cmd ## _pack

#define blorp_emit(brw, cmd, name)                                \
   for (struct cmd name = { _blorp_cmd_header(cmd) },             \
        *_dst = blorp_emit_dwords(brw, _blorp_cmd_length(cmd));   \
        __builtin_expect(_dst != NULL, 1);                        \
        _blorp_cmd_pack(cmd)(brw, (void *)_dst, &name),           \
        _dst = NULL)

static void
blorp_emit_sf_config(struct brw_context *brw,
                     const struct brw_blorp_params *params)
{
   const struct brw_blorp_prog_data *prog_data = params->wm_prog_data;

   /* 3DSTATE_SF
    *
    * Disable ViewportTransformEnable (dw2.1)
    *
    * From the SandyBridge PRM, Volume 2, Part 1, Section 1.3, "3D
    * Primitives Overview":
    *     RECTLIST: Viewport Mapping must be DISABLED (as is typical with the
    *     use of screen- space coordinates).
    *
    * A solid rectangle must be rendered, so set FrontFaceFillMode (dw2.4:3)
    * and BackFaceFillMode (dw2.5:6) to SOLID(0).
    *
    * From the Sandy Bridge PRM, Volume 2, Part 1, Section
    * 6.4.1.1 3DSTATE_SF, Field FrontFaceFillMode:
    *     SOLID: Any triangle or rectangle object found to be front-facing
    *     is rendered as a solid object. This setting is required when
    *     (rendering rectangle (RECTLIST) objects.
    */
   blorp_emit(brw, GENX(3DSTATE_SF), sf) {
      sf.FrontFaceFillMode = FILL_MODE_SOLID;
      sf.BackFaceFillMode = FILL_MODE_SOLID;

      sf.MultisampleRasterizationMode = params->dst.surf.samples > 1 ?
         MSRASTMODE_ON_PATTERN : MSRASTMODE_OFF_PIXEL;

      sf.VertexURBEntryReadOffset = BRW_SF_URB_ENTRY_READ_OFFSET;
      if (prog_data) {
         sf.NumberofSFOutputAttributes = prog_data->num_varying_inputs;
         sf.VertexURBEntryReadLength = brw_blorp_get_urb_length(prog_data);
         sf.ConstantInterpolationEnable = prog_data->flat_inputs;
      } else {
         sf.NumberofSFOutputAttributes = 0;
         sf.VertexURBEntryReadLength = 1;
      }
   }
}

static void
blorp_emit_wm_config(struct brw_context *brw,
                     const struct brw_blorp_params *params)
{
   const struct brw_blorp_prog_data *prog_data = params->wm_prog_data;

   /* Even when thread dispatch is disabled, max threads (dw5.25:31) must be
    * nonzero to prevent the GPU from hanging.  While the documentation doesn't
    * mention this explicitly, it notes that the valid range for the field is
    * [1,39] = [2,40] threads, which excludes zero.
    *
    * To be safe (and to minimize extraneous code) we go ahead and fully
    * configure the WM state whether or not there is a WM program.
    */
   blorp_emit(brw, GENX(3DSTATE_WM), wm) {
      wm.MaximumNumberofThreads = brw->max_wm_threads - 1;

      switch (params->hiz_op) {
      case GEN6_HIZ_OP_DEPTH_CLEAR:
         wm.DepthBufferClear = true;
         break;
      case GEN6_HIZ_OP_DEPTH_RESOLVE:
         wm.DepthBufferResolveEnable = true;
         break;
      case GEN6_HIZ_OP_HIZ_RESOLVE:
         wm.HierarchicalDepthBufferResolveEnable = true;
         break;
      case GEN6_HIZ_OP_NONE:
         break;
      default:
         unreachable("not reached");
      }

      if (prog_data) {
         wm.ThreadDispatchEnable = true;

         wm.DispatchGRFStartRegisterforConstantSetupData0 =
            prog_data->first_curbe_grf_0;
         wm.DispatchGRFStartRegisterforConstantSetupData2 =
            prog_data->first_curbe_grf_2;

         wm.KernelStartPointer0 = params->wm_prog_kernel;
         wm.KernelStartPointer2 =
            params->wm_prog_kernel + prog_data->ksp_offset_2;

         wm._8PixelDispatchEnable = prog_data->dispatch_8;
         wm._16PixelDispatchEnable = prog_data->dispatch_16;

         wm.NumberofSFOutputAttributes = prog_data->num_varying_inputs;
      }

      if (params->src.bo) {
         wm.SamplerCount = 1; /* Up to 4 samplers */
         wm.PixelShaderKillPixel = true; /* TODO: temporarily smash on */
      }

      if (params->dst.surf.samples > 1) {
         wm.MultisampleRasterizationMode = MSRASTMODE_ON_PATTERN;
         wm.MultisampleDispatchMode =
            (prog_data && prog_data->persample_msaa_dispatch) ?
            MSDISPMODE_PERSAMPLE : MSDISPMODE_PERPIXEL;
      } else {
         wm.MultisampleRasterizationMode = MSRASTMODE_OFF_PIXEL;
         wm.MultisampleDispatchMode = MSDISPMODE_PERSAMPLE;
      }
   }
}


static void
blorp_emit_depth_stencil_config(struct brw_context *brw,
                                const struct brw_blorp_params *params)
{
   brw_emit_depth_stall_flushes(brw);

   blorp_emit(brw, GENX(3DSTATE_DEPTH_BUFFER), db) {
      switch (params->depth.surf.dim) {
      case ISL_SURF_DIM_1D:
         db.SurfaceType = SURFTYPE_1D;
         break;
      case ISL_SURF_DIM_2D:
         db.SurfaceType = SURFTYPE_2D;
         break;
      case ISL_SURF_DIM_3D:
         db.SurfaceType = SURFTYPE_3D;
         break;
      }

      db.SurfaceFormat = params->depth_format;

      db.TiledSurface = true;
      db.TileWalk = TILEWALK_YMAJOR;
      db.MIPMapLayoutMode = MIPLAYOUT_BELOW;

      db.HierarchicalDepthBufferEnable = true;
      db.SeparateStencilBufferEnable = true;

      db.Width = params->depth.surf.logical_level0_px.width - 1;
      db.Height = params->depth.surf.logical_level0_px.height - 1;
      db.RenderTargetViewExtent = db.Depth =
         MAX2(params->depth.surf.logical_level0_px.depth,
              params->depth.surf.logical_level0_px.array_len) - 1;

      db.LOD = params->depth.view.base_level;
      db.MinimumArrayElement = params->depth.view.base_array_layer;

      db.SurfacePitch = params->depth.surf.row_pitch - 1;
      db.SurfaceBaseAddress = (struct blorp_address) {
         .buffer = params->depth.bo,
         .read_domains = I915_GEM_DOMAIN_RENDER,
         .write_domain = I915_GEM_DOMAIN_RENDER,
         .offset = params->depth.offset,
      };
   }

   blorp_emit(brw, GENX(3DSTATE_HIER_DEPTH_BUFFER), hiz) {
      hiz.SurfacePitch = params->depth.aux_surf.row_pitch - 1;
      hiz.SurfaceBaseAddress = (struct blorp_address) {
         .buffer = params->depth.aux_bo,
         .read_domains = I915_GEM_DOMAIN_RENDER,
         .write_domain = I915_GEM_DOMAIN_RENDER,
         .offset = params->depth.aux_offset,
      };
   }

   blorp_emit(brw, GENX(3DSTATE_STENCIL_BUFFER), sb);
}

static uint32_t
blorp_emit_blend_state(struct brw_context *brw,
                       const struct brw_blorp_params *params)
{
   struct GENX(BLEND_STATE) blend;
   memset(&blend, 0, sizeof(blend));

   for (unsigned i = 0; i < params->num_draw_buffers; ++i) {
      blend.Entry[i].PreBlendColorClampEnable = true;
      blend.Entry[i].PostBlendColorClampEnable = true;
      blend.Entry[i].ColorClampRange = COLORCLAMP_RTFORMAT;

      blend.Entry[i].WriteDisableRed = params->color_write_disable[0];
      blend.Entry[i].WriteDisableGreen = params->color_write_disable[1];
      blend.Entry[i].WriteDisableBlue = params->color_write_disable[2];
      blend.Entry[i].WriteDisableAlpha = params->color_write_disable[3];
   }

   uint32_t offset;
   void *state = brw_state_batch(brw, AUB_TRACE_BLEND_STATE,
                                 GENX(BLEND_STATE_length) * 4, 64, &offset);
   GENX(BLEND_STATE_pack)(NULL, state, &blend);

   return offset;
}

static uint32_t
blorp_emit_color_calc_state(struct brw_context *brw,
                            const struct brw_blorp_params *params)
{
   uint32_t offset;
   void *state = brw_state_batch(brw, AUB_TRACE_CC_STATE,
                                 GENX(COLOR_CALC_STATE_length) * 4, 64, &offset);
   memset(state, 0, GENX(COLOR_CALC_STATE_length) * 4);

   return offset;
}

static uint32_t
blorp_emit_depth_stencil_state(struct brw_context *brw,
                               const struct brw_blorp_params *params)
{
   /* See the following sections of the Sandy Bridge PRM, Volume 1, Part2:
    *   - 7.5.3.1 Depth Buffer Clear
    *   - 7.5.3.2 Depth Buffer Resolve
    *   - 7.5.3.3 Hierarchical Depth Buffer Resolve
    */
   struct GENX(DEPTH_STENCIL_STATE) ds = {
      .DepthBufferWriteEnable = true,
   };

   if (params->hiz_op == GEN6_HIZ_OP_DEPTH_RESOLVE) {
      ds.DepthTestEnable = true;
      ds.DepthTestFunction = COMPAREFUNCTION_NEVER;
   }

   uint32_t offset;
   void *state = brw_state_batch(brw, AUB_TRACE_DEPTH_STENCIL_STATE,
                                 GENX(DEPTH_STENCIL_STATE_length) * 4, 64,
                                 &offset);
   GENX(DEPTH_STENCIL_STATE_pack)(NULL, state, &ds);

   return offset;
}

/* 3DSTATE_VIEWPORT_STATE_POINTERS */
static void
blorp_emit_viewport_state(struct brw_context *brw,
                          const struct brw_blorp_params *params)
{
   uint32_t cc_vp_offset;

   void *state = brw_state_batch(brw, AUB_TRACE_CC_VP_STATE,
                                 GENX(CC_VIEWPORT_length) * 4, 32,
                                 &cc_vp_offset);

   GENX(CC_VIEWPORT_pack)(brw, state,
      &(struct GENX(CC_VIEWPORT)) {
         .MinimumDepth = 0.0,
         .MaximumDepth = 1.0,
      });

   blorp_emit(brw, GENX(3DSTATE_VIEWPORT_STATE_POINTERS), vsp) {
      vsp.CCViewportStateChange = true;
      vsp.PointertoCC_VIEWPORT = cc_vp_offset;
   }
}


/**
 * \brief Execute a blit or render pass operation.
 *
 * To execute the operation, this function manually constructs and emits a
 * batch to draw a rectangle primitive. The batchbuffer is flushed before
 * constructing and after emitting the batch.
 *
 * This function alters no GL state.
 */
void
genX(blorp_exec)(struct brw_context *brw,
                 const struct brw_blorp_params *params)
{
   uint32_t blend_state_offset = 0;
   uint32_t color_calc_state_offset = 0;
   uint32_t depth_stencil_state_offset;
   uint32_t wm_bind_bo_offset = 0;

   /* Emit workaround flushes when we switch from drawing to blorping. */
   brw_emit_post_sync_nonzero_flush(brw);

   brw_upload_state_base_address(brw);

   gen6_blorp_emit_vertices(brw, params);

   /* 3DSTATE_URB
    *
    * Assign the entire URB to the VS. Even though the VS disabled, URB space
    * is still needed because the clipper loads the VUE's from the URB. From
    * the Sandybridge PRM, Volume 2, Part 1, Section 3DSTATE,
    * Dword 1.15:0 "VS Number of URB Entries":
    *     This field is always used (even if VS Function Enable is DISABLED).
    *
    * The warning below appears in the PRM (Section 3DSTATE_URB), but we can
    * safely ignore it because this batch contains only one draw call.
    *     Because of URB corruption caused by allocating a previous GS unit
    *     URB entry to the VS unit, software is required to send a “GS NULL
    *     Fence” (Send URB fence with VS URB size == 1 and GS URB size == 0)
    *     plus a dummy DRAW call before any case where VS will be taking over
    *     GS URB space.
    */
   blorp_emit(brw, GENX(3DSTATE_URB), urb) {
      urb.VSNumberofURBEntries = brw->urb.max_vs_entries;
   }

   if (params->wm_prog_data) {
      blend_state_offset = blorp_emit_blend_state(brw, params);
      color_calc_state_offset = blorp_emit_color_calc_state(brw, params);
   }
   depth_stencil_state_offset = blorp_emit_depth_stencil_state(brw, params);

   /* 3DSTATE_CC_STATE_POINTERS
    *
    * The pointer offsets are relative to
    * CMD_STATE_BASE_ADDRESS.DynamicStateBaseAddress.
    *
    * The HiZ op doesn't use BLEND_STATE or COLOR_CALC_STATE.
    */
   blorp_emit(brw, GENX(3DSTATE_CC_STATE_POINTERS), cc) {
      cc.BLEND_STATEChange = true;
      cc.COLOR_CALC_STATEChange = true;
      cc.DEPTH_STENCIL_STATEChange = true;
      cc.PointertoBLEND_STATE = blend_state_offset;
      cc.PointertoCOLOR_CALC_STATE = color_calc_state_offset;
      cc.PointertoDEPTH_STENCIL_STATE = depth_stencil_state_offset;
   }

   blorp_emit(brw, GENX(3DSTATE_CONSTANT_VS), vs);
   blorp_emit(brw, GENX(3DSTATE_CONSTANT_GS), gs);
   blorp_emit(brw, GENX(3DSTATE_CONSTANT_PS), ps);

   if (params->wm_prog_data) {
      uint32_t wm_surf_offset_renderbuffer;
      uint32_t wm_surf_offset_texture = 0;

      wm_surf_offset_renderbuffer =
         brw_blorp_emit_surface_state(brw, &params->dst,
                                      I915_GEM_DOMAIN_RENDER,
                                      I915_GEM_DOMAIN_RENDER, true);
      if (params->src.bo) {
         wm_surf_offset_texture =
            brw_blorp_emit_surface_state(brw, &params->src,
                                         I915_GEM_DOMAIN_SAMPLER, 0, false);
      }
      wm_bind_bo_offset =
         gen6_blorp_emit_binding_table(brw,
                                       wm_surf_offset_renderbuffer,
                                       wm_surf_offset_texture);

      blorp_emit(brw, GENX(3DSTATE_BINDING_TABLE_POINTERS), bt) {
         bt.PSBindingTableChange = true;
         bt.PointertoPSBindingTable = wm_bind_bo_offset;
      }
   }

   if (params->src.bo) {
      const uint32_t sampler_offset =
         gen6_blorp_emit_sampler_state(brw, MAPFILTER_LINEAR, 0, true);

      blorp_emit(brw, GENX(3DSTATE_SAMPLER_STATE_POINTERS), ssp) {
         ssp.VSSamplerStateChange = true;
         ssp.GSSamplerStateChange = true;
         ssp.PSSamplerStateChange = true;
         ssp.PointertoPSSamplerState = sampler_offset;
      }
   }

   gen6_emit_3dstate_multisample(brw, params->dst.surf.samples);

   blorp_emit(brw, GENX(3DSTATE_SAMPLE_MASK), mask) {
      mask.SampleMask = (1 << params->dst.surf.samples) - 1;
   }

   /* From the BSpec, 3D Pipeline > Geometry > Vertex Shader > State,
    * 3DSTATE_VS, Dword 5.0 "VS Function Enable":
    *
    *   [DevSNB] A pipeline flush must be programmed prior to a
    *   3DSTATE_VS command that causes the VS Function Enable to
    *   toggle. Pipeline flush can be executed by sending a PIPE_CONTROL
    *   command with CS stall bit set and a post sync operation.
    *
    * We've already done one at the start of the BLORP operation.
    */
   blorp_emit(brw, GENX(3DSTATE_VS), vs);
   blorp_emit(brw, GENX(3DSTATE_GS), gs);

   blorp_emit(brw, GENX(3DSTATE_CLIP), clip) {
      clip.PerspectiveDivideDisable = true;
   }

   blorp_emit_sf_config(brw, params);
   blorp_emit_wm_config(brw, params);

   blorp_emit_viewport_state(brw, params);

   if (params->depth.bo) {
      blorp_emit_depth_stencil_config(brw, params);
   } else {
      brw_emit_depth_stall_flushes(brw);

      blorp_emit(brw, GENX(3DSTATE_DEPTH_BUFFER), db) {
         db.SurfaceType = SURFTYPE_NULL;
         db.SurfaceFormat = D32_FLOAT;
      }
      blorp_emit(brw, GENX(3DSTATE_HIER_DEPTH_BUFFER), hiz);
      blorp_emit(brw, GENX(3DSTATE_STENCIL_BUFFER), sb);
   }

   /* 3DSTATE_CLEAR_PARAMS
    *
    * From the Sandybridge PRM, Volume 2, Part 1, Section 3DSTATE_CLEAR_PARAMS:
    *   [DevSNB] 3DSTATE_CLEAR_PARAMS packet must follow the DEPTH_BUFFER_STATE
    *   packet when HiZ is enabled and the DEPTH_BUFFER_STATE changes.
    */
   blorp_emit(brw, GENX(3DSTATE_CLEAR_PARAMS), clear) {
      clear.DepthClearValueValid = true;
      clear.DepthClearValue = params->depth.clear_color.u32[0];
   }

   blorp_emit(brw, GENX(3DSTATE_DRAWING_RECTANGLE), rect) {
      rect.ClippedDrawingRectangleXMax = MAX2(params->x1, params->x0) - 1;
      rect.ClippedDrawingRectangleYMax = MAX2(params->y1, params->y0) - 1;
   }

   blorp_emit(brw, GENX(3DPRIMITIVE), prim) {
      prim.VertexAccessType = SEQUENTIAL;
      prim.PrimitiveTopologyType = _3DPRIM_RECTLIST;
      prim.VertexCountPerInstance = 3;
      prim.InstanceCount = params->num_layers;
   }
}
