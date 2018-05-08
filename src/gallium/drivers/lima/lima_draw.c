/*
 * Copyright (c) 2011-2013 Luc Verhaegen <libv@skynet.be>
 * Copyright (c) 2017 Lima Project
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

#include "util/u_math.h"
#include "util/u_format.h"
#include "util/u_debug.h"
#include "util/u_half.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_pack_color.h"
#include "util/hash_table.h"

#include "lima_context.h"
#include "lima_screen.h"
#include "lima_resource.h"
#include "lima_program.h"
#include "lima_bo.h"
#include "lima_submit.h"
#include "lima_texture.h"
#include "lima_util.h"

#include <lima_drm.h>

struct lima_gp_frame_reg {
   uint32_t vs_cmd_start;
   uint32_t vs_cmd_end;
   uint32_t plbu_cmd_start;
   uint32_t plbu_cmd_end;
   uint32_t tile_heap_start;
   uint32_t tile_heap_end;
};

struct lima_pp_frame_reg {
   uint32_t plbu_array_address;
   uint32_t render_address;
   uint32_t unused_0;
   uint32_t flags;
   uint32_t clear_value_depth;
   uint32_t clear_value_stencil;
   uint32_t clear_value_color;
   uint32_t clear_value_color_1;
   uint32_t clear_value_color_2;
   uint32_t clear_value_color_3;
   uint32_t width;
   uint32_t height;
   uint32_t fragment_stack_address;
   uint32_t fragment_stack_size;
   uint32_t unused_1;
   uint32_t unused_2;
   uint32_t one;
   uint32_t supersampled_height;
   uint32_t dubya;
   uint32_t onscreen;
   uint32_t blocking;
   uint32_t scale;
   uint32_t foureight;
};

struct lima_pp_wb_reg {
   uint32_t type;
   uint32_t address;
   uint32_t pixel_format;
   uint32_t downsample_factor;
   uint32_t pixel_layout;
   uint32_t pitch;
   uint32_t mrt_bits;
   uint32_t mrt_pitch;
   uint32_t zero;
   uint32_t unused0;
   uint32_t unused1;
   uint32_t unused2;
};

static void
lima_clear(struct pipe_context *pctx, unsigned buffers,
           const union pipe_color_union *color, double depth, unsigned stencil)
{
   debug_checkpoint();

   struct lima_context *ctx = lima_context(pctx);
   struct lima_context_clear *clear = &ctx->clear;

   clear->buffers = buffers;

   if (buffers & PIPE_CLEAR_COLOR0)
      clear->color =
         ((uint32_t)float_to_ubyte(color->f[3]) << 24) |
         ((uint32_t)float_to_ubyte(color->f[2]) << 16) |
         ((uint32_t)float_to_ubyte(color->f[1]) << 8) |
         float_to_ubyte(color->f[0]);

   if (buffers & PIPE_CLEAR_DEPTH)
      clear->depth = util_pack_z(PIPE_FORMAT_Z24X8_UNORM, depth);

   if (buffers & PIPE_CLEAR_STENCIL)
      clear->stencil = stencil;

   ctx->dirty |= LIMA_CONTEXT_DIRTY_CLEAR;
}

static void
hilbert_rotate(int n, int *x, int *y, int rx, int ry)
{
   if (ry == 0) {
      if (rx == 1) {
         *x = n-1 - *x;
         *y = n-1 - *y;
      }

      /* Swap x and y */
      int t  = *x;
      *x = *y;
      *y = t;
   }
}

static void
hilbert_coords(int n, int d, int *x, int *y)
{
   int rx, ry, i, t=d;

   *x = *y = 0;

   for (i = 0; (1 << i) < n; i++) {

      rx = 1 & (t / 2);
      ry = 1 & (t ^ rx);

      hilbert_rotate(1 << i, x, y, rx, ry);

      *x += rx << i;
      *y += ry << i;

      t /= 4;
   }
}

static void
lima_update_plb(struct lima_context *ctx, struct lima_ctx_plb_pp_stream *s)
{
   struct lima_context_framebuffer *fb = &ctx->framebuffer;
   struct lima_screen *screen = lima_screen(ctx->base.screen);

   if (s->bo)
      return;

   /* carefully calculate each stream start address:
    * 1. overflow: each stream size may be different due to
    *    fb->tiled_w * fb->tiled_h can't be divided by num_pp,
    *    extra size should be added to the preceeding stream
    * 2. alignment: each stream address should be 0x20 aligned
    */
   int i, num_pp = screen->num_pp;
   int delta = fb->tiled_w * fb->tiled_h / num_pp * 16 + 8;
   int remain = fb->tiled_w * fb->tiled_h % num_pp;
   int offset = 0;

   for (i = 0; i < num_pp; i++) {
      s->offset[i] = offset;

      offset += delta;
      if (remain) {
         offset += 16;
         remain--;
      }
      offset = align(offset, 0x20);
   }

   unsigned size = align(offset, LIMA_PAGE_SIZE);
   s->bo = lima_bo_create(screen, size, 0, true, true);

   /* use hilbert_coords to generates 1D to 2D relationship.
    * 1D for pp stream index and 2D for plb block x/y on framebuffer.
    * if multi pp, interleave the 1D index to make each pp's render target
    * close enough which should result close workload
    */
   int max = MAX2(fb->tiled_w, fb->tiled_h);
   int dim = util_logbase2_ceil(max);
   int count = 1 << (dim + dim);
   int index = 0;
   uint32_t *stream[4];
   int si[4] = {0};

   for (i = 0; i < num_pp; i++)
      stream[i] = s->bo->map + s->offset[i];

   for (i = 0; i < count; i++) {
      int x, y;
      hilbert_coords(max, i, &x, &y);
      if (x < fb->tiled_w && y < fb->tiled_h) {
         int pp = index % num_pp;
         int offset = ((y >> fb->shift_h) * fb->block_w +
                       (x >> fb->shift_w)) * LIMA_CTX_PLB_BLK_SIZE;
         int plb_va = ctx->plb[s->key.plb_index]->va + offset;

         stream[pp][si[pp]++] = 0;
         stream[pp][si[pp]++] = 0xB8000000 | x | (y << 8);
         stream[pp][si[pp]++] = 0xE0000002 | ((plb_va >> 3) & ~0xE0000003);
         stream[pp][si[pp]++] = 0xB0000000;

         index++;
      }
   }

   for (i = 0; i < num_pp; i++) {
      stream[i][si[i]++] = 0;
      stream[i][si[i]++] = 0xBC000000;

      lima_dump_command_stream_print(
         stream[i], si[i] * 4, false, "pp plb stream %d at va %x\n",
         i, s->bo->va + s->offset[i]);
   }
}

