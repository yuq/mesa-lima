/*
 * Copyright © 2014 Intel Corporation
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

struct deref_node {
   struct deref_node *parent;
   const struct glsl_type *type;

   bool lower_to_ssa;

   struct set *loads;
   struct set *stores;
   struct set *copies;

   nir_ssa_def **def_stack;
   nir_ssa_def **def_stack_tail;

   struct deref_node *wildcard;
   struct deref_node *indirect;
   struct deref_node *children[0];
};

struct lower_variables_state {
   void *mem_ctx;
   void *dead_ctx;
   nir_function_impl *impl;

   /* A hash table mapping variables to deref_node data */
   struct hash_table *deref_var_nodes;
   /* A hash table mapping dereference leaves to deref_node data */
   struct hash_table *deref_leaves;

   /* A hash table mapping phi nodes to deref_state data */
   struct hash_table *phi_table;
};

/* The following two functions implement a hash and equality check for
 * variable dreferences.  When the hash or equality function encounters an
 * array, all indirects are treated as equal and are never equal to a
 * direct dereference or a wildcard.
 */
static uint32_t
hash_deref(const void *void_deref)
{
   const nir_deref *deref = void_deref;

   uint32_t hash;
   if (deref->child) {
      hash = hash_deref(deref->child);
   } else {
      hash = 2166136261ul;
   }

   switch (deref->deref_type) {
   case nir_deref_type_var:
      hash ^= _mesa_hash_pointer(nir_deref_as_var(deref)->var);
      break;
   case nir_deref_type_array: {
      nir_deref_array *array = nir_deref_as_array(deref);
      hash += 268435183 * array->deref_array_type;
      if (array->deref_array_type == nir_deref_array_type_direct)
         hash ^= array->base_offset; /* Some prime */
      break;
   }
   case nir_deref_type_struct:
      hash ^= nir_deref_as_struct(deref)->index;
      break;
   }

   return hash * 0x01000193;
}

static bool
derefs_equal(const void *void_a, const void *void_b)
{
   const nir_deref *a = void_a;
   const nir_deref *b = void_b;

   if (a->deref_type != b->deref_type)
      return false;

   switch (a->deref_type) {
   case nir_deref_type_var:
      if (nir_deref_as_var(a)->var != nir_deref_as_var(b)->var)
         return false;
      break;
   case nir_deref_type_array: {
      nir_deref_array *a_arr = nir_deref_as_array(a);
      nir_deref_array *b_arr = nir_deref_as_array(b);

      if (a_arr->deref_array_type != b_arr->deref_array_type)
         return false;

      if (a_arr->deref_array_type == nir_deref_array_type_direct &&
          a_arr->base_offset != b_arr->base_offset)
         return false;
      break;
   }
   case nir_deref_type_struct:
      if (nir_deref_as_struct(a)->index != nir_deref_as_struct(b)->index)
         return false;
      break;
   default:
      unreachable("Invalid dreference type");
   }

   assert((a->child == NULL) == (b->child == NULL));
   if (a->child)
      return derefs_equal(a->child, b->child);
   else
      return true;
}

static int
type_get_length(const struct glsl_type *type)
{
   switch (glsl_get_base_type(type)) {
   case GLSL_TYPE_STRUCT:
   case GLSL_TYPE_ARRAY:
      return glsl_get_length(type);
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_INT:
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_BOOL:
      if (glsl_type_is_matrix(type))
         return glsl_get_matrix_columns(type);
      else
         return glsl_get_vector_elements(type);
   default:
      unreachable("Invalid deref base type");
   }
}

static struct deref_node *
deref_node_create(struct deref_node *parent,
                  const struct glsl_type *type, void *mem_ctx)
{
   size_t size = sizeof(struct deref_node) +
                 type_get_length(type) * sizeof(struct deref_node *);

   struct deref_node *node = rzalloc_size(mem_ctx, size);
   node->type = type;
   node->parent = parent;

   return node;
}

