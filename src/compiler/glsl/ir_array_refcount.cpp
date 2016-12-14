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
 * \file ir_array_refcount.cpp
 *
 * Provides a visitor which produces a list of variables referenced.
 */

#include "ir.h"
#include "ir_visitor.h"
#include "ir_array_refcount.h"
#include "compiler/glsl_types.h"
#include "util/hash_table.h"

ir_array_refcount_visitor::ir_array_refcount_visitor()
   : derefs(0), num_derefs(0), derefs_size(0)
{
   this->mem_ctx = ralloc_context(NULL);
   this->ht = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                                      _mesa_key_pointer_equal);
}

static void
free_entry(struct hash_entry *entry)
{
   ir_array_refcount_entry *ivre = (ir_array_refcount_entry *) entry->data;
   delete ivre;
}

ir_array_refcount_visitor::~ir_array_refcount_visitor()
{
   ralloc_free(this->mem_ctx);
   _mesa_hash_table_destroy(this->ht, free_entry);
}

ir_array_refcount_entry::ir_array_refcount_entry(ir_variable *var)
   : var(var), is_referenced(false)
{
   num_bits = MAX2(1, var->type->arrays_of_arrays_size());
   bits = new BITSET_WORD[BITSET_WORDS(num_bits)];
   memset(bits, 0, BITSET_WORDS(num_bits) * sizeof(bits[0]));
}


ir_array_refcount_entry::~ir_array_refcount_entry()
{
   delete [] bits;
}


ir_array_refcount_entry *
ir_array_refcount_visitor::get_variable_entry(ir_variable *var)
{
   assert(var);

   struct hash_entry *e = _mesa_hash_table_search(this->ht, var);
   if (e)
      return (ir_array_refcount_entry *)e->data;

   ir_array_refcount_entry *entry = new ir_array_refcount_entry(var);
   _mesa_hash_table_insert(this->ht, var, entry);

   return entry;
}


array_deref_range *
ir_array_refcount_visitor::get_array_deref()
{
   if ((num_derefs + 1) * sizeof(array_deref_range) > derefs_size) {
      void *ptr = reralloc_size(mem_ctx, derefs, derefs_size + 4096);

      if (ptr == NULL)
         return NULL;

      derefs_size += 4096;
      derefs = (array_deref_range *)ptr;
   }

   array_deref_range *d = &derefs[num_derefs];
   num_derefs++;

   return d;
}


ir_visitor_status
ir_array_refcount_visitor::visit(ir_dereference_variable *ir)
{
   ir_variable *const var = ir->variable_referenced();
   ir_array_refcount_entry *entry = this->get_variable_entry(var);

   entry->is_referenced = true;

   return visit_continue;
}


ir_visitor_status
ir_array_refcount_visitor::visit_enter(ir_function_signature *ir)
{
   /* We don't want to descend into the function parameters and
    * dead-code eliminate them, so just accept the body here.
    */
   visit_list_elements(this, &ir->body);
   return visit_continue_with_parent;
}
