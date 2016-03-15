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

#include <stdio.h>
#include <thread>
#include <algorithm>
#include <unordered_set>
#include <float.h>
#include <vector>
#include <utility>
#include <fstream>
#include <string>

#if defined(__linux__) || defined(__gnu_linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#include "common/os.h"
#include "context.h"
#include "frontend.h"
#include "backend.h"
#include "rasterizer.h"
#include "rdtsc_core.h"
#include "tilemgr.h"
#include "core/multisample.h"




// ThreadId
struct Core
{
    uint32_t                procGroup = 0;
    std::vector<uint32_t>   threadIds;
};

struct NumaNode
{
    std::vector<Core> cores;
};

typedef std::vector<NumaNode> CPUNumaNodes;

void CalculateProcessorTopology(CPUNumaNodes& out_nodes, uint32_t& out_numThreadsPerProcGroup)
{
    out_nodes.clear();
    out_numThreadsPerProcGroup = 0;

#if defined(_WIN32)

    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer[KNOB_MAX_NUM_THREADS];
    DWORD bufSize = sizeof(buffer);

    BOOL ret = GetLogicalProcessorInformationEx(RelationProcessorCore, buffer, &bufSize);
    SWR_ASSERT(ret != FALSE, "Failed to get Processor Topology Information");

    uint32_t count = bufSize / buffer->Size;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX pBuffer = buffer;

    for (uint32_t i = 0; i < count; ++i)
    {
        SWR_ASSERT(pBuffer->Relationship == RelationProcessorCore);
        for (uint32_t g = 0; g < pBuffer->Processor.GroupCount; ++g)
        {
            auto& gmask = pBuffer->Processor.GroupMask[g];
            uint32_t threadId = 0;
            uint32_t procGroup = gmask.Group;

            Core* pCore = nullptr;

            uint32_t numThreads = (uint32_t)_mm_popcount_sizeT(gmask.Mask);

            while (BitScanForwardSizeT((unsigned long*)&threadId, gmask.Mask))
            {
                // clear mask
                gmask.Mask &= ~(KAFFINITY(1) << threadId);

                // Find Numa Node
                PROCESSOR_NUMBER procNum = {};
                procNum.Group = WORD(procGroup);
                procNum.Number = UCHAR(threadId);

                uint32_t numaId = 0;
                ret = GetNumaProcessorNodeEx(&procNum, (PUSHORT)&numaId);
                SWR_ASSERT(ret);

                // Store data
                if (out_nodes.size() <= numaId) out_nodes.resize(numaId + 1);
                auto& numaNode = out_nodes[numaId];

                uint32_t coreId = 0;

                if (nullptr == pCore)
                {
                    numaNode.cores.push_back(Core());
                    pCore = &numaNode.cores.back();
                    pCore->procGroup = procGroup;
#if !defined(_WIN64)
                    coreId = (uint32_t)numaNode.cores.size();
                    if ((coreId * numThreads) >= 32)
                    {
                        // Windows doesn't return threadIds >= 32 for a processor group correctly
                        // when running a 32-bit application.
                        // Just save -1 as the threadId
                        threadId = uint32_t(-1);
                    }
#endif
                }
                pCore->threadIds.push_back(threadId);
                if (procGroup == 0)
                {
                    out_numThreadsPerProcGroup++;
                }
            }
        }
        pBuffer = PtrAdd(pBuffer, pBuffer->Size);
    }


#elif defined(__linux__) || defined (__gnu_linux__)

    // Parse /proc/cpuinfo to get full topology
    std::ifstream input("/proc/cpuinfo");
    std::string line;
    char* c;
    uint32_t threadId = uint32_t(-1);
    uint32_t coreId = uint32_t(-1);
    uint32_t numaId = uint32_t(-1);

    while (std::getline(input, line))
    {
        if (line.find("processor") != std::string::npos)
        {
            if (threadId != uint32_t(-1))
            {
                // Save information.
                if (out_nodes.size() <= numaId) out_nodes.resize(numaId + 1);
                auto& numaNode = out_nodes[numaId];
                if (numaNode.cores.size() <= coreId) numaNode.cores.resize(coreId + 1);
                auto& core = numaNode.cores[coreId];

                core.procGroup = coreId;
                core.threadIds.push_back(threadId);

                out_numThreadsPerProcGroup++;
            }

            auto data_start = line.find(": ") + 2;
            threadId = std::strtoul(&line.c_str()[data_start], &c, 10);
            continue;
        }
        if (line.find("core id") != std::string::npos)
        {
            auto data_start = line.find(": ") + 2;
            coreId = std::strtoul(&line.c_str()[data_start], &c, 10);
            continue;
        }
        if (line.find("physical id") != std::string::npos)
        {
            auto data_start = line.find(": ") + 2;
            numaId = std::strtoul(&line.c_str()[data_start], &c, 10);
            continue;
        }
    }

    if (threadId != uint32_t(-1))
    {
        // Save information.
        if (out_nodes.size() <= numaId) out_nodes.resize(numaId + 1);
        auto& numaNode = out_nodes[numaId];
        if (numaNode.cores.size() <= coreId) numaNode.cores.resize(coreId + 1);
        auto& core = numaNode.cores[coreId];

        core.procGroup = coreId;
        core.threadIds.push_back(threadId);
        out_numThreadsPerProcGroup++;
    }

    for (uint32_t node = 0; node < out_nodes.size(); node++) {
        auto& numaNode = out_nodes[node];
        auto it = numaNode.cores.begin();
        for ( ; it != numaNode.cores.end(); ) {
            if (it->threadIds.size() == 0)
                numaNode.cores.erase(it);
            else
                ++it;
        }
    }

#else

#error Unsupported platform

#endif
}


