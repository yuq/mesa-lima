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
* @file api.cpp
*
* @brief API implementation
*
******************************************************************************/

#include <cfloat>
#include <cmath>
#include <cstdio>

#include "core/api.h"
#include "core/backend.h"
#include "core/context.h"
#include "core/frontend.h"
#include "core/rasterizer.h"
#include "core/rdtsc_core.h"
#include "core/threads.h"
#include "core/tilemgr.h"
#include "core/clip.h"

#include "common/simdintrin.h"
#include "common/os.h"

void SetupDefaultState(SWR_CONTEXT *pContext);

//////////////////////////////////////////////////////////////////////////
/// @brief Create SWR Context.
/// @param pCreateInfo - pointer to creation info.
HANDLE SwrCreateContext(
    const SWR_CREATECONTEXT_INFO* pCreateInfo)
{
    RDTSC_RESET();
    RDTSC_INIT(0);

    void* pContextMem = _aligned_malloc(sizeof(SWR_CONTEXT), KNOB_SIMD_WIDTH * 4);
    memset(pContextMem, 0, sizeof(SWR_CONTEXT));
    SWR_CONTEXT *pContext = new (pContextMem) SWR_CONTEXT();

    pContext->driverType = pCreateInfo->driver;
    pContext->privateStateSize = pCreateInfo->privateStateSize;

    pContext->dcRing = (DRAW_CONTEXT*)_aligned_malloc(sizeof(DRAW_CONTEXT)*KNOB_MAX_DRAWS_IN_FLIGHT, 64);
    memset(pContext->dcRing, 0, sizeof(DRAW_CONTEXT)*KNOB_MAX_DRAWS_IN_FLIGHT);

    pContext->dsRing = (DRAW_STATE*)_aligned_malloc(sizeof(DRAW_STATE)*KNOB_MAX_DRAWS_IN_FLIGHT, 64);
    memset(pContext->dsRing, 0, sizeof(DRAW_STATE)*KNOB_MAX_DRAWS_IN_FLIGHT);

    pContext->numSubContexts = pCreateInfo->maxSubContexts;
    if (pContext->numSubContexts > 1)
    {
        pContext->subCtxSave = (DRAW_STATE*)_aligned_malloc(sizeof(DRAW_STATE) * pContext->numSubContexts, 64);
        memset(pContext->subCtxSave, 0, sizeof(DRAW_STATE) * pContext->numSubContexts);
    }

    for (uint32_t dc = 0; dc < KNOB_MAX_DRAWS_IN_FLIGHT; ++dc)
    {
        pContext->dcRing[dc].pArena = new Arena();
        pContext->dcRing[dc].inUse = false;
        pContext->dcRing[dc].pTileMgr = new MacroTileMgr(*(pContext->dcRing[dc].pArena));
        pContext->dcRing[dc].pDispatch = new DispatchQueue(); /// @todo Could lazily allocate this if Dispatch seen.

        pContext->dsRing[dc].pArena = new Arena();
    }

    if (!KNOB_SINGLE_THREADED)
    {
        memset(&pContext->WaitLock, 0, sizeof(pContext->WaitLock));
        memset(&pContext->FifosNotEmpty, 0, sizeof(pContext->FifosNotEmpty));
        new (&pContext->WaitLock) std::mutex();
        new (&pContext->FifosNotEmpty) std::condition_variable();

        CreateThreadPool(pContext, &pContext->threadPool);
    }

    // Calling createThreadPool() above can set SINGLE_THREADED
    if (KNOB_SINGLE_THREADED)
    {
        pContext->NumWorkerThreads = 1;
    }

    // Allocate scratch space for workers.
    ///@note We could lazily allocate this but its rather small amount of memory.
    for (uint32_t i = 0; i < pContext->NumWorkerThreads; ++i)
    {
        ///@todo Use numa API for allocations using numa information from thread data (if exists).
        pContext->pScratch[i] = (uint8_t*)_aligned_malloc((32 * 1024), KNOB_SIMD_WIDTH * 4);
    }

    pContext->nextDrawId = 1;
    pContext->DrawEnqueued = 1;

    // State setup AFTER context is fully initialized
    SetupDefaultState(pContext);

    // initialize hot tile manager
    pContext->pHotTileMgr = new HotTileMgr();

    // initialize function pointer tables
    InitClearTilesTable();

    // initialize store tiles function
    pContext->pfnLoadTile = pCreateInfo->pfnLoadTile;
    pContext->pfnStoreTile = pCreateInfo->pfnStoreTile;
    pContext->pfnClearTile = pCreateInfo->pfnClearTile;

    return (HANDLE)pContext;
}

void SwrDestroyContext(HANDLE hContext)
{
    SWR_CONTEXT *pContext = (SWR_CONTEXT*)hContext;
    DestroyThreadPool(pContext, &pContext->threadPool);

    // free the fifos
    for (uint32_t i = 0; i < KNOB_MAX_DRAWS_IN_FLIGHT; ++i)
    {
        delete pContext->dcRing[i].pArena;
        delete pContext->dsRing[i].pArena;
        delete(pContext->dcRing[i].pTileMgr);
        delete(pContext->dcRing[i].pDispatch);
    }

    // Free scratch space.
    for (uint32_t i = 0; i < pContext->NumWorkerThreads; ++i)
    {
        _aligned_free(pContext->pScratch[i]);
    }

    _aligned_free(pContext->dcRing);
    _aligned_free(pContext->dsRing);
    _aligned_free(pContext->subCtxSave);

    delete(pContext->pHotTileMgr);

    pContext->~SWR_CONTEXT();
    _aligned_free((SWR_CONTEXT*)hContext);
}

void CopyState(DRAW_STATE& dst, const DRAW_STATE& src)
{
    memcpy(&dst.state, &src.state, sizeof(API_STATE));
}

void WakeAllThreads(SWR_CONTEXT *pContext)
{
    pContext->FifosNotEmpty.notify_all();
}

bool StillDrawing(SWR_CONTEXT *pContext, DRAW_CONTEXT *pDC)
{
    // For single thread nothing should still be drawing.
    if (KNOB_SINGLE_THREADED) { return false; }

    if (pDC->isCompute)
    {
        if (pDC->doneCompute)
        {
            pDC->inUse = false;
            return false;
        }
    }

    // Check if backend work is done. First make sure all triangles have been binned.
    if (pDC->doneFE == true)
    {
        // ensure workers have all moved passed this draw
        if (pDC->threadsDoneFE != pContext->NumWorkerThreads)
        {
            return true;
        }

        if (pDC->threadsDoneBE != pContext->NumWorkerThreads)
        {
            return true;
        }

        pDC->inUse = false;    // all work is done.
    }

    return pDC->inUse;
}

