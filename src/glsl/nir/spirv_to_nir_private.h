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
};

struct vtn_block {
   const uint32_t *label;
   const uint32_t *branch;
   nir_block *block;
};

struct vtn_function {
   struct exec_node node;

   nir_function_overload *overload;
   struct vtn_block *start_block;
};

typedef bool (*vtn_instruction_handler)(struct vtn_builder *, uint32_t,
                                        const uint32_t *, unsigned);

struct vtn_value {
   enum vtn_value_type value_type;
   const char *name;
   struct vtn_decoration *decoration;
   const struct glsl_type *type;
   union {
      void *ptr;
      char *str;
      nir_constant *constant;
      nir_deref_var *deref;
      struct vtn_function *func;
      struct vtn_block *block;
      nir_ssa_def *ssa;
   };
};

struct vtn_decoration {
   struct vtn_decoration *next;
   const uint32_t *literals;
   struct vtn_value *group;
   SpvDecoration decoration;
};

struct vtn_builder {
   nir_builder nb;

   nir_shader *shader;
   nir_function_impl *impl;
   struct vtn_block *block;
   struct vtn_block *merge_block;

   unsigned value_id_bound;
   struct vtn_value *values;

   SpvExecutionModel execution_model;
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

nir_ssa_def *vtn_ssa_value(struct vtn_builder *b, uint32_t value_id);

typedef void (*vtn_decoration_foreach_cb)(struct vtn_builder *,
                                          struct vtn_value *,
                                          const struct vtn_decoration *,
                                          void *);

void vtn_foreach_decoration(struct vtn_builder *b, struct vtn_value *value,
                            vtn_decoration_foreach_cb cb, void *data);
