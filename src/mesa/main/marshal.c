/*
 * Copyright © 2012 Intel Corporation
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

/** \file marshal.c
 *
 * Custom functions for marshalling GL calls from the main thread to a worker
 * thread when automatic code generation isn't appropriate.
 */

#include "marshal.h"
#include "dispatch.h"
#include "marshal_generated.h"

struct marshal_cmd_Flush
{
   struct marshal_cmd_base cmd_base;
};


void
_mesa_unmarshal_Flush(struct gl_context *ctx,
                      const struct marshal_cmd_Flush *cmd)
{
   CALL_Flush(ctx->CurrentServerDispatch, ());
}


void GLAPIENTRY
_mesa_marshal_Flush(void)
{
   GET_CURRENT_CONTEXT(ctx);
   struct marshal_cmd_Flush *cmd =
      _mesa_glthread_allocate_command(ctx, DISPATCH_CMD_Flush,
                                      sizeof(struct marshal_cmd_Flush));
   (void) cmd;
   _mesa_post_marshal_hook(ctx);

   /* Flush() needs to be handled specially.  In addition to telling the
    * background thread to flush, we need to ensure that our own buffer is
    * submitted to the background thread so that it will complete in a finite
    * amount of time.
    */
   _mesa_glthread_flush_batch(ctx);
}


struct marshal_cmd_ShaderSource
{
   struct marshal_cmd_base cmd_base;
   GLuint shader;
   GLsizei count;
   /* Followed by GLint length[count], then the contents of all strings,
    * concatenated.
    */
};


void
_mesa_unmarshal_ShaderSource(struct gl_context *ctx,
                             const struct marshal_cmd_ShaderSource *cmd)
{
   const GLint *cmd_length = (const GLint *) (cmd + 1);
   const GLchar *cmd_strings = (const GLchar *) (cmd_length + cmd->count);
   /* TODO: how to deal with malloc failure? */
   const GLchar * *string = malloc(cmd->count * sizeof(const GLchar *));
   int i;

   for (i = 0; i < cmd->count; ++i) {
      string[i] = cmd_strings;
      cmd_strings += cmd_length[i];
   }
   CALL_ShaderSource(ctx->CurrentServerDispatch,
                     (cmd->shader, cmd->count, string, cmd_length));
   free(string);
}


static size_t
measure_ShaderSource_strings(GLsizei count, const GLchar * const *string,
                             const GLint *length_in, GLint *length_out)
{
   int i;
   size_t total_string_length = 0;

   for (i = 0; i < count; ++i) {
      if (length_in == NULL || length_in[i] < 0) {
         if (string[i])
            length_out[i] = strlen(string[i]);
      } else {
         length_out[i] = length_in[i];
      }
      total_string_length += length_out[i];
   }
   return total_string_length;
}


void GLAPIENTRY
_mesa_marshal_ShaderSource(GLuint shader, GLsizei count,
                           const GLchar * const *string, const GLint *length)
{
   /* TODO: how to report an error if count < 0? */

   GET_CURRENT_CONTEXT(ctx);
   /* TODO: how to deal with malloc failure? */
   const size_t fixed_cmd_size = sizeof(struct marshal_cmd_ShaderSource);
   STATIC_ASSERT(sizeof(struct marshal_cmd_ShaderSource) % sizeof(GLint) == 0);
   size_t length_size = count * sizeof(GLint);
   GLint *length_tmp = malloc(length_size);
   size_t total_string_length =
      measure_ShaderSource_strings(count, string, length, length_tmp);
   size_t total_cmd_size = fixed_cmd_size + length_size + total_string_length;

   if (total_cmd_size <= MARSHAL_MAX_CMD_SIZE) {
      struct marshal_cmd_ShaderSource *cmd =
         _mesa_glthread_allocate_command(ctx, DISPATCH_CMD_ShaderSource,
                                         total_cmd_size);
      GLint *cmd_length = (GLint *) (cmd + 1);
      GLchar *cmd_strings = (GLchar *) (cmd_length + count);
      int i;

      cmd->shader = shader;
      cmd->count = count;
      memcpy(cmd_length, length_tmp, length_size);
      for (i = 0; i < count; ++i) {
         memcpy(cmd_strings, string[i], cmd_length[i]);
         cmd_strings += cmd_length[i];
      }
      _mesa_post_marshal_hook(ctx);
   } else {
      _mesa_glthread_finish(ctx);
      CALL_ShaderSource(ctx->CurrentServerDispatch,
                        (shader, count, string, length_tmp));
   }
   free(length_tmp);
}
