/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2012-2014 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Chia-I Wu <olv@lunarg.com>
 */

#include "genhw/genhw.h"
#include "util/u_dual_blend.h"
#include "util/u_framebuffer.h"
#include "util/u_half.h"

#include "ilo_format.h"
#include "ilo_image.h"
#include "ilo_state_3d.h"
#include "../ilo_shader.h"

static void
rasterizer_init_clip(const struct ilo_dev *dev,
                     const struct pipe_rasterizer_state *state,
                     struct ilo_rasterizer_clip *clip)
{
   uint32_t dw1, dw2, dw3;

   ILO_DEV_ASSERT(dev, 6, 8);

   dw1 = GEN6_CLIP_DW1_STATISTICS;

   if (ilo_dev_gen(dev) >= ILO_GEN(7)) {
      /*
       * From the Ivy Bridge PRM, volume 2 part 1, page 219:
       *
       *     "Workaround : Due to Hardware issue "EarlyCull" needs to be
       *      enabled only for the cases where the incoming primitive topology
       *      into the clipper guaranteed to be Trilist."
       *
       * What does this mean?
       */
      dw1 |= 0 << 19 |
             GEN7_CLIP_DW1_EARLY_CULL_ENABLE;

      if (ilo_dev_gen(dev) < ILO_GEN(8)) {
         if (state->front_ccw)
            dw1 |= GEN6_FRONTWINDING_CCW << 20;

         switch (state->cull_face) {
         case PIPE_FACE_NONE:
            dw1 |= GEN6_CULLMODE_NONE << 16;
            break;
         case PIPE_FACE_FRONT:
            dw1 |= GEN6_CULLMODE_FRONT << 16;
            break;
         case PIPE_FACE_BACK:
            dw1 |= GEN6_CULLMODE_BACK << 16;
            break;
         case PIPE_FACE_FRONT_AND_BACK:
            dw1 |= GEN6_CULLMODE_BOTH << 16;
            break;
         }
      }
   }

   dw2 = GEN6_CLIP_DW2_CLIP_ENABLE |
         GEN6_CLIP_DW2_XY_TEST_ENABLE |
         state->clip_plane_enable << GEN6_CLIP_DW2_UCP_CLIP_ENABLES__SHIFT |
         GEN6_CLIPMODE_NORMAL << 13;

   if (state->clip_halfz)
      dw2 |= GEN6_CLIP_DW2_APIMODE_D3D;
   else
      dw2 |= GEN6_CLIP_DW2_APIMODE_OGL;

   if (ilo_dev_gen(dev) < ILO_GEN(8) && state->depth_clip)
      dw2 |= GEN6_CLIP_DW2_Z_TEST_ENABLE;

   if (state->flatshade_first) {
      dw2 |= 0 << GEN6_CLIP_DW2_TRI_PROVOKE__SHIFT |
             0 << GEN6_CLIP_DW2_LINE_PROVOKE__SHIFT |
             1 << GEN6_CLIP_DW2_TRIFAN_PROVOKE__SHIFT;
   }
   else {
      dw2 |= 2 << GEN6_CLIP_DW2_TRI_PROVOKE__SHIFT |
             1 << GEN6_CLIP_DW2_LINE_PROVOKE__SHIFT |
             2 << GEN6_CLIP_DW2_TRIFAN_PROVOKE__SHIFT;
   }

   dw3 = 0x1 << GEN6_CLIP_DW3_MIN_POINT_WIDTH__SHIFT |
         0x7ff << GEN6_CLIP_DW3_MAX_POINT_WIDTH__SHIFT;

   clip->payload[0] = dw1;
   clip->payload[1] = dw2;
   clip->payload[2] = dw3;

   clip->can_enable_guardband = true;

   /*
    * There are several reasons that guard band test should be disabled
    *
    *  - GL wide points (to avoid partially visibie object)
    *  - GL wide or AA lines (to avoid partially visibie object)
    */
   if (state->point_size_per_vertex || state->point_size > 1.0f)
      clip->can_enable_guardband = false;
   if (state->line_smooth || state->line_width > 1.0f)
      clip->can_enable_guardband = false;
}

static void
rasterizer_init_sf_depth_offset_gen6(const struct ilo_dev *dev,
                                     const struct pipe_rasterizer_state *state,
                                     struct ilo_rasterizer_sf *sf)
{
   ILO_DEV_ASSERT(dev, 6, 8);

   /*
    * Scale the constant term.  The minimum representable value used by the HW
    * is not large enouch to be the minimum resolvable difference.
    */
   sf->dw_depth_offset_const = fui(state->offset_units * 2.0f);
   sf->dw_depth_offset_scale = fui(state->offset_scale);
   sf->dw_depth_offset_clamp = fui(state->offset_clamp);
}

static void
rasterizer_init_sf_gen6(const struct ilo_dev *dev,
                        const struct pipe_rasterizer_state *state,
                        struct ilo_rasterizer_sf *sf)
{
   int line_width, point_width;
   uint32_t dw1, dw2, dw3;

   ILO_DEV_ASSERT(dev, 6, 7.5);

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 248:
    *
    *     "This bit (Statistics Enable) should be set whenever clipping is
    *      enabled and the Statistics Enable bit is set in CLIP_STATE. It
    *      should be cleared if clipping is disabled or Statistics Enable in
    *      CLIP_STATE is clear."
    */
   dw1 = GEN7_SF_DW1_STATISTICS |
         GEN7_SF_DW1_VIEWPORT_TRANSFORM;

   /* XXX GEN6 path seems to work fine for GEN7 */
   if (false && ilo_dev_gen(dev) >= ILO_GEN(7)) {
      /*
       * From the Ivy Bridge PRM, volume 2 part 1, page 258:
       *
       *     "This bit (Legacy Global Depth Bias Enable, Global Depth Offset
       *      Enable Solid , Global Depth Offset Enable Wireframe, and Global
       *      Depth Offset Enable Point) should be set whenever non zero depth
       *      bias (Slope, Bias) values are used. Setting this bit may have
       *      some degradation of performance for some workloads."
       */
      if (state->offset_tri || state->offset_line || state->offset_point) {
         /* XXX need to scale offset_const according to the depth format */
         dw1 |= GEN7_SF_DW1_LEGACY_DEPTH_OFFSET;

         dw1 |= GEN7_SF_DW1_DEPTH_OFFSET_SOLID |
                GEN7_SF_DW1_DEPTH_OFFSET_WIREFRAME |
                GEN7_SF_DW1_DEPTH_OFFSET_POINT;
      }
   } else {
      if (state->offset_tri)
         dw1 |= GEN7_SF_DW1_DEPTH_OFFSET_SOLID;
      if (state->offset_line)
         dw1 |= GEN7_SF_DW1_DEPTH_OFFSET_WIREFRAME;
      if (state->offset_point)
         dw1 |= GEN7_SF_DW1_DEPTH_OFFSET_POINT;
   }

   switch (state->fill_front) {
   case PIPE_POLYGON_MODE_FILL:
      dw1 |= GEN6_FILLMODE_SOLID << 5;
      break;
   case PIPE_POLYGON_MODE_LINE:
      dw1 |= GEN6_FILLMODE_WIREFRAME << 5;
      break;
   case PIPE_POLYGON_MODE_POINT:
      dw1 |= GEN6_FILLMODE_POINT << 5;
      break;
   }

   switch (state->fill_back) {
   case PIPE_POLYGON_MODE_FILL:
      dw1 |= GEN6_FILLMODE_SOLID << 3;
      break;
   case PIPE_POLYGON_MODE_LINE:
      dw1 |= GEN6_FILLMODE_WIREFRAME << 3;
      break;
   case PIPE_POLYGON_MODE_POINT:
      dw1 |= GEN6_FILLMODE_POINT << 3;
      break;
   }

   if (state->front_ccw)
      dw1 |= GEN6_FRONTWINDING_CCW;

   dw2 = 0;

   if (state->line_smooth) {
      /*
       * From the Sandy Bridge PRM, volume 2 part 1, page 251:
       *
       *     "This field (Anti-aliasing Enable) must be disabled if any of the
       *      render targets have integer (UINT or SINT) surface format."
       *
       * From the Sandy Bridge PRM, volume 2 part 1, page 317:
       *
       *     "This field (Hierarchical Depth Buffer Enable) must be disabled
       *      if Anti-aliasing Enable in 3DSTATE_SF is enabled.
       *
       * TODO We do not check those yet.
       */
      dw2 |= GEN7_SF_DW2_AA_LINE_ENABLE |
             GEN7_SF_DW2_AA_LINE_CAP_1_0;
   }

