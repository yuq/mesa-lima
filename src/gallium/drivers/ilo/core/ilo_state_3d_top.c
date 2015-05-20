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
#include "util/u_resource.h"

#include "ilo_buffer.h"
#include "ilo_format.h"
#include "ilo_image.h"
#include "ilo_state_3d.h"
#include "../ilo_shader.h"

static void
ve_init_cso(const struct ilo_dev *dev,
            const struct pipe_vertex_element *state,
            unsigned vb_index,
            struct ilo_ve_cso *cso)
{
   int comp[4] = {
      GEN6_VFCOMP_STORE_SRC,
      GEN6_VFCOMP_STORE_SRC,
      GEN6_VFCOMP_STORE_SRC,
      GEN6_VFCOMP_STORE_SRC,
   };
   int format;

   ILO_DEV_ASSERT(dev, 6, 8);

   switch (util_format_get_nr_components(state->src_format)) {
   case 1: comp[1] = GEN6_VFCOMP_STORE_0;
   case 2: comp[2] = GEN6_VFCOMP_STORE_0;
   case 3: comp[3] = (util_format_is_pure_integer(state->src_format)) ?
                     GEN6_VFCOMP_STORE_1_INT :
                     GEN6_VFCOMP_STORE_1_FP;
   }

   format = ilo_format_translate_vertex(dev, state->src_format);

   STATIC_ASSERT(Elements(cso->payload) >= 2);
   cso->payload[0] =
      vb_index << GEN6_VE_DW0_VB_INDEX__SHIFT |
      GEN6_VE_DW0_VALID |
      format << GEN6_VE_DW0_FORMAT__SHIFT |
      state->src_offset << GEN6_VE_DW0_VB_OFFSET__SHIFT;

   cso->payload[1] =
         comp[0] << GEN6_VE_DW1_COMP0__SHIFT |
         comp[1] << GEN6_VE_DW1_COMP1__SHIFT |
         comp[2] << GEN6_VE_DW1_COMP2__SHIFT |
         comp[3] << GEN6_VE_DW1_COMP3__SHIFT;
}

void
ilo_gpe_init_ve(const struct ilo_dev *dev,
                unsigned num_states,
                const struct pipe_vertex_element *states,
                struct ilo_ve_state *ve)
{
   unsigned i;

   ILO_DEV_ASSERT(dev, 6, 8);

   ve->count = num_states;
   ve->vb_count = 0;

   for (i = 0; i < num_states; i++) {
      const unsigned pipe_idx = states[i].vertex_buffer_index;
      const unsigned instance_divisor = states[i].instance_divisor;
      unsigned hw_idx;

      /*
       * map the pipe vb to the hardware vb, which has a fixed instance
       * divisor
       */
      for (hw_idx = 0; hw_idx < ve->vb_count; hw_idx++) {
         if (ve->vb_mapping[hw_idx] == pipe_idx &&
             ve->instance_divisors[hw_idx] == instance_divisor)
            break;
      }

      /* create one if there is no matching hardware vb */
      if (hw_idx >= ve->vb_count) {
         hw_idx = ve->vb_count++;

         ve->vb_mapping[hw_idx] = pipe_idx;
         ve->instance_divisors[hw_idx] = instance_divisor;
      }

      ve_init_cso(dev, &states[i], hw_idx, &ve->cso[i]);
   }
}

void
ilo_gpe_set_ve_edgeflag(const struct ilo_dev *dev,
                        struct ilo_ve_cso *cso)
{
   int format;

   ILO_DEV_ASSERT(dev, 6, 8);

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 94:
    *
    *     "- This bit (Edge Flag Enable) must only be ENABLED on the last
    *        valid VERTEX_ELEMENT structure.
    *
    *      - When set, Component 0 Control must be set to VFCOMP_STORE_SRC,
    *        and Component 1-3 Control must be set to VFCOMP_NOSTORE.
    *
    *      - The Source Element Format must be set to the UINT format.
    *
    *      - [DevSNB]: Edge Flags are not supported for QUADLIST
    *        primitives.  Software may elect to convert QUADLIST primitives
    *        to some set of corresponding edge-flag-supported primitive
    *        types (e.g., POLYGONs) prior to submission to the 3D pipeline."
    */
   cso->payload[0] |= GEN6_VE_DW0_EDGE_FLAG_ENABLE;

