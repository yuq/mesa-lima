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
* @file backend.h
*
* @brief Backend handles rasterization, pixel shading and output merger
*        operations.
*
******************************************************************************/
#pragma once

#include "common/os.h"
#include "core/context.h"
#include "core/multisample.h"

void ProcessComputeBE(DRAW_CONTEXT* pDC, uint32_t workerId, uint32_t threadGroupId, void*& pSpillFillBuffer);
void ProcessSyncBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pUserData);
void ProcessQueryStatsBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pUserData);
void ProcessClearBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pUserData);
void ProcessStoreTileBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pData);
void ProcessDiscardInvalidateTilesBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pData);
void BackendNullPS(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t x, uint32_t y, SWR_TRIANGLE_DESC &work, RenderOutputBuffers &renderBuffers);
void InitClearTilesTable();
simdmask ComputeUserClipMask(uint8_t clipMask, float* pUserClipBuffer, simdscalar vI, simdscalar vJ);
void InitBackendFuncTables();
void InitCPSFuncTables();

enum SWR_BACKEND_FUNCS
{
    SWR_BACKEND_SINGLE_SAMPLE,
    SWR_BACKEND_MSAA_PIXEL_RATE,
    SWR_BACKEND_MSAA_SAMPLE_RATE,
    SWR_BACKEND_FUNCS_MAX,
};

#if KNOB_SIMD_WIDTH == 8
extern const __m256 vCenterOffsetsX;
extern const __m256 vCenterOffsetsY;
extern const __m256 vULOffsetsX;
extern const __m256 vULOffsetsY;
#define MASK 0xff
#endif

template<typename T>
INLINE void generateInputCoverage(const uint64_t *const coverageMask, uint32_t (&inputMask)[KNOB_SIMD_WIDTH], const uint32_t sampleMask)
{

    // will need to update for avx512
    assert(KNOB_SIMD_WIDTH == 8);

    __m256i mask[2];
    __m256i sampleCoverage[2];
    if(T::bIsStandardPattern)
    {
        __m256i src = _mm256_set1_epi32(0);
        __m256i index0 = _mm256_set_epi32(7, 6, 5, 4, 3, 2, 1, 0), index1;

        if(T::MultisampleT::numSamples == 1)
        {
            mask[0] = _mm256_set_epi32(0, 0, 0, 0, 0, 0, 0, -1);
        }
        else if(T::MultisampleT::numSamples == 2)
        {
            mask[0] = _mm256_set_epi32(0, 0, 0, 0, 0, 0, -1, -1);
        }
        else if(T::MultisampleT::numSamples == 4)
        {
            mask[0] = _mm256_set_epi32(0, 0, 0, 0, -1, -1, -1, -1);
        }
        else if(T::MultisampleT::numSamples == 8)
        {
            mask[0] = _mm256_set1_epi32(-1);
        }
        else if(T::MultisampleT::numSamples == 16)
        {
            mask[0] = _mm256_set1_epi32(-1);
            mask[1] = _mm256_set1_epi32(-1);
            index1 = _mm256_set_epi32(15, 14, 13, 12, 11, 10, 9, 8);
        }

        // gather coverage for samples 0-7
        sampleCoverage[0] = _mm256_castps_si256(_simd_mask_i32gather_ps(_mm256_castsi256_ps(src), (const float*)coverageMask, index0, _mm256_castsi256_ps(mask[0]), 8));
        if(T::MultisampleT::numSamples > 8)
        {
            // gather coverage for samples 8-15
            sampleCoverage[1] = _mm256_castps_si256(_simd_mask_i32gather_ps(_mm256_castsi256_ps(src), (const float*)coverageMask, index1, _mm256_castsi256_ps(mask[1]), 8));
        }
    }
    else
    {
        // center coverage is the same for all samples; just broadcast to the sample slots
        uint32_t centerCoverage = ((uint32_t)(*coverageMask) & MASK);
        if(T::MultisampleT::numSamples == 1)
        {
            sampleCoverage[0] = _mm256_set_epi32(0, 0, 0, 0, 0, 0, 0, centerCoverage);
        }
        else if(T::MultisampleT::numSamples == 2)
        {
            sampleCoverage[0] = _mm256_set_epi32(0, 0, 0, 0, 0, 0, centerCoverage, centerCoverage);
        }
        else if(T::MultisampleT::numSamples == 4)
        {
            sampleCoverage[0] = _mm256_set_epi32(0, 0, 0, 0, centerCoverage, centerCoverage, centerCoverage, centerCoverage);
        }
        else if(T::MultisampleT::numSamples == 8)
        {
            sampleCoverage[0] = _mm256_set1_epi32(centerCoverage);
        }
        else if(T::MultisampleT::numSamples == 16)
        {
            sampleCoverage[0] = _mm256_set1_epi32(centerCoverage);
            sampleCoverage[1] = _mm256_set1_epi32(centerCoverage);
        }
    }

    mask[0] = _mm256_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0xC, 0x8, 0x4, 0x0,
                              -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0xC, 0x8, 0x4, 0x0);
    // pull out the the 8bit 4x2 coverage for samples 0-7 into the lower 32 bits of each 128bit lane
    __m256i packedCoverage0 = _simd_shuffle_epi8(sampleCoverage[0], mask[0]);

    __m256i packedCoverage1;
    if(T::MultisampleT::numSamples > 8)
    {
        // pull out the the 8bit 4x2 coverage for samples 8-15 into the lower 32 bits of each 128bit lane
        packedCoverage1 = _simd_shuffle_epi8(sampleCoverage[1], mask[0]);
    }

