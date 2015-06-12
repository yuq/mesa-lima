/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2014 LunarG, Inc.
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

#ifndef ILO_BUILDER_3D_BOTTOM_H
#define ILO_BUILDER_3D_BOTTOM_H

#include "genhw/genhw.h"
#include "../ilo_shader.h"
#include "intel_winsys.h"

#include "ilo_core.h"
#include "ilo_dev.h"
#include "ilo_format.h"
#include "ilo_state_cc.h"
#include "ilo_state_raster.h"
#include "ilo_state_sbe.h"
#include "ilo_state_viewport.h"
#include "ilo_builder.h"
#include "ilo_builder_3d_top.h"

static inline void
gen6_3DSTATE_CLIP(struct ilo_builder *builder,
                  const struct ilo_state_raster *rs)
{
   const uint8_t cmd_len = 4;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_CLIP) | (cmd_len - 2);
   /* see raster_set_gen6_3DSTATE_CLIP() */
   dw[1] = rs->clip[0];
   dw[2] = rs->clip[1];
   dw[3] = rs->clip[2];
}

static inline void
gen6_3DSTATE_SF(struct ilo_builder *builder,
                const struct ilo_state_raster *rs,
                const struct ilo_state_sbe *sbe)
{
   const uint8_t cmd_len = 20;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 6);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_SF) | (cmd_len - 2);
   /* see sbe_set_gen8_3DSTATE_SBE() */
   dw[1] = sbe->sbe[0];

   /* see raster_set_gen7_3DSTATE_SF() */
   dw[2] = rs->sf[0];
   dw[3] = rs->sf[1];
   dw[4] = rs->sf[2];
   dw[5] = rs->raster[1];
   dw[6] = rs->raster[2];
   dw[7] = rs->raster[3];

   /* see sbe_set_gen8_3DSTATE_SBE_SWIZ() */
   memcpy(&dw[8], sbe->swiz, sizeof(*dw) * 8);

   dw[16] = sbe->sbe[1];
   dw[17] = sbe->sbe[2];
   /* WrapShortest enables */
   dw[18] = 0;
   dw[19] = 0;
}

static inline void
gen7_3DSTATE_SF(struct ilo_builder *builder,
                const struct ilo_state_raster *rs)
{
   const uint8_t cmd_len = (ilo_dev_gen(builder->dev) >= ILO_GEN(8)) ? 4 : 7;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 7, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_SF) | (cmd_len - 2);

   /* see raster_set_gen7_3DSTATE_SF() or raster_set_gen8_3DSTATE_SF() */
   dw[1] = rs->sf[0];
   dw[2] = rs->sf[1];
   dw[3] = rs->sf[2];
   if (ilo_dev_gen(builder->dev) < ILO_GEN(8)) {
      dw[4] = rs->raster[1];
      dw[5] = rs->raster[2];
      dw[6] = rs->raster[3];
   }
}

static inline void
gen7_3DSTATE_SBE(struct ilo_builder *builder,
                 const struct ilo_state_sbe *sbe)
{
   const uint8_t cmd_len = 14;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 7, 7.5);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_SBE) | (cmd_len - 2);
   /* see sbe_set_gen8_3DSTATE_SBE() and sbe_set_gen8_3DSTATE_SBE_SWIZ() */
   dw[1] = sbe->sbe[0];
   memcpy(&dw[2], sbe->swiz, sizeof(*dw) * 8);
   dw[10] = sbe->sbe[1];
   dw[11] = sbe->sbe[2];

   /* WrapShortest enables */
   dw[12] = 0;
   dw[13] = 0;
}

static inline void
gen8_3DSTATE_SBE(struct ilo_builder *builder,
                 const struct ilo_state_sbe *sbe)
{
   const uint8_t cmd_len = 4;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   /* see sbe_set_gen8_3DSTATE_SBE() */
   dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_SBE) | (cmd_len - 2);
   dw[1] = sbe->sbe[0];
   dw[2] = sbe->sbe[1];
   dw[3] = sbe->sbe[2];
}

static inline void
gen8_3DSTATE_SBE_SWIZ(struct ilo_builder *builder,
                      const struct ilo_state_sbe *sbe)
{
   const uint8_t cmd_len = 11;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN8_RENDER_CMD(3D, 3DSTATE_SBE_SWIZ) | (cmd_len - 2);
   /* see sbe_set_gen8_3DSTATE_SBE_SWIZ() */
   memcpy(&dw[1], sbe->swiz, sizeof(*dw) * 8);
   /* WrapShortest enables */
   dw[9] = 0;
   dw[10] = 0;
}

static inline void
gen8_3DSTATE_RASTER(struct ilo_builder *builder,
                    const struct ilo_state_raster *rs)
{
   const uint8_t cmd_len = 5;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN8_RENDER_CMD(3D, 3DSTATE_RASTER) | (cmd_len - 2);
   /* see raster_set_gen8_3DSTATE_RASTER() */
   dw[1] = rs->raster[0];
   dw[2] = rs->raster[1];
   dw[3] = rs->raster[2];
   dw[4] = rs->raster[3];
}

