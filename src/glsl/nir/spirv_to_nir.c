/*
 * Copyright © 2015 Intel Corporation
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
 * Authors:
 *    Jason Ekstrand (jason@jlekstrand.net)
 *
 */

#include "nir_spirv.h"
#include "spirv.h"

struct vtn_decoration;

enum vtn_value_type {
   vtn_value_type_invalid = 0,
   vtn_value_type_undef,
   vtn_value_type_string,
   vtn_value_type_decoration_group,
   vtn_value_type_ssa,
   vtn_value_type_deref,
};

struct vtn_value {
   enum vtn_value_type value_type;
   const char *name;
   struct vtn_decoration *decoration;
   union {
      void *ptr;
      char *str;
      nir_ssa_def *ssa;
      nir_deref_var *deref;
   };
};

struct vtn_decoration {
   struct vtn_decoration *next;
   const uint32_t *literals;
   struct vtn_value *group;
   SpvDecoration decoration;
};

struct vtn_builder {
   nir_shader *shader;
   nir_function_impl *impl;

   unsigned value_id_bound;
   struct vtn_value *values;
};

static void
vtn_push_value(struct vtn_builder *b, uint32_t value_id,
               enum vtn_value_type value_type, void *ptr)
{
   assert(value_id < b->value_id_bound);
   assert(b->values[value_id].value_type == vtn_value_type_invalid);

   b->values[value_id].value_type = value_type;
   b->values[value_id].ptr = ptr;
}

static void
vtn_push_token(struct vtn_builder *b, uint32_t value_id,
               enum vtn_value_type value_type)
{
   vtn_push_value(b, value_id, value_type, NULL);
}

static char *
vtn_string_literal(struct vtn_builder *b, const uint32_t *words,
                   unsigned word_count)
{
   return ralloc_strndup(b, (char *)words, (word_count - 2) * sizeof(*words));
}

typedef void (*decoration_foreach_cb)(struct vtn_builder *,
                                      struct vtn_value *,
                                      const struct vtn_decoration *,
                                      void *);

static void
_foreach_decoration_helper(struct vtn_builder *b,
                           struct vtn_value *base_value,
                           struct vtn_value *value,
                           decoration_foreach_cb cb, void *data)
{
   for (struct vtn_decoration *dec = value->decoration; dec; dec = dec->next) {
      if (dec->group) {
         assert(dec->group->value_type == vtn_value_type_decoration_group);
         _foreach_decoration_helper(b, base_value, dec->group, cb, data);
      } else {
         cb(b, base_value, dec, data);
      }
   }
}

/** Iterates (recursively if needed) over all of the decorations on a value
 *
 * This function iterates over all of the decorations applied to a given
 * value.  If it encounters a decoration group, it recurses into the group
 * and iterates over all of those decorations as well.
 */
static void
vtn_foreach_decoration(struct vtn_builder *b, struct vtn_value *value,
                       decoration_foreach_cb cb, void *data)
{
   _foreach_decoration_helper(b, value, value, cb, data);
}

static void
vtn_handle_decoration(struct vtn_builder *b, SpvOp opcode,
                      const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpDecorationGroup:
      vtn_push_token(b, w[1], vtn_value_type_undef);
      break;

   case SpvOpDecorate: {
      struct vtn_value *val = &b->values[w[1]];

      struct vtn_decoration *dec = rzalloc(b, struct vtn_decoration);
      dec->decoration = w[2];
      dec->literals = &w[3];

      /* Link into the list */
      dec->next = val->decoration;
      val->decoration = dec;
      break;
   }

   case SpvOpGroupDecorate: {
      struct vtn_value *group = &b->values[w[1]];
      assert(group->value_type == vtn_value_type_decoration_group);

      for (unsigned i = 2; i < count; i++) {
         struct vtn_value *val = &b->values[w[i]];
         struct vtn_decoration *dec = rzalloc(b, struct vtn_decoration);
         dec->group = group;

         /* Link into the list */
         dec->next = val->decoration;
         val->decoration = dec;
      }
      break;
   }

   case SpvOpGroupMemberDecorate:
      assert(!"Bad instruction.  Khronos Bug #13513");
      break;

   default:
      unreachable("Unhandled opcode");
   }
}

static void
vtn_handle_type(struct vtn_builder *b, SpvOp opcode,
                const uint32_t *w, unsigned count)
{
   unreachable("Unhandled opcode");
}

static void
vtn_handle_constant(struct vtn_builder *b, SpvOp opcode,
                    const uint32_t *w, unsigned count)
{
   unreachable("Unhandled opcode");
}