   /*
    * Edge flags have format GEN6_FORMAT_R8_USCALED when defined via
    * glEdgeFlagPointer(), and format GEN6_FORMAT_R32_FLOAT when defined
    * via glEdgeFlag(), as can be seen in vbo_attrib_tmp.h.
    *
    * Since all the hardware cares about is whether the flags are zero or not,
    * we can treat them as the corresponding _UINT formats.
    */
   format = GEN_EXTRACT(cso->payload[0], GEN6_VE_DW0_FORMAT);
   cso->payload[0] &= ~GEN6_VE_DW0_FORMAT__MASK;

   switch (format) {
   case GEN6_FORMAT_R32_FLOAT:
      format = GEN6_FORMAT_R32_UINT;
      break;
   case GEN6_FORMAT_R8_USCALED:
      format = GEN6_FORMAT_R8_UINT;
      break;
   default:
      break;
   }

   cso->payload[0] |= GEN_SHIFT32(format, GEN6_VE_DW0_FORMAT);

   cso->payload[1] =
         GEN6_VFCOMP_STORE_SRC << GEN6_VE_DW1_COMP0__SHIFT |
         GEN6_VFCOMP_NOSTORE << GEN6_VE_DW1_COMP1__SHIFT |
         GEN6_VFCOMP_NOSTORE << GEN6_VE_DW1_COMP2__SHIFT |
         GEN6_VFCOMP_NOSTORE << GEN6_VE_DW1_COMP3__SHIFT;
}

void
ilo_gpe_init_ve_nosrc(const struct ilo_dev *dev,
                          int comp0, int comp1, int comp2, int comp3,
                          struct ilo_ve_cso *cso)
{
   ILO_DEV_ASSERT(dev, 6, 8);

   STATIC_ASSERT(Elements(cso->payload) >= 2);

   assert(comp0 != GEN6_VFCOMP_STORE_SRC &&
          comp1 != GEN6_VFCOMP_STORE_SRC &&
          comp2 != GEN6_VFCOMP_STORE_SRC &&
          comp3 != GEN6_VFCOMP_STORE_SRC);

   cso->payload[0] = GEN6_VE_DW0_VALID;
   cso->payload[1] =
         comp0 << GEN6_VE_DW1_COMP0__SHIFT |
         comp1 << GEN6_VE_DW1_COMP1__SHIFT |
         comp2 << GEN6_VE_DW1_COMP2__SHIFT |
         comp3 << GEN6_VE_DW1_COMP3__SHIFT;
}

void
ilo_gpe_init_vs_cso(const struct ilo_dev *dev,
                    const struct ilo_shader_state *vs,
                    struct ilo_shader_cso *cso)
{
   int start_grf, vue_read_len, sampler_count, max_threads;
   uint32_t dw2, dw4, dw5;

   ILO_DEV_ASSERT(dev, 6, 8);

   start_grf = ilo_shader_get_kernel_param(vs, ILO_KERNEL_URB_DATA_START_REG);
   vue_read_len = ilo_shader_get_kernel_param(vs, ILO_KERNEL_INPUT_COUNT);
   sampler_count = ilo_shader_get_kernel_param(vs, ILO_KERNEL_SAMPLER_COUNT);

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 135:
    *
    *     "(Vertex URB Entry Read Length) Specifies the number of pairs of
    *      128-bit vertex elements to be passed into the payload for each
    *      vertex."
    *
    *     "It is UNDEFINED to set this field to 0 indicating no Vertex URB
    *      data to be read and passed to the thread."
    */
   vue_read_len = (vue_read_len + 1) / 2;
   if (!vue_read_len)
      vue_read_len = 1;

   max_threads = dev->thread_count;
   if (ilo_dev_gen(dev) == ILO_GEN(7.5) && dev->gt == 2)
      max_threads *= 2;

   dw2 = (true) ? 0 : GEN6_THREADDISP_FP_MODE_ALT;
   dw2 |= ((sampler_count + 3) / 4) << GEN6_THREADDISP_SAMPLER_COUNT__SHIFT;

   dw4 = start_grf << GEN6_VS_DW4_URB_GRF_START__SHIFT |
         vue_read_len << GEN6_VS_DW4_URB_READ_LEN__SHIFT |
         0 << GEN6_VS_DW4_URB_READ_OFFSET__SHIFT;

   dw5 = GEN6_VS_DW5_STATISTICS |
         GEN6_VS_DW5_VS_ENABLE;

   if (ilo_dev_gen(dev) >= ILO_GEN(7.5))
      dw5 |= (max_threads - 1) << GEN75_VS_DW5_MAX_THREADS__SHIFT;
   else
      dw5 |= (max_threads - 1) << GEN6_VS_DW5_MAX_THREADS__SHIFT;

