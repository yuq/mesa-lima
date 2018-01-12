/*
 * Copyright (C) 2018 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdio.h>
#include <time.h>

#include "lima_util.h"

bool lima_get_absolute_timeout(uint64_t *timeout, bool relative)
{
   if (relative) {
      struct timespec current;
      uint64_t current_ns;

      if (clock_gettime(CLOCK_MONOTONIC, &current))
         return false;

      current_ns = ((uint64_t)current.tv_sec) * 1000000000ull;
      current_ns += current.tv_nsec;
      *timeout += current_ns;
   }
   return true;
}

void lima_dump_blob(void *data, int size, bool is_float)
{
   if (is_float) {
      float *blob = data;
      for (int i = 0; i * 4 < size; i += 4)
         printf ("%04x: %f %f %f %f\n", i * 4,
                 blob[i], blob[i + 1], blob[i + 2], blob[i + 3]);
   }
   else {
      uint32_t *blob = data;
      for (int i = 0; i * 4 < size; i += 4)
         printf ("%04x: 0x%08x 0x%08x 0x%08x 0x%08x\n", i * 4,
                 blob[i], blob[i + 1], blob[i + 2], blob[i + 3]);
   }
}
