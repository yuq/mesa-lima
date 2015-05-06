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

#include "spirv_to_nir_private.h"
#include "nir_vla.h"

nir_ssa_def *
vtn_ssa_value(struct vtn_builder *b, uint32_t value_id)
{
   struct vtn_value *val = vtn_untyped_value(b, value_id);
   switch (val->value_type) {
   case vtn_value_type_constant: {
      assert(glsl_type_is_vector_or_scalar(val->type));
      unsigned num_components = glsl_get_vector_elements(val->type);
      nir_load_const_instr *load =
         nir_load_const_instr_create(b->shader, num_components);

      for (unsigned i = 0; i < num_components; i++)
         load->value.u[0] = val->constant->value.u[0];

      nir_builder_instr_insert(&b->nb, &load->instr);
      return &load->def;
   }

   case vtn_value_type_ssa:
      return val->ssa;
   default:
      unreachable("Invalid type for an SSA value");
   }
}

static char *
vtn_string_literal(struct vtn_builder *b, const uint32_t *words,
                   unsigned word_count)
{
   return ralloc_strndup(b, (char *)words, word_count * sizeof(*words));
}

static const uint32_t *
vtn_foreach_instruction(struct vtn_builder *b, const uint32_t *start,
                        const uint32_t *end, vtn_instruction_handler handler)
{
   const uint32_t *w = start;
   while (w < end) {
      SpvOp opcode = w[0] & SpvOpCodeMask;
      unsigned count = w[0] >> SpvWordCountShift;
      assert(count >= 1 && w + count <= end);

      if (!handler(b, opcode, w, count))
         return w;

      w += count;
   }
   assert(w == end);
   return w;
}

static void
vtn_handle_extension(struct vtn_builder *b, SpvOp opcode,
                     const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpExtInstImport: {
      struct vtn_value *val = vtn_push_value(b, w[1], vtn_value_type_extension);
      if (strcmp((const char *)&w[2], "GLSL.std.450") == 0) {
         val->ext_handler = vtn_handle_glsl450_instruction;
      } else {
         assert(!"Unsupported extension");
      }
      break;
   }

   case SpvOpExtInst: {
      struct vtn_value *val = vtn_value(b, w[3], vtn_value_type_extension);
      bool handled = val->ext_handler(b, w[4], w, count);
      (void)handled;
      assert(handled);
      break;
   }

   default:
      unreachable("Unhandled opcode");
   }
}

static void
_foreach_decoration_helper(struct vtn_builder *b,
                           struct vtn_value *base_value,
                           struct vtn_value *value,
                           vtn_decoration_foreach_cb cb, void *data)
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
void
vtn_foreach_decoration(struct vtn_builder *b, struct vtn_value *value,
                       vtn_decoration_foreach_cb cb, void *data)
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
      const struct glsl_type *base =
         vtn_value(b, args[0], vtn_value_type_type)->type;
      unsigned elems = args[1];

      assert(glsl_type_is_scalar(base));
      return glsl_vector_type(glsl_get_base_type(base), elems);
   }

   case SpvOpTypeMatrix: {
      const struct glsl_type *base =
         vtn_value(b, args[0], vtn_value_type_type)->type;
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
         fields[i].type = vtn_value(b, args[i], vtn_value_type_type)->type;
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
         params[i - 1].type = vtn_value(b, args[i], vtn_value_type_type)->type;

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
      return vtn_value(b, args[1], vtn_value_type_type)->type;

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
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_constant);
   val->type = vtn_value(b, w[1], vtn_value_type_type)->type;
   val->constant = ralloc(b, nir_constant);
   switch (opcode) {
   case SpvOpConstantTrue:
      assert(val->type == glsl_bool_type());
      val->constant->value.u[0] = NIR_TRUE;
      break;
   case SpvOpConstantFalse:
      assert(val->type == glsl_bool_type());
      val->constant->value.u[0] = NIR_FALSE;
      break;
   case SpvOpConstant:
      assert(glsl_type_is_scalar(val->type));
      val->constant->value.u[0] = w[3];
      break;
   case SpvOpConstantComposite: {
      unsigned elem_count = count - 3;
      nir_constant **elems = ralloc_array(b, nir_constant *, elem_count);
      for (unsigned i = 0; i < elem_count; i++)
         elems[i] = vtn_value(b, w[i + 3], vtn_value_type_constant)->constant;

      switch (glsl_get_base_type(val->type)) {
      case GLSL_TYPE_UINT:
      case GLSL_TYPE_INT:
      case GLSL_TYPE_FLOAT:
      case GLSL_TYPE_BOOL:
         if (glsl_type_is_matrix(val->type)) {
            unsigned rows = glsl_get_vector_elements(val->type);
            assert(glsl_get_matrix_columns(val->type) == elem_count);
            for (unsigned i = 0; i < elem_count; i++)
               for (unsigned j = 0; j < rows; j++)
                  val->constant->value.u[rows * i + j] = elems[i]->value.u[j];
         } else {
            assert(glsl_type_is_vector(val->type));
            assert(glsl_get_vector_elements(val->type) == elem_count);
            for (unsigned i = 0; i < elem_count; i++)
               val->constant->value.u[i] = elems[i]->value.u[0];
         }
         ralloc_free(elems);
         break;

      case GLSL_TYPE_STRUCT:
      case GLSL_TYPE_ARRAY:
         ralloc_steal(val->constant, elems);
         val->constant->elements = elems;
         break;

      default:
         unreachable("Unsupported type for constants");
      }
      break;
   }

   default:
      unreachable("Unhandled opcode");
   }
}

