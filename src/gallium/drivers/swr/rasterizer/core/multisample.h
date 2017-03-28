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
*
* @file multisample.h
*
******************************************************************************/

#pragma once

#include "context.h"
#include "format_traits.h"

//////////////////////////////////////////////////////////////////////////
/// @brief convenience typedef for testing for single sample case
typedef std::integral_constant<int, 1> SingleSampleT;

INLINE
uint32_t GetNumSamples(SWR_MULTISAMPLE_COUNT sampleCount)
{
    static const uint32_t sampleCountLUT[SWR_MULTISAMPLE_TYPE_COUNT] {1, 2, 4, 8, 16};
    assert(sampleCount < SWR_MULTISAMPLE_TYPE_COUNT);
    return sampleCountLUT[sampleCount];
}

INLINE
SWR_MULTISAMPLE_COUNT GetSampleCount(uint32_t numSamples)
{
    switch(numSamples)
    {
    case 1: return SWR_MULTISAMPLE_1X;
    case 2: return SWR_MULTISAMPLE_2X;
    case 4: return SWR_MULTISAMPLE_4X;
    case 8: return SWR_MULTISAMPLE_8X;
    case 16: return SWR_MULTISAMPLE_16X;
    default: assert(0); return SWR_MULTISAMPLE_1X;
    }
}

// hardcoded offsets based on Direct3d standard multisample positions
// 8 x 8 pixel grid ranging from (0, 0) to (15, 15), with (0, 0) = UL pixel corner
// coords are 0.8 fixed point offsets from (0, 0)
template<SWR_MULTISAMPLE_COUNT sampleCount, bool isCenter = false>
struct MultisampleTraits
{
    INLINE static float X(uint32_t sampleNum) = delete;
    INLINE static float Y(uint32_t sampleNum) = delete;
    INLINE static simdscalari FullSampleMask() = delete;

    static const uint32_t numSamples = 0;
};

template<>
struct MultisampleTraits<SWR_MULTISAMPLE_1X, false>
{
    INLINE static float X(uint32_t sampleNum) {return samplePosX;};
    INLINE static float Y(uint32_t sampleNum) {return samplePosY;};
    INLINE static simdscalari FullSampleMask(){return _simd_set1_epi32(0x1);};

    static const uint32_t samplePosXi;
    static const uint32_t samplePosYi;
    static const float samplePosX;
    static const float samplePosY;
    static const uint32_t numSamples = 1;
    static const SWR_MULTISAMPLE_COUNT sampleCount = SWR_MULTISAMPLE_1X;
    static const uint32_t numCoverageSamples = 1; 
};

template<>
struct MultisampleTraits<SWR_MULTISAMPLE_1X, true>
{
    INLINE static float X(uint32_t sampleNum) {return 0.5f;};
    INLINE static float Y(uint32_t sampleNum) {return 0.5f;};
    INLINE static simdscalari FullSampleMask(){return _simd_set1_epi32(0x1);};
    
    static const uint32_t numSamples = 1;
    static const float samplePosX;
    static const float samplePosY;
    static const SWR_MULTISAMPLE_COUNT sampleCount = SWR_MULTISAMPLE_1X;
    static const uint32_t numCoverageSamples = 1;
};

template<>
struct MultisampleTraits<SWR_MULTISAMPLE_2X, false>
{
    INLINE static float X(uint32_t sampleNum) { SWR_ASSERT(sampleNum < numSamples); return samplePosX[sampleNum]; };
    INLINE static float Y(uint32_t sampleNum) { SWR_ASSERT(sampleNum < numSamples); return samplePosY[sampleNum]; };
    INLINE static simdscalari FullSampleMask()
    {
         static const simdscalari mask =_simd_set1_epi32(0x3);
         return mask;
    }

