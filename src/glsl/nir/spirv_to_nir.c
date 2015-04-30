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
#include "nir_vla.h"
#include "spirv.h"

struct vtn_decoration;

enum vtn_value_type {
   vtn_value_type_invalid = 0,
   vtn_value_type_undef,
   vtn_value_type_string,
   vtn_value_type_decoration_group,
   vtn_value_type_type,
   vtn_value_type_constant,
   vtn_value_type_variable,
   vtn_value_type_function,
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
      const struct glsl_type *type;
      nir_constant *constant;
      nir_variable *var;
      nir_function_impl *impl;
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
   struct exec_list *cf_list;

   unsigned value_id_bound;
   struct vtn_value *values;

   SpvExecutionModel execution_model;
   struct vtn_value *entry_point;
};

static struct vtn_value *
vtn_push_value(struct vtn_builder *b, uint32_t value_id,
               enum vtn_value_type value_type)
{
   assert(value_id < b->value_id_bound);
   assert(b->values[value_id].value_type == vtn_value_type_invalid);

   b->values[value_id].value_type = value_type;

   return &b->values[value_id];
}

static struct vtn_value *
vtn_value(struct vtn_builder *b, uint32_t value_id,
          enum vtn_value_type value_type)
{
   assert(value_id < b->value_id_bound);
   assert(b->values[value_id].value_type == value_type);
   return &b->values[value_id];
}

static char *
vtn_string_literal(struct vtn_builder *b, const uint32_t *words,
                   unsigned word_count)
{
   return ralloc_strndup(b, (char *)words, (word_count - 2) * sizeof(*words));
}

static void
vtn_handle_extension(struct vtn_builder *b, SpvOp opcode,
                     const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpExtInstImport:
      /* Do nothing for the moment */
      break;

   case SpvOpExtInst:
   default:
      unreachable("Unhandled opcode");
   }
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
      vtn_push_value(b, w[1], vtn_value_type_undef);
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

static const struct glsl_type *
vtn_handle_type(struct vtn_builder *b, SpvOp opcode,
                const uint32_t *args, unsigned count)
{
   switch (opcode) {
   case SpvOpTypeVoid:
      return glsl_void_type();
   case SpvOpTypeBool:
      return glsl_bool_type();
   case SpvOpTypeInt:
      return glsl_int_type();
   case SpvOpTypeFloat:
      return glsl_float_type();

   case SpvOpTypeVector: {
      const struct glsl_type *base = b->values[args[0]].type;
      unsigned elems = args[1];

      assert(glsl_type_is_scalar(base));
      return glsl_vector_type(glsl_get_base_type(base), elems);
   }

   case SpvOpTypeMatrix: {
      const struct glsl_type *base = b->values[args[0]].type;
      unsigned columns = args[1];

      assert(glsl_type_is_vector(base));
      return glsl_matrix_type(glsl_get_base_type(base),
                              glsl_get_vector_elements(base),
                              columns);
   }

   case SpvOpTypeArray:
      return glsl_array_type(b->values[args[0]].type, args[1]);

   case SpvOpTypeStruct: {
      NIR_VLA(struct glsl_struct_field, fields, count);
      for (unsigned i = 0; i < count; i++) {
         /* TODO: Handle decorators */
         fields[i].type = b->values[args[i]].type;
         fields[i].name = ralloc_asprintf(b, "field%d", i);
         fields[i].location = -1;
         fields[i].interpolation = 0;
         fields[i].centroid = 0;
         fields[i].sample = 0;
         fields[i].matrix_layout = 2;
         fields[i].stream = -1;
      }
      return glsl_struct_type(fields, count, "struct");
   }

   case SpvOpTypeFunction: {
      const struct glsl_type *return_type = b->values[args[0]].type;
      NIR_VLA(struct glsl_function_param, params, count - 1);
      for (unsigned i = 1; i < count; i++) {
         params[i - 1].type = b->values[args[i]].type;

         /* FIXME: */
         params[i - 1].in = true;
         params[i - 1].out = true;
      }
      return glsl_function_type(return_type, params, count - 1);
   }

   case SpvOpTypePointer:
      /* FIXME:  For now, we'll just do the really lame thing and return
       * the same type.  The validator should ensure that the proper number
       * of dereferences happen
       */
      return b->values[args[0]].type;

   case SpvOpTypeSampler:
   case SpvOpTypeRuntimeArray:
   case SpvOpTypeOpaque:
   case SpvOpTypeEvent:
   case SpvOpTypeDeviceEvent:
   case SpvOpTypeReserveId:
   case SpvOpTypeQueue:
   case SpvOpTypePipe:
   default:
      unreachable("Unhandled opcode");
   }
}