   STATIC_ASSERT(Elements(cso->payload) >= 3);
   cso->payload[0] = dw2;
   cso->payload[1] = dw4;
   cso->payload[2] = dw5;
}

static void
gs_init_cso_gen6(const struct ilo_dev *dev,
                 const struct ilo_shader_state *gs,
                 struct ilo_shader_cso *cso)
{
   int start_grf, vue_read_len, max_threads;
   uint32_t dw2, dw4, dw5, dw6;

   ILO_DEV_ASSERT(dev, 6, 6);

   if (ilo_shader_get_type(gs) == PIPE_SHADER_GEOMETRY) {
      start_grf = ilo_shader_get_kernel_param(gs,
            ILO_KERNEL_URB_DATA_START_REG);

      vue_read_len = ilo_shader_get_kernel_param(gs, ILO_KERNEL_INPUT_COUNT);
   }
   else {
      start_grf = ilo_shader_get_kernel_param(gs,
            ILO_KERNEL_VS_GEN6_SO_START_REG);

      vue_read_len = ilo_shader_get_kernel_param(gs, ILO_KERNEL_OUTPUT_COUNT);
   }

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 153:
    *
    *     "Specifies the amount of URB data read and passed in the thread
    *      payload for each Vertex URB entry, in 256-bit register increments.
    *
    *      It is UNDEFINED to set this field (Vertex URB Entry Read Length) to
    *      0 indicating no Vertex URB data to be read and passed to the
    *      thread."
    */
   vue_read_len = (vue_read_len + 1) / 2;
   if (!vue_read_len)
      vue_read_len = 1;

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 154:
    *
    *     "Maximum Number of Threads valid range is [0,27] when Rendering
    *      Enabled bit is set."
    *
    * From the Sandy Bridge PRM, volume 2 part 1, page 173:
    *
    *     "Programming Note: If the GS stage is enabled, software must always
    *      allocate at least one GS URB Entry. This is true even if the GS
    *      thread never needs to output vertices to the pipeline, e.g., when
    *      only performing stream output. This is an artifact of the need to
    *      pass the GS thread an initial destination URB handle."
    *
    * As such, we always enable rendering, and limit the number of threads.
    */
   if (dev->gt == 2) {
      /* maximum is 60, but limited to 28 */
      max_threads = 28;
   }
   else {
      /* maximum is 24, but limited to 21 (see brwCreateContext()) */
      max_threads = 21;
   }

   dw2 = GEN6_THREADDISP_SPF;

   dw4 = vue_read_len << GEN6_GS_DW4_URB_READ_LEN__SHIFT |
         0 << GEN6_GS_DW4_URB_READ_OFFSET__SHIFT |
         start_grf << GEN6_GS_DW4_URB_GRF_START__SHIFT;

   dw5 = (max_threads - 1) << GEN6_GS_DW5_MAX_THREADS__SHIFT |
         GEN6_GS_DW5_STATISTICS |
         GEN6_GS_DW5_SO_STATISTICS |
         GEN6_GS_DW5_RENDER_ENABLE;

   /*
    * we cannot make use of GEN6_GS_REORDER because it will reorder
    * triangle strips according to D3D rules (triangle 2N+1 uses vertices
    * (2N+1, 2N+3, 2N+2)), instead of GL rules (triangle 2N+1 uses vertices
    * (2N+2, 2N+1, 2N+3)).
    */
   dw6 = GEN6_GS_DW6_GS_ENABLE;

   if (ilo_shader_get_kernel_param(gs, ILO_KERNEL_GS_DISCARD_ADJACENCY))
      dw6 |= GEN6_GS_DW6_DISCARD_ADJACENCY;

   if (ilo_shader_get_kernel_param(gs, ILO_KERNEL_VS_GEN6_SO)) {
      const uint32_t svbi_post_inc =
         ilo_shader_get_kernel_param(gs, ILO_KERNEL_GS_GEN6_SVBI_POST_INC);

      dw6 |= GEN6_GS_DW6_SVBI_PAYLOAD_ENABLE;
      if (svbi_post_inc) {
         dw6 |= GEN6_GS_DW6_SVBI_POST_INC_ENABLE |
                svbi_post_inc << GEN6_GS_DW6_SVBI_POST_INC_VAL__SHIFT;
      }
   }

   STATIC_ASSERT(Elements(cso->payload) >= 4);
   cso->payload[0] = dw2;
   cso->payload[1] = dw4;
   cso->payload[2] = dw5;
   cso->payload[3] = dw6;
}

