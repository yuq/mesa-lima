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
* @file frontend.cpp
*
* @brief Implementation for Frontend which handles vertex processing,
*        primitive assembly, clipping, binning, etc.
*
******************************************************************************/

#include "api.h"
#include "frontend.h"
#include "backend.h"
#include "context.h"
#include "rdtsc_core.h"
#include "rasterizer.h"
#include "conservativeRast.h"
#include "utils.h"
#include "threads.h"
#include "pa.h"
#include "clip.h"
#include "tilemgr.h"
#include "tessellator.h"
#include <limits>

//////////////////////////////////////////////////////////////////////////
/// @brief Helper macro to generate a bitmask
static INLINE uint32_t GenMask(uint32_t numBits)
{
    SWR_ASSERT(numBits <= (sizeof(uint32_t) * 8), "Too many bits (%d) for %s", numBits, __FUNCTION__);
    return ((1U << numBits) - 1);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Offsets added to post-viewport vertex positions based on
/// raster state.
static const simdscalar g_pixelOffsets[SWR_PIXEL_LOCATION_UL + 1] =
{
    _simd_set1_ps(0.0f), // SWR_PIXEL_LOCATION_CENTER
    _simd_set1_ps(0.5f), // SWR_PIXEL_LOCATION_UL
};

//////////////////////////////////////////////////////////////////////////
/// @brief FE handler for SwrSync.
/// @param pContext - pointer to SWR context.
/// @param pDC - pointer to draw context.
/// @param workerId - thread's worker id. Even thread has a unique id.
/// @param pUserData - Pointer to user data passed back to sync callback.
/// @todo This should go away when we switch this to use compute threading.
void ProcessSync(
    SWR_CONTEXT *pContext,
    DRAW_CONTEXT *pDC,
    uint32_t workerId,
    void *pUserData)
{
    BE_WORK work;
    work.type = SYNC;
    work.pfnWork = ProcessSyncBE;

    MacroTileMgr *pTileMgr = pDC->pTileMgr;
    pTileMgr->enqueue(0, 0, &work);
}

//////////////////////////////////////////////////////////////////////////
/// @brief FE handler for SwrDestroyContext.
/// @param pContext - pointer to SWR context.
/// @param pDC - pointer to draw context.
/// @param workerId - thread's worker id. Even thread has a unique id.
/// @param pUserData - Pointer to user data passed back to sync callback.
void ProcessShutdown(
    SWR_CONTEXT *pContext,
    DRAW_CONTEXT *pDC,
    uint32_t workerId,
    void *pUserData)
{
    BE_WORK work;
    work.type = SHUTDOWN;
    work.pfnWork = ProcessShutdownBE;

    MacroTileMgr *pTileMgr = pDC->pTileMgr;
    // Enqueue at least 1 work item for each worker thread
    // account for number of numa nodes
    uint32_t numNumaNodes = pContext->threadPool.numaMask + 1;

    for (uint32_t i = 0; i < pContext->threadPool.numThreads; ++i)
    {
        for (uint32_t n = 0; n < numNumaNodes; ++n)
        {
            pTileMgr->enqueue(i, n, &work);
        }
    }
}

//////////////////////////////////////////////////////////////////////////
/// @brief FE handler for SwrClearRenderTarget.
/// @param pContext - pointer to SWR context.
/// @param pDC - pointer to draw context.
/// @param workerId - thread's worker id. Even thread has a unique id.
/// @param pUserData - Pointer to user data passed back to clear callback.
/// @todo This should go away when we switch this to use compute threading.
void ProcessClear(
    SWR_CONTEXT *pContext,
    DRAW_CONTEXT *pDC,
    uint32_t workerId,
    void *pUserData)
{
    CLEAR_DESC *pDesc = (CLEAR_DESC*)pUserData;
    MacroTileMgr *pTileMgr = pDC->pTileMgr;

    // queue a clear to each macro tile
    // compute macro tile bounds for the specified rect
    uint32_t macroTileXMin = pDesc->rect.xmin / KNOB_MACROTILE_X_DIM;
    uint32_t macroTileXMax = (pDesc->rect.xmax - 1) / KNOB_MACROTILE_X_DIM;
    uint32_t macroTileYMin = pDesc->rect.ymin / KNOB_MACROTILE_Y_DIM;
    uint32_t macroTileYMax = (pDesc->rect.ymax - 1) / KNOB_MACROTILE_Y_DIM;

    BE_WORK work;
    work.type = CLEAR;
    work.pfnWork = ProcessClearBE;
    work.desc.clear = *pDesc;

    for (uint32_t y = macroTileYMin; y <= macroTileYMax; ++y)
    {
        for (uint32_t x = macroTileXMin; x <= macroTileXMax; ++x)
        {
            pTileMgr->enqueue(x, y, &work);
        }
    }
}

//////////////////////////////////////////////////////////////////////////
/// @brief FE handler for SwrStoreTiles.
/// @param pContext - pointer to SWR context.
/// @param pDC - pointer to draw context.
/// @param workerId - thread's worker id. Even thread has a unique id.
/// @param pUserData - Pointer to user data passed back to callback.
/// @todo This should go away when we switch this to use compute threading.
void ProcessStoreTiles(
    SWR_CONTEXT *pContext,
    DRAW_CONTEXT *pDC,
    uint32_t workerId,
    void *pUserData)
{
    AR_BEGIN(FEProcessStoreTiles, pDC->drawId);
    MacroTileMgr *pTileMgr = pDC->pTileMgr;
    STORE_TILES_DESC* pDesc = (STORE_TILES_DESC*)pUserData;

    // queue a store to each macro tile
    // compute macro tile bounds for the specified rect
    uint32_t macroTileXMin = pDesc->rect.xmin / KNOB_MACROTILE_X_DIM;
    uint32_t macroTileXMax = (pDesc->rect.xmax - 1) / KNOB_MACROTILE_X_DIM;
    uint32_t macroTileYMin = pDesc->rect.ymin / KNOB_MACROTILE_Y_DIM;
    uint32_t macroTileYMax = (pDesc->rect.ymax - 1) / KNOB_MACROTILE_Y_DIM;

    // store tiles
    BE_WORK work;
    work.type = STORETILES;
    work.pfnWork = ProcessStoreTileBE;
    work.desc.storeTiles = *pDesc;

    for (uint32_t y = macroTileYMin; y <= macroTileYMax; ++y)
    {
        for (uint32_t x = macroTileXMin; x <= macroTileXMax; ++x)
        {
            pTileMgr->enqueue(x, y, &work);
        }
    }

    AR_END(FEProcessStoreTiles, 0);
}

//////////////////////////////////////////////////////////////////////////
/// @brief FE handler for SwrInvalidateTiles.
/// @param pContext - pointer to SWR context.
/// @param pDC - pointer to draw context.
/// @param workerId - thread's worker id. Even thread has a unique id.
/// @param pUserData - Pointer to user data passed back to callback.
/// @todo This should go away when we switch this to use compute threading.
void ProcessDiscardInvalidateTiles(
    SWR_CONTEXT *pContext,
    DRAW_CONTEXT *pDC,
    uint32_t workerId,
    void *pUserData)
{
    AR_BEGIN(FEProcessInvalidateTiles, pDC->drawId);
    DISCARD_INVALIDATE_TILES_DESC *pDesc = (DISCARD_INVALIDATE_TILES_DESC*)pUserData;
    MacroTileMgr *pTileMgr = pDC->pTileMgr;

    // compute macro tile bounds for the specified rect
    uint32_t macroTileXMin = (pDesc->rect.xmin + KNOB_MACROTILE_X_DIM - 1) / KNOB_MACROTILE_X_DIM;
    uint32_t macroTileXMax = (pDesc->rect.xmax / KNOB_MACROTILE_X_DIM) - 1;
    uint32_t macroTileYMin = (pDesc->rect.ymin + KNOB_MACROTILE_Y_DIM - 1) / KNOB_MACROTILE_Y_DIM;
    uint32_t macroTileYMax = (pDesc->rect.ymax / KNOB_MACROTILE_Y_DIM) - 1;

    if (pDesc->fullTilesOnly == false)
    {
        // include partial tiles
        macroTileXMin = pDesc->rect.xmin / KNOB_MACROTILE_X_DIM;
        macroTileXMax = (pDesc->rect.xmax - 1) / KNOB_MACROTILE_X_DIM;
        macroTileYMin = pDesc->rect.ymin / KNOB_MACROTILE_Y_DIM;
        macroTileYMax = (pDesc->rect.ymax - 1) / KNOB_MACROTILE_Y_DIM;
    }

    SWR_ASSERT(macroTileXMax <= KNOB_NUM_HOT_TILES_X);
    SWR_ASSERT(macroTileYMax <= KNOB_NUM_HOT_TILES_Y);

    macroTileXMax = std::min<int32_t>(macroTileXMax, KNOB_NUM_HOT_TILES_X);
    macroTileYMax = std::min<int32_t>(macroTileYMax, KNOB_NUM_HOT_TILES_Y);

    // load tiles
    BE_WORK work;
    work.type = DISCARDINVALIDATETILES;
    work.pfnWork = ProcessDiscardInvalidateTilesBE;
    work.desc.discardInvalidateTiles = *pDesc;

    for (uint32_t x = macroTileXMin; x <= macroTileXMax; ++x)
    {
        for (uint32_t y = macroTileYMin; y <= macroTileYMax; ++y)
        {
            pTileMgr->enqueue(x, y, &work);
        }
    }

    AR_END(FEProcessInvalidateTiles, 0);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Computes the number of primitives given the number of verts.
/// @param mode - primitive topology for draw operation.
/// @param numPrims - number of vertices or indices for draw.
/// @todo Frontend needs to be refactored. This will go in appropriate place then.
uint32_t GetNumPrims(
    PRIMITIVE_TOPOLOGY mode,
    uint32_t numPrims)
{
    switch (mode)
    {
    case TOP_POINT_LIST: return numPrims;
    case TOP_TRIANGLE_LIST: return numPrims / 3;
    case TOP_TRIANGLE_STRIP: return numPrims < 3 ? 0 : numPrims - 2;
    case TOP_TRIANGLE_FAN: return numPrims < 3 ? 0 : numPrims - 2;
    case TOP_TRIANGLE_DISC: return numPrims < 2 ? 0 : numPrims - 1;
    case TOP_QUAD_LIST: return numPrims / 4;
    case TOP_QUAD_STRIP: return numPrims < 4 ? 0 : (numPrims - 2) / 2;
    case TOP_LINE_STRIP: return numPrims < 2 ? 0 : numPrims - 1;
    case TOP_LINE_LIST: return numPrims / 2;
    case TOP_LINE_LOOP: return numPrims;
    case TOP_RECT_LIST: return numPrims / 3;
    case TOP_LINE_LIST_ADJ: return numPrims / 4;
    case TOP_LISTSTRIP_ADJ: return numPrims < 3 ? 0 : numPrims - 3;
    case TOP_TRI_LIST_ADJ: return numPrims / 6;
    case TOP_TRI_STRIP_ADJ: return numPrims < 4 ? 0 : (numPrims / 2) - 2;

    case TOP_PATCHLIST_1:
    case TOP_PATCHLIST_2:
    case TOP_PATCHLIST_3:
    case TOP_PATCHLIST_4:
    case TOP_PATCHLIST_5:
    case TOP_PATCHLIST_6:
    case TOP_PATCHLIST_7:
    case TOP_PATCHLIST_8:
    case TOP_PATCHLIST_9:
    case TOP_PATCHLIST_10:
    case TOP_PATCHLIST_11:
    case TOP_PATCHLIST_12:
    case TOP_PATCHLIST_13:
    case TOP_PATCHLIST_14:
    case TOP_PATCHLIST_15:
    case TOP_PATCHLIST_16:
    case TOP_PATCHLIST_17:
    case TOP_PATCHLIST_18:
    case TOP_PATCHLIST_19:
    case TOP_PATCHLIST_20:
    case TOP_PATCHLIST_21:
    case TOP_PATCHLIST_22:
    case TOP_PATCHLIST_23:
    case TOP_PATCHLIST_24:
    case TOP_PATCHLIST_25:
    case TOP_PATCHLIST_26:
    case TOP_PATCHLIST_27:
    case TOP_PATCHLIST_28:
    case TOP_PATCHLIST_29:
    case TOP_PATCHLIST_30:
    case TOP_PATCHLIST_31:
    case TOP_PATCHLIST_32:
        return numPrims / (mode - TOP_PATCHLIST_BASE);

    case TOP_POLYGON:
    case TOP_POINT_LIST_BF:
    case TOP_LINE_STRIP_CONT:
    case TOP_LINE_STRIP_BF:
    case TOP_LINE_STRIP_CONT_BF:
    case TOP_TRIANGLE_FAN_NOSTIPPLE:
    case TOP_TRI_STRIP_REVERSE:
    case TOP_PATCHLIST_BASE:
    case TOP_UNKNOWN:
        SWR_ASSERT(false, "Unsupported topology: %d", mode);
        return 0;
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////////
/// @brief Computes the number of verts given the number of primitives.
/// @param mode - primitive topology for draw operation.
/// @param numPrims - number of primitives for draw.
uint32_t GetNumVerts(
    PRIMITIVE_TOPOLOGY mode,
    uint32_t numPrims)
{
    switch (mode)
    {
    case TOP_POINT_LIST: return numPrims;
    case TOP_TRIANGLE_LIST: return numPrims * 3;
    case TOP_TRIANGLE_STRIP: return numPrims ? numPrims + 2 : 0;
    case TOP_TRIANGLE_FAN: return numPrims ? numPrims + 2 : 0;
    case TOP_TRIANGLE_DISC: return numPrims ? numPrims + 1 : 0;
    case TOP_QUAD_LIST: return numPrims * 4;
    case TOP_QUAD_STRIP: return numPrims ? numPrims * 2 + 2 : 0;
    case TOP_LINE_STRIP: return numPrims ? numPrims + 1 : 0;
    case TOP_LINE_LIST: return numPrims * 2;
    case TOP_LINE_LOOP: return numPrims;
    case TOP_RECT_LIST: return numPrims * 3;
    case TOP_LINE_LIST_ADJ: return numPrims * 4;
    case TOP_LISTSTRIP_ADJ: return numPrims ? numPrims + 3 : 0;
    case TOP_TRI_LIST_ADJ: return numPrims * 6;
    case TOP_TRI_STRIP_ADJ: return numPrims ? (numPrims + 2) * 2 : 0;

    case TOP_PATCHLIST_1:
    case TOP_PATCHLIST_2:
    case TOP_PATCHLIST_3:
    case TOP_PATCHLIST_4:
    case TOP_PATCHLIST_5:
    case TOP_PATCHLIST_6:
    case TOP_PATCHLIST_7:
    case TOP_PATCHLIST_8:
    case TOP_PATCHLIST_9:
    case TOP_PATCHLIST_10:
    case TOP_PATCHLIST_11:
    case TOP_PATCHLIST_12:
    case TOP_PATCHLIST_13:
    case TOP_PATCHLIST_14:
    case TOP_PATCHLIST_15:
    case TOP_PATCHLIST_16:
    case TOP_PATCHLIST_17:
    case TOP_PATCHLIST_18:
    case TOP_PATCHLIST_19:
    case TOP_PATCHLIST_20:
    case TOP_PATCHLIST_21:
    case TOP_PATCHLIST_22:
    case TOP_PATCHLIST_23:
    case TOP_PATCHLIST_24:
    case TOP_PATCHLIST_25:
    case TOP_PATCHLIST_26:
    case TOP_PATCHLIST_27:
    case TOP_PATCHLIST_28:
    case TOP_PATCHLIST_29:
    case TOP_PATCHLIST_30:
    case TOP_PATCHLIST_31:
    case TOP_PATCHLIST_32:
        return numPrims * (mode - TOP_PATCHLIST_BASE);

    case TOP_POLYGON:
    case TOP_POINT_LIST_BF:
    case TOP_LINE_STRIP_CONT:
    case TOP_LINE_STRIP_BF:
    case TOP_LINE_STRIP_CONT_BF:
    case TOP_TRIANGLE_FAN_NOSTIPPLE:
    case TOP_TRI_STRIP_REVERSE:
    case TOP_PATCHLIST_BASE:
    case TOP_UNKNOWN:
        SWR_ASSERT(false, "Unsupported topology: %d", mode);
        return 0;
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////////
/// @brief Return number of verts per primitive.
/// @param topology - topology
/// @param includeAdjVerts - include adjacent verts in primitive vertices
INLINE uint32_t NumVertsPerPrim(PRIMITIVE_TOPOLOGY topology, bool includeAdjVerts)
{
    uint32_t numVerts = 0;
    switch (topology)
    {
    case TOP_POINT_LIST:
    case TOP_POINT_LIST_BF:
        numVerts = 1;
        break;
    case TOP_LINE_LIST:
    case TOP_LINE_STRIP:
    case TOP_LINE_LIST_ADJ:
    case TOP_LINE_LOOP:
    case TOP_LINE_STRIP_CONT:
    case TOP_LINE_STRIP_BF:
    case TOP_LISTSTRIP_ADJ:
        numVerts = 2;
        break;
    case TOP_TRIANGLE_LIST:
    case TOP_TRIANGLE_STRIP:
    case TOP_TRIANGLE_FAN:
    case TOP_TRI_LIST_ADJ:
    case TOP_TRI_STRIP_ADJ:
    case TOP_TRI_STRIP_REVERSE:
    case TOP_RECT_LIST:
        numVerts = 3;
        break;
    case TOP_QUAD_LIST:
    case TOP_QUAD_STRIP:
        numVerts = 4;
        break;
    case TOP_PATCHLIST_1:
    case TOP_PATCHLIST_2:
    case TOP_PATCHLIST_3:
    case TOP_PATCHLIST_4:
    case TOP_PATCHLIST_5:
    case TOP_PATCHLIST_6:
    case TOP_PATCHLIST_7:
    case TOP_PATCHLIST_8:
    case TOP_PATCHLIST_9:
    case TOP_PATCHLIST_10:
    case TOP_PATCHLIST_11:
    case TOP_PATCHLIST_12:
    case TOP_PATCHLIST_13:
    case TOP_PATCHLIST_14:
    case TOP_PATCHLIST_15:
    case TOP_PATCHLIST_16:
    case TOP_PATCHLIST_17:
    case TOP_PATCHLIST_18:
    case TOP_PATCHLIST_19:
    case TOP_PATCHLIST_20:
    case TOP_PATCHLIST_21:
    case TOP_PATCHLIST_22:
    case TOP_PATCHLIST_23:
    case TOP_PATCHLIST_24:
    case TOP_PATCHLIST_25:
    case TOP_PATCHLIST_26:
    case TOP_PATCHLIST_27:
    case TOP_PATCHLIST_28:
    case TOP_PATCHLIST_29:
    case TOP_PATCHLIST_30:
    case TOP_PATCHLIST_31:
    case TOP_PATCHLIST_32:
        numVerts = topology - TOP_PATCHLIST_BASE;
        break;
    default:
        SWR_ASSERT(false, "Unsupported topology: %d", topology);
        break;
    }

    if (includeAdjVerts)
    {
        switch (topology)
        {
        case TOP_LISTSTRIP_ADJ:
        case TOP_LINE_LIST_ADJ: numVerts = 4; break;
        case TOP_TRI_STRIP_ADJ:
        case TOP_TRI_LIST_ADJ: numVerts = 6; break;
        default: break;
        }
    }

    return numVerts;
}

//////////////////////////////////////////////////////////////////////////
/// @brief Generate mask from remaining work.
/// @param numWorkItems - Number of items being worked on by a SIMD.
static INLINE simdscalari GenerateMask(uint32_t numItemsRemaining)
{
    uint32_t numActive = (numItemsRemaining >= KNOB_SIMD_WIDTH) ? KNOB_SIMD_WIDTH : numItemsRemaining;
    uint32_t mask = (numActive > 0) ? ((1 << numActive) - 1) : 0;
    return _simd_castps_si(vMask(mask));
}


//////////////////////////////////////////////////////////////////////////
/// @brief  Gather scissor rect data based on per-prim viewport indices.
/// @param pScissorsInFixedPoint - array of scissor rects in 16.8 fixed point.
/// @param pViewportIndex - array of per-primitive vewport indexes.
/// @param scisXmin - output vector of per-prmitive scissor rect Xmin data.
/// @param scisYmin - output vector of per-prmitive scissor rect Ymin data.
/// @param scisXmax - output vector of per-prmitive scissor rect Xmax data.
/// @param scisYmax - output vector of per-prmitive scissor rect Ymax data.
//
/// @todo:  Look at speeding this up -- weigh against corresponding costs in rasterizer.
template<size_t SimdWidth>
struct GatherScissors
{
    static void Gather(const SWR_RECT* pScissorsInFixedPoint, const uint32_t* pViewportIndex,
        simdscalari &scisXmin, simdscalari &scisYmin,
        simdscalari &scisXmax, simdscalari &scisYmax)
    {
        SWR_ASSERT(0, "Unhandled Simd Width in Scissor Rect Gather");
    }
};

template<>
struct GatherScissors<8>
{
    static void Gather(const SWR_RECT* pScissorsInFixedPoint, const uint32_t* pViewportIndex,
        simdscalari &scisXmin, simdscalari &scisYmin,
        simdscalari &scisXmax, simdscalari &scisYmax)
    {
        scisXmin = _simd_set_epi32(pScissorsInFixedPoint[pViewportIndex[0]].xmin,
            pScissorsInFixedPoint[pViewportIndex[1]].xmin,
            pScissorsInFixedPoint[pViewportIndex[2]].xmin,
            pScissorsInFixedPoint[pViewportIndex[3]].xmin,
            pScissorsInFixedPoint[pViewportIndex[4]].xmin,
            pScissorsInFixedPoint[pViewportIndex[5]].xmin,
            pScissorsInFixedPoint[pViewportIndex[6]].xmin,
            pScissorsInFixedPoint[pViewportIndex[7]].xmin);
        scisYmin = _simd_set_epi32(pScissorsInFixedPoint[pViewportIndex[0]].ymin,
            pScissorsInFixedPoint[pViewportIndex[1]].ymin,
            pScissorsInFixedPoint[pViewportIndex[2]].ymin,
            pScissorsInFixedPoint[pViewportIndex[3]].ymin,
            pScissorsInFixedPoint[pViewportIndex[4]].ymin,
            pScissorsInFixedPoint[pViewportIndex[5]].ymin,
            pScissorsInFixedPoint[pViewportIndex[6]].ymin,
            pScissorsInFixedPoint[pViewportIndex[7]].ymin);
        scisXmax = _simd_set_epi32(pScissorsInFixedPoint[pViewportIndex[0]].xmax,
            pScissorsInFixedPoint[pViewportIndex[1]].xmax,
            pScissorsInFixedPoint[pViewportIndex[2]].xmax,
            pScissorsInFixedPoint[pViewportIndex[3]].xmax,
            pScissorsInFixedPoint[pViewportIndex[4]].xmax,
            pScissorsInFixedPoint[pViewportIndex[5]].xmax,
            pScissorsInFixedPoint[pViewportIndex[6]].xmax,
            pScissorsInFixedPoint[pViewportIndex[7]].xmax);
        scisYmax = _simd_set_epi32(pScissorsInFixedPoint[pViewportIndex[0]].ymax,
            pScissorsInFixedPoint[pViewportIndex[1]].ymax,
            pScissorsInFixedPoint[pViewportIndex[2]].ymax,
            pScissorsInFixedPoint[pViewportIndex[3]].ymax,
            pScissorsInFixedPoint[pViewportIndex[4]].ymax,
            pScissorsInFixedPoint[pViewportIndex[5]].ymax,
            pScissorsInFixedPoint[pViewportIndex[6]].ymax,
            pScissorsInFixedPoint[pViewportIndex[7]].ymax);
    }
};

//////////////////////////////////////////////////////////////////////////
/// @brief StreamOut - Streams vertex data out to SO buffers.
///        Generally, we are only streaming out a SIMDs worth of triangles.
/// @param pDC - pointer to draw context.
/// @param workerId - thread's worker id. Even thread has a unique id.
/// @param numPrims - Number of prims to streamout (e.g. points, lines, tris)
static void StreamOut(
    DRAW_CONTEXT* pDC,
    PA_STATE& pa,
    uint32_t workerId,
    uint32_t* pPrimData,
    uint32_t streamIndex)
{
    SWR_CONTEXT *pContext = pDC->pContext;

    AR_BEGIN(FEStreamout, pDC->drawId);

    const API_STATE& state = GetApiState(pDC);
    const SWR_STREAMOUT_STATE &soState = state.soState;

    uint32_t soVertsPerPrim = NumVertsPerPrim(pa.binTopology, false);

    // The pPrimData buffer is sparse in that we allocate memory for all 32 attributes for each vertex.
    uint32_t primDataDwordVertexStride = (KNOB_NUM_ATTRIBUTES * sizeof(float) * 4) / sizeof(uint32_t);

    SWR_STREAMOUT_CONTEXT soContext = { 0 };

    // Setup buffer state pointers.
    for (uint32_t i = 0; i < 4; ++i)
    {
        soContext.pBuffer[i] = &state.soBuffer[i];
    }

    uint32_t numPrims = pa.NumPrims();
    for (uint32_t primIndex = 0; primIndex < numPrims; ++primIndex)
    {
        DWORD slot = 0;
        uint32_t soMask = soState.streamMasks[streamIndex];

        // Write all entries into primitive data buffer for SOS.
        while (_BitScanForward(&slot, soMask))
        {
            __m128 attrib[MAX_NUM_VERTS_PER_PRIM];    // prim attribs (always 4 wide)
            uint32_t paSlot = slot + VERTEX_ATTRIB_START_SLOT;
            pa.AssembleSingle(paSlot, primIndex, attrib);

            // Attribute offset is relative offset from start of vertex.
            // Note that attributes start at slot 1 in the PA buffer. We need to write this
            // to prim data starting at slot 0. Which is why we do (slot - 1).
            // Also note: GL works slightly differently, and needs slot 0
            uint32_t primDataAttribOffset = slot * sizeof(float) * 4 / sizeof(uint32_t);

            // Store each vertex's attrib at appropriate locations in pPrimData buffer.
            for (uint32_t v = 0; v < soVertsPerPrim; ++v)
            {
                uint32_t* pPrimDataAttrib = pPrimData + primDataAttribOffset + (v * primDataDwordVertexStride);

                _mm_store_ps((float*)pPrimDataAttrib, attrib[v]);
            }
            soMask &= ~(1 << slot);
        }

        // Update pPrimData pointer 
        soContext.pPrimData = pPrimData;

        // Call SOS
        SWR_ASSERT(state.pfnSoFunc[streamIndex] != nullptr, "Trying to execute uninitialized streamout jit function.");
        state.pfnSoFunc[streamIndex](soContext);
    }

    // Update SO write offset. The driver provides memory for the update.
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (state.soBuffer[i].pWriteOffset)
        {
            *state.soBuffer[i].pWriteOffset = soContext.pBuffer[i]->streamOffset * sizeof(uint32_t);
        }

        if (state.soBuffer[i].soWriteEnable)
        {
            pDC->dynState.SoWriteOffset[i] = soContext.pBuffer[i]->streamOffset * sizeof(uint32_t);
            pDC->dynState.SoWriteOffsetDirty[i] = true;
        }
    }

    UPDATE_STAT_FE(SoPrimStorageNeeded[streamIndex], soContext.numPrimStorageNeeded);
    UPDATE_STAT_FE(SoNumPrimsWritten[streamIndex], soContext.numPrimsWritten);

    AR_END(FEStreamout, 1);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Computes number of invocations. The current index represents
///        the start of the SIMD. The max index represents how much work
///        items are remaining. If there is less then a SIMD's xmin of work
///        then return the remaining amount of work.
/// @param curIndex - The start index for the SIMD.
/// @param maxIndex - The last index for all work items.
static INLINE uint32_t GetNumInvocations(
    uint32_t curIndex,
    uint32_t maxIndex)
{
    uint32_t remainder = (maxIndex - curIndex);
    return (remainder >= KNOB_SIMD_WIDTH) ? KNOB_SIMD_WIDTH : remainder;
}

//////////////////////////////////////////////////////////////////////////
/// @brief Converts a streamId buffer to a cut buffer for the given stream id.
///        The geometry shader will loop over each active streamout buffer, assembling
///        primitives for the downstream stages. When multistream output is enabled,
///        the generated stream ID buffer from the GS needs to be converted to a cut
///        buffer for the primitive assembler.
/// @param stream - stream id to generate the cut buffer for
/// @param pStreamIdBase - pointer to the stream ID buffer
/// @param numEmittedVerts - Number of total verts emitted by the GS
/// @param pCutBuffer - output buffer to write cuts to
void ProcessStreamIdBuffer(uint32_t stream, uint8_t* pStreamIdBase, uint32_t numEmittedVerts, uint8_t *pCutBuffer)
{
    SWR_ASSERT(stream < MAX_SO_STREAMS);

    uint32_t numInputBytes = (numEmittedVerts * 2  + 7) / 8;
    uint32_t numOutputBytes = std::max(numInputBytes / 2, 1U);

    for (uint32_t b = 0; b < numOutputBytes; ++b)
    {
        uint8_t curInputByte = pStreamIdBase[2*b];
        uint8_t outByte = 0;
        for (uint32_t i = 0; i < 4; ++i)
        {
            if ((curInputByte & 0x3) != stream)
            {
                outByte |= (1 << i);
            }
            curInputByte >>= 2;
        }

        curInputByte = pStreamIdBase[2 * b + 1];
        for (uint32_t i = 0; i < 4; ++i)
        {
            if ((curInputByte & 0x3) != stream)
            {
                outByte |= (1 << (i + 4));
            }
            curInputByte >>= 2;
        }
        
        *pCutBuffer++ = outByte;
    }
}

THREAD SWR_GS_CONTEXT tlsGsContext;

//////////////////////////////////////////////////////////////////////////
/// @brief Implements GS stage.
/// @param pDC - pointer to draw context.
/// @param workerId - thread's worker id. Even thread has a unique id.
/// @param pa - The primitive assembly object.
/// @param pGsOut - output stream for GS
template <
    typename HasStreamOutT,
    typename HasRastT>
static void GeometryShaderStage(
    DRAW_CONTEXT *pDC,
    uint32_t workerId,
    PA_STATE& pa,
    void* pGsOut,
    void* pCutBuffer,
    void* pStreamCutBuffer,
    uint32_t* pSoPrimData,
    simdscalari primID)
{
    SWR_CONTEXT *pContext = pDC->pContext;

    AR_BEGIN(FEGeometryShader, pDC->drawId);

    const API_STATE& state = GetApiState(pDC);
    const SWR_GS_STATE* pState = &state.gsState;

    SWR_ASSERT(pGsOut != nullptr, "GS output buffer should be initialized");
    SWR_ASSERT(pCutBuffer != nullptr, "GS output cut buffer should be initialized");

    tlsGsContext.pStream = (uint8_t*)pGsOut;
    tlsGsContext.pCutOrStreamIdBuffer = (uint8_t*)pCutBuffer;
    tlsGsContext.PrimitiveID = primID;

    uint32_t numVertsPerPrim = NumVertsPerPrim(pa.binTopology, true);
    simdvector attrib[MAX_ATTRIBUTES];

    // assemble all attributes for the input primitive
    for (uint32_t slot = 0; slot < pState->numInputAttribs; ++slot)
    {
        uint32_t attribSlot = VERTEX_ATTRIB_START_SLOT + slot;
        pa.Assemble(attribSlot, attrib);

        for (uint32_t i = 0; i < numVertsPerPrim; ++i)
        {
            tlsGsContext.vert[i].attrib[attribSlot] = attrib[i];
        }
    }
    
    // assemble position
    pa.Assemble(VERTEX_POSITION_SLOT, attrib);
    for (uint32_t i = 0; i < numVertsPerPrim; ++i)
    {
        tlsGsContext.vert[i].attrib[VERTEX_POSITION_SLOT] = attrib[i];
    }

    const uint32_t vertexStride = sizeof(simdvertex);
    const uint32_t numSimdBatches = (state.gsState.maxNumVerts + KNOB_SIMD_WIDTH - 1) / KNOB_SIMD_WIDTH;
    const uint32_t inputPrimStride = numSimdBatches * vertexStride;
    const uint32_t instanceStride = inputPrimStride * KNOB_SIMD_WIDTH;
    uint32_t cutPrimStride;
    uint32_t cutInstanceStride;

    if (pState->isSingleStream)
    {
        cutPrimStride = (state.gsState.maxNumVerts + 7) / 8;
        cutInstanceStride = cutPrimStride * KNOB_SIMD_WIDTH;
    }
    else
    {
        cutPrimStride = AlignUp(state.gsState.maxNumVerts * 2 / 8, 4);
        cutInstanceStride = cutPrimStride * KNOB_SIMD_WIDTH;
    }

    // record valid prims from the frontend to avoid over binning the newly generated
    // prims from the GS
    uint32_t numInputPrims = pa.NumPrims();

    for (uint32_t instance = 0; instance < pState->instanceCount; ++instance)
    {
        tlsGsContext.InstanceID = instance;
        tlsGsContext.mask = GenerateMask(numInputPrims);

        // execute the geometry shader
        state.pfnGsFunc(GetPrivateState(pDC), &tlsGsContext);

        tlsGsContext.pStream += instanceStride;
        tlsGsContext.pCutOrStreamIdBuffer += cutInstanceStride;
    }

    // set up new binner and state for the GS output topology
    PFN_PROCESS_PRIMS pfnClipFunc = nullptr;
    if (HasRastT::value)
    {
        switch (pState->outputTopology)
        {
        case TOP_TRIANGLE_STRIP:    pfnClipFunc = ClipTriangles; break;
        case TOP_LINE_STRIP:        pfnClipFunc = ClipLines; break;
        case TOP_POINT_LIST:        pfnClipFunc = ClipPoints; break;
        default: SWR_ASSERT(false, "Unexpected GS output topology: %d", pState->outputTopology);
        }
    }

    // foreach input prim:
    // - setup a new PA based on the emitted verts for that prim
    // - loop over the new verts, calling PA to assemble each prim
    uint32_t* pVertexCount = (uint32_t*)&tlsGsContext.vertexCount;
    uint32_t* pPrimitiveId = (uint32_t*)&primID;

    uint32_t totalPrimsGenerated = 0;
    for (uint32_t inputPrim = 0; inputPrim < numInputPrims; ++inputPrim)
    {
        uint8_t* pInstanceBase = (uint8_t*)pGsOut + inputPrim * inputPrimStride;
        uint8_t* pCutBufferBase = (uint8_t*)pCutBuffer + inputPrim * cutPrimStride;
        for (uint32_t instance = 0; instance < pState->instanceCount; ++instance)
        {
            uint32_t numEmittedVerts = pVertexCount[inputPrim];
            if (numEmittedVerts == 0)
            {
                continue;
            }

            uint8_t* pBase = pInstanceBase + instance * instanceStride;
            uint8_t* pCutBase = pCutBufferBase + instance * cutInstanceStride;
            
            uint32_t numAttribs = state.feNumAttributes;

            for (uint32_t stream = 0; stream < MAX_SO_STREAMS; ++stream)
            {
                bool processCutVerts = false;

                uint8_t* pCutBuffer = pCutBase;

                // assign default stream ID, only relevant when GS is outputting a single stream
                uint32_t streamID = 0;
                if (pState->isSingleStream)
                {
                    processCutVerts = true;
                    streamID = pState->singleStreamID;
                    if (streamID != stream) continue;
                }
                else
                {
                    // early exit if this stream is not enabled for streamout
                    if (HasStreamOutT::value && !state.soState.streamEnable[stream])
                    {
                        continue;
                    }

                    // multi-stream output, need to translate StreamID buffer to a cut buffer
                    ProcessStreamIdBuffer(stream, pCutBase, numEmittedVerts, (uint8_t*)pStreamCutBuffer);
                    pCutBuffer = (uint8_t*)pStreamCutBuffer;
                    processCutVerts = false;
                }

                PA_STATE_CUT gsPa(pDC, pBase, numEmittedVerts, pCutBuffer, numEmittedVerts, numAttribs, pState->outputTopology, processCutVerts);

                while (gsPa.GetNextStreamOutput())
                {
                    do
                    {
                        bool assemble = gsPa.Assemble(VERTEX_POSITION_SLOT, attrib);

                        if (assemble)
                        {
                            totalPrimsGenerated += gsPa.NumPrims();

                            if (HasStreamOutT::value)
                            {
                                StreamOut(pDC, gsPa, workerId, pSoPrimData, stream);
                            }

                            if (HasRastT::value && state.soState.streamToRasterizer == stream)
                            {
                                simdscalari vPrimId;
                                // pull primitiveID from the GS output if available
                                if (state.gsState.emitsPrimitiveID)
                                {
                                    simdvector primIdAttrib[3];
                                    gsPa.Assemble(VERTEX_PRIMID_SLOT, primIdAttrib);
                                    vPrimId = _simd_castps_si(primIdAttrib[0].x);
                                }
                                else
                                {
                                    vPrimId = _simd_set1_epi32(pPrimitiveId[inputPrim]);
                                }

                                // use viewport array index if GS declares it as an output attribute. Otherwise use index 0.
                                simdscalari vViewPortIdx;
                                if (state.gsState.emitsViewportArrayIndex)
                                {
                                    simdvector vpiAttrib[3];
                                    gsPa.Assemble(VERTEX_VIEWPORT_ARRAY_INDEX_SLOT, vpiAttrib);

                                    // OOB indices => forced to zero.
                                    simdscalari vNumViewports = _simd_set1_epi32(KNOB_NUM_VIEWPORTS_SCISSORS);
                                    simdscalari vClearMask = _simd_cmplt_epi32(_simd_castps_si(vpiAttrib[0].x), vNumViewports);
                                    vpiAttrib[0].x = _simd_and_ps(_simd_castsi_ps(vClearMask), vpiAttrib[0].x);

                                    vViewPortIdx = _simd_castps_si(vpiAttrib[0].x);
                                }
                                else
                                {
                                    vViewPortIdx = _simd_set1_epi32(0);
                                }

                                pfnClipFunc(pDC, gsPa, workerId, attrib, GenMask(gsPa.NumPrims()), vPrimId, vViewPortIdx);
                            }
                        }
                    } while (gsPa.NextPrim());
                }
            }
        }
    }

    // update GS pipeline stats
    UPDATE_STAT_FE(GsInvocations, numInputPrims * pState->instanceCount);
    UPDATE_STAT_FE(GsPrimitives, totalPrimsGenerated);

    AR_END(FEGeometryShader, 1);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Allocate GS buffers
/// @param pDC - pointer to draw context.
/// @param state - API state
/// @param ppGsOut - pointer to GS output buffer allocation
/// @param ppCutBuffer - pointer to GS output cut buffer allocation
static INLINE void AllocateGsBuffers(DRAW_CONTEXT* pDC, const API_STATE& state, void** ppGsOut, void** ppCutBuffer,
    void **ppStreamCutBuffer)
{
    auto pArena = pDC->pArena;
    SWR_ASSERT(pArena != nullptr);
    SWR_ASSERT(state.gsState.gsEnable);
    // allocate arena space to hold GS output verts
    // @todo pack attribs
    // @todo support multiple streams
    const uint32_t vertexStride = sizeof(simdvertex);
    const uint32_t numSimdBatches = (state.gsState.maxNumVerts + KNOB_SIMD_WIDTH - 1) / KNOB_SIMD_WIDTH;
    uint32_t size = state.gsState.instanceCount * numSimdBatches * vertexStride * KNOB_SIMD_WIDTH;
    *ppGsOut = pArena->AllocAligned(size, KNOB_SIMD_WIDTH * sizeof(float));

    const uint32_t cutPrimStride = (state.gsState.maxNumVerts + 7) / 8;
    const uint32_t streamIdPrimStride = AlignUp(state.gsState.maxNumVerts * 2 / 8, 4);
    const uint32_t cutBufferSize = cutPrimStride * state.gsState.instanceCount * KNOB_SIMD_WIDTH;
    const uint32_t streamIdSize = streamIdPrimStride * state.gsState.instanceCount * KNOB_SIMD_WIDTH;

    // allocate arena space to hold cut or streamid buffer, which is essentially a bitfield sized to the
    // maximum vertex output as defined by the GS state, per SIMD lane, per GS instance

    // allocate space for temporary per-stream cut buffer if multi-stream is enabled
    if (state.gsState.isSingleStream)
    {
        *ppCutBuffer = pArena->AllocAligned(cutBufferSize, KNOB_SIMD_WIDTH * sizeof(float));
        *ppStreamCutBuffer = nullptr;
    }
    else
    {
        *ppCutBuffer = pArena->AllocAligned(streamIdSize, KNOB_SIMD_WIDTH * sizeof(float));
        *ppStreamCutBuffer = pArena->AllocAligned(cutBufferSize, KNOB_SIMD_WIDTH * sizeof(float));
    }

}

//////////////////////////////////////////////////////////////////////////
/// @brief Contains all data generated by the HS and passed to the
/// tessellator and DS.
struct TessellationThreadLocalData
{
    SWR_HS_CONTEXT hsContext;
    ScalarPatch patchData[KNOB_SIMD_WIDTH];
    void* pTxCtx;
    size_t tsCtxSize;

    simdscalar* pDSOutput;
    size_t numDSOutputVectors;
};

THREAD TessellationThreadLocalData* gt_pTessellationThreadData = nullptr;

//////////////////////////////////////////////////////////////////////////
/// @brief Allocate tessellation data for this worker thread.
INLINE
static void AllocateTessellationData(SWR_CONTEXT* pContext)
{
    /// @TODO - Don't use thread local storage.  Use Worker local storage instead.
    if (gt_pTessellationThreadData == nullptr)
    {
        gt_pTessellationThreadData = (TessellationThreadLocalData*)
            AlignedMalloc(sizeof(TessellationThreadLocalData), 64);
        memset(gt_pTessellationThreadData, 0, sizeof(*gt_pTessellationThreadData));
    }
}

//////////////////////////////////////////////////////////////////////////
/// @brief Implements Tessellation Stages.
/// @param pDC - pointer to draw context.
/// @param workerId - thread's worker id. Even thread has a unique id.
/// @param pa - The primitive assembly object.
/// @param pGsOut - output stream for GS
template <
    typename HasGeometryShaderT,
    typename HasStreamOutT,
    typename HasRastT>
static void TessellationStages(
    DRAW_CONTEXT *pDC,
    uint32_t workerId,
    PA_STATE& pa,
    void* pGsOut,
    void* pCutBuffer,
    void* pCutStreamBuffer,
    uint32_t* pSoPrimData,
    simdscalari primID)
{
    SWR_CONTEXT *pContext = pDC->pContext;
    const API_STATE& state = GetApiState(pDC);
    const SWR_TS_STATE& tsState = state.tsState;

    SWR_ASSERT(gt_pTessellationThreadData);

    HANDLE tsCtx = TSInitCtx(
        tsState.domain,
        tsState.partitioning,
        tsState.tsOutputTopology,
        gt_pTessellationThreadData->pTxCtx,
        gt_pTessellationThreadData->tsCtxSize);
    if (tsCtx == nullptr)
    {
        gt_pTessellationThreadData->pTxCtx = AlignedMalloc(gt_pTessellationThreadData->tsCtxSize, 64);
        tsCtx = TSInitCtx(
            tsState.domain,
            tsState.partitioning,
            tsState.tsOutputTopology,
            gt_pTessellationThreadData->pTxCtx,
            gt_pTessellationThreadData->tsCtxSize);
    }
    SWR_ASSERT(tsCtx);

    PFN_PROCESS_PRIMS pfnClipFunc = nullptr;
    if (HasRastT::value)
    {
        switch (tsState.postDSTopology)
        {
        case TOP_TRIANGLE_LIST: pfnClipFunc = ClipTriangles; break;
        case TOP_LINE_LIST:     pfnClipFunc = ClipLines; break;
        case TOP_POINT_LIST:    pfnClipFunc = ClipPoints; break;
        default: SWR_ASSERT(false, "Unexpected DS output topology: %d", tsState.postDSTopology);
        }
    }

    SWR_HS_CONTEXT& hsContext = gt_pTessellationThreadData->hsContext;
    hsContext.pCPout = gt_pTessellationThreadData->patchData;
    hsContext.PrimitiveID = primID;

    uint32_t numVertsPerPrim = NumVertsPerPrim(pa.binTopology, false);
    // Max storage for one attribute for an entire simdprimitive
    simdvector simdattrib[MAX_NUM_VERTS_PER_PRIM];

    // assemble all attributes for the input primitives
    for (uint32_t slot = 0; slot < tsState.numHsInputAttribs; ++slot)
    {
        uint32_t attribSlot = VERTEX_ATTRIB_START_SLOT + slot;
        pa.Assemble(attribSlot, simdattrib);

        for (uint32_t i = 0; i < numVertsPerPrim; ++i)
        {
            hsContext.vert[i].attrib[attribSlot] = simdattrib[i];
        }
    }

#if defined(_DEBUG)
    memset(hsContext.pCPout, 0x90, sizeof(ScalarPatch) * KNOB_SIMD_WIDTH);
#endif

    uint32_t numPrims = pa.NumPrims();
    hsContext.mask = GenerateMask(numPrims);

    // Run the HS
    AR_BEGIN(FEHullShader, pDC->drawId);
    state.pfnHsFunc(GetPrivateState(pDC), &hsContext);
    AR_END(FEHullShader, 0);

    UPDATE_STAT_FE(HsInvocations, numPrims);

    const uint32_t* pPrimId = (const uint32_t*)&primID;

    for (uint32_t p = 0; p < numPrims; ++p)
    {
        // Run Tessellator
        SWR_TS_TESSELLATED_DATA tsData = { 0 };
        AR_BEGIN(FETessellation, pDC->drawId);
        TSTessellate(tsCtx, hsContext.pCPout[p].tessFactors, tsData);
        AR_END(FETessellation, 0);

        if (tsData.NumPrimitives == 0)
        {
            continue;
        }
        SWR_ASSERT(tsData.NumDomainPoints);

        // Allocate DS Output memory
        uint32_t requiredDSVectorInvocations = AlignUp(tsData.NumDomainPoints, KNOB_SIMD_WIDTH) / KNOB_SIMD_WIDTH;
        size_t requiredDSOutputVectors = requiredDSVectorInvocations * tsState.numDsOutputAttribs;
        size_t requiredAllocSize = sizeof(simdvector) * requiredDSOutputVectors;
        if (requiredDSOutputVectors > gt_pTessellationThreadData->numDSOutputVectors)
        {
            AlignedFree(gt_pTessellationThreadData->pDSOutput);
            gt_pTessellationThreadData->pDSOutput = (simdscalar*)AlignedMalloc(requiredAllocSize, 64);
            gt_pTessellationThreadData->numDSOutputVectors = requiredDSOutputVectors;
        }
        SWR_ASSERT(gt_pTessellationThreadData->pDSOutput);
        SWR_ASSERT(gt_pTessellationThreadData->numDSOutputVectors >= requiredDSOutputVectors);

#if defined(_DEBUG)
        memset(gt_pTessellationThreadData->pDSOutput, 0x90, requiredAllocSize);
#endif

        // Run Domain Shader
        SWR_DS_CONTEXT dsContext;
        dsContext.PrimitiveID = pPrimId[p];
        dsContext.pCpIn = &hsContext.pCPout[p];
        dsContext.pDomainU = (simdscalar*)tsData.pDomainPointsU;
        dsContext.pDomainV = (simdscalar*)tsData.pDomainPointsV;
        dsContext.pOutputData = gt_pTessellationThreadData->pDSOutput;
        dsContext.vectorStride = requiredDSVectorInvocations;

        uint32_t dsInvocations = 0;

        for (dsContext.vectorOffset = 0; dsContext.vectorOffset < requiredDSVectorInvocations; ++dsContext.vectorOffset)
        {
            dsContext.mask = GenerateMask(tsData.NumDomainPoints - dsInvocations);

            AR_BEGIN(FEDomainShader, pDC->drawId);
            state.pfnDsFunc(GetPrivateState(pDC), &dsContext);
            AR_END(FEDomainShader, 0);

            dsInvocations += KNOB_SIMD_WIDTH;
        }
        UPDATE_STAT_FE(DsInvocations, tsData.NumDomainPoints);

        PA_TESS tessPa(
            pDC,
            dsContext.pOutputData,
            dsContext.vectorStride,
            tsState.numDsOutputAttribs,
            tsData.ppIndices,
            tsData.NumPrimitives,
            tsState.postDSTopology);

        while (tessPa.HasWork())
        {
            if (HasGeometryShaderT::value)
            {
                GeometryShaderStage<HasStreamOutT, HasRastT>(
                    pDC, workerId, tessPa, pGsOut, pCutBuffer, pCutStreamBuffer, pSoPrimData,
                    _simd_set1_epi32(dsContext.PrimitiveID));
            }
            else
            {
                if (HasStreamOutT::value)
                {
                    StreamOut(pDC, tessPa, workerId, pSoPrimData, 0);
                }

                if (HasRastT::value)
                {
                    simdvector prim[3]; // Only deal with triangles, lines, or points
                    AR_BEGIN(FEPAAssemble, pDC->drawId);
#if SWR_ENABLE_ASSERTS
                    bool assemble =
#endif
                        tessPa.Assemble(VERTEX_POSITION_SLOT, prim);
                    AR_END(FEPAAssemble, 1);
                    SWR_ASSERT(assemble);

                    SWR_ASSERT(pfnClipFunc);
                    pfnClipFunc(pDC, tessPa, workerId, prim,
                        GenMask(tessPa.NumPrims()), _simd_set1_epi32(dsContext.PrimitiveID), _simd_set1_epi32(0));
                }
            }

            tessPa.NextPrim();

        } // while (tessPa.HasWork())
    } // for (uint32_t p = 0; p < numPrims; ++p)

    TSDestroyCtx(tsCtx);
}

//////////////////////////////////////////////////////////////////////////
/// @brief FE handler for SwrDraw.
/// @tparam IsIndexedT - Is indexed drawing enabled
/// @tparam HasTessellationT - Is tessellation enabled
/// @tparam HasGeometryShaderT::value - Is the geometry shader stage enabled
/// @tparam HasStreamOutT - Is stream-out enabled
/// @tparam HasRastT - Is rasterization enabled
/// @param pContext - pointer to SWR context.
/// @param pDC - pointer to draw context.
/// @param workerId - thread's worker id.
/// @param pUserData - Pointer to DRAW_WORK
template <
    typename IsIndexedT,
    typename IsCutIndexEnabledT,
    typename HasTessellationT,
    typename HasGeometryShaderT,
    typename HasStreamOutT,
    typename HasRastT>
void ProcessDraw(
    SWR_CONTEXT *pContext,
    DRAW_CONTEXT *pDC,
    uint32_t workerId,
    void *pUserData)
{

#if KNOB_ENABLE_TOSS_POINTS
    if (KNOB_TOSS_QUEUE_FE)
    {
        return;
    }
#endif

    AR_BEGIN(FEProcessDraw, pDC->drawId);

    DRAW_WORK&          work = *(DRAW_WORK*)pUserData;
    const API_STATE&    state = GetApiState(pDC);
    __m256i             vScale = _mm256_set_epi32(7, 6, 5, 4, 3, 2, 1, 0);
    SWR_VS_CONTEXT      vsContext;
    simdvertex          vin;

    int indexSize = 0;
    uint32_t endVertex = work.numVerts; 

    const int32_t* pLastRequestedIndex = nullptr;
    if (IsIndexedT::value)
    {
        switch (work.type)
        {
        case R32_UINT:
            indexSize = sizeof(uint32_t);
            pLastRequestedIndex = &(work.pIB[endVertex]);
            break;
        case R16_UINT:
            indexSize = sizeof(uint16_t);
            // nasty address offset to last index
            pLastRequestedIndex = (int32_t*)(&(((uint16_t*)work.pIB)[endVertex]));
            break;
        case R8_UINT:
            indexSize = sizeof(uint8_t);
            // nasty address offset to last index
            pLastRequestedIndex = (int32_t*)(&(((uint8_t*)work.pIB)[endVertex]));
            break;
        default:
            SWR_ASSERT(0);
        }
    }
    else
    {
        // No cuts, prune partial primitives.
        endVertex = GetNumVerts(state.topology, GetNumPrims(state.topology, work.numVerts));
    }

    SWR_FETCH_CONTEXT fetchInfo = { 0 };
    fetchInfo.pStreams = &state.vertexBuffers[0];
    fetchInfo.StartInstance = work.startInstance;
    fetchInfo.StartVertex = 0;

    vsContext.pVin = &vin;

    if (IsIndexedT::value)
    {
        fetchInfo.BaseVertex = work.baseVertex;

        // if the entire index buffer isn't being consumed, set the last index
        // so that fetches < a SIMD wide will be masked off
        fetchInfo.pLastIndex = (const int32_t*)(((uint8_t*)state.indexBuffer.pIndices) + state.indexBuffer.size);
        if (pLastRequestedIndex < fetchInfo.pLastIndex)
        {
            fetchInfo.pLastIndex = pLastRequestedIndex;
        }
    }
    else
    {
        fetchInfo.StartVertex = work.startVertex;
    }

#ifdef KNOB_ENABLE_RDTSC
    uint32_t numPrims = GetNumPrims(state.topology, work.numVerts);
#endif

    void* pGsOut = nullptr;
    void* pCutBuffer = nullptr;
    void* pStreamCutBuffer = nullptr;
    if (HasGeometryShaderT::value)
    {
        AllocateGsBuffers(pDC, state, &pGsOut, &pCutBuffer, &pStreamCutBuffer);
    }

    if (HasTessellationT::value)
    {
        SWR_ASSERT(state.tsState.tsEnable == true);
        SWR_ASSERT(state.pfnHsFunc != nullptr);
        SWR_ASSERT(state.pfnDsFunc != nullptr);

        AllocateTessellationData(pContext);
    }
    else
    {
        SWR_ASSERT(state.tsState.tsEnable == false);
        SWR_ASSERT(state.pfnHsFunc == nullptr);
        SWR_ASSERT(state.pfnDsFunc == nullptr);
    }

    // allocate space for streamout input prim data
    uint32_t* pSoPrimData = nullptr;
    if (HasStreamOutT::value)
    {
        pSoPrimData = (uint32_t*)pDC->pArena->AllocAligned(4096, 16);
    }

    // choose primitive assembler
    PA_FACTORY<IsIndexedT, IsCutIndexEnabledT> paFactory(pDC, state.topology, work.numVerts);
    PA_STATE& pa = paFactory.GetPA();

    /// @todo: temporarily move instance loop in the FE to ensure SO ordering
    for (uint32_t instanceNum = 0; instanceNum < work.numInstances; instanceNum++)
    {
        simdscalari vIndex;
        uint32_t  i = 0;

        if (IsIndexedT::value)
        {
            fetchInfo.pIndices = work.pIB;
        }
        else
        {
            vIndex = _simd_add_epi32(_simd_set1_epi32(work.startVertexID), vScale);
            fetchInfo.pIndices = (const int32_t*)&vIndex;
        }

        fetchInfo.CurInstance = instanceNum;
        vsContext.InstanceID = instanceNum;

        while (pa.HasWork())
        {
            // PaGetNextVsOutput currently has the side effect of updating some PA state machine state.
            // So we need to keep this outside of (i < endVertex) check.
            simdmask* pvCutIndices = nullptr;
            if (IsIndexedT::value)
            {
                pvCutIndices = &pa.GetNextVsIndices();
            }

            simdvertex& vout = pa.GetNextVsOutput();
            vsContext.pVout = &vout;

            if (i < endVertex)
            {

                // 1. Execute FS/VS for a single SIMD.
                AR_BEGIN(FEFetchShader, pDC->drawId);
                state.pfnFetchFunc(fetchInfo, vin);
                AR_END(FEFetchShader, 0);

                // forward fetch generated vertex IDs to the vertex shader
                vsContext.VertexID = fetchInfo.VertexID;

                // Setup active mask for vertex shader.
                vsContext.mask = GenerateMask(endVertex - i);

                // forward cut mask to the PA
                if (IsIndexedT::value)
                {
                    *pvCutIndices = _simd_movemask_ps(_simd_castsi_ps(fetchInfo.CutMask));
                }

                UPDATE_STAT_FE(IaVertices, GetNumInvocations(i, endVertex));

#if KNOB_ENABLE_TOSS_POINTS
                if (!KNOB_TOSS_FETCH)
#endif
                {
                    AR_BEGIN(FEVertexShader, pDC->drawId);
                    state.pfnVertexFunc(GetPrivateState(pDC), &vsContext);
                    AR_END(FEVertexShader, 0);

                    UPDATE_STAT_FE(VsInvocations, GetNumInvocations(i, endVertex));
                }
            }

            // 2. Assemble primitives given the last two SIMD.
            do
            {
                simdvector prim[MAX_NUM_VERTS_PER_PRIM];
                // PaAssemble returns false if there is not enough verts to assemble.
                AR_BEGIN(FEPAAssemble, pDC->drawId);
                bool assemble = pa.Assemble(VERTEX_POSITION_SLOT, prim);
                AR_END(FEPAAssemble, 1);

#if KNOB_ENABLE_TOSS_POINTS
                if (!KNOB_TOSS_FETCH)
#endif
                {
#if KNOB_ENABLE_TOSS_POINTS
                    if (!KNOB_TOSS_VS)
#endif
                    {
                        if (assemble)
                        {
                            UPDATE_STAT_FE(IaPrimitives, pa.NumPrims());

                            if (HasTessellationT::value)
                            {
                                TessellationStages<HasGeometryShaderT, HasStreamOutT, HasRastT>(
                                    pDC, workerId, pa, pGsOut, pCutBuffer, pStreamCutBuffer, pSoPrimData, pa.GetPrimID(work.startPrimID));
                            }
                            else if (HasGeometryShaderT::value)
                            {
                                GeometryShaderStage<HasStreamOutT, HasRastT>(
                                    pDC, workerId, pa, pGsOut, pCutBuffer, pStreamCutBuffer, pSoPrimData, pa.GetPrimID(work.startPrimID));
                            }
                            else
                            {
                                // If streamout is enabled then stream vertices out to memory.
                                if (HasStreamOutT::value)
                                {
                                    StreamOut(pDC, pa, workerId, pSoPrimData, 0);
                                }

                                if (HasRastT::value)
                                {
                                    SWR_ASSERT(pDC->pState->pfnProcessPrims);
                                    pDC->pState->pfnProcessPrims(pDC, pa, workerId, prim,
                                        GenMask(pa.NumPrims()), pa.GetPrimID(work.startPrimID), _simd_set1_epi32(0));
                                }
                            }
                        }
                    }
                }
            } while (pa.NextPrim());

            i += KNOB_SIMD_WIDTH;
            if (IsIndexedT::value)
            {
                fetchInfo.pIndices = (int*)((uint8_t*)fetchInfo.pIndices + KNOB_SIMD_WIDTH * indexSize);
            }
            else
            {
                vIndex = _simd_add_epi32(vIndex, _simd_set1_epi32(KNOB_SIMD_WIDTH));
            }
        }
        pa.Reset();
    }

    AR_END(FEProcessDraw, numPrims * work.numInstances);
}

struct FEDrawChooser
{
    typedef PFN_FE_WORK_FUNC FuncType;

    template <typename... ArgsB>
    static FuncType GetFunc()
    {
        return ProcessDraw<ArgsB...>;
    }
};


// Selector for correct templated Draw front-end function
PFN_FE_WORK_FUNC GetProcessDrawFunc(
    bool IsIndexed,
    bool IsCutIndexEnabled,
    bool HasTessellation,
    bool HasGeometryShader,
    bool HasStreamOut,
    bool HasRasterization)
{
    return TemplateArgUnroller<FEDrawChooser>::GetFunc(IsIndexed, IsCutIndexEnabled, HasTessellation, HasGeometryShader, HasStreamOut, HasRasterization);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Processes attributes for the backend based on linkage mask and
///        linkage map.  Essentially just doing an SOA->AOS conversion and pack.
/// @param pDC - Draw context
/// @param pa - Primitive Assembly state
/// @param linkageMask - Specifies which VS outputs are routed to PS.
/// @param pLinkageMap - maps VS attribute slot to PS slot
/// @param triIndex - Triangle to process attributes for
/// @param pBuffer - Output result
template<typename NumVertsT, typename IsSwizzledT, typename HasConstantInterpT, typename IsDegenerate>
INLINE void ProcessAttributes(
    DRAW_CONTEXT *pDC,
    PA_STATE&pa,
    uint32_t triIndex,
    uint32_t primId,
    float *pBuffer)
{
    static_assert(NumVertsT::value > 0 && NumVertsT::value <= 3, "Invalid value for NumVertsT");
    const SWR_BACKEND_STATE& backendState = pDC->pState->state.backendState;
    // Conservative Rasterization requires degenerate tris to have constant attribute interpolation
    LONG constantInterpMask = IsDegenerate::value ? 0xFFFFFFFF : backendState.constantInterpolationMask;
    const uint32_t provokingVertex = pDC->pState->state.frontendState.topologyProvokingVertex;
    const PRIMITIVE_TOPOLOGY topo = pDC->pState->state.topology;

    static const float constTable[3][4] = {
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f}
    };

    for (uint32_t i = 0; i < backendState.numAttributes; ++i)
    {
        uint32_t inputSlot;
        if (IsSwizzledT::value)
        {
            SWR_ATTRIB_SWIZZLE attribSwizzle = backendState.swizzleMap[i];
            inputSlot = VERTEX_ATTRIB_START_SLOT + attribSwizzle.sourceAttrib;

        }
        else
        {
            inputSlot = VERTEX_ATTRIB_START_SLOT + i;
        }

        __m128 attrib[3];    // triangle attribs (always 4 wide)
        float* pAttribStart = pBuffer;

        if (HasConstantInterpT::value || IsDegenerate::value)
        {
            if (_bittest(&constantInterpMask, i))
            {
                uint32_t vid;
                uint32_t adjustedTriIndex;
                static const uint32_t tristripProvokingVertex[] = { 0, 2, 1 };
                static const int32_t quadProvokingTri[2][4] = { {0, 0, 0, 1}, {0, -1, 0, 0} };
                static const uint32_t quadProvokingVertex[2][4] = { {0, 1, 2, 2}, {0, 1, 1, 2} };
                static const int32_t qstripProvokingTri[2][4] = { {0, 0, 0, 1}, {-1, 0, 0, 0} };
                static const uint32_t qstripProvokingVertex[2][4] = { {0, 1, 2, 1}, {0, 0, 2, 1} };

                switch (topo) {
                case TOP_QUAD_LIST:
                    adjustedTriIndex = triIndex + quadProvokingTri[triIndex & 1][provokingVertex];
                    vid = quadProvokingVertex[triIndex & 1][provokingVertex];
                    break;
                case TOP_QUAD_STRIP:
                    adjustedTriIndex = triIndex + qstripProvokingTri[triIndex & 1][provokingVertex];
                    vid = qstripProvokingVertex[triIndex & 1][provokingVertex];
                    break;
                case TOP_TRIANGLE_STRIP:
                    adjustedTriIndex = triIndex;
                    vid = (triIndex & 1)
                        ? tristripProvokingVertex[provokingVertex]
                        : provokingVertex;
                    break;
                default:
                    adjustedTriIndex = triIndex;
                    vid = provokingVertex;
                    break;
                }

                pa.AssembleSingle(inputSlot, adjustedTriIndex, attrib);

                for (uint32_t i = 0; i < NumVertsT::value; ++i)
                {
                    _mm_store_ps(pBuffer, attrib[vid]);
                    pBuffer += 4;
                }
            }
            else
            {
                pa.AssembleSingle(inputSlot, triIndex, attrib);

                for (uint32_t i = 0; i < NumVertsT::value; ++i)
                {
                    _mm_store_ps(pBuffer, attrib[i]);
                    pBuffer += 4;
                }
            }
        }
        else
        {
            pa.AssembleSingle(inputSlot, triIndex, attrib);

            for (uint32_t i = 0; i < NumVertsT::value; ++i)
            {
                _mm_store_ps(pBuffer, attrib[i]);
                pBuffer += 4;
            }
        }

        // pad out the attrib buffer to 3 verts to ensure the triangle
        // interpolation code in the pixel shader works correctly for the
        // 3 topologies - point, line, tri.  This effectively zeros out the
        // effect of the missing vertices in the triangle interpolation.
        for (uint32_t v = NumVertsT::value; v < 3; ++v)
        {
            _mm_store_ps(pBuffer, attrib[NumVertsT::value - 1]);
            pBuffer += 4;
        }

        // check for constant source overrides
        if (IsSwizzledT::value)
        {
            uint32_t mask = backendState.swizzleMap[i].componentOverrideMask;
            if (mask)
            {
                DWORD comp;
                while (_BitScanForward(&comp, mask))
                {
                    mask &= ~(1 << comp);

                    float constantValue = 0.0f;
                    switch ((SWR_CONSTANT_SOURCE)backendState.swizzleMap[i].constantSource)
                    {
                    case SWR_CONSTANT_SOURCE_CONST_0000:
                    case SWR_CONSTANT_SOURCE_CONST_0001_FLOAT:
                    case SWR_CONSTANT_SOURCE_CONST_1111_FLOAT:
                        constantValue = constTable[backendState.swizzleMap[i].constantSource][comp];
                        break;
                    case SWR_CONSTANT_SOURCE_PRIM_ID:
                        constantValue = *(float*)&primId;
                        break;
                    }

                    // apply constant value to all 3 vertices
                    for (uint32_t v = 0; v < 3; ++v)
                    {
                        pAttribStart[comp + v * 4] = constantValue;
                    }
                }
            }
        }
    }
}


typedef void(*PFN_PROCESS_ATTRIBUTES)(DRAW_CONTEXT*, PA_STATE&, uint32_t, uint32_t, float*);

struct ProcessAttributesChooser
{
    typedef PFN_PROCESS_ATTRIBUTES FuncType;

    template <typename... ArgsB>
    static FuncType GetFunc()
    {
        return ProcessAttributes<ArgsB...>;
    }
};

PFN_PROCESS_ATTRIBUTES GetProcessAttributesFunc(uint32_t NumVerts, bool IsSwizzled, bool HasConstantInterp, bool IsDegenerate = false)
{
    return TemplateArgUnroller<ProcessAttributesChooser>::GetFunc(IntArg<1, 3>{NumVerts}, IsSwizzled, HasConstantInterp, IsDegenerate);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Processes enabled user clip distances. Loads the active clip
///        distances from the PA, sets up barycentric equations, and
///        stores the results to the output buffer
/// @param pa - Primitive Assembly state
/// @param primIndex - primitive index to process
/// @param clipDistMask - mask of enabled clip distances
/// @param pUserClipBuffer - buffer to store results
template<uint32_t NumVerts>
void ProcessUserClipDist(PA_STATE& pa, uint32_t primIndex, uint8_t clipDistMask, float* pUserClipBuffer)
{
    DWORD clipDist;
    while (_BitScanForward(&clipDist, clipDistMask))
    {
        clipDistMask &= ~(1 << clipDist);
        uint32_t clipSlot = clipDist >> 2;
        uint32_t clipComp = clipDist & 0x3;
        uint32_t clipAttribSlot = clipSlot == 0 ?
            VERTEX_CLIPCULL_DIST_LO_SLOT : VERTEX_CLIPCULL_DIST_HI_SLOT;

        __m128 primClipDist[3];
        pa.AssembleSingle(clipAttribSlot, primIndex, primClipDist);

        float vertClipDist[NumVerts];
        for (uint32_t e = 0; e < NumVerts; ++e)
        {
            OSALIGNSIMD(float) aVertClipDist[4];
            _mm_store_ps(aVertClipDist, primClipDist[e]);
            vertClipDist[e] = aVertClipDist[clipComp];
        };

        // setup plane equations for barycentric interpolation in the backend
        float baryCoeff[NumVerts];
        for (uint32_t e = 0; e < NumVerts - 1; ++e)
        {
            baryCoeff[e] = vertClipDist[e] - vertClipDist[NumVerts - 1];
        }
        baryCoeff[NumVerts - 1] = vertClipDist[NumVerts - 1];

        for (uint32_t e = 0; e < NumVerts; ++e)
        {
            *(pUserClipBuffer++) = baryCoeff[e];
        }
    }
}

//////////////////////////////////////////////////////////////////////////
/// @brief Convert the X,Y coords of a triangle to the requested Fixed 
/// Point precision from FP32.
template <typename PT = FixedPointTraits<Fixed_16_8>>
INLINE simdscalari fpToFixedPointVertical(const simdscalar vIn)
{
    simdscalar vFixed = _simd_mul_ps(vIn, _simd_set1_ps(PT::ScaleT::value));
    return _simd_cvtps_epi32(vFixed);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Helper function to set the X,Y coords of a triangle to the 
/// requested Fixed Point precision from FP32.
/// @param tri: simdvector[3] of FP triangle verts
/// @param vXi: fixed point X coords of tri verts
/// @param vYi: fixed point Y coords of tri verts
INLINE static void FPToFixedPoint(const simdvector * const tri, simdscalari (&vXi)[3], simdscalari (&vYi)[3])
{
    vXi[0] = fpToFixedPointVertical(tri[0].x);
    vYi[0] = fpToFixedPointVertical(tri[0].y);
    vXi[1] = fpToFixedPointVertical(tri[1].x);
    vYi[1] = fpToFixedPointVertical(tri[1].y);
    vXi[2] = fpToFixedPointVertical(tri[2].x);
    vYi[2] = fpToFixedPointVertical(tri[2].y);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Calculate bounding box for current triangle
/// @tparam CT: ConservativeRastFETraits type
/// @param vX: fixed point X position for triangle verts
/// @param vY: fixed point Y position for triangle verts
/// @param bbox: fixed point bbox
/// *Note*: expects vX, vY to be in the correct precision for the type 
/// of rasterization. This avoids unnecessary FP->fixed conversions.
template <typename CT>
INLINE void calcBoundingBoxIntVertical(const simdvector * const tri, simdscalari (&vX)[3], simdscalari (&vY)[3], simdBBox &bbox)
{
    simdscalari vMinX = vX[0];
    vMinX = _simd_min_epi32(vMinX, vX[1]);
    vMinX = _simd_min_epi32(vMinX, vX[2]);

    simdscalari vMaxX = vX[0];
    vMaxX = _simd_max_epi32(vMaxX, vX[1]);
    vMaxX = _simd_max_epi32(vMaxX, vX[2]);

    simdscalari vMinY = vY[0];
    vMinY = _simd_min_epi32(vMinY, vY[1]);
    vMinY = _simd_min_epi32(vMinY, vY[2]);

    simdscalari vMaxY = vY[0];
    vMaxY = _simd_max_epi32(vMaxY, vY[1]);
    vMaxY = _simd_max_epi32(vMaxY, vY[2]);

    bbox.xmin = vMinX;
    bbox.xmax = vMaxX;
    bbox.ymin = vMinY;
    bbox.ymax = vMaxY;
}

//////////////////////////////////////////////////////////////////////////
/// @brief FEConservativeRastT specialization of calcBoundingBoxIntVertical
/// Offsets BBox for conservative rast
template <>
INLINE void calcBoundingBoxIntVertical<FEConservativeRastT>(const simdvector * const tri, simdscalari (&vX)[3], simdscalari (&vY)[3], simdBBox &bbox)
{
    // FE conservative rast traits
    typedef FEConservativeRastT CT;

    simdscalari vMinX = vX[0];
    vMinX = _simd_min_epi32(vMinX, vX[1]);
    vMinX = _simd_min_epi32(vMinX, vX[2]);

    simdscalari vMaxX = vX[0];
    vMaxX = _simd_max_epi32(vMaxX, vX[1]);
    vMaxX = _simd_max_epi32(vMaxX, vX[2]);

    simdscalari vMinY = vY[0];
    vMinY = _simd_min_epi32(vMinY, vY[1]);
    vMinY = _simd_min_epi32(vMinY, vY[2]);

    simdscalari vMaxY = vY[0];
    vMaxY = _simd_max_epi32(vMaxY, vY[1]);
    vMaxY = _simd_max_epi32(vMaxY, vY[2]);
    
    /// Bounding box needs to be expanded by 1/512 before snapping to 16.8 for conservative rasterization
    /// expand bbox by 1/256; coverage will be correctly handled in the rasterizer.
    bbox.xmin = _simd_sub_epi32(vMinX, _simd_set1_epi32(CT::BoundingBoxOffsetT::value));
    bbox.xmax = _simd_add_epi32(vMaxX, _simd_set1_epi32(CT::BoundingBoxOffsetT::value));
    bbox.ymin = _simd_sub_epi32(vMinY, _simd_set1_epi32(CT::BoundingBoxOffsetT::value));
    bbox.ymax = _simd_add_epi32(vMaxY, _simd_set1_epi32(CT::BoundingBoxOffsetT::value));
}

//////////////////////////////////////////////////////////////////////////
/// @brief Bin triangle primitives to macro tiles. Performs setup, clipping
///        culling, viewport transform, etc.
/// @param pDC - pointer to draw context.
/// @param pa - The primitive assembly object.
/// @param workerId - thread's worker id. Even thread has a unique id.
/// @param tri - Contains triangle position data for SIMDs worth of triangles.
/// @param primID - Primitive ID for each triangle.
/// @param viewportIdx - viewport array index for each triangle.
/// @tparam CT - ConservativeRastFETraits
template <typename CT>
void BinTriangles(
    DRAW_CONTEXT *pDC,
    PA_STATE& pa,
    uint32_t workerId,
    simdvector tri[3],
    uint32_t triMask,
    simdscalari primID,
    simdscalari viewportIdx)
{
    SWR_CONTEXT *pContext = pDC->pContext;

    AR_BEGIN(FEBinTriangles, pDC->drawId);

    const API_STATE& state = GetApiState(pDC);
    const SWR_RASTSTATE& rastState = state.rastState;
    const SWR_FRONTEND_STATE& feState = state.frontendState;
    const SWR_GS_STATE& gsState = state.gsState;
    MacroTileMgr *pTileMgr = pDC->pTileMgr;


    simdscalar vRecipW0 = _simd_set1_ps(1.0f);
    simdscalar vRecipW1 = _simd_set1_ps(1.0f);
    simdscalar vRecipW2 = _simd_set1_ps(1.0f);

    if (feState.vpTransformDisable)
    {
        // RHW is passed in directly when VP transform is disabled
        vRecipW0 = tri[0].v[3];
        vRecipW1 = tri[1].v[3];
        vRecipW2 = tri[2].v[3];
    }
    else
    {
        // Perspective divide
        vRecipW0 = _simd_div_ps(_simd_set1_ps(1.0f), tri[0].w);
        vRecipW1 = _simd_div_ps(_simd_set1_ps(1.0f), tri[1].w);
        vRecipW2 = _simd_div_ps(_simd_set1_ps(1.0f), tri[2].w);

        tri[0].v[0] = _simd_mul_ps(tri[0].v[0], vRecipW0);
        tri[1].v[0] = _simd_mul_ps(tri[1].v[0], vRecipW1);
        tri[2].v[0] = _simd_mul_ps(tri[2].v[0], vRecipW2);

        tri[0].v[1] = _simd_mul_ps(tri[0].v[1], vRecipW0);
        tri[1].v[1] = _simd_mul_ps(tri[1].v[1], vRecipW1);
        tri[2].v[1] = _simd_mul_ps(tri[2].v[1], vRecipW2);

        tri[0].v[2] = _simd_mul_ps(tri[0].v[2], vRecipW0);
        tri[1].v[2] = _simd_mul_ps(tri[1].v[2], vRecipW1);
        tri[2].v[2] = _simd_mul_ps(tri[2].v[2], vRecipW2);

        // Viewport transform to screen space coords
        if (state.gsState.emitsViewportArrayIndex)
        {
            viewportTransform<3>(tri, state.vpMatrices, viewportIdx);
        }
        else
        {
            viewportTransform<3>(tri, state.vpMatrices);
        }
    }

    // Adjust for pixel center location
    simdscalar offset = g_pixelOffsets[rastState.pixelLocation];
    tri[0].x = _simd_add_ps(tri[0].x, offset);
    tri[0].y = _simd_add_ps(tri[0].y, offset);

    tri[1].x = _simd_add_ps(tri[1].x, offset);
    tri[1].y = _simd_add_ps(tri[1].y, offset);

    tri[2].x = _simd_add_ps(tri[2].x, offset);
    tri[2].y = _simd_add_ps(tri[2].y, offset);

    simdscalari vXi[3], vYi[3];
    // Set vXi, vYi to required fixed point precision
    FPToFixedPoint(tri, vXi, vYi);

    // triangle setup
    simdscalari vAi[3], vBi[3];
    triangleSetupABIntVertical(vXi, vYi, vAi, vBi);
        
    // determinant
    simdscalari vDet[2];
    calcDeterminantIntVertical(vAi, vBi, vDet);

    // cull zero area
    int maskLo = _simd_movemask_pd(_simd_castsi_pd(_simd_cmpeq_epi64(vDet[0], _simd_setzero_si())));
    int maskHi = _simd_movemask_pd(_simd_castsi_pd(_simd_cmpeq_epi64(vDet[1], _simd_setzero_si())));

    int cullZeroAreaMask = maskLo | (maskHi << (KNOB_SIMD_WIDTH / 2));

    uint32_t origTriMask = triMask;
    // don't cull degenerate triangles if we're conservatively rasterizing
    if(!CT::IsConservativeT::value)
    {
        triMask &= ~cullZeroAreaMask;
    }

    // determine front winding tris
    // CW  +det
    // CCW det <= 0; 0 area triangles are marked as backfacing, which is required behavior for conservative rast
    maskLo = _simd_movemask_pd(_simd_castsi_pd(_simd_cmpgt_epi64(vDet[0], _simd_setzero_si())));
    maskHi = _simd_movemask_pd(_simd_castsi_pd(_simd_cmpgt_epi64(vDet[1], _simd_setzero_si())));
    int cwTriMask = maskLo | (maskHi << (KNOB_SIMD_WIDTH /2) );

    uint32_t frontWindingTris;
    if (rastState.frontWinding == SWR_FRONTWINDING_CW)
    {
        frontWindingTris = cwTriMask;
    }
    else
    {
        frontWindingTris = ~cwTriMask;
    }

    // cull
    uint32_t cullTris;
    switch ((SWR_CULLMODE)rastState.cullMode)
    {
    case SWR_CULLMODE_BOTH:  cullTris = 0xffffffff; break;
    case SWR_CULLMODE_NONE:  cullTris = 0x0; break;
    case SWR_CULLMODE_FRONT: cullTris = frontWindingTris; break;
    // 0 area triangles are marked as backfacing, which is required behavior for conservative rast
    case SWR_CULLMODE_BACK:  cullTris = ~frontWindingTris; break;
    default: SWR_ASSERT(false, "Invalid cull mode: %d", rastState.cullMode); cullTris = 0x0; break;
    }

    triMask &= ~cullTris;

    if (origTriMask ^ triMask)
    {
        RDTSC_EVENT(FECullZeroAreaAndBackface, _mm_popcnt_u32(origTriMask ^ triMask), 0);
    }

    /// Note: these variable initializations must stay above any 'goto endBenTriangles'
    // compute per tri backface
    uint32_t frontFaceMask = frontWindingTris;
    uint32_t *pPrimID = (uint32_t *)&primID;
    const uint32_t *pViewportIndex = (uint32_t *)&viewportIdx;
    DWORD triIndex = 0;
    // for center sample pattern, all samples are at pixel center; calculate coverage
    // once at center and broadcast the results in the backend
    const SWR_MULTISAMPLE_COUNT sampleCount = (rastState.samplePattern == SWR_MSAA_STANDARD_PATTERN) ? rastState.sampleCount : SWR_MULTISAMPLE_1X;
    uint32_t edgeEnable;
    PFN_WORK_FUNC pfnWork;
    if(CT::IsConservativeT::value)
    {
        // determine which edges of the degenerate tri, if any, are valid to rasterize.
        // used to call the appropriate templated rasterizer function
        if(cullZeroAreaMask > 0)
        {
            // e0 = v1-v0
            simdscalari x0x1Mask = _simd_cmpeq_epi32(vXi[0], vXi[1]);
            simdscalari y0y1Mask = _simd_cmpeq_epi32(vYi[0], vYi[1]);
            uint32_t e0Mask = _simd_movemask_ps(_simd_castsi_ps(_simd_and_si(x0x1Mask, y0y1Mask)));

            // e1 = v2-v1
            simdscalari x1x2Mask = _simd_cmpeq_epi32(vXi[1], vXi[2]);
            simdscalari y1y2Mask = _simd_cmpeq_epi32(vYi[1], vYi[2]);
            uint32_t e1Mask = _simd_movemask_ps(_simd_castsi_ps(_simd_and_si(x1x2Mask, y1y2Mask)));

            // e2 = v0-v2
            // if v0 == v1 & v1 == v2, v0 == v2
            uint32_t e2Mask = e0Mask & e1Mask;
            SWR_ASSERT(KNOB_SIMD_WIDTH == 8, "Need to update degenerate mask code for avx512");

            // edge order: e0 = v0v1, e1 = v1v2, e2 = v0v2
            // 32 bit binary: 0000 0000 0010 0100 1001 0010 0100 1001
            e0Mask = pdep_u32(e0Mask, 0x00249249);
            // 32 bit binary: 0000 0000 0100 1001 0010 0100 1001 0010
            e1Mask = pdep_u32(e1Mask, 0x00492492);
            // 32 bit binary: 0000 0000 1001 0010 0100 1001 0010 0100
            e2Mask = pdep_u32(e2Mask, 0x00924924);

            edgeEnable = (0x00FFFFFF & (~(e0Mask | e1Mask | e2Mask)));
        }
        else
        {
            edgeEnable = 0x00FFFFFF;
        }
    }
    else
    {
        // degenerate triangles won't be sent to rasterizer; just enable all edges
        pfnWork = GetRasterizerFunc(sampleCount, (rastState.conservativeRast > 0),
                                    (SWR_INPUT_COVERAGE)pDC->pState->state.psState.inputCoverage, ALL_EDGES_VALID, 
                                    (state.scissorsTileAligned == false));
    }

    if (!triMask)
    {
        goto endBinTriangles;
    }

    // Calc bounding box of triangles
    simdBBox bbox;
    calcBoundingBoxIntVertical<CT>(tri, vXi, vYi, bbox);

    // determine if triangle falls between pixel centers and discard
    // only discard for non-MSAA case and when conservative rast is disabled
    // (xmin + 127) & ~255
    // (xmax + 128) & ~255
    if(rastState.sampleCount == SWR_MULTISAMPLE_1X && (!CT::IsConservativeT::value))
    {
        origTriMask = triMask;

        int cullCenterMask;
        {
            simdscalari xmin = _simd_add_epi32(bbox.xmin, _simd_set1_epi32(127));
            xmin = _simd_and_si(xmin, _simd_set1_epi32(~255));
            simdscalari xmax = _simd_add_epi32(bbox.xmax, _simd_set1_epi32(128));
            xmax = _simd_and_si(xmax, _simd_set1_epi32(~255));

            simdscalari vMaskH = _simd_cmpeq_epi32(xmin, xmax);

            simdscalari ymin = _simd_add_epi32(bbox.ymin, _simd_set1_epi32(127));
            ymin = _simd_and_si(ymin, _simd_set1_epi32(~255));
            simdscalari ymax = _simd_add_epi32(bbox.ymax, _simd_set1_epi32(128));
            ymax = _simd_and_si(ymax, _simd_set1_epi32(~255));

            simdscalari vMaskV = _simd_cmpeq_epi32(ymin, ymax);
            vMaskV = _simd_or_si(vMaskH, vMaskV);
            cullCenterMask = _simd_movemask_ps(_simd_castsi_ps(vMaskV));
        }

        triMask &= ~cullCenterMask;

        if(origTriMask ^ triMask)
        {
            RDTSC_EVENT(FECullBetweenCenters, _mm_popcnt_u32(origTriMask ^ triMask), 0);
        }
    }

    // Intersect with scissor/viewport. Subtract 1 ULP in x.8 fixed point since xmax/ymax edge is exclusive.
    // Gather the AOS effective scissor rects based on the per-prim VP index.
    /// @todo:  Look at speeding this up -- weigh against corresponding costs in rasterizer.
    simdscalari scisXmin, scisYmin, scisXmax, scisYmax;
    if (state.gsState.emitsViewportArrayIndex)
    {
        GatherScissors<KNOB_SIMD_WIDTH>::Gather(&state.scissorsInFixedPoint[0], pViewportIndex,
                                                scisXmin, scisYmin, scisXmax, scisYmax);
    }
    else // broadcast fast path for non-VPAI case.
    {
        scisXmin = _simd_set1_epi32(state.scissorsInFixedPoint[0].xmin);
        scisYmin = _simd_set1_epi32(state.scissorsInFixedPoint[0].ymin);
        scisXmax = _simd_set1_epi32(state.scissorsInFixedPoint[0].xmax);
        scisYmax = _simd_set1_epi32(state.scissorsInFixedPoint[0].ymax);
    }

    bbox.xmin = _simd_max_epi32(bbox.xmin, scisXmin);
    bbox.ymin = _simd_max_epi32(bbox.ymin, scisYmin);
    bbox.xmax = _simd_min_epi32(_simd_sub_epi32(bbox.xmax, _simd_set1_epi32(1)), scisXmax);
    bbox.ymax = _simd_min_epi32(_simd_sub_epi32(bbox.ymax, _simd_set1_epi32(1)), scisYmax);

    if(CT::IsConservativeT::value)
    {
        // in the case where a degenerate triangle is on a scissor edge, we need to make sure the primitive bbox has
        // some area. Bump the xmax/ymax edges out 
        simdscalari topEqualsBottom = _simd_cmpeq_epi32(bbox.ymin, bbox.ymax);
        bbox.ymax = _simd_blendv_epi32(bbox.ymax, _simd_add_epi32(bbox.ymax, _simd_set1_epi32(1)), topEqualsBottom);
        simdscalari leftEqualsRight = _simd_cmpeq_epi32(bbox.xmin, bbox.xmax);
        bbox.xmax = _simd_blendv_epi32(bbox.xmax, _simd_add_epi32(bbox.xmax, _simd_set1_epi32(1)), leftEqualsRight);
    }

    // Cull tris completely outside scissor
    {
        simdscalari maskOutsideScissorX = _simd_cmpgt_epi32(bbox.xmin, bbox.xmax);
        simdscalari maskOutsideScissorY = _simd_cmpgt_epi32(bbox.ymin, bbox.ymax);
        simdscalari maskOutsideScissorXY = _simd_or_si(maskOutsideScissorX, maskOutsideScissorY);
        uint32_t maskOutsideScissor = _simd_movemask_ps(_simd_castsi_ps(maskOutsideScissorXY));
        triMask = triMask & ~maskOutsideScissor;
    }

    if (!triMask)
    {
        goto endBinTriangles;
    }

    // Convert triangle bbox to macrotile units.
    bbox.xmin = _simd_srai_epi32(bbox.xmin, KNOB_MACROTILE_X_DIM_FIXED_SHIFT);
    bbox.ymin = _simd_srai_epi32(bbox.ymin, KNOB_MACROTILE_Y_DIM_FIXED_SHIFT);
    bbox.xmax = _simd_srai_epi32(bbox.xmax, KNOB_MACROTILE_X_DIM_FIXED_SHIFT);
    bbox.ymax = _simd_srai_epi32(bbox.ymax, KNOB_MACROTILE_Y_DIM_FIXED_SHIFT);

    OSALIGNSIMD(uint32_t) aMTLeft[KNOB_SIMD_WIDTH], aMTRight[KNOB_SIMD_WIDTH], aMTTop[KNOB_SIMD_WIDTH], aMTBottom[KNOB_SIMD_WIDTH];
    _simd_store_si((simdscalari*)aMTLeft, bbox.xmin);
    _simd_store_si((simdscalari*)aMTRight, bbox.xmax);
    _simd_store_si((simdscalari*)aMTTop, bbox.ymin);
    _simd_store_si((simdscalari*)aMTBottom, bbox.ymax);

    // transpose verts needed for backend
    /// @todo modify BE to take non-transformed verts
    __m128 vHorizX[8], vHorizY[8], vHorizZ[8], vHorizW[8];
    vTranspose3x8(vHorizX, tri[0].x, tri[1].x, tri[2].x);
    vTranspose3x8(vHorizY, tri[0].y, tri[1].y, tri[2].y);
    vTranspose3x8(vHorizZ, tri[0].z, tri[1].z, tri[2].z);
    vTranspose3x8(vHorizW, vRecipW0, vRecipW1, vRecipW2);

    // store render target array index
    OSALIGNSIMD(uint32_t) aRTAI[KNOB_SIMD_WIDTH];
    if (gsState.gsEnable && gsState.emitsRenderTargetArrayIndex)
    {
        simdvector vRtai[3];
        pa.Assemble(VERTEX_RTAI_SLOT, vRtai);
        simdscalari vRtaii;
        vRtaii = _simd_castps_si(vRtai[0].x);
        _simd_store_si((simdscalari*)aRTAI, vRtaii);
    }
    else
    {
        _simd_store_si((simdscalari*)aRTAI, _simd_setzero_si());
    }

    // scan remaining valid triangles and bin each separately
    while (_BitScanForward(&triIndex, triMask))
    {
        uint32_t linkageCount = state.backendState.numAttributes;
        uint32_t numScalarAttribs = linkageCount * 4;

        BE_WORK work;
        work.type = DRAW;
        
        bool isDegenerate;
        if(CT::IsConservativeT::value)
        {
            // only rasterize valid edges if we have a degenerate primitive
            int32_t triEdgeEnable = (edgeEnable >> (triIndex * 3)) & ALL_EDGES_VALID;
            work.pfnWork = GetRasterizerFunc(sampleCount, (rastState.conservativeRast > 0),
                                        (SWR_INPUT_COVERAGE)pDC->pState->state.psState.inputCoverage, triEdgeEnable,
                                        (state.scissorsTileAligned == false));

            // Degenerate triangles are required to be constant interpolated
            isDegenerate = (triEdgeEnable != ALL_EDGES_VALID) ? true : false;
        }
        else
        {
            isDegenerate = false;
            work.pfnWork = pfnWork;
        }

        // Select attribute processor
        PFN_PROCESS_ATTRIBUTES pfnProcessAttribs = GetProcessAttributesFunc(3,
            state.backendState.swizzleEnable,  state.backendState.constantInterpolationMask, isDegenerate);

        TRIANGLE_WORK_DESC &desc = work.desc.tri;

        desc.triFlags.frontFacing = state.forceFront ? 1 : ((frontFaceMask >> triIndex) & 1);
        desc.triFlags.primID = pPrimID[triIndex];
        desc.triFlags.renderTargetArrayIndex = aRTAI[triIndex];
        desc.triFlags.viewportIndex = pViewportIndex[triIndex];

        auto pArena = pDC->pArena;
        SWR_ASSERT(pArena != nullptr);

        // store active attribs
        float *pAttribs = (float*)pArena->AllocAligned(numScalarAttribs * 3 * sizeof(float), 16);
        desc.pAttribs = pAttribs;
        desc.numAttribs = linkageCount;
        pfnProcessAttribs(pDC, pa, triIndex, pPrimID[triIndex], desc.pAttribs);

        // store triangle vertex data
        desc.pTriBuffer = (float*)pArena->AllocAligned(4 * 4 * sizeof(float), 16);

        _mm_store_ps(&desc.pTriBuffer[0], vHorizX[triIndex]);
        _mm_store_ps(&desc.pTriBuffer[4], vHorizY[triIndex]);
        _mm_store_ps(&desc.pTriBuffer[8], vHorizZ[triIndex]);
        _mm_store_ps(&desc.pTriBuffer[12], vHorizW[triIndex]);

        // store user clip distances
        if (rastState.clipDistanceMask)
        {
            uint32_t numClipDist = _mm_popcnt_u32(rastState.clipDistanceMask);
            desc.pUserClipBuffer = (float*)pArena->Alloc(numClipDist * 3 * sizeof(float));
            ProcessUserClipDist<3>(pa, triIndex, rastState.clipDistanceMask, desc.pUserClipBuffer);
        }

        for (uint32_t y = aMTTop[triIndex]; y <= aMTBottom[triIndex]; ++y)
        {
            for (uint32_t x = aMTLeft[triIndex]; x <= aMTRight[triIndex]; ++x)
            {
#if KNOB_ENABLE_TOSS_POINTS
                if (!KNOB_TOSS_SETUP_TRIS)
#endif
                {
                    pTileMgr->enqueue(x, y, &work);
                }
            }
        }
        triMask &= ~(1 << triIndex);
    }

endBinTriangles:
    AR_END(FEBinTriangles, 1);
}

struct FEBinTrianglesChooser
{
    typedef PFN_PROCESS_PRIMS FuncType;

    template <typename... ArgsB>
    static FuncType GetFunc()
    {
        return BinTriangles<ConservativeRastFETraits<ArgsB...>>;
    }
};

// Selector for correct templated BinTrinagles function
PFN_PROCESS_PRIMS GetBinTrianglesFunc(bool IsConservative)
{
    return TemplateArgUnroller<FEBinTrianglesChooser>::GetFunc(IsConservative);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Bin SIMD points to the backend.  Only supports point size of 1
/// @param pDC - pointer to draw context.
/// @param pa - The primitive assembly object.
/// @param workerId - thread's worker id. Even thread has a unique id.
/// @param tri - Contains point position data for SIMDs worth of points.
/// @param primID - Primitive ID for each point.
void BinPoints(
    DRAW_CONTEXT *pDC,
    PA_STATE& pa,
    uint32_t workerId,
    simdvector prim[3],
    uint32_t primMask,
    simdscalari primID,
    simdscalari viewportIdx)
{
    SWR_CONTEXT *pContext = pDC->pContext;

    AR_BEGIN(FEBinPoints, pDC->drawId);

    simdvector& primVerts = prim[0];

    const API_STATE& state = GetApiState(pDC);
    const SWR_FRONTEND_STATE& feState = state.frontendState;
    const SWR_GS_STATE& gsState = state.gsState;
    const SWR_RASTSTATE& rastState = state.rastState;
    const uint32_t *pViewportIndex = (uint32_t *)&viewportIdx;

    // Select attribute processor
    PFN_PROCESS_ATTRIBUTES pfnProcessAttribs = GetProcessAttributesFunc(1,
        state.backendState.swizzleEnable, state.backendState.constantInterpolationMask);

    if (!feState.vpTransformDisable)
    {
        // perspective divide
        simdscalar vRecipW0 = _simd_div_ps(_simd_set1_ps(1.0f), primVerts.w);
        primVerts.x = _simd_mul_ps(primVerts.x, vRecipW0);
        primVerts.y = _simd_mul_ps(primVerts.y, vRecipW0);
        primVerts.z = _simd_mul_ps(primVerts.z, vRecipW0);

        // viewport transform to screen coords
        if (state.gsState.emitsViewportArrayIndex)
        {
            viewportTransform<1>(&primVerts, state.vpMatrices, viewportIdx);
        }
        else
        {
            viewportTransform<1>(&primVerts, state.vpMatrices);
        }
    }

    // adjust for pixel center location
    simdscalar offset = g_pixelOffsets[rastState.pixelLocation];
    primVerts.x = _simd_add_ps(primVerts.x, offset);
    primVerts.y = _simd_add_ps(primVerts.y, offset);

    // convert to fixed point
    simdscalari vXi, vYi;
    vXi = fpToFixedPointVertical(primVerts.x);
    vYi = fpToFixedPointVertical(primVerts.y);

    if (CanUseSimplePoints(pDC))
    {
        // adjust for ymin-xmin rule
        vXi = _simd_sub_epi32(vXi, _simd_set1_epi32(1));
        vYi = _simd_sub_epi32(vYi, _simd_set1_epi32(1));

        // cull points off the ymin-xmin edge of the viewport
        primMask &= ~_simd_movemask_ps(_simd_castsi_ps(vXi));
        primMask &= ~_simd_movemask_ps(_simd_castsi_ps(vYi));

        // compute macro tile coordinates 
        simdscalari macroX = _simd_srai_epi32(vXi, KNOB_MACROTILE_X_DIM_FIXED_SHIFT);
        simdscalari macroY = _simd_srai_epi32(vYi, KNOB_MACROTILE_Y_DIM_FIXED_SHIFT);

        OSALIGNSIMD(uint32_t) aMacroX[KNOB_SIMD_WIDTH], aMacroY[KNOB_SIMD_WIDTH];
        _simd_store_si((simdscalari*)aMacroX, macroX);
        _simd_store_si((simdscalari*)aMacroY, macroY);

        // compute raster tile coordinates
        simdscalari rasterX = _simd_srai_epi32(vXi, KNOB_TILE_X_DIM_SHIFT + FIXED_POINT_SHIFT);
        simdscalari rasterY = _simd_srai_epi32(vYi, KNOB_TILE_Y_DIM_SHIFT + FIXED_POINT_SHIFT);

        // compute raster tile relative x,y for coverage mask
        simdscalari tileAlignedX = _simd_slli_epi32(rasterX, KNOB_TILE_X_DIM_SHIFT);
        simdscalari tileAlignedY = _simd_slli_epi32(rasterY, KNOB_TILE_Y_DIM_SHIFT);

        simdscalari tileRelativeX = _simd_sub_epi32(_simd_srai_epi32(vXi, FIXED_POINT_SHIFT), tileAlignedX);
        simdscalari tileRelativeY = _simd_sub_epi32(_simd_srai_epi32(vYi, FIXED_POINT_SHIFT), tileAlignedY);

        OSALIGNSIMD(uint32_t) aTileRelativeX[KNOB_SIMD_WIDTH];
        OSALIGNSIMD(uint32_t) aTileRelativeY[KNOB_SIMD_WIDTH];
        _simd_store_si((simdscalari*)aTileRelativeX, tileRelativeX);
        _simd_store_si((simdscalari*)aTileRelativeY, tileRelativeY);

        OSALIGNSIMD(uint32_t) aTileAlignedX[KNOB_SIMD_WIDTH];
        OSALIGNSIMD(uint32_t) aTileAlignedY[KNOB_SIMD_WIDTH];
        _simd_store_si((simdscalari*)aTileAlignedX, tileAlignedX);
        _simd_store_si((simdscalari*)aTileAlignedY, tileAlignedY);

        OSALIGNSIMD(float) aZ[KNOB_SIMD_WIDTH];
        _simd_store_ps((float*)aZ, primVerts.z);

        // store render target array index
        OSALIGNSIMD(uint32_t) aRTAI[KNOB_SIMD_WIDTH];
        if (gsState.gsEnable && gsState.emitsRenderTargetArrayIndex)
        {
            simdvector vRtai;
            pa.Assemble(VERTEX_RTAI_SLOT, &vRtai);
            simdscalari vRtaii = _simd_castps_si(vRtai.x);
            _simd_store_si((simdscalari*)aRTAI, vRtaii);
        }
        else
        {
            _simd_store_si((simdscalari*)aRTAI, _simd_setzero_si());
        }

        uint32_t *pPrimID = (uint32_t *)&primID;
        DWORD primIndex = 0;

        const SWR_BACKEND_STATE& backendState = pDC->pState->state.backendState;

        // scan remaining valid triangles and bin each separately
        while (_BitScanForward(&primIndex, primMask))
        {
            uint32_t linkageCount = backendState.numAttributes;
            uint32_t numScalarAttribs = linkageCount * 4;

            BE_WORK work;
            work.type = DRAW;

            TRIANGLE_WORK_DESC &desc = work.desc.tri;

            // points are always front facing
            desc.triFlags.frontFacing = 1;
            desc.triFlags.primID = pPrimID[primIndex];
            desc.triFlags.renderTargetArrayIndex = aRTAI[primIndex];
            desc.triFlags.viewportIndex = pViewportIndex[primIndex];

            work.pfnWork = RasterizeSimplePoint;

            auto pArena = pDC->pArena;
            SWR_ASSERT(pArena != nullptr);

            // store attributes
            float *pAttribs = (float*)pArena->AllocAligned(3 * numScalarAttribs * sizeof(float), 16);
            desc.pAttribs = pAttribs;
            desc.numAttribs = linkageCount;

            pfnProcessAttribs(pDC, pa, primIndex, pPrimID[primIndex], pAttribs);

            // store raster tile aligned x, y, perspective correct z
            float *pTriBuffer = (float*)pArena->AllocAligned(4 * sizeof(float), 16);
            desc.pTriBuffer = pTriBuffer;
            *(uint32_t*)pTriBuffer++ = aTileAlignedX[primIndex];
            *(uint32_t*)pTriBuffer++ = aTileAlignedY[primIndex];
            *pTriBuffer = aZ[primIndex];

            uint32_t tX = aTileRelativeX[primIndex];
            uint32_t tY = aTileRelativeY[primIndex];

            // pack the relative x,y into the coverageMask, the rasterizer will
            // generate the true coverage mask from it
            work.desc.tri.triFlags.coverageMask = tX | (tY << 4);

            // bin it
            MacroTileMgr *pTileMgr = pDC->pTileMgr;
#if KNOB_ENABLE_TOSS_POINTS
            if (!KNOB_TOSS_SETUP_TRIS)
#endif
            {
                pTileMgr->enqueue(aMacroX[primIndex], aMacroY[primIndex], &work);
            }
            primMask &= ~(1 << primIndex);
        }
    }
    else
    {
        // non simple points need to be potentially binned to multiple macro tiles
        simdscalar vPointSize;
        if (rastState.pointParam)
        {
            simdvector size[3];
            pa.Assemble(VERTEX_POINT_SIZE_SLOT, size);
            vPointSize = size[0].x;
        }
        else
        {
            vPointSize = _simd_set1_ps(rastState.pointSize);
        }

        // bloat point to bbox
        simdBBox bbox;
        bbox.xmin = bbox.xmax = vXi;
        bbox.ymin = bbox.ymax = vYi;

        simdscalar vHalfWidth = _simd_mul_ps(vPointSize, _simd_set1_ps(0.5f));
        simdscalari vHalfWidthi = fpToFixedPointVertical(vHalfWidth);
        bbox.xmin = _simd_sub_epi32(bbox.xmin, vHalfWidthi);
        bbox.xmax = _simd_add_epi32(bbox.xmax, vHalfWidthi);
        bbox.ymin = _simd_sub_epi32(bbox.ymin, vHalfWidthi);
        bbox.ymax = _simd_add_epi32(bbox.ymax, vHalfWidthi);

        // Intersect with scissor/viewport. Subtract 1 ULP in x.8 fixed point since xmax/ymax edge is exclusive.
        // Gather the AOS effective scissor rects based on the per-prim VP index.
        /// @todo:  Look at speeding this up -- weigh against corresponding costs in rasterizer.
        simdscalari scisXmin, scisYmin, scisXmax, scisYmax;
        if (state.gsState.emitsViewportArrayIndex)
        {
            GatherScissors<KNOB_SIMD_WIDTH>::Gather(&state.scissorsInFixedPoint[0], pViewportIndex,
                scisXmin, scisYmin, scisXmax, scisYmax);
        }
        else // broadcast fast path for non-VPAI case.
        {
            scisXmin = _simd_set1_epi32(state.scissorsInFixedPoint[0].xmin);
            scisYmin = _simd_set1_epi32(state.scissorsInFixedPoint[0].ymin);
            scisXmax = _simd_set1_epi32(state.scissorsInFixedPoint[0].xmax);
            scisYmax = _simd_set1_epi32(state.scissorsInFixedPoint[0].ymax);
        }

        bbox.xmin = _simd_max_epi32(bbox.xmin, scisXmin);
        bbox.ymin = _simd_max_epi32(bbox.ymin, scisYmin);
        bbox.xmax = _simd_min_epi32(_simd_sub_epi32(bbox.xmax, _simd_set1_epi32(1)), scisXmax);
        bbox.ymax = _simd_min_epi32(_simd_sub_epi32(bbox.ymax, _simd_set1_epi32(1)), scisYmax);

        // Cull bloated points completely outside scissor
        simdscalari maskOutsideScissorX = _simd_cmpgt_epi32(bbox.xmin, bbox.xmax);
        simdscalari maskOutsideScissorY = _simd_cmpgt_epi32(bbox.ymin, bbox.ymax);
        simdscalari maskOutsideScissorXY = _simd_or_si(maskOutsideScissorX, maskOutsideScissorY);
        uint32_t maskOutsideScissor = _simd_movemask_ps(_simd_castsi_ps(maskOutsideScissorXY));
        primMask = primMask & ~maskOutsideScissor;

        // Convert bbox to macrotile units.
        bbox.xmin = _simd_srai_epi32(bbox.xmin, KNOB_MACROTILE_X_DIM_FIXED_SHIFT);
        bbox.ymin = _simd_srai_epi32(bbox.ymin, KNOB_MACROTILE_Y_DIM_FIXED_SHIFT);
        bbox.xmax = _simd_srai_epi32(bbox.xmax, KNOB_MACROTILE_X_DIM_FIXED_SHIFT);
        bbox.ymax = _simd_srai_epi32(bbox.ymax, KNOB_MACROTILE_Y_DIM_FIXED_SHIFT);

        OSALIGNSIMD(uint32_t) aMTLeft[KNOB_SIMD_WIDTH], aMTRight[KNOB_SIMD_WIDTH], aMTTop[KNOB_SIMD_WIDTH], aMTBottom[KNOB_SIMD_WIDTH];
        _simd_store_si((simdscalari*)aMTLeft, bbox.xmin);
        _simd_store_si((simdscalari*)aMTRight, bbox.xmax);
        _simd_store_si((simdscalari*)aMTTop, bbox.ymin);
        _simd_store_si((simdscalari*)aMTBottom, bbox.ymax);

        // store render target array index
        OSALIGNSIMD(uint32_t) aRTAI[KNOB_SIMD_WIDTH];
        if (gsState.gsEnable && gsState.emitsRenderTargetArrayIndex)
        {
            simdvector vRtai[2];
            pa.Assemble(VERTEX_RTAI_SLOT, vRtai);
            simdscalari vRtaii = _simd_castps_si(vRtai[0].x);
            _simd_store_si((simdscalari*)aRTAI, vRtaii);
        }
        else
        {
            _simd_store_si((simdscalari*)aRTAI, _simd_setzero_si());
        }

        OSALIGNSIMD(float) aPointSize[KNOB_SIMD_WIDTH];
        _simd_store_ps((float*)aPointSize, vPointSize);

        uint32_t *pPrimID = (uint32_t *)&primID;

        OSALIGNSIMD(float) aPrimVertsX[KNOB_SIMD_WIDTH];
        OSALIGNSIMD(float) aPrimVertsY[KNOB_SIMD_WIDTH];
        OSALIGNSIMD(float) aPrimVertsZ[KNOB_SIMD_WIDTH];

        _simd_store_ps((float*)aPrimVertsX, primVerts.x);
        _simd_store_ps((float*)aPrimVertsY, primVerts.y);
        _simd_store_ps((float*)aPrimVertsZ, primVerts.z);

        // scan remaining valid prims and bin each separately
        const SWR_BACKEND_STATE& backendState = state.backendState;
        DWORD primIndex;
        while (_BitScanForward(&primIndex, primMask))
        {
            uint32_t linkageCount = backendState.numAttributes;
            uint32_t numScalarAttribs = linkageCount * 4;

            BE_WORK work;
            work.type = DRAW;

            TRIANGLE_WORK_DESC &desc = work.desc.tri;

            desc.triFlags.frontFacing = 1;
            desc.triFlags.primID = pPrimID[primIndex];
            desc.triFlags.pointSize = aPointSize[primIndex];
            desc.triFlags.renderTargetArrayIndex = aRTAI[primIndex];
            desc.triFlags.viewportIndex = pViewportIndex[primIndex];

            work.pfnWork = RasterizeTriPoint;

            auto pArena = pDC->pArena;
            SWR_ASSERT(pArena != nullptr);

            // store active attribs
            desc.pAttribs = (float*)pArena->AllocAligned(numScalarAttribs * 3 * sizeof(float), 16);
            desc.numAttribs = linkageCount;
            pfnProcessAttribs(pDC, pa, primIndex, pPrimID[primIndex], desc.pAttribs);

            // store point vertex data
            float *pTriBuffer = (float*)pArena->AllocAligned(4 * sizeof(float), 16);
            desc.pTriBuffer = pTriBuffer;
            *pTriBuffer++ = aPrimVertsX[primIndex];
            *pTriBuffer++ = aPrimVertsY[primIndex];
            *pTriBuffer = aPrimVertsZ[primIndex];

            // store user clip distances
            if (rastState.clipDistanceMask)
            {
                uint32_t numClipDist = _mm_popcnt_u32(rastState.clipDistanceMask);
                desc.pUserClipBuffer = (float*)pArena->Alloc(numClipDist * 2 * sizeof(float));
                ProcessUserClipDist<2>(pa, primIndex, rastState.clipDistanceMask, desc.pUserClipBuffer);
            }

            MacroTileMgr *pTileMgr = pDC->pTileMgr;
            for (uint32_t y = aMTTop[primIndex]; y <= aMTBottom[primIndex]; ++y)
            {
                for (uint32_t x = aMTLeft[primIndex]; x <= aMTRight[primIndex]; ++x)
                {
#if KNOB_ENABLE_TOSS_POINTS
                    if (!KNOB_TOSS_SETUP_TRIS)
#endif
                    {
                        pTileMgr->enqueue(x, y, &work);
                    }
                }
            }

            primMask &= ~(1 << primIndex);
        }
    }

    AR_END(FEBinPoints, 1);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Bin SIMD lines to the backend.
/// @param pDC - pointer to draw context.
/// @param pa - The primitive assembly object.
/// @param workerId - thread's worker id. Even thread has a unique id.
/// @param tri - Contains line position data for SIMDs worth of points.
/// @param primID - Primitive ID for each line.
/// @param viewportIdx - Viewport Array Index for each line.
void BinLines(
    DRAW_CONTEXT *pDC,
    PA_STATE& pa,
    uint32_t workerId,
    simdvector prim[],
    uint32_t primMask,
    simdscalari primID,
    simdscalari viewportIdx)
{
    SWR_CONTEXT *pContext = pDC->pContext;

    AR_BEGIN(FEBinLines, pDC->drawId);

    const API_STATE& state = GetApiState(pDC);
    const SWR_RASTSTATE& rastState = state.rastState;
    const SWR_FRONTEND_STATE& feState = state.frontendState;
    const SWR_GS_STATE& gsState = state.gsState;

    // Select attribute processor
    PFN_PROCESS_ATTRIBUTES pfnProcessAttribs = GetProcessAttributesFunc(2,
    state.backendState.swizzleEnable, state.backendState.constantInterpolationMask);

    simdscalar vRecipW0 = _simd_set1_ps(1.0f);
    simdscalar vRecipW1 = _simd_set1_ps(1.0f);

    if (!feState.vpTransformDisable)
    {
        // perspective divide
        vRecipW0 = _simd_div_ps(_simd_set1_ps(1.0f), prim[0].w);
        vRecipW1 = _simd_div_ps(_simd_set1_ps(1.0f), prim[1].w);

        prim[0].v[0] = _simd_mul_ps(prim[0].v[0], vRecipW0);
        prim[1].v[0] = _simd_mul_ps(prim[1].v[0], vRecipW1);

        prim[0].v[1] = _simd_mul_ps(prim[0].v[1], vRecipW0);
        prim[1].v[1] = _simd_mul_ps(prim[1].v[1], vRecipW1);

        prim[0].v[2] = _simd_mul_ps(prim[0].v[2], vRecipW0);
        prim[1].v[2] = _simd_mul_ps(prim[1].v[2], vRecipW1);

        // viewport transform to screen coords
        if (state.gsState.emitsViewportArrayIndex)
        {
            viewportTransform<2>(prim, state.vpMatrices, viewportIdx);
        }
        else
        {
            viewportTransform<2>(prim, state.vpMatrices);
        }
    }

    // adjust for pixel center location
    simdscalar offset = g_pixelOffsets[rastState.pixelLocation];
    prim[0].x = _simd_add_ps(prim[0].x, offset);
    prim[0].y = _simd_add_ps(prim[0].y, offset);

    prim[1].x = _simd_add_ps(prim[1].x, offset);
    prim[1].y = _simd_add_ps(prim[1].y, offset);

    // convert to fixed point
    simdscalari vXi[2], vYi[2];
    vXi[0] = fpToFixedPointVertical(prim[0].x);
    vYi[0] = fpToFixedPointVertical(prim[0].y);
    vXi[1] = fpToFixedPointVertical(prim[1].x);
    vYi[1] = fpToFixedPointVertical(prim[1].y);

    // compute x-major vs y-major mask
    simdscalari xLength = _simd_abs_epi32(_simd_sub_epi32(vXi[0], vXi[1]));
    simdscalari yLength = _simd_abs_epi32(_simd_sub_epi32(vYi[0], vYi[1]));
    simdscalar vYmajorMask = _simd_castsi_ps(_simd_cmpgt_epi32(yLength, xLength));
    uint32_t yMajorMask = _simd_movemask_ps(vYmajorMask);

    // cull zero-length lines
    simdscalari vZeroLengthMask = _simd_cmpeq_epi32(xLength, _simd_setzero_si());
    vZeroLengthMask = _simd_and_si(vZeroLengthMask, _simd_cmpeq_epi32(yLength, _simd_setzero_si()));

    primMask &= ~_simd_movemask_ps(_simd_castsi_ps(vZeroLengthMask));

    uint32_t *pPrimID = (uint32_t *)&primID;
    const uint32_t *pViewportIndex = (uint32_t *)&viewportIdx;

    simdscalar vUnused = _simd_setzero_ps();

    // Calc bounding box of lines
    simdBBox bbox;
    bbox.xmin = _simd_min_epi32(vXi[0], vXi[1]);
    bbox.xmax = _simd_max_epi32(vXi[0], vXi[1]);
    bbox.ymin = _simd_min_epi32(vYi[0], vYi[1]);
    bbox.ymax = _simd_max_epi32(vYi[0], vYi[1]);

    // bloat bbox by line width along minor axis
    simdscalar vHalfWidth = _simd_set1_ps(rastState.lineWidth / 2.0f);
    simdscalari vHalfWidthi = fpToFixedPointVertical(vHalfWidth);
    simdBBox bloatBox;
    bloatBox.xmin = _simd_sub_epi32(bbox.xmin, vHalfWidthi);
    bloatBox.xmax = _simd_add_epi32(bbox.xmax, vHalfWidthi);
    bloatBox.ymin = _simd_sub_epi32(bbox.ymin, vHalfWidthi);
    bloatBox.ymax = _simd_add_epi32(bbox.ymax, vHalfWidthi);

    bbox.xmin = _simd_blendv_epi32(bbox.xmin, bloatBox.xmin, vYmajorMask);
    bbox.xmax = _simd_blendv_epi32(bbox.xmax, bloatBox.xmax, vYmajorMask);
    bbox.ymin = _simd_blendv_epi32(bloatBox.ymin, bbox.ymin, vYmajorMask);
    bbox.ymax = _simd_blendv_epi32(bloatBox.ymax, bbox.ymax, vYmajorMask);

    // Intersect with scissor/viewport. Subtract 1 ULP in x.8 fixed point since xmax/ymax edge is exclusive.
    simdscalari scisXmin, scisYmin, scisXmax, scisYmax;
    if (state.gsState.emitsViewportArrayIndex)
    {
        GatherScissors<KNOB_SIMD_WIDTH>::Gather(&state.scissorsInFixedPoint[0], pViewportIndex,
            scisXmin, scisYmin, scisXmax, scisYmax);
    }
    else // broadcast fast path for non-VPAI case.
    {
        scisXmin = _simd_set1_epi32(state.scissorsInFixedPoint[0].xmin);
        scisYmin = _simd_set1_epi32(state.scissorsInFixedPoint[0].ymin);
        scisXmax = _simd_set1_epi32(state.scissorsInFixedPoint[0].xmax);
        scisYmax = _simd_set1_epi32(state.scissorsInFixedPoint[0].ymax);
    }

    bbox.xmin = _simd_max_epi32(bbox.xmin, scisXmin);
    bbox.ymin = _simd_max_epi32(bbox.ymin, scisYmin);
    bbox.xmax = _simd_min_epi32(_simd_sub_epi32(bbox.xmax, _simd_set1_epi32(1)), scisXmax);
    bbox.ymax = _simd_min_epi32(_simd_sub_epi32(bbox.ymax, _simd_set1_epi32(1)), scisYmax);

    // Cull prims completely outside scissor
    {
        simdscalari maskOutsideScissorX = _simd_cmpgt_epi32(bbox.xmin, bbox.xmax);
        simdscalari maskOutsideScissorY = _simd_cmpgt_epi32(bbox.ymin, bbox.ymax);
        simdscalari maskOutsideScissorXY = _simd_or_si(maskOutsideScissorX, maskOutsideScissorY);
        uint32_t maskOutsideScissor = _simd_movemask_ps(_simd_castsi_ps(maskOutsideScissorXY));
        primMask = primMask & ~maskOutsideScissor;
    }

    if (!primMask)
    {
        goto endBinLines;
    }

    // Convert triangle bbox to macrotile units.
    bbox.xmin = _simd_srai_epi32(bbox.xmin, KNOB_MACROTILE_X_DIM_FIXED_SHIFT);
    bbox.ymin = _simd_srai_epi32(bbox.ymin, KNOB_MACROTILE_Y_DIM_FIXED_SHIFT);
    bbox.xmax = _simd_srai_epi32(bbox.xmax, KNOB_MACROTILE_X_DIM_FIXED_SHIFT);
    bbox.ymax = _simd_srai_epi32(bbox.ymax, KNOB_MACROTILE_Y_DIM_FIXED_SHIFT);

    OSALIGNSIMD(uint32_t) aMTLeft[KNOB_SIMD_WIDTH], aMTRight[KNOB_SIMD_WIDTH], aMTTop[KNOB_SIMD_WIDTH], aMTBottom[KNOB_SIMD_WIDTH];
    _simd_store_si((simdscalari*)aMTLeft, bbox.xmin);
    _simd_store_si((simdscalari*)aMTRight, bbox.xmax);
    _simd_store_si((simdscalari*)aMTTop, bbox.ymin);
    _simd_store_si((simdscalari*)aMTBottom, bbox.ymax);

    // transpose verts needed for backend
    /// @todo modify BE to take non-transformed verts
    __m128 vHorizX[8], vHorizY[8], vHorizZ[8], vHorizW[8];
    vTranspose3x8(vHorizX, prim[0].x, prim[1].x, vUnused);
    vTranspose3x8(vHorizY, prim[0].y, prim[1].y, vUnused);
    vTranspose3x8(vHorizZ, prim[0].z, prim[1].z, vUnused);
    vTranspose3x8(vHorizW, vRecipW0, vRecipW1, vUnused);

    // store render target array index
    OSALIGNSIMD(uint32_t) aRTAI[KNOB_SIMD_WIDTH];
    if (gsState.gsEnable && gsState.emitsRenderTargetArrayIndex)
    {
        simdvector vRtai[2];
        pa.Assemble(VERTEX_RTAI_SLOT, vRtai);
        simdscalari vRtaii = _simd_castps_si(vRtai[0].x);
        _simd_store_si((simdscalari*)aRTAI, vRtaii);
    }
    else
    {
        _simd_store_si((simdscalari*)aRTAI, _simd_setzero_si());
    }

    // scan remaining valid prims and bin each separately
    DWORD primIndex;
    while (_BitScanForward(&primIndex, primMask))
    {
        uint32_t linkageCount = state.backendState.numAttributes;
        uint32_t numScalarAttribs = linkageCount * 4;

        BE_WORK work;
        work.type = DRAW;

        TRIANGLE_WORK_DESC &desc = work.desc.tri;

        desc.triFlags.frontFacing = 1;
        desc.triFlags.primID = pPrimID[primIndex];
        desc.triFlags.yMajor = (yMajorMask >> primIndex) & 1;
        desc.triFlags.renderTargetArrayIndex = aRTAI[primIndex];
        desc.triFlags.viewportIndex = pViewportIndex[primIndex];

        work.pfnWork = RasterizeLine;

        auto pArena = pDC->pArena;
        SWR_ASSERT(pArena != nullptr);

        // store active attribs
        desc.pAttribs = (float*)pArena->AllocAligned(numScalarAttribs * 3 * sizeof(float), 16);
        desc.numAttribs = linkageCount;
        pfnProcessAttribs(pDC, pa, primIndex, pPrimID[primIndex], desc.pAttribs);

        // store line vertex data
        desc.pTriBuffer = (float*)pArena->AllocAligned(4 * 4 * sizeof(float), 16);
        _mm_store_ps(&desc.pTriBuffer[0], vHorizX[primIndex]);
        _mm_store_ps(&desc.pTriBuffer[4], vHorizY[primIndex]);
        _mm_store_ps(&desc.pTriBuffer[8], vHorizZ[primIndex]);
        _mm_store_ps(&desc.pTriBuffer[12], vHorizW[primIndex]);

        // store user clip distances
        if (rastState.clipDistanceMask)
        {
            uint32_t numClipDist = _mm_popcnt_u32(rastState.clipDistanceMask);
            desc.pUserClipBuffer = (float*)pArena->Alloc(numClipDist * 2 * sizeof(float));
            ProcessUserClipDist<2>(pa, primIndex, rastState.clipDistanceMask, desc.pUserClipBuffer);
        }

        MacroTileMgr *pTileMgr = pDC->pTileMgr;
        for (uint32_t y = aMTTop[primIndex]; y <= aMTBottom[primIndex]; ++y)
        {
            for (uint32_t x = aMTLeft[primIndex]; x <= aMTRight[primIndex]; ++x)
            {
#if KNOB_ENABLE_TOSS_POINTS
                if (!KNOB_TOSS_SETUP_TRIS)
#endif
                {
                    pTileMgr->enqueue(x, y, &work);
                }
            }
        }

        primMask &= ~(1 << primIndex);
    }

endBinLines:

    AR_END(FEBinLines, 1);
}
