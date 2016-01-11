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

#include "vtn_private.h"
#include "nir/nir_vla.h"
#include "nir/nir_control_flow.h"

static struct vtn_ssa_value *
vtn_undef_ssa_value(struct vtn_builder *b, const struct glsl_type *type)
{
   struct vtn_ssa_value *val = rzalloc(b, struct vtn_ssa_value);
   val->type = type;

   if (glsl_type_is_vector_or_scalar(type)) {
      unsigned num_components = glsl_get_vector_elements(val->type);
      nir_ssa_undef_instr *undef =
         nir_ssa_undef_instr_create(b->shader, num_components);

      nir_instr_insert_before_cf_list(&b->impl->body, &undef->instr);
      val->def = &undef->def;
   } else {
      unsigned elems = glsl_get_length(val->type);
      val->elems = ralloc_array(b, struct vtn_ssa_value *, elems);
      if (glsl_type_is_matrix(type)) {
         const struct glsl_type *elem_type =
            glsl_vector_type(glsl_get_base_type(type),
                             glsl_get_vector_elements(type));

         for (unsigned i = 0; i < elems; i++)
            val->elems[i] = vtn_undef_ssa_value(b, elem_type);
      } else if (glsl_type_is_array(type)) {
         const struct glsl_type *elem_type = glsl_get_array_element(type);
         for (unsigned i = 0; i < elems; i++)
            val->elems[i] = vtn_undef_ssa_value(b, elem_type);
      } else {
         for (unsigned i = 0; i < elems; i++) {
            const struct glsl_type *elem_type = glsl_get_struct_field(type, i);
            val->elems[i] = vtn_undef_ssa_value(b, elem_type);
         }
      }
   }

   return val;
}

static struct vtn_ssa_value *
vtn_const_ssa_value(struct vtn_builder *b, nir_constant *constant,
                    const struct glsl_type *type)
{
   struct hash_entry *entry = _mesa_hash_table_search(b->const_table, constant);

   if (entry)
      return entry->data;

   struct vtn_ssa_value *val = rzalloc(b, struct vtn_ssa_value);
   val->type = type;

   switch (glsl_get_base_type(type)) {
   case GLSL_TYPE_INT:
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_BOOL:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_DOUBLE:
      if (glsl_type_is_vector_or_scalar(type)) {
         unsigned num_components = glsl_get_vector_elements(val->type);
         nir_load_const_instr *load =
            nir_load_const_instr_create(b->shader, num_components);

         for (unsigned i = 0; i < num_components; i++)
            load->value.u[i] = constant->value.u[i];

         nir_instr_insert_before_cf_list(&b->impl->body, &load->instr);
         val->def = &load->def;
      } else {
         assert(glsl_type_is_matrix(type));
         unsigned rows = glsl_get_vector_elements(val->type);
         unsigned columns = glsl_get_matrix_columns(val->type);
         val->elems = ralloc_array(b, struct vtn_ssa_value *, columns);

         for (unsigned i = 0; i < columns; i++) {
            struct vtn_ssa_value *col_val = rzalloc(b, struct vtn_ssa_value);
            col_val->type = glsl_get_column_type(val->type);
            nir_load_const_instr *load =
               nir_load_const_instr_create(b->shader, rows);

            for (unsigned j = 0; j < rows; j++)
               load->value.u[j] = constant->value.u[rows * i + j];

            nir_instr_insert_before_cf_list(&b->impl->body, &load->instr);
            col_val->def = &load->def;

            val->elems[i] = col_val;
         }
      }
      break;

   case GLSL_TYPE_ARRAY: {
      unsigned elems = glsl_get_length(val->type);
      val->elems = ralloc_array(b, struct vtn_ssa_value *, elems);
      const struct glsl_type *elem_type = glsl_get_array_element(val->type);
      for (unsigned i = 0; i < elems; i++)
         val->elems[i] = vtn_const_ssa_value(b, constant->elements[i],
                                             elem_type);
      break;
   }

   case GLSL_TYPE_STRUCT: {
      unsigned elems = glsl_get_length(val->type);
      val->elems = ralloc_array(b, struct vtn_ssa_value *, elems);
      for (unsigned i = 0; i < elems; i++) {
         const struct glsl_type *elem_type =
            glsl_get_struct_field(val->type, i);
         val->elems[i] = vtn_const_ssa_value(b, constant->elements[i],
                                             elem_type);
      }
      break;
   }

   default:
      unreachable("bad constant type");
   }

   return val;
}

static struct vtn_ssa_value *
vtn_variable_load(struct vtn_builder *b, nir_deref_var *src,
                  struct vtn_type *src_type);

struct vtn_ssa_value *
vtn_ssa_value(struct vtn_builder *b, uint32_t value_id)
{
   struct vtn_value *val = vtn_untyped_value(b, value_id);
   switch (val->value_type) {
   case vtn_value_type_undef:
      return vtn_undef_ssa_value(b, val->type->type);

   case vtn_value_type_constant:
      return vtn_const_ssa_value(b, val->constant, val->const_type);

   case vtn_value_type_ssa:
      return val->ssa;

   case vtn_value_type_deref:
      /* This is needed for function parameters */
      return vtn_variable_load(b, val->deref, val->deref_type);

   default:
      unreachable("Invalid type for an SSA value");
   }
}

static char *
vtn_string_literal(struct vtn_builder *b, const uint32_t *words,
                   unsigned word_count, unsigned *words_used)
{
   char *dup = ralloc_strndup(b, (char *)words, word_count * sizeof(*words));
   if (words_used) {
      /* Ammount of space taken by the string (including the null) */
      unsigned len = strlen(dup) + 1;
      *words_used = DIV_ROUND_UP(len, sizeof(*words));
   }
   return dup;
}