void bindThread(uint32_t threadId, uint32_t procGroupId = 0, bool bindProcGroup=false)
{
    // Only bind threads when MAX_WORKER_THREADS isn't set.
    if (KNOB_MAX_WORKER_THREADS && bindProcGroup == false)
    {
        return;
    }

#if defined(_WIN32)
    {
        GROUP_AFFINITY affinity = {};
        affinity.Group = procGroupId;

#if !defined(_WIN64)
        if (threadId >= 32)
        {
            // In a 32-bit process on Windows it is impossible to bind
            // to logical processors 32-63 within a processor group.
            // In this case set the mask to 0 and let the system assign
            // the processor.  Hopefully it will make smart choices.
            affinity.Mask = 0;
        }
        else
#endif
        {
            // If KNOB_MAX_WORKER_THREADS is set, only bind to the proc group,
            // Not the individual HW thread.
            if (!KNOB_MAX_WORKER_THREADS)
            {
                affinity.Mask = KAFFINITY(1) << threadId;
            }
        }

        SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr);
    }
#else
    cpu_set_t cpuset;
    pthread_t thread = pthread_self();
    CPU_ZERO(&cpuset);
    CPU_SET(threadId, &cpuset);

    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
#endif
}

INLINE
uint64_t GetEnqueuedDraw(SWR_CONTEXT *pContext)
{
    //uint64_t result = _InterlockedCompareExchange64((volatile __int64*)&pContext->DrawEnqueued, 0, 0);
    //return result;
    return pContext->DrawEnqueued;
}

INLINE
DRAW_CONTEXT *GetDC(SWR_CONTEXT *pContext, uint64_t drawId)
{
    return &pContext->dcRing[(drawId-1) % KNOB_MAX_DRAWS_IN_FLIGHT];
}

// returns true if dependency not met
INLINE
bool CheckDependency(SWR_CONTEXT *pContext, DRAW_CONTEXT *pDC, uint64_t lastRetiredDraw)
{
    return (pDC->dependency > lastRetiredDraw);
}

void ClearColorHotTile(const HOTTILE* pHotTile)  // clear a macro tile from float4 clear data.
{
    // Load clear color into SIMD register...
    float *pClearData = (float*)(pHotTile->clearData);
    simdscalar valR = _simd_broadcast_ss(&pClearData[0]);
    simdscalar valG = _simd_broadcast_ss(&pClearData[1]);
    simdscalar valB = _simd_broadcast_ss(&pClearData[2]);
    simdscalar valA = _simd_broadcast_ss(&pClearData[3]);

    float *pfBuf = (float*)pHotTile->pBuffer;
    uint32_t numSamples = pHotTile->numSamples;

    for (uint32_t row = 0; row < KNOB_MACROTILE_Y_DIM; row += KNOB_TILE_Y_DIM)
    {
        for (uint32_t col = 0; col < KNOB_MACROTILE_X_DIM; col += KNOB_TILE_X_DIM)
        {
            for (uint32_t si = 0; si < (KNOB_TILE_X_DIM * KNOB_TILE_Y_DIM * numSamples); si += SIMD_TILE_X_DIM * SIMD_TILE_Y_DIM) //SIMD_TILE_X_DIM * SIMD_TILE_Y_DIM); si++)
            {
                _simd_store_ps(pfBuf, valR);
                pfBuf += KNOB_SIMD_WIDTH;
                _simd_store_ps(pfBuf, valG);
                pfBuf += KNOB_SIMD_WIDTH;
                _simd_store_ps(pfBuf, valB);
                pfBuf += KNOB_SIMD_WIDTH;
                _simd_store_ps(pfBuf, valA);
                pfBuf += KNOB_SIMD_WIDTH;
            }
        }
    }
}

