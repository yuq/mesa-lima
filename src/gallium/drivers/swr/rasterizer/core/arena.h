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
* @file arena.h
*
* @brief Arena memory manager
*        The arena is convenient and fast for managing allocations for any of
*        our allocations that are associated with operations and can all be freed
*        once when their operation has completed. Allocations are cheap since
*        most of the time its simply an increment of an offset. Also, no need to
*        free individual allocations. All of the arena memory can be freed at once.
*
******************************************************************************/
#pragma once

#include <mutex>

class Arena
{
public:
    Arena();
   ~Arena();

    void        Init();

    void*       AllocAligned(size_t size, size_t  align);
    void*       Alloc(size_t  size);

    void*       AllocAlignedSync(size_t size, size_t align);
    void*       AllocSync(size_t size);

    void        Reset(bool removeAll = false);
    size_t      Size() { return m_size; }

private:

    struct ArenaBlock
    {
        void*       pMem        = nullptr;
        size_t      blockSize   = 0;
        size_t      offset      = 0;
        ArenaBlock* pNext       = nullptr;
    };

    ArenaBlock*     m_pCurBlock = nullptr;
    size_t          m_size      = 0;

    /// @note Mutex is only used by sync allocation functions.
    std::mutex*     m_pMutex;
};
