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
#include <algorithm>
#include <atomic>
#include "core/utils.h"

class DefaultAllocator
{
public:
    void* AllocateAligned(size_t size, size_t align)
    {
        void* p = _aligned_malloc(size, align);
        return p;
    }
    void  Free(void* pMem)
    {
        _aligned_free(pMem);
    }
};

static const size_t ARENA_BLOCK_ALIGN = 64;

struct ArenaBlock
{
    size_t      blockSize = 0;
    ArenaBlock* pNext = nullptr;
};
static_assert(sizeof(ArenaBlock) <= ARENA_BLOCK_ALIGN,
              "Increase BLOCK_ALIGN size");

// Caching Allocator for Arena
template<uint32_t NumBucketsT = 4, uint32_t StartBucketBitT = 16>
struct CachingAllocatorT : DefaultAllocator
{
    static uint32_t GetBucketId(size_t blockSize)
    {
        uint32_t bucketId = 0;

#if defined(BitScanReverseSizeT)
        BitScanReverseSizeT((unsigned long*)&bucketId, blockSize >> CACHE_START_BUCKET_BIT);
        bucketId = std::min<uint32_t>(bucketId, CACHE_NUM_BUCKETS - 1);
#endif

        return bucketId;
    }

    void* AllocateAligned(size_t size, size_t align)
    {
        SWR_ASSERT(size >= sizeof(ArenaBlock));
        SWR_ASSERT(size <= uint32_t(-1));

        size_t blockSize = size - ARENA_BLOCK_ALIGN;

        {
            // search cached blocks
            std::lock_guard<std::mutex> l(m_mutex);
            ArenaBlock* pPrevBlock = &m_cachedBlocks[GetBucketId(blockSize)];
            ArenaBlock* pBlock = pPrevBlock->pNext;
            ArenaBlock* pPotentialBlock = nullptr;
            ArenaBlock* pPotentialPrev = nullptr;

            while (pBlock)
            {
                if (pBlock->blockSize >= blockSize)
                {
                    if (pBlock == AlignUp(pBlock, align))
                    {
                        if (pBlock->blockSize == blockSize)
                        {
                            // Won't find a better match
                            break;
                        }

                        // We could use this as it is larger than we wanted, but
                        // continue to search for a better match
                        pPotentialBlock = pBlock;
                        pPotentialPrev = pPrevBlock;
                    }
                }
                else
                {
                    // Blocks are sorted by size (biggest first)
                    // So, if we get here, there are no blocks 
                    // large enough, fall through to allocation.
                    pBlock = nullptr;
                    break;
                }

                pPrevBlock = pBlock;
                pBlock = pBlock->pNext;
            }

            if (!pBlock)
            {
                // Couldn't find an exact match, use next biggest size
                pBlock = pPotentialBlock;
                pPrevBlock = pPotentialPrev;
            }

            if (pBlock)
            {
                SWR_ASSERT(pPrevBlock && pPrevBlock->pNext == pBlock);
                pPrevBlock->pNext = pBlock->pNext;
                pBlock->pNext = nullptr;

                return pBlock;
            }

            m_totalAllocated += size;

#if 0
            {
                static uint32_t count = 0;
                char buf[128];
                sprintf_s(buf, "Arena Alloc %d 0x%llx bytes - 0x%llx total\n", ++count, uint64_t(size), uint64_t(m_totalAllocated));
                OutputDebugStringA(buf);
            }
#endif
        }

        return this->DefaultAllocator::AllocateAligned(size, align);
    }

    void  Free(void* pMem)
    {
        if (pMem)
        {
            ArenaBlock* pNewBlock = reinterpret_cast<ArenaBlock*>(pMem);
            SWR_ASSERT(pNewBlock->blockSize >= 0);

            std::unique_lock<std::mutex> l(m_mutex);
            ArenaBlock* pPrevBlock = &m_cachedBlocks[GetBucketId(pNewBlock->blockSize)];
            ArenaBlock* pBlock = pPrevBlock->pNext;

            while (pBlock)
            {
                if (pNewBlock->blockSize >= pBlock->blockSize)
                {
                    // Insert here
                    break;
                }
                pPrevBlock = pBlock;
                pBlock = pBlock->pNext;
            }

            // Insert into list
            SWR_ASSERT(pPrevBlock);
            pPrevBlock->pNext = pNewBlock;
            pNewBlock->pNext = pBlock;
        }
    }