static void
gs_init_cso_gen7(const struct ilo_dev *dev,
                 const struct ilo_shader_state *gs,
                 struct ilo_shader_cso *cso)
{
   int start_grf, vue_read_len, sampler_count, max_threads;
   uint32_t dw2, dw4, dw5;

   ILO_DEV_ASSERT(dev, 7, 7.5);

   start_grf = ilo_shader_get_kernel_param(gs, ILO_KERNEL_URB_DATA_START_REG);
   vue_read_len = ilo_shader_get_kernel_param(gs, ILO_KERNEL_INPUT_COUNT);
   sampler_count = ilo_shader_get_kernel_param(gs, ILO_KERNEL_SAMPLER_COUNT);

   /* in pairs */
   vue_read_len = (vue_read_len + 1) / 2;

   switch (ilo_dev_gen(dev)) {
   case ILO_GEN(7.5):
      max_threads = (dev->gt >= 2) ? 256 : 70;
      break;
   case ILO_GEN(7):
      max_threads = (dev->gt == 2) ? 128 : 36;
      break;
   default:
      max_threads = 1;
      break;
   }

   dw2 = (true) ? 0 : GEN6_THREADDISP_FP_MODE_ALT;
   dw2 |= ((sampler_count + 3) / 4) << GEN6_THREADDISP_SAMPLER_COUNT__SHIFT;

   dw4 = vue_read_len << GEN7_GS_DW4_URB_READ_LEN__SHIFT |
         GEN7_GS_DW4_INCLUDE_VERTEX_HANDLES |
         0 << GEN7_GS_DW4_URB_READ_OFFSET__SHIFT |
         start_grf << GEN7_GS_DW4_URB_GRF_START__SHIFT;

   dw5 = (max_threads - 1) << GEN7_GS_DW5_MAX_THREADS__SHIFT |
         GEN7_GS_DW5_STATISTICS |
         GEN7_GS_DW5_GS_ENABLE;

   STATIC_ASSERT(Elements(cso->payload) >= 3);
   cso->payload[0] = dw2;
   cso->payload[1] = dw4;
   cso->payload[2] = dw5;
}

void
ilo_gpe_init_gs_cso(const struct ilo_dev *dev,
                    const struct ilo_shader_state *gs,
                    struct ilo_shader_cso *cso)
{
   if (ilo_dev_gen(dev) >= ILO_GEN(7))
      gs_init_cso_gen7(dev, gs, cso);
   else
      gs_init_cso_gen6(dev, gs, cso);
}

static void
sampler_init_border_color_gen6(const struct ilo_dev *dev,
                               const union pipe_color_union *color,
                               uint32_t *dw, int num_dwords)
{
   float rgba[4] = {
      color->f[0], color->f[1], color->f[2], color->f[3],
   };

   ILO_DEV_ASSERT(dev, 6, 6);

   assert(num_dwords >= 12);

   /*
    * This state is not documented in the Sandy Bridge PRM, but in the
    * Ironlake PRM.  SNORM8 seems to be in DW11 instead of DW1.
    */

   /* IEEE_FP */
   dw[1] = fui(rgba[0]);
   dw[2] = fui(rgba[1]);
   dw[3] = fui(rgba[2]);
   dw[4] = fui(rgba[3]);

   /* FLOAT_16 */
   dw[5] = util_float_to_half(rgba[0]) |
           util_float_to_half(rgba[1]) << 16;
   dw[6] = util_float_to_half(rgba[2]) |
           util_float_to_half(rgba[3]) << 16;

   /* clamp to [-1.0f, 1.0f] */
   rgba[0] = CLAMP(rgba[0], -1.0f, 1.0f);
   rgba[1] = CLAMP(rgba[1], -1.0f, 1.0f);
   rgba[2] = CLAMP(rgba[2], -1.0f, 1.0f);
   rgba[3] = CLAMP(rgba[3], -1.0f, 1.0f);

   /* SNORM16 */
   dw[9] =  (int16_t) util_iround(rgba[0] * 32767.0f) |
            (int16_t) util_iround(rgba[1] * 32767.0f) << 16;
   dw[10] = (int16_t) util_iround(rgba[2] * 32767.0f) |
            (int16_t) util_iround(rgba[3] * 32767.0f) << 16;

   /* SNORM8 */
   dw[11] = (int8_t) util_iround(rgba[0] * 127.0f) |
            (int8_t) util_iround(rgba[1] * 127.0f) << 8 |
            (int8_t) util_iround(rgba[2] * 127.0f) << 16 |
            (int8_t) util_iround(rgba[3] * 127.0f) << 24;

   /* clamp to [0.0f, 1.0f] */
   rgba[0] = CLAMP(rgba[0], 0.0f, 1.0f);
   rgba[1] = CLAMP(rgba[1], 0.0f, 1.0f);
   rgba[2] = CLAMP(rgba[2], 0.0f, 1.0f);
   rgba[3] = CLAMP(rgba[3], 0.0f, 1.0f);

   /* UNORM8 */
   dw[0] = (uint8_t) util_iround(rgba[0] * 255.0f) |
           (uint8_t) util_iround(rgba[1] * 255.0f) << 8 |
           (uint8_t) util_iround(rgba[2] * 255.0f) << 16 |
           (uint8_t) util_iround(rgba[3] * 255.0f) << 24;

   /* UNORM16 */
   dw[7] = (uint16_t) util_iround(rgba[0] * 65535.0f) |
           (uint16_t) util_iround(rgba[1] * 65535.0f) << 16;
   dw[8] = (uint16_t) util_iround(rgba[2] * 65535.0f) |
           (uint16_t) util_iround(rgba[3] * 65535.0f) << 16;
}