void ClearDepthHotTile(const HOTTILE* pHotTile)  // clear a macro tile from float4 clear data.
{
    // Load clear color into SIMD register...
    float *pClearData = (float*)(pHotTile->clearData);
    simdscalar valZ = _simd_broadcast_ss(&pClearData[0]);

    float *pfBuf = (float*)pHotTile->pBuffer;
    uint32_t numSamples = pHotTile->numSamples;

    for (uint32_t row = 0; row < KNOB_MACROTILE_Y_DIM; row += KNOB_TILE_Y_DIM)
    {
        for (uint32_t col = 0; col < KNOB_MACROTILE_X_DIM; col += KNOB_TILE_X_DIM)
        {
            for (uint32_t si = 0; si < (KNOB_TILE_X_DIM * KNOB_TILE_Y_DIM * numSamples); si += SIMD_TILE_X_DIM * SIMD_TILE_Y_DIM)
            {
                _simd_store_ps(pfBuf, valZ);
                pfBuf += KNOB_SIMD_WIDTH;
            }
        }
    }
}

void ClearStencilHotTile(const HOTTILE* pHotTile)
{
    // convert from F32 to U8.
    uint8_t clearVal = (uint8_t)(pHotTile->clearData[0]);
    //broadcast 32x into __m256i...
    simdscalari valS = _simd_set1_epi8(clearVal);

    simdscalari* pBuf = (simdscalari*)pHotTile->pBuffer;
    uint32_t numSamples = pHotTile->numSamples;

    for (uint32_t row = 0; row < KNOB_MACROTILE_Y_DIM; row += KNOB_TILE_Y_DIM)
    {
        for (uint32_t col = 0; col < KNOB_MACROTILE_X_DIM; col += KNOB_TILE_X_DIM)
        {
            // We're putting 4 pixels in each of the 32-bit slots, so increment 4 times as quickly.
            for (uint32_t si = 0; si < (KNOB_TILE_X_DIM * KNOB_TILE_Y_DIM * numSamples); si += SIMD_TILE_X_DIM * SIMD_TILE_Y_DIM * 4)
            {
                _simd_store_si(pBuf, valS);
                pBuf += 1;
            }
        }
    }
}

