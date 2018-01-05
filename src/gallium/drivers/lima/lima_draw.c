/*
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

#include "lima_context.h"
#include "lima_screen.h"
#include "lima_resource.h"

#include <lima_drm.h>

bool lima_dump_command_stream = false;

static void lima_dump_blob(void *data, int size, bool is_float)
{
   if (is_float) {
      float *blob = data;
      for (int i = 0; i * 4 < size; i += 4)
         printf ("%04x: %f %f %f %f\n", i * 4,
                 blob[i], blob[i + 1], blob[i + 2], blob[i + 3]);
   }
   else {
      uint32_t *blob = data;
      for (int i = 0; i * 4 < size; i += 4)
         printf ("%04x: 0x%08x 0x%08x 0x%08x 0x%08x\n", i * 4,
                 blob[i], blob[i + 1], blob[i + 2], blob[i + 3]);
   }
}

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
      clear->depth = fui(depth);

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
lima_update_plb(struct lima_context *ctx)
{
   struct lima_context_framebuffer *fb = &ctx->framebuffer;
   struct lima_screen *screen = lima_screen(ctx->base.screen);

   /* use hilbert_coords to generates 1D to 2D relationship.
    * 1D for pp stream index and 2D for plb block x/y on framebuffer.
    * if multi pp, interleave the 1D index to make each pp's render target
    * close enough which should result close workload
    */
   int max = MAX2(fb->tiled_w, fb->tiled_h);
   int dim = util_logbase2_ceil(max);
   int count = 1 << (dim + dim);
   int index = 0, i;
   int num_pp = screen->info.num_pp;
   uint32_t *stream[4];

   for (i = 0; i < num_pp; i++)
      stream[i] = ctx->pp_buffer->map + pp_plb_offset(i, num_pp);

   for (i = 0; i < count; i++) {
      int x, y;
      hilbert_coords(max, i, &x, &y);
      if (x < fb->tiled_w && y < fb->tiled_h) {
         int pp = index % num_pp;
         int offset = ((y >> fb->shift_h) * fb->block_w + (x >> fb->shift_w)) * 512;
         int plb_va = ctx->share_buffer->va + sh_plb_offset + offset;

         stream[pp][0] = 0;
         stream[pp][1] = 0xB8000000 | x | (y << 8);
         stream[pp][2] = 0xE0000002 | ((plb_va >> 3) & ~0xE0000003);
         stream[pp][3] = 0xB0000000;

         stream[pp] += 4;
         index++;
      }
   }

   for (i = 0; i < num_pp; i++) {
      stream[i][0] = 0;
      stream[i][1] = 0xBC000000;
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
   ctx->buffer_state[lima_ctx_buff_gp_vs_cmd].offset +=
      ctx->buffer_state[lima_ctx_buff_gp_vs_cmd].size;

   int i = 0;
   uint32_t *vs_cmd = ctx->gp_buffer->map + gp_vs_cmd_offset +
      ctx->buffer_state[lima_ctx_buff_gp_vs_cmd].offset;

   if (!info->index_size) {
      vs_cmd[i++] = 0x00028000; /* ARRAYS_SEMAPHORE_BEGIN_1 */
      vs_cmd[i++] = 0x50000000; /* ARRAYS_SEMAPHORE */
      vs_cmd[i++] = 0x00000001; /* ARRAYS_SEMAPHORE_BEGIN_2 */
      vs_cmd[i++] = 0x50000000; /* ARRAYS_SEMAPHORE */
   }

   int uniform_size = ctx->const_buffer[PIPE_SHADER_VERTEX].size + ctx->vs->constant_size;
   vs_cmd[i++] = ctx->gp_buffer->va + gp_uniform_offset +
      ctx->buffer_state[lima_ctx_buff_gp_uniform].offset;
   vs_cmd[i++] = 0x30000000 | (align(uniform_size, 16) << 12); /* UNIFORMS_ADDRESS */

   vs_cmd[i++] = ctx->gp_buffer->va + gp_vs_program_offset +
      ctx->buffer_state[lima_ctx_buff_gp_vs_program].offset;
   vs_cmd[i++] = 0x40000000 | ((ctx->vs->shader_size >> 4) << 16); /* SHADER_ADDRESS */

   vs_cmd[i++] = (ctx->vs->prefetch << 20) | ((align(ctx->vs->shader_size, 16) / 16 - 1) << 10);
   vs_cmd[i++] = 0x10000040; /* SHADER_INFO */

   int num_varryings = ctx->vs->num_varying;
   int num_attributes = ctx->vertex_elements->num_elements;

   vs_cmd[i++] = ((num_varryings - 1) << 8) | ((num_attributes - 1) << 24);
   vs_cmd[i++] = 0x10000042; /* VARYING_ATTRIBUTE_COUNT */

   vs_cmd[i++] = 0x00000003;
   vs_cmd[i++] = 0x10000041; /* ?? */

   vs_cmd[i++] = ctx->gp_buffer->va + gp_attribute_info_offset +
      ctx->buffer_state[lima_ctx_buff_gp_attribute_info].offset;
   vs_cmd[i++] = 0x20000000 | (num_attributes << 17); /* ATTRIBUTES_ADDRESS */

   vs_cmd[i++] = ctx->gp_buffer->va + gp_varying_info_offset +
      ctx->buffer_state[lima_ctx_buff_gp_varying_info].offset;
   vs_cmd[i++] = 0x20000008 | (num_varryings << 17); /* VARYINGS_ADDRESS */

   vs_cmd[i++] = (info->count << 24) | (info->index_size ? 1 : 0);
   vs_cmd[i++] = 0x00000000 | (info->count >> 8); /* DRAW */

   vs_cmd[i++] = 0x00000000;
   vs_cmd[i++] = 0x60000000; /* ?? */

   vs_cmd[i++] = info->index_size ? 0x00018000 : 0x00000000; /* ARRAYS_SEMAPHORE_NEXT : ARRAYS_SEMAPHORE_END */
   vs_cmd[i++] = 0x50000000; /* ARRAYS_SEMAPHORE */

   if (lima_dump_command_stream) {
      printf("lima add vs cmd at va %x\n",
             ctx->gp_buffer->va + gp_vs_cmd_offset +
             ctx->buffer_state[lima_ctx_buff_gp_vs_cmd].offset);
      lima_dump_blob(vs_cmd, i * 4, false);
   }

   ctx->buffer_state[lima_ctx_buff_gp_vs_cmd].size = i * 4;
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
   ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].offset +=
      ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].size;

   int i = 0;
   uint32_t *plbu_cmd = ctx->gp_buffer->map + gp_plbu_cmd_offset +
      ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].offset;

   /* first draw need create a PLBU command header */
   if (!ctx->num_draws) {
      struct lima_context_framebuffer *fb = &ctx->framebuffer;

      plbu_cmd[i++] = 0x00000200;
      plbu_cmd[i++] = 0x1000010B; /* PRIMITIVE_SETUP */

      plbu_cmd[i++] = (fb->shift_max << 28) | (fb->shift_h << 16) | fb->shift_w;
      plbu_cmd[i++] = 0x1000010C; /* BLOCK_STEP */

      plbu_cmd[i++] = ((fb->tiled_w - 1) << 24) | ((fb->tiled_h - 1) << 8);
      plbu_cmd[i++] = 0x10000109; /* TILED_DIMENSIONS */

      plbu_cmd[i++] = fb->block_w;
      plbu_cmd[i++] = 0x30000000; /* PLBU_BLOCK_STRIDE */

      plbu_cmd[i++] = ctx->gp_buffer->va + gp_plbu_plb_offset;
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

   uint32_t gl_position_va = ctx->share_buffer->va + sh_gl_pos_offset +
      ctx->buffer_state[lima_ctx_buff_sh_gl_pos].offset;
   plbu_cmd[i++] = ctx->pp_buffer->va + pp_plb_rsw_offset +
      ctx->buffer_state[lima_ctx_buff_pp_plb_rsw].offset +
      ctx->buffer_state[lima_ctx_buff_pp_plb_rsw].size;
   plbu_cmd[i++] = 0x80000000 | (gl_position_va >> 4); /* RSW_VERTEX_ARRAY */

   /* TODO
    * - we should set it only for the first draw that enabled the scissor and for latter draw only if scissor is dirty
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

      struct lima_resource *res = lima_resource(info->index.resource);
      lima_buffer_update(res->buffer, LIMA_BUFFER_ALLOC_VA);
      plbu_cmd[i++] = res->buffer->va + info->start +
         info->start_instance * info->index_size;
      plbu_cmd[i++] = 0x10000101; /* INDICES */
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
      plbu_cmd[i++] = (info->count << 24) | info->start;
      plbu_cmd[i++] = 0x00000000 | 0x00200000 |
         ((info->mode & 0x1F) << 16) | (info->count >> 8); /* DRAW | DRAW_ELEMENTS */
   }

