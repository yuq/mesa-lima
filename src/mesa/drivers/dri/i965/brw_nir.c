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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "brw_nir.h"
#include "brw_shader.h"
#include "glsl/glsl_parser_extras.h"
#include "glsl/nir/glsl_to_nir.h"
#include "program/prog_to_nir.h"

static void
brw_nir_lower_inputs(nir_shader *nir, bool is_scalar)
{
   switch (nir->stage) {
   case MESA_SHADER_GEOMETRY:
      foreach_list_typed(nir_variable, var, node, &nir->inputs) {
         var->data.driver_location = var->data.location;
      }
      break;
   default:
      nir_assign_var_locations(&nir->inputs, &nir->num_inputs,
                               is_scalar ? type_size_scalar : type_size_vec4);
      break;
   }
}

static void
brw_nir_lower_outputs(nir_shader *nir, bool is_scalar)
{
   if (is_scalar) {
      nir_assign_var_locations(&nir->outputs, &nir->num_outputs, type_size_scalar);
   } else {
      nir_foreach_variable(var, &nir->outputs)
         var->data.driver_location = var->data.location;
   }
}

static void
nir_optimize(nir_shader *nir, bool is_scalar)
{
   bool progress;
   do {
      progress = false;
      nir_lower_vars_to_ssa(nir);
      nir_validate_shader(nir);

      if (is_scalar) {
         nir_lower_alu_to_scalar(nir);
         nir_validate_shader(nir);
      }

      progress |= nir_copy_prop(nir);
      nir_validate_shader(nir);

      if (is_scalar) {
         nir_lower_phis_to_scalar(nir);
         nir_validate_shader(nir);
      }

      progress |= nir_copy_prop(nir);
      nir_validate_shader(nir);
      progress |= nir_opt_dce(nir);
      nir_validate_shader(nir);
      progress |= nir_opt_cse(nir);
      nir_validate_shader(nir);
      progress |= nir_opt_peephole_select(nir);
      nir_validate_shader(nir);
      progress |= nir_opt_algebraic(nir);
      nir_validate_shader(nir);
      progress |= nir_opt_constant_folding(nir);
      nir_validate_shader(nir);
      progress |= nir_opt_dead_cf(nir);
      nir_validate_shader(nir);
      progress |= nir_opt_remove_phis(nir);
      nir_validate_shader(nir);
      progress |= nir_opt_undef(nir);
      nir_validate_shader(nir);
   } while (progress);
}

nir_shader *
brw_create_nir(struct brw_context *brw,
               const struct gl_shader_program *shader_prog,
               const struct gl_program *prog,
               gl_shader_stage stage,
               bool is_scalar)
{
   struct gl_context *ctx = &brw->ctx;
   const nir_shader_compiler_options *options =
      ctx->Const.ShaderCompilerOptions[stage].NirOptions;
   nir_shader *nir;

   /* First, lower the GLSL IR or Mesa IR to NIR */
   if (shader_prog) {
      nir = glsl_to_nir(shader_prog, stage, options);
   } else {
      nir = prog_to_nir(prog, options);
      nir_convert_to_ssa(nir); /* turn registers into SSA */
   }
   nir_validate_shader(nir);

   brw_process_nir(nir, brw->intelScreen->devinfo, shader_prog, stage, is_scalar);

   static GLuint msg_id = 0;
   _mesa_gl_debug(&brw->ctx, &msg_id,
                  MESA_DEBUG_SOURCE_SHADER_COMPILER,
                  MESA_DEBUG_TYPE_OTHER,
                  MESA_DEBUG_SEVERITY_NOTIFICATION,
                  "%s NIR shader:\n",
                  _mesa_shader_stage_to_abbrev(stage));

   return nir;
}

