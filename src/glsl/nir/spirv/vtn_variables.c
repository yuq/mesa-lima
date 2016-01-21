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

static nir_variable *
get_builtin_variable(struct vtn_builder *b,
                     nir_variable_mode mode,
                     const struct glsl_type *type,
                     SpvBuiltIn builtin);

nir_deref_var *
vtn_access_chain_to_deref(struct vtn_builder *b, struct vtn_access_chain *chain)
{
   nir_deref_var *deref_var = nir_deref_var_create(b, chain->var);
   nir_deref *tail = &deref_var->deref;
   struct vtn_type *deref_type = chain->var_type;

   for (unsigned i = 0; i < chain->length; i++) {
      struct vtn_value *idx_val = vtn_untyped_value(b, chain->ids[i]);
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
            deref_arr->deref_array_type = nir_deref_array_type_direct;
            deref_arr->base_offset = idx_val->constant->value.u[0];
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
         if (glsl_type_is_array(chain->var->type)) {
            /* This builtin is a per-vertex builtin */
            assert(b->shader->stage == MESA_SHADER_GEOMETRY);
            assert(chain->var->data.mode == nir_var_shader_in);
            builtin_type = glsl_array_type(builtin_type,
                                           b->shader->info.gs.vertices_in);

            /* The first non-var deref should be an array deref. */
            assert(deref_var->deref.child->deref_type ==
                   nir_deref_type_array);
            per_vertex_deref = nir_deref_as_array(deref_var->deref.child);
         }

         nir_variable *builtin = get_builtin_variable(b,
                                                      chain->var->data.mode,
                                                      builtin_type,
                                                      deref_type->builtin);
         deref_var = nir_deref_var_create(b, builtin);

         if (per_vertex_deref) {
            /* Since deref chains start at the variable, we can just
             * steal that link and use it.
             */
            deref_var->deref.child = &per_vertex_deref->deref;
            per_vertex_deref->deref.child = NULL;
            per_vertex_deref->deref.type =
               glsl_get_array_element(builtin_type);

            tail = &per_vertex_deref->deref;
         } else {
            tail = &deref_var->deref;
         }
      } else {
         tail = tail->child;
      }
   }

   return deref_var;
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

