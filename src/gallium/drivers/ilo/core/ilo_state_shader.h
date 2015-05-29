/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2015 LunarG, Inc.
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

#ifndef ILO_STATE_SHADER_H
#define ILO_STATE_SHADER_H

#include "genhw/genhw.h"

#include "ilo_core.h"
#include "ilo_dev.h"

/**
 * Kernel information.
 */
struct ilo_state_shader_kernel_info {
   /* usually 0 unless the shader has multiple kernels */
   uint32_t offset;

   uint8_t grf_start;
   uint8_t pcb_attr_count;

   uint32_t scratch_size;
};

/**
 * Shader resources.
 */
struct ilo_state_shader_resource_info {
   /* for prefetches */
   uint8_t sampler_count;
   uint8_t surface_count;

   bool has_uav;
};

/**
 * URB inputs/outputs.
 */
struct ilo_state_shader_urb_info {
   uint8_t cv_input_attr_count;

   uint8_t read_base;
   uint8_t read_count;

   uint8_t output_attr_count;

   uint8_t user_cull_enables;
   uint8_t user_clip_enables;
};

struct ilo_state_vs_info {
   struct ilo_state_shader_kernel_info kernel;
   struct ilo_state_shader_resource_info resource;
   struct ilo_state_shader_urb_info urb;

   bool dispatch_enable;
   bool stats_enable;
};

struct ilo_state_hs_info {
   struct ilo_state_shader_kernel_info kernel;
   struct ilo_state_shader_resource_info resource;
   struct ilo_state_shader_urb_info urb;

   bool dispatch_enable;
   bool stats_enable;
};

struct ilo_state_ds_info {
   struct ilo_state_shader_kernel_info kernel;
   struct ilo_state_shader_resource_info resource;
   struct ilo_state_shader_urb_info urb;

   bool dispatch_enable;
   bool stats_enable;
};

/**
 * Stream output.  Must be consistent with ilo_state_sol_info.
 */
struct ilo_state_gs_sol_info {
   bool sol_enable;
   bool stats_enable;
   bool render_disable;

   uint16_t svbi_post_inc;

   enum gen_reorder_mode tristrip_reorder;
};

struct ilo_state_gs_info {
   struct ilo_state_shader_kernel_info kernel;
   struct ilo_state_shader_resource_info resource;
   struct ilo_state_shader_urb_info urb;

   struct ilo_state_gs_sol_info sol;

   bool dispatch_enable;
   bool stats_enable;
};

struct ilo_state_vs {
   uint32_t vs[5];
};

struct ilo_state_hs {
   uint32_t hs[4];
};

struct ilo_state_ds {
   uint32_t te[3];
   uint32_t ds[5];
};

struct ilo_state_gs {
   uint32_t gs[5];
};

bool
ilo_state_vs_init(struct ilo_state_vs *vs,
                  const struct ilo_dev *dev,
                  const struct ilo_state_vs_info *info);

bool
ilo_state_vs_init_disabled(struct ilo_state_vs *vs,
                           const struct ilo_dev *dev);

bool
ilo_state_hs_init(struct ilo_state_hs *hs,
                  const struct ilo_dev *dev,
                  const struct ilo_state_hs_info *info);

bool
ilo_state_hs_init_disabled(struct ilo_state_hs *hs,
                           const struct ilo_dev *dev);


bool
ilo_state_ds_init(struct ilo_state_ds *ds,
                  const struct ilo_dev *dev,
                  const struct ilo_state_ds_info *info);

bool
ilo_state_ds_init_disabled(struct ilo_state_ds *ds,
                           const struct ilo_dev *dev);

bool
ilo_state_gs_init(struct ilo_state_gs *gs,
                  const struct ilo_dev *dev,
                  const struct ilo_state_gs_info *info);

bool
ilo_state_gs_init_disabled(struct ilo_state_gs *gs,
                           const struct ilo_dev *dev);

#endif /* ILO_STATE_SHADER_H */
