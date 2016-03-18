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

template<typename MutexT = std::mutex, typename T = DefaultAllocator>
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

        static const size_t ArenaBlockSize = 1024 * 1024;
        size_t blockSize = std::max<size_t>(m_size + ArenaBlockSize, std::max(size, ArenaBlockSize));

        // Add in one BLOCK_ALIGN unit to store ArenaBlock in.
        blockSize = AlignUp(blockSize + BLOCK_ALIGN, BLOCK_ALIGN);

        void *pMem = m_allocator.AllocateAligned(blockSize, BLOCK_ALIGN);    // Arena blocks are always simd byte aligned.
        SWR_ASSERT(pMem != nullptr);

        ArenaBlock* pNewBlock = new (pMem) ArenaBlock();

        if (pNewBlock != nullptr)
        {
            pNewBlock->pNext = m_pCurBlock;

            m_pCurBlock = pNewBlock;
            m_pCurBlock->pMem = PtrAdd(pMem, BLOCK_ALIGN);
            m_pCurBlock->blockSize = blockSize - BLOCK_ALIGN;

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
        if (m_pCurBlock)
        {
            m_pCurBlock->offset = 0;

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

        m_size = 0;
    }

    size_t Size() const { return m_size; }

private:

    static const size_t BLOCK_ALIGN = KNOB_SIMD_WIDTH * 4;

    DefaultAllocator    m_defAllocator;
    T&                  m_allocator;

    struct ArenaBlock
    {
        void*       pMem        = nullptr;
        size_t      blockSize   = 0;
        size_t      offset      = 0;
        ArenaBlock* pNext       = nullptr;
    };
    static_assert(sizeof(ArenaBlock) <= BLOCK_ALIGN, "Increase BLOCK_ALIGN size");

    ArenaBlock*     m_pCurBlock = nullptr;
    size_t          m_size      = 0;

    /// @note Mutex is only used by sync allocation functions.
    MutexT          m_mutex;
};

typedef TArena<> Arena;

struct NullMutex
{
    void lock() {}
    void unlock() {}
};

// Ref counted Arena for ArenaAllocator
// NOT THREAD SAFE!!
struct RefArena : TArena<NullMutex>
{
    uint32_t AddRef() { return ++m_refCount; }
    uint32_t Release() { if (--m_refCount) { return m_refCount; } delete this; return 0; }

    void* allocate(std::size_t n)
    {
        ++m_numAllocations;
        return Alloc(n);
    }

    void deallocate(void* p) { --m_numAllocations; }
    void clear() { SWR_ASSERT(0 == m_numAllocations); Reset(); }

private:
    uint32_t m_refCount = 0;
    uint32_t m_numAllocations = 0;
};

#if 0 // THIS DOESN'T WORK!!!
// Arena based replacement for std::allocator
template <typename T>
struct ArenaAllocator
{
    typedef T value_type;
    ArenaAllocator()
    {
        m_pArena = new RefArena();
        m_pArena->AddRef();
    }
    ~ArenaAllocator()
    {
        m_pArena->Release(); m_pArena = nullptr;
    }
    ArenaAllocator(const ArenaAllocator& copy)
    {
        m_pArena = const_cast<RefArena*>(copy.m_pArena); m_pArena->AddRef();
    }


    template <class U> ArenaAllocator(const ArenaAllocator<U>& copy)
    {
        m_pArena = const_cast<RefArena*>(copy.m_pArena); m_pArena->AddRef();
    }
    T* allocate(std::size_t n)
    {
#if defined(_DEBUG)
        char buf[32];
        sprintf_s(buf, "Alloc: %lld\n", n);
        OutputDebugStringA(buf);
#endif
        void* p = m_pArena->allocate(n * sizeof(T));
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t n)
    {
#if defined(_DEBUG)
        char buf[32];
        sprintf_s(buf, "Dealloc: %lld\n", n);
        OutputDebugStringA(buf);
#endif
        m_pArena->deallocate(p);
    }
    void clear() { m_pArena->clear(); }

    RefArena* m_pArena = nullptr;
};

template <class T, class U>
bool operator== (const ArenaAllocator<T>&, const ArenaAllocator<U>&)
{
    return true;
}

template <class T, class U>
bool operator!= (const ArenaAllocator<T>&, const ArenaAllocator<U>&)
{
    return false;
}
#endif
