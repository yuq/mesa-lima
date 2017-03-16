/*
 * Copyright © 2017 Intel Corporation
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

#include "common/gen_device_info.h"
#include "genxml/gen_macros.h"

#include "brw_context.h"
#include "brw_state.h"

#include "intel_batchbuffer.h"
#include "intel_fbo.h"

#include "main/fbobject.h"
#include "main/framebuffer.h"
#include "main/stencil.h"

UNUSED static void *
emit_dwords(struct brw_context *brw, unsigned n)
{
   intel_batchbuffer_begin(brw, n, RENDER_RING);
   uint32_t *map = brw->batch.map_next;
   brw->batch.map_next += n;
   intel_batchbuffer_advance(brw);
   return map;
}

struct brw_address {
   struct brw_bo *bo;
   uint32_t read_domains;
   uint32_t write_domain;
   uint32_t offset;
};

static uint64_t
emit_reloc(struct brw_context *brw,
           void *location, struct brw_address address, uint32_t delta)
{
   uint32_t offset = (char *) location - (char *) brw->batch.map;

   return brw_emit_reloc(&brw->batch, offset, address.bo,
                         address.offset + delta,
                         address.read_domains,
                         address.write_domain);
}

#define __gen_address_type struct brw_address
#define __gen_user_data struct brw_context

static uint64_t
__gen_combine_address(struct brw_context *brw, void *location,
                      struct brw_address address, uint32_t delta)
{
   if (address.bo == NULL) {
      return address.offset + delta;
   } else {
      return emit_reloc(brw, location, address, delta);
   }
}

#include "genxml/genX_pack.h"

#define _brw_cmd_length(cmd) cmd ## _length
#define _brw_cmd_length_bias(cmd) cmd ## _length_bias
#define _brw_cmd_header(cmd) cmd ## _header
#define _brw_cmd_pack(cmd) cmd ## _pack

#define brw_batch_emit(brw, cmd, name)                  \
   for (struct cmd name = { _brw_cmd_header(cmd) },     \
        *_dst = emit_dwords(brw, _brw_cmd_length(cmd)); \
        __builtin_expect(_dst != NULL, 1);              \
        _brw_cmd_pack(cmd)(brw, (void *)_dst, &name),   \
        _dst = NULL)

#define brw_batch_emitn(brw, cmd, n) ({                \
      uint32_t *_dw = emit_dwords(brw, n);             \
      struct cmd template = {                          \
         _brw_cmd_header(cmd),                         \
         .DWordLength = n - _brw_cmd_length_bias(cmd), \
      };                                               \
      _brw_cmd_pack(cmd)(brw, _dw, &template);         \
      _dw + 1; /* Array starts at dw[1] */             \
   })

#define brw_state_emit(brw, cmd, align, offset, name)              \
   for (struct cmd name = { 0, },                                  \
        *_dst = brw_state_batch(brw, _brw_cmd_length(cmd) * 4,     \
                                align, offset);                    \
        __builtin_expect(_dst != NULL, 1);                         \
        _brw_cmd_pack(cmd)(brw, (void *)_dst, &name),              \
        _dst = NULL)

/* ---------------------------------------------------------------------- */

#if GEN_GEN >= 6

/* ---------------------------------------------------------------------- */

