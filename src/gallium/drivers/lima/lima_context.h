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
   int dummy;
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
   int dummy;
};

struct lima_blend_state {
   int dummy;
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

struct lima_context {
   struct pipe_context base;

   enum {
      LIMA_CONTEXT_DIRTY_FRAMEBUFFER  = (1 << 0),
      LIMA_CONTEXT_DIRTY_CLEAR        = (1 << 1),
      LIMA_CONTEXT_DIRTY_SHADER_VERT  = (1 << 2),
      LIMA_CONTEXT_DIRTY_SHADER_FRAG  = (1 << 3),
      LIMA_CONTEXT_DIRTY_VERTEX_ELEM  = (1 << 4),
      LIMA_CONTEXT_DIRTY_VERTEX_BUFF  = (1 << 5),
   } dirty;

   struct u_upload_mgr *uploader;

   struct slab_child_pool transfer_pool;

   struct lima_context_framebuffer framebuffer;
   struct lima_context_clear clear;
   struct lima_vs_shader_state *vs;
   struct lima_fs_shader_state *fs;
   struct lima_vertex_element_state *vertex_elements;
   struct lima_context_vertex_buffer vertex_buffers;

   struct lima_buffer *tile_heap;

   struct lima_buffer *plb;
   int plb_plbu_offset;
   int plb_pp_offset[4];
   int plb_offset;

   struct lima_buffer *vs_program;
   struct lima_buffer *fs_program;
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
