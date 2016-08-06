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

static void
gen6_blorp_emit_input_varying_data(struct brw_context *brw,
                                   const struct brw_blorp_params *params,
                                   unsigned *offset,
                                   unsigned *size)
{
   const unsigned vec4_size_in_bytes = 4 * sizeof(float);
   const unsigned max_num_varyings =
      DIV_ROUND_UP(sizeof(params->wm_inputs), vec4_size_in_bytes);
   const unsigned num_varyings = params->wm_prog_data->num_varying_inputs;

   *size = num_varyings * vec4_size_in_bytes;

   const float *const inputs_src = (const float *)&params->wm_inputs;
   float *inputs = (float *)brw_state_batch(brw, AUB_TRACE_VERTEX_BUFFER,
                                            *size, 32, offset);

   /* Walk over the attribute slots, determine if the attribute is used by
    * the program and when necessary copy the values from the input storage to
    * the vertex data buffer.
    */
   for (unsigned i = 0; i < max_num_varyings; i++) {
      const gl_varying_slot attr = VARYING_SLOT_VAR0 + i;

      if (!(params->wm_prog_data->inputs_read & BITFIELD64_BIT(attr)))
         continue;

      memcpy(inputs, inputs_src + i * 4, vec4_size_in_bytes);

      inputs += 4;
   }
}

static void
gen6_blorp_emit_vertex_data(struct brw_context *brw,
                            const struct brw_blorp_params *params)
{
   uint32_t vertex_offset;
   uint32_t const_data_offset = 0;
   unsigned const_data_size = 0;

   /* Setup VBO for the rectangle primitive..
    *
    * A rectangle primitive (3DPRIM_RECTLIST) consists of only three
    * vertices. The vertices reside in screen space with DirectX coordinates
    * (that is, (0, 0) is the upper left corner).
    *
    *   v2 ------ implied
    *    |        |
    *    |        |
    *   v0 ----- v1
    *
    * Since the VS is disabled, the clipper loads each VUE directly from
    * the URB. This is controlled by the 3DSTATE_VERTEX_BUFFERS and
    * 3DSTATE_VERTEX_ELEMENTS packets below. The VUE contents are as follows:
    *   dw0: Reserved, MBZ.
    *   dw1: Render Target Array Index. The HiZ op does not use indexed
    *        vertices, so set the dword to 0.
    *   dw2: Viewport Index. The HiZ op disables viewport mapping and
    *        scissoring, so set the dword to 0.
    *   dw3: Point Width: The HiZ op does not emit the POINTLIST primitive, so
    *        set the dword to 0.
    *   dw4: Vertex Position X.
    *   dw5: Vertex Position Y.
    *   dw6: Vertex Position Z.
    *   dw7: Vertex Position W.
    *
    *   dw8: Flat vertex input 0
    *   dw9: Flat vertex input 1
    *   ...
    *   dwn: Flat vertex input n - 8
    *
    * For details, see the Sandybridge PRM, Volume 2, Part 1, Section 1.5.1
    * "Vertex URB Entry (VUE) Formats".
    *
    * Only vertex position X and Y are going to be variable, Z is fixed to
    * zero and W to one. Header words dw0-3 are all zero. There is no need to
    * include the fixed values in the vertex buffer. Vertex fetcher can be
    * instructed to fill vertex elements with constant values of one and zero
    * instead of reading them from the buffer.
    * Flat inputs are program constants that are not interpolated. Moreover
    * their values will be the same between vertices.
    *
    * See the vertex element setup below.
    */
   const float vertices[] = {
      /* v0 */ (float)params->x0, (float)params->y1,
      /* v1 */ (float)params->x1, (float)params->y1,
      /* v2 */ (float)params->x0, (float)params->y0,
   };

   float *const vertex_data = (float *)brw_state_batch(
                                          brw, AUB_TRACE_VERTEX_BUFFER,
                                          sizeof(vertices), 32,
                                          &vertex_offset);
   memcpy(vertex_data, vertices, sizeof(vertices));

   if (params->wm_prog_data && params->wm_prog_data->num_varying_inputs)
      gen6_blorp_emit_input_varying_data(brw, params,
                                         &const_data_offset,
                                         &const_data_size);

   /* 3DSTATE_VERTEX_BUFFERS */
   const int num_buffers = 1 + (const_data_size > 0);
   const int batch_length = 1 + 4 * num_buffers;

   BEGIN_BATCH(batch_length);
   OUT_BATCH((_3DSTATE_VERTEX_BUFFERS << 16) | (batch_length - 2));

   const unsigned blorp_num_vue_elems = 2;
   const unsigned stride = blorp_num_vue_elems * sizeof(float);
   EMIT_VERTEX_BUFFER_STATE(brw, 0 /* buffer_nr */, brw->batch.bo,
                            vertex_offset, vertex_offset + sizeof(vertices),
                            stride, 0 /* steprate */);

   if (const_data_size) {
      /* Tell vertex fetcher not to advance the pointer in the buffer when
       * moving to the next vertex. This will effectively provide the same
       * data for all the vertices. For flat inputs only the data provided
       * for the first provoking vertex actually matters.
       */
      const unsigned stride_zero = 0;
      EMIT_VERTEX_BUFFER_STATE(brw, 1 /* buffer_nr */, brw->batch.bo,
                               const_data_offset,
                               const_data_offset + const_data_size,
                               stride_zero, 0 /* step_rate */);
   }

   ADVANCE_BATCH();
}

