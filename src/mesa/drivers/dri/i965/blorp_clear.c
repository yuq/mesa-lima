/*
 * Copyright © 2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "main/teximage.h"
#include "main/blend.h"
#include "main/fbobject.h"
#include "main/renderbuffer.h"
#include "main/glformats.h"

#include "util/ralloc.h"

#include "intel_fbo.h"

#include "blorp_priv.h"
#include "brw_meta_util.h"
#include "brw_context.h"
#include "brw_eu.h"

#include "nir_builder.h"

#define FILE_DEBUG_FLAG DEBUG_BLORP

struct brw_blorp_const_color_prog_key
{
   bool use_simd16_replicated_data;
   bool pad[3];
};

static void
brw_blorp_params_get_clear_kernel(struct blorp_context *blorp,
                                  struct brw_blorp_params *params,
                                  bool use_replicated_data)
{
   struct brw_blorp_const_color_prog_key blorp_key;
   memset(&blorp_key, 0, sizeof(blorp_key));
   blorp_key.use_simd16_replicated_data = use_replicated_data;

   if (blorp->lookup_shader(blorp, &blorp_key, sizeof(blorp_key),
                            &params->wm_prog_kernel, &params->wm_prog_data))
      return;

   void *mem_ctx = ralloc_context(NULL);

   nir_builder b;
   nir_builder_init_simple_shader(&b, NULL, MESA_SHADER_FRAGMENT, NULL);
   b.shader->info.name = ralloc_strdup(b.shader, "BLORP-clear");

   nir_variable *v_color = nir_variable_create(b.shader, nir_var_shader_in,
                                               glsl_vec4_type(), "v_color");
   v_color->data.location = VARYING_SLOT_VAR0;
   v_color->data.interpolation = INTERP_MODE_FLAT;

   nir_variable *frag_color = nir_variable_create(b.shader, nir_var_shader_out,
                                                  glsl_vec4_type(),
                                                  "gl_FragColor");
   frag_color->data.location = FRAG_RESULT_COLOR;

   nir_copy_var(&b, frag_color, v_color);

   struct brw_wm_prog_key wm_key;
   brw_blorp_init_wm_prog_key(&wm_key);

   struct brw_blorp_prog_data prog_data;
   unsigned program_size;
   const unsigned *program =
      brw_blorp_compile_nir_shader(blorp, b.shader, &wm_key, use_replicated_data,
                                   &prog_data, &program_size);

   blorp->upload_shader(blorp, &blorp_key, sizeof(blorp_key),
                        program, program_size,
                        &prog_data, sizeof(prog_data),
                        &params->wm_prog_kernel, &params->wm_prog_data);

   ralloc_free(mem_ctx);
}

void
blorp_fast_clear(struct blorp_batch *batch,
                 const struct brw_blorp_surf *surf,
                 uint32_t level, uint32_t layer,
                 uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
   struct brw_blorp_params params;
   brw_blorp_params_init(&params);

   params.x0 = x0;
   params.y0 = y0;
   params.x1 = x1;
   params.y1 = y1;

   memset(&params.wm_inputs, 0xff, 4*sizeof(float));
   params.fast_clear_op = BLORP_FAST_CLEAR_OP_CLEAR;

   brw_get_fast_clear_rect(batch->blorp->isl_dev, surf->aux_surf,
                           &params.x0, &params.y0, &params.x1, &params.y1);

   brw_blorp_params_get_clear_kernel(batch->blorp, &params, true);

   brw_blorp_surface_info_init(batch->blorp, &params.dst, surf, level, layer,
                               surf->surf->format, true);

   batch->blorp->exec(batch, &params);
}


void
blorp_clear(struct blorp_batch *batch,
            const struct brw_blorp_surf *surf,
            uint32_t level, uint32_t layer,
            uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
            enum isl_format format, union isl_color_value clear_color,
            bool color_write_disable[4])
{
   struct brw_blorp_params params;
   brw_blorp_params_init(&params);

   params.x0 = x0;
   params.y0 = y0;
   params.x1 = x1;
   params.y1 = y1;

   memcpy(&params.wm_inputs, clear_color.f32, sizeof(float) * 4);

   bool use_simd16_replicated_data = true;

   /* From the SNB PRM (Vol4_Part1):
    *
    *     "Replicated data (Message Type = 111) is only supported when
    *      accessing tiled memory.  Using this Message Type to access linear
    *      (untiled) memory is UNDEFINED."
    */
   if (surf->surf->tiling == ISL_TILING_LINEAR)
      use_simd16_replicated_data = false;

   /* Constant color writes ignore everyting in blend and color calculator
    * state.  This is not documented.
    */
   for (unsigned i = 0; i < 4; i++) {
      params.color_write_disable[i] = color_write_disable[i];
      if (color_write_disable[i])
         use_simd16_replicated_data = false;
   }

   brw_blorp_params_get_clear_kernel(batch->blorp, &params,
                                     use_simd16_replicated_data);

   brw_blorp_surface_info_init(batch->blorp, &params.dst, surf, level, layer,
                               format, true);

   batch->blorp->exec(batch, &params);
}

void
brw_blorp_ccs_resolve(struct blorp_batch *batch,
                      struct brw_blorp_surf *surf, enum isl_format format)
{
   struct brw_blorp_params params;
   brw_blorp_params_init(&params);

   brw_blorp_surface_info_init(batch->blorp, &params.dst, surf,
                               0 /* level */, 0 /* layer */, format, true);

   brw_get_ccs_resolve_rect(batch->blorp->isl_dev, &params.dst.aux_surf,
                            &params.x0, &params.y0,
                            &params.x1, &params.y1);

   if (batch->blorp->isl_dev->info->gen >= 9) {
      if (params.dst.aux_usage == ISL_AUX_USAGE_CCS_E)
         params.fast_clear_op = BLORP_FAST_CLEAR_OP_RESOLVE_FULL;
      else
         params.fast_clear_op = BLORP_FAST_CLEAR_OP_RESOLVE_PARTIAL;
   } else {
      /* Broadwell and earlier do not have a partial resolve */
      params.fast_clear_op = BLORP_FAST_CLEAR_OP_RESOLVE_FULL;
   }

   /* Note: there is no need to initialize push constants because it doesn't
    * matter what data gets dispatched to the render target.  However, we must
    * ensure that the fragment shader delivers the data using the "replicated
    * color" message.
    */

   brw_blorp_params_get_clear_kernel(batch->blorp, &params, true);

   batch->blorp->exec(batch, &params);
}
