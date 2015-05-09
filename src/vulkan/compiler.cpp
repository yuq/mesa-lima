/*
 * Copyright © 2015 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <brw_context.h>
#include <brw_wm.h> /* brw_new_shader_program is here */

#include <brw_vs.h>
#include <brw_gs.h>

#include <mesa/main/shaderobj.h>
#include <mesa/main/fbobject.h>
#include <mesa/program/program.h>
#include <glsl/program.h>

#include "private.h"

static void
fail_if(int cond, const char *format, ...)
{
   va_list args;

   if (!cond)
      return;

   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);

   exit(1);
}

static VkResult
set_binding_table_layout(struct brw_stage_prog_data *prog_data,
                         struct anv_pipeline *pipeline, uint32_t stage)
{
   uint32_t count, bias, set, *map;

   struct anv_pipeline_layout_entry *entries;

   if (stage == VK_SHADER_STAGE_FRAGMENT)
      bias = MAX_RTS;
   else
      bias = 0;

   count = pipeline->layout->stage[stage].count;
   entries = pipeline->layout->stage[stage].entries;

   prog_data->map_entries =
      (uint32_t *) malloc(count * sizeof(prog_data->map_entries[0]));
   if (prog_data->map_entries == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   set = 0;
   map = prog_data->map_entries;
   for (uint32_t i = 0; i < count; i++) {
      if (entries[i].set == set) {
         prog_data->bind_map[set] = map;
         set++;
      }
      *map++ = bias + i;
   }

   return VK_SUCCESS;
}

static void
brw_vs_populate_key(struct brw_context *brw,
                    struct brw_vertex_program *vp,
                    struct brw_vs_prog_key *key)
{
   struct gl_context *ctx = &brw->ctx;
   /* BRW_NEW_VERTEX_PROGRAM */
   struct gl_program *prog = (struct gl_program *) vp;

   memset(key, 0, sizeof(*key));

   /* Just upload the program verbatim for now.  Always send it all
    * the inputs it asks for, whether they are varying or not.
    */
   key->base.program_string_id = vp->id;
   brw_setup_vue_key_clip_info(brw, &key->base,
                               vp->program.Base.UsesClipDistanceOut);

   /* _NEW_POLYGON */
   if (brw->gen < 6) {
      key->copy_edgeflag = (ctx->Polygon.FrontMode != GL_FILL ||
                           ctx->Polygon.BackMode != GL_FILL);
   }

   if (prog->OutputsWritten & (VARYING_BIT_COL0 | VARYING_BIT_COL1 |
                               VARYING_BIT_BFC0 | VARYING_BIT_BFC1)) {
      /* _NEW_LIGHT | _NEW_BUFFERS */
      key->clamp_vertex_color = ctx->Light._ClampVertexColor;
   }

   /* _NEW_POINT */
   if (brw->gen < 6 && ctx->Point.PointSprite) {
      for (int i = 0; i < 8; i++) {
         if (ctx->Point.CoordReplace[i])
            key->point_coord_replace |= (1 << i);
      }
   }

   /* _NEW_TEXTURE */
   brw_populate_sampler_prog_key_data(ctx, prog, brw->vs.base.sampler_count,
                                      &key->base.tex);
}

static bool
really_do_vs_prog(struct brw_context *brw,
                  struct gl_shader_program *prog,
                  struct brw_vertex_program *vp,
                  struct brw_vs_prog_key *key, struct anv_pipeline *pipeline)
{
   GLuint program_size;
   const GLuint *program;
   struct brw_vs_compile c;
   struct brw_vs_prog_data *prog_data = &pipeline->vs_prog_data;
   struct brw_stage_prog_data *stage_prog_data = &prog_data->base.base;
   void *mem_ctx;
   struct gl_shader *vs = NULL;

   if (prog)
      vs = prog->_LinkedShaders[MESA_SHADER_VERTEX];

   memset(&c, 0, sizeof(c));
   memcpy(&c.key, key, sizeof(*key));
   memset(prog_data, 0, sizeof(*prog_data));

   mem_ctx = ralloc_context(NULL);

   c.vp = vp;