static inline void
gen6_3DSTATE_WM(struct ilo_builder *builder,
                const struct ilo_state_raster *rs,
                const struct ilo_shader_state *fs,
                bool dual_blend, bool cc_may_kill)
{
   const uint8_t cmd_len = 9;
   const bool multisample = false;
   const int num_samples = 1;
   uint32_t dw2, dw4, dw5, dw6, *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 6);

   dw2 = 0;
   /* see raster_set_gen6_3dstate_wm() */
   dw4 = rs->raster[0];
   dw5 = rs->raster[1];
   dw6 = rs->raster[2];

   if (fs) {
      const union ilo_shader_cso *cso;

      cso = ilo_shader_get_kernel_cso(fs);
      /* see fs_init_cso_gen6() */
      dw2 |= cso->ps_payload[0];
      dw4 |= cso->ps_payload[1];
      dw5 |= cso->ps_payload[2];
      dw6 |= cso->ps_payload[3];
   } else {
      const int max_threads = (builder->dev->gt == 2) ? 80 : 40;

      /* honor the valid range even if dispatching is disabled */
      dw5 |= (max_threads - 1) << GEN6_WM_DW5_MAX_THREADS__SHIFT;
   }

   if (cc_may_kill)
      dw5 |= GEN6_WM_DW5_PS_KILL_PIXEL | GEN6_WM_DW5_PS_DISPATCH_ENABLE;

   if (dual_blend)
      dw5 |= GEN6_WM_DW5_PS_DUAL_SOURCE_BLEND;

   if (multisample && num_samples > 1)
      dw6 |= GEN6_WM_DW6_MSDISPMODE_PERPIXEL;

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_WM) | (cmd_len - 2);
   dw[1] = ilo_shader_get_kernel_offset(fs);
   dw[2] = dw2;
   dw[3] = 0; /* scratch */
   dw[4] = dw4;
   dw[5] = dw5;
   dw[6] = dw6;
   dw[7] = 0; /* kernel 1 */
   dw[8] = 0; /* kernel 2 */
}

static inline void
gen7_3DSTATE_WM(struct ilo_builder *builder,
                const struct ilo_state_raster *rs,
                const struct ilo_shader_state *fs,
                bool cc_may_kill)
{
   const uint8_t cmd_len = 3;
   const bool multisample = false;
   const int num_samples = 1;
   uint32_t dw1, dw2, *dw;

   ILO_DEV_ASSERT(builder->dev, 7, 7.5);

   /* see raster_set_gen8_3DSTATE_WM() */
   dw1 = rs->wm[0];

   if (fs) {
      const union ilo_shader_cso *cso;

      cso = ilo_shader_get_kernel_cso(fs);
      /* see fs_init_cso_gen7() */
      dw1 |= cso->ps_payload[3];
   }

   if (cc_may_kill)
      dw1 |= GEN7_WM_DW1_PS_DISPATCH_ENABLE | GEN7_WM_DW1_PS_KILL_PIXEL;

   dw2 = 0;
   if (multisample && num_samples > 1)
      dw2 |= GEN7_WM_DW2_MSDISPMODE_PERPIXEL;

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_WM) | (cmd_len - 2);
   dw[1] = dw1;
   dw[2] = dw2;
}

static inline void
gen8_3DSTATE_WM(struct ilo_builder *builder,
                const struct ilo_state_raster *rs)
{
   const uint8_t cmd_len = 2;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_WM) | (cmd_len - 2);
   /* see raster_set_gen8_3DSTATE_WM() */
   dw[1] = rs->wm[0];
}

static inline void
gen8_3DSTATE_WM_DEPTH_STENCIL(struct ilo_builder *builder,
                              const struct ilo_state_cc *cc)
{
   const uint8_t cmd_len = 3;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN8_RENDER_CMD(3D, 3DSTATE_WM_DEPTH_STENCIL) | (cmd_len - 2);
   /* see cc_set_gen8_3DSTATE_WM_DEPTH_STENCIL() */
   dw[1] = cc->ds[0];
   dw[2] = cc->ds[1];
}

static inline void
gen8_3DSTATE_WM_HZ_OP(struct ilo_builder *builder,
                      const struct ilo_state_raster *rs,
                      uint16_t width, uint16_t height)
{
   const uint8_t cmd_len = 5;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN8_RENDER_CMD(3D, 3DSTATE_WM_HZ_OP) | (cmd_len - 2);
   /* see raster_set_gen8_3dstate_wm_hz_op() */
   dw[1] = rs->wm[1];
   dw[2] = 0;
   /* exclusive */
   dw[3] = height << 16 | width;
   dw[4] = rs->wm[2];
}

