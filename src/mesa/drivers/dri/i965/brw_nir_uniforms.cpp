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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "brw_shader.h"
#include "brw_nir.h"

void
brw_nir_setup_arb_uniforms(nir_shader *shader, struct gl_program *prog,
                           struct brw_stage_prog_data *stage_prog_data)
{
   struct gl_program_parameter_list *plist = prog->Parameters;

#ifndef NDEBUG
   if (!shader->uniforms.is_empty()) {
      /* For ARB programs, only a single "parameters" variable is generated to
       * support uniform data.
       */
      assert(shader->uniforms.length() == 1);
      nir_variable *var = (nir_variable *) shader->uniforms.get_head();
      assert(strcmp(var->name, "parameters") == 0);
      assert(var->type->array_size() == (int)plist->NumParameters);
   }
#endif

   for (unsigned p = 0; p < plist->NumParameters; p++) {
      /* Parameters should be either vec4 uniforms or single component
       * constants; matrices and other larger types should have been broken
       * down earlier.
       */
      assert(plist->Parameters[p].Size <= 4);

      unsigned i;
      for (i = 0; i < plist->Parameters[p].Size; i++) {
         stage_prog_data->param[4 * p + i] = &plist->ParameterValues[p][i];
      }
      for (; i < 4; i++) {
         static const gl_constant_value zero = { 0.0 };
         stage_prog_data->param[4 * p + i] = &zero;
      }
   }
}
