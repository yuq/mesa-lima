/****************************************************************************
* Copyright (C) 2014-2015 Intel Corporation.   All Rights Reserved.
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
****************************************************************************/

#ifndef __SWR_INTRIN_H__
#define __SWR_INTRIN_H__

#include "os.h"

#include <cassert>

#include <emmintrin.h>
#include <immintrin.h>
#include <xmmintrin.h>

#if KNOB_SIMD_WIDTH == 8 
typedef __m256 simdscalar;
typedef __m256i simdscalari;
typedef uint8_t simdmask;
#else
#error Unsupported vector width
#endif

// simd vector
OSALIGNSIMD(union) simdvector
{
    simdscalar  v[4];
    struct
    {
        simdscalar x, y, z, w;
    };

    simdscalar& operator[] (const int i) { return v[i]; }
    const simdscalar& operator[] (const int i) const { return v[i]; }
};

#if ENABLE_AVX512_SIMD16

#if KNOB_SIMD16_WIDTH == 16

#if ENABLE_AVX512_EMULATION
struct simd16scalar
{
    __m256  lo;
    __m256  hi;
};
struct simd16scalard
{
    __m256d lo;
    __m256d hi;
};
struct simd16scalari
{
    __m256i lo;
    __m256i hi;
};
typedef uint16_t simd16mask;

#else
typedef __m512 simd16scalar;
typedef __m512d simd16scalard;
typedef __m512i simd16scalari;
typedef __mmask16 simd16mask;
#endif//ENABLE_AVX512_EMULATION
#else
#error Unsupported vector width
#endif//KNOB_SIMD16_WIDTH == 16

#define _simd16_masklo(mask) ((mask) & 0xFF)
#define _simd16_maskhi(mask) (((mask) >> 8) & 0xFF)
#define _simd16_setmask(hi, lo) (((hi) << 8) | (lo))

#if defined(_WIN32)
#define SIMDAPI __vectorcall
#else
#define SIMDAPI
#endif

OSALIGN(union, KNOB_SIMD16_BYTES) simd16vector
{
    simd16scalar  v[4];
    struct
    {
        simd16scalar x, y, z, w;
    };

    simd16scalar& operator[] (const int i) { return v[i]; }
    const simd16scalar& operator[] (const int i) const { return v[i]; }
};

#endif // ENABLE_AVX512_SIMD16

INLINE
UINT pdep_u32(UINT a, UINT mask)
{
#if KNOB_ARCH >= KNOB_ARCH_AVX2
    return _pdep_u32(a, mask);
#else
    UINT result = 0;

    // copied from http://wm.ite.pl/articles/pdep-soft-emu.html 
    // using bsf instead of funky loop
    DWORD maskIndex;
    while (_BitScanForward(&maskIndex, mask))
    {
        // 1. isolate lowest set bit of mask
        const UINT lowest = 1 << maskIndex;

        // 2. populate LSB from src
        const UINT LSB = (UINT)((int)(a << 31) >> 31);

        // 3. copy bit from mask
        result |= LSB & lowest;

        // 4. clear lowest bit
        mask &= ~lowest;

        // 5. prepare for next iteration
        a >>= 1;
    }

    return result;
#endif
}

INLINE
UINT pext_u32(UINT a, UINT mask)
{
#if KNOB_ARCH >= KNOB_ARCH_AVX2
    return _pext_u32(a, mask);
#else
    UINT result = 0;
    DWORD maskIndex;
    uint32_t currentBit = 0;
    while (_BitScanForward(&maskIndex, mask))
    {
        // 1. isolate lowest set bit of mask
        const UINT lowest = 1 << maskIndex;

        // 2. copy bit from mask
        result |= ((a & lowest) > 0) << currentBit++;

        // 3. clear lowest bit
        mask &= ~lowest;
    }
    return result;
#endif
}

#endif//__SWR_INTRIN_H__
