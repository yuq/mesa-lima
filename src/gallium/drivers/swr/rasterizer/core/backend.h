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

void ProcessComputeBE(DRAW_CONTEXT* pDC, uint32_t workerId, uint32_t threadGroupId);
void ProcessSyncBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pUserData);
void ProcessQueryStatsBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pUserData);
void ProcessClearBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pUserData);
void ProcessStoreTileBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pData);
void ProcessInvalidateTilesBE(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pData);
void BackendNullPS(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t x, uint32_t y, SWR_TRIANGLE_DESC &work, RenderOutputBuffers &renderBuffers);
void InitClearTilesTable();

enum SWR_BACKEND_FUNCS
{
    SWR_BACKEND_SINGLE_SAMPLE,
    SWR_BACKEND_MSAA_PIXEL_RATE,
    SWR_BACKEND_MSAA_SAMPLE_RATE,
    SWR_BACKEND_FUNCS_MAX,
};
void InitBackendFuncTables();

extern PFN_BACKEND_FUNC gBackendNullPs[SWR_MULTISAMPLE_TYPE_MAX];
extern PFN_BACKEND_FUNC gBackendSingleSample[2][2];
extern PFN_BACKEND_FUNC gBackendPixelRateTable[SWR_MULTISAMPLE_TYPE_MAX][SWR_MSAA_SAMPLE_PATTERN_MAX][SWR_INPUT_COVERAGE_MAX][2][2];
extern PFN_BACKEND_FUNC gBackendSampleRateTable[SWR_MULTISAMPLE_TYPE_MAX][SWR_INPUT_COVERAGE_MAX][2];
extern PFN_OUTPUT_MERGER gBackendOutputMergerTable[SWR_NUM_RENDERTARGETS+1][SWR_MULTISAMPLE_TYPE_MAX];
extern PFN_CALC_PIXEL_BARYCENTRICS gPixelBarycentricTable[2];
extern PFN_CALC_SAMPLE_BARYCENTRICS gSampleBarycentricTable[2];
extern PFN_CALC_CENTROID_BARYCENTRICS gCentroidBarycentricTable[SWR_MULTISAMPLE_TYPE_MAX][2][2][2];