enum lima_attrib_type {
   LIMA_ATTRIB_FLOAT = 0x000,
   /* todo: find out what lives here. */
   LIMA_ATTRIB_I16   = 0x004,
   LIMA_ATTRIB_U16   = 0x005,
   LIMA_ATTRIB_I8    = 0x006,
   LIMA_ATTRIB_U8    = 0x007,
   LIMA_ATTRIB_I8N   = 0x008,
   LIMA_ATTRIB_U8N   = 0x009,
   LIMA_ATTRIB_I16N  = 0x00A,
   LIMA_ATTRIB_U16N  = 0x00B,
   /* todo: where is the 32 int */
   /* todo: find out what lives here. */
   LIMA_ATTRIB_FIXED = 0x101
};

static enum lima_attrib_type
lima_pipe_format_to_attrib_type(enum pipe_format format)
{
   const struct util_format_description *desc = util_format_description(format);
   int i = util_format_get_first_non_void_channel(format);
   const struct util_format_channel_description *c = desc->channel + i;

   switch (c->type) {
   case UTIL_FORMAT_TYPE_FLOAT:
      return LIMA_ATTRIB_FLOAT;
   case UTIL_FORMAT_TYPE_FIXED:
      return LIMA_ATTRIB_FIXED;
   case UTIL_FORMAT_TYPE_SIGNED:
      if (c->size == 8) {
         if (c->normalized)
            return LIMA_ATTRIB_I8N;
         else
            return LIMA_ATTRIB_I8;
      }
      else if (c->size == 16) {
         if (c->normalized)
            return LIMA_ATTRIB_I16N;
         else
            return LIMA_ATTRIB_I16;
      }
      break;
   case UTIL_FORMAT_TYPE_UNSIGNED:
      if (c->size == 8) {
         if (c->normalized)
            return LIMA_ATTRIB_U8N;
         else
            return LIMA_ATTRIB_U8;
      }
      else if (c->size == 16) {
         if (c->normalized)
            return LIMA_ATTRIB_U16N;
         else
            return LIMA_ATTRIB_U16;
      }
      break;
   }

   return LIMA_ATTRIB_FLOAT;
}

static void
lima_pack_vs_cmd(struct lima_context *ctx, const struct pipe_draw_info *info)
{
   int i = 0, max_n = 24;
   uint32_t *vs_cmd = util_dynarray_enlarge(&ctx->vs_cmd_array, max_n * 4);

   if (!info->index_size) {
      vs_cmd[i++] = 0x00028000; /* ARRAYS_SEMAPHORE_BEGIN_1 */
      vs_cmd[i++] = 0x50000000; /* ARRAYS_SEMAPHORE */
      vs_cmd[i++] = 0x00000001; /* ARRAYS_SEMAPHORE_BEGIN_2 */
      vs_cmd[i++] = 0x50000000; /* ARRAYS_SEMAPHORE */
   }

   int uniform_size = ctx->const_buffer[PIPE_SHADER_VERTEX].size + ctx->vs->constant_size + 32;
   vs_cmd[i++] = lima_ctx_buff_va(ctx, lima_ctx_buff_gp_uniform);
   vs_cmd[i++] = 0x30000000 | (align(uniform_size, 16) << 12); /* UNIFORMS_ADDRESS */

   vs_cmd[i++] = ctx->vs->bo->va;
   vs_cmd[i++] = 0x40000000 | ((ctx->vs->shader_size >> 4) << 16); /* SHADER_ADDRESS */

   vs_cmd[i++] = (ctx->vs->prefetch << 20) | ((align(ctx->vs->shader_size, 16) / 16 - 1) << 10);
   vs_cmd[i++] = 0x10000040; /* SHADER_INFO */

   int num_varryings = ctx->vs->num_varying;
   int num_attributes = ctx->vertex_elements->num_elements;

   vs_cmd[i++] = ((num_varryings - 1) << 8) | ((num_attributes - 1) << 24);
   vs_cmd[i++] = 0x10000042; /* VARYING_ATTRIBUTE_COUNT */

   vs_cmd[i++] = 0x00000003;
   vs_cmd[i++] = 0x10000041; /* ?? */

   vs_cmd[i++] = lima_ctx_buff_va(ctx, lima_ctx_buff_gp_attribute_info);
   vs_cmd[i++] = 0x20000000 | (num_attributes << 17); /* ATTRIBUTES_ADDRESS */

   vs_cmd[i++] = lima_ctx_buff_va(ctx, lima_ctx_buff_gp_varying_info);
   vs_cmd[i++] = 0x20000008 | (num_varryings << 17); /* VARYINGS_ADDRESS */

   unsigned num = info->index_size ? (info->max_index - info->min_index + 1) : info->count;
   vs_cmd[i++] = (num << 24) | (info->index_size ? 1 : 0);
   vs_cmd[i++] = 0x00000000 | (num >> 8); /* DRAW */

   vs_cmd[i++] = 0x00000000;
   vs_cmd[i++] = 0x60000000; /* ?? */

   vs_cmd[i++] = info->index_size ? 0x00018000 : 0x00000000; /* ARRAYS_SEMAPHORE_NEXT : ARRAYS_SEMAPHORE_END */
   vs_cmd[i++] = 0x50000000; /* ARRAYS_SEMAPHORE */

   assert(i <= max_n);
   ctx->vs_cmd_array.size += i * 4;

   lima_dump_command_stream_print(vs_cmd, i * 4, false, "add vs cmd\n");
}

