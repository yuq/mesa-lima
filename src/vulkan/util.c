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

#include "private.h"

int
anv_vector_init(struct anv_vector *vector, uint32_t element_size, uint32_t size)
{
   assert(is_power_of_two(size));
   assert(element_size < size && is_power_of_two(element_size));

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
      split = ALIGN_U32(vector->tail, vector->size);
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
