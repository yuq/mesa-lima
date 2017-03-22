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
#if GEN_GEN == 6
#include "brw_defines.h"
#endif
#include "brw_state.h"
#include "brw_wm.h"
#include "brw_util.h"

#include "intel_batchbuffer.h"
#include "intel_buffer_objects.h"
#include "intel_fbo.h"

#include "main/fbobject.h"
#include "main/framebuffer.h"
#include "main/stencil.h"
#include "main/transformfeedback.h"

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

static inline struct brw_address
render_bo(struct brw_bo *bo, uint32_t offset)
{
   return (struct brw_address) {
            .bo = bo,
            .offset = offset,
            .read_domains = I915_GEM_DOMAIN_RENDER,
            .write_domain = I915_GEM_DOMAIN_RENDER,
   };
}

static inline struct brw_address
instruction_bo(struct brw_bo *bo, uint32_t offset)
{
   return (struct brw_address) {
            .bo = bo,
            .offset = offset,
            .read_domains = I915_GEM_DOMAIN_INSTRUCTION,
            .write_domain = I915_GEM_DOMAIN_INSTRUCTION,
   };
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

#define brw_batch_emitn(brw, cmd, n, ...) ({           \
      uint32_t *_dw = emit_dwords(brw, n);             \
      struct cmd template = {                          \
         _brw_cmd_header(cmd),                         \
         .DWordLength = n - _brw_cmd_length_bias(cmd), \
         __VA_ARGS__                                   \
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

#if GEN_GEN >= 6
/**
 * Determine the appropriate attribute override value to store into the
 * 3DSTATE_SF structure for a given fragment shader attribute.  The attribute
 * override value contains two pieces of information: the location of the
 * attribute in the VUE (relative to urb_entry_read_offset, see below), and a
 * flag indicating whether to "swizzle" the attribute based on the direction
 * the triangle is facing.
 *
 * If an attribute is "swizzled", then the given VUE location is used for
 * front-facing triangles, and the VUE location that immediately follows is
 * used for back-facing triangles.  We use this to implement the mapping from
 * gl_FrontColor/gl_BackColor to gl_Color.
 *
 * urb_entry_read_offset is the offset into the VUE at which the SF unit is
 * being instructed to begin reading attribute data.  It can be set to a
 * nonzero value to prevent the SF unit from wasting time reading elements of
 * the VUE that are not needed by the fragment shader.  It is measured in
 * 256-bit increments.
 */
static void
genX(get_attr_override)(struct GENX(SF_OUTPUT_ATTRIBUTE_DETAIL) *attr,
                        const struct brw_vue_map *vue_map,
                        int urb_entry_read_offset, int fs_attr,
                        bool two_side_color, uint32_t *max_source_attr)
{
   /* Find the VUE slot for this attribute. */
   int slot = vue_map->varying_to_slot[fs_attr];

   /* Viewport and Layer are stored in the VUE header.  We need to override
    * them to zero if earlier stages didn't write them, as GL requires that
    * they read back as zero when not explicitly set.
    */
   if (fs_attr == VARYING_SLOT_VIEWPORT || fs_attr == VARYING_SLOT_LAYER) {
      attr->ComponentOverrideX = true;
      attr->ComponentOverrideW = true;
      attr->ConstantSource = CONST_0000;

      if (!(vue_map->slots_valid & VARYING_BIT_LAYER))
         attr->ComponentOverrideY = true;
      if (!(vue_map->slots_valid & VARYING_BIT_VIEWPORT))
         attr->ComponentOverrideZ = true;

      return;
   }

   /* If there was only a back color written but not front, use back
    * as the color instead of undefined
    */
   if (slot == -1 && fs_attr == VARYING_SLOT_COL0)
      slot = vue_map->varying_to_slot[VARYING_SLOT_BFC0];
   if (slot == -1 && fs_attr == VARYING_SLOT_COL1)
      slot = vue_map->varying_to_slot[VARYING_SLOT_BFC1];

   if (slot == -1) {
      /* This attribute does not exist in the VUE--that means that the vertex
       * shader did not write to it.  This means that either:
       *
       * (a) This attribute is a texture coordinate, and it is going to be
       * replaced with point coordinates (as a consequence of a call to
       * glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, GL_TRUE)), so the
       * hardware will ignore whatever attribute override we supply.
       *
       * (b) This attribute is read by the fragment shader but not written by
       * the vertex shader, so its value is undefined.  Therefore the
       * attribute override we supply doesn't matter.
       *
       * (c) This attribute is gl_PrimitiveID, and it wasn't written by the
       * previous shader stage.
       *
       * Note that we don't have to worry about the cases where the attribute
       * is gl_PointCoord or is undergoing point sprite coordinate
       * replacement, because in those cases, this function isn't called.
       *
       * In case (c), we need to program the attribute overrides so that the
       * primitive ID will be stored in this slot.  In every other case, the
       * attribute override we supply doesn't matter.  So just go ahead and
       * program primitive ID in every case.
       */
      attr->ComponentOverrideW = true;
      attr->ComponentOverrideX = true;
      attr->ComponentOverrideY = true;
      attr->ComponentOverrideZ = true;
      attr->ConstantSource = PRIM_ID;
      return;
   }

   /* Compute the location of the attribute relative to urb_entry_read_offset.
    * Each increment of urb_entry_read_offset represents a 256-bit value, so
    * it counts for two 128-bit VUE slots.
    */
   int source_attr = slot - 2 * urb_entry_read_offset;
   assert(source_attr >= 0 && source_attr < 32);

   /* If we are doing two-sided color, and the VUE slot following this one
    * represents a back-facing color, then we need to instruct the SF unit to
    * do back-facing swizzling.
    */
   bool swizzling = two_side_color &&
      ((vue_map->slot_to_varying[slot] == VARYING_SLOT_COL0 &&
        vue_map->slot_to_varying[slot+1] == VARYING_SLOT_BFC0) ||
       (vue_map->slot_to_varying[slot] == VARYING_SLOT_COL1 &&
        vue_map->slot_to_varying[slot+1] == VARYING_SLOT_BFC1));

   /* Update max_source_attr.  If swizzling, the SF will read this slot + 1. */
   if (*max_source_attr < source_attr + swizzling)
      *max_source_attr = source_attr + swizzling;

   attr->SourceAttribute = source_attr;
   if (swizzling)
      attr->SwizzleSelect = INPUTATTR_FACING;
}


static void
genX(calculate_attr_overrides)(const struct brw_context *brw,
                               struct GENX(SF_OUTPUT_ATTRIBUTE_DETAIL) *attr_overrides,
                               uint32_t *point_sprite_enables,
                               uint32_t *urb_entry_read_length,
                               uint32_t *urb_entry_read_offset)
{
   const struct gl_context *ctx = &brw->ctx;

   /* _NEW_POINT */
   const struct gl_point_attrib *point = &ctx->Point;

   /* BRW_NEW_FS_PROG_DATA */
   const struct brw_wm_prog_data *wm_prog_data =
      brw_wm_prog_data(brw->wm.base.prog_data);
   uint32_t max_source_attr = 0;

   *point_sprite_enables = 0;

   /* BRW_NEW_FRAGMENT_PROGRAM
    *
    * If the fragment shader reads VARYING_SLOT_LAYER, then we need to pass in
    * the full vertex header.  Otherwise, we can program the SF to start
    * reading at an offset of 1 (2 varying slots) to skip unnecessary data:
    * - VARYING_SLOT_PSIZ and BRW_VARYING_SLOT_NDC on gen4-5
    * - VARYING_SLOT_{PSIZ,LAYER} and VARYING_SLOT_POS on gen6+
    */

   bool fs_needs_vue_header = brw->fragment_program->info.inputs_read &
      (VARYING_BIT_LAYER | VARYING_BIT_VIEWPORT);

   *urb_entry_read_offset = fs_needs_vue_header ? 0 : 1;

   /* From the Ivybridge PRM, Vol 2 Part 1, 3DSTATE_SBE,
    * description of dw10 Point Sprite Texture Coordinate Enable:
    *
    * "This field must be programmed to zero when non-point primitives
    * are rendered."
    *
    * The SandyBridge PRM doesn't explicitly say that point sprite enables
    * must be programmed to zero when rendering non-point primitives, but
    * the IvyBridge PRM does, and if we don't, we get garbage.
    *
    * This is not required on Haswell, as the hardware ignores this state
    * when drawing non-points -- although we do still need to be careful to
    * correctly set the attr overrides.
    *
    * _NEW_POLYGON
    * BRW_NEW_PRIMITIVE | BRW_NEW_GS_PROG_DATA | BRW_NEW_TES_PROG_DATA
    */
   bool drawing_points = brw_is_drawing_points(brw);

   for (int attr = 0; attr < VARYING_SLOT_MAX; attr++) {
      int input_index = wm_prog_data->urb_setup[attr];

      if (input_index < 0)
         continue;

      /* _NEW_POINT */
      bool point_sprite = false;
      if (drawing_points) {
         if (point->PointSprite &&
             (attr >= VARYING_SLOT_TEX0 && attr <= VARYING_SLOT_TEX7) &&
             (point->CoordReplace & (1u << (attr - VARYING_SLOT_TEX0)))) {
            point_sprite = true;
         }

         if (attr == VARYING_SLOT_PNTC)
            point_sprite = true;

         if (point_sprite)
            *point_sprite_enables |= (1 << input_index);
      }

      /* BRW_NEW_VUE_MAP_GEOM_OUT | _NEW_LIGHT | _NEW_PROGRAM */
      struct GENX(SF_OUTPUT_ATTRIBUTE_DETAIL) attribute = { 0 };

      if (!point_sprite) {
         genX(get_attr_override)(&attribute,
                                 &brw->vue_map_geom_out,
                                 *urb_entry_read_offset, attr,
                                 brw->ctx.VertexProgram._TwoSideEnabled,
                                 &max_source_attr);
      }

      /* The hardware can only do the overrides on 16 overrides at a
       * time, and the other up to 16 have to be lined up so that the
       * input index = the output index.  We'll need to do some
       * tweaking to make sure that's the case.
       */
      if (input_index < 16)
         attr_overrides[input_index] = attribute;
      else
         assert(attribute.SourceAttribute == input_index);
   }

   /* From the Sandy Bridge PRM, Volume 2, Part 1, documentation for
    * 3DSTATE_SF DWord 1 bits 15:11, "Vertex URB Entry Read Length":
    *
    * "This field should be set to the minimum length required to read the
    *  maximum source attribute.  The maximum source attribute is indicated
    *  by the maximum value of the enabled Attribute # Source Attribute if
    *  Attribute Swizzle Enable is set, Number of Output Attributes-1 if
    *  enable is not set.
    *  read_length = ceiling((max_source_attr + 1) / 2)
    *
    *  [errata] Corruption/Hang possible if length programmed larger than
    *  recommended"
    *
    * Similar text exists for Ivy Bridge.
    */
   *urb_entry_read_length = DIV_ROUND_UP(max_source_attr + 1, 2);
}

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

/* ---------------------------------------------------------------------- */

static void
genX(upload_sf)(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   float point_size;

#if GEN_GEN <= 7
   /* _NEW_BUFFERS */
   bool render_to_fbo = _mesa_is_user_fbo(ctx->DrawBuffer);
   const bool multisampled_fbo = _mesa_geometric_samples(ctx->DrawBuffer) > 1;
#endif

   brw_batch_emit(brw, GENX(3DSTATE_SF), sf) {
      sf.StatisticsEnable = true;
      sf.ViewportTransformEnable = brw->sf.viewport_transform_enable;

#if GEN_GEN == 7
      /* _NEW_BUFFERS */
      sf.DepthBufferSurfaceFormat = brw_depthbuffer_format(brw);
#endif

#if GEN_GEN <= 7
      /* _NEW_POLYGON */
      sf.FrontWinding = ctx->Polygon._FrontBit == render_to_fbo;
      sf.GlobalDepthOffsetEnableSolid = ctx->Polygon.OffsetFill;
      sf.GlobalDepthOffsetEnableWireframe = ctx->Polygon.OffsetLine;
      sf.GlobalDepthOffsetEnablePoint = ctx->Polygon.OffsetPoint;

      switch (ctx->Polygon.FrontMode) {
         case GL_FILL:
            sf.FrontFaceFillMode = FILL_MODE_SOLID;
            break;
         case GL_LINE:
            sf.FrontFaceFillMode = FILL_MODE_WIREFRAME;
            break;
         case GL_POINT:
            sf.FrontFaceFillMode = FILL_MODE_POINT;
            break;
         default:
            unreachable("not reached");
      }

      switch (ctx->Polygon.BackMode) {
         case GL_FILL:
            sf.BackFaceFillMode = FILL_MODE_SOLID;
            break;
         case GL_LINE:
            sf.BackFaceFillMode = FILL_MODE_WIREFRAME;
            break;
         case GL_POINT:
            sf.BackFaceFillMode = FILL_MODE_POINT;
            break;
         default:
            unreachable("not reached");
      }

      sf.ScissorRectangleEnable = true;

      if (ctx->Polygon.CullFlag) {
         switch (ctx->Polygon.CullFaceMode) {
            case GL_FRONT:
               sf.CullMode = CULLMODE_FRONT;
               break;
            case GL_BACK:
               sf.CullMode = CULLMODE_BACK;
               break;
            case GL_FRONT_AND_BACK:
               sf.CullMode = CULLMODE_BOTH;
               break;
            default:
               unreachable("not reached");
         }
      } else {
         sf.CullMode = CULLMODE_NONE;
      }

#if GEN_IS_HASWELL
      sf.LineStippleEnable = ctx->Line.StippleFlag;
#endif

      if (multisampled_fbo && ctx->Multisample.Enabled)
         sf.MultisampleRasterizationMode = MSRASTMODE_ON_PATTERN;

      sf.GlobalDepthOffsetConstant = ctx->Polygon.OffsetUnits * 2;
      sf.GlobalDepthOffsetScale = ctx->Polygon.OffsetFactor;
      sf.GlobalDepthOffsetClamp = ctx->Polygon.OffsetClamp;
#endif

      /* _NEW_LINE */
      sf.LineWidth = brw_get_line_width_float(brw);

      if (ctx->Line.SmoothFlag) {
         sf.LineEndCapAntialiasingRegionWidth = _10pixels;
#if GEN_GEN <= 7
         sf.AntiAliasingEnable = true;
#endif
      }

      /* _NEW_POINT - Clamp to ARB_point_parameters user limits */
      point_size = CLAMP(ctx->Point.Size, ctx->Point.MinSize, ctx->Point.MaxSize);
      /* Clamp to the hardware limits */
      sf.PointWidth = CLAMP(point_size, 0.125f, 255.875f);

      /* _NEW_PROGRAM | _NEW_POINT, BRW_NEW_VUE_MAP_GEOM_OUT */
      if (use_state_point_size(brw))
         sf.PointWidthSource = State;

#if GEN_GEN >= 8
      /* _NEW_POINT | _NEW_MULTISAMPLE */
      if ((ctx->Point.SmoothFlag || _mesa_is_multisample_enabled(ctx)) &&
          !ctx->Point.PointSprite)
         sf.SmoothPointEnable = true;
#endif

      sf.AALineDistanceMode = AALINEDISTANCE_TRUE;

      /* _NEW_LIGHT */
      if (ctx->Light.ProvokingVertex != GL_FIRST_VERTEX_CONVENTION) {
         sf.TriangleStripListProvokingVertexSelect = 2;
         sf.TriangleFanProvokingVertexSelect = 2;
         sf.LineStripListProvokingVertexSelect = 1;
      } else {
         sf.TriangleFanProvokingVertexSelect = 1;
      }

#if GEN_GEN == 6
      /* BRW_NEW_FS_PROG_DATA */
      const struct brw_wm_prog_data *wm_prog_data =
         brw_wm_prog_data(brw->wm.base.prog_data);

      sf.AttributeSwizzleEnable = true;
      sf.NumberofSFOutputAttributes = wm_prog_data->num_varying_inputs;

      /*
       * Window coordinates in an FBO are inverted, which means point
       * sprite origin must be inverted, too.
       */
      if ((ctx->Point.SpriteOrigin == GL_LOWER_LEFT) != render_to_fbo) {
         sf.PointSpriteTextureCoordinateOrigin = LOWERLEFT;
      } else {
         sf.PointSpriteTextureCoordinateOrigin = UPPERLEFT;
      }

      /* BRW_NEW_VUE_MAP_GEOM_OUT | BRW_NEW_FRAGMENT_PROGRAM |
       * _NEW_POINT | _NEW_LIGHT | _NEW_PROGRAM | BRW_NEW_FS_PROG_DATA
       */
      uint32_t urb_entry_read_length;
      uint32_t urb_entry_read_offset;
      uint32_t point_sprite_enables;
      genX(calculate_attr_overrides)(brw, sf.Attribute, &point_sprite_enables,
                                     &urb_entry_read_length,
                                     &urb_entry_read_offset);
      sf.VertexURBEntryReadLength = urb_entry_read_length;
      sf.VertexURBEntryReadOffset = urb_entry_read_offset;
      sf.PointSpriteTextureCoordinateEnable = point_sprite_enables;
      sf.ConstantInterpolationEnable = wm_prog_data->flat_inputs;
#endif
   }
}

static const struct brw_tracked_state genX(sf_state) = {
   .dirty = {
      .mesa  = _NEW_LIGHT |
               _NEW_LINE |
               _NEW_MULTISAMPLE |
               _NEW_POINT |
               _NEW_PROGRAM |
               (GEN_GEN <= 7 ? _NEW_BUFFERS | _NEW_POLYGON : 0),
      .brw   = BRW_NEW_BLORP |
               BRW_NEW_CONTEXT |
               BRW_NEW_VUE_MAP_GEOM_OUT |
               (GEN_GEN <= 7 ? BRW_NEW_GS_PROG_DATA |
                               BRW_NEW_PRIMITIVE |
                               BRW_NEW_TES_PROG_DATA
                             : 0) |
               (GEN_GEN == 6 ? BRW_NEW_FS_PROG_DATA |
                               BRW_NEW_FRAGMENT_PROGRAM
                             : 0),
   },
   .emit = genX(upload_sf),
};

/* ---------------------------------------------------------------------- */

static void
genX(upload_wm)(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;

   /* BRW_NEW_FS_PROG_DATA */
   const struct brw_wm_prog_data *wm_prog_data =
      brw_wm_prog_data(brw->wm.base.prog_data);

   UNUSED bool writes_depth =
      wm_prog_data->computed_depth_mode != BRW_PSCDEPTH_OFF;

#if GEN_GEN < 7
   const struct brw_stage_state *stage_state = &brw->wm.base;
   const struct gen_device_info *devinfo = &brw->screen->devinfo;

   /* We can't fold this into gen6_upload_wm_push_constants(), because
    * according to the SNB PRM, vol 2 part 1 section 7.2.2
    * (3DSTATE_CONSTANT_PS [DevSNB]):
    *
    *     "[DevSNB]: This packet must be followed by WM_STATE."
    */
   brw_batch_emit(brw, GENX(3DSTATE_CONSTANT_PS), wmcp) {
      if (wm_prog_data->base.nr_params != 0) {
         wmcp.Buffer0Valid = true;
         /* Pointer to the WM constant buffer.  Covered by the set of
          * state flags from gen6_upload_wm_push_constants.
          */
         wmcp.PointertoPSConstantBuffer0 = stage_state->push_const_offset;
         wmcp.PSConstantBuffer0ReadLength = stage_state->push_const_size - 1;
      }
   }
#endif

   brw_batch_emit(brw, GENX(3DSTATE_WM), wm) {
      wm.StatisticsEnable = true;
      wm.LineAntialiasingRegionWidth = _10pixels;
      wm.LineEndCapAntialiasingRegionWidth = _05pixels;

#if GEN_GEN < 7
      if (wm_prog_data->base.use_alt_mode)
         wm.FloatingPointMode = Alternate;

      wm.SamplerCount = DIV_ROUND_UP(stage_state->sampler_count, 4);
      wm.BindingTableEntryCount = wm_prog_data->base.binding_table.size_bytes / 4;
      wm.MaximumNumberofThreads = devinfo->max_wm_threads - 1;
      wm._8PixelDispatchEnable = wm_prog_data->dispatch_8;
      wm._16PixelDispatchEnable = wm_prog_data->dispatch_16;
      wm.DispatchGRFStartRegisterForConstantSetupData0 =
         wm_prog_data->base.dispatch_grf_start_reg;
      wm.DispatchGRFStartRegisterForConstantSetupData2 =
         wm_prog_data->dispatch_grf_start_reg_2;
      wm.KernelStartPointer0 = stage_state->prog_offset;
      wm.KernelStartPointer2 = stage_state->prog_offset +
         wm_prog_data->prog_offset_2;
      wm.DualSourceBlendEnable =
         wm_prog_data->dual_src_blend && (ctx->Color.BlendEnabled & 1) &&
         ctx->Color.Blend[0]._UsesDualSrc;
      wm.oMaskPresenttoRenderTarget = wm_prog_data->uses_omask;
      wm.NumberofSFOutputAttributes = wm_prog_data->num_varying_inputs;

      /* From the SNB PRM, volume 2 part 1, page 281:
       * "If the PS kernel does not need the Position XY Offsets
       * to compute a Position XY value, then this field should be
       * programmed to POSOFFSET_NONE."
       *
       * "SW Recommendation: If the PS kernel needs the Position Offsets
       * to compute a Position XY value, this field should match Position
       * ZW Interpolation Mode to ensure a consistent position.xyzw
       * computation."
       * We only require XY sample offsets. So, this recommendation doesn't
       * look useful at the moment. We might need this in future.
       */
      if (wm_prog_data->uses_pos_offset)
         wm.PositionXYOffsetSelect = POSOFFSET_SAMPLE;
      else
         wm.PositionXYOffsetSelect = POSOFFSET_NONE;

      if (wm_prog_data->base.total_scratch) {
         wm.ScratchSpaceBasePointer =
            render_bo(stage_state->scratch_bo,
                      ffs(stage_state->per_thread_scratch) - 11);
      }

      wm.PixelShaderComputedDepth = writes_depth;
#endif

      wm.PointRasterizationRule = RASTRULE_UPPER_RIGHT;

      /* _NEW_LINE */
      wm.LineStippleEnable = ctx->Line.StippleFlag;

      /* _NEW_POLYGON */
      wm.PolygonStippleEnable = ctx->Polygon.StippleFlag;
      wm.BarycentricInterpolationMode = wm_prog_data->barycentric_interp_modes;

#if GEN_GEN < 8
      /* _NEW_BUFFERS */
      const bool multisampled_fbo = _mesa_geometric_samples(ctx->DrawBuffer) > 1;

      wm.PixelShaderUsesSourceDepth = wm_prog_data->uses_src_depth;
      wm.PixelShaderUsesSourceW = wm_prog_data->uses_src_w;
      if (wm_prog_data->uses_kill ||
          _mesa_is_alpha_test_enabled(ctx) ||
          _mesa_is_alpha_to_coverage_enabled(ctx) ||
          wm_prog_data->uses_omask) {
         wm.PixelShaderKillsPixel = true;
      }

      /* _NEW_BUFFERS | _NEW_COLOR */
      if (brw_color_buffer_write_enabled(brw) || writes_depth ||
          wm_prog_data->has_side_effects || wm.PixelShaderKillsPixel) {
         wm.ThreadDispatchEnable = true;
      }
      if (multisampled_fbo) {
         /* _NEW_MULTISAMPLE */
         if (ctx->Multisample.Enabled)
            wm.MultisampleRasterizationMode = MSRASTMODE_ON_PATTERN;
         else
            wm.MultisampleRasterizationMode = MSRASTMODE_OFF_PIXEL;

         if (wm_prog_data->persample_dispatch)
            wm.MultisampleDispatchMode = MSDISPMODE_PERSAMPLE;
         else
            wm.MultisampleDispatchMode = MSDISPMODE_PERPIXEL;
      } else {
         wm.MultisampleRasterizationMode = MSRASTMODE_OFF_PIXEL;
         wm.MultisampleDispatchMode = MSDISPMODE_PERSAMPLE;
      }

#if GEN_GEN >= 7
      wm.PixelShaderComputedDepthMode = wm_prog_data->computed_depth_mode;
      wm.PixelShaderUsesInputCoverageMask = wm_prog_data->uses_sample_mask;
#endif

      /* The "UAV access enable" bits are unnecessary on HSW because they only
       * seem to have an effect on the HW-assisted coherency mechanism which we
       * don't need, and the rasterization-related UAV_ONLY flag and the
       * DISPATCH_ENABLE bit can be set independently from it.
       * C.f. gen8_upload_ps_extra().
       *
       * BRW_NEW_FRAGMENT_PROGRAM | BRW_NEW_FS_PROG_DATA | _NEW_BUFFERS |
       * _NEW_COLOR
       */
#if GEN_IS_HASWELL
      if (!(brw_color_buffer_write_enabled(brw) || writes_depth) &&
          wm_prog_data->has_side_effects)
         wm.PSUAVonly = ON;
#endif
#endif

#if GEN_GEN >= 7
      /* BRW_NEW_FS_PROG_DATA */
      if (wm_prog_data->early_fragment_tests)
         wm.EarlyDepthStencilControl = EDSC_PREPS;
      else if (wm_prog_data->has_side_effects)
         wm.EarlyDepthStencilControl = EDSC_PSEXEC;
#endif
   }
}

static const struct brw_tracked_state genX(wm_state) = {
   .dirty = {
      .mesa  = _NEW_LINE |
               _NEW_POLYGON |
               (GEN_GEN < 8 ? _NEW_BUFFERS |
                              _NEW_COLOR |
                              _NEW_MULTISAMPLE :
                              0) |
               (GEN_GEN < 7 ? _NEW_PROGRAM_CONSTANTS : 0),
      .brw   = BRW_NEW_BLORP |
               BRW_NEW_FS_PROG_DATA |
               (GEN_GEN < 7 ? BRW_NEW_PUSH_CONSTANT_ALLOCATION |
                              BRW_NEW_BATCH
                            : BRW_NEW_CONTEXT),
   },
   .emit = genX(upload_wm),
};

/* ---------------------------------------------------------------------- */

#define INIT_THREAD_DISPATCH_FIELDS(pkt, prefix) \
   pkt.KernelStartPointer = stage_state->prog_offset;                     \
   pkt.SamplerCount       =                                               \
      DIV_ROUND_UP(CLAMP(stage_state->sampler_count, 0, 16), 4);          \
   pkt.BindingTableEntryCount =                                           \
      stage_prog_data->binding_table.size_bytes / 4;                      \
   pkt.FloatingPointMode  = stage_prog_data->use_alt_mode;                \
                                                                          \
   if (stage_prog_data->total_scratch) {                                  \
      pkt.ScratchSpaceBasePointer =                                       \
         render_bo(stage_state->scratch_bo, 0);                           \
      pkt.PerThreadScratchSpace =                                         \
         ffs(stage_state->per_thread_scratch) - 11;                       \
   }                                                                      \
                                                                          \
   pkt.DispatchGRFStartRegisterForURBData =                               \
      stage_prog_data->dispatch_grf_start_reg;                            \
   pkt.prefix##URBEntryReadLength = vue_prog_data->urb_read_length;       \
   pkt.prefix##URBEntryReadOffset = 0;                                    \
                                                                          \
   pkt.StatisticsEnable = true;                                           \
   pkt.Enable           = true;


static void
genX(upload_vs_state)(struct brw_context *brw)
{
   const struct gen_device_info *devinfo = &brw->screen->devinfo;
   const struct brw_stage_state *stage_state = &brw->vs.base;

   /* BRW_NEW_VS_PROG_DATA */
   const struct brw_vue_prog_data *vue_prog_data =
      brw_vue_prog_data(brw->vs.base.prog_data);
   const struct brw_stage_prog_data *stage_prog_data = &vue_prog_data->base;

   assert(vue_prog_data->dispatch_mode == DISPATCH_MODE_SIMD8 ||
          vue_prog_data->dispatch_mode == DISPATCH_MODE_4X2_DUAL_OBJECT);

   /* From the BSpec, 3D Pipeline > Geometry > Vertex Shader > State,
    * 3DSTATE_VS, Dword 5.0 "VS Function Enable":
    *
    *   [DevSNB] A pipeline flush must be programmed prior to a 3DSTATE_VS
    *   command that causes the VS Function Enable to toggle. Pipeline
    *   flush can be executed by sending a PIPE_CONTROL command with CS
    *   stall bit set and a post sync operation.
    *
    * We've already done such a flush at the start of state upload, so we
    * don't need to do another one here.
    */

#if GEN_GEN < 7
   brw_batch_emit(brw, GENX(3DSTATE_CONSTANT_VS), cvs) {
      if (stage_state->push_const_size != 0) {
         cvs.Buffer0Valid = true;
         cvs.PointertoVSConstantBuffer0 = stage_state->push_const_offset;
         cvs.VSConstantBuffer0ReadLength = stage_state->push_const_size - 1;
      }
   }
#endif

   if (GEN_GEN == 7 && devinfo->is_ivybridge)
      gen7_emit_vs_workaround_flush(brw);

   brw_batch_emit(brw, GENX(3DSTATE_VS), vs) {
      INIT_THREAD_DISPATCH_FIELDS(vs, Vertex);

      vs.MaximumNumberofThreads = devinfo->max_vs_threads - 1;

#if GEN_GEN >= 8
      vs.SIMD8DispatchEnable =
         vue_prog_data->dispatch_mode == DISPATCH_MODE_SIMD8;

      vs.UserClipDistanceCullTestEnableBitmask =
         vue_prog_data->cull_distance_mask;
#endif
   }

#if GEN_GEN < 7
   /* Based on my reading of the simulator, the VS constants don't get
    * pulled into the VS FF unit until an appropriate pipeline flush
    * happens, and instead the 3DSTATE_CONSTANT_VS packet just adds
    * references to them into a little FIFO.  The flushes are common,
    * but don't reliably happen between this and a 3DPRIMITIVE, causing
    * the primitive to use the wrong constants.  Then the FIFO
    * containing the constant setup gets added to again on the next
    * constants change, and eventually when a flush does happen the
    * unit is overwhelmed by constant changes and dies.
    *
    * To avoid this, send a PIPE_CONTROL down the line that will
    * update the unit immediately loading the constants.  The flush
    * type bits here were those set by the STATE_BASE_ADDRESS whose
    * move in a82a43e8d99e1715dd11c9c091b5ab734079b6a6 triggered the
    * bug reports that led to this workaround, and may be more than
    * what is strictly required to avoid the issue.
    */
   brw_emit_pipe_control_flush(brw,
                               PIPE_CONTROL_DEPTH_STALL |
                               PIPE_CONTROL_INSTRUCTION_INVALIDATE |
                               PIPE_CONTROL_STATE_CACHE_INVALIDATE);
#endif
}

static const struct brw_tracked_state genX(vs_state) = {
   .dirty = {
      .mesa  = (GEN_GEN < 7 ? (_NEW_PROGRAM_CONSTANTS | _NEW_TRANSFORM) : 0),
      .brw   = BRW_NEW_BATCH |
               BRW_NEW_BLORP |
               BRW_NEW_CONTEXT |
               BRW_NEW_VS_PROG_DATA |
               (GEN_GEN < 7 ? BRW_NEW_PUSH_CONSTANT_ALLOCATION |
                              BRW_NEW_VERTEX_PROGRAM
                            : 0),
   },
   .emit = genX(upload_vs_state),
};

#endif

/* ---------------------------------------------------------------------- */

#if GEN_GEN >= 7
static void
genX(upload_sbe)(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   /* BRW_NEW_FS_PROG_DATA */
   const struct brw_wm_prog_data *wm_prog_data =
      brw_wm_prog_data(brw->wm.base.prog_data);
#if GEN_GEN >= 8
   struct GENX(SF_OUTPUT_ATTRIBUTE_DETAIL) attr_overrides[16] = { { 0 } };
#else
#define attr_overrides sbe.Attribute
#endif
   uint32_t urb_entry_read_length;
   uint32_t urb_entry_read_offset;
   uint32_t point_sprite_enables;

   brw_batch_emit(brw, GENX(3DSTATE_SBE), sbe) {
      sbe.AttributeSwizzleEnable = true;
      sbe.NumberofSFOutputAttributes = wm_prog_data->num_varying_inputs;

      /* _NEW_BUFFERS */
      bool render_to_fbo = _mesa_is_user_fbo(ctx->DrawBuffer);

      /* _NEW_POINT
       *
       * Window coordinates in an FBO are inverted, which means point
       * sprite origin must be inverted.
       */
      if ((ctx->Point.SpriteOrigin == GL_LOWER_LEFT) != render_to_fbo)
         sbe.PointSpriteTextureCoordinateOrigin = LOWERLEFT;
      else
         sbe.PointSpriteTextureCoordinateOrigin = UPPERLEFT;

      /* _NEW_POINT | _NEW_LIGHT | _NEW_PROGRAM,
       * BRW_NEW_FS_PROG_DATA | BRW_NEW_FRAGMENT_PROGRAM |
       * BRW_NEW_GS_PROG_DATA | BRW_NEW_PRIMITIVE | BRW_NEW_TES_PROG_DATA |
       * BRW_NEW_VUE_MAP_GEOM_OUT
       */
      genX(calculate_attr_overrides)(brw,
                                     attr_overrides,
                                     &point_sprite_enables,
                                     &urb_entry_read_length,
                                     &urb_entry_read_offset);

      /* Typically, the URB entry read length and offset should be programmed
       * in 3DSTATE_VS and 3DSTATE_GS; SBE inherits it from the last active
       * stage which produces geometry.  However, we don't know the proper
       * value until we call calculate_attr_overrides().
       *
       * To fit with our existing code, we override the inherited values and
       * specify it here directly, as we did on previous generations.
       */
      sbe.VertexURBEntryReadLength = urb_entry_read_length;
      sbe.VertexURBEntryReadOffset = urb_entry_read_offset;
      sbe.PointSpriteTextureCoordinateEnable = point_sprite_enables;
      sbe.ConstantInterpolationEnable = wm_prog_data->flat_inputs;

#if GEN_GEN >= 8
      sbe.ForceVertexURBEntryReadLength = true;
      sbe.ForceVertexURBEntryReadOffset = true;
#endif

#if GEN_GEN >= 9
      /* prepare the active component dwords */
      int input_index = 0;
      for (int attr = 0; attr < VARYING_SLOT_MAX; attr++) {
         if (!(brw->fragment_program->info.inputs_read &
               BITFIELD64_BIT(attr))) {
            continue;
         }

         assert(input_index < 32);

         sbe.AttributeActiveComponentFormat[input_index] = ACTIVE_COMPONENT_XYZW;
         ++input_index;
      }
#endif
   }

#if GEN_GEN >= 8
   brw_batch_emit(brw, GENX(3DSTATE_SBE_SWIZ), sbes) {
      for (int i = 0; i < 16; i++)
         sbes.Attribute[i] = attr_overrides[i];
   }
#endif

#undef attr_overrides
}

static const struct brw_tracked_state genX(sbe_state) = {
   .dirty = {
      .mesa  = _NEW_BUFFERS |
               _NEW_LIGHT |
               _NEW_POINT |
               _NEW_POLYGON |
               _NEW_PROGRAM,
      .brw   = BRW_NEW_BLORP |
               BRW_NEW_CONTEXT |
               BRW_NEW_FRAGMENT_PROGRAM |
               BRW_NEW_FS_PROG_DATA |
               BRW_NEW_GS_PROG_DATA |
               BRW_NEW_TES_PROG_DATA |
               BRW_NEW_VUE_MAP_GEOM_OUT |
               (GEN_GEN == 7 ? BRW_NEW_PRIMITIVE
                             : 0),
   },
   .emit = genX(upload_sbe),
};

/* ---------------------------------------------------------------------- */

/**
 * Outputs the 3DSTATE_SO_DECL_LIST command.
 *
 * The data output is a series of 64-bit entries containing a SO_DECL per
 * stream.  We only have one stream of rendering coming out of the GS unit, so
 * we only emit stream 0 (low 16 bits) SO_DECLs.
 */
static void
genX(upload_3dstate_so_decl_list)(struct brw_context *brw,
                                  const struct brw_vue_map *vue_map)
{
   struct gl_context *ctx = &brw->ctx;
   /* BRW_NEW_TRANSFORM_FEEDBACK */
   struct gl_transform_feedback_object *xfb_obj =
      ctx->TransformFeedback.CurrentObject;
   const struct gl_transform_feedback_info *linked_xfb_info =
      xfb_obj->program->sh.LinkedTransformFeedback;
   struct GENX(SO_DECL) so_decl[MAX_VERTEX_STREAMS][128];
   int buffer_mask[MAX_VERTEX_STREAMS] = {0, 0, 0, 0};
   int next_offset[MAX_VERTEX_STREAMS] = {0, 0, 0, 0};
   int decls[MAX_VERTEX_STREAMS] = {0, 0, 0, 0};
   int max_decls = 0;
   STATIC_ASSERT(ARRAY_SIZE(so_decl[0]) >= MAX_PROGRAM_OUTPUTS);

   memset(so_decl, 0, sizeof(so_decl));

   /* Construct the list of SO_DECLs to be emitted.  The formatting of the
    * command feels strange -- each dword pair contains a SO_DECL per stream.
    */
   for (unsigned i = 0; i < linked_xfb_info->NumOutputs; i++) {
      int buffer = linked_xfb_info->Outputs[i].OutputBuffer;
      struct GENX(SO_DECL) decl = {0};
      int varying = linked_xfb_info->Outputs[i].OutputRegister;
      const unsigned components = linked_xfb_info->Outputs[i].NumComponents;
      unsigned component_mask = (1 << components) - 1;
      unsigned stream_id = linked_xfb_info->Outputs[i].StreamId;
      unsigned decl_buffer_slot = buffer;
      assert(stream_id < MAX_VERTEX_STREAMS);

      /* gl_PointSize is stored in VARYING_SLOT_PSIZ.w
       * gl_Layer is stored in VARYING_SLOT_PSIZ.y
       * gl_ViewportIndex is stored in VARYING_SLOT_PSIZ.z
       */
      if (varying == VARYING_SLOT_PSIZ) {
         assert(components == 1);
         component_mask <<= 3;
      } else if (varying == VARYING_SLOT_LAYER) {
         assert(components == 1);
         component_mask <<= 1;
      } else if (varying == VARYING_SLOT_VIEWPORT) {
         assert(components == 1);
         component_mask <<= 2;
      } else {
         component_mask <<= linked_xfb_info->Outputs[i].ComponentOffset;
      }

      buffer_mask[stream_id] |= 1 << buffer;

      decl.OutputBufferSlot = decl_buffer_slot;
      if (varying == VARYING_SLOT_LAYER || varying == VARYING_SLOT_VIEWPORT) {
         decl.RegisterIndex = vue_map->varying_to_slot[VARYING_SLOT_PSIZ];
      } else {
         assert(vue_map->varying_to_slot[varying] >= 0);
         decl.RegisterIndex = vue_map->varying_to_slot[varying];
      }
      decl.ComponentMask = component_mask;

      /* Mesa doesn't store entries for gl_SkipComponents in the Outputs[]
       * array.  Instead, it simply increments DstOffset for the following
       * input by the number of components that should be skipped.
       *
       * Our hardware is unusual in that it requires us to program SO_DECLs
       * for fake "hole" components, rather than simply taking the offset
       * for each real varying.  Each hole can have size 1, 2, 3, or 4; we
       * program as many size = 4 holes as we can, then a final hole to
       * accommodate the final 1, 2, or 3 remaining.
       */
      int skip_components =
         linked_xfb_info->Outputs[i].DstOffset - next_offset[buffer];

      next_offset[buffer] += skip_components;

      while (skip_components >= 4) {
         struct GENX(SO_DECL) *d = &so_decl[stream_id][decls[stream_id]++];
         d->HoleFlag = 1;
         d->OutputBufferSlot = decl_buffer_slot;
         d->ComponentMask = 0xf;
         skip_components -= 4;
      }

      if (skip_components > 0) {
         struct GENX(SO_DECL) *d = &so_decl[stream_id][decls[stream_id]++];
         d->HoleFlag = 1;
         d->OutputBufferSlot = decl_buffer_slot;
         d->ComponentMask = (1 << skip_components) - 1;
      }

      assert(linked_xfb_info->Outputs[i].DstOffset == next_offset[buffer]);

      next_offset[buffer] += components;

      so_decl[stream_id][decls[stream_id]++] = decl;

      if (decls[stream_id] > max_decls)
         max_decls = decls[stream_id];
   }

   uint32_t *dw;
   dw = brw_batch_emitn(brw, GENX(3DSTATE_SO_DECL_LIST), 3 + 2 * max_decls,
                        .StreamtoBufferSelects0 = buffer_mask[0],
                        .StreamtoBufferSelects1 = buffer_mask[1],
                        .StreamtoBufferSelects2 = buffer_mask[2],
                        .StreamtoBufferSelects3 = buffer_mask[3],
                        .NumEntries0 = decls[0],
                        .NumEntries1 = decls[1],
                        .NumEntries2 = decls[2],
                        .NumEntries3 = decls[3]);

   for (int i = 0; i < max_decls; i++) {
      GENX(SO_DECL_ENTRY_pack)(
         brw, dw + 2 + i * 2,
         &(struct GENX(SO_DECL_ENTRY)) {
            .Stream0Decl = so_decl[0][i],
            .Stream1Decl = so_decl[1][i],
            .Stream2Decl = so_decl[2][i],
            .Stream3Decl = so_decl[3][i],
         });
   }
}

static void
genX(upload_3dstate_so_buffers)(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   /* BRW_NEW_TRANSFORM_FEEDBACK */
   struct gl_transform_feedback_object *xfb_obj =
      ctx->TransformFeedback.CurrentObject;
#if GEN_GEN < 8
   const struct gl_transform_feedback_info *linked_xfb_info =
      xfb_obj->program->sh.LinkedTransformFeedback;
#else
   struct brw_transform_feedback_object *brw_obj =
      (struct brw_transform_feedback_object *) xfb_obj;
   uint32_t mocs_wb = brw->gen >= 9 ? SKL_MOCS_WB : BDW_MOCS_WB;
#endif

   /* Set up the up to 4 output buffers.  These are the ranges defined in the
    * gl_transform_feedback_object.
    */
   for (int i = 0; i < 4; i++) {
      struct intel_buffer_object *bufferobj =
         intel_buffer_object(xfb_obj->Buffers[i]);

      if (!bufferobj) {
         brw_batch_emit(brw, GENX(3DSTATE_SO_BUFFER), sob) {
            sob.SOBufferIndex = i;
         }
         continue;
      }

      uint32_t start = xfb_obj->Offset[i];
      assert(start % 4 == 0);
      uint32_t end = ALIGN(start + xfb_obj->Size[i], 4);
      struct brw_bo *bo =
         intel_bufferobj_buffer(brw, bufferobj, start, end - start);
      assert(end <= bo->size);

      brw_batch_emit(brw, GENX(3DSTATE_SO_BUFFER), sob) {
         sob.SOBufferIndex = i;

         sob.SurfaceBaseAddress = render_bo(bo, start);
#if GEN_GEN < 8
         sob.SurfacePitch = linked_xfb_info->Buffers[i].Stride * 4;
         sob.SurfaceEndAddress = render_bo(bo, end);
#else
         sob.SOBufferEnable = true;
         sob.StreamOffsetWriteEnable = true;
         sob.StreamOutputBufferOffsetAddressEnable = true;
         sob.SOBufferMOCS = mocs_wb;

         sob.SurfaceSize = MAX2(xfb_obj->Size[i] / 4, 1) - 1;
         sob.StreamOutputBufferOffsetAddress =
            instruction_bo(brw_obj->offset_bo, i * sizeof(uint32_t));

         if (brw_obj->zero_offsets) {
            /* Zero out the offset and write that to offset_bo */
            sob.StreamOffset = 0;
         } else {
            /* Use offset_bo as the "Stream Offset." */
            sob.StreamOffset = 0xFFFFFFFF;
         }
#endif
      }
   }

#if GEN_GEN >= 8
   brw_obj->zero_offsets = false;
#endif
}

static inline bool
query_active(struct gl_query_object *q)
{
   return q && q->Active;
}

static void
genX(upload_3dstate_streamout)(struct brw_context *brw, bool active,
                               const struct brw_vue_map *vue_map)
{
   struct gl_context *ctx = &brw->ctx;
   /* BRW_NEW_TRANSFORM_FEEDBACK */
   struct gl_transform_feedback_object *xfb_obj =
      ctx->TransformFeedback.CurrentObject;

   brw_batch_emit(brw, GENX(3DSTATE_STREAMOUT), sos) {
      if (active) {
         int urb_entry_read_offset = 0;
         int urb_entry_read_length = (vue_map->num_slots + 1) / 2 -
            urb_entry_read_offset;

         sos.SOFunctionEnable = true;
         sos.SOStatisticsEnable = true;

         /* BRW_NEW_RASTERIZER_DISCARD */
         if (ctx->RasterDiscard) {
            if (!query_active(ctx->Query.PrimitivesGenerated[0])) {
               sos.RenderingDisable = true;
            } else {
               perf_debug("Rasterizer discard with a GL_PRIMITIVES_GENERATED "
                          "query active relies on the clipper.");
            }
         }

         /* _NEW_LIGHT */
         if (ctx->Light.ProvokingVertex != GL_FIRST_VERTEX_CONVENTION)
            sos.ReorderMode = TRAILING;

#if GEN_GEN < 8
         sos.SOBufferEnable0 = xfb_obj->Buffers[0] != NULL;
         sos.SOBufferEnable1 = xfb_obj->Buffers[1] != NULL;
         sos.SOBufferEnable2 = xfb_obj->Buffers[2] != NULL;
         sos.SOBufferEnable3 = xfb_obj->Buffers[3] != NULL;
#else
         const struct gl_transform_feedback_info *linked_xfb_info =
            xfb_obj->program->sh.LinkedTransformFeedback;
         /* Set buffer pitches; 0 means unbound. */
         if (xfb_obj->Buffers[0])
            sos.Buffer0SurfacePitch = linked_xfb_info->Buffers[0].Stride * 4;
         if (xfb_obj->Buffers[1])
            sos.Buffer1SurfacePitch = linked_xfb_info->Buffers[1].Stride * 4;
         if (xfb_obj->Buffers[2])
            sos.Buffer2SurfacePitch = linked_xfb_info->Buffers[2].Stride * 4;
         if (xfb_obj->Buffers[3])
            sos.Buffer3SurfacePitch = linked_xfb_info->Buffers[3].Stride * 4;
#endif

         /* We always read the whole vertex.  This could be reduced at some
          * point by reading less and offsetting the register index in the
          * SO_DECLs.
          */
         sos.Stream0VertexReadOffset = urb_entry_read_offset;
         sos.Stream0VertexReadLength = urb_entry_read_length - 1;
         sos.Stream1VertexReadOffset = urb_entry_read_offset;
         sos.Stream1VertexReadLength = urb_entry_read_length - 1;
         sos.Stream2VertexReadOffset = urb_entry_read_offset;
         sos.Stream2VertexReadLength = urb_entry_read_length - 1;
         sos.Stream3VertexReadOffset = urb_entry_read_offset;
         sos.Stream3VertexReadLength = urb_entry_read_length - 1;
      }
   }
}

static void
genX(upload_sol)(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   /* BRW_NEW_TRANSFORM_FEEDBACK */
   bool active = _mesa_is_xfb_active_and_unpaused(ctx);

   if (active) {
      genX(upload_3dstate_so_buffers)(brw);

      /* BRW_NEW_VUE_MAP_GEOM_OUT */
      genX(upload_3dstate_so_decl_list)(brw, &brw->vue_map_geom_out);
   }

   /* Finally, set up the SOL stage.  This command must always follow updates to
    * the nonpipelined SOL state (3DSTATE_SO_BUFFER, 3DSTATE_SO_DECL_LIST) or
    * MMIO register updates (current performed by the kernel at each batch
    * emit).
    */
   genX(upload_3dstate_streamout)(brw, active, &brw->vue_map_geom_out);
}

static const struct brw_tracked_state genX(sol_state) = {
   .dirty = {
      .mesa  = _NEW_LIGHT,
      .brw   = BRW_NEW_BATCH |
               BRW_NEW_BLORP |
               BRW_NEW_RASTERIZER_DISCARD |
               BRW_NEW_VUE_MAP_GEOM_OUT |
               BRW_NEW_TRANSFORM_FEEDBACK,
   },
   .emit = genX(upload_sol),
};

/* ---------------------------------------------------------------------- */

static void
genX(upload_ps)(struct brw_context *brw)
{
   UNUSED const struct gl_context *ctx = &brw->ctx;
   UNUSED const struct gen_device_info *devinfo = &brw->screen->devinfo;

   /* BRW_NEW_FS_PROG_DATA */
   const struct brw_wm_prog_data *prog_data =
      brw_wm_prog_data(brw->wm.base.prog_data);
   const struct brw_stage_state *stage_state = &brw->wm.base;

#if GEN_GEN < 8
#endif

   brw_batch_emit(brw, GENX(3DSTATE_PS), ps) {
      /* Initialize the execution mask with VMask.  Otherwise, derivatives are
       * incorrect for subspans where some of the pixels are unlit.  We believe
       * the bit just didn't take effect in previous generations.
       */
      ps.VectorMaskEnable = GEN_GEN >= 8;

      ps.SamplerCount =
         DIV_ROUND_UP(CLAMP(stage_state->sampler_count, 0, 16), 4);

      /* BRW_NEW_FS_PROG_DATA */
      ps.BindingTableEntryCount = prog_data->base.binding_table.size_bytes / 4;

      if (prog_data->base.use_alt_mode)
         ps.FloatingPointMode = Alternate;

      /* Haswell requires the sample mask to be set in this packet as well as
       * in 3DSTATE_SAMPLE_MASK; the values should match.
       */

      /* _NEW_BUFFERS, _NEW_MULTISAMPLE */
#if GEN_IS_HASWELL
      ps.SampleMask = gen6_determine_sample_mask(brw);
#endif

      /* 3DSTATE_PS expects the number of threads per PSD, which is always 64;
       * it implicitly scales for different GT levels (which have some # of
       * PSDs).
       *
       * In Gen8 the format is U8-2 whereas in Gen9 it is U8-1.
       */
#if GEN_GEN >= 9
      ps.MaximumNumberofThreadsPerPSD = 64 - 1;
#elif GEN_GEN >= 8
      ps.MaximumNumberofThreadsPerPSD = 64 - 2;
#else
      ps.MaximumNumberofThreads = devinfo->max_wm_threads - 1;
#endif

      if (prog_data->base.nr_params > 0)
         ps.PushConstantEnable = true;

#if GEN_GEN < 8
      /* From the IVB PRM, volume 2 part 1, page 287:
       * "This bit is inserted in the PS payload header and made available to
       * the DataPort (either via the message header or via header bypass) to
       * indicate that oMask data (one or two phases) is included in Render
       * Target Write messages. If present, the oMask data is used to mask off
       * samples."
       */
      ps.oMaskPresenttoRenderTarget = prog_data->uses_omask;

      /* The hardware wedges if you have this bit set but don't turn on any
       * dual source blend factors.
       *
       * BRW_NEW_FS_PROG_DATA | _NEW_COLOR
       */
      ps.DualSourceBlendEnable = prog_data->dual_src_blend &&
                                 (ctx->Color.BlendEnabled & 1) &&
                                 ctx->Color.Blend[0]._UsesDualSrc;

      /* BRW_NEW_FS_PROG_DATA */
      ps.AttributeEnable = (prog_data->num_varying_inputs != 0);
#endif

      /* From the documentation for this packet:
       * "If the PS kernel does not need the Position XY Offsets to
       *  compute a Position Value, then this field should be programmed
       *  to POSOFFSET_NONE."
       *
       * "SW Recommendation: If the PS kernel needs the Position Offsets
       *  to compute a Position XY value, this field should match Position
       *  ZW Interpolation Mode to ensure a consistent position.xyzw
       *  computation."
       *
       * We only require XY sample offsets. So, this recommendation doesn't
       * look useful at the moment. We might need this in future.
       */
      if (prog_data->uses_pos_offset)
         ps.PositionXYOffsetSelect = POSOFFSET_SAMPLE;
      else
         ps.PositionXYOffsetSelect = POSOFFSET_NONE;

      ps.RenderTargetFastClearEnable = brw->wm.fast_clear_op;
      ps._8PixelDispatchEnable = prog_data->dispatch_8;
      ps._16PixelDispatchEnable = prog_data->dispatch_16;
      ps.DispatchGRFStartRegisterForConstantSetupData0 =
         prog_data->base.dispatch_grf_start_reg;
      ps.DispatchGRFStartRegisterForConstantSetupData2 =
         prog_data->dispatch_grf_start_reg_2;

      ps.KernelStartPointer0 = stage_state->prog_offset;
      ps.KernelStartPointer2 = stage_state->prog_offset +
         prog_data->prog_offset_2;

      if (prog_data->base.total_scratch) {
         ps.ScratchSpaceBasePointer =
            render_bo(stage_state->scratch_bo,
                      ffs(stage_state->per_thread_scratch) - 11);
      }
   }
}

static const struct brw_tracked_state genX(ps_state) = {
   .dirty = {
      .mesa  = _NEW_MULTISAMPLE |
               (GEN_GEN < 8 ? _NEW_BUFFERS |
                              _NEW_COLOR
                            : 0),
      .brw   = BRW_NEW_BATCH |
               BRW_NEW_BLORP |
               BRW_NEW_FS_PROG_DATA,
   },
   .emit = genX(upload_ps),
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

/* ---------------------------------------------------------------------- */

static void
genX(upload_ps_extra)(struct brw_context *brw)
{
   UNUSED struct gl_context *ctx = &brw->ctx;

   const struct brw_wm_prog_data *prog_data =
      brw_wm_prog_data(brw->wm.base.prog_data);

   brw_batch_emit(brw, GENX(3DSTATE_PS_EXTRA), psx) {
      psx.PixelShaderValid = true;
      psx.PixelShaderComputedDepthMode = prog_data->computed_depth_mode;
      psx.PixelShaderKillsPixel = prog_data->uses_kill;
      psx.AttributeEnable = prog_data->num_varying_inputs != 0;
      psx.PixelShaderUsesSourceDepth = prog_data->uses_src_depth;
      psx.PixelShaderUsesSourceW = prog_data->uses_src_w;
      psx.PixelShaderIsPerSample = prog_data->persample_dispatch;

      /* _NEW_MULTISAMPLE | BRW_NEW_CONSERVATIVE_RASTERIZATION */
      if (prog_data->uses_sample_mask) {
#if GEN_GEN >= 9
         if (prog_data->post_depth_coverage)
            psx.InputCoverageMaskState = ICMS_DEPTH_COVERAGE;
         else if (prog_data->inner_coverage && ctx->IntelConservativeRasterization)
            psx.InputCoverageMaskState = ICMS_INNER_CONSERVATIVE;
         else
            psx.InputCoverageMaskState = ICMS_NORMAL;
#else
         psx.PixelShaderUsesInputCoverageMask = true;
#endif
      }

      psx.oMaskPresenttoRenderTarget = prog_data->uses_omask;
#if GEN_GEN >= 9
      psx.PixelShaderPullsBary = prog_data->pulls_bary;
      psx.PixelShaderComputesStencil = prog_data->computed_stencil;
#endif

      /* The stricter cross-primitive coherency guarantees that the hardware
       * gives us with the "Accesses UAV" bit set for at least one shader stage
       * and the "UAV coherency required" bit set on the 3DPRIMITIVE command
       * are redundant within the current image, atomic counter and SSBO GL
       * APIs, which all have very loose ordering and coherency requirements
       * and generally rely on the application to insert explicit barriers when
       * a shader invocation is expected to see the memory writes performed by
       * the invocations of some previous primitive.  Regardless of the value
       * of "UAV coherency required", the "Accesses UAV" bits will implicitly
       * cause an in most cases useless DC flush when the lowermost stage with
       * the bit set finishes execution.
       *
       * It would be nice to disable it, but in some cases we can't because on
       * Gen8+ it also has an influence on rasterization via the PS UAV-only
       * signal (which could be set independently from the coherency mechanism
       * in the 3DSTATE_WM command on Gen7), and because in some cases it will
       * determine whether the hardware skips execution of the fragment shader
       * or not via the ThreadDispatchEnable signal.  However if we know that
       * GEN8_PS_BLEND_HAS_WRITEABLE_RT is going to be set and
       * GEN8_PSX_PIXEL_SHADER_NO_RT_WRITE is not set it shouldn't make any
       * difference so we may just disable it here.
       *
       * Gen8 hardware tries to compute ThreadDispatchEnable for us but doesn't
       * take into account KillPixels when no depth or stencil writes are
       * enabled.  In order for occlusion queries to work correctly with no
       * attachments, we need to force-enable here.
       *
       * BRW_NEW_FS_PROG_DATA | BRW_NEW_FRAGMENT_PROGRAM | _NEW_BUFFERS |
       * _NEW_COLOR
       */
      if ((prog_data->has_side_effects || prog_data->uses_kill) &&
          !brw_color_buffer_write_enabled(brw))
         psx.PixelShaderHasUAV = true;
   }
}

const struct brw_tracked_state genX(ps_extra) = {
   .dirty = {
      .mesa  = _NEW_BUFFERS | _NEW_COLOR,
      .brw   = BRW_NEW_BLORP |
               BRW_NEW_CONTEXT |
               BRW_NEW_FRAGMENT_PROGRAM |
               BRW_NEW_FS_PROG_DATA |
               BRW_NEW_CONSERVATIVE_RASTERIZATION,
   },
   .emit = genX(upload_ps_extra),
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

      &genX(vs_state),
      &gen6_gs_state,
      &genX(clip_state),
      &genX(sf_state),
      &genX(wm_state),

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

      &genX(vs_state),
      &gen7_hs_state,
      &gen7_te_state,
      &gen7_ds_state,
      &gen7_gs_state,
      &genX(sol_state),
      &genX(clip_state),
      &genX(sbe_state),
      &genX(sf_state),
      &genX(wm_state),
      &genX(ps_state),

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

      &genX(vs_state),
      &gen8_hs_state,
      &gen7_te_state,
      &gen8_ds_state,
      &gen8_gs_state,
      &genX(sol_state),
      &genX(clip_state),
      &genX(raster_state),
      &genX(sbe_state),
      &genX(sf_state),
      &gen8_ps_blend,
      &genX(ps_extra),
      &genX(ps_state),
      &genX(depth_stencil_state),
      &genX(wm_state),

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