const uint32_t *
vtn_foreach_instruction(struct vtn_builder *b, const uint32_t *start,
                        const uint32_t *end, vtn_instruction_handler handler)
{
   const uint32_t *w = start;
   while (w < end) {
      SpvOp opcode = w[0] & SpvOpCodeMask;
      unsigned count = w[0] >> SpvWordCountShift;
      assert(count >= 1 && w + count <= end);

      if (opcode == SpvOpNop) {
         w++;
         continue;
      }

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
                           int parent_member,
                           struct vtn_value *value,
                           vtn_decoration_foreach_cb cb, void *data)
{
   for (struct vtn_decoration *dec = value->decoration; dec; dec = dec->next) {
      int member;
      if (dec->scope == VTN_DEC_DECORATION) {
         member = parent_member;
      } else if (dec->scope >= VTN_DEC_STRUCT_MEMBER0) {
         assert(parent_member == -1);
         member = dec->scope - VTN_DEC_STRUCT_MEMBER0;
      } else {
         /* Not a decoration */
         continue;
      }

      if (dec->group) {
         assert(dec->group->value_type == vtn_value_type_decoration_group);
         _foreach_decoration_helper(b, base_value, member, dec->group,
                                    cb, data);
      } else {
         cb(b, base_value, member, dec, data);
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
   _foreach_decoration_helper(b, value, -1, value, cb, data);
}

void
vtn_foreach_execution_mode(struct vtn_builder *b, struct vtn_value *value,
                           vtn_execution_mode_foreach_cb cb, void *data)
{
   for (struct vtn_decoration *dec = value->decoration; dec; dec = dec->next) {
      if (dec->scope != VTN_DEC_EXECUTION_MODE)
         continue;

      assert(dec->group == NULL);
      cb(b, value, dec, data);
   }
}

static void
vtn_handle_decoration(struct vtn_builder *b, SpvOp opcode,
                      const uint32_t *w, unsigned count)
{
   const uint32_t *w_end = w + count;
   const uint32_t target = w[1];
   w += 2;

   switch (opcode) {
   case SpvOpDecorationGroup:
      vtn_push_value(b, target, vtn_value_type_decoration_group);
      break;

   case SpvOpDecorate:
   case SpvOpMemberDecorate:
   case SpvOpExecutionMode: {
      struct vtn_value *val = &b->values[target];

      struct vtn_decoration *dec = rzalloc(b, struct vtn_decoration);
      switch (opcode) {
      case SpvOpDecorate:
         dec->scope = VTN_DEC_DECORATION;
         break;
      case SpvOpMemberDecorate:
         dec->scope = VTN_DEC_STRUCT_MEMBER0 + *(w++);
         break;
      case SpvOpExecutionMode:
         dec->scope = VTN_DEC_EXECUTION_MODE;
         break;
      default:
         unreachable("Invalid decoration opcode");
      }
      dec->decoration = *(w++);
      dec->literals = w;

      /* Link into the list */
      dec->next = val->decoration;
      val->decoration = dec;
      break;
   }

   case SpvOpGroupMemberDecorate:
   case SpvOpGroupDecorate: {
      struct vtn_value *group =
         vtn_value(b, target, vtn_value_type_decoration_group);

      for (; w < w_end; w++) {
         struct vtn_value *val = vtn_untyped_value(b, *w);
         struct vtn_decoration *dec = rzalloc(b, struct vtn_decoration);

         dec->group = group;
         if (opcode == SpvOpGroupDecorate) {
            dec->scope = VTN_DEC_DECORATION;
         } else {
            dec->scope = VTN_DEC_STRUCT_MEMBER0 + *(w++);
         }

         /* Link into the list */
         dec->next = val->decoration;
         val->decoration = dec;
      }
      break;
   }

   default:
      unreachable("Unhandled opcode");
   }
}

struct member_decoration_ctx {
   struct glsl_struct_field *fields;
   struct vtn_type *type;
};

/* does a shallow copy of a vtn_type */

static struct vtn_type *
vtn_type_copy(struct vtn_builder *b, struct vtn_type *src)
{
   struct vtn_type *dest = ralloc(b, struct vtn_type);
   dest->type = src->type;
   dest->is_builtin = src->is_builtin;
   if (src->is_builtin)
      dest->builtin = src->builtin;

   if (!glsl_type_is_vector_or_scalar(src->type)) {
      switch (glsl_get_base_type(src->type)) {
      case GLSL_TYPE_ARRAY:
         dest->array_element = src->array_element;
         dest->stride = src->stride;
         break;

      case GLSL_TYPE_INT:
      case GLSL_TYPE_UINT:
      case GLSL_TYPE_BOOL:
      case GLSL_TYPE_FLOAT:
      case GLSL_TYPE_DOUBLE:
         /* matrices */
         dest->row_major = src->row_major;
         dest->stride = src->stride;
         break;

      case GLSL_TYPE_STRUCT: {
         unsigned elems = glsl_get_length(src->type);

         dest->members = ralloc_array(b, struct vtn_type *, elems);
         memcpy(dest->members, src->members, elems * sizeof(struct vtn_type *));

         dest->offsets = ralloc_array(b, unsigned, elems);
         memcpy(dest->offsets, src->offsets, elems * sizeof(unsigned));
         break;
      }

      default:
         unreachable("unhandled type");
      }
   }

   return dest;
}

static struct vtn_type *
mutable_matrix_member(struct vtn_builder *b, struct vtn_type *type, int member)
{
   type->members[member] = vtn_type_copy(b, type->members[member]);
   type = type->members[member];

   /* We may have an array of matrices.... Oh, joy! */
   while (glsl_type_is_array(type->type)) {
      type->array_element = vtn_type_copy(b, type->array_element);
      type = type->array_element;
   }

   assert(glsl_type_is_matrix(type->type));

   return type;
}

static void
struct_member_decoration_cb(struct vtn_builder *b,
                            struct vtn_value *val, int member,
                            const struct vtn_decoration *dec, void *void_ctx)
{
   struct member_decoration_ctx *ctx = void_ctx;

   if (member < 0)
      return;

   switch (dec->decoration) {
   case SpvDecorationRelaxedPrecision:
      break; /* FIXME: Do nothing with this for now. */
   case SpvDecorationNoPerspective:
      ctx->fields[member].interpolation = INTERP_QUALIFIER_NOPERSPECTIVE;
      break;
   case SpvDecorationFlat:
      ctx->fields[member].interpolation = INTERP_QUALIFIER_FLAT;
      break;
   case SpvDecorationCentroid:
      ctx->fields[member].centroid = true;
      break;
   case SpvDecorationSample:
      ctx->fields[member].sample = true;
      break;
   case SpvDecorationLocation:
      ctx->fields[member].location = dec->literals[0];
      break;
   case SpvDecorationBuiltIn:
      ctx->type->members[member] = vtn_type_copy(b, ctx->type->members[member]);
      ctx->type->members[member]->is_builtin = true;
      ctx->type->members[member]->builtin = dec->literals[0];
      ctx->type->builtin_block = true;
      break;
   case SpvDecorationOffset:
      ctx->type->offsets[member] = dec->literals[0];
      break;
   case SpvDecorationMatrixStride:
      mutable_matrix_member(b, ctx->type, member)->stride = dec->literals[0];
      break;
   case SpvDecorationColMajor:
      break; /* Nothing to do here.  Column-major is the default. */
   case SpvDecorationRowMajor:
      mutable_matrix_member(b, ctx->type, member)->row_major = true;
      break;
   default:
      unreachable("Unhandled member decoration");
   }
}

static void
type_decoration_cb(struct vtn_builder *b,
                   struct vtn_value *val, int member,
                    const struct vtn_decoration *dec, void *ctx)
{
   struct vtn_type *type = val->type;

   if (member != -1)
      return;

   switch (dec->decoration) {
   case SpvDecorationArrayStride:
      type->stride = dec->literals[0];
      break;
   case SpvDecorationBlock:
      type->block = true;
      break;
   case SpvDecorationBufferBlock:
      type->buffer_block = true;
      break;
   case SpvDecorationGLSLShared:
   case SpvDecorationGLSLPacked:
      /* Ignore these, since we get explicit offsets anyways */
      break;

   case SpvDecorationStream:
      assert(dec->literals[0] == 0);
      break;

   default:
      unreachable("Unhandled type decoration");
   }
}

static unsigned
translate_image_format(SpvImageFormat format)
{
   switch (format) {
   case SpvImageFormatUnknown:      return 0;      /* GL_NONE */
   case SpvImageFormatRgba32f:      return 0x8814; /* GL_RGBA32F */
   case SpvImageFormatRgba16f:      return 0x881A; /* GL_RGBA16F */
   case SpvImageFormatR32f:         return 0x822E; /* GL_R32F */
   case SpvImageFormatRgba8:        return 0x8058; /* GL_RGBA8 */
   case SpvImageFormatRgba8Snorm:   return 0x8F97; /* GL_RGBA8_SNORM */
   case SpvImageFormatRg32f:        return 0x8230; /* GL_RG32F */
   case SpvImageFormatRg16f:        return 0x822F; /* GL_RG16F */
   case SpvImageFormatR11fG11fB10f: return 0x8C3A; /* GL_R11F_G11F_B10F */
   case SpvImageFormatR16f:         return 0x822D; /* GL_R16F */
   case SpvImageFormatRgba16:       return 0x805B; /* GL_RGBA16 */
   case SpvImageFormatRgb10A2:      return 0x8059; /* GL_RGB10_A2 */
   case SpvImageFormatRg16:         return 0x822C; /* GL_RG16 */
   case SpvImageFormatRg8:          return 0x822B; /* GL_RG8 */
   case SpvImageFormatR16:          return 0x822A; /* GL_R16 */
   case SpvImageFormatR8:           return 0x8229; /* GL_R8 */
   case SpvImageFormatRgba16Snorm:  return 0x8F9B; /* GL_RGBA16_SNORM */
   case SpvImageFormatRg16Snorm:    return 0x8F99; /* GL_RG16_SNORM */
   case SpvImageFormatRg8Snorm:     return 0x8F95; /* GL_RG8_SNORM */
   case SpvImageFormatR16Snorm:     return 0x8F98; /* GL_R16_SNORM */
   case SpvImageFormatR8Snorm:      return 0x8F94; /* GL_R8_SNORM */
   case SpvImageFormatRgba32i:      return 0x8D82; /* GL_RGBA32I */
   case SpvImageFormatRgba16i:      return 0x8D88; /* GL_RGBA16I */
   case SpvImageFormatRgba8i:       return 0x8D8E; /* GL_RGBA8I */
   case SpvImageFormatR32i:         return 0x8235; /* GL_R32I */
   case SpvImageFormatRg32i:        return 0x823B; /* GL_RG32I */
   case SpvImageFormatRg16i:        return 0x8239; /* GL_RG16I */
   case SpvImageFormatRg8i:         return 0x8237; /* GL_RG8I */
   case SpvImageFormatR16i:         return 0x8233; /* GL_R16I */
   case SpvImageFormatR8i:          return 0x8231; /* GL_R8I */
   case SpvImageFormatRgba32ui:     return 0x8D70; /* GL_RGBA32UI */
   case SpvImageFormatRgba16ui:     return 0x8D76; /* GL_RGBA16UI */
   case SpvImageFormatRgba8ui:      return 0x8D7C; /* GL_RGBA8UI */
   case SpvImageFormatR32ui:        return 0x8236; /* GL_R32UI */
   case SpvImageFormatRgb10a2ui:    return 0x906F; /* GL_RGB10_A2UI */
   case SpvImageFormatRg32ui:       return 0x823C; /* GL_RG32UI */
   case SpvImageFormatRg16ui:       return 0x823A; /* GL_RG16UI */
   case SpvImageFormatRg8ui:        return 0x8238; /* GL_RG8UI */
   case SpvImageFormatR16ui:        return 0x823A; /* GL_RG16UI */
   case SpvImageFormatR8ui:         return 0x8232; /* GL_R8UI */
   default:
      assert(!"Invalid image format");
      return 0;
   }
}

static void
vtn_handle_type(struct vtn_builder *b, SpvOp opcode,
                const uint32_t *w, unsigned count)
{
   struct vtn_value *val = vtn_push_value(b, w[1], vtn_value_type_type);

   val->type = rzalloc(b, struct vtn_type);
   val->type->is_builtin = false;

   switch (opcode) {
   case SpvOpTypeVoid:
      val->type->type = glsl_void_type();
      break;
   case SpvOpTypeBool:
      val->type->type = glsl_bool_type();
      break;
   case SpvOpTypeInt:
      val->type->type = glsl_int_type();
      break;
   case SpvOpTypeFloat:
      val->type->type = glsl_float_type();
      break;

   case SpvOpTypeVector: {
      struct vtn_type *base = vtn_value(b, w[2], vtn_value_type_type)->type;
      unsigned elems = w[3];

      assert(glsl_type_is_scalar(base->type));
      val->type->type = glsl_vector_type(glsl_get_base_type(base->type), elems);

      /* Vectors implicitly have sizeof(base_type) stride.  For now, this
       * is always 4 bytes.  This will have to change if we want to start
       * supporting doubles or half-floats.
       */
      val->type->stride = 4;
      val->type->array_element = base;
      break;
   }

   case SpvOpTypeMatrix: {
      struct vtn_type *base = vtn_value(b, w[2], vtn_value_type_type)->type;
      unsigned columns = w[3];

      assert(glsl_type_is_vector(base->type));
      val->type->type = glsl_matrix_type(glsl_get_base_type(base->type),
                                         glsl_get_vector_elements(base->type),
                                         columns);
      assert(!glsl_type_is_error(val->type->type));
      val->type->array_element = base;
      val->type->row_major = false;
      val->type->stride = 0;
      break;
   }

   case SpvOpTypeRuntimeArray:
   case SpvOpTypeArray: {
      struct vtn_type *array_element =
         vtn_value(b, w[2], vtn_value_type_type)->type;

      unsigned length;
      if (opcode == SpvOpTypeRuntimeArray) {
         /* A length of 0 is used to denote unsized arrays */
         length = 0;
      } else {
         length =
            vtn_value(b, w[3], vtn_value_type_constant)->constant->value.u[0];
      }

      val->type->type = glsl_array_type(array_element->type, length);
      val->type->array_element = array_element;
      val->type->stride = 0;
      break;
   }

   case SpvOpTypeStruct: {
      unsigned num_fields = count - 2;
      val->type->members = ralloc_array(b, struct vtn_type *, num_fields);
      val->type->offsets = ralloc_array(b, unsigned, num_fields);

      NIR_VLA(struct glsl_struct_field, fields, count);
      for (unsigned i = 0; i < num_fields; i++) {
         val->type->members[i] =
            vtn_value(b, w[i + 2], vtn_value_type_type)->type;
         fields[i] = (struct glsl_struct_field) {
            .type = val->type->members[i]->type,
            .name = ralloc_asprintf(b, "field%d", i),
            .location = -1,
         };
      }

      struct member_decoration_ctx ctx = {
         .fields = fields,
         .type = val->type
      };

      vtn_foreach_decoration(b, val, struct_member_decoration_cb, &ctx);

      const char *name = val->name ? val->name : "struct";

      val->type->type = glsl_struct_type(fields, num_fields, name);
      break;
   }

   case SpvOpTypeFunction: {
      const struct glsl_type *return_type =
         vtn_value(b, w[2], vtn_value_type_type)->type->type;
      NIR_VLA(struct glsl_function_param, params, count - 3);
      for (unsigned i = 0; i < count - 3; i++) {
         params[i].type = vtn_value(b, w[i + 3], vtn_value_type_type)->type->type;

         /* FIXME: */
         params[i].in = true;
         params[i].out = true;
      }
      val->type->type = glsl_function_type(return_type, params, count - 3);
      break;
   }

   case SpvOpTypePointer:
      /* FIXME:  For now, we'll just do the really lame thing and return
       * the same type.  The validator should ensure that the proper number
       * of dereferences happen
       */
      val->type = vtn_value(b, w[3], vtn_value_type_type)->type;
      break;

   case SpvOpTypeImage: {
      const struct glsl_type *sampled_type =
         vtn_value(b, w[2], vtn_value_type_type)->type->type;

      assert(glsl_type_is_vector_or_scalar(sampled_type));

      enum glsl_sampler_dim dim;
      switch ((SpvDim)w[3]) {
      case SpvDim1D:       dim = GLSL_SAMPLER_DIM_1D;    break;
      case SpvDim2D:       dim = GLSL_SAMPLER_DIM_2D;    break;
      case SpvDim3D:       dim = GLSL_SAMPLER_DIM_3D;    break;
      case SpvDimCube:     dim = GLSL_SAMPLER_DIM_CUBE;  break;
      case SpvDimRect:     dim = GLSL_SAMPLER_DIM_RECT;  break;
      case SpvDimBuffer:   dim = GLSL_SAMPLER_DIM_BUF;   break;
      default:
         unreachable("Invalid SPIR-V Sampler dimension");
      }

      bool is_shadow = w[4];
      bool is_array = w[5];
      bool multisampled = w[6];
      unsigned sampled = w[7];
      SpvImageFormat format = w[8];

      assert(!multisampled && "FIXME: Handl multi-sampled textures");

      val->type->image_format = translate_image_format(format);

      if (sampled == 1) {
         val->type->type = glsl_sampler_type(dim, is_shadow, is_array,
                                             glsl_get_base_type(sampled_type));
      } else if (sampled == 2) {
         assert(format);
         assert(!is_shadow);
         val->type->type = glsl_image_type(dim, is_array,
                                           glsl_get_base_type(sampled_type));
      } else {
         assert(!"We need to know if the image will be sampled");
      }
      break;
   }

   case SpvOpTypeSampledImage:
      val->type = vtn_value(b, w[2], vtn_value_type_type)->type;
      break;

   case SpvOpTypeSampler:
      /* The actual sampler type here doesn't really matter.  It gets
       * thrown away the moment you combine it with an image.  What really
       * matters is that it's a sampler type as opposed to an integer type
       * so the backend knows what to do.
       *
       * TODO: Eventually we should consider adding a "bare sampler" type
       * to glsl_types.
       */
      val->type->type = glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false,
                                          GLSL_TYPE_FLOAT);
      break;

   case SpvOpTypeOpaque:
   case SpvOpTypeEvent:
   case SpvOpTypeDeviceEvent:
   case SpvOpTypeReserveId:
   case SpvOpTypeQueue:
   case SpvOpTypePipe:
   default:
      unreachable("Unhandled opcode");
   }

   vtn_foreach_decoration(b, val, type_decoration_cb, NULL);
}

static nir_constant *
vtn_null_constant(struct vtn_builder *b, const struct glsl_type *type)
{
   nir_constant *c = rzalloc(b, nir_constant);

   switch (glsl_get_base_type(type)) {
   case GLSL_TYPE_INT:
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_BOOL:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_DOUBLE:
      /* Nothing to do here.  It's already initialized to zero */
      break;

   case GLSL_TYPE_ARRAY:
      assert(glsl_get_length(type) > 0);
      c->num_elements = glsl_get_length(type);
      c->elements = ralloc_array(b, nir_constant *, c->num_elements);

      c->elements[0] = vtn_null_constant(b, glsl_get_array_element(type));
      for (unsigned i = 1; i < c->num_elements; i++)
         c->elements[i] = c->elements[0];
      break;

   case GLSL_TYPE_STRUCT:
      c->num_elements = glsl_get_length(type);
      c->elements = ralloc_array(b, nir_constant *, c->num_elements);

      for (unsigned i = 0; i < c->num_elements; i++) {
         c->elements[i] = vtn_null_constant(b, glsl_get_struct_field(type, i));
      }
      break;

   default:
      unreachable("Invalid type for null constant");
   }

   return c;
}

static void
vtn_handle_constant(struct vtn_builder *b, SpvOp opcode,
                    const uint32_t *w, unsigned count)
{
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_constant);
   val->const_type = vtn_value(b, w[1], vtn_value_type_type)->type->type;
   val->constant = rzalloc(b, nir_constant);
   switch (opcode) {
   case SpvOpConstantTrue:
      assert(val->const_type == glsl_bool_type());
      val->constant->value.u[0] = NIR_TRUE;
      break;
   case SpvOpConstantFalse:
      assert(val->const_type == glsl_bool_type());
      val->constant->value.u[0] = NIR_FALSE;
      break;
   case SpvOpConstant:
      assert(glsl_type_is_scalar(val->const_type));
      val->constant->value.u[0] = w[3];
      break;
   case SpvOpConstantComposite: {
      unsigned elem_count = count - 3;
      nir_constant **elems = ralloc_array(b, nir_constant *, elem_count);
      for (unsigned i = 0; i < elem_count; i++)
         elems[i] = vtn_value(b, w[i + 3], vtn_value_type_constant)->constant;

      switch (glsl_get_base_type(val->const_type)) {
      case GLSL_TYPE_UINT:
      case GLSL_TYPE_INT:
      case GLSL_TYPE_FLOAT:
      case GLSL_TYPE_BOOL:
         if (glsl_type_is_matrix(val->const_type)) {
            unsigned rows = glsl_get_vector_elements(val->const_type);
            assert(glsl_get_matrix_columns(val->const_type) == elem_count);
            for (unsigned i = 0; i < elem_count; i++)
               for (unsigned j = 0; j < rows; j++)
                  val->constant->value.u[rows * i + j] = elems[i]->value.u[j];
         } else {
            assert(glsl_type_is_vector(val->const_type));
            assert(glsl_get_vector_elements(val->const_type) == elem_count);
            for (unsigned i = 0; i < elem_count; i++)
               val->constant->value.u[i] = elems[i]->value.u[0];
         }
         ralloc_free(elems);
         break;

      case GLSL_TYPE_STRUCT:
      case GLSL_TYPE_ARRAY:
         ralloc_steal(val->constant, elems);
         val->constant->num_elements = elem_count;
         val->constant->elements = elems;
         break;

      default:
         unreachable("Unsupported type for constants");
      }
      break;
   }

   case SpvOpConstantNull:
      val->constant = vtn_null_constant(b, val->const_type);
      break;

   case SpvOpConstantSampler:
      assert(!"OpConstantSampler requires Kernel Capability");
      break;

   default:
      unreachable("Unhandled opcode");
   }
}

static void
set_mode_system_value(nir_variable_mode *mode)
{
   assert(*mode == nir_var_system_value || *mode == nir_var_shader_in);
   *mode = nir_var_system_value;
}

static void
vtn_get_builtin_location(struct vtn_builder *b,
                         SpvBuiltIn builtin, int *location,
                         nir_variable_mode *mode)
{
   switch (builtin) {
   case SpvBuiltInPosition:
      *location = VARYING_SLOT_POS;
      break;
   case SpvBuiltInPointSize:
      *location = VARYING_SLOT_PSIZ;
      break;
   case SpvBuiltInClipDistance:
      *location = VARYING_SLOT_CLIP_DIST0; /* XXX CLIP_DIST1? */
      break;
   case SpvBuiltInCullDistance:
      /* XXX figure this out */
      unreachable("unhandled builtin");
   case SpvBuiltInVertexId:
      /* Vulkan defines VertexID to be zero-based and reserves the new
       * builtin keyword VertexIndex to indicate the non-zero-based value.
       */
      *location = SYSTEM_VALUE_VERTEX_ID_ZERO_BASE;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInInstanceId:
      *location = SYSTEM_VALUE_INSTANCE_ID;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInPrimitiveId:
      *location = VARYING_SLOT_PRIMITIVE_ID;
      *mode = nir_var_shader_out;
      break;
   case SpvBuiltInInvocationId:
      *location = SYSTEM_VALUE_INVOCATION_ID;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInLayer:
      *location = VARYING_SLOT_LAYER;
      *mode = nir_var_shader_out;
      break;
   case SpvBuiltInViewportIndex:
      *location = VARYING_SLOT_VIEWPORT;
      if (b->shader->stage == MESA_SHADER_GEOMETRY)
         *mode = nir_var_shader_out;
      else if (b->shader->stage == MESA_SHADER_FRAGMENT)
         *mode = nir_var_shader_in;
      else
         unreachable("invalid stage for SpvBuiltInViewportIndex");
      break;
   case SpvBuiltInTessLevelOuter:
   case SpvBuiltInTessLevelInner:
   case SpvBuiltInTessCoord:
   case SpvBuiltInPatchVertices:
      unreachable("no tessellation support");
   case SpvBuiltInFragCoord:
      *location = VARYING_SLOT_POS;
      assert(*mode == nir_var_shader_in);
      break;
   case SpvBuiltInPointCoord:
      *location = VARYING_SLOT_PNTC;
      assert(*mode == nir_var_shader_in);
      break;
   case SpvBuiltInFrontFacing:
      *location = VARYING_SLOT_FACE;
      assert(*mode == nir_var_shader_in);
      break;
   case SpvBuiltInSampleId:
      *location = SYSTEM_VALUE_SAMPLE_ID;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInSamplePosition:
      *location = SYSTEM_VALUE_SAMPLE_POS;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInSampleMask:
      *location = SYSTEM_VALUE_SAMPLE_MASK_IN; /* XXX out? */
      set_mode_system_value(mode);
      break;
   case SpvBuiltInFragDepth:
      *location = FRAG_RESULT_DEPTH;
      assert(*mode == nir_var_shader_out);
      break;
   case SpvBuiltInNumWorkgroups:
      *location = SYSTEM_VALUE_NUM_WORK_GROUPS;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInWorkgroupSize:
      /* This should already be handled */
      unreachable("unsupported builtin");
      break;
   case SpvBuiltInWorkgroupId:
      *location = SYSTEM_VALUE_WORK_GROUP_ID;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInLocalInvocationId:
      *location = SYSTEM_VALUE_LOCAL_INVOCATION_ID;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInLocalInvocationIndex:
      *location = SYSTEM_VALUE_LOCAL_INVOCATION_INDEX;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInGlobalInvocationId:
      *location = SYSTEM_VALUE_GLOBAL_INVOCATION_ID;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInHelperInvocation:
   default:
      unreachable("unsupported builtin");
   }
}

static void
var_decoration_cb(struct vtn_builder *b, struct vtn_value *val, int member,
                  const struct vtn_decoration *dec, void *void_var)
{
   assert(val->value_type == vtn_value_type_deref);
   assert(val->deref->deref.child == NULL);
   assert(val->deref->var == void_var);

   nir_variable *var = void_var;
   switch (dec->decoration) {
   case SpvDecorationRelaxedPrecision:
      break; /* FIXME: Do nothing with this for now. */
   case SpvDecorationNoPerspective:
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
   case SpvDecorationNonWritable:
      var->data.read_only = true;
      break;
   case SpvDecorationLocation:
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
   case SpvDecorationDescriptorSet:
      var->data.descriptor_set = dec->literals[0];
      break;
   case SpvDecorationBuiltIn: {
      SpvBuiltIn builtin = dec->literals[0];

      if (builtin == SpvBuiltInWorkgroupSize) {
         /* This shouldn't be a builtin.  It's actually a constant. */
         var->data.mode = nir_var_global;
         var->data.read_only = true;

         nir_constant *val = rzalloc(var, nir_constant);
         val->value.u[0] = b->shader->info.cs.local_size[0];
         val->value.u[1] = b->shader->info.cs.local_size[1];
         val->value.u[2] = b->shader->info.cs.local_size[2];
         var->constant_initializer = val;
         break;
      }

      nir_variable_mode mode = var->data.mode;
      vtn_get_builtin_location(b, builtin, &var->data.location, &mode);
      var->data.explicit_location = true;
      var->data.mode = mode;
      if (mode == nir_var_shader_in || mode == nir_var_system_value)
         var->data.read_only = true;

      if (builtin == SpvBuiltInFragCoord || builtin == SpvBuiltInSamplePosition)
         var->data.origin_upper_left = b->origin_upper_left;

      if (mode == nir_var_shader_out)
         b->builtins[dec->literals[0]].out = var;
      else
         b->builtins[dec->literals[0]].in = var;
      break;
   }
   case SpvDecorationRowMajor:
   case SpvDecorationColMajor:
   case SpvDecorationGLSLShared:
   case SpvDecorationPatch:
   case SpvDecorationRestrict:
   case SpvDecorationAliased:
   case SpvDecorationVolatile:
   case SpvDecorationCoherent:
   case SpvDecorationNonReadable:
   case SpvDecorationUniform:
      /* This is really nice but we have no use for it right now. */
   case SpvDecorationCPacked:
   case SpvDecorationSaturatedConversion:
   case SpvDecorationStream:
   case SpvDecorationOffset:
   case SpvDecorationXfbBuffer:
   case SpvDecorationFuncParamAttr:
   case SpvDecorationFPRoundingMode:
   case SpvDecorationFPFastMathMode:
   case SpvDecorationLinkageAttributes:
   case SpvDecorationSpecId:
      break;
   default:
      unreachable("Unhandled variable decoration");
   }
}

static nir_variable *
get_builtin_variable(struct vtn_builder *b,
                     nir_variable_mode mode,
                     const struct glsl_type *type,
                     SpvBuiltIn builtin)
{
   nir_variable *var;
   if (mode == nir_var_shader_out)
      var = b->builtins[builtin].out;
   else
      var = b->builtins[builtin].in;

   if (!var) {
      int location;
      vtn_get_builtin_location(b, builtin, &location, &mode);

      var = nir_variable_create(b->shader, mode, type, "builtin");

      var->data.location = location;
      var->data.explicit_location = true;

      if (builtin == SpvBuiltInFragCoord || builtin == SpvBuiltInSamplePosition)
         var->data.origin_upper_left = b->origin_upper_left;

      if (mode == nir_var_shader_out)
         b->builtins[builtin].out = var;
      else
         b->builtins[builtin].in = var;
   }

   return var;
}

static struct vtn_ssa_value *
_vtn_variable_load(struct vtn_builder *b,
                   nir_deref_var *src_deref, nir_deref *src_deref_tail)
{
   struct vtn_ssa_value *val = rzalloc(b, struct vtn_ssa_value);
   val->type = src_deref_tail->type;

   /* The deref tail may contain a deref to select a component of a vector (in
    * other words, it might not be an actual tail) so we have to save it away
    * here since we overwrite it later.
    */
   nir_deref *old_child = src_deref_tail->child;

   if (glsl_type_is_vector_or_scalar(val->type)) {
      /* Terminate the deref chain in case there is one more link to pick
       * off a component of the vector.
       */
      src_deref_tail->child = NULL;

      nir_intrinsic_instr *load =
         nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_var);
      load->variables[0] =
         nir_deref_as_var(nir_copy_deref(load, &src_deref->deref));
      load->num_components = glsl_get_vector_elements(val->type);
      nir_ssa_dest_init(&load->instr, &load->dest, load->num_components, NULL);

      nir_builder_instr_insert(&b->nb, &load->instr);

      if (src_deref->var->data.mode == nir_var_uniform &&
          glsl_get_base_type(val->type) == GLSL_TYPE_BOOL) {
         /* Uniform boolean loads need to be fixed up since they're defined
          * to be zero/nonzero rather than NIR_FALSE/NIR_TRUE.
          */
         val->def = nir_ine(&b->nb, &load->dest.ssa, nir_imm_int(&b->nb, 0));
      } else {
         val->def = &load->dest.ssa;
      }
   } else if (glsl_get_base_type(val->type) == GLSL_TYPE_ARRAY ||
              glsl_type_is_matrix(val->type)) {
      unsigned elems = glsl_get_length(val->type);
      val->elems = ralloc_array(b, struct vtn_ssa_value *, elems);

      nir_deref_array *deref = nir_deref_array_create(b);
      deref->deref_array_type = nir_deref_array_type_direct;
      deref->deref.type = glsl_get_array_element(val->type);
      src_deref_tail->child = &deref->deref;
      for (unsigned i = 0; i < elems; i++) {
         deref->base_offset = i;
         val->elems[i] = _vtn_variable_load(b, src_deref, &deref->deref);
      }
   } else {
      assert(glsl_get_base_type(val->type) == GLSL_TYPE_STRUCT);
      unsigned elems = glsl_get_length(val->type);
      val->elems = ralloc_array(b, struct vtn_ssa_value *, elems);

      nir_deref_struct *deref = nir_deref_struct_create(b, 0);
      src_deref_tail->child = &deref->deref;
      for (unsigned i = 0; i < elems; i++) {
         deref->index = i;
         deref->deref.type = glsl_get_struct_field(val->type, i);
         val->elems[i] = _vtn_variable_load(b, src_deref, &deref->deref);
      }
   }

   src_deref_tail->child = old_child;

   return val;
}

