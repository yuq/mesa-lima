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

#ifndef __SWR_ASSERT_H__
#define __SWR_ASSERT_H__

#if !defined(__SWR_OS_H__)
#error swr_assert.h should not be included directly, please include "common/os.h" instead.
#endif

#if !defined(SWR_ENABLE_ASSERTS)

#if !defined(NDEBUG)
#define SWR_ENABLE_ASSERTS 1
#else
#define SWR_ENABLE_ASSERTS 0
#endif // _DEBUG

#endif // SWR_ENABLE_ASSERTS

#if !defined(SWR_ENABLE_REL_ASSERTS)
#define SWR_ENABLE_REL_ASSERTS 1
#endif

#if SWR_ENABLE_ASSERTS || SWR_ENABLE_REL_ASSERTS
#include "assert.h"

#if !defined(__cplusplus)

#pragma message("C++ is required for SWR Asserts, falling back to assert.h")

#if SWR_ENABLE_ASSERTS
#define SWR_ASSERT(e, ...) assert(e)
#endif

#if SWR_ENABLE_REL_ASSERTS
#define SWR_REL_ASSERT(e, ...) assert(e)
#endif

#else

bool SwrAssert(
    bool        chkDebugger,
    bool&       enabled,
    const char* pExpression,
    const char* pFileName,
    uint32_t    lineNum,
    const char* function,
    const char* pFmtString = nullptr,
    ...);

#define _SWR_ASSERT(chkDebugger, e, ...) {\
    bool expFailed = !(e);\
    if (expFailed) {\
        static bool swrAssertEnabled = true;\
        expFailed = SwrAssert(chkDebugger, swrAssertEnabled, #e, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__);\
        if (expFailed) { DEBUGBREAK; }\
    }\
}

#if SWR_ENABLE_ASSERTS
#define SWR_ASSERT(e, ...) _SWR_ASSERT(true, e, ##__VA_ARGS__)

#if defined(assert)
#undef assert
#endif
#define assert(exp) SWR_ASSERT(exp)

#endif

#if SWR_ENABLE_REL_ASSERTS
#define SWR_REL_ASSERT(e, ...) _SWR_ASSERT(false, e, ##__VA_ARGS__)
#endif
#endif // C++

#endif // SWR_ENABLE_ASSERTS || SWR_ENABLE_REL_ASSERTS

#if !SWR_ENABLE_ASSERTS
#define SWR_ASSERT(e, ...) (void)(0)
#endif

#if !SWR_ENABLE_REL_ASSERTS
#define SWR_REL_ASSERT(e, ...) (void)(0)
#endif

#define SWR_NOT_IMPL SWR_ASSERT(0, "%s not implemented", __FUNCTION__)

#endif//__SWR_ASSERT_H__