   switch (state->cull_face) {
   case PIPE_FACE_NONE:
      dw2 |= GEN6_CULLMODE_NONE << 29;
      break;
   case PIPE_FACE_FRONT:
      dw2 |= GEN6_CULLMODE_FRONT << 29;
      break;
   case PIPE_FACE_BACK:
      dw2 |= GEN6_CULLMODE_BACK << 29;
      break;
   case PIPE_FACE_FRONT_AND_BACK:
      dw2 |= GEN6_CULLMODE_BOTH << 29;
      break;
   }

   /*
    * Smooth lines should intersect ceil(line_width) or (ceil(line_width) + 1)
    * pixels in the minor direction.  We have to make the lines slightly
    * thicker, 0.5 pixel on both sides, so that they intersect that many
    * pixels are considered into the lines.
    *
    * Line width is in U3.7.
    */
   line_width = (int)
      ((state->line_width + (float) state->line_smooth) * 128.0f + 0.5f);
   line_width = CLAMP(line_width, 0, 1023);

   /* use GIQ rules */
   if (line_width == 128 && !state->line_smooth)
      line_width = 0;

   dw2 |= line_width << GEN7_SF_DW2_LINE_WIDTH__SHIFT;

   if (ilo_dev_gen(dev) == ILO_GEN(7.5) && state->line_stipple_enable)
      dw2 |= GEN75_SF_DW2_LINE_STIPPLE_ENABLE;

   if (state->scissor)
      dw2 |= GEN7_SF_DW2_SCISSOR_ENABLE;

   dw3 = GEN7_SF_DW3_TRUE_AA_LINE_DISTANCE |
         GEN7_SF_DW3_SUBPIXEL_8BITS;

   if (state->line_last_pixel)
      dw3 |= GEN7_SF_DW3_LINE_LAST_PIXEL_ENABLE;

   if (state->flatshade_first) {
      dw3 |= 0 << GEN7_SF_DW3_TRI_PROVOKE__SHIFT |
             0 << GEN7_SF_DW3_LINE_PROVOKE__SHIFT |
             1 << GEN7_SF_DW3_TRIFAN_PROVOKE__SHIFT;
   } else {
      dw3 |= 2 << GEN7_SF_DW3_TRI_PROVOKE__SHIFT |
             1 << GEN7_SF_DW3_LINE_PROVOKE__SHIFT |
             2 << GEN7_SF_DW3_TRIFAN_PROVOKE__SHIFT;
   }

   if (!state->point_size_per_vertex)
      dw3 |= GEN7_SF_DW3_USE_POINT_WIDTH;

   /* in U8.3 */
   point_width = (int) (state->point_size * 8.0f + 0.5f);
   point_width = CLAMP(point_width, 1, 2047);

   dw3 |= point_width;

   STATIC_ASSERT(Elements(sf->payload) >= 3);
   sf->payload[0] = dw1;
   sf->payload[1] = dw2;
   sf->payload[2] = dw3;

   if (state->multisample) {
      sf->dw_msaa = GEN6_MSRASTMODE_ON_PATTERN << 8;

      /*
       * From the Sandy Bridge PRM, volume 2 part 1, page 251:
       *
       *     "Software must not program a value of 0.0 when running in
       *      MSRASTMODE_ON_xxx modes - zero-width lines are not available
       *      when multisampling rasterization is enabled."
       */
      if (!line_width) {
         line_width = 128; /* 1.0f */

         sf->dw_msaa |= line_width << GEN7_SF_DW2_LINE_WIDTH__SHIFT;
      }
   } else {
      sf->dw_msaa = 0;
   }

   rasterizer_init_sf_depth_offset_gen6(dev, state, sf);
   /* 3DSTATE_RASTER is Gen8+ only */
   sf->dw_raster = 0;
}

static uint32_t
rasterizer_get_sf_raster_gen8(const struct ilo_dev *dev,
                              const struct pipe_rasterizer_state *state)
{
   uint32_t dw = 0;

   ILO_DEV_ASSERT(dev, 8, 8);

   if (state->front_ccw)
      dw |= GEN6_FRONTWINDING_CCW << 21;

   switch (state->cull_face) {
   case PIPE_FACE_NONE:
      dw |= GEN6_CULLMODE_NONE << 16;
      break;
   case PIPE_FACE_FRONT:
      dw |= GEN6_CULLMODE_FRONT << 16;
      break;
   case PIPE_FACE_BACK:
      dw |= GEN6_CULLMODE_BACK << 16;
      break;
   case PIPE_FACE_FRONT_AND_BACK:
      dw |= GEN6_CULLMODE_BOTH << 16;
      break;
   }

   if (state->point_smooth)
      dw |= GEN8_RASTER_DW1_SMOOTH_POINT_ENABLE;

   if (state->multisample)
      dw |= GEN8_RASTER_DW1_API_MULTISAMPLE_ENABLE;

   if (state->offset_tri)
      dw|= GEN8_RASTER_DW1_DEPTH_OFFSET_SOLID;
   if (state->offset_line)
      dw|= GEN8_RASTER_DW1_DEPTH_OFFSET_WIREFRAME;
   if (state->offset_point)
      dw|= GEN8_RASTER_DW1_DEPTH_OFFSET_POINT;

   switch (state->fill_front) {
   case PIPE_POLYGON_MODE_FILL:
      dw |= GEN6_FILLMODE_SOLID << 5;
      break;
   case PIPE_POLYGON_MODE_LINE:
      dw |= GEN6_FILLMODE_WIREFRAME << 5;
      break;
   case PIPE_POLYGON_MODE_POINT:
      dw |= GEN6_FILLMODE_POINT << 5;
      break;
   }

   switch (state->fill_back) {
   case PIPE_POLYGON_MODE_FILL:
      dw |= GEN6_FILLMODE_SOLID << 3;
      break;
   case PIPE_POLYGON_MODE_LINE:
      dw |= GEN6_FILLMODE_WIREFRAME << 3;
      break;
   case PIPE_POLYGON_MODE_POINT:
      dw |= GEN6_FILLMODE_POINT << 3;
      break;
   }

   if (state->line_smooth)
      dw |= GEN8_RASTER_DW1_AA_LINE_ENABLE;

   if (state->scissor)
      dw |= GEN8_RASTER_DW1_SCISSOR_ENABLE;

   if (state->depth_clip)
      dw |= GEN8_RASTER_DW1_Z_TEST_ENABLE;

   return dw;
}

static void
rasterizer_init_sf_gen8(const struct ilo_dev *dev,
                        const struct pipe_rasterizer_state *state,
                        struct ilo_rasterizer_sf *sf)
{
   int line_width, point_width;
   uint32_t dw1, dw2, dw3;

   ILO_DEV_ASSERT(dev, 8, 8);

   /* in U3.7 */
   line_width = (int)
      ((state->line_width + (float) state->line_smooth) * 128.0f + 0.5f);
   line_width = CLAMP(line_width, 0, 1023);

   /* use GIQ rules */
   if (line_width == 128 && !state->line_smooth)
      line_width = 0;

   /* in U8.3 */
   point_width = (int) (state->point_size * 8.0f + 0.5f);
   point_width = CLAMP(point_width, 1, 2047);

   dw1 = GEN7_SF_DW1_STATISTICS |
         GEN7_SF_DW1_VIEWPORT_TRANSFORM;

   dw2 = line_width << GEN7_SF_DW2_LINE_WIDTH__SHIFT;
   if (state->line_smooth)
      dw2 |= GEN7_SF_DW2_AA_LINE_CAP_1_0;

   dw3 = GEN7_SF_DW3_TRUE_AA_LINE_DISTANCE |
         GEN7_SF_DW3_SUBPIXEL_8BITS |
         point_width;

   if (state->line_last_pixel)
      dw3 |= GEN7_SF_DW3_LINE_LAST_PIXEL_ENABLE;

   if (state->flatshade_first) {
      dw3 |= 0 << GEN7_SF_DW3_TRI_PROVOKE__SHIFT |
             0 << GEN7_SF_DW3_LINE_PROVOKE__SHIFT |
             1 << GEN7_SF_DW3_TRIFAN_PROVOKE__SHIFT;
   } else {
      dw3 |= 2 << GEN7_SF_DW3_TRI_PROVOKE__SHIFT |
             1 << GEN7_SF_DW3_LINE_PROVOKE__SHIFT |
             2 << GEN7_SF_DW3_TRIFAN_PROVOKE__SHIFT;
   }

   if (!state->point_size_per_vertex)
      dw3 |= GEN7_SF_DW3_USE_POINT_WIDTH;