static bool
lima_is_scissor_zero(struct lima_context *ctx)
{
   if (!ctx->rasterizer->base.scissor)
      return false;

   struct pipe_scissor_state *scissor = &ctx->scissor;
   return
      scissor->minx == scissor->maxx
      && scissor->miny == scissor->maxy;
}

static void
lima_pack_plbu_cmd(struct lima_context *ctx, const struct pipe_draw_info *info)
{
   int i = 0, max_n = 40;
   uint32_t *plbu_cmd = util_dynarray_enlarge(&ctx->plbu_cmd_array, max_n * 4);

   /* first draw need create a PLBU command header */
   if (!ctx->plbu_cmd_array.size) {
      struct lima_context_framebuffer *fb = &ctx->framebuffer;

      plbu_cmd[i++] = 0x00000200;
      plbu_cmd[i++] = 0x1000010B; /* PRIMITIVE_SETUP */

      plbu_cmd[i++] = (fb->shift_max << 28) | (fb->shift_h << 16) | fb->shift_w;
      plbu_cmd[i++] = 0x1000010C; /* BLOCK_STEP */

      plbu_cmd[i++] = ((fb->tiled_w - 1) << 24) | ((fb->tiled_h - 1) << 8);
      plbu_cmd[i++] = 0x10000109; /* TILED_DIMENSIONS */

      plbu_cmd[i++] = fb->block_w;
      plbu_cmd[i++] = 0x30000000; /* PLBU_BLOCK_STRIDE */

      plbu_cmd[i++] = ctx->plb_gp_stream->va + ctx->plb_index * ctx->plb_gp_size;
      plbu_cmd[i++] = 0x28000000 | (fb->block_w * fb->block_h - 1); /* PLBU_ARRAY_ADDRESS */

      plbu_cmd[i++] = fui(ctx->viewport.x);
      plbu_cmd[i++] = 0x10000107; /* VIEWPORT_X */

      plbu_cmd[i++] = fui(ctx->viewport.width);
      plbu_cmd[i++] = 0x10000108; /* VIEWPORT_W */

      plbu_cmd[i++] = fui(ctx->viewport.y);
      plbu_cmd[i++] = 0x10000105; /* VIEWPORT_Y */

      plbu_cmd[i++] = fui(ctx->viewport.height);
      plbu_cmd[i++] = 0x10000106; /* VIEWPORT_H */
   }

   /* If it's zero scissor, we skip adding all other commands */
   if (lima_is_scissor_zero(ctx))
      goto done;

   if (!info->index_size) {
      plbu_cmd[i++] = 0x00010002; /* ARRAYS_SEMAPHORE_BEGIN */
      plbu_cmd[i++] = 0x60000000; /* ARRAYS_SEMAPHORE */
   }

   int cf = ctx->rasterizer->base.cull_face;
   int ccw = ctx->rasterizer->base.front_ccw;
   uint32_t cull = 0;
   if (cf != PIPE_FACE_NONE) {
      if (cf & PIPE_FACE_FRONT)
         cull |= ccw ? 0x00040000 : 0x00020000;
      if (cf & PIPE_FACE_BACK)
         cull |= ccw ? 0x00020000 : 0x00040000;
   }
   plbu_cmd[i++] = 0x00002000 | 0x00000200 | cull |
      (info->index_size == 2 ? 0x00000400 : 0);
   plbu_cmd[i++] = 0x1000010B; /* PRIMITIVE_SETUP */

   uint32_t gl_position_va = lima_ctx_buff_va(ctx, lima_ctx_buff_sh_gl_pos);
   plbu_cmd[i++] = lima_ctx_buff_va(ctx, lima_ctx_buff_pp_plb_rsw);
   plbu_cmd[i++] = 0x80000000 | (gl_position_va >> 4); /* RSW_VERTEX_ARRAY */

   /* TODO
    * - we should set it only for the first draw that enabled the scissor and for
    *   latter draw only if scissor is dirty
    * - check why scissor is not affecting bounds of region cleared by glClear
    */
   if (ctx->rasterizer->base.scissor) {
      struct pipe_scissor_state *scissor = &ctx->scissor;
      plbu_cmd[i++] = (scissor->minx << 30) | (scissor->maxy - 1) << 15 | scissor->miny;
      plbu_cmd[i++] = 0x70000000 | (scissor->maxx - 1) << 13 | (scissor->minx >> 2); /* PLBU_CMD_SCISSORS */
   }

   plbu_cmd[i++] = 0x00000000;
   plbu_cmd[i++] = 0x1000010A; /* ?? */

   plbu_cmd[i++] = fui(ctx->viewport.near);
   plbu_cmd[i++] = 0x1000010E; /* DEPTH_RANGE_NEAR */

   plbu_cmd[i++] = fui(ctx->viewport.far);
   plbu_cmd[i++] = 0x1000010F; /* DEPTH_RANGE_FAR */

   if (info->index_size) {
      plbu_cmd[i++] = gl_position_va;
      plbu_cmd[i++] = 0x10000100; /* INDEXED_DEST */

      struct pipe_resource *indexbuf = NULL;
      unsigned index_offset = 0;
      struct lima_resource *res;
      if (info->has_user_indices) {
         util_upload_index_buffer(&ctx->base, info, &indexbuf, &index_offset);
         res = lima_resource(indexbuf);
      }
      else
         res = lima_resource(info->index.resource);

      lima_bo_update(res->bo, false, true);
      lima_submit_add_bo(ctx->gp_submit, res->bo, LIMA_SUBMIT_BO_READ);
      plbu_cmd[i++] = res->bo->va + info->start * info->index_size + index_offset;
      plbu_cmd[i++] = 0x10000101; /* INDICES */

      if (indexbuf)
         pipe_resource_reference(&indexbuf, NULL);
   }
   else {
      /* can this make the attribute info static? */
      plbu_cmd[i++] = (info->count << 24) | info->start;
      plbu_cmd[i++] = 0x00000000 | 0x00000000 |
         ((info->mode & 0x1F) << 16) | (info->count >> 8); /* DRAW | DRAW_ARRAYS */
   }

   plbu_cmd[i++] = 0x00010001; /* ARRAYS_SEMAPHORE_END */
   plbu_cmd[i++] = 0x60000000; /* ARRAYS_SEMAPHORE */

   if (info->index_size) {
      unsigned num = info->max_index - info->min_index + 1;
      plbu_cmd[i++] = (num << 24) | info->min_index;
      plbu_cmd[i++] = 0x00000000 | 0x00200000 |
         ((info->mode & 0x1F) << 16) | (info->min_index >> 8); /* DRAW | DRAW_ELEMENTS */
   }

done:
   assert(i <= max_n);
   ctx->plbu_cmd_array.size += i * 4;

   lima_dump_command_stream_print(plbu_cmd, i * 4, false, "add plbu cmd\n");
}

