/**
 * \file format_utils.h
 * A collection of format conversion utility functions.
 */

/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2006  Brian Paul  All Rights Reserved.
 * Copyright (C) 2014  Intel Corporation  All Rights Reserved.
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
 */

#ifndef FORMAT_UTILS_H
#define FORMAT_UTILS_H

#include "imports.h"

/* Only guaranteed to work for BITS <= 32 */
#define MAX_UINT(BITS) ((BITS) == 32 ? UINT32_MAX : ((1u << (BITS)) - 1))
#define MAX_INT(BITS) ((int)MAX_UINT((BITS) - 1))

/* Extends an integer of size SRC_BITS to one of size DST_BITS linearly */
#define EXTEND_NORMALIZED_INT(X, SRC_BITS, DST_BITS) \
      (((X) * (int)(MAX_UINT(DST_BITS) / MAX_UINT(SRC_BITS))) + \
       ((DST_BITS % SRC_BITS) ? ((X) >> (SRC_BITS - DST_BITS % SRC_BITS)) : 0))

static inline float
_mesa_unorm_to_float(unsigned x, unsigned src_bits)
{
   return x * (1.0f / (float)MAX_UINT(src_bits));
}

static inline float
_mesa_snorm_to_float(int x, unsigned src_bits)
{
   if (x <= -MAX_INT(src_bits))
      return -1.0f;
   else
      return x * (1.0f / (float)MAX_INT(src_bits));
}

static inline uint16_t
_mesa_unorm_to_half(unsigned x, unsigned src_bits)
{
   return _mesa_float_to_half(_mesa_unorm_to_float(x, src_bits));
}

static inline uint16_t
_mesa_snorm_to_half(int x, unsigned src_bits)
{
   return _mesa_float_to_half(_mesa_snorm_to_float(x, src_bits));
}

static inline unsigned
_mesa_float_to_unorm(float x, unsigned dst_bits)
{
   if (x < 0.0f)
      return 0;
   else if (x > 1.0f)
      return MAX_UINT(dst_bits);
   else
      return F_TO_I(x * MAX_UINT(dst_bits));
}

static inline unsigned
_mesa_half_to_unorm(uint16_t x, unsigned dst_bits)
{
   return _mesa_float_to_unorm(_mesa_half_to_float(x), dst_bits);
}

static inline unsigned
_mesa_unorm_to_unorm(unsigned x, unsigned src_bits, unsigned dst_bits)
{
   if (src_bits < dst_bits)
      return EXTEND_NORMALIZED_INT(x, src_bits, dst_bits);
   else
      return x >> (src_bits - dst_bits);
}

static inline unsigned
_mesa_snorm_to_unorm(int x, unsigned src_bits, unsigned dst_bits)
{
   if (x < 0)
      return 0;
   else
      return _mesa_unorm_to_unorm(x, src_bits - 1, dst_bits);
}

static inline int
_mesa_float_to_snorm(float x, unsigned dst_bits)
{
   if (x < -1.0f)
      return -MAX_INT(dst_bits);
   else if (x > 1.0f)
      return MAX_INT(dst_bits);
   else
      return F_TO_I(x * MAX_INT(dst_bits));
}

static inline int
_mesa_half_to_snorm(uint16_t x, unsigned dst_bits)
{
   return _mesa_float_to_snorm(_mesa_half_to_float(x), dst_bits);
}

static inline int
_mesa_unorm_to_snorm(unsigned x, unsigned src_bits, unsigned dst_bits)
{
   return _mesa_unorm_to_unorm(x, src_bits, dst_bits - 1);
}

static inline int
_mesa_snorm_to_snorm(int x, unsigned src_bits, unsigned dst_bits)
{
   if (x < -MAX_INT(src_bits))
      return -MAX_INT(dst_bits);
   else if (src_bits < dst_bits)
      return EXTEND_NORMALIZED_INT(x, src_bits - 1, dst_bits - 1);
   else
      return x >> (src_bits - dst_bits);
}

bool
_mesa_format_to_array(mesa_format, GLenum *type, int *num_components,
                      uint8_t swizzle[4], bool *normalized);

void
_mesa_swizzle_and_convert(void *dst, GLenum dst_type, int num_dst_channels,
                          const void *src, GLenum src_type, int num_src_channels,
                          const uint8_t swizzle[4], bool normalized, int count);

#endif
