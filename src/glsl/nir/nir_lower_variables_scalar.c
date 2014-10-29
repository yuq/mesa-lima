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
 *    Connor Abbott (cwabbott0@gmail.com)
 *
 */

/*
 * This lowering pass converts references to variables with loads/stores to
 * registers or inputs/outputs. We assume that structure splitting has already
 * been run, or else structures with indirect references can't be split. We
 * also assume that this pass will be consumed by a scalar backend, so we pack
 * things more tightly.
 */

#include "nir.h"

static unsigned
type_size(const struct glsl_type *type)
{
   unsigned int size, i;

   switch (glsl_get_base_type(type)) {
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_BOOL:
      return glsl_get_components(type);
   case GLSL_TYPE_ARRAY:
      return type_size(glsl_get_array_element(type)) * glsl_get_length(type);
   case GLSL_TYPE_STRUCT:
      size = 0;
      for (i = 0; i < glsl_get_length(type); i++) {
         size += type_size(glsl_get_struct_elem_type(type, i));
      }
      return size;
   case GLSL_TYPE_SAMPLER:
      return 0;
   case GLSL_TYPE_ATOMIC_UINT:
      return 0;
   case GLSL_TYPE_INTERFACE:
      return 0;
   case GLSL_TYPE_IMAGE:
      return 0;
   case GLSL_TYPE_VOID:
   case GLSL_TYPE_ERROR:
      unreachable("not reached");
   }

   return 0;
}

/*
 * for inputs, outputs, and uniforms, assigns starting locations for variables
 */

static void
assign_var_locations(struct hash_table *ht, unsigned *size)
{
   unsigned location = 0;

   struct hash_entry *entry;
   hash_table_foreach(ht, entry) {
      nir_variable *var = (nir_variable *) entry->data;

      /*
       * UBO's have their own address spaces, so don't count them towards the
       * number of global uniforms
       */
      if (var->data.mode == nir_var_uniform && var->interface_type != NULL)
         continue;

      var->data.driver_location = location;
      location += type_size(var->type);
   }

   *size = location;
}

static void
assign_var_locations_shader(nir_shader *shader)
{
   assign_var_locations(shader->inputs, &shader->num_inputs);
   assign_var_locations(shader->outputs, &shader->num_outputs);
   assign_var_locations(shader->uniforms, &shader->num_uniforms);
}

static void
init_reg(nir_variable *var, nir_register *reg, struct hash_table *ht,
         bool add_names)
{
   if (!glsl_type_is_scalar(var->type) &&
       !glsl_type_is_vector(var->type)) {
      reg->is_packed = true;
      reg->num_components = 1;
      reg->num_array_elems = type_size(var->type);
   } else {
      reg->num_components = glsl_get_components(var->type);
   }
   if (add_names)
      reg->name = ralloc_strdup(reg, var->name);
   _mesa_hash_table_insert(ht, var, reg);
}

static struct hash_table *
init_var_ht(nir_shader *shader, bool lower_globals, bool lower_io,
            bool add_names)
{
   struct hash_table *ht = _mesa_hash_table_create(NULL,
                                                   _mesa_hash_pointer,
                                                   _mesa_key_pointer_equal);

   if (lower_globals) {
      foreach_list_typed(nir_variable, var, node, &shader->globals) {
         nir_register *reg = nir_global_reg_create(shader);
         init_reg(var, reg, ht, add_names);
      }
   }

   if (lower_io) {
      struct hash_entry *entry;
      hash_table_foreach(shader->outputs, entry) {
         nir_variable *var = (nir_variable *) entry->data;
         nir_register *reg = nir_global_reg_create(shader);
         init_reg(var, reg, ht, add_names);
      }
   }

   nir_foreach_overload(shader, overload) {
      if (overload->impl) {
         nir_function_impl *impl = overload->impl;

         foreach_list_typed(nir_variable, var, node, &impl->locals) {
            nir_register *reg = nir_local_reg_create(impl);
            init_reg(var, reg, ht, add_names);
         }
      }
   }

   return ht;
}

static bool
deref_has_indirect(nir_deref_var *deref_var)
{
   nir_deref *deref = &deref_var->deref;

   while (deref->child != NULL) {
      deref = deref->child;
      if (deref->deref_type == nir_deref_type_array) {
         nir_deref_array *deref_array = nir_deref_as_array(deref);
         if (deref_array->has_indirect)
            return true;
      }
   }

   return false;
}