struct lima_render_state {
   uint32_t blend_color_bg;
   uint32_t blend_color_ra;
   uint32_t alpha_blend;
   uint32_t depth_test;
   uint32_t depth_range;
   uint32_t stencil_front;
   uint32_t stencil_back;
   uint32_t stencil_test;
   uint32_t multi_sample;
   uint32_t shader_address;
   uint32_t varying_types;
   uint32_t uniforms_address;
   uint32_t textures_address;
   uint32_t aux0;
   uint32_t aux1;
   uint32_t varyings_address;
};

static int
lima_blend_func(enum pipe_blend_func pipe)
{
   switch (pipe) {
   case PIPE_BLEND_ADD:
      return 2;
   case PIPE_BLEND_SUBTRACT:
      return 0;
   case PIPE_BLEND_REVERSE_SUBTRACT:
      return 1;
   case PIPE_BLEND_MIN:
      return 4;
   case PIPE_BLEND_MAX:
      return 5;
   }
   return -1;
}

static int
lima_blend_factor(enum pipe_blendfactor pipe)
{
   switch (pipe) {
   case PIPE_BLENDFACTOR_ONE:
      return 11;
   case PIPE_BLENDFACTOR_SRC_COLOR:
      return 0;
   case PIPE_BLENDFACTOR_SRC_ALPHA:
      return 16;
   case PIPE_BLENDFACTOR_DST_ALPHA:
      return 17;
   case PIPE_BLENDFACTOR_DST_COLOR:
      return 1;
   case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE:
      return 7;
   case PIPE_BLENDFACTOR_CONST_COLOR:
      return 2;
   case PIPE_BLENDFACTOR_CONST_ALPHA:
      return 18;
   case PIPE_BLENDFACTOR_ZERO:
      return 3;
   case PIPE_BLENDFACTOR_INV_SRC_COLOR:
      return 8;
   case PIPE_BLENDFACTOR_INV_SRC_ALPHA:
      return 24;
   case PIPE_BLENDFACTOR_INV_DST_ALPHA:
      return 25;
   case PIPE_BLENDFACTOR_INV_DST_COLOR:
      return 9;
   case PIPE_BLENDFACTOR_INV_CONST_COLOR:
      return 10;
   case PIPE_BLENDFACTOR_INV_CONST_ALPHA:
      return 26;
   case PIPE_BLENDFACTOR_SRC1_COLOR:
   case PIPE_BLENDFACTOR_SRC1_ALPHA:
   case PIPE_BLENDFACTOR_INV_SRC1_COLOR:
   case PIPE_BLENDFACTOR_INV_SRC1_ALPHA:
      return -1; /* not support */
   }
   return -1;
}

static int
lima_calculate_alpha_blend(enum pipe_blend_func rgb_func, enum pipe_blend_func alpha_func,
                           enum pipe_blendfactor rgb_src_factor, enum pipe_blendfactor rgb_dst_factor,
                           enum pipe_blendfactor alpha_src_factor, enum pipe_blendfactor alpha_dst_factor)
{
   return lima_blend_func(rgb_func) |
      (lima_blend_func(alpha_func) << 3) |
      (lima_blend_factor(rgb_src_factor) << 6) |
      (lima_blend_factor(rgb_dst_factor) << 11) |
      ((lima_blend_factor(alpha_src_factor) & 0xF) << 16) |
      ((lima_blend_factor(alpha_dst_factor) & 0xF) << 20) |
      0x0C000000; /* need check if this GLESv1 glAlphaFunc */
}

static int
lima_stencil_op(enum pipe_stencil_op pipe)
{
   switch (pipe) {
   case PIPE_STENCIL_OP_KEEP:
      return 0;
   case PIPE_STENCIL_OP_ZERO:
      return 2;
   case PIPE_STENCIL_OP_REPLACE:
      return 1;
   case PIPE_STENCIL_OP_INCR:
      return 6;
   case PIPE_STENCIL_OP_DECR:
      return 7;
   case PIPE_STENCIL_OP_INCR_WRAP:
      return 4;
   case PIPE_STENCIL_OP_DECR_WRAP:
      return 5;
   case PIPE_STENCIL_OP_INVERT:
      return 3;
   }
   return -1;
}

static int
lima_calculate_depth_test(struct pipe_depth_state *depth, struct pipe_rasterizer_state *rst)
{
   enum pipe_compare_func func = (depth->enabled ? depth->func : PIPE_FUNC_ALWAYS);

   int offset_scale = 0;

   //TODO: implement polygon offset
#if 0
   if (rst->offset_scale < -32)
      offset_scale = -32;
   else if (rst->offset_scale > 31)
      offset_scale = 31;
   else
      offset_scale = rst->offset_scale * 4;

   if (offset_scale < 0)
      offset_scale = 0x100 + offset_scale;
#endif

   return (depth->enabled && depth->writemask) |
      ((int)func << 1) |
      (offset_scale << 16) |
      0x30; /* find out what is this */
}