static void
var_decoration_cb(struct vtn_builder *b, struct vtn_value *val,
                  const struct vtn_decoration *dec, void *void_var)
{
   assert(val->value_type == vtn_value_type_deref);
   assert(val->deref->deref.child == NULL);
   assert(val->deref->var == void_var);

   nir_variable *var = void_var;
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
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_deref);

      nir_variable *var = ralloc(b->shader, nir_variable);

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

      if (var->data.mode == nir_var_local) {
         exec_list_push_tail(&b->impl->locals, &var->node);
      } else {
         exec_list_push_tail(&b->shader->globals, &var->node);
      }

      val->deref = nir_deref_var_create(b->shader, var);

      vtn_foreach_decoration(b, val, var_decoration_cb, var);
      break;
   }

   case SpvOpAccessChain:
   case SpvOpInBoundsAccessChain: {
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_deref);
      nir_deref_var *base = vtn_value(b, w[3], vtn_value_type_deref)->deref;
      val->deref = nir_deref_as_var(nir_copy_deref(b, &base->deref));

      nir_deref *tail = &val->deref->deref;
      while (tail->child)
         tail = tail->child;

      for (unsigned i = 0; i < count - 4; i++) {
         assert(w[i + 4] < b->value_id_bound);
         struct vtn_value *idx_val = &b->values[w[i + 4]];

         enum glsl_base_type base_type = glsl_get_base_type(tail->type);
         switch (base_type) {
         case GLSL_TYPE_UINT:
         case GLSL_TYPE_INT:
         case GLSL_TYPE_FLOAT:
         case GLSL_TYPE_DOUBLE:
         case GLSL_TYPE_BOOL:
         case GLSL_TYPE_ARRAY: {
            nir_deref_array *deref_arr = nir_deref_array_create(b);
            if (base_type == GLSL_TYPE_ARRAY) {
               deref_arr->deref.type = glsl_get_array_element(tail->type);
            } else if (glsl_type_is_matrix(tail->type)) {
               deref_arr->deref.type = glsl_get_column_type(tail->type);
            } else {
               assert(glsl_type_is_vector(tail->type));
               deref_arr->deref.type = glsl_scalar_type(base_type);
            }

            if (idx_val->value_type == vtn_value_type_constant) {
               unsigned idx = idx_val->constant->value.u[0];
               deref_arr->deref_array_type = nir_deref_array_type_direct;
               deref_arr->base_offset = idx;
            } else {
               assert(idx_val->value_type == vtn_value_type_ssa);
               deref_arr->deref_array_type = nir_deref_array_type_indirect;
               deref_arr->base_offset = 0;
               deref_arr->indirect = nir_src_for_ssa(vtn_ssa_value(b, w[1]));
            }
            tail->child = &deref_arr->deref;
            break;
         }

         case GLSL_TYPE_STRUCT: {
            assert(idx_val->value_type == vtn_value_type_constant);
            unsigned idx = idx_val->constant->value.u[0];
            nir_deref_struct *deref_struct = nir_deref_struct_create(b, idx);
            deref_struct->deref.type = glsl_get_struct_field(tail->type, idx);
            tail->child = &deref_struct->deref;
            break;
         }
         default:
            unreachable("Invalid type for deref");
         }
         tail = tail->child;
      }
      break;
   }

   case SpvOpCopyMemory: {
      nir_deref_var *dest = vtn_value(b, w[1], vtn_value_type_deref)->deref;
      nir_deref_var *src = vtn_value(b, w[2], vtn_value_type_deref)->deref;

      nir_intrinsic_instr *copy =
         nir_intrinsic_instr_create(b->shader, nir_intrinsic_copy_var);
      copy->variables[0] = nir_deref_as_var(nir_copy_deref(copy, &dest->deref));
      copy->variables[1] = nir_deref_as_var(nir_copy_deref(copy, &src->deref));

      nir_builder_instr_insert(&b->nb, &copy->instr);
      break;
   }

   case SpvOpLoad: {
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
      nir_deref_var *src = vtn_value(b, w[3], vtn_value_type_deref)->deref;
      const struct glsl_type *src_type = nir_deref_tail(&src->deref)->type;
      assert(glsl_type_is_vector_or_scalar(src_type));

      nir_intrinsic_instr *load =
         nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_var);
      load->variables[0] = nir_deref_as_var(nir_copy_deref(load, &src->deref));
      load->num_components = glsl_get_vector_elements(src_type);
      nir_ssa_dest_init(&load->instr, &load->dest, load->num_components,
                        val->name);

      nir_builder_instr_insert(&b->nb, &load->instr);
      val->type = src_type;
      val->ssa = &load->dest.ssa;
      break;
   }

   case SpvOpStore: {
      nir_deref_var *dest = vtn_value(b, w[1], vtn_value_type_deref)->deref;
      const struct glsl_type *dest_type = nir_deref_tail(&dest->deref)->type;
      struct vtn_value *src_val = vtn_untyped_value(b, w[2]);
      if (src_val->value_type == vtn_value_type_ssa) {
         assert(glsl_type_is_vector_or_scalar(dest_type));
         nir_intrinsic_instr *store =
            nir_intrinsic_instr_create(b->shader, nir_intrinsic_store_var);
         store->src[0] = nir_src_for_ssa(src_val->ssa);
         store->variables[0] = nir_deref_as_var(nir_copy_deref(store, &dest->deref));
         store->num_components = glsl_get_vector_elements(dest_type);

         nir_builder_instr_insert(&b->nb, &store->instr);
      } else {
         assert(src_val->value_type == vtn_value_type_constant);

         nir_variable *const_tmp = rzalloc(b->shader, nir_variable);
         const_tmp->type = dest_type;
         const_tmp->name = "const_temp";
         const_tmp->data.mode = nir_var_local;
         const_tmp->data.read_only = true;
         exec_list_push_tail(&b->impl->locals, &const_tmp->node);

         nir_intrinsic_instr *copy =
            nir_intrinsic_instr_create(b->shader, nir_intrinsic_copy_var);
         copy->variables[0] = nir_deref_as_var(nir_copy_deref(copy, &dest->deref));
         copy->variables[1] = nir_deref_var_create(copy, const_tmp);

         nir_builder_instr_insert(&b->nb, &copy->instr);
      }
      break;
   }

   case SpvOpVariableArray:
   case SpvOpCopyMemorySized:
   case SpvOpArrayLength:
   case SpvOpImagePointer:
   default:
      unreachable("Unhandled opcode");
   }
}