static inline void
gen8_disable_3DSTATE_WM_HZ_OP(struct ilo_builder *builder)
{
   const uint8_t cmd_len = 5;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN8_RENDER_CMD(3D, 3DSTATE_WM_HZ_OP) | (cmd_len - 2);
   dw[1] = 0;
   dw[2] = 0;
   dw[3] = 0;
   dw[4] = 0;
}

static inline void
gen8_3DSTATE_WM_CHROMAKEY(struct ilo_builder *builder)
{
   const uint8_t cmd_len = 2;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN8_RENDER_CMD(3D, 3DSTATE_WM_CHROMAKEY) | (cmd_len - 2);
   dw[1] = 0;
}

static inline void
gen7_3DSTATE_PS(struct ilo_builder *builder,
                const struct ilo_shader_state *fs,
                bool dual_blend)
{
   const uint8_t cmd_len = 8;
   const union ilo_shader_cso *cso;
   uint32_t dw2, dw4, dw5, *dw;

   ILO_DEV_ASSERT(builder->dev, 7, 7.5);

   /* see fs_init_cso_gen7() */
   cso = ilo_shader_get_kernel_cso(fs);
   dw2 = cso->ps_payload[0];
   dw4 = cso->ps_payload[1];
   dw5 = cso->ps_payload[2];

   if (dual_blend)
      dw4 |= GEN7_PS_DW4_DUAL_SOURCE_BLEND;

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_PS) | (cmd_len - 2);
   dw[1] = ilo_shader_get_kernel_offset(fs);
   dw[2] = dw2;
   dw[3] = 0; /* scratch */
   dw[4] = dw4;
   dw[5] = dw5;
   dw[6] = 0; /* kernel 1 */
   dw[7] = 0; /* kernel 2 */
}

static inline void
gen7_disable_3DSTATE_PS(struct ilo_builder *builder)
{
   const uint8_t cmd_len = 8;
   int max_threads;
   uint32_t dw4, *dw;

   ILO_DEV_ASSERT(builder->dev, 7, 7.5);

   /* GPU hangs if none of the dispatch enable bits is set */
   dw4 = GEN6_PS_DISPATCH_8 << GEN7_PS_DW4_DISPATCH_MODE__SHIFT;

   /* see brwCreateContext() */
   switch (ilo_dev_gen(builder->dev)) {
   case ILO_GEN(7.5):
      max_threads = (builder->dev->gt == 3) ? 408 :
                    (builder->dev->gt == 2) ? 204 : 102;
      dw4 |= (max_threads - 1) << GEN75_PS_DW4_MAX_THREADS__SHIFT;
      break;
   case ILO_GEN(7):
   default:
      max_threads = (builder->dev->gt == 2) ? 172 : 48;
      dw4 |= (max_threads - 1) << GEN7_PS_DW4_MAX_THREADS__SHIFT;
      break;
   }

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_PS) | (cmd_len - 2);
   dw[1] = 0;
   dw[2] = 0;
   dw[3] = 0;
   dw[4] = dw4;
   dw[5] = 0;
   dw[6] = 0;
   dw[7] = 0;
}

static inline void
gen8_3DSTATE_PS(struct ilo_builder *builder,
                const struct ilo_shader_state *fs)
{
   const uint8_t cmd_len = 12;
   const union ilo_shader_cso *cso;
   uint32_t dw3, dw6, dw7, *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   /* see fs_init_cso_gen8() */
   cso = ilo_shader_get_kernel_cso(fs);
   dw3 = cso->ps_payload[0];
   dw6 = cso->ps_payload[1];
   dw7 = cso->ps_payload[2];

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_PS) | (cmd_len - 2);
   dw[1] = ilo_shader_get_kernel_offset(fs);
   dw[2] = 0;
   dw[3] = dw3;
   dw[4] = 0; /* scratch */
   dw[5] = 0;
   dw[6] = dw6;
   dw[7] = dw7;
   dw[8] = 0; /* kernel 1 */
   dw[9] = 0;
   dw[10] = 0; /* kernel 2 */
   dw[11] = 0;
}

static inline void
gen8_3DSTATE_PS_EXTRA(struct ilo_builder *builder,
                      const struct ilo_shader_state *fs,
                      bool cc_may_kill, bool per_sample)
{
   const uint8_t cmd_len = 2;
   const union ilo_shader_cso *cso;
   uint32_t dw1, *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   /* see fs_init_cso_gen8() */
   cso = ilo_shader_get_kernel_cso(fs);
   dw1 = cso->ps_payload[3];

   if (cc_may_kill)
      dw1 |= GEN8_PSX_DW1_VALID | GEN8_PSX_DW1_KILL_PIXEL;
   if (per_sample)
      dw1 |= GEN8_PSX_DW1_PER_SAMPLE;

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN8_RENDER_CMD(3D, 3DSTATE_PS_EXTRA) | (cmd_len - 2);
   dw[1] = dw1;
}

