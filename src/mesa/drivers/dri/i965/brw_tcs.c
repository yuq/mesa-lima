/*
 * Copyright © 2014 Intel Corporation
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

/**
 * \file brw_tcs.c
 *
 * Tessellation control shader state upload code.
 */

#include "brw_context.h"
#include "brw_nir.h"
#include "brw_program.h"
#include "brw_shader.h"
#include "brw_state.h"
#include "program/prog_parameter.h"

static void
brw_tcs_debug_recompile(struct brw_context *brw,
                       struct gl_shader_program *shader_prog,
                       const struct brw_tcs_prog_key *key)
{
   struct brw_cache_item *c = NULL;
   const struct brw_tcs_prog_key *old_key = NULL;
   bool found = false;

   perf_debug("Recompiling tessellation control shader for program %d\n",
              shader_prog->Name);

   for (unsigned int i = 0; i < brw->cache.size; i++) {
      for (c = brw->cache.items[i]; c; c = c->next) {
         if (c->cache_id == BRW_CACHE_TCS_PROG) {
            old_key = c->key;

            if (old_key->program_string_id == key->program_string_id)
               break;
         }
      }
      if (c)
         break;
   }

   if (!c) {
      perf_debug("  Didn't find previous compile in the shader cache for "
                 "debug\n");
      return;
   }

   found |= key_debug(brw, "input vertices", old_key->input_vertices,
                      key->input_vertices);
   found |= key_debug(brw, "TES primitive mode", old_key->tes_primitive_mode,
                      key->tes_primitive_mode);
   found |= brw_debug_recompile_sampler_key(brw, &old_key->tex, &key->tex);

   if (!found) {
      perf_debug("  Something else\n");
   }
}

static bool
brw_codegen_tcs_prog(struct brw_context *brw,
                     struct gl_shader_program *shader_prog,
                     struct brw_tess_ctrl_program *tcp,
                     struct brw_tcs_prog_key *key)
{
   const struct brw_compiler *compiler = brw->intelScreen->compiler;
   struct brw_stage_state *stage_state = &brw->tcs.base;
   nir_shader *nir = tcp->program.Base.nir;
   struct brw_tcs_prog_data prog_data;
   bool start_busy = false;
   double start_time = 0;

   memset(&prog_data, 0, sizeof(prog_data));

   /* Allocate the references to the uniforms that will end up in the
    * prog_data associated with the compiled program, and which will be freed
    * by the state cache.
    *
    * Note: param_count needs to be num_uniform_components * 4, since we add
    * padding around uniform values below vec4 size, so the worst case is that
    * every uniform is a float which gets padded to the size of a vec4.
    */
   struct gl_shader *tcs = shader_prog->_LinkedShaders[MESA_SHADER_TESS_CTRL];
   int param_count = nir->num_uniforms;
   if (!compiler->scalar_stage[MESA_SHADER_TESS_CTRL])
      param_count *= 4;

   prog_data.base.base.param =
      rzalloc_array(NULL, const gl_constant_value *, param_count);
   prog_data.base.base.pull_param =
      rzalloc_array(NULL, const gl_constant_value *, param_count);
   prog_data.base.base.image_param =
      rzalloc_array(NULL, struct brw_image_param, tcs->NumImages);
   prog_data.base.base.nr_params = param_count;
   prog_data.base.base.nr_image_params = tcs->NumImages;

   brw_nir_setup_glsl_uniforms(nir, shader_prog, &tcp->program.Base,
                               &prog_data.base.base, false);

   if (unlikely(INTEL_DEBUG & DEBUG_TCS))
      brw_dump_ir("tessellation control", shader_prog, tcs, NULL);

   int st_index = -1;
   if (unlikely(INTEL_DEBUG & DEBUG_SHADER_TIME))
      st_index = brw_get_shader_time_index(brw, shader_prog, NULL, ST_TCS);

   if (unlikely(brw->perf_debug)) {
      start_busy = brw->batch.last_bo && drm_intel_bo_busy(brw->batch.last_bo);
      start_time = get_time();
   }

   void *mem_ctx = ralloc_context(NULL);
   unsigned program_size;
   char *error_str;
   const unsigned *program =
      brw_compile_tcs(compiler, brw, mem_ctx, key, &prog_data, nir, st_index,
                      &program_size, &error_str);
   if (program == NULL) {
      if (shader_prog) {
         shader_prog->LinkStatus = false;
         ralloc_strcat(&shader_prog->InfoLog, error_str);
      }

      _mesa_problem(NULL, "Failed to compile tessellation control shader: "
                    "%s\n", error_str);

      ralloc_free(mem_ctx);
      return false;
   }