static void
vtn_handle_function_call(struct vtn_builder *b, SpvOp opcode,
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
vtn_handle_matrix_alu(struct vtn_builder *b, SpvOp opcode,
                      const uint32_t *w, unsigned count)
{
   unreachable("Matrix math not handled");
}

static void
vtn_handle_alu(struct vtn_builder *b, SpvOp opcode,
               const uint32_t *w, unsigned count)
{
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   val->type = vtn_value(b, w[1], vtn_value_type_type)->type;

   /* Collect the various SSA sources */
   unsigned num_inputs = count - 3;
   nir_ssa_def *src[4];
   for (unsigned i = 0; i < num_inputs; i++)
      src[i] = vtn_ssa_value(b, w[i + 3]);

   /* Indicates that the first two arguments should be swapped.  This is
    * used for implementing greater-than and less-than-or-equal.
    */
   bool swap = false;

   nir_op op;
   switch (opcode) {
   /* Basic ALU operations */
   case SpvOpSNegate:               op = nir_op_ineg;    break;
   case SpvOpFNegate:               op = nir_op_fneg;    break;
   case SpvOpNot:                   op = nir_op_inot;    break;

   case SpvOpAny:
      switch (src[0]->num_components) {
      case 1:  op = nir_op_imov;    break;
      case 2:  op = nir_op_bany2;   break;
      case 3:  op = nir_op_bany3;   break;
      case 4:  op = nir_op_bany4;   break;
      }
      break;

   case SpvOpAll:
      switch (src[0]->num_components) {
      case 1:  op = nir_op_imov;    break;
      case 2:  op = nir_op_ball2;   break;
      case 3:  op = nir_op_ball3;   break;
      case 4:  op = nir_op_ball4;   break;
      }
      break;

   case SpvOpIAdd:                  op = nir_op_iadd;    break;
   case SpvOpFAdd:                  op = nir_op_fadd;    break;
   case SpvOpISub:                  op = nir_op_isub;    break;
   case SpvOpFSub:                  op = nir_op_fsub;    break;
   case SpvOpIMul:                  op = nir_op_imul;    break;
   case SpvOpFMul:                  op = nir_op_fmul;    break;
   case SpvOpUDiv:                  op = nir_op_udiv;    break;
   case SpvOpSDiv:                  op = nir_op_idiv;    break;
   case SpvOpFDiv:                  op = nir_op_fdiv;    break;
   case SpvOpUMod:                  op = nir_op_umod;    break;
   case SpvOpSMod:                  op = nir_op_umod;    break; /* FIXME? */
   case SpvOpFMod:                  op = nir_op_fmod;    break;

   case SpvOpDot:
      assert(src[0]->num_components == src[1]->num_components);
      switch (src[0]->num_components) {
      case 1:  op = nir_op_fmul;    break;
      case 2:  op = nir_op_fdot2;   break;
      case 3:  op = nir_op_fdot3;   break;
      case 4:  op = nir_op_fdot4;   break;
      }
      break;

   case SpvOpShiftRightLogical:     op = nir_op_ushr;    break;
   case SpvOpShiftRightArithmetic:  op = nir_op_ishr;    break;
   case SpvOpShiftLeftLogical:      op = nir_op_ishl;    break;
   case SpvOpLogicalOr:             op = nir_op_ior;     break;
   case SpvOpLogicalXor:            op = nir_op_ixor;    break;
   case SpvOpLogicalAnd:            op = nir_op_iand;    break;
   case SpvOpBitwiseOr:             op = nir_op_ior;     break;
   case SpvOpBitwiseXor:            op = nir_op_ixor;    break;
   case SpvOpBitwiseAnd:            op = nir_op_iand;    break;
   case SpvOpSelect:                op = nir_op_bcsel;   break;
   case SpvOpIEqual:                op = nir_op_ieq;     break;

   /* Comparisons: (TODO: How do we want to handled ordered/unordered?) */
   case SpvOpFOrdEqual:             op = nir_op_feq;     break;
   case SpvOpFUnordEqual:           op = nir_op_feq;     break;
   case SpvOpINotEqual:             op = nir_op_ine;     break;
   case SpvOpFOrdNotEqual:          op = nir_op_fne;     break;
   case SpvOpFUnordNotEqual:        op = nir_op_fne;     break;
   case SpvOpULessThan:             op = nir_op_ult;     break;
   case SpvOpSLessThan:             op = nir_op_ilt;     break;
   case SpvOpFOrdLessThan:          op = nir_op_flt;     break;
   case SpvOpFUnordLessThan:        op = nir_op_flt;     break;
   case SpvOpUGreaterThan:          op = nir_op_ult;  swap = true;   break;
   case SpvOpSGreaterThan:          op = nir_op_ilt;  swap = true;   break;
   case SpvOpFOrdGreaterThan:       op = nir_op_flt;  swap = true;   break;
   case SpvOpFUnordGreaterThan:     op = nir_op_flt;  swap = true;   break;
   case SpvOpULessThanEqual:        op = nir_op_uge;  swap = true;   break;
   case SpvOpSLessThanEqual:        op = nir_op_ige;  swap = true;   break;
   case SpvOpFOrdLessThanEqual:     op = nir_op_fge;  swap = true;   break;
   case SpvOpFUnordLessThanEqual:   op = nir_op_fge;  swap = true;   break;
   case SpvOpUGreaterThanEqual:     op = nir_op_uge;     break;
   case SpvOpSGreaterThanEqual:     op = nir_op_ige;     break;
   case SpvOpFOrdGreaterThanEqual:  op = nir_op_fge;     break;
   case SpvOpFUnordGreaterThanEqual:op = nir_op_fge;     break;

   /* Conversions: */
   case SpvOpConvertFToU:           op = nir_op_f2u;     break;
   case SpvOpConvertFToS:           op = nir_op_f2i;     break;
   case SpvOpConvertSToF:           op = nir_op_i2f;     break;
   case SpvOpConvertUToF:           op = nir_op_u2f;     break;
   case SpvOpBitcast:               op = nir_op_imov;    break;
   case SpvOpUConvert:
   case SpvOpSConvert:
      op = nir_op_imov; /* TODO: NIR is 32-bit only; these are no-ops. */
      break;
   case SpvOpFConvert:
      op = nir_op_fmov;
      break;

   /* Derivatives: */
   case SpvOpDPdx:         op = nir_op_fddx;          break;
   case SpvOpDPdy:         op = nir_op_fddy;          break;
   case SpvOpDPdxFine:     op = nir_op_fddx_fine;     break;
   case SpvOpDPdyFine:     op = nir_op_fddy_fine;     break;
   case SpvOpDPdxCoarse:   op = nir_op_fddx_coarse;   break;
   case SpvOpDPdyCoarse:   op = nir_op_fddy_coarse;   break;
   case SpvOpFwidth:
      val->ssa = nir_fadd(&b->nb,
                          nir_fabs(&b->nb, nir_fddx(&b->nb, src[0])),
                          nir_fabs(&b->nb, nir_fddx(&b->nb, src[1])));
      return;
   case SpvOpFwidthFine:
      val->ssa = nir_fadd(&b->nb,
                          nir_fabs(&b->nb, nir_fddx_fine(&b->nb, src[0])),
                          nir_fabs(&b->nb, nir_fddx_fine(&b->nb, src[1])));
      return;
   case SpvOpFwidthCoarse:
      val->ssa = nir_fadd(&b->nb,
                          nir_fabs(&b->nb, nir_fddx_coarse(&b->nb, src[0])),
                          nir_fabs(&b->nb, nir_fddx_coarse(&b->nb, src[1])));
      return;

   case SpvOpVectorTimesScalar:
      /* The builder will take care of splatting for us. */
      val->ssa = nir_fmul(&b->nb, src[0], src[1]);
      return;

   case SpvOpSRem:
   case SpvOpFRem:
      unreachable("No NIR equivalent");

   case SpvOpIsNan:
   case SpvOpIsInf:
   case SpvOpIsFinite:
   case SpvOpIsNormal:
   case SpvOpSignBitSet:
   case SpvOpLessOrGreater:
   case SpvOpOrdered:
   case SpvOpUnordered:
   default:
      unreachable("Unhandled opcode");
   }

   if (swap) {
      nir_ssa_def *tmp = src[0];
      src[0] = src[1];
      src[1] = tmp;
   }

   nir_alu_instr *instr = nir_alu_instr_create(b->shader, op);
   nir_ssa_dest_init(&instr->instr, &instr->dest.dest,
                     glsl_get_vector_elements(val->type), val->name);
   val->ssa = &instr->dest.dest.ssa;

   for (unsigned i = 0; i < nir_op_infos[op].num_inputs; i++)
      instr->src[i].src = nir_src_for_ssa(src[i]);

   nir_builder_instr_insert(&b->nb, &instr->instr);
}

static bool
vtn_handle_preamble_instruction(struct vtn_builder *b, SpvOp opcode,
                                const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpSource:
   case SpvOpSourceExtension:
   case SpvOpCompileFlag:
   case SpvOpExtension:
      /* Unhandled, but these are for debug so that's ok. */
      break;

   case SpvOpExtInstImport:
      vtn_handle_extension(b, opcode, w, count);
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

   case SpvOpExecutionMode:
      unreachable("Execution modes not yet implemented");
      break;

   case SpvOpString:
      vtn_push_value(b, w[1], vtn_value_type_string)->str =
         vtn_string_literal(b, &w[2], count - 2);
      break;

   case SpvOpName:
      b->values[w[1]].name = vtn_string_literal(b, &w[2], count - 2);
      break;

   case SpvOpMemberName:
      /* TODO */
      break;

   case SpvOpLine:
      break; /* Ignored for now */

   case SpvOpDecorationGroup:
   case SpvOpDecorate:
   case SpvOpMemberDecorate:
   case SpvOpGroupDecorate:
   case SpvOpGroupMemberDecorate:
      vtn_handle_decoration(b, opcode, w, count);
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
      vtn_handle_variables(b, opcode, w, count);
      break;

   default:
      return false; /* End of preamble */
   }

   return true;
}