static void
_vtn_variable_store(struct vtn_builder *b,
                    nir_deref_var *dest_deref, nir_deref *dest_deref_tail,
                    struct vtn_ssa_value *src)
{
   nir_deref *old_child = dest_deref_tail->child;

   if (glsl_type_is_vector_or_scalar(src->type)) {
      /* Terminate the deref chain in case there is one more link to pick
       * off a component of the vector.
       */
      dest_deref_tail->child = NULL;

      nir_intrinsic_instr *store =
         nir_intrinsic_instr_create(b->shader, nir_intrinsic_store_var);
      store->variables[0] =
         nir_deref_as_var(nir_copy_deref(store, &dest_deref->deref));
      store->num_components = glsl_get_vector_elements(src->type);
      store->const_index[0] = (1 << store->num_components) - 1;
      store->src[0] = nir_src_for_ssa(src->def);

      nir_builder_instr_insert(&b->nb, &store->instr);
   } else if (glsl_get_base_type(src->type) == GLSL_TYPE_ARRAY ||
              glsl_type_is_matrix(src->type)) {
      unsigned elems = glsl_get_length(src->type);

      nir_deref_array *deref = nir_deref_array_create(b);
      deref->deref_array_type = nir_deref_array_type_direct;
      deref->deref.type = glsl_get_array_element(src->type);
      dest_deref_tail->child = &deref->deref;
      for (unsigned i = 0; i < elems; i++) {
         deref->base_offset = i;
         _vtn_variable_store(b, dest_deref, &deref->deref, src->elems[i]);
      }
   } else {
      assert(glsl_get_base_type(src->type) == GLSL_TYPE_STRUCT);
      unsigned elems = glsl_get_length(src->type);

      nir_deref_struct *deref = nir_deref_struct_create(b, 0);
      dest_deref_tail->child = &deref->deref;
      for (unsigned i = 0; i < elems; i++) {
         deref->index = i;
         deref->deref.type = glsl_get_struct_field(src->type, i);
         _vtn_variable_store(b, dest_deref, &deref->deref, src->elems[i]);
      }
   }

   dest_deref_tail->child = old_child;
}

static nir_ssa_def *
deref_array_offset(struct vtn_builder *b, nir_deref *deref)
{
   assert(deref->deref_type == nir_deref_type_array);
   nir_deref_array *deref_array = nir_deref_as_array(deref);
   nir_ssa_def *offset = nir_imm_int(&b->nb, deref_array->base_offset);

   if (deref_array->deref_array_type == nir_deref_array_type_indirect)
      offset = nir_iadd(&b->nb, offset, deref_array->indirect.ssa);

   return offset;
}

static nir_ssa_def *
get_vulkan_resource_index(struct vtn_builder *b,
                          nir_deref **deref, struct vtn_type **type)
{
   assert((*deref)->deref_type == nir_deref_type_var);
   nir_variable *var = nir_deref_as_var(*deref)->var;

   assert(var->interface_type && "variable is a block");

   nir_ssa_def *array_index;
   if ((*deref)->child && (*deref)->child->deref_type == nir_deref_type_array) {
      *deref = (*deref)->child;
      *type = (*type)->array_element;
      array_index = deref_array_offset(b, *deref);
   } else {
      array_index = nir_imm_int(&b->nb, 0);
   }

   nir_intrinsic_instr *instr =
      nir_intrinsic_instr_create(b->nb.shader,
                                 nir_intrinsic_vulkan_resource_index);
   instr->src[0] = nir_src_for_ssa(array_index);
   instr->const_index[0] = var->data.descriptor_set;
   instr->const_index[1] = var->data.binding;
   instr->const_index[2] = var->data.mode;

   nir_ssa_dest_init(&instr->instr, &instr->dest, 1, NULL);
   nir_builder_instr_insert(&b->nb, &instr->instr);

   return &instr->dest.ssa;
}

static void
_vtn_load_store_tail(struct vtn_builder *b, nir_intrinsic_op op, bool load,
                     nir_ssa_def *index, nir_ssa_def *offset,
                     struct vtn_ssa_value **inout, const struct glsl_type *type)
{
   nir_intrinsic_instr *instr = nir_intrinsic_instr_create(b->nb.shader, op);
   instr->num_components = glsl_get_vector_elements(type);

   int src = 0;
   if (!load) {
      instr->const_index[0] = (1 << instr->num_components) - 1; /* write mask */
      instr->src[src++] = nir_src_for_ssa((*inout)->def);
   }

   if (index)
      instr->src[src++] = nir_src_for_ssa(index);

   instr->src[src++] = nir_src_for_ssa(offset);

   if (load) {
      nir_ssa_dest_init(&instr->instr, &instr->dest,
                        instr->num_components, NULL);
      (*inout)->def = &instr->dest.ssa;
   }

   nir_builder_instr_insert(&b->nb, &instr->instr);

   if (load && glsl_get_base_type(type) == GLSL_TYPE_BOOL)
      (*inout)->def = nir_ine(&b->nb, (*inout)->def, nir_imm_int(&b->nb, 0));
}

static void
_vtn_block_load_store(struct vtn_builder *b, nir_intrinsic_op op, bool load,
                      nir_ssa_def *index, nir_ssa_def *offset, nir_deref *deref,
                      struct vtn_type *type, struct vtn_ssa_value **inout)
{
   if (load && deref == NULL && *inout == NULL)
      *inout = vtn_create_ssa_value(b, type->type);