/**
 * Translate a pipe texture mipfilter to the matching hardware mipfilter.
 */
static int
gen6_translate_tex_mipfilter(unsigned filter)
{
   switch (filter) {
   case PIPE_TEX_MIPFILTER_NEAREST: return GEN6_MIPFILTER_NEAREST;
   case PIPE_TEX_MIPFILTER_LINEAR:  return GEN6_MIPFILTER_LINEAR;
   case PIPE_TEX_MIPFILTER_NONE:    return GEN6_MIPFILTER_NONE;
   default:
      assert(!"unknown mipfilter");
      return GEN6_MIPFILTER_NONE;
   }
}

/**
 * Translate a pipe texture filter to the matching hardware mapfilter.
 */
static int
gen6_translate_tex_filter(unsigned filter)
{
   switch (filter) {
   case PIPE_TEX_FILTER_NEAREST: return GEN6_MAPFILTER_NEAREST;
   case PIPE_TEX_FILTER_LINEAR:  return GEN6_MAPFILTER_LINEAR;
   default:
      assert(!"unknown sampler filter");
      return GEN6_MAPFILTER_NEAREST;
   }
}

/**
 * Translate a pipe texture coordinate wrapping mode to the matching hardware
 * wrapping mode.
 */
static int
gen6_translate_tex_wrap(unsigned wrap)
{
   switch (wrap) {
   case PIPE_TEX_WRAP_CLAMP:              return GEN8_TEXCOORDMODE_HALF_BORDER;
   case PIPE_TEX_WRAP_REPEAT:             return GEN6_TEXCOORDMODE_WRAP;
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:      return GEN6_TEXCOORDMODE_CLAMP;
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:    return GEN6_TEXCOORDMODE_CLAMP_BORDER;
   case PIPE_TEX_WRAP_MIRROR_REPEAT:      return GEN6_TEXCOORDMODE_MIRROR;
   case PIPE_TEX_WRAP_MIRROR_CLAMP:
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER:
   default:
      assert(!"unknown sampler wrap mode");
      return GEN6_TEXCOORDMODE_WRAP;
   }
}

/**
 * Translate a pipe shadow compare function to the matching hardware shadow
 * function.
 */
static int
gen6_translate_shadow_func(unsigned func)
{
   /*
    * For PIPE_FUNC_x, the reference value is on the left-hand side of the
    * comparison, and 1.0 is returned when the comparison is true.
    *
    * For GEN6_COMPAREFUNCTION_x, the reference value is on the right-hand side of
    * the comparison, and 0.0 is returned when the comparison is true.
    */
   switch (func) {
   case PIPE_FUNC_NEVER:      return GEN6_COMPAREFUNCTION_ALWAYS;
   case PIPE_FUNC_LESS:       return GEN6_COMPAREFUNCTION_LEQUAL;
   case PIPE_FUNC_EQUAL:      return GEN6_COMPAREFUNCTION_NOTEQUAL;
   case PIPE_FUNC_LEQUAL:     return GEN6_COMPAREFUNCTION_LESS;
   case PIPE_FUNC_GREATER:    return GEN6_COMPAREFUNCTION_GEQUAL;
   case PIPE_FUNC_NOTEQUAL:   return GEN6_COMPAREFUNCTION_EQUAL;
   case PIPE_FUNC_GEQUAL:     return GEN6_COMPAREFUNCTION_GREATER;
   case PIPE_FUNC_ALWAYS:     return GEN6_COMPAREFUNCTION_NEVER;
   default:
      assert(!"unknown shadow compare function");
      return GEN6_COMPAREFUNCTION_NEVER;
   }
}

