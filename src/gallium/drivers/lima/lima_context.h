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

struct pipe_screen;
struct pipe_surface;
struct lima_buffer;

struct lima_context_framebuffer {
   struct pipe_surface *cbuf, *zsbuf;
   int tiled_w, tiled_h;
   int shift_w, shift_h;
   int block_w, block_h;
   int shift_max;
   bool dirty_dim;
};

struct lima_context_clear {
   unsigned buffers;
   uint32_t color[4];
   uint32_t depth;
   uint32_t stencil;
};

struct lima_depth_stencil_alpha_state {
   struct pipe_depth_stencil_alpha_state base;
};

struct lima_fs_shader_state {
   void *shader;
   int shader_size;
};

struct lima_vs_shader_state {
   void *shader;
   int shader_size;
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
   uint32_t dirty_mask;
};

struct lima_context_viewport_state {
   struct pipe_viewport_state transform;
   float x, y, width, height;
   float near, far;
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
      LIMA_CONTEXT_DIRTY_INDEX_BUFF   = (1 << 8),
      LIMA_CONTEXT_DIRTY_RASTERIZER   = (1 << 9),
      LIMA_CONTEXT_DIRTY_ZSA          = (1 << 10),
      LIMA_CONTEXT_DIRTY_BLEND_COLOR  = (1 << 11),
      LIMA_CONTEXT_DIRTY_BLEND        = (1 << 12),
      LIMA_CONTEXT_DIRTY_STENCIL_REF  = (1 << 13),
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
   struct pipe_index_buffer index_buffer;
   struct lima_rasterizer_state *rasterizer;
   struct lima_depth_stencil_alpha_state *zsa;
   struct pipe_blend_color blend_color;
   struct lima_blend_state *blend;
   struct pipe_stencil_ref stencil_ref;

   struct lima_buffer *plb;
   int plb_plbu_offset;
   int plb_pp_offset[4];
   int plb_offset;

   struct lima_buffer *gp_buffer;
   #define vs_program_offset      0x0000
   #define fs_program_offset      0x0800
   #define varying_offset         0x1000
   #define varying_info_offset    0x2000
   #define attribute_info_offset  0x2100
   #define render_state_offset    0x2200
   #define vs_cmd_offset          0x2300
   #define plbu_cmd_offset        0x2900
   #define tile_heap_offset       0x3000
   #define gp_buffer_size         0x5000
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

struct pipe_context *
lima_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags);

#endif