void
gen6_blorp_emit_vertices(struct brw_context *brw,
                         const struct brw_blorp_params *params)
{
   gen6_blorp_emit_vertex_data(brw, params);

   const unsigned num_varyings =
      params->wm_prog_data ? params->wm_prog_data->num_varying_inputs : 0;
   const unsigned num_elements = 2 + num_varyings;
   const int batch_length = 1 + 2 * num_elements;

   BEGIN_BATCH(batch_length);

   /* 3DSTATE_VERTEX_ELEMENTS
    *
    * Fetch dwords 0 - 7 from each VUE. See the comments above where
    * the vertex_bo is filled with data. First element contains dwords
    * for the VUE header, second the actual position values and the
    * remaining contain the flat inputs.
    */
   {
      OUT_BATCH((_3DSTATE_VERTEX_ELEMENTS << 16) | (batch_length - 2));
      /* Element 0 */
      OUT_BATCH(GEN6_VE0_VALID |
                BRW_SURFACEFORMAT_R32G32B32A32_FLOAT << BRW_VE0_FORMAT_SHIFT |
                0 << BRW_VE0_SRC_OFFSET_SHIFT);
      OUT_BATCH(BRW_VE1_COMPONENT_STORE_0 << BRW_VE1_COMPONENT_0_SHIFT |
                BRW_VE1_COMPONENT_STORE_0 << BRW_VE1_COMPONENT_1_SHIFT |
                BRW_VE1_COMPONENT_STORE_0 << BRW_VE1_COMPONENT_2_SHIFT |
                BRW_VE1_COMPONENT_STORE_0 << BRW_VE1_COMPONENT_3_SHIFT);
      /* Element 1 */
      OUT_BATCH(GEN6_VE0_VALID |
                BRW_SURFACEFORMAT_R32G32_FLOAT << BRW_VE0_FORMAT_SHIFT |
                0 << BRW_VE0_SRC_OFFSET_SHIFT);
      OUT_BATCH(BRW_VE1_COMPONENT_STORE_SRC << BRW_VE1_COMPONENT_0_SHIFT |
                BRW_VE1_COMPONENT_STORE_SRC << BRW_VE1_COMPONENT_1_SHIFT |
                BRW_VE1_COMPONENT_STORE_0 << BRW_VE1_COMPONENT_2_SHIFT |
                BRW_VE1_COMPONENT_STORE_1_FLT << BRW_VE1_COMPONENT_3_SHIFT);
   }

   for (unsigned i = 0; i < num_varyings; ++i) {
      /* Element 2 + i */
      OUT_BATCH(1 << GEN6_VE0_INDEX_SHIFT |
                GEN6_VE0_VALID |
                BRW_SURFACEFORMAT_R32G32B32A32_FLOAT << BRW_VE0_FORMAT_SHIFT |
                (i * 4 * sizeof(float)) << BRW_VE0_SRC_OFFSET_SHIFT);
      OUT_BATCH(BRW_VE1_COMPONENT_STORE_SRC << BRW_VE1_COMPONENT_0_SHIFT |
                BRW_VE1_COMPONENT_STORE_SRC << BRW_VE1_COMPONENT_1_SHIFT |
                BRW_VE1_COMPONENT_STORE_SRC << BRW_VE1_COMPONENT_2_SHIFT |
                BRW_VE1_COMPONENT_STORE_SRC << BRW_VE1_COMPONENT_3_SHIFT);
   }

   ADVANCE_BATCH();
}


