/*
 * Copyright © 2016 Intel Corporation
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
 * \file ir_array_refcount.h
 *
 * Provides a visitor which produces a list of variables referenced.
 */

#include "ir.h"
#include "ir_visitor.h"
#include "compiler/glsl_types.h"

class ir_array_refcount_entry
{
public:
   ir_array_refcount_entry(ir_variable *var);

   ir_variable *var; /* The key: the variable's pointer. */

   /** Has the variable been referenced? */
   bool is_referenced;
};

class ir_array_refcount_visitor : public ir_hierarchical_visitor {
public:
   ir_array_refcount_visitor(void);
   ~ir_array_refcount_visitor(void);

   virtual ir_visitor_status visit(ir_dereference_variable *);

   virtual ir_visitor_status visit_enter(ir_function_signature *);

   /**
    * Find variable in the hash table, and insert it if not present
    */
   ir_array_refcount_entry *get_variable_entry(ir_variable *var);

   /**
    * Hash table mapping ir_variable to ir_array_refcount_entry.
    */
   struct hash_table *ht;

   void *mem_ctx;
};
