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
#include "util/u_framebuffer.h"
#include "util/u_half.h"

#include "ilo_format.h"
#include "ilo_image.h"
#include "ilo_state_3d.h"
#include "../ilo_shader.h"

static void
fs_init_cso_gen6(const struct ilo_dev *dev,
                 const struct ilo_shader_state *fs,
                 struct ilo_shader_cso *cso)
{
   int start_grf, input_count, sampler_count, max_threads;
   uint32_t dw2, dw4, dw5, dw6;

   ILO_DEV_ASSERT(dev, 6, 6);

   start_grf = ilo_shader_get_kernel_param(fs, ILO_KERNEL_URB_DATA_START_REG);
   input_count = ilo_shader_get_kernel_param(fs, ILO_KERNEL_INPUT_COUNT);
   sampler_count = ilo_shader_get_kernel_param(fs, ILO_KERNEL_SAMPLER_COUNT);

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
         GEN6_POSOFFSET_NONE << GEN6_WM_DW6_PS_POSOFFSET__SHIFT;

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

   dw = 0;

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

   STATIC_ASSERT(Elements(cso->payload) >= 4);
   cso->payload[0] = dw3;
   cso->payload[1] = dw6;
   cso->payload[2] = dw7;
   cso->payload[3] = fs_get_psx_gen8(dev, fs);
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

   caps->is_unorm = (ch >= 0 && desc->channel[ch].normalized &&
         desc->channel[ch].type == UTIL_FORMAT_TYPE_UNSIGNED &&
         desc->colorspace == UTIL_FORMAT_COLORSPACE_RGB);
   caps->is_integer = util_format_is_pure_integer(format);

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 365:
    *
    *     "Logic Ops are only supported on *_UNORM surfaces (excluding _SRGB
    *      variants), otherwise Logic Ops must be DISABLED."
    *
    * According to the classic driver, this is lifted on Gen8+.
    */
   caps->can_logicop = (ilo_dev_gen(dev) >= ILO_GEN(8) || caps->is_unorm);

   /* no blending for pure integer formats */
   caps->can_blend = !caps->is_integer;

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 382:
    *
    *     "Alpha Test can only be enabled if Pixel Shader outputs a float
    *      alpha value."
    */
   caps->can_alpha_test = !caps->is_integer;

   caps->force_dst_alpha_one =
      (ilo_format_translate_render(dev, format) !=
       ilo_format_translate_color(dev, format));

   /* sanity check */
   if (caps->force_dst_alpha_one) {
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

   fb->has_integer_rt = false;
   for (i = 0; i < state->nr_cbufs; i++) {
      if (state->cbufs[i]) {
         fb_set_blend_caps(dev, state->cbufs[i]->format, &fb->blend_caps[i]);

         fb->has_integer_rt |= fb->blend_caps[i].is_integer;

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

   if (state->zsbuf) {
      const struct ilo_surface_cso *cso =
         (const struct ilo_surface_cso *) state->zsbuf;

      fb->has_hiz = cso->u.zs.hiz_bo;
      fb->depth_offset_format =
         ilo_state_zs_get_depth_format(&cso->u.zs, dev);
   } else {
      fb->has_hiz = false;
      fb->depth_offset_format = GEN6_ZFORMAT_D32_FLOAT;
   }

   /*
    * The PRMs list several restrictions when the framebuffer has more than
    * one surface.  It seems they are actually lifted on GEN6+.
    */
}
