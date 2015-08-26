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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "anv_private.h"

/** Log an error message.  */
void anv_printflike(1, 2)
anv_loge(const char *format, ...)
{
   va_list va;

   va_start(va, format);
   anv_loge_v(format, va);
   va_end(va);
}

/** \see anv_loge() */
void
anv_loge_v(const char *format, va_list va)
{
   fprintf(stderr, "vk: error: ");
   vfprintf(stderr, format, va);
   fprintf(stderr, "\n");
}

void anv_printflike(3, 4)
__anv_finishme(const char *file, int line, const char *format, ...)
{
   va_list ap;
   char buffer[256];

   va_start(ap, format);
   vsnprintf(buffer, sizeof(buffer), format, ap);
   va_end(ap);

   fprintf(stderr, "%s:%d: FINISHME: %s\n", file, line, buffer);
}

void anv_noreturn anv_printflike(1, 2)
anv_abortf(const char *format, ...)
{
   va_list va;

   va_start(va, format);
   anv_abortfv(format, va);
   va_end(va);
}

void anv_noreturn
anv_abortfv(const char *format, va_list va)
{
   fprintf(stderr, "vk: error: ");
   vfprintf(stderr, format, va);
   fprintf(stderr, "\n");
   abort();
}

VkResult
__vk_errorf(VkResult error, const char *file, int line, const char *format, ...)
{
   va_list ap;
   char buffer[256];

   static const char *error_names[] = {
      "VK_ERROR_UNKNOWN",
      "VK_ERROR_UNAVAILABLE",
      "VK_ERROR_INITIALIZATION_FAILED",
      "VK_ERROR_OUT_OF_HOST_MEMORY",
      "VK_ERROR_OUT_OF_DEVICE_MEMORY",
      "VK_ERROR_DEVICE_ALREADY_CREATED",
      "VK_ERROR_DEVICE_LOST",
      "VK_ERROR_INVALID_POINTER",
      "VK_ERROR_INVALID_VALUE",
      "VK_ERROR_INVALID_HANDLE",
      "VK_ERROR_INVALID_ORDINAL",
      "VK_ERROR_INVALID_MEMORY_SIZE",
      "VK_ERROR_INVALID_EXTENSION",
      "VK_ERROR_INVALID_FLAGS",
      "VK_ERROR_INVALID_ALIGNMENT",
      "VK_ERROR_INVALID_FORMAT",
      "VK_ERROR_INVALID_IMAGE",
      "VK_ERROR_INVALID_DESCRIPTOR_SET_DATA",
      "VK_ERROR_INVALID_QUEUE_TYPE",
      "VK_ERROR_UNSUPPORTED_SHADER_IL_VERSION",
      "VK_ERROR_BAD_SHADER_CODE",
      "VK_ERROR_BAD_PIPELINE_DATA",
      "VK_ERROR_NOT_MAPPABLE",
      "VK_ERROR_MEMORY_MAP_FAILED",
      "VK_ERROR_MEMORY_UNMAP_FAILED",
      "VK_ERROR_INCOMPATIBLE_DEVICE",
      "VK_ERROR_INCOMPATIBLE_DRIVER",
      "VK_ERROR_INCOMPLETE_COMMAND_BUFFER",
      "VK_ERROR_BUILDING_COMMAND_BUFFER",
      "VK_ERROR_MEMORY_NOT_BOUND",
      "VK_ERROR_INCOMPATIBLE_QUEUE",
      "VK_ERROR_INVALID_LAYER",
   };

   assert(error <= VK_ERROR_UNKNOWN && error >= VK_ERROR_INVALID_LAYER);

   if (format) {
      va_start(ap, format);
      vsnprintf(buffer, sizeof(buffer), format, ap);
      va_end(ap);

      fprintf(stderr, "%s:%d: %s (%s)\n", file, line,
              buffer, error_names[-error - 1]);
   } else {
      fprintf(stderr, "%s:%d: %s\n", file, line, error_names[-error - 1]);
   }

   return error;
}

int
anv_vector_init(struct anv_vector *vector, uint32_t element_size, uint32_t size)
{
   assert(util_is_power_of_two(size));
   assert(element_size < size && util_is_power_of_two(element_size));

   vector->head = 0;
   vector->tail = 0;
   vector->element_size = element_size;
   vector->size = size;
   vector->data = malloc(size);

   return vector->data != NULL;
}

void *
anv_vector_add(struct anv_vector *vector)
{
   uint32_t offset, size, split, tail;
   void *data;

   if (vector->head - vector->tail == vector->size) {
      size = vector->size * 2;
      data = malloc(size);
      if (data == NULL)
         return NULL;
      split = align_u32(vector->tail, vector->size);
      tail = vector->tail & (vector->size - 1);
      if (vector->head - split < vector->size) {
         memcpy(data + tail,
                vector->data + tail,
                split - vector->tail);
         memcpy(data + vector->size,
                vector->data, vector->head - split);
      } else {
         memcpy(data + tail,
                vector->data + tail,
                vector->head - vector->tail);
      }
      free(vector->data);
      vector->data = data;
      vector->size = size;
   }

   assert(vector->head - vector->tail < vector->size);

   offset = vector->head & (vector->size - 1);
   vector->head += vector->element_size;

   return vector->data + offset;
}

void *
anv_vector_remove(struct anv_vector *vector)
{
   uint32_t offset;

   if (vector->head == vector->tail)
      return NULL;

   assert(vector->head - vector->tail <= vector->size);

   offset = vector->tail & (vector->size - 1);
   vector->tail += vector->element_size;

   return vector->data + offset;
}