void
ilo_gpe_init_sampler_cso(const struct ilo_dev *dev,
                         const struct pipe_sampler_state *state,
                         struct ilo_sampler_cso *sampler)
{
   int mip_filter, min_filter, mag_filter, max_aniso;
   int lod_bias, max_lod, min_lod;
   int wrap_s, wrap_t, wrap_r, wrap_cube;
   uint32_t dw0, dw1, dw3;

   ILO_DEV_ASSERT(dev, 6, 8);

   memset(sampler, 0, sizeof(*sampler));

   mip_filter = gen6_translate_tex_mipfilter(state->min_mip_filter);
   min_filter = gen6_translate_tex_filter(state->min_img_filter);
   mag_filter = gen6_translate_tex_filter(state->mag_img_filter);

   sampler->anisotropic = state->max_anisotropy;

   if (state->max_anisotropy >= 2 && state->max_anisotropy <= 16)
      max_aniso = state->max_anisotropy / 2 - 1;
   else if (state->max_anisotropy > 16)
      max_aniso = GEN6_ANISORATIO_16;
   else
      max_aniso = GEN6_ANISORATIO_2;

   /*
    *
    * Here is how the hardware calculate per-pixel LOD, from my reading of the
    * PRMs:
    *
    *  1) LOD is set to log2(ratio of texels to pixels) if not specified in
    *     other ways.  The number of texels is measured using level
    *     SurfMinLod.
    *  2) Bias is added to LOD.
    *  3) LOD is clamped to [MinLod, MaxLod], and the clamped value is
    *     compared with Base to determine whether magnification or
    *     minification is needed.  (if preclamp is disabled, LOD is compared
    *     with Base before clamping)
    *  4) If magnification is needed, or no mipmapping is requested, LOD is
    *     set to floor(MinLod).
    *  5) LOD is clamped to [0, MIPCnt], and SurfMinLod is added to LOD.
    *
    * With Gallium interface, Base is always zero and
    * pipe_sampler_view::u.tex.first_level specifies SurfMinLod.
    */
   if (ilo_dev_gen(dev) >= ILO_GEN(7)) {
      const float scale = 256.0f;

      /* [-16.0, 16.0) in S4.8 */
      lod_bias = (int)
         (CLAMP(state->lod_bias, -16.0f, 15.9f) * scale);
      lod_bias &= 0x1fff;

      /* [0.0, 14.0] in U4.8 */
      max_lod = (int) (CLAMP(state->max_lod, 0.0f, 14.0f) * scale);
      min_lod = (int) (CLAMP(state->min_lod, 0.0f, 14.0f) * scale);
   }
   else {
      const float scale = 64.0f;

      /* [-16.0, 16.0) in S4.6 */
      lod_bias = (int)
         (CLAMP(state->lod_bias, -16.0f, 15.9f) * scale);
      lod_bias &= 0x7ff;

      /* [0.0, 13.0] in U4.6 */
      max_lod = (int) (CLAMP(state->max_lod, 0.0f, 13.0f) * scale);
      min_lod = (int) (CLAMP(state->min_lod, 0.0f, 13.0f) * scale);
   }

   /*
    * We want LOD to be clamped to determine magnification/minification, and
    * get set to zero when it is magnification or when mipmapping is disabled.
    * The hardware would set LOD to floor(MinLod) and that is a problem when
    * MinLod is greater than or equal to 1.0f.
    *
    * With Base being zero, it is always minification when MinLod is non-zero.
    * To achieve our goal, we just need to set MinLod to zero and set
    * MagFilter to MinFilter when mipmapping is disabled.
    */
   if (state->min_mip_filter == PIPE_TEX_MIPFILTER_NONE && min_lod) {
      min_lod = 0;
      mag_filter = min_filter;
   }

