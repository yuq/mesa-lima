/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2015 Intel Corporation.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "main/enums.h"
#include "main/macros.h"
#include "main/mtypes.h"
#include "main/shaderapi.h"
#include "main/shaderobj.h"
#include "program_resource.h"

static bool
supported_interface_enum(GLenum iface)
{
   switch (iface) {
   case GL_UNIFORM:
   case GL_UNIFORM_BLOCK:
   case GL_PROGRAM_INPUT:
   case GL_PROGRAM_OUTPUT:
   case GL_TRANSFORM_FEEDBACK_VARYING:
   case GL_ATOMIC_COUNTER_BUFFER:
      return true;
   case GL_VERTEX_SUBROUTINE:
   case GL_TESS_CONTROL_SUBROUTINE:
   case GL_TESS_EVALUATION_SUBROUTINE:
   case GL_GEOMETRY_SUBROUTINE:
   case GL_FRAGMENT_SUBROUTINE:
   case GL_COMPUTE_SUBROUTINE:
   case GL_VERTEX_SUBROUTINE_UNIFORM:
   case GL_TESS_CONTROL_SUBROUTINE_UNIFORM:
   case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM:
   case GL_GEOMETRY_SUBROUTINE_UNIFORM:
   case GL_FRAGMENT_SUBROUTINE_UNIFORM:
   case GL_COMPUTE_SUBROUTINE_UNIFORM:
   case GL_BUFFER_VARIABLE:
   case GL_SHADER_STORAGE_BLOCK:
   default:
      return false;
   }
}

void GLAPIENTRY
_mesa_GetProgramInterfaceiv(GLuint program, GLenum programInterface,
                            GLenum pname, GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   unsigned i;
   struct gl_shader_program *shProg =
      _mesa_lookup_shader_program_err(ctx, program,
                                      "glGetProgramInterfaceiv");
   if (!shProg)
      return;

   if (!params) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glGetProgramInterfaceiv(params NULL)");
      return;
   }

   /* Validate interface. */
   if (!supported_interface_enum(programInterface)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "glGetProgramInterfaceiv(%s)",
                  _mesa_lookup_enum_by_nr(programInterface));
      return;
   }

   /* Validate pname against interface. */
   switch(pname) {
   case GL_ACTIVE_RESOURCES:
      for (i = 0, *params = 0; i < shProg->NumProgramResourceList; i++)
         if (shProg->ProgramResourceList[i].Type == programInterface)
            (*params)++;
      break;
   case GL_MAX_NAME_LENGTH:
      if (programInterface == GL_ATOMIC_COUNTER_BUFFER) {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "glGetProgramInterfaceiv(%s pname %s)",
                     _mesa_lookup_enum_by_nr(programInterface),
                     _mesa_lookup_enum_by_nr(pname));
         return;
      }
      /* Name length consists of base name, 3 additional chars '[0]' if
       * resource is an array and finally 1 char for string terminator.
       */
      for (i = 0, *params = 0; i < shProg->NumProgramResourceList; i++) {
         if (shProg->ProgramResourceList[i].Type != programInterface)
            continue;
         const char *name =
            _mesa_program_resource_name(&shProg->ProgramResourceList[i]);
         unsigned array_size =
            _mesa_program_resource_array_size(&shProg->ProgramResourceList[i]);
         *params = MAX2(*params, strlen(name) + (array_size ? 3 : 0) + 1);
      }
      break;
   case GL_MAX_NUM_ACTIVE_VARIABLES:
      switch (programInterface) {
      case GL_UNIFORM_BLOCK:
         for (i = 0, *params = 0; i < shProg->NumProgramResourceList; i++) {
            if (shProg->ProgramResourceList[i].Type == programInterface) {
               struct gl_uniform_block *block =
                  (struct gl_uniform_block *)
                  shProg->ProgramResourceList[i].Data;
               *params = MAX2(*params, block->NumUniforms);
            }
         }
         break;
      case GL_ATOMIC_COUNTER_BUFFER:
         for (i = 0, *params = 0; i < shProg->NumProgramResourceList; i++) {
            if (shProg->ProgramResourceList[i].Type == programInterface) {
               struct gl_active_atomic_buffer *buffer =
                  (struct gl_active_atomic_buffer *)
                  shProg->ProgramResourceList[i].Data;
               *params = MAX2(*params, buffer->NumUniforms);
            }
         }
         break;
      default:
        _mesa_error(ctx, GL_INVALID_OPERATION,
                    "glGetProgramInterfaceiv(%s pname %s)",
                    _mesa_lookup_enum_by_nr(programInterface),
                    _mesa_lookup_enum_by_nr(pname));
      };
      break;
   case GL_MAX_NUM_COMPATIBLE_SUBROUTINES:
   default:
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glGetProgramInterfaceiv(pname %s)",
                  _mesa_lookup_enum_by_nr(pname));
   }
}

GLuint GLAPIENTRY
_mesa_GetProgramResourceIndex(GLuint program, GLenum programInterface,
                              const GLchar *name)
{
   return 0;
}

void GLAPIENTRY
_mesa_GetProgramResourceName(GLuint program, GLenum programInterface,
                             GLuint index, GLsizei bufSize, GLsizei *length,
                             GLchar *name)
{
}

void GLAPIENTRY
_mesa_GetProgramResourceiv(GLuint program, GLenum programInterface,
                           GLuint index, GLsizei propCount,
                           const GLenum *props, GLsizei bufSize,
                           GLsizei *length, GLint *params)
{
}

GLint GLAPIENTRY
_mesa_GetProgramResourceLocation(GLuint program, GLenum programInterface,
                                 const GLchar *name)
{
   return -1;
}

GLint GLAPIENTRY
_mesa_GetProgramResourceLocationIndex(GLuint program, GLenum programInterface,
                                      const GLchar *name)
{
   return -1;
}