void QueueDraw(SWR_CONTEXT *pContext)
{
    SWR_ASSERT(pContext->pCurDrawContext->inUse == false);
    pContext->pCurDrawContext->inUse = true;

    _ReadWriteBarrier();
    {
        std::unique_lock<std::mutex> lock(pContext->WaitLock);
        pContext->DrawEnqueued++;
    }

    if (KNOB_SINGLE_THREADED)
    {
        // flush denormals to 0
        uint32_t mxcsr = _mm_getcsr();
        _mm_setcsr(mxcsr | _MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON);

        std::unordered_set<uint32_t> lockedTiles;
        uint64_t curDraw[2] = { pContext->pCurDrawContext->drawId, pContext->pCurDrawContext->drawId };
        WorkOnFifoFE(pContext, 0, curDraw[0], 0);
        WorkOnFifoBE(pContext, 0, curDraw[1], lockedTiles);

        // restore csr
        _mm_setcsr(mxcsr);
    }
    else
    {
        RDTSC_START(APIDrawWakeAllThreads);
        WakeAllThreads(pContext);
        RDTSC_STOP(APIDrawWakeAllThreads, 1, 0);
    }

    // Set current draw context to NULL so that next state call forces a new draw context to be created and populated.
    pContext->pPrevDrawContext = pContext->pCurDrawContext;
    pContext->pCurDrawContext = nullptr;
}

///@todo Combine this with QueueDraw
void QueueDispatch(SWR_CONTEXT *pContext)
{
    SWR_ASSERT(pContext->pCurDrawContext->inUse == false);
    pContext->pCurDrawContext->inUse = true;

    _ReadWriteBarrier();
    {
        std::unique_lock<std::mutex> lock(pContext->WaitLock);
        pContext->DrawEnqueued++;
    }

    if (KNOB_SINGLE_THREADED)
    {
        // flush denormals to 0
        uint32_t mxcsr = _mm_getcsr();
        _mm_setcsr(mxcsr | _MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON);

        uint64_t curDispatch = pContext->pCurDrawContext->drawId;
        WorkOnCompute(pContext, 0, curDispatch);

        // restore csr
        _mm_setcsr(mxcsr);
    }
    else
    {
        RDTSC_START(APIDrawWakeAllThreads);
        WakeAllThreads(pContext);
        RDTSC_STOP(APIDrawWakeAllThreads, 1, 0);
    }

    // Set current draw context to NULL so that next state call forces a new draw context to be created and populated.
    pContext->pPrevDrawContext = pContext->pCurDrawContext;
    pContext->pCurDrawContext = nullptr;
}

DRAW_CONTEXT* GetDrawContext(SWR_CONTEXT *pContext, bool isSplitDraw = false)
{
    RDTSC_START(APIGetDrawContext);
    // If current draw context is null then need to obtain a new draw context to use from ring.
    if (pContext->pCurDrawContext == nullptr)
    {
        uint32_t dcIndex = pContext->nextDrawId % KNOB_MAX_DRAWS_IN_FLIGHT;

        DRAW_CONTEXT* pCurDrawContext = &pContext->dcRing[dcIndex];
        pContext->pCurDrawContext = pCurDrawContext;

        // Need to wait until this draw context is available to use.
        while (StillDrawing(pContext, pCurDrawContext))
        {
            _mm_pause();
        }

        // Assign next available entry in DS ring to this DC.
        uint32_t dsIndex = pContext->curStateId % KNOB_MAX_DRAWS_IN_FLIGHT;
        pCurDrawContext->pState = &pContext->dsRing[dsIndex];

        Arena& stateArena = *(pCurDrawContext->pState->pArena);

        // Copy previous state to current state.
        if (pContext->pPrevDrawContext)
        {
            DRAW_CONTEXT* pPrevDrawContext = pContext->pPrevDrawContext;

            // If we're splitting our draw then we can just use the same state from the previous
            // draw. In this case, we won't increment the DS ring index so the next non-split
            // draw can receive the state.
            if (isSplitDraw == false)
            {
                CopyState(*pCurDrawContext->pState, *pPrevDrawContext->pState);

                stateArena.Reset(true);    // Reset memory.
                pCurDrawContext->pState->pPrivateState = nullptr;

                pContext->curStateId++;  // Progress state ring index forward.
            }
            else
            {
                // If its a split draw then just copy the state pointer over
                // since its the same draw.
                pCurDrawContext->pState = pPrevDrawContext->pState;
            }
        }
        else
        {
            stateArena.Reset();    // Reset memory.
            pContext->curStateId++;  // Progress state ring index forward.
        }

        pCurDrawContext->dependency = 0;
        pCurDrawContext->pArena->Reset();
        pCurDrawContext->pContext = pContext;
        pCurDrawContext->isCompute = false; // Dispatch has to set this to true.
        pCurDrawContext->inUse = false;

        pCurDrawContext->doneCompute = false;
        pCurDrawContext->doneFE = false;
        pCurDrawContext->FeLock = 0;
        pCurDrawContext->threadsDoneFE = 0;
        pCurDrawContext->threadsDoneBE = 0;

        pCurDrawContext->pTileMgr->initialize();

        // Assign unique drawId for this DC
        pCurDrawContext->drawId = pContext->nextDrawId++;
    }
    else
    {
        SWR_ASSERT(isSplitDraw == false, "Split draw should only be used when obtaining a new DC");
    }

    RDTSC_STOP(APIGetDrawContext, 0, 0);
    return pContext->pCurDrawContext;
}

void SWR_API SwrSetActiveSubContext(
    HANDLE hContext,
    uint32_t subContextIndex)
{
    SWR_CONTEXT *pContext = (SWR_CONTEXT*)hContext;
    if (subContextIndex >= pContext->numSubContexts)
    {
        return;
    }

    if (subContextIndex != pContext->curSubCtxId)
    {
        // Save and restore draw state
        DRAW_CONTEXT* pDC = GetDrawContext(pContext);
        CopyState(
            pContext->subCtxSave[pContext->curSubCtxId],
            *(pDC->pState));

        CopyState(
            *(pDC->pState),
            pContext->subCtxSave[subContextIndex]);

        pContext->curSubCtxId = subContextIndex;
    }
}

API_STATE* GetDrawState(SWR_CONTEXT *pContext)
{
    DRAW_CONTEXT* pDC = GetDrawContext(pContext);
    SWR_ASSERT(pDC->pState != nullptr);

    return &pDC->pState->state;
}

void SetupDefaultState(SWR_CONTEXT *pContext)
{
    API_STATE* pState = GetDrawState(pContext);

    pState->rastState.cullMode = SWR_CULLMODE_NONE;
    pState->rastState.frontWinding = SWR_FRONTWINDING_CCW;
}

static INLINE SWR_CONTEXT* GetContext(HANDLE hContext)
{
    return (SWR_CONTEXT*)hContext;
}

