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

#include "nir.h"
#include "nir_spirv.h"
#include "nir_builder.h"
#include "spirv.h"

struct vtn_builder;
struct vtn_decoration;

enum vtn_value_type {
   vtn_value_type_invalid = 0,
   vtn_value_type_undef,
   vtn_value_type_string,
   vtn_value_type_decoration_group,
   vtn_value_type_type,
   vtn_value_type_constant,
   vtn_value_type_deref,
   vtn_value_type_function,
   vtn_value_type_block,
   vtn_value_type_ssa,
   vtn_value_type_extension,
};

struct vtn_block {
   /* Merge opcode if this block contains a merge; SpvOpNop otherwise. */
   SpvOp merge_op;
   uint32_t merge_block_id;
   const uint32_t *label;
   const uint32_t *branch;
   nir_block *block;
};

struct vtn_function {
   struct exec_node node;

   nir_function_overload *overload;
   struct vtn_block *start_block;

   const uint32_t *end;
};

typedef bool (*vtn_instruction_handler)(struct vtn_builder *, uint32_t,
                                        const uint32_t *, unsigned);

struct vtn_ssa_value {
   union {
      nir_ssa_def *def;
      struct vtn_ssa_value **elems;
   };

   /* For matrices, a transposed version of the value, or NULL if it hasn't
    * been computed
    */
   struct vtn_ssa_value *transposed;

   const struct glsl_type *type;
};

struct vtn_type {
   const struct glsl_type *type;

   /* for matrices, whether the matrix is stored row-major */
   bool row_major;

   /* for structs, the offset of each member */
   unsigned *offsets;

   /* for structs, whether it was decorated as a "non-SSBO-like" block */
   bool block;

   /* for structs, whether it was decorated as an "SSBO-like" block */
   bool buffer_block;

   /* for structs with block == true, whether this is a builtin block (i.e. a
    * block that contains only builtins).
    */
   bool builtin_block;

   /* for arrays and matrices, the array stride */
   unsigned stride;

   /* for arrays, the vtn_type for the elements of the array */
   struct vtn_type *array_element;

   /* for structures, the vtn_type for each member */
   struct vtn_type **members;

   /* Whether this type, or a parent type, has been decorated as a builtin */
   bool is_builtin;

   SpvBuiltIn builtin;
};

struct vtn_value {
   enum vtn_value_type value_type;
   const char *name;
   struct vtn_decoration *decoration;
   union {
      void *ptr;
      char *str;
      struct vtn_type *type;
      struct {
         nir_constant *constant;
         const struct glsl_type *const_type;
      };
      struct {
         nir_deref_var *deref;
         struct vtn_type *deref_type;
      };
      struct vtn_function *func;
      struct vtn_block *block;
      struct vtn_ssa_value *ssa;
      vtn_instruction_handler ext_handler;
   };
};

struct vtn_decoration {
   struct vtn_decoration *next;
   int member; /* -1 if not a member decoration */
   const uint32_t *literals;
   struct vtn_value *group;
   SpvDecoration decoration;
};

struct vtn_builder {
   nir_builder nb;

   nir_shader *shader;
   nir_function_impl *impl;
   struct vtn_block *block;

   /*
    * In SPIR-V, constants are global, whereas in NIR, the load_const
    * instruction we use is per-function. So while we parse each function, we
    * keep a hash table of constants we've resolved to nir_ssa_value's so
    * far, and we lazily resolve them when we see them used in a function.
    */
   struct hash_table *const_table;

   /*
    * Map from nir_block to the vtn_block which ends with it -- used for
    * handling phi nodes.
    */
   struct hash_table *block_table;

   /*
    * NIR variable for each SPIR-V builtin.
    */
   nir_variable *builtins[42]; /* XXX need symbolic constant from SPIR-V header */

   unsigned value_id_bound;
   struct vtn_value *values;

   SpvExecutionModel execution_model;
   bool origin_upper_left;
   struct vtn_value *entry_point;

   struct vtn_function *func;
   struct exec_list functions;
};

static inline struct vtn_value *
vtn_push_value(struct vtn_builder *b, uint32_t value_id,
               enum vtn_value_type value_type)
{
   assert(value_id < b->value_id_bound);
   assert(b->values[value_id].value_type == vtn_value_type_invalid);

   b->values[value_id].value_type = value_type;

   return &b->values[value_id];
}

static inline struct vtn_value *
vtn_untyped_value(struct vtn_builder *b, uint32_t value_id)
{
   assert(value_id < b->value_id_bound);
   return &b->values[value_id];
}

static inline struct vtn_value *
vtn_value(struct vtn_builder *b, uint32_t value_id,
          enum vtn_value_type value_type)
{
   struct vtn_value *val = vtn_untyped_value(b, value_id);
   assert(val->value_type == value_type);
   return val;
}

struct vtn_ssa_value *vtn_ssa_value(struct vtn_builder *b, uint32_t value_id);

typedef void (*vtn_decoration_foreach_cb)(struct vtn_builder *,
                                          struct vtn_value *,
                                          int member,
                                          const struct vtn_decoration *,
                                          void *);

void vtn_foreach_decoration(struct vtn_builder *b, struct vtn_value *value,
                            vtn_decoration_foreach_cb cb, void *data);

bool vtn_handle_glsl450_instruction(struct vtn_builder *b, uint32_t ext_opcode,
                                    const uint32_t *words, unsigned count);