   dw3 |= point_width;

   STATIC_ASSERT(Elements(sf->payload) >= 3);
   sf->payload[0] = dw1;
   sf->payload[1] = dw2;
   sf->payload[2] = dw3;

   rasterizer_init_sf_depth_offset_gen6(dev, state, sf);

   sf->dw_msaa = 0;
   sf->dw_raster = rasterizer_get_sf_raster_gen8(dev, state);
}

static void
rasterizer_init_wm_gen6(const struct ilo_dev *dev,
                        const struct pipe_rasterizer_state *state,
                        struct ilo_rasterizer_wm *wm)
{
   uint32_t dw5, dw6;

   ILO_DEV_ASSERT(dev, 6, 6);

   /* only the FF unit states are set, as in GEN7 */

   dw5 = GEN6_WM_DW5_AA_LINE_WIDTH_2_0;

   /* same value as in 3DSTATE_SF */
   if (state->line_smooth)
      dw5 |= GEN6_WM_DW5_AA_LINE_CAP_1_0;

   if (state->poly_stipple_enable)
      dw5 |= GEN6_WM_DW5_POLY_STIPPLE_ENABLE;
   if (state->line_stipple_enable)
      dw5 |= GEN6_WM_DW5_LINE_STIPPLE_ENABLE;

   /*
    * assertion that makes sure
    *
    *   dw6 |= wm->dw_msaa_rast | wm->dw_msaa_disp;
    *
    * is valid
    */
   STATIC_ASSERT(GEN6_MSRASTMODE_OFF_PIXEL == 0 &&
                 GEN6_WM_DW6_MSDISPMODE_PERSAMPLE == 0);
   dw6 = GEN6_ZW_INTERP_PIXEL << GEN6_WM_DW6_ZW_INTERP__SHIFT;

   if (state->bottom_edge_rule)
      dw6 |= GEN6_WM_DW6_POINT_RASTRULE_UPPER_RIGHT;

   wm->dw_msaa_rast =
      (state->multisample) ? (GEN6_MSRASTMODE_ON_PATTERN << 1) : 0;
   wm->dw_msaa_disp = GEN6_WM_DW6_MSDISPMODE_PERPIXEL;

   STATIC_ASSERT(Elements(wm->payload) >= 2);
   wm->payload[0] = dw5;
   wm->payload[1] = dw6;
}

static void
rasterizer_init_wm_gen7(const struct ilo_dev *dev,
                        const struct pipe_rasterizer_state *state,
                        struct ilo_rasterizer_wm *wm)
{
   uint32_t dw1, dw2;

   ILO_DEV_ASSERT(dev, 7, 7.5);

   /*
    * assertion that makes sure
    *
    *   dw1 |= wm->dw_msaa_rast;
    *   dw2 |= wm->dw_msaa_disp;
    *
    * is valid
    */
   STATIC_ASSERT(GEN6_MSRASTMODE_OFF_PIXEL == 0 &&
                 GEN7_WM_DW2_MSDISPMODE_PERSAMPLE == 0);
   dw1 = GEN6_ZW_INTERP_PIXEL << GEN7_WM_DW1_ZW_INTERP__SHIFT |
         GEN7_WM_DW1_AA_LINE_WIDTH_2_0;
   dw2 = 0;

   /* same value as in 3DSTATE_SF */
   if (state->line_smooth)
      dw1 |= GEN7_WM_DW1_AA_LINE_CAP_1_0;

   if (state->poly_stipple_enable)
      dw1 |= GEN7_WM_DW1_POLY_STIPPLE_ENABLE;
   if (state->line_stipple_enable)
      dw1 |= GEN7_WM_DW1_LINE_STIPPLE_ENABLE;

   if (state->bottom_edge_rule)
      dw1 |= GEN7_WM_DW1_POINT_RASTRULE_UPPER_RIGHT;

   wm->dw_msaa_rast =
      (state->multisample) ? GEN6_MSRASTMODE_ON_PATTERN : 0;
   wm->dw_msaa_disp = GEN7_WM_DW2_MSDISPMODE_PERPIXEL;

   STATIC_ASSERT(Elements(wm->payload) >= 2);
   wm->payload[0] = dw1;
   wm->payload[1] = dw2;
}

static uint32_t
rasterizer_get_wm_gen8(const struct ilo_dev *dev,
                       const struct pipe_rasterizer_state *state)
{
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 8, 8);

   dw = GEN6_ZW_INTERP_PIXEL << GEN7_WM_DW1_ZW_INTERP__SHIFT |
        GEN7_WM_DW1_AA_LINE_WIDTH_2_0;

   /* same value as in 3DSTATE_SF */
   if (state->line_smooth)
      dw |= GEN7_WM_DW1_AA_LINE_CAP_1_0;

   if (state->poly_stipple_enable)
      dw |= GEN7_WM_DW1_POLY_STIPPLE_ENABLE;
   if (state->line_stipple_enable)
      dw |= GEN7_WM_DW1_LINE_STIPPLE_ENABLE;

   if (state->bottom_edge_rule)
      dw |= GEN7_WM_DW1_POINT_RASTRULE_UPPER_RIGHT;

   return dw;
}

void
ilo_gpe_init_rasterizer(const struct ilo_dev *dev,
                        const struct pipe_rasterizer_state *state,
                        struct ilo_rasterizer_state *rasterizer)
{
   rasterizer_init_clip(dev, state, &rasterizer->clip);

   if (ilo_dev_gen(dev) >= ILO_GEN(8)) {
      memset(&rasterizer->wm, 0, sizeof(rasterizer->wm));
      rasterizer->wm.payload[0] = rasterizer_get_wm_gen8(dev, state);

      rasterizer_init_sf_gen8(dev, state, &rasterizer->sf);
   } else if (ilo_dev_gen(dev) >= ILO_GEN(7)) {
      rasterizer_init_wm_gen7(dev, state, &rasterizer->wm);
      rasterizer_init_sf_gen6(dev, state, &rasterizer->sf);
   } else {
      rasterizer_init_wm_gen6(dev, state, &rasterizer->wm);
      rasterizer_init_sf_gen6(dev, state, &rasterizer->sf);
   }
}

static void
fs_init_cso_gen6(const struct ilo_dev *dev,
                 const struct ilo_shader_state *fs,
                 struct ilo_shader_cso *cso)
{
   int start_grf, input_count, sampler_count, interps, max_threads;
   uint32_t dw2, dw4, dw5, dw6;

   ILO_DEV_ASSERT(dev, 6, 6);

   start_grf = ilo_shader_get_kernel_param(fs, ILO_KERNEL_URB_DATA_START_REG);
   input_count = ilo_shader_get_kernel_param(fs, ILO_KERNEL_INPUT_COUNT);
   sampler_count = ilo_shader_get_kernel_param(fs, ILO_KERNEL_SAMPLER_COUNT);
   interps = ilo_shader_get_kernel_param(fs,
         ILO_KERNEL_FS_BARYCENTRIC_INTERPOLATIONS);

   /* see brwCreateContext() */
   max_threads = (dev->gt == 2) ? 80 : 40;

   dw2 = (true) ? 0 : GEN6_THREADDISP_FP_MODE_ALT;
   dw2 |= ((sampler_count + 3) / 4) << GEN6_THREADDISP_SAMPLER_COUNT__SHIFT;

   dw4 = start_grf << GEN6_WM_DW4_URB_GRF_START0__SHIFT |
         0 << GEN6_WM_DW4_URB_GRF_START1__SHIFT |
         0 << GEN6_WM_DW4_URB_GRF_START2__SHIFT;

