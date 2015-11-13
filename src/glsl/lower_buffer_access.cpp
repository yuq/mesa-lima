/*
 * Copyright (c) 2015 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file lower_buffer_access.cpp
 *
 * Helper for IR lowering pass to replace dereferences of buffer object based
 * shader variables with intrinsic function calls.
 *
 * This helper is used by lowering passes for UBOs, SSBOs and compute shader
 * shared variables.
 */

#include "lower_buffer_access.h"
#include "ir_builder.h"
#include "main/macros.h"
#include "util/list.h"
#include "glsl_parser_extras.h"

using namespace ir_builder;

namespace lower_buffer_access {

static inline int
writemask_for_size(unsigned n)
{
   return ((1 << n) - 1);
}

/**
 * Takes a deref and recursively calls itself to break the deref down to the
 * point that the reads or writes generated are contiguous scalars or vectors.
 */
void
lower_buffer_access::emit_access(void *mem_ctx,
                                 bool is_write,
                                 ir_dereference *deref,
                                 ir_variable *base_offset,
                                 unsigned int deref_offset,
                                 bool row_major,
                                 int matrix_columns,
                                 unsigned int packing,
                                 unsigned int write_mask)
{
   if (deref->type->is_record()) {
      unsigned int field_offset = 0;

      for (unsigned i = 0; i < deref->type->length; i++) {
         const struct glsl_struct_field *field =
            &deref->type->fields.structure[i];
         ir_dereference *field_deref =
            new(mem_ctx) ir_dereference_record(deref->clone(mem_ctx, NULL),
                                               field->name);

         field_offset =
            glsl_align(field_offset,
                       field->type->std140_base_alignment(row_major));

         emit_access(mem_ctx, is_write, field_deref, base_offset,
                     deref_offset + field_offset,
                     row_major, 1, packing,
                     writemask_for_size(field_deref->type->vector_elements));

         field_offset += field->type->std140_size(row_major);
      }
      return;
   }

   if (deref->type->is_array()) {
      unsigned array_stride = packing == GLSL_INTERFACE_PACKING_STD430 ?
         deref->type->fields.array->std430_array_stride(row_major) :
         glsl_align(deref->type->fields.array->std140_size(row_major), 16);

      for (unsigned i = 0; i < deref->type->length; i++) {
         ir_constant *element = new(mem_ctx) ir_constant(i);
         ir_dereference *element_deref =
            new(mem_ctx) ir_dereference_array(deref->clone(mem_ctx, NULL),
                                              element);
         emit_access(mem_ctx, is_write, element_deref, base_offset,
                     deref_offset + i * array_stride,
                     row_major, 1, packing,
                     writemask_for_size(element_deref->type->vector_elements));
      }
      return;
   }

   if (deref->type->is_matrix()) {
      for (unsigned i = 0; i < deref->type->matrix_columns; i++) {
         ir_constant *col = new(mem_ctx) ir_constant(i);
         ir_dereference *col_deref =
            new(mem_ctx) ir_dereference_array(deref->clone(mem_ctx, NULL), col);

         if (row_major) {
            /* For a row-major matrix, the next column starts at the next
             * element.
             */
            int size_mul = deref->type->is_double() ? 8 : 4;
            emit_access(mem_ctx, is_write, col_deref, base_offset,
                        deref_offset + i * size_mul,
                        row_major, deref->type->matrix_columns, packing,
                        writemask_for_size(col_deref->type->vector_elements));
         } else {
            int size_mul;

            /* std430 doesn't round up vec2 size to a vec4 size */
            if (packing == GLSL_INTERFACE_PACKING_STD430 &&
                deref->type->vector_elements == 2 &&
                !deref->type->is_double()) {
               size_mul = 8;
            } else {
               /* std140 always rounds the stride of arrays (and matrices) to a
                * vec4, so matrices are always 16 between columns/rows. With
                * doubles, they will be 32 apart when there are more than 2 rows.
                *
                * For both std140 and std430, if the member is a
                * three-'component vector with components consuming N basic
                * machine units, the base alignment is 4N. For vec4, base
                * alignment is 4N.
                */
               size_mul = (deref->type->is_double() &&
                           deref->type->vector_elements > 2) ? 32 : 16;
            }

            emit_access(mem_ctx, is_write, col_deref, base_offset,
                        deref_offset + i * size_mul,
                        row_major, deref->type->matrix_columns, packing,
                        writemask_for_size(col_deref->type->vector_elements));
         }
      }
      return;
   }

   assert(deref->type->is_scalar() || deref->type->is_vector());

   if (!row_major) {
      ir_rvalue *offset =
         add(base_offset, new(mem_ctx) ir_constant(deref_offset));
      unsigned mask =
         is_write ? write_mask : (1 << deref->type->vector_elements) - 1;
      insert_buffer_access(mem_ctx, deref, deref->type, offset, mask, -1);
   } else {
      unsigned N = deref->type->is_double() ? 8 : 4;

      /* We're dereffing a column out of a row-major matrix, so we
       * gather the vector from each stored row.
      */
      assert(deref->type->base_type == GLSL_TYPE_FLOAT ||
             deref->type->base_type == GLSL_TYPE_DOUBLE);
      /* Matrices, row_major or not, are stored as if they were
       * arrays of vectors of the appropriate size in std140.
       * Arrays have their strides rounded up to a vec4, so the
       * matrix stride is always 16. However a double matrix may either be 16
       * or 32 depending on the number of columns.
       */
      assert(matrix_columns <= 4);
      unsigned matrix_stride = 0;
      /* Matrix stride for std430 mat2xY matrices are not rounded up to
       * vec4 size. From OpenGL 4.3 spec, section 7.6.2.2 "Standard Uniform
       * Block Layout":
       *
       * "2. If the member is a two- or four-component vector with components
       * consuming N basic machine units, the base alignment is 2N or 4N,
       * respectively." [...]
       * "4. If the member is an array of scalars or vectors, the base alignment
       * and array stride are set to match the base alignment of a single array
       * element, according to rules (1), (2), and (3), and rounded up to the
       * base alignment of a vec4." [...]
       * "7. If the member is a row-major matrix with C columns and R rows, the
       * matrix is stored identically to an array of R row vectors with C
       * components each, according to rule (4)." [...]
       * "When using the std430 storage layout, shader storage blocks will be
       * laid out in buffer storage identically to uniform and shader storage
       * blocks using the std140 layout, except that the base alignment and
       * stride of arrays of scalars and vectors in rule 4 and of structures in
       * rule 9 are not rounded up a multiple of the base alignment of a vec4."
       */
      if (packing == GLSL_INTERFACE_PACKING_STD430 && matrix_columns == 2)
         matrix_stride = 2 * N;
      else
         matrix_stride = glsl_align(matrix_columns * N, 16);

      const glsl_type *deref_type = deref->type->base_type == GLSL_TYPE_FLOAT ?
         glsl_type::float_type : glsl_type::double_type;

      for (unsigned i = 0; i < deref->type->vector_elements; i++) {
         ir_rvalue *chan_offset =
            add(base_offset,
                new(mem_ctx) ir_constant(deref_offset + i * matrix_stride));
         if (!is_write || ((1U << i) & write_mask))
            insert_buffer_access(mem_ctx, deref, deref_type, chan_offset,
                                 (1U << i), i);
      }
   }
}

/**
 * Determine if a thing being dereferenced is row-major
 *
 * There is some trickery here.
 *
 * If the thing being dereferenced is a member of uniform block \b without an
 * instance name, then the name of the \c ir_variable is the field name of an
 * interface type.  If this field is row-major, then the thing referenced is
 * row-major.
 *
 * If the thing being dereferenced is a member of uniform block \b with an
 * instance name, then the last dereference in the tree will be an
 * \c ir_dereference_record.  If that record field is row-major, then the
 * thing referenced is row-major.
 */
bool
lower_buffer_access::is_dereferenced_thing_row_major(const ir_rvalue *deref)
{
   bool matrix = false;
   const ir_rvalue *ir = deref;

   while (true) {
      matrix = matrix || ir->type->without_array()->is_matrix();

      switch (ir->ir_type) {
      case ir_type_dereference_array: {
         const ir_dereference_array *const array_deref =
            (const ir_dereference_array *) ir;

         ir = array_deref->array;
         break;
      }

      case ir_type_dereference_record: {
         const ir_dereference_record *const record_deref =
            (const ir_dereference_record *) ir;

         ir = record_deref->record;

         const int idx = ir->type->field_index(record_deref->field);
         assert(idx >= 0);

         const enum glsl_matrix_layout matrix_layout =
            glsl_matrix_layout(ir->type->fields.structure[idx].matrix_layout);

         switch (matrix_layout) {
         case GLSL_MATRIX_LAYOUT_INHERITED:
            break;
         case GLSL_MATRIX_LAYOUT_COLUMN_MAJOR:
            return false;
         case GLSL_MATRIX_LAYOUT_ROW_MAJOR:
            return matrix || deref->type->without_array()->is_record();
         }

         break;
      }

      case ir_type_dereference_variable: {
         const ir_dereference_variable *const var_deref =
            (const ir_dereference_variable *) ir;

         const enum glsl_matrix_layout matrix_layout =
            glsl_matrix_layout(var_deref->var->data.matrix_layout);

         switch (matrix_layout) {
         case GLSL_MATRIX_LAYOUT_INHERITED:
            assert(!matrix);
            return false;
         case GLSL_MATRIX_LAYOUT_COLUMN_MAJOR:
            return false;
         case GLSL_MATRIX_LAYOUT_ROW_MAJOR:
            return matrix || deref->type->without_array()->is_record();
         }

         unreachable("invalid matrix layout");
         break;
      }

      default:
         return false;
      }
   }

   /* The tree must have ended with a dereference that wasn't an
    * ir_dereference_variable.  That is invalid, and it should be impossible.
    */
   unreachable("invalid dereference tree");
   return false;
}

} /* namespace lower_buffer_access */