void
brw_process_nir(nir_shader *nir,
                const struct brw_device_info *devinfo,
                const struct gl_shader_program *shader_prog,
                gl_shader_stage stage, bool is_scalar)
{
   bool debug_enabled = INTEL_DEBUG & intel_debug_flag_for_shader_stage(stage);
   static const nir_lower_tex_options tex_options = {
      .lower_txp = ~0,
   };

   if (stage == MESA_SHADER_GEOMETRY) {
      nir_lower_gs_intrinsics(nir);
      nir_validate_shader(nir);
   }

   nir_lower_global_vars_to_local(nir);
   nir_validate_shader(nir);

   nir_lower_tex(nir, &tex_options);
   nir_validate_shader(nir);

   nir_normalize_cubemap_coords(nir);
   nir_validate_shader(nir);

   nir_split_var_copies(nir);
   nir_validate_shader(nir);

   nir_optimize(nir, is_scalar);

   /* Lower a bunch of stuff */
   nir_lower_var_copies(nir);
   nir_validate_shader(nir);

   /* Get rid of split copies */
   nir_optimize(nir, is_scalar);

   brw_nir_lower_inputs(nir, is_scalar);
   brw_nir_lower_outputs(nir, is_scalar);
   nir_assign_var_locations(&nir->uniforms,
                            &nir->num_uniforms,
                            is_scalar ? type_size_scalar : type_size_vec4);
   nir_lower_io(nir, -1, is_scalar ? type_size_scalar : type_size_vec4);
   nir_validate_shader(nir);

   nir_remove_dead_variables(nir);
   nir_validate_shader(nir);

   if (shader_prog) {
      nir_lower_samplers(nir, shader_prog);
   } else {
      nir_lower_samplers_for_vk(nir);
   }
   nir_validate_shader(nir);

   nir_lower_system_values(nir);
   nir_validate_shader(nir);

   nir_lower_atomics(nir);
   nir_validate_shader(nir);

   nir_optimize(nir, is_scalar);

   if (devinfo->gen >= 6) {
      /* Try and fuse multiply-adds */
      nir_opt_peephole_ffma(nir);
      nir_validate_shader(nir);
   }

   nir_opt_algebraic_late(nir);
   nir_validate_shader(nir);

   nir_lower_locals_to_regs(nir);
   nir_validate_shader(nir);

   nir_lower_to_source_mods(nir);
   nir_validate_shader(nir);
   nir_copy_prop(nir);
   nir_validate_shader(nir);
   nir_opt_dce(nir);
   nir_validate_shader(nir);

   if (unlikely(debug_enabled)) {
      /* Re-index SSA defs so we print more sensible numbers. */
      nir_foreach_overload(nir, overload) {
         if (overload->impl)
            nir_index_ssa_defs(overload->impl);
      }

      fprintf(stderr, "NIR (SSA form) for %s shader:\n",
              _mesa_shader_stage_to_string(stage));
      nir_print_shader(nir, stderr);
   }

   nir_convert_from_ssa(nir, true);
   nir_validate_shader(nir);

   if (!is_scalar) {
      nir_move_vec_src_uses_to_dest(nir);
      nir_validate_shader(nir);

      nir_lower_vec_to_movs(nir);
      nir_validate_shader(nir);
   }

   /* This is the last pass we run before we start emitting stuff.  It
    * determines when we need to insert boolean resolves on Gen <= 5.  We
    * run it last because it stashes data in instr->pass_flags and we don't
    * want that to be squashed by other NIR passes.
    */
   if (devinfo->gen <= 5)
      brw_nir_analyze_boolean_resolves(nir);

   nir_sweep(nir);

   if (unlikely(debug_enabled)) {
      fprintf(stderr, "NIR (final form) for %s shader:\n",
              _mesa_shader_stage_to_string(stage));
      nir_print_shader(nir, stderr);
   }
}

enum brw_reg_type
brw_type_for_nir_type(nir_alu_type type)
{
   switch (type) {
   case nir_type_unsigned:
      return BRW_REGISTER_TYPE_UD;
   case nir_type_bool:
   case nir_type_int:
      return BRW_REGISTER_TYPE_D;
   case nir_type_float:
      return BRW_REGISTER_TYPE_F;
   default:
      unreachable("unknown type");
   }

   return BRW_REGISTER_TYPE_F;
}

/* Returns the glsl_base_type corresponding to a nir_alu_type.
 * This is used by both brw_vec4_nir and brw_fs_nir.
 */
enum glsl_base_type
brw_glsl_base_type_for_nir_type(nir_alu_type type)
{
   switch (type) {
   case nir_type_float:
      return GLSL_TYPE_FLOAT;

   case nir_type_int:
      return GLSL_TYPE_INT;

   case nir_type_unsigned:
      return GLSL_TYPE_UINT;

   default:
      unreachable("bad type");
   }
}
