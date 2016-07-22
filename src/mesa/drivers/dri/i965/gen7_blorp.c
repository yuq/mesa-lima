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

#include "brw_blorp.h"

/* Once vertex fetcher has written full VUE entries with complete
 * header the space requirement is as follows per vertex (in bytes):
 *
 *     Header    Position    Program constants
 *   +--------+------------+-------------------+
 *   |   16   |     16     |      n x 16       |
 *   +--------+------------+-------------------+
 *
 * where 'n' stands for number of varying inputs expressed as vec4s.
 *
 * The URB size is in turn expressed in 64 bytes (512 bits).
 */
static unsigned
gen7_blorp_get_vs_entry_size(const struct brw_blorp_params *params)
{
    const unsigned num_varyings =
       params->wm_prog_data ? params->wm_prog_data->num_varying_inputs : 0;
    const unsigned total_needed = 16 + 16 + num_varyings * 16;

   return DIV_ROUND_UP(total_needed, 64);
}

/* 3DSTATE_URB_VS
 * 3DSTATE_URB_HS
 * 3DSTATE_URB_DS
 * 3DSTATE_URB_GS
 *
 * If the 3DSTATE_URB_VS is emitted, than the others must be also.
 * From the Ivybridge PRM, Volume 2 Part 1, section 1.7.1 3DSTATE_URB_VS:
 *
 *     3DSTATE_URB_HS, 3DSTATE_URB_DS, and 3DSTATE_URB_GS must also be
 *     programmed in order for the programming of this state to be
 *     valid.
 */
void
gen7_blorp_emit_urb_config(struct brw_context *brw,
                           const struct brw_blorp_params *params)
{
   const unsigned vs_entry_size = gen7_blorp_get_vs_entry_size(params);

   if (!(brw->ctx.NewDriverState & (BRW_NEW_CONTEXT | BRW_NEW_URB_SIZE)) &&
       brw->urb.vsize >= vs_entry_size)
      return;

   brw->ctx.NewDriverState |= BRW_NEW_URB_SIZE;

   gen7_upload_urb(brw, vs_entry_size, false, false);
}


/* 3DSTATE_BLEND_STATE_POINTERS */
void
gen7_blorp_emit_blend_state_pointer(struct brw_context *brw,
                                    uint32_t cc_blend_state_offset)
{
   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_BLEND_STATE_POINTERS << 16 | (2 - 2));
   OUT_BATCH(cc_blend_state_offset | 1);
   ADVANCE_BATCH();
}


/* 3DSTATE_CC_STATE_POINTERS */
void
gen7_blorp_emit_cc_state_pointer(struct brw_context *brw,
                                 uint32_t cc_state_offset)
{
   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_CC_STATE_POINTERS << 16 | (2 - 2));
   OUT_BATCH(cc_state_offset | 1);
   ADVANCE_BATCH();
}

void
gen7_blorp_emit_cc_viewport(struct brw_context *brw)
{
   struct brw_cc_viewport *ccv;
   uint32_t cc_vp_offset;

   ccv = (struct brw_cc_viewport *)brw_state_batch(brw, AUB_TRACE_CC_VP_STATE,
						   sizeof(*ccv), 32,
						   &cc_vp_offset);
   ccv->min_depth = 0.0;
   ccv->max_depth = 1.0;

   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_VIEWPORT_STATE_POINTERS_CC << 16 | (2 - 2));
   OUT_BATCH(cc_vp_offset);
   ADVANCE_BATCH();
}


/* 3DSTATE_DEPTH_STENCIL_STATE_POINTERS
 *
 * The offset is relative to CMD_STATE_BASE_ADDRESS.DynamicStateBaseAddress.
 */
static void
gen7_blorp_emit_depth_stencil_state_pointers(struct brw_context *brw,
                                             uint32_t depthstencil_offset)
{
   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_DEPTH_STENCIL_STATE_POINTERS << 16 | (2 - 2));
   OUT_BATCH(depthstencil_offset | 1);
   ADVANCE_BATCH();
}