static unsigned
get_deref_offset(nir_deref_var *deref_var, nir_instr *instr,
                 nir_function_impl *impl, bool native_integers,
                 nir_src *indirect)
{
   void *mem_ctx = ralloc_parent(instr);

   bool first_indirect = true;

   unsigned base_offset = 0;
   nir_deref *deref = &deref_var->deref;
   while (deref->child != NULL) {
      const struct glsl_type *parent_type = deref->type;
      deref = deref->child;

      if (deref->deref_type == nir_deref_type_array) {
         nir_deref_array *deref_array = nir_deref_as_array(deref);
         unsigned size = type_size(deref->type);

         base_offset += size * deref_array->base_offset;

         if (deref_array->has_indirect) {
            nir_src src;
            if (size == 1) {
               src = deref_array->indirect;
            } else {
               /* temp1 = size * deref_array->indirect */

               nir_register *const_reg = nir_local_reg_create(impl);
               const_reg->num_components = 1;

               nir_load_const_instr *load_const =
                  nir_load_const_instr_create(mem_ctx);
               load_const->dest.reg.reg = const_reg;
               load_const->num_components = 1;
               load_const->value.u[0] = size;
               nir_instr_insert_before(instr, &load_const->instr);

               nir_register *reg = nir_local_reg_create(impl);
               reg->num_components = 1;

               nir_op op;
               if (native_integers)
                  op = nir_op_imul;
               else
                  op = nir_op_fmul;
               nir_alu_instr *mul_instr = nir_alu_instr_create(mem_ctx, op);
               mul_instr->dest.write_mask = 1;
               mul_instr->dest.dest.reg.reg = reg;
               mul_instr->src[0].src = deref_array->indirect;
               mul_instr->src[1].src.reg.reg = const_reg;
               nir_instr_insert_before(instr, &mul_instr->instr);

               src.is_ssa = false;
               src.reg.reg = reg;
               src.reg.base_offset = 0;
               src.reg.indirect = NULL;
            }

            if (!first_indirect) {
               /* temp2 = indirect + temp1 */

               nir_register *reg = nir_local_reg_create(impl);
               reg->num_components = 1;

               nir_op op;
               if (native_integers)
                  op = nir_op_iadd;
               else
                  op = nir_op_fadd;
               nir_alu_instr *add_instr = nir_alu_instr_create(mem_ctx, op);
               add_instr->dest.write_mask = 1;
               add_instr->dest.dest.reg.reg = reg;
               add_instr->src[0].src = *indirect;
               add_instr->src[1].src = src;
               nir_instr_insert_before(instr, &add_instr->instr);

               src.is_ssa = false;
               src.reg.reg = reg;
               src.reg.base_offset = 0;
               src.reg.indirect = NULL;
            }

            /* indirect = tempX */
            *indirect = src;
            first_indirect = false;
         }
      } else {
         nir_deref_struct *deref_struct = nir_deref_as_struct(deref);

         unsigned i = 0;
         while(strcmp(glsl_get_struct_elem_name(parent_type, i),
                      deref_struct->elem) != 0) {
            base_offset += type_size(glsl_get_struct_elem_type(parent_type, i));
            i++;
         }
      }
   }

   return base_offset;
}

/*
 * We cannot convert variables used in calls, so remove them from the hash
 * table.
 */

static bool
remove_call_vars_cb(nir_block *block, void *state)
{
   struct hash_table *ht = (struct hash_table *) state;

   nir_foreach_instr(block, instr) {
      if (instr->type == nir_instr_type_call) {
         nir_call_instr *call = nir_instr_as_call(instr);
         if (call->return_deref) {
            struct hash_entry *entry =
               _mesa_hash_table_search(ht, call->return_deref->var);
            if (entry)
               _mesa_hash_table_remove(ht, entry);
         }

         for (unsigned i = 0; i < call->num_params; i++) {
            struct hash_entry *entry =
               _mesa_hash_table_search(ht, call->params[i]->var);
            if (entry)
               _mesa_hash_table_remove(ht, entry);
         }
      }
   }

   return true;
}

static void
remove_local_vars(nir_function_impl *impl, struct hash_table *ht)
{
   if (impl->return_var) {
      struct hash_entry *entry =
         _mesa_hash_table_search(ht, impl->return_var);

      if (entry)
         _mesa_hash_table_remove(ht, entry);
   }

   for (unsigned i = 0; i < impl->num_params; i++) {
      struct hash_entry *entry =
         _mesa_hash_table_search(ht, impl->params[i]);
      if (entry)
         _mesa_hash_table_remove(ht, entry);
   }

   nir_foreach_block(impl, remove_call_vars_cb, ht);
}

static void
remove_local_vars_shader(nir_shader *shader, struct hash_table *ht)
{
   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         remove_local_vars(overload->impl, ht);
   }
}

static nir_deref *
get_deref_tail(nir_deref *deref)
{
   while (deref->child != NULL)
      deref = deref->child;
   return deref;
}

/* helper for reg_const_load which emits a single instruction */
static void
reg_const_load_single_instr(nir_reg_dest reg, nir_constant *constant,
                            enum glsl_base_type base_type,
                            unsigned num_components, unsigned offset,
                            nir_function_impl *impl, void *mem_ctx)
{
   nir_load_const_instr *instr = nir_load_const_instr_create(mem_ctx);
   instr->num_components = num_components;
   for (unsigned i = 0; i < num_components; i++) {
      switch (base_type) {
      case GLSL_TYPE_FLOAT:
      case GLSL_TYPE_INT:
      case GLSL_TYPE_UINT:
         instr->value.u[i] = constant->value.u[i + offset];
         break;
      case GLSL_TYPE_BOOL:
         instr->value.u[i] = constant->value.u[i + offset] ?
                             NIR_TRUE : NIR_FALSE;
         break;
      default:
         unreachable("Invalid immediate type");
      }
   }
   instr->dest.reg = reg;
   instr->dest.reg.base_offset += offset;

   nir_instr_insert_before_cf_list(&impl->body, &instr->instr);
}

