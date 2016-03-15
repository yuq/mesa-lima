/****************************************************************************
 * Copyright (C) 2015 Intel Corporation.   All Rights Reserved.
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
 ***************************************************************************/

#include "JitManager.h"
#include "state.h"
#include "state_llvm.h"
#include "builder.h"

#include "llvm-c/Core.h"
#include "llvm/Support/CBindingWrapping.h"

#include "tgsi/tgsi_strings.h"
#include "gallivm/lp_bld_init.h"
#include "gallivm/lp_bld_flow.h"
#include "gallivm/lp_bld_struct.h"
#include "gallivm/lp_bld_tgsi.h"

#include "swr_context.h"
#include "swr_context_llvm.h"
#include "swr_state.h"
#include "swr_screen.h"

bool operator==(const swr_jit_key &lhs, const swr_jit_key &rhs)
{
   return !memcmp(&lhs, &rhs, sizeof(lhs));
}

void
swr_generate_fs_key(struct swr_jit_key &key,
                    struct swr_context *ctx,
                    swr_fragment_shader *swr_fs)
{
   key.nr_cbufs = ctx->framebuffer.nr_cbufs;
   key.light_twoside = ctx->rasterizer->light_twoside;
   memcpy(&key.vs_output_semantic_name,
          &ctx->vs->info.base.output_semantic_name,
          sizeof(key.vs_output_semantic_name));
   memcpy(&key.vs_output_semantic_idx,
          &ctx->vs->info.base.output_semantic_index,
          sizeof(key.vs_output_semantic_idx));

   key.nr_samplers = swr_fs->info.base.file_max[TGSI_FILE_SAMPLER] + 1;

   for (unsigned i = 0; i < key.nr_samplers; i++) {
      if (swr_fs->info.base.file_mask[TGSI_FILE_SAMPLER] & (1 << i)) {
         lp_sampler_static_sampler_state(
            &key.sampler[i].sampler_state,
            ctx->samplers[PIPE_SHADER_FRAGMENT][i]);
      }
   }

   /*
    * XXX If TGSI_FILE_SAMPLER_VIEW exists assume all texture opcodes
    * are dx10-style? Can't really have mixed opcodes, at least not
    * if we want to skip the holes here (without rescanning tgsi).
    */
   if (swr_fs->info.base.file_max[TGSI_FILE_SAMPLER_VIEW] != -1) {
      key.nr_sampler_views =
         swr_fs->info.base.file_max[TGSI_FILE_SAMPLER_VIEW] + 1;
      for (unsigned i = 0; i < key.nr_sampler_views; i++) {
         if (swr_fs->info.base.file_mask[TGSI_FILE_SAMPLER_VIEW] & (1 << i)) {
            lp_sampler_static_texture_state(
               &key.sampler[i].texture_state,
               ctx->sampler_views[PIPE_SHADER_FRAGMENT][i]);
         }
      }
   } else {
      key.nr_sampler_views = key.nr_samplers;
      for (unsigned i = 0; i < key.nr_sampler_views; i++) {
         if (swr_fs->info.base.file_mask[TGSI_FILE_SAMPLER] & (1 << i)) {
            lp_sampler_static_texture_state(
               &key.sampler[i].texture_state,
               ctx->sampler_views[PIPE_SHADER_FRAGMENT][i]);
         }
      }
   }
}

struct BuilderSWR : public Builder {
   BuilderSWR(JitManager *pJitMgr)
      : Builder(pJitMgr)
   {
      pJitMgr->SetupNewModule();
   }

   PFN_VERTEX_FUNC
   CompileVS(struct pipe_context *ctx, swr_vertex_shader *swr_vs);
   PFN_PIXEL_KERNEL CompileFS(struct swr_context *ctx, swr_jit_key &key);
};