/* Hardware seems to try to fetch the constants even though the corresponding
 * stage gets disabled. Therefore make sure the settings for the constant
 * buffer are valid.
 */
static void
gen7_blorp_disable_constant_state(struct brw_context *brw,
                                       unsigned opcode)
{
   BEGIN_BATCH(7);
   OUT_BATCH(opcode << 16 | (7 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}

/* 3DSTATE_VS
 *
 * Disable vertex shader.
 */
static void
gen7_blorp_emit_vs_disable(struct brw_context *brw)
{
   BEGIN_BATCH(6);
   OUT_BATCH(_3DSTATE_VS << 16 | (6 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


/* 3DSTATE_HS
 *
 * Disable the hull shader.
 */
static void
gen7_blorp_emit_hs_disable(struct brw_context *brw)
{
   BEGIN_BATCH(7);
   OUT_BATCH(_3DSTATE_HS << 16 | (7 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


/* 3DSTATE_TE
 *
 * Disable the tesselation engine.
 */
void
gen7_blorp_emit_te_disable(struct brw_context *brw)
{
   BEGIN_BATCH(4);
   OUT_BATCH(_3DSTATE_TE << 16 | (4 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


/* 3DSTATE_DS
 *
 * Disable the domain shader.
 */
static void
gen7_blorp_emit_ds_disable(struct brw_context *brw)
{
   BEGIN_BATCH(6);
   OUT_BATCH(_3DSTATE_DS << 16 | (6 - 2));
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
gen7_blorp_emit_gs_disable(struct brw_context *brw)
{
   /**
    * From Graphics BSpec: 3D-Media-GPGPU Engine > 3D Pipeline Stages >
    * Geometry > Geometry Shader > State:
    *
    *     "Note: Because of corruption in IVB:GT2, software needs to flush the
    *     whole fixed function pipeline when the GS enable changes value in
    *     the 3DSTATE_GS."
    *
    * The hardware architects have clarified that in this context "flush the
    * whole fixed function pipeline" means to emit a PIPE_CONTROL with the "CS
    * Stall" bit set.
    */
   if (brw->gen < 8 && !brw->is_haswell && brw->gt == 2 && brw->gs.enabled)
      gen7_emit_cs_stall_flush(brw);

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

/* 3DSTATE_STREAMOUT
 *
 * Disable streamout.
 */
static void
gen7_blorp_emit_streamout_disable(struct brw_context *brw)
{
   BEGIN_BATCH(3);
   OUT_BATCH(_3DSTATE_STREAMOUT << 16 | (3 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


static void
gen7_blorp_emit_sf_config(struct brw_context *brw,
                          const struct brw_blorp_params *params)
{
   /* 3DSTATE_SF
    *
    * Disable ViewportTransformEnable (dw1.1)
    *
    * From the SandyBridge PRM, Volume 2, Part 1, Section 1.3, "3D
    * Primitives Overview":
    *     RECTLIST: Viewport Mapping must be DISABLED (as is typical with the
    *     use of screen- space coordinates).
    *
    * A solid rectangle must be rendered, so set FrontFaceFillMode (dw1.6:5)
    * and BackFaceFillMode (dw1.4:3) to SOLID(0).
    *
    * From the Sandy Bridge PRM, Volume 2, Part 1, Section
    * 6.4.1.1 3DSTATE_SF, Field FrontFaceFillMode:
    *     SOLID: Any triangle or rectangle object found to be front-facing
    *     is rendered as a solid object. This setting is required when
    *     (rendering rectangle (RECTLIST) objects.
    */
   {
      BEGIN_BATCH(7);
      OUT_BATCH(_3DSTATE_SF << 16 | (7 - 2));
      OUT_BATCH(params->depth_format <<
                GEN7_SF_DEPTH_BUFFER_SURFACE_FORMAT_SHIFT);
      OUT_BATCH(params->dst.surf.samples > 1 ? GEN6_SF_MSRAST_ON_PATTERN : 0);
      OUT_BATCH(0);
      OUT_BATCH(0);
      OUT_BATCH(0);
      OUT_BATCH(0);
      ADVANCE_BATCH();
   }

   /* 3DSTATE_SBE */
   {
      const unsigned num_varyings =
         params->wm_prog_data ? params->wm_prog_data->num_varying_inputs : 0;
      const unsigned urb_read_length =
         brw_blorp_get_urb_length(params->wm_prog_data);

      BEGIN_BATCH(14);
      OUT_BATCH(_3DSTATE_SBE << 16 | (14 - 2));

      /* There is no need for swizzling (GEN7_SBE_SWIZZLE_ENABLE). All the
       * vertex data coming from vertex fetcher is taken as unmodified
       * (i.e., passed through). Vertex shader state is disabled and vertex
       * fetcher builds complete vertex entries including VUE header.
       * This is for unknown reason really needed to be disabled when more
       * than one vec4 worth of vertex attributes are needed.
       */
      OUT_BATCH(num_varyings << GEN7_SBE_NUM_OUTPUTS_SHIFT |
                urb_read_length << GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT |
                BRW_SF_URB_ENTRY_READ_OFFSET <<
                   GEN7_SBE_URB_ENTRY_READ_OFFSET_SHIFT);
      for (int i = 0; i < 9; ++i)
         OUT_BATCH(0);
      OUT_BATCH(params->wm_prog_data ? params->wm_prog_data->flat_inputs : 0);
      OUT_BATCH(0);
      OUT_BATCH(0);
      ADVANCE_BATCH();
   }
}


/**
 * Disable thread dispatch (dw5.19) and enable the HiZ op.
 */
static void
gen7_blorp_emit_wm_config(struct brw_context *brw,
                          const struct brw_blorp_params *params)
{
   const struct brw_blorp_prog_data *prog_data = params->wm_prog_data;
   uint32_t dw1 = 0, dw2 = 0;

   switch (params->hiz_op) {
   case GEN6_HIZ_OP_DEPTH_CLEAR:
      dw1 |= GEN7_WM_DEPTH_CLEAR;
      break;
   case GEN6_HIZ_OP_DEPTH_RESOLVE:
      dw1 |= GEN7_WM_DEPTH_RESOLVE;
      break;
   case GEN6_HIZ_OP_HIZ_RESOLVE:
      dw1 |= GEN7_WM_HIERARCHICAL_DEPTH_RESOLVE;
      break;
   case GEN6_HIZ_OP_NONE:
      break;
   default:
      unreachable("not reached");
   }
   dw1 |= GEN7_WM_LINE_AA_WIDTH_1_0;
   dw1 |= GEN7_WM_LINE_END_CAP_AA_WIDTH_0_5;
   dw1 |= 0 << GEN7_WM_BARYCENTRIC_INTERPOLATION_MODE_SHIFT; /* No interp */

   if (params->wm_prog_data)
      dw1 |= GEN7_WM_DISPATCH_ENABLE; /* We are rendering */

   if (params->src.bo)
      dw1 |= GEN7_WM_KILL_ENABLE; /* TODO: temporarily smash on */

   if (params->dst.surf.samples > 1) {
      dw1 |= GEN7_WM_MSRAST_ON_PATTERN;
      if (prog_data && prog_data->persample_msaa_dispatch)
         dw2 |= GEN7_WM_MSDISPMODE_PERSAMPLE;
      else
         dw2 |= GEN7_WM_MSDISPMODE_PERPIXEL;
   } else {
      dw1 |= GEN7_WM_MSRAST_OFF_PIXEL;
      dw2 |= GEN7_WM_MSDISPMODE_PERSAMPLE;
   }

   BEGIN_BATCH(3);
   OUT_BATCH(_3DSTATE_WM << 16 | (3 - 2));
   OUT_BATCH(dw1);
   OUT_BATCH(dw2);
   ADVANCE_BATCH();
}


/**
 * 3DSTATE_PS
 *
 * Pixel shader dispatch is disabled above in 3DSTATE_WM, dw1.29. Despite
 * that, thread dispatch info must still be specified.
 *     - Maximum Number of Threads (dw4.24:31) must be nonzero, as the
 *       valid range for this field is [0x3, 0x2f].
 *     - A dispatch mode must be given; that is, at least one of the
 *       "N Pixel Dispatch Enable" (N=8,16,32) fields must be set. This was
 *       discovered through simulator error messages.
 */
static void
gen7_blorp_emit_ps_config(struct brw_context *brw,
                          const struct brw_blorp_params *params)
{
   const struct brw_blorp_prog_data *prog_data = params->wm_prog_data;
   uint32_t dw2, dw4, dw5, ksp0, ksp2;
   const int max_threads_shift = brw->is_haswell ?
      HSW_PS_MAX_THREADS_SHIFT : IVB_PS_MAX_THREADS_SHIFT;

   dw2 = dw4 = dw5 = ksp0 = ksp2 = 0;
   dw4 |= (brw->max_wm_threads - 1) << max_threads_shift;

   if (brw->is_haswell)
      dw4 |= SET_FIELD(1, HSW_PS_SAMPLE_MASK); /* 1 sample for now */
   if (params->wm_prog_data) {
      dw5 |= prog_data->first_curbe_grf_0 << GEN7_PS_DISPATCH_START_GRF_SHIFT_0;
      dw5 |= prog_data->first_curbe_grf_2 << GEN7_PS_DISPATCH_START_GRF_SHIFT_2;

      ksp0 = params->wm_prog_kernel;
      ksp2 = params->wm_prog_kernel + params->wm_prog_data->ksp_offset_2;

      if (params->wm_prog_data->dispatch_8)
         dw4 |= GEN7_PS_8_DISPATCH_ENABLE;
      if (params->wm_prog_data->dispatch_16)
         dw4 |= GEN7_PS_16_DISPATCH_ENABLE;
      if (params->wm_prog_data->num_varying_inputs)
         dw4 |= GEN7_PS_ATTRIBUTE_ENABLE;
   } else {
      /* The hardware gets angry if we don't enable at least one dispatch
       * mode, so just enable 16-pixel dispatch if we don't have a program.
       */
      dw4 |= GEN7_PS_16_DISPATCH_ENABLE;
   }

   if (params->src.bo)
      dw2 |= 1 << GEN7_PS_SAMPLER_COUNT_SHIFT; /* Up to 4 samplers */

   dw4 |= params->fast_clear_op;

   BEGIN_BATCH(8);
   OUT_BATCH(_3DSTATE_PS << 16 | (8 - 2));
   OUT_BATCH(ksp0);
   OUT_BATCH(dw2);
   OUT_BATCH(0);
   OUT_BATCH(dw4);
   OUT_BATCH(dw5);
   OUT_BATCH(0); /* kernel 1 pointer */
   OUT_BATCH(ksp2);
   ADVANCE_BATCH();
}


void
gen7_blorp_emit_binding_table_pointers_ps(struct brw_context *brw,
                                          uint32_t wm_bind_bo_offset)
{
   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_BINDING_TABLE_POINTERS_PS << 16 | (2 - 2));
   OUT_BATCH(wm_bind_bo_offset);
   ADVANCE_BATCH();
}


void
gen7_blorp_emit_sampler_state_pointers_ps(struct brw_context *brw,
                                          uint32_t sampler_offset)
{
   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_SAMPLER_STATE_POINTERS_PS << 16 | (2 - 2));
   OUT_BATCH(sampler_offset);
   ADVANCE_BATCH();
}

static void
gen7_blorp_emit_depth_stencil_config(struct brw_context *brw,
                                     const struct brw_blorp_params *params)
{
   const uint8_t mocs = GEN7_MOCS_L3;
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
      OUT_BATCH(GEN7_3DSTATE_DEPTH_BUFFER << 16 | (7 - 2));
      OUT_BATCH((params->depth.surf.row_pitch - 1) |
                params->depth_format << 18 |
                1 << 22 | /* hiz enable */
                1 << 28 | /* depth write */
                surftype << 29);
      OUT_RELOC(params->depth.bo,
                I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
                params->depth.offset);
      OUT_BATCH((params->depth.surf.logical_level0_px.width - 1) << 4 |
                (params->depth.surf.logical_level0_px.height - 1) << 18 |
                params->depth.view.base_level);
      OUT_BATCH(((depth - 1) << 21) |
                (params->depth.view.base_array_layer << 10) |
                mocs);
      OUT_BATCH(0);
      OUT_BATCH((depth - 1) << 21);
      ADVANCE_BATCH();
   }

   /* 3DSTATE_HIER_DEPTH_BUFFER */
   {
      BEGIN_BATCH(3);
      OUT_BATCH((GEN7_3DSTATE_HIER_DEPTH_BUFFER << 16) | (3 - 2));
      OUT_BATCH((mocs << 25) |
                (params->depth.aux_surf.row_pitch - 1));
      OUT_RELOC(params->depth.aux_bo,
                I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
                params->depth.aux_offset);
      ADVANCE_BATCH();
   }

   /* 3DSTATE_STENCIL_BUFFER */
   {
      BEGIN_BATCH(3);
      OUT_BATCH((GEN7_3DSTATE_STENCIL_BUFFER << 16) | (3 - 2));
      OUT_BATCH(0);
      OUT_BATCH(0);
      ADVANCE_BATCH();
   }
}


static void
gen7_blorp_emit_depth_disable(struct brw_context *brw)
{
   brw_emit_depth_stall_flushes(brw);

   BEGIN_BATCH(7);
   OUT_BATCH(GEN7_3DSTATE_DEPTH_BUFFER << 16 | (7 - 2));
   OUT_BATCH(BRW_DEPTHFORMAT_D32_FLOAT << 18 | (BRW_SURFACE_NULL << 29));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();

   BEGIN_BATCH(3);
   OUT_BATCH(GEN7_3DSTATE_HIER_DEPTH_BUFFER << 16 | (3 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();

   BEGIN_BATCH(3);
   OUT_BATCH(GEN7_3DSTATE_STENCIL_BUFFER << 16 | (3 - 2));
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


/* 3DSTATE_CLEAR_PARAMS
 *
 * From the Ivybridge PRM, Volume 2 Part 1, Section 11.5.5.4
 * 3DSTATE_CLEAR_PARAMS:
 *    3DSTATE_CLEAR_PARAMS must always be programmed in the along
 *    with the other Depth/Stencil state commands(i.e.  3DSTATE_DEPTH_BUFFER,
 *    3DSTATE_STENCIL_BUFFER, or 3DSTATE_HIER_DEPTH_BUFFER).
 */
void
gen7_blorp_emit_clear_params(struct brw_context *brw,
                             const struct brw_blorp_params *params)
{
   BEGIN_BATCH(3);
   OUT_BATCH(GEN7_3DSTATE_CLEAR_PARAMS << 16 | (3 - 2));
   OUT_BATCH(params->depth.clear_color.u32[0]);
   OUT_BATCH(GEN7_DEPTH_CLEAR_VALID);
   ADVANCE_BATCH();
}


/* 3DPRIMITIVE */
void
gen7_blorp_emit_primitive(struct brw_context *brw,
                          const struct brw_blorp_params *params)
{
   BEGIN_BATCH(7);
   OUT_BATCH(CMD_3D_PRIM << 16 | (7 - 2));
   OUT_BATCH(GEN7_3DPRIM_VERTEXBUFFER_ACCESS_SEQUENTIAL |
             _3DPRIM_RECTLIST);
   OUT_BATCH(3); /* vertex count per instance */
   OUT_BATCH(0);
   OUT_BATCH(params->num_layers); /* instance count */
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


/**
 * \copydoc gen6_blorp_exec()
 */
void
gen7_blorp_exec(struct brw_context *brw,
                const struct brw_blorp_params *params)
{
   if (brw->gen >= 8)
      return;

   uint32_t cc_blend_state_offset = 0;
   uint32_t cc_state_offset = 0;
   uint32_t depthstencil_offset;
   uint32_t wm_bind_bo_offset = 0;

   brw_upload_state_base_address(brw);

   gen6_emit_3dstate_multisample(brw, params->dst.surf.samples);
   gen6_emit_3dstate_sample_mask(brw,
                                 params->dst.surf.samples > 1 ?
                                 (1 << params->dst.surf.samples) - 1 : 1);
   gen6_blorp_emit_vertices(brw, params);
   gen7_blorp_emit_urb_config(brw, params);
   if (params->wm_prog_data) {
      cc_blend_state_offset = gen6_blorp_emit_blend_state(brw, params);
      cc_state_offset = gen6_blorp_emit_cc_state(brw);
      gen7_blorp_emit_blend_state_pointer(brw, cc_blend_state_offset);
      gen7_blorp_emit_cc_state_pointer(brw, cc_state_offset);
   }

   gen7_blorp_disable_constant_state(brw, _3DSTATE_CONSTANT_VS);
   gen7_blorp_disable_constant_state(brw, _3DSTATE_CONSTANT_HS);
   gen7_blorp_disable_constant_state(brw, _3DSTATE_CONSTANT_DS);
   gen7_blorp_disable_constant_state(brw, _3DSTATE_CONSTANT_GS);
   gen7_blorp_disable_constant_state(brw, _3DSTATE_CONSTANT_PS);

   depthstencil_offset = gen6_blorp_emit_depth_stencil_state(brw, params);
   gen7_blorp_emit_depth_stencil_state_pointers(brw, depthstencil_offset);
   if (brw->use_resource_streamer)
      gen7_disable_hw_binding_tables(brw);
   if (params->wm_prog_data) {
      uint32_t wm_surf_offset_renderbuffer;
      uint32_t wm_surf_offset_texture = 0;

      wm_surf_offset_renderbuffer =
         brw_blorp_emit_surface_state(brw, &params->dst,
                                      I915_GEM_DOMAIN_RENDER,
                                      I915_GEM_DOMAIN_RENDER,
                                      true /* is_render_target */);
      if (params->src.bo) {
         wm_surf_offset_texture =
            brw_blorp_emit_surface_state(brw, &params->src,
                                         I915_GEM_DOMAIN_SAMPLER, 0,
                                         false /* is_render_target */);
      }
      wm_bind_bo_offset =
         gen6_blorp_emit_binding_table(brw,
                                       wm_surf_offset_renderbuffer,
                                       wm_surf_offset_texture);
   }
   gen7_blorp_emit_vs_disable(brw);
   gen7_blorp_emit_hs_disable(brw);
   gen7_blorp_emit_te_disable(brw);
   gen7_blorp_emit_ds_disable(brw);
   gen7_blorp_emit_gs_disable(brw);
   gen7_blorp_emit_streamout_disable(brw);
   gen6_blorp_emit_clip_disable(brw);
   gen7_blorp_emit_sf_config(brw, params);
   gen7_blorp_emit_wm_config(brw, params);
   if (params->wm_prog_data)
      gen7_blorp_emit_binding_table_pointers_ps(brw, wm_bind_bo_offset);

   if (params->src.bo) {
      const uint32_t sampler_offset =
         gen6_blorp_emit_sampler_state(brw, BRW_MAPFILTER_LINEAR, 0, true);
      gen7_blorp_emit_sampler_state_pointers_ps(brw, sampler_offset);
   }

   gen7_blorp_emit_ps_config(brw, params);
   gen7_blorp_emit_cc_viewport(brw);

   if (params->depth.bo)
      gen7_blorp_emit_depth_stencil_config(brw, params);
   else
      gen7_blorp_emit_depth_disable(brw);
   gen7_blorp_emit_clear_params(brw, params);
   gen6_blorp_emit_drawing_rectangle(brw, params);
   gen7_blorp_emit_primitive(brw, params);
}