static void
lima_pack_render_state(struct lima_context *ctx)
{
   struct lima_render_state *render =
      lima_ctx_buff_alloc(ctx, lima_ctx_buff_pp_plb_rsw,
                          sizeof(*render), LIMA_CTX_BUFF_SUBMIT_PP, true);

   /* do hw support RGBA independ blend?
    * PIPE_CAP_INDEP_BLEND_ENABLE
    *
    * how to handle the no cbuf only zbuf case?
    */
   struct pipe_rt_blend_state *rt = ctx->blend->base.rt;
   render->blend_color_bg = float_to_ubyte(ctx->blend_color.color[2]) |
      (float_to_ubyte(ctx->blend_color.color[1]) << 16);
   render->blend_color_ra = float_to_ubyte(ctx->blend_color.color[0]) |
      (float_to_ubyte(ctx->blend_color.color[3]) << 16);

   if (rt->blend_enable) {
      render->alpha_blend = lima_calculate_alpha_blend(rt->rgb_func, rt->alpha_func,
         rt->rgb_src_factor, rt->rgb_dst_factor,
         rt->alpha_src_factor, rt->alpha_dst_factor);
   }
   else {
      /*
       * Special handling for blending disabled.
       * Binary driver is generating the same alpha_value,
       * as when we would just enable blending, without changing/setting any blend equation/params.
       * Normaly in this case mesa would set all rt fields (func/factor) to zero.
       */
      render->alpha_blend = lima_calculate_alpha_blend(PIPE_BLEND_ADD, PIPE_BLEND_ADD,
         PIPE_BLENDFACTOR_ONE, PIPE_BLENDFACTOR_ZERO,
         PIPE_BLENDFACTOR_ONE, PIPE_BLENDFACTOR_ZERO);
   }

   render->alpha_blend |= (rt->colormask & PIPE_MASK_RGBA) << 28;

   struct pipe_rasterizer_state *rst = &ctx->rasterizer->base;
   struct pipe_depth_state *depth = &ctx->zsa->base.depth;
   render->depth_test = lima_calculate_depth_test(depth, rst);

   /* overlap with plbu? any place can remove one? */
   render->depth_range = float_to_ushort(ctx->viewport.near) |
      (float_to_ushort(ctx->viewport.far) << 16);

#if 0
   struct pipe_stencil_state *stencil = ctx->zsa->base.stencil;
   struct pipe_stencil_ref *ref = &ctx->stencil_ref;
   render->stencil_front = stencil[0].func |
      (lima_stencil_op(stencil[0].fail_op) << 3) |
      (lima_stencil_op(stencil[0].zfail_op) << 6) |
      (lima_stencil_op(stencil[0].zpass_op) << 9) |
      (ref->ref_value[0] << 16) |
      (stencil[0].valuemask << 24);
   render->stencil_back = stencil[1].func |
      (lima_stencil_op(stencil[1].fail_op) << 3) |
      (lima_stencil_op(stencil[1].zfail_op) << 6) |
      (lima_stencil_op(stencil[1].zpass_op) << 9) |
      (ref->ref_value[1] << 16) |
      (stencil[1].valuemask << 24);
#else
   render->stencil_front = 0xff000007;
   render->stencil_back = 0xff000007;
#endif

   /* seems not correct? */
   //struct pipe_alpha_state *alpha = &ctx->zsa->base.alpha;
   render->stencil_test = 0;
   //(stencil->enabled ? 0xFF : 0x00) | (float_to_ubyte(alpha->ref_value) << 16)

   /* need more investigation */
   render->multi_sample = 0x0000F807;

   render->shader_address =
      ctx->fs->bo->va | (((uint32_t *)ctx->fs->bo->map)[0] & 0x1F);

   /* seems not needed */
   render->uniforms_address = 0x00000000;

   render->textures_address = 0x00000000;

   /* more investigation */
   render->aux0 = 0x00000300 | (ctx->vs->varying_stride >> 3);
   render->aux1 = 0x00003000;

   if (ctx->tex_stateobj.num_samplers) {
      render->textures_address = lima_ctx_buff_va(ctx, lima_ctx_buff_pp_tex_desc);
      render->aux0 |= ctx->tex_stateobj.num_samplers << 14;
      render->aux0 |= 0x20;
   }

   if (ctx->const_buffer[PIPE_SHADER_FRAGMENT].buffer) {
      render->uniforms_address = lima_ctx_buff_va(ctx, lima_ctx_buff_pp_uniform_array);
      render->uniforms_address |= ((ctx->buffer_state[lima_ctx_buff_pp_uniform].size) / 4 - 1);
      render->aux0 |= 0x80;
      render->aux1 |= 0x10000;
   }

   if (ctx->vs->num_varying > 1) {
      render->varying_types = 0x00000000;
      render->varyings_address = lima_ctx_buff_va(ctx, lima_ctx_buff_sh_varying);
      for (int i = 1; i < ctx->vs->num_varying; i++) {
         int val;

         struct lima_varying_info *v = ctx->vs->varying + i;
         if (v->component_size == 4)
            val = v->components == 4 ? 0 : 1;
         else
            val = v->components == 4 ? 2 : 3;

         int index = i - 1;
         if (index < 10)
            render->varying_types |= val << (3 * index);
         else if (index == 10) {
            render->varying_types |= val << 30;
            render->varyings_address |= val >> 2;
         }
         else if (index == 11)
            render->varyings_address |= val << 1;
      }
   }
   else {
      render->varying_types = 0x00000000;
      render->varyings_address = 0x00000000;
   }

   lima_dump_command_stream_print(
      render, sizeof(*render), false, "add render state at va %x\n",
      lima_ctx_buff_va(ctx, lima_ctx_buff_pp_plb_rsw));
}

static void
lima_update_gp_attribute_info(struct lima_context *ctx, const struct pipe_draw_info *info)
{
   struct lima_vertex_element_state *ve = ctx->vertex_elements;
   struct lima_context_vertex_buffer *vb = &ctx->vertex_buffers;

   uint32_t *attribute =
      lima_ctx_buff_alloc(ctx, lima_ctx_buff_gp_attribute_info,
                          ve->num_elements * 8, LIMA_CTX_BUFF_SUBMIT_GP, true);

   int n = 0;
   for (int i = 0; i < ve->num_elements; i++) {
      struct pipe_vertex_element *pve = ve->pipe + i;

      assert(pve->vertex_buffer_index < vb->count);
      assert(vb->enabled_mask & (1 << pve->vertex_buffer_index));

      struct pipe_vertex_buffer *pvb = vb->vb + pve->vertex_buffer_index;
      struct lima_resource *res = lima_resource(pvb->buffer.resource);
      lima_bo_update(res->bo, false, true);

      lima_submit_add_bo(ctx->gp_submit, res->bo, LIMA_SUBMIT_BO_READ);

      unsigned start = info->index_size ? info->min_index : info->start;
      attribute[n++] = res->bo->va + pvb->buffer_offset + pve->src_offset
         + start * pvb->stride;
      attribute[n++] = (pvb->stride << 11) |
         (lima_pipe_format_to_attrib_type(pve->src_format) << 2) |
         (util_format_get_nr_components(pve->src_format) - 1);
   }

   lima_dump_command_stream_print(
      attribute, n * 4, false, "update attribute info at va %x\n",
      lima_ctx_buff_va(ctx, lima_ctx_buff_gp_attribute_info));
}