static struct deref_node *
get_deref_node(nir_deref_var *deref, bool add_to_leaves,
               struct lower_variables_state *state)
{
   bool is_leaf = true;
   struct deref_node *parent = NULL;
   nir_deref *tail = &deref->deref;
   while (tail) {
      struct deref_node *node;

      switch (tail->deref_type) {
      case nir_deref_type_var: {
         assert(tail == &deref->deref);
         assert(parent == NULL);

         struct hash_entry *entry =
            _mesa_hash_table_search(state->deref_var_nodes, deref->var);
         if (entry) {
            node = entry->data;
         } else {
            node = deref_node_create(NULL, tail->type, state->dead_ctx);
            _mesa_hash_table_insert(state->deref_var_nodes, deref->var, node);
         }
         break;
      }

      case nir_deref_type_struct: {
         assert(parent != NULL);

         nir_deref_struct *deref_struct = nir_deref_as_struct(tail);
         assert(deref_struct->index < type_get_length(parent->type));
         if (parent->children[deref_struct->index]) {
            node = parent->children[deref_struct->index];
         } else {
            node = deref_node_create(parent, tail->type, state->dead_ctx);
            parent->children[deref_struct->index] = node;
         }
         break;
      }

      case nir_deref_type_array: {
         assert(parent != NULL);

         nir_deref_array *arr = nir_deref_as_array(tail);
         switch (arr->deref_array_type) {
         case nir_deref_array_type_direct:
            if (arr->base_offset >= type_get_length(parent->type)) {
               /* This is possible if a loop unrolls and generates an
                * out-of-bounds offset.  We need to handle this at least
                * somewhat gracefully.
                */
               return NULL;
            } else if (parent->children[arr->base_offset]) {
               node = parent->children[arr->base_offset];
            } else {
               node = deref_node_create(parent, tail->type, state->dead_ctx);
               parent->children[arr->base_offset] = node;
            }
            break;
         case nir_deref_array_type_indirect:
            if (parent->indirect) {
               node = parent->indirect;
            } else {
               node = deref_node_create(parent, tail->type, state->dead_ctx);
               parent->indirect = node;
            }
            is_leaf = false;
            break;
         case nir_deref_array_type_wildcard:
            if (parent->wildcard) {
               node = parent->wildcard;
            } else {
               node = deref_node_create(parent, tail->type, state->dead_ctx);
               parent->wildcard = node;
            }
            is_leaf = false;
            break;
         default:
            unreachable("Invalid array deref type");
         }
         break;
      }
      default:
         unreachable("Invalid deref type");
      }

      parent = node;
      tail = tail->child;
   }

   assert(parent);

   if (is_leaf && add_to_leaves)
      _mesa_hash_table_insert(state->deref_leaves, deref, parent);

   return parent;
}

static void
register_load_instr(nir_intrinsic_instr *load_instr, bool create_node,
                    struct lower_variables_state *state)
{
   struct deref_node *node = get_deref_node(load_instr->variables[0],
                                            create_node, state);
   if (node == NULL)
      return;

   if (node->loads == NULL)
      node->loads = _mesa_set_create(state->dead_ctx,
                                     _mesa_key_pointer_equal);

   _mesa_set_add(node->loads, _mesa_hash_pointer(load_instr), load_instr);
}

static void
register_store_instr(nir_intrinsic_instr *store_instr, bool create_node,
                     struct lower_variables_state *state)
{
   struct deref_node *node = get_deref_node(store_instr->variables[0],
                                            create_node, state);
   if (node == NULL)
      return;

   if (node->stores == NULL)
      node->stores = _mesa_set_create(state->dead_ctx,
                                     _mesa_key_pointer_equal);

   _mesa_set_add(node->stores, _mesa_hash_pointer(store_instr), store_instr);
}

static void
register_copy_instr(nir_intrinsic_instr *copy_instr, bool create_node,
                    struct lower_variables_state *state)
{
   for (unsigned idx = 0; idx < 2; idx++) {
      struct deref_node *node = get_deref_node(copy_instr->variables[idx],
                                               create_node, state);
      if (node == NULL)
         continue;

      if (node->copies == NULL)
         node->copies = _mesa_set_create(state->dead_ctx,
                                         _mesa_key_pointer_equal);

      _mesa_set_add(node->copies, _mesa_hash_pointer(copy_instr), copy_instr);
   }
}