    static const uint32_t samplePosXi[2];
    static const uint32_t samplePosYi[2];
    static const float samplePosX[2];
    static const float samplePosY[2];
    static const uint32_t numSamples = 2;
    static const SWR_MULTISAMPLE_COUNT sampleCount = SWR_MULTISAMPLE_2X;
    static const uint32_t numCoverageSamples = 2;
};

template<>
struct MultisampleTraits<SWR_MULTISAMPLE_2X, true>
{
    INLINE static float X(uint32_t sampleNum) {return 0.5f;};
    INLINE static float Y(uint32_t sampleNum) {return 0.5f;};
    INLINE static simdscalari FullSampleMask()
    {
         static const simdscalari mask =_simd_set1_epi32(0x3);
         return mask;
    }
    static const uint32_t numSamples = 2;
    static const float samplePosX[2];
    static const float samplePosY[2];
    static const SWR_MULTISAMPLE_COUNT sampleCount = SWR_MULTISAMPLE_2X;
    static const uint32_t numCoverageSamples = 1;
};

template<>
struct MultisampleTraits<SWR_MULTISAMPLE_4X, false>
{
    INLINE static float X(uint32_t sampleNum) { SWR_ASSERT(sampleNum < numSamples); return samplePosX[sampleNum]; };
    INLINE static float Y(uint32_t sampleNum) { SWR_ASSERT(sampleNum < numSamples); return samplePosY[sampleNum]; };
    INLINE static simdscalari FullSampleMask()
    {
        static const simdscalari mask = _simd_set1_epi32(0xF);
        return mask;
    }

    static const uint32_t samplePosXi[4];
    static const uint32_t samplePosYi[4];
    static const float samplePosX[4];
    static const float samplePosY[4];
    static const uint32_t numSamples = 4;
    static const SWR_MULTISAMPLE_COUNT sampleCount = SWR_MULTISAMPLE_4X;
    static const uint32_t numCoverageSamples = 4;
};

template<>
struct MultisampleTraits<SWR_MULTISAMPLE_4X, true>
{
    INLINE static float X(uint32_t sampleNum) {return 0.5f;};
    INLINE static float Y(uint32_t sampleNum) {return 0.5f;};
    INLINE static simdscalari FullSampleMask()
    {
        static const simdscalari mask = _simd_set1_epi32(0xF);
        return mask;
    }

    static const uint32_t numSamples = 4;
    static const float samplePosX[4];
    static const float samplePosY[4];
    static const SWR_MULTISAMPLE_COUNT sampleCount = SWR_MULTISAMPLE_4X;
    static const uint32_t numCoverageSamples = 1;
};

template<>
struct MultisampleTraits<SWR_MULTISAMPLE_8X, false>
{
    INLINE static float X(uint32_t sampleNum) { SWR_ASSERT(sampleNum < numSamples); return samplePosX[sampleNum]; };
    INLINE static float Y(uint32_t sampleNum) { SWR_ASSERT(sampleNum < numSamples); return samplePosY[sampleNum]; };
    INLINE static simdscalari FullSampleMask()
    {
        static const simdscalari mask = _simd_set1_epi32(0xFF);
        return mask;
    }

    static const uint32_t samplePosXi[8];
    static const uint32_t samplePosYi[8];
    static const float samplePosX[8];
    static const float samplePosY[8];
    static const uint32_t numSamples = 8;
    static const SWR_MULTISAMPLE_COUNT sampleCount = SWR_MULTISAMPLE_8X;
    static const uint32_t numCoverageSamples = 8;
};

template<>
struct MultisampleTraits<SWR_MULTISAMPLE_8X, true>
{
    INLINE static float X(uint32_t sampleNum) {return 0.5f;};
    INLINE static float Y(uint32_t sampleNum) {return 0.5f;};
    INLINE static simdscalari FullSampleMask()
    {
        static const simdscalari mask = _simd_set1_epi32(0xFF);
        return mask;
    }
    static const uint32_t numSamples = 8;
    static const float samplePosX[8];
    static const float samplePosY[8];
    static const SWR_MULTISAMPLE_COUNT sampleCount = SWR_MULTISAMPLE_8X;
    static const uint32_t numCoverageSamples = 1;
};

