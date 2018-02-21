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
* @file builder_misc.cpp
*
* @brief Implementation for miscellaneous builder functions
*
* Notes:
*
******************************************************************************/
#include "jit_pch.hpp"
#include "builder.h"
#include "common/rdtsc_buckets.h"

#include <cstdarg>


namespace SwrJit
{

    Value *Builder::GEP(Value* ptr, const std::initializer_list<Value*> &indexList)
    {
        std::vector<Value*> indices;
        for (auto i : indexList)
            indices.push_back(i);
        return GEPA(ptr, indices);
    }

    Value *Builder::GEP(Value* ptr, const std::initializer_list<uint32_t> &indexList)
    {
        std::vector<Value*> indices;
        for (auto i : indexList)
            indices.push_back(C(i));
        return GEPA(ptr, indices);
    }

    Value *Builder::IN_BOUNDS_GEP(Value* ptr, const std::initializer_list<Value*> &indexList)
    {
        std::vector<Value*> indices;
        for (auto i : indexList)
            indices.push_back(i);
        return IN_BOUNDS_GEP(ptr, indices);
    }

    Value *Builder::IN_BOUNDS_GEP(Value* ptr, const std::initializer_list<uint32_t> &indexList)
    {
        std::vector<Value*> indices;
        for (auto i : indexList)
            indices.push_back(C(i));
        return IN_BOUNDS_GEP(ptr, indices);
    }

    LoadInst* Builder::LOAD(Value *Ptr, const char *Name)
    {
        return IRB()->CreateLoad(Ptr, Name);
    }

    LoadInst* Builder::LOAD(Value *Ptr, const Twine &Name)
    {
        return IRB()->CreateLoad(Ptr, Name);
    }

    LoadInst* Builder::LOAD(Type *Ty, Value *Ptr, const Twine &Name)
    {
        return IRB()->CreateLoad(Ty, Ptr, Name);
    }

    LoadInst* Builder::LOAD(Value *Ptr, bool isVolatile, const Twine &Name)
    {
        return IRB()->CreateLoad(Ptr, isVolatile, Name);
    }

    LoadInst *Builder::LOAD(Value *basePtr, const std::initializer_list<uint32_t> &indices, const llvm::Twine& name)
    {
        std::vector<Value*> valIndices;
        for (auto i : indices)
            valIndices.push_back(C(i));
        return LOAD(GEPA(basePtr, valIndices), name);
    }

    LoadInst *Builder::LOADV(Value *basePtr, const std::initializer_list<Value*> &indices, const llvm::Twine& name)
    {
        std::vector<Value*> valIndices;
        for (auto i : indices)
            valIndices.push_back(i);
        return LOAD(GEPA(basePtr, valIndices), name);
    }

    StoreInst *Builder::STORE(Value *val, Value *basePtr, const std::initializer_list<uint32_t> &indices)
    {
        std::vector<Value*> valIndices;
        for (auto i : indices)
            valIndices.push_back(C(i));
        return STORE(val, GEPA(basePtr, valIndices));
    }

    StoreInst *Builder::STOREV(Value *val, Value *basePtr, const std::initializer_list<Value*> &indices)
    {
        std::vector<Value*> valIndices;
        for (auto i : indices)
            valIndices.push_back(i);
        return STORE(val, GEPA(basePtr, valIndices));
    }

