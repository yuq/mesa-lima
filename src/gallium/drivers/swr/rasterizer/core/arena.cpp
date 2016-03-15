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
* @file arena.cpp
*
* @brief Arena memory manager
*        The arena is convenient and fast for managing allocations for any of
*        our allocations that are associated with operations and can all be freed
*        once when their operation has completed. Allocations are cheap since
*        most of the time its simply an increment of an offset. Also, no need to
*        free individual allocations. All of the arena memory can be freed at once.
*
******************************************************************************/

#include "context.h"
#include "arena.h"

#include <cmath>

Arena::Arena()
    : m_pCurBlock(nullptr), m_size(0)
{
    m_pMutex = new std::mutex();
}

Arena::~Arena()
{
    Reset();        // Reset just in case to avoid leaking memory.

    if (m_pCurBlock)
    {
        _aligned_free(m_pCurBlock->pMem);
        delete m_pCurBlock;
    }

    delete m_pMutex;
}

///@todo Remove this when all users have stopped using this.
void Arena::Init()
{
    m_size = 0;
    m_pCurBlock = nullptr;

    m_pMutex = new std::mutex();
}

void* Arena::AllocAligned(size_t size, size_t align)
{
    if (m_pCurBlock)
    {
        ArenaBlock* pCurBlock = m_pCurBlock;
        pCurBlock->offset = AlignUp(pCurBlock->offset, align);

        if ((pCurBlock->offset + size) <= pCurBlock->blockSize)
        {
            void* pMem = PtrAdd(pCurBlock->pMem, pCurBlock->offset);
            pCurBlock->offset += size;
            m_size += size;
            return pMem;
        }

        // Not enough memory in this block, fall through to allocate
        // a new block
    }

    static const size_t ArenaBlockSize = 1024*1024;
    size_t blockSize = std::max(m_size + ArenaBlockSize, std::max(size, ArenaBlockSize));
    blockSize = AlignUp(blockSize, KNOB_SIMD_WIDTH*4);

    void *pMem = _aligned_malloc(blockSize, KNOB_SIMD_WIDTH*4);    // Arena blocks are always simd byte aligned.
    SWR_ASSERT(pMem != nullptr);

    ArenaBlock* pNewBlock = new (std::nothrow) ArenaBlock();
    SWR_ASSERT(pNewBlock != nullptr);

    if (pNewBlock != nullptr)
    {
        pNewBlock->pNext        = m_pCurBlock;

        m_pCurBlock             = pNewBlock;
        m_pCurBlock->pMem       = pMem;
        m_pCurBlock->blockSize  = blockSize;

    }

    return AllocAligned(size, align);
}

void* Arena::Alloc(size_t size)
{
    return AllocAligned(size, 1);
}

void* Arena::AllocAlignedSync(size_t size, size_t align)
{
    void* pAlloc = nullptr;

    SWR_ASSERT(m_pMutex != nullptr);

    m_pMutex->lock();
    pAlloc = AllocAligned(size, align);
    m_pMutex->unlock();

    return pAlloc;
}

void* Arena::AllocSync(size_t size)
{
    void* pAlloc = nullptr;

    SWR_ASSERT(m_pMutex != nullptr);

    m_pMutex->lock();
    pAlloc = Alloc(size);
    m_pMutex->unlock();

    return pAlloc;
}

void Arena::Reset(bool removeAll)
{
    if (m_pCurBlock)
    {
        m_pCurBlock->offset = 0;

        ArenaBlock *pUsedBlocks = m_pCurBlock->pNext;
        m_pCurBlock->pNext = nullptr;
        while(pUsedBlocks)
        {
            ArenaBlock* pBlock = pUsedBlocks;
            pUsedBlocks = pBlock->pNext;

            _aligned_free(pBlock->pMem);
            delete pBlock;
        }

        if (removeAll)
        {
            _aligned_free(m_pCurBlock->pMem);
            delete m_pCurBlock;
            m_pCurBlock = nullptr;
        }
    }

    m_size = 0;
}
