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
* @file backend.cpp
*
* @brief Backend handles rasterization, pixel shading and output merger
*        operations.
*
******************************************************************************/

#include <smmintrin.h>

#include "rdtsc_core.h"
#include "backend.h"
#include "depthstencil.h"
#include "tilemgr.h"
#include "memory/tilingtraits.h"
#include "core/multisample.h"

#include <algorithm>

const __m128 vTileOffsetsX = {0.5, KNOB_TILE_X_DIM - 0.5, 0.5, KNOB_TILE_X_DIM - 0.5};
const __m128 vTileOffsetsY = {0.5, 0.5, KNOB_TILE_Y_DIM - 0.5, KNOB_TILE_Y_DIM - 0.5};

/// @todo move to common lib
#define MASKTOVEC(i3,i2,i1,i0) {-i0,-i1,-i2,-i3}
static const __m128 gMaskToVec[] = {
    MASKTOVEC(0,0,0,0),
    MASKTOVEC(0,0,0,1),
    MASKTOVEC(0,0,1,0),
    MASKTOVEC(0,0,1,1),
    MASKTOVEC(0,1,0,0),
    MASKTOVEC(0,1,0,1),
    MASKTOVEC(0,1,1,0),
    MASKTOVEC(0,1,1,1),
    MASKTOVEC(1,0,0,0),
    MASKTOVEC(1,0,0,1),
    MASKTOVEC(1,0,1,0),
    MASKTOVEC(1,0,1,1),
    MASKTOVEC(1,1,0,0),
    MASKTOVEC(1,1,0,1),
    MASKTOVEC(1,1,1,0),
    MASKTOVEC(1,1,1,1),
};

typedef void(*PFN_CLEAR_TILES)(DRAW_CONTEXT*, SWR_RENDERTARGET_ATTACHMENT rt, uint32_t, DWORD[4]);
static PFN_CLEAR_TILES sClearTilesTable[NUM_SWR_FORMATS];

//////////////////////////////////////////////////////////////////////////
/// @brief Process compute work.
/// @param pDC - pointer to draw context (dispatch).
/// @param workerId - The unique worker ID that is assigned to this thread.
/// @param threadGroupId - the linear index for the thread group within the dispatch.
void ProcessComputeBE(DRAW_CONTEXT* pDC, uint32_t workerId, uint32_t threadGroupId, void*& pSpillFillBuffer)
{
    RDTSC_START(BEDispatch);

    SWR_CONTEXT *pContext = pDC->pContext;

    const COMPUTE_DESC* pTaskData = (COMPUTE_DESC*)pDC->pDispatch->GetTasksData();
    SWR_ASSERT(pTaskData != nullptr);

    // Ensure spill fill memory has been allocated.
    size_t spillFillSize = pDC->pState->state.totalSpillFillSize;
    if (spillFillSize && pSpillFillBuffer == nullptr)
    {
        pSpillFillBuffer = pDC->pArena->AllocAlignedSync(spillFillSize, KNOB_SIMD_BYTES);
    }

    const API_STATE& state = GetApiState(pDC);

    SWR_CS_CONTEXT csContext{ 0 };
    csContext.tileCounter = threadGroupId;
    csContext.dispatchDims[0] = pTaskData->threadGroupCountX;
    csContext.dispatchDims[1] = pTaskData->threadGroupCountY;
    csContext.dispatchDims[2] = pTaskData->threadGroupCountZ;
    csContext.pTGSM = pContext->pScratch[workerId];
    csContext.pSpillFillBuffer = (uint8_t*)pSpillFillBuffer;

    state.pfnCsFunc(GetPrivateState(pDC), &csContext);

    UPDATE_STAT(CsInvocations, state.totalThreadsInGroup);

    RDTSC_STOP(BEDispatch, 1, 0);
}

void ProcessSyncBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pUserData)
{
    SYNC_DESC *pSync = (SYNC_DESC*)pUserData;

    uint32_t x, y;
    MacroTileMgr::getTileIndices(macroTile, x, y);
    SWR_ASSERT(x == 0 && y == 0);

    if (pSync->pfnCallbackFunc != nullptr)
    {
        pSync->pfnCallbackFunc(pSync->userData, pSync->userData2, pSync->userData3);
    }
}

void ProcessQueryStatsBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pUserData)
{
    QUERY_DESC* pQueryDesc = (QUERY_DESC*)pUserData;
    SWR_STATS* pStats = pQueryDesc->pStats;
    SWR_CONTEXT *pContext = pDC->pContext;

    SWR_ASSERT(pStats != nullptr);

    for (uint32_t i = 0; i < pContext->NumWorkerThreads; ++i)
    {
        pStats->DepthPassCount += pContext->stats[i].DepthPassCount;

        pStats->IaVertices    += pContext->stats[i].IaVertices;
        pStats->IaPrimitives  += pContext->stats[i].IaPrimitives;
        pStats->VsInvocations += pContext->stats[i].VsInvocations;
        pStats->HsInvocations += pContext->stats[i].HsInvocations;
        pStats->DsInvocations += pContext->stats[i].DsInvocations;
        pStats->GsInvocations += pContext->stats[i].GsInvocations;
        pStats->PsInvocations += pContext->stats[i].PsInvocations;
        pStats->CInvocations  += pContext->stats[i].CInvocations;
        pStats->CsInvocations += pContext->stats[i].CsInvocations;
        pStats->CPrimitives   += pContext->stats[i].CPrimitives;
        pStats->GsPrimitives  += pContext->stats[i].GsPrimitives;

        for (uint32_t stream = 0; stream < MAX_SO_STREAMS; ++stream)
        {
            pStats->SoWriteOffset[stream] += pContext->stats[i].SoWriteOffset[stream];

            /// @note client is required to provide valid write offset before every draw, so we clear
            /// out the contents of the write offset when storing stats
            pContext->stats[i].SoWriteOffset[stream] = 0;

            pStats->SoPrimStorageNeeded[stream] += pContext->stats[i].SoPrimStorageNeeded[stream];
            pStats->SoNumPrimsWritten[stream] += pContext->stats[i].SoNumPrimsWritten[stream];
        }
    }
}

template<SWR_FORMAT format>
void ClearRasterTile(uint8_t *pTileBuffer, simdvector &value)
{
    auto lambda = [&](int comp)
    {
        FormatTraits<format>::storeSOA(comp, pTileBuffer, value.v[comp]);
        pTileBuffer += (KNOB_SIMD_WIDTH * FormatTraits<format>::GetBPC(comp) / 8);
    };

    const uint32_t numIter = (KNOB_TILE_Y_DIM / SIMD_TILE_Y_DIM) * (KNOB_TILE_X_DIM / SIMD_TILE_X_DIM);
    for (uint32_t i = 0; i < numIter; ++i)
    {
        UnrollerL<0, FormatTraits<format>::numComps, 1>::step(lambda);
    }
}

template<SWR_FORMAT format>
INLINE void ClearMacroTile(DRAW_CONTEXT *pDC, SWR_RENDERTARGET_ATTACHMENT rt, uint32_t macroTile, DWORD clear[4])
{
    // convert clear color to hottile format
    // clear color is in RGBA float/uint32
    simdvector vClear;
    for (uint32_t comp = 0; comp < FormatTraits<format>::numComps; ++comp)
    {
        simdscalar vComp;
        vComp = _simd_load1_ps((const float*)&clear[comp]);
        if (FormatTraits<format>::isNormalized(comp))
        {
            vComp = _simd_mul_ps(vComp, _simd_set1_ps(FormatTraits<format>::fromFloat(comp)));
            vComp = _simd_castsi_ps(_simd_cvtps_epi32(vComp));
        }
        vComp = FormatTraits<format>::pack(comp, vComp);
        vClear.v[FormatTraits<format>::swizzle(comp)] = vComp;
    }

    uint32_t tileX, tileY;
    MacroTileMgr::getTileIndices(macroTile, tileX, tileY);
    const API_STATE& state = GetApiState(pDC);
    
    int top = KNOB_MACROTILE_Y_DIM_FIXED * tileY;
    int bottom = top + KNOB_MACROTILE_Y_DIM_FIXED - 1;
    int left = KNOB_MACROTILE_X_DIM_FIXED * tileX;
    int right = left + KNOB_MACROTILE_X_DIM_FIXED - 1;

    // intersect with scissor
    top = std::max(top, state.scissorInFixedPoint.top);
    left = std::max(left, state.scissorInFixedPoint.left);
    bottom = std::min(bottom, state.scissorInFixedPoint.bottom);
    right = std::min(right, state.scissorInFixedPoint.right);

    // translate to local hottile origin
    top -= KNOB_MACROTILE_Y_DIM_FIXED * tileY;
    bottom -= KNOB_MACROTILE_Y_DIM_FIXED * tileY;
    left -= KNOB_MACROTILE_X_DIM_FIXED * tileX;
    right -= KNOB_MACROTILE_X_DIM_FIXED * tileX;

    // convert to raster tiles
    top >>= (KNOB_TILE_Y_DIM_SHIFT + FIXED_POINT_SHIFT);
    bottom >>= (KNOB_TILE_Y_DIM_SHIFT + FIXED_POINT_SHIFT);
    left >>= (KNOB_TILE_X_DIM_SHIFT + FIXED_POINT_SHIFT);
    right >>= (KNOB_TILE_X_DIM_SHIFT + FIXED_POINT_SHIFT);

    const int numSamples = GetNumSamples(pDC->pState->state.rastState.sampleCount);
    // compute steps between raster tile samples / raster tiles / macro tile rows
    const uint32_t rasterTileSampleStep = KNOB_TILE_X_DIM * KNOB_TILE_Y_DIM * FormatTraits<format>::bpp / 8;
    const uint32_t rasterTileStep = (KNOB_TILE_X_DIM * KNOB_TILE_Y_DIM * (FormatTraits<format>::bpp / 8)) * numSamples;
    const uint32_t macroTileRowStep = (KNOB_MACROTILE_X_DIM / KNOB_TILE_X_DIM) * rasterTileStep;
    const uint32_t pitch = (FormatTraits<format>::bpp * KNOB_MACROTILE_X_DIM / 8);

    HOTTILE *pHotTile = pDC->pContext->pHotTileMgr->GetHotTile(pDC->pContext, pDC, macroTile, rt, true, numSamples);
    uint32_t rasterTileStartOffset = (ComputeTileOffset2D< TilingTraits<SWR_TILE_SWRZ, FormatTraits<format>::bpp > >(pitch, left, top)) * numSamples;
    uint8_t* pRasterTileRow = pHotTile->pBuffer + rasterTileStartOffset; //(ComputeTileOffset2D< TilingTraits<SWR_TILE_SWRZ, FormatTraits<format>::bpp > >(pitch, x, y)) * numSamples;

    // loop over all raster tiles in the current hot tile
    for (int y = top; y <= bottom; ++y)
    {
        uint8_t* pRasterTile = pRasterTileRow;
        for (int x = left; x <= right; ++x)
        {
            for( int sampleNum = 0; sampleNum < numSamples; sampleNum++)
            {
                ClearRasterTile<format>(pRasterTile, vClear);
                pRasterTile += rasterTileSampleStep;
            }
        }
        pRasterTileRow += macroTileRowStep;
    }

    pHotTile->state = HOTTILE_DIRTY;
}


void ProcessClearBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pUserData)
{
    if (KNOB_FAST_CLEAR)
    {
        CLEAR_DESC *pClear = (CLEAR_DESC*)pUserData;
        SWR_CONTEXT *pContext = pDC->pContext;
        SWR_MULTISAMPLE_COUNT sampleCount = pDC->pState->state.rastState.sampleCount;
        uint32_t numSamples = GetNumSamples(sampleCount);

        SWR_ASSERT(pClear->flags.bits != 0); // shouldn't be here without a reason.

        RDTSC_START(BEClear);

        if (pClear->flags.mask & SWR_CLEAR_COLOR)
        {
            HOTTILE *pHotTile = pContext->pHotTileMgr->GetHotTile(pContext, pDC, macroTile, SWR_ATTACHMENT_COLOR0, true, numSamples);
            // All we want to do here is to mark the hot tile as being in a "needs clear" state.
            pHotTile->clearData[0] = *(DWORD*)&(pClear->clearRTColor[0]);
            pHotTile->clearData[1] = *(DWORD*)&(pClear->clearRTColor[1]);
            pHotTile->clearData[2] = *(DWORD*)&(pClear->clearRTColor[2]);
            pHotTile->clearData[3] = *(DWORD*)&(pClear->clearRTColor[3]);
            pHotTile->state = HOTTILE_CLEAR;
        }

        if (pClear->flags.mask & SWR_CLEAR_DEPTH)
        {
            HOTTILE *pHotTile = pContext->pHotTileMgr->GetHotTile(pContext, pDC, macroTile, SWR_ATTACHMENT_DEPTH, true, numSamples);
            pHotTile->clearData[0] = *(DWORD*)&pClear->clearDepth;
            pHotTile->state = HOTTILE_CLEAR;
        }

        if (pClear->flags.mask & SWR_CLEAR_STENCIL)
        {
            HOTTILE *pHotTile = pContext->pHotTileMgr->GetHotTile(pContext, pDC, macroTile, SWR_ATTACHMENT_STENCIL, true, numSamples);

            pHotTile->clearData[0] = *(DWORD*)&pClear->clearStencil;
            pHotTile->state = HOTTILE_CLEAR;
        }

        RDTSC_STOP(BEClear, 0, 0);
    }
    else
    {
        // Legacy clear
        CLEAR_DESC *pClear = (CLEAR_DESC*)pUserData;
        RDTSC_START(BEClear);

        if (pClear->flags.mask & SWR_CLEAR_COLOR)
        {
            /// @todo clear data should come in as RGBA32_FLOAT
            DWORD clearData[4];
            float clearFloat[4];
            clearFloat[0] = ((uint8_t*)(&pClear->clearRTColor))[0] / 255.0f;
            clearFloat[1] = ((uint8_t*)(&pClear->clearRTColor))[1] / 255.0f;
            clearFloat[2] = ((uint8_t*)(&pClear->clearRTColor))[2] / 255.0f;
            clearFloat[3] = ((uint8_t*)(&pClear->clearRTColor))[3] / 255.0f;
            clearData[0] = *(DWORD*)&clearFloat[0];
            clearData[1] = *(DWORD*)&clearFloat[1];
            clearData[2] = *(DWORD*)&clearFloat[2];
            clearData[3] = *(DWORD*)&clearFloat[3];

            PFN_CLEAR_TILES pfnClearTiles = sClearTilesTable[KNOB_COLOR_HOT_TILE_FORMAT];
            SWR_ASSERT(pfnClearTiles != nullptr);

            pfnClearTiles(pDC, SWR_ATTACHMENT_COLOR0, macroTile, clearData);
        }

        if (pClear->flags.mask & SWR_CLEAR_DEPTH)
        {
            DWORD clearData[4];
            clearData[0] = *(DWORD*)&pClear->clearDepth;
            PFN_CLEAR_TILES pfnClearTiles = sClearTilesTable[KNOB_DEPTH_HOT_TILE_FORMAT];
            SWR_ASSERT(pfnClearTiles != nullptr);

            pfnClearTiles(pDC, SWR_ATTACHMENT_DEPTH, macroTile, clearData);
        }

        if (pClear->flags.mask & SWR_CLEAR_STENCIL)
        {
            uint32_t value = pClear->clearStencil;
            DWORD clearData[4];
            clearData[0] = *(DWORD*)&value;
            PFN_CLEAR_TILES pfnClearTiles = sClearTilesTable[KNOB_STENCIL_HOT_TILE_FORMAT];

            pfnClearTiles(pDC, SWR_ATTACHMENT_STENCIL, macroTile, clearData);
        }

        RDTSC_STOP(BEClear, 0, 0);
    }
}


void ProcessStoreTileBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pData)
{
    RDTSC_START(BEStoreTiles);
    STORE_TILES_DESC *pDesc = (STORE_TILES_DESC*)pData;
    SWR_CONTEXT *pContext = pDC->pContext;

#ifdef KNOB_ENABLE_RDTSC
    uint32_t numTiles = 0;
#endif
    SWR_FORMAT srcFormat;
    switch (pDesc->attachment)
    {
    case SWR_ATTACHMENT_COLOR0:
    case SWR_ATTACHMENT_COLOR1:
    case SWR_ATTACHMENT_COLOR2:
    case SWR_ATTACHMENT_COLOR3:
    case SWR_ATTACHMENT_COLOR4:
    case SWR_ATTACHMENT_COLOR5:
    case SWR_ATTACHMENT_COLOR6:
    case SWR_ATTACHMENT_COLOR7: srcFormat = KNOB_COLOR_HOT_TILE_FORMAT; break;
    case SWR_ATTACHMENT_DEPTH: srcFormat = KNOB_DEPTH_HOT_TILE_FORMAT; break;
    case SWR_ATTACHMENT_STENCIL: srcFormat = KNOB_STENCIL_HOT_TILE_FORMAT; break;
    default: SWR_ASSERT(false, "Unknown attachment: %d", pDesc->attachment); srcFormat = KNOB_COLOR_HOT_TILE_FORMAT; break;
    }

    uint32_t x, y;
    MacroTileMgr::getTileIndices(macroTile, x, y);

    // Only need to store the hottile if it's been rendered to...
    HOTTILE *pHotTile = pContext->pHotTileMgr->GetHotTile(pContext, pDC, macroTile, pDesc->attachment, false);
    if (pHotTile)
    {
        // clear if clear is pending (i.e., not rendered to), then mark as dirty for store.
        if (pHotTile->state == HOTTILE_CLEAR)
        {
            PFN_CLEAR_TILES pfnClearTiles = sClearTilesTable[srcFormat];
            SWR_ASSERT(pfnClearTiles != nullptr);

            pfnClearTiles(pDC, pDesc->attachment, macroTile, pHotTile->clearData);
        }

        if (pHotTile->state == HOTTILE_DIRTY || pDesc->postStoreTileState == (SWR_TILE_STATE)HOTTILE_DIRTY)
        {
            int destX = KNOB_MACROTILE_X_DIM * x;
            int destY = KNOB_MACROTILE_Y_DIM * y;

            pContext->pfnStoreTile(GetPrivateState(pDC), srcFormat,
                pDesc->attachment, destX, destY, pHotTile->renderTargetArrayIndex, pHotTile->pBuffer);
        }
        

        if (pHotTile->state == HOTTILE_DIRTY || pHotTile->state == HOTTILE_RESOLVED)
        {
            pHotTile->state = (HOTTILE_STATE)pDesc->postStoreTileState;
        }
    }
    RDTSC_STOP(BEStoreTiles, numTiles, pDC->drawId);
}


void ProcessDiscardInvalidateTilesBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pData)
{
    DISCARD_INVALIDATE_TILES_DESC *pDesc = (DISCARD_INVALIDATE_TILES_DESC *)pData;
    SWR_CONTEXT *pContext = pDC->pContext;

    const int numSamples = GetNumSamples(pDC->pState->state.rastState.sampleCount);

    for (uint32_t i = 0; i < SWR_NUM_ATTACHMENTS; ++i)
    {
        if (pDesc->attachmentMask & (1 << i))
        {
            HOTTILE *pHotTile = pContext->pHotTileMgr->GetHotTileNoLoad(
                pContext, pDC, macroTile, (SWR_RENDERTARGET_ATTACHMENT)i, pDesc->createNewTiles, numSamples);
            if (pHotTile)
            {
                pHotTile->state = (HOTTILE_STATE)pDesc->newTileState;
            }
        }
    }
}

#if KNOB_SIMD_WIDTH == 8
const __m256 vCenterOffsetsX = {0.5, 1.5, 0.5, 1.5, 2.5, 3.5, 2.5, 3.5};
const __m256 vCenterOffsetsY = {0.5, 0.5, 1.5, 1.5, 0.5, 0.5, 1.5, 1.5};
const __m256 vULOffsetsX = {0.0, 1.0, 0.0, 1.0, 2.0, 3.0, 2.0, 3.0};
const __m256 vULOffsetsY = {0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 1.0};
#else
#error Unsupported vector width
#endif

INLINE
bool CanEarlyZ(const SWR_PS_STATE *pPSState)
{
    return (pPSState->forceEarlyZ || (!pPSState->writesODepth && !pPSState->usesSourceDepth && !pPSState->usesUAV));
}

simdmask ComputeUserClipMask(uint8_t clipMask, float* pUserClipBuffer, simdscalar vI, simdscalar vJ)
{
    simdscalar vClipMask = _simd_setzero_ps();
    uint32_t numClipDistance = _mm_popcnt_u32(clipMask);

    for (uint32_t i = 0; i < numClipDistance; ++i)
    {
        // pull triangle clip distance values from clip buffer
        simdscalar vA = _simd_broadcast_ss(pUserClipBuffer++);
        simdscalar vB = _simd_broadcast_ss(pUserClipBuffer++);
        simdscalar vC = _simd_broadcast_ss(pUserClipBuffer++);

        // interpolate
        simdscalar vInterp = vplaneps(vA, vB, vC, vI, vJ);
        
        // clip if interpolated clip distance is < 0 || NAN
        simdscalar vCull = _simd_cmp_ps(_simd_setzero_ps(), vInterp, _CMP_NLE_UQ);

        vClipMask = _simd_or_ps(vClipMask, vCull);
    }

    return _simd_movemask_ps(vClipMask);
}

template<bool bGenerateBarycentrics>
INLINE void CalcPixelBarycentrics(const BarycentricCoeffs& coeffs, SWR_PS_CONTEXT &psContext)
{
    if(bGenerateBarycentrics)
    {
        // evaluate I,J
        psContext.vI.center = vplaneps(coeffs.vIa, coeffs.vIb, coeffs.vIc, psContext.vX.center, psContext.vY.center);
        psContext.vJ.center = vplaneps(coeffs.vJa, coeffs.vJb, coeffs.vJc, psContext.vX.center, psContext.vY.center);
        psContext.vI.center = _simd_mul_ps(psContext.vI.center, coeffs.vRecipDet);
        psContext.vJ.center = _simd_mul_ps(psContext.vJ.center, coeffs.vRecipDet);

        // interpolate 1/w
        psContext.vOneOverW.center = vplaneps(coeffs.vAOneOverW, coeffs.vBOneOverW, coeffs.vCOneOverW, psContext.vI.center, psContext.vJ.center);
    }
}

