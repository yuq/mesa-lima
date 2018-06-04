/*
 * Copyright (c) 2011-2013 Luc Verhaegen <libv@skynet.be>
 * Copyright (c) 2018 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "util/u_math.h"
#include "util/u_debug.h"
#include "util/u_transfer.h"

#include "lima_bo.h"
#include "lima_context.h"
#include "lima_screen.h"
#include "lima_texture.h"
#include "lima_resource.h"
#include "lima_submit.h"
#include "lima_util.h"

#include <lima_drm.h>

#define LIMA_TEXEL_FORMAT_BGR_565      0x0e
#define LIMA_TEXEL_FORMAT_RGB_888      0x15
#define LIMA_TEXEL_FORMAT_RGBA_8888    0x16

#define lima_tex_desc_size 64
#define lima_tex_list_size 64

static uint32_t pipe_format_to_lima(enum pipe_format pformat)
{
   unsigned swap_chans, flag1, format;

   switch (pformat) {
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      swap_chans = 1;
      flag1 = 0;
      format = LIMA_TEXEL_FORMAT_RGBA_8888;
      break;
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      swap_chans = 0;
      flag1 = 0;
      format = LIMA_TEXEL_FORMAT_RGBA_8888;
      break;
   case PIPE_FORMAT_R8G8B8_UNORM:
      swap_chans = 1;
      flag1 = 0;
      format = LIMA_TEXEL_FORMAT_RGB_888;
      break;
   case PIPE_FORMAT_B5G6R5_UNORM:
      swap_chans = 0;
      flag1 = 0;
      format = LIMA_TEXEL_FORMAT_BGR_565;
      break;
   default:
      assert(0);
      break;
   }

   return (swap_chans << 7) | (flag1 << 6) | format;
}

static void
lima_update_tex_desc(struct lima_context *ctx, struct lima_sampler_state *sampler,
                     struct lima_sampler_view *texture, void *pdesc)
{
   uint32_t *desc = pdesc;
   unsigned width, height, layout;
   struct pipe_resource *prsc = texture->base.texture;
   struct lima_resource *lima_res = lima_resource(prsc);

   /* TODO: - do we need to align width/height to 16?
            - does hardware support stride different from width? */
   width = prsc->width0;
   height = prsc->height0;

   if (lima_res->tiled)
      layout = 3;
   else
      layout = 0;

   desc[0] = pipe_format_to_lima(prsc->format);

   /* 2D texture */
   desc[1] = 0x400;
   desc[2] = (width << 22);
   desc[3] = 0x10000 | (height << 3) | (width >> 10);
   desc[6] = layout << 13;

   lima_submit_add_bo(ctx->pp_submit, lima_res->bo, LIMA_SUBMIT_BO_READ);
   lima_bo_update(lima_res->bo, false, true);

   /* attach level 0 */
   desc[6] &= ~0xc0000000;
   desc[6] |= lima_res->bo->va << 24;
   desc[7] &= ~0x00ffffff;
   desc[7] |= lima_res->bo->va >> 8;

   desc[1] &= ~0xff000000;
   switch (sampler->base.mag_img_filter) {
   case PIPE_TEX_FILTER_LINEAR:
      desc[2] &= ~0x1000;
      /* no mipmap, filter_mag = linear */
      desc[1] |= 0x80000000;
      break;
   case PIPE_TEX_FILTER_NEAREST:
   default:
      desc[2] |= 0x1000;
      break;
   }

   switch (sampler->base.min_img_filter) {
      break;
   case PIPE_TEX_FILTER_LINEAR:
      desc[2] &= ~0x0800;
      break;
   case PIPE_TEX_FILTER_NEAREST:
   default:
      desc[2] |= 0x0800;
      break;
   }

   /* Only clamp to edge and mirror repeat are supported */
   desc[2] &= ~0xe000;
   switch (sampler->base.wrap_s) {
   case PIPE_TEX_WRAP_CLAMP:
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
      desc[2] |= 0x2000;
      break;
   case PIPE_TEX_WRAP_REPEAT:
   case PIPE_TEX_WRAP_MIRROR_REPEAT:
      desc[2] |= 0x8000;
      break;
   }

   /* Only clamp to edge and mirror repeat are supported */
   desc[2] &= ~0x070000;
   switch (sampler->base.wrap_s) {
   case PIPE_TEX_WRAP_CLAMP:
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
      desc[2] |= 0x010000;
      break;
   case PIPE_TEX_WRAP_REPEAT:
   case PIPE_TEX_WRAP_MIRROR_REPEAT:
      desc[2] |= 0x040000;
      break;
   }
}

void
lima_update_textures(struct lima_context *ctx)
{
   struct lima_texture_stateobj *lima_tex = &ctx->tex_stateobj;

   assert (lima_tex->num_samplers <= 16);

   /* Nothing to do - we have no samplers or textures */
   if (!lima_tex->num_samplers || !lima_tex->num_textures)
      return;

   unsigned size = lima_tex_list_size + lima_tex->num_samplers * lima_tex_desc_size;
   uint32_t *descs =
      lima_ctx_buff_alloc(ctx, lima_ctx_buff_pp_tex_desc,
                          size, LIMA_CTX_BUFF_SUBMIT_PP, true);

   for (int i = 0; i < lima_tex->num_samplers; i++) {
      off_t offset = lima_tex_desc_size * i + lima_tex_list_size;
      struct lima_sampler_state *sampler = lima_sampler_state(lima_tex->samplers[i]);
      struct lima_sampler_view *texture = lima_sampler_view(lima_tex->textures[i]);

      descs[i] = lima_ctx_buff_va(ctx, lima_ctx_buff_pp_tex_desc) + offset;
      lima_update_tex_desc(ctx, sampler, texture, (void *)descs + offset);
   }

   lima_dump_command_stream_print(
      descs, size, false, "add textures_desc at va %x\n",
      lima_ctx_buff_va(ctx, lima_ctx_buff_pp_tex_desc));
}
