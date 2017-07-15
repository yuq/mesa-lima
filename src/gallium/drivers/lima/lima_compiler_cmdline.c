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

#include "ir/gp/gpir.h"

static void
print_usage(void)
{
   printf("Usage: lima_compiler [OPTIONS]... FILE\n");
   printf("    --help            - show this message\n");
}

static const nir_shader_compiler_options nir_options = {
   .lower_fpow = true,
   .lower_ffract = true,
   .lower_fdiv = true,
   .lower_fsqrt = true,
};

static void
lima_optimize_nir(struct nir_shader *s)
{
   bool progress;

   do {
      progress = false;

      NIR_PASS_V(s, nir_lower_vars_to_ssa);
      NIR_PASS(progress, s, nir_lower_alu_to_scalar);
      NIR_PASS(progress, s, nir_lower_phis_to_scalar);
      NIR_PASS(progress, s, nir_copy_prop);
      NIR_PASS(progress, s, nir_opt_remove_phis);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_dead_cf);
      NIR_PASS(progress, s, nir_opt_cse);
      NIR_PASS(progress, s, nir_opt_peephole_select, 8);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_undef);
      NIR_PASS(progress, s, nir_opt_loop_unroll,
               nir_var_shader_in |
               nir_var_shader_out |
               nir_var_local);
   } while (progress);
}

#define PRINT_NIR 0

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
   unsigned stage = 0;

   if (!strcmp(ext, ".frag"))
      stage = MESA_SHADER_FRAGMENT;
   else if (!strcmp(ext, ".vert"))
      stage = MESA_SHADER_VERTEX;
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

   nir_shader *nir = glsl_to_nir(prog, stage, &nir_options);

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

   NIR_PASS_V(nir, st_nir_lower_builtin);
   printf("\nst_nir_lower_builtin\n");
   //nir_print_shader(nir, stdout);
//*/
/*
   NIR_PASS_V(nir, nir_lower_io_to_scalar,
              nir_var_shader_out);
   printf("\nnir_lower_io_to_scalar\n");
   //nir_print_shader(nir, stdout);
//*/

   lima_optimize_nir(nir);
   printf("\nlima_optimize_nir\n");
   //nir_print_shader(nir, stdout);

   NIR_PASS_V(nir, nir_lower_locals_to_regs);
   printf("\nnir_lower_locals_to_regs\n");
   //nir_print_shader(nir, stdout);

   NIR_PASS_V(nir, nir_convert_from_ssa, true);
   printf("\nnir_convert_from_ssa\n");
   //nir_print_shader(nir, stdout);
/*
   NIR_PASS_V(nir, nir_move_vec_src_uses_to_dest);
   printf("\nnir_move_vec_src_uses_to_dest\n");
   //nir_print_shader(nir, stdout);

   NIR_PASS_V(nir, nir_lower_vec_to_movs);
   printf("\nnir_lower_vec_to_movs\n");
   //nir_print_shader(nir, stdout);
//*/

   nir_print_shader(nir, stdout);

   gpir_prog *gpir = nir_to_gpir(nir);
   if (gpir) {
      printf("convert to gpir\n");
   }

   ralloc_free(nir);
   return 0;
}