static void
vtn_handle_constant(struct vtn_builder *b, SpvOp opcode,
                    const uint32_t *w, unsigned count)
{
   const struct glsl_type *type = vtn_value(b, w[1], vtn_value_type_type)->type;
   nir_constant *constant = ralloc(b, nir_constant);
   switch (opcode) {
   case SpvOpConstantTrue:
      assert(type == glsl_bool_type());
      constant->value.u[0] = NIR_TRUE;
      break;
   case SpvOpConstantFalse:
      assert(type == glsl_bool_type());
      constant->value.u[0] = NIR_FALSE;
      break;
   case SpvOpConstant:
      assert(glsl_type_is_scalar(type));
      constant->value.u[0] = w[3];
      break;
   case SpvOpConstantComposite: {
      unsigned elem_count = count - 3;
      nir_constant **elems = ralloc_array(b, nir_constant *, elem_count);
      for (unsigned i = 0; i < elem_count; i++)
         elems[i] = vtn_value(b, w[i + 3], vtn_value_type_constant)->constant;

      switch (glsl_get_base_type(type)) {
      case GLSL_TYPE_UINT:
      case GLSL_TYPE_INT:
      case GLSL_TYPE_FLOAT:
      case GLSL_TYPE_BOOL:
         if (glsl_type_is_matrix(type)) {
            unsigned rows = glsl_get_vector_elements(type);
            assert(glsl_get_matrix_columns(type) == elem_count);
            for (unsigned i = 0; i < elem_count; i++)
               for (unsigned j = 0; j < rows; j++)
                  constant->value.u[rows * i + j] = elems[i]->value.u[j];
         } else {
            assert(glsl_type_is_vector(type));
            assert(glsl_get_vector_elements(type) == elem_count);
            for (unsigned i = 0; i < elem_count; i++)
               constant->value.u[i] = elems[i]->value.u[0];
         }
         ralloc_free(elems);
         break;

      case GLSL_TYPE_STRUCT:
      case GLSL_TYPE_ARRAY:
         constant->elements = elems;
         break;

      default:
         unreachable("Unsupported type for constants");
      }
      break;
   }

   default:
      unreachable("Unhandled opcode");
   }
   vtn_push_value(b, w[2], vtn_value_type_constant)->constant = constant;
}

static void
var_decoration_cb(struct vtn_builder *b, struct vtn_value *val,
                  const struct vtn_decoration *dec, void *unused)
{
   assert(val->value_type == vtn_value_type_variable);
   nir_variable *var = val->var;
   switch (dec->decoration) {
   case SpvDecorationPrecisionLow:
   case SpvDecorationPrecisionMedium:
   case SpvDecorationPrecisionHigh:
      break; /* FIXME: Do nothing with these for now. */
   case SpvDecorationSmooth:
      var->data.interpolation = INTERP_QUALIFIER_SMOOTH;
      break;
   case SpvDecorationNoperspective:
      var->data.interpolation = INTERP_QUALIFIER_NOPERSPECTIVE;
      break;
   case SpvDecorationFlat:
      var->data.interpolation = INTERP_QUALIFIER_FLAT;
      break;
   case SpvDecorationCentroid:
      var->data.centroid = true;
      break;
   case SpvDecorationSample:
      var->data.sample = true;
      break;
   case SpvDecorationInvariant:
      var->data.invariant = true;
      break;
   case SpvDecorationConstant:
      assert(var->constant_initializer != NULL);
      var->data.read_only = true;
      break;
   case SpvDecorationNonwritable:
      var->data.read_only = true;
      break;
   case SpvDecorationLocation:
      var->data.explicit_location = true;
      var->data.location = dec->literals[0];
      break;
   case SpvDecorationComponent:
      var->data.location_frac = dec->literals[0];
      break;
   case SpvDecorationIndex:
      var->data.explicit_index = true;
      var->data.index = dec->literals[0];
      break;
   case SpvDecorationBinding:
      var->data.explicit_binding = true;
      var->data.binding = dec->literals[0];
      break;
   case SpvDecorationBlock:
   case SpvDecorationBufferBlock:
   case SpvDecorationRowMajor:
   case SpvDecorationColMajor:
   case SpvDecorationGLSLShared:
   case SpvDecorationGLSLStd140:
   case SpvDecorationGLSLStd430:
   case SpvDecorationGLSLPacked:
   case SpvDecorationPatch:
   case SpvDecorationRestrict:
   case SpvDecorationAliased:
   case SpvDecorationVolatile:
   case SpvDecorationCoherent:
   case SpvDecorationNonreadable:
   case SpvDecorationUniform:
      /* This is really nice but we have no use for it right now. */
   case SpvDecorationNoStaticUse:
   case SpvDecorationCPacked:
   case SpvDecorationSaturatedConversion:
   case SpvDecorationStream:
   case SpvDecorationDescriptorSet:
   case SpvDecorationOffset:
   case SpvDecorationAlignment:
   case SpvDecorationXfbBuffer:
   case SpvDecorationStride:
   case SpvDecorationBuiltIn:
   case SpvDecorationFuncParamAttr:
   case SpvDecorationFPRoundingMode:
   case SpvDecorationFPFastMathMode:
   case SpvDecorationLinkageAttributes:
   case SpvDecorationSpecId:
   default:
      unreachable("Unhandled variable decoration");
   }
}

