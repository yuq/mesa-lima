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
#include "brw_defines.h"
#include "brw_state.h"

#include "blorp_priv.h"
#include "vbo/vbo.h"
#include "brw_draw.h"

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
static void
gen6_blorp_emit_urb_config(struct brw_context *brw,
                           const struct brw_blorp_params *params)
{
   BEGIN_BATCH(3);
   OUT_BATCH(_3DSTATE_URB << 16 | (3 - 2));
   OUT_BATCH(brw->urb.max_vs_entries << GEN6_URB_VS_ENTRIES_SHIFT);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


/* 3DSTATE_CC_STATE_POINTERS
 *
 * The pointer offsets are relative to
 * CMD_STATE_BASE_ADDRESS.DynamicStateBaseAddress.
 *
 * The HiZ op doesn't use BLEND_STATE or COLOR_CALC_STATE.
 */
static void
gen6_blorp_emit_cc_state_pointers(struct brw_context *brw,
                                  const struct brw_blorp_params *params,
                                  uint32_t cc_blend_state_offset,
                                  uint32_t depthstencil_offset,
                                  uint32_t cc_state_offset)
{
   BEGIN_BATCH(4);
   OUT_BATCH(_3DSTATE_CC_STATE_POINTERS << 16 | (4 - 2));
   OUT_BATCH(cc_blend_state_offset | 1); /* BLEND_STATE offset */
   OUT_BATCH(depthstencil_offset | 1); /* DEPTH_STENCIL_STATE offset */
   OUT_BATCH(cc_state_offset | 1); /* COLOR_CALC_STATE offset */
   ADVANCE_BATCH();
}


/**
 * 3DSTATE_SAMPLER_STATE_POINTERS.  See upload_sampler_state_pointers().
 */
static void
gen6_blorp_emit_sampler_state_pointers(struct brw_context *brw,
                                       uint32_t sampler_offset)
{
   BEGIN_BATCH(4);
   OUT_BATCH(_3DSTATE_SAMPLER_STATE_POINTERS << 16 |
             VS_SAMPLER_STATE_CHANGE |
             GS_SAMPLER_STATE_CHANGE |
             PS_SAMPLER_STATE_CHANGE |
             (4 - 2));
   OUT_BATCH(0); /* VS */
   OUT_BATCH(0); /* GS */
   OUT_BATCH(sampler_offset);
   ADVANCE_BATCH();
}


/* 3DSTATE_VS
 *
 * Disable vertex shader.
 */
static void
gen6_blorp_emit_vs_disable(struct brw_context *brw,
                           const struct brw_blorp_params *params)
{
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

   BEGIN_BATCH(6);
   OUT_BATCH(_3DSTATE_VS << 16 | (6 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


/* 3DSTATE_GS
 *
 * Disable the geometry shader.
 */
static void
gen6_blorp_emit_gs_disable(struct brw_context *brw,
                           const struct brw_blorp_params *params)
{
   BEGIN_BATCH(7);
   OUT_BATCH(_3DSTATE_GS << 16 | (7 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
   brw->gs.enabled = false;
}


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
static void
gen6_blorp_emit_sf_config(struct brw_context *brw,
                          const struct brw_blorp_params *params)
{
   const unsigned num_varyings =
      params->wm_prog_data ? params->wm_prog_data->num_varying_inputs : 0;
   const unsigned urb_read_length =
      brw_blorp_get_urb_length(params->wm_prog_data);

   BEGIN_BATCH(20);
   OUT_BATCH(_3DSTATE_SF << 16 | (20 - 2));
   OUT_BATCH(num_varyings << GEN6_SF_NUM_OUTPUTS_SHIFT |
             urb_read_length << GEN6_SF_URB_ENTRY_READ_LENGTH_SHIFT |
             BRW_SF_URB_ENTRY_READ_OFFSET <<
                GEN6_SF_URB_ENTRY_READ_OFFSET_SHIFT);
   OUT_BATCH(0); /* dw2 */
   OUT_BATCH(params->dst.surf.samples > 1 ? GEN6_SF_MSRAST_ON_PATTERN : 0);
   for (int i = 0; i < 13; ++i)
      OUT_BATCH(0);
   OUT_BATCH(params->wm_prog_data ? params->wm_prog_data->flat_inputs : 0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


/**
 * Enable or disable thread dispatch and set the HiZ op appropriately.
 */
static void
gen6_blorp_emit_wm_config(struct brw_context *brw,
                          const struct brw_blorp_params *params)
{
   const struct brw_blorp_prog_data *prog_data = params->wm_prog_data;
   uint32_t dw2, dw4, dw5, dw6, ksp0, ksp2;

   /* Even when thread dispatch is disabled, max threads (dw5.25:31) must be
    * nonzero to prevent the GPU from hanging.  While the documentation doesn't
    * mention this explicitly, it notes that the valid range for the field is
    * [1,39] = [2,40] threads, which excludes zero.
    *
    * To be safe (and to minimize extraneous code) we go ahead and fully
    * configure the WM state whether or not there is a WM program.
    */

   dw2 = dw4 = dw5 = dw6 = ksp0 = ksp2 = 0;
   switch (params->hiz_op) {
   case GEN6_HIZ_OP_DEPTH_CLEAR:
      dw4 |= GEN6_WM_DEPTH_CLEAR;
      break;
   case GEN6_HIZ_OP_DEPTH_RESOLVE:
      dw4 |= GEN6_WM_DEPTH_RESOLVE;
      break;
   case GEN6_HIZ_OP_HIZ_RESOLVE:
      dw4 |= GEN6_WM_HIERARCHICAL_DEPTH_RESOLVE;
      break;
   case GEN6_HIZ_OP_NONE:
      break;
   default:
      unreachable("not reached");
   }
   dw5 |= GEN6_WM_LINE_AA_WIDTH_1_0;
   dw5 |= GEN6_WM_LINE_END_CAP_AA_WIDTH_0_5;
   dw5 |= (brw->max_wm_threads - 1) << GEN6_WM_MAX_THREADS_SHIFT;
   dw6 |= 0 << GEN6_WM_BARYCENTRIC_INTERPOLATION_MODE_SHIFT; /* No interp */
   dw6 |= (params->wm_prog_data ? prog_data->num_varying_inputs : 0) <<
          GEN6_WM_NUM_SF_OUTPUTS_SHIFT;

   if (params->wm_prog_data) {
      dw5 |= GEN6_WM_DISPATCH_ENABLE; /* We are rendering */

      dw4 |= prog_data->first_curbe_grf_0 << GEN6_WM_DISPATCH_START_GRF_SHIFT_0;
      dw4 |= prog_data->first_curbe_grf_2 << GEN6_WM_DISPATCH_START_GRF_SHIFT_2;

      ksp0 = params->wm_prog_kernel;
      ksp2 = params->wm_prog_kernel + params->wm_prog_data->ksp_offset_2;

      if (params->wm_prog_data->dispatch_8)
         dw5 |= GEN6_WM_8_DISPATCH_ENABLE;
      if (params->wm_prog_data->dispatch_16)
         dw5 |= GEN6_WM_16_DISPATCH_ENABLE;
   }

   if (params->src.bo) {
      dw5 |= GEN6_WM_KILL_ENABLE; /* TODO: temporarily smash on */
      dw2 |= 1 << GEN6_WM_SAMPLER_COUNT_SHIFT; /* Up to 4 samplers */
   }

   if (params->dst.surf.samples > 1) {
      dw6 |= GEN6_WM_MSRAST_ON_PATTERN;
      if (prog_data && prog_data->persample_msaa_dispatch)
         dw6 |= GEN6_WM_MSDISPMODE_PERSAMPLE;
      else
         dw6 |= GEN6_WM_MSDISPMODE_PERPIXEL;
   } else {
      dw6 |= GEN6_WM_MSRAST_OFF_PIXEL;
      dw6 |= GEN6_WM_MSDISPMODE_PERSAMPLE;
   }

   BEGIN_BATCH(9);
   OUT_BATCH(_3DSTATE_WM << 16 | (9 - 2));
   OUT_BATCH(ksp0);
   OUT_BATCH(dw2);
   OUT_BATCH(0); /* No scratch needed */
   OUT_BATCH(dw4);
   OUT_BATCH(dw5);
   OUT_BATCH(dw6);
   OUT_BATCH(0); /* kernel 1 pointer */
   OUT_BATCH(ksp2);
   ADVANCE_BATCH();
}

static void
gen6_blorp_emit_constant_disable(struct brw_context *brw, unsigned opcode)
{
   /* Disable the push constant buffers. */
   BEGIN_BATCH(5);
   OUT_BATCH(opcode << 16 | (5 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}

/**
 * 3DSTATE_BINDING_TABLE_POINTERS
 */
static void
gen6_blorp_emit_binding_table_pointers(struct brw_context *brw,
                                       uint32_t wm_bind_bo_offset)
{
   BEGIN_BATCH(4);
   OUT_BATCH(_3DSTATE_BINDING_TABLE_POINTERS << 16 |
             GEN6_BINDING_TABLE_MODIFY_PS |
             (4 - 2));
   OUT_BATCH(0); /* vs -- ignored */
   OUT_BATCH(0); /* gs -- ignored */
   OUT_BATCH(wm_bind_bo_offset); /* wm/ps */
   ADVANCE_BATCH();
}


static void
gen6_blorp_emit_depth_stencil_config(struct brw_context *brw,
                                     const struct brw_blorp_params *params)
{
   uint32_t surftype;

   switch (params->depth.surf.dim) {
   case ISL_SURF_DIM_1D:
      surftype = BRW_SURFACE_1D;
      break;
   case ISL_SURF_DIM_2D:
      surftype = BRW_SURFACE_2D;
      break;
   case ISL_SURF_DIM_3D:
      surftype = BRW_SURFACE_3D;
      break;
   }

   /* 3DSTATE_DEPTH_BUFFER */
   {
      brw_emit_depth_stall_flushes(brw);

      unsigned depth = MAX2(params->depth.surf.logical_level0_px.depth,
                            params->depth.surf.logical_level0_px.array_len);

      BEGIN_BATCH(7);
      /* 3DSTATE_DEPTH_BUFFER dw0 */
      OUT_BATCH(_3DSTATE_DEPTH_BUFFER << 16 | (7 - 2));

      /* 3DSTATE_DEPTH_BUFFER dw1 */
      OUT_BATCH((params->depth.surf.row_pitch - 1) |
                params->depth_format << 18 |
                1 << 21 | /* separate stencil enable */
                1 << 22 | /* hiz enable */
                BRW_TILEWALK_YMAJOR << 26 |
                1 << 27 | /* y-tiled */
                surftype << 29);

      /* 3DSTATE_DEPTH_BUFFER dw2 */
      OUT_RELOC(params->depth.bo,
                I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
                params->depth.offset);

      /* 3DSTATE_DEPTH_BUFFER dw3 */
      OUT_BATCH(BRW_SURFACE_MIPMAPLAYOUT_BELOW << 1 |
                (params->depth.surf.logical_level0_px.width - 1) << 6 |
                (params->depth.surf.logical_level0_px.height - 1) << 19 |
                params->depth.view.base_level << 2);

      /* 3DSTATE_DEPTH_BUFFER dw4 */
      OUT_BATCH((depth - 1) << 21 |
                params->depth.view.base_array_layer << 10 |
                (depth - 1) << 1);

      /* 3DSTATE_DEPTH_BUFFER dw5 */
      OUT_BATCH(0);

      /* 3DSTATE_DEPTH_BUFFER dw6 */
      OUT_BATCH(0);
      ADVANCE_BATCH();
   }

   /* 3DSTATE_HIER_DEPTH_BUFFER */
   {
      BEGIN_BATCH(3);
      OUT_BATCH((_3DSTATE_HIER_DEPTH_BUFFER << 16) | (3 - 2));
      OUT_BATCH(params->depth.aux_surf.row_pitch - 1);
      OUT_RELOC(params->depth.aux_bo,
                I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
                params->depth.aux_offset);
      ADVANCE_BATCH();
   }

   /* 3DSTATE_STENCIL_BUFFER */
   {
      BEGIN_BATCH(3);
      OUT_BATCH((_3DSTATE_STENCIL_BUFFER << 16) | (3 - 2));
      OUT_BATCH(0);
      OUT_BATCH(0);
      ADVANCE_BATCH();
   }
}


static void
gen6_blorp_emit_depth_disable(struct brw_context *brw,
                              const struct brw_blorp_params *params)
{
   brw_emit_depth_stall_flushes(brw);

   BEGIN_BATCH(7);
   OUT_BATCH(_3DSTATE_DEPTH_BUFFER << 16 | (7 - 2));
   OUT_BATCH((BRW_DEPTHFORMAT_D32_FLOAT << 18) |
             (BRW_SURFACE_NULL << 29));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();

   BEGIN_BATCH(3);
   OUT_BATCH(_3DSTATE_HIER_DEPTH_BUFFER << 16 | (3 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();

   BEGIN_BATCH(3);
   OUT_BATCH(_3DSTATE_STENCIL_BUFFER << 16 | (3 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


/* 3DSTATE_CLEAR_PARAMS
 *
 * From the Sandybridge PRM, Volume 2, Part 1, Section 3DSTATE_CLEAR_PARAMS:
 *   [DevSNB] 3DSTATE_CLEAR_PARAMS packet must follow the DEPTH_BUFFER_STATE
 *   packet when HiZ is enabled and the DEPTH_BUFFER_STATE changes.
 */
static void
gen6_blorp_emit_clear_params(struct brw_context *brw,
                             const struct brw_blorp_params *params)
{
   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_CLEAR_PARAMS << 16 |
	     GEN5_DEPTH_CLEAR_VALID |
	     (2 - 2));
   OUT_BATCH(params->depth.clear_color.u32[0]);
   ADVANCE_BATCH();
}

/* 3DSTATE_VIEWPORT_STATE_POINTERS */
static void
gen6_blorp_emit_viewport_state(struct brw_context *brw,
			       const struct brw_blorp_params *params)
{
   struct brw_cc_viewport *ccv;
   uint32_t cc_vp_offset;

   ccv = (struct brw_cc_viewport *)brw_state_batch(brw, AUB_TRACE_CC_VP_STATE,
						   sizeof(*ccv), 32,
						   &cc_vp_offset);

   ccv->min_depth = 0.0;
   ccv->max_depth = 1.0;

   BEGIN_BATCH(4);
   OUT_BATCH(_3DSTATE_VIEWPORT_STATE_POINTERS << 16 | (4 - 2) |
	     GEN6_CC_VIEWPORT_MODIFY);
   OUT_BATCH(0); /* clip VP */
   OUT_BATCH(0); /* SF VP */
   OUT_BATCH(cc_vp_offset);
   ADVANCE_BATCH();
}


/* 3DPRIMITIVE */
static void
gen6_blorp_emit_primitive(struct brw_context *brw,
                          const struct brw_blorp_params *params)
{
   BEGIN_BATCH(6);
   OUT_BATCH(CMD_3D_PRIM << 16 | (6 - 2) |
             _3DPRIM_RECTLIST << GEN4_3DPRIM_TOPOLOGY_TYPE_SHIFT |
             GEN4_3DPRIM_VERTEXBUFFER_ACCESS_SEQUENTIAL);
   OUT_BATCH(3); /* vertex count per instance */
   OUT_BATCH(0);
   OUT_BATCH(params->num_layers); /* instance count */
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
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
gen6_blorp_exec(struct brw_context *brw,
                const struct brw_blorp_params *params)
{
   uint32_t cc_blend_state_offset = 0;
   uint32_t cc_state_offset = 0;
   uint32_t depthstencil_offset;
   uint32_t wm_bind_bo_offset = 0;

   /* Emit workaround flushes when we switch from drawing to blorping. */
   brw_emit_post_sync_nonzero_flush(brw);

   brw_upload_state_base_address(brw);

   gen6_emit_3dstate_multisample(brw, params->dst.surf.samples);
   gen6_emit_3dstate_sample_mask(brw,
                                 params->dst.surf.samples > 1 ?
                                 (1 << params->dst.surf.samples) - 1 : 1);
   gen6_blorp_emit_vertices(brw, params);
   gen6_blorp_emit_urb_config(brw, params);
   if (params->wm_prog_data) {
      cc_blend_state_offset = gen6_blorp_emit_blend_state(brw, params);
      cc_state_offset = gen6_blorp_emit_cc_state(brw);
   }
   depthstencil_offset = gen6_blorp_emit_depth_stencil_state(brw, params);
   gen6_blorp_emit_cc_state_pointers(brw, params, cc_blend_state_offset,
                                     depthstencil_offset, cc_state_offset);

   gen6_blorp_emit_constant_disable(brw, _3DSTATE_CONSTANT_VS);
   gen6_blorp_emit_constant_disable(brw, _3DSTATE_CONSTANT_GS);
   gen6_blorp_emit_constant_disable(brw, _3DSTATE_CONSTANT_PS);

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
      gen6_blorp_emit_binding_table_pointers(brw, wm_bind_bo_offset);
   }

   if (params->src.bo) {
      const uint32_t sampler_offset =
         gen6_blorp_emit_sampler_state(brw, BRW_MAPFILTER_LINEAR, 0, true);
      gen6_blorp_emit_sampler_state_pointers(brw, sampler_offset);
   }

   gen6_blorp_emit_vs_disable(brw, params);
   gen6_blorp_emit_gs_disable(brw, params);
   gen6_blorp_emit_clip_disable(brw);
   gen6_blorp_emit_sf_config(brw, params);
   gen6_blorp_emit_wm_config(brw, params);
   gen6_blorp_emit_viewport_state(brw, params);

   if (params->depth.bo)
      gen6_blorp_emit_depth_stencil_config(brw, params);
   else
      gen6_blorp_emit_depth_disable(brw, params);
   gen6_blorp_emit_clear_params(brw, params);
   gen6_blorp_emit_drawing_rectangle(brw, params);
   gen6_blorp_emit_primitive(brw, params);
}