   dw5 = (max_threads - 1) << GEN6_WM_DW5_MAX_THREADS__SHIFT;

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 275:
    *
    *     "This bit (Pixel Shader Kill Pixel), if ENABLED, indicates that the
    *      PS kernel or color calculator has the ability to kill (discard)
    *      pixels or samples, other than due to depth or stencil testing.
    *      This bit is required to be ENABLED in the following situations:
    *
    *      The API pixel shader program contains "killpix" or "discard"
    *      instructions, or other code in the pixel shader kernel that can
    *      cause the final pixel mask to differ from the pixel mask received
    *      on dispatch.
    *
    *      A sampler with chroma key enabled with kill pixel mode is used by
    *      the pixel shader.
    *
    *      Any render target has Alpha Test Enable or AlphaToCoverage Enable
    *      enabled.
    *
    *      The pixel shader kernel generates and outputs oMask.
    *
    *      Note: As ClipDistance clipping is fully supported in hardware and
    *      therefore not via PS instructions, there should be no need to
    *      ENABLE this bit due to ClipDistance clipping."
    */
   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_USE_KILL))
      dw5 |= GEN6_WM_DW5_PS_KILL_PIXEL;

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 275:
    *
    *     "If a NULL Depth Buffer is selected, the Pixel Shader Computed Depth
    *      field must be set to disabled."
    *
    * TODO This is not checked yet.
    */
   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_OUTPUT_Z))
      dw5 |= GEN6_WM_DW5_PS_COMPUTE_DEPTH;

   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_INPUT_Z))
      dw5 |= GEN6_WM_DW5_PS_USE_DEPTH;

   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_INPUT_W))
      dw5 |= GEN6_WM_DW5_PS_USE_W;

   /*
    * TODO set this bit only when
    *
    *  a) fs writes colors and color is not masked, or
    *  b) fs writes depth, or
    *  c) fs or cc kills
    */
   if (true)
      dw5 |= GEN6_WM_DW5_PS_DISPATCH_ENABLE;

   assert(!ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_DISPATCH_16_OFFSET));
   dw5 |= GEN6_PS_DISPATCH_8 << GEN6_WM_DW5_PS_DISPATCH_MODE__SHIFT;

   dw6 = input_count << GEN6_WM_DW6_SF_ATTR_COUNT__SHIFT |
         GEN6_POSOFFSET_NONE << GEN6_WM_DW6_PS_POSOFFSET__SHIFT |
         interps << GEN6_WM_DW6_BARYCENTRIC_INTERP__SHIFT;

   STATIC_ASSERT(Elements(cso->payload) >= 4);
   cso->payload[0] = dw2;
   cso->payload[1] = dw4;
   cso->payload[2] = dw5;
   cso->payload[3] = dw6;
}

static uint32_t
fs_get_wm_gen7(const struct ilo_dev *dev,
               const struct ilo_shader_state *fs)
{
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 7, 7.5);

   dw = ilo_shader_get_kernel_param(fs,
         ILO_KERNEL_FS_BARYCENTRIC_INTERPOLATIONS) <<
      GEN7_WM_DW1_BARYCENTRIC_INTERP__SHIFT;

   /*
    * TODO set this bit only when
    *
    *  a) fs writes colors and color is not masked, or
    *  b) fs writes depth, or
    *  c) fs or cc kills
    */
   dw |= GEN7_WM_DW1_PS_DISPATCH_ENABLE;

   /*
    * From the Ivy Bridge PRM, volume 2 part 1, page 278:
    *
    *     "This bit (Pixel Shader Kill Pixel), if ENABLED, indicates that
    *      the PS kernel or color calculator has the ability to kill
    *      (discard) pixels or samples, other than due to depth or stencil
    *      testing. This bit is required to be ENABLED in the following
    *      situations:
    *
    *      - The API pixel shader program contains "killpix" or "discard"
    *        instructions, or other code in the pixel shader kernel that
    *        can cause the final pixel mask to differ from the pixel mask
    *        received on dispatch.
    *
    *      - A sampler with chroma key enabled with kill pixel mode is used
    *        by the pixel shader.
    *
    *      - Any render target has Alpha Test Enable or AlphaToCoverage
    *        Enable enabled.
    *
    *      - The pixel shader kernel generates and outputs oMask.
    *
    *      Note: As ClipDistance clipping is fully supported in hardware
    *      and therefore not via PS instructions, there should be no need
    *      to ENABLE this bit due to ClipDistance clipping."
    */
   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_USE_KILL))
      dw |= GEN7_WM_DW1_PS_KILL_PIXEL;

   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_OUTPUT_Z))
      dw |= GEN7_PSCDEPTH_ON << GEN7_WM_DW1_PSCDEPTH__SHIFT;

   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_INPUT_Z))
      dw |= GEN7_WM_DW1_PS_USE_DEPTH;

   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_INPUT_W))
      dw |= GEN7_WM_DW1_PS_USE_W;

   return dw;
}

static void
fs_init_cso_gen7(const struct ilo_dev *dev,
                 const struct ilo_shader_state *fs,
                 struct ilo_shader_cso *cso)
{
   int start_grf, sampler_count, max_threads;
   uint32_t dw2, dw4, dw5;

   ILO_DEV_ASSERT(dev, 7, 7.5);

   start_grf = ilo_shader_get_kernel_param(fs, ILO_KERNEL_URB_DATA_START_REG);
   sampler_count = ilo_shader_get_kernel_param(fs, ILO_KERNEL_SAMPLER_COUNT);

   dw2 = (true) ? 0 : GEN6_THREADDISP_FP_MODE_ALT;
   dw2 |= ((sampler_count + 3) / 4) << GEN6_THREADDISP_SAMPLER_COUNT__SHIFT;

   dw4 = GEN6_POSOFFSET_NONE << GEN7_PS_DW4_POSOFFSET__SHIFT;

   /* see brwCreateContext() */
   switch (ilo_dev_gen(dev)) {
   case ILO_GEN(7.5):
      max_threads = (dev->gt == 3) ? 408 : (dev->gt == 2) ? 204 : 102;
      dw4 |= (max_threads - 1) << GEN75_PS_DW4_MAX_THREADS__SHIFT;
      dw4 |= 1 << GEN75_PS_DW4_SAMPLE_MASK__SHIFT;
      break;
   case ILO_GEN(7):
   default:
      max_threads = (dev->gt == 2) ? 172 : 48;
      dw4 |= (max_threads - 1) << GEN7_PS_DW4_MAX_THREADS__SHIFT;
      break;
   }

   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_PCB_CBUF0_SIZE))
      dw4 |= GEN7_PS_DW4_PUSH_CONSTANT_ENABLE;

   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_INPUT_COUNT))
      dw4 |= GEN7_PS_DW4_ATTR_ENABLE;

   assert(!ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_DISPATCH_16_OFFSET));
   dw4 |= GEN6_PS_DISPATCH_8 << GEN7_PS_DW4_DISPATCH_MODE__SHIFT;

   dw5 = start_grf << GEN7_PS_DW5_URB_GRF_START0__SHIFT |
         0 << GEN7_PS_DW5_URB_GRF_START1__SHIFT |
         0 << GEN7_PS_DW5_URB_GRF_START2__SHIFT;

   STATIC_ASSERT(Elements(cso->payload) >= 4);
   cso->payload[0] = dw2;
   cso->payload[1] = dw4;
   cso->payload[2] = dw5;
   cso->payload[3] = fs_get_wm_gen7(dev, fs);
}

static uint32_t
fs_get_psx_gen8(const struct ilo_dev *dev,
                const struct ilo_shader_state *fs)
{
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 8, 8);

   dw = GEN8_PSX_DW1_VALID;

   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_USE_KILL))
      dw |= GEN8_PSX_DW1_KILL_PIXEL;
   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_OUTPUT_Z))
      dw |= GEN7_PSCDEPTH_ON << GEN8_PSX_DW1_PSCDEPTH__SHIFT;
   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_INPUT_Z))
      dw |= GEN8_PSX_DW1_USE_DEPTH;
   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_INPUT_W))
      dw |= GEN8_PSX_DW1_USE_W;
   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_INPUT_COUNT))
      dw |= GEN8_PSX_DW1_ATTR_ENABLE;

   return dw;
}

static uint32_t
fs_get_wm_gen8(const struct ilo_dev *dev,
               const struct ilo_shader_state *fs)
{
   ILO_DEV_ASSERT(dev, 8, 8);

   return ilo_shader_get_kernel_param(fs,
         ILO_KERNEL_FS_BARYCENTRIC_INTERPOLATIONS) <<
      GEN7_WM_DW1_BARYCENTRIC_INTERP__SHIFT;
}

static void
fs_init_cso_gen8(const struct ilo_dev *dev,
                 const struct ilo_shader_state *fs,
                 struct ilo_shader_cso *cso)
{
   int start_grf, sampler_count;
   uint32_t dw3, dw6, dw7;

   ILO_DEV_ASSERT(dev, 8, 8);

   start_grf = ilo_shader_get_kernel_param(fs, ILO_KERNEL_URB_DATA_START_REG);
   sampler_count = ilo_shader_get_kernel_param(fs, ILO_KERNEL_SAMPLER_COUNT);

   dw3 = (true) ? 0 : GEN6_THREADDISP_FP_MODE_ALT;
   dw3 |= ((sampler_count + 3) / 4) << GEN6_THREADDISP_SAMPLER_COUNT__SHIFT;