/* loads a constant value into a register */
static void
reg_const_load(nir_reg_dest reg, nir_constant *constant,
               const struct glsl_type *type, nir_function_impl *impl,
               void *mem_ctx)
{
   unsigned offset = 0;
   const struct glsl_type *subtype;
   unsigned subtype_size;

   enum glsl_base_type base_type = glsl_get_base_type(type);
   switch (base_type) {
      case GLSL_TYPE_FLOAT:
      case GLSL_TYPE_INT:
      case GLSL_TYPE_UINT:
      case GLSL_TYPE_BOOL:
         if (glsl_type_is_matrix(type)) {
            for (unsigned i = 0; i < glsl_get_matrix_columns(type); i++) {
               reg_const_load_single_instr(reg, constant, base_type,
                                           glsl_get_vector_elements(type),
                                           i * glsl_get_vector_elements(type),
                                           impl, mem_ctx);
            }
         } else {
            reg_const_load_single_instr(reg, constant, base_type,
                                        glsl_get_vector_elements(type), 0,
                                        impl, mem_ctx);
         }
         break;

      case GLSL_TYPE_STRUCT:
         for (unsigned i = 0; i < glsl_get_length(type); i++) {
            const struct glsl_type *field = glsl_get_struct_elem_type(type, i);
            nir_reg_dest new_reg = reg;
            new_reg.base_offset += offset;
            reg_const_load(new_reg, constant->elements[i], field, impl,
                           mem_ctx);
            offset += type_size(field);
         }
         break;

      case GLSL_TYPE_ARRAY:
         subtype = glsl_get_array_element(type);
         subtype_size = type_size(subtype);
         for (unsigned i = 0; i < glsl_get_length(type); i++) {
            nir_reg_dest new_reg = reg;
            new_reg.base_offset += subtype_size * i;
            reg_const_load(new_reg, constant->elements[i], subtype, impl,
                           mem_ctx);
         }
         break;

      default:
         assert(0);
         break;
   }
}