static bool
foreach_deref_node_worker(struct deref_node *node, nir_deref *deref,
                          bool (* cb)(struct deref_node *node,
                                      struct lower_variables_state *state),
                          struct lower_variables_state *state)
{
   if (deref->child == NULL) {
      return cb(node, state);
   } else {
      switch (deref->child->deref_type) {
      case nir_deref_type_array: {
         nir_deref_array *arr = nir_deref_as_array(deref->child);
         assert(arr->deref_array_type == nir_deref_array_type_direct);
         if (node->children[arr->base_offset] &&
             !foreach_deref_node_worker(node->children[arr->base_offset],
                                        deref->child, cb, state))
            return false;

         if (node->wildcard &&
             !foreach_deref_node_worker(node->wildcard,
                                        deref->child, cb, state))
            return false;

         return true;
      }

      case nir_deref_type_struct: {
         nir_deref_struct *str = nir_deref_as_struct(deref->child);
         return foreach_deref_node_worker(node->children[str->index],
                                          deref->child, cb, state);
      }

      default:
         unreachable("Invalid deref child type");
      }
   }
}

static bool
foreach_deref_node_match(nir_deref_var *deref,
                         bool (* cb)(struct deref_node *node,
                                     struct lower_variables_state *state),
                         struct lower_variables_state *state)
{
   nir_deref_var var_deref = *deref;
   var_deref.deref.child = NULL;
   struct deref_node *node = get_deref_node(&var_deref, false, state);

   if (node == NULL)
      return false;

   return foreach_deref_node_worker(node, &deref->deref, cb, state);
}

/* This question can only be asked about leaves.  Searching down the tree
 * is much harder than searching up.
 */
static bool
deref_may_be_aliased_node(struct deref_node *node, nir_deref *deref,
                          struct lower_variables_state *state)
{
   if (deref->child == NULL) {
      return false;
   } else {
      switch (deref->child->deref_type) {
      case nir_deref_type_array: {
         nir_deref_array *arr = nir_deref_as_array(deref->child);
         if (arr->deref_array_type == nir_deref_array_type_indirect)
            return true;

         assert(arr->deref_array_type == nir_deref_array_type_direct);

         if (node->children[arr->base_offset] &&
             deref_may_be_aliased_node(node->children[arr->base_offset],
                                       deref->child, state))
            return true;

         if (node->wildcard &&
             deref_may_be_aliased_node(node->wildcard, deref->child, state))
            return true;

         return false;
      }

      case nir_deref_type_struct: {
         nir_deref_struct *str = nir_deref_as_struct(deref->child);
         if (node->children[str->index]) {
             return deref_may_be_aliased_node(node->children[str->index],
                                              deref->child, state);
         } else {
            return false;
         }
      }

      default:
         unreachable("Invalid nir_deref child type");
      }
   }
}

static bool
deref_may_be_aliased(nir_deref_var *deref,
                     struct lower_variables_state *state)
{
   nir_deref_var var_deref = *deref;
   var_deref.deref.child = NULL;
   struct deref_node *node = get_deref_node(&var_deref, false, state);

   /* An invalid dereference can't be aliased. */
   if (node == NULL)
      return false;

   return deref_may_be_aliased_node(node, &deref->deref, state);
}

static bool
fill_deref_tables_block(nir_block *block, void *void_state)
{
   struct lower_variables_state *state = void_state;

   nir_foreach_instr_safe(block, instr) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

      switch (intrin->intrinsic) {
      case nir_intrinsic_load_var:
         register_load_instr(intrin, true, state);
         break;

      case nir_intrinsic_store_var:
         register_store_instr(intrin, true, state);
         break;

      case nir_intrinsic_copy_var:
         register_copy_instr(intrin, true, state);
         break;

      default:
         continue;
      }
   }

   return true;
}

static nir_deref *
deref_next_wildcard_parent(nir_deref *deref)
{
   for (nir_deref *tail = deref; tail->child; tail = tail->child) {
      if (tail->child->deref_type != nir_deref_type_array)
         continue;

      nir_deref_array *arr = nir_deref_as_array(tail->child);

      if (arr->deref_array_type == nir_deref_array_type_wildcard)
         return tail;
   }

   return NULL;
}

static nir_deref *
get_deref_tail(nir_deref *deref)
{
   while (deref->child)
      deref = deref->child;

   return deref;
}

static void
emit_copy_load_store(nir_intrinsic_instr *copy_instr,
                     nir_deref_var *dest_head, nir_deref_var *src_head,
                     nir_deref *dest_tail, nir_deref *src_tail,
                     struct lower_variables_state *state)
{
   nir_deref *src_arr_parent = deref_next_wildcard_parent(src_tail);
   nir_deref *dest_arr_parent = deref_next_wildcard_parent(dest_tail);