   /* always 64? */
   dw6 = (64 - 2) << GEN8_PS_DW6_MAX_THREADS__SHIFT |
         GEN6_POSOFFSET_NONE << GEN8_PS_DW6_POSOFFSET__SHIFT;
   if (ilo_shader_get_kernel_param(fs, ILO_KERNEL_PCB_CBUF0_SIZE))
      dw6 |= GEN8_PS_DW6_PUSH_CONSTANT_ENABLE;

   assert(!ilo_shader_get_kernel_param(fs, ILO_KERNEL_FS_DISPATCH_16_OFFSET));
   dw6 |= GEN6_PS_DISPATCH_8 << GEN8_PS_DW6_DISPATCH_MODE__SHIFT;

   dw7 = start_grf << GEN8_PS_DW7_URB_GRF_START0__SHIFT |
         0 << GEN8_PS_DW7_URB_GRF_START1__SHIFT |
         0 << GEN8_PS_DW7_URB_GRF_START2__SHIFT;

   STATIC_ASSERT(Elements(cso->payload) >= 5);
   cso->payload[0] = dw3;
   cso->payload[1] = dw6;
   cso->payload[2] = dw7;
   cso->payload[3] = fs_get_psx_gen8(dev, fs);
   cso->payload[4] = fs_get_wm_gen8(dev, fs);
}

void
ilo_gpe_init_fs_cso(const struct ilo_dev *dev,
                    const struct ilo_shader_state *fs,
                    struct ilo_shader_cso *cso)
{
   if (ilo_dev_gen(dev) >= ILO_GEN(8))
      fs_init_cso_gen8(dev, fs, cso);
   else if (ilo_dev_gen(dev) >= ILO_GEN(7))
      fs_init_cso_gen7(dev, fs, cso);
   else
      fs_init_cso_gen6(dev, fs, cso);
}

/**
 * Translate a pipe logicop to the matching hardware logicop.
 */
static int
gen6_translate_pipe_logicop(unsigned logicop)
{
   switch (logicop) {
   case PIPE_LOGICOP_CLEAR:         return GEN6_LOGICOP_CLEAR;
   case PIPE_LOGICOP_NOR:           return GEN6_LOGICOP_NOR;
   case PIPE_LOGICOP_AND_INVERTED:  return GEN6_LOGICOP_AND_INVERTED;
   case PIPE_LOGICOP_COPY_INVERTED: return GEN6_LOGICOP_COPY_INVERTED;
   case PIPE_LOGICOP_AND_REVERSE:   return GEN6_LOGICOP_AND_REVERSE;
   case PIPE_LOGICOP_INVERT:        return GEN6_LOGICOP_INVERT;
   case PIPE_LOGICOP_XOR:           return GEN6_LOGICOP_XOR;
   case PIPE_LOGICOP_NAND:          return GEN6_LOGICOP_NAND;
   case PIPE_LOGICOP_AND:           return GEN6_LOGICOP_AND;
   case PIPE_LOGICOP_EQUIV:         return GEN6_LOGICOP_EQUIV;
   case PIPE_LOGICOP_NOOP:          return GEN6_LOGICOP_NOOP;
   case PIPE_LOGICOP_OR_INVERTED:   return GEN6_LOGICOP_OR_INVERTED;
   case PIPE_LOGICOP_COPY:          return GEN6_LOGICOP_COPY;
   case PIPE_LOGICOP_OR_REVERSE:    return GEN6_LOGICOP_OR_REVERSE;
   case PIPE_LOGICOP_OR:            return GEN6_LOGICOP_OR;
   case PIPE_LOGICOP_SET:           return GEN6_LOGICOP_SET;
   default:
      assert(!"unknown logicop function");
      return GEN6_LOGICOP_CLEAR;
   }
}

/**
 * Translate a pipe blend function to the matching hardware blend function.
 */
static int
gen6_translate_pipe_blend(unsigned blend)
{
   switch (blend) {
   case PIPE_BLEND_ADD:                return GEN6_BLENDFUNCTION_ADD;
   case PIPE_BLEND_SUBTRACT:           return GEN6_BLENDFUNCTION_SUBTRACT;
   case PIPE_BLEND_REVERSE_SUBTRACT:   return GEN6_BLENDFUNCTION_REVERSE_SUBTRACT;
   case PIPE_BLEND_MIN:                return GEN6_BLENDFUNCTION_MIN;
   case PIPE_BLEND_MAX:                return GEN6_BLENDFUNCTION_MAX;
   default:
      assert(!"unknown blend function");
      return GEN6_BLENDFUNCTION_ADD;
   };
}

/**
 * Translate a pipe blend factor to the matching hardware blend factor.
 */
static int
gen6_translate_pipe_blendfactor(unsigned blendfactor)
{
   switch (blendfactor) {
   case PIPE_BLENDFACTOR_ONE:                return GEN6_BLENDFACTOR_ONE;
   case PIPE_BLENDFACTOR_SRC_COLOR:          return GEN6_BLENDFACTOR_SRC_COLOR;
   case PIPE_BLENDFACTOR_SRC_ALPHA:          return GEN6_BLENDFACTOR_SRC_ALPHA;
   case PIPE_BLENDFACTOR_DST_ALPHA:          return GEN6_BLENDFACTOR_DST_ALPHA;
   case PIPE_BLENDFACTOR_DST_COLOR:          return GEN6_BLENDFACTOR_DST_COLOR;
   case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE: return GEN6_BLENDFACTOR_SRC_ALPHA_SATURATE;
   case PIPE_BLENDFACTOR_CONST_COLOR:        return GEN6_BLENDFACTOR_CONST_COLOR;
   case PIPE_BLENDFACTOR_CONST_ALPHA:        return GEN6_BLENDFACTOR_CONST_ALPHA;
   case PIPE_BLENDFACTOR_SRC1_COLOR:         return GEN6_BLENDFACTOR_SRC1_COLOR;
   case PIPE_BLENDFACTOR_SRC1_ALPHA:         return GEN6_BLENDFACTOR_SRC1_ALPHA;
   case PIPE_BLENDFACTOR_ZERO:               return GEN6_BLENDFACTOR_ZERO;
   case PIPE_BLENDFACTOR_INV_SRC_COLOR:      return GEN6_BLENDFACTOR_INV_SRC_COLOR;
   case PIPE_BLENDFACTOR_INV_SRC_ALPHA:      return GEN6_BLENDFACTOR_INV_SRC_ALPHA;
   case PIPE_BLENDFACTOR_INV_DST_ALPHA:      return GEN6_BLENDFACTOR_INV_DST_ALPHA;
   case PIPE_BLENDFACTOR_INV_DST_COLOR:      return GEN6_BLENDFACTOR_INV_DST_COLOR;
   case PIPE_BLENDFACTOR_INV_CONST_COLOR:    return GEN6_BLENDFACTOR_INV_CONST_COLOR;
   case PIPE_BLENDFACTOR_INV_CONST_ALPHA:    return GEN6_BLENDFACTOR_INV_CONST_ALPHA;
   case PIPE_BLENDFACTOR_INV_SRC1_COLOR:     return GEN6_BLENDFACTOR_INV_SRC1_COLOR;
   case PIPE_BLENDFACTOR_INV_SRC1_ALPHA:     return GEN6_BLENDFACTOR_INV_SRC1_ALPHA;
   default:
      assert(!"unknown blend factor");
      return GEN6_BLENDFACTOR_ONE;
   };
}

/**
 * Translate a pipe stencil op to the matching hardware stencil op.
 */
static int
gen6_translate_pipe_stencil_op(unsigned stencil_op)
{
   switch (stencil_op) {
   case PIPE_STENCIL_OP_KEEP:       return GEN6_STENCILOP_KEEP;
   case PIPE_STENCIL_OP_ZERO:       return GEN6_STENCILOP_ZERO;
   case PIPE_STENCIL_OP_REPLACE:    return GEN6_STENCILOP_REPLACE;
   case PIPE_STENCIL_OP_INCR:       return GEN6_STENCILOP_INCRSAT;
   case PIPE_STENCIL_OP_DECR:       return GEN6_STENCILOP_DECRSAT;
   case PIPE_STENCIL_OP_INCR_WRAP:  return GEN6_STENCILOP_INCR;
   case PIPE_STENCIL_OP_DECR_WRAP:  return GEN6_STENCILOP_DECR;
   case PIPE_STENCIL_OP_INVERT:     return GEN6_STENCILOP_INVERT;
   default:
      assert(!"unknown stencil op");
      return GEN6_STENCILOP_KEEP;
   }
}