/* recursively emits a register <-> dereference block copy */
static void
var_reg_block_copy_impl(nir_reg_src reg, nir_deref_var *deref_head,
                        nir_src *predicate, const struct glsl_type *type,
                        nir_instr *after, bool var_dest, void *mem_ctx)
{
   unsigned offset;

   switch (glsl_get_base_type(type)) {
      case GLSL_TYPE_FLOAT:
      case GLSL_TYPE_INT:
      case GLSL_TYPE_UINT:
      case GLSL_TYPE_BOOL:
         if (glsl_type_is_matrix(type)) {
            for (unsigned i = 0; i < glsl_get_matrix_columns(type); i++) {
               nir_deref_array *deref_array = nir_deref_array_create(mem_ctx);
               deref_array->base_offset = i;
               deref_array->deref.type = glsl_get_column_type(type);

               nir_deref_var *new_deref_head =
                  nir_deref_as_var(nir_copy_deref(mem_ctx, &deref_head->deref));
               get_deref_tail(&new_deref_head->deref)->child =
                  &deref_array->deref;

               nir_reg_src new_reg = reg;
               new_reg.base_offset += i * glsl_get_vector_elements(type);

               var_reg_block_copy_impl(new_reg, new_deref_head, predicate,
                                       glsl_get_column_type(type), after,
                                       var_dest, mem_ctx);
            }
         } else {
            if (var_dest) {
               nir_intrinsic_op op;
               switch (glsl_get_vector_elements(type)) {
                  case 1: op = nir_intrinsic_store_var_vec1; break;
                  case 2: op = nir_intrinsic_store_var_vec2; break;
                  case 3: op = nir_intrinsic_store_var_vec3; break;
                  case 4: op = nir_intrinsic_store_var_vec4; break;
                  default: assert(0); break;
               }

               nir_intrinsic_instr *store =
                  nir_intrinsic_instr_create(mem_ctx, op);
               store->variables[0] = deref_head;
               store->src[0].reg.reg = reg.reg;
               store->src[0].reg.base_offset = reg.base_offset;
               if (reg.indirect) {
                  store->src[0].reg.indirect = ralloc(mem_ctx, nir_src);
                  *store->src[0].reg.indirect = *reg.indirect;
               }

               if (predicate) {
                  store->has_predicate = true;
                  store->predicate = nir_src_copy(*predicate, mem_ctx);
               }

               nir_instr_insert_before(after, &store->instr);
            } else {
               nir_intrinsic_op op;
               switch (glsl_get_vector_elements(type)) {
                  case 1: op = nir_intrinsic_load_var_vec1; break;
                  case 2: op = nir_intrinsic_load_var_vec2; break;
                  case 3: op = nir_intrinsic_load_var_vec3; break;
                  case 4: op = nir_intrinsic_load_var_vec4; break;
                  default: assert(0); break;
               }

               nir_intrinsic_instr *load =
                  nir_intrinsic_instr_create(mem_ctx, op);
               load->variables[0] = deref_head;
               load->dest.reg.reg = reg.reg;
               load->dest.reg.base_offset = reg.base_offset;
               if (reg.indirect) {
                  load->dest.reg.indirect = ralloc(mem_ctx, nir_src);
                  *load->dest.reg.indirect = *reg.indirect;
               }

               if (predicate) {
                  load->has_predicate = true;
                  load->predicate = nir_src_copy(*predicate, mem_ctx);
               }

               nir_instr_insert_before(after, &load->instr);
            }
         }
         break;

      case GLSL_TYPE_STRUCT:
         offset = 0;
         for (unsigned i = 0; i < glsl_get_length(type); i++) {
            const struct glsl_type *field_type =
               glsl_get_struct_elem_type(type, i);
            const char *field_name = glsl_get_struct_elem_name(type, i);

            nir_deref_struct *deref_struct =
               nir_deref_struct_create(mem_ctx, field_name);
            deref_struct->deref.type = field_type;
            deref_struct->elem = field_name;

            nir_deref_var *new_deref_head =
               nir_deref_as_var(nir_copy_deref(mem_ctx, &deref_head->deref));
            get_deref_tail(&new_deref_head->deref)->child =
               &deref_struct->deref;

            nir_reg_src new_reg = reg;
            new_reg.base_offset += offset;

            var_reg_block_copy_impl(new_reg, new_deref_head, predicate,
                                    field_type, after, var_dest, mem_ctx);

            offset += type_size(field_type);
         }
         break;

      case GLSL_TYPE_ARRAY:
         for (unsigned i = 0; i < glsl_get_length(type);
                  i++) {
            const struct glsl_type *elem_type = glsl_get_array_element(type);

            nir_deref_array *deref_array = nir_deref_array_create(mem_ctx);
            deref_array->base_offset = i;
            deref_array->deref.type = elem_type;

            nir_deref_var *new_deref_head =
               nir_deref_as_var(nir_copy_deref(mem_ctx, &deref_head->deref));
            get_deref_tail(&new_deref_head->deref)->child =
               &deref_array->deref;

            nir_reg_src new_reg = reg;
            new_reg.base_offset += i * type_size(elem_type);

            var_reg_block_copy_impl(new_reg, new_deref_head, predicate,
                                    elem_type, after, var_dest, mem_ctx);
         }
         break;

      default:
         break;
   }
}

static nir_intrinsic_op
get_load_op(nir_variable_mode mode, bool indirect, unsigned num_components)
{
   if (indirect) {
      switch (mode) {
         case nir_var_shader_in:
            switch (num_components) {
               case 1: return nir_intrinsic_load_input_vec1_indirect;
               case 2: return nir_intrinsic_load_input_vec2_indirect;
               case 3: return nir_intrinsic_load_input_vec3_indirect;
               case 4: return nir_intrinsic_load_input_vec4_indirect;
               default: assert(0); break;
            }
            break;

         case nir_var_uniform:
            switch (num_components) {
               case 1: return nir_intrinsic_load_uniform_vec1_indirect;
               case 2: return nir_intrinsic_load_uniform_vec2_indirect;
               case 3: return nir_intrinsic_load_uniform_vec3_indirect;
               case 4: return nir_intrinsic_load_uniform_vec4_indirect;
               default: assert(0); break;
            }
            break;

         default:
            assert(0);
            break;
      }
   } else {
      switch (mode) {
         case nir_var_shader_in:
            switch (num_components) {
               case 1: return nir_intrinsic_load_input_vec1;
               case 2: return nir_intrinsic_load_input_vec2;
               case 3: return nir_intrinsic_load_input_vec3;
               case 4: return nir_intrinsic_load_input_vec4;
               default: assert(0); break;
            }
            break;

         case nir_var_uniform:
            switch (num_components) {
               case 1: return nir_intrinsic_load_uniform_vec1;
               case 2: return nir_intrinsic_load_uniform_vec2;
               case 3: return nir_intrinsic_load_uniform_vec3;
               case 4: return nir_intrinsic_load_uniform_vec4;
               default: assert(0); break;
            }
            break;

         default:
            assert(0);
            break;
      }
   }

   return nir_intrinsic_load_input_vec1;
}

/* emits an input -> reg block copy */

static void
reg_input_block_copy(nir_reg_dest dest, unsigned src_index, nir_src *indirect,
                     nir_src *predicate, unsigned size,
                     unsigned num_components, nir_variable_mode mode,
                     nir_instr *after, void *mem_ctx)
{
   nir_intrinsic_op op = get_load_op(mode, indirect != NULL, num_components);

   nir_intrinsic_instr *load = nir_intrinsic_instr_create(mem_ctx, op);
   load->const_index[0] = src_index;
   load->const_index[1] = size;
   if (indirect)
      load->src[0] = *indirect;
   if (predicate) {
      load->has_predicate = true;
      load->predicate = nir_src_copy(*predicate, mem_ctx);
   }
   load->dest.reg = dest;
   nir_instr_insert_before(after, &load->instr);
}

