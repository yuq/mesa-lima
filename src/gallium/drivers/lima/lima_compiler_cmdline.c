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

#include <err.h>
#include <stdio.h>
#include <string.h>

#include "compiler/glsl/standalone.h"
#include "compiler/glsl/glsl_to_nir.h"
#include "mesa/state_tracker/st_nir.h"
#include "mesa/state_tracker/st_glsl_types.h"

#include "lima_program.h"
#include "ir/gp/gpir.h"

static void
print_usage(void)
{
   printf("Usage: lima_compiler [OPTIONS]... FILE\n");
   printf("    --help            - show this message\n");
}

static void
insert_sorted(struct exec_list *var_list, nir_variable *new_var)
{
   nir_foreach_variable(var, var_list) {
      if (var->data.location > new_var->data.location) {
         exec_node_insert_node_before(&var->node, &new_var->node);
         return;
      }
   }
   exec_list_push_tail(var_list, &new_var->node);
}

static void
sort_varyings(struct exec_list *var_list)
{
   struct exec_list new_list;
   exec_list_make_empty(&new_list);
   nir_foreach_variable_safe(var, var_list) {
      exec_node_remove(&var->node);
      insert_sorted(&new_list, var);
   }
   exec_list_move_nodes_to(&new_list, var_list);
}

static void
fixup_varying_slots(struct exec_list *var_list)
{
   nir_foreach_variable(var, var_list) {
      if (var->data.location >= VARYING_SLOT_VAR0) {
         var->data.location += 9;
      } else if ((var->data.location >= VARYING_SLOT_TEX0) &&
                 (var->data.location <= VARYING_SLOT_TEX7)) {
         var->data.location += VARYING_SLOT_VAR0 - VARYING_SLOT_TEX0;
      }
   }
}

int
main(int argc, char **argv)
{
   int n = 1;

   while (n < argc) {
      if (!strcmp(argv[n], "--help")) {
         print_usage();
         return 0;
      }

      break;
   }

   char *filename[10] = {0};
   filename[0] = argv[n];

   char *ext = rindex(filename[0], '.');
   unsigned stage = 0, shader = 0;

   if (!strcmp(ext, ".frag")) {
      stage = MESA_SHADER_FRAGMENT;
      shader = PIPE_SHADER_FRAGMENT;
   }
   else if (!strcmp(ext, ".vert")) {
      stage = MESA_SHADER_VERTEX;
      shader = PIPE_SHADER_VERTEX;
   }
   else {
      print_usage();
      return -1;
   }

   static const struct standalone_options options = {
      .glsl_version = 100,
      .do_link = false,
   };
   struct gl_shader_program *prog;

   prog = standalone_compile_shader(&options, 1, filename);
   if (!prog)
      errx(1, "couldn't parse `%s'", filename[0]);

   nir_shader *nir = glsl_to_nir(prog, stage,
                                 lima_program_get_compiler_options(shader));

   standalone_compiler_cleanup(prog);

   //nir_print_shader(nir, stdout);

//* already in st_glsl_to_nir
   NIR_PASS_V(nir, nir_lower_io_to_temporaries,
              nir_shader_get_entrypoint(nir),
              true, true);
   printf("\nnir_lower_io_to_temporaries\n");
   //nir_print_shader(nir, stdout);

   NIR_PASS_V(nir, nir_lower_global_vars_to_local);
   printf("\nnir_lower_global_vars_to_local\n");
   //nir_print_shader(nir, stdout);

   NIR_PASS_V(nir, nir_split_var_copies);
   printf("\nnir_split_var_copies\n");
   //nir_print_shader(nir, stdout);

   NIR_PASS_V(nir, nir_lower_var_copies);
   printf("\nnir_lower_var_copies\n");
   //nir_print_shader(nir, stdout);

   //NIR_PASS_V(nir, st_nir_lower_builtin);
   //printf("\nst_nir_lower_builtin\n");
   //nir_print_shader(nir, stdout);
//*/
   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_lower_var_copies);
   NIR_PASS_V(nir, nir_lower_io_types);
   printf("\nnir_lower_io_types\n");
   //nir_print_shader(nir, stdout);

   switch (stage) {
   case MESA_SHADER_VERTEX:
      nir_assign_var_locations(&nir->inputs,
                               &nir->num_inputs,
                               st_glsl_type_size);

      /* Re-lower global vars, to deal with any dead VS inputs. */
      NIR_PASS_V(nir, nir_lower_global_vars_to_local);

      sort_varyings(&nir->outputs);
      nir_assign_var_locations(&nir->outputs,
                               &nir->num_outputs,
                               st_glsl_type_size);
      fixup_varying_slots(&nir->outputs);
      break;
   case MESA_SHADER_FRAGMENT:
      sort_varyings(&nir->inputs);
      nir_assign_var_locations(&nir->inputs,
                               &nir->num_inputs,
                               st_glsl_type_size);
      fixup_varying_slots(&nir->inputs);
      nir_assign_var_locations(&nir->outputs,
                               &nir->num_outputs,
                               st_glsl_type_size);
      break;
   default:
      errx(1, "unhandled shader stage: %d", stage);
   }
   printf("\nfixup_varying_slots\n");
   //nir_print_shader(nir, stdout);

   nir_assign_var_locations(&nir->uniforms,
                            &nir->num_uniforms,
                            st_glsl_type_size);
   printf("\nnir_assign_var_locations\n");
   //nir_print_shader(nir, stdout);

   NIR_PASS_V(nir, nir_lower_system_values);
   printf("\nnir_lower_system_values\n");
   //nir_print_shader(nir, stdout);

   NIR_PASS_V(nir, nir_lower_io, nir_var_all, st_glsl_type_size, 0);
   printf("\nnir_lower_io\n");
   //nir_print_shader(nir, stdout);

   NIR_PASS_V(nir, nir_lower_samplers, prog);
   printf("\nnir_lower_samplers\n");
   //nir_print_shader(nir, stdout);

/*
   NIR_PASS_V(nir, nir_lower_io_to_scalar,
              nir_var_shader_out);
   printf("\nnir_lower_io_to_scalar\n");
   //nir_print_shader(nir, stdout);
//*/

   lima_program_optimize_nir(nir);
   printf("\nlima_optimize_nir\n");
   //nir_print_shader(nir, stdout);

   nir_print_shader(nir, stdout);

   gpir_prog *gpir = nir_to_gpir(nir);
   if (gpir) {
      printf("convert to gpir\n");
   }

   ralloc_free(nir);
   return 0;
}