static int
gen6_blend_factor_dst_alpha_forced_one(int factor)
{
   switch (factor) {
   case GEN6_BLENDFACTOR_DST_ALPHA:
      return GEN6_BLENDFACTOR_ONE;
   case GEN6_BLENDFACTOR_INV_DST_ALPHA:
   case GEN6_BLENDFACTOR_SRC_ALPHA_SATURATE:
      return GEN6_BLENDFACTOR_ZERO;
   default:
      return factor;
   }
}

static uint32_t
blend_get_rt_blend_enable_gen6(const struct ilo_dev *dev,
                               const struct pipe_rt_blend_state *rt,
                               bool dst_alpha_forced_one)
{
   int rgb_src, rgb_dst, a_src, a_dst;
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 6, 7.5);

   if (!rt->blend_enable)
      return 0;

   rgb_src = gen6_translate_pipe_blendfactor(rt->rgb_src_factor);
   rgb_dst = gen6_translate_pipe_blendfactor(rt->rgb_dst_factor);
   a_src = gen6_translate_pipe_blendfactor(rt->alpha_src_factor);
   a_dst = gen6_translate_pipe_blendfactor(rt->alpha_dst_factor);

   if (dst_alpha_forced_one) {
      rgb_src = gen6_blend_factor_dst_alpha_forced_one(rgb_src);
      rgb_dst = gen6_blend_factor_dst_alpha_forced_one(rgb_dst);
      a_src = gen6_blend_factor_dst_alpha_forced_one(a_src);
      a_dst = gen6_blend_factor_dst_alpha_forced_one(a_dst);
   }

   dw = GEN6_RT_DW0_BLEND_ENABLE |
        gen6_translate_pipe_blend(rt->alpha_func) << 26 |
        a_src << 20 |
        a_dst << 15 |
        gen6_translate_pipe_blend(rt->rgb_func) << 11 |
        rgb_src << 5 |
        rgb_dst;

   if (rt->rgb_func != rt->alpha_func ||
       rgb_src != a_src || rgb_dst != a_dst)
      dw |= GEN6_RT_DW0_INDEPENDENT_ALPHA_ENABLE;

   return dw;
}

static uint32_t
blend_get_rt_blend_enable_gen8(const struct ilo_dev *dev,
                               const struct pipe_rt_blend_state *rt,
                               bool dst_alpha_forced_one,
                               bool *independent_alpha)
{
   int rgb_src, rgb_dst, a_src, a_dst;
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 8, 8);

   if (!rt->blend_enable) {
      *independent_alpha = false;
      return 0;
   }

   rgb_src = gen6_translate_pipe_blendfactor(rt->rgb_src_factor);
   rgb_dst = gen6_translate_pipe_blendfactor(rt->rgb_dst_factor);
   a_src = gen6_translate_pipe_blendfactor(rt->alpha_src_factor);
   a_dst = gen6_translate_pipe_blendfactor(rt->alpha_dst_factor);

   if (dst_alpha_forced_one) {
      rgb_src = gen6_blend_factor_dst_alpha_forced_one(rgb_src);
      rgb_dst = gen6_blend_factor_dst_alpha_forced_one(rgb_dst);
      a_src = gen6_blend_factor_dst_alpha_forced_one(a_src);
      a_dst = gen6_blend_factor_dst_alpha_forced_one(a_dst);
   }

   dw = GEN8_RT_DW0_BLEND_ENABLE |
        rgb_src << 26 |
        rgb_dst << 21 |
        gen6_translate_pipe_blend(rt->rgb_func) << 18 |
        a_src << 13 |
        a_dst << 8 |
        gen6_translate_pipe_blend(rt->alpha_func) << 5;

   *independent_alpha = (rt->rgb_func != rt->alpha_func ||
                         rgb_src != a_src ||
                         rgb_dst != a_dst);

   return dw;
}

static void
blend_init_cso_gen6(const struct ilo_dev *dev,
                    const struct pipe_blend_state *state,
                    struct ilo_blend_state *blend,
                    unsigned index)
{
   const struct pipe_rt_blend_state *rt = &state->rt[index];
   struct ilo_blend_cso *cso = &blend->cso[index];

   ILO_DEV_ASSERT(dev, 6, 7.5);

   cso->payload[0] = 0;
   cso->payload[1] = GEN6_RT_DW1_COLORCLAMP_RTFORMAT |
                     GEN6_RT_DW1_PRE_BLEND_CLAMP |
                     GEN6_RT_DW1_POST_BLEND_CLAMP;

   if (!(rt->colormask & PIPE_MASK_A))
      cso->payload[1] |= GEN6_RT_DW1_WRITE_DISABLES_A;
   if (!(rt->colormask & PIPE_MASK_R))
      cso->payload[1] |= GEN6_RT_DW1_WRITE_DISABLES_R;
   if (!(rt->colormask & PIPE_MASK_G))
      cso->payload[1] |= GEN6_RT_DW1_WRITE_DISABLES_G;
   if (!(rt->colormask & PIPE_MASK_B))
      cso->payload[1] |= GEN6_RT_DW1_WRITE_DISABLES_B;

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 365:
    *
    *     "Color Buffer Blending and Logic Ops must not be enabled
    *      simultaneously, or behavior is UNDEFINED."
    *
    * Since state->logicop_enable takes precedence over rt->blend_enable,
    * no special care is needed.
    */
   if (state->logicop_enable) {
      cso->dw_blend = 0;
      cso->dw_blend_dst_alpha_forced_one = 0;
   } else {
      cso->dw_blend = blend_get_rt_blend_enable_gen6(dev, rt, false);
      cso->dw_blend_dst_alpha_forced_one =
         blend_get_rt_blend_enable_gen6(dev, rt, true);
   }
}

static bool
blend_init_cso_gen8(const struct ilo_dev *dev,
                    const struct pipe_blend_state *state,
                    struct ilo_blend_state *blend,
                    unsigned index)
{
   const struct pipe_rt_blend_state *rt = &state->rt[index];
   struct ilo_blend_cso *cso = &blend->cso[index];
   bool independent_alpha = false;

   ILO_DEV_ASSERT(dev, 8, 8);

   cso->payload[0] = 0;
   cso->payload[1] = GEN8_RT_DW1_COLORCLAMP_RTFORMAT |
                     GEN8_RT_DW1_PRE_BLEND_CLAMP |
                     GEN8_RT_DW1_POST_BLEND_CLAMP;

   if (!(rt->colormask & PIPE_MASK_A))
      cso->payload[0] |= GEN8_RT_DW0_WRITE_DISABLES_A;
   if (!(rt->colormask & PIPE_MASK_R))
      cso->payload[0] |= GEN8_RT_DW0_WRITE_DISABLES_R;
   if (!(rt->colormask & PIPE_MASK_G))
      cso->payload[0] |= GEN8_RT_DW0_WRITE_DISABLES_G;
   if (!(rt->colormask & PIPE_MASK_B))
      cso->payload[0] |= GEN8_RT_DW0_WRITE_DISABLES_B;

   if (state->logicop_enable) {
      cso->dw_blend = 0;
      cso->dw_blend_dst_alpha_forced_one = 0;
   } else {
      bool tmp[2];

      cso->dw_blend = blend_get_rt_blend_enable_gen8(dev, rt, false, &tmp[0]);
      cso->dw_blend_dst_alpha_forced_one =
         blend_get_rt_blend_enable_gen8(dev, rt, true, &tmp[1]);

      if (tmp[0] || tmp[1])
         independent_alpha = true;
   }

   return independent_alpha;
}

static uint32_t
blend_get_logicop_enable_gen6(const struct ilo_dev *dev,
                              const struct pipe_blend_state *state)
{
   ILO_DEV_ASSERT(dev, 6, 7.5);

   if (!state->logicop_enable)
      return 0;

   return GEN6_RT_DW1_LOGICOP_ENABLE |
          gen6_translate_pipe_logicop(state->logicop_func) << 18;
}

static uint32_t
blend_get_logicop_enable_gen8(const struct ilo_dev *dev,
                              const struct pipe_blend_state *state)
{
   ILO_DEV_ASSERT(dev, 8, 8);

   if (!state->logicop_enable)
      return 0;

   return GEN8_RT_DW1_LOGICOP_ENABLE |
          gen6_translate_pipe_logicop(state->logicop_func) << 27;
}