static bool
vtn_handle_first_cfg_pass_instruction(struct vtn_builder *b, SpvOp opcode,
                                      const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpFunction: {
      assert(b->func == NULL);
      b->func = rzalloc(b, struct vtn_function);

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
      b->func->overload = overload;
      break;
   }

   case SpvOpFunctionEnd:
      b->func = NULL;
      break;

   case SpvOpFunctionParameter:
      break; /* Does nothing */

   case SpvOpLabel: {
      assert(b->block == NULL);
      b->block = rzalloc(b, struct vtn_block);
      b->block->label = w;
      vtn_push_value(b, w[1], vtn_value_type_block)->block = b->block;

      if (b->func->start_block == NULL) {
         /* This is the first block encountered for this function.  In this
          * case, we set the start block and add it to the list of
          * implemented functions that we'll walk later.
          */
         b->func->start_block = b->block;
         exec_list_push_tail(&b->functions, &b->func->node);
      }
      break;
   }

   case SpvOpBranch:
   case SpvOpBranchConditional:
   case SpvOpSwitch:
   case SpvOpKill:
   case SpvOpReturn:
   case SpvOpReturnValue:
   case SpvOpUnreachable:
      assert(b->block);
      b->block->branch = w;
      b->block = NULL;
      break;

   case SpvOpSelectionMerge:
   case SpvOpLoopMerge:
      assert(b->block && b->block->merge_op == SpvOpNop);
      b->block->merge_op = opcode;
      b->block->merge_block_id = w[1];
      break;

   default:
      /* Continue on as per normal */
      return true;
   }

   return true;
}