   /* determine wrap s/t/r */
   wrap_s = gen6_translate_tex_wrap(state->wrap_s);
   wrap_t = gen6_translate_tex_wrap(state->wrap_t);
   wrap_r = gen6_translate_tex_wrap(state->wrap_r);
   if (ilo_dev_gen(dev) < ILO_GEN(8)) {
      /*
       * For nearest filtering, PIPE_TEX_WRAP_CLAMP means
       * PIPE_TEX_WRAP_CLAMP_TO_EDGE;  for linear filtering,
       * PIPE_TEX_WRAP_CLAMP means PIPE_TEX_WRAP_CLAMP_TO_BORDER while
       * additionally clamping the texture coordinates to [0.0, 1.0].
       *
       * PIPE_TEX_WRAP_CLAMP is not supported natively until Gen8.  The
       * clamping has to be taken care of in the shaders.  There are two
       * filters here, but let the minification one has a say.
       */
      const bool clamp_is_to_edge =
         (state->min_img_filter == PIPE_TEX_FILTER_NEAREST);

      if (clamp_is_to_edge) {
         if (wrap_s == GEN8_TEXCOORDMODE_HALF_BORDER)
            wrap_s = GEN6_TEXCOORDMODE_CLAMP;
         if (wrap_t == GEN8_TEXCOORDMODE_HALF_BORDER)
            wrap_t = GEN6_TEXCOORDMODE_CLAMP;
         if (wrap_r == GEN8_TEXCOORDMODE_HALF_BORDER)
            wrap_r = GEN6_TEXCOORDMODE_CLAMP;
      } else {
         if (wrap_s == GEN8_TEXCOORDMODE_HALF_BORDER) {
            wrap_s = GEN6_TEXCOORDMODE_CLAMP_BORDER;
            sampler->saturate_s = true;
         }
         if (wrap_t == GEN8_TEXCOORDMODE_HALF_BORDER) {
            wrap_t = GEN6_TEXCOORDMODE_CLAMP_BORDER;
            sampler->saturate_t = true;
         }
         if (wrap_r == GEN8_TEXCOORDMODE_HALF_BORDER) {
            wrap_r = GEN6_TEXCOORDMODE_CLAMP_BORDER;
            sampler->saturate_r = true;
         }
      }
   }

   /*
    * From the Sandy Bridge PRM, volume 4 part 1, page 107:
    *
    *     "When using cube map texture coordinates, only TEXCOORDMODE_CLAMP
    *      and TEXCOORDMODE_CUBE settings are valid, and each TC component
    *      must have the same Address Control mode."
    *
    * From the Ivy Bridge PRM, volume 4 part 1, page 96:
    *
    *     "This field (Cube Surface Control Mode) must be set to
    *      CUBECTRLMODE_PROGRAMMED"
    *
    * Therefore, we cannot use "Cube Surface Control Mode" for semless cube
    * map filtering.
    */
   if (state->seamless_cube_map &&
       (state->min_img_filter != PIPE_TEX_FILTER_NEAREST ||
        state->mag_img_filter != PIPE_TEX_FILTER_NEAREST)) {
      wrap_cube = GEN6_TEXCOORDMODE_CUBE;
   }
   else {
      wrap_cube = GEN6_TEXCOORDMODE_CLAMP;
   }

   if (!state->normalized_coords) {
      /*
       * From the Ivy Bridge PRM, volume 4 part 1, page 98:
       *
       *     "The following state must be set as indicated if this field
       *      (Non-normalized Coordinate Enable) is enabled:
       *
       *      - TCX/Y/Z Address Control Mode must be TEXCOORDMODE_CLAMP,
       *        TEXCOORDMODE_HALF_BORDER, or TEXCOORDMODE_CLAMP_BORDER.
       *      - Surface Type must be SURFTYPE_2D or SURFTYPE_3D.
       *      - Mag Mode Filter must be MAPFILTER_NEAREST or
       *        MAPFILTER_LINEAR.
       *      - Min Mode Filter must be MAPFILTER_NEAREST or
       *        MAPFILTER_LINEAR.
       *      - Mip Mode Filter must be MIPFILTER_NONE.
       *      - Min LOD must be 0.
       *      - Max LOD must be 0.
       *      - MIP Count must be 0.
       *      - Surface Min LOD must be 0.
       *      - Texture LOD Bias must be 0."
       */
      assert(wrap_s == GEN6_TEXCOORDMODE_CLAMP ||
             wrap_s == GEN6_TEXCOORDMODE_CLAMP_BORDER);
      assert(wrap_t == GEN6_TEXCOORDMODE_CLAMP ||
             wrap_t == GEN6_TEXCOORDMODE_CLAMP_BORDER);
      assert(wrap_r == GEN6_TEXCOORDMODE_CLAMP ||
             wrap_r == GEN6_TEXCOORDMODE_CLAMP_BORDER);

      assert(mag_filter == GEN6_MAPFILTER_NEAREST ||
             mag_filter == GEN6_MAPFILTER_LINEAR);
      assert(min_filter == GEN6_MAPFILTER_NEAREST ||
             min_filter == GEN6_MAPFILTER_LINEAR);

      /* work around a bug in util_blitter */
      mip_filter = GEN6_MIPFILTER_NONE;

      assert(mip_filter == GEN6_MIPFILTER_NONE);
   }