   enum glsl_base_type base_type = glsl_get_base_type(type->type);
   switch (base_type) {
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_BOOL:
      /* This is where things get interesting.  At this point, we've hit
       * a vector, a scalar, or a matrix.
       */
      if (glsl_type_is_matrix(type->type)) {
         if (deref == NULL) {
            /* Loading the whole matrix */
            struct vtn_ssa_value *transpose;
            unsigned num_ops, vec_width;
            if (type->row_major) {
               num_ops = glsl_get_vector_elements(type->type);
               vec_width = glsl_get_matrix_columns(type->type);
               if (load) {
                  const struct glsl_type *transpose_type =
                     glsl_matrix_type(base_type, vec_width, num_ops);
                  *inout = vtn_create_ssa_value(b, transpose_type);
               } else {
                  transpose = vtn_ssa_transpose(b, *inout);
                  inout = &transpose;
               }
            } else {
               num_ops = glsl_get_matrix_columns(type->type);
               vec_width = glsl_get_vector_elements(type->type);
            }

            for (unsigned i = 0; i < num_ops; i++) {
               nir_ssa_def *elem_offset =
                  nir_iadd(&b->nb, offset,
                           nir_imm_int(&b->nb, i * type->stride));
               _vtn_load_store_tail(b, op, load, index, elem_offset,
                                    &(*inout)->elems[i],
                                    glsl_vector_type(base_type, vec_width));
            }

            if (load && type->row_major)
               *inout = vtn_ssa_transpose(b, *inout);

            return;
         } else if (type->row_major) {
            /* Row-major but with a deref. */
            nir_ssa_def *col_offset =
               nir_imul(&b->nb, deref_array_offset(b, deref),
                        nir_imm_int(&b->nb, type->array_element->stride));
            offset = nir_iadd(&b->nb, offset, col_offset);

            if (deref->child) {
               /* Picking off a single element */
               nir_ssa_def *row_offset =
                  nir_imul(&b->nb, deref_array_offset(b, deref->child),
                           nir_imm_int(&b->nb, type->stride));
               offset = nir_iadd(&b->nb, offset, row_offset);
               _vtn_load_store_tail(b, op, load, index, offset, inout,
                                    glsl_scalar_type(base_type));
               return;
            } else {
               unsigned num_comps = glsl_get_vector_elements(type->type);
               nir_ssa_def *comps[4];
               for (unsigned i = 0; i < num_comps; i++) {
                  nir_ssa_def *elem_offset =
                     nir_iadd(&b->nb, offset,
                              nir_imm_int(&b->nb, i * type->stride));

                  struct vtn_ssa_value *comp = NULL, temp_val;
                  if (!load) {
                     temp_val.def = nir_channel(&b->nb, (*inout)->def, i);
                     temp_val.type = glsl_scalar_type(base_type);
                     comp = &temp_val;
                  }
                  _vtn_load_store_tail(b, op, load, index, elem_offset,
                                       &comp, glsl_scalar_type(base_type));
                  comps[i] = comp->def;
               }

               if (load)
                  (*inout)->def = nir_vec(&b->nb, comps, num_comps);
               return;
            }
         } else {
            /* Column-major with a deref. Fall through to array case. */
         }
      } else if (deref == NULL) {
         assert(glsl_type_is_vector_or_scalar(type->type));
         _vtn_load_store_tail(b, op, load, index, offset, inout, type->type);
         return;
      } else {
         /* Single component of a vector. Fall through to array case. */
      }
      /* Fall through */

   case GLSL_TYPE_ARRAY:
      if (deref) {
         offset = nir_iadd(&b->nb, offset,
                           nir_imul(&b->nb, deref_array_offset(b, deref),
                                    nir_imm_int(&b->nb, type->stride)));

         _vtn_block_load_store(b, op, load, index, offset, deref->child,
                               type->array_element, inout);
         return;
      } else {
         unsigned elems = glsl_get_length(type->type);
         for (unsigned i = 0; i < elems; i++) {
            nir_ssa_def *elem_off =
               nir_iadd(&b->nb, offset, nir_imm_int(&b->nb, i * type->stride));
            _vtn_block_load_store(b, op, load, index, elem_off, NULL,
                                  type->array_element, &(*inout)->elems[i]);
         }
         return;
      }
      unreachable("Both branches above return");

   case GLSL_TYPE_STRUCT:
      if (deref) {
         unsigned member = nir_deref_as_struct(deref)->index;
         offset = nir_iadd(&b->nb, offset,
                           nir_imm_int(&b->nb, type->offsets[member]));

         _vtn_block_load_store(b, op, load, index, offset, deref->child,
                               type->members[member], inout);
         return;
      } else {
         unsigned elems = glsl_get_length(type->type);
         for (unsigned i = 0; i < elems; i++) {
            nir_ssa_def *elem_off =
               nir_iadd(&b->nb, offset, nir_imm_int(&b->nb, type->offsets[i]));
            _vtn_block_load_store(b, op, load, index, elem_off, NULL,
                                  type->members[i], &(*inout)->elems[i]);
         }
         return;
      }
      unreachable("Both branches above return");

   default:
      unreachable("Invalid block member type");
   }
}

static struct vtn_ssa_value *
vtn_block_load(struct vtn_builder *b, nir_deref_var *src,
               struct vtn_type *type)
{
   nir_intrinsic_op op;
   if (src->var->data.mode == nir_var_uniform) {
      if (src->var->data.descriptor_set >= 0) {
         /* UBO load */
         assert(src->var->data.binding >= 0);

         op = nir_intrinsic_load_ubo;
      } else {
         /* Push constant load */
         assert(src->var->data.descriptor_set == -1 &&
                src->var->data.binding == -1);

         op = nir_intrinsic_load_push_constant;
      }
   } else {
      assert(src->var->data.mode == nir_var_shader_storage);
      op = nir_intrinsic_load_ssbo;
   }

   nir_deref *block_deref = &src->deref;
   nir_ssa_def *index = NULL;
   if (op == nir_intrinsic_load_ubo || op == nir_intrinsic_load_ssbo)
      index = get_vulkan_resource_index(b, &block_deref, &type);

   struct vtn_ssa_value *value = NULL;
   _vtn_block_load_store(b, op, true, index, nir_imm_int(&b->nb, 0),
                         block_deref->child, type, &value);
   return value;
}

/*
 * Gets the NIR-level deref tail, which may have as a child an array deref
 * selecting which component due to OpAccessChain supporting per-component
 * indexing in SPIR-V.
 */

static nir_deref *
get_deref_tail(nir_deref_var *deref)
{
   nir_deref *cur = &deref->deref;
   while (!glsl_type_is_vector_or_scalar(cur->type) && cur->child)
      cur = cur->child;

   return cur;
}

static nir_ssa_def *vtn_vector_extract(struct vtn_builder *b,
                                       nir_ssa_def *src, unsigned index);

static nir_ssa_def *vtn_vector_extract_dynamic(struct vtn_builder *b,
                                               nir_ssa_def *src,
                                               nir_ssa_def *index);

static bool
variable_is_external_block(nir_variable *var)
{
   return var->interface_type &&
          glsl_type_is_struct(var->interface_type) &&
          (var->data.mode == nir_var_uniform ||
           var->data.mode == nir_var_shader_storage);
}

static struct vtn_ssa_value *
vtn_variable_load(struct vtn_builder *b, nir_deref_var *src,
                  struct vtn_type *src_type)
{
   if (variable_is_external_block(src->var))
      return vtn_block_load(b, src, src_type);

   nir_deref *src_tail = get_deref_tail(src);
   struct vtn_ssa_value *val = _vtn_variable_load(b, src, src_tail);

   if (src_tail->child) {
      nir_deref_array *vec_deref = nir_deref_as_array(src_tail->child);
      assert(vec_deref->deref.child == NULL);
      val->type = vec_deref->deref.type;
      if (vec_deref->deref_array_type == nir_deref_array_type_direct)
         val->def = vtn_vector_extract(b, val->def, vec_deref->base_offset);
      else
         val->def = vtn_vector_extract_dynamic(b, val->def,
                                               vec_deref->indirect.ssa);
   }

   return val;
}

static void
vtn_block_store(struct vtn_builder *b, struct vtn_ssa_value *src,
                nir_deref_var *dest, struct vtn_type *type)
{
   nir_deref *block_deref = &dest->deref;
   nir_ssa_def *index = get_vulkan_resource_index(b, &block_deref, &type);

   _vtn_block_load_store(b, nir_intrinsic_store_ssbo, false, index,
                         nir_imm_int(&b->nb, 0), block_deref->child,
                         type, &src);
}

static nir_ssa_def * vtn_vector_insert(struct vtn_builder *b,
                                       nir_ssa_def *src, nir_ssa_def *insert,
                                       unsigned index);

static nir_ssa_def * vtn_vector_insert_dynamic(struct vtn_builder *b,
                                               nir_ssa_def *src,
                                               nir_ssa_def *insert,
                                               nir_ssa_def *index);
void
vtn_variable_store(struct vtn_builder *b, struct vtn_ssa_value *src,
                   nir_deref_var *dest, struct vtn_type *dest_type)
{
   if (variable_is_external_block(dest->var)) {
      assert(dest->var->data.mode == nir_var_shader_storage);
      vtn_block_store(b, src, dest, dest_type);
   } else {
      nir_deref *dest_tail = get_deref_tail(dest);
      if (dest_tail->child) {
         struct vtn_ssa_value *val = _vtn_variable_load(b, dest, dest_tail);
         nir_deref_array *deref = nir_deref_as_array(dest_tail->child);
         assert(deref->deref.child == NULL);
         if (deref->deref_array_type == nir_deref_array_type_direct)
            val->def = vtn_vector_insert(b, val->def, src->def,
                                         deref->base_offset);
         else
            val->def = vtn_vector_insert_dynamic(b, val->def, src->def,
                                                 deref->indirect.ssa);
         _vtn_variable_store(b, dest, dest_tail, val);
      } else {
         _vtn_variable_store(b, dest, dest_tail, src);
      }
   }
}

static void
vtn_variable_copy(struct vtn_builder *b,
                  nir_deref_var *dest, struct vtn_type *dest_type,
                  nir_deref_var *src, struct vtn_type *src_type)
{
   if (src->var->interface_type || dest->var->interface_type) {
      struct vtn_ssa_value *val = vtn_variable_load(b, src, src_type);
      vtn_variable_store(b, val, dest, dest_type);
   } else {
      nir_intrinsic_instr *copy =
         nir_intrinsic_instr_create(b->shader, nir_intrinsic_copy_var);
      copy->variables[0] = nir_deref_as_var(nir_copy_deref(copy, &dest->deref));
      copy->variables[1] = nir_deref_as_var(nir_copy_deref(copy, &src->deref));

      nir_builder_instr_insert(&b->nb, &copy->instr);
   }
}

/* Tries to compute the size of an interface block based on the strides and
 * offsets that are provided to us in the SPIR-V source.
 */
static unsigned
vtn_type_block_size(struct vtn_type *type)
{
   enum glsl_base_type base_type = glsl_get_base_type(type->type);
   switch (base_type) {
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_BOOL:
   case GLSL_TYPE_DOUBLE: {
      unsigned cols = type->row_major ? glsl_get_vector_elements(type->type) :
                                        glsl_get_matrix_columns(type->type);
      if (cols > 1) {
         assert(type->stride > 0);
         return type->stride * cols;
      } else if (base_type == GLSL_TYPE_DOUBLE) {
         return glsl_get_vector_elements(type->type) * 8;
      } else {
         return glsl_get_vector_elements(type->type) * 4;
      }
   }

   case GLSL_TYPE_STRUCT:
   case GLSL_TYPE_INTERFACE: {
      unsigned size = 0;
      unsigned num_fields = glsl_get_length(type->type);
      for (unsigned f = 0; f < num_fields; f++) {
         unsigned field_end = type->offsets[f] +
                              vtn_type_block_size(type->members[f]);
         size = MAX2(size, field_end);
      }
      return size;
   }

   case GLSL_TYPE_ARRAY:
      assert(type->stride > 0);
      assert(glsl_get_length(type->type) > 0);
      return type->stride * glsl_get_length(type->type);

   default:
      assert(!"Invalid block type");
      return 0;
   }
}

static bool
is_interface_type(struct vtn_type *type)
{
   return type->block || type->buffer_block ||
          glsl_type_is_sampler(type->type) ||
          glsl_type_is_image(type->type);
}

