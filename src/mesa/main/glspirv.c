/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "glspirv.h"

#include "errors.h"

#include "util/u_atomic.h"

void
_mesa_spirv_module_reference(struct gl_spirv_module **dest,
                             struct gl_spirv_module *src)
{
   struct gl_spirv_module *old = *dest;

   if (old && p_atomic_dec_zero(&old->RefCount))
      free(old);

   *dest = src;

   if (src)
      p_atomic_inc(&src->RefCount);
}

void
_mesa_shader_spirv_data_reference(struct gl_shader_spirv_data **dest,
                                  struct gl_shader_spirv_data *src)
{
   struct gl_shader_spirv_data *old = *dest;

   if (old && p_atomic_dec_zero(&old->RefCount)) {
      _mesa_spirv_module_reference(&(*dest)->SpirVModule, NULL);
      ralloc_free(old);
   }

   *dest = src;

   if (src)
      p_atomic_inc(&src->RefCount);
}

void GLAPIENTRY
_mesa_SpecializeShaderARB(GLuint shader,
                          const GLchar *pEntryPoint,
                          GLuint numSpecializationConstants,
                          const GLuint *pConstantIndex,
                          const GLuint *pConstantValue)
{
   GET_CURRENT_CONTEXT(ctx);

   /* Just return GL_INVALID_OPERATION error while this is boilerplate */
   _mesa_error(ctx, GL_INVALID_OPERATION, "SpecializeShaderARB");
}