static inline void
gen8_3DSTATE_PS_BLEND(struct ilo_builder *builder,
                      const struct ilo_state_cc *cc)
{
   const uint8_t cmd_len = 2;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN8_RENDER_CMD(3D, 3DSTATE_PS_BLEND) | (cmd_len - 2);
   /* see cc_set_gen8_3DSTATE_PS_BLEND() */
   dw[1] = cc->blend[0];
}

static inline void
gen6_3DSTATE_CONSTANT_PS(struct ilo_builder *builder,
                         const uint32_t *bufs, const int *sizes,
                         int num_bufs)
{
   gen6_3dstate_constant(builder, GEN6_RENDER_OPCODE_3DSTATE_CONSTANT_PS,
         bufs, sizes, num_bufs);
}

static inline void
gen7_3DSTATE_CONSTANT_PS(struct ilo_builder *builder,
                         const uint32_t *bufs, const int *sizes,
                         int num_bufs)
{
   gen7_3dstate_constant(builder, GEN6_RENDER_OPCODE_3DSTATE_CONSTANT_PS,
         bufs, sizes, num_bufs);
}

static inline void
gen7_3DSTATE_BINDING_TABLE_POINTERS_PS(struct ilo_builder *builder,
                                       uint32_t binding_table)
{
   ILO_DEV_ASSERT(builder->dev, 7, 8);

   gen7_3dstate_pointer(builder,
         GEN7_RENDER_OPCODE_3DSTATE_BINDING_TABLE_POINTERS_PS,
         binding_table);
}

static inline void
gen7_3DSTATE_SAMPLER_STATE_POINTERS_PS(struct ilo_builder *builder,
                                       uint32_t sampler_state)
{
   ILO_DEV_ASSERT(builder->dev, 7, 8);

   gen7_3dstate_pointer(builder,
         GEN7_RENDER_OPCODE_3DSTATE_SAMPLER_STATE_POINTERS_PS,
         sampler_state);
}

static inline void
gen6_3DSTATE_MULTISAMPLE(struct ilo_builder *builder,
                         const struct ilo_state_raster *rs,
                         const uint32_t *pattern, int pattern_len)
{
   const uint8_t cmd_len = (ilo_dev_gen(builder->dev) >= ILO_GEN(7)) ? 4 : 3;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 7.5);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_MULTISAMPLE) | (cmd_len - 2);
   /* see raster_set_gen8_3DSTATE_MULTISAMPLE() */
   dw[1] = rs->sample[0];

   assert(pattern_len == 1 || pattern_len == 2);
   dw[2] = pattern[0];
   if (ilo_dev_gen(builder->dev) >= ILO_GEN(7))
      dw[3] = (pattern_len == 2) ? pattern[1] : 0;
}

static inline void
gen8_3DSTATE_MULTISAMPLE(struct ilo_builder *builder,
                         const struct ilo_state_raster *rs)
{
   const uint8_t cmd_len = 2;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN8_RENDER_CMD(3D, 3DSTATE_MULTISAMPLE) | (cmd_len - 2);
   /* see raster_set_gen8_3DSTATE_MULTISAMPLE() */
   dw[1] = rs->sample[0];
}

static inline void
gen8_3DSTATE_SAMPLE_PATTERN(struct ilo_builder *builder,
                            const uint32_t *pattern_1x,
                            const uint32_t *pattern_2x,
                            const uint32_t *pattern_4x,
                            const uint32_t *pattern_8x,
                            const uint32_t *pattern_16x)
{
   const uint8_t cmd_len = 9;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN8_RENDER_CMD(3D, 3DSTATE_SAMPLE_PATTERN) | (cmd_len - 2);
   dw[1] = pattern_16x[3];
   dw[2] = pattern_16x[2];
   dw[3] = pattern_16x[1];
   dw[4] = pattern_16x[0];
   dw[5] = pattern_8x[1];
   dw[6] = pattern_8x[0];
   dw[7] = pattern_4x[0];
   dw[8] = pattern_1x[0] << 16 |
           pattern_2x[0];
}

static inline void
gen6_3DSTATE_SAMPLE_MASK(struct ilo_builder *builder,
                         const struct ilo_state_raster *rs)
{
   const uint8_t cmd_len = 2;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_SAMPLE_MASK) | (cmd_len - 2);
   /* see raster_set_gen6_3DSTATE_SAMPLE_MASK() */
   dw[1] = rs->sample[1];
}