static void
lima_update_gp_uniform(struct lima_context *ctx)
{
   struct lima_context_constant_buffer *ccb =
      ctx->const_buffer + PIPE_SHADER_VERTEX;
   struct lima_vs_shader_state *vs = ctx->vs;

   void *vs_const_buff =
      lima_ctx_buff_alloc(ctx, lima_ctx_buff_gp_uniform,
                          ccb->size + vs->constant_size + 32,
                          LIMA_CTX_BUFF_SUBMIT_GP, true);

   if (ccb->buffer)
      memcpy(vs_const_buff, ccb->buffer, ccb->size);

   memcpy(vs_const_buff + ccb->size, ctx->viewport.transform.scale,
          sizeof(ctx->viewport.transform.scale));
   memcpy(vs_const_buff + ccb->size + 16, ctx->viewport.transform.translate,
          sizeof(ctx->viewport.transform.translate));

   if (vs->constant)
      memcpy(vs_const_buff + ccb->size + 32, vs->constant, vs->constant_size);

   lima_dump_command_stream_print(
      vs_const_buff, ccb->size + vs->constant_size + 32, true,
      "update gp uniform at va %x\n",
      lima_ctx_buff_va(ctx, lima_ctx_buff_gp_uniform));
}

static void
lima_update_pp_uniform(struct lima_context *ctx)
{
   const float *const_buff = ctx->const_buffer[PIPE_SHADER_FRAGMENT].buffer;
   size_t const_buff_size = ctx->const_buffer[PIPE_SHADER_FRAGMENT].size / sizeof(float);

   if (!const_buff)
      return;

   uint16_t *fp16_const_buff =
      lima_ctx_buff_alloc(ctx, lima_ctx_buff_pp_uniform,
                          const_buff_size * sizeof(uint16_t),
                          LIMA_CTX_BUFF_SUBMIT_PP, true);

   uint32_t *array =
      lima_ctx_buff_alloc(ctx, lima_ctx_buff_pp_uniform_array,
                          4, LIMA_CTX_BUFF_SUBMIT_PP, true);

   for (int i = 0; i < const_buff_size; i++)
       fp16_const_buff[i] = util_float_to_half(const_buff[i]);

   *array = lima_ctx_buff_va(ctx, lima_ctx_buff_pp_uniform);

   lima_dump_command_stream_print(
      fp16_const_buff, const_buff_size * 2, false, "add pp uniform data at va %x\n",
      lima_ctx_buff_va(ctx, lima_ctx_buff_pp_uniform));
   lima_dump_command_stream_print(
      array, 4, false, "add pp uniform info at va %x\n",
      lima_ctx_buff_va(ctx, lima_ctx_buff_pp_uniform_array));
}

static void
lima_update_varying(struct lima_context *ctx, const struct pipe_draw_info *info)
{
   struct lima_vs_shader_state *vs = ctx->vs;

   uint32_t *varying =
      lima_ctx_buff_alloc(ctx, lima_ctx_buff_gp_varying_info,
                          vs->num_varying * 8, LIMA_CTX_BUFF_SUBMIT_GP, true);
   int n = 0;

   /* should be LIMA_SUBMIT_BO_WRITE for GP, but each draw will use
    * different part of this bo, so no need to set exclusive constraint */
   lima_ctx_buff_alloc(ctx, lima_ctx_buff_sh_gl_pos,
                       4 * 4 * info->count,
                       LIMA_CTX_BUFF_SUBMIT_GP | LIMA_CTX_BUFF_SUBMIT_PP,
                       false);

   /* for gl_Position */
   varying[n++] = lima_ctx_buff_va(ctx, lima_ctx_buff_sh_gl_pos);
   varying[n++] = 0x8020;

   int offset = 0;
   for (int i = 1; i < vs->num_varying; i++) {
      struct lima_varying_info *v = vs->varying + i;

      v->components = align(v->components, 2);

      int size = v->components * v->component_size;
      size = align(size, 8);
      if (size == 16)
         offset = align(offset, 16);

      v->offset = offset;
      offset += size;
   }
   vs->varying_stride = align(offset, 8);

   if (vs->num_varying > 1)
      lima_ctx_buff_alloc(ctx, lima_ctx_buff_sh_varying,
                          vs->varying_stride * info->count,
                          LIMA_CTX_BUFF_SUBMIT_GP | LIMA_CTX_BUFF_SUBMIT_PP,
                          false);

   for (int i = 1; i < vs->num_varying; i++) {
      struct lima_varying_info *v = vs->varying + i;
      varying[n++] = lima_ctx_buff_va(ctx, lima_ctx_buff_sh_varying) + v->offset;
      varying[n++] = (vs->varying_stride << 11) | (v->components - 1) |
         (v->component_size == 2 ? 0x0C : 0);
   }

   lima_dump_command_stream_print(
      varying, n * 4, false, "update varying info at va %x\n",
      lima_ctx_buff_va(ctx, lima_ctx_buff_gp_varying_info));
}