// for draw calls, we initialize the active hot tiles and perform deferred
// load on them if tile is in invalid state. we do this in the outer thread loop instead of inside
// the draw routine itself mainly for performance, to avoid unnecessary setup
// every triangle
// @todo support deferred clear
INLINE
void InitializeHotTiles(SWR_CONTEXT* pContext, DRAW_CONTEXT* pDC, uint32_t macroID, const TRIANGLE_WORK_DESC* pWork)
{
    const API_STATE& state = GetApiState(pDC);
    HotTileMgr *pHotTileMgr = pContext->pHotTileMgr;

    uint32_t x, y;
    MacroTileMgr::getTileIndices(macroID, x, y);
    x *= KNOB_MACROTILE_X_DIM;
    y *= KNOB_MACROTILE_Y_DIM;

    uint32_t numSamples = GetNumSamples(state.rastState.sampleCount);

    // check RT if enabled
    unsigned long rtSlot = 0;
    uint32_t colorHottileEnableMask = state.colorHottileEnable;
    while(_BitScanForward(&rtSlot, colorHottileEnableMask))
    {
        HOTTILE* pHotTile = pHotTileMgr->GetHotTile(pContext, pDC, macroID, (SWR_RENDERTARGET_ATTACHMENT)(SWR_ATTACHMENT_COLOR0 + rtSlot), true, numSamples);

        if (pHotTile->state == HOTTILE_INVALID)
        {
            RDTSC_START(BELoadTiles);
            // invalid hottile before draw requires a load from surface before we can draw to it
            pContext->pfnLoadTile(GetPrivateState(pDC), KNOB_COLOR_HOT_TILE_FORMAT, (SWR_RENDERTARGET_ATTACHMENT)(SWR_ATTACHMENT_COLOR0 + rtSlot), x, y, pHotTile->renderTargetArrayIndex, pHotTile->pBuffer);
            pHotTile->state = HOTTILE_DIRTY;
            RDTSC_STOP(BELoadTiles, 0, 0);
        }
        else if (pHotTile->state == HOTTILE_CLEAR)
        {
            RDTSC_START(BELoadTiles);
            // Clear the tile.
            ClearColorHotTile(pHotTile);
            pHotTile->state = HOTTILE_DIRTY;
            RDTSC_STOP(BELoadTiles, 0, 0);
        }
        colorHottileEnableMask &= ~(1 << rtSlot);
    }

    // check depth if enabled
    if (state.depthHottileEnable)
    {
        HOTTILE* pHotTile = pHotTileMgr->GetHotTile(pContext, pDC, macroID, SWR_ATTACHMENT_DEPTH, true, numSamples);
        if (pHotTile->state == HOTTILE_INVALID)
        {
            RDTSC_START(BELoadTiles);
            // invalid hottile before draw requires a load from surface before we can draw to it
            pContext->pfnLoadTile(GetPrivateState(pDC), KNOB_DEPTH_HOT_TILE_FORMAT, SWR_ATTACHMENT_DEPTH, x, y, pHotTile->renderTargetArrayIndex, pHotTile->pBuffer);
            pHotTile->state = HOTTILE_DIRTY;
            RDTSC_STOP(BELoadTiles, 0, 0);
        }
        else if (pHotTile->state == HOTTILE_CLEAR)
        {
            RDTSC_START(BELoadTiles);
            // Clear the tile.
            ClearDepthHotTile(pHotTile);
            pHotTile->state = HOTTILE_DIRTY;
            RDTSC_STOP(BELoadTiles, 0, 0);
        }
    }

    // check stencil if enabled
    if (state.stencilHottileEnable)
    {
        HOTTILE* pHotTile = pHotTileMgr->GetHotTile(pContext, pDC, macroID, SWR_ATTACHMENT_STENCIL, true, numSamples);
        if (pHotTile->state == HOTTILE_INVALID)
        {
            RDTSC_START(BELoadTiles);
            // invalid hottile before draw requires a load from surface before we can draw to it
            pContext->pfnLoadTile(GetPrivateState(pDC), KNOB_STENCIL_HOT_TILE_FORMAT, SWR_ATTACHMENT_STENCIL, x, y, pHotTile->renderTargetArrayIndex, pHotTile->pBuffer);
            pHotTile->state = HOTTILE_DIRTY;
            RDTSC_STOP(BELoadTiles, 0, 0);
        }
        else if (pHotTile->state == HOTTILE_CLEAR)
        {
            RDTSC_START(BELoadTiles);
            // Clear the tile.
            ClearStencilHotTile(pHotTile);
            pHotTile->state = HOTTILE_DIRTY;
            RDTSC_STOP(BELoadTiles, 0, 0);
        }
    }
}

INLINE bool FindFirstIncompleteDraw(SWR_CONTEXT* pContext, uint64_t& curDrawBE)
{
    // increment our current draw id to the first incomplete draw
    uint64_t drawEnqueued = GetEnqueuedDraw(pContext);
    while (curDrawBE < drawEnqueued)
    {
        DRAW_CONTEXT *pDC = &pContext->dcRing[curDrawBE % KNOB_MAX_DRAWS_IN_FLIGHT];

        // If its not compute and FE is not done then break out of loop.
        if (!pDC->doneFE && !pDC->isCompute) break;

        bool isWorkComplete = (pDC->isCompute) ?
            pDC->pDispatch->isWorkComplete() : pDC->pTileMgr->isWorkComplete();

        if (isWorkComplete)
        {
            curDrawBE++;
            InterlockedIncrement(&pDC->threadsDoneBE);
        }
        else
        {
            break;
        }
    }

    // If there are no more incomplete draws then return false.
    return (curDrawBE >= drawEnqueued) ? false : true;
}