static void
vtn_handle_variables(struct vtn_builder *b, SpvOp opcode,
                     const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpVariable: {
      struct vtn_type *type =
         vtn_value(b, w[1], vtn_value_type_type)->type;
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_deref);
      SpvStorageClass storage_class = w[3];

      nir_variable *var = rzalloc(b->shader, nir_variable);

      var->type = type->type;
      var->name = ralloc_strdup(var, val->name);

      struct vtn_type *interface_type;
      if (is_interface_type(type)) {
         interface_type = type;
      } else if (glsl_type_is_array(type->type) &&
                 is_interface_type(type->array_element)) {
         interface_type = type->array_element;
      } else {
         interface_type = NULL;
      }

      if (interface_type)
         var->interface_type = interface_type->type;

      switch (storage_class) {
      case SpvStorageClassUniform:
      case SpvStorageClassUniformConstant:
         if (interface_type && interface_type->buffer_block) {
            var->data.mode = nir_var_shader_storage;
            b->shader->info.num_ssbos++;
         } else {
            /* UBO's and samplers */
            var->data.mode = nir_var_uniform;
            var->data.read_only = true;
            if (interface_type) {
               if (glsl_type_is_image(interface_type->type)) {
                  b->shader->info.num_images++;
                  var->data.image.format = interface_type->image_format;
               } else if (glsl_type_is_sampler(interface_type->type)) {
                  b->shader->info.num_textures++;
               } else {
                  assert(glsl_type_is_struct(interface_type->type));
                  b->shader->info.num_ubos++;
               }
            }
         }
         break;
      case SpvStorageClassPushConstant:
         assert(interface_type && interface_type->block);
         var->data.mode = nir_var_uniform;
         var->data.read_only = true;
         var->data.descriptor_set = -1;
         var->data.binding = -1;

         /* We have exactly one push constant block */
         assert(b->shader->num_uniforms == 0);
         b->shader->num_uniforms = vtn_type_block_size(type) * 4;
         break;
      case SpvStorageClassInput:
         var->data.mode = nir_var_shader_in;
         var->data.read_only = true;
         break;
      case SpvStorageClassOutput:
         var->data.mode = nir_var_shader_out;
         break;
      case SpvStorageClassPrivate:
         var->data.mode = nir_var_global;
         var->interface_type = NULL;
         break;
      case SpvStorageClassFunction:
         var->data.mode = nir_var_local;
         var->interface_type = NULL;
         break;
      case SpvStorageClassWorkgroup:
      case SpvStorageClassCrossWorkgroup:
      case SpvStorageClassGeneric:
      case SpvStorageClassAtomicCounter:
      default:
         unreachable("Unhandled variable storage class");
      }

      if (count > 4) {
         assert(count == 5);
         nir_constant *constant =
            vtn_value(b, w[4], vtn_value_type_constant)->constant;
         var->constant_initializer = nir_constant_clone(constant, var);
      }

      val->deref = nir_deref_var_create(b, var);
      val->deref_type = type;

      /* We handle decorations first because decorations might give us
       * location information.  We use the data.explicit_location field to
       * note that the location provided is the "final" location.  If
       * data.explicit_location == false, this means that it's relative to
       * whatever the base location is.
       */
      vtn_foreach_decoration(b, val, var_decoration_cb, var);

      if (!var->data.explicit_location) {
         if (b->shader->stage == MESA_SHADER_FRAGMENT &&
             var->data.mode == nir_var_shader_out) {
            var->data.location += FRAG_RESULT_DATA0;
         } else if (b->shader->stage == MESA_SHADER_VERTEX &&
                    var->data.mode == nir_var_shader_in) {
            var->data.location += VERT_ATTRIB_GENERIC0;
         } else if (var->data.mode == nir_var_shader_in ||
                    var->data.mode == nir_var_shader_out) {
            var->data.location += VARYING_SLOT_VAR0;
         }
      }

      /* XXX: Work around what appears to be a glslang bug.  While the
       * SPIR-V spec doesn't say that setting a descriptor set on a push
       * constant is invalid, it certainly makes no sense.  However, at
       * some point, glslang started setting descriptor set 0 on push
       * constants for some unknown reason.  Hopefully this can be removed
       * at some point in the future.
       */
      if (storage_class == SpvStorageClassPushConstant) {
         var->data.descriptor_set = -1;
         var->data.binding = -1;
      }

      /* Interface block variables aren't actually going to be referenced
       * by the generated NIR, so we don't put them in the list
       */
      if (var->interface_type && glsl_type_is_struct(var->interface_type))
         break;

      if (var->data.mode == nir_var_local) {
         nir_function_impl_add_variable(b->impl, var);
      } else {
         nir_shader_add_variable(b->shader, var);
      }

      break;
   }

   case SpvOpAccessChain:
   case SpvOpInBoundsAccessChain: {
      nir_deref_var *base;
      struct vtn_value *base_val = vtn_untyped_value(b, w[3]);
      if (base_val->value_type == vtn_value_type_sampled_image) {
         /* This is rather insane.  SPIR-V allows you to use OpSampledImage
          * to combine an array of images with a single sampler to get an
          * array of sampled images that all share the same sampler.
          * Fortunately, this means that we can more-or-less ignore the
          * sampler when crawling the access chain, but it does leave us
          * with this rather awkward little special-case.
          */
         base = base_val->sampled_image->image;
      } else {
         assert(base_val->value_type == vtn_value_type_deref);
         base = base_val->deref;
      }

      nir_deref_var *deref = nir_deref_as_var(nir_copy_deref(b, &base->deref));
      struct vtn_type *deref_type = vtn_value(b, w[3], vtn_value_type_deref)->deref_type;

      nir_deref *tail = &deref->deref;
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
            if (base_type == GLSL_TYPE_ARRAY ||
                glsl_type_is_matrix(tail->type)) {
               deref_type = deref_type->array_element;
            } else {
               assert(glsl_type_is_vector(tail->type));
               deref_type = ralloc(b, struct vtn_type);
               deref_type->type = glsl_scalar_type(base_type);
            }

            deref_arr->deref.type = deref_type->type;

            if (idx_val->value_type == vtn_value_type_constant) {
               unsigned idx = idx_val->constant->value.u[0];
               deref_arr->deref_array_type = nir_deref_array_type_direct;
               deref_arr->base_offset = idx;
            } else {
               assert(idx_val->value_type == vtn_value_type_ssa);
               assert(glsl_type_is_scalar(idx_val->ssa->type));
               deref_arr->deref_array_type = nir_deref_array_type_indirect;
               deref_arr->base_offset = 0;
               deref_arr->indirect = nir_src_for_ssa(idx_val->ssa->def);
            }
            tail->child = &deref_arr->deref;
            break;
         }

         case GLSL_TYPE_STRUCT: {
            assert(idx_val->value_type == vtn_value_type_constant);
            unsigned idx = idx_val->constant->value.u[0];
            deref_type = deref_type->members[idx];
            nir_deref_struct *deref_struct = nir_deref_struct_create(b, idx);
            deref_struct->deref.type = deref_type->type;
            tail->child = &deref_struct->deref;
            break;
         }
         default:
            unreachable("Invalid type for deref");
         }

         if (deref_type->is_builtin) {
            /* If we encounter a builtin, we throw away the ress of the
             * access chain, jump to the builtin, and keep building.
             */
            const struct glsl_type *builtin_type = deref_type->type;

            nir_deref_array *per_vertex_deref = NULL;
            if (glsl_type_is_array(base->var->type)) {
               /* This builtin is a per-vertex builtin */
               assert(b->shader->stage == MESA_SHADER_GEOMETRY);
               assert(base->var->data.mode == nir_var_shader_in);
               builtin_type = glsl_array_type(builtin_type,
                                              b->shader->info.gs.vertices_in);

               /* The first non-var deref should be an array deref. */
               assert(deref->deref.child->deref_type ==
                      nir_deref_type_array);
               per_vertex_deref = nir_deref_as_array(deref->deref.child);
            }

            nir_variable *builtin = get_builtin_variable(b,
                                                         base->var->data.mode,
                                                         builtin_type,
                                                         deref_type->builtin);
            deref = nir_deref_var_create(b, builtin);

            if (per_vertex_deref) {
               /* Since deref chains start at the variable, we can just
                * steal that link and use it.
                */
               deref->deref.child = &per_vertex_deref->deref;
               per_vertex_deref->deref.child = NULL;
               per_vertex_deref->deref.type =
                  glsl_get_array_element(builtin_type);

               tail = &per_vertex_deref->deref;
            } else {
               tail = &deref->deref;
            }
         } else {
            tail = tail->child;
         }
      }

      /* For uniform blocks, we don't resolve the access chain until we
       * actually access the variable, so we need to keep around the original
       * type of the variable.
       */
      if (variable_is_external_block(base->var))
         deref_type = vtn_value(b, w[3], vtn_value_type_deref)->deref_type;

      if (base_val->value_type == vtn_value_type_sampled_image) {
         struct vtn_value *val =
            vtn_push_value(b, w[2], vtn_value_type_sampled_image);
         val->sampled_image = ralloc(b, struct vtn_sampled_image);
         val->sampled_image->image = deref;
         val->sampled_image->sampler = base_val->sampled_image->sampler;
      } else {
         struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_deref);
         val->deref = deref;
         val->deref_type = deref_type;
      }

      break;
   }

   case SpvOpCopyMemory: {
      struct vtn_value *dest = vtn_value(b, w[1], vtn_value_type_deref);
      struct vtn_value *src = vtn_value(b, w[2], vtn_value_type_deref);

      vtn_variable_copy(b, dest->deref, dest->deref_type,
                        src->deref, src->deref_type);
      break;
   }

   case SpvOpLoad: {
      nir_deref_var *src = vtn_value(b, w[3], vtn_value_type_deref)->deref;
      struct vtn_type *src_type =
         vtn_value(b, w[3], vtn_value_type_deref)->deref_type;

      if (src->var->interface_type &&
          (glsl_type_is_sampler(src->var->interface_type) ||
           glsl_type_is_image(src->var->interface_type))) {
         vtn_push_value(b, w[2], vtn_value_type_deref)->deref = src;
         return;
      }

      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
      val->ssa = vtn_variable_load(b, src, src_type);
      break;
   }

   case SpvOpStore: {
      nir_deref_var *dest = vtn_value(b, w[1], vtn_value_type_deref)->deref;
      struct vtn_type *dest_type =
         vtn_value(b, w[1], vtn_value_type_deref)->deref_type;
      struct vtn_ssa_value *src = vtn_ssa_value(b, w[2]);
      vtn_variable_store(b, src, dest, dest_type);
      break;
   }

   case SpvOpCopyMemorySized:
   case SpvOpArrayLength:
   default:
      unreachable("Unhandled opcode");
   }
}

static void
vtn_handle_function_call(struct vtn_builder *b, SpvOp opcode,
                         const uint32_t *w, unsigned count)
{
   struct nir_function *callee =
      vtn_value(b, w[3], vtn_value_type_function)->func->impl->function;

   nir_call_instr *call = nir_call_instr_create(b->nb.shader, callee);
   for (unsigned i = 0; i < call->num_params; i++) {
      unsigned arg_id = w[4 + i];
      struct vtn_value *arg = vtn_untyped_value(b, arg_id);
      if (arg->value_type == vtn_value_type_deref) {
         call->params[i] =
            nir_deref_as_var(nir_copy_deref(call, &arg->deref->deref));
      } else {
         struct vtn_ssa_value *arg_ssa = vtn_ssa_value(b, arg_id);

         /* Make a temporary to store the argument in */
         nir_variable *tmp =
            nir_local_variable_create(b->impl, arg_ssa->type, "arg_tmp");
         call->params[i] = nir_deref_var_create(call, tmp);

         vtn_variable_store(b, arg_ssa, call->params[i], arg->type);
      }
   }

   nir_variable *out_tmp = NULL;
   if (!glsl_type_is_void(callee->return_type)) {
      out_tmp = nir_local_variable_create(b->impl, callee->return_type,
                                          "out_tmp");
      call->return_deref = nir_deref_var_create(call, out_tmp);
   }

   nir_builder_instr_insert(&b->nb, &call->instr);

   if (glsl_type_is_void(callee->return_type)) {
      vtn_push_value(b, w[2], vtn_value_type_undef);
   } else {
      struct vtn_type *rettype = vtn_value(b, w[1], vtn_value_type_type)->type;
      struct vtn_value *retval = vtn_push_value(b, w[2], vtn_value_type_ssa);
      retval->ssa = vtn_variable_load(b, call->return_deref, rettype);
   }
}

struct vtn_ssa_value *
vtn_create_ssa_value(struct vtn_builder *b, const struct glsl_type *type)
{
   struct vtn_ssa_value *val = rzalloc(b, struct vtn_ssa_value);
   val->type = type;

   if (!glsl_type_is_vector_or_scalar(type)) {
      unsigned elems = glsl_get_length(type);
      val->elems = ralloc_array(b, struct vtn_ssa_value *, elems);
      for (unsigned i = 0; i < elems; i++) {
         const struct glsl_type *child_type;

         switch (glsl_get_base_type(type)) {
         case GLSL_TYPE_INT:
         case GLSL_TYPE_UINT:
         case GLSL_TYPE_BOOL:
         case GLSL_TYPE_FLOAT:
         case GLSL_TYPE_DOUBLE:
            child_type = glsl_get_column_type(type);
            break;
         case GLSL_TYPE_ARRAY:
            child_type = glsl_get_array_element(type);
            break;
         case GLSL_TYPE_STRUCT:
            child_type = glsl_get_struct_field(type, i);
            break;
         default:
            unreachable("unkown base type");
         }

         val->elems[i] = vtn_create_ssa_value(b, child_type);
      }
   }

   return val;
}

static nir_tex_src
vtn_tex_src(struct vtn_builder *b, unsigned index, nir_tex_src_type type)
{
   nir_tex_src src;
   src.src = nir_src_for_ssa(vtn_ssa_value(b, index)->def);
   src.src_type = type;
   return src;
}