static uint32_t
blend_get_alpha_mod_gen6(const struct ilo_dev *dev,
                         const struct pipe_blend_state *state,
                         bool dual_blend)
{
   uint32_t dw = 0;

   ILO_DEV_ASSERT(dev, 6, 7.5);

   if (state->alpha_to_coverage) {
      dw |= GEN6_RT_DW1_ALPHA_TO_COVERAGE;
      if (ilo_dev_gen(dev) >= ILO_GEN(7))
         dw |= GEN6_RT_DW1_ALPHA_TO_COVERAGE_DITHER;
   }
   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 378:
    *
    *     "If Dual Source Blending is enabled, this bit (AlphaToOne Enable)
    *      must be disabled."
    */
   if (state->alpha_to_one && !dual_blend)
      dw |= GEN6_RT_DW1_ALPHA_TO_ONE;

   return dw;
}

static uint32_t
blend_get_alpha_mod_gen8(const struct ilo_dev *dev,
                         const struct pipe_blend_state *state,
                         bool dual_blend)
{
   uint32_t dw = 0;

   ILO_DEV_ASSERT(dev, 8, 8);

   if (state->alpha_to_coverage) {
      dw |= GEN8_BLEND_DW0_ALPHA_TO_COVERAGE |
            GEN8_BLEND_DW0_ALPHA_TO_COVERAGE_DITHER;
   }

   if (state->alpha_to_one && !dual_blend)
      dw |= GEN8_BLEND_DW0_ALPHA_TO_ONE;

   return dw;
}

static uint32_t
blend_get_ps_blend_gen8(const struct ilo_dev *dev, uint32_t rt_dw0)
{
   int rgb_src, rgb_dst, a_src, a_dst;
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 8, 8);

   if (!(rt_dw0 & GEN8_RT_DW0_BLEND_ENABLE))
      return 0;

   a_src = GEN_EXTRACT(rt_dw0, GEN8_RT_DW0_SRC_ALPHA_FACTOR);
   a_dst = GEN_EXTRACT(rt_dw0, GEN8_RT_DW0_DST_ALPHA_FACTOR);
   rgb_src = GEN_EXTRACT(rt_dw0, GEN8_RT_DW0_SRC_COLOR_FACTOR);
   rgb_dst = GEN_EXTRACT(rt_dw0, GEN8_RT_DW0_DST_COLOR_FACTOR);

   dw = GEN8_PS_BLEND_DW1_BLEND_ENABLE;
   dw |= GEN_SHIFT32(a_src, GEN8_PS_BLEND_DW1_SRC_ALPHA_FACTOR);
   dw |= GEN_SHIFT32(a_dst, GEN8_PS_BLEND_DW1_DST_ALPHA_FACTOR);
   dw |= GEN_SHIFT32(rgb_src, GEN8_PS_BLEND_DW1_SRC_COLOR_FACTOR);
   dw |= GEN_SHIFT32(rgb_dst, GEN8_PS_BLEND_DW1_DST_COLOR_FACTOR);

   if (a_src != rgb_src || a_dst != rgb_dst)
      dw |= GEN8_PS_BLEND_DW1_INDEPENDENT_ALPHA_ENABLE;

   return dw;
}

void
ilo_gpe_init_blend(const struct ilo_dev *dev,
                   const struct pipe_blend_state *state,
                   struct ilo_blend_state *blend)
{
   unsigned i;

   ILO_DEV_ASSERT(dev, 6, 8);

   blend->dual_blend = (util_blend_state_is_dual(state, 0) &&
                        state->rt[0].blend_enable &&
                        !state->logicop_enable);
   blend->alpha_to_coverage = state->alpha_to_coverage;

   if (ilo_dev_gen(dev) >= ILO_GEN(8)) {
      bool independent_alpha;

      blend->dw_alpha_mod =
         blend_get_alpha_mod_gen8(dev, state, blend->dual_blend);
      blend->dw_logicop = blend_get_logicop_enable_gen8(dev, state);
      blend->dw_shared = (state->dither) ? GEN8_BLEND_DW0_DITHER_ENABLE : 0;

      independent_alpha = blend_init_cso_gen8(dev, state, blend, 0);
      if (independent_alpha)
         blend->dw_shared |= GEN8_BLEND_DW0_INDEPENDENT_ALPHA_ENABLE;

      blend->dw_ps_blend = blend_get_ps_blend_gen8(dev,
            blend->cso[0].dw_blend);
      blend->dw_ps_blend_dst_alpha_forced_one = blend_get_ps_blend_gen8(dev,
            blend->cso[0].dw_blend_dst_alpha_forced_one);

      if (state->independent_blend_enable) {
         for (i = 1; i < Elements(blend->cso); i++) {
            independent_alpha = blend_init_cso_gen8(dev, state, blend, i);
            if (independent_alpha)
               blend->dw_shared |= GEN8_BLEND_DW0_INDEPENDENT_ALPHA_ENABLE;
         }
      } else {
         for (i = 1; i < Elements(blend->cso); i++)
            blend->cso[i] = blend->cso[0];
      }
   } else {
      blend->dw_alpha_mod =
         blend_get_alpha_mod_gen6(dev, state, blend->dual_blend);
      blend->dw_logicop = blend_get_logicop_enable_gen6(dev, state);
      blend->dw_shared = (state->dither) ? GEN6_RT_DW1_DITHER_ENABLE : 0;

      blend->dw_ps_blend = 0;
      blend->dw_ps_blend_dst_alpha_forced_one = 0;

      blend_init_cso_gen6(dev, state, blend, 0);
      if (state->independent_blend_enable) {
         for (i = 1; i < Elements(blend->cso); i++)
            blend_init_cso_gen6(dev, state, blend, i);
      } else {
         for (i = 1; i < Elements(blend->cso); i++)
            blend->cso[i] = blend->cso[0];
      }
   }
}

/**
 * Translate a pipe DSA test function to the matching hardware compare
 * function.
 */
static int
gen6_translate_dsa_func(unsigned func)
{
   switch (func) {
   case PIPE_FUNC_NEVER:      return GEN6_COMPAREFUNCTION_NEVER;
   case PIPE_FUNC_LESS:       return GEN6_COMPAREFUNCTION_LESS;
   case PIPE_FUNC_EQUAL:      return GEN6_COMPAREFUNCTION_EQUAL;
   case PIPE_FUNC_LEQUAL:     return GEN6_COMPAREFUNCTION_LEQUAL;
   case PIPE_FUNC_GREATER:    return GEN6_COMPAREFUNCTION_GREATER;
   case PIPE_FUNC_NOTEQUAL:   return GEN6_COMPAREFUNCTION_NOTEQUAL;
   case PIPE_FUNC_GEQUAL:     return GEN6_COMPAREFUNCTION_GEQUAL;
   case PIPE_FUNC_ALWAYS:     return GEN6_COMPAREFUNCTION_ALWAYS;
   default:
      assert(!"unknown depth/stencil/alpha test function");
      return GEN6_COMPAREFUNCTION_NEVER;
   }
}

static uint32_t
dsa_get_stencil_enable_gen6(const struct ilo_dev *dev,
                            const struct pipe_stencil_state *stencil0,
                            const struct pipe_stencil_state *stencil1)
{
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 6, 7.5);

   if (!stencil0->enabled)
      return 0;

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 359:
    *
    *     "If the Depth Buffer is either undefined or does not have a surface
    *      format of D32_FLOAT_S8X24_UINT or D24_UNORM_S8_UINT and separate
    *      stencil buffer is disabled, Stencil Test Enable must be DISABLED"
    *
    * From the Sandy Bridge PRM, volume 2 part 1, page 370:
    *
    *     "This field (Stencil Test Enable) cannot be enabled if
    *      Surface Format in 3DSTATE_DEPTH_BUFFER is set to D16_UNORM."
    *
    * TODO We do not check these yet.
    */
   dw = GEN6_ZS_DW0_STENCIL_TEST_ENABLE |
        gen6_translate_dsa_func(stencil0->func) << 28 |
        gen6_translate_pipe_stencil_op(stencil0->fail_op) << 25 |
        gen6_translate_pipe_stencil_op(stencil0->zfail_op) << 22 |
        gen6_translate_pipe_stencil_op(stencil0->zpass_op) << 19;
   if (stencil0->writemask)
      dw |= GEN6_ZS_DW0_STENCIL_WRITE_ENABLE;

   if (stencil1->enabled) {
      dw |= GEN6_ZS_DW0_STENCIL1_ENABLE |
            gen6_translate_dsa_func(stencil1->func) << 12 |
            gen6_translate_pipe_stencil_op(stencil1->fail_op) << 9 |
            gen6_translate_pipe_stencil_op(stencil1->zfail_op) << 6 |
            gen6_translate_pipe_stencil_op(stencil1->zpass_op) << 3;
      if (stencil1->writemask)
         dw |= GEN6_ZS_DW0_STENCIL_WRITE_ENABLE;
   }

   return dw;
}