//////////////////////////////////////////////////////////////////////////
/// @brief If there is any BE work then go work on it.
/// @param pContext - pointer to SWR context.
/// @param workerId - The unique worker ID that is assigned to this thread.
/// @param curDrawBE - This tracks the draw contexts that this thread has processed. Each worker thread
///                    has its own curDrawBE counter and this ensures that each worker processes all the
///                    draws in order.
/// @param lockedTiles - This is the set of tiles locked by other threads. Each thread maintains its
///                      own set and each time it fails to lock a macrotile, because its already locked,
///                      then it will add that tile to the lockedTiles set. As a worker begins to work
///                      on future draws the lockedTiles ensure that it doesn't work on tiles that may
///                      still have work pending in a previous draw. Additionally, the lockedTiles is
///                      hueristic that can steer a worker back to the same macrotile that it had been
///                      working on in a previous draw.
void WorkOnFifoBE(
    SWR_CONTEXT *pContext,
    uint32_t workerId,
    uint64_t &curDrawBE,
    std::unordered_set<uint32_t>& lockedTiles)
{
    // Find the first incomplete draw that has pending work. If no such draw is found then
    // return. FindFirstIncompleteDraw is responsible for incrementing the curDrawBE.
    if (FindFirstIncompleteDraw(pContext, curDrawBE) == false)
    {
        return;
    }

    uint64_t lastRetiredDraw = pContext->dcRing[curDrawBE % KNOB_MAX_DRAWS_IN_FLIGHT].drawId - 1;

    // Reset our history for locked tiles. We'll have to re-learn which tiles are locked.
    lockedTiles.clear();

    // Try to work on each draw in order of the available draws in flight.
    //   1. If we're on curDrawBE, we can work on any macrotile that is available.
    //   2. If we're trying to work on draws after curDrawBE, we are restricted to 
    //      working on those macrotiles that are known to be complete in the prior draw to
    //      maintain order. The locked tiles provides the history to ensures this.
    for (uint64_t i = curDrawBE; i < GetEnqueuedDraw(pContext); ++i)
    {
        DRAW_CONTEXT *pDC = &pContext->dcRing[i % KNOB_MAX_DRAWS_IN_FLIGHT];

        if (pDC->isCompute) return; // We don't look at compute work.

        // First wait for FE to be finished with this draw. This keeps threading model simple
        // but if there are lots of bubbles between draws then serializing FE and BE may
        // need to be revisited.
        if (!pDC->doneFE) return;
        
        // If this draw is dependent on a previous draw then we need to bail.
        if (CheckDependency(pContext, pDC, lastRetiredDraw))
        {
            return;
        }

        // Grab the list of all dirty macrotiles. A tile is dirty if it has work queued to it.
        std::vector<uint32_t> &macroTiles = pDC->pTileMgr->getDirtyTiles();

        for (uint32_t tileID : macroTiles)
        {
            MacroTileQueue &tile = pDC->pTileMgr->getMacroTileQueue(tileID);
            
            // can only work on this draw if it's not in use by other threads
            if (lockedTiles.find(tileID) == lockedTiles.end())
            {
                if (tile.getNumQueued())
                {
                    if (tile.tryLock())
                    {
                        BE_WORK *pWork;

                        RDTSC_START(WorkerFoundWork);

                        uint32_t numWorkItems = tile.getNumQueued();

                        if (numWorkItems != 0)
                        {
                            pWork = tile.peek();
                            SWR_ASSERT(pWork);
                            if (pWork->type == DRAW)
                            {
                                InitializeHotTiles(pContext, pDC, tileID, (const TRIANGLE_WORK_DESC*)&pWork->desc);
                            }
                        }

                        while ((pWork = tile.peek()) != nullptr)
                        {
                            pWork->pfnWork(pDC, workerId, tileID, &pWork->desc);
                            tile.dequeue();
                        }
                        RDTSC_STOP(WorkerFoundWork, numWorkItems, pDC->drawId);

                        _ReadWriteBarrier();

                        pDC->pTileMgr->markTileComplete(tileID);

                        // Optimization: If the draw is complete and we're the last one to have worked on it then
                        // we can reset the locked list as we know that all previous draws before the next are guaranteed to be complete.
                        if ((curDrawBE == i) && pDC->pTileMgr->isWorkComplete())
                        {
                            // We can increment the current BE and safely move to next draw since we know this draw is complete.
                            curDrawBE++;
                            InterlockedIncrement(&pDC->threadsDoneBE);

                            lastRetiredDraw++;

                            lockedTiles.clear();
                            break;
                        }
                    }
                    else
                    {
                        // This tile is already locked. So let's add it to our locked tiles set. This way we don't try locking this one again.
                        lockedTiles.insert(tileID);
                    }
                }
            }
        }
    }
}