   if (unlikely(brw->perf_debug)) {
      struct brw_shader *btcs = (struct brw_shader *) tcs;
      if (btcs->compiled_once) {
         brw_tcs_debug_recompile(brw, shader_prog, key);
      }
      if (start_busy && !drm_intel_bo_busy(brw->batch.last_bo)) {
         perf_debug("TCS compile took %.03f ms and stalled the GPU\n",
                    (get_time() - start_time) * 1000);
      }
      btcs->compiled_once = true;
   }

   /* Scratch space is used for register spilling */
   if (prog_data.base.base.total_scratch) {
      brw_get_scratch_bo(brw, &stage_state->scratch_bo,
			 prog_data.base.base.total_scratch *
                         brw->max_hs_threads);
   }

   brw_upload_cache(&brw->cache, BRW_CACHE_TCS_PROG,
                    key, sizeof(*key),
                    program, program_size,
                    &prog_data, sizeof(prog_data),
                    &stage_state->prog_offset, &brw->tcs.prog_data);
   ralloc_free(mem_ctx);

   return true;
}


void
brw_upload_tcs_prog(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   struct gl_shader_program **current = ctx->_Shader->CurrentProgram;
   struct brw_stage_state *stage_state = &brw->tcs.base;
   struct brw_tcs_prog_key key;
   /* BRW_NEW_TESS_CTRL_PROGRAM */
   struct brw_tess_ctrl_program *tcp =
      (struct brw_tess_ctrl_program *) brw->tess_ctrl_program;

   if (!brw_state_dirty(brw,
                        _NEW_TEXTURE,
                        BRW_NEW_PATCH_PRIMITIVE |
                        BRW_NEW_TESS_CTRL_PROGRAM |
                        BRW_NEW_TESS_EVAL_PROGRAM))
      return;

   if (tcp == NULL) {
      /* Other state atoms had better not try to access prog_data, since
       * there's no HS program.
       */
      brw->tcs.prog_data = NULL;
      brw->tcs.base.prog_data = NULL;
      return;
   }

   struct gl_program *prog = &tcp->program.Base;

   memset(&key, 0, sizeof(key));

   key.program_string_id = tcp->id;

   key.input_vertices = ctx->TessCtrlProgram.patch_vertices;

   /* _NEW_TEXTURE */
   brw_populate_sampler_prog_key_data(ctx, prog, stage_state->sampler_count,
                                      &key.tex);

   /* BRW_NEW_TESS_EVAL_PROGRAM */
   /* We need to specialize our code generation for tessellation levels
    * based on the domain the DS is expecting to tessellate.
    */
   struct brw_tess_eval_program *tep =
      (struct brw_tess_eval_program *) brw->tess_eval_program;
   assert(tep);
   key.tes_primitive_mode = tep->program.PrimitiveMode;

   if (!brw_search_cache(&brw->cache, BRW_CACHE_TCS_PROG,
                         &key, sizeof(key),
                         &stage_state->prog_offset, &brw->tcs.prog_data)) {
      bool success = brw_codegen_tcs_prog(brw, current[MESA_SHADER_TESS_CTRL],
                                          tcp, &key);
      assert(success);
      (void)success;
   }
   brw->tcs.base.prog_data = &brw->tcs.prog_data->base.base;
}


bool
brw_tcs_precompile(struct gl_context *ctx,
                   struct gl_shader_program *shader_prog,
                   struct gl_program *prog)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_tcs_prog_key key;
   uint32_t old_prog_offset = brw->tcs.base.prog_offset;
   struct brw_tcs_prog_data *old_prog_data = brw->tcs.prog_data;
   bool success;

   struct gl_tess_ctrl_program *tcp = (struct gl_tess_ctrl_program *)prog;
   struct brw_tess_ctrl_program *btcp = brw_tess_ctrl_program(tcp);

   memset(&key, 0, sizeof(key));

   key.program_string_id = btcp->id;
   brw_setup_tex_for_precompile(brw, &key.tex, prog);

   /* Guess that the input and output patches have the same dimensionality. */
   key.input_vertices = shader_prog->TessCtrl.VerticesOut;

   key.tes_primitive_mode = GL_TRIANGLES;

   success = brw_codegen_tcs_prog(brw, shader_prog, btcp, &key);

   brw->tcs.base.prog_offset = old_prog_offset;
   brw->tcs.prog_data = old_prog_data;

   return success;
}
