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

#include "anv_nir.h"
#include "glsl/nir/nir_builder.h"
#include "util/ralloc.h"

/* This file includes NIR helpers used by meta shaders in the Vulkan
 * driver.  Eventually, these will all be merged into nir_builder.
 * However, for now, keeping them in their own file helps to prevent merge
 * conflicts.
 */

static inline void
nir_builder_init_simple_shader(nir_builder *b, gl_shader_stage stage)
{
   b->shader = nir_shader_create(NULL, stage, NULL);

   nir_function *func = nir_function_create(b->shader,
                                            ralloc_strdup(b->shader, "main"));
   nir_function_overload *overload = nir_function_overload_create(func);
   overload->num_params = 0;

   b->impl = nir_function_impl_create(overload);
   b->cursor = nir_after_cf_list(&b->impl->body);
}

static inline void
nir_copy_var(nir_builder *build, nir_variable *dest, nir_variable *src)
{
   nir_intrinsic_instr *copy =
      nir_intrinsic_instr_create(build->shader, nir_intrinsic_copy_var);
   copy->variables[0] = nir_deref_var_create(copy, dest);
   copy->variables[1] = nir_deref_var_create(copy, src);
   nir_builder_instr_insert(build, &copy->instr);
}