void WorkOnFifoFE(SWR_CONTEXT *pContext, uint32_t workerId, uint64_t &curDrawFE, UCHAR numaNode)
{
    // Try to grab the next DC from the ring
    uint64_t drawEnqueued = GetEnqueuedDraw(pContext);
    while (curDrawFE < drawEnqueued)
    {
        uint32_t dcSlot = curDrawFE % KNOB_MAX_DRAWS_IN_FLIGHT;
        DRAW_CONTEXT *pDC = &pContext->dcRing[dcSlot];
        if (pDC->isCompute || pDC->doneFE || pDC->FeLock)
        {
            curDrawFE++;
            InterlockedIncrement(&pDC->threadsDoneFE);
        }
        else
        {
            break;
        }
    }

    uint64_t curDraw = curDrawFE;
    while (curDraw < drawEnqueued)
    {
        uint32_t dcSlot = curDraw % KNOB_MAX_DRAWS_IN_FLIGHT;
        DRAW_CONTEXT *pDC = &pContext->dcRing[dcSlot];

        if (!pDC->isCompute && !pDC->FeLock)
        {
            uint32_t initial = InterlockedCompareExchange((volatile uint32_t*)&pDC->FeLock, 1, 0);
            if (initial == 0)
            {
                // successfully grabbed the DC, now run the FE
                pDC->FeWork.pfnWork(pContext, pDC, workerId, &pDC->FeWork.desc);

                _ReadWriteBarrier();
                pDC->doneFE = true;
            }
        }
        curDraw++;
    }
}

//////////////////////////////////////////////////////////////////////////
/// @brief If there is any compute work then go work on it.
/// @param pContext - pointer to SWR context.
/// @param workerId - The unique worker ID that is assigned to this thread.
/// @param curDrawBE - This tracks the draw contexts that this thread has processed. Each worker thread
///                    has its own curDrawBE counter and this ensures that each worker processes all the
///                    draws in order.
void WorkOnCompute(
    SWR_CONTEXT *pContext,
    uint32_t workerId,
    uint64_t& curDrawBE)
{
    if (FindFirstIncompleteDraw(pContext, curDrawBE) == false)
    {
        return;
    }

    uint64_t lastRetiredDraw = pContext->dcRing[curDrawBE % KNOB_MAX_DRAWS_IN_FLIGHT].drawId - 1;

    DRAW_CONTEXT *pDC = &pContext->dcRing[curDrawBE % KNOB_MAX_DRAWS_IN_FLIGHT];
    if (pDC->isCompute == false) return;

    // check dependencies
    if (CheckDependency(pContext, pDC, lastRetiredDraw))
    {
        return;
    }

    SWR_ASSERT(pDC->pDispatch != nullptr);
    DispatchQueue& queue = *pDC->pDispatch;

    // Is there any work remaining?
    if (queue.getNumQueued() > 0)
    {
        bool lastToComplete = false;

        uint32_t threadGroupId = 0;
        while (queue.getWork(threadGroupId))
        {
            ProcessComputeBE(pDC, workerId, threadGroupId);

            lastToComplete = queue.finishedWork();
        }

        _ReadWriteBarrier();

        if (lastToComplete)
        {
            SWR_ASSERT(queue.isWorkComplete() == true);
            pDC->doneCompute = true;
        }
    }
}