/* emits a variable/input -> register block copy */

static void
var_reg_block_copy(nir_deref_var *src, nir_reg_dest dest, nir_src *predicate,
                   bool lower_io, nir_instr *after, nir_function_impl *impl,
                   bool native_integers, void *mem_ctx)
{
   const struct glsl_type *src_type = get_deref_tail(&src->deref)->type;

   if (lower_io && (src->var->data.mode == nir_var_shader_in ||
       src->var->data.mode == nir_var_uniform)) {
      unsigned size, num_components;
      if (glsl_type_is_scalar(src_type) || glsl_type_is_vector(src_type)) {
         num_components = glsl_get_vector_elements(src_type);
         size = 1;
      } else {
         num_components = 1;
         size = type_size(src_type);
      }
      bool has_indirect = deref_has_indirect(src);
      nir_src indirect;
      nir_src *indirect_ptr = has_indirect ? &indirect : NULL;
      unsigned offset = get_deref_offset(src, after, impl, native_integers,
                                         indirect_ptr);
      offset += src->var->data.driver_location;

      reg_input_block_copy(dest, offset, indirect_ptr, predicate, size,
                           num_components, src->var->data.mode, after,
                           mem_ctx);
   } else {
      nir_reg_src reg;
      reg.reg = dest.reg;
      reg.base_offset = dest.base_offset;
      reg.indirect = dest.indirect;

      var_reg_block_copy_impl(reg, src, predicate, src_type, after, false,
                              mem_ctx);
   }
}

/* emits a register -> variable copy */
static void
reg_var_block_copy(nir_reg_src src, nir_deref_var *dest, nir_src *predicate,
                   nir_instr *after, void *mem_ctx)
{
   const struct glsl_type *dest_type = get_deref_tail(&dest->deref)->type;

   var_reg_block_copy_impl(src, dest, predicate, dest_type, after, true,
                           mem_ctx);
}

/*
 * emits an input -> variable block copy using an intermediate register
 */
static void
var_var_block_copy(nir_deref_var *src, nir_deref_var *dest, nir_src *predicate,
                   nir_instr *after, nir_function_impl *impl,
                   bool native_integers, void *mem_ctx)
{
   const struct glsl_type *type = get_deref_tail(&dest->deref)->type;
   nir_register *reg = nir_local_reg_create(impl);
   if (glsl_type_is_scalar(type) || glsl_type_is_vector(type)) {
      reg->num_components = glsl_get_vector_elements(type);
   } else {
      reg->is_packed = true;
      reg->num_components = 1;
      reg->num_array_elems = type_size(type);
   }

   nir_reg_src reg_src;
   reg_src.base_offset = 0;
   reg_src.indirect = NULL;
   reg_src.reg = reg;

   nir_reg_dest reg_dest;
   reg_dest.base_offset = 0;
   reg_dest.indirect = NULL;
   reg_dest.reg = reg;

   var_reg_block_copy(src, reg_dest, predicate, true, after, impl,
                      native_integers, mem_ctx);
   reg_var_block_copy(reg_src, dest, predicate, after, mem_ctx);
}

/* emits a register -> register block copy */
static void
reg_reg_block_copy(nir_reg_dest dest, nir_reg_src src, nir_src *predicate,
                   const struct glsl_type *type, nir_instr *after,
                   void *mem_ctx)
{
   if (!dest.reg->is_packed && !src.reg->is_packed)
      assert(dest.reg->num_components == src.reg->num_components);

   unsigned size, num_components;
   if (dest.reg->is_packed && src.reg->is_packed) {
      size = type_size(type);
      num_components = 1;
   } else {
      size = 1;
      if (dest.reg->is_packed)
         num_components = src.reg->num_components;
      else
         num_components = dest.reg->num_components;
   }

   for (unsigned i = 0; i < size; i++) {
      nir_alu_instr *move = nir_alu_instr_create(mem_ctx, nir_op_imov);
      move->dest.write_mask = (1 << num_components) - 1;

      move->dest.dest.reg.reg = dest.reg;
      move->dest.dest.reg.base_offset = dest.base_offset + i;
      if (dest.indirect != NULL) {
         move->dest.dest.reg.indirect = ralloc(mem_ctx, nir_src);
         *move->dest.dest.reg.indirect = *dest.indirect;
      }

      if (predicate) {
         move->has_predicate = true;
         move->predicate = nir_src_copy(*predicate, mem_ctx);
      }

      move->src[0].src.reg = src;
      move->src[0].src.reg.base_offset += i;

      nir_instr_insert_before(after, &move->instr);
   }
}