    ~CachingAllocatorT()
    {
        // Free all cached blocks
        for (uint32_t i = 0; i < CACHE_NUM_BUCKETS; ++i)
        {
            ArenaBlock* pBlock = m_cachedBlocks[i].pNext;
            while (pBlock)
            {
                ArenaBlock* pNext = pBlock->pNext;
                this->DefaultAllocator::Free(pBlock);
                pBlock = pNext;
            }
        }
    }

    // buckets, for block sizes < (1 << (start+1)), < (1 << (start+2)), ...
    static const uint32_t   CACHE_NUM_BUCKETS       = NumBucketsT;
    static const uint32_t   CACHE_START_BUCKET_BIT  = StartBucketBitT;

    ArenaBlock              m_cachedBlocks[CACHE_NUM_BUCKETS];
    std::mutex              m_mutex;

    size_t                  m_totalAllocated = 0;
};
typedef CachingAllocatorT<> CachingAllocator;

template<typename T = DefaultAllocator, size_t BlockSizeT = 128 * sizeof(KILOBYTE)>
class TArena
{
public:
    TArena(T& in_allocator)  : m_allocator(in_allocator) {}
    TArena()                 : m_allocator(m_defAllocator) {}
    ~TArena()
    {
        Reset(true);
    }

    void* AllocAligned(size_t size, size_t  align)
    {
        if (0 == size)
        {
            return nullptr;
        }

        SWR_ASSERT(align <= ARENA_BLOCK_ALIGN);

        if (m_pCurBlock)
        {
            ArenaBlock* pCurBlock = m_pCurBlock;
            size_t offset = AlignUp(m_offset, align);

            if ((offset + size) <= pCurBlock->blockSize)
            {
                void* pMem = PtrAdd(pCurBlock, offset + ARENA_BLOCK_ALIGN);
                m_offset = offset + size;
                return pMem;
            }

            // Not enough memory in this block, fall through to allocate
            // a new block
        }

        static const size_t ArenaBlockSize = BlockSizeT - ARENA_BLOCK_ALIGN;
        size_t blockSize = std::max(size, ArenaBlockSize);

        // Add in one BLOCK_ALIGN unit to store ArenaBlock in.
        blockSize = AlignUp(blockSize, ARENA_BLOCK_ALIGN);

        void *pMem = m_allocator.AllocateAligned(blockSize + ARENA_BLOCK_ALIGN, ARENA_BLOCK_ALIGN);    // Arena blocks are always simd byte aligned.
        SWR_ASSERT(pMem != nullptr);

        ArenaBlock* pNewBlock = new (pMem) ArenaBlock();

        if (pNewBlock != nullptr)
        {
            m_offset = 0;
            pNewBlock->pNext = m_pCurBlock;

            m_pCurBlock = pNewBlock;
            m_pCurBlock->blockSize = blockSize;
        }

        return AllocAligned(size, align);
    }

    void* Alloc(size_t  size)
    {
        return AllocAligned(size, 1);
    }

    void* AllocAlignedSync(size_t size, size_t align)
    {
        void* pAlloc = nullptr;

        m_mutex.lock();
        pAlloc = AllocAligned(size, align);
        m_mutex.unlock();

        return pAlloc;
    }

    void* AllocSync(size_t size)
    {
        void* pAlloc = nullptr;

        m_mutex.lock();
        pAlloc = Alloc(size);
        m_mutex.unlock();

        return pAlloc;
    }

    void Reset(bool removeAll = false)
    {
        m_offset = 0;

        if (m_pCurBlock)
        {
            ArenaBlock *pUsedBlocks = m_pCurBlock->pNext;
            m_pCurBlock->pNext = nullptr;
            while (pUsedBlocks)
            {
                ArenaBlock* pBlock = pUsedBlocks;
                pUsedBlocks = pBlock->pNext;

                m_allocator.Free(pBlock);
            }

            if (removeAll)
            {
                m_allocator.Free(m_pCurBlock);
                m_pCurBlock = nullptr;
            }
        }
    }

    bool IsEmpty()
    {
        return (m_pCurBlock == nullptr) || (m_offset == 0 && m_pCurBlock->pNext == nullptr);
    }

private:

    ArenaBlock*         m_pCurBlock = nullptr;
    size_t              m_offset    = 0;

    /// @note Mutex is only used by sync allocation functions.
    std::mutex          m_mutex;

    DefaultAllocator    m_defAllocator;
    T&                  m_allocator;
};

using StdArena      = TArena<DefaultAllocator>;
using CachingArena  = TArena<CachingAllocator>;