void SwrSync(HANDLE hContext, PFN_CALLBACK_FUNC pfnFunc, uint64_t userData, uint64_t userData2, uint64_t userData3)
{
    RDTSC_START(APISync);

    SWR_ASSERT(pfnFunc != nullptr);

    SWR_CONTEXT *pContext = GetContext(hContext);
    DRAW_CONTEXT* pDC = GetDrawContext(pContext);

    pDC->FeWork.type = SYNC;
    pDC->FeWork.pfnWork = ProcessSync;
    pDC->FeWork.desc.sync.pfnCallbackFunc = pfnFunc;
    pDC->FeWork.desc.sync.userData = userData;
    pDC->FeWork.desc.sync.userData2 = userData2;
    pDC->FeWork.desc.sync.userData3 = userData3;

    // cannot execute until all previous draws have completed
    pDC->dependency = pDC->drawId - 1;

    //enqueue
    QueueDraw(pContext);

    RDTSC_STOP(APISync, 1, 0);
}

void SwrWaitForIdle(HANDLE hContext)
{
    SWR_CONTEXT *pContext = GetContext(hContext);

    RDTSC_START(APIWaitForIdle);
    // Wait for all work to complete.
    for (uint32_t dc = 0; dc < KNOB_MAX_DRAWS_IN_FLIGHT; ++dc)
    {
        DRAW_CONTEXT *pDC = &pContext->dcRing[dc];

        while (StillDrawing(pContext, pDC))
        {
            _mm_pause();
        }
    }
    RDTSC_STOP(APIWaitForIdle, 1, 0);
}

void SwrSetVertexBuffers(
    HANDLE hContext,
    uint32_t numBuffers,
    const SWR_VERTEX_BUFFER_STATE* pVertexBuffers)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));

    for (uint32_t i = 0; i < numBuffers; ++i)
    {
        const SWR_VERTEX_BUFFER_STATE *pVB = &pVertexBuffers[i];
        pState->vertexBuffers[pVB->index] = *pVB;
    }
}

void SwrSetIndexBuffer(
    HANDLE hContext,
    const SWR_INDEX_BUFFER_STATE* pIndexBuffer)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));

    pState->indexBuffer = *pIndexBuffer;
}

void SwrSetFetchFunc(
    HANDLE hContext,
    PFN_FETCH_FUNC    pfnFetchFunc)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));

    pState->pfnFetchFunc = pfnFetchFunc;
}

void SwrSetSoFunc(
    HANDLE hContext,
    PFN_SO_FUNC    pfnSoFunc,
    uint32_t streamIndex)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));

    SWR_ASSERT(streamIndex < MAX_SO_STREAMS);

    pState->pfnSoFunc[streamIndex] = pfnSoFunc;
}

void SwrSetSoState(
    HANDLE hContext,
    SWR_STREAMOUT_STATE* pSoState)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));

    pState->soState = *pSoState;
}

void SwrSetSoBuffers(
    HANDLE hContext,
    SWR_STREAMOUT_BUFFER* pSoBuffer,
    uint32_t slot)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));

    SWR_ASSERT((slot < 4), "There are only 4 SO buffer slots [0, 3]\nSlot requested: %d", slot);

    pState->soBuffer[slot] = *pSoBuffer;
}

void SwrSetVertexFunc(
    HANDLE hContext,
    PFN_VERTEX_FUNC pfnVertexFunc)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));

    pState->pfnVertexFunc = pfnVertexFunc;
}

void SwrSetFrontendState(
    HANDLE hContext,
    SWR_FRONTEND_STATE *pFEState)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));
    pState->frontendState = *pFEState;
}

void SwrSetGsState(
    HANDLE hContext,
    SWR_GS_STATE *pGSState)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));
    pState->gsState = *pGSState;
}

void SwrSetGsFunc(
    HANDLE hContext,
    PFN_GS_FUNC pfnGsFunc)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));
    pState->pfnGsFunc = pfnGsFunc;
}

void SwrSetCsFunc(
    HANDLE hContext,
    PFN_CS_FUNC pfnCsFunc,
    uint32_t totalThreadsInGroup)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));
    pState->pfnCsFunc = pfnCsFunc;
    pState->totalThreadsInGroup = totalThreadsInGroup;
}

void SwrSetTsState(
    HANDLE hContext,
    SWR_TS_STATE *pState)
{
    API_STATE* pApiState = GetDrawState(GetContext(hContext));
    pApiState->tsState = *pState;
}

void SwrSetHsFunc(
    HANDLE hContext,
    PFN_HS_FUNC pfnFunc)
{
    API_STATE* pApiState = GetDrawState(GetContext(hContext));
    pApiState->pfnHsFunc = pfnFunc;
}

void SwrSetDsFunc(
    HANDLE hContext,
    PFN_DS_FUNC pfnFunc)
{
    API_STATE* pApiState = GetDrawState(GetContext(hContext));
    pApiState->pfnDsFunc = pfnFunc;
}

void SwrSetDepthStencilState(
    HANDLE hContext,
    SWR_DEPTH_STENCIL_STATE *pDSState)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));

    pState->depthStencilState = *pDSState;
}

void SwrSetBackendState(
    HANDLE hContext,
    SWR_BACKEND_STATE *pBEState)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));

    pState->backendState = *pBEState;
}

void SwrSetPixelShaderState(
    HANDLE hContext,
    SWR_PS_STATE *pPSState)
{
    API_STATE *pState = GetDrawState(GetContext(hContext));
    pState->psState = *pPSState;
}

void SwrSetBlendState(
    HANDLE hContext,
    SWR_BLEND_STATE *pBlendState)
{
    API_STATE *pState = GetDrawState(GetContext(hContext));
    memcpy(&pState->blendState, pBlendState, sizeof(SWR_BLEND_STATE));
}

void SwrSetBlendFunc(
    HANDLE hContext,
    uint32_t renderTarget,
    PFN_BLEND_JIT_FUNC pfnBlendFunc)
{
    SWR_ASSERT(renderTarget < SWR_NUM_RENDERTARGETS);
    API_STATE *pState = GetDrawState(GetContext(hContext));
    pState->pfnBlendFunc[renderTarget] = pfnBlendFunc;
}

void SwrSetLinkage(
    HANDLE hContext,
    uint32_t mask,
    const uint8_t* pMap)
{
    API_STATE* pState = GetDrawState(GetContext(hContext));

    static const uint8_t IDENTITY_MAP[] =
    {
         0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    };
    static_assert(sizeof(IDENTITY_MAP) == sizeof(pState->linkageMap),
        "Update for new value of MAX_ATTRIBUTES");

    pState->linkageMask = mask;
    pState->linkageCount = _mm_popcnt_u32(mask);

    if (!pMap)
    {
        pMap = IDENTITY_MAP;
    }
    memcpy(pState->linkageMap, pMap, pState->linkageCount);
}