DWORD workerThreadMain(LPVOID pData)
{
    THREAD_DATA *pThreadData = (THREAD_DATA*)pData;
    SWR_CONTEXT *pContext = pThreadData->pContext;
    uint32_t threadId = pThreadData->threadId;
    uint32_t workerId = pThreadData->workerId;

    bindThread(threadId, pThreadData->procGroupId, pThreadData->forceBindProcGroup); 

    RDTSC_INIT(threadId);

    int numaNode = (int)pThreadData->numaId;

    // flush denormals to 0
    _mm_setcsr(_mm_getcsr() | _MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON);

    // Track tiles locked by other threads. If we try to lock a macrotile and find its already
    // locked then we'll add it to this list so that we don't try and lock it again.
    std::unordered_set<uint32_t> lockedTiles;

    // each worker has the ability to work on any of the queued draws as long as certain
    // conditions are met. the data associated
    // with a draw is guaranteed to be active as long as a worker hasn't signaled that he 
    // has moved on to the next draw when he determines there is no more work to do. The api
    // thread will not increment the head of the dc ring until all workers have moved past the
    // current head.
    // the logic to determine what to work on is:
    // 1- try to work on the FE any draw that is queued. For now there are no dependencies
    //    on the FE work, so any worker can grab any FE and process in parallel.  Eventually
    //    we'll need dependency tracking to force serialization on FEs.  The worker will try
    //    to pick an FE by atomically incrementing a counter in the swr context.  he'll keep
    //    trying until he reaches the tail.
    // 2- BE work must be done in strict order. we accomplish this today by pulling work off
    //    the oldest draw (ie the head) of the dcRing. the worker can determine if there is
    //    any work left by comparing the total # of binned work items and the total # of completed
    //    work items. If they are equal, then there is no more work to do for this draw, and
    //    the worker can safely increment its oldestDraw counter and move on to the next draw.
    std::unique_lock<std::mutex> lock(pContext->WaitLock, std::defer_lock);

    auto threadHasWork = [&](uint64_t curDraw) { return curDraw != pContext->DrawEnqueued; };

    uint64_t curDrawBE = 1;
    uint64_t curDrawFE = 1;

    while (pContext->threadPool.inThreadShutdown == false)
    {
        uint32_t loop = 0;
        while (loop++ < KNOB_WORKER_SPIN_LOOP_COUNT && !threadHasWork(curDrawBE))
        {
            _mm_pause();
        }

        if (!threadHasWork(curDrawBE))
        {
            lock.lock();

            // check for thread idle condition again under lock
            if (threadHasWork(curDrawBE))
            {
                lock.unlock();
                continue;
            }

            if (pContext->threadPool.inThreadShutdown)
            {
                lock.unlock();
                break;
            }

            RDTSC_START(WorkerWaitForThreadEvent);

            pContext->FifosNotEmpty.wait(lock);
            lock.unlock();

            RDTSC_STOP(WorkerWaitForThreadEvent, 0, 0);

            if (pContext->threadPool.inThreadShutdown)
            {
                break;
            }
        }

        RDTSC_START(WorkerWorkOnFifoBE);
        WorkOnFifoBE(pContext, workerId, curDrawBE, lockedTiles);
        RDTSC_STOP(WorkerWorkOnFifoBE, 0, 0);

        WorkOnCompute(pContext, workerId, curDrawBE);

        WorkOnFifoFE(pContext, workerId, curDrawFE, numaNode);
    }

    return 0;
}

DWORD workerThreadInit(LPVOID pData)
{
#if defined(_WIN32)
    __try
#endif // _WIN32
    {
        return workerThreadMain(pData);
    }

#if defined(_WIN32)
    __except(EXCEPTION_CONTINUE_SEARCH)
    {
    }

#endif // _WIN32

    return 1;
}

