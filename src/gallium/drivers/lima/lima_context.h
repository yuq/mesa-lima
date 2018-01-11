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

#ifndef H_LIMA_CONTEXT
#define H_LIMA_CONTEXT

#include "util/slab.h"

#include "pipe/p_context.h"
#include "pipe/p_state.h"

struct lima_context_framebuffer {
   struct pipe_surface *cbuf, *zsbuf;
   int width, height;
   int tiled_w, tiled_h;
   int shift_w, shift_h;
   int block_w, block_h;
   int shift_max;
   bool dirty_dim;
};

struct lima_context_clear {
   unsigned buffers;
   uint32_t color;
   uint32_t depth;
   uint32_t stencil;
};

struct lima_depth_stencil_alpha_state {
   struct pipe_depth_stencil_alpha_state base;
};

struct lima_fs_shader_state {
   void *shader;
   int shader_size;
   struct lima_bo *bo;
};

#define LIMA_MAX_VARYING_NUM 13

struct lima_varying_info {
   int components;
   int component_size;
   int offset;
};

struct lima_vs_shader_state {
   void *shader;
   int shader_size;
   int prefetch;

   void *constant;
   int constant_size;

   struct lima_varying_info varying[LIMA_MAX_VARYING_NUM];
   int varying_stride;
   int num_varying;

   struct lima_bo *bo;
};

struct lima_rasterizer_state {
   struct pipe_rasterizer_state base;
};

struct lima_blend_state {
   struct pipe_blend_state base;
};

struct lima_vertex_element_state {
   struct pipe_vertex_element pipe[PIPE_MAX_ATTRIBS];
   unsigned num_elements;
};

struct lima_context_vertex_buffer {
   struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
   unsigned count;
   uint32_t enabled_mask;
};

struct lima_context_viewport_state {
   struct pipe_viewport_state transform;
   float x, y, width, height;
   float near, far;
};

struct lima_context_constant_buffer {
   const void *buffer;
   uint32_t size;
   bool dirty;
};

enum lima_ctx_buff {
   lima_ctx_buff_sh_varying,
   lima_ctx_buff_sh_gl_pos,
   lima_ctx_buff_gp_varying_info,
   lima_ctx_buff_gp_attribute_info,
   lima_ctx_buff_gp_uniform,
   lima_ctx_buff_gp_vs_cmd,
   lima_ctx_buff_gp_plbu_cmd,
   lima_ctx_buff_pp_plb_rsw,
   lima_ctx_buff_pp_uniform_array,
   lima_ctx_buff_pp_uniform,
   lima_ctx_buff_num,
};

struct lima_ctx_buff_state {
   unsigned offset;
   unsigned size;
};

struct lima_context {
   struct pipe_context base;

   enum {
      LIMA_CONTEXT_DIRTY_FRAMEBUFFER  = (1 << 0),
      LIMA_CONTEXT_DIRTY_CLEAR        = (1 << 1),
      LIMA_CONTEXT_DIRTY_SHADER_VERT  = (1 << 2),
      LIMA_CONTEXT_DIRTY_SHADER_FRAG  = (1 << 3),
      LIMA_CONTEXT_DIRTY_VERTEX_ELEM  = (1 << 4),
      LIMA_CONTEXT_DIRTY_VERTEX_BUFF  = (1 << 5),
      LIMA_CONTEXT_DIRTY_VIEWPORT     = (1 << 6),
      LIMA_CONTEXT_DIRTY_SCISSOR      = (1 << 7),
      LIMA_CONTEXT_DIRTY_RASTERIZER   = (1 << 8),
      LIMA_CONTEXT_DIRTY_ZSA          = (1 << 9),
      LIMA_CONTEXT_DIRTY_BLEND_COLOR  = (1 << 10),
      LIMA_CONTEXT_DIRTY_BLEND        = (1 << 11),
      LIMA_CONTEXT_DIRTY_STENCIL_REF  = (1 << 12),
      LIMA_CONTEXT_DIRTY_CONST_BUFF   = (1 << 13),
   } dirty;

   struct u_upload_mgr *uploader;

   struct slab_child_pool transfer_pool;

   struct lima_context_framebuffer framebuffer;
   struct lima_context_viewport_state viewport;
   struct pipe_scissor_state scissor;
   struct lima_context_clear clear;
   struct lima_vs_shader_state *vs;
   struct lima_fs_shader_state *fs;
   struct lima_vertex_element_state *vertex_elements;
   struct lima_context_vertex_buffer vertex_buffers;
   struct lima_rasterizer_state *rasterizer;
   struct lima_depth_stencil_alpha_state *zsa;
   struct pipe_blend_color blend_color;
   struct lima_blend_state *blend;
   struct pipe_stencil_ref stencil_ref;
   struct lima_context_constant_buffer const_buffer[PIPE_SHADER_TYPES];

   struct lima_bo *share_buffer;
   #define sh_plb_offset             0x00000
   /* max_plb = 512, block_size = 0x200, size = block_size * max_plb */
   #define sh_varying_offset         0x40000
   #define sh_gl_pos_offset          0x41000
   #define sh_buffer_size            0x42000

   struct lima_bo *gp_buffer;
   #define gp_plbu_plb_offset        0x0000
   #define gp_varying_info_offset    0x0800
   #define gp_attribute_info_offset  0x1000
   /* max_attr/varying_info = 16, each_info = 8, size = max * each */
   #define gp_uniform_offset         0x2000
   #define gp_vs_cmd_offset          0x3000
   #define gp_plbu_cmd_offset        0x3800
   #define gp_tile_heap_offset       0x4000
   #define gp_buffer_size            0x6000

   struct lima_bo *pp_buffer;
   #define pp_uniform_array_offset   0x00000
   #define pp_uniform_offset         0x00400
   #define pp_frame_rsw_offset       0x01400
   #define pp_clear_program_offset   0x01440
   #define pp_plb_rsw_offset         0x01480
   #define pp_plb_offset_start       0x02000
   /* max_screen_w/h_size = 2048, max_pp = 4, plb_stream_size = ((max >> 4)^2 + max_pp) * 16 */
   #define pp_stack_offset           0x42100
   #define pp_buffer_size            0x44000
   #define pp_plb_offset(i, n)       \
      (pp_plb_offset_start + i * ((pp_stack_offset - pp_plb_offset_start) / n))

   struct lima_ctx_buff_state buffer_state[lima_ctx_buff_num];

   unsigned num_draws;

   struct lima_submit *gp_submit;
   struct lima_submit *pp_submit;
};

static inline struct lima_context *
lima_context(struct pipe_context *pctx)
{
   return (struct lima_context *)pctx;
}

void lima_state_init(struct lima_context *ctx);
void lima_state_fini(struct lima_context *ctx);
void lima_draw_init(struct lima_context *ctx);
void lima_program_init(struct lima_context *ctx);
void lima_query_init(struct lima_context *ctx);

struct pipe_context *
lima_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags);

#endif