done:
   if (lima_dump_command_stream) {
      printf("lima add plbu cmd at va %x\n",
             ctx->gp_buffer->va + gp_plbu_cmd_offset +
             ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].offset);
      lima_dump_blob(plbu_cmd, i * 4, false);
   }

   ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].size = i * 4;
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

static void
lima_pack_render_state(struct lima_context *ctx)
{
   ctx->buffer_state[lima_ctx_buff_pp_plb_rsw].offset +=
      ctx->buffer_state[lima_ctx_buff_pp_plb_rsw].size;

   struct lima_render_state *render = ctx->pp_buffer->map + pp_plb_rsw_offset +
      ctx->buffer_state[lima_ctx_buff_pp_plb_rsw].offset;

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

#if 0
   struct pipe_depth_state *depth = &ctx->zsa->base.depth;
   //struct pipe_rasterizer_state *rst = &ctx->rasterizer->base;
   render->depth_test = depth->enabled |
      (depth->func << 1);
   /* need more investigation */
   //(some_transform(rst->offset_scale) << 16) |
   //(some_transform(rst->offset_units) << 24) |
#else
   render->depth_test = 0x0000003e;
#endif

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
      (ctx->pp_buffer->va + pp_fs_program_offset +
       ctx->buffer_state[lima_ctx_buff_pp_fs_program].offset) |
      (((uint32_t *)ctx->fs->shader)[0] & 0x1F);

   /* seems not needed */
   render->uniforms_address = 0x00000000;

   render->textures_address = 0x00000000;

   /* more investigation */
   render->aux0 = 0x00000300 | (ctx->vs->varying_stride >> 3);
   render->aux1 = 0x00003000;

   if (ctx->buffer_state[lima_ctx_buff_pp_uniform].size) {
       render->uniforms_address = ctx->pp_buffer->va + pp_uniform_array_offset;
       render->uniforms_address |= ((ctx->buffer_state[lima_ctx_buff_pp_uniform].size) / 4 - 1);
       render->aux0 |= 0x80;
       render->aux1 |= 0x10000;
   }

   if (ctx->vs->num_varying > 1) {
      render->varying_types = 0x00000000;
      render->varyings_address = ctx->share_buffer->va + sh_varying_offset +
         ctx->buffer_state[lima_ctx_buff_sh_varying].offset;
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

   if (lima_dump_command_stream) {
      printf("lima: add render state at va %x\n",
             ctx->pp_buffer->va + pp_plb_rsw_offset +
             ctx->buffer_state[lima_ctx_buff_pp_plb_rsw].offset);
      lima_dump_blob(render, sizeof(*render), false);
   }

   ctx->buffer_state[lima_ctx_buff_pp_plb_rsw].size = sizeof(*render);
}

static void
lima_update_gp_attribute_info(struct lima_context *ctx, const struct pipe_draw_info *info)
{
   ctx->buffer_state[lima_ctx_buff_gp_attribute_info].offset +=
      ctx->buffer_state[lima_ctx_buff_gp_attribute_info].size;

   uint32_t *attribute = ctx->gp_buffer->map + gp_attribute_info_offset +
      ctx->buffer_state[lima_ctx_buff_gp_attribute_info].offset;
   int n = 0;

   struct lima_vertex_element_state *ve = ctx->vertex_elements;
   struct lima_context_vertex_buffer *vb = &ctx->vertex_buffers;

   for (int i = 0; i < ve->num_elements; i++) {
      struct pipe_vertex_element *pve = ve->pipe + i;

      assert(pve->vertex_buffer_index < vb->count);
      assert(vb->enabled_mask & (1 << pve->vertex_buffer_index));

      struct pipe_vertex_buffer *pvb = vb->vb + pve->vertex_buffer_index;
      struct lima_resource *res = lima_resource(pvb->buffer.resource);
      lima_buffer_update(res->buffer, LIMA_BUFFER_ALLOC_VA);

      attribute[n++] = res->buffer->va + pvb->buffer_offset + pve->src_offset
         + info->start * pvb->stride;
      attribute[n++] = (pvb->stride << 11) |
         (lima_pipe_format_to_attrib_type(pve->src_format) << 2) |
         (util_format_get_nr_components(pve->src_format) - 1);
   }

   if (lima_dump_command_stream) {
      printf("lima: update attribute info at va %x\n",
             ctx->gp_buffer->va + gp_attribute_info_offset +
             ctx->buffer_state[lima_ctx_buff_gp_attribute_info].offset);
      lima_dump_blob(attribute, n * 4, false);
   }

   ctx->buffer_state[lima_ctx_buff_gp_attribute_info].size = align(n * 4, 0x40);
}

static void
lima_update_gp_uniform(struct lima_context *ctx)
{
   ctx->buffer_state[lima_ctx_buff_gp_uniform].offset +=
      ctx->buffer_state[lima_ctx_buff_gp_uniform].size;
   ctx->buffer_state[lima_ctx_buff_gp_uniform].size = 0;

   void *vs_const_buff = ctx->gp_buffer->map + gp_uniform_offset +
      ctx->buffer_state[lima_ctx_buff_gp_uniform].offset;

   struct lima_context_constant_buffer *cbs = ctx->const_buffer + PIPE_SHADER_VERTEX;
   if (cbs->buffer) {
      memcpy(vs_const_buff, cbs->buffer, cbs->size);
      ctx->buffer_state[lima_ctx_buff_gp_uniform].size += cbs->size;
   }

   struct lima_vs_shader_state *vs = ctx->vs;
   if (vs->constant) {
      memcpy(vs_const_buff + cbs->size, vs->constant, vs->constant_size);
      ctx->buffer_state[lima_ctx_buff_gp_uniform].size += vs->constant_size;
   }

   ctx->buffer_state[lima_ctx_buff_gp_uniform].size =
      align(ctx->buffer_state[lima_ctx_buff_gp_uniform].size, 0x40);
}

static void
lima_update_pp_uniform(struct lima_context *ctx)
{
   uint32_t addr = ctx->pp_buffer->va + pp_uniform_offset +
      ctx->buffer_state[lima_ctx_buff_pp_uniform].offset;
   uint16_t *fp16_const_buff = ctx->pp_buffer->map + pp_uniform_offset +
      ctx->buffer_state[lima_ctx_buff_pp_uniform].offset;
   const float *const_buff = ctx->const_buffer[PIPE_SHADER_FRAGMENT].buffer;
   size_t const_buff_size = ctx->const_buffer[PIPE_SHADER_FRAGMENT].size / sizeof(float);

   ctx->buffer_state[lima_ctx_buff_pp_uniform].size = 0;

   if (!const_buff)
       return;

   memcpy(ctx->pp_buffer->map + pp_uniform_array_offset, &addr, sizeof(addr));

   for (int i = 0; i < const_buff_size; i++)
       fp16_const_buff[i] = util_float_to_half(const_buff[i]);

   ctx->buffer_state[lima_ctx_buff_pp_uniform].size =
      align(const_buff_size * sizeof(uint16_t), 0x4);
}

static void
lima_update_gp_vs_program(struct lima_context *ctx)
{
   ctx->buffer_state[lima_ctx_buff_gp_vs_program].offset +=
      ctx->buffer_state[lima_ctx_buff_gp_vs_program].size;

   void *vs_program = ctx->gp_buffer->map + gp_vs_program_offset +
      ctx->buffer_state[lima_ctx_buff_gp_vs_program].offset;

   struct lima_vs_shader_state *vs = ctx->vs;
   memcpy(vs_program, vs->shader, vs->shader_size);
   ctx->buffer_state[lima_ctx_buff_gp_vs_program].size = align(vs->shader_size, 0x40);
}

static void
lima_update_varying(struct lima_context *ctx, const struct pipe_draw_info *info)
{
   ctx->buffer_state[lima_ctx_buff_gp_varying_info].offset +=
      ctx->buffer_state[lima_ctx_buff_gp_varying_info].size;

   ctx->buffer_state[lima_ctx_buff_sh_varying].offset +=
      ctx->buffer_state[lima_ctx_buff_sh_varying].size;

   ctx->buffer_state[lima_ctx_buff_sh_gl_pos].offset +=
      ctx->buffer_state[lima_ctx_buff_sh_gl_pos].size;

   uint32_t *varying = ctx->gp_buffer->map + gp_varying_info_offset +
      ctx->buffer_state[lima_ctx_buff_gp_varying_info].offset;
   int n = 0;

   /* for gl_Position */
   varying[n++] = ctx->share_buffer->va + sh_gl_pos_offset +
      ctx->buffer_state[lima_ctx_buff_sh_gl_pos].offset;
   varying[n++] = 0x8020;

   int offset = 0;
   struct lima_vs_shader_state *vs = ctx->vs;
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
   for (int i = 1; i < vs->num_varying; i++) {
      struct lima_varying_info *v = vs->varying + i;
      varying[n++] = ctx->share_buffer->va + sh_varying_offset +
         ctx->buffer_state[lima_ctx_buff_sh_varying].offset + v->offset;
      varying[n++] = (vs->varying_stride << 11) | (v->components - 1) |
         (v->component_size == 2 ? 0x0C : 0);
   }

   if (lima_dump_command_stream) {
      printf("lima: update varying info at va %x\n",
             ctx->gp_buffer->va + gp_varying_info_offset +
             ctx->buffer_state[lima_ctx_buff_gp_varying_info].offset);
      lima_dump_blob(varying, n * 4, false);
   }

   ctx->buffer_state[lima_ctx_buff_gp_varying_info].size = align(n * 4, 0x40);
   ctx->buffer_state[lima_ctx_buff_sh_varying].size =
      align(vs->varying_stride * info->count, 0x40);
   ctx->buffer_state[lima_ctx_buff_sh_gl_pos].size =
      align(4 * 4 * info->count, 0x40);
}

static void
lima_update_pp_fs_program(struct lima_context *ctx)
{
   ctx->buffer_state[lima_ctx_buff_pp_fs_program].offset +=
      ctx->buffer_state[lima_ctx_buff_pp_fs_program].size;

   void *fs_program = ctx->pp_buffer->map + pp_fs_program_offset +
      ctx->buffer_state[lima_ctx_buff_pp_fs_program].offset;

   struct lima_fs_shader_state *fs = ctx->fs;
   memcpy(fs_program, fs->shader, fs->shader_size);
   ctx->buffer_state[lima_ctx_buff_pp_fs_program].size = align(fs->shader_size, 0x40);
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

   lima_bo_wait(ctx->gp_buffer->bo, LIMA_BO_WAIT_FLAG_WRITE, 1000000000, true);

   /* TODO:
    *   1. dynamic command stream buffers
    *   2. reuse buffer across draws of different frame
    */

   if (!ctx->num_draws ||
       ctx->dirty & (LIMA_CONTEXT_DIRTY_VERTEX_ELEM|LIMA_CONTEXT_DIRTY_VERTEX_BUFF) ||
       info->start != ctx->draw_start) {
      lima_update_gp_attribute_info(ctx, info);
      ctx->draw_start = info->start;
   }

   if (!ctx->num_draws ||
       (ctx->dirty & LIMA_CONTEXT_DIRTY_CONST_BUFF &&
        ctx->const_buffer[PIPE_SHADER_VERTEX].dirty) ||
       ctx->dirty & LIMA_CONTEXT_DIRTY_SHADER_VERT) {
      lima_update_gp_uniform(ctx);
      ctx->const_buffer[PIPE_SHADER_VERTEX].dirty = false;
   }

   if (!ctx->num_draws || ctx->dirty & LIMA_CONTEXT_DIRTY_SHADER_VERT)
      lima_update_gp_vs_program(ctx);

   lima_update_varying(ctx, info);

   /* If it's zero scissor, don't build vs cmd list */
   if (!lima_is_scissor_zero(ctx))
      lima_pack_vs_cmd(ctx, info);

   lima_pack_plbu_cmd(ctx, info);

   lima_bo_wait(ctx->pp_buffer->bo, LIMA_BO_WAIT_FLAG_WRITE, 1000000000, true);

   /* TODO: handle fb change case */
   if (ctx->dirty & LIMA_CONTEXT_DIRTY_FRAMEBUFFER) {
      struct lima_context_framebuffer *fb = &ctx->framebuffer;

      if (fb->dirty_dim) {
         lima_update_plb(ctx);
         fb->dirty_dim = false;
      }
   }

   if (!ctx->num_draws ||
       (ctx->dirty & LIMA_CONTEXT_DIRTY_CONST_BUFF &&
        ctx->const_buffer[PIPE_SHADER_FRAGMENT].dirty) ||
       ctx->dirty & LIMA_CONTEXT_DIRTY_SHADER_FRAG) {
      lima_update_pp_uniform(ctx);
      ctx->const_buffer[PIPE_SHADER_FRAGMENT].dirty = false;
   }

   if (!ctx->num_draws || ctx->dirty & LIMA_CONTEXT_DIRTY_SHADER_FRAG)
      lima_update_pp_fs_program(ctx);

   lima_pack_render_state(ctx);

   ctx->dirty = 0;
   ctx->num_draws++;
}

static void
lima_finish_plbu_cmd(struct lima_context *ctx)
{
   ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].offset +=
      ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].size;

   uint32_t *plbu_cmd = ctx->gp_buffer->map + gp_plbu_cmd_offset +
      ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].offset;
   int i = 0;

   plbu_cmd[i++] = 0x00000000;
   plbu_cmd[i++] = 0x50000000; /* END */

   ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].size = i * 4;
}

