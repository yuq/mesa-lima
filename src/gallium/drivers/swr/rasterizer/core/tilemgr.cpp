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
* @file tilemgr.cpp
*
* @brief Implementation for Macro Tile Manager which provides the facilities
*        for threads to work on an macro tile.
*
******************************************************************************/
#include <unordered_map>

#include "fifo.hpp"
#include "tilemgr.h"

#define TILE_ID(x,y) ((x << 16 | y))

// override new/delete for alignment
void *MacroTileMgr::operator new(size_t size)
{
    return _aligned_malloc(size, 64);
}

void MacroTileMgr::operator delete(void *p)
{
    _aligned_free(p);
}

void* DispatchQueue::operator new(size_t size)
{
    return _aligned_malloc(size, 64);
}

void DispatchQueue::operator delete(void *p)
{
    _aligned_free(p);
}

MacroTileMgr::MacroTileMgr(Arena& arena) : mArena(arena)
{
}

void MacroTileMgr::initialize()
{
    mWorkItemsProduced = 0;
    mWorkItemsConsumed = 0;

    mDirtyTiles.clear();
}

void MacroTileMgr::enqueue(uint32_t x, uint32_t y, BE_WORK *pWork)
{
    // Should not enqueue more then what we have backing for in the hot tile manager.
    SWR_ASSERT(x < KNOB_NUM_HOT_TILES_X);
    SWR_ASSERT(y < KNOB_NUM_HOT_TILES_Y);

    uint32_t id = TILE_ID(x, y);

    MacroTileQueue &tile = mTiles[id];
    tile.mWorkItemsFE++;

    if (tile.mWorkItemsFE == 1)
    {
        tile.clear(mArena);
        mDirtyTiles.push_back(id);
    }

    mWorkItemsProduced++;
    tile.enqueue_try_nosync(mArena, pWork);
}

void MacroTileMgr::markTileComplete(uint32_t id)
{
    SWR_ASSERT(mTiles.find(id) != mTiles.end());
    MacroTileQueue &tile = mTiles[id];
    uint32_t numTiles = tile.mWorkItemsFE;
    InterlockedExchangeAdd(&mWorkItemsConsumed, numTiles);

    _ReadWriteBarrier();
    tile.mWorkItemsBE += numTiles;
    SWR_ASSERT(tile.mWorkItemsFE == tile.mWorkItemsBE);

    // clear out tile, but defer fifo clear until the next DC first queues to it.
    // this prevents worker threads from constantly locking a completed macro tile
    tile.mWorkItemsFE = 0;
    tile.mWorkItemsBE = 0;
}
