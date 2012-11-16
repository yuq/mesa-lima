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

/** \file marshal.h
 *
 * Declarations of functions related to marshalling GL calls from a client
 * thread to a server thread.
 */

#ifndef MARSHAL_H
#define MARSHAL_H

#include "main/glthread.h"
#include "main/context.h"

struct marshal_cmd_base
{
   /**
    * Type of command.  See enum marshal_dispatch_cmd_id.
    */
   uint16_t cmd_id;

   /**
    * Size of command, in multiples of 4 bytes, including cmd_base.
    */
   uint16_t cmd_size;
};


static inline void *
_mesa_glthread_allocate_command(struct gl_context *ctx,
                                uint16_t cmd_id,
                                size_t size)
{
   struct glthread_state *glthread = ctx->GLThread;
   struct marshal_cmd_base *cmd_base;

   if (unlikely(glthread->batch->used + size > MARSHAL_MAX_CMD_SIZE))
      _mesa_glthread_flush_batch(ctx);

   cmd_base = (struct marshal_cmd_base *)
      &glthread->batch->buffer[glthread->batch->used];
   glthread->batch->used += size;
   cmd_base->cmd_id = cmd_id;
   cmd_base->cmd_size = size;
   return cmd_base;
}

#define DEBUG_MARSHAL_PRINT_CALLS 0

static inline void
debug_print_sync(const char *func)
{
#if DEBUG_MARSHAL_PRINT_CALLS
   printf("sync: %s\n", func);
#endif
}

static inline void
debug_print_marshal(const char *func)
{
#if DEBUG_MARSHAL_PRINT_CALLS
   printf("marshal: %s\n", func);
#endif
}

static inline void
debug_print_unmarshal(const char *func)
{
#if DEBUG_MARSHAL_PRINT_CALLS
   printf("unmarshal: %s\n", func);
#endif
}

struct _glapi_table *
_mesa_create_marshal_table(const struct gl_context *ctx);

size_t
_mesa_unmarshal_dispatch_cmd(struct gl_context *ctx, const void *cmd);

static inline void
_mesa_post_marshal_hook(struct gl_context *ctx)
{
   /* This can be enabled for debugging whether a failure is a synchronization
    * problem between the main thread and the worker thread, or a failure in
    * how we actually marshal.
    */
   if (false)
      _mesa_glthread_finish(ctx);
}

struct marshal_cmd_ShaderSource;
struct marshal_cmd_Flush;

void GLAPIENTRY
_mesa_marshal_ShaderSource(GLuint shader, GLsizei count,
                           const GLchar * const *string, const GLint *length);

void
_mesa_unmarshal_ShaderSource(struct gl_context *ctx,
                             const struct marshal_cmd_ShaderSource *cmd);

void GLAPIENTRY
_mesa_marshal_Flush(void);

void
_mesa_unmarshal_Flush(struct gl_context *ctx,
                      const struct marshal_cmd_Flush *cmd);

#endif /* MARSHAL_H */
