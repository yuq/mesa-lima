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
* @file rasterizer.h
*
* @brief Definitions for the rasterizer.
*
******************************************************************************/
#pragma once

#include "context.h"
#include <type_traits>
#include "conservativeRast.h"
#include "multisample.h"

void RasterizeLine(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pData);
void RasterizeSimplePoint(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pData);
void RasterizeTriPoint(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pData);

INLINE
__m128i fpToFixedPoint(const __m128 vIn)
{
    __m128 vFixed = _mm_mul_ps(vIn, _mm_set1_ps(FIXED_POINT_SCALE));
    return _mm_cvtps_epi32(vFixed);
}

// Selector for correct templated RasterizeTriangle function
PFN_WORK_FUNC GetRasterizerFunc(
    uint32_t numSamples,
    bool IsConservative,
    uint32_t InputCoverage,
    bool RasterizeScissorEdges);

//////////////////////////////////////////////////////////////////////////
/// @struct RasterScissorEdgesT
/// @brief Primary RasterScissorEdgesT templated struct that holds compile 
/// time information about the number of edges needed to be rasterized, 
/// If either the scissor rect or conservative rast is enabled, 
/// the scissor test is enabled and the rasterizer will test
/// 3 triangle edges + 4 scissor edges for coverage.
/// @tparam RasterScissorEdgesT: number of multisamples
/// @tparam ConservativeT: is this a conservative rasterization
template <typename RasterScissorEdgesT, typename ConservativeT>
struct RasterEdgeTraits
{
    typedef std::true_type RasterizeScissorEdgesT;
    typedef std::integral_constant<uint32_t, 7> NumEdgesT;
};

//////////////////////////////////////////////////////////////////////////
/// @brief specialization of RasterEdgeTraits. If neither scissor rect
/// nor conservative rast is enabled, only test 3 triangle edges 
/// for coverage
template <>
struct RasterEdgeTraits<std::false_type, std::false_type>
{
    typedef std::false_type RasterizeScissorEdgesT;
    typedef std::integral_constant<uint32_t, 3> NumEdgesT;
};

//////////////////////////////////////////////////////////////////////////
/// @struct RasterizerTraits
/// @brief templated struct that holds compile time information used 
/// during rasterization. Inherits EdgeTraits and ConservativeRastBETraits.
/// @tparam NumSamplesT: number of multisamples
/// @tparam ConservativeT: is this a conservative rasterization
/// @tparam InputCoverageT: what type of input coverage is the PS expecting?
/// (only used with conservative rasterization)
/// @tparam RasterScissorEdgesT: do we need to rasterize with a scissor?
template <typename NumSamplesT, typename ConservativeT, typename InputCoverageT, typename RasterScissorEdgesT>
struct RasterizerTraits final : public ConservativeRastBETraits<ConservativeT, InputCoverageT>,
                                public RasterEdgeTraits<RasterScissorEdgesT, ConservativeT>
{
    typedef MultisampleTraits<static_cast<SWR_MULTISAMPLE_COUNT>(NumSamplesT::value)> MT;
    
    /// Fixed point precision the rasterizer is using
    typedef FixedPointTraits<Fixed_16_8> PrecisionT;
    /// Fixed point precision of the edge tests used during rasterization
    typedef FixedPointTraits<Fixed_X_16> EdgePrecisionT;

    static_assert(EdgePrecisionT::BitsT::value >=  ConservativeRastBETraits<ConservativeT, InputCoverageT>::ConservativePrecisionT::BitsT::value,
                  "Rasterizer edge fixed point precision < required conservative rast precision");

    /// constants used to offset between different types of raster tiles
    static const int colorRasterTileStep{(KNOB_TILE_X_DIM * KNOB_TILE_Y_DIM * (FormatTraits<KNOB_COLOR_HOT_TILE_FORMAT>::bpp / 8)) * MT::numSamples};
    static const int depthRasterTileStep{(KNOB_TILE_X_DIM * KNOB_TILE_Y_DIM * (FormatTraits<KNOB_DEPTH_HOT_TILE_FORMAT>::bpp / 8)) * MT::numSamples};
    static const int stencilRasterTileStep{(KNOB_TILE_X_DIM * KNOB_TILE_Y_DIM * (FormatTraits<KNOB_STENCIL_HOT_TILE_FORMAT>::bpp / 8)) * MT::numSamples};
    static const int colorRasterTileRowStep{(KNOB_MACROTILE_X_DIM / KNOB_TILE_X_DIM) * colorRasterTileStep};
    static const int depthRasterTileRowStep{(KNOB_MACROTILE_X_DIM / KNOB_TILE_X_DIM)* depthRasterTileStep};
    static const int stencilRasterTileRowStep{(KNOB_MACROTILE_X_DIM / KNOB_TILE_X_DIM) * stencilRasterTileStep};
};