   if (ilo_dev_gen(dev) >= ILO_GEN(7)) {
      dw0 = 1 << 28 |
            mip_filter << 20 |
            lod_bias << 1;

      sampler->dw_filter = mag_filter << 17 |
                           min_filter << 14;

      sampler->dw_filter_aniso = GEN6_MAPFILTER_ANISOTROPIC << 17 |
                                 GEN6_MAPFILTER_ANISOTROPIC << 14 |
                                 1;

      dw1 = min_lod << 20 |
            max_lod << 8;

      if (state->compare_mode != PIPE_TEX_COMPARE_NONE)
         dw1 |= gen6_translate_shadow_func(state->compare_func) << 1;

      dw3 = max_aniso << 19;

      /* round the coordinates for linear filtering */
      if (min_filter != GEN6_MAPFILTER_NEAREST) {
         dw3 |= (GEN6_SAMPLER_DW3_U_MIN_ROUND |
                 GEN6_SAMPLER_DW3_V_MIN_ROUND |
                 GEN6_SAMPLER_DW3_R_MIN_ROUND);
      }
      if (mag_filter != GEN6_MAPFILTER_NEAREST) {
         dw3 |= (GEN6_SAMPLER_DW3_U_MAG_ROUND |
                 GEN6_SAMPLER_DW3_V_MAG_ROUND |
                 GEN6_SAMPLER_DW3_R_MAG_ROUND);
      }

      if (!state->normalized_coords)
         dw3 |= 1 << 10;

      sampler->dw_wrap = wrap_s << 6 |
                         wrap_t << 3 |
                         wrap_r;

      /*
       * As noted in the classic i965 driver, the HW may still reference
       * wrap_t and wrap_r for 1D textures.  We need to set them to a safe
       * mode
       */
      sampler->dw_wrap_1d = wrap_s << 6 |
                            GEN6_TEXCOORDMODE_WRAP << 3 |
                            GEN6_TEXCOORDMODE_WRAP;

      sampler->dw_wrap_cube = wrap_cube << 6 |
                              wrap_cube << 3 |
                              wrap_cube;

      STATIC_ASSERT(Elements(sampler->payload) >= 7);

      sampler->payload[0] = dw0;
      sampler->payload[1] = dw1;
      sampler->payload[2] = dw3;

      memcpy(&sampler->payload[3],
            state->border_color.ui, sizeof(state->border_color.ui));
   }
   else {
      dw0 = 1 << 28 |
            mip_filter << 20 |
            lod_bias << 3;

      if (state->compare_mode != PIPE_TEX_COMPARE_NONE)
         dw0 |= gen6_translate_shadow_func(state->compare_func);

      sampler->dw_filter = (min_filter != mag_filter) << 27 |
                           mag_filter << 17 |
                           min_filter << 14;

      sampler->dw_filter_aniso = GEN6_MAPFILTER_ANISOTROPIC << 17 |
                                 GEN6_MAPFILTER_ANISOTROPIC << 14;

      dw1 = min_lod << 22 |
            max_lod << 12;

      sampler->dw_wrap = wrap_s << 6 |
                         wrap_t << 3 |
                         wrap_r;

      sampler->dw_wrap_1d = wrap_s << 6 |
                            GEN6_TEXCOORDMODE_WRAP << 3 |
                            GEN6_TEXCOORDMODE_WRAP;

      sampler->dw_wrap_cube = wrap_cube << 6 |
                              wrap_cube << 3 |
                              wrap_cube;

      dw3 = max_aniso << 19;

      /* round the coordinates for linear filtering */
      if (min_filter != GEN6_MAPFILTER_NEAREST) {
         dw3 |= (GEN6_SAMPLER_DW3_U_MIN_ROUND |
                 GEN6_SAMPLER_DW3_V_MIN_ROUND |
                 GEN6_SAMPLER_DW3_R_MIN_ROUND);
      }
      if (mag_filter != GEN6_MAPFILTER_NEAREST) {
         dw3 |= (GEN6_SAMPLER_DW3_U_MAG_ROUND |
                 GEN6_SAMPLER_DW3_V_MAG_ROUND |
                 GEN6_SAMPLER_DW3_R_MAG_ROUND);
      }

      if (!state->normalized_coords)
         dw3 |= 1;

      STATIC_ASSERT(Elements(sampler->payload) >= 15);

      sampler->payload[0] = dw0;
      sampler->payload[1] = dw1;
      sampler->payload[2] = dw3;

      sampler_init_border_color_gen6(dev,
            &state->border_color, &sampler->payload[3], 12);
   }
}
