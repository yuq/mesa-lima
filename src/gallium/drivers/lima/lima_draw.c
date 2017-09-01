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

#include "lima_context.h"
#include "lima_screen.h"
#include "lima_resource.h"

#include <lima_drm.h>

static void
lima_clear(struct pipe_context *pctx, unsigned buffers,
           const union pipe_color_union *color, double depth, unsigned stencil)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);
   struct lima_context_clear *clear = &ctx->clear;

   clear->buffers = buffers;

   if (buffers & PIPE_CLEAR_COLOR0) {
      clear->color[0] = color->ui[0];
      clear->color[1] = color->ui[1];
      clear->color[2] = color->ui[2];
      clear->color[3] = color->ui[3];
   }

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

static int
lima_pack_vs_cmd(struct lima_context *ctx, const struct pipe_draw_info *info)
{
   int i = 0;
   uint32_t *vs_cmd = ctx->gp_buffer->map + gp_vs_cmd_offset;

   if (!info->indexed) {
      vs_cmd[i++] = 0x00028000; /* ARRAYS_SEMAPHORE_BEGIN_1 */
      vs_cmd[i++] = 0x50000000; /* ARRAYS_SEMAPHORE */
      vs_cmd[i++] = 0x00000001; /* ARRAYS_SEMAPHORE_BEGIN_2 */
      vs_cmd[i++] = 0x50000000; /* ARRAYS_SEMAPHORE */
   }

   /* static uniform only for viewport transform now */
   vs_cmd[i++] = ctx->gp_buffer->va + gp_uniform_offset;
   vs_cmd[i++] = 0x30000000 | (48 << 12); /* UNIFORMS_ADDRESS */

   vs_cmd[i++] = ctx->gp_buffer->va + gp_vs_program_offset;
   vs_cmd[i++] = 0x40000000 | ((ctx->vs->shader_size >> 4) << 16); /* SHADER_ADDRESS */

   /* 3 is prefetch, what's it? */
   vs_cmd[i++] = (ctx->vs->prefetch << 20) | ((align(ctx->vs->shader_size, 16) / 16 - 1) << 10);
   vs_cmd[i++] = 0x10000040; /* SHADER_INFO */

   /* assume to 1 before vs compiler is ready */
   int num_varryings = 1;
   int num_attributes = ctx->vertex_elements->num_elements;

   vs_cmd[i++] = ((num_varryings - 1) << 8) | ((num_attributes - 1) << 24);
   vs_cmd[i++] = 0x10000042; /* VARYING_ATTRIBUTE_COUNT */

   vs_cmd[i++] = 0x00000003;
   vs_cmd[i++] = 0x10000041; /* ?? */

   vs_cmd[i++] = ctx->gp_buffer->va + gp_attribute_info_offset;
   vs_cmd[i++] = 0x20000000 | (num_attributes << 17); /* ATTRIBUTES_ADDRESS */

   vs_cmd[i++] = ctx->gp_buffer->va + gp_varying_info_offset;
   vs_cmd[i++] = 0x20000008 | (num_varryings << 17); /* VARYINGS_ADDRESS */

   vs_cmd[i++] = (info->count << 24) | (info->indexed ? 1 : 0);
   vs_cmd[i++] = 0x00000000 | (info->count >> 8); /* DRAW */

   vs_cmd[i++] = 0x00000000;
   vs_cmd[i++] = 0x60000000; /* ?? */

   vs_cmd[i++] = info->indexed ? 0x00018000 : 0x00000000; /* ARRAYS_SEMAPHORE_NEXT : ARRAYS_SEMAPHORE_END */
   vs_cmd[i++] = 0x50000000; /* ARRAYS_SEMAPHORE */

   return i << 2;
}

