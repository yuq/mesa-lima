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

#ifndef ILO_STATE_3D_H
#define ILO_STATE_3D_H

#include "genhw/genhw.h"
#include "pipe/p_state.h"

#include "ilo_core.h"
#include "ilo_dev.h"
#include "ilo_state_shader.h"
#include "ilo_state_surface.h"
#include "ilo_state_zs.h"

/**
 * \see brw_context.h
 */
#define ILO_MAX_DRAW_BUFFERS    8
#define ILO_MAX_CONST_BUFFERS   (1 + 12)
#define ILO_MAX_SAMPLER_VIEWS   16
#define ILO_MAX_SAMPLERS        16
#define ILO_MAX_SO_BINDINGS     64
#define ILO_MAX_SO_BUFFERS      4
#define ILO_MAX_VIEWPORTS       1

#define ILO_MAX_SURFACES        256

struct intel_bo;
struct ilo_buffer;
struct ilo_image;
struct ilo_shader_state;

struct ilo_vb_state {
   struct pipe_vertex_buffer states[PIPE_MAX_ATTRIBS];
   uint32_t enabled_mask;
};

struct ilo_ib_state {
   struct pipe_resource *buffer;
   const void *user_buffer;
   unsigned offset;
   unsigned index_size;

   /* these are not valid until the state is finalized */
   struct pipe_resource *hw_resource;
   unsigned hw_index_size;
   /* an offset to be added to pipe_draw_info::start */
   int64_t draw_start_offset;
};

struct ilo_so_state {
   struct pipe_stream_output_target *states[ILO_MAX_SO_BUFFERS];
   unsigned count;
   unsigned append_bitmask;

   bool enabled;
};

struct ilo_surface_cso {
   struct pipe_surface base;

   bool is_rt;
   union {
      struct ilo_state_surface rt;
      struct ilo_state_zs zs;
   } u;
};

struct ilo_fb_state {
   struct pipe_framebuffer_state state;

   struct ilo_state_surface null_rt;
   struct ilo_state_zs null_zs;

   struct ilo_fb_blend_caps {
      bool is_unorm;
      bool is_integer;
      bool force_dst_alpha_one;

      bool can_logicop;
      bool can_blend;
      bool can_alpha_test;
   } blend_caps[PIPE_MAX_COLOR_BUFS];

   unsigned num_samples;

   bool has_integer_rt;
   bool has_hiz;
   enum gen_depth_format depth_offset_format;
};

union ilo_shader_cso {
   struct ilo_state_vs vs;
   struct ilo_state_hs hs;
   struct ilo_state_ds ds;
   struct ilo_state_gs gs;

   uint32_t ps_payload[5];

   struct {
      struct ilo_state_vs vs;
      struct ilo_state_gs sol;
   } vs_sol;
};

void
ilo_gpe_init_fs_cso(const struct ilo_dev *dev,
                    const struct ilo_shader_state *fs,
                    union ilo_shader_cso *cso);

void
ilo_gpe_set_fb(const struct ilo_dev *dev,
               const struct pipe_framebuffer_state *state,
               struct ilo_fb_state *fb);

#endif /* ILO_STATE_3D_H */