    //////////////////////////////////////////////////////////////////////////
    /// @brief Generate an i32 masked load operation in LLVM IR.  If not  
    /// supported on the underlying platform, emulate it with float masked load
    /// @param src - base address pointer for the load
    /// @param vMask - SIMD wide mask that controls whether to access memory load 0
    Value *Builder::MASKLOADD(Value* src, Value* mask)
    {
        Value* vResult;
        // use avx2 gather instruction is available
        if (JM()->mArch.AVX2())
        {
            Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx2_maskload_d_256);
            vResult = CALL(func, { src,mask });
        }
        else
        {
            // maskload intrinsic expects integer mask operand in llvm >= 3.8
            mask = BITCAST(mask, VectorType::get(mInt32Ty, mVWidth));
            Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_maskload_ps_256);
            vResult = BITCAST(CALL(func, { src,mask }), VectorType::get(mInt32Ty, mVWidth));
        }
        return vResult;
    }

    Value* Builder::OFFSET_TO_NEXT_COMPONENT(Value* base, Constant *offset)
    {
        return GEP(base, offset);
    }

    //////////////////////////////////////////////////////////////////////////
    /// @brief Generate a masked gather operation in LLVM IR.  If not  
    /// supported on the underlying platform, emulate it with loads
    /// @param vSrc - SIMD wide value that will be loaded if mask is invalid
    /// @param pBase - Int8* base VB address pointer value
    /// @param vIndices - SIMD wide value of VB byte offsets
    /// @param vMask - SIMD wide mask that controls whether to access memory or the src values
    /// @param scale - value to scale indices by
    Value *Builder::GATHERPS(Value *vSrc, Value *pBase, Value *vIndices, Value *vMask, uint8_t scale)
    {
        Value *vGather;
        Value *pBasePtr = INT_TO_PTR(pBase, PointerType::get(mInt8Ty, 0));

        // use avx2 gather instruction if available
        if (JM()->mArch.AVX2())
        {
            // force mask to <N x float>, required by vgather
            Value *mask = BITCAST(VMASK(vMask), mSimdFP32Ty);

            vGather = VGATHERPS(vSrc, pBasePtr, vIndices, mask, C(scale));
        }
        else
        {
            Value* pStack = STACKSAVE();

            // store vSrc on the stack.  this way we can select between a valid load address and the vSrc address
            Value* vSrcPtr = ALLOCA(vSrc->getType());
            STORE(vSrc, vSrcPtr);

            vGather = VUNDEF_F();
            Value *vScaleVec = VIMMED1((uint32_t)scale);
            Value *vOffsets = MUL(vIndices, vScaleVec);
            for (uint32_t i = 0; i < mVWidth; ++i)
            {
                // single component byte index
                Value *offset = VEXTRACT(vOffsets, C(i));
                // byte pointer to component
                Value *loadAddress = GEP(pBasePtr, offset);
                loadAddress = BITCAST(loadAddress, PointerType::get(mFP32Ty, 0));
                // pointer to the value to load if we're masking off a component
                Value *maskLoadAddress = GEP(vSrcPtr, { C(0), C(i) });
                Value *selMask = VEXTRACT(vMask, C(i));
                // switch in a safe address to load if we're trying to access a vertex 
                Value *validAddress = SELECT(selMask, loadAddress, maskLoadAddress);
                Value *val = LOAD(validAddress);
                vGather = VINSERT(vGather, val, C(i));
            }

            STACKRESTORE(pStack);
        }

        return vGather;
    }

    Value *Builder::GATHERPS_16(Value *vSrc, Value *pBase, Value *vIndices, Value *vMask, uint8_t scale)
    {
        Value *vGather = VUNDEF_F_16();

        // use AVX512F gather instruction if available
        if (JM()->mArch.AVX512F())
        {
            // force mask to <N-bit Integer>, required by vgather2
            Value *mask = BITCAST(vMask, mInt16Ty);

            vGather = VGATHERPS_16(vSrc, pBase, vIndices, mask, C((uint32_t)scale));
        }
        else
        {
            Value *src0 = EXTRACT_16(vSrc, 0);
            Value *src1 = EXTRACT_16(vSrc, 1);

            Value *indices0 = EXTRACT_16(vIndices, 0);
            Value *indices1 = EXTRACT_16(vIndices, 1);

            Value *mask0 = EXTRACT_16(vMask, 0);
            Value *mask1 = EXTRACT_16(vMask, 1);

            Value *gather0 = GATHERPS(src0, pBase, indices0, mask0, scale);
            Value *gather1 = GATHERPS(src1, pBase, indices1, mask1, scale);

            vGather = JOIN_16(gather0, gather1);
        }

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
    Value *Builder::GATHERDD(Value* vSrc, Value* pBase, Value* vIndices, Value* vMask, uint8_t scale)
    {
        Value* vGather;

        // use avx2 gather instruction if available
        if (JM()->mArch.AVX2())
        {
            vGather = VGATHERDD(vSrc, pBase, vIndices, VMASK(vMask), C(scale));
        }
        else
        {
            Value* pStack = STACKSAVE();

            // store vSrc on the stack.  this way we can select between a valid load address and the vSrc address
            Value* vSrcPtr = ALLOCA(vSrc->getType());
            STORE(vSrc, vSrcPtr);

            vGather = VUNDEF_I();
            Value *vScaleVec = VIMMED1((uint32_t)scale);
            Value *vOffsets = MUL(vIndices, vScaleVec);
            for (uint32_t i = 0; i < mVWidth; ++i)
            {
                // single component byte index
                Value *offset = VEXTRACT(vOffsets, C(i));
                // byte pointer to component
                Value *loadAddress = GEP(pBase, offset);
                loadAddress = BITCAST(loadAddress, PointerType::get(mInt32Ty, 0));
                // pointer to the value to load if we're masking off a component
                Value *maskLoadAddress = GEP(vSrcPtr, { C(0), C(i) });
                Value *selMask = VEXTRACT(vMask, C(i));
                // switch in a safe address to load if we're trying to access a vertex 
                Value *validAddress = SELECT(selMask, loadAddress, maskLoadAddress);
                Value *val = LOAD(validAddress, C(0));
                vGather = VINSERT(vGather, val, C(i));
            }

            STACKRESTORE(pStack);
        }

        return vGather;
    }

    Value *Builder::GATHERDD_16(Value *vSrc, Value *pBase, Value *vIndices, Value *vMask, uint8_t scale)
    {
        Value *vGather = VUNDEF_I_16();

        // use AVX512F gather instruction if available
        if (JM()->mArch.AVX512F())
        {
            // force mask to <N-bit Integer>, required by vgather2
            Value *mask = BITCAST(vMask, mInt16Ty);

            vGather = VGATHERDD_16(vSrc, pBase, vIndices, mask, C((uint32_t)scale));
        }
        else
        {
            Value *src0 = EXTRACT_16(vSrc, 0);
            Value *src1 = EXTRACT_16(vSrc, 1);

            Value *indices0 = EXTRACT_16(vIndices, 0);
            Value *indices1 = EXTRACT_16(vIndices, 1);

            Value *mask0 = EXTRACT_16(vMask, 0);
            Value *mask1 = EXTRACT_16(vMask, 1);

            Value *gather0 = GATHERDD(src0, pBase, indices0, mask0, scale);
            Value *gather1 = GATHERDD(src1, pBase, indices1, mask1, scale);

            vGather = JOIN_16(gather0, gather1);
        }

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
    Value *Builder::GATHERPD(Value* vSrc, Value* pBase, Value* vIndices, Value* vMask, uint8_t scale)
    {
        Value* vGather;

        // use avx2 gather instruction if available
        if (JM()->mArch.AVX2())
        {
            vMask = BITCAST(S_EXT(vMask, VectorType::get(mInt64Ty, mVWidth / 2)), VectorType::get(mDoubleTy, mVWidth / 2));
            vGather = VGATHERPD(vSrc, pBase, vIndices, vMask, C(scale));
        }
        else
        {
            Value* pStack = STACKSAVE();

            // store vSrc on the stack.  this way we can select between a valid load address and the vSrc address
            Value* vSrcPtr = ALLOCA(vSrc->getType());
            STORE(vSrc, vSrcPtr);

            vGather = UndefValue::get(VectorType::get(mDoubleTy, 4));
            Value *vScaleVec = VECTOR_SPLAT(4, C((uint32_t)scale));
            Value *vOffsets = MUL(vIndices, vScaleVec);
            for (uint32_t i = 0; i < mVWidth / 2; ++i)
            {
                // single component byte index
                Value *offset = VEXTRACT(vOffsets, C(i));
                // byte pointer to component
                Value *loadAddress = GEP(pBase, offset);
                loadAddress = BITCAST(loadAddress, PointerType::get(mDoubleTy, 0));
                // pointer to the value to load if we're masking off a component
                Value *maskLoadAddress = GEP(vSrcPtr, { C(0), C(i) });
                Value *selMask = VEXTRACT(vMask, C(i));
                // switch in a safe address to load if we're trying to access a vertex
                Value *validAddress = SELECT(selMask, loadAddress, maskLoadAddress);
                Value *val = LOAD(validAddress);
                vGather = VINSERT(vGather, val, C(i));
            }
            STACKRESTORE(pStack);
        }
        return vGather;
    }

    //////////////////////////////////////////////////////////////////////////
    /// @brief Alternative masked gather where source is a vector of pointers
    /// @param pVecSrcPtr   - SIMD wide vector of pointers
    /// @param pVecMask     - SIMD active lanes
    /// @param pVecPassthru - SIMD wide vector of values to load when lane is inactive
    Value* Builder::GATHER_PTR(Value* pVecSrcPtr, Value* pVecMask, Value* pVecPassthru)
    {
        Function* pMaskedGather = llvm::Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::masked_gather, { pVecPassthru->getType() });

        return CALL(pMaskedGather, { pVecSrcPtr, C(0), pVecMask, pVecPassthru });
    }

    void Builder::Gather4(const SWR_FORMAT format, Value* pSrcBase, Value* byteOffsets,
        Value* mask, Value* vGatherComponents[], bool bPackedOutput)
    {
        const SWR_FORMAT_INFO &info = GetFormatInfo(format);
        if (info.type[0] == SWR_TYPE_FLOAT && info.bpc[0] == 32)
        {
            GATHER4PS(info, pSrcBase, byteOffsets, mask, vGatherComponents, bPackedOutput);
        }
        else
        {
            GATHER4DD(info, pSrcBase, byteOffsets, mask, vGatherComponents, bPackedOutput);
        }
    }

    void Builder::GATHER4PS(const SWR_FORMAT_INFO &info, Value* pSrcBase, Value* byteOffsets,
        Value* vMask, Value* vGatherComponents[], bool bPackedOutput)
    {
        switch (info.bpp / info.numComps)
        {
        case 16:
        {
            Value* vGatherResult[2];

            // TODO: vGatherMaskedVal
            Value* vGatherMaskedVal = VIMMED1((float)0);

            // always have at least one component out of x or y to fetch

            vGatherResult[0] = GATHERPS(vGatherMaskedVal, pSrcBase, byteOffsets, vMask);
            // e.g. result of first 8x32bit integer gather for 16bit components
            // 256i - 0    1    2    3    4    5    6    7
            //        xyxy xyxy xyxy xyxy xyxy xyxy xyxy xyxy
            //

            // if we have at least one component out of x or y to fetch
            if (info.numComps > 2)
            {
                // offset base to the next components(zw) in the vertex to gather
                pSrcBase = OFFSET_TO_NEXT_COMPONENT(pSrcBase, C((intptr_t)4));

                vGatherResult[1] = GATHERPS(vGatherMaskedVal, pSrcBase, byteOffsets, vMask);
                // e.g. result of second 8x32bit integer gather for 16bit components
                // 256i - 0    1    2    3    4    5    6    7
                //        zwzw zwzw zwzw zwzw zwzw zwzw zwzw zwzw 
                //
            }
            else
            {
                vGatherResult[1] = vGatherMaskedVal;
            }

            // Shuffle gathered components into place, each row is a component
            Shuffle16bpcGather4(info, vGatherResult, vGatherComponents, bPackedOutput);
        }
        break;
        case 32:
        {
            // apply defaults
            for (uint32_t i = 0; i < 4; ++i)
            {
                vGatherComponents[i] = VIMMED1(*(float*)&info.defaults[i]);
            }

            for (uint32_t i = 0; i < info.numComps; i++)
            {
                uint32_t swizzleIndex = info.swizzle[i];

                // Gather a SIMD of components
                vGatherComponents[swizzleIndex] = GATHERPS(vGatherComponents[swizzleIndex], pSrcBase, byteOffsets, vMask);

                // offset base to the next component to gather
                pSrcBase = OFFSET_TO_NEXT_COMPONENT(pSrcBase, C((intptr_t)4));
            }
        }
        break;
        default:
            SWR_INVALID("Invalid float format");
            break;
        }
    }

    void Builder::GATHER4DD(const SWR_FORMAT_INFO &info, Value* pSrcBase, Value* byteOffsets,
        Value* vMask, Value* vGatherComponents[], bool bPackedOutput)
    {
        switch (info.bpp / info.numComps)
        {
        case 8:
        {
            Value* vGatherMaskedVal = VIMMED1((int32_t)0);
            Value* vGatherResult = GATHERDD(vGatherMaskedVal, pSrcBase, byteOffsets, vMask);
            // e.g. result of an 8x32bit integer gather for 8bit components
            // 256i - 0    1    2    3    4    5    6    7
            //        xyzw xyzw xyzw xyzw xyzw xyzw xyzw xyzw 

            Shuffle8bpcGather4(info, vGatherResult, vGatherComponents, bPackedOutput);
        }
        break;
        case 16:
        {
            Value* vGatherResult[2];

            // TODO: vGatherMaskedVal
            Value* vGatherMaskedVal = VIMMED1((int32_t)0);

            // always have at least one component out of x or y to fetch

            vGatherResult[0] = GATHERDD(vGatherMaskedVal, pSrcBase, byteOffsets, vMask);
            // e.g. result of first 8x32bit integer gather for 16bit components
            // 256i - 0    1    2    3    4    5    6    7
            //        xyxy xyxy xyxy xyxy xyxy xyxy xyxy xyxy
            //

            // if we have at least one component out of x or y to fetch
            if (info.numComps > 2)
            {
                // offset base to the next components(zw) in the vertex to gather
                pSrcBase = OFFSET_TO_NEXT_COMPONENT(pSrcBase, C((intptr_t)4));

                vGatherResult[1] = GATHERDD(vGatherMaskedVal, pSrcBase, byteOffsets, vMask);
                // e.g. result of second 8x32bit integer gather for 16bit components
                // 256i - 0    1    2    3    4    5    6    7
                //        zwzw zwzw zwzw zwzw zwzw zwzw zwzw zwzw 
                //
            }
            else
            {
                vGatherResult[1] = vGatherMaskedVal;
            }

            // Shuffle gathered components into place, each row is a component
            Shuffle16bpcGather4(info, vGatherResult, vGatherComponents, bPackedOutput);

        }
        break;
        case 32:
        {
            // apply defaults
            for (uint32_t i = 0; i < 4; ++i)
            {
                vGatherComponents[i] = VIMMED1((int)info.defaults[i]);
            }

            for (uint32_t i = 0; i < info.numComps; i++)
            {
                uint32_t swizzleIndex = info.swizzle[i];

                // Gather a SIMD of components
                vGatherComponents[swizzleIndex] = GATHERDD(vGatherComponents[swizzleIndex], pSrcBase, byteOffsets, vMask);

                // offset base to the next component to gather
                pSrcBase = OFFSET_TO_NEXT_COMPONENT(pSrcBase, C((intptr_t)4));
            }
        }
        break;
        default:
            SWR_INVALID("unsupported format");
            break;
        }
    }

    void Builder::Shuffle16bpcGather4(const SWR_FORMAT_INFO &info, Value* vGatherInput[2], Value* vGatherOutput[4], bool bPackedOutput)
    {
        // cast types
        Type* vGatherTy = VectorType::get(IntegerType::getInt32Ty(JM()->mContext), mVWidth);
        Type* v32x8Ty = VectorType::get(mInt8Ty, mVWidth * 4); // vwidth is units of 32 bits

                                                               // input could either be float or int vector; do shuffle work in int
        vGatherInput[0] = BITCAST(vGatherInput[0], mSimdInt32Ty);
        vGatherInput[1] = BITCAST(vGatherInput[1], mSimdInt32Ty);

        if (bPackedOutput)
        {
            Type* v128bitTy = VectorType::get(IntegerType::getIntNTy(JM()->mContext, 128), mVWidth / 4); // vwidth is units of 32 bits

                                                                                                         // shuffle mask
            Value* vConstMask = C<char>({ 0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15,
                0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15 });
            Value* vShufResult = BITCAST(PSHUFB(BITCAST(vGatherInput[0], v32x8Ty), vConstMask), vGatherTy);
            // after pshufb: group components together in each 128bit lane
            // 256i - 0    1    2    3    4    5    6    7
            //        xxxx xxxx yyyy yyyy xxxx xxxx yyyy yyyy

            Value* vi128XY = BITCAST(PERMD(vShufResult, C<int32_t>({ 0, 1, 4, 5, 2, 3, 6, 7 })), v128bitTy);
            // after PERMD: move and pack xy components into each 128bit lane
            // 256i - 0    1    2    3    4    5    6    7
            //        xxxx xxxx xxxx xxxx yyyy yyyy yyyy yyyy

            // do the same for zw components
            Value* vi128ZW = nullptr;
            if (info.numComps > 2)
            {
                Value* vShufResult = BITCAST(PSHUFB(BITCAST(vGatherInput[1], v32x8Ty), vConstMask), vGatherTy);
                vi128ZW = BITCAST(PERMD(vShufResult, C<int32_t>({ 0, 1, 4, 5, 2, 3, 6, 7 })), v128bitTy);
            }

            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t swizzleIndex = info.swizzle[i];
                // todo: fixed for packed
                Value* vGatherMaskedVal = VIMMED1((int32_t)(info.defaults[i]));
                if (i >= info.numComps)
                {
                    // set the default component val
                    vGatherOutput[swizzleIndex] = vGatherMaskedVal;
                    continue;
                }

                // if x or z, extract 128bits from lane 0, else for y or w, extract from lane 1
                uint32_t lane = ((i == 0) || (i == 2)) ? 0 : 1;
                // if x or y, use vi128XY permute result, else use vi128ZW
                Value* selectedPermute = (i < 2) ? vi128XY : vi128ZW;

                // extract packed component 128 bit lanes 
                vGatherOutput[swizzleIndex] = VEXTRACT(selectedPermute, C(lane));
            }

        }
        else
        {
            // pshufb masks for each component
            Value* vConstMask[2];
            // x/z shuffle mask
            vConstMask[0] = C<char>({ 0, 1, -1, -1, 4, 5, -1, -1, 8, 9, -1, -1, 12, 13, -1, -1,
                0, 1, -1, -1, 4, 5, -1, -1, 8, 9, -1, -1, 12, 13, -1, -1, });

            // y/w shuffle mask
            vConstMask[1] = C<char>({ 2, 3, -1, -1, 6, 7, -1, -1, 10, 11, -1, -1, 14, 15, -1, -1,
                2, 3, -1, -1, 6, 7, -1, -1, 10, 11, -1, -1, 14, 15, -1, -1 });


            // shuffle enabled components into lower word of each 32bit lane, 0 extending to 32 bits
            // apply defaults
            for (uint32_t i = 0; i < 4; ++i)
            {
                vGatherOutput[i] = VIMMED1((int32_t)info.defaults[i]);
            }

            for (uint32_t i = 0; i < info.numComps; i++)
            {
                uint32_t swizzleIndex = info.swizzle[i];

                // select correct constMask for x/z or y/w pshufb
                uint32_t selectedMask = ((i == 0) || (i == 2)) ? 0 : 1;
                // if x or y, use vi128XY permute result, else use vi128ZW
                uint32_t selectedGather = (i < 2) ? 0 : 1;

                vGatherOutput[swizzleIndex] = BITCAST(PSHUFB(BITCAST(vGatherInput[selectedGather], v32x8Ty), vConstMask[selectedMask]), vGatherTy);
                // after pshufb mask for x channel; z uses the same shuffle from the second gather
                // 256i - 0    1    2    3    4    5    6    7
                //        xx00 xx00 xx00 xx00 xx00 xx00 xx00 xx00 
            }
        }
    }

    void Builder::Shuffle8bpcGather4(const SWR_FORMAT_INFO &info, Value* vGatherInput, Value* vGatherOutput[], bool bPackedOutput)
    {
        // cast types
        Type* vGatherTy = VectorType::get(IntegerType::getInt32Ty(JM()->mContext), mVWidth);
        Type* v32x8Ty = VectorType::get(mInt8Ty, mVWidth * 4); // vwidth is units of 32 bits

        if (bPackedOutput)
        {
            Type* v128Ty = VectorType::get(IntegerType::getIntNTy(JM()->mContext, 128), mVWidth / 4); // vwidth is units of 32 bits
                                                                                                      // shuffle mask
            Value* vConstMask = C<char>({ 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15,
                0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15 });
            Value* vShufResult = BITCAST(PSHUFB(BITCAST(vGatherInput, v32x8Ty), vConstMask), vGatherTy);
            // after pshufb: group components together in each 128bit lane
            // 256i - 0    1    2    3    4    5    6    7
            //        xxxx yyyy zzzz wwww xxxx yyyy zzzz wwww

            Value* vi128XY = BITCAST(PERMD(vShufResult, C<int32_t>({ 0, 4, 0, 0, 1, 5, 0, 0 })), v128Ty);
            // after PERMD: move and pack xy and zw components in low 64 bits of each 128bit lane
            // 256i - 0    1    2    3    4    5    6    7
            //        xxxx xxxx dcdc dcdc yyyy yyyy dcdc dcdc (dc - don't care)

            // do the same for zw components
            Value* vi128ZW = nullptr;
            if (info.numComps > 2)
            {
                vi128ZW = BITCAST(PERMD(vShufResult, C<int32_t>({ 2, 6, 0, 0, 3, 7, 0, 0 })), v128Ty);
            }

            // sign extend all enabled components. If we have a fill vVertexElements, output to current simdvertex
            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t swizzleIndex = info.swizzle[i];
                // todo: fix for packed
                Value* vGatherMaskedVal = VIMMED1((int32_t)(info.defaults[i]));
                if (i >= info.numComps)
                {
                    // set the default component val
                    vGatherOutput[swizzleIndex] = vGatherMaskedVal;
                    continue;
                }

                // if x or z, extract 128bits from lane 0, else for y or w, extract from lane 1
                uint32_t lane = ((i == 0) || (i == 2)) ? 0 : 1;
                // if x or y, use vi128XY permute result, else use vi128ZW
                Value* selectedPermute = (i < 2) ? vi128XY : vi128ZW;

                // sign extend
                vGatherOutput[swizzleIndex] = VEXTRACT(selectedPermute, C(lane));
            }
        }
        // else zero extend
        else {
            // shuffle enabled components into lower byte of each 32bit lane, 0 extending to 32 bits
            // apply defaults
            for (uint32_t i = 0; i < 4; ++i)
            {
                vGatherOutput[i] = VIMMED1((int32_t)info.defaults[i]);
            }

            for (uint32_t i = 0; i < info.numComps; i++) {
                uint32_t swizzleIndex = info.swizzle[i];

                // pshufb masks for each component
                Value* vConstMask;
                switch (i)
                {
                case 0:
                    // x shuffle mask
                    vConstMask = C<char>({ 0, -1, -1, -1, 4, -1, -1, -1, 8, -1, -1, -1, 12, -1, -1, -1,
                        0, -1, -1, -1, 4, -1, -1, -1, 8, -1, -1, -1, 12, -1, -1, -1 });
                    break;
                case 1:
                    // y shuffle mask
                    vConstMask = C<char>({ 1, -1, -1, -1, 5, -1, -1, -1, 9, -1, -1, -1, 13, -1, -1, -1,
                        1, -1, -1, -1, 5, -1, -1, -1, 9, -1, -1, -1, 13, -1, -1, -1 });
                    break;
                case 2:
                    // z shuffle mask
                    vConstMask = C<char>({ 2, -1, -1, -1, 6, -1, -1, -1, 10, -1, -1, -1, 14, -1, -1, -1,
                        2, -1, -1, -1, 6, -1, -1, -1, 10, -1, -1, -1, 14, -1, -1, -1 });
                    break;
                case 3:
                    // w shuffle mask
                    vConstMask = C<char>({ 3, -1, -1, -1, 7, -1, -1, -1, 11, -1, -1, -1, 15, -1, -1, -1,
                        3, -1, -1, -1, 7, -1, -1, -1, 11, -1, -1, -1, 15, -1, -1, -1 });
                    break;
                default:
                    vConstMask = nullptr;
                    break;
                }

                vGatherOutput[swizzleIndex] = BITCAST(PSHUFB(BITCAST(vGatherInput, v32x8Ty), vConstMask), vGatherTy);
                // after pshufb for x channel
                // 256i - 0    1    2    3    4    5    6    7
                //        x000 x000 x000 x000 x000 x000 x000 x000 
            }
        }
    }

    //////////////////////////////////////////////////////////////////////////
    /// @brief emulates a scatter operation.
    /// @param pDst - pointer to destination 
    /// @param vSrc - vector of src data to scatter
    /// @param vOffsets - vector of byte offsets from pDst
    /// @param vMask - mask of valid lanes
    void Builder::SCATTERPS(Value* pDst, Value* vSrc, Value* vOffsets, Value* vMask)
    {
        /* Scatter algorithm

        while(Index = BitScanForward(mask))
        srcElem = srcVector[Index]
        offsetElem = offsetVector[Index]
        *(pDst + offsetElem) = srcElem
        Update mask (&= ~(1<<Index)

        */

        BasicBlock* pCurBB = IRB()->GetInsertBlock();
        Function* pFunc = pCurBB->getParent();
        Type* pSrcTy = vSrc->getType()->getVectorElementType();

        // Store vectors on stack
        if (pScatterStackSrc == nullptr)
        {
            // Save off stack allocations and reuse per scatter. Significantly reduces stack
            // requirements for shaders with a lot of scatters.
            pScatterStackSrc = CreateEntryAlloca(pFunc, mSimdInt64Ty);
            pScatterStackOffsets = CreateEntryAlloca(pFunc, mSimdInt32Ty);
        }

        Value* pSrcArrayPtr = BITCAST(pScatterStackSrc, PointerType::get(vSrc->getType(), 0));
        Value* pOffsetsArrayPtr = pScatterStackOffsets;
        STORE(vSrc, pSrcArrayPtr);
        STORE(vOffsets, pOffsetsArrayPtr);

        // Cast to pointers for random access
        pSrcArrayPtr = POINTER_CAST(pSrcArrayPtr, PointerType::get(pSrcTy, 0));
        pOffsetsArrayPtr = POINTER_CAST(pOffsetsArrayPtr, PointerType::get(mInt32Ty, 0));

        Value* pMask = VMOVMSKPS(BITCAST(vMask, mSimdFP32Ty));

        // Get cttz function
        Function* pfnCttz = Intrinsic::getDeclaration(mpJitMgr->mpCurrentModule, Intrinsic::cttz, { mInt32Ty });

        // Setup loop basic block
        BasicBlock* pLoop = BasicBlock::Create(mpJitMgr->mContext, "Scatter_Loop", pFunc);

        // compute first set bit
        Value* pIndex = CALL(pfnCttz, { pMask, C(false) });

        Value* pIsUndef = ICMP_EQ(pIndex, C(32));

        // Split current block
        BasicBlock* pPostLoop = pCurBB->splitBasicBlock(cast<Instruction>(pIsUndef)->getNextNode());

        // Remove unconditional jump created by splitBasicBlock
        pCurBB->getTerminator()->eraseFromParent();

        // Add terminator to end of original block
        IRB()->SetInsertPoint(pCurBB);

        // Add conditional branch
        COND_BR(pIsUndef, pPostLoop, pLoop);

        // Add loop basic block contents
        IRB()->SetInsertPoint(pLoop);
        PHINode* pIndexPhi = PHI(mInt32Ty, 2);
        PHINode* pMaskPhi = PHI(mInt32Ty, 2);

        pIndexPhi->addIncoming(pIndex, pCurBB);
        pMaskPhi->addIncoming(pMask, pCurBB);

        // Extract elements for this index
        Value* pSrcElem = LOADV(pSrcArrayPtr, { pIndexPhi });
        Value* pOffsetElem = LOADV(pOffsetsArrayPtr, { pIndexPhi });

        // GEP to this offset in dst
        Value* pCurDst = GEP(pDst, pOffsetElem);
        pCurDst = POINTER_CAST(pCurDst, PointerType::get(pSrcTy, 0));
        STORE(pSrcElem, pCurDst);

        // Update the mask
        Value* pNewMask = AND(pMaskPhi, NOT(SHL(C(1), pIndexPhi)));

        // Terminator
        Value* pNewIndex = CALL(pfnCttz, { pNewMask, C(false) });

        pIsUndef = ICMP_EQ(pNewIndex, C(32));
        COND_BR(pIsUndef, pPostLoop, pLoop);

        // Update phi edges
        pIndexPhi->addIncoming(pNewIndex, pLoop);
        pMaskPhi->addIncoming(pNewMask, pLoop);

        // Move builder to beginning of post loop
        IRB()->SetInsertPoint(pPostLoop, pPostLoop->begin());
    }

    //////////////////////////////////////////////////////////////////////////
    /// @brief save/restore stack, providing ability to push/pop the stack and 
    ///        reduce overall stack requirements for temporary stack use
    Value* Builder::STACKSAVE()
    {
        Function* pfnStackSave = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::stacksave);
        return CALLA(pfnStackSave);
    }

    void Builder::STACKRESTORE(Value* pSaved)
    {
        Function* pfnStackRestore = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::stackrestore);
        CALL(pfnStackRestore, std::initializer_list<Value*>{pSaved});
    }

}