static int
lima_pack_plbu_cmd(struct lima_context *ctx, const struct pipe_draw_info *info)
{
   int i = 0;
   uint32_t *plbu_cmd = ctx->gp_buffer->map + gp_plbu_cmd_offset;
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

   if (!info->indexed) {
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
      (info->indexed && ctx->index_buffer.index_size == 2 ? 0x00000400 : 0);
   plbu_cmd[i++] = 0x1000010B; /* PRIMITIVE_SETUP */

   /* before we have a compiler, assume gl_position here */
   uint32_t gl_position_va = ctx->share_buffer->va + sh_varying_offset;
   plbu_cmd[i++] = ctx->pp_buffer->va + pp_plb_rsw_offset;
   plbu_cmd[i++] = 0x80000000 | (gl_position_va >> 4); /* RSW_VERTEX_ARRAY */

   plbu_cmd[i++] = 0x00000000;
   plbu_cmd[i++] = 0x1000010A; /* ?? */

   plbu_cmd[i++] = fui(ctx->viewport.near);
   plbu_cmd[i++] = 0x1000010E; /* DEPTH_RANGE_NEAR */

   plbu_cmd[i++] = fui(ctx->viewport.far);
   plbu_cmd[i++] = 0x1000010F; /* DEPTH_RANGE_FAR */

   if (info->indexed) {
      plbu_cmd[i++] = gl_position_va;
      plbu_cmd[i++] = 0x10000100; /* INDEXED_DEST */

      struct lima_resource *res = lima_resource(ctx->index_buffer.buffer);
      lima_buffer_update(res->buffer, LIMA_BUFFER_ALLOC_VA);
      plbu_cmd[i++] = res->buffer->va + ctx->index_buffer.offset +
         info->start_instance * ctx->index_buffer.index_size;
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

   if (info->indexed) {
      plbu_cmd[i++] = (info->count << 24) | info->start;
      plbu_cmd[i++] = 0x00000000 | 0x00200000 |
         ((info->mode & 0x1F) << 16) | (info->count >> 8); /* DRAW | DRAW_ELEMENTS */
   }

   plbu_cmd[i++] = 0x00000000;
   plbu_cmd[i++] = 0x50000000; /* END */

   return i << 2;
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
   struct lima_render_state *render = ctx->pp_buffer->map + pp_plb_rsw_offset;

   /* do we need to check if blend enabled to setup these fields?
    * ctx->blend->base.rt[0].blend_enable
    *
    * do hw support RGBA independ blend?
    * PIPE_CAP_INDEP_BLEND_ENABLE
    *
    * how to handle the no cbuf only zbuf case?
    */
   struct pipe_rt_blend_state *rt = ctx->blend->base.rt;
   render->blend_color_bg = float_to_ubyte(ctx->blend_color.color[2]) |
      (float_to_ubyte(ctx->blend_color.color[1]) << 16);
   render->blend_color_ra = float_to_ubyte(ctx->blend_color.color[0]) |
      (float_to_ubyte(ctx->blend_color.color[3]) << 16);
#if 1
   render->alpha_blend = lima_blend_func(rt->rgb_func) |
      (lima_blend_func(rt->alpha_func) << 3) |
      (lima_blend_factor(rt->rgb_src_factor) << 6) |
      (lima_blend_factor(rt->rgb_dst_factor) << 11) |
      ((lima_blend_factor(rt->alpha_src_factor) & 0xF) << 16) |
      ((lima_blend_factor(rt->alpha_dst_factor) & 0xF) << 20) |
      0xFC000000; /* need check if this GLESv1 glAlphaFunc */
#else
   render->alpha_blend = 0xfc3b1ad2;
#endif

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

   render->shader_address = (ctx->pp_buffer->va + pp_fs_program_offset) |
      (((uint32_t *)ctx->fs->shader)[0] & 0x1F);

   /* after compiler */
   render->varying_types = 0x00000000;

   /* seems not needed */
   render->uniforms_address = 0x00000000;

   render->textures_address = 0x00000000;

   /* more investigation */
   render->aux0 = 0x00000300;
   render->aux1 = 0x00003000;

   /* seems not needed */
   render->varyings_address = 0x00000000;
}

static void
lima_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   lima_bo_wait(ctx->gp_buffer->bo, LIMA_BO_WAIT_FLAG_WRITE, 1000000000, true);

   if (ctx->dirty & (LIMA_CONTEXT_DIRTY_VERTEX_ELEM|LIMA_CONTEXT_DIRTY_VERTEX_BUFF)) {
      struct lima_vertex_element_state *ve = ctx->vertex_elements;
      struct lima_context_vertex_buffer *vb = &ctx->vertex_buffers;
      uint32_t *attribute = ctx->gp_buffer->map + gp_attribute_info_offset;
      int n = 0;

      for (int i = 0; i < ve->num_elements; i++) {
         struct pipe_vertex_element *pve = ve->pipe + i;

         assert(pve->vertex_buffer_index < vb->count);
         assert(vb->enabled_mask & (1 << pve->vertex_buffer_index));

         struct pipe_vertex_buffer *pvb = vb->vb + pve->vertex_buffer_index;
         struct lima_resource *res = lima_resource(pvb->buffer);
         lima_buffer_update(res->buffer, LIMA_BUFFER_ALLOC_VA);

         /* draw_info start vertex should also be here which is very bad
          * make this bo must be updated also when start vertex change
          * now ignore it first
          */
         attribute[n++] = res->buffer->va + pvb->buffer_offset + pve->src_offset
            //+ info->start * pvb->stride
            ;
         attribute[n++] = (pvb->stride << 11) |
            (lima_pipe_format_to_attrib_type(pve->src_format) << 2) |
            (util_format_get_nr_components(pve->src_format) - 1);
      }

      ctx->dirty &= ~(LIMA_CONTEXT_DIRTY_VERTEX_ELEM|LIMA_CONTEXT_DIRTY_VERTEX_BUFF);
   }

   bool vs_need_update_const = false;
   void *vs_const_buff = ctx->gp_buffer->map + gp_uniform_offset;

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_CONST_BUFF) {
      if (ctx->const_buffer[PIPE_SHADER_VERTEX].dirty) {
         struct lima_context_constant_buffer *cbs = ctx->const_buffer + PIPE_SHADER_VERTEX;

         if (cbs->buffer)
            memcpy(vs_const_buff, cbs->buffer, cbs->size);

         cbs->dirty = false;
         vs_need_update_const = true;
      }

      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_CONST_BUFF;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_SHADER_VERT) {
      struct lima_vs_shader_state *vs = ctx->vs;
      uint32_t *varying = ctx->gp_buffer->map + gp_varying_info_offset;
      int n = 0;

      memcpy(ctx->gp_buffer->map + gp_vs_program_offset, vs->shader, vs->shader_size);

      /* no varing info build for now, just assume only gl_Position
       * it should be built when create vs state with compiled vs info
       */
      varying[n++] = ctx->share_buffer->va + sh_varying_offset;
      varying[n++] = 0x8020;

      vs_need_update_const = true;
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_SHADER_VERT;
   }

   if (vs_need_update_const) {
      struct lima_context_constant_buffer *cbs = ctx->const_buffer + PIPE_SHADER_VERTEX;
      struct lima_vs_shader_state *vs = ctx->vs;
      if (vs->constant)
         memcpy(vs_const_buff + cbs->size, vs->constant, vs->constant_size);
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_VIEWPORT) {
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_VIEWPORT;
   }

   int vs_cmd_size = lima_pack_vs_cmd(ctx, info);
   int plbu_cmd_size = lima_pack_plbu_cmd(ctx, info);

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
      printf("gp submit error\n");

/*
   if (lima_submit_wait(ctx->gp_submit, 1000000000, true))
      printf("gp submit wait error\n");
   lima_buffer_update(ctx->share_buffer, LIMA_BUFFER_ALLOC_MAP);
   float *varying = ctx->share_buffer->map + sh_varying_offset;
   printf("varing %f %f %f %f %f %f %f %f %f %f %f %f\n",
          varying[0], varying[1], varying[2], varying[3],
          varying[4], varying[5], varying[6], varying[7],
          varying[8], varying[9], varying[10], varying[11]);
   uint32_t *plb = ctx->share_buffer->map + sh_plb_offset;
   printf("plb %x %x %x %x %x %x %x %x\n",
          plb[0], plb[1], plb[2], plb[3],
          plb[4], plb[5], plb[6], plb[7]);
//*/

   lima_bo_wait(ctx->pp_buffer->bo, LIMA_BO_WAIT_FLAG_WRITE, 1000000000, true);

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_FRAMEBUFFER) {
      struct lima_context_framebuffer *fb = &ctx->framebuffer;

      if (fb->dirty_dim) {
         lima_update_plb(ctx);
         fb->dirty_dim = false;
      }

      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_FRAMEBUFFER;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_SHADER_FRAG) {
      struct lima_fs_shader_state *fs = ctx->fs;

      memcpy(ctx->pp_buffer->map + pp_fs_program_offset, fs->shader, fs->shader_size);
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_SHADER_FRAG;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_SCISSOR) {
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_SCISSOR;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_INDEX_BUFF) {
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_INDEX_BUFF;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_RASTERIZER) {
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_RASTERIZER;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_ZSA) {
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_ZSA;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_BLEND_COLOR) {
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_BLEND_COLOR;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_BLEND) {
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_BLEND;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_STENCIL_REF) {
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_STENCIL_REF;
   }

   lima_pack_render_state(ctx);

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
         .clear_value_depth = ctx->clear.depth,
         //.clear_value_depth = 0x00FFFFFF,
         .clear_value_stencil = ctx->clear.stencil,
         .clear_value_color = ctx->clear.color[0],
         .clear_value_color_1 = ctx->clear.color[1],
         .clear_value_color_2 = ctx->clear.color[2],
         .clear_value_color_3 = ctx->clear.color[3],
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
      printf("pp submit error\n");
}

void
lima_draw_init(struct lima_context *ctx)
{
   ctx->base.clear = lima_clear;
   ctx->base.draw_vbo = lima_draw_vbo;
}
