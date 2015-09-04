/*
 * Copyright (c) 2014 - 2015 Intel Corporation
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

#include "util/ralloc.h"
#include "brw_context.h"
#include "brw_cs.h"
#include "brw_eu.h"
#include "brw_wm.h"
#include "brw_shader.h"
#include "intel_mipmap_tree.h"
#include "brw_state.h"
#include "intel_batchbuffer.h"

bool
brw_cs_prog_data_compare(const void *in_a, const void *in_b)
{
   const struct brw_cs_prog_data *a =
      (const struct brw_cs_prog_data *)in_a;
   const struct brw_cs_prog_data *b =
      (const struct brw_cs_prog_data *)in_b;

   /* Compare the base structure. */
   if (!brw_stage_prog_data_compare(&a->base, &b->base))
      return false;

   /* Compare the rest of the structure. */
   const unsigned offset = sizeof(struct brw_stage_prog_data);
   if (memcmp(((char *) a) + offset, ((char *) b) + offset,
              sizeof(struct brw_cs_prog_data) - offset))
      return false;

   return true;
}

static bool
brw_codegen_cs_prog(struct brw_context *brw,
                    struct gl_shader_program *prog,
                    struct brw_compute_program *cp,
                    struct brw_cs_prog_key *key)
{
   struct gl_context *ctx = &brw->ctx;
   const GLuint *program;
   void *mem_ctx = ralloc_context(NULL);
   GLuint program_size;
   struct brw_cs_prog_data prog_data;

   struct gl_shader *cs = prog->_LinkedShaders[MESA_SHADER_COMPUTE];
   assert (cs);

   memset(&prog_data, 0, sizeof(prog_data));

   /* Allocate the references to the uniforms that will end up in the
    * prog_data associated with the compiled program, and which will be freed
    * by the state cache.
    */
   int param_count = cs->num_uniform_components +
                     cs->NumImages * BRW_IMAGE_PARAM_SIZE;

   /* The backend also sometimes adds params for texture size. */
   param_count += 2 * ctx->Const.Program[MESA_SHADER_COMPUTE].MaxTextureImageUnits;
   prog_data.base.param =
      rzalloc_array(NULL, const gl_constant_value *, param_count);
   prog_data.base.pull_param =
      rzalloc_array(NULL, const gl_constant_value *, param_count);
   prog_data.base.image_param =
      rzalloc_array(NULL, struct brw_image_param, cs->NumImages);
   prog_data.base.nr_params = param_count;
   prog_data.base.nr_image_params = cs->NumImages;

   program = brw_cs_emit(brw, mem_ctx, key, &prog_data,
                         &cp->program, prog, &program_size);
   if (program == NULL) {
      ralloc_free(mem_ctx);
      return false;
   }

   if (prog_data.base.total_scratch) {
      brw_get_scratch_bo(brw, &brw->cs.base.scratch_bo,
                         prog_data.base.total_scratch * brw->max_cs_threads);
   }

   if (unlikely(INTEL_DEBUG & DEBUG_CS))
      fprintf(stderr, "\n");

   brw_upload_cache(&brw->cache, BRW_CACHE_CS_PROG,
                    key, sizeof(*key),
                    program, program_size,
                    &prog_data, sizeof(prog_data),
                    &brw->cs.base.prog_offset, &brw->cs.prog_data);
   ralloc_free(mem_ctx);

   return true;
}


static void
brw_cs_populate_key(struct brw_context *brw, struct brw_cs_prog_key *key)
{
   struct gl_context *ctx = &brw->ctx;
   /* BRW_NEW_COMPUTE_PROGRAM */
   const struct brw_compute_program *cp =
      (struct brw_compute_program *) brw->compute_program;
   const struct gl_program *prog = (struct gl_program *) cp;

   memset(key, 0, sizeof(*key));

   /* _NEW_TEXTURE */
   brw_populate_sampler_prog_key_data(ctx, prog, brw->cs.base.sampler_count,
                                      &key->tex);

   /* The unique compute program ID */
   key->program_string_id = cp->id;
}


void
brw_upload_cs_prog(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   struct brw_cs_prog_key key;
   struct brw_compute_program *cp = (struct brw_compute_program *)
      brw->compute_program;

   if (!cp)
      return;

   if (!brw_state_dirty(brw, _NEW_TEXTURE, BRW_NEW_COMPUTE_PROGRAM))
      return;

   brw->cs.base.sampler_count =
      _mesa_fls(ctx->ComputeProgram._Current->Base.SamplersUsed);

   brw_cs_populate_key(brw, &key);

   if (!brw_search_cache(&brw->cache, BRW_CACHE_CS_PROG,
                         &key, sizeof(key),
                         &brw->cs.base.prog_offset, &brw->cs.prog_data)) {
      bool success =
         brw_codegen_cs_prog(brw,
                             ctx->Shader.CurrentProgram[MESA_SHADER_COMPUTE],
                             cp, &key);
      (void) success;
      assert(success);
   }
   brw->cs.base.prog_data = &brw->cs.prog_data->base;
}


bool
brw_cs_precompile(struct gl_context *ctx,
                  struct gl_shader_program *shader_prog,
                  struct gl_program *prog)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_cs_prog_key key;

   struct gl_compute_program *cp = (struct gl_compute_program *) prog;
   struct brw_compute_program *bcp = brw_compute_program(cp);

   memset(&key, 0, sizeof(key));
   key.program_string_id = bcp->id;

   brw_setup_tex_for_precompile(brw, &key.tex, prog);

   uint32_t old_prog_offset = brw->cs.base.prog_offset;
   struct brw_cs_prog_data *old_prog_data = brw->cs.prog_data;

   bool success = brw_codegen_cs_prog(brw, shader_prog, bcp, &key);

   brw->cs.base.prog_offset = old_prog_offset;
   brw->cs.prog_data = old_prog_data;

   return success;
}
