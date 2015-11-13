/*
 * Copyright 2015 Intel Corporation
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice (including the next
 *  paragraph) shall be included in all copies or substantial portions of the
 *  Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 *  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

#include <assert.h>

#include "mesa/main/imports.h"

#include "isl.h"

/**
 * Log base 2, rounding towards zero.
 */
static inline uint32_t
isl_log2u(uint32_t n)
{
   assert(n != 0);
   return 31 - __builtin_clz(n);
}

void
isl_device_init(struct isl_device *dev, uint8_t gen10x)
{
   assert(gen10x % 5 == 0);
   dev->gen = gen10x;
}

/**
 * The returned extent's units are (width=bytes, height=rows).
 */
void
isl_tiling_get_extent(const struct isl_device *dev,
                      enum isl_tiling tiling,
                      uint32_t cpp,
                      struct isl_extent2d *e)
{
   static const struct isl_extent2d legacy_extents[] = {
      [ISL_TILING_LINEAR]  = {   1,   1 },
      [ISL_TILING_X]       = { 512,   8 },
      [ISL_TILING_Y]       = { 128,  32 },
      [ISL_TILING_W]       = { 128,  32 },
   };

   static const struct isl_extent2d yf_extents[] = {
      /*cpp*/
      /* 1*/ [0] = {   64, 64 },
      /* 2*/ [1] = {  128, 32 },
      /* 4*/ [2] = {  128, 32 },
      /* 8*/ [3] = {  256, 16 },
      /*16*/ [4] = {  256, 16 },
   };

   assert(cpp > 0);

   switch (tiling) {
   case ISL_TILING_LINEAR:
   case ISL_TILING_X:
   case ISL_TILING_Y:
   case ISL_TILING_W:
      *e = legacy_extents[tiling];
      return;
   case ISL_TILING_Yf:
   case ISL_TILING_Ys:
      assert(_mesa_is_pow_two(cpp));
      *e = yf_extents[isl_log2u(cpp)];
      if (tiling == ISL_TILING_Ys) {
         e->width *= 4;
         e->height *= 4;
      }
      return;
   }
}