static void
lima_update_submit_bo(struct lima_context *ctx)
{
   lima_submit_add_bo(ctx->gp_submit, ctx->vs->bo, LIMA_SUBMIT_BO_READ);
   lima_submit_add_bo(ctx->pp_submit, ctx->fs->bo, LIMA_SUBMIT_BO_READ);

   if (!ctx->num_draws) {
      struct lima_screen *screen = lima_screen(ctx->base.screen);
      lima_submit_add_bo(ctx->gp_submit, ctx->plb_gp_stream, LIMA_SUBMIT_BO_READ);
      lima_submit_add_bo(ctx->gp_submit, ctx->plb[ctx->plb_index], LIMA_SUBMIT_BO_WRITE);
      lima_submit_add_bo(ctx->gp_submit, screen->gp_buffer, LIMA_SUBMIT_BO_READ);

      lima_dump_command_stream_print(
         ctx->plb_gp_stream->map + ctx->plb_index * ctx->plb_gp_size,
         ctx->plb_gp_size, false, "gp plb stream at va %x\n",
         ctx->plb_gp_stream->va + ctx->plb_index * ctx->plb_gp_size);

      if (ctx->plb_pp_stream) {
         struct lima_ctx_plb_pp_stream_key key = {
            .plb_index = ctx->plb_index,
            .tiled_w = ctx->framebuffer.tiled_w,
            .tiled_h = ctx->framebuffer.tiled_h,
         };

         struct hash_entry *entry =
            _mesa_hash_table_search(ctx->plb_pp_stream, &key);
         struct lima_ctx_plb_pp_stream *s = entry->data;
         lima_update_plb(ctx, s);
         ctx->current_plb_pp_stream = s;

         lima_submit_add_bo(ctx->pp_submit, s->bo, LIMA_SUBMIT_BO_READ);
      }

      struct lima_resource *res = lima_resource(ctx->framebuffer.cbuf->texture);
      lima_submit_add_bo(ctx->pp_submit, res->bo, LIMA_SUBMIT_BO_WRITE);
      lima_submit_add_bo(ctx->pp_submit, ctx->plb[ctx->plb_index], LIMA_SUBMIT_BO_READ);
      lima_submit_add_bo(ctx->pp_submit, screen->pp_buffer, LIMA_SUBMIT_BO_READ);
   }
}

static void
lima_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info)
{
   debug_checkpoint();

   struct lima_context *ctx = lima_context(pctx);

   if (!ctx->vs || !ctx->fs) {
      debug_warn_once("no shader, skip draw\n");
      return;
   }

   if (!lima_update_vs_state(ctx) || !lima_update_fs_state(ctx))
      return;

   lima_dump_command_stream_print(
      ctx->vs->bo->map, ctx->vs->shader_size, false,
      "add vs at va %x\n", ctx->vs->bo->va);

   lima_dump_command_stream_print(
      ctx->fs->bo->map, ctx->fs->shader_size, false,
      "add fs at va %x\n", ctx->fs->bo->va);

   lima_update_submit_bo(ctx);

   lima_update_gp_attribute_info(ctx, info);

   if ((ctx->dirty & LIMA_CONTEXT_DIRTY_CONST_BUFF &&
        ctx->const_buffer[PIPE_SHADER_VERTEX].dirty) ||
       ctx->dirty & LIMA_CONTEXT_DIRTY_VIEWPORT ||
       ctx->dirty & LIMA_CONTEXT_DIRTY_SHADER_VERT) {
      lima_update_gp_uniform(ctx);
      ctx->const_buffer[PIPE_SHADER_VERTEX].dirty = false;
   }

   lima_update_varying(ctx, info);

   /* If it's zero scissor, don't build vs cmd list */
   if (!lima_is_scissor_zero(ctx))
      lima_pack_vs_cmd(ctx, info);

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_CONST_BUFF &&
       ctx->const_buffer[PIPE_SHADER_FRAGMENT].dirty) {
      lima_update_pp_uniform(ctx);
      ctx->const_buffer[PIPE_SHADER_FRAGMENT].dirty = false;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_TEXTURES)
      lima_update_textures(ctx);

   lima_pack_render_state(ctx);
   lima_pack_plbu_cmd(ctx, info);

   ctx->dirty = 0;
   ctx->num_draws++;
}

static void
lima_finish_plbu_cmd(struct lima_context *ctx)
{
   int i = 0;
   uint32_t *plbu_cmd = util_dynarray_enlarge(&ctx->plbu_cmd_array, 2 * 4);

   plbu_cmd[i++] = 0x00000000;
   plbu_cmd[i++] = 0x50000000; /* END */

   ctx->plbu_cmd_array.size += i * 4;
}

static void
lima_pack_pp_frame_reg(struct lima_context *ctx, uint32_t *frame_reg,
                       uint32_t *wb_reg)
{
   struct lima_resource *res = lima_resource(ctx->framebuffer.cbuf->texture);
   lima_bo_update(res->bo, false, true);

   bool swap_channels = false;
   switch (ctx->framebuffer.cbuf->format) {
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      swap_channels = true;
      break;
   default:
      break;
   }

   struct lima_pp_frame_reg *frame = (void *)frame_reg;
   struct lima_screen *screen = lima_screen(ctx->base.screen);
   frame->render_address = screen->pp_buffer->va + pp_frame_rsw_offset;
   frame->flags = 0x02;
   frame->clear_value_depth = ctx->clear.depth;
   frame->clear_value_stencil = ctx->clear.stencil;
   frame->clear_value_color = ctx->clear.color;
   frame->clear_value_color_1 = ctx->clear.color;
   frame->clear_value_color_2 = ctx->clear.color;
   frame->clear_value_color_3 = ctx->clear.color;
   frame->one = 1;
   frame->supersampled_height = ctx->framebuffer.height * 2 - 1;
   frame->dubya = 0x77;
   frame->onscreen = 1;
   frame->blocking = (ctx->framebuffer.shift_max << 28) |
      (ctx->framebuffer.shift_h << 16) | ctx->framebuffer.shift_w;
   frame->scale = 0xE0C;
   frame->foureight = 0x8888;

   struct lima_pp_wb_reg *wb = (void *)wb_reg;
   wb[0].type = 0x02; /* 1 for depth, stencil */
   wb[0].address = res->bo->va;
   wb[0].pixel_format = 0x03; /* BGRA8888 */
   wb[0].pitch = res->stride / 8;
   wb[0].mrt_bits = swap_channels ? 0x4 : 0x0;
}