// update guardband multipliers for the viewport
void updateGuardband(API_STATE *pState)
{
    // guardband center is viewport center
    pState->gbState.left    = KNOB_GUARDBAND_WIDTH  / pState->vp[0].width;
    pState->gbState.right   = KNOB_GUARDBAND_WIDTH  / pState->vp[0].width;
    pState->gbState.top     = KNOB_GUARDBAND_HEIGHT / pState->vp[0].height;
    pState->gbState.bottom  = KNOB_GUARDBAND_HEIGHT / pState->vp[0].height;
}

void SwrSetRastState(
    HANDLE hContext,
    const SWR_RASTSTATE *pRastState)
{
    SWR_CONTEXT *pContext = GetContext(hContext);
    API_STATE* pState = GetDrawState(pContext);

    memcpy(&pState->rastState, pRastState, sizeof(SWR_RASTSTATE));
}

void SwrSetViewports(
    HANDLE hContext,
    uint32_t numViewports,
    const SWR_VIEWPORT* pViewports,
    const SWR_VIEWPORT_MATRIX* pMatrices)
{
    SWR_ASSERT(numViewports <= KNOB_NUM_VIEWPORTS_SCISSORS,
        "Invalid number of viewports.");

    SWR_CONTEXT *pContext = GetContext(hContext);
    API_STATE* pState = GetDrawState(pContext);

    memcpy(&pState->vp[0], pViewports, sizeof(SWR_VIEWPORT) * numViewports);

    if (pMatrices != nullptr)
    {
        memcpy(&pState->vpMatrix[0], pMatrices, sizeof(SWR_VIEWPORT_MATRIX) * numViewports);
    }
    else
    {
        // Compute default viewport transform.
        for (uint32_t i = 0; i < numViewports; ++i)
        {
            if (pContext->driverType == DX)
            {
                pState->vpMatrix[i].m00 = pState->vp[i].width / 2.0f;
                pState->vpMatrix[i].m11 = -pState->vp[i].height / 2.0f;
                pState->vpMatrix[i].m22 = pState->vp[i].maxZ - pState->vp[i].minZ;
                pState->vpMatrix[i].m30 = pState->vp[i].x + pState->vpMatrix[i].m00;
                pState->vpMatrix[i].m31 = pState->vp[i].y - pState->vpMatrix[i].m11;
                pState->vpMatrix[i].m32 = pState->vp[i].minZ;
            }
            else
            {
                // Standard, with the exception that Y is inverted.
                pState->vpMatrix[i].m00 = (pState->vp[i].width - pState->vp[i].x) / 2.0f;
                pState->vpMatrix[i].m11 = (pState->vp[i].y - pState->vp[i].height) / 2.0f;
                pState->vpMatrix[i].m22 = (pState->vp[i].maxZ - pState->vp[i].minZ) / 2.0f;
                pState->vpMatrix[i].m30 = pState->vp[i].x + pState->vpMatrix[i].m00;
                pState->vpMatrix[i].m31 = pState->vp[i].height + pState->vpMatrix[i].m11;
                pState->vpMatrix[i].m32 = pState->vp[i].minZ + pState->vpMatrix[i].m22;

                // Now that the matrix is calculated, clip the view coords to screen size.
                // OpenGL allows for -ve x,y in the viewport.
                pState->vp[i].x = std::max(pState->vp[i].x, 0.0f);
                pState->vp[i].y = std::max(pState->vp[i].y, 0.0f);
            }
        }
    }

    updateGuardband(pState);
}

void SwrSetScissorRects(
    HANDLE hContext,
    uint32_t numScissors,
    const BBOX* pScissors)
{
    SWR_ASSERT(numScissors <= KNOB_NUM_VIEWPORTS_SCISSORS,
        "Invalid number of scissor rects.");

    API_STATE* pState = GetDrawState(GetContext(hContext));
    memcpy(&pState->scissorRects[0], pScissors, numScissors * sizeof(BBOX));
};

void SetupMacroTileScissors(DRAW_CONTEXT *pDC)
{
    API_STATE *pState = &pDC->pState->state;
    uint32_t left, right, top, bottom;

    // Set up scissor dimensions based on scissor or viewport
    if (pState->rastState.scissorEnable)
    {
        // scissor rect right/bottom edge are exclusive, core expects scissor dimensions to be inclusive, so subtract one pixel from right/bottom edges
        left = pState->scissorRects[0].left;
        right = pState->scissorRects[0].right;
        top = pState->scissorRects[0].top;
        bottom = pState->scissorRects[0].bottom;
    }
    else
    {
        left = (int32_t)pState->vp[0].x;
        right = (int32_t)pState->vp[0].x + (int32_t)pState->vp[0].width;
        top = (int32_t)pState->vp[0].y;
        bottom = (int32_t)pState->vp[0].y + (int32_t)pState->vp[0].height;
    }

    right = std::min<uint32_t>(right, KNOB_MAX_SCISSOR_X);
    bottom = std::min<uint32_t>(bottom, KNOB_MAX_SCISSOR_Y);

    if (left > KNOB_MAX_SCISSOR_X || top > KNOB_MAX_SCISSOR_Y)
    {
        pState->scissorInFixedPoint.left = 0;
        pState->scissorInFixedPoint.right = 0;
        pState->scissorInFixedPoint.top = 0;
        pState->scissorInFixedPoint.bottom = 0;
    }
    else
    {
        pState->scissorInFixedPoint.left = left * FIXED_POINT_SCALE;
        pState->scissorInFixedPoint.right = right * FIXED_POINT_SCALE - 1;
        pState->scissorInFixedPoint.top = top * FIXED_POINT_SCALE;
        pState->scissorInFixedPoint.bottom = bottom * FIXED_POINT_SCALE - 1;
    }
}