static nir_reg_dest
create_dest(nir_deref_var *deref, nir_instr *instr, nir_register *reg,
            nir_function_impl *impl, bool native_integers, void *mem_ctx)
{
   nir_reg_dest dest;
   if (deref_has_indirect(deref)) {
      dest.indirect = ralloc(mem_ctx, nir_src);
      dest.indirect->is_ssa = false;
      dest.base_offset = get_deref_offset(deref, instr,
                                          impl, native_integers,
                                          dest.indirect);
   } else {
      dest.base_offset = get_deref_offset(deref, instr,
                                          impl, native_integers, NULL);
      dest.indirect = NULL;
   }
   dest.reg = reg;

   return dest;
}

static nir_reg_src
create_src(nir_deref_var *deref, nir_instr *instr, nir_register *reg,
           nir_function_impl *impl, bool native_integers, void *mem_ctx)
{
   nir_reg_src src;
   if (deref_has_indirect(deref)) {
      src.indirect = ralloc(mem_ctx, nir_src);
      src.indirect->is_ssa = false;
      src.base_offset = get_deref_offset(deref, instr,
                                         impl, native_integers,
                                         src.indirect);
   } else {
      src.base_offset = get_deref_offset(deref, instr,
                                         impl, native_integers, NULL);
      src.indirect = NULL;
   }
   src.reg = reg;

   return src;
}

static void
handle_var_copy(nir_intrinsic_instr *instr, nir_function_impl *impl,
                bool native_integers, bool lower_io, struct hash_table *ht)
{
   void *mem_ctx = ralloc_parent(instr);

   struct hash_entry *entry;

   nir_variable *dest_var = instr->variables[0]->var;
   nir_variable *src_var = instr->variables[1]->var;

   const struct glsl_type *type =
      get_deref_tail(&instr->variables[0]->deref)->type;

   nir_src *predicate = instr->has_predicate ? &instr->predicate : NULL;

   /*
    * The source can be either:
    * 1. a variable we're lowering to a register
    * 2. an input or uniform we're lowering to loads from an index
    * 3. a variable we can't lower yet
    *
    * and similarly, the destination can be either:
    * 1. a variable we're lowering to a register
    * 2. a variable we can't lower yet
    *
    * meaning that there are six cases, including the trivial one (where
    * source and destination are #3 and #2 respectively) where we can't do
    * anything.
    */

   entry = _mesa_hash_table_search(ht, dest_var);
   if (entry) {
      nir_reg_dest dest = create_dest(instr->variables[0], &instr->instr,
                                      (nir_register *) entry->data, impl,
                                      native_integers, mem_ctx);

      entry = _mesa_hash_table_search(ht, src_var);
      if (entry) {
         nir_reg_src src = create_src(instr->variables[1], &instr->instr,
                                     (nir_register *) entry->data, impl,
                                     native_integers, mem_ctx);

         reg_reg_block_copy(dest, src, predicate, type, &instr->instr, mem_ctx);
      } else {
         var_reg_block_copy(instr->variables[1], dest, predicate, lower_io,
                            &instr->instr, impl, native_integers, mem_ctx);
      }
   } else {
      entry = _mesa_hash_table_search(ht, src_var);
      if (entry) {
         nir_reg_src src = create_src(instr->variables[1], &instr->instr,
                                     (nir_register *) entry->data, impl,
                                     native_integers, mem_ctx);

         reg_var_block_copy(src, instr->variables[0], predicate, &instr->instr,
                            mem_ctx);
      } else {
         if (!lower_io || (src_var->data.mode != nir_var_shader_in &&
             src_var->data.mode != nir_var_uniform)) {
            /* nothing to do here */
            return;
         }

         var_var_block_copy(instr->variables[1], instr->variables[0], predicate,
                            &instr->instr, impl, native_integers, mem_ctx);
      }
   }

   nir_instr_remove(&instr->instr);
}

static void
handle_var_load(nir_intrinsic_instr *instr, nir_function_impl *impl,
                bool native_integers, bool lower_io, struct hash_table *ht)
{
   void *mem_ctx = ralloc_parent(instr);

   struct hash_entry *entry =
      _mesa_hash_table_search(ht, instr->variables[0]->var);

   if (entry == NULL) {
      nir_variable *src_var = instr->variables[0]->var;

      if (lower_io && (src_var->data.mode == nir_var_shader_in ||
          src_var->data.mode == nir_var_uniform)) {
         bool has_indirect = deref_has_indirect(instr->variables[0]);
         unsigned num_components =
            nir_intrinsic_infos[instr->intrinsic].dest_components;
         nir_src indirect;
         unsigned offset = get_deref_offset(instr->variables[0], &instr->instr,
                                            impl, native_integers, &indirect);
         offset += src_var->data.driver_location;

         nir_intrinsic_op op = get_load_op(src_var->data.mode, has_indirect,
                                           num_components);
         nir_intrinsic_instr *load = nir_intrinsic_instr_create(mem_ctx, op);
         load->dest = instr->dest;
         load->const_index[0] = (int) offset;
         load->const_index[1] = 1;
         if (has_indirect)
            load->src[0] = indirect;

         if (instr->has_predicate) {
            load->has_predicate = true;
            load->predicate = nir_src_copy(instr->predicate, mem_ctx);
         }

         nir_instr_insert_before(&instr->instr, &load->instr);
      } else {
         return;
      }
   } else {
      nir_register *reg = (nir_register *) entry->data;

      nir_alu_instr *move = nir_alu_instr_create(mem_ctx, nir_op_imov);
      unsigned dest_components =
         nir_intrinsic_infos[instr->intrinsic].dest_components;
      move->dest.dest = instr->dest;
      move->dest.write_mask = (1 << dest_components) - 1;
      move->src[0].src.reg = create_src(instr->variables[0], &instr->instr,
                                        reg, impl, native_integers, mem_ctx);
      if (instr->has_predicate) {
         move->has_predicate = true;
         move->predicate = nir_src_copy(instr->predicate, mem_ctx);
      }
      nir_instr_insert_before(&instr->instr, &move->instr);
   }

   nir_instr_remove(&instr->instr);
}