static void
lima_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence,
           unsigned flags)
{
   debug_checkpoint();
   debug_printf("%s: flags=%x\n", __FUNCTION__, flags);

   struct lima_context *ctx = lima_context(pctx);
   if (!ctx->num_draws) {
      debug_printf("%s: do nothing\n", __FUNCTION__);
      return;
   }

   lima_finish_plbu_cmd(ctx);

   int vs_cmd_size = ctx->buffer_state[lima_ctx_buff_gp_vs_cmd].offset +
      ctx->buffer_state[lima_ctx_buff_gp_vs_cmd].size;
   int plbu_cmd_size = ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].offset +
      ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].size;

   struct drm_lima_m400_gp_frame gp_frame = {
      .vs_cmd_start = ctx->gp_buffer->va + gp_vs_cmd_offset,
      .vs_cmd_end = ctx->gp_buffer->va + gp_vs_cmd_offset + vs_cmd_size,
      .plbu_cmd_start = ctx->gp_buffer->va + gp_plbu_cmd_offset,
      .plbu_cmd_end = ctx->gp_buffer->va + gp_plbu_cmd_offset + plbu_cmd_size,
      .tile_heap_start = ctx->gp_buffer->va + gp_tile_heap_offset,
      .tile_heap_end = ctx->gp_buffer->va + gp_buffer_size,
   };

   lima_submit_set_frame(ctx->gp_submit, &gp_frame, sizeof(gp_frame));
   if (lima_submit_start(ctx->gp_submit))
      fprintf(stderr, "gp submit error\n");

   if (lima_dump_command_stream) {
      if (lima_submit_wait(ctx->gp_submit, 1000000000, true))
         fprintf(stderr, "gp submit wait error\n");

      lima_buffer_update(ctx->share_buffer, LIMA_BUFFER_ALLOC_MAP);

      float *varying = ctx->share_buffer->map + sh_varying_offset;
      printf("lima varying dump at va %x\n",
             ctx->share_buffer->va + sh_varying_offset);
      lima_dump_blob(varying, 4 * 4 * 16, true);

      varying = ctx->share_buffer->map + sh_gl_pos_offset;
      printf("lima gl_pos dump at va %x\n",
             ctx->share_buffer->va + sh_gl_pos_offset);
      lima_dump_blob(varying, 4 * 4 * 16, true);

      uint32_t *plb = ctx->share_buffer->map + sh_plb_offset;
      debug_printf("plb %x %x %x %x %x %x %x %x\n",
                   plb[0], plb[1], plb[2], plb[3],
                   plb[4], plb[5], plb[6], plb[7]);
   }

   struct lima_screen *screen = lima_screen(pctx->screen);
   struct lima_resource *res = lima_resource(ctx->framebuffer.cbuf->texture);
   lima_buffer_update(res->buffer, LIMA_BUFFER_ALLOC_VA);

   int num_pp = screen->info.num_pp;
   struct drm_lima_m400_pp_frame pp_frame = {
      .frame = {
         .plbu_array_address = 0,
         .render_address = ctx->pp_buffer->va + pp_frame_rsw_offset,
         .unused_0 = 0,
         .flags = 0x02,
         //.clear_value_depth = ctx->clear.depth,
         .clear_value_depth = 0x00FFFFFF,
         .clear_value_stencil = ctx->clear.stencil,
         .clear_value_color = ctx->clear.color,
         .clear_value_color_1 = ctx->clear.color,
         .clear_value_color_2 = ctx->clear.color,
         .clear_value_color_3 = ctx->clear.color,
         /* different with limare */
         .width = 0,
         .height = 0,
         .fragment_stack_address = 0,
         .fragment_stack_size = 0,
         .unused_1 = 0,
         .unused_2 = 0,
         .one = 1,
         .supersampled_height = ctx->framebuffer.height * 2 - 1,
         .dubya = 0x77,
         .onscreen = 1,
         .blocking = (ctx->framebuffer.shift_max << 28) |
                     (ctx->framebuffer.shift_h << 16) | ctx->framebuffer.shift_w,
         /* different with limare */
         .scale = 0xE0C,
         .foureight = 0x8888,
      },

      .wb[0] = {
         .type = 0x02, /* 1 for depth, stencil */
         .address = res->buffer->va,
         .pixel_format = 0x03, /* RGBA8888 */
         .downsample_factor = 0,
         .pixel_layout = 0,
         .pitch = res->stride / 8,
         .mrt_bits = 0,
         .mrt_pitch = 0,
         .zero = 0,
         .unused0 = 0,
         .unused1 = 0,
         .unused2 = 0,
      },
      .wb[1] = {0},
      .wb[2] = {0},

      .plbu_array_address = {0},
      .fragment_stack_address = {0},
      .num_pp = num_pp,
   };

   for (int i = 0; i < num_pp; i++)
      pp_frame.plbu_array_address[i] = ctx->pp_buffer->va + pp_plb_offset(i, num_pp);

   lima_submit_set_frame(ctx->pp_submit, &pp_frame, sizeof(pp_frame));

   if (lima_submit_start(ctx->pp_submit))
      fprintf(stderr, "pp submit error\n");

   ctx->num_draws = 0;

   assert(ctx->buffer_state[lima_ctx_buff_sh_varying].offset +
          ctx->buffer_state[lima_ctx_buff_sh_varying].size <=
          sh_gl_pos_offset - sh_varying_offset);
   assert(ctx->buffer_state[lima_ctx_buff_sh_gl_pos].offset +
          ctx->buffer_state[lima_ctx_buff_sh_gl_pos].size <=
          sh_buffer_size - sh_gl_pos_offset);
   assert(ctx->buffer_state[lima_ctx_buff_gp_vs_program].offset +
          ctx->buffer_state[lima_ctx_buff_gp_vs_program].size <=
          gp_plbu_plb_offset - gp_vs_program_offset);
   assert(ctx->buffer_state[lima_ctx_buff_gp_varying_info].offset +
          ctx->buffer_state[lima_ctx_buff_gp_varying_info].size <=
          gp_attribute_info_offset - gp_varying_info_offset);
   assert(ctx->buffer_state[lima_ctx_buff_gp_attribute_info].offset +
          ctx->buffer_state[lima_ctx_buff_gp_attribute_info].size <=
          gp_uniform_offset - gp_attribute_info_offset);
   assert(ctx->buffer_state[lima_ctx_buff_gp_uniform].offset +
          ctx->buffer_state[lima_ctx_buff_gp_uniform].size <=
          gp_vs_cmd_offset - gp_uniform_offset);
   assert(ctx->buffer_state[lima_ctx_buff_gp_vs_cmd].offset +
          ctx->buffer_state[lima_ctx_buff_gp_vs_cmd].size <=
          gp_plbu_cmd_offset - gp_vs_cmd_offset);
   assert(ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].offset +
          ctx->buffer_state[lima_ctx_buff_gp_plbu_cmd].size <=
          gp_tile_heap_offset - gp_plbu_cmd_offset);
   assert(ctx->buffer_state[lima_ctx_buff_pp_fs_program].offset +
          ctx->buffer_state[lima_ctx_buff_pp_fs_program].size <=
          pp_frame_rsw_offset - pp_fs_program_offset);
   assert(ctx->buffer_state[lima_ctx_buff_pp_plb_rsw].offset +
          ctx->buffer_state[lima_ctx_buff_pp_plb_rsw].size <=
          pp_plb_offset_start - pp_plb_rsw_offset);
   memset(ctx->buffer_state, 0, sizeof(ctx->buffer_state));
}

void
lima_draw_init(struct lima_context *ctx)
{
   ctx->base.clear = lima_clear;
   ctx->base.draw_vbo = lima_draw_vbo;
   ctx->base.flush = lima_flush;
}