static inline void
gen6_3DSTATE_DRAWING_RECTANGLE(struct ilo_builder *builder,
                               unsigned x, unsigned y,
                               unsigned width, unsigned height)
{
   const uint8_t cmd_len = 4;
   unsigned xmax = x + width - 1;
   unsigned ymax = y + height - 1;
   unsigned rect_limit;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   if (ilo_dev_gen(builder->dev) >= ILO_GEN(7)) {
      rect_limit = 16383;
   }
   else {
      /*
       * From the Sandy Bridge PRM, volume 2 part 1, page 230:
       *
       *     "[DevSNB] Errata: This field (Clipped Drawing Rectangle Y Min)
       *      must be an even number"
       */
      assert(y % 2 == 0);

      rect_limit = 8191;
   }

   if (x > rect_limit) x = rect_limit;
   if (y > rect_limit) y = rect_limit;
   if (xmax > rect_limit) xmax = rect_limit;
   if (ymax > rect_limit) ymax = rect_limit;

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_DRAWING_RECTANGLE) | (cmd_len - 2);
   dw[1] = y << 16 | x;
   dw[2] = ymax << 16 | xmax;
   /*
    * There is no need to set the origin.  It is intended to support front
    * buffer rendering.
    */
   dw[3] = 0;
}

static inline void
gen6_3DSTATE_POLY_STIPPLE_OFFSET(struct ilo_builder *builder,
                                 int x_offset, int y_offset)
{
   const uint8_t cmd_len = 2;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   assert(x_offset >= 0 && x_offset <= 31);
   assert(y_offset >= 0 && y_offset <= 31);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_POLY_STIPPLE_OFFSET) | (cmd_len - 2);
   dw[1] = x_offset << 8 | y_offset;
}

static inline void
gen6_3DSTATE_POLY_STIPPLE_PATTERN(struct ilo_builder *builder,
                                  const struct pipe_poly_stipple *pattern)
{
   const uint8_t cmd_len = 33;
   uint32_t *dw;
   int i;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_POLY_STIPPLE_PATTERN) | (cmd_len - 2);
   dw++;

   STATIC_ASSERT(Elements(pattern->stipple) == 32);
   for (i = 0; i < 32; i++)
      dw[i] = pattern->stipple[i];
}

static inline void
gen6_3DSTATE_LINE_STIPPLE(struct ilo_builder *builder,
                          unsigned pattern, unsigned factor)
{
   const uint8_t cmd_len = 3;
   unsigned inverse;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   assert((pattern & 0xffff) == pattern);
   assert(factor >= 1 && factor <= 256);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_LINE_STIPPLE) | (cmd_len - 2);
   dw[1] = pattern;

   if (ilo_dev_gen(builder->dev) >= ILO_GEN(7)) {
      /* in U1.16 */
      inverse = 65536 / factor;

      dw[2] = inverse << GEN7_LINE_STIPPLE_DW2_INVERSE_REPEAT_COUNT__SHIFT |
              factor;
   }
   else {
      /* in U1.13 */
      inverse = 8192 / factor;

      dw[2] = inverse << GEN6_LINE_STIPPLE_DW2_INVERSE_REPEAT_COUNT__SHIFT |
              factor;
   }
}

static inline void
gen6_3DSTATE_AA_LINE_PARAMETERS(struct ilo_builder *builder)
{
   const uint8_t cmd_len = 3;
   const uint32_t dw[3] = {
      GEN6_RENDER_CMD(3D, 3DSTATE_AA_LINE_PARAMETERS) | (cmd_len - 2),
      0 << GEN6_AA_LINE_DW1_BIAS__SHIFT | 0,
      0 << GEN6_AA_LINE_DW2_CAP_BIAS__SHIFT | 0,
   };

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   ilo_builder_batch_write(builder, cmd_len, dw);
}

static inline void
gen6_3DSTATE_DEPTH_BUFFER(struct ilo_builder *builder,
                          const struct ilo_state_zs *zs)
{
   const uint32_t cmd = (ilo_dev_gen(builder->dev) >= ILO_GEN(7)) ?
      GEN7_RENDER_CMD(3D, 3DSTATE_DEPTH_BUFFER) :
      GEN6_RENDER_CMD(3D, 3DSTATE_DEPTH_BUFFER);
   const uint8_t cmd_len = (ilo_dev_gen(builder->dev) >= ILO_GEN(8)) ? 8 : 7;
   uint32_t *dw;
   unsigned pos;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   pos = ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = cmd | (cmd_len - 2);

   /*
    * see zs_set_gen6_3DSTATE_DEPTH_BUFFER() and
    * zs_set_gen7_3DSTATE_DEPTH_BUFFER()
    */
   if (ilo_dev_gen(builder->dev) >= ILO_GEN(8)) {
      dw[1] = zs->depth[0];
      dw[2] = 0;
      dw[3] = 0;
      dw[4] = zs->depth[2];
      dw[5] = zs->depth[3];
      dw[6] = 0;
      dw[7] = zs->depth[4];

      dw[5] |= builder->mocs << GEN8_DEPTH_DW5_MOCS__SHIFT;

      if (zs->depth_bo) {
         ilo_builder_batch_reloc64(builder, pos + 2, zs->depth_bo,
               zs->depth[1], (zs->z_readonly) ? 0 : INTEL_RELOC_WRITE);
      }
   } else {
      dw[1] = zs->depth[0];
      dw[2] = 0;
      dw[3] = zs->depth[2];
      dw[4] = zs->depth[3];
      dw[5] = 0;
      dw[6] = zs->depth[4];

      if (ilo_dev_gen(builder->dev) >= ILO_GEN(7))
         dw[4] |= builder->mocs << GEN7_DEPTH_DW4_MOCS__SHIFT;
      else
         dw[6] |= builder->mocs << GEN6_DEPTH_DW6_MOCS__SHIFT;

      if (zs->depth_bo) {
         ilo_builder_batch_reloc(builder, pos + 2, zs->depth_bo,
               zs->depth[1], (zs->z_readonly) ? 0 : INTEL_RELOC_WRITE);
      }
   }
}