   /* Allocate the references to the uniforms that will end up in the
    * prog_data associated with the compiled program, and which will be freed
    * by the state cache.
    */
   int param_count;
   if (vs) {
      /* We add padding around uniform values below vec4 size, with the worst
       * case being a float value that gets blown up to a vec4, so be
       * conservative here.
       */
      param_count = vs->num_uniform_components * 4;

   } else {
      param_count = vp->program.Base.Parameters->NumParameters * 4;
   }
   /* vec4_visitor::setup_uniform_clipplane_values() also uploads user clip
    * planes as uniforms.
    */
   param_count += c.key.base.nr_userclip_plane_consts * 4;

   /* Setting nr_params here NOT to the size of the param and pull_param
    * arrays, but to the number of uniform components vec4_visitor
    * needs. vec4_visitor::setup_uniforms() will set it back to a proper value.
    */
   stage_prog_data->nr_params = ALIGN(param_count, 4) / 4;
   if (vs) {
      stage_prog_data->nr_params += vs->num_samplers;
   }

   GLbitfield64 outputs_written = vp->program.Base.OutputsWritten;
   prog_data->inputs_read = vp->program.Base.InputsRead;

   if (c.key.copy_edgeflag) {
      outputs_written |= BITFIELD64_BIT(VARYING_SLOT_EDGE);
      prog_data->inputs_read |= VERT_BIT_EDGEFLAG;
   }

   if (brw->gen < 6) {
      /* Put dummy slots into the VUE for the SF to put the replaced
       * point sprite coords in.  We shouldn't need these dummy slots,
       * which take up precious URB space, but it would mean that the SF
       * doesn't get nice aligned pairs of input coords into output
       * coords, which would be a pain to handle.
       */
      for (int i = 0; i < 8; i++) {
         if (c.key.point_coord_replace & (1 << i))
            outputs_written |= BITFIELD64_BIT(VARYING_SLOT_TEX0 + i);
      }

      /* if back colors are written, allocate slots for front colors too */
      if (outputs_written & BITFIELD64_BIT(VARYING_SLOT_BFC0))
         outputs_written |= BITFIELD64_BIT(VARYING_SLOT_COL0);
      if (outputs_written & BITFIELD64_BIT(VARYING_SLOT_BFC1))
         outputs_written |= BITFIELD64_BIT(VARYING_SLOT_COL1);
   }

   /* In order for legacy clipping to work, we need to populate the clip
    * distance varying slots whenever clipping is enabled, even if the vertex
    * shader doesn't write to gl_ClipDistance.
    */
   if (c.key.base.userclip_active) {
      outputs_written |= BITFIELD64_BIT(VARYING_SLOT_CLIP_DIST0);
      outputs_written |= BITFIELD64_BIT(VARYING_SLOT_CLIP_DIST1);
   }

   brw_compute_vue_map(brw->intelScreen->devinfo,
                       &prog_data->base.vue_map, outputs_written);
\
   set_binding_table_layout(&prog_data->base.base, pipeline,
                            VK_SHADER_STAGE_VERTEX);

   /* Emit GEN4 code.
    */
   program = brw_vs_emit(brw, prog, &c, prog_data, mem_ctx, &program_size);
   if (program == NULL) {
      ralloc_free(mem_ctx);
      return false;
   }

   pipeline->vs_simd8 = pipeline->program_next;
   memcpy((char *) pipeline->device->instruction_block_pool.map +
          pipeline->vs_simd8, program, program_size);

   pipeline->program_next = align(pipeline->program_next + program_size, 64);

   ralloc_free(mem_ctx);

   if (stage_prog_data->total_scratch > 0)
      if (!anv_bo_init_new(&pipeline->vs_scratch_bo,
                           pipeline->device,
                           stage_prog_data->total_scratch))
         return false;


   return true;
}

void brw_wm_populate_key(struct brw_context *brw,
                         struct brw_fragment_program *fp,
                         struct brw_wm_prog_key *key)
{
   struct gl_context *ctx = &brw->ctx;
   struct gl_program *prog = (struct gl_program *) brw->fragment_program;
   GLuint lookup = 0;
   GLuint line_aa;
   bool program_uses_dfdy = fp->program.UsesDFdy;
   struct gl_framebuffer draw_buffer;
   bool multisample_fbo;

   memset(key, 0, sizeof(*key));

   for (int i = 0; i < MAX_SAMPLERS; i++) {
      /* Assume color sampler, no swizzling. */
      key->tex.swizzles[i] = SWIZZLE_XYZW;
   }

