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
* @file builder_misc.h
*
* @brief miscellaneous builder functions
*
* Notes:
*
******************************************************************************/
#pragma once

Value *GEP(Value* ptr, const std::initializer_list<Value*> &indexList);
Value *GEP(Value* ptr, const std::initializer_list<uint32_t> &indexList);
Value *IN_BOUNDS_GEP(Value* ptr, const std::initializer_list<Value*> &indexList);
Value *IN_BOUNDS_GEP(Value* ptr, const std::initializer_list<uint32_t> &indexList);

LoadInst *LOAD(Value *BasePtr, const std::initializer_list<uint32_t> &offset, const llvm::Twine& name = "");
LoadInst *LOADV(Value *BasePtr, const std::initializer_list<Value*> &offset, const llvm::Twine& name = "");
StoreInst *STORE(Value *Val, Value *BasePtr, const std::initializer_list<uint32_t> &offset);
StoreInst *STOREV(Value *Val, Value *BasePtr, const std::initializer_list<Value*> &offset);

Value *MASKLOADD(Value* src, Value* mask);

void Gather4(const SWR_FORMAT format, Value* pSrcBase, Value* byteOffsets,
    Value* mask, Value* vGatherComponents[], bool bPackedOutput);

virtual Value* OFFSET_TO_NEXT_COMPONENT(Value* base, Constant *offset);

virtual Value *GATHERPS(Value *src, Value *pBase, Value *indices, Value *mask, uint8_t scale = 1);
Value *GATHERPS_16(Value *src, Value *pBase, Value *indices, Value *mask, uint8_t scale = 1);

virtual void GATHER4PS(const SWR_FORMAT_INFO &info, Value* pSrcBase, Value* byteOffsets,
    Value* mask, Value* vGatherComponents[], bool bPackedOutput);

virtual Value *GATHERDD(Value* src, Value* pBase, Value* indices, Value* mask, uint8_t scale = 1);
Value *GATHERDD_16(Value *src, Value *pBase, Value *indices, Value *mask, uint8_t scale = 1);

virtual void GATHER4DD(const SWR_FORMAT_INFO &info, Value* pSrcBase, Value* byteOffsets,
    Value* mask, Value* vGatherComponents[], bool bPackedOutput);

Value *GATHERPD(Value* src, Value* pBase, Value* indices, Value* mask, uint8_t scale = 1);

Value *GATHER_PTR(Value* pVecSrcPtr, Value* pVecMask, Value* pVecPassthru);

void SCATTERPS(Value* pDst, Value* vSrc, Value* vOffsets, Value* vMask);

void Shuffle8bpcGather4(const SWR_FORMAT_INFO &info, Value* vGatherInput, Value* vGatherOutput[], bool bPackedOutput);
void Shuffle16bpcGather4(const SWR_FORMAT_INFO &info, Value* vGatherInput[], Value* vGatherOutput[], bool bPackedOutput);

Value* STACKSAVE();
void STACKRESTORE(Value* pSaved);

// Static stack allocations for scatter operations
Value* pScatterStackSrc{ nullptr };
Value* pScatterStackOffsets{ nullptr };


virtual Value* TRANSLATE_ADDRESS(Value* address) { return address; }
