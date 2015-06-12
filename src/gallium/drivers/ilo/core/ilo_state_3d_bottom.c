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