static void
handle_var_store(nir_intrinsic_instr *instr, nir_function_impl *impl,
                 bool native_integers, bool lower_io, struct hash_table *ht)
{
   void *mem_ctx = ralloc_parent(instr);

   struct hash_entry *entry =
      _mesa_hash_table_search(ht, instr->variables[0]->var);
   if (entry == NULL)
         return;

   nir_register *reg = (nir_register *) entry->data;

   nir_alu_instr *move = nir_alu_instr_create(mem_ctx, nir_op_imov);
   unsigned src_components =
      nir_intrinsic_infos[instr->intrinsic].src_components[0];
   move->dest.dest.reg = create_dest(instr->variables[0], &instr->instr,
                                       reg, impl, native_integers, mem_ctx);
   move->dest.write_mask = (1 << src_components) - 1;
   move->src[0].src = instr->src[0];
   if (instr->has_predicate) {
      move->has_predicate = true;
      move->predicate = nir_src_copy(instr->predicate, mem_ctx);
   }
   nir_instr_insert_before(&instr->instr, &move->instr);
   nir_instr_remove(&instr->instr);
}

typedef struct {
   struct hash_table *ht;
   bool native_integers, lower_io;
   nir_function_impl *impl;
} rewrite_state;

static bool
rewrite_block_cb(nir_block *block, void *_state)
{
   rewrite_state *state = (rewrite_state *) _state;

   nir_foreach_instr_safe(block, instr) {
      if (instr->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
            case nir_intrinsic_load_var_vec1:
            case nir_intrinsic_load_var_vec2:
            case nir_intrinsic_load_var_vec3:
            case nir_intrinsic_load_var_vec4:
               handle_var_load(intrin, state->impl, state->native_integers,
                               state->lower_io, state->ht);
               break;

            case nir_intrinsic_store_var_vec1:
            case nir_intrinsic_store_var_vec2:
            case nir_intrinsic_store_var_vec3:
            case nir_intrinsic_store_var_vec4:
               handle_var_store(intrin, state->impl, state->native_integers,
                                state->lower_io, state->ht);
               break;

            case nir_intrinsic_copy_var:
               handle_var_copy(intrin, state->impl, state->native_integers,
                               state->lower_io, state->ht);
               break;

            default:
               break;
         }
      }
   }

   return true;
}

static void
rewrite_impl(nir_function_impl *impl, struct hash_table *ht,
             bool native_integers, bool lower_io)
{
   rewrite_state state;
   state.ht = ht;
   state.native_integers = native_integers;
   state.lower_io = lower_io;
   state.impl = impl;

   nir_foreach_block(impl, rewrite_block_cb, &state);
}

static void
insert_load_const_impl(nir_function_impl *impl, struct exec_list *vars,
                       struct hash_table *ht)
{
   void *mem_ctx = ralloc_parent(impl);

   foreach_list_typed(nir_variable, var, node, vars) {
      if (var->constant_initializer == NULL)
         continue;

      struct hash_entry *entry = _mesa_hash_table_search(ht, var);
      if (entry) {
         nir_register *reg = (nir_register *) entry->data;
         nir_reg_dest dest;
         dest.reg = reg;
         dest.base_offset = 0;
         dest.indirect = NULL;
         reg_const_load(dest, var->constant_initializer, var->type, impl,
                        mem_ctx);
      }
   }
}

static nir_intrinsic_op
get_store_op(bool indirect, unsigned num_components)
{
   if (indirect) {
      switch (num_components) {
         case 1: return nir_intrinsic_store_output_vec1_indirect;
         case 2: return nir_intrinsic_store_output_vec2_indirect;
         case 3: return nir_intrinsic_store_output_vec3_indirect;
         case 4: return nir_intrinsic_store_output_vec4_indirect;
         default: assert(0); break;
      }
   } else {
      switch (num_components) {
         case 1: return nir_intrinsic_store_output_vec1;
         case 2: return nir_intrinsic_store_output_vec2;
         case 3: return nir_intrinsic_store_output_vec3;
         case 4: return nir_intrinsic_store_output_vec4;
         default: assert(0); break;
      }
   }

   return nir_intrinsic_store_output_vec1;
}