/* BLEND_STATE */
uint32_t
gen6_blorp_emit_blend_state(struct brw_context *brw,
                            const struct brw_blorp_params *params)
{
   uint32_t cc_blend_state_offset;

   assume(params->num_draw_buffers);

   const unsigned size = params->num_draw_buffers *
                         sizeof(struct gen6_blend_state);
   struct gen6_blend_state *blend = (struct gen6_blend_state *)
      brw_state_batch(brw, AUB_TRACE_BLEND_STATE, size, 64,
                      &cc_blend_state_offset);

   memset(blend, 0, size);

   for (unsigned i = 0; i < params->num_draw_buffers; ++i) {
      blend[i].blend1.pre_blend_clamp_enable = 1;
      blend[i].blend1.post_blend_clamp_enable = 1;
      blend[i].blend1.clamp_range = BRW_RENDERTARGET_CLAMPRANGE_FORMAT;

      blend[i].blend1.write_disable_r = params->color_write_disable[0];
      blend[i].blend1.write_disable_g = params->color_write_disable[1];
      blend[i].blend1.write_disable_b = params->color_write_disable[2];
      blend[i].blend1.write_disable_a = params->color_write_disable[3];
   }

   return cc_blend_state_offset;
}


/* CC_STATE */
uint32_t
gen6_blorp_emit_cc_state(struct brw_context *brw)
{
   uint32_t cc_state_offset;

   struct gen6_color_calc_state *cc = (struct gen6_color_calc_state *)
      brw_state_batch(brw, AUB_TRACE_CC_STATE,
                      sizeof(gen6_color_calc_state), 64,
                      &cc_state_offset);
   memset(cc, 0, sizeof(*cc));

   return cc_state_offset;
}


/**
 * \param out_offset is relative to
 *        CMD_STATE_BASE_ADDRESS.DynamicStateBaseAddress.
 */
uint32_t
gen6_blorp_emit_depth_stencil_state(struct brw_context *brw,
                                    const struct brw_blorp_params *params)
{
   uint32_t depthstencil_offset;

   struct gen6_depth_stencil_state *state;
   state = (struct gen6_depth_stencil_state *)
      brw_state_batch(brw, AUB_TRACE_DEPTH_STENCIL_STATE,
                      sizeof(*state), 64,
                      &depthstencil_offset);
   memset(state, 0, sizeof(*state));

   /* See the following sections of the Sandy Bridge PRM, Volume 1, Part2:
    *   - 7.5.3.1 Depth Buffer Clear
    *   - 7.5.3.2 Depth Buffer Resolve
    *   - 7.5.3.3 Hierarchical Depth Buffer Resolve
    */
   state->ds2.depth_write_enable = 1;
   if (params->hiz_op == GEN6_HIZ_OP_DEPTH_RESOLVE) {
      state->ds2.depth_test_enable = 1;
      state->ds2.depth_test_func = BRW_COMPAREFUNCTION_NEVER;
   }

   return depthstencil_offset;
}


