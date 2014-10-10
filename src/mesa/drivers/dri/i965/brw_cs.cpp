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
#include "brw_fs.h"
#include "brw_eu.h"
#include "brw_wm.h"
#include "intel_mipmap_tree.h"
#include "brw_state.h"
#include "intel_batchbuffer.h"

extern "C"
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


static const unsigned *
brw_cs_emit(struct brw_context *brw,
            void *mem_ctx,
            const struct brw_cs_prog_key *key,
            struct brw_cs_prog_data *prog_data,
            struct gl_compute_program *cp,
            struct gl_shader_program *prog,
            unsigned *final_assembly_size)
{
   bool start_busy = false;
   double start_time = 0;

   if (unlikely(brw->perf_debug)) {
      start_busy = (brw->batch.last_bo &&
                    drm_intel_bo_busy(brw->batch.last_bo));
      start_time = get_time();
   }

   struct brw_shader *shader =
      (struct brw_shader *) prog->_LinkedShaders[MESA_SHADER_COMPUTE];

   if (unlikely(INTEL_DEBUG & DEBUG_CS))
      brw_dump_ir("compute", prog, &shader->base, &cp->Base);

   prog_data->local_size[0] = cp->LocalSize[0];
   prog_data->local_size[1] = cp->LocalSize[1];
   prog_data->local_size[2] = cp->LocalSize[2];
   unsigned local_workgroup_size =
      cp->LocalSize[0] * cp->LocalSize[1] * cp->LocalSize[2];

   cfg_t *cfg = NULL;
   const char *fail_msg = NULL;

   int st_index = -1;
   if (INTEL_DEBUG & DEBUG_SHADER_TIME)
      st_index = brw_get_shader_time_index(brw, prog, &cp->Base, ST_CS);

   /* Now the main event: Visit the shader IR and generate our CS IR for it.
    */
   fs_visitor v8(brw->intelScreen->compiler, brw,
                 mem_ctx, MESA_SHADER_COMPUTE, key, &prog_data->base, prog,
                 &cp->Base, 8, st_index);
   if (!v8.run_cs()) {
      fail_msg = v8.fail_msg;
   } else if (local_workgroup_size <= 8 * brw->max_cs_threads) {
      cfg = v8.cfg;
      prog_data->simd_size = 8;
   }

   fs_visitor v16(brw->intelScreen->compiler, brw,
                  mem_ctx, MESA_SHADER_COMPUTE, key, &prog_data->base, prog,
                  &cp->Base, 16, st_index);
   if (likely(!(INTEL_DEBUG & DEBUG_NO16)) &&
       !fail_msg && !v8.simd16_unsupported &&
       local_workgroup_size <= 16 * brw->max_cs_threads) {
      /* Try a SIMD16 compile */
      v16.import_uniforms(&v8);
      if (!v16.run_cs()) {
         perf_debug("SIMD16 shader failed to compile: %s", v16.fail_msg);
         if (!cfg) {
            fail_msg =
               "Couldn't generate SIMD16 program and not "
               "enough threads for SIMD8";
         }
      } else {
         cfg = v16.cfg;
         prog_data->simd_size = 16;
      }
   }

   if (unlikely(cfg == NULL)) {
      assert(fail_msg);
      prog->LinkStatus = false;
      ralloc_strcat(&prog->InfoLog, fail_msg);
      _mesa_problem(NULL, "Failed to compile compute shader: %s\n",
                    fail_msg);
      return NULL;
   }

   fs_generator g(brw->intelScreen->compiler, brw,
                  mem_ctx, (void*) key, &prog_data->base, &cp->Base,
                  v8.promoted_constants, v8.runtime_check_aads_emit, "CS");
   if (INTEL_DEBUG & DEBUG_CS) {
      char *name = ralloc_asprintf(mem_ctx, "%s compute shader %d",
                                   prog->Label ? prog->Label : "unnamed",
                                   prog->Name);
      g.enable_debug(name);
   }

   g.generate_code(cfg, prog_data->simd_size);

   if (unlikely(brw->perf_debug) && shader) {
      if (shader->compiled_once) {
         _mesa_problem(&brw->ctx, "CS programs shouldn't need recompiles");
      }
      shader->compiled_once = true;

      if (start_busy && !drm_intel_bo_busy(brw->batch.last_bo)) {
         perf_debug("CS compile took %.03f ms and stalled the GPU\n",
                    (get_time() - start_time) * 1000);
      }
   }

   return g.get_assembly(final_assembly_size);
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


extern "C"
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


extern "C" bool
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


static unsigned
get_cs_thread_count(const struct brw_cs_prog_data *cs_prog_data)
{
   const unsigned simd_size = cs_prog_data->simd_size;
   unsigned group_size = cs_prog_data->local_size[0] *
      cs_prog_data->local_size[1] * cs_prog_data->local_size[2];

   return (group_size + simd_size - 1) / simd_size;
}


static void
brw_upload_cs_state(struct brw_context *brw)
{
   if (!brw->cs.prog_data)
      return;

   uint32_t offset;
   uint32_t *desc = (uint32_t*) brw_state_batch(brw, AUB_TRACE_SURFACE_STATE,
                                                8 * 4, 64, &offset);
   struct gl_program *prog = (struct gl_program *) brw->compute_program;
   struct brw_stage_state *stage_state = &brw->cs.base;
   struct brw_cs_prog_data *cs_prog_data = brw->cs.prog_data;
   struct brw_stage_prog_data *prog_data = &cs_prog_data->base;

   if (INTEL_DEBUG & DEBUG_SHADER_TIME) {
      brw->vtbl.emit_buffer_surface_state(
         brw, &stage_state->surf_offset[
                 prog_data->binding_table.shader_time_start],
         brw->shader_time.bo, 0, BRW_SURFACEFORMAT_RAW,
         brw->shader_time.bo->size, 1, true);
   }

   uint32_t *bind = (uint32_t*) brw_state_batch(brw, AUB_TRACE_BINDING_TABLE,
                                            prog_data->binding_table.size_bytes,
                                            32, &stage_state->bind_bo_offset);

   unsigned local_id_dwords = 0;

   if (prog->SystemValuesRead & SYSTEM_BIT_LOCAL_INVOCATION_ID) {
      local_id_dwords =
         brw_cs_prog_local_id_payload_dwords(prog, cs_prog_data->simd_size);
   }

   unsigned push_constant_data_size =
      (prog_data->nr_params + local_id_dwords) * sizeof(gl_constant_value);
   unsigned reg_aligned_constant_size = ALIGN(push_constant_data_size, 32);
   unsigned push_constant_regs = reg_aligned_constant_size / 32;
   unsigned threads = get_cs_thread_count(cs_prog_data);

   uint32_t dwords = brw->gen < 8 ? 8 : 9;
   BEGIN_BATCH(dwords);
   OUT_BATCH(MEDIA_VFE_STATE << 16 | (dwords - 2));

   if (prog_data->total_scratch) {
      if (brw->gen >= 8)
         OUT_RELOC64(stage_state->scratch_bo,
                     I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
                     ffs(prog_data->total_scratch) - 11);
      else
         OUT_RELOC(stage_state->scratch_bo,
                   I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
                   ffs(prog_data->total_scratch) - 11);
   } else {
      OUT_BATCH(0);
      if (brw->gen >= 8)
         OUT_BATCH(0);
   }

   const uint32_t vfe_num_urb_entries = brw->gen >= 8 ? 2 : 0;
   const uint32_t vfe_gpgpu_mode =
      brw->gen == 7 ? SET_FIELD(1, GEN7_MEDIA_VFE_STATE_GPGPU_MODE) : 0;
   OUT_BATCH(SET_FIELD(brw->max_cs_threads - 1, MEDIA_VFE_STATE_MAX_THREADS) |
             SET_FIELD(vfe_num_urb_entries, MEDIA_VFE_STATE_URB_ENTRIES) |
             SET_FIELD(1, MEDIA_VFE_STATE_RESET_GTW_TIMER) |
             SET_FIELD(1, MEDIA_VFE_STATE_BYPASS_GTW) |
             vfe_gpgpu_mode);

   OUT_BATCH(0);
   const uint32_t vfe_urb_allocation = brw->gen >= 8 ? 2 : 0;

   /* We are uploading duplicated copies of push constant uniforms for each
    * thread. Although the local id data needs to vary per thread, it won't
    * change for other uniform data. Unfortunately this duplication is
    * required for gen7. As of Haswell, this duplication can be avoided, but
    * this older mechanism with duplicated data continues to work.
    *
    * FINISHME: As of Haswell, we could make use of the
    * INTERFACE_DESCRIPTOR_DATA "Cross-Thread Constant Data Read Length" field
    * to only store one copy of uniform data.
    *
    * FINISHME: Broadwell adds a new alternative "Indirect Payload Storage"
    * which is described in the GPGPU_WALKER command and in the Broadwell PRM
    * Volume 7: 3D Media GPGPU, under Media GPGPU Pipeline => Mode of
    * Operations => GPGPU Mode => Indirect Payload Storage.
    *
    * Note: The constant data is built in brw_upload_cs_push_constants below.
    */
   const uint32_t vfe_curbe_allocation = push_constant_regs * threads;
   OUT_BATCH(SET_FIELD(vfe_urb_allocation, MEDIA_VFE_STATE_URB_ALLOC) |
             SET_FIELD(vfe_curbe_allocation, MEDIA_VFE_STATE_CURBE_ALLOC));
   OUT_BATCH(0);
   OUT_BATCH(0);
   OUT_BATCH(0);
   ADVANCE_BATCH();

   if (reg_aligned_constant_size > 0) {
      BEGIN_BATCH(4);
      OUT_BATCH(MEDIA_CURBE_LOAD << 16 | (4 - 2));
      OUT_BATCH(0);
      OUT_BATCH(reg_aligned_constant_size * threads);
      OUT_BATCH(stage_state->push_const_offset);
      ADVANCE_BATCH();
   }

   /* BRW_NEW_SURFACES and BRW_NEW_*_CONSTBUF */
   memcpy(bind, stage_state->surf_offset,
          prog_data->binding_table.size_bytes);

   memset(desc, 0, 8 * 4);

   int dw = 0;
   desc[dw++] = brw->cs.base.prog_offset;
   if (brw->gen >= 8)
      desc[dw++] = 0; /* Kernel Start Pointer High */
   desc[dw++] = 0;
   desc[dw++] = stage_state->sampler_offset |
      ((stage_state->sampler_count + 3) / 4);
   desc[dw++] = stage_state->bind_bo_offset;
   desc[dw++] = SET_FIELD(push_constant_regs, MEDIA_CURBE_READ_LENGTH);
   const uint32_t media_threads =
      brw->gen >= 8 ?
      SET_FIELD(threads, GEN8_MEDIA_GPGPU_THREAD_COUNT) :
      SET_FIELD(threads, MEDIA_GPGPU_THREAD_COUNT);
   assert(threads <= brw->max_cs_threads);
   desc[dw++] =
      SET_FIELD(cs_prog_data->uses_barrier, MEDIA_BARRIER_ENABLE) |
      media_threads;

   BEGIN_BATCH(4);
   OUT_BATCH(MEDIA_INTERFACE_DESCRIPTOR_LOAD << 16 | (4 - 2));
   OUT_BATCH(0);
   OUT_BATCH(8 * 4);
   OUT_BATCH(offset);
   ADVANCE_BATCH();
}


extern "C"
const struct brw_tracked_state brw_cs_state = {
   /* explicit initialisers aren't valid C++, comment
    * them for documentation purposes */
   /* .dirty = */{
      /* .mesa = */ _NEW_PROGRAM_CONSTANTS,
      /* .brw = */  BRW_NEW_CS_PROG_DATA |
                    BRW_NEW_PUSH_CONSTANT_ALLOCATION,
   },
   /* .emit = */ brw_upload_cs_state
};


/**
 * We are building the local ID push constant data using the simplest possible
 * method. We simply push the local IDs directly as they should appear in the
 * registers for the uvec3 gl_LocalInvocationID variable.
 *
 * Therefore, for SIMD8, we use 3 full registers, and for SIMD16 we use 6
 * registers worth of push constant space.
 *
 * Note: Any updates to brw_cs_prog_local_id_payload_dwords,
 * fill_local_id_payload or fs_visitor::emit_cs_local_invocation_id_setup need
 * to coordinated.
 *
 * FINISHME: There are a few easy optimizations to consider.
 *
 * 1. If gl_WorkGroupSize x, y or z is 1, we can just use zero, and there is
 *    no need for using push constant space for that dimension.
 *
 * 2. Since GL_MAX_COMPUTE_WORK_GROUP_SIZE is currently 1024 or less, we can
 *    easily use 16-bit words rather than 32-bit dwords in the push constant
 *    data.
 *
 * 3. If gl_WorkGroupSize x, y or z is small, then we can use bytes for
 *    conveying the data, and thereby reduce push constant usage.
 *
 */
unsigned
brw_cs_prog_local_id_payload_dwords(const struct gl_program *prog,
                                    unsigned dispatch_width)
{
   return 3 * dispatch_width;
}


static void
fill_local_id_payload(const struct brw_cs_prog_data *cs_prog_data,
                      void *buffer, unsigned *x, unsigned *y, unsigned *z)
{
   uint32_t *param = (uint32_t *)buffer;
   for (unsigned i = 0; i < cs_prog_data->simd_size; i++) {
      param[0 * cs_prog_data->simd_size + i] = *x;
      param[1 * cs_prog_data->simd_size + i] = *y;
      param[2 * cs_prog_data->simd_size + i] = *z;

      (*x)++;
      if (*x == cs_prog_data->local_size[0]) {
         *x = 0;
         (*y)++;
         if (*y == cs_prog_data->local_size[1]) {
            *y = 0;
            (*z)++;
            if (*z == cs_prog_data->local_size[2])
               *z = 0;
         }
      }
   }
}


fs_reg *
fs_visitor::emit_cs_local_invocation_id_setup()
{
   assert(stage == MESA_SHADER_COMPUTE);

   fs_reg *reg = new(this->mem_ctx) fs_reg(vgrf(glsl_type::uvec3_type));

   struct brw_reg src =
      brw_vec8_grf(payload.local_invocation_id_reg, 0);
   src = retype(src, BRW_REGISTER_TYPE_UD);
   bld.MOV(*reg, src);
   src.nr += dispatch_width / 8;
   bld.MOV(offset(*reg, bld, 1), src);
   src.nr += dispatch_width / 8;
   bld.MOV(offset(*reg, bld, 2), src);

   return reg;
}


/**
 * Creates a region containing the push constants for the CS on gen7+.
 *
 * Push constants are constant values (such as GLSL uniforms) that are
 * pre-loaded into a shader stage's register space at thread spawn time.
 *
 * For other stages, see brw_curbe.c:brw_upload_constant_buffer for the
 * equivalent gen4/5 code and gen6_vs_state.c:gen6_upload_push_constants for
 * gen6+.
 */
static void
brw_upload_cs_push_constants(struct brw_context *brw,
                             const struct gl_program *prog,
                             const struct brw_cs_prog_data *cs_prog_data,
                             struct brw_stage_state *stage_state,
                             enum aub_state_struct_type type)
{
   struct gl_context *ctx = &brw->ctx;
   const struct brw_stage_prog_data *prog_data =
      (brw_stage_prog_data*) cs_prog_data;
   unsigned local_id_dwords = 0;

   if (prog->SystemValuesRead & SYSTEM_BIT_LOCAL_INVOCATION_ID) {
      local_id_dwords =
         brw_cs_prog_local_id_payload_dwords(prog, cs_prog_data->simd_size);
   }

   /* Updates the ParamaterValues[i] pointers for all parameters of the
    * basic type of PROGRAM_STATE_VAR.
    */
   /* XXX: Should this happen somewhere before to get our state flag set? */
   _mesa_load_state_parameters(ctx, prog->Parameters);

   if (prog_data->nr_params == 0 && local_id_dwords == 0) {
      stage_state->push_const_size = 0;
   } else {
      gl_constant_value *param;
      unsigned i, t;

      const unsigned push_constant_data_size =
         (local_id_dwords + prog_data->nr_params) * sizeof(gl_constant_value);
      const unsigned reg_aligned_constant_size = ALIGN(push_constant_data_size, 32);
      const unsigned param_aligned_count =
         reg_aligned_constant_size / sizeof(*param);

      unsigned threads = get_cs_thread_count(cs_prog_data);

      param = (gl_constant_value*)
         brw_state_batch(brw, type,
                         reg_aligned_constant_size * threads,
                         32, &stage_state->push_const_offset);
      assert(param);

      STATIC_ASSERT(sizeof(gl_constant_value) == sizeof(float));

      /* _NEW_PROGRAM_CONSTANTS */
      unsigned x = 0, y = 0, z = 0;
      for (t = 0; t < threads; t++) {
         gl_constant_value *next_param = &param[t * param_aligned_count];
         if (local_id_dwords > 0) {
            fill_local_id_payload(cs_prog_data, (void*)next_param, &x, &y, &z);
            next_param += local_id_dwords;
         }
         for (i = 0; i < prog_data->nr_params; i++) {
            next_param[i] = *prog_data->param[i];
         }
      }

      stage_state->push_const_size = ALIGN(prog_data->nr_params, 8) / 8;
   }
}


static void
gen7_upload_cs_push_constants(struct brw_context *brw)
{
   struct brw_stage_state *stage_state = &brw->cs.base;

   /* BRW_NEW_COMPUTE_PROGRAM */
   const struct brw_compute_program *cp =
      (struct brw_compute_program *) brw->compute_program;

   if (cp) {
      /* CACHE_NEW_CS_PROG */
      struct brw_cs_prog_data *cs_prog_data = brw->cs.prog_data;

      brw_upload_cs_push_constants(brw, &cp->program.Base, cs_prog_data,
                                   stage_state, AUB_TRACE_WM_CONSTANTS);
   }
}


const struct brw_tracked_state gen7_cs_push_constants = {
   /* .dirty = */{
      /* .mesa  = */ _NEW_PROGRAM_CONSTANTS,
      /* .brw   = */ BRW_NEW_COMPUTE_PROGRAM |
                     BRW_NEW_PUSH_CONSTANT_ALLOCATION,
   },
   /* .emit = */ gen7_upload_cs_push_constants,
};


fs_reg *
fs_visitor::emit_cs_work_group_id_setup()
{
   assert(stage == MESA_SHADER_COMPUTE);

   fs_reg *reg = new(this->mem_ctx) fs_reg(vgrf(glsl_type::uvec3_type));

   struct brw_reg r0_1(retype(brw_vec1_grf(0, 1), BRW_REGISTER_TYPE_UD));
   struct brw_reg r0_6(retype(brw_vec1_grf(0, 6), BRW_REGISTER_TYPE_UD));
   struct brw_reg r0_7(retype(brw_vec1_grf(0, 7), BRW_REGISTER_TYPE_UD));

   bld.MOV(*reg, r0_1);
   bld.MOV(offset(*reg, bld, 1), r0_6);
   bld.MOV(offset(*reg, bld, 2), r0_7);

   return reg;
}