void SetupPipeline(DRAW_CONTEXT *pDC)
{
    DRAW_STATE* pState = pDC->pState;
    const SWR_RASTSTATE &rastState = pState->state.rastState;
    BACKEND_FUNCS& backendFuncs = pState->backendFuncs;
    const uint32_t forcedSampleCount = (rastState.bForcedSampleCount) ? 1 : 0;

    // setup backend
    if (pState->state.psState.pfnPixelShader == nullptr)
    {
        backendFuncs.pfnBackend = gBackendNullPs[pState->state.rastState.sampleCount];
        // always need to generate I & J per sample for Z interpolation
        backendFuncs.pfnCalcSampleBarycentrics = gSampleBarycentricTable[1];
    }
    else
    {
        const bool bMultisampleEnable = ((rastState.sampleCount > SWR_MULTISAMPLE_1X) || rastState.bForcedSampleCount) ? 1 : 0;
        const uint32_t centroid = ((pState->state.psState.barycentricsMask & SWR_BARYCENTRIC_CENTROID_MASK) > 0) ? 1 : 0;

        // currently only support 'normal' input coverage
        SWR_ASSERT(pState->state.psState.inputCoverage == SWR_INPUT_COVERAGE_NORMAL ||
                   pState->state.psState.inputCoverage == SWR_INPUT_COVERAGE_NONE);
     
        SWR_BARYCENTRICS_MASK barycentricsMask = (SWR_BARYCENTRICS_MASK)pState->state.psState.barycentricsMask;
        
        // select backend function
        switch(pState->state.psState.shadingRate)
        {
        case SWR_SHADING_RATE_PIXEL:
            if(bMultisampleEnable)
            {
                // always need to generate I & J per sample for Z interpolation
                barycentricsMask = (SWR_BARYCENTRICS_MASK)(barycentricsMask | SWR_BARYCENTRIC_PER_SAMPLE_MASK);
                backendFuncs.pfnBackend = gBackendPixelRateTable[rastState.sampleCount][rastState.samplePattern][pState->state.psState.inputCoverage][centroid][forcedSampleCount];
                backendFuncs.pfnOutputMerger = gBackendOutputMergerTable[pState->state.psState.numRenderTargets][pState->state.blendState.sampleCount];
            }
            else
            {
                // always need to generate I & J per pixel for Z interpolation
                barycentricsMask = (SWR_BARYCENTRICS_MASK)(barycentricsMask | SWR_BARYCENTRIC_PER_PIXEL_MASK);
                backendFuncs.pfnBackend = gBackendSingleSample[pState->state.psState.inputCoverage][centroid];
                backendFuncs.pfnOutputMerger = gBackendOutputMergerTable[pState->state.psState.numRenderTargets][SWR_MULTISAMPLE_1X];
            }
            break;
        case SWR_SHADING_RATE_SAMPLE:
            SWR_ASSERT(rastState.samplePattern == SWR_MSAA_STANDARD_PATTERN);
            // always need to generate I & J per sample for Z interpolation
            barycentricsMask = (SWR_BARYCENTRICS_MASK)(barycentricsMask | SWR_BARYCENTRIC_PER_SAMPLE_MASK);
            backendFuncs.pfnBackend = gBackendSampleRateTable[rastState.sampleCount][pState->state.psState.inputCoverage][centroid];
            backendFuncs.pfnOutputMerger = gBackendOutputMergerTable[pState->state.psState.numRenderTargets][pState->state.blendState.sampleCount];
            break;
        case SWR_SHADING_RATE_COARSE:
        default:
            SWR_ASSERT(0 && "Invalid shading rate");
            break;
        }

        // setup pointer to function that generates necessary barycentrics required by the PS
        bool bBarycentrics = (barycentricsMask & SWR_BARYCENTRIC_PER_PIXEL_MASK) > 0 ? 1 : 0;
        backendFuncs.pfnCalcPixelBarycentrics = gPixelBarycentricTable[bBarycentrics];

        bBarycentrics = (barycentricsMask & SWR_BARYCENTRIC_PER_SAMPLE_MASK) > 0 ? 1 : 0;
        backendFuncs.pfnCalcSampleBarycentrics = gSampleBarycentricTable[bBarycentrics];

        bBarycentrics = (barycentricsMask & SWR_BARYCENTRIC_CENTROID_MASK) > 0 ? 1 : 0;
        backendFuncs.pfnCalcCentroidBarycentrics = gCentroidBarycentricTable[rastState.sampleCount][bBarycentrics][rastState.samplePattern][forcedSampleCount];
    }
    
    PFN_PROCESS_PRIMS pfnBinner;
    switch (pState->state.topology)
    {
    case TOP_POINT_LIST:
        pState->pfnProcessPrims = ClipPoints;
        pfnBinner = BinPoints;
        break;
    case TOP_LINE_LIST:
    case TOP_LINE_STRIP:
    case TOP_LINE_LOOP:
    case TOP_LINE_LIST_ADJ:
    case TOP_LISTSTRIP_ADJ:
        pState->pfnProcessPrims = ClipLines;
        pfnBinner = BinLines;
        break;
    default:
        pState->pfnProcessPrims = ClipTriangles;
        pfnBinner = BinTriangles;
        break;
    };

    // disable clipper if viewport transform is disabled
    if (pState->state.frontendState.vpTransformDisable)
    {
        pState->pfnProcessPrims = pfnBinner;
    }

    if ((pState->state.psState.pfnPixelShader == nullptr) &&
        (pState->state.depthStencilState.depthTestEnable == FALSE) &&
        (pState->state.depthStencilState.depthWriteEnable == FALSE) &&
        (pState->state.depthStencilState.stencilTestEnable == FALSE) &&
        (pState->state.depthStencilState.stencilWriteEnable == FALSE) &&
        (pState->state.linkageCount == 0))
    {
        pState->pfnProcessPrims = nullptr;
        pState->state.linkageMask = 0;
    }

    if (pState->state.soState.rasterizerDisable == true)
    {
        pState->pfnProcessPrims = nullptr;
        pState->state.linkageMask = 0;
    }

    // set up the frontend attrib mask
    pState->state.feAttribMask = pState->state.linkageMask;
    if (pState->state.soState.soEnable)
    {
        for (uint32_t i = 0; i < 4; ++i)
        {
            pState->state.feAttribMask |= pState->state.soState.streamMasks[i];
        }
    }

    // complicated logic to test for cases where we don't need backing hottile memory for a draw
    // have to check for the special case where depth/stencil test is enabled but depthwrite is disabled.
    pState->state.depthHottileEnable = ((!(pState->state.depthStencilState.depthTestEnable &&
                                           !pState->state.depthStencilState.depthWriteEnable &&
                                           pState->state.depthStencilState.depthTestFunc == ZFUNC_ALWAYS)) && 
                                        (pState->state.depthStencilState.depthTestEnable || 
                                         pState->state.depthStencilState.depthWriteEnable)) ? true : false;

    pState->state.stencilHottileEnable = (((!(pState->state.depthStencilState.stencilTestEnable &&
                                             !pState->state.depthStencilState.stencilWriteEnable &&
                                              pState->state.depthStencilState.stencilTestFunc == ZFUNC_ALWAYS)) ||
                                          // for stencil we have to check the double sided state as well
                                          (!(pState->state.depthStencilState.doubleSidedStencilTestEnable &&
                                             !pState->state.depthStencilState.stencilWriteEnable &&
                                              pState->state.depthStencilState.backfaceStencilTestFunc == ZFUNC_ALWAYS))) && 
                                          (pState->state.depthStencilState.stencilTestEnable  ||
                                           pState->state.depthStencilState.stencilWriteEnable)) ? true : false;

    uint32_t numRTs = pState->state.psState.numRenderTargets;
    pState->state.colorHottileEnable = 0;
    if(pState->state.psState.pfnPixelShader != nullptr)
    {
        for (uint32_t rt = 0; rt < numRTs; ++rt)
        {
            pState->state.colorHottileEnable |=  
                (!pState->state.blendState.renderTarget[rt].writeDisableAlpha ||
                 !pState->state.blendState.renderTarget[rt].writeDisableRed ||
                 !pState->state.blendState.renderTarget[rt].writeDisableGreen ||
                 !pState->state.blendState.renderTarget[rt].writeDisableBlue) ? (1 << rt) : 0;
        }
    }
}