/* BINDING_TABLE.  See brw_wm_binding_table(). */
uint32_t
gen6_blorp_emit_binding_table(struct brw_context *brw,
                              uint32_t wm_surf_offset_renderbuffer,
                              uint32_t wm_surf_offset_texture)
{
   uint32_t wm_bind_bo_offset;
   uint32_t *bind = (uint32_t *)
      brw_state_batch(brw, AUB_TRACE_BINDING_TABLE,
                      sizeof(uint32_t) *
                      BRW_BLORP_NUM_BINDING_TABLE_ENTRIES,
                      32, /* alignment */
                      &wm_bind_bo_offset);
   bind[BRW_BLORP_RENDERBUFFER_BINDING_TABLE_INDEX] =
      wm_surf_offset_renderbuffer;
   bind[BRW_BLORP_TEXTURE_BINDING_TABLE_INDEX] = wm_surf_offset_texture;

   return wm_bind_bo_offset;
}


/**
 * SAMPLER_STATE.  See brw_update_sampler_state().
 */
uint32_t
gen6_blorp_emit_sampler_state(struct brw_context *brw,
                              unsigned tex_filter, unsigned max_lod,
                              bool non_normalized_coords)
{
   uint32_t sampler_offset;
   uint32_t *sampler_state = (uint32_t *)
      brw_state_batch(brw, AUB_TRACE_SAMPLER_STATE, 16, 32, &sampler_offset);

   unsigned address_rounding = BRW_ADDRESS_ROUNDING_ENABLE_U_MIN |
                               BRW_ADDRESS_ROUNDING_ENABLE_V_MIN |
                               BRW_ADDRESS_ROUNDING_ENABLE_R_MIN |
                               BRW_ADDRESS_ROUNDING_ENABLE_U_MAG |
                               BRW_ADDRESS_ROUNDING_ENABLE_V_MAG |
                               BRW_ADDRESS_ROUNDING_ENABLE_R_MAG;

   /* XXX: I don't think that using firstLevel, lastLevel works,
    * because we always setup the surface state as if firstLevel ==
    * level zero.  Probably have to subtract firstLevel from each of
    * these:
    */
   brw_emit_sampler_state(brw,
                          sampler_state,
                          sampler_offset,
                          tex_filter, /* min filter */
                          tex_filter, /* mag filter */
                          BRW_MIPFILTER_NONE,
                          BRW_ANISORATIO_2,
                          address_rounding,
                          BRW_TEXCOORDMODE_CLAMP,
                          BRW_TEXCOORDMODE_CLAMP,
                          BRW_TEXCOORDMODE_CLAMP,
                          0, /* min LOD */
                          max_lod,
                          0, /* LOD bias */
                          0, /* shadow function */
                          non_normalized_coords,
                          0); /* border color offset - unused */

   return sampler_offset;
}


/* 3DSTATE_CLIP
 *
 * Disable the clipper.
 *
 * The BLORP op emits a rectangle primitive, which requires clipping to
 * be disabled. From page 10 of the Sandy Bridge PRM Volume 2 Part 1
 * Section 1.3 "3D Primitives Overview":
 *    RECTLIST:
 *    Either the CLIP unit should be DISABLED, or the CLIP unit's Clip
 *    Mode should be set to a value other than CLIPMODE_NORMAL.
 *
 * Also disable perspective divide. This doesn't change the clipper's
 * output, but does spare a few electrons.
 */
void
gen6_blorp_emit_clip_disable(struct brw_context *brw)
{
   BEGIN_BATCH(4);
   OUT_BATCH(_3DSTATE_CLIP << 16 | (4 - 2));
   OUT_BATCH(0);
   OUT_BATCH(GEN6_CLIP_PERSPECTIVE_DIVIDE_DISABLE);
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


/* 3DSTATE_DRAWING_RECTANGLE */
void
gen6_blorp_emit_drawing_rectangle(struct brw_context *brw,
                                  const struct brw_blorp_params *params)
{
   BEGIN_BATCH(4);
   OUT_BATCH(_3DSTATE_DRAWING_RECTANGLE << 16 | (4 - 2));
   OUT_BATCH(0);
   OUT_BATCH(((MAX2(params->x1, params->x0) - 1) & 0xffff) |
             ((MAX2(params->y1, params->y0) - 1) << 16));
   OUT_BATCH(0);
   ADVANCE_BATCH();
}


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
