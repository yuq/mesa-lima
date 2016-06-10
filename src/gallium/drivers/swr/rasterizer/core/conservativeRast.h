/****************************************************************************
* Copyright (C) 2014-2016 Intel Corporation.   All Rights Reserved.
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
* @file conservativerast.h
*
******************************************************************************/
#pragma once
#include <type_traits>
#include "common/simdintrin.h"

enum FixedPointFmt
{
    FP_UNINIT,
    _16_8,
    _16_9
};

//////////////////////////////////////////////////////////////////////////
/// @brief convenience typedefs for supported Fixed Point precisions
typedef std::integral_constant<uint32_t, FP_UNINIT> Fixed_Uninit;
typedef std::integral_constant<uint32_t, _16_8> Fixed_16_8;
typedef std::integral_constant<uint32_t, _16_9> Fixed_16_9;

//////////////////////////////////////////////////////////////////////////
/// @struct FixedPointTraits
/// @brief holds constants relating to converting between FP and Fixed point 
/// @tparam FT: fixed precision type
template<typename FT>
struct FixedPointTraits{};

//////////////////////////////////////////////////////////////////////////
/// @brief Fixed_16_8 specialization of FixedPointTraits
template<>
struct FixedPointTraits<Fixed_16_8>
{
    /// multiplier to go from FP32 to Fixed Point 16.8
    typedef std::integral_constant<uint32_t, 256> FixedPointScaleT;
    /// number of bits to shift to go from 16.8 fixed => int32
    typedef std::integral_constant<uint32_t, 8> FixedPointShiftT;
    typedef Fixed_16_8 TypeT;
};

//////////////////////////////////////////////////////////////////////////
/// @brief Fixed_16_9 specialization of FixedPointTraits
template<>
struct FixedPointTraits<Fixed_16_9>
{
    /// multiplier to go from FP32 to Fixed Point 16.9
    typedef std::integral_constant<uint32_t, 512> FixedPointScaleT;
    /// number of bits to shift to go from 16.9 fixed => int32
    typedef std::integral_constant<uint32_t, 9> FixedPointShiftT;
    typedef Fixed_16_9 TypeT;
};

//////////////////////////////////////////////////////////////////////////
/// @brief convenience typedefs for conservative rasterization modes  
typedef std::false_type StandardRastT;
typedef std::true_type ConservativeRastT;

//////////////////////////////////////////////////////////////////////////
/// @brief convenience typedefs for Input Coverage rasterization modes  
typedef std::integral_constant<uint32_t, SWR_INPUT_COVERAGE_NONE> NoInputCoverageT;
typedef std::integral_constant<uint32_t, SWR_INPUT_COVERAGE_NORMAL> OuterConservativeCoverageT;
typedef std::integral_constant<uint32_t, SWR_INPUT_COVERAGE_INNER_CONSERVATIVE> InnerConservativeCoverageT;

//////////////////////////////////////////////////////////////////////////
/// @struct ConservativeRastTraits
/// @brief primary ConservativeRastTraits template. Shouldn't be instantiated
/// @tparam ConservativeT: type of conservative rasterization
template <typename ConservativeT>
struct ConservativeRastFETraits {};

//////////////////////////////////////////////////////////////////////////
/// @brief StandardRast specialization of ConservativeRastTraits
template <>
struct ConservativeRastFETraits<StandardRastT>
{
    typedef std::false_type IsConservativeT;
    typedef FixedPointTraits<Fixed_16_8> BBoxPrecisionT;
    typedef FixedPointTraits<Fixed_16_8> ZeroAreaPrecisionT;
};

//////////////////////////////////////////////////////////////////////////
/// @brief ConservativeRastT specialization of ConservativeRastTraits
template <>
struct ConservativeRastFETraits<ConservativeRastT>
{
    typedef std::true_type IsConservativeT;
    typedef FixedPointTraits<Fixed_16_8> ZeroAreaPrecisionT;

    /// Conservative bounding box needs to expand the area around each vertex by 1/512, which 
    /// is the potential snapping error when going from FP-> 16.8 fixed
    typedef FixedPointTraits<Fixed_16_9> BBoxPrecisionT;
    typedef std::integral_constant<uint32_t, 1> BoundingBoxOffsetT;
    typedef std::integral_constant<uint32_t, 1> BoundingBoxShiftT;
};

//////////////////////////////////////////////////////////////////////////
/// @brief convenience typedefs for ConservativeRastFETraits 
typedef ConservativeRastFETraits<StandardRastT> FEStandardRastT;
typedef ConservativeRastFETraits<ConservativeRastT> FEConservativeRastT;