//////////////////////////////////////////////////////////////////////////
/// @brief InitDraw
/// @param pDC - Draw context to initialize for this draw.
void InitDraw(
    DRAW_CONTEXT *pDC,
    bool isSplitDraw)
{
    // We don't need to re-setup the scissors/pipeline state again for split draw.
    if (isSplitDraw == false)
    {
        SetupMacroTileScissors(pDC);
        SetupPipeline(pDC);
    }
}

//////////////////////////////////////////////////////////////////////////
/// @brief We can split the draw for certain topologies for better performance.
/// @param totalVerts - Total vertices for draw
/// @param topology - Topology used for draw
uint32_t MaxVertsPerDraw(
    DRAW_CONTEXT* pDC,
    uint32_t totalVerts,
    PRIMITIVE_TOPOLOGY topology)
{
    API_STATE& state = pDC->pState->state;

    uint32_t vertsPerDraw = totalVerts;

    if (state.soState.soEnable)
    {
        return totalVerts;
    }

    switch (topology)
    {
    case TOP_POINT_LIST:
    case TOP_TRIANGLE_LIST:
        vertsPerDraw = KNOB_MAX_PRIMS_PER_DRAW;
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
        if (pDC->pState->state.tsState.tsEnable)
        {
            uint32_t vertsPerPrim = topology - TOP_PATCHLIST_BASE;
            vertsPerDraw = vertsPerPrim * KNOB_MAX_TESS_PRIMS_PER_DRAW;
        }
        break;

    default:
        // We are not splitting up draws for other topologies.
        break;
    }

    return vertsPerDraw;
}

// Recursive template used to auto-nest conditionals.  Converts dynamic boolean function
// arguments to static template arguments.
template <bool... ArgsB>
struct FEDrawChooser
{
    // Last Arg Terminator
    static PFN_FE_WORK_FUNC GetFunc(bool bArg)
    {
        if (bArg)
        {
            return ProcessDraw<ArgsB..., true>;
        }

        return ProcessDraw<ArgsB..., false>;
    }

    // Recursively parse args
    template <typename... TArgsT>
    static PFN_FE_WORK_FUNC GetFunc(bool bArg, TArgsT... remainingArgs)
    {
        if (bArg)
        {
            return FEDrawChooser<ArgsB..., true>::GetFunc(remainingArgs...);
        }

        return FEDrawChooser<ArgsB..., false>::GetFunc(remainingArgs...);
    }
};

// Selector for correct templated Draw front-end function
INLINE
static PFN_FE_WORK_FUNC GetFEDrawFunc(bool IsIndexed, bool HasTessellation, bool HasGeometryShader, bool HasStreamOut, bool RasterizerEnabled)
{
    return FEDrawChooser<>::GetFunc(IsIndexed, HasTessellation, HasGeometryShader, HasStreamOut, RasterizerEnabled);
}


//////////////////////////////////////////////////////////////////////////
/// @brief DrawInstanced
/// @param hContext - Handle passed back from SwrCreateContext
/// @param topology - Specifies topology for draw.
/// @param numVerts - How many vertices to read sequentially from vertex data (per instance).
/// @param startVertex - Specifies start vertex for draw. (vertex data)
/// @param numInstances - How many instances to render.
/// @param startInstance - Which instance to start sequentially fetching from in each buffer (instanced data)
void DrawInstanced(
    HANDLE hContext,
    PRIMITIVE_TOPOLOGY topology,
    uint32_t numVertices,
    uint32_t startVertex,
    uint32_t numInstances = 1,
    uint32_t startInstance = 0)
{
    if (KNOB_TOSS_DRAW)
    {
        return;
    }

    RDTSC_START(APIDraw);

    SWR_CONTEXT *pContext = GetContext(hContext);
    DRAW_CONTEXT* pDC = GetDrawContext(pContext);

    int32_t maxVertsPerDraw = MaxVertsPerDraw(pDC, numVertices, topology);
    uint32_t primsPerDraw = GetNumPrims(topology, maxVertsPerDraw);
    int32_t remainingVerts = numVertices;

    API_STATE    *pState = &pDC->pState->state;
    pState->topology = topology;
    pState->forceFront = false;

    // disable culling for points/lines
    uint32_t oldCullMode = pState->rastState.cullMode;
    if (topology == TOP_POINT_LIST)
    {
        pState->rastState.cullMode = SWR_CULLMODE_NONE;
        pState->forceFront = true;
    }

    int draw = 0;
    while (remainingVerts)
    {
        uint32_t numVertsForDraw = (remainingVerts < maxVertsPerDraw) ?
        remainingVerts : maxVertsPerDraw;

        bool isSplitDraw = (draw > 0) ? true : false;
        DRAW_CONTEXT* pDC = GetDrawContext(pContext, isSplitDraw);
        InitDraw(pDC, isSplitDraw);

        pDC->FeWork.type = DRAW;
        pDC->FeWork.pfnWork = GetFEDrawFunc(
            false,  // IsIndexed
            pState->tsState.tsEnable,
            pState->gsState.gsEnable,
            pState->soState.soEnable,
            pDC->pState->pfnProcessPrims != nullptr);
        pDC->FeWork.desc.draw.numVerts = numVertsForDraw;
        pDC->FeWork.desc.draw.startVertex = startVertex;
        pDC->FeWork.desc.draw.numInstances = numInstances;
        pDC->FeWork.desc.draw.startInstance = startInstance;
        pDC->FeWork.desc.draw.startPrimID = draw * primsPerDraw;
        pDC->FeWork.desc.draw.startVertexID = draw * maxVertsPerDraw;

        //enqueue DC
        QueueDraw(pContext);

        remainingVerts -= numVertsForDraw;
        draw++;
    }

    // restore culling state
    pDC = GetDrawContext(pContext);
    pDC->pState->state.rastState.cullMode = oldCullMode;

    RDTSC_STOP(APIDraw, numVertices * numInstances, 0);
}

//////////////////////////////////////////////////////////////////////////
/// @brief SwrDraw
/// @param hContext - Handle passed back from SwrCreateContext
/// @param topology - Specifies topology for draw.
/// @param startVertex - Specifies start vertex in vertex buffer for draw.
/// @param primCount - Number of vertices.
void SwrDraw(
    HANDLE hContext,
    PRIMITIVE_TOPOLOGY topology,
    uint32_t startVertex,
    uint32_t numVertices)
{
    DrawInstanced(hContext, topology, numVertices, startVertex);
}

