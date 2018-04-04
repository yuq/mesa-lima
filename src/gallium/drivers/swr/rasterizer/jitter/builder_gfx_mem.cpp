/****************************************************************************
* Copyright (C) 2014-2018 Intel Corporation.   All Rights Reserved.
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
* @file builder_gfx_mem.cpp
*
* @brief Definition of the gfx mem builder
*
* Notes:
*
******************************************************************************/
#include "jit_pch.hpp"
#include "builder.h"
#include "common/rdtsc_buckets.h"
#include "builder_gfx_mem.h"


namespace SwrJit
{
    using namespace llvm;

    BuilderGfxMem::BuilderGfxMem(JitManager* pJitMgr) :
        Builder(pJitMgr)
    {
        mpfnTranslateGfxAddress = nullptr;
        mpParamSimDC = nullptr;

    }

    void BuilderGfxMem::NotifyPrivateContextSet()
    {
    }

    void BuilderGfxMem::AssertGFXMemoryParams(Value* ptr, Builder::JIT_MEM_CLIENT usage)
    {
        SWR_ASSERT(ptr->getType() == mInt64Ty, "GFX addresses must be gfxptr_t and not converted to system pointers.");
        SWR_ASSERT(usage != MEM_CLIENT_INTERNAL, "Internal memory should not go through the translation path and should not be gfxptr_t.");
    }

    //////////////////////////////////////////////////////////////////////////
    /// @brief Generate a masked gather operation in LLVM IR.  If not  
    /// supported on the underlying platform, emulate it with loads
    /// @param vSrc - SIMD wide value that will be loaded if mask is invalid
    /// @param pBase - Int8* base VB address pointer value
    /// @param vIndices - SIMD wide value of VB byte offsets
    /// @param vMask - SIMD wide mask that controls whether to access memory or the src values
    /// @param scale - value to scale indices by
    Value *BuilderGfxMem::GATHERPS(Value *vSrc, Value *pBase, Value *vIndices, Value *vMask, uint8_t scale, JIT_MEM_CLIENT usage)
    {
        Value *vGather;

        // address may be coming in as 64bit int now so get the pointer
        if (pBase->getType() == mInt64Ty)
        {
            pBase = INT_TO_PTR(pBase, PointerType::get(mInt8Ty, 0));
        }

        vGather = Builder::GATHERPS(vSrc, pBase, vIndices, vMask, scale);
        return vGather;
    }

    //////////////////////////////////////////////////////////////////////////
    /// @brief Generate a masked gather operation in LLVM IR.  If not  
    /// supported on the underlying platform, emulate it with loads
    /// @param vSrc - SIMD wide value that will be loaded if mask is invalid
    /// @param pBase - Int8* base VB address pointer value
    /// @param vIndices - SIMD wide value of VB byte offsets
    /// @param vMask - SIMD wide mask that controls whether to access memory or the src values
    /// @param scale - value to scale indices by
    Value *BuilderGfxMem::GATHERDD(Value* vSrc, Value* pBase, Value* vIndices, Value* vMask, uint8_t scale, JIT_MEM_CLIENT usage)
    {
        Value* vGather = VIMMED1(0.0f);


        // address may be coming in as 64bit int now so get the pointer
        if (pBase->getType() == mInt64Ty)
        {
            pBase = INT_TO_PTR(pBase, PointerType::get(mInt8Ty, 0));
        }

        vGather = Builder::GATHERDD(vSrc, pBase, vIndices, vMask, scale);
        return vGather;
    }


    Value* BuilderGfxMem::OFFSET_TO_NEXT_COMPONENT(Value* base, Constant *offset)
    {
        return ADD(base, offset);
    }
    
    LoadInst* BuilderGfxMem::LOAD(Value *Ptr, const char *Name, JIT_MEM_CLIENT usage)
    {
        // the 64 bit gfx pointers are not yet propagated up the stack 
        // so there is some casting in here and the test for type is not yet enabled

        return Builder::LOAD(Ptr, Name);
    }

    LoadInst* BuilderGfxMem::LOAD(Value *Ptr, const Twine &Name, JIT_MEM_CLIENT usage)
    {
        return Builder::LOAD(Ptr, Name);
    }

    LoadInst* BuilderGfxMem::LOAD(Type *Ty, Value *Ptr, const Twine &Name, JIT_MEM_CLIENT usage)
    {
        return Builder::LOAD(Ty, Ptr, Name);
    }
    
    LoadInst* BuilderGfxMem::LOAD(Value *Ptr, bool isVolatile, const Twine &Name, JIT_MEM_CLIENT usage)
    {
        return Builder::LOAD(Ptr, isVolatile, Name);
    }

    LoadInst *BuilderGfxMem::LOAD(Value *BasePtr, const std::initializer_list<uint32_t> &offset, const llvm::Twine& name, JIT_MEM_CLIENT usage)
    {
        return Builder::LOAD(BasePtr, offset, name);
    }

    Value* BuilderGfxMem::TranslateGfxAddress(Value* xpGfxAddress)
    {
        return INT_TO_PTR(xpGfxAddress, PointerType::get(mInt8Ty, 0));
    }
}