static bool
vtn_handle_body_instruction(struct vtn_builder *b, SpvOp opcode,
                            const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpLabel: {
      struct vtn_block *block = vtn_value(b, w[1], vtn_value_type_block)->block;
      assert(block->block == NULL);

      struct exec_node *list_tail = exec_list_get_tail(b->nb.cf_node_list);
      nir_cf_node *tail_node = exec_node_data(nir_cf_node, list_tail, node);
      assert(tail_node->type == nir_cf_node_block);
      block->block = nir_cf_node_as_block(tail_node);

      assert(exec_list_is_empty(&block->block->instr_list));
      break;
   }

   case SpvOpLoopMerge:
   case SpvOpSelectionMerge:
      /* This is handled by cfg pre-pass and walk_blocks */
      break;

   case SpvOpUndef:
      vtn_push_value(b, w[2], vtn_value_type_undef);
      break;

   case SpvOpExtInst:
      vtn_handle_extension(b, opcode, w, count);
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

   case SpvOpFunctionCall:
      vtn_handle_function_call(b, opcode, w, count);
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

   case SpvOpTranspose:
   case SpvOpOuterProduct:
   case SpvOpMatrixTimesScalar:
   case SpvOpVectorTimesMatrix:
   case SpvOpMatrixTimesVector:
   case SpvOpMatrixTimesMatrix:
      vtn_handle_matrix_alu(b, opcode, w, count);
      break;

   default:
      unreachable("Unhandled opcode");
   }

   return true;
}