//////////////////////////////////////////////////////////////////////////
/// @brief SwrDrawInstanced
/// @param hContext - Handle passed back from SwrCreateContext
/// @param topology - Specifies topology for draw.
/// @param numVertsPerInstance - How many vertices to read sequentially from vertex data.
/// @param numInstances - How many instances to render.
/// @param startVertex - Specifies start vertex for draw. (vertex data)
/// @param startInstance - Which instance to start sequentially fetching from in each buffer (instanced data)
void SwrDrawInstanced(
    HANDLE hContext,
    PRIMITIVE_TOPOLOGY topology,
    uint32_t numVertsPerInstance,
    uint32_t numInstances,
    uint32_t startVertex,
    uint32_t startInstance
    )
{
    DrawInstanced(hContext, topology, numVertsPerInstance, startVertex, numInstances, startInstance);
}

//////////////////////////////////////////////////////////////////////////
/// @brief DrawIndexedInstanced
/// @param hContext - Handle passed back from SwrCreateContext
/// @param topology - Specifies topology for draw.
/// @param numIndices - Number of indices to read sequentially from index buffer.
/// @param indexOffset - Starting index into index buffer.
/// @param baseVertex - Vertex in vertex buffer to consider as index "0". Note value is signed.
/// @param numInstances - Number of instances to render.
/// @param startInstance - Which instance to start sequentially fetching from in each buffer (instanced data)
void DrawIndexedInstance(
    HANDLE hContext,
    PRIMITIVE_TOPOLOGY topology,
    uint32_t numIndices,
    uint32_t indexOffset,
    int32_t baseVertex,
    uint32_t numInstances = 1,
    uint32_t startInstance = 0)
{
    if (KNOB_TOSS_DRAW)
    {
        return;
    }

    RDTSC_START(APIDrawIndexed);

    SWR_CONTEXT *pContext = GetContext(hContext);
    DRAW_CONTEXT* pDC = GetDrawContext(pContext);
    API_STATE* pState = &pDC->pState->state;

    int32_t maxIndicesPerDraw = MaxVertsPerDraw(pDC, numIndices, topology);
    uint32_t primsPerDraw = GetNumPrims(topology, maxIndicesPerDraw);
    int32_t remainingIndices = numIndices;

    uint32_t indexSize = 0;
    switch (pState->indexBuffer.format)
    {
    case R32_UINT: indexSize = sizeof(uint32_t); break;
    case R16_UINT: indexSize = sizeof(uint16_t); break;
    case R8_UINT: indexSize = sizeof(uint8_t); break;
    default:
        SWR_ASSERT(0);
    }

    int draw = 0;
    uint8_t *pIB = (uint8_t*)pState->indexBuffer.pIndices;
    pIB += (uint64_t)indexOffset * (uint64_t)indexSize;

    pState->topology = topology;
    pState->forceFront = false;

    // disable culling for points/lines
    uint32_t oldCullMode = pState->rastState.cullMode;
    if (topology == TOP_POINT_LIST)
    {
        pState->rastState.cullMode = SWR_CULLMODE_NONE;
        pState->forceFront = true;
    }

    while (remainingIndices)
    {
        uint32_t numIndicesForDraw = (remainingIndices < maxIndicesPerDraw) ?
        remainingIndices : maxIndicesPerDraw;

        // When breaking up draw, we need to obtain new draw context for each iteration.
        bool isSplitDraw = (draw > 0) ? true : false;
        pDC = GetDrawContext(pContext, isSplitDraw);
        InitDraw(pDC, isSplitDraw);

        pDC->FeWork.type = DRAW;
        pDC->FeWork.pfnWork = GetFEDrawFunc(
            true,   // IsIndexed
            pState->tsState.tsEnable,
            pState->gsState.gsEnable,
            pState->soState.soEnable,
            pDC->pState->pfnProcessPrims != nullptr);
        pDC->FeWork.desc.draw.pDC = pDC;
        pDC->FeWork.desc.draw.numIndices = numIndicesForDraw;
        pDC->FeWork.desc.draw.pIB = (int*)pIB;
        pDC->FeWork.desc.draw.type = pDC->pState->state.indexBuffer.format;

        pDC->FeWork.desc.draw.numInstances = numInstances;
        pDC->FeWork.desc.draw.startInstance = startInstance;
        pDC->FeWork.desc.draw.baseVertex = baseVertex;
        pDC->FeWork.desc.draw.startPrimID = draw * primsPerDraw;

        //enqueue DC
        QueueDraw(pContext);

        pIB += maxIndicesPerDraw * indexSize;
        remainingIndices -= numIndicesForDraw;
        draw++;
    }

    // restore culling state
    pDC = GetDrawContext(pContext);
    pDC->pState->state.rastState.cullMode = oldCullMode;

    RDTSC_STOP(APIDrawIndexed, numIndices * numInstances, 0);
}


//////////////////////////////////////////////////////////////////////////
/// @brief DrawIndexed
/// @param hContext - Handle passed back from SwrCreateContext
/// @param topology - Specifies topology for draw.
/// @param numIndices - Number of indices to read sequentially from index buffer.
/// @param indexOffset - Starting index into index buffer.
/// @param baseVertex - Vertex in vertex buffer to consider as index "0". Note value is signed.
void SwrDrawIndexed(
    HANDLE hContext,
    PRIMITIVE_TOPOLOGY topology,
    uint32_t numIndices,
    uint32_t indexOffset,
    int32_t baseVertex
    )
{
    DrawIndexedInstance(hContext, topology, numIndices, indexOffset, baseVertex);
}

//////////////////////////////////////////////////////////////////////////
/// @brief SwrDrawIndexedInstanced
/// @param hContext - Handle passed back from SwrCreateContext
/// @param topology - Specifies topology for draw.
/// @param numIndices - Number of indices to read sequentially from index buffer.
/// @param numInstances - Number of instances to render.
/// @param indexOffset - Starting index into index buffer.
/// @param baseVertex - Vertex in vertex buffer to consider as index "0". Note value is signed.
/// @param startInstance - Which instance to start sequentially fetching from in each buffer (instanced data)
void SwrDrawIndexedInstanced(
    HANDLE hContext,
    PRIMITIVE_TOPOLOGY topology,
    uint32_t numIndices,
    uint32_t numInstances,
    uint32_t indexOffset,
    int32_t baseVertex,
    uint32_t startInstance)
{
    DrawIndexedInstance(hContext, topology, numIndices, indexOffset, baseVertex, numInstances, startInstance);
}

// Attach surfaces to pipeline
void SwrInvalidateTiles(
    HANDLE hContext,
    uint32_t attachmentMask)
{
    SWR_CONTEXT *pContext = (SWR_CONTEXT*)hContext;
    DRAW_CONTEXT* pDC = GetDrawContext(pContext);

    // Queue a load to the hottile
    pDC->FeWork.type = INVALIDATETILES;
    pDC->FeWork.pfnWork = ProcessInvalidateTiles;
    pDC->FeWork.desc.invalidateTiles.attachmentMask = attachmentMask;

    //enqueue
    QueueDraw(pContext);
}