static void
vtn_handle_texture(struct vtn_builder *b, SpvOp opcode,
                   const uint32_t *w, unsigned count)
{
   if (opcode == SpvOpSampledImage) {
      struct vtn_value *val =
         vtn_push_value(b, w[2], vtn_value_type_sampled_image);
      val->sampled_image = ralloc(b, struct vtn_sampled_image);
      val->sampled_image->image =
         vtn_value(b, w[3], vtn_value_type_deref)->deref;
      val->sampled_image->sampler =
         vtn_value(b, w[4], vtn_value_type_deref)->deref;
      return;
   }

   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);

   struct vtn_sampled_image sampled;
   struct vtn_value *sampled_val = vtn_untyped_value(b, w[3]);
   if (sampled_val->value_type == vtn_value_type_sampled_image) {
      sampled = *sampled_val->sampled_image;
   } else {
      assert(sampled_val->value_type == vtn_value_type_deref);
      sampled.image = NULL;
      sampled.sampler = sampled_val->deref;
   }

   nir_tex_src srcs[8]; /* 8 should be enough */
   nir_tex_src *p = srcs;

   unsigned idx = 4;

   unsigned coord_components = 0;
   switch (opcode) {
   case SpvOpImageSampleImplicitLod:
   case SpvOpImageSampleExplicitLod:
   case SpvOpImageSampleDrefImplicitLod:
   case SpvOpImageSampleDrefExplicitLod:
   case SpvOpImageSampleProjImplicitLod:
   case SpvOpImageSampleProjExplicitLod:
   case SpvOpImageSampleProjDrefImplicitLod:
   case SpvOpImageSampleProjDrefExplicitLod:
   case SpvOpImageFetch:
   case SpvOpImageGather:
   case SpvOpImageDrefGather:
   case SpvOpImageQueryLod: {
      /* All these types have the coordinate as their first real argument */
      struct vtn_ssa_value *coord = vtn_ssa_value(b, w[idx++]);
      coord_components = glsl_get_vector_elements(coord->type);
      p->src = nir_src_for_ssa(coord->def);
      p->src_type = nir_tex_src_coord;
      p++;
      break;
   }

   default:
      break;
   }

   /* These all have an explicit depth value as their next source */
   switch (opcode) {
   case SpvOpImageSampleDrefImplicitLod:
   case SpvOpImageSampleDrefExplicitLod:
   case SpvOpImageSampleProjDrefImplicitLod:
   case SpvOpImageSampleProjDrefExplicitLod:
      (*p++) = vtn_tex_src(b, w[idx++], nir_tex_src_comparitor);
      break;
   default:
      break;
   }

   /* Figure out the base texture operation */
   nir_texop texop;
   switch (opcode) {
   case SpvOpImageSampleImplicitLod:
   case SpvOpImageSampleDrefImplicitLod:
   case SpvOpImageSampleProjImplicitLod:
   case SpvOpImageSampleProjDrefImplicitLod:
      texop = nir_texop_tex;
      break;

   case SpvOpImageSampleExplicitLod:
   case SpvOpImageSampleDrefExplicitLod:
   case SpvOpImageSampleProjExplicitLod:
   case SpvOpImageSampleProjDrefExplicitLod:
      texop = nir_texop_txl;
      break;

   case SpvOpImageFetch:
      texop = nir_texop_txf;
      break;

   case SpvOpImageGather:
   case SpvOpImageDrefGather:
      texop = nir_texop_tg4;
      break;

   case SpvOpImageQuerySizeLod:
   case SpvOpImageQuerySize:
      texop = nir_texop_txs;
      break;

   case SpvOpImageQueryLod:
      texop = nir_texop_lod;
      break;

   case SpvOpImageQueryLevels:
      texop = nir_texop_query_levels;
      break;

   case SpvOpImageQuerySamples:
   default:
      unreachable("Unhandled opcode");
   }

   /* Now we need to handle some number of optional arguments */
   if (idx < count) {
      uint32_t operands = w[idx++];

      if (operands & SpvImageOperandsBiasMask) {
         assert(texop == nir_texop_tex);
         texop = nir_texop_txb;
         (*p++) = vtn_tex_src(b, w[idx++], nir_tex_src_bias);
      }

      if (operands & SpvImageOperandsLodMask) {
         assert(texop == nir_texop_txl || texop == nir_texop_txf ||
                texop == nir_texop_txs);
         (*p++) = vtn_tex_src(b, w[idx++], nir_tex_src_lod);
      }

      if (operands & SpvImageOperandsGradMask) {
         assert(texop == nir_texop_tex);
         texop = nir_texop_txd;
         (*p++) = vtn_tex_src(b, w[idx++], nir_tex_src_ddx);
         (*p++) = vtn_tex_src(b, w[idx++], nir_tex_src_ddy);
      }

      if (operands & SpvImageOperandsOffsetMask ||
          operands & SpvImageOperandsConstOffsetMask)
         (*p++) = vtn_tex_src(b, w[idx++], nir_tex_src_offset);

      if (operands & SpvImageOperandsConstOffsetsMask)
         assert(!"Constant offsets to texture gather not yet implemented");

      if (operands & SpvImageOperandsSampleMask) {
         assert(texop == nir_texop_txf);
         texop = nir_texop_txf_ms;
         (*p++) = vtn_tex_src(b, w[idx++], nir_tex_src_ms_index);
      }
   }
   /* We should have now consumed exactly all of the arguments */
   assert(idx == count);

   nir_tex_instr *instr = nir_tex_instr_create(b->shader, p - srcs);

   const struct glsl_type *sampler_type =
      nir_deref_tail(&sampled.sampler->deref)->type;
   instr->sampler_dim = glsl_get_sampler_dim(sampler_type);

   switch (glsl_get_sampler_result_type(sampler_type)) {
   case GLSL_TYPE_FLOAT:   instr->dest_type = nir_type_float;     break;
   case GLSL_TYPE_INT:     instr->dest_type = nir_type_int;       break;
   case GLSL_TYPE_UINT:    instr->dest_type = nir_type_uint;  break;
   case GLSL_TYPE_BOOL:    instr->dest_type = nir_type_bool;      break;
   default:
      unreachable("Invalid base type for sampler result");
   }

   instr->op = texop;
   memcpy(instr->src, srcs, instr->num_srcs * sizeof(*instr->src));
   instr->coord_components = coord_components;
   instr->is_array = glsl_sampler_type_is_array(sampler_type);
   instr->is_shadow = glsl_sampler_type_is_shadow(sampler_type);

   instr->sampler =
      nir_deref_as_var(nir_copy_deref(instr, &sampled.sampler->deref));
   if (sampled.image) {
      instr->texture =
         nir_deref_as_var(nir_copy_deref(instr, &sampled.image->deref));
   } else {
      instr->texture = NULL;
   }

   nir_ssa_dest_init(&instr->instr, &instr->dest, 4, NULL);
   val->ssa = vtn_create_ssa_value(b, glsl_vector_type(GLSL_TYPE_FLOAT, 4));
   val->ssa->def = &instr->dest.ssa;

   nir_builder_instr_insert(&b->nb, &instr->instr);
}

static nir_ssa_def *
get_image_coord(struct vtn_builder *b, uint32_t value)
{
   struct vtn_ssa_value *coord = vtn_ssa_value(b, value);

   /* The image_load_store intrinsics assume a 4-dim coordinate */
   unsigned dim = glsl_get_vector_elements(coord->type);
   unsigned swizzle[4];
   for (unsigned i = 0; i < 4; i++)
      swizzle[i] = MIN2(i, dim - 1);

   return nir_swizzle(&b->nb, coord->def, swizzle, 4, false);
}

static void
vtn_handle_image(struct vtn_builder *b, SpvOp opcode,
                 const uint32_t *w, unsigned count)
{
   /* Just get this one out of the way */
   if (opcode == SpvOpImageTexelPointer) {
      struct vtn_value *val =
         vtn_push_value(b, w[2], vtn_value_type_image_pointer);
      val->image = ralloc(b, struct vtn_image_pointer);

      val->image->deref = vtn_value(b, w[3], vtn_value_type_deref)->deref;
      val->image->coord = get_image_coord(b, w[4]);
      val->image->sample = vtn_ssa_value(b, w[5])->def;
      return;
   }

   struct vtn_image_pointer image;

   switch (opcode) {
   case SpvOpAtomicExchange:
   case SpvOpAtomicCompareExchange:
   case SpvOpAtomicCompareExchangeWeak:
   case SpvOpAtomicIIncrement:
   case SpvOpAtomicIDecrement:
   case SpvOpAtomicIAdd:
   case SpvOpAtomicISub:
   case SpvOpAtomicSMin:
   case SpvOpAtomicUMin:
   case SpvOpAtomicSMax:
   case SpvOpAtomicUMax:
   case SpvOpAtomicAnd:
   case SpvOpAtomicOr:
   case SpvOpAtomicXor:
      image = *vtn_value(b, w[3], vtn_value_type_image_pointer)->image;
      break;

   case SpvOpImageRead:
      image.deref = vtn_value(b, w[3], vtn_value_type_deref)->deref;
      image.coord = get_image_coord(b, w[4]);

      if (count > 5 && (w[5] & SpvImageOperandsSampleMask)) {
         assert(w[5] == SpvImageOperandsSampleMask);
         image.sample = vtn_ssa_value(b, w[6])->def;
      } else {
         image.sample = nir_ssa_undef(&b->nb, 1);
      }
      break;

   case SpvOpImageWrite:
      image.deref = vtn_value(b, w[1], vtn_value_type_deref)->deref;
      image.coord = get_image_coord(b, w[2]);

      /* texel = w[3] */

      if (count > 4 && (w[4] & SpvImageOperandsSampleMask)) {
         assert(w[4] == SpvImageOperandsSampleMask);
         image.sample = vtn_ssa_value(b, w[5])->def;
      } else {
         image.sample = nir_ssa_undef(&b->nb, 1);
      }
      break;

   default:
      unreachable("Invalid image opcode");
   }

   nir_intrinsic_op op;
   switch (opcode) {
#define OP(S, N) case SpvOp##S: op = nir_intrinsic_image_##N; break;
   OP(ImageRead,              load)
   OP(ImageWrite,             store)
   OP(AtomicExchange,         atomic_exchange)
   OP(AtomicCompareExchange,  atomic_comp_swap)
   OP(AtomicIIncrement,       atomic_add)
   OP(AtomicIDecrement,       atomic_add)
   OP(AtomicIAdd,             atomic_add)
   OP(AtomicISub,             atomic_add)
   OP(AtomicSMin,             atomic_min)
   OP(AtomicUMin,             atomic_min)
   OP(AtomicSMax,             atomic_max)
   OP(AtomicUMax,             atomic_max)
   OP(AtomicAnd,              atomic_and)
   OP(AtomicOr,               atomic_or)
   OP(AtomicXor,              atomic_xor)
#undef OP
   default:
      unreachable("Invalid image opcode");
   }

   nir_intrinsic_instr *intrin = nir_intrinsic_instr_create(b->shader, op);
   intrin->variables[0] =
      nir_deref_as_var(nir_copy_deref(&intrin->instr, &image.deref->deref));
   intrin->src[0] = nir_src_for_ssa(image.coord);
   intrin->src[1] = nir_src_for_ssa(image.sample);

   switch (opcode) {
   case SpvOpImageRead:
      break;
   case SpvOpImageWrite:
      intrin->src[2] = nir_src_for_ssa(vtn_ssa_value(b, w[3])->def);
      break;
   case SpvOpAtomicIIncrement:
      intrin->src[2] = nir_src_for_ssa(nir_imm_int(&b->nb, 1));
      break;
   case SpvOpAtomicIDecrement:
      intrin->src[2] = nir_src_for_ssa(nir_imm_int(&b->nb, -1));
      break;

   case SpvOpAtomicExchange:
   case SpvOpAtomicIAdd:
   case SpvOpAtomicSMin:
   case SpvOpAtomicUMin:
   case SpvOpAtomicSMax:
   case SpvOpAtomicUMax:
   case SpvOpAtomicAnd:
   case SpvOpAtomicOr:
   case SpvOpAtomicXor:
      intrin->src[2] = nir_src_for_ssa(vtn_ssa_value(b, w[6])->def);
      break;

   case SpvOpAtomicCompareExchange:
      intrin->src[2] = nir_src_for_ssa(vtn_ssa_value(b, w[7])->def);
      intrin->src[3] = nir_src_for_ssa(vtn_ssa_value(b, w[6])->def);
      break;

   case SpvOpAtomicISub:
      intrin->src[2] = nir_src_for_ssa(nir_ineg(&b->nb, vtn_ssa_value(b, w[6])->def));
      break;

   default:
      unreachable("Invalid image opcode");
   }

   if (opcode != SpvOpImageWrite) {
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
      struct vtn_type *type = vtn_value(b, w[1], vtn_value_type_type)->type;
      nir_ssa_dest_init(&intrin->instr, &intrin->dest,
                        glsl_get_vector_elements(type->type), NULL);
      val->ssa = vtn_create_ssa_value(b, type->type);
      val->ssa->def = &intrin->dest.ssa;
   }

   nir_builder_instr_insert(&b->nb, &intrin->instr);
}

static void
vtn_handle_ssbo_atomic(struct vtn_builder *b, SpvOp opcode,
                       const uint32_t *w, unsigned count)
{
   struct vtn_value *pointer = vtn_value(b, w[3], vtn_value_type_deref);
   struct vtn_type *type = pointer->deref_type;
   nir_deref *deref = &pointer->deref->deref;
   nir_ssa_def *index = get_vulkan_resource_index(b, &deref, &type);

   nir_ssa_def *offset = nir_imm_int(&b->nb, 0);
   while (deref->child) {
      deref = deref->child;
      switch (deref->deref_type) {
      case nir_deref_type_array:
         offset = nir_iadd(&b->nb, offset,
                           nir_imul(&b->nb, deref_array_offset(b, deref),
                                    nir_imm_int(&b->nb, type->stride)));
         type = type->array_element;
         continue;

      case nir_deref_type_struct: {
         unsigned member = nir_deref_as_struct(deref)->index;
         offset = nir_iadd(&b->nb, offset,
                           nir_imm_int(&b->nb, type->offsets[member]));
         type = type->members[member];
         continue;
      }

      default:
         unreachable("Invalid deref type");
      }
   }

   /*
   SpvScope scope = w[4];
   SpvMemorySemanticsMask semantics = w[5];
   */

   nir_intrinsic_op op;
   switch (opcode) {
#define OP(S, N) case SpvOp##S: op = nir_intrinsic_ssbo_##N; break;
   OP(AtomicExchange,         atomic_exchange)
   OP(AtomicCompareExchange,  atomic_comp_swap)
   OP(AtomicIIncrement,       atomic_add)
   OP(AtomicIDecrement,       atomic_add)
   OP(AtomicIAdd,             atomic_add)
   OP(AtomicISub,             atomic_add)
   OP(AtomicSMin,             atomic_imin)
   OP(AtomicUMin,             atomic_umin)
   OP(AtomicSMax,             atomic_imax)
   OP(AtomicUMax,             atomic_umax)
   OP(AtomicAnd,              atomic_and)
   OP(AtomicOr,               atomic_or)
   OP(AtomicXor,              atomic_xor)
#undef OP
   default:
      unreachable("Invalid SSBO atomic");
   }

   nir_intrinsic_instr *atomic = nir_intrinsic_instr_create(b->nb.shader, op);
   atomic->src[0] = nir_src_for_ssa(index);
   atomic->src[1] = nir_src_for_ssa(offset);

   switch (opcode) {
   case SpvOpAtomicIIncrement:
      atomic->src[2] = nir_src_for_ssa(nir_imm_int(&b->nb, 1));
      break;

   case SpvOpAtomicIDecrement:
      atomic->src[2] = nir_src_for_ssa(nir_imm_int(&b->nb, -1));
      break;

   case SpvOpAtomicISub:
      atomic->src[2] =
         nir_src_for_ssa(nir_ineg(&b->nb, vtn_ssa_value(b, w[6])->def));
      break;

   case SpvOpAtomicCompareExchange:
      atomic->src[2] = nir_src_for_ssa(vtn_ssa_value(b, w[7])->def);
      atomic->src[3] = nir_src_for_ssa(vtn_ssa_value(b, w[8])->def);
      break;
      /* Fall through */

   case SpvOpAtomicExchange:
   case SpvOpAtomicIAdd:
   case SpvOpAtomicSMin:
   case SpvOpAtomicUMin:
   case SpvOpAtomicSMax:
   case SpvOpAtomicUMax:
   case SpvOpAtomicAnd:
   case SpvOpAtomicOr:
   case SpvOpAtomicXor:
      atomic->src[2] = nir_src_for_ssa(vtn_ssa_value(b, w[6])->def);
      break;

   default:
      unreachable("Invalid SSBO atomic");
   }

   nir_ssa_dest_init(&atomic->instr, &atomic->dest, 1, NULL);

   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   val->ssa = rzalloc(b, struct vtn_ssa_value);
   val->ssa->def = &atomic->dest.ssa;
   val->ssa->type = type->type;

   nir_builder_instr_insert(&b->nb, &atomic->instr);
}

static nir_alu_instr *
create_vec(nir_shader *shader, unsigned num_components)
{
   nir_op op;
   switch (num_components) {
   case 1: op = nir_op_fmov; break;
   case 2: op = nir_op_vec2; break;
   case 3: op = nir_op_vec3; break;
   case 4: op = nir_op_vec4; break;
   default: unreachable("bad vector size");
   }

   nir_alu_instr *vec = nir_alu_instr_create(shader, op);
   nir_ssa_dest_init(&vec->instr, &vec->dest.dest, num_components, NULL);
   vec->dest.write_mask = (1 << num_components) - 1;

   return vec;
}