   if (src_arr_parent || dest_arr_parent) {
      assert(dest_arr_parent && dest_arr_parent);

      nir_deref_array *src_arr = nir_deref_as_array(src_arr_parent->child);
      nir_deref_array *dest_arr = nir_deref_as_array(dest_arr_parent->child);

      unsigned length = type_get_length(src_arr_parent->type);
      assert(length == type_get_length(dest_arr_parent->type));
      assert(length > 0);

      src_arr->deref_array_type = nir_deref_array_type_direct;
      dest_arr->deref_array_type = nir_deref_array_type_direct;
      for (unsigned i = 0; i < length; i++) {
         src_arr->base_offset = i;
         dest_arr->base_offset = i;
         emit_copy_load_store(copy_instr, dest_head, src_head,
                              &dest_arr->deref, &src_arr->deref, state);
      }
      src_arr->deref_array_type = nir_deref_array_type_wildcard;
      dest_arr->deref_array_type = nir_deref_array_type_wildcard;
   } else {
      /* Base case. Actually do the copy */
      src_tail = get_deref_tail(src_tail);
      dest_tail = get_deref_tail(dest_tail);

      assert(src_tail->type == dest_tail->type);

      unsigned num_components = glsl_get_vector_elements(src_tail->type);

      nir_deref *src_deref = nir_copy_deref(state->mem_ctx, &src_head->deref);
      nir_deref *dest_deref = nir_copy_deref(state->mem_ctx, &dest_head->deref);

      nir_intrinsic_instr *load =
         nir_intrinsic_instr_create(state->mem_ctx, nir_intrinsic_load_var);
      load->num_components = num_components;
      load->variables[0] = nir_deref_as_var(src_deref);
      load->dest.is_ssa = true;
      nir_ssa_def_init(&load->instr, &load->dest.ssa, num_components, NULL);

      nir_instr_insert_before(&copy_instr->instr, &load->instr);
      register_load_instr(load, false, state);

      nir_intrinsic_instr *store =
         nir_intrinsic_instr_create(state->mem_ctx, nir_intrinsic_store_var);
      store->num_components = num_components;
      store->variables[0] = nir_deref_as_var(dest_deref);
      store->src[0].is_ssa = true;
      store->src[0].ssa = &load->dest.ssa;

      if (copy_instr->has_predicate) {
         store->has_predicate = true;
         store->predicate = nir_src_copy(copy_instr->predicate, state->mem_ctx);
      }

      nir_instr_insert_before(&copy_instr->instr, &store->instr);
      register_store_instr(store, false, state);
   }
}

static bool
lower_copies_to_load_store(struct deref_node *node,
                           struct lower_variables_state *state)
{
   if (!node->copies)
      return true;

   struct set_entry *copy_entry;
   set_foreach(node->copies, copy_entry) {
      nir_intrinsic_instr *copy = (void *)copy_entry->key;

      emit_copy_load_store(copy, copy->variables[0], copy->variables[1],
                           &copy->variables[0]->deref,
                           &copy->variables[1]->deref,
                           state);

      for (unsigned i = 0; i < 2; ++i) {
         struct deref_node *arg_node = get_deref_node(copy->variables[i],
                                                      false, state);
         if (arg_node == NULL)
            continue;

         struct set_entry *arg_entry = _mesa_set_search(arg_node->copies,
                                                        copy_entry->hash,
                                                        copy);
         assert(arg_entry);
         _mesa_set_remove(node->copies, arg_entry);
      }

      nir_instr_remove(&copy->instr);
   }

   return true;
}

static nir_load_const_instr *
get_const_initializer_load(const nir_deref_var *deref,
                           struct lower_variables_state *state)
{
   nir_constant *constant = deref->var->constant_initializer;
   const nir_deref *tail = &deref->deref;
   unsigned matrix_offset = 0;
   while (tail->child) {
      switch (tail->child->deref_type) {
      case nir_deref_type_array: {
         nir_deref_array *arr = nir_deref_as_array(tail->child);
         assert(arr->deref_array_type == nir_deref_array_type_direct);
         if (glsl_type_is_matrix(tail->type)) {
            assert(arr->deref.child == NULL);
            matrix_offset = arr->base_offset;
         } else {
            constant = constant->elements[arr->base_offset];
         }
         break;
      }

      case nir_deref_type_struct: {
         constant = constant->elements[nir_deref_as_struct(tail->child)->index];
         break;
      }

      default:
         unreachable("Invalid deref child type");
      }

      tail = tail->child;
   }