   /* A non-zero framebuffer name indicates that the framebuffer was created by
    * the user rather than the window system. */
   draw_buffer.Name = 1;
   draw_buffer.Visual.samples = 1;
   draw_buffer._NumColorDrawBuffers = 1;
   draw_buffer._NumColorDrawBuffers = 1;
   draw_buffer.Width = 400;
   draw_buffer.Height = 400;
   ctx->DrawBuffer = &draw_buffer;

   multisample_fbo = ctx->DrawBuffer->Visual.samples > 1;

   /* Build the index for table lookup
    */
   if (brw->gen < 6) {
      /* _NEW_COLOR */
      if (fp->program.UsesKill || ctx->Color.AlphaEnabled)
         lookup |= IZ_PS_KILL_ALPHATEST_BIT;

      if (fp->program.Base.OutputsWritten & BITFIELD64_BIT(FRAG_RESULT_DEPTH))
         lookup |= IZ_PS_COMPUTES_DEPTH_BIT;

      /* _NEW_DEPTH */
      if (ctx->Depth.Test)
         lookup |= IZ_DEPTH_TEST_ENABLE_BIT;

      if (ctx->Depth.Test && ctx->Depth.Mask) /* ?? */
         lookup |= IZ_DEPTH_WRITE_ENABLE_BIT;

      /* _NEW_STENCIL | _NEW_BUFFERS */
      if (ctx->Stencil._Enabled) {
         lookup |= IZ_STENCIL_TEST_ENABLE_BIT;

         if (ctx->Stencil.WriteMask[0] ||
             ctx->Stencil.WriteMask[ctx->Stencil._BackFace])
            lookup |= IZ_STENCIL_WRITE_ENABLE_BIT;
      }
      key->iz_lookup = lookup;
   }

   line_aa = AA_NEVER;

   /* _NEW_LINE, _NEW_POLYGON, BRW_NEW_REDUCED_PRIMITIVE */
   if (ctx->Line.SmoothFlag) {
      if (brw->reduced_primitive == GL_LINES) {
         line_aa = AA_ALWAYS;
      }
      else if (brw->reduced_primitive == GL_TRIANGLES) {
         if (ctx->Polygon.FrontMode == GL_LINE) {
            line_aa = AA_SOMETIMES;

            if (ctx->Polygon.BackMode == GL_LINE ||
                (ctx->Polygon.CullFlag &&
                 ctx->Polygon.CullFaceMode == GL_BACK))
               line_aa = AA_ALWAYS;
         }
         else if (ctx->Polygon.BackMode == GL_LINE) {
            line_aa = AA_SOMETIMES;

            if ((ctx->Polygon.CullFlag &&
                 ctx->Polygon.CullFaceMode == GL_FRONT))
               line_aa = AA_ALWAYS;
         }
      }
   }

   key->line_aa = line_aa;

   /* _NEW_HINT */
   key->high_quality_derivatives =
      ctx->Hint.FragmentShaderDerivative == GL_NICEST;

   if (brw->gen < 6)
      key->stats_wm = brw->stats_wm;

   /* _NEW_LIGHT */
   key->flat_shade = (ctx->Light.ShadeModel == GL_FLAT);

   /* _NEW_FRAG_CLAMP | _NEW_BUFFERS */
   key->clamp_fragment_color = ctx->Color._ClampFragmentColor;

   /* _NEW_TEXTURE */
   brw_populate_sampler_prog_key_data(ctx, prog, brw->wm.base.sampler_count,
                                      &key->tex);

   /* _NEW_BUFFERS */
   /*
    * Include the draw buffer origin and height so that we can calculate
    * fragment position values relative to the bottom left of the drawable,
    * from the incoming screen origin relative position we get as part of our
    * payload.
    *
    * This is only needed for the WM_WPOSXY opcode when the fragment program
    * uses the gl_FragCoord input.
    *
    * We could avoid recompiling by including this as a constant referenced by
    * our program, but if we were to do that it would also be nice to handle
    * getting that constant updated at batchbuffer submit time (when we
    * hold the lock and know where the buffer really is) rather than at emit
    * time when we don't hold the lock and are just guessing.  We could also
    * just avoid using this as key data if the program doesn't use
    * fragment.position.
    *
    * For DRI2 the origin_x/y will always be (0,0) but we still need the
    * drawable height in order to invert the Y axis.
    */
   if (fp->program.Base.InputsRead & VARYING_BIT_POS) {
      key->drawable_height = ctx->DrawBuffer->Height;
   }