nir_deref_var *
vtn_nir_deref(struct vtn_builder *b, uint32_t id)
{
   struct vtn_access_chain *chain =
      vtn_value(b, id, vtn_value_type_access_chain)->access_chain;

   return vtn_access_chain_to_deref(b, chain);
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

struct vtn_ssa_value *
vtn_local_load(struct vtn_builder *b, nir_deref_var *src)
{
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

void
vtn_local_store(struct vtn_builder *b, struct vtn_ssa_value *src,
                nir_deref_var *dest)
{
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

static nir_ssa_def *
get_vulkan_resource_index(struct vtn_builder *b, struct vtn_access_chain *chain,
                          struct vtn_type **type, unsigned *chain_idx)
{
   assert(chain->var->interface_type && "variable is a block");

   nir_ssa_def *array_index;
   if (glsl_type_is_array(chain->var->type)) {
      assert(chain->length > 0);
      array_index = vtn_ssa_value(b, chain->ids[0])->def;
      *chain_idx = 1;
      *type = chain->var_type->array_element;
   } else {
      array_index = nir_imm_int(&b->nb, 0);
      *chain_idx = 0;
      *type = chain->var_type;
   }

   nir_intrinsic_instr *instr =
      nir_intrinsic_instr_create(b->nb.shader,
                                 nir_intrinsic_vulkan_resource_index);
   instr->src[0] = nir_src_for_ssa(array_index);
   instr->const_index[0] = chain->var->data.descriptor_set;
   instr->const_index[1] = chain->var->data.binding;
   instr->const_index[2] = chain->var->data.mode;

   nir_ssa_dest_init(&instr->instr, &instr->dest, 1, NULL);
   nir_builder_instr_insert(&b->nb, &instr->instr);

   return &instr->dest.ssa;
}

static bool
variable_is_external_block(nir_variable *var)
{
   return var->interface_type &&
          glsl_type_is_struct(var->interface_type) &&
          (var->data.mode == nir_var_uniform ||
           var->data.mode == nir_var_shader_storage);
}

nir_ssa_def *
vtn_access_chain_to_offset(struct vtn_builder *b,
                           struct vtn_access_chain *chain,
                           nir_ssa_def **index_out, struct vtn_type **type_out,
                           unsigned *end_idx_out, bool stop_at_matrix)
{
   unsigned idx = 0;
   struct vtn_type *type;
   *index_out = get_vulkan_resource_index(b, chain, &type, &idx);

   nir_ssa_def *offset = nir_imm_int(&b->nb, 0);
   for (; idx < chain->length; idx++) {
      enum glsl_base_type base_type = glsl_get_base_type(type->type);
      switch (base_type) {
      case GLSL_TYPE_UINT:
      case GLSL_TYPE_INT:
      case GLSL_TYPE_FLOAT:
      case GLSL_TYPE_DOUBLE:
      case GLSL_TYPE_BOOL:
         /* Some users may not want matrix or vector derefs */
         if (stop_at_matrix) {
            idx++;
            goto end;
         }
         /* Fall through */

      case GLSL_TYPE_ARRAY:
         offset = nir_iadd(&b->nb, offset,
                           nir_imul(&b->nb,
                                    vtn_ssa_value(b, chain->ids[idx])->def,
                                    nir_imm_int(&b->nb, type->stride)));

         if (glsl_type_is_vector(type->type)) {
            /* This had better be the tail */
            assert(idx == chain->length - 1);
            type = rzalloc(b, struct vtn_type);
            type->type = glsl_scalar_type(base_type);
         } else {
            type = type->array_element;
         }
         break;

      case GLSL_TYPE_STRUCT: {
         struct vtn_value *member_val =
            vtn_value(b, chain->ids[idx], vtn_value_type_constant);
         unsigned member = member_val->constant->value.u[0];

         offset = nir_iadd(&b->nb, offset,
                           nir_imm_int(&b->nb, type->offsets[member]));
         type = type->members[member];
         break;
      }

      default:
         unreachable("Invalid type for deref");
      }
   }

end:
   *type_out = type;
   if (end_idx_out)
      *end_idx_out = idx;

   return offset;
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

   /* We set the base and size for push constant load to the entire push
    * constant block for now.
    */
   if (op == nir_intrinsic_load_push_constant) {
      instr->const_index[0] = 0;
      instr->const_index[1] = 128;
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
                      nir_ssa_def *index, nir_ssa_def *offset,
                      struct vtn_access_chain *chain, unsigned chain_idx,
                      struct vtn_type *type, struct vtn_ssa_value **inout)
{
   if (chain_idx >= chain->length)
      chain = NULL;

   if (load && chain == NULL && *inout == NULL)
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
         if (chain == NULL) {
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
         } else if (type->row_major) {
            /* Row-major but with an access chiain. */
            nir_ssa_def *col_offset =
               nir_imul(&b->nb, vtn_ssa_value(b, chain->ids[chain_idx])->def,
                        nir_imm_int(&b->nb, type->array_element->stride));
            offset = nir_iadd(&b->nb, offset, col_offset);

            if (chain_idx + 1 < chain->length) {
               /* Picking off a single element */
               nir_ssa_def *row_offset =
                  nir_imul(&b->nb,
                           vtn_ssa_value(b, chain->ids[chain_idx + 1])->def,
                           nir_imm_int(&b->nb, type->stride));
               offset = nir_iadd(&b->nb, offset, row_offset);
               _vtn_load_store_tail(b, op, load, index, offset, inout,
                                    glsl_scalar_type(base_type));
            } else {
               /* Picking one element off each column */
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
            }
         } else {
            /* Column-major with a deref. Fall through to array case. */
            nir_ssa_def *col_offset =
               nir_imul(&b->nb, vtn_ssa_value(b, chain->ids[chain_idx])->def,
                        nir_imm_int(&b->nb, type->stride));
            offset = nir_iadd(&b->nb, offset, col_offset);

            _vtn_block_load_store(b, op, load, index, offset,
                                  chain, chain_idx + 1,
                                  type->array_element, inout);
         }
      } else if (chain == NULL) {
         /* Single whole vector */
         assert(glsl_type_is_vector_or_scalar(type->type));
         _vtn_load_store_tail(b, op, load, index, offset, inout, type->type);
      } else {
         /* Single component of a vector. Fall through to array case. */
         nir_ssa_def *elem_offset =
            nir_imul(&b->nb, vtn_ssa_value(b, chain->ids[chain_idx])->def,
                     nir_imm_int(&b->nb, type->stride));
         offset = nir_iadd(&b->nb, offset, elem_offset);

         _vtn_block_load_store(b, op, load, index, offset, NULL, 0,
                               type->array_element, inout);
      }
      return;

   case GLSL_TYPE_ARRAY: {
      unsigned elems = glsl_get_length(type->type);
      for (unsigned i = 0; i < elems; i++) {
         nir_ssa_def *elem_off =
            nir_iadd(&b->nb, offset, nir_imm_int(&b->nb, i * type->stride));
         _vtn_block_load_store(b, op, load, index, elem_off, NULL, 0,
                               type->array_element, &(*inout)->elems[i]);
      }
      return;
   }

   case GLSL_TYPE_STRUCT: {
      unsigned elems = glsl_get_length(type->type);
      for (unsigned i = 0; i < elems; i++) {
         nir_ssa_def *elem_off =
            nir_iadd(&b->nb, offset, nir_imm_int(&b->nb, type->offsets[i]));
         _vtn_block_load_store(b, op, load, index, elem_off, NULL, 0,
                               type->members[i], &(*inout)->elems[i]);
      }
      return;
   }

   default:
      unreachable("Invalid block member type");
   }
}

static struct vtn_ssa_value *
vtn_block_load(struct vtn_builder *b, struct vtn_access_chain *src)
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

   nir_ssa_def *offset, *index = NULL;
   struct vtn_type *type;
   unsigned chain_idx;
   offset = vtn_access_chain_to_offset(b, src, &index, &type, &chain_idx, true);

   if (op == nir_intrinsic_load_push_constant)
      index = NULL;

   struct vtn_ssa_value *value = NULL;
   _vtn_block_load_store(b, op, true, index, offset,
                         src, chain_idx, type, &value);
   return value;
}

static void
vtn_block_store(struct vtn_builder *b, struct vtn_ssa_value *src,
                struct vtn_access_chain *dst)
{
   nir_ssa_def *offset, *index = NULL;
   struct vtn_type *type;
   unsigned chain_idx;
   offset = vtn_access_chain_to_offset(b, dst, &index, &type, &chain_idx, true);

   _vtn_block_load_store(b, nir_intrinsic_store_ssbo, false, index, offset,
                         dst, chain_idx, type, &src);
}

struct vtn_ssa_value *
vtn_variable_load(struct vtn_builder *b, struct vtn_access_chain *src)
{
   if (variable_is_external_block(src->var))
      return vtn_block_load(b, src);
   else
      return vtn_local_load(b, vtn_access_chain_to_deref(b, src));
}

void
vtn_variable_store(struct vtn_builder *b, struct vtn_ssa_value *src,
                   struct vtn_access_chain *dest)
{
   if (variable_is_external_block(dest->var)) {
      assert(dest->var->data.mode == nir_var_shader_storage);
      vtn_block_store(b, src, dest);
   } else {
      vtn_local_store(b, src, vtn_access_chain_to_deref(b, dest));
   }
}

static void
vtn_variable_copy(struct vtn_builder *b, struct vtn_access_chain *dest,
                  struct vtn_access_chain *src)
{
   if (src->var->interface_type || dest->var->interface_type) {
      struct vtn_ssa_value *val = vtn_variable_load(b, src);
      vtn_variable_store(b, val, dest);
   } else {
      /* TODO: Handle single components of vectors */
      nir_deref_var *src_deref = vtn_access_chain_to_deref(b, src);
      nir_deref_var *dest_deref = vtn_access_chain_to_deref(b, dest);

      nir_intrinsic_instr *copy =
         nir_intrinsic_instr_create(b->shader, nir_intrinsic_copy_var);
      copy->variables[0] =
         nir_deref_as_var(nir_copy_deref(copy, &dest_deref->deref));
      copy->variables[1] =
         nir_deref_as_var(nir_copy_deref(copy, &src_deref->deref));

      nir_builder_instr_insert(&b->nb, &copy->instr);
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
   case SpvBuiltInVertexIndex:
      *location = SYSTEM_VALUE_VERTEX_ID;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInVertexId:
      /* Vulkan defines VertexID to be zero-based and reserves the new
       * builtin keyword VertexIndex to indicate the non-zero-based value.
       */
      *location = SYSTEM_VALUE_VERTEX_ID_ZERO_BASE;
      set_mode_system_value(mode);
      break;
   case SpvBuiltInInstanceIndex:
      /* XXX */
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
   assert(val->value_type == vtn_value_type_access_chain);
   assert(val->access_chain->length == 0);
   assert(val->access_chain->var == void_var);

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

void
vtn_handle_variables(struct vtn_builder *b, SpvOp opcode,
                     const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpVariable: {
      struct vtn_type *type =
         vtn_value(b, w[1], vtn_value_type_type)->type;
      struct vtn_value *val =
         vtn_push_value(b, w[2], vtn_value_type_access_chain);
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

                  switch (interface_type->access_qualifier) {
                  case SpvAccessQualifierReadOnly:
                     var->data.image.read_only = true;
                     break;
                  case SpvAccessQualifierWriteOnly:
                     var->data.image.write_only = true;
                     break;
                  default:
                     break;
                  }
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
         var->data.mode = nir_var_shared;
         break;
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

      val->access_chain = ralloc(b, struct vtn_access_chain);
      val->access_chain->var = var;
      val->access_chain->var_type = type;
      val->access_chain->length = 0;

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
      struct vtn_access_chain *base, *chain;
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
         assert(base_val->value_type == vtn_value_type_access_chain);
         base = base_val->access_chain;
      }

      uint32_t new_len = base->length + count - 4;
      chain = ralloc_size(b, sizeof(*chain) + new_len * sizeof(chain->ids[0]));

      *chain = *base;

      chain->length = new_len;
      unsigned idx = 0;
      for (int i = 0; i < base->length; i++)
         chain->ids[idx++] = base->ids[i];

      for (int i = 4; i < count; i++)
         chain->ids[idx++] = w[i];

      if (base_val->value_type == vtn_value_type_sampled_image) {
         struct vtn_value *val =
            vtn_push_value(b, w[2], vtn_value_type_sampled_image);
         val->sampled_image = ralloc(b, struct vtn_sampled_image);
         val->sampled_image->image = chain;
         val->sampled_image->sampler = base_val->sampled_image->sampler;
      } else {
         struct vtn_value *val =
            vtn_push_value(b, w[2], vtn_value_type_access_chain);
         val->access_chain = chain;
      }
      break;
   }

   case SpvOpCopyMemory: {
      struct vtn_value *dest = vtn_value(b, w[1], vtn_value_type_access_chain);
      struct vtn_value *src = vtn_value(b, w[2], vtn_value_type_access_chain);

      vtn_variable_copy(b, dest->access_chain, src->access_chain);
      break;
   }

   case SpvOpLoad: {
      struct vtn_access_chain *src =
         vtn_value(b, w[3], vtn_value_type_access_chain)->access_chain;

      if (src->var->interface_type &&
          (glsl_type_is_sampler(src->var->interface_type) ||
           glsl_type_is_image(src->var->interface_type))) {
         vtn_push_value(b, w[2], vtn_value_type_access_chain)->access_chain = src;
         return;
      }

      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
      val->ssa = vtn_variable_load(b, src);
      break;
   }

   case SpvOpStore: {
      struct vtn_access_chain *dest =
         vtn_value(b, w[1], vtn_value_type_access_chain)->access_chain;
      struct vtn_ssa_value *src = vtn_ssa_value(b, w[2]);
      vtn_variable_store(b, src, dest);
      break;
   }

   case SpvOpArrayLength: {
      struct vtn_access_chain *chain =
         vtn_value(b, w[3], vtn_value_type_access_chain)->access_chain;

      const uint32_t offset = chain->var_type->offsets[w[4]];
      const uint32_t stride = chain->var_type->members[w[4]]->stride;

      unsigned chain_idx;
      struct vtn_type *type;
      nir_ssa_def *index =
         get_vulkan_resource_index(b, chain, &type, &chain_idx);

      nir_intrinsic_instr *instr =
         nir_intrinsic_instr_create(b->nb.shader,
                                    nir_intrinsic_get_buffer_size);
      instr->src[0] = nir_src_for_ssa(index);
      nir_ssa_dest_init(&instr->instr, &instr->dest, 1, NULL);
      nir_builder_instr_insert(&b->nb, &instr->instr);
      nir_ssa_def *buf_size = &instr->dest.ssa;

      /* array_length = max(buffer_size - offset, 0) / stride */
      nir_ssa_def *array_length =
         nir_idiv(&b->nb,
                  nir_imax(&b->nb,
                           nir_isub(&b->nb,
                                    buf_size,
                                    nir_imm_int(&b->nb, offset)),
                           nir_imm_int(&b->nb, 0u)),
                  nir_imm_int(&b->nb, stride));

      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
      val->ssa = vtn_create_ssa_value(b, glsl_uint_type());
      val->ssa->def = array_length;
      break;
   }

   case SpvOpCopyMemorySized:
   default:
      unreachable("Unhandled opcode");
   }
}