   nir_load_const_instr *load = nir_load_const_instr_create(state->mem_ctx);
   load->array_elems = 0;
   load->num_components = glsl_get_vector_elements(tail->type);

   matrix_offset *= load->num_components;
   for (unsigned i = 0; i < load->num_components; i++) {
      switch (glsl_get_base_type(tail->type)) {
      case GLSL_TYPE_FLOAT:
      case GLSL_TYPE_INT:
      case GLSL_TYPE_UINT:
         load->value.u[i] = constant->value.u[matrix_offset + i];
         break;
      case GLSL_TYPE_BOOL:
         load->value.u[i] = constant->value.u[matrix_offset + i] ?
                             NIR_TRUE : NIR_FALSE;
         break;
      default:
         unreachable("Invalid immediate type");
      }
   }

   return load;
}

static void
def_stack_push(struct deref_node *node, nir_ssa_def *def,
               struct lower_variables_state *state)
{
   if (node->def_stack == NULL) {
      node->def_stack = ralloc_array(state->dead_ctx, nir_ssa_def *,
                                     state->impl->num_blocks);
      node->def_stack_tail = node->def_stack - 1;
   }

   if (node->def_stack_tail >= node->def_stack) {
      nir_ssa_def *top_def = *node->def_stack_tail;

      if (def->parent_instr->block == top_def->parent_instr->block) {
         /* They're in the same block, just replace the top */
         *node->def_stack_tail = def;
         return;
      }
   }

   *(++node->def_stack_tail) = def;
}

static nir_ssa_def *
get_ssa_def_for_block(struct deref_node *node, nir_block *block,
                      struct lower_variables_state *state)
{
   if (node->def_stack) {
      while (node->def_stack_tail >= node->def_stack) {
         nir_ssa_def *def = *node->def_stack_tail;

         for (nir_block *dom = block; dom != NULL; dom = dom->imm_dom) {
            if (def->parent_instr->block == dom)
               return def;
         }

         node->def_stack_tail--;
      }
   }

   /* If we got here then we don't have a definition that dominates the
    * given block.  This means that we need to add an undef and use that.
    */
   nir_ssa_undef_instr *undef = nir_ssa_undef_instr_create(state->mem_ctx);
   nir_ssa_def_init(&undef->instr, &undef->def,
                    glsl_get_vector_elements(node->type), NULL);
   nir_instr_insert_before_cf_list(&state->impl->body, &undef->instr);
   def_stack_push(node, &undef->def, state);
   return &undef->def;
}

static void
add_phi_sources(nir_block *block, nir_block *pred,
                struct lower_variables_state *state)
{
   nir_foreach_instr(block, instr) {
      if (instr->type != nir_instr_type_phi)
         break;

      nir_phi_instr *phi = nir_instr_as_phi(instr);

      struct hash_entry *entry =
            _mesa_hash_table_search(state->phi_table, phi);
      if (!entry)
         continue;

      struct deref_node *node = entry->data;

      nir_phi_src *src = ralloc(state->mem_ctx, nir_phi_src);
      src->pred = pred;
      src->src.is_ssa = true;
      src->src.ssa = get_ssa_def_for_block(node, pred, state);

      _mesa_set_add(src->src.ssa->uses, _mesa_hash_pointer(instr), instr);

      exec_list_push_tail(&phi->srcs, &src->node);
   }
}