PFN_VERTEX_FUNC
BuilderSWR::CompileVS(struct pipe_context *ctx, swr_vertex_shader *swr_vs)
{
   swr_vs->linkageMask = 0;

   for (unsigned i = 0; i < swr_vs->info.base.num_outputs; i++) {
      switch (swr_vs->info.base.output_semantic_name[i]) {
      case TGSI_SEMANTIC_POSITION:
         break;
      default:
         swr_vs->linkageMask |= (1 << i);
         break;
      }
   }

   //   tgsi_dump(swr_vs->pipe.tokens, 0);

   struct gallivm_state *gallivm =
      gallivm_create("VS", wrap(&JM()->mContext));
   gallivm->module = wrap(JM()->mpCurrentModule);

   LLVMValueRef inputs[PIPE_MAX_SHADER_INPUTS][TGSI_NUM_CHANNELS];
   LLVMValueRef outputs[PIPE_MAX_SHADER_OUTPUTS][TGSI_NUM_CHANNELS];

   memset(outputs, 0, sizeof(outputs));

   AttrBuilder attrBuilder;
   attrBuilder.addStackAlignmentAttr(JM()->mVWidth * sizeof(float));
   AttributeSet attrSet = AttributeSet::get(
      JM()->mContext, AttributeSet::FunctionIndex, attrBuilder);

   std::vector<Type *> vsArgs{PointerType::get(Gen_swr_draw_context(JM()), 0),
                              PointerType::get(Gen_SWR_VS_CONTEXT(JM()), 0)};
   FunctionType *vsFuncType =
      FunctionType::get(Type::getVoidTy(JM()->mContext), vsArgs, false);

   // create new vertex shader function
   auto pFunction = Function::Create(vsFuncType,
                                     GlobalValue::ExternalLinkage,
                                     "VS",
                                     JM()->mpCurrentModule);
   pFunction->addAttributes(AttributeSet::FunctionIndex, attrSet);

   BasicBlock *block = BasicBlock::Create(JM()->mContext, "entry", pFunction);
   IRB()->SetInsertPoint(block);
   LLVMPositionBuilderAtEnd(gallivm->builder, wrap(block));

   auto argitr = pFunction->arg_begin();
   Value *hPrivateData = &*argitr++;
   hPrivateData->setName("hPrivateData");
   Value *pVsCtx = &*argitr++;
   pVsCtx->setName("vsCtx");
   
   Value *consts_ptr = GEP(hPrivateData, {C(0), C(swr_draw_context_constantVS)});

   consts_ptr->setName("vs_constants");
   Value *const_sizes_ptr =
      GEP(hPrivateData, {0, swr_draw_context_num_constantsVS});
   const_sizes_ptr->setName("num_vs_constants");

   Value *vtxInput = LOAD(pVsCtx, {0, SWR_VS_CONTEXT_pVin});

   for (uint32_t attrib = 0; attrib < PIPE_MAX_SHADER_INPUTS; attrib++) {
      const unsigned mask = swr_vs->info.base.input_usage_mask[attrib];
      for (uint32_t channel = 0; channel < TGSI_NUM_CHANNELS; channel++) {
         if (mask & (1 << channel)) {
            inputs[attrib][channel] =
               wrap(LOAD(vtxInput, {0, 0, attrib, channel}));
         }
      }
   }

   struct lp_bld_tgsi_system_values system_values;
   memset(&system_values, 0, sizeof(system_values));
   system_values.instance_id = wrap(LOAD(pVsCtx, {0, SWR_VS_CONTEXT_InstanceID}));
   system_values.vertex_id = wrap(LOAD(pVsCtx, {0, SWR_VS_CONTEXT_VertexID}));

   lp_build_tgsi_soa(gallivm,
                     swr_vs->pipe.tokens,
                     lp_type_float_vec(32, 32 * 8),
                     NULL, // mask
                     wrap(consts_ptr),
                     wrap(const_sizes_ptr),
                     &system_values,
                     inputs,
                     outputs,
                     NULL, // wrap(hPrivateData), (sampler context)
                     NULL, // thread data
                     NULL, // sampler
                     &swr_vs->info.base,
                     NULL); // geometry shader face

   IRB()->SetInsertPoint(unwrap(LLVMGetInsertBlock(gallivm->builder)));

   Value *vtxOutput = LOAD(pVsCtx, {0, SWR_VS_CONTEXT_pVout});

   for (uint32_t channel = 0; channel < TGSI_NUM_CHANNELS; channel++) {
      for (uint32_t attrib = 0; attrib < PIPE_MAX_SHADER_OUTPUTS; attrib++) {
         if (!outputs[attrib][channel])
            continue;

         Value *val = LOAD(unwrap(outputs[attrib][channel]));

         uint32_t outSlot = attrib;
         if (swr_vs->info.base.output_semantic_name[attrib] == TGSI_SEMANTIC_PSIZE)
            outSlot = VERTEX_POINT_SIZE_SLOT;
         STORE(val, vtxOutput, {0, 0, outSlot, channel});
      }
   }

   RET_VOID();

   gallivm_verify_function(gallivm, wrap(pFunction));
   gallivm_compile_module(gallivm);

   //   lp_debug_dump_value(func);

   PFN_VERTEX_FUNC pFunc =
      (PFN_VERTEX_FUNC)gallivm_jit_function(gallivm, wrap(pFunction));

   debug_printf("vert shader  %p\n", pFunc);
   assert(pFunc && "Error: VertShader = NULL");

#if (LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR >= 5)
   JM()->mIsModuleFinalized = true;
#endif

   return pFunc;
}