//////////////////////////////////////////////////////////////////////////
/// @brief SwrDispatch
/// @param hContext - Handle passed back from SwrCreateContext
/// @param threadGroupCountX - Number of thread groups dispatched in X direction
/// @param threadGroupCountY - Number of thread groups dispatched in Y direction
/// @param threadGroupCountZ - Number of thread groups dispatched in Z direction
void SwrDispatch(
    HANDLE hContext,
    uint32_t threadGroupCountX,
    uint32_t threadGroupCountY,
    uint32_t threadGroupCountZ)
{
    if (KNOB_TOSS_DRAW)
    {
        return;
    }

    RDTSC_START(APIDispatch);
    SWR_CONTEXT *pContext = (SWR_CONTEXT*)hContext;
    DRAW_CONTEXT* pDC = GetDrawContext(pContext);

    pDC->isCompute = true;      // This is a compute context.

    // Ensure spill fill pointers are initialized to nullptr.
    memset(pDC->pSpillFill, 0, sizeof(pDC->pSpillFill));

    COMPUTE_DESC* pTaskData = (COMPUTE_DESC*)pDC->pArena->AllocAligned(sizeof(COMPUTE_DESC), 64);

    pTaskData->threadGroupCountX = threadGroupCountX;
    pTaskData->threadGroupCountY = threadGroupCountY;
    pTaskData->threadGroupCountZ = threadGroupCountZ;

    uint32_t totalThreadGroups = threadGroupCountX * threadGroupCountY * threadGroupCountZ;
    pDC->pDispatch->initialize(totalThreadGroups, pTaskData);

    QueueDispatch(pContext);
    RDTSC_STOP(APIDispatch, threadGroupCountX * threadGroupCountY * threadGroupCountZ, 0);
}

// Deswizzles, converts and stores current contents of the hot tiles to surface
// described by pState
void SwrStoreTiles(
    HANDLE hContext,
    SWR_RENDERTARGET_ATTACHMENT attachment,
    SWR_TILE_STATE postStoreTileState)
{
    RDTSC_START(APIStoreTiles);

    SWR_CONTEXT *pContext = (SWR_CONTEXT*)hContext;
    DRAW_CONTEXT* pDC = GetDrawContext(pContext);

    SetupMacroTileScissors(pDC);

    pDC->FeWork.type = STORETILES;
    pDC->FeWork.pfnWork = ProcessStoreTiles;
    pDC->FeWork.desc.storeTiles.attachment = attachment;
    pDC->FeWork.desc.storeTiles.postStoreTileState = postStoreTileState;

    //enqueue
    QueueDraw(pContext);

    RDTSC_STOP(APIStoreTiles, 0, 0);
}

void SwrClearRenderTarget(
    HANDLE hContext,
    uint32_t clearMask,
    const float clearColor[4],
    float z,
    BYTE stencil)
{
    RDTSC_START(APIClearRenderTarget);

    SWR_CONTEXT *pContext = (SWR_CONTEXT*)hContext;

    DRAW_CONTEXT* pDC = GetDrawContext(pContext);

    SetupMacroTileScissors(pDC);

    CLEAR_FLAGS flags;
    flags.mask = clearMask;

    pDC->FeWork.type = CLEAR;
    pDC->FeWork.pfnWork = ProcessClear;
    pDC->FeWork.desc.clear.flags = flags;
    pDC->FeWork.desc.clear.clearDepth = z;
    pDC->FeWork.desc.clear.clearRTColor[0] = clearColor[0];
    pDC->FeWork.desc.clear.clearRTColor[1] = clearColor[1];
    pDC->FeWork.desc.clear.clearRTColor[2] = clearColor[2];
    pDC->FeWork.desc.clear.clearRTColor[3] = clearColor[3];
    pDC->FeWork.desc.clear.clearStencil = stencil;

    // enqueue draw
    QueueDraw(pContext);

    RDTSC_STOP(APIClearRenderTarget, 0, pDC->drawId);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Returns a pointer to the private context state for the current
///        draw operation. This is used for external componets such as the
///        sampler.
///        SWR is responsible for the allocation of the private context state.
/// @param hContext - Handle passed back from SwrCreateContext
VOID* SwrGetPrivateContextState(
    HANDLE hContext)
{
    SWR_CONTEXT* pContext = GetContext(hContext);
    DRAW_CONTEXT* pDC = GetDrawContext(pContext);
    DRAW_STATE* pState = pDC->pState;

    if (pState->pPrivateState == nullptr)
    {
        pState->pPrivateState = pState->pArena->AllocAligned(pContext->privateStateSize, KNOB_SIMD_WIDTH*sizeof(float));
    }

    return pState->pPrivateState;
}

//////////////////////////////////////////////////////////////////////////
/// @brief Clients can use this to allocate memory for draw/dispatch
///        operations. The memory will automatically be freed once operation
///        has completed. Client can use this to allocate binding tables,
///        etc. needed for shader execution.
/// @param hContext - Handle passed back from SwrCreateContext
/// @param size - Size of allocation
/// @param align - Alignment needed for allocation.
VOID* SwrAllocDrawContextMemory(
    HANDLE hContext,
    uint32_t size,
    uint32_t align)
{
    SWR_CONTEXT* pContext = GetContext(hContext);
    DRAW_CONTEXT* pDC = GetDrawContext(pContext);

    return pDC->pState->pArena->AllocAligned(size, align);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Returns pointer to SWR stats.
/// @note The counters are atomically incremented by multiple threads.
///       When calling this, you need to ensure all previous operations
///       have completed.
/// @todo If necessary, add a callback to avoid stalling the pipe to
///       sample the counters.
/// @param hContext - Handle passed back from SwrCreateContext
/// @param pStats - SWR will fill this out for caller.
void SwrGetStats(
    HANDLE hContext,
    SWR_STATS* pStats)
{
    SWR_CONTEXT *pContext = GetContext(hContext);
    DRAW_CONTEXT* pDC = GetDrawContext(pContext);

    pDC->FeWork.type = QUERYSTATS;
    pDC->FeWork.pfnWork = ProcessQueryStats;
    pDC->FeWork.desc.queryStats.pStats = pStats;

    // cannot execute until all previous draws have completed
    pDC->dependency = pDC->drawId - 1;

    //enqueue
    QueueDraw(pContext);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Enables stats counting
/// @param hContext - Handle passed back from SwrCreateContext
/// @param enable - If true then counts are incremented.
void SwrEnableStats(
    HANDLE hContext,
    bool enable)
{
    SWR_CONTEXT *pContext = GetContext(hContext);
    DRAW_CONTEXT* pDC = GetDrawContext(pContext);

    pDC->pState->state.enableStats = enable;
}

//////////////////////////////////////////////////////////////////////////
/// @brief Mark end of frame - used for performance profiling
/// @param hContext - Handle passed back from SwrCreateContext
void SWR_API SwrEndFrame(
    HANDLE hContext)
{
    RDTSC_ENDFRAME();
}