template<bool bGenerateBarycentrics>
INLINE void CalcSampleBarycentrics(const BarycentricCoeffs& coeffs, SWR_PS_CONTEXT &psContext)
{
    if(bGenerateBarycentrics)
    {
        // evaluate I,J
        psContext.vI.sample = vplaneps(coeffs.vIa, coeffs.vIb, coeffs.vIc, psContext.vX.sample, psContext.vY.sample);
        psContext.vJ.sample = vplaneps(coeffs.vJa, coeffs.vJb, coeffs.vJc, psContext.vX.sample, psContext.vY.sample);
        psContext.vI.sample = _simd_mul_ps(psContext.vI.sample, coeffs.vRecipDet);
        psContext.vJ.sample = _simd_mul_ps(psContext.vJ.sample, coeffs.vRecipDet);

        // interpolate 1/w
        psContext.vOneOverW.sample = vplaneps(coeffs.vAOneOverW, coeffs.vBOneOverW, coeffs.vCOneOverW, psContext.vI.sample, psContext.vJ.sample);
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Centroid behaves exactly as follows :
// (1) If all samples in the primitive are covered, the attribute is evaluated at the pixel center (even if the sample pattern does not happen to 
//     have a sample location there).
// (2) Else the attribute is evaluated at the first covered sample, in increasing order of sample index, where sample coverage is after ANDing the 
//     coverage with the SampleMask Rasterizer State.
// (3) If no samples are covered, such as on helper pixels executed off the bounds of a primitive to fill out 2x2 pixel stamps, the attribute is 
//     evaluated as follows : If the SampleMask Rasterizer state is a subset of the samples in the pixel, then the first sample covered by the 
//     SampleMask Rasterizer State is the evaluation point.Otherwise (full SampleMask), the pixel center is the evaluation point.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template<typename T>
INLINE void CalcCentroidPos(SWR_PS_CONTEXT &psContext, const uint64_t *const coverageMask, const uint32_t sampleMask,
                            const simdscalar vXSamplePosUL, const simdscalar vYSamplePosUL)
{
    uint32_t inputMask[KNOB_SIMD_WIDTH];
    generateInputCoverage<T>(coverageMask, inputMask, sampleMask);

    // Case (2) - partially covered pixel

    // scan for first covered sample per pixel in the 4x2 span
    unsigned long sampleNum[KNOB_SIMD_WIDTH];
    (inputMask[0] > 0) ? (_BitScanForward(&sampleNum[0], inputMask[0])) : (sampleNum[0] = 0);
    (inputMask[1] > 0) ? (_BitScanForward(&sampleNum[1], inputMask[1])) : (sampleNum[1] = 0);
    (inputMask[2] > 0) ? (_BitScanForward(&sampleNum[2], inputMask[2])) : (sampleNum[2] = 0);
    (inputMask[3] > 0) ? (_BitScanForward(&sampleNum[3], inputMask[3])) : (sampleNum[3] = 0);
    (inputMask[4] > 0) ? (_BitScanForward(&sampleNum[4], inputMask[4])) : (sampleNum[4] = 0);
    (inputMask[5] > 0) ? (_BitScanForward(&sampleNum[5], inputMask[5])) : (sampleNum[5] = 0);
    (inputMask[6] > 0) ? (_BitScanForward(&sampleNum[6], inputMask[6])) : (sampleNum[6] = 0);
    (inputMask[7] > 0) ? (_BitScanForward(&sampleNum[7], inputMask[7])) : (sampleNum[7] = 0);

    // look up and set the sample offsets from UL pixel corner for first covered sample 
    __m256 vXSample = _mm256_set_ps(T::MultisampleT::X(sampleNum[7]),
                                    T::MultisampleT::X(sampleNum[6]),
                                    T::MultisampleT::X(sampleNum[5]),
                                    T::MultisampleT::X(sampleNum[4]),
                                    T::MultisampleT::X(sampleNum[3]),
                                    T::MultisampleT::X(sampleNum[2]),
                                    T::MultisampleT::X(sampleNum[1]),
                                    T::MultisampleT::X(sampleNum[0]));

    __m256 vYSample = _mm256_set_ps(T::MultisampleT::Y(sampleNum[7]),
                                    T::MultisampleT::Y(sampleNum[6]),
                                    T::MultisampleT::Y(sampleNum[5]),
                                    T::MultisampleT::Y(sampleNum[4]),
                                    T::MultisampleT::Y(sampleNum[3]),
                                    T::MultisampleT::Y(sampleNum[2]),
                                    T::MultisampleT::Y(sampleNum[1]),
                                    T::MultisampleT::Y(sampleNum[0]));
    // add sample offset to UL pixel corner
    vXSample = _simd_add_ps(vXSamplePosUL, vXSample);
    vYSample = _simd_add_ps(vYSamplePosUL, vYSample);

    // Case (1) and case (3b) - All samples covered or not covered with full SampleMask
    static const __m256i vFullyCoveredMask = T::MultisampleT::FullSampleMask();
    __m256i vInputCoveragei =  _mm256_set_epi32(inputMask[7], inputMask[6], inputMask[5], inputMask[4], inputMask[3], inputMask[2], inputMask[1], inputMask[0]);
    __m256i vAllSamplesCovered = _simd_cmpeq_epi32(vInputCoveragei, vFullyCoveredMask);

    static const __m256i vZero = _simd_setzero_si();
    const __m256i vSampleMask = _simd_and_si(_simd_set1_epi32(sampleMask), vFullyCoveredMask);
    __m256i vNoSamplesCovered = _simd_cmpeq_epi32(vInputCoveragei, vZero);
    __m256i vIsFullSampleMask = _simd_cmpeq_epi32(vSampleMask, vFullyCoveredMask);
    __m256i vCase3b = _simd_and_si(vNoSamplesCovered, vIsFullSampleMask);

    __m256i vEvalAtCenter = _simd_or_si(vAllSamplesCovered, vCase3b);

    // set the centroid position based on results from above
    psContext.vX.centroid = _simd_blendv_ps(vXSample, psContext.vX.center, _simd_castsi_ps(vEvalAtCenter));
    psContext.vY.centroid = _simd_blendv_ps(vYSample, psContext.vY.center, _simd_castsi_ps(vEvalAtCenter));

    // Case (3a) No samples covered and partial sample mask
    __m256i vSomeSampleMaskSamples = _simd_cmplt_epi32(vSampleMask, vFullyCoveredMask);
    // sample mask should never be all 0's for this case, but handle it anyways
    unsigned long firstCoveredSampleMaskSample = 0;
    (sampleMask > 0) ? (_BitScanForward(&firstCoveredSampleMaskSample, sampleMask)) : (firstCoveredSampleMaskSample = 0);

    __m256i vCase3a = _simd_and_si(vNoSamplesCovered, vSomeSampleMaskSamples);

    vXSample = _simd_set1_ps(T::MultisampleT::X(firstCoveredSampleMaskSample));
    vYSample = _simd_set1_ps(T::MultisampleT::Y(firstCoveredSampleMaskSample));

    // blend in case 3a pixel locations
    psContext.vX.centroid = _simd_blendv_ps(psContext.vX.centroid, vXSample, _simd_castsi_ps(vCase3a));
    psContext.vY.centroid = _simd_blendv_ps(psContext.vY.centroid, vYSample, _simd_castsi_ps(vCase3a));
}

template<typename T>
INLINE void CalcCentroidBarycentrics(const BarycentricCoeffs& coeffs, SWR_PS_CONTEXT &psContext,
                                     const uint64_t *const coverageMask, const uint32_t sampleMask,
                                     const simdscalar vXSamplePosUL, const simdscalar vYSamplePosUL)
{
    if(T::bIsStandardPattern)
    {
        ///@ todo: don't need to generate input coverage 2x if input coverage and centroid
        CalcCentroidPos<T>(psContext, coverageMask, sampleMask, vXSamplePosUL, vYSamplePosUL);
    }
    else
    {
        static const __m256 pixelCenter = _simd_set1_ps(0.5f);
        psContext.vX.centroid = _simd_add_ps(vXSamplePosUL, pixelCenter);
        psContext.vY.centroid = _simd_add_ps(vYSamplePosUL, pixelCenter);
    }
    // evaluate I,J
    psContext.vI.centroid = vplaneps(coeffs.vIa, coeffs.vIb, coeffs.vIc, psContext.vX.centroid, psContext.vY.centroid);
    psContext.vJ.centroid = vplaneps(coeffs.vJa, coeffs.vJb, coeffs.vJc, psContext.vX.centroid, psContext.vY.centroid);
    psContext.vI.centroid = _simd_mul_ps(psContext.vI.centroid, coeffs.vRecipDet);
    psContext.vJ.centroid = _simd_mul_ps(psContext.vJ.centroid, coeffs.vRecipDet);

    // interpolate 1/w
    psContext.vOneOverW.centroid = vplaneps(coeffs.vAOneOverW, coeffs.vBOneOverW, coeffs.vCOneOverW, psContext.vI.centroid, psContext.vJ.centroid);
}

template<uint32_t NumRT, uint32_t sampleCountT>
void OutputMerger(SWR_PS_CONTEXT &psContext, uint8_t* (&pColorBase)[SWR_NUM_RENDERTARGETS], uint32_t sample, const SWR_BLEND_STATE *pBlendState,
                  const PFN_BLEND_JIT_FUNC (&pfnBlendFunc)[SWR_NUM_RENDERTARGETS], simdscalar &coverageMask, simdscalar depthPassMask)
{
    // type safety guaranteed from template instantiation in BEChooser<>::GetFunc
    static const SWR_MULTISAMPLE_COUNT sampleCount = (SWR_MULTISAMPLE_COUNT)sampleCountT;
    uint32_t rasterTileColorOffset = MultisampleTraits<sampleCount>::RasterTileColorOffset(sample);
    simdvector blendOut;

    for(uint32_t rt = 0; rt < NumRT; ++rt)
    {
        uint8_t *pColorSample;
        if(sampleCount == SWR_MULTISAMPLE_1X)
        {
            pColorSample = pColorBase[rt];
        }
        else
        {
            pColorSample = pColorBase[rt] + rasterTileColorOffset;
        }

        const SWR_RENDER_TARGET_BLEND_STATE *pRTBlend = &pBlendState->renderTarget[rt];
        // pfnBlendFunc may not update all channels.  Initialize with PS output.
        /// TODO: move this into the blend JIT.
        blendOut = psContext.shaded[rt];

        // Blend outputs and update coverage mask for alpha test
        if(pfnBlendFunc[rt] != nullptr)
        {
            pfnBlendFunc[rt](
                pBlendState,
                psContext.shaded[rt],
                psContext.shaded[1],
                sample,
                pColorSample,
                blendOut,
                &psContext.oMask,
                (simdscalari*)&coverageMask);
        }

        // final write mask 
        simdscalari outputMask = _simd_castps_si(_simd_and_ps(coverageMask, depthPassMask));

        ///@todo can only use maskstore fast path if bpc is 32. Assuming hot tile is RGBA32_FLOAT.
        static_assert(KNOB_COLOR_HOT_TILE_FORMAT == R32G32B32A32_FLOAT, "Unsupported hot tile format");

        const uint32_t simd = KNOB_SIMD_WIDTH * sizeof(float);

        // store with color mask
        if(!pRTBlend->writeDisableRed)
        {
            _simd_maskstore_ps((float*)pColorSample, outputMask, blendOut.x);
        }
        if(!pRTBlend->writeDisableGreen)
        {
            _simd_maskstore_ps((float*)(pColorSample + simd), outputMask, blendOut.y);
        }
        if(!pRTBlend->writeDisableBlue)
        {
            _simd_maskstore_ps((float*)(pColorSample + simd * 2), outputMask, blendOut.z);
        }
        if(!pRTBlend->writeDisableAlpha)
        {
            _simd_maskstore_ps((float*)(pColorSample + simd * 3), outputMask, blendOut.w);
        }
    }
}

template<typename T>
void BackendSingleSample(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t x, uint32_t y, SWR_TRIANGLE_DESC &work, RenderOutputBuffers &renderBuffers)
{
    RDTSC_START(BESetup);

    SWR_CONTEXT *pContext = pDC->pContext;
    const API_STATE& state = GetApiState(pDC);
    const SWR_RASTSTATE& rastState = state.rastState;
    const SWR_PS_STATE *pPSState = &state.psState;
    const SWR_BLEND_STATE *pBlendState = &state.blendState;
    const BACKEND_FUNCS& backendFuncs = pDC->pState->backendFuncs;
    uint64_t coverageMask = work.coverageMask[0];

    // broadcast scalars
    BarycentricCoeffs coeffs;
    coeffs.vIa = _simd_broadcast_ss(&work.I[0]);
    coeffs.vIb = _simd_broadcast_ss(&work.I[1]);
    coeffs.vIc = _simd_broadcast_ss(&work.I[2]);

    coeffs.vJa = _simd_broadcast_ss(&work.J[0]);
    coeffs.vJb = _simd_broadcast_ss(&work.J[1]);
    coeffs.vJc = _simd_broadcast_ss(&work.J[2]);

    coeffs.vZa = _simd_broadcast_ss(&work.Z[0]);
    coeffs.vZb = _simd_broadcast_ss(&work.Z[1]);
    coeffs.vZc = _simd_broadcast_ss(&work.Z[2]);

    coeffs.vRecipDet = _simd_broadcast_ss(&work.recipDet);

    coeffs.vAOneOverW = _simd_broadcast_ss(&work.OneOverW[0]);
    coeffs.vBOneOverW = _simd_broadcast_ss(&work.OneOverW[1]);
    coeffs.vCOneOverW = _simd_broadcast_ss(&work.OneOverW[2]);

    uint8_t *pColorBase[SWR_NUM_RENDERTARGETS];
    uint32_t NumRT = state.psState.numRenderTargets;
    for(uint32_t rt = 0; rt < NumRT; ++rt)
    {
        pColorBase[rt] = renderBuffers.pColor[rt];
    }
    uint8_t *pDepthBase = renderBuffers.pDepth, *pStencilBase = renderBuffers.pStencil;
    RDTSC_STOP(BESetup, 0, 0);

    SWR_PS_CONTEXT psContext;
    psContext.pAttribs = work.pAttribs;
    psContext.pPerspAttribs = work.pPerspAttribs;
    psContext.frontFace = work.triFlags.frontFacing;
    psContext.primID = work.triFlags.primID;

    // save Ia/Ib/Ic and Ja/Jb/Jc if we need to reevaluate i/j/k in the shader because of pull attribs
    psContext.I = work.I;
    psContext.J = work.J;
    psContext.recipDet = work.recipDet;
    psContext.pRecipW = work.pRecipW;
    psContext.pSamplePosX = (const float*)&T::MultisampleT::samplePosX;
    psContext.pSamplePosY = (const float*)&T::MultisampleT::samplePosY;

    for(uint32_t yy = y; yy < y + KNOB_TILE_Y_DIM; yy += SIMD_TILE_Y_DIM)
    {
        // UL pixel corner
        psContext.vY.UL = _simd_add_ps(vULOffsetsY, _simd_set1_ps((float)yy));
        // pixel center
        psContext.vY.center = _simd_add_ps(vCenterOffsetsY, _simd_set1_ps((float)yy));

        for(uint32_t xx = x; xx < x + KNOB_TILE_X_DIM; xx += SIMD_TILE_X_DIM)
        {
            if(T::bInputCoverage)
            {
                generateInputCoverage<T>(&work.coverageMask[0], psContext.inputMask, pBlendState->sampleMask);
            }

            if(coverageMask & MASK)
            {
                RDTSC_START(BEBarycentric);
                psContext.vX.UL = _simd_add_ps(vULOffsetsX, _simd_set1_ps((float)xx));
                // pixel center
                psContext.vX.center = _simd_add_ps(vCenterOffsetsX, _simd_set1_ps((float)xx));

                backendFuncs.pfnCalcPixelBarycentrics(coeffs, psContext);

                if(T::bCentroidPos)
                {
                    // for 1x case, centroid is pixel center
                    psContext.vX.centroid = psContext.vX.center;
                    psContext.vY.centroid = psContext.vY.center;
                    psContext.vI.centroid = psContext.vI.center;
                    psContext.vJ.centroid = psContext.vJ.center;
                    psContext.vOneOverW.centroid = psContext.vOneOverW.center;
                }

                // interpolate and quantize z
                psContext.vZ = vplaneps(coeffs.vZa, coeffs.vZb, coeffs.vZc, psContext.vI.center, psContext.vJ.center);
                psContext.vZ = state.pfnQuantizeDepth(psContext.vZ);

                RDTSC_STOP(BEBarycentric, 0, 0);

                simdmask clipCoverageMask = coverageMask & MASK;

                // interpolate user clip distance if available
                if(rastState.clipDistanceMask)
                {
                    clipCoverageMask &= ~ComputeUserClipMask(rastState.clipDistanceMask, work.pUserClipBuffer,
                                                             psContext.vI.center, psContext.vJ.center);
                }

                simdscalar vCoverageMask = vMask(clipCoverageMask);
                simdscalar depthPassMask = vCoverageMask;
                simdscalar stencilPassMask = vCoverageMask;

                // Early-Z?
                if(CanEarlyZ(pPSState))
                {
                    RDTSC_START(BEEarlyDepthTest);
                    depthPassMask = DepthStencilTest(&state, work.triFlags.frontFacing,
                                                        psContext.vZ, pDepthBase, vCoverageMask, pStencilBase, &stencilPassMask);
                    RDTSC_STOP(BEEarlyDepthTest, 0, 0);

                    // early-exit if no pixels passed depth or earlyZ is forced on
                    if(pPSState->forceEarlyZ || !_simd_movemask_ps(depthPassMask))
                    {
                        DepthStencilWrite(&state.vp[0], &state.depthStencilState, work.triFlags.frontFacing, psContext.vZ,
                                            pDepthBase, depthPassMask, vCoverageMask, pStencilBase, stencilPassMask);

                        if (!_simd_movemask_ps(depthPassMask))
                        {
                            goto Endtile;
                        }
                    }
                }

                psContext.sampleIndex = 0;
                psContext.activeMask = _simd_castps_si(vCoverageMask);

                // execute pixel shader
                RDTSC_START(BEPixelShader);
                UPDATE_STAT(PsInvocations, _mm_popcnt_u32(_simd_movemask_ps(vCoverageMask)));
                state.psState.pfnPixelShader(GetPrivateState(pDC), &psContext);
                RDTSC_STOP(BEPixelShader, 0, 0);

                vCoverageMask = _simd_castsi_ps(psContext.activeMask);

                // late-Z
                if(!CanEarlyZ(pPSState))
                {
                    RDTSC_START(BELateDepthTest);
                    depthPassMask = DepthStencilTest(&state, work.triFlags.frontFacing,
                                                        psContext.vZ, pDepthBase, vCoverageMask, pStencilBase, &stencilPassMask);
                    RDTSC_STOP(BELateDepthTest, 0, 0);

                    if(!_simd_movemask_ps(depthPassMask))
                    {
                        // need to call depth/stencil write for stencil write
                        DepthStencilWrite(&state.vp[0], &state.depthStencilState, work.triFlags.frontFacing, psContext.vZ,
                                            pDepthBase, depthPassMask, vCoverageMask, pStencilBase, stencilPassMask);
                        goto Endtile;
                    }
                }

                uint32_t statMask = _simd_movemask_ps(depthPassMask);
                uint32_t statCount = _mm_popcnt_u32(statMask);
                UPDATE_STAT(DepthPassCount, statCount);

                // output merger
                RDTSC_START(BEOutputMerger);
                backendFuncs.pfnOutputMerger(psContext, pColorBase, 0, pBlendState, state.pfnBlendFunc,
                                             vCoverageMask, depthPassMask);

                // do final depth write after all pixel kills
                if (!pPSState->forceEarlyZ)
                {
                    DepthStencilWrite(&state.vp[0], &state.depthStencilState, work.triFlags.frontFacing, psContext.vZ,
                        pDepthBase, depthPassMask, vCoverageMask, pStencilBase, stencilPassMask);
                }
                RDTSC_STOP(BEOutputMerger, 0, 0);
            }

Endtile:
            RDTSC_START(BEEndTile);
            coverageMask >>= (SIMD_TILE_Y_DIM * SIMD_TILE_X_DIM);
            pDepthBase += (KNOB_SIMD_WIDTH * FormatTraits<KNOB_DEPTH_HOT_TILE_FORMAT>::bpp) / 8;
            pStencilBase += (KNOB_SIMD_WIDTH * FormatTraits<KNOB_STENCIL_HOT_TILE_FORMAT>::bpp) / 8;

            for(uint32_t rt = 0; rt < NumRT; ++rt)
            {
                pColorBase[rt] += (KNOB_SIMD_WIDTH * FormatTraits<KNOB_COLOR_HOT_TILE_FORMAT>::bpp) / 8;
            }
            RDTSC_STOP(BEEndTile, 0, 0);
        }
    }
}

template<typename T>
void BackendSampleRate(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t x, uint32_t y, SWR_TRIANGLE_DESC &work, RenderOutputBuffers &renderBuffers)
{
    RDTSC_START(BESetup);

    SWR_CONTEXT *pContext = pDC->pContext;
    const API_STATE& state = GetApiState(pDC);
    const SWR_RASTSTATE& rastState = state.rastState;
    const SWR_PS_STATE *pPSState = &state.psState;
    const SWR_BLEND_STATE *pBlendState = &state.blendState;
    const BACKEND_FUNCS& backendFuncs = pDC->pState->backendFuncs;

    // broadcast scalars
    BarycentricCoeffs coeffs;
    coeffs.vIa = _simd_broadcast_ss(&work.I[0]);
    coeffs.vIb = _simd_broadcast_ss(&work.I[1]);
    coeffs.vIc = _simd_broadcast_ss(&work.I[2]);

    coeffs.vJa = _simd_broadcast_ss(&work.J[0]);
    coeffs.vJb = _simd_broadcast_ss(&work.J[1]);
    coeffs.vJc = _simd_broadcast_ss(&work.J[2]);

    coeffs.vZa = _simd_broadcast_ss(&work.Z[0]);
    coeffs.vZb = _simd_broadcast_ss(&work.Z[1]);
    coeffs.vZc = _simd_broadcast_ss(&work.Z[2]);

    coeffs.vRecipDet = _simd_broadcast_ss(&work.recipDet);

    coeffs.vAOneOverW = _simd_broadcast_ss(&work.OneOverW[0]);
    coeffs.vBOneOverW = _simd_broadcast_ss(&work.OneOverW[1]);
    coeffs.vCOneOverW = _simd_broadcast_ss(&work.OneOverW[2]);

    uint8_t *pColorBase[SWR_NUM_RENDERTARGETS];
    uint32_t NumRT = state.psState.numRenderTargets;
    for(uint32_t rt = 0; rt < NumRT; ++rt)
    {
        pColorBase[rt] = renderBuffers.pColor[rt];
    }
    uint8_t *pDepthBase = renderBuffers.pDepth, *pStencilBase = renderBuffers.pStencil;
    RDTSC_STOP(BESetup, 0, 0);

    SWR_PS_CONTEXT psContext;
    psContext.pAttribs = work.pAttribs;
    psContext.pPerspAttribs = work.pPerspAttribs;
    psContext.pRecipW = work.pRecipW;
    psContext.frontFace = work.triFlags.frontFacing;
    psContext.primID = work.triFlags.primID;

    // save Ia/Ib/Ic and Ja/Jb/Jc if we need to reevaluate i/j/k in the shader because of pull attribs
    psContext.I = work.I;
    psContext.J = work.J;
    psContext.recipDet = work.recipDet;
    psContext.pSamplePosX = (const float*)&T::MultisampleT::samplePosX;
    psContext.pSamplePosY = (const float*)&T::MultisampleT::samplePosY;
    const uint32_t numSamples = T::MultisampleT::numSamples;

    for (uint32_t yy = y; yy < y + KNOB_TILE_Y_DIM; yy += SIMD_TILE_Y_DIM)
    {
        // UL pixel corner
        psContext.vY.UL = _simd_add_ps(vULOffsetsY, _simd_set1_ps((float)yy));
        // pixel center
        psContext.vY.center = _simd_add_ps(vCenterOffsetsY, _simd_set1_ps((float)yy));
        
        for (uint32_t xx = x; xx < x + KNOB_TILE_X_DIM; xx += SIMD_TILE_X_DIM)
        {
            psContext.vX.UL = _simd_add_ps(vULOffsetsX, _simd_set1_ps((float)xx));
            // pixel center
            psContext.vX.center = _simd_add_ps(vCenterOffsetsX, _simd_set1_ps((float)xx));

            RDTSC_START(BEBarycentric);
            backendFuncs.pfnCalcPixelBarycentrics(coeffs, psContext);
            RDTSC_STOP(BEBarycentric, 0, 0);

            if(T::bInputCoverage)
            {
                generateInputCoverage<T>(&work.coverageMask[0], psContext.inputMask, pBlendState->sampleMask);
            }

            if(T::bCentroidPos)
            {
                ///@ todo: don't need to genererate input coverage 2x if input coverage and centroid
                RDTSC_START(BEBarycentric);
                CalcCentroidBarycentrics<T>(coeffs, psContext, &work.coverageMask[0], pBlendState->sampleMask, psContext.vX.UL, psContext.vY.UL);
                RDTSC_STOP(BEBarycentric, 0, 0);
            }

            for(uint32_t sample = 0; sample < numSamples; sample++)
            {
                if (work.coverageMask[sample] & MASK)
                {
                    RDTSC_START(BEBarycentric);

                    // calculate per sample positions
                    psContext.vX.sample = _simd_add_ps(psContext.vX.UL, T::MultisampleT::vX(sample));
                    psContext.vY.sample = _simd_add_ps(psContext.vY.UL, T::MultisampleT::vY(sample));
                    
                    simdmask coverageMask = work.coverageMask[sample] & MASK;
                    simdscalar vCoverageMask = vMask(coverageMask);

                    backendFuncs.pfnCalcSampleBarycentrics(coeffs, psContext);

                    // interpolate and quantize z
                    psContext.vZ = vplaneps(coeffs.vZa, coeffs.vZb, coeffs.vZc, psContext.vI.sample, psContext.vJ.sample);
                    psContext.vZ = state.pfnQuantizeDepth(psContext.vZ);

                    RDTSC_STOP(BEBarycentric, 0, 0);

                    // interpolate user clip distance if available
                    if (rastState.clipDistanceMask)
                    {
                        coverageMask &= ~ComputeUserClipMask(rastState.clipDistanceMask, work.pUserClipBuffer,
                            psContext.vI.sample, psContext.vJ.sample);
                    }
                    
                    simdscalar depthPassMask = vCoverageMask;
                    simdscalar stencilPassMask = vCoverageMask;

                    // offset depth/stencil buffers current sample
                    uint8_t *pDepthSample = pDepthBase + T::MultisampleT::RasterTileDepthOffset(sample);
                    uint8_t *pStencilSample = pStencilBase + T::MultisampleT::RasterTileStencilOffset(sample);

                    // Early-Z?
                    if (CanEarlyZ(pPSState))
                    {
                        RDTSC_START(BEEarlyDepthTest);
                        depthPassMask = DepthStencilTest(&state, work.triFlags.frontFacing,
                                              psContext.vZ, pDepthSample, vCoverageMask, pStencilSample, &stencilPassMask);
                        RDTSC_STOP(BEEarlyDepthTest, 0, 0);

                        // early-exit if no samples passed depth or earlyZ is forced on.
                        if (pPSState->forceEarlyZ || !_simd_movemask_ps(depthPassMask))
                        {
                            DepthStencilWrite(&state.vp[0], &state.depthStencilState, work.triFlags.frontFacing, psContext.vZ,
                                pDepthSample, depthPassMask, vCoverageMask, pStencilSample, stencilPassMask);

                            if (!_simd_movemask_ps(depthPassMask))
                            {
                                work.coverageMask[sample] >>= (SIMD_TILE_Y_DIM * SIMD_TILE_X_DIM);
                                continue;
                            }
                        }
                    }

                    psContext.sampleIndex = sample;
                    psContext.activeMask = _simd_castps_si(vCoverageMask);

                    // execute pixel shader
                    RDTSC_START(BEPixelShader);
                    UPDATE_STAT(PsInvocations, _mm_popcnt_u32(_simd_movemask_ps(vCoverageMask)));
                    state.psState.pfnPixelShader(GetPrivateState(pDC), &psContext);
                    RDTSC_STOP(BEPixelShader, 0, 0);

                    vCoverageMask = _simd_castsi_ps(psContext.activeMask);

                    // late-Z
                    if (!CanEarlyZ(pPSState))
                    {
                        RDTSC_START(BELateDepthTest);
                        depthPassMask = DepthStencilTest(&state, work.triFlags.frontFacing,
                                              psContext.vZ, pDepthSample, vCoverageMask, pStencilSample, &stencilPassMask);
                        RDTSC_STOP(BELateDepthTest, 0, 0);

                        if (!_simd_movemask_ps(depthPassMask))
                        {
                            // need to call depth/stencil write for stencil write
                            DepthStencilWrite(&state.vp[0], &state.depthStencilState, work.triFlags.frontFacing, psContext.vZ,
                                pDepthSample, depthPassMask, vCoverageMask, pStencilSample, stencilPassMask);

                            work.coverageMask[sample] >>= (SIMD_TILE_Y_DIM * SIMD_TILE_X_DIM);
                            continue;
                        }
                    }

                    uint32_t statMask = _simd_movemask_ps(depthPassMask);
                    uint32_t statCount = _mm_popcnt_u32(statMask);
                    UPDATE_STAT(DepthPassCount, statCount);

                    // output merger
                    RDTSC_START(BEOutputMerger);
                    backendFuncs.pfnOutputMerger(psContext, pColorBase, sample, pBlendState, state.pfnBlendFunc, 
                                                 vCoverageMask, depthPassMask);

                    // do final depth write after all pixel kills
                    if (!pPSState->forceEarlyZ)
                    {
                        DepthStencilWrite(&state.vp[0], &state.depthStencilState, work.triFlags.frontFacing, psContext.vZ,
                            pDepthSample, depthPassMask, vCoverageMask, pStencilSample, stencilPassMask);
                    }
                    RDTSC_STOP(BEOutputMerger, 0, 0);
                }
                work.coverageMask[sample] >>= (SIMD_TILE_Y_DIM * SIMD_TILE_X_DIM);
            }
            RDTSC_START(BEEndTile);
            pDepthBase += (KNOB_SIMD_WIDTH * FormatTraits<KNOB_DEPTH_HOT_TILE_FORMAT>::bpp) / 8;
            pStencilBase += (KNOB_SIMD_WIDTH * FormatTraits<KNOB_STENCIL_HOT_TILE_FORMAT>::bpp) / 8;

            for (uint32_t rt = 0; rt < NumRT; ++rt)
            {
                pColorBase[rt] += (KNOB_SIMD_WIDTH * FormatTraits<KNOB_COLOR_HOT_TILE_FORMAT>::bpp) / 8;
            }
            RDTSC_STOP(BEEndTile, 0, 0);
        }
    }
}

template<typename T>
void BackendPixelRate(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t x, uint32_t y, SWR_TRIANGLE_DESC &work, RenderOutputBuffers &renderBuffers)
{
    RDTSC_START(BESetup);

    SWR_CONTEXT *pContext = pDC->pContext;
    const API_STATE& state = GetApiState(pDC);
    const SWR_RASTSTATE& rastState = state.rastState;
    const SWR_PS_STATE *pPSState = &state.psState;
    const SWR_BLEND_STATE *pBlendState = &state.blendState;
    const BACKEND_FUNCS& backendFuncs = pDC->pState->backendFuncs;

    // broadcast scalars
    BarycentricCoeffs coeffs;
    coeffs.vIa = _simd_broadcast_ss(&work.I[0]);
    coeffs.vIb = _simd_broadcast_ss(&work.I[1]);
    coeffs.vIc = _simd_broadcast_ss(&work.I[2]);

    coeffs.vJa = _simd_broadcast_ss(&work.J[0]);
    coeffs.vJb = _simd_broadcast_ss(&work.J[1]);
    coeffs.vJc = _simd_broadcast_ss(&work.J[2]);

    coeffs.vZa = _simd_broadcast_ss(&work.Z[0]);
    coeffs.vZb = _simd_broadcast_ss(&work.Z[1]);
    coeffs.vZc = _simd_broadcast_ss(&work.Z[2]);

    coeffs.vRecipDet = _simd_broadcast_ss(&work.recipDet);

    coeffs.vAOneOverW = _simd_broadcast_ss(&work.OneOverW[0]);
    coeffs.vBOneOverW = _simd_broadcast_ss(&work.OneOverW[1]);
    coeffs.vCOneOverW = _simd_broadcast_ss(&work.OneOverW[2]);

    uint8_t *pColorBase[SWR_NUM_RENDERTARGETS];
    uint32_t NumRT = state.psState.numRenderTargets;
    for(uint32_t rt = 0; rt < NumRT; ++rt)
    {
        pColorBase[rt] = renderBuffers.pColor[rt];
    }
    uint8_t *pDepthBase = renderBuffers.pDepth, *pStencilBase = renderBuffers.pStencil;
    RDTSC_STOP(BESetup, 0, 0);

    SWR_PS_CONTEXT psContext;
    psContext.pAttribs = work.pAttribs;
    psContext.pPerspAttribs = work.pPerspAttribs;
    psContext.frontFace = work.triFlags.frontFacing;
    psContext.primID = work.triFlags.primID;
    psContext.pRecipW = work.pRecipW;
    // save Ia/Ib/Ic and Ja/Jb/Jc if we need to reevaluate i/j/k in the shader because of pull attribs
    psContext.I = work.I;
    psContext.J = work.J;
    psContext.recipDet = work.recipDet;
    psContext.pSamplePosX = (const float*)&T::MultisampleT::samplePosX;
    psContext.pSamplePosY = (const float*)&T::MultisampleT::samplePosY;
    psContext.sampleIndex = 0;

    uint32_t numOMSamples;
    // RT has to be single sample if we're in forcedMSAA mode
    if(T::bForcedSampleCount && (T::MultisampleT::sampleCount > SWR_MULTISAMPLE_1X))
    {
        numOMSamples = 1;
    }
    // unless we're forced to single sample, in which case we run the OM at the sample count of the RT
    else if(T::bForcedSampleCount && (T::MultisampleT::sampleCount == SWR_MULTISAMPLE_1X))
    {
        numOMSamples = GetNumSamples(pBlendState->sampleCount);
    }
    // else we're in normal MSAA mode and rasterizer and OM are running at the same sample count
    else
    {
        numOMSamples = T::MultisampleT::numSamples;
    }
    
    for(uint32_t yy = y; yy < y + KNOB_TILE_Y_DIM; yy += SIMD_TILE_Y_DIM)
    {
        psContext.vY.UL = _simd_add_ps(vULOffsetsY, _simd_set1_ps((float)yy));
        psContext.vY.center = _simd_add_ps(vCenterOffsetsY, _simd_set1_ps((float)yy));
        for(uint32_t xx = x; xx < x + KNOB_TILE_X_DIM; xx += SIMD_TILE_X_DIM)
        {
            simdscalar vZ[T::MultisampleT::numSamples]{ 0 };
            psContext.vX.UL = _simd_add_ps(vULOffsetsX, _simd_set1_ps((float)xx));
            // set pixel center positions
            psContext.vX.center = _simd_add_ps(vCenterOffsetsX, _simd_set1_ps((float)xx));

            if (T::bInputCoverage)
            {
                generateInputCoverage<T>(&work.coverageMask[0], psContext.inputMask, pBlendState->sampleMask);
            }

            if(T::bCentroidPos)
            {
                ///@ todo: don't need to genererate input coverage 2x if input coverage and centroid
                RDTSC_START(BEBarycentric);
                CalcCentroidBarycentrics<T>(coeffs, psContext, &work.coverageMask[0], pBlendState->sampleMask, psContext.vX.UL, psContext.vY.UL);
                RDTSC_STOP(BEBarycentric, 0, 0);
            }

            // if oDepth written to, or there is a potential to discard any samples, we need to 
            // run the PS early, then interp or broadcast Z and test
            if(pPSState->writesODepth || pPSState->killsPixel)
            {
                RDTSC_START(BEBarycentric);
                backendFuncs.pfnCalcPixelBarycentrics(coeffs, psContext);

                // interpolate and quantize z
                psContext.vZ = vplaneps(coeffs.vZa, coeffs.vZb, coeffs.vZc, psContext.vI.center, psContext.vJ.center);
                psContext.vZ = state.pfnQuantizeDepth(psContext.vZ);
                RDTSC_STOP(BEBarycentric, 0, 0);

                // execute pixel shader
                RDTSC_START(BEPixelShader);
                state.psState.pfnPixelShader(GetPrivateState(pDC), &psContext);
                RDTSC_STOP(BEPixelShader, 0, 0);
            }
            else
            {
                psContext.activeMask = _simd_set1_epi32(-1);
            }

            // need to declare enough space for all samples
            simdscalar vCoverageMask[T::MultisampleT::numSamples];
            simdscalar depthPassMask[T::MultisampleT::numSamples]; 
            simdscalar stencilPassMask[T::MultisampleT::numSamples];
            simdscalar anyDepthSamplePassed = _simd_setzero_ps();
            simdscalar anyStencilSamplePassed = _simd_setzero_ps();
            for(uint32_t sample = 0; sample < T::MultisampleT::numCoverageSamples; sample++)
            {
                vCoverageMask[sample] = vMask(work.coverageMask[sample] & MASK);

                // pull mask back out for any discards and and with coverage
                vCoverageMask[sample] = _simd_and_ps(vCoverageMask[sample], _simd_castsi_ps(psContext.activeMask));

                if (!_simd_movemask_ps(vCoverageMask[sample]))
                {
                    vCoverageMask[sample] = depthPassMask[sample] = stencilPassMask[sample] =  _simd_setzero_ps();
                    continue;
                }

                if(T::bForcedSampleCount)
                {
                    // candidate pixels (that passed coverage) will cause shader invocation if any bits in the samplemask are set
                    const simdscalar vSampleMask = _simd_castsi_ps(_simd_cmpgt_epi32(_simd_set1_epi32(pBlendState->sampleMask), _simd_setzero_si()));
                    anyDepthSamplePassed = _simd_or_ps(anyDepthSamplePassed, _simd_and_ps(vCoverageMask[sample], vSampleMask));
                    continue;
                }

                depthPassMask[sample] = vCoverageMask[sample];

                // if oDepth isn't written to, we need to interpolate Z for each sample
                // if clip distances are enabled, we need to interpolate for each sample
                if(!pPSState->writesODepth || rastState.clipDistanceMask)
                {
                    RDTSC_START(BEBarycentric);
                    if(T::bIsStandardPattern)
                    {
                        // calculate per sample positions
                        psContext.vX.sample = _simd_add_ps(psContext.vX.UL, T::MultisampleT::vX(sample));
                        psContext.vY.sample = _simd_add_ps(psContext.vY.UL, T::MultisampleT::vY(sample));
                    }
                    else
                    {
                        psContext.vX.sample = psContext.vX.center;
                        psContext.vY.sample = psContext.vY.center;
                    }

                    // calc I & J per sample
                    backendFuncs.pfnCalcSampleBarycentrics(coeffs, psContext);

                    // interpolate and quantize z
                    if (!pPSState->writesODepth)
                    {
                        vZ[sample] = vplaneps(coeffs.vZa, coeffs.vZb, coeffs.vZc, psContext.vI.sample, psContext.vJ.sample);
                        vZ[sample] = state.pfnQuantizeDepth(vZ[sample]);
                    }
                    
                    ///@todo: perspective correct vs non-perspective correct clipping?
                    // interpolate clip distances
                    if (rastState.clipDistanceMask)
                    {
                        uint8_t clipMask = ComputeUserClipMask(rastState.clipDistanceMask, work.pUserClipBuffer,
                            psContext.vI.sample, psContext.vJ.sample);
                        vCoverageMask[sample] = _simd_and_ps(vCoverageMask[sample], vMask(~clipMask));
                    }
                    RDTSC_STOP(BEBarycentric, 0, 0);
                }
                // else 'broadcast' and test psContext.vZ written from the PS each sample
                else
                {
                    vZ[sample] = psContext.vZ;
                }

                // offset depth/stencil buffers current sample
                uint8_t *pDepthSample = pDepthBase + T::MultisampleT::RasterTileDepthOffset(sample);
                uint8_t * pStencilSample = pStencilBase + T::MultisampleT::RasterTileStencilOffset(sample);

                // ZTest for this sample
                RDTSC_START(BEEarlyDepthTest);
                stencilPassMask[sample] = vCoverageMask[sample];
                depthPassMask[sample] = DepthStencilTest(&state, work.triFlags.frontFacing,
                                        vZ[sample], pDepthSample, vCoverageMask[sample], pStencilSample, &stencilPassMask[sample]);
                RDTSC_STOP(BEEarlyDepthTest, 0, 0);

                anyDepthSamplePassed = _simd_or_ps(anyDepthSamplePassed, depthPassMask[sample]);
                anyStencilSamplePassed = _simd_or_ps(anyStencilSamplePassed, stencilPassMask[sample]);
                uint32_t statMask = _simd_movemask_ps(depthPassMask[sample]);
                uint32_t statCount = _mm_popcnt_u32(statMask);
                UPDATE_STAT(DepthPassCount, statCount);
            }

            // if we didn't have to execute the PS early, and at least 1 sample passed the depth test, run the PS
            if(!pPSState->writesODepth && !pPSState->killsPixel && _simd_movemask_ps(anyDepthSamplePassed))
            {
                RDTSC_START(BEBarycentric);
                backendFuncs.pfnCalcPixelBarycentrics(coeffs, psContext);
                // interpolate and quantize z
                psContext.vZ = vplaneps(coeffs.vZa, coeffs.vZb, coeffs.vZc, psContext.vI.center, psContext.vJ.center);
                psContext.vZ = state.pfnQuantizeDepth(psContext.vZ);
                RDTSC_STOP(BEBarycentric, 0, 0);

                // execute pixel shader
                RDTSC_START(BEPixelShader);
                state.psState.pfnPixelShader(GetPrivateState(pDC), &psContext);
                RDTSC_STOP(BEPixelShader, 0, 0);
            }
            ///@todo: make sure this works for kill pixel
            else if(!_simd_movemask_ps(anyStencilSamplePassed))
            {
                goto Endtile;
            }

            // loop over all samples, broadcasting the results of the PS to all passing pixels
            for(uint32_t sample = 0; sample < numOMSamples; sample++)
            {
                uint8_t *pDepthSample = pDepthBase + T::MultisampleT::RasterTileDepthOffset(sample);
                uint8_t * pStencilSample = pStencilBase + T::MultisampleT::RasterTileStencilOffset(sample);

                // output merger
                RDTSC_START(BEOutputMerger);

                // skip if none of the pixels for this sample passed
                simdscalar coverageMaskSample;
                simdscalar depthMaskSample;
                simdscalar stencilMaskSample;
                simdscalar vInterpolatedZ;

                // forcedSampleCount outputs to any pixels with covered samples not masked off by SampleMask
                // depth test is disabled, so just set the z val to 0.
                if(T::bForcedSampleCount)
                {
                    coverageMaskSample = depthMaskSample = anyDepthSamplePassed;
                    vInterpolatedZ = _simd_setzero_ps();
                }
                else if(T::bIsStandardPattern)
                {
                    if(!_simd_movemask_ps(depthPassMask[sample]))
                    {
                        depthPassMask[sample] = _simd_setzero_ps();
                        DepthStencilWrite(&state.vp[0], &state.depthStencilState, work.triFlags.frontFacing, vZ[sample], pDepthSample, depthPassMask[sample],
                                          vCoverageMask[sample], pStencilSample, stencilPassMask[sample]);
                        continue;
                    }
                    coverageMaskSample = vCoverageMask[sample];
                    depthMaskSample = depthPassMask[sample];
                    stencilMaskSample = stencilPassMask[sample];
                    vInterpolatedZ = vZ[sample];
                }
                else
                {
                    // center pattern only needs to use a single depth test as all samples are at the same position
                    if(!_simd_movemask_ps(depthPassMask[0]))
                    {
                        depthPassMask[0] = _simd_setzero_ps();
                        DepthStencilWrite(&state.vp[0], &state.depthStencilState, work.triFlags.frontFacing, vZ[0], pDepthSample, depthPassMask[0],
                                          vCoverageMask[0], pStencilSample, stencilPassMask[0]);
                        continue;
                    }
                    coverageMaskSample = (vCoverageMask[0]);
                    depthMaskSample = depthPassMask[0];
                    stencilMaskSample = stencilPassMask[0];
                    vInterpolatedZ = vZ[0];
                }

                // output merger
                RDTSC_START(BEOutputMerger);
                backendFuncs.pfnOutputMerger(psContext, pColorBase, sample, pBlendState, state.pfnBlendFunc,
                                             coverageMaskSample, depthMaskSample);

                DepthStencilWrite(&state.vp[0], &state.depthStencilState, work.triFlags.frontFacing, vInterpolatedZ, pDepthSample, depthMaskSample,
                                  coverageMaskSample, pStencilSample, stencilMaskSample);
                RDTSC_STOP(BEOutputMerger, 0, 0);
            }

Endtile:
            RDTSC_START(BEEndTile);
            for(uint32_t sample = 0; sample < T::MultisampleT::numCoverageSamples; sample++)
            {
                work.coverageMask[sample] >>= (SIMD_TILE_Y_DIM * SIMD_TILE_X_DIM);
            }

            pDepthBase += (KNOB_SIMD_WIDTH * FormatTraits<KNOB_DEPTH_HOT_TILE_FORMAT>::bpp) / 8;
            pStencilBase += (KNOB_SIMD_WIDTH * FormatTraits<KNOB_STENCIL_HOT_TILE_FORMAT>::bpp) / 8;

            for(uint32_t rt = 0; rt < NumRT; ++rt)
            {
                pColorBase[rt] += (KNOB_SIMD_WIDTH * FormatTraits<KNOB_COLOR_HOT_TILE_FORMAT>::bpp) / 8;
            }
            RDTSC_STOP(BEEndTile, 0, 0);
        }
    }
}
// optimized backend flow with NULL PS
template<uint32_t sampleCountT>
void BackendNullPS(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t x, uint32_t y, SWR_TRIANGLE_DESC &work, RenderOutputBuffers &renderBuffers)
{
    ///@todo: handle center multisample pattern
    typedef SwrBackendTraits<sampleCountT, SWR_MSAA_STANDARD_PATTERN> T;
    RDTSC_START(BESetup);

    SWR_CONTEXT *pContext = pDC->pContext;
    const API_STATE& state = GetApiState(pDC);
    const BACKEND_FUNCS& backendFuncs = pDC->pState->backendFuncs;
    const SWR_RASTSTATE& rastState = pDC->pState->state.rastState;

    // broadcast scalars
    BarycentricCoeffs coeffs;
    coeffs.vIa = _simd_broadcast_ss(&work.I[0]);
    coeffs.vIb = _simd_broadcast_ss(&work.I[1]);
    coeffs.vIc = _simd_broadcast_ss(&work.I[2]);

    coeffs.vJa = _simd_broadcast_ss(&work.J[0]);
    coeffs.vJb = _simd_broadcast_ss(&work.J[1]);
    coeffs.vJc = _simd_broadcast_ss(&work.J[2]);

    coeffs.vZa = _simd_broadcast_ss(&work.Z[0]);
    coeffs.vZb = _simd_broadcast_ss(&work.Z[1]);
    coeffs.vZc = _simd_broadcast_ss(&work.Z[2]);

    coeffs.vRecipDet = _simd_broadcast_ss(&work.recipDet);

    uint8_t *pDepthBase = renderBuffers.pDepth, *pStencilBase = renderBuffers.pStencil;

    RDTSC_STOP(BESetup, 0, 0);

    SWR_PS_CONTEXT psContext;
    for (uint32_t yy = y; yy < y + KNOB_TILE_Y_DIM; yy += SIMD_TILE_Y_DIM)
    {
        // UL pixel corner
        simdscalar vYSamplePosUL = _simd_add_ps(vULOffsetsY, _simd_set1_ps((float)yy));

        for (uint32_t xx = x; xx < x + KNOB_TILE_X_DIM; xx += SIMD_TILE_X_DIM)
        {
            // UL pixel corners
            simdscalar vXSamplePosUL = _simd_add_ps(vULOffsetsX, _simd_set1_ps((float)xx));

            // iterate over active samples
            unsigned long sample = 0;
            uint32_t sampleMask = state.blendState.sampleMask;
            while (_BitScanForward(&sample, sampleMask))
            {
                sampleMask &= ~(1 << sample);
                simdmask coverageMask = work.coverageMask[sample] & MASK;
                if (coverageMask)
                {
                    RDTSC_START(BEBarycentric);
                    // calculate per sample positions
                    psContext.vX.sample = _simd_add_ps(vXSamplePosUL, T::MultisampleT::vX(sample));
                    psContext.vY.sample = _simd_add_ps(vYSamplePosUL, T::MultisampleT::vY(sample));

                    backendFuncs.pfnCalcSampleBarycentrics(coeffs, psContext);

                    // interpolate and quantize z
                    psContext.vZ = vplaneps(coeffs.vZa, coeffs.vZb, coeffs.vZc, psContext.vI.sample, psContext.vJ.sample);
                    psContext.vZ = state.pfnQuantizeDepth(psContext.vZ);

                    RDTSC_STOP(BEBarycentric, 0, 0);

                    // interpolate user clip distance if available
                    if (rastState.clipDistanceMask)
                    {
                        coverageMask &= ~ComputeUserClipMask(rastState.clipDistanceMask, work.pUserClipBuffer,
                            psContext.vI.sample, psContext.vJ.sample);
                    }

                    simdscalar vCoverageMask = vMask(coverageMask);
                    simdscalar stencilPassMask = vCoverageMask;

                    // offset depth/stencil buffers current sample
                    uint8_t *pDepthSample = pDepthBase + T::MultisampleT::RasterTileDepthOffset(sample);
                    uint8_t *pStencilSample = pStencilBase + T::MultisampleT::RasterTileStencilOffset(sample);

                    RDTSC_START(BEEarlyDepthTest);
                    simdscalar depthPassMask = DepthStencilTest(&state, work.triFlags.frontFacing,
                        psContext.vZ, pDepthSample, vCoverageMask, pStencilSample, &stencilPassMask);
                    DepthStencilWrite(&state.vp[0], &state.depthStencilState, work.triFlags.frontFacing, psContext.vZ,
                        pDepthSample, depthPassMask, vCoverageMask, pStencilSample, stencilPassMask);
                    RDTSC_STOP(BEEarlyDepthTest, 0, 0);

                    uint32_t statMask = _simd_movemask_ps(depthPassMask);
                    uint32_t statCount = _mm_popcnt_u32(statMask);
                    UPDATE_STAT(DepthPassCount, statCount);
                }
                work.coverageMask[sample] >>= (SIMD_TILE_Y_DIM * SIMD_TILE_X_DIM);
            }
            pDepthBase += (KNOB_SIMD_WIDTH * FormatTraits<KNOB_DEPTH_HOT_TILE_FORMAT>::bpp) / 8;
            pStencilBase += (KNOB_SIMD_WIDTH * FormatTraits<KNOB_STENCIL_HOT_TILE_FORMAT>::bpp) / 8;
        }
    }
}

void InitClearTilesTable()
{
    memset(sClearTilesTable, 0, sizeof(sClearTilesTable));

    sClearTilesTable[R8G8B8A8_UNORM] = ClearMacroTile<R8G8B8A8_UNORM>;
    sClearTilesTable[B8G8R8A8_UNORM] = ClearMacroTile<B8G8R8A8_UNORM>;
    sClearTilesTable[R32_FLOAT] = ClearMacroTile<R32_FLOAT>;
    sClearTilesTable[R32G32B32A32_FLOAT] = ClearMacroTile<R32G32B32A32_FLOAT>;
    sClearTilesTable[R8_UINT] = ClearMacroTile<R8_UINT>;
}

PFN_BACKEND_FUNC gBackendNullPs[SWR_MULTISAMPLE_TYPE_MAX];
PFN_BACKEND_FUNC gBackendSingleSample[2][2] = {};
PFN_BACKEND_FUNC gBackendPixelRateTable[SWR_MULTISAMPLE_TYPE_MAX][SWR_MSAA_SAMPLE_PATTERN_MAX][SWR_INPUT_COVERAGE_MAX][2][2] = {};
PFN_BACKEND_FUNC gBackendSampleRateTable[SWR_MULTISAMPLE_TYPE_MAX][SWR_INPUT_COVERAGE_MAX][2] = {};
PFN_OUTPUT_MERGER gBackendOutputMergerTable[SWR_NUM_RENDERTARGETS+1][SWR_MULTISAMPLE_TYPE_MAX] = {};
PFN_CALC_PIXEL_BARYCENTRICS gPixelBarycentricTable[2] = {};
PFN_CALC_SAMPLE_BARYCENTRICS gSampleBarycentricTable[2] = {};

// Recursive template used to auto-nest conditionals.  Converts dynamic enum function
// arguments to static template arguments.
template <uint32_t... ArgsT>
struct OMChooser
{
    // Last Arg Terminator
    static PFN_OUTPUT_MERGER GetFunc(SWR_MULTISAMPLE_COUNT tArg)
    {
        switch(tArg)
        {
        case SWR_MULTISAMPLE_1X: return OutputMerger<ArgsT..., SWR_MULTISAMPLE_1X>; break;
        case SWR_MULTISAMPLE_2X: return OutputMerger<ArgsT..., SWR_MULTISAMPLE_2X>; break;
        case SWR_MULTISAMPLE_4X: return OutputMerger<ArgsT..., SWR_MULTISAMPLE_4X>; break;
        case SWR_MULTISAMPLE_8X: return OutputMerger<ArgsT..., SWR_MULTISAMPLE_8X>; break;
        case SWR_MULTISAMPLE_16X: return OutputMerger<ArgsT..., SWR_MULTISAMPLE_16X>; break;
        default:
            SWR_ASSERT(0 && "Invalid sample count\n");
            return nullptr;
            break;
        }
    }

    // Recursively parse args
    template <typename... TArgsT>
    static PFN_OUTPUT_MERGER GetFunc(uint32_t tArg, TArgsT... remainingArgs)
    {
        switch(tArg)
        {
        case 0: return OMChooser<ArgsT..., 0>::GetFunc(remainingArgs...); break;
        case 1: return OMChooser<ArgsT..., 1>::GetFunc(remainingArgs...); break;
        case 2: return OMChooser<ArgsT..., 2>::GetFunc(remainingArgs...); break;
        case 3: return OMChooser<ArgsT..., 3>::GetFunc(remainingArgs...); break;
        case 4: return OMChooser<ArgsT..., 4>::GetFunc(remainingArgs...); break;
        case 5: return OMChooser<ArgsT..., 5>::GetFunc(remainingArgs...); break;
        case 6: return OMChooser<ArgsT..., 6>::GetFunc(remainingArgs...); break;
        case 7: return OMChooser<ArgsT..., 7>::GetFunc(remainingArgs...); break;
        case 8: return OMChooser<ArgsT..., 8>::GetFunc(remainingArgs...); break;
        default:
            SWR_ASSERT(0 && "Invalid RT index\n");
            return nullptr;
            break;
        }
    }
};

// Recursive template used to auto-nest conditionals.  Converts dynamic enum function
// arguments to static template arguments.
template <uint32_t... ArgsT>
struct BEChooser
{
    // Last Arg Terminator
    static PFN_BACKEND_FUNC GetFunc(SWR_BACKEND_FUNCS tArg)
    {
        switch(tArg)
        {
        case SWR_BACKEND_SINGLE_SAMPLE: return BackendSingleSample<SwrBackendTraits<ArgsT...>>; break;
        case SWR_BACKEND_MSAA_PIXEL_RATE: return BackendPixelRate<SwrBackendTraits<ArgsT...>>; break;
        case SWR_BACKEND_MSAA_SAMPLE_RATE: return BackendSampleRate<SwrBackendTraits<ArgsT...>>; break;
        default:
            SWR_ASSERT(0 && "Invalid backend func\n");
            return nullptr;
            break;
        }
    }

    // Recursively parse args
    template <typename... TArgsT>
    static PFN_BACKEND_FUNC GetFunc(SWR_MSAA_SAMPLE_PATTERN tArg, TArgsT... remainingArgs)
    {
        switch(tArg)
        {
        case SWR_MSAA_CENTER_PATTERN: return BEChooser<ArgsT..., SWR_MSAA_CENTER_PATTERN>::GetFunc(remainingArgs...); break;
        case SWR_MSAA_STANDARD_PATTERN: return BEChooser<ArgsT..., SWR_MSAA_STANDARD_PATTERN>::GetFunc(remainingArgs...); break;
        default:
        SWR_ASSERT(0 && "Invalid sample pattern\n");
        return BEChooser<ArgsT..., SWR_MSAA_STANDARD_PATTERN>::GetFunc(remainingArgs...);
        break;
        }
    }

    // Recursively parse args
    template <typename... TArgsT>
    static PFN_BACKEND_FUNC GetFunc(SWR_MULTISAMPLE_COUNT tArg, TArgsT... remainingArgs)
    {
        switch(tArg)
        {
        case SWR_MULTISAMPLE_1X: return BEChooser<ArgsT..., SWR_MULTISAMPLE_1X>::GetFunc(remainingArgs...); break;
        case SWR_MULTISAMPLE_2X: return BEChooser<ArgsT..., SWR_MULTISAMPLE_2X>::GetFunc(remainingArgs...); break;
        case SWR_MULTISAMPLE_4X: return BEChooser<ArgsT..., SWR_MULTISAMPLE_4X>::GetFunc(remainingArgs...); break;
        case SWR_MULTISAMPLE_8X: return BEChooser<ArgsT..., SWR_MULTISAMPLE_8X>::GetFunc(remainingArgs...); break;
        case SWR_MULTISAMPLE_16X: return BEChooser<ArgsT..., SWR_MULTISAMPLE_16X>::GetFunc(remainingArgs...); break;
        default:
        SWR_ASSERT(0 && "Invalid sample count\n");
        return BEChooser<ArgsT..., SWR_MULTISAMPLE_1X>::GetFunc(remainingArgs...);
        break;
        }
    }

    // Recursively parse args
    template <typename... TArgsT>
    static PFN_BACKEND_FUNC GetFunc(bool tArg, TArgsT... remainingArgs)
    {
        if(tArg == true)
        {
            return BEChooser<ArgsT..., 1>::GetFunc(remainingArgs...);
        }

        return BEChooser<ArgsT..., 0>::GetFunc(remainingArgs...);
    }
};

template <uint32_t numRenderTargets, SWR_MULTISAMPLE_COUNT numSampleRates>
void InitBackendOMFuncTable(PFN_OUTPUT_MERGER (&table)[numRenderTargets][numSampleRates])
{
    for(uint32_t rtNum = SWR_ATTACHMENT_COLOR0; rtNum < numRenderTargets; rtNum++)
    {
        for(uint32_t sampleCount = SWR_MULTISAMPLE_1X; sampleCount < numSampleRates; sampleCount++)
        {
            table[rtNum][sampleCount] =
                OMChooser<>::GetFunc((SWR_RENDERTARGET_ATTACHMENT)rtNum, (SWR_MULTISAMPLE_COUNT)sampleCount);
        }
    }
}

template <SWR_MULTISAMPLE_COUNT numSampleRates>
void InitBackendBarycentricsTables(PFN_CALC_PIXEL_BARYCENTRICS (&pixelTable)[2], 
                                   PFN_CALC_SAMPLE_BARYCENTRICS (&sampleTable)[2])
{
    pixelTable[0] = CalcPixelBarycentrics<0>;
    pixelTable[1] = CalcPixelBarycentrics<1>;

    sampleTable[0] = CalcSampleBarycentrics<0>;
    sampleTable[1] = CalcSampleBarycentrics<1>;
}

void InitBackendSampleFuncTable(PFN_BACKEND_FUNC (&table)[2][2])
{
    gBackendSingleSample[0][0] = BEChooser<>::GetFunc(SWR_MULTISAMPLE_1X, SWR_MSAA_STANDARD_PATTERN, false, false, false, false, (SWR_BACKEND_FUNCS)SWR_BACKEND_SINGLE_SAMPLE);
    gBackendSingleSample[0][1] = BEChooser<>::GetFunc(SWR_MULTISAMPLE_1X, SWR_MSAA_STANDARD_PATTERN, false, true, false, false, (SWR_BACKEND_FUNCS)SWR_BACKEND_SINGLE_SAMPLE);
    gBackendSingleSample[1][0] = BEChooser<>::GetFunc(SWR_MULTISAMPLE_1X, SWR_MSAA_STANDARD_PATTERN, true, false, false, false, (SWR_BACKEND_FUNCS)SWR_BACKEND_SINGLE_SAMPLE);
    gBackendSingleSample[1][1] = BEChooser<>::GetFunc(SWR_MULTISAMPLE_1X, SWR_MSAA_STANDARD_PATTERN, true, true, false, false,(SWR_BACKEND_FUNCS)SWR_BACKEND_SINGLE_SAMPLE);
}

template <SWR_MULTISAMPLE_COUNT numSampleRates, SWR_MSAA_SAMPLE_PATTERN numSamplePatterns, SWR_INPUT_COVERAGE numCoverageModes>
void InitBackendPixelFuncTable(PFN_BACKEND_FUNC (&table)[numSampleRates][numSamplePatterns][numCoverageModes][2][2])
{
    for(uint32_t sampleCount = SWR_MULTISAMPLE_1X; sampleCount < numSampleRates; sampleCount++)
    {
        for(uint32_t samplePattern = SWR_MSAA_CENTER_PATTERN; samplePattern < numSamplePatterns; samplePattern++)
        {
            for(uint32_t inputCoverage = SWR_INPUT_COVERAGE_NONE; inputCoverage < numCoverageModes; inputCoverage++)
            {
                for(uint32_t isCentroid = 0; isCentroid < 2; isCentroid++)
                {
                    table[sampleCount][samplePattern][inputCoverage][isCentroid][0] =
                        BEChooser<>::GetFunc((SWR_MULTISAMPLE_COUNT)sampleCount, (SWR_MSAA_SAMPLE_PATTERN)samplePattern, (inputCoverage == SWR_INPUT_COVERAGE_NORMAL), (isCentroid > 0),
                                                     false, false, SWR_BACKEND_MSAA_PIXEL_RATE);
                    table[sampleCount][samplePattern][inputCoverage][isCentroid][1] =
                        BEChooser<>::GetFunc((SWR_MULTISAMPLE_COUNT)sampleCount, (SWR_MSAA_SAMPLE_PATTERN)samplePattern, (inputCoverage == SWR_INPUT_COVERAGE_NORMAL), (isCentroid > 0),
                                             true, false, SWR_BACKEND_MSAA_PIXEL_RATE);
                }
            }
        }
    }
}

template <uint32_t numSampleRates, uint32_t numCoverageModes>
void InitBackendSampleFuncTable(PFN_BACKEND_FUNC (&table)[numSampleRates][numCoverageModes][2])
{
    for(uint32_t sampleCount = SWR_MULTISAMPLE_1X; sampleCount < numSampleRates; sampleCount++)
    {
        for(uint32_t inputCoverage = SWR_INPUT_COVERAGE_NONE; inputCoverage < numCoverageModes; inputCoverage++)
        {
            table[sampleCount][inputCoverage][0] =
                BEChooser<>::GetFunc((SWR_MULTISAMPLE_COUNT)sampleCount, SWR_MSAA_STANDARD_PATTERN, (inputCoverage == SWR_INPUT_COVERAGE_NORMAL), false, false, false, (SWR_BACKEND_FUNCS)SWR_BACKEND_MSAA_SAMPLE_RATE);
            table[sampleCount][inputCoverage][1] =
                BEChooser<>::GetFunc((SWR_MULTISAMPLE_COUNT)sampleCount, SWR_MSAA_STANDARD_PATTERN, (inputCoverage == SWR_INPUT_COVERAGE_NORMAL), true, false, false, (SWR_BACKEND_FUNCS)SWR_BACKEND_MSAA_SAMPLE_RATE);
        }
    }
}

void InitBackendFuncTables()
{    
    InitBackendSampleFuncTable(gBackendSingleSample);
    InitBackendPixelFuncTable<(SWR_MULTISAMPLE_COUNT)SWR_MULTISAMPLE_TYPE_MAX, SWR_MSAA_SAMPLE_PATTERN_MAX, SWR_INPUT_COVERAGE_MAX>(gBackendPixelRateTable);
    InitBackendSampleFuncTable<SWR_MULTISAMPLE_TYPE_MAX, SWR_INPUT_COVERAGE_MAX>(gBackendSampleRateTable);
    InitBackendOMFuncTable<SWR_NUM_RENDERTARGETS+1, SWR_MULTISAMPLE_TYPE_MAX>(gBackendOutputMergerTable);
    InitBackendBarycentricsTables<(SWR_MULTISAMPLE_COUNT)(SWR_MULTISAMPLE_TYPE_MAX)>(gPixelBarycentricTable, gSampleBarycentricTable);

    gBackendNullPs[SWR_MULTISAMPLE_1X] = &BackendNullPS < SWR_MULTISAMPLE_1X > ;
    gBackendNullPs[SWR_MULTISAMPLE_2X] = &BackendNullPS < SWR_MULTISAMPLE_2X > ;
    gBackendNullPs[SWR_MULTISAMPLE_4X] = &BackendNullPS < SWR_MULTISAMPLE_4X > ;
    gBackendNullPs[SWR_MULTISAMPLE_8X] = &BackendNullPS < SWR_MULTISAMPLE_8X > ;
    gBackendNullPs[SWR_MULTISAMPLE_16X] = &BackendNullPS < SWR_MULTISAMPLE_16X > ;
}
