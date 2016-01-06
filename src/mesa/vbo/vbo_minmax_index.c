/*
 * Mesa 3-D graphics library
 *
 * Copyright 2003 VMware, Inc.
 * Copyright 2009 VMware, Inc.
 * All Rights Reserved.
 * Copyright (C) 2016 Advanced Micro Devices, Inc.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "main/glheader.h"
#include "main/context.h"
#include "main/varray.h"
#include "main/macros.h"
#include "main/sse_minmax.h"
#include "x86/common_x86_asm.h"


/**
 * Compute min and max elements by scanning the index buffer for
 * glDraw[Range]Elements() calls.
 * If primitive restart is enabled, we need to ignore restart
 * indexes when computing min/max.
 */
static void
vbo_get_minmax_index(struct gl_context *ctx,
                     const struct _mesa_prim *prim,
                     const struct _mesa_index_buffer *ib,
                     GLuint *min_index, GLuint *max_index,
                     const GLuint count)
{
   const GLboolean restart = ctx->Array._PrimitiveRestart;
   const GLuint restartIndex = _mesa_primitive_restart_index(ctx, ib->type);
   const int index_size = vbo_sizeof_ib_type(ib->type);
   const char *indices;
   GLuint i;

   indices = (char *) ib->ptr + prim->start * index_size;
   if (_mesa_is_bufferobj(ib->obj)) {
      GLsizeiptr size = MIN2(count * index_size, ib->obj->Size);
      indices = ctx->Driver.MapBufferRange(ctx, (GLintptr) indices, size,
                                           GL_MAP_READ_BIT, ib->obj,
                                           MAP_INTERNAL);
   }

   switch (ib->type) {
   case GL_UNSIGNED_INT: {
      const GLuint *ui_indices = (const GLuint *)indices;
      GLuint max_ui = 0;
      GLuint min_ui = ~0U;
      if (restart) {
         for (i = 0; i < count; i++) {
            if (ui_indices[i] != restartIndex) {
               if (ui_indices[i] > max_ui) max_ui = ui_indices[i];
               if (ui_indices[i] < min_ui) min_ui = ui_indices[i];
            }
         }
      }
      else {
#if defined(USE_SSE41)
         if (cpu_has_sse4_1) {
            _mesa_uint_array_min_max(ui_indices, &min_ui, &max_ui, count);
         }
         else
#endif
            for (i = 0; i < count; i++) {
               if (ui_indices[i] > max_ui) max_ui = ui_indices[i];
               if (ui_indices[i] < min_ui) min_ui = ui_indices[i];
            }
      }
      *min_index = min_ui;
      *max_index = max_ui;
      break;
   }
   case GL_UNSIGNED_SHORT: {
      const GLushort *us_indices = (const GLushort *)indices;
      GLuint max_us = 0;
      GLuint min_us = ~0U;
      if (restart) {
         for (i = 0; i < count; i++) {
            if (us_indices[i] != restartIndex) {
               if (us_indices[i] > max_us) max_us = us_indices[i];
               if (us_indices[i] < min_us) min_us = us_indices[i];
            }
         }
      }
      else {
         for (i = 0; i < count; i++) {
            if (us_indices[i] > max_us) max_us = us_indices[i];
            if (us_indices[i] < min_us) min_us = us_indices[i];
         }
      }
      *min_index = min_us;
      *max_index = max_us;
      break;
   }
   case GL_UNSIGNED_BYTE: {
      const GLubyte *ub_indices = (const GLubyte *)indices;
      GLuint max_ub = 0;
      GLuint min_ub = ~0U;
      if (restart) {
         for (i = 0; i < count; i++) {
            if (ub_indices[i] != restartIndex) {
               if (ub_indices[i] > max_ub) max_ub = ub_indices[i];
               if (ub_indices[i] < min_ub) min_ub = ub_indices[i];
            }
         }
      }
      else {
         for (i = 0; i < count; i++) {
            if (ub_indices[i] > max_ub) max_ub = ub_indices[i];
            if (ub_indices[i] < min_ub) min_ub = ub_indices[i];
         }
      }
      *min_index = min_ub;
      *max_index = max_ub;
      break;
   }
   default:
      unreachable("not reached");
   }

   if (_mesa_is_bufferobj(ib->obj)) {
      ctx->Driver.UnmapBuffer(ctx, ib->obj, MAP_INTERNAL);
   }
}

/**
 * Compute min and max elements for nr_prims
 */
void
vbo_get_minmax_indices(struct gl_context *ctx,
                       const struct _mesa_prim *prims,
                       const struct _mesa_index_buffer *ib,
                       GLuint *min_index,
                       GLuint *max_index,
                       GLuint nr_prims)
{
   GLuint tmp_min, tmp_max;
   GLuint i;
   GLuint count;

   *min_index = ~0;
   *max_index = 0;

   for (i = 0; i < nr_prims; i++) {
      const struct _mesa_prim *start_prim;

      start_prim = &prims[i];
      count = start_prim->count;
      /* Do combination if possible to reduce map/unmap count */
      while ((i + 1 < nr_prims) &&
             (prims[i].start + prims[i].count == prims[i+1].start)) {
         count += prims[i+1].count;
         i++;
      }
      vbo_get_minmax_index(ctx, start_prim, ib, &tmp_min, &tmp_max, count);
      *min_index = MIN2(*min_index, tmp_min);
      *max_index = MAX2(*max_index, tmp_max);
   }
}