struct vtn_ssa_value *
vtn_ssa_transpose(struct vtn_builder *b, struct vtn_ssa_value *src)
{
   if (src->transposed)
      return src->transposed;

   struct vtn_ssa_value *dest =
      vtn_create_ssa_value(b, glsl_transposed_type(src->type));

   for (unsigned i = 0; i < glsl_get_matrix_columns(dest->type); i++) {
      nir_alu_instr *vec = create_vec(b->shader,
                                      glsl_get_matrix_columns(src->type));
      if (glsl_type_is_vector_or_scalar(src->type)) {
          vec->src[0].src = nir_src_for_ssa(src->def);
          vec->src[0].swizzle[0] = i;
      } else {
         for (unsigned j = 0; j < glsl_get_matrix_columns(src->type); j++) {
            vec->src[j].src = nir_src_for_ssa(src->elems[j]->def);
            vec->src[j].swizzle[0] = i;
         }
      }
      nir_builder_instr_insert(&b->nb, &vec->instr);
      dest->elems[i]->def = &vec->dest.dest.ssa;
   }

   dest->transposed = src;

   return dest;
}

static nir_ssa_def *
vtn_vector_extract(struct vtn_builder *b, nir_ssa_def *src, unsigned index)
{
   unsigned swiz[4] = { index };
   return nir_swizzle(&b->nb, src, swiz, 1, true);
}


static nir_ssa_def *
vtn_vector_insert(struct vtn_builder *b, nir_ssa_def *src, nir_ssa_def *insert,
                  unsigned index)
{
   nir_alu_instr *vec = create_vec(b->shader, src->num_components);

   for (unsigned i = 0; i < src->num_components; i++) {
      if (i == index) {
         vec->src[i].src = nir_src_for_ssa(insert);
      } else {
         vec->src[i].src = nir_src_for_ssa(src);
         vec->src[i].swizzle[0] = i;
      }
   }

   nir_builder_instr_insert(&b->nb, &vec->instr);

   return &vec->dest.dest.ssa;
}

static nir_ssa_def *
vtn_vector_extract_dynamic(struct vtn_builder *b, nir_ssa_def *src,
                           nir_ssa_def *index)
{
   nir_ssa_def *dest = vtn_vector_extract(b, src, 0);
   for (unsigned i = 1; i < src->num_components; i++)
      dest = nir_bcsel(&b->nb, nir_ieq(&b->nb, index, nir_imm_int(&b->nb, i)),
                       vtn_vector_extract(b, src, i), dest);

   return dest;
}

static nir_ssa_def *
vtn_vector_insert_dynamic(struct vtn_builder *b, nir_ssa_def *src,
                          nir_ssa_def *insert, nir_ssa_def *index)
{
   nir_ssa_def *dest = vtn_vector_insert(b, src, insert, 0);
   for (unsigned i = 1; i < src->num_components; i++)
      dest = nir_bcsel(&b->nb, nir_ieq(&b->nb, index, nir_imm_int(&b->nb, i)),
                       vtn_vector_insert(b, src, insert, i), dest);

   return dest;
}

static nir_ssa_def *
vtn_vector_shuffle(struct vtn_builder *b, unsigned num_components,
                   nir_ssa_def *src0, nir_ssa_def *src1,
                   const uint32_t *indices)
{
   nir_alu_instr *vec = create_vec(b->shader, num_components);

   nir_ssa_undef_instr *undef = nir_ssa_undef_instr_create(b->shader, 1);
   nir_builder_instr_insert(&b->nb, &undef->instr);

   for (unsigned i = 0; i < num_components; i++) {
      uint32_t index = indices[i];
      if (index == 0xffffffff) {
         vec->src[i].src = nir_src_for_ssa(&undef->def);
      } else if (index < src0->num_components) {
         vec->src[i].src = nir_src_for_ssa(src0);
         vec->src[i].swizzle[0] = index;
      } else {
         vec->src[i].src = nir_src_for_ssa(src1);
         vec->src[i].swizzle[0] = index - src0->num_components;
      }
   }

   nir_builder_instr_insert(&b->nb, &vec->instr);

   return &vec->dest.dest.ssa;
}

/*
 * Concatentates a number of vectors/scalars together to produce a vector
 */
static nir_ssa_def *
vtn_vector_construct(struct vtn_builder *b, unsigned num_components,
                     unsigned num_srcs, nir_ssa_def **srcs)
{
   nir_alu_instr *vec = create_vec(b->shader, num_components);

   unsigned dest_idx = 0;
   for (unsigned i = 0; i < num_srcs; i++) {
      nir_ssa_def *src = srcs[i];
      for (unsigned j = 0; j < src->num_components; j++) {
         vec->src[dest_idx].src = nir_src_for_ssa(src);
         vec->src[dest_idx].swizzle[0] = j;
         dest_idx++;
      }
   }

   nir_builder_instr_insert(&b->nb, &vec->instr);

   return &vec->dest.dest.ssa;
}

static struct vtn_ssa_value *
vtn_composite_copy(void *mem_ctx, struct vtn_ssa_value *src)
{
   struct vtn_ssa_value *dest = rzalloc(mem_ctx, struct vtn_ssa_value);
   dest->type = src->type;

   if (glsl_type_is_vector_or_scalar(src->type)) {
      dest->def = src->def;
   } else {
      unsigned elems = glsl_get_length(src->type);

      dest->elems = ralloc_array(mem_ctx, struct vtn_ssa_value *, elems);
      for (unsigned i = 0; i < elems; i++)
         dest->elems[i] = vtn_composite_copy(mem_ctx, src->elems[i]);
   }

   return dest;
}

static struct vtn_ssa_value *
vtn_composite_insert(struct vtn_builder *b, struct vtn_ssa_value *src,
                     struct vtn_ssa_value *insert, const uint32_t *indices,
                     unsigned num_indices)
{
   struct vtn_ssa_value *dest = vtn_composite_copy(b, src);

   struct vtn_ssa_value *cur = dest;
   unsigned i;
   for (i = 0; i < num_indices - 1; i++) {
      cur = cur->elems[indices[i]];
   }

   if (glsl_type_is_vector_or_scalar(cur->type)) {
      /* According to the SPIR-V spec, OpCompositeInsert may work down to
       * the component granularity. In that case, the last index will be
       * the index to insert the scalar into the vector.
       */

      cur->def = vtn_vector_insert(b, cur->def, insert->def, indices[i]);
   } else {
      cur->elems[indices[i]] = insert;
   }

   return dest;
}

static struct vtn_ssa_value *
vtn_composite_extract(struct vtn_builder *b, struct vtn_ssa_value *src,
                      const uint32_t *indices, unsigned num_indices)
{
   struct vtn_ssa_value *cur = src;
   for (unsigned i = 0; i < num_indices; i++) {
      if (glsl_type_is_vector_or_scalar(cur->type)) {
         assert(i == num_indices - 1);
         /* According to the SPIR-V spec, OpCompositeExtract may work down to
          * the component granularity. The last index will be the index of the
          * vector to extract.
          */

         struct vtn_ssa_value *ret = rzalloc(b, struct vtn_ssa_value);
         ret->type = glsl_scalar_type(glsl_get_base_type(cur->type));
         ret->def = vtn_vector_extract(b, cur->def, indices[i]);
         return ret;
      } else {
         cur = cur->elems[indices[i]];
      }
   }

   return cur;
}

static void
vtn_handle_composite(struct vtn_builder *b, SpvOp opcode,
                     const uint32_t *w, unsigned count)
{
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   const struct glsl_type *type =
      vtn_value(b, w[1], vtn_value_type_type)->type->type;
   val->ssa = vtn_create_ssa_value(b, type);

   switch (opcode) {
   case SpvOpVectorExtractDynamic:
      val->ssa->def = vtn_vector_extract_dynamic(b, vtn_ssa_value(b, w[3])->def,
                                                 vtn_ssa_value(b, w[4])->def);
      break;

   case SpvOpVectorInsertDynamic:
      val->ssa->def = vtn_vector_insert_dynamic(b, vtn_ssa_value(b, w[3])->def,
                                                vtn_ssa_value(b, w[4])->def,
                                                vtn_ssa_value(b, w[5])->def);
      break;

   case SpvOpVectorShuffle:
      val->ssa->def = vtn_vector_shuffle(b, glsl_get_vector_elements(type),
                                         vtn_ssa_value(b, w[3])->def,
                                         vtn_ssa_value(b, w[4])->def,
                                         w + 5);
      break;

   case SpvOpCompositeConstruct: {
      unsigned elems = count - 3;
      if (glsl_type_is_vector_or_scalar(type)) {
         nir_ssa_def *srcs[4];
         for (unsigned i = 0; i < elems; i++)
            srcs[i] = vtn_ssa_value(b, w[3 + i])->def;
         val->ssa->def =
            vtn_vector_construct(b, glsl_get_vector_elements(type),
                                 elems, srcs);
      } else {
         val->ssa->elems = ralloc_array(b, struct vtn_ssa_value *, elems);
         for (unsigned i = 0; i < elems; i++)
            val->ssa->elems[i] = vtn_ssa_value(b, w[3 + i]);
      }
      break;
   }
   case SpvOpCompositeExtract:
      val->ssa = vtn_composite_extract(b, vtn_ssa_value(b, w[3]),
                                       w + 4, count - 4);
      break;

   case SpvOpCompositeInsert:
      val->ssa = vtn_composite_insert(b, vtn_ssa_value(b, w[4]),
                                      vtn_ssa_value(b, w[3]),
                                      w + 5, count - 5);
      break;

   case SpvOpCopyObject:
      val->ssa = vtn_composite_copy(b, vtn_ssa_value(b, w[3]));
      break;

   default:
      unreachable("unknown composite operation");
   }
}

static void
vtn_handle_barrier(struct vtn_builder *b, SpvOp opcode,
                   const uint32_t *w, unsigned count)
{
   nir_intrinsic_op intrinsic_op;
   switch (opcode) {
   case SpvOpEmitVertex:
   case SpvOpEmitStreamVertex:
      intrinsic_op = nir_intrinsic_emit_vertex;
      break;
   case SpvOpEndPrimitive:
   case SpvOpEndStreamPrimitive:
      intrinsic_op = nir_intrinsic_end_primitive;
      break;
   case SpvOpMemoryBarrier:
      intrinsic_op = nir_intrinsic_memory_barrier;
      break;
   case SpvOpControlBarrier:
      intrinsic_op = nir_intrinsic_barrier;
      break;
   default:
      unreachable("unknown barrier instruction");
   }

   nir_intrinsic_instr *intrin =
      nir_intrinsic_instr_create(b->shader, intrinsic_op);

   if (opcode == SpvOpEmitStreamVertex || opcode == SpvOpEndStreamPrimitive)
      intrin->const_index[0] = w[1];

   nir_builder_instr_insert(&b->nb, &intrin->instr);
}

static void
vtn_handle_phi_first_pass(struct vtn_builder *b, const uint32_t *w)
{
   /* For handling phi nodes, we do a poor-man's out-of-ssa on the spot.
    * For each phi, we create a variable with the appropreate type and do a
    * load from that variable.  Then, in a second pass, we add stores to
    * that variable to each of the predecessor blocks.
    *
    * We could do something more intelligent here.  However, in order to
    * handle loops and things properly, we really need dominance
    * information.  It would end up basically being the into-SSA algorithm
    * all over again.  It's easier if we just let lower_vars_to_ssa do that
    * for us instead of repeating it here.
    */
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);

   struct vtn_type *type = vtn_value(b, w[1], vtn_value_type_type)->type;
   nir_variable *phi_var =
      nir_local_variable_create(b->nb.impl, type->type, "phi");
   _mesa_hash_table_insert(b->phi_table, w, phi_var);

   val->ssa = vtn_variable_load(b, nir_deref_var_create(b, phi_var), type);
}

static bool
vtn_handle_phi_second_pass(struct vtn_builder *b, SpvOp opcode,
                           const uint32_t *w, unsigned count)
{
   if (opcode == SpvOpLabel) {
      b->block = vtn_value(b, w[1], vtn_value_type_block)->block;
      return true;
   }

   if (opcode != SpvOpPhi)
      return true;

   struct hash_entry *phi_entry = _mesa_hash_table_search(b->phi_table, w);
   assert(phi_entry);
   nir_variable *phi_var = phi_entry->data;

   struct vtn_type *type = vtn_value(b, w[1], vtn_value_type_type)->type;

   for (unsigned i = 3; i < count; i += 2) {
      struct vtn_ssa_value *src = vtn_ssa_value(b, w[i]);
      struct vtn_block *pred =
         vtn_value(b, w[i + 1], vtn_value_type_block)->block;

      b->nb.cursor = nir_after_block_before_jump(pred->end_block);

      vtn_variable_store(b, src, nir_deref_var_create(b, phi_var), type);
   }

   return true;
}

static unsigned
gl_primitive_from_spv_execution_mode(SpvExecutionMode mode)
{
   switch (mode) {
   case SpvExecutionModeInputPoints:
   case SpvExecutionModeOutputPoints:
      return 0; /* GL_POINTS */
   case SpvExecutionModeInputLines:
      return 1; /* GL_LINES */
   case SpvExecutionModeInputLinesAdjacency:
      return 0x000A; /* GL_LINE_STRIP_ADJACENCY_ARB */
   case SpvExecutionModeTriangles:
      return 4; /* GL_TRIANGLES */
   case SpvExecutionModeInputTrianglesAdjacency:
      return 0x000C; /* GL_TRIANGLES_ADJACENCY_ARB */
   case SpvExecutionModeQuads:
      return 7; /* GL_QUADS */
   case SpvExecutionModeIsolines:
      return 0x8E7A; /* GL_ISOLINES */
   case SpvExecutionModeOutputLineStrip:
      return 3; /* GL_LINE_STRIP */
   case SpvExecutionModeOutputTriangleStrip:
      return 5; /* GL_TRIANGLE_STRIP */
   default:
      assert(!"Invalid primitive type");
      return 4;
   }
}

static unsigned
vertices_in_from_spv_execution_mode(SpvExecutionMode mode)
{
   switch (mode) {
   case SpvExecutionModeInputPoints:
      return 1;
   case SpvExecutionModeInputLines:
      return 2;
   case SpvExecutionModeInputLinesAdjacency:
      return 4;
   case SpvExecutionModeTriangles:
      return 3;
   case SpvExecutionModeInputTrianglesAdjacency:
      return 6;
   default:
      assert(!"Invalid GS input mode");
      return 0;
   }
}

static gl_shader_stage
stage_for_execution_model(SpvExecutionModel model)
{
   switch (model) {
   case SpvExecutionModelVertex:
      return MESA_SHADER_VERTEX;
   case SpvExecutionModelTessellationControl:
      return MESA_SHADER_TESS_CTRL;
   case SpvExecutionModelTessellationEvaluation:
      return MESA_SHADER_TESS_EVAL;
   case SpvExecutionModelGeometry:
      return MESA_SHADER_GEOMETRY;
   case SpvExecutionModelFragment:
      return MESA_SHADER_FRAGMENT;
   case SpvExecutionModelGLCompute:
      return MESA_SHADER_COMPUTE;
   default:
      unreachable("Unsupported execution model");
   }
}