static void
vtn_handle_variables(struct vtn_builder *b, SpvOp opcode,
                     const uint32_t *w, unsigned count)
{
   unreachable("Unhandled opcode");
}

static void
vtn_handle_texture(struct vtn_builder *b, SpvOp opcode,
                   const uint32_t *w, unsigned count)
{
   unreachable("Unhandled opcode");
}

static void
vtn_handle_alu(struct vtn_builder *b, SpvOp opcode,
               const uint32_t *w, unsigned count)
{
   unreachable("Unhandled opcode");
}

static void
vtn_handle_instruction(struct vtn_builder *b, SpvOp opcode,
                       const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpSource:
   case SpvOpSourceExtension:
   case SpvOpMemberName:
   case SpvOpLine:
      /* Unhandled, but these are for debug so that's ok. */
      break;

   case SpvOpName:
      b->values[w[1]].name = vtn_string_literal(b, &w[2], count - 2);
      break;

   case SpvOpString:
      vtn_push_value(b, w[1], vtn_value_type_string,
                     vtn_string_literal(b, &w[2], count - 2));
      break;

   case SpvOpUndef:
      vtn_push_token(b, w[2], vtn_value_type_undef);
      break;

   case SpvOpTypeVoid:
   case SpvOpTypeBool:
   case SpvOpTypeInt:
   case SpvOpTypeFloat:
   case SpvOpTypeVector:
   case SpvOpTypeMatrix:
   case SpvOpTypeSampler:
   case SpvOpTypeArray:
   case SpvOpTypeRuntimeArray:
   case SpvOpTypeStruct:
   case SpvOpTypeOpaque:
   case SpvOpTypePointer:
   case SpvOpTypeFunction:
   case SpvOpTypeEvent:
   case SpvOpTypeDeviceEvent:
   case SpvOpTypeReserveId:
   case SpvOpTypeQueue:
   case SpvOpTypePipe:
      vtn_handle_type(b, opcode, w, count);
      break;

   case SpvOpConstantTrue:
   case SpvOpConstantFalse:
   case SpvOpConstant:
   case SpvOpConstantComposite:
   case SpvOpConstantSampler:
   case SpvOpConstantNullPointer:
   case SpvOpConstantNullObject:
   case SpvOpSpecConstantTrue:
   case SpvOpSpecConstantFalse:
   case SpvOpSpecConstant:
   case SpvOpSpecConstantComposite:
      vtn_handle_constant(b, opcode, w, count);
      break;

   case SpvOpVariable:
   case SpvOpVariableArray:
   case SpvOpLoad:
   case SpvOpStore:
   case SpvOpCopyMemory:
   case SpvOpCopyMemorySized:
   case SpvOpAccessChain:
   case SpvOpInBoundsAccessChain:
   case SpvOpArrayLength:
   case SpvOpImagePointer:
      vtn_handle_variables(b, opcode, w, count);
      break;

   case SpvOpDecorationGroup:
   case SpvOpDecorate:
   case SpvOpMemberDecorate:
   case SpvOpGroupDecorate:
   case SpvOpGroupMemberDecorate:
      vtn_handle_decoration(b, opcode, w, count);
      break;

   case SpvOpTextureSample:
   case SpvOpTextureSampleDref:
   case SpvOpTextureSampleLod:
   case SpvOpTextureSampleProj:
   case SpvOpTextureSampleGrad:
   case SpvOpTextureSampleOffset:
   case SpvOpTextureSampleProjLod:
   case SpvOpTextureSampleProjGrad:
   case SpvOpTextureSampleLodOffset:
   case SpvOpTextureSampleProjOffset:
   case SpvOpTextureSampleGradOffset:
   case SpvOpTextureSampleProjLodOffset:
   case SpvOpTextureSampleProjGradOffset:
   case SpvOpTextureFetchTexelLod:
   case SpvOpTextureFetchTexelOffset:
   case SpvOpTextureFetchSample:
   case SpvOpTextureFetchTexel:
   case SpvOpTextureGather:
   case SpvOpTextureGatherOffset:
   case SpvOpTextureGatherOffsets:
   case SpvOpTextureQuerySizeLod:
   case SpvOpTextureQuerySize:
   case SpvOpTextureQueryLod:
   case SpvOpTextureQueryLevels:
   case SpvOpTextureQuerySamples:
      vtn_handle_texture(b, opcode, w, count);
      break;

   case SpvOpSNegate:
   case SpvOpFNegate:
   case SpvOpNot:
   case SpvOpAny:
   case SpvOpAll:
   case SpvOpConvertFToU:
   case SpvOpConvertFToS:
   case SpvOpConvertSToF:
   case SpvOpConvertUToF:
   case SpvOpUConvert:
   case SpvOpSConvert:
   case SpvOpFConvert:
   case SpvOpConvertPtrToU:
   case SpvOpConvertUToPtr:
   case SpvOpPtrCastToGeneric:
   case SpvOpGenericCastToPtr:
   case SpvOpBitcast:
   case SpvOpTranspose:
   case SpvOpIsNan:
   case SpvOpIsInf:
   case SpvOpIsFinite:
   case SpvOpIsNormal:
   case SpvOpSignBitSet:
   case SpvOpLessOrGreater:
   case SpvOpOrdered:
   case SpvOpUnordered:
   case SpvOpIAdd:
   case SpvOpFAdd:
   case SpvOpISub:
   case SpvOpFSub:
   case SpvOpIMul:
   case SpvOpFMul:
   case SpvOpUDiv:
   case SpvOpSDiv:
   case SpvOpFDiv:
   case SpvOpUMod:
   case SpvOpSRem:
   case SpvOpSMod:
   case SpvOpFRem:
   case SpvOpFMod:
   case SpvOpVectorTimesScalar:
   case SpvOpMatrixTimesScalar:
   case SpvOpVectorTimesMatrix:
   case SpvOpMatrixTimesVector:
   case SpvOpMatrixTimesMatrix:
   case SpvOpOuterProduct:
   case SpvOpDot:
   case SpvOpShiftRightLogical:
   case SpvOpShiftRightArithmetic:
   case SpvOpShiftLeftLogical:
   case SpvOpLogicalOr:
   case SpvOpLogicalXor:
   case SpvOpLogicalAnd:
   case SpvOpBitwiseOr:
   case SpvOpBitwiseXor:
   case SpvOpBitwiseAnd:
   case SpvOpSelect:
   case SpvOpIEqual:
   case SpvOpFOrdEqual:
   case SpvOpFUnordEqual:
   case SpvOpINotEqual:
   case SpvOpFOrdNotEqual:
   case SpvOpFUnordNotEqual:
   case SpvOpULessThan:
   case SpvOpSLessThan:
   case SpvOpFOrdLessThan:
   case SpvOpFUnordLessThan:
   case SpvOpUGreaterThan:
   case SpvOpSGreaterThan:
   case SpvOpFOrdGreaterThan:
   case SpvOpFUnordGreaterThan:
   case SpvOpULessThanEqual:
   case SpvOpSLessThanEqual:
   case SpvOpFOrdLessThanEqual:
   case SpvOpFUnordLessThanEqual:
   case SpvOpUGreaterThanEqual:
   case SpvOpSGreaterThanEqual:
   case SpvOpFOrdGreaterThanEqual:
   case SpvOpFUnordGreaterThanEqual:
   case SpvOpDPdx:
   case SpvOpDPdy:
   case SpvOpFwidth:
   case SpvOpDPdxFine:
   case SpvOpDPdyFine:
   case SpvOpFwidthFine:
   case SpvOpDPdxCoarse:
   case SpvOpDPdyCoarse:
   case SpvOpFwidthCoarse:
      vtn_handle_alu(b, opcode, w, count);
      break;

   default:
      unreachable("Unhandled opcode");
   }
}

nir_shader *
spirv_to_nir(const uint32_t *words, size_t word_count,
             gl_shader_stage stage,
             const nir_shader_compiler_options *options)
{
   /* Handle the SPIR-V header (first 4 dwords)  */
   assert(word_count > 5);

   assert(words[0] == SpvMagicNumber);
   assert(words[1] == 99);
   /* words[2] == generator magic */
   unsigned value_id_bound = words[3];
   assert(words[4] == 0);

   words+= 5;

   nir_shader *shader = nir_shader_create(NULL, stage, options);

   /* Initialize the stn_builder object */
   struct vtn_builder *b = rzalloc(NULL, struct vtn_builder);
   b->shader = shader;
   b->value_id_bound = value_id_bound;
   b->values = ralloc_array(b, struct vtn_value, value_id_bound);

   /* Start handling instructions */
   const uint32_t *word_end = words + word_count;
   while (words < word_end) {
      SpvOp opcode = words[0] & SpvOpCodeMask;
      unsigned count = words[0] >> SpvWordCountShift;
      assert(words + count <= word_end);

      vtn_handle_instruction(b, opcode, words, count);

      words += count;
   }

   ralloc_free(b);

   return shader;
}