void CreateThreadPool(SWR_CONTEXT *pContext, THREAD_POOL *pPool)
{
    bindThread(0);

    CPUNumaNodes nodes;
    uint32_t numThreadsPerProcGroup = 0;
    CalculateProcessorTopology(nodes, numThreadsPerProcGroup);

    uint32_t numHWNodes         = (uint32_t)nodes.size();
    uint32_t numHWCoresPerNode  = (uint32_t)nodes[0].cores.size();
    uint32_t numHWHyperThreads  = (uint32_t)nodes[0].cores[0].threadIds.size();

    uint32_t numNodes           = numHWNodes;
    uint32_t numCoresPerNode    = numHWCoresPerNode;
    uint32_t numHyperThreads    = numHWHyperThreads;

    if (KNOB_MAX_NUMA_NODES)
    {
        numNodes = std::min(numNodes, KNOB_MAX_NUMA_NODES);
    }

    if (KNOB_MAX_CORES_PER_NUMA_NODE)
    {
        numCoresPerNode = std::min(numCoresPerNode, KNOB_MAX_CORES_PER_NUMA_NODE);
    }

    if (KNOB_MAX_THREADS_PER_CORE)
    {
        numHyperThreads = std::min(numHyperThreads, KNOB_MAX_THREADS_PER_CORE);
    }

    // Calculate numThreads
    uint32_t numThreads = numNodes * numCoresPerNode * numHyperThreads;

    if (KNOB_MAX_WORKER_THREADS)
    {
        uint32_t maxHWThreads = numHWNodes * numHWCoresPerNode * numHWHyperThreads;
        numThreads = std::min(KNOB_MAX_WORKER_THREADS, maxHWThreads);
    }

    if (numThreads > KNOB_MAX_NUM_THREADS)
    {
        printf("WARNING: system thread count %u exceeds max %u, "
            "performance will be degraded\n",
            numThreads, KNOB_MAX_NUM_THREADS);
    }

    if (numThreads == 1)
    {
        // If only 1 worker thread, try to move it to an available
        // HW thread.  If that fails, use the API thread.
        if (numCoresPerNode < numHWCoresPerNode)
        {
            numCoresPerNode++;
        }
        else if (numHyperThreads < numHWHyperThreads)
        {
            numHyperThreads++;
        }
        else if (numNodes < numHWNodes)
        {
            numNodes++;
        }
        else
        {
            pPool->numThreads = 0;
            SET_KNOB(SINGLE_THREADED, true);
            return;
        }
    }
    else
    {
        // Save a HW thread for the API thread.
        numThreads--;
    }

    pPool->numThreads = numThreads;
    pContext->NumWorkerThreads = pPool->numThreads;

    pPool->inThreadShutdown = false;
    pPool->pThreadData = (THREAD_DATA *)malloc(pPool->numThreads * sizeof(THREAD_DATA));

    if (KNOB_MAX_WORKER_THREADS)
    {
        bool bForceBindProcGroup = (numThreads > numThreadsPerProcGroup);
        uint32_t numProcGroups = (numThreads + numThreadsPerProcGroup - 1) / numThreadsPerProcGroup;
        // When MAX_WORKER_THREADS is set we don't bother to bind to specific HW threads
        // But Windows will still require binding to specific process groups
        for (uint32_t workerId = 0; workerId < numThreads; ++workerId)
        {
            pPool->pThreadData[workerId].workerId = workerId;
            pPool->pThreadData[workerId].procGroupId = workerId % numProcGroups;
            pPool->pThreadData[workerId].threadId = 0;
            pPool->pThreadData[workerId].numaId = 0;
            pPool->pThreadData[workerId].pContext = pContext;
            pPool->pThreadData[workerId].forceBindProcGroup = bForceBindProcGroup;
            pPool->threads[workerId] = new std::thread(workerThreadInit, &pPool->pThreadData[workerId]);
        }
    }
    else
    {
        uint32_t workerId = 0;
        for (uint32_t n = 0; n < numNodes; ++n)
        {
            auto& node = nodes[n];

            uint32_t numCores = numCoresPerNode;
            for (uint32_t c = 0; c < numCores; ++c)
            {
                auto& core = node.cores[c];
                for (uint32_t t = 0; t < numHyperThreads; ++t)
                {
                    if (c == 0 && n == 0 && t == 0)
                    {
                        // Skip core 0, thread0  on node 0 to reserve for API thread
                        continue;
                    }

                    pPool->pThreadData[workerId].workerId = workerId;
                    pPool->pThreadData[workerId].procGroupId = core.procGroup;
                    pPool->pThreadData[workerId].threadId = core.threadIds[t];
                    pPool->pThreadData[workerId].numaId = n;
                    pPool->pThreadData[workerId].pContext = pContext;
                    pPool->threads[workerId] = new std::thread(workerThreadInit, &pPool->pThreadData[workerId]);

                    ++workerId;
                }
            }
        }
    }
}

void DestroyThreadPool(SWR_CONTEXT *pContext, THREAD_POOL *pPool)
{
    if (!KNOB_SINGLE_THREADED)
    {
        // Inform threads to finish up
        std::unique_lock<std::mutex> lock(pContext->WaitLock);
        pPool->inThreadShutdown = true;
        _mm_mfence();
        pContext->FifosNotEmpty.notify_all();
        lock.unlock();

        // Wait for threads to finish and destroy them
        for (uint32_t t = 0; t < pPool->numThreads; ++t)
        {
            pPool->threads[t]->join();
            delete(pPool->threads[t]);
        }

        // Clean up data used by threads
        free(pPool->pThreadData);
    }
}