   if ((fp->program.Base.InputsRead & VARYING_BIT_POS) || program_uses_dfdy) {
      key->render_to_fbo = _mesa_is_user_fbo(ctx->DrawBuffer);
   }

   /* _NEW_BUFFERS */
   key->nr_color_regions = ctx->DrawBuffer->_NumColorDrawBuffers;

   /* _NEW_MULTISAMPLE, _NEW_COLOR, _NEW_BUFFERS */
   key->replicate_alpha = ctx->DrawBuffer->_NumColorDrawBuffers > 1 &&
      (ctx->Multisample.SampleAlphaToCoverage || ctx->Color.AlphaEnabled);

   /* _NEW_BUFFERS _NEW_MULTISAMPLE */
   /* Ignore sample qualifier while computing this flag. */
   key->persample_shading =
      _mesa_get_min_invocations_per_fragment(ctx, &fp->program, true) > 1;
   if (key->persample_shading)
      key->persample_2x = ctx->DrawBuffer->Visual.samples == 2;

   key->compute_pos_offset =
      _mesa_get_min_invocations_per_fragment(ctx, &fp->program, false) > 1 &&
      fp->program.Base.SystemValuesRead & SYSTEM_BIT_SAMPLE_POS;

   key->compute_sample_id =
      multisample_fbo &&
      ctx->Multisample.Enabled &&
      (fp->program.Base.SystemValuesRead & SYSTEM_BIT_SAMPLE_ID);

   /* BRW_NEW_VUE_MAP_GEOM_OUT */
   if (brw->gen < 6 || _mesa_bitcount_64(fp->program.Base.InputsRead &
                                         BRW_FS_VARYING_INPUT_MASK) > 16)
      key->input_slots_valid = brw->vue_map_geom_out.slots_valid;


   /* _NEW_COLOR | _NEW_BUFFERS */
   /* Pre-gen6, the hardware alpha test always used each render
    * target's alpha to do alpha test, as opposed to render target 0's alpha
    * like GL requires.  Fix that by building the alpha test into the
    * shader, and we'll skip enabling the fixed function alpha test.
    */
   if (brw->gen < 6 && ctx->DrawBuffer->_NumColorDrawBuffers > 1 && ctx->Color.AlphaEnabled) {
      key->alpha_test_func = ctx->Color.AlphaFunc;
      key->alpha_test_ref = ctx->Color.AlphaRef;
   }

   /* The unique fragment program ID */
   key->program_string_id = fp->id;

   ctx->DrawBuffer = NULL;
}

static uint8_t
computed_depth_mode(struct gl_fragment_program *fp)
{
   if (fp->Base.OutputsWritten & BITFIELD64_BIT(FRAG_RESULT_DEPTH)) {
      switch (fp->FragDepthLayout) {
      case FRAG_DEPTH_LAYOUT_NONE:
      case FRAG_DEPTH_LAYOUT_ANY:
         return BRW_PSCDEPTH_ON;
      case FRAG_DEPTH_LAYOUT_GREATER:
         return BRW_PSCDEPTH_ON_GE;
      case FRAG_DEPTH_LAYOUT_LESS:
         return BRW_PSCDEPTH_ON_LE;
      case FRAG_DEPTH_LAYOUT_UNCHANGED:
         return BRW_PSCDEPTH_OFF;
      }
   }
   return BRW_PSCDEPTH_OFF;
}

static bool
really_do_wm_prog(struct brw_context *brw,
                  struct gl_shader_program *prog,
                  struct brw_fragment_program *fp,
                  struct brw_wm_prog_key *key, struct anv_pipeline *pipeline)
{
   struct gl_context *ctx = &brw->ctx;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_wm_prog_data *prog_data = &pipeline->wm_prog_data;
   struct gl_shader *fs = NULL;
   unsigned int program_size;
   const uint32_t *program;
   uint32_t offset;

   if (prog)
      fs = prog->_LinkedShaders[MESA_SHADER_FRAGMENT];

   memset(prog_data, 0, sizeof(*prog_data));