template<>
struct MultisampleTraits<SWR_MULTISAMPLE_16X, false>
{
    INLINE static float X(uint32_t sampleNum) { SWR_ASSERT(sampleNum < numSamples); return samplePosX[sampleNum]; };
    INLINE static float Y(uint32_t sampleNum) { SWR_ASSERT(sampleNum < numSamples); return samplePosY[sampleNum]; };
    INLINE static simdscalari FullSampleMask()
    {
        static const simdscalari mask = _simd_set1_epi32(0xFFFF);
        return mask;
    }

    static const uint32_t samplePosXi[16];
    static const uint32_t samplePosYi[16];
    static const float samplePosX[16];
    static const float samplePosY[16];
    static const uint32_t numSamples = 16;
    static const SWR_MULTISAMPLE_COUNT sampleCount = SWR_MULTISAMPLE_16X;
    static const uint32_t numCoverageSamples = 16;
};

template<>
struct MultisampleTraits<SWR_MULTISAMPLE_16X, true>
{
    INLINE static float X(uint32_t sampleNum) {return 0.5f;};
    INLINE static float Y(uint32_t sampleNum) {return 0.5f;};
    INLINE static simdscalari FullSampleMask()
    {
        static const simdscalari mask = _simd_set1_epi32(0xFFFF);
        return mask;
    }
    static const uint32_t numSamples = 16;
    static const float samplePosX[16];
    static const float samplePosY[16];
    static const SWR_MULTISAMPLE_COUNT sampleCount = SWR_MULTISAMPLE_16X;
    static const uint32_t numCoverageSamples = 1;
};

INLINE
bool isNonStandardPattern(const SWR_MULTISAMPLE_COUNT sampleCount, const SWR_MULTISAMPLE_POS& samplePos)
{
    // detect if we're using standard or center sample patterns
    const uint32_t *standardPosX, *standardPosY;
    switch(sampleCount)
    {
    case SWR_MULTISAMPLE_1X:
        standardPosX = &MultisampleTraits<SWR_MULTISAMPLE_1X>::samplePosXi;
        standardPosY = &MultisampleTraits<SWR_MULTISAMPLE_1X>::samplePosYi;
        break;
    case SWR_MULTISAMPLE_2X:
        standardPosX = MultisampleTraits<SWR_MULTISAMPLE_2X>::samplePosXi;
        standardPosY = MultisampleTraits<SWR_MULTISAMPLE_2X>::samplePosYi;
        break;
    case SWR_MULTISAMPLE_4X:
        standardPosX = MultisampleTraits<SWR_MULTISAMPLE_4X>::samplePosXi;
        standardPosY = MultisampleTraits<SWR_MULTISAMPLE_4X>::samplePosYi;
        break;
    case SWR_MULTISAMPLE_8X:
        standardPosX = MultisampleTraits<SWR_MULTISAMPLE_8X>::samplePosXi;
        standardPosY = MultisampleTraits<SWR_MULTISAMPLE_8X>::samplePosYi;
        break;
    case SWR_MULTISAMPLE_16X:
        standardPosX = MultisampleTraits<SWR_MULTISAMPLE_16X>::samplePosXi;
        standardPosY = MultisampleTraits<SWR_MULTISAMPLE_16X>::samplePosYi;
        break;
    default:
        break;
    }

    // scan sample pattern for standard or center
    uint32_t numSamples = GetNumSamples(sampleCount);
    bool bIsStandard = true;
    if(numSamples > 1)
    {
        for(uint32_t i = 0; i < numSamples; i++)
        {
            bIsStandard = (standardPosX[i] == samplePos.Xi(i)) ||
                (standardPosY[i] == samplePos.Yi(i));
            if(!bIsStandard)
                break;
        }
    }
    return !bIsStandard;
}