static void
vtn_walk_blocks(struct vtn_builder *b, struct vtn_block *start,
                struct vtn_block *break_block, struct vtn_block *cont_block,
                struct vtn_block *end_block)
{
   struct vtn_block *block = start;
   while (block != end_block) {
      const uint32_t *w = block->branch;
      SpvOp branch_op = w[0] & SpvOpCodeMask;

      if (block->block != NULL) {
         /* We've already visited this block once before so this is a
          * back-edge.  Back-edges are only allowed to point to a loop
          * merge.
          */
         assert(block == cont_block);
         return;
      }

      b->block = block;
      vtn_foreach_instruction(b, block->label, block->branch,
                              vtn_handle_body_instruction);

      switch (branch_op) {
      case SpvOpBranch: {
         struct vtn_block *branch_block =
            vtn_value(b, w[1], vtn_value_type_block)->block;

         if (branch_block == break_block) {
            nir_jump_instr *jump = nir_jump_instr_create(b->shader,
                                                         nir_jump_break);
            nir_builder_instr_insert(&b->nb, &jump->instr);

            return;
         } else if (branch_block == cont_block) {
            nir_jump_instr *jump = nir_jump_instr_create(b->shader,
                                                         nir_jump_continue);
            nir_builder_instr_insert(&b->nb, &jump->instr);

            return;
         } else if (branch_block == end_block) {
            return;
         } else if (branch_block->merge_op == SpvOpLoopMerge) {
            /* This is the jump into a loop. */
            cont_block = branch_block;
            break_block = vtn_value(b, branch_block->merge_block_id,
                                    vtn_value_type_block)->block;

            nir_loop *loop = nir_loop_create(b->shader);
            nir_cf_node_insert_end(b->nb.cf_node_list, &loop->cf_node);

            struct exec_list *old_list = b->nb.cf_node_list;

            nir_builder_insert_after_cf_list(&b->nb, &loop->body);
            vtn_walk_blocks(b, branch_block, break_block, cont_block, NULL);

            nir_builder_insert_after_cf_list(&b->nb, old_list);
            block = break_block;
            continue;
         } else {
            /* TODO: Can this ever happen? */
            block = branch_block;
            continue;
         }
      }

      case SpvOpBranchConditional: {
         /* Gather up the branch blocks */
         struct vtn_block *then_block =
            vtn_value(b, w[2], vtn_value_type_block)->block;
         struct vtn_block *else_block =
            vtn_value(b, w[3], vtn_value_type_block)->block;

         nir_if *if_stmt = nir_if_create(b->shader);
         if_stmt->condition = nir_src_for_ssa(vtn_ssa_value(b, w[1]));
         nir_cf_node_insert_end(b->nb.cf_node_list, &if_stmt->cf_node);

         if (then_block == break_block) {
            nir_jump_instr *jump = nir_jump_instr_create(b->shader,
                                                         nir_jump_break);
            nir_instr_insert_after_cf_list(&if_stmt->then_list,
                                           &jump->instr);
            block = else_block;
         } else if (else_block == break_block) {
            nir_jump_instr *jump = nir_jump_instr_create(b->shader,
                                                         nir_jump_break);
            nir_instr_insert_after_cf_list(&if_stmt->else_list,
                                           &jump->instr);
            block = then_block;
         } else if (then_block == cont_block) {
            nir_jump_instr *jump = nir_jump_instr_create(b->shader,
                                                         nir_jump_continue);
            nir_instr_insert_after_cf_list(&if_stmt->then_list,
                                           &jump->instr);
            block = else_block;
         } else if (else_block == cont_block) {
            nir_jump_instr *jump = nir_jump_instr_create(b->shader,
                                                         nir_jump_continue);
            nir_instr_insert_after_cf_list(&if_stmt->else_list,
                                           &jump->instr);
            block = then_block;
         } else {
            /* Conventional if statement */
            assert(block->merge_op == SpvOpSelectionMerge);
            struct vtn_block *merge_block =
               vtn_value(b, block->merge_block_id, vtn_value_type_block)->block;

            struct exec_list *old_list = b->nb.cf_node_list;

            nir_builder_insert_after_cf_list(&b->nb, &if_stmt->then_list);
            vtn_walk_blocks(b, then_block, break_block, cont_block, merge_block);

            nir_builder_insert_after_cf_list(&b->nb, &if_stmt->else_list);
            vtn_walk_blocks(b, else_block, break_block, cont_block, merge_block);

            nir_builder_insert_after_cf_list(&b->nb, old_list);
            block = merge_block;
            continue;
         }

         /* If we got here then we inserted a predicated break or continue
          * above and we need to handle the other case.  We already set
          * `block` above to indicate what block to visit after the
          * predicated break.
          */

         /* It's possible that the other branch is also a break/continue.
          * If it is, we handle that here.
          */
         if (block == break_block) {
            nir_jump_instr *jump = nir_jump_instr_create(b->shader,
                                                         nir_jump_break);
            nir_builder_instr_insert(&b->nb, &jump->instr);

            return;
         } else if (block == cont_block) {
            nir_jump_instr *jump = nir_jump_instr_create(b->shader,
                                                         nir_jump_continue);
            nir_builder_instr_insert(&b->nb, &jump->instr);

            return;
         }

         /* If we got here then there was a predicated break/continue but
          * the other half of the if has stuff in it.  `block` was already
          * set above so there is nothing left for us to do.
          */
         continue;
      }

      case SpvOpReturn: {
         nir_jump_instr *jump = nir_jump_instr_create(b->shader,
                                                      nir_jump_return);
         nir_builder_instr_insert(&b->nb, &jump->instr);
         return;
      }

      case SpvOpKill: {
         nir_intrinsic_instr *discard =
            nir_intrinsic_instr_create(b->shader, nir_intrinsic_discard);
         nir_builder_instr_insert(&b->nb, &discard->instr);
         return;
      }

      case SpvOpSwitch:
      case SpvOpReturnValue:
      case SpvOpUnreachable:
      default:
         unreachable("Unhandled opcode");
      }
   }
}

nir_shader *
spirv_to_nir(const uint32_t *words, size_t word_count,
             gl_shader_stage stage,
             const nir_shader_compiler_options *options)
{
   const uint32_t *word_end = words + word_count;

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
   exec_list_make_empty(&b->functions);

   /* Handle all the preamble instructions */
   words = vtn_foreach_instruction(b, words, word_end,
                                   vtn_handle_preamble_instruction);

   /* Do a very quick CFG analysis pass */
   vtn_foreach_instruction(b, words, word_end,
                           vtn_handle_first_cfg_pass_instruction);

   foreach_list_typed(struct vtn_function, func, node, &b->functions) {
      b->impl = nir_function_impl_create(func->overload);
      nir_builder_init(&b->nb, b->impl);
      nir_builder_insert_after_cf_list(&b->nb, &b->impl->body);
      vtn_walk_blocks(b, func->start_block, NULL, NULL, NULL);
   }

   ralloc_free(b);

   return shader;
}