   /* key->alpha_test_func means simulating alpha testing via discards,
    * so the shader definitely kills pixels.
    */
   prog_data->uses_kill = fp->program.UsesKill || key->alpha_test_func;

   prog_data->computed_depth_mode = computed_depth_mode(&fp->program);

   /* Allocate the references to the uniforms that will end up in the
    * prog_data associated with the compiled program, and which will be freed
    * by the state cache.
    */
   int param_count;
   if (fs) {
      param_count = fs->num_uniform_components;
   } else {
      param_count = fp->program.Base.Parameters->NumParameters * 4;
   }
   /* The backend also sometimes adds params for texture size. */
   param_count += 2 * ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits;
   prog_data->base.param =
      rzalloc_array(NULL, const gl_constant_value *, param_count);
   prog_data->base.pull_param =
      rzalloc_array(NULL, const gl_constant_value *, param_count);
   prog_data->base.nr_params = param_count;

   prog_data->barycentric_interp_modes =
      brw_compute_barycentric_interp_modes(brw, key->flat_shade,
                                           key->persample_shading,
                                           &fp->program);

   set_binding_table_layout(&prog_data->base, pipeline,
                            VK_SHADER_STAGE_FRAGMENT);
   /* This needs to come after shader time and pull constant entries, but we
    * don't have those set up now, so just put it after the layout entries.
    */
   prog_data->binding_table.render_target_start = 0;

   program = brw_wm_fs_emit(brw, mem_ctx, key, prog_data,
                            &fp->program, prog, &program_size);
   if (program == NULL) {
      ralloc_free(mem_ctx);
      return false;
   }

   offset = pipeline->program_next;
   pipeline->program_next = align(pipeline->program_next + program_size, 64);

   if (prog_data->no_8)
      pipeline->ps_simd8 = NO_KERNEL;
   else
      pipeline->ps_simd8 = offset;

   if (prog_data->no_8 || prog_data->prog_offset_16)
      pipeline->ps_simd16 = offset + prog_data->prog_offset_16;
   else
      pipeline->ps_simd16 = NO_KERNEL;

   memcpy((char *) pipeline->device->instruction_block_pool.map +
          offset, program, program_size);

   ralloc_free(mem_ctx);

   if (prog_data->base.total_scratch > 0)
      if (!anv_bo_init_new(&pipeline->ps_scratch_bo,
                           pipeline->device,
                           prog_data->base.total_scratch))
         return false;

   return true;
}

static void
brw_gs_populate_key(struct brw_context *brw,
                    struct anv_pipeline *pipeline,
                    struct brw_geometry_program *gp,
                    struct brw_gs_prog_key *key)
{
   struct gl_context *ctx = &brw->ctx;
   struct brw_stage_state *stage_state = &brw->gs.base;
   struct gl_program *prog = &gp->program.Base;

   memset(key, 0, sizeof(*key));

   key->base.program_string_id = gp->id;
   brw_setup_vue_key_clip_info(brw, &key->base,
                               gp->program.Base.UsesClipDistanceOut);

   /* _NEW_TEXTURE */
   brw_populate_sampler_prog_key_data(ctx, prog, stage_state->sampler_count,
                                      &key->base.tex);

   struct brw_vs_prog_data *prog_data = &pipeline->vs_prog_data;

   /* BRW_NEW_VUE_MAP_VS */
   key->input_varyings = prog_data->base.vue_map.slots_valid;
}

static bool
really_do_gs_prog(struct brw_context *brw,
                  struct gl_shader_program *prog,
                  struct brw_geometry_program *gp,
                  struct brw_gs_prog_key *key, struct anv_pipeline *pipeline)
{
   struct brw_gs_compile_output output;
   uint32_t offset;

   /* FIXME: We pass the bind map to the compile in the output struct. Need
    * something better. */
   set_binding_table_layout(&output.prog_data.base.base,
                            pipeline, VK_SHADER_STAGE_GEOMETRY);

   brw_compile_gs_prog(brw, prog, gp, key, &output);

   offset = pipeline->program_next;
   pipeline->program_next = align(pipeline->program_next + output.program_size, 64);

   pipeline->gs_vec4 = offset;
   pipeline->gs_vertex_count = gp->program.VerticesIn;

   memcpy((char *) pipeline->device->instruction_block_pool.map +
          offset, output.program, output.program_size);

   ralloc_free(output.mem_ctx);