static bool
vtn_handle_preamble_instruction(struct vtn_builder *b, SpvOp opcode,
                                const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpSource:
   case SpvOpSourceExtension:
   case SpvOpSourceContinued:
   case SpvOpExtension:
      /* Unhandled, but these are for debug so that's ok. */
      break;

   case SpvOpCapability:
      switch ((SpvCapability)w[1]) {
      case SpvCapabilityMatrix:
      case SpvCapabilityShader:
      case SpvCapabilityGeometry:
         break;
      default:
         assert(!"Unsupported capability");
      }
      break;

   case SpvOpExtInstImport:
      vtn_handle_extension(b, opcode, w, count);
      break;

   case SpvOpMemoryModel:
      assert(w[1] == SpvAddressingModelLogical);
      assert(w[2] == SpvMemoryModelGLSL450);
      break;

   case SpvOpEntryPoint: {
      struct vtn_value *entry_point = &b->values[w[2]];
      /* Let this be a name label regardless */
      unsigned name_words;
      entry_point->name = vtn_string_literal(b, &w[3], count - 3, &name_words);

      if (strcmp(entry_point->name, b->entry_point_name) != 0 ||
          stage_for_execution_model(w[1]) != b->entry_point_stage)
         break;

      assert(b->entry_point == NULL);
      b->entry_point = entry_point;
      break;
   }

   case SpvOpString:
      vtn_push_value(b, w[1], vtn_value_type_string)->str =
         vtn_string_literal(b, &w[2], count - 2, NULL);
      break;

   case SpvOpName:
      b->values[w[1]].name = vtn_string_literal(b, &w[2], count - 2, NULL);
      break;

   case SpvOpMemberName:
      /* TODO */
      break;

   case SpvOpExecutionMode:
   case SpvOpDecorationGroup:
   case SpvOpDecorate:
   case SpvOpMemberDecorate:
   case SpvOpGroupDecorate:
   case SpvOpGroupMemberDecorate:
      vtn_handle_decoration(b, opcode, w, count);
      break;

   default:
      return false; /* End of preamble */
   }

   return true;
}

static void
vtn_handle_execution_mode(struct vtn_builder *b, struct vtn_value *entry_point,
                          const struct vtn_decoration *mode, void *data)
{
   assert(b->entry_point == entry_point);

   switch(mode->exec_mode) {
   case SpvExecutionModeOriginUpperLeft:
   case SpvExecutionModeOriginLowerLeft:
      b->origin_upper_left =
         (mode->exec_mode == SpvExecutionModeOriginUpperLeft);
      break;

   case SpvExecutionModeEarlyFragmentTests:
      assert(b->shader->stage == MESA_SHADER_FRAGMENT);
      b->shader->info.fs.early_fragment_tests = true;
      break;

   case SpvExecutionModeInvocations:
      assert(b->shader->stage == MESA_SHADER_GEOMETRY);
      b->shader->info.gs.invocations = MAX2(1, mode->literals[0]);
      break;

   case SpvExecutionModeDepthReplacing:
      assert(b->shader->stage == MESA_SHADER_FRAGMENT);
      b->shader->info.fs.depth_layout = FRAG_DEPTH_LAYOUT_ANY;
      break;
   case SpvExecutionModeDepthGreater:
      assert(b->shader->stage == MESA_SHADER_FRAGMENT);
      b->shader->info.fs.depth_layout = FRAG_DEPTH_LAYOUT_GREATER;
      break;
   case SpvExecutionModeDepthLess:
      assert(b->shader->stage == MESA_SHADER_FRAGMENT);
      b->shader->info.fs.depth_layout = FRAG_DEPTH_LAYOUT_LESS;
      break;
   case SpvExecutionModeDepthUnchanged:
      assert(b->shader->stage == MESA_SHADER_FRAGMENT);
      b->shader->info.fs.depth_layout = FRAG_DEPTH_LAYOUT_UNCHANGED;
      break;

   case SpvExecutionModeLocalSize:
      assert(b->shader->stage == MESA_SHADER_COMPUTE);
      b->shader->info.cs.local_size[0] = mode->literals[0];
      b->shader->info.cs.local_size[1] = mode->literals[1];
      b->shader->info.cs.local_size[2] = mode->literals[2];
      break;
   case SpvExecutionModeLocalSizeHint:
      break; /* Nothing do do with this */

   case SpvExecutionModeOutputVertices:
      assert(b->shader->stage == MESA_SHADER_GEOMETRY);
      b->shader->info.gs.vertices_out = mode->literals[0];
      break;

   case SpvExecutionModeInputPoints:
   case SpvExecutionModeInputLines:
   case SpvExecutionModeInputLinesAdjacency:
   case SpvExecutionModeTriangles:
   case SpvExecutionModeInputTrianglesAdjacency:
   case SpvExecutionModeQuads:
   case SpvExecutionModeIsolines:
      if (b->shader->stage == MESA_SHADER_GEOMETRY) {
         b->shader->info.gs.vertices_in =
            vertices_in_from_spv_execution_mode(mode->exec_mode);
      } else {
         assert(!"Tesselation shaders not yet supported");
      }
      break;

   case SpvExecutionModeOutputPoints:
   case SpvExecutionModeOutputLineStrip:
   case SpvExecutionModeOutputTriangleStrip:
      assert(b->shader->stage == MESA_SHADER_GEOMETRY);
      b->shader->info.gs.output_primitive =
         gl_primitive_from_spv_execution_mode(mode->exec_mode);
      break;

   case SpvExecutionModeSpacingEqual:
   case SpvExecutionModeSpacingFractionalEven:
   case SpvExecutionModeSpacingFractionalOdd:
   case SpvExecutionModeVertexOrderCw:
   case SpvExecutionModeVertexOrderCcw:
   case SpvExecutionModePointMode:
      assert(!"TODO: Add tessellation metadata");
      break;

   case SpvExecutionModePixelCenterInteger:
   case SpvExecutionModeXfb:
      assert(!"Unhandled execution mode");
      break;

   case SpvExecutionModeVecTypeHint:
   case SpvExecutionModeContractionOff:
      break; /* OpenCL */
   }
}

static bool
vtn_handle_variable_or_type_instruction(struct vtn_builder *b, SpvOp opcode,
                                        const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpSource:
   case SpvOpSourceContinued:
   case SpvOpSourceExtension:
   case SpvOpExtension:
   case SpvOpCapability:
   case SpvOpExtInstImport:
   case SpvOpMemoryModel:
   case SpvOpEntryPoint:
   case SpvOpExecutionMode:
   case SpvOpString:
   case SpvOpName:
   case SpvOpMemberName:
   case SpvOpDecorationGroup:
   case SpvOpDecorate:
   case SpvOpMemberDecorate:
   case SpvOpGroupDecorate:
   case SpvOpGroupMemberDecorate:
      assert(!"Invalid opcode types and variables section");
      break;

   case SpvOpLine:
   case SpvOpNoLine:
      break; /* Ignored for now */

   case SpvOpTypeVoid:
   case SpvOpTypeBool:
   case SpvOpTypeInt:
   case SpvOpTypeFloat:
   case SpvOpTypeVector:
   case SpvOpTypeMatrix:
   case SpvOpTypeImage:
   case SpvOpTypeSampler:
   case SpvOpTypeSampledImage:
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
   case SpvOpConstantNull:
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
vtn_handle_body_instruction(struct vtn_builder *b, SpvOp opcode,
                            const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpLine:
   case SpvOpNoLine:
      break; /* Ignored for now */

   case SpvOpLabel:
      break;

   case SpvOpLoopMerge:
   case SpvOpSelectionMerge:
      /* This is handled by cfg pre-pass and walk_blocks */
      break;

   case SpvOpUndef: {
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_undef);
      val->type = vtn_value(b, w[1], vtn_value_type_type)->type;
      break;
   }

   case SpvOpExtInst:
      vtn_handle_extension(b, opcode, w, count);
      break;

   case SpvOpVariable:
   case SpvOpLoad:
   case SpvOpStore:
   case SpvOpCopyMemory:
   case SpvOpCopyMemorySized:
   case SpvOpAccessChain:
   case SpvOpInBoundsAccessChain:
   case SpvOpArrayLength:
      vtn_handle_variables(b, opcode, w, count);
      break;

   case SpvOpFunctionCall:
      vtn_handle_function_call(b, opcode, w, count);
      break;

   case SpvOpSampledImage:
   case SpvOpImageSampleImplicitLod:
   case SpvOpImageSampleExplicitLod:
   case SpvOpImageSampleDrefImplicitLod:
   case SpvOpImageSampleDrefExplicitLod:
   case SpvOpImageSampleProjImplicitLod:
   case SpvOpImageSampleProjExplicitLod:
   case SpvOpImageSampleProjDrefImplicitLod:
   case SpvOpImageSampleProjDrefExplicitLod:
   case SpvOpImageFetch:
   case SpvOpImageGather:
   case SpvOpImageDrefGather:
   case SpvOpImageQuerySizeLod:
   case SpvOpImageQuerySize:
   case SpvOpImageQueryLod:
   case SpvOpImageQueryLevels:
   case SpvOpImageQuerySamples:
      vtn_handle_texture(b, opcode, w, count);
      break;

   case SpvOpImageRead:
   case SpvOpImageWrite:
   case SpvOpImageTexelPointer:
      vtn_handle_image(b, opcode, w, count);
      break;

   case SpvOpAtomicExchange:
   case SpvOpAtomicCompareExchange:
   case SpvOpAtomicCompareExchangeWeak:
   case SpvOpAtomicIIncrement:
   case SpvOpAtomicIDecrement:
   case SpvOpAtomicIAdd:
   case SpvOpAtomicISub:
   case SpvOpAtomicSMin:
   case SpvOpAtomicUMin:
   case SpvOpAtomicSMax:
   case SpvOpAtomicUMax:
   case SpvOpAtomicAnd:
   case SpvOpAtomicOr:
   case SpvOpAtomicXor: {
      struct vtn_value *pointer = vtn_untyped_value(b, w[3]);
      if (pointer->value_type == vtn_value_type_image_pointer) {
         vtn_handle_image(b, opcode, w, count);
      } else {
         assert(pointer->value_type == vtn_value_type_deref);
         vtn_handle_ssbo_atomic(b, opcode, w, count);
      }
      break;
   }

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
   case SpvOpIAddCarry:
   case SpvOpISubBorrow:
   case SpvOpUMulExtended:
   case SpvOpSMulExtended:
   case SpvOpShiftRightLogical:
   case SpvOpShiftRightArithmetic:
   case SpvOpShiftLeftLogical:
   case SpvOpLogicalEqual:
   case SpvOpLogicalNotEqual:
   case SpvOpLogicalOr:
   case SpvOpLogicalAnd:
   case SpvOpLogicalNot:
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
   case SpvOpBitFieldInsert:
   case SpvOpBitFieldSExtract:
   case SpvOpBitFieldUExtract:
   case SpvOpBitReverse:
   case SpvOpBitCount:
   case SpvOpTranspose:
   case SpvOpOuterProduct:
   case SpvOpMatrixTimesScalar:
   case SpvOpVectorTimesMatrix:
   case SpvOpMatrixTimesVector:
   case SpvOpMatrixTimesMatrix:
      vtn_handle_alu(b, opcode, w, count);
      break;

   case SpvOpVectorExtractDynamic:
   case SpvOpVectorInsertDynamic:
   case SpvOpVectorShuffle:
   case SpvOpCompositeConstruct:
   case SpvOpCompositeExtract:
   case SpvOpCompositeInsert:
   case SpvOpCopyObject:
      vtn_handle_composite(b, opcode, w, count);
      break;

   case SpvOpPhi:
      vtn_handle_phi_first_pass(b, w);
      break;

   case SpvOpEmitVertex:
   case SpvOpEndPrimitive:
   case SpvOpEmitStreamVertex:
   case SpvOpEndStreamPrimitive:
   case SpvOpControlBarrier:
   case SpvOpMemoryBarrier:
      vtn_handle_barrier(b, opcode, w, count);
      break;

   default:
      unreachable("Unhandled opcode");
   }

   return true;
}

nir_function *
spirv_to_nir(const uint32_t *words, size_t word_count,
             gl_shader_stage stage, const char *entry_point_name,
             const nir_shader_compiler_options *options)
{
   const uint32_t *word_end = words + word_count;

   /* Handle the SPIR-V header (first 4 dwords)  */
   assert(word_count > 5);

   assert(words[0] == SpvMagicNumber);
   assert(words[1] >= 0x10000);
   /* words[2] == generator magic */
   unsigned value_id_bound = words[3];
   assert(words[4] == 0);

   words+= 5;

   /* Initialize the stn_builder object */
   struct vtn_builder *b = rzalloc(NULL, struct vtn_builder);
   b->value_id_bound = value_id_bound;
   b->values = rzalloc_array(b, struct vtn_value, value_id_bound);
   exec_list_make_empty(&b->functions);
   b->entry_point_stage = stage;
   b->entry_point_name = entry_point_name;

   /* Handle all the preamble instructions */
   words = vtn_foreach_instruction(b, words, word_end,
                                   vtn_handle_preamble_instruction);

   if (b->entry_point == NULL) {
      assert(!"Entry point not found");
      ralloc_free(b);
      return NULL;
   }

   b->shader = nir_shader_create(NULL, stage, options);

   /* Parse execution modes */
   vtn_foreach_execution_mode(b, b->entry_point,
                              vtn_handle_execution_mode, NULL);

   /* Handle all variable, type, and constant instructions */
   words = vtn_foreach_instruction(b, words, word_end,
                                   vtn_handle_variable_or_type_instruction);

   vtn_build_cfg(b, words, word_end);

   foreach_list_typed(struct vtn_function, func, node, &b->functions) {
      b->impl = func->impl;
      b->const_table = _mesa_hash_table_create(b, _mesa_hash_pointer,
                                               _mesa_key_pointer_equal);
      b->phi_table = _mesa_hash_table_create(b, _mesa_hash_pointer,
                                             _mesa_key_pointer_equal);
      vtn_function_emit(b, func, vtn_handle_body_instruction);
      vtn_foreach_instruction(b, func->start_block->label, func->end,
                              vtn_handle_phi_second_pass);
   }

   assert(b->entry_point->value_type == vtn_value_type_function);
   nir_function *entry_point = b->entry_point->func->impl->function;
   assert(entry_point);

   ralloc_free(b);

   return entry_point;
}