static void
vtn_handle_variables(struct vtn_builder *b, SpvOp opcode,
                     const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpVariable: {
      const struct glsl_type *type =
         vtn_value(b, w[1], vtn_value_type_type)->type;
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_variable);

      nir_variable *var = ralloc(b->shader, nir_variable);
      val->var = var;

      var->type = type;
      var->name = ralloc_strdup(var, val->name);

      switch ((SpvStorageClass)w[3]) {
      case SpvStorageClassUniformConstant:
         var->data.mode = nir_var_uniform;
         var->data.read_only = true;
         break;
      case SpvStorageClassInput:
         var->data.mode = nir_var_shader_in;
         var->data.read_only = true;
         break;
      case SpvStorageClassOutput:
         var->data.mode = nir_var_shader_out;
         break;
      case SpvStorageClassPrivateGlobal:
         var->data.mode = nir_var_global;
         break;
      case SpvStorageClassFunction:
         var->data.mode = nir_var_local;
         break;
      case SpvStorageClassUniform:
      case SpvStorageClassWorkgroupLocal:
      case SpvStorageClassWorkgroupGlobal:
      case SpvStorageClassGeneric:
      case SpvStorageClassPrivate:
      case SpvStorageClassAtomicCounter:
      default:
         unreachable("Unhandled variable storage class");
      }

      if (count > 4) {
         assert(count == 5);
         var->constant_initializer =
            vtn_value(b, w[4], vtn_value_type_constant)->constant;
      }

      vtn_foreach_decoration(b, val, var_decoration_cb, NULL);
      break;
   }

   case SpvOpVariableArray:
   case SpvOpLoad:
   case SpvOpStore:
   case SpvOpCopyMemory:
   case SpvOpCopyMemorySized:
   case SpvOpAccessChain:
   case SpvOpInBoundsAccessChain:
   case SpvOpArrayLength:
   case SpvOpImagePointer:
   default:
      unreachable("Unhandled opcode");
   }
}

static void
vtn_handle_functions(struct vtn_builder *b, SpvOp opcode,
                     const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpFunction: {
      assert(b->impl == NULL);

      const struct glsl_type *result_type =
         vtn_value(b, w[1], vtn_value_type_type)->type;
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_function);
      const struct glsl_type *func_type =
         vtn_value(b, w[4], vtn_value_type_type)->type;

      assert(glsl_get_function_return_type(func_type) == result_type);

      nir_function *func =
         nir_function_create(b->shader, ralloc_strdup(b->shader, val->name));

      nir_function_overload *overload = nir_function_overload_create(func);
      overload->num_params = glsl_get_length(func_type);
      overload->params = ralloc_array(overload, nir_parameter,
                                      overload->num_params);
      for (unsigned i = 0; i < overload->num_params; i++) {
         const struct glsl_function_param *param =
            glsl_get_function_param(func_type, i);
         overload->params[i].type = param->type;
         if (param->in) {
            if (param->out) {
               overload->params[i].param_type = nir_parameter_inout;
            } else {
               overload->params[i].param_type = nir_parameter_in;
            }
         } else {
            if (param->out) {
               overload->params[i].param_type = nir_parameter_out;
            } else {
               assert(!"Parameter is neither in nor out");
            }
         }
      }

      val->impl = b->impl = nir_function_impl_create(overload);
      b->cf_list = &b->impl->body;

      break;
   }
   case SpvOpFunctionEnd:
      b->impl = NULL;
      break;
   case SpvOpFunctionParameter:
   case SpvOpFunctionCall:
   default:
      unreachable("Unhandled opcode");
   }
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
   case SpvOpExtension:
      /* Unhandled, but these are for debug so that's ok. */
      break;

   case SpvOpName:
      b->values[w[1]].name = vtn_string_literal(b, &w[2], count - 2);
      break;

   case SpvOpString:
      vtn_push_value(b, w[1], vtn_value_type_string)->str =
         vtn_string_literal(b, &w[2], count - 2);
      break;

   case SpvOpUndef:
      vtn_push_value(b, w[2], vtn_value_type_undef);
      break;

   case SpvOpMemoryModel:
      assert(w[1] == SpvAddressingModelLogical);
      assert(w[2] == SpvMemoryModelGLSL450);
      break;

   case SpvOpEntryPoint:
      assert(b->entry_point == NULL);
      b->entry_point = &b->values[w[2]];
      b->execution_model = w[1];
      break;

   case SpvOpExtInstImport:
   case SpvOpExtInst:
      vtn_handle_extension(b, opcode, w, count);
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
      vtn_push_value(b, w[1], vtn_value_type_type)->type =
         vtn_handle_type(b, opcode, &w[2], count - 2);
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

   case SpvOpFunction:
   case SpvOpFunctionEnd:
   case SpvOpFunctionParameter:
   case SpvOpFunctionCall:
      vtn_handle_functions(b, opcode, w, count);
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