static bool
lower_deref_to_ssa_block(nir_block *block, void *void_state)
{
   struct lower_variables_state *state = void_state;

   nir_foreach_instr_safe(block, instr) {
      if (instr->type == nir_instr_type_phi) {
         nir_phi_instr *phi = nir_instr_as_phi(instr);

         struct hash_entry *entry =
            _mesa_hash_table_search(state->phi_table, phi);

         /* This can happen if we already have phi nodes in the program
          * that were not created in this pass.
          */
         if (!entry)
            continue;

         struct deref_node *node = entry->data;

         def_stack_push(node, &phi->dest.ssa, state);
      } else if (instr->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

         switch (intrin->intrinsic) {
         case nir_intrinsic_load_var: {
            struct deref_node *node = get_deref_node(intrin->variables[0],
                                                     false, state);

            if (node == NULL) {
               /* If we hit this path then we are referencing an invalid
                * value.  Most likely, we unrolled something and are
                * reading past the end of some array.  In any case, this
                * should result in an undefined value.
                */
               nir_ssa_undef_instr *undef =
                  nir_ssa_undef_instr_create(state->mem_ctx);
               nir_ssa_def_init(&undef->instr, &undef->def,
                                intrin->num_components, NULL);

               nir_instr_insert_before(&intrin->instr, &undef->instr);
               nir_instr_remove(&intrin->instr);

               nir_src new_src = {
                  .is_ssa = true,
                  .ssa = &undef->def,
               };

               nir_ssa_def_rewrite_uses(&intrin->dest.ssa, new_src,
                                        state->mem_ctx);
               continue;
            }

            if (!node->lower_to_ssa)
               continue;

            nir_alu_instr *mov = nir_alu_instr_create(state->mem_ctx,
                                                      nir_op_imov);
            mov->src[0].src.is_ssa = true;
            mov->src[0].src.ssa = get_ssa_def_for_block(node, block, state);
            for (unsigned i = intrin->num_components; i < 4; i++)
               mov->src[0].swizzle[i] = 0;

            assert(intrin->dest.is_ssa);

            mov->dest.write_mask = (1 << intrin->num_components) - 1;
            mov->dest.dest.is_ssa = true;
            nir_ssa_def_init(&mov->instr, &mov->dest.dest.ssa,
                             intrin->num_components, NULL);

            nir_instr_insert_before(&intrin->instr, &mov->instr);
            nir_instr_remove(&intrin->instr);

            nir_src new_src = {
               .is_ssa = true,
               .ssa = &mov->dest.dest.ssa,
            };

            nir_ssa_def_rewrite_uses(&intrin->dest.ssa, new_src,
                                     state->mem_ctx);
            break;
         }

         case nir_intrinsic_store_var: {
            struct deref_node *node = get_deref_node(intrin->variables[0],
                                                     false, state);

            if (node == NULL) {
               /* Probably an out-of-bounds array store.  That should be a
                * no-op. */
               nir_instr_remove(&intrin->instr);
               continue;
            }

            if (!node->lower_to_ssa)
               continue;

            assert(intrin->num_components ==
                   glsl_get_vector_elements(node->type));

            assert(intrin->src[0].is_ssa);

            nir_alu_instr *mov;
            if (intrin->has_predicate) {
               mov = nir_alu_instr_create(state->mem_ctx, nir_op_bcsel);
               mov->src[0].src = nir_src_copy(intrin->predicate,
                                              state->mem_ctx);
               memset(mov->src[0].swizzle, 0, sizeof mov->src[0].swizzle);

               mov->src[1].src.is_ssa = true;
               mov->src[1].src.ssa = intrin->src[0].ssa;
               for (unsigned i = intrin->num_components; i < 4; i++)
                  mov->src[1].swizzle[i] = 0;

               mov->src[2].src.is_ssa = true;
               mov->src[2].src.ssa = get_ssa_def_for_block(node, block, state);
               for (unsigned i = intrin->num_components; i < 4; i++)
                  mov->src[2].swizzle[i] = 0;

            } else {
               mov = nir_alu_instr_create(state->mem_ctx, nir_op_imov);

               mov->src[0].src.is_ssa = true;
               mov->src[0].src.ssa = intrin->src[0].ssa;
               for (unsigned i = intrin->num_components; i < 4; i++)
                  mov->src[0].swizzle[i] = 0;
            }

            mov->dest.write_mask = (1 << intrin->num_components) - 1;
            mov->dest.dest.is_ssa = true;
            nir_ssa_def_init(&mov->instr, &mov->dest.dest.ssa,
                             intrin->num_components, NULL);

            nir_instr_insert_before(&intrin->instr, &mov->instr);
            nir_instr_remove(&intrin->instr);

            def_stack_push(node, &mov->dest.dest.ssa, state);
            break;
         }

         default:
            break;
         }
      }
   }

   if (block->successors[0])
      add_phi_sources(block->successors[0], block, state);
   if (block->successors[1])
      add_phi_sources(block->successors[1], block, state);

   return true;
}