/* emits a reg -> output block copy after a block */
static void
reg_output_block_copy_block(nir_reg_src src, unsigned dest_index,
                            unsigned num_components, unsigned size,
                            nir_block *block, void *mem_ctx)
{
   nir_intrinsic_op op = get_store_op(false, num_components);

   nir_intrinsic_instr *store = nir_intrinsic_instr_create(mem_ctx, op);
   store->const_index[0] = dest_index;
   store->const_index[1] = (size == 0) ? 1 : size;
   store->src[0].reg = src;
   nir_instr_insert_after_block(block, &store->instr);
}

/* emits a reg -> output copy after an instruction */
static void
reg_output_block_copy_instr(nir_reg_src src, unsigned dest_index,
                            unsigned num_components, unsigned size,
                            nir_instr *after, void *mem_ctx)
{
   nir_intrinsic_op op = get_store_op(false, num_components);

   nir_intrinsic_instr *store = nir_intrinsic_instr_create(mem_ctx, op);
   store->const_index[0] = dest_index;
   store->const_index[1] = (size == 0) ? 1 : size;
   store->src[0].reg = src;
   nir_instr_insert_before(after, &store->instr);
}

static nir_function_impl *
find_main(nir_shader *shader)
{
   foreach_list_typed(nir_function, func, node, &shader->functions) {
      if (strcmp(func->name, "main") == 0) {
         assert(exec_list_length(&func->overload_list) == 1);
         nir_function_overload *overload = nir_function_first_overload(func);
         return overload->impl;
      }
   }

   assert(0);
   return NULL;
}

static void
insert_output_reg_copies(nir_shader *shader, nir_block *block,
                         nir_instr *after, struct hash_table *ht)
{
   struct hash_entry *entry;
   hash_table_foreach(shader->outputs, entry) {
      nir_variable *var = (nir_variable *) entry->data;

      struct hash_entry *entry2;
      entry2 = _mesa_hash_table_search(ht, var);
      if (entry2) {
         nir_register *reg = (nir_register *) entry2->data;
         nir_reg_src src;
         src.reg = reg;
         src.base_offset = 0;
         src.indirect = NULL;

         if (after) {
            reg_output_block_copy_instr(src, var->data.driver_location,
                                        reg->num_components,
                                        reg->num_array_elems,
                                        after, shader);
         } else {
            reg_output_block_copy_block(src, var->data.driver_location,
                                        reg->num_components,
                                        reg->num_array_elems,
                                        block, shader);
         }
      }
   }
}

typedef struct {
   struct hash_table *ht;
   nir_shader *shader;
   bool found_emit_vertex;
} reg_output_state;

static bool
insert_output_reg_copies_emit_vertex(nir_block *block, void *_state)
{
   reg_output_state *state = (reg_output_state *) _state;

   nir_foreach_instr(block, instr) {
      if (instr->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *intrin_instr = nir_instr_as_intrinsic(instr);
         if (intrin_instr->intrinsic == nir_intrinsic_emit_vertex) {
            insert_output_reg_copies(state->shader, NULL, instr, state->ht);
            state->found_emit_vertex = true;
         }
      }
   }

   return true;
}

static void
insert_output_reg_copies_shader(nir_shader *shader, struct hash_table *ht)
{
   nir_function_impl *main_impl = find_main(shader);

   reg_output_state state;
   state.shader = shader;
   state.ht = ht;
   state.found_emit_vertex = false;
   nir_foreach_block(main_impl, insert_output_reg_copies_emit_vertex, &state);

   if (!state.found_emit_vertex) {
      struct set_entry *entry;
      set_foreach(main_impl->end_block->predecessors, entry) {
         nir_block *block = (nir_block *) entry->key;
         insert_output_reg_copies(shader, block, NULL, ht);
      }
   }
}

static void
rewrite_shader(nir_shader *shader, struct hash_table *ht, bool native_integers,
               bool lower_globals, bool lower_io)
{
   nir_foreach_overload(shader, overload) {
      if (overload->impl) {
         insert_load_const_impl(overload->impl, &overload->impl->locals, ht);
         if (lower_globals && strcmp(overload->function->name, "main") == 0)
            insert_load_const_impl(overload->impl, &shader->globals, ht);
         rewrite_impl(overload->impl, ht, native_integers, lower_io);
      }
   }
}

void
nir_lower_variables_scalar(nir_shader *shader, bool lower_globals,
                           bool lower_io, bool add_names, bool native_integers)
{
   if (lower_io)
      assign_var_locations_shader(shader);
   struct hash_table *ht = init_var_ht(shader, lower_globals, lower_io,
                                       add_names);
   remove_local_vars_shader(shader, ht);
   rewrite_shader(shader, ht, native_integers, lower_globals, lower_io);
   if (lower_io)
      insert_output_reg_copies_shader(shader, ht);
   _mesa_hash_table_destroy(ht, NULL);
}