static inline void
gen6_3DSTATE_STENCIL_BUFFER(struct ilo_builder *builder,
                            const struct ilo_state_zs *zs)
{
   const uint32_t cmd = (ilo_dev_gen(builder->dev) >= ILO_GEN(7)) ?
      GEN7_RENDER_CMD(3D, 3DSTATE_STENCIL_BUFFER) :
      GEN6_RENDER_CMD(3D, 3DSTATE_STENCIL_BUFFER);
   const uint8_t cmd_len = (ilo_dev_gen(builder->dev) >= ILO_GEN(8)) ? 5 : 3;
   uint32_t *dw;
   unsigned pos;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   pos = ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = cmd | (cmd_len - 2);

   /* see zs_set_gen6_3DSTATE_STENCIL_BUFFER() */
   if (ilo_dev_gen(builder->dev) >= ILO_GEN(8)) {
      dw[1] = zs->stencil[0];
      dw[2] = 0;
      dw[3] = 0;
      dw[4] = zs->stencil[2];

      dw[1] |= builder->mocs << GEN8_STENCIL_DW1_MOCS__SHIFT;

      if (zs->stencil_bo) {
         ilo_builder_batch_reloc64(builder, pos + 2, zs->stencil_bo,
               zs->stencil[1], (zs->s_readonly) ? 0 : INTEL_RELOC_WRITE);
      }
   } else {
      dw[1] = zs->stencil[0];
      dw[2] = 0;

      dw[1] |= builder->mocs << GEN6_STENCIL_DW1_MOCS__SHIFT;

      if (zs->stencil_bo) {
         ilo_builder_batch_reloc(builder, pos + 2, zs->stencil_bo,
               zs->stencil[1], (zs->s_readonly) ? 0 : INTEL_RELOC_WRITE);
      }
   }
}

static inline void
gen6_3DSTATE_HIER_DEPTH_BUFFER(struct ilo_builder *builder,
                               const struct ilo_state_zs *zs)
{
   const uint32_t cmd = (ilo_dev_gen(builder->dev) >= ILO_GEN(7)) ?
      GEN7_RENDER_CMD(3D, 3DSTATE_HIER_DEPTH_BUFFER) :
      GEN6_RENDER_CMD(3D, 3DSTATE_HIER_DEPTH_BUFFER);
   const uint8_t cmd_len = (ilo_dev_gen(builder->dev) >= ILO_GEN(8)) ? 5 : 3;
   uint32_t *dw;
   unsigned pos;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   pos = ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = cmd | (cmd_len - 2);

   /* see zs_set_gen6_3DSTATE_HIER_DEPTH_BUFFER() */
   if (ilo_dev_gen(builder->dev) >= ILO_GEN(8)) {
      dw[1] = zs->hiz[0];
      dw[2] = 0;
      dw[3] = 0;
      dw[4] = zs->hiz[2];

      dw[1] |= builder->mocs << GEN8_HIZ_DW1_MOCS__SHIFT;

      if (zs->hiz_bo) {
         ilo_builder_batch_reloc64(builder, pos + 2, zs->hiz_bo,
               zs->hiz[1], (zs->z_readonly) ? 0 : INTEL_RELOC_WRITE);
      }
   } else {
      dw[1] = zs->hiz[0];
      dw[2] = 0;

      dw[1] |= builder->mocs << GEN6_HIZ_DW1_MOCS__SHIFT;

      if (zs->hiz_bo) {
         ilo_builder_batch_reloc(builder, pos + 2, zs->hiz_bo,
               zs->hiz[1], (zs->z_readonly) ? 0 : INTEL_RELOC_WRITE);
      }
   }
}

static inline void
gen6_3DSTATE_CLEAR_PARAMS(struct ilo_builder *builder,
                          uint32_t clear_val)
{
   const uint8_t cmd_len = 2;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 6);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_CLEAR_PARAMS) |
           GEN6_CLEAR_PARAMS_DW0_VALID |
           (cmd_len - 2);
   dw[1] = clear_val;
}

