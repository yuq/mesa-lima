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