   if (output.prog_data.base.base.total_scratch) {
      if (!anv_bo_init_new(&pipeline->gs_scratch_bo,
                           pipeline->device,
                           output.prog_data.base.base.total_scratch))
         return false;
   }

   memcpy(&pipeline->gs_prog_data, &output.prog_data, sizeof pipeline->gs_prog_data);

   return true;
}

static void
fail_on_compile_error(int status, const char *msg)
{
   int source, line, column;
   char error[256];

   if (status)
      return;

   if (sscanf(msg, "%d:%d(%d): error: %255[^\n]", &source, &line, &column, error) == 4)
      fail_if(!status, "%d:%s\n", line, error);
   else
      fail_if(!status, "%s\n", msg);
}

struct anv_compiler {
   struct intel_screen *screen;
   struct brw_context *brw;
};


extern "C" {

struct anv_compiler *
anv_compiler_create(int fd)
{
   struct anv_compiler *compiler;

   compiler = (struct anv_compiler *) malloc(sizeof *compiler);
   if (compiler == NULL)
      return NULL;

   compiler->screen = intel_screen_create(fd);
   if (compiler->screen == NULL) {
      free(compiler);
      return NULL;
   }

   compiler->brw = intel_context_create(compiler->screen);
   if (compiler->brw == NULL) {
      free(compiler);
      return NULL;
   }

   compiler->brw->precompile = false;

   return compiler;
}

void
anv_compiler_destroy(struct anv_compiler *compiler)
{
   intel_context_destroy(compiler->brw);
   intel_screen_destroy(compiler->screen);
   free(compiler);
}

/* From gen7_urb.c */

/* FIXME: Add to struct intel_device_info */

static const int gen8_push_size = 32 * 1024;

static void
gen7_compute_urb_partition(struct anv_pipeline *pipeline)
{
   const struct brw_device_info *devinfo = &pipeline->device->info;
   unsigned vs_size = pipeline->vs_prog_data.base.urb_entry_size;
   unsigned vs_entry_size_bytes = vs_size * 64;
   bool gs_present = pipeline->gs_vec4 != NO_KERNEL;
   unsigned gs_size = gs_present ? pipeline->gs_prog_data.base.urb_entry_size : 1;
   unsigned gs_entry_size_bytes = gs_size * 64;

   /* From p35 of the Ivy Bridge PRM (section 1.7.1: 3DSTATE_URB_GS):
    *
    *     VS Number of URB Entries must be divisible by 8 if the VS URB Entry
    *     Allocation Size is less than 9 512-bit URB entries.
    *
    * Similar text exists for GS.
    */
   unsigned vs_granularity = (vs_size < 9) ? 8 : 1;
   unsigned gs_granularity = (gs_size < 9) ? 8 : 1;

   /* URB allocations must be done in 8k chunks. */
   unsigned chunk_size_bytes = 8192;

   /* Determine the size of the URB in chunks. */
   unsigned urb_chunks = devinfo->urb.size * 1024 / chunk_size_bytes;

   /* Reserve space for push constants */
   unsigned push_constant_bytes = gen8_push_size;
   unsigned push_constant_chunks =
      push_constant_bytes / chunk_size_bytes;

   /* Initially, assign each stage the minimum amount of URB space it needs,
    * and make a note of how much additional space it "wants" (the amount of
    * additional space it could actually make use of).
    */

   /* VS has a lower limit on the number of URB entries */
   unsigned vs_chunks =
      ALIGN(devinfo->urb.min_vs_entries * vs_entry_size_bytes,
            chunk_size_bytes) / chunk_size_bytes;
   unsigned vs_wants =
      ALIGN(devinfo->urb.max_vs_entries * vs_entry_size_bytes,
            chunk_size_bytes) / chunk_size_bytes - vs_chunks;

   unsigned gs_chunks = 0;
   unsigned gs_wants = 0;
   if (gs_present) {
      /* There are two constraints on the minimum amount of URB space we can
       * allocate:
       *
       * (1) We need room for at least 2 URB entries, since we always operate
       * the GS in DUAL_OBJECT mode.
       *
       * (2) We can't allocate less than nr_gs_entries_granularity.
       */
      gs_chunks = ALIGN(MAX2(gs_granularity, 2) * gs_entry_size_bytes,
                        chunk_size_bytes) / chunk_size_bytes;
      gs_wants =
         ALIGN(devinfo->urb.max_gs_entries * gs_entry_size_bytes,
               chunk_size_bytes) / chunk_size_bytes - gs_chunks;
   }

   /* There should always be enough URB space to satisfy the minimum
    * requirements of each stage.
    */
   unsigned total_needs = push_constant_chunks + vs_chunks + gs_chunks;
   assert(total_needs <= urb_chunks);

   /* Mete out remaining space (if any) in proportion to "wants". */
   unsigned total_wants = vs_wants + gs_wants;
   unsigned remaining_space = urb_chunks - total_needs;
   if (remaining_space > total_wants)
      remaining_space = total_wants;
   if (remaining_space > 0) {
      unsigned vs_additional = (unsigned)
         round(vs_wants * (((double) remaining_space) / total_wants));
      vs_chunks += vs_additional;
      remaining_space -= vs_additional;
      gs_chunks += remaining_space;
   }

   /* Sanity check that we haven't over-allocated. */
   assert(push_constant_chunks + vs_chunks + gs_chunks <= urb_chunks);

   /* Finally, compute the number of entries that can fit in the space
    * allocated to each stage.
    */
   unsigned nr_vs_entries = vs_chunks * chunk_size_bytes / vs_entry_size_bytes;
   unsigned nr_gs_entries = gs_chunks * chunk_size_bytes / gs_entry_size_bytes;

   /* Since we rounded up when computing *_wants, this may be slightly more
    * than the maximum allowed amount, so correct for that.
    */
   nr_vs_entries = MIN2(nr_vs_entries, devinfo->urb.max_vs_entries);
   nr_gs_entries = MIN2(nr_gs_entries, devinfo->urb.max_gs_entries);

   /* Ensure that we program a multiple of the granularity. */
   nr_vs_entries = ROUND_DOWN_TO(nr_vs_entries, vs_granularity);
   nr_gs_entries = ROUND_DOWN_TO(nr_gs_entries, gs_granularity);

   /* Finally, sanity check to make sure we have at least the minimum number
    * of entries needed for each stage.
    */
   assert(nr_vs_entries >= devinfo->urb.min_vs_entries);
   if (gs_present)
      assert(nr_gs_entries >= 2);

   /* Lay out the URB in the following order:
    * - push constants
    * - VS
    * - GS
    */
   pipeline->urb.vs_start = push_constant_chunks;
   pipeline->urb.vs_size = vs_size;
   pipeline->urb.nr_vs_entries = nr_vs_entries;

   pipeline->urb.gs_start = push_constant_chunks + vs_chunks;
   pipeline->urb.gs_size = gs_size;
   pipeline->urb.nr_gs_entries = nr_gs_entries;
}

static const struct {
   uint32_t token;
   const char *name;
} stage_info[] = {
   { GL_VERTEX_SHADER, "vertex" },
   { GL_TESS_CONTROL_SHADER, "tess control" },
   { GL_TESS_EVALUATION_SHADER, "tess evaluation" },
   { GL_GEOMETRY_SHADER, "geometry" },
   { GL_FRAGMENT_SHADER, "fragment" },
   { GL_COMPUTE_SHADER, "compute" },
};

static void
anv_compile_shader(struct anv_compiler *compiler,
                   struct gl_shader_program *program,
                   struct anv_pipeline *pipeline, uint32_t stage)
{
   struct brw_context *brw = compiler->brw;
   struct gl_shader *shader;
   int name = 0;

   shader = brw_new_shader(&brw->ctx, name, stage_info[stage].token);
   fail_if(shader == NULL, "failed to create %s shader\n", stage_info[stage].name);
   shader->Source = strdup(pipeline->shaders[stage]->data);
   _mesa_glsl_compile_shader(&brw->ctx, shader, false, false);
   fail_on_compile_error(shader->CompileStatus, shader->InfoLog);

   program->Shaders[program->NumShaders] = shader;
   program->NumShaders++;
}

int
anv_compiler_run(struct anv_compiler *compiler, struct anv_pipeline *pipeline)
{
   struct gl_shader_program *program;
   int name = 0;
   struct brw_context *brw = compiler->brw;
   struct anv_device *device = pipeline->device;

   brw->use_rep_send = pipeline->use_repclear;
   brw->no_simd8 = pipeline->use_repclear;

   program = brw->ctx.Driver.NewShaderProgram(name);
   program->Shaders = (struct gl_shader **)
      calloc(VK_NUM_SHADER_STAGE, sizeof(struct gl_shader *));
   fail_if(program == NULL || program->Shaders == NULL,
           "failed to create program\n");

   /* FIXME: Only supports vs and fs combo at the moment */
   assert(pipeline->shaders[VK_SHADER_STAGE_VERTEX]);
   assert(pipeline->shaders[VK_SHADER_STAGE_FRAGMENT]);

   anv_compile_shader(compiler, program, pipeline, VK_SHADER_STAGE_VERTEX);
   anv_compile_shader(compiler, program, pipeline, VK_SHADER_STAGE_FRAGMENT);
   if (pipeline->shaders[VK_SHADER_STAGE_GEOMETRY])
      anv_compile_shader(compiler, program, pipeline, VK_SHADER_STAGE_GEOMETRY);

   _mesa_glsl_link_shader(&brw->ctx, program);
   fail_on_compile_error(program->LinkStatus,
                         program->InfoLog);

   pipeline->program_block =
      anv_block_pool_alloc(&device->instruction_block_pool);
   pipeline->program_next = pipeline->program_block;


   bool success;
   struct brw_wm_prog_key wm_key;
   struct gl_fragment_program *fp = (struct gl_fragment_program *)
      program->_LinkedShaders[MESA_SHADER_FRAGMENT]->Program;
   struct brw_fragment_program *bfp = brw_fragment_program(fp);

   brw_wm_populate_key(brw, bfp, &wm_key);

   success = really_do_wm_prog(brw, program, bfp, &wm_key, pipeline);
   fail_if(!success, "do_wm_prog failed\n");
   pipeline->prog_data[VK_SHADER_STAGE_FRAGMENT] = &pipeline->wm_prog_data.base;


   struct brw_vs_prog_key vs_key;
   struct gl_vertex_program *vp = (struct gl_vertex_program *)
      program->_LinkedShaders[MESA_SHADER_VERTEX]->Program;
   struct brw_vertex_program *bvp = brw_vertex_program(vp);

   brw_vs_populate_key(brw, bvp, &vs_key);

   success = really_do_vs_prog(brw, program, bvp, &vs_key, pipeline);
   fail_if(!success, "do_wm_prog failed\n");
   pipeline->prog_data[VK_SHADER_STAGE_VERTEX] = &pipeline->vs_prog_data.base.base;

   if (pipeline->shaders[VK_SHADER_STAGE_GEOMETRY]) {
      struct brw_gs_prog_key gs_key;
      struct gl_geometry_program *gp = (struct gl_geometry_program *)
         program->_LinkedShaders[MESA_SHADER_GEOMETRY]->Program;
      struct brw_geometry_program *bgp = brw_geometry_program(gp);

      brw_gs_populate_key(brw, pipeline, bgp, &gs_key);

      success = really_do_gs_prog(brw, program, bgp, &gs_key, pipeline);
      fail_if(!success, "do_gs_prog failed\n");
      pipeline->active_stages = VK_SHADER_STAGE_VERTEX_BIT |
         VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      pipeline->prog_data[VK_SHADER_STAGE_GEOMETRY] = &pipeline->gs_prog_data.base.base;

   } else {
      pipeline->gs_vec4 = NO_KERNEL;
      pipeline->active_stages =
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
   }


   /* FIXME: Allocate more blocks if we fill up this one and worst case,
    * allocate multiple continuous blocks from end of pool to hold really big
    * programs. */
   assert(pipeline->program_next - pipeline->program_block < 8192);

   brw->ctx.Driver.DeleteShaderProgram(&brw->ctx, program);

   gen7_compute_urb_partition(pipeline);

   return 0;
}

/* This badly named function frees the struct anv_pipeline data that the compiler
 * allocates.  Currently just the prog_data structs.
 */
void
anv_compiler_free(struct anv_pipeline *pipeline)
{
   struct anv_device *device = pipeline->device;

   for (uint32_t stage = 0; stage < VK_NUM_SHADER_STAGE; stage++)
      if (pipeline->prog_data[stage])
         free(pipeline->prog_data[stage]->map_entries);

   anv_block_pool_free(&device->instruction_block_pool,
                       pipeline->program_block);
}

}