static void
genX(upload_depth_stencil_state)(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;

   /* _NEW_BUFFERS */
   struct intel_renderbuffer *depth_irb =
      intel_get_renderbuffer(ctx->DrawBuffer, BUFFER_DEPTH);

   /* _NEW_DEPTH */
   struct gl_depthbuffer_attrib *depth = &ctx->Depth;

   /* _NEW_STENCIL */
   struct gl_stencil_attrib *stencil = &ctx->Stencil;
   const int b = stencil->_BackFace;

#if GEN_GEN >= 8
   brw_batch_emit(brw, GENX(3DSTATE_WM_DEPTH_STENCIL), wmds) {
#else
   uint32_t ds_offset;
   brw_state_emit(brw, GENX(DEPTH_STENCIL_STATE), 64, &ds_offset, wmds) {
#endif
      if (depth->Test && depth_irb) {
         wmds.DepthTestEnable = true;
         wmds.DepthBufferWriteEnable = brw_depth_writes_enabled(brw);
         wmds.DepthTestFunction = intel_translate_compare_func(depth->Func);
      }

      if (stencil->_Enabled) {
         wmds.StencilTestEnable = true;
         wmds.StencilWriteMask = stencil->WriteMask[0] & 0xff;
         wmds.StencilTestMask = stencil->ValueMask[0] & 0xff;

         wmds.StencilTestFunction =
            intel_translate_compare_func(stencil->Function[0]);
         wmds.StencilFailOp =
            intel_translate_stencil_op(stencil->FailFunc[0]);
         wmds.StencilPassDepthPassOp =
            intel_translate_stencil_op(stencil->ZPassFunc[0]);
         wmds.StencilPassDepthFailOp =
            intel_translate_stencil_op(stencil->ZFailFunc[0]);

         wmds.StencilBufferWriteEnable = stencil->_WriteEnabled;

         if (stencil->_TestTwoSide) {
            wmds.DoubleSidedStencilEnable = true;
            wmds.BackfaceStencilWriteMask = stencil->WriteMask[b] & 0xff;
            wmds.BackfaceStencilTestMask = stencil->ValueMask[b] & 0xff;

            wmds.BackfaceStencilTestFunction =
               intel_translate_compare_func(stencil->Function[b]);
            wmds.BackfaceStencilFailOp =
               intel_translate_stencil_op(stencil->FailFunc[b]);
            wmds.BackfaceStencilPassDepthPassOp =
               intel_translate_stencil_op(stencil->ZPassFunc[b]);
            wmds.BackfaceStencilPassDepthFailOp =
               intel_translate_stencil_op(stencil->ZFailFunc[b]);
         }

#if GEN_GEN >= 9
         wmds.StencilReferenceValue = _mesa_get_stencil_ref(ctx, 0);
         wmds.BackfaceStencilReferenceValue = _mesa_get_stencil_ref(ctx, b);
#endif
      }
   }

#if GEN_GEN == 6
   brw_batch_emit(brw, GENX(3DSTATE_CC_STATE_POINTERS), ptr) {
      ptr.PointertoDEPTH_STENCIL_STATE = ds_offset;
      ptr.DEPTH_STENCIL_STATEChange = true;
   }
#elif GEN_GEN == 7
   brw_batch_emit(brw, GENX(3DSTATE_DEPTH_STENCIL_STATE_POINTERS), ptr) {
      ptr.PointertoDEPTH_STENCIL_STATE = ds_offset;
   }
#endif
}

static const struct brw_tracked_state genX(depth_stencil_state) = {
   .dirty = {
      .mesa = _NEW_BUFFERS |
              _NEW_DEPTH |
              _NEW_STENCIL,
      .brw  = BRW_NEW_BLORP |
              (GEN_GEN >= 8 ? BRW_NEW_CONTEXT
                            : BRW_NEW_BATCH |
                              BRW_NEW_STATE_BASE_ADDRESS),
   },
   .emit = genX(upload_depth_stencil_state),
};

/* ---------------------------------------------------------------------- */

static void
genX(upload_clip_state)(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;

   /* _NEW_BUFFERS */
   struct gl_framebuffer *fb = ctx->DrawBuffer;

   /* BRW_NEW_FS_PROG_DATA */
   struct brw_wm_prog_data *wm_prog_data =
      brw_wm_prog_data(brw->wm.base.prog_data);

   brw_batch_emit(brw, GENX(3DSTATE_CLIP), clip) {
      clip.StatisticsEnable = !brw->meta_in_progress;

      if (wm_prog_data->barycentric_interp_modes &
          BRW_BARYCENTRIC_NONPERSPECTIVE_BITS)
         clip.NonPerspectiveBarycentricEnable = true;

#if GEN_GEN >= 7
      clip.EarlyCullEnable = true;
#endif

#if GEN_GEN == 7
      clip.FrontWinding = ctx->Polygon._FrontBit == _mesa_is_user_fbo(fb);

      if (ctx->Polygon.CullFlag) {
         switch (ctx->Polygon.CullFaceMode) {
         case GL_FRONT:
            clip.CullMode = CULLMODE_FRONT;
            break;
         case GL_BACK:
            clip.CullMode = CULLMODE_BACK;
            break;
         case GL_FRONT_AND_BACK:
            clip.CullMode = CULLMODE_BOTH;
            break;
         default:
            unreachable("Should not get here: invalid CullFlag");
         }
      } else {
         clip.CullMode = CULLMODE_NONE;
      }
#endif

#if GEN_GEN < 8
      clip.UserClipDistanceCullTestEnableBitmask =
         brw_vue_prog_data(brw->vs.base.prog_data)->cull_distance_mask;

      clip.ViewportZClipTestEnable = !ctx->Transform.DepthClamp;
#endif

      /* _NEW_LIGHT */
      if (ctx->Light.ProvokingVertex == GL_FIRST_VERTEX_CONVENTION) {
         clip.TriangleStripListProvokingVertexSelect = 0;
         clip.TriangleFanProvokingVertexSelect = 1;
         clip.LineStripListProvokingVertexSelect = 0;
      } else {
         clip.TriangleStripListProvokingVertexSelect = 2;
         clip.TriangleFanProvokingVertexSelect = 2;
         clip.LineStripListProvokingVertexSelect = 1;
      }

      /* _NEW_TRANSFORM */
      clip.UserClipDistanceClipTestEnableBitmask =
         ctx->Transform.ClipPlanesEnabled;

#if GEN_GEN >= 8
      clip.ForceUserClipDistanceClipTestEnableBitmask = true;
#endif

      if (ctx->Transform.ClipDepthMode == GL_ZERO_TO_ONE)
         clip.APIMode = APIMODE_D3D;
      else
         clip.APIMode = APIMODE_OGL;

      clip.GuardbandClipTestEnable = true;

      /* BRW_NEW_VIEWPORT_COUNT */
      const unsigned viewport_count = brw->clip.viewport_count;

      if (ctx->RasterDiscard) {
         clip.ClipMode = CLIPMODE_REJECT_ALL;
#if GEN_GEN == 6
         perf_debug("Rasterizer discard is currently implemented via the "
                    "clipper; having the GS not write primitives would "
                    "likely be faster.\n");
#endif
      } else {
         clip.ClipMode = CLIPMODE_NORMAL;
      }

      clip.ClipEnable = brw->primitive != _3DPRIM_RECTLIST;

      /* _NEW_POLYGON,
       * BRW_NEW_GEOMETRY_PROGRAM | BRW_NEW_TES_PROG_DATA | BRW_NEW_PRIMITIVE
       */
      if (!brw_is_drawing_points(brw) && !brw_is_drawing_lines(brw))
         clip.ViewportXYClipTestEnable = true;

      clip.MinimumPointWidth = 0.125;
      clip.MaximumPointWidth = 255.875;
      clip.MaximumVPIndex = viewport_count - 1;
      if (_mesa_geometric_layers(fb) == 0)
         clip.ForceZeroRTAIndexEnable = true;
   }
}

static const struct brw_tracked_state genX(clip_state) = {
   .dirty = {
      .mesa  = _NEW_BUFFERS |
               _NEW_LIGHT |
               _NEW_POLYGON |
               _NEW_TRANSFORM,
      .brw   = BRW_NEW_BLORP |
               BRW_NEW_CONTEXT |
               BRW_NEW_FS_PROG_DATA |
               BRW_NEW_GS_PROG_DATA |
               BRW_NEW_VS_PROG_DATA |
               BRW_NEW_META_IN_PROGRESS |
               BRW_NEW_PRIMITIVE |
               BRW_NEW_RASTERIZER_DISCARD |
               BRW_NEW_TES_PROG_DATA |
               BRW_NEW_VIEWPORT_COUNT,
   },
   .emit = genX(upload_clip_state),
};

#endif

/* ---------------------------------------------------------------------- */

#if GEN_GEN >= 8
static void
genX(upload_raster)(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;

   /* _NEW_BUFFERS */
   bool render_to_fbo = _mesa_is_user_fbo(ctx->DrawBuffer);

   /* _NEW_POLYGON */
   struct gl_polygon_attrib *polygon = &ctx->Polygon;

   /* _NEW_POINT */
   struct gl_point_attrib *point = &ctx->Point;

   brw_batch_emit(brw, GENX(3DSTATE_RASTER), raster) {
      if (polygon->_FrontBit == render_to_fbo)
         raster.FrontWinding = CounterClockwise;

      if (polygon->CullFlag) {
         switch (polygon->CullFaceMode) {
         case GL_FRONT:
            raster.CullMode = CULLMODE_FRONT;
            break;
         case GL_BACK:
            raster.CullMode = CULLMODE_BACK;
            break;
         case GL_FRONT_AND_BACK:
            raster.CullMode = CULLMODE_BOTH;
            break;
         default:
            unreachable("not reached");
         }
      } else {
         raster.CullMode = CULLMODE_NONE;
      }

      point->SmoothFlag = raster.SmoothPointEnable;

      raster.DXMultisampleRasterizationEnable =
         _mesa_is_multisample_enabled(ctx);

      raster.GlobalDepthOffsetEnableSolid = polygon->OffsetFill;
      raster.GlobalDepthOffsetEnableWireframe = polygon->OffsetLine;
      raster.GlobalDepthOffsetEnablePoint = polygon->OffsetPoint;

      switch (polygon->FrontMode) {
      case GL_FILL:
         raster.FrontFaceFillMode = FILL_MODE_SOLID;
         break;
      case GL_LINE:
         raster.FrontFaceFillMode = FILL_MODE_WIREFRAME;
         break;
      case GL_POINT:
         raster.FrontFaceFillMode = FILL_MODE_POINT;
         break;
      default:
         unreachable("not reached");
      }

      switch (polygon->BackMode) {
      case GL_FILL:
         raster.BackFaceFillMode = FILL_MODE_SOLID;
         break;
      case GL_LINE:
         raster.BackFaceFillMode = FILL_MODE_WIREFRAME;
         break;
      case GL_POINT:
         raster.BackFaceFillMode = FILL_MODE_POINT;
         break;
      default:
         unreachable("not reached");
      }

      /* _NEW_LINE */
      raster.AntialiasingEnable = ctx->Line.SmoothFlag;

      /* _NEW_SCISSOR */
      raster.ScissorRectangleEnable = ctx->Scissor.EnableFlags;

      /* _NEW_TRANSFORM */
      if (!ctx->Transform.DepthClamp) {
#if GEN_GEN >= 9
         raster.ViewportZFarClipTestEnable = true;
         raster.ViewportZNearClipTestEnable = true;
#else
         raster.ViewportZClipTestEnable = true;
#endif
      }

      /* BRW_NEW_CONSERVATIVE_RASTERIZATION */
#if GEN_GEN >= 9
      raster.ConservativeRasterizationEnable =
         ctx->IntelConservativeRasterization;
#endif

      raster.GlobalDepthOffsetClamp = polygon->OffsetClamp;
      raster.GlobalDepthOffsetScale = polygon->OffsetFactor;

      raster.GlobalDepthOffsetConstant = polygon->OffsetUnits * 2;
   }
}

static const struct brw_tracked_state genX(raster_state) = {
   .dirty = {
      .mesa  = _NEW_BUFFERS |
               _NEW_LINE |
               _NEW_MULTISAMPLE |
               _NEW_POINT |
               _NEW_POLYGON |
               _NEW_SCISSOR |
               _NEW_TRANSFORM,
      .brw   = BRW_NEW_BLORP |
               BRW_NEW_CONTEXT |
               BRW_NEW_CONSERVATIVE_RASTERIZATION,
   },
   .emit = genX(upload_raster),
};
#endif

/* ---------------------------------------------------------------------- */

void
genX(init_atoms)(struct brw_context *brw)
{
#if GEN_GEN < 6
   static const struct brw_tracked_state *render_atoms[] =
   {
      /* Once all the programs are done, we know how large urb entry
       * sizes need to be and can decide if we need to change the urb
       * layout.
       */
      &brw_curbe_offsets,
      &brw_recalculate_urb_fence,

      &brw_cc_vp,
      &brw_cc_unit,

      /* Surface state setup.  Must come before the VS/WM unit.  The binding
       * table upload must be last.
       */
      &brw_vs_pull_constants,
      &brw_wm_pull_constants,
      &brw_renderbuffer_surfaces,
      &brw_renderbuffer_read_surfaces,
      &brw_texture_surfaces,
      &brw_vs_binding_table,
      &brw_wm_binding_table,

      &brw_fs_samplers,
      &brw_vs_samplers,

      /* These set up state for brw_psp_urb_cbs */
      &brw_wm_unit,
      &brw_sf_vp,
      &brw_sf_unit,
      &brw_vs_unit,		/* always required, enabled or not */
      &brw_clip_unit,
      &brw_gs_unit,

      /* Command packets:
       */
      &brw_invariant_state,

      &brw_binding_table_pointers,
      &brw_blend_constant_color,

      &brw_depthbuffer,

      &brw_polygon_stipple,
      &brw_polygon_stipple_offset,

      &brw_line_stipple,

      &brw_psp_urb_cbs,

      &brw_drawing_rect,
      &brw_indices, /* must come before brw_vertices */
      &brw_index_buffer,
      &brw_vertices,

      &brw_constant_buffer
   };
#elif GEN_GEN == 6
   static const struct brw_tracked_state *render_atoms[] =
   {
      &gen6_sf_and_clip_viewports,

      /* Command packets: */

      &brw_cc_vp,
      &gen6_viewport_state,	/* must do after *_vp stages */

      &gen6_urb,
      &gen6_blend_state,		/* must do before cc unit */
      &gen6_color_calc_state,	/* must do before cc unit */
      &gen6_depth_stencil_state,	/* must do before cc unit */

      &gen6_vs_push_constants, /* Before vs_state */
      &gen6_gs_push_constants, /* Before gs_state */
      &gen6_wm_push_constants, /* Before wm_state */

      /* Surface state setup.  Must come before the VS/WM unit.  The binding
       * table upload must be last.
       */
      &brw_vs_pull_constants,
      &brw_vs_ubo_surfaces,
      &brw_gs_pull_constants,
      &brw_gs_ubo_surfaces,
      &brw_wm_pull_constants,
      &brw_wm_ubo_surfaces,
      &gen6_renderbuffer_surfaces,
      &brw_renderbuffer_read_surfaces,
      &brw_texture_surfaces,
      &gen6_sol_surface,
      &brw_vs_binding_table,
      &gen6_gs_binding_table,
      &brw_wm_binding_table,

      &brw_fs_samplers,
      &brw_vs_samplers,
      &brw_gs_samplers,
      &gen6_sampler_state,
      &gen6_multisample_state,

      &gen6_vs_state,
      &gen6_gs_state,
      &genX(clip_state),
      &gen6_sf_state,
      &gen6_wm_state,

      &gen6_scissor_state,

      &gen6_binding_table_pointers,

      &brw_depthbuffer,

      &brw_polygon_stipple,
      &brw_polygon_stipple_offset,

      &brw_line_stipple,

      &brw_drawing_rect,

      &brw_indices, /* must come before brw_vertices */
      &brw_index_buffer,
      &brw_vertices,
   };
#elif GEN_GEN == 7
   static const struct brw_tracked_state *render_atoms[] =
   {
      /* Command packets: */

      &brw_cc_vp,
      &gen7_sf_clip_viewport,

      &gen7_l3_state,
      &gen7_push_constant_space,
      &gen7_urb,
      &gen6_blend_state,		/* must do before cc unit */
      &gen6_color_calc_state,	/* must do before cc unit */
      &genX(depth_stencil_state),	/* must do before cc unit */

      &brw_vs_image_surfaces, /* Before vs push/pull constants and binding table */
      &brw_tcs_image_surfaces, /* Before tcs push/pull constants and binding table */
      &brw_tes_image_surfaces, /* Before tes push/pull constants and binding table */
      &brw_gs_image_surfaces, /* Before gs push/pull constants and binding table */
      &brw_wm_image_surfaces, /* Before wm push/pull constants and binding table */

      &gen6_vs_push_constants, /* Before vs_state */
      &gen7_tcs_push_constants,
      &gen7_tes_push_constants,
      &gen6_gs_push_constants, /* Before gs_state */
      &gen6_wm_push_constants, /* Before wm_surfaces and constant_buffer */

      /* Surface state setup.  Must come before the VS/WM unit.  The binding
       * table upload must be last.
       */
      &brw_vs_pull_constants,
      &brw_vs_ubo_surfaces,
      &brw_vs_abo_surfaces,
      &brw_tcs_pull_constants,
      &brw_tcs_ubo_surfaces,
      &brw_tcs_abo_surfaces,
      &brw_tes_pull_constants,
      &brw_tes_ubo_surfaces,
      &brw_tes_abo_surfaces,
      &brw_gs_pull_constants,
      &brw_gs_ubo_surfaces,
      &brw_gs_abo_surfaces,
      &brw_wm_pull_constants,
      &brw_wm_ubo_surfaces,
      &brw_wm_abo_surfaces,
      &gen6_renderbuffer_surfaces,
      &brw_renderbuffer_read_surfaces,
      &brw_texture_surfaces,
      &brw_vs_binding_table,
      &brw_tcs_binding_table,
      &brw_tes_binding_table,
      &brw_gs_binding_table,
      &brw_wm_binding_table,

      &brw_fs_samplers,
      &brw_vs_samplers,
      &brw_tcs_samplers,
      &brw_tes_samplers,
      &brw_gs_samplers,
      &gen6_multisample_state,

      &gen7_vs_state,
      &gen7_hs_state,
      &gen7_te_state,
      &gen7_ds_state,
      &gen7_gs_state,
      &gen7_sol_state,
      &genX(clip_state),
      &gen7_sbe_state,
      &gen7_sf_state,
      &gen7_wm_state,
      &gen7_ps_state,

      &gen6_scissor_state,

      &gen7_depthbuffer,

      &brw_polygon_stipple,
      &brw_polygon_stipple_offset,

      &brw_line_stipple,

      &brw_drawing_rect,

      &brw_indices, /* must come before brw_vertices */
      &brw_index_buffer,
      &brw_vertices,

      &haswell_cut_index,
   };
#elif GEN_GEN >= 8
   static const struct brw_tracked_state *render_atoms[] =
   {
      &brw_cc_vp,
      &gen8_sf_clip_viewport,

      &gen7_l3_state,
      &gen7_push_constant_space,
      &gen7_urb,
      &gen8_blend_state,
      &gen6_color_calc_state,

      &brw_vs_image_surfaces, /* Before vs push/pull constants and binding table */
      &brw_tcs_image_surfaces, /* Before tcs push/pull constants and binding table */
      &brw_tes_image_surfaces, /* Before tes push/pull constants and binding table */
      &brw_gs_image_surfaces, /* Before gs push/pull constants and binding table */
      &brw_wm_image_surfaces, /* Before wm push/pull constants and binding table */

      &gen6_vs_push_constants, /* Before vs_state */
      &gen7_tcs_push_constants,
      &gen7_tes_push_constants,
      &gen6_gs_push_constants, /* Before gs_state */
      &gen6_wm_push_constants, /* Before wm_surfaces and constant_buffer */

      /* Surface state setup.  Must come before the VS/WM unit.  The binding
       * table upload must be last.
       */
      &brw_vs_pull_constants,
      &brw_vs_ubo_surfaces,
      &brw_vs_abo_surfaces,
      &brw_tcs_pull_constants,
      &brw_tcs_ubo_surfaces,
      &brw_tcs_abo_surfaces,
      &brw_tes_pull_constants,
      &brw_tes_ubo_surfaces,
      &brw_tes_abo_surfaces,
      &brw_gs_pull_constants,
      &brw_gs_ubo_surfaces,
      &brw_gs_abo_surfaces,
      &brw_wm_pull_constants,
      &brw_wm_ubo_surfaces,
      &brw_wm_abo_surfaces,
      &gen6_renderbuffer_surfaces,
      &brw_renderbuffer_read_surfaces,
      &brw_texture_surfaces,
      &brw_vs_binding_table,
      &brw_tcs_binding_table,
      &brw_tes_binding_table,
      &brw_gs_binding_table,
      &brw_wm_binding_table,

      &brw_fs_samplers,
      &brw_vs_samplers,
      &brw_tcs_samplers,
      &brw_tes_samplers,
      &brw_gs_samplers,
      &gen8_multisample_state,

      &gen8_vs_state,
      &gen8_hs_state,
      &gen7_te_state,
      &gen8_ds_state,
      &gen8_gs_state,
      &gen7_sol_state,
      &genX(clip_state),
      &genX(raster_state),
      &gen8_sbe_state,
      &gen8_sf_state,
      &gen8_ps_blend,
      &gen8_ps_extra,
      &gen8_ps_state,
      &genX(depth_stencil_state),
      &gen8_wm_state,

      &gen6_scissor_state,

      &gen7_depthbuffer,

      &brw_polygon_stipple,
      &brw_polygon_stipple_offset,

      &brw_line_stipple,

      &brw_drawing_rect,

      &gen8_vf_topology,

      &brw_indices,
      &gen8_index_buffer,
      &gen8_vertices,

      &haswell_cut_index,
      &gen8_pma_fix,
   };
#endif

   STATIC_ASSERT(ARRAY_SIZE(render_atoms) <= ARRAY_SIZE(brw->render_atoms));
   brw_copy_pipeline_atoms(brw, BRW_RENDER_PIPELINE,
                           render_atoms, ARRAY_SIZE(render_atoms));

#if GEN_GEN >= 7
   static const struct brw_tracked_state *compute_atoms[] =
   {
      &gen7_l3_state,
      &brw_cs_image_surfaces,
      &gen7_cs_push_constants,
      &brw_cs_pull_constants,
      &brw_cs_ubo_surfaces,
      &brw_cs_abo_surfaces,
      &brw_cs_texture_surfaces,
      &brw_cs_work_groups_surface,
      &brw_cs_samplers,
      &brw_cs_state,
   };

   STATIC_ASSERT(ARRAY_SIZE(compute_atoms) <= ARRAY_SIZE(brw->compute_atoms));
   brw_copy_pipeline_atoms(brw, BRW_COMPUTE_PIPELINE,
                           compute_atoms, ARRAY_SIZE(compute_atoms));
#endif
}