static inline void
gen7_3DSTATE_CLEAR_PARAMS(struct ilo_builder *builder,
                          uint32_t clear_val)
{
   const uint8_t cmd_len = 3;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 7, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_CLEAR_PARAMS) | (cmd_len - 2);
   dw[1] = clear_val;
   dw[2] = GEN7_CLEAR_PARAMS_DW2_VALID;
}

static inline void
gen6_3DSTATE_VIEWPORT_STATE_POINTERS(struct ilo_builder *builder,
                                     uint32_t clip_viewport,
                                     uint32_t sf_viewport,
                                     uint32_t cc_viewport)
{
   const uint8_t cmd_len = 4;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 6);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_VIEWPORT_STATE_POINTERS) |
           GEN6_VP_PTR_DW0_CLIP_CHANGED |
           GEN6_VP_PTR_DW0_SF_CHANGED |
           GEN6_VP_PTR_DW0_CC_CHANGED |
           (cmd_len - 2);
   dw[1] = clip_viewport;
   dw[2] = sf_viewport;
   dw[3] = cc_viewport;
}

static inline void
gen6_3DSTATE_SCISSOR_STATE_POINTERS(struct ilo_builder *builder,
                                    uint32_t scissor_rect)
{
   const uint8_t cmd_len = 2;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_SCISSOR_STATE_POINTERS) |
           (cmd_len - 2);
   dw[1] = scissor_rect;
}

static inline void
gen6_3DSTATE_CC_STATE_POINTERS(struct ilo_builder *builder,
                               uint32_t blend_state,
                               uint32_t depth_stencil_state,
                               uint32_t color_calc_state)
{
   const uint8_t cmd_len = 4;
   uint32_t *dw;

   ILO_DEV_ASSERT(builder->dev, 6, 6);

   ilo_builder_batch_pointer(builder, cmd_len, &dw);

   dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_CC_STATE_POINTERS) | (cmd_len - 2);
   dw[1] = blend_state | GEN6_CC_PTR_DW1_BLEND_CHANGED;
   dw[2] = depth_stencil_state | GEN6_CC_PTR_DW2_ZS_CHANGED;
   dw[3] = color_calc_state | GEN6_CC_PTR_DW3_CC_CHANGED;
}

static inline void
gen7_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP(struct ilo_builder *builder,
                                             uint32_t sf_clip_viewport)
{
   ILO_DEV_ASSERT(builder->dev, 7, 8);

   gen7_3dstate_pointer(builder,
         GEN7_RENDER_OPCODE_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP,
         sf_clip_viewport);
}

static inline void
gen7_3DSTATE_VIEWPORT_STATE_POINTERS_CC(struct ilo_builder *builder,
                                        uint32_t cc_viewport)
{
   ILO_DEV_ASSERT(builder->dev, 7, 8);

   gen7_3dstate_pointer(builder,
         GEN7_RENDER_OPCODE_3DSTATE_VIEWPORT_STATE_POINTERS_CC,
         cc_viewport);
}

static inline void
gen7_3DSTATE_CC_STATE_POINTERS(struct ilo_builder *builder,
                               uint32_t color_calc_state)
{
   ILO_DEV_ASSERT(builder->dev, 7, 8);

   if (ilo_dev_gen(builder->dev) >= ILO_GEN(8))
      color_calc_state |= 1;

   gen7_3dstate_pointer(builder,
         GEN6_RENDER_OPCODE_3DSTATE_CC_STATE_POINTERS, color_calc_state);
}

static inline void
gen7_3DSTATE_DEPTH_STENCIL_STATE_POINTERS(struct ilo_builder *builder,
                                          uint32_t depth_stencil_state)
{
   ILO_DEV_ASSERT(builder->dev, 7, 8);

   gen7_3dstate_pointer(builder,
         GEN7_RENDER_OPCODE_3DSTATE_DEPTH_STENCIL_STATE_POINTERS,
         depth_stencil_state);
}

static inline void
gen7_3DSTATE_BLEND_STATE_POINTERS(struct ilo_builder *builder,
                                  uint32_t blend_state)
{
   ILO_DEV_ASSERT(builder->dev, 7, 8);

   if (ilo_dev_gen(builder->dev) >= ILO_GEN(8))
      blend_state |= 1;

   gen7_3dstate_pointer(builder,
         GEN7_RENDER_OPCODE_3DSTATE_BLEND_STATE_POINTERS,
         blend_state);
}

static inline uint32_t
gen6_CLIP_VIEWPORT(struct ilo_builder *builder,
                   const struct ilo_state_viewport *vp)
{
   const int state_align = 32;
   const int state_len = 4 * vp->count;
   uint32_t state_offset, *dw;
   int i;

   ILO_DEV_ASSERT(builder->dev, 6, 6);

   state_offset = ilo_builder_dynamic_pointer(builder,
         ILO_BUILDER_ITEM_CLIP_VIEWPORT, state_align, state_len, &dw);

   for (i = 0; i < vp->count; i++) {
      /* see viewport_matrix_set_gen7_SF_CLIP_VIEWPORT() */
      dw[0] = vp->sf_clip[i][8];
      dw[1] = vp->sf_clip[i][9];
      dw[2] = vp->sf_clip[i][10];
      dw[3] = vp->sf_clip[i][11];

      dw += 4;
   }

   return state_offset;
}