static uint32_t
dsa_get_stencil_enable_gen8(const struct ilo_dev *dev,
                            const struct pipe_stencil_state *stencil0,
                            const struct pipe_stencil_state *stencil1)
{
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 8, 8);

   if (!stencil0->enabled)
      return 0;

   dw = gen6_translate_pipe_stencil_op(stencil0->fail_op) << 29 |
        gen6_translate_pipe_stencil_op(stencil0->zfail_op) << 26 |
        gen6_translate_pipe_stencil_op(stencil0->zpass_op) << 23 |
        gen6_translate_dsa_func(stencil0->func) << 8 |
        GEN8_ZS_DW1_STENCIL_TEST_ENABLE;
   if (stencil0->writemask)
      dw |= GEN8_ZS_DW1_STENCIL_WRITE_ENABLE;

   if (stencil1->enabled) {
      dw |= gen6_translate_dsa_func(stencil1->func) << 20 |
            gen6_translate_pipe_stencil_op(stencil1->fail_op) << 17 |
            gen6_translate_pipe_stencil_op(stencil1->zfail_op) << 14 |
            gen6_translate_pipe_stencil_op(stencil1->zpass_op) << 11 |
            GEN8_ZS_DW1_STENCIL1_ENABLE;
      if (stencil1->writemask)
         dw |= GEN8_ZS_DW1_STENCIL_WRITE_ENABLE;
   }

   return dw;
}

static uint32_t
dsa_get_depth_enable_gen6(const struct ilo_dev *dev,
                          const struct pipe_depth_state *state)
{
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 6, 7.5);

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 360:
    *
    *     "Enabling the Depth Test function without defining a Depth Buffer is
    *      UNDEFINED."
    *
    * From the Sandy Bridge PRM, volume 2 part 1, page 375:
    *
    *     "A Depth Buffer must be defined before enabling writes to it, or
    *      operation is UNDEFINED."
    *
    * TODO We do not check these yet.
    */
   if (state->enabled) {
      dw = GEN6_ZS_DW2_DEPTH_TEST_ENABLE |
           gen6_translate_dsa_func(state->func) << 27;
   } else {
      dw = GEN6_COMPAREFUNCTION_ALWAYS << 27;
   }

   if (state->writemask)
      dw |= GEN6_ZS_DW2_DEPTH_WRITE_ENABLE;

   return dw;
}

static uint32_t
dsa_get_depth_enable_gen8(const struct ilo_dev *dev,
                          const struct pipe_depth_state *state)
{
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 8, 8);

   if (state->enabled) {
      dw = GEN8_ZS_DW1_DEPTH_TEST_ENABLE |
           gen6_translate_dsa_func(state->func) << 5;
   } else {
      dw = GEN6_COMPAREFUNCTION_ALWAYS << 5;
   }

   if (state->writemask)
      dw |= GEN8_ZS_DW1_DEPTH_WRITE_ENABLE;

   return dw;
}

static uint32_t
dsa_get_alpha_enable_gen6(const struct ilo_dev *dev,
                          const struct pipe_alpha_state *state)
{
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 6, 7.5);

   if (!state->enabled)
      return 0;

   /* this will be ORed to BLEND_STATE */
   dw = GEN6_RT_DW1_ALPHA_TEST_ENABLE |
        gen6_translate_dsa_func(state->func) << 13;

   return dw;
}

static uint32_t
dsa_get_alpha_enable_gen8(const struct ilo_dev *dev,
                          const struct pipe_alpha_state *state)
{
   uint32_t dw;

   ILO_DEV_ASSERT(dev, 8, 8);

   if (!state->enabled)
      return 0;

   /* this will be ORed to BLEND_STATE */
   dw = GEN8_BLEND_DW0_ALPHA_TEST_ENABLE |
        gen6_translate_dsa_func(state->func) << 24;

   return dw;
}

void
ilo_gpe_init_dsa(const struct ilo_dev *dev,
                 const struct pipe_depth_stencil_alpha_state *state,
                 struct ilo_dsa_state *dsa)
{
   ILO_DEV_ASSERT(dev, 6, 8);

   STATIC_ASSERT(Elements(dsa->payload) >= 3);

   if (ilo_dev_gen(dev) >= ILO_GEN(8)) {
      const uint32_t dw_stencil = dsa_get_stencil_enable_gen8(dev,
            &state->stencil[0], &state->stencil[1]);
      const uint32_t dw_depth = dsa_get_depth_enable_gen8(dev, &state->depth);

      assert(!(dw_stencil & dw_depth));
      dsa->payload[0] = dw_stencil | dw_depth;

      dsa->dw_blend_alpha = dsa_get_alpha_enable_gen8(dev, &state->alpha);
      dsa->dw_ps_blend_alpha = (state->alpha.enabled) ?
         GEN8_PS_BLEND_DW1_ALPHA_TEST_ENABLE : 0;
   } else {
      dsa->payload[0] = dsa_get_stencil_enable_gen6(dev,
            &state->stencil[0], &state->stencil[1]);
      dsa->payload[2] = dsa_get_depth_enable_gen6(dev, &state->depth);

      dsa->dw_blend_alpha = dsa_get_alpha_enable_gen6(dev, &state->alpha);
      dsa->dw_ps_blend_alpha = 0;
   }

   dsa->payload[1] = state->stencil[0].valuemask << 24 |
                     state->stencil[0].writemask << 16 |
                     state->stencil[1].valuemask << 8 |
                     state->stencil[1].writemask;

   dsa->alpha_ref = float_to_ubyte(state->alpha.ref_value);
}

static void
fb_set_blend_caps(const struct ilo_dev *dev,
                  enum pipe_format format,
                  struct ilo_fb_blend_caps *caps)
{
   const struct util_format_description *desc =
      util_format_description(format);
   const int ch = util_format_get_first_non_void_channel(format);

   memset(caps, 0, sizeof(*caps));

   if (format == PIPE_FORMAT_NONE || desc->is_mixed)
      return;

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 365:
    *
    *     "Logic Ops are only supported on *_UNORM surfaces (excluding _SRGB
    *      variants), otherwise Logic Ops must be DISABLED."
    *
    * According to the classic driver, this is lifted on Gen8+.
    */
   if (ilo_dev_gen(dev) >= ILO_GEN(8)) {
      caps->can_logicop = true;
   } else {
      caps->can_logicop = (ch >= 0 && desc->channel[ch].normalized &&
            desc->channel[ch].type == UTIL_FORMAT_TYPE_UNSIGNED &&
            desc->colorspace == UTIL_FORMAT_COLORSPACE_RGB);
   }

   /* no blending for pure integer formats */
   caps->can_blend = !util_format_is_pure_integer(format);

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 382:
    *
    *     "Alpha Test can only be enabled if Pixel Shader outputs a float
    *      alpha value."
    */
   caps->can_alpha_test = !util_format_is_pure_integer(format);

   caps->dst_alpha_forced_one =
      (ilo_format_translate_render(dev, format) !=
       ilo_format_translate_color(dev, format));

   /* sanity check */
   if (caps->dst_alpha_forced_one) {
      enum pipe_format render_format;

      switch (format) {
      case PIPE_FORMAT_B8G8R8X8_UNORM:
         render_format = PIPE_FORMAT_B8G8R8A8_UNORM;
         break;
      default:
         render_format = PIPE_FORMAT_NONE;
         break;
      }

      assert(ilo_format_translate_render(dev, format) ==
             ilo_format_translate_color(dev, render_format));
   }
}

void
ilo_gpe_set_fb(const struct ilo_dev *dev,
               const struct pipe_framebuffer_state *state,
               struct ilo_fb_state *fb)
{
   const struct pipe_surface *first_surf = NULL;
   int i;

   ILO_DEV_ASSERT(dev, 6, 8);

   util_copy_framebuffer_state(&fb->state, state);

   for (i = 0; i < state->nr_cbufs; i++) {
      if (state->cbufs[i]) {
         fb_set_blend_caps(dev, state->cbufs[i]->format, &fb->blend_caps[i]);

         if (!first_surf)
            first_surf = state->cbufs[i];
      } else {
         fb_set_blend_caps(dev, PIPE_FORMAT_NONE, &fb->blend_caps[i]);
      }
   }

   if (!first_surf && state->zsbuf)
      first_surf = state->zsbuf;

   fb->num_samples = (first_surf) ? first_surf->texture->nr_samples : 1;
   if (!fb->num_samples)
      fb->num_samples = 1;

   /*
    * The PRMs list several restrictions when the framebuffer has more than
    * one surface.  It seems they are actually lifted on GEN6+.
    */
}
