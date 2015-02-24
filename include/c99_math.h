/**************************************************************************
 *
 * Copyright 2007-2015 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/**
 * Wrapper for math.h which makes sure we have definitions of all the c99
 * functions.
 */


#ifndef _C99_MATH_H_
#define _C99_MATH_H_

#include <math.h>
#include "c99_compat.h"


#if defined(_MSC_VER)

#if _MSC_VER < 1400 && !defined(__cplusplus)

static inline float cosf( float f )
{
   return (float) cos( (double) f );
}

static inline float sinf( float f )
{
   return (float) sin( (double) f );
}

static inline float ceilf( float f )
{
   return (float) ceil( (double) f );
}

static inline float floorf( float f )
{
   return (float) floor( (double) f );
}

static inline float powf( float f, float g )
{
   return (float) pow( (double) f, (double) g );
}

static inline float sqrtf( float f )
{
   return (float) sqrt( (double) f );
}

static inline float fabsf( float f )
{
   return (float) fabs( (double) f );
}

static inline float logf( float f )
{
   return (float) log( (double) f );
}

static inline float frexpf(float x, int *exp)
{
   return (float) frexp(x, exp);
}

static inline float ldexpf(float x, int exp)
{
   return (float) ldexp(x, exp);
}

static inline float logf(float x)
{
   return (float) log(x);
}

static inline float expf(float x)
{
   return (float) exp(x);
}


#else
/* Work-around an extra semi-colon in VS 2005 logf definition */
#ifdef logf
#undef logf
#define logf(x) ((float)log((double)(x)))
#endif /* logf */

#if _MSC_VER < 1800
#define isfinite(x) _finite((double)(x))
#define isnan(x) _isnan((double)(x))
#endif /* _MSC_VER < 1800 */
#endif /* _MSC_VER < 1400 && !defined(__cplusplus) */

#if _MSC_VER < 1800
static inline double log2( double x )
{
   const double invln2 = 1.442695041;
   return log( x ) * invln2;
}

static inline double
round(double x)
{
   return x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5);
}

static inline float
roundf(float x)
{
   return x >= 0.0f ? floorf(x + 0.5f) : ceilf(x - 0.5f);
}
#endif

#ifndef INFINITY
#define INFINITY (DBL_MAX + DBL_MAX)
#endif

#ifndef NAN
#define NAN (INFINITY - INFINITY)
#endif

#endif /* _MSC_VER */


#if __STDC_VERSION__ < 199901L && (!defined(__cplusplus) || defined(_MSC_VER))
static inline long int
lrint(double d)
{
   long int rounded = (long int)(d + 0.5);

   if (d - floor(d) == 0.5) {
      if (rounded % 2 != 0)
         rounded += (d > 0) ? -1 : 1;
   }

   return rounded;
}

static inline long int
lrintf(float f)
{
   long int rounded = (long int)(f + 0.5f);

   if (f - floorf(f) == 0.5f) {
      if (rounded % 2 != 0)
         rounded += (f > 0) ? -1 : 1;
   }

   return rounded;
}

static inline long long int
llrint(double d)
{
   long long int rounded = (long long int)(d + 0.5);

   if (d - floor(d) == 0.5) {
      if (rounded % 2 != 0)
         rounded += (d > 0) ? -1 : 1;
   }

   return rounded;
}

static inline long long int
llrintf(float f)
{
   long long int rounded = (long long int)(f + 0.5f);

   if (f - floorf(f) == 0.5f) {
      if (rounded % 2 != 0)
         rounded += (f > 0) ? -1 : 1;
   }

   return rounded;
}
#endif /* C99 */


#endif /* #define _C99_MATH_H_ */