PFN_VERTEX_FUNC
swr_compile_vs(struct pipe_context *ctx, swr_vertex_shader *swr_vs)
{
   BuilderSWR builder(
      reinterpret_cast<JitManager *>(swr_screen(ctx->screen)->hJitMgr));
   return builder.CompileVS(ctx, swr_vs);
}

static unsigned
locate_linkage(ubyte name, ubyte index, struct tgsi_shader_info *info)
{
   for (int i = 0; i < PIPE_MAX_SHADER_OUTPUTS; i++) {
      if ((info->output_semantic_name[i] == name)
          && (info->output_semantic_index[i] == index)) {
         return i - 1; // position is not part of the linkage
      }
   }

   if (name == TGSI_SEMANTIC_COLOR) { // BCOLOR fallback
      for (int i = 0; i < PIPE_MAX_SHADER_OUTPUTS; i++) {
         if ((info->output_semantic_name[i] == TGSI_SEMANTIC_BCOLOR)
             && (info->output_semantic_index[i] == index)) {
            return i - 1; // position is not part of the linkage
         }
      }
   }

   return 0xFFFFFFFF;
}

PFN_PIXEL_KERNEL
BuilderSWR::CompileFS(struct swr_context *ctx, swr_jit_key &key)
{
   struct swr_fragment_shader *swr_fs = ctx->fs;

   //   tgsi_dump(swr_fs->pipe.tokens, 0);

   struct gallivm_state *gallivm =
      gallivm_create("FS", wrap(&JM()->mContext));
   gallivm->module = wrap(JM()->mpCurrentModule);

   LLVMValueRef inputs[PIPE_MAX_SHADER_INPUTS][TGSI_NUM_CHANNELS];
   LLVMValueRef outputs[PIPE_MAX_SHADER_OUTPUTS][TGSI_NUM_CHANNELS];

   memset(inputs, 0, sizeof(inputs));
   memset(outputs, 0, sizeof(outputs));

   struct lp_build_sampler_soa *sampler = NULL;

   AttrBuilder attrBuilder;
   attrBuilder.addStackAlignmentAttr(JM()->mVWidth * sizeof(float));
   AttributeSet attrSet = AttributeSet::get(
      JM()->mContext, AttributeSet::FunctionIndex, attrBuilder);

   std::vector<Type *> fsArgs{PointerType::get(Gen_swr_draw_context(JM()), 0),
                              PointerType::get(Gen_SWR_PS_CONTEXT(JM()), 0)};
   FunctionType *funcType =
      FunctionType::get(Type::getVoidTy(JM()->mContext), fsArgs, false);

   auto pFunction = Function::Create(funcType,
                                     GlobalValue::ExternalLinkage,
                                     "FS",
                                     JM()->mpCurrentModule);
   pFunction->addAttributes(AttributeSet::FunctionIndex, attrSet);

   BasicBlock *block = BasicBlock::Create(JM()->mContext, "entry", pFunction);
   IRB()->SetInsertPoint(block);
   LLVMPositionBuilderAtEnd(gallivm->builder, wrap(block));

   auto args = pFunction->arg_begin();
   Value *hPrivateData = &*args++;
   hPrivateData->setName("hPrivateData");
   Value *pPS = &*args++;
   pPS->setName("psCtx");

   Value *consts_ptr = GEP(hPrivateData, {0, swr_draw_context_constantFS});
   consts_ptr->setName("fs_constants");
   Value *const_sizes_ptr =
      GEP(hPrivateData, {0, swr_draw_context_num_constantsFS});
   const_sizes_ptr->setName("num_fs_constants");

   // xxx should check for flat shading versus interpolation


   // load *pAttribs, *pPerspAttribs
   Value *pRawAttribs = LOAD(pPS, {0, SWR_PS_CONTEXT_pAttribs}, "pRawAttribs");
   Value *pPerspAttribs =
      LOAD(pPS, {0, SWR_PS_CONTEXT_pPerspAttribs}, "pPerspAttribs");

   swr_fs->constantMask = 0;
   swr_fs->pointSpriteMask = 0;

   for (int attrib = 0; attrib < PIPE_MAX_SHADER_INPUTS; attrib++) {
      const unsigned mask = swr_fs->info.base.input_usage_mask[attrib];
      const unsigned interpMode = swr_fs->info.base.input_interpolate[attrib];
      const unsigned interpLoc = swr_fs->info.base.input_interpolate_loc[attrib];

      if (!mask)
         continue;

      // load i,j
      Value *vi = nullptr, *vj = nullptr;
      switch (interpLoc) {
      case TGSI_INTERPOLATE_LOC_CENTER:
         vi = LOAD(pPS, {0, SWR_PS_CONTEXT_vI, PixelPositions_center}, "i");
         vj = LOAD(pPS, {0, SWR_PS_CONTEXT_vJ, PixelPositions_center}, "j");
         break;
      case TGSI_INTERPOLATE_LOC_CENTROID:
         vi = LOAD(pPS, {0, SWR_PS_CONTEXT_vI, PixelPositions_centroid}, "i");
         vj = LOAD(pPS, {0, SWR_PS_CONTEXT_vJ, PixelPositions_centroid}, "j");
         break;
      case TGSI_INTERPOLATE_LOC_SAMPLE:
         vi = LOAD(pPS, {0, SWR_PS_CONTEXT_vI, PixelPositions_sample}, "i");
         vj = LOAD(pPS, {0, SWR_PS_CONTEXT_vJ, PixelPositions_sample}, "j");
         break;
      }

      // load/compute w
      Value *vw = nullptr, *pAttribs;
      if (interpMode == TGSI_INTERPOLATE_PERSPECTIVE) {
         pAttribs = pPerspAttribs;
         switch (interpLoc) {
         case TGSI_INTERPOLATE_LOC_CENTER:
            vw = VRCP(LOAD(pPS, {0, SWR_PS_CONTEXT_vOneOverW, PixelPositions_center}));
            break;
         case TGSI_INTERPOLATE_LOC_CENTROID:
            vw = VRCP(LOAD(pPS, {0, SWR_PS_CONTEXT_vOneOverW, PixelPositions_centroid}));
            break;
         case TGSI_INTERPOLATE_LOC_SAMPLE:
            vw = VRCP(LOAD(pPS, {0, SWR_PS_CONTEXT_vOneOverW, PixelPositions_sample}));
            break;
         }
      } else {
         pAttribs = pRawAttribs;
         vw = VIMMED1(1.f);
      }

      vw->setName("w");

      ubyte semantic_name = swr_fs->info.base.input_semantic_name[attrib];
      ubyte semantic_idx = swr_fs->info.base.input_semantic_index[attrib];

      if (semantic_name == TGSI_SEMANTIC_FACE) {
         Value *ff =
            UI_TO_FP(LOAD(pPS, {0, SWR_PS_CONTEXT_frontFace}), mFP32Ty);
         ff = FSUB(FMUL(ff, C(2.0f)), C(1.0f));
         ff = VECTOR_SPLAT(JM()->mVWidth, ff, "vFrontFace");

         inputs[attrib][0] = wrap(ff);
         inputs[attrib][1] = wrap(VIMMED1(0.0f));
         inputs[attrib][2] = wrap(VIMMED1(0.0f));
         inputs[attrib][3] = wrap(VIMMED1(1.0f));
         continue;
      } else if (semantic_name == TGSI_SEMANTIC_POSITION) { // gl_FragCoord
         inputs[attrib][0] = wrap(LOAD(pPS, {0, SWR_PS_CONTEXT_vX, PixelPositions_center}, "vX"));
         inputs[attrib][1] = wrap(LOAD(pPS, {0, SWR_PS_CONTEXT_vY, PixelPositions_center}, "vY"));
         inputs[attrib][2] = wrap(LOAD(pPS, {0, SWR_PS_CONTEXT_vZ}, "vZ"));
         inputs[attrib][3] =
            wrap(LOAD(pPS, {0, SWR_PS_CONTEXT_vOneOverW, PixelPositions_center}, "vOneOverW"));
         continue;
      } else if (semantic_name == TGSI_SEMANTIC_PRIMID) {
         Value *primID = LOAD(pPS, {0, SWR_PS_CONTEXT_primID}, "primID");
         inputs[attrib][0] = wrap(VECTOR_SPLAT(JM()->mVWidth, primID));
         inputs[attrib][1] = wrap(VIMMED1(0));
         inputs[attrib][2] = wrap(VIMMED1(0));
         inputs[attrib][3] = wrap(VIMMED1(0));
         continue;
      }

      unsigned linkedAttrib =
         locate_linkage(semantic_name, semantic_idx, &ctx->vs->info.base);
      if (linkedAttrib == 0xFFFFFFFF) {
         // not found - check for point sprite
         if (ctx->rasterizer->sprite_coord_enable) {
            linkedAttrib = ctx->vs->info.base.num_outputs - 1;
            swr_fs->pointSpriteMask |= (1 << linkedAttrib);
         } else {
            fprintf(stderr,
                    "Missing %s[%d]\n",
                    tgsi_semantic_names[semantic_name],
                    semantic_idx);
            assert(0 && "attribute linkage not found");
         }
      }

      if (interpMode == TGSI_INTERPOLATE_CONSTANT) {
         swr_fs->constantMask |= 1 << linkedAttrib;
      }

      for (int channel = 0; channel < TGSI_NUM_CHANNELS; channel++) {
         if (mask & (1 << channel)) {
            Value *indexA = C(linkedAttrib * 12 + channel);
            Value *indexB = C(linkedAttrib * 12 + channel + 4);
            Value *indexC = C(linkedAttrib * 12 + channel + 8);

            if ((semantic_name == TGSI_SEMANTIC_COLOR)
                && ctx->rasterizer->light_twoside) {
               unsigned bcolorAttrib = locate_linkage(
                  TGSI_SEMANTIC_BCOLOR, semantic_idx, &ctx->vs->info.base);

               unsigned diff = 12 * (bcolorAttrib - linkedAttrib);

               Value *back =
                  XOR(C(1), LOAD(pPS, {0, SWR_PS_CONTEXT_frontFace}), "backFace");

               Value *offset = MUL(back, C(diff));
               offset->setName("offset");

               indexA = ADD(indexA, offset);
               indexB = ADD(indexB, offset);
               indexC = ADD(indexC, offset);

               if (interpMode == TGSI_INTERPOLATE_CONSTANT) {
                  swr_fs->constantMask |= 1 << bcolorAttrib;
               }
            }

            Value *va = VBROADCAST(LOAD(GEP(pAttribs, indexA)));
            Value *vb = VBROADCAST(LOAD(GEP(pAttribs, indexB)));
            Value *vc = VBROADCAST(LOAD(GEP(pAttribs, indexC)));

            if (interpMode == TGSI_INTERPOLATE_CONSTANT) {
               inputs[attrib][channel] = wrap(va);
            } else {
               Value *vk = FSUB(FSUB(VIMMED1(1.0f), vi), vj);

               vc = FMUL(vk, vc);

               Value *interp = FMUL(va, vi);
               Value *interp1 = FMUL(vb, vj);
               interp = FADD(interp, interp1);
               interp = FADD(interp, vc);
               if (interpMode == TGSI_INTERPOLATE_PERSPECTIVE)
                  interp = FMUL(interp, vw);
               inputs[attrib][channel] = wrap(interp);
            }
         }
      }
   }

   sampler = swr_sampler_soa_create(key.sampler);

   struct lp_bld_tgsi_system_values system_values;
   memset(&system_values, 0, sizeof(system_values));

   struct lp_build_mask_context mask;

   if (swr_fs->info.base.uses_kill) {
      Value *mask_val = LOAD(pPS, {0, SWR_PS_CONTEXT_activeMask}, "activeMask");
      lp_build_mask_begin(
         &mask, gallivm, lp_type_float_vec(32, 32 * 8), wrap(mask_val));
   }

   lp_build_tgsi_soa(gallivm,
                     swr_fs->pipe.tokens,
                     lp_type_float_vec(32, 32 * 8),
                     swr_fs->info.base.uses_kill ? &mask : NULL, // mask
                     wrap(consts_ptr),
                     wrap(const_sizes_ptr),
                     &system_values,
                     inputs,
                     outputs,
                     wrap(hPrivateData),
                     NULL, // thread data
                     sampler, // sampler
                     &swr_fs->info.base,
                     NULL); // geometry shader face

   IRB()->SetInsertPoint(unwrap(LLVMGetInsertBlock(gallivm->builder)));

   for (uint32_t attrib = 0; attrib < swr_fs->info.base.num_outputs;
        attrib++) {
      switch (swr_fs->info.base.output_semantic_name[attrib]) {
      case TGSI_SEMANTIC_POSITION: {
         // write z
         LLVMValueRef outZ =
            LLVMBuildLoad(gallivm->builder, outputs[attrib][2], "");
         STORE(unwrap(outZ), pPS, {0, SWR_PS_CONTEXT_vZ});
         break;
      }
      case TGSI_SEMANTIC_COLOR: {
         for (uint32_t channel = 0; channel < TGSI_NUM_CHANNELS; channel++) {
            if (!outputs[attrib][channel])
               continue;

            LLVMValueRef out =
               LLVMBuildLoad(gallivm->builder, outputs[attrib][channel], "");
            if (swr_fs->info.base.properties[TGSI_PROPERTY_FS_COLOR0_WRITES_ALL_CBUFS]) {
               for (uint32_t rt = 0; rt < key.nr_cbufs; rt++) {
                  STORE(unwrap(out),
                        pPS,
                        {0, SWR_PS_CONTEXT_shaded, rt, channel});
               }
            } else {
               STORE(unwrap(out),
                     pPS,
                     {0,
                           SWR_PS_CONTEXT_shaded,
                           swr_fs->info.base.output_semantic_index[attrib],
                           channel});
            }
         }
         break;
      }
      default: {
         fprintf(stderr,
                 "unknown output from FS %s[%d]\n",
                 tgsi_semantic_names[swr_fs->info.base
                                        .output_semantic_name[attrib]],
                 swr_fs->info.base.output_semantic_index[attrib]);
         break;
      }
      }
   }

   LLVMValueRef mask_result = 0;
   if (swr_fs->info.base.uses_kill) {
      mask_result = lp_build_mask_end(&mask);
   }

   IRB()->SetInsertPoint(unwrap(LLVMGetInsertBlock(gallivm->builder)));

   if (swr_fs->info.base.uses_kill) {
      STORE(unwrap(mask_result), pPS, {0, SWR_PS_CONTEXT_activeMask});
   }

   RET_VOID();

   gallivm_verify_function(gallivm, wrap(pFunction));

   gallivm_compile_module(gallivm);

   PFN_PIXEL_KERNEL kernel =
      (PFN_PIXEL_KERNEL)gallivm_jit_function(gallivm, wrap(pFunction));
   debug_printf("frag shader  %p\n", kernel);
   assert(kernel && "Error: FragShader = NULL");

#if (LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR >= 5)
   JM()->mIsModuleFinalized = true;
#endif

   return kernel;
}

PFN_PIXEL_KERNEL
swr_compile_fs(struct swr_context *ctx, swr_jit_key &key)
{
   BuilderSWR builder(
      reinterpret_cast<JitManager *>(swr_screen(ctx->pipe.screen)->hJitMgr));
   return builder.CompileFS(ctx, key);
}