void
lima_flush(struct lima_context *ctx)
{
   if (!ctx->num_draws) {
      debug_printf("%s: do nothing\n", __FUNCTION__);
      return;
   }

   lima_finish_plbu_cmd(ctx);

   int vs_cmd_size = ctx->vs_cmd_array.size;
   int plbu_cmd_size = ctx->plbu_cmd_array.size;

   void *vs_cmd =
      lima_ctx_buff_alloc(ctx, lima_ctx_buff_gp_vs_cmd, vs_cmd_size,
                          LIMA_CTX_BUFF_SUBMIT_GP, true);
   memcpy(vs_cmd, util_dynarray_begin(&ctx->vs_cmd_array), vs_cmd_size);
   util_dynarray_clear(&ctx->vs_cmd_array);

   void *plbu_cmd =
      lima_ctx_buff_alloc(ctx, lima_ctx_buff_gp_plbu_cmd, plbu_cmd_size,
                          LIMA_CTX_BUFF_SUBMIT_GP, true);
   memcpy(plbu_cmd, util_dynarray_begin(&ctx->plbu_cmd_array), plbu_cmd_size);
   util_dynarray_clear(&ctx->plbu_cmd_array);

   struct lima_screen *screen = lima_screen(ctx->base.screen);
   uint32_t vs_cmd_va = lima_ctx_buff_va(ctx, lima_ctx_buff_gp_vs_cmd);
   uint32_t plbu_cmd_va = lima_ctx_buff_va(ctx, lima_ctx_buff_gp_plbu_cmd);
   struct drm_lima_gp_frame gp_frame;
   struct lima_gp_frame_reg *gp_frame_reg = (void *)gp_frame.frame;
   gp_frame_reg->vs_cmd_start = vs_cmd_va;
   gp_frame_reg->vs_cmd_end = vs_cmd_va + vs_cmd_size;
   gp_frame_reg->plbu_cmd_start = plbu_cmd_va;
   gp_frame_reg->plbu_cmd_end = plbu_cmd_va + plbu_cmd_size;
   gp_frame_reg->tile_heap_start = screen->gp_buffer->va + gp_tile_heap_offset;
   gp_frame_reg->tile_heap_end = screen->gp_buffer->va + gp_buffer_size;

   lima_dump_command_stream_print(
      vs_cmd, vs_cmd_size, false, "flush vs cmd at va %x\n", vs_cmd_va);

   lima_dump_command_stream_print(
      plbu_cmd, plbu_cmd_size, false, "flush plbu cmd at va %x\n", plbu_cmd_va);

   lima_dump_command_stream_print(
      &gp_frame, sizeof(gp_frame), false, "add gp frame\n");

   if (!lima_submit_start(ctx->gp_submit, &gp_frame, sizeof(gp_frame)))
      fprintf(stderr, "gp submit error\n");

   if (lima_dump_command_stream) {
      if (lima_submit_wait(ctx->gp_submit, PIPE_TIMEOUT_INFINITE, false)) {
         float *pos = lima_ctx_buff_map(ctx, lima_ctx_buff_sh_gl_pos);
         lima_dump_command_stream_print(
            pos, 4 * 4 * 16, true, "gl_pos dump at va %x\n",
            lima_ctx_buff_va(ctx, lima_ctx_buff_sh_gl_pos));

         lima_bo_update(ctx->plb[ctx->plb_index], true, false);
         uint32_t *plb = ctx->plb[ctx->plb_index]->map;
         lima_dump_command_stream_print(
            plb, LIMA_CTX_PLB_BLK_SIZE, false, "plb dump at va %x\n",
            ctx->plb[ctx->plb_index]->va);
      }
      else
         fprintf(stderr, "gp submit wait error\n");
   }

   if (screen->gpu_type == LIMA_INFO_GPU_MALI400) {
      struct drm_lima_m400_pp_frame pp_frame = {0};
      lima_pack_pp_frame_reg(ctx, pp_frame.frame, pp_frame.wb);
      pp_frame.num_pp = screen->num_pp;

      struct lima_ctx_plb_pp_stream *s = ctx->current_plb_pp_stream;
      for (int i = 0; i < screen->num_pp; i++)
         pp_frame.plbu_array_address[i] = s->bo->va + s->offset[i];

      lima_dump_command_stream_print(
         &pp_frame, sizeof(pp_frame), false, "add pp frame\n");

      if (!lima_submit_start(ctx->pp_submit, &pp_frame, sizeof(pp_frame)))
         fprintf(stderr, "pp submit error\n");

      ctx->current_plb_pp_stream = NULL;
   }
   else {
      struct drm_lima_m450_pp_frame pp_frame = {0};
      lima_pack_pp_frame_reg(ctx, pp_frame.frame, pp_frame.wb);

      struct lima_context_framebuffer *fb = &ctx->framebuffer;
      pp_frame.dlbu_regs[0] = ctx->plb[ctx->plb_index]->va;
      pp_frame.dlbu_regs[1] = ((fb->tiled_h - 1) << 16) | (fb->tiled_w - 1);
      unsigned s = util_logbase2(LIMA_CTX_PLB_BLK_SIZE) - 7;
      pp_frame.dlbu_regs[2] = (s << 28) | (fb->shift_h << 16) | fb->shift_w;
      pp_frame.dlbu_regs[3] = ((fb->tiled_h - 1) << 24) | ((fb->tiled_w - 1) << 16);

      lima_dump_command_stream_print(
         &pp_frame, sizeof(pp_frame), false, "add pp frame\n");

      if (!lima_submit_start(ctx->pp_submit, &pp_frame, sizeof(pp_frame)))
         fprintf(stderr, "pp submit error\n");
   }

   ctx->num_draws = 0;
   ctx->plb_index = (ctx->plb_index + 1) % lima_ctx_num_plb;
}

static void
lima_pipe_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence,
                unsigned flags)
{
   debug_checkpoint();
   debug_printf("%s: flags=%x\n", __FUNCTION__, flags);

   struct lima_context *ctx = lima_context(pctx);
   lima_flush(ctx);
}

void
lima_draw_init(struct lima_context *ctx)
{
   ctx->base.clear = lima_clear;
   ctx->base.draw_vbo = lima_draw_vbo;
   ctx->base.flush = lima_pipe_flush;
}