#if (KNOB_ARCH == KNOB_ARCH_AVX)
    // pack lower 32 bits of each 128 bit lane into lower 64 bits of single 128 bit lane 
    __m256i hiToLow = _mm256_permute2f128_si256(packedCoverage0, packedCoverage0, 0x83);
    __m256 shufRes = _mm256_shuffle_ps(_mm256_castsi256_ps(hiToLow), _mm256_castsi256_ps(hiToLow), _MM_SHUFFLE(1, 1, 0, 1));
    packedCoverage0 = _mm256_castps_si256(_mm256_blend_ps(_mm256_castsi256_ps(packedCoverage0), shufRes, 0xFE));

    __m256i packedSampleCoverage;
    if(T::MultisampleT::numSamples > 8)
    {
        // pack lower 32 bits of each 128 bit lane into upper 64 bits of single 128 bit lane
        hiToLow = _mm256_permute2f128_si256(packedCoverage1, packedCoverage1, 0x83);
        shufRes = _mm256_shuffle_ps(_mm256_castsi256_ps(hiToLow), _mm256_castsi256_ps(hiToLow), _MM_SHUFFLE(1, 1, 0, 1));
        shufRes = _mm256_blend_ps(_mm256_castsi256_ps(packedCoverage1), shufRes, 0xFE);
        packedCoverage1 = _mm256_castps_si256(_mm256_castpd_ps(_mm256_shuffle_pd(_mm256_castps_pd(shufRes), _mm256_castps_pd(shufRes), 0x01)));
        packedSampleCoverage = _mm256_castps_si256(_mm256_blend_ps(_mm256_castsi256_ps(packedCoverage0), _mm256_castsi256_ps(packedCoverage1), 0xFC));
    }
    else
    {
        packedSampleCoverage = packedCoverage0;
    }
#else
    __m256i permMask = _mm256_set_epi32(0x7, 0x7, 0x7, 0x7, 0x7, 0x7, 0x4, 0x0);
    // pack lower 32 bits of each 128 bit lane into lower 64 bits of single 128 bit lane 
    packedCoverage0 = _mm256_permutevar8x32_epi32(packedCoverage0, permMask);

    __m256i packedSampleCoverage;
    if(T::MultisampleT::numSamples > 8)
    {
        permMask = _mm256_set_epi32(0x7, 0x7, 0x7, 0x7, 0x4, 0x0, 0x7, 0x7);
        // pack lower 32 bits of each 128 bit lane into upper 64 bits of single 128 bit lane
        packedCoverage1 = _mm256_permutevar8x32_epi32(packedCoverage1, permMask);

        // blend coverage masks for samples 0-7 and samples 8-15 into single 128 bit lane
        packedSampleCoverage = _mm256_blend_epi32(packedCoverage0, packedCoverage1, 0x0C);
    }
    else
    {
        packedSampleCoverage = packedCoverage0;
    }
#endif

    for(int32_t i = KNOB_SIMD_WIDTH - 1; i >= 0; i--)
    {
        // convert packed sample coverage masks into single coverage masks for all samples for each pixel in the 4x2
        inputMask[i] = _simd_movemask_epi8(packedSampleCoverage);

        if(!T::bForcedSampleCount)
        {
            // input coverage has to be anded with sample mask if MSAA isn't forced on
            inputMask[i] &= sampleMask;
        }

        // shift to the next pixel in the 4x2
        packedSampleCoverage = _simd_slli_epi32(packedSampleCoverage, 1);
    }
}

template<typename T>
INLINE void generateInputCoverage(const uint64_t *const coverageMask, __m256 &inputCoverage, const uint32_t sampleMask)
{
    uint32_t inputMask[KNOB_SIMD_WIDTH]; 
    generateInputCoverage<T>(coverageMask, inputMask, sampleMask);
    inputCoverage = _simd_castsi_ps(_mm256_set_epi32(inputMask[7], inputMask[6], inputMask[5], inputMask[4], inputMask[3], inputMask[2], inputMask[1], inputMask[0]));
}

template<uint32_t sampleCountT = SWR_MULTISAMPLE_1X, uint32_t samplePattern = SWR_MSAA_STANDARD_PATTERN,
         uint32_t coverage = 0, uint32_t centroid = 0, uint32_t forced = 0, uint32_t odepth = 0>
struct SwrBackendTraits
{
    static const bool bIsStandardPattern = (samplePattern == SWR_MSAA_STANDARD_PATTERN);
    static const bool bInputCoverage = (coverage == 1);
    static const bool bCentroidPos = (centroid == 1);
    static const bool bForcedSampleCount = (forced == 1);
    static const bool bWritesODepth = (odepth == 1);
    typedef MultisampleTraits<(SWR_MULTISAMPLE_COUNT)sampleCountT, (bIsStandardPattern) ? SWR_MSAA_STANDARD_PATTERN : SWR_MSAA_CENTER_PATTERN> MultisampleT;
};