static inline uint32_t
gen6_SF_VIEWPORT(struct ilo_builder *builder,
                 const struct ilo_state_viewport *vp)
{
   const int state_align = 32;
   const int state_len = 8 * vp->count;
   uint32_t state_offset, *dw;
   int i;

   ILO_DEV_ASSERT(builder->dev, 6, 6);

   state_offset = ilo_builder_dynamic_pointer(builder,
         ILO_BUILDER_ITEM_SF_VIEWPORT, state_align, state_len, &dw);

   for (i = 0; i < vp->count; i++) {
      /* see viewport_matrix_set_gen7_SF_CLIP_VIEWPORT() */
      memcpy(dw, vp->sf_clip[i], sizeof(*dw) * 8);

      dw += 8;
   }

   return state_offset;
}

static inline uint32_t
gen7_SF_CLIP_VIEWPORT(struct ilo_builder *builder,
                      const struct ilo_state_viewport *vp)
{
   const int state_align = 64;
   const int state_len = 16 * vp->count;

   ILO_DEV_ASSERT(builder->dev, 7, 8);

   /* see viewport_matrix_set_gen7_SF_CLIP_VIEWPORT() */
   return ilo_builder_dynamic_write(builder, ILO_BUILDER_ITEM_SF_VIEWPORT,
         state_align, state_len, (const uint32_t *) vp->sf_clip);
}

static inline uint32_t
gen6_CC_VIEWPORT(struct ilo_builder *builder,
                 const struct ilo_state_viewport *vp)
{
   const int state_align = 32;
   const int state_len = 2 * vp->count;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   /* see viewport_matrix_set_gen6_CC_VIEWPORT() */
   return ilo_builder_dynamic_write(builder, ILO_BUILDER_ITEM_CC_VIEWPORT,
         state_align, state_len, (const uint32_t *) vp->cc);
}

static inline uint32_t
gen6_SCISSOR_RECT(struct ilo_builder *builder,
                  const struct ilo_state_viewport *vp)
{
   const int state_align = 32;
   const int state_len = 2 * vp->count;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   /* see viewport_scissor_set_gen6_SCISSOR_RECT() */
   return ilo_builder_dynamic_write(builder, ILO_BUILDER_ITEM_SCISSOR_RECT,
         state_align, state_len, (const uint32_t *) vp->scissor);
}

static inline uint32_t
gen6_COLOR_CALC_STATE(struct ilo_builder *builder,
                      const struct ilo_state_cc *cc)
{
   const int state_align = 64;
   const int state_len = 6;

   ILO_DEV_ASSERT(builder->dev, 6, 8);

   /* see cc_params_set_gen6_COLOR_CALC_STATE() */
   return ilo_builder_dynamic_write(builder, ILO_BUILDER_ITEM_COLOR_CALC,
         state_align, state_len, cc->cc);
}

static inline uint32_t
gen6_DEPTH_STENCIL_STATE(struct ilo_builder *builder,
                         const struct ilo_state_cc *cc)
{
   const int state_align = 64;
   const int state_len = 3;

   ILO_DEV_ASSERT(builder->dev, 6, 7.5);

   /* see cc_set_gen6_DEPTH_STENCIL_STATE() */
   return ilo_builder_dynamic_write(builder, ILO_BUILDER_ITEM_DEPTH_STENCIL,
         state_align, state_len, cc->ds);
}

static inline uint32_t
gen6_BLEND_STATE(struct ilo_builder *builder,
                 const struct ilo_state_cc *cc)
{
   const int state_align = 64;
   const int state_len = 2 * cc->blend_state_count;

   ILO_DEV_ASSERT(builder->dev, 6, 7.5);

   if (!state_len)
      return 0;

   /* see cc_set_gen6_BLEND_STATE() */
   return ilo_builder_dynamic_write(builder, ILO_BUILDER_ITEM_BLEND,
         state_align, state_len, cc->blend);
}

static inline uint32_t
gen8_BLEND_STATE(struct ilo_builder *builder,
                 const struct ilo_state_cc *cc)
{
   const int state_align = 64;
   const int state_len = 1 + 2 * cc->blend_state_count;

   ILO_DEV_ASSERT(builder->dev, 8, 8);

   /* see cc_set_gen8_BLEND_STATE() */
   return ilo_builder_dynamic_write(builder, ILO_BUILDER_ITEM_BLEND,
         state_align, state_len, &cc->blend[1]);
}

#endif /* ILO_BUILDER_3D_BOTTOM_H */