static void
insert_phi_nodes(struct lower_variables_state *state)
{
   unsigned work[state->impl->num_blocks];
   unsigned has_already[state->impl->num_blocks];
   nir_block *W[state->impl->num_blocks];

   memset(work, 0, sizeof work);
   memset(has_already, 0, sizeof has_already);

   unsigned w_start, w_end;
   unsigned iter_count = 0;

   struct hash_entry *deref_entry;
   hash_table_foreach(state->deref_leaves, deref_entry) {
      struct deref_node *node = deref_entry->data;

      if (node->stores == NULL)
         continue;

      if (!node->lower_to_ssa)
         continue;

      w_start = w_end = 0;
      iter_count++;

      struct set_entry *store_entry;
      set_foreach(node->stores, store_entry) {
         nir_intrinsic_instr *store = (nir_intrinsic_instr *)store_entry->key;
         if (work[store->instr.block->index] < iter_count)
            W[w_end++] = store->instr.block;
         work[store->instr.block->index] = iter_count;
      }

      while (w_start != w_end) {
         nir_block *cur = W[w_start++];
         struct set_entry *dom_entry;
         set_foreach(cur->dom_frontier, dom_entry) {
            nir_block *next = (nir_block *) dom_entry->key;

            /*
             * If there's more than one return statement, then the end block
             * can be a join point for some definitions. However, there are
             * no instructions in the end block, so nothing would use those
             * phi nodes. Of course, we couldn't place those phi nodes
             * anyways due to the restriction of having no instructions in the
             * end block...
             */
            if (next == state->impl->end_block)
               continue;

            if (has_already[next->index] < iter_count) {
               nir_phi_instr *phi = nir_phi_instr_create(state->mem_ctx);
               phi->dest.is_ssa = true;
               nir_ssa_def_init(&phi->instr, &phi->dest.ssa,
                                glsl_get_vector_elements(node->type), NULL);
               nir_instr_insert_before_block(next, &phi->instr);

               _mesa_hash_table_insert(state->phi_table, phi, node);

               has_already[next->index] = iter_count;
               if (work[next->index] < iter_count) {
                  work[next->index] = iter_count;
                  W[w_end++] = next;
               }
            }
         }
      }
   }
}

static bool
nir_lower_variables_impl(nir_function_impl *impl)
{
   struct lower_variables_state state;

   state.mem_ctx = ralloc_parent(impl);
   state.dead_ctx = ralloc_context(state.mem_ctx);
   state.impl = impl;

   state.deref_var_nodes = _mesa_hash_table_create(state.dead_ctx,
                                                   _mesa_hash_pointer,
                                                   _mesa_key_pointer_equal);
   state.deref_leaves = _mesa_hash_table_create(state.dead_ctx,
                                                hash_deref, derefs_equal);
   state.phi_table = _mesa_hash_table_create(state.dead_ctx,
                                             _mesa_hash_pointer,
                                             _mesa_key_pointer_equal);

   nir_foreach_block(impl, fill_deref_tables_block, &state);

   struct set *outputs = _mesa_set_create(state.dead_ctx,
                                          _mesa_key_pointer_equal);

   bool progress = false;

   nir_metadata_require(impl, nir_metadata_block_index);

   struct hash_entry *entry;
   hash_table_foreach(state.deref_leaves, entry) {
      nir_deref_var *deref = (void *)entry->key;
      struct deref_node *node = entry->data;

      if (deref->var->data.mode != nir_var_local) {
         _mesa_hash_table_remove(state.deref_leaves, entry);
         continue;
      }

      if (deref_may_be_aliased(deref, &state)) {
         _mesa_hash_table_remove(state.deref_leaves, entry);
         continue;
      }

      node->lower_to_ssa = true;
      progress = true;

      if (deref->var->constant_initializer) {
         nir_load_const_instr *load = get_const_initializer_load(deref, &state);
         load->dest.is_ssa = true;
         nir_ssa_def_init(&load->instr, &load->dest.ssa,
                          glsl_get_vector_elements(node->type), NULL);
         nir_instr_insert_before_cf_list(&impl->body, &load->instr);
         def_stack_push(node, &load->dest.ssa, &state);
      }

      if (deref->var->data.mode == nir_var_shader_out)
         _mesa_set_add(outputs, _mesa_hash_pointer(node), node);

      foreach_deref_node_match(deref, lower_copies_to_load_store, &state);
   }

   if (!progress)
      return false;

   nir_metadata_require(impl, nir_metadata_dominance);

   insert_phi_nodes(&state);
   nir_foreach_block(impl, lower_deref_to_ssa_block, &state);

   nir_metadata_preserve(impl, nir_metadata_block_index |
                               nir_metadata_dominance);

   ralloc_free(state.dead_ctx);

   return progress;
}

void
nir_lower_variables(nir_shader *shader)
{
   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         nir_lower_variables_impl(overload->impl);
   }
}
