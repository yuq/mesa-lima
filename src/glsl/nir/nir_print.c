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

#include "nir.h"
#include <stdio.h>
#include <stdlib.h>

static void
print_tabs(unsigned num_tabs, FILE *fp)
{
   for (unsigned i = 0; i < num_tabs; i++)
      fprintf(fp, "\t");
}

typedef struct {
   /** map from nir_variable -> printable name */
   struct hash_table *ht;

   /** set of names used so far for nir_variables */
   struct set *syms;

   /* an index used to make new non-conflicting names */
   unsigned index;
} print_var_state;

static void
print_register(nir_register *reg, FILE *fp)
{
   if (reg->name != NULL)
      fprintf(fp, "/* %s */ ", reg->name);
   if (reg->is_global)
      fprintf(fp, "gr%u", reg->index);
   else
      fprintf(fp, "r%u", reg->index);
}

static const char *sizes[] = { "error", "vec1", "vec2", "vec3", "vec4" };

static void
print_register_decl(nir_register *reg, FILE *fp)
{
   fprintf(fp, "decl_reg %s ", sizes[reg->num_components]);
   if (reg->is_packed)
      fprintf(fp, "(packed) ");
   print_register(reg, fp);
   if (reg->num_array_elems != 0)
      fprintf(fp, "[%u]", reg->num_array_elems);
   fprintf(fp, "\n");
}

static void
print_ssa_def(nir_ssa_def *def, FILE *fp)
{
   if (def->name != NULL)
      fprintf(fp, "/* %s */ ", def->name);
   fprintf(fp, "%s ssa_%u", sizes[def->num_components], def->index);
}

static void
print_ssa_use(nir_ssa_def *def, FILE *fp)
{
   if (def->name != NULL)
      fprintf(fp, "/* %s */ ", def->name);
   fprintf(fp, "ssa_%u", def->index);
}

static void print_src(nir_src *src, FILE *fp);

static void
print_reg_src(nir_reg_src *src, FILE *fp)
{
   print_register(src->reg, fp);
   if (src->reg->num_array_elems != 0) {
      fprintf(fp, "[%u", src->base_offset);
      if (src->indirect != NULL) {
         fprintf(fp, " + ");
         print_src(src->indirect, fp);
      }
      fprintf(fp, "]");
   }
}

static void
print_reg_dest(nir_reg_dest *dest, FILE *fp)
{
   print_register(dest->reg, fp);
   if (dest->reg->num_array_elems != 0) {
      fprintf(fp, "[%u", dest->base_offset);
      if (dest->indirect != NULL) {
         fprintf(fp, " + ");
         print_src(dest->indirect, fp);
      }
      fprintf(fp, "]");
   }
}

static void
print_src(nir_src *src, FILE *fp)
{
   if (src->is_ssa)
      print_ssa_use(src->ssa, fp);
   else
      print_reg_src(&src->reg, fp);
}

static void
print_dest(nir_dest *dest, FILE *fp)
{
   if (dest->is_ssa)
      print_ssa_def(&dest->ssa, fp);
   else
      print_reg_dest(&dest->reg, fp);
}

static void
print_alu_src(nir_alu_src *src, FILE *fp)
{
   if (src->negate)
      fprintf(fp, "-");
   if (src->abs)
      fprintf(fp, "abs(");

   print_src(&src->src, fp);

   if (src->swizzle[0] != 0 ||
       src->swizzle[1] != 1 ||
       src->swizzle[2] != 2 ||
       src->swizzle[3] != 3) {
      fprintf(fp, ".");
      for (unsigned i = 0; i < 4; i++)
         fprintf(fp, "%c", "xyzw"[src->swizzle[i]]);
   }

   if (src->abs)
      fprintf(fp, ")");
}

static void
print_alu_dest(nir_alu_dest *dest, FILE *fp)
{
   /* we're going to print the saturate modifier later, after the opcode */

   print_dest(&dest->dest, fp);

   if (!dest->dest.is_ssa &&
       dest->write_mask != (1 << dest->dest.reg.reg->num_components) - 1) {
      fprintf(fp, ".");
      for (unsigned i = 0; i < 4; i++)
         if ((dest->write_mask >> i) & 1)
            fprintf(fp, "%c", "xyzw"[i]);
   }
}

static void
print_alu_instr(nir_alu_instr *instr, FILE *fp)
{
   if (instr->has_predicate) {
      fprintf(fp, "(");
      print_src(&instr->predicate, fp);
      fprintf(fp, ") ");
   }

   print_alu_dest(&instr->dest, fp);

   fprintf(fp, " = %s", nir_op_infos[instr->op].name);
   if (instr->dest.saturate)
      fprintf(fp, ".sat");
   fprintf(fp, " ");

   bool first = true;
   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
      if (!first)
         fprintf(fp, ", ");

      print_alu_src(&instr->src[i], fp);

      first = false;
   }
}

static void
print_var_decl(nir_variable *var, print_var_state *state, FILE *fp)
{
   fprintf(fp, "decl_var ");

   const char *const cent = (var->data.centroid) ? "centroid " : "";
   const char *const samp = (var->data.sample) ? "sample " : "";
   const char *const inv = (var->data.invariant) ? "invariant " : "";
   const char *const mode[] = { "shader_in ", "shader_out ", "", "",
                                "uniform ", "system " };
   const char *const interp[] = { "", "smooth", "flat", "noperspective" };

   fprintf(fp, "%s%s%s%s%s ",
      cent, samp, inv, mode[var->data.mode], interp[var->data.interpolation]);

   glsl_print_type(var->type, fp);

   struct set_entry *entry =
      _mesa_set_search(state->syms, _mesa_hash_string(var->name), var->name);

   char *name;

   if (entry != NULL) {
      /* we have a collision with another name, append an @ + a unique index */
      name = ralloc_asprintf(state->syms, "%s@%u", var->name, state->index++);
   } else {
      name = var->name;
   }

   fprintf(fp, " %s", name);

   if (var->data.mode == nir_var_shader_in ||
       var->data.mode == nir_var_shader_out ||
       var->data.mode == nir_var_uniform) {
      fprintf(fp, " (%u)", var->data.driver_location);
   }

   fprintf(fp, "\n");

   _mesa_set_add(state->syms, _mesa_hash_string(name), name);
   _mesa_hash_table_insert(state->ht, var, name);
}

static void
print_var(nir_variable *var, print_var_state *state, FILE *fp)
{
   struct hash_entry *entry = _mesa_hash_table_search(state->ht, var);

   assert(entry != NULL);

   fprintf(fp, "%s", (char *) entry->data);
}

static void
print_deref_var(nir_deref_var *deref, print_var_state *state, FILE *fp)
{
   print_var(deref->var, state, fp);
}

static void
print_deref_array(nir_deref_array *deref, print_var_state *state, FILE *fp)
{
   fprintf(fp, "[");
   switch (deref->deref_array_type) {
   case nir_deref_array_type_direct:
      fprintf(fp, "%u", deref->base_offset);
      break;
   case nir_deref_array_type_indirect:
      if (deref->base_offset != 0)
         fprintf(fp, "%u + ", deref->base_offset);
      print_src(&deref->indirect, fp);
      break;
   }
   fprintf(fp, "]");
}

static void
print_deref_struct(nir_deref_struct *deref, print_var_state *state, FILE *fp)
{
   fprintf(fp, ".%s", deref->elem);
}

static void
print_deref(nir_deref *deref, print_var_state *state, FILE *fp)
{
   while (deref != NULL) {
      switch (deref->deref_type) {
      case nir_deref_type_var:
         print_deref_var(nir_deref_as_var(deref), state, fp);
         break;

      case nir_deref_type_array:
         print_deref_array(nir_deref_as_array(deref), state, fp);
         break;

      case nir_deref_type_struct:
         print_deref_struct(nir_deref_as_struct(deref), state, fp);
         break;

      default:
         unreachable("Invalid deref type");
      }

      deref = deref->child;
   }
}

static void
print_intrinsic_instr(nir_intrinsic_instr *instr, print_var_state *state,
                      FILE *fp)
{
   unsigned num_srcs = nir_intrinsic_infos[instr->intrinsic].num_srcs;

   if (instr->has_predicate) {
      fprintf(fp, "(");
      print_src(&instr->predicate, fp);
      fprintf(fp, ") ");
   }

   if (nir_intrinsic_infos[instr->intrinsic].has_dest) {
      print_dest(&instr->dest, fp);
      fprintf(fp, " = ");
   }

   fprintf(fp, "intrinsic %s (", nir_intrinsic_infos[instr->intrinsic].name);

   bool first = true;
   for (unsigned i = 0; i < num_srcs; i++) {
      if (!first)
         fprintf(fp, ", ");

      print_src(&instr->src[i], fp);

      first = false;
   }

   fprintf(fp, ") (");

   unsigned num_vars = nir_intrinsic_infos[instr->intrinsic].num_variables;

   first = true;
   for (unsigned i = 0; i < num_vars; i++) {
      if (!first)
         fprintf(fp, ", ");

      print_deref(&instr->variables[i]->deref, state, fp);

      first = false;
   }

   fprintf(fp, ") (");

   unsigned num_indices = nir_intrinsic_infos[instr->intrinsic].num_indices;

   first = true;
   for (unsigned i = 0; i < num_indices; i++) {
      if (!first)
         fprintf(fp, ", ");

      fprintf(fp, "%u", instr->const_index[i]);

      first = false;
   }

   fprintf(fp, ")");
}

static void
print_tex_instr(nir_tex_instr *instr, print_var_state *state, FILE *fp)
{
   if (instr->has_predicate) {
      fprintf(fp, "(");
      print_src(&instr->predicate, fp);
      fprintf(fp, ") ");
   }

   print_dest(&instr->dest, fp);

   fprintf(fp, " = ");

   switch (instr->op) {
   case nir_texop_tex:
      fprintf(fp, "tex ");
      break;
   case nir_texop_txb:
      fprintf(fp, "txb ");
      break;
   case nir_texop_txl:
      fprintf(fp, "txl ");
      break;
   case nir_texop_txd:
      fprintf(fp, "txd ");
      break;
   case nir_texop_txf:
      fprintf(fp, "txf ");
      break;
   case nir_texop_txf_ms:
      fprintf(fp, "txf_ms ");
      break;
   case nir_texop_txs:
      fprintf(fp, "txs ");
      break;
   case nir_texop_lod:
      fprintf(fp, "lod ");
      break;
   case nir_texop_tg4:
      fprintf(fp, "tg4 ");
      break;
   case nir_texop_query_levels:
      fprintf(fp, "query_levels ");
      break;

   default:
      unreachable("Invalid texture operation");
      break;
   }

   for (unsigned i = 0; i < instr->num_srcs; i++) {
      print_src(&instr->src[i], fp);

      fprintf(fp, " ");

      switch(instr->src_type[i]) {
      case nir_tex_src_coord:
         fprintf(fp, "(coord)");
         break;
      case nir_tex_src_projector:
         fprintf(fp, "(projector)");
         break;
      case nir_tex_src_comparitor:
         fprintf(fp, "(comparitor)");
         break;
      case nir_tex_src_offset:
         fprintf(fp, "(offset)");
         break;
      case nir_tex_src_bias:
         fprintf(fp, "(bias)");
         break;
      case nir_tex_src_lod:
         fprintf(fp, "(lod)");
         break;
      case nir_tex_src_ms_index:
         fprintf(fp, "(ms_index)");
         break;
      case nir_tex_src_ddx:
         fprintf(fp, "(ddx)");
         break;
      case nir_tex_src_ddy:
         fprintf(fp, "(ddy)");
         break;
      case nir_tex_src_sampler_index:
         fprintf(fp, "(sampler_index)");
         break;

      default:
         unreachable("Invalid texture source type");
         break;
      }

      fprintf(fp, ", ");
   }

   bool offset_nonzero = false;
   for (unsigned i = 0; i < 4; i++)
      if (instr->const_offset[i] != 0) {
         offset_nonzero = true;
         break;
      }

   if (offset_nonzero) {
      fprintf(fp, "[%i %i %i %i] (offset), ",
              instr->const_offset[0], instr->const_offset[1],
              instr->const_offset[2], instr->const_offset[3]);
   }

   if (instr->op == nir_texop_tg4) {
      fprintf(fp, "%u (gather_component), ", instr->component);
   }

   if (instr->sampler) {
      print_deref(&instr->sampler->deref, state, fp);
   } else {
      fprintf(fp, "%u", instr->sampler_index);
   }

   fprintf(fp, " (sampler)");
}

static void
print_call_instr(nir_call_instr *instr, print_var_state *state, FILE *fp)
{
   if (instr->has_predicate) {
      fprintf(fp, "(");
      print_src(&instr->predicate, fp);
      fprintf(fp, ") ");
   }

   fprintf(fp, "call %s ", instr->callee->function->name);

   for (unsigned i = 0; i < instr->num_params; i++) {
      if (i != 0)
         fprintf(fp, ", ");

      print_deref(&instr->params[i]->deref, state, fp);
   }

   if (instr->return_deref != NULL) {
      if (instr->num_params != 0)
         fprintf(fp, ", ");
      fprintf(fp, "returning ");
      print_deref(&instr->return_deref->deref, state, fp);
   }
}

static void
print_const_value(nir_const_value value, unsigned num_components, FILE *fp)
{
   fprintf(fp, "(");

   bool first = true;
   for (unsigned i = 0; i < num_components; i++) {
      if (!first)
         fprintf(fp, ", ");

      /*
       * we don't really know the type of the constant (if it will be used as a
       * float or an int), so just print the raw constant in hex for fidelity
       * and then print the float in a comment for readability.
       */

      fprintf(fp, "0x%08x /* %f */", value.u[i], value.f[i]);

      first = false;
   }

   fprintf(fp, ")");
}

static void
print_load_const_instr(nir_load_const_instr *instr, unsigned tabs, FILE *fp)
{
   if (instr->has_predicate) {
      fprintf(fp, "(");
      print_src(&instr->predicate, fp);
      fprintf(fp, ") ");
   }

   print_dest(&instr->dest, fp);

   fprintf(fp, " = load_const ");

   if (instr->array_elems == 0) {
      print_const_value(instr->value, instr->num_components, fp);
   } else {
      fprintf(fp, "{\n");
      for (unsigned i = 0; i < instr->array_elems; i++) {
         print_tabs(tabs + 1, fp);
         print_const_value(instr->array[i], instr->num_components, fp);
         fprintf(fp, ", \n");
      }
      fprintf(fp, "}");
   }
}

static void
print_jump_instr(nir_jump_instr *instr, FILE *fp)
{
   switch (instr->type) {
   case nir_jump_break:
      fprintf(fp, "break");
      break;

   case nir_jump_continue:
      fprintf(fp, "continue");
      break;

   case nir_jump_return:
      fprintf(fp, "return");
      break;
   }
}

static void
print_ssa_undef_instr(nir_ssa_undef_instr* instr, FILE *fp)
{
   print_ssa_def(&instr->def, fp);
   fprintf(fp, " = undefined");
}

static void
print_phi_instr(nir_phi_instr *instr, FILE *fp)
{
   print_dest(&instr->dest, fp);
   fprintf(fp, " = phi ");
   bool first = true;
   foreach_list_typed(nir_phi_src, src, node, &instr->srcs) {
      if (!first)
         fprintf(fp, ", ");

      fprintf(fp, "block_%u: ", src->pred->index);
      print_src(&src->src, fp);

      first = false;
   }
}

static void
print_parallel_copy_instr(nir_parallel_copy_instr *instr, FILE *fp)
{
   bool first = true;
   fprintf(fp, "pcopy: ");
   foreach_list_typed(nir_parallel_copy_copy, copy, node, &instr->copies) {
      if (!first)
         fprintf(fp, "; ");

      print_dest(&copy->dest, fp);
      fprintf(fp, " = ");
      print_src(&copy->src, fp);

      first = false;
   }
}

static void
print_instr(nir_instr *instr, print_var_state *state, unsigned tabs, FILE *fp)
{
   print_tabs(tabs, fp);

   switch (instr->type) {
   case nir_instr_type_alu:
      print_alu_instr(nir_instr_as_alu(instr), fp);
      break;

   case nir_instr_type_call:
      print_call_instr(nir_instr_as_call(instr), state, fp);
      break;

   case nir_instr_type_intrinsic:
      print_intrinsic_instr(nir_instr_as_intrinsic(instr), state, fp);
      break;

   case nir_instr_type_texture:
      print_tex_instr(nir_instr_as_texture(instr), state, fp);
      break;

   case nir_instr_type_load_const:
      print_load_const_instr(nir_instr_as_load_const(instr), tabs, fp);
      break;

   case nir_instr_type_jump:
      print_jump_instr(nir_instr_as_jump(instr), fp);
      break;

   case nir_instr_type_ssa_undef:
      print_ssa_undef_instr(nir_instr_as_ssa_undef(instr), fp);
      break;

   case nir_instr_type_phi:
      print_phi_instr(nir_instr_as_phi(instr), fp);
      break;

   case nir_instr_type_parallel_copy:
      print_parallel_copy_instr(nir_instr_as_parallel_copy(instr), fp);
      break;

   default:
      unreachable("Invalid instruction type");
      break;
   }

   fprintf(fp, "\n");
}

static int
compare_block_index(const void *p1, const void *p2)
{
   const nir_block *block1 = *((const nir_block **) p1);
   const nir_block *block2 = *((const nir_block **) p2);

   return (int) block1->index - (int) block2->index;
}

static void print_cf_node(nir_cf_node *node, print_var_state *state,
                          unsigned tabs, FILE *fp);

static void
print_block(nir_block *block, print_var_state *state, unsigned tabs, FILE *fp)
{
   print_tabs(tabs, fp);
   fprintf(fp, "block block_%u:\n", block->index);

   /* sort the predecessors by index so we consistently print the same thing */

   nir_block **preds =
      malloc(block->predecessors->entries * sizeof(nir_block *));

   struct set_entry *entry;
   unsigned i = 0;
   set_foreach(block->predecessors, entry) {
      preds[i++] = (nir_block *) entry->key;
   }

   qsort(preds, block->predecessors->entries, sizeof(nir_block *),
         compare_block_index);

   print_tabs(tabs, fp);
   fprintf(fp, "/* preds: ");
   for (unsigned i = 0; i < block->predecessors->entries; i++) {
      fprintf(fp, "block_%u ", preds[i]->index);
   }
   fprintf(fp, "*/\n");

   free(preds);

   nir_foreach_instr(block, instr) {
      print_instr(instr, state, tabs, fp);
   }

   print_tabs(tabs, fp);
   fprintf(fp, "/* succs: ");
   for (unsigned i = 0; i < 2; i++)
      if (block->successors[i]) {
         fprintf(fp, "block_%u ", block->successors[i]->index);
      }
   fprintf(fp, "*/\n");
}

static void
print_if(nir_if *if_stmt, print_var_state *state, unsigned tabs, FILE *fp)
{
   print_tabs(tabs, fp);
   fprintf(fp, "if ");
   print_src(&if_stmt->condition, fp);
   fprintf(fp, " {\n");
   foreach_list_typed(nir_cf_node, node, node, &if_stmt->then_list) {
      print_cf_node(node, state, tabs + 1, fp);
   }
   print_tabs(tabs, fp);
   fprintf(fp, "} else {\n");
   foreach_list_typed(nir_cf_node, node, node, &if_stmt->else_list) {
      print_cf_node(node, state, tabs + 1, fp);
   }
   print_tabs(tabs, fp);
   fprintf(fp, "}\n");
}

static void
print_loop(nir_loop *loop, print_var_state *state, unsigned tabs, FILE *fp)
{
   print_tabs(tabs, fp);
   fprintf(fp, "loop {\n");
   foreach_list_typed(nir_cf_node, node, node, &loop->body) {
      print_cf_node(node, state, tabs + 1, fp);
   }
   print_tabs(tabs, fp);
   fprintf(fp, "}\n");
}

static void
print_cf_node(nir_cf_node *node, print_var_state *state, unsigned int tabs,
              FILE *fp)
{
   switch (node->type) {
   case nir_cf_node_block:
      print_block(nir_cf_node_as_block(node), state, tabs, fp);
      break;

   case nir_cf_node_if:
      print_if(nir_cf_node_as_if(node), state, tabs, fp);
      break;

   case nir_cf_node_loop:
      print_loop(nir_cf_node_as_loop(node), state, tabs, fp);
      break;

   default:
      unreachable("Invalid CFG node type");
   }
}

static void
print_function_impl(nir_function_impl *impl, print_var_state *state, FILE *fp)
{
   fprintf(fp, "\nimpl %s ", impl->overload->function->name);

   for (unsigned i = 0; i < impl->num_params; i++) {
      if (i != 0)
         fprintf(fp, ", ");

      print_var(impl->params[i], state, fp);
   }

   if (impl->return_var != NULL) {
      if (impl->num_params != 0)
         fprintf(fp, ", ");
      fprintf(fp, "returning ");
      print_var(impl->return_var, state, fp);
   }

   fprintf(fp, "{\n");

   foreach_list_typed(nir_variable, var, node, &impl->locals) {
      fprintf(fp, "\t");
      print_var_decl(var, state, fp);
   }

   foreach_list_typed(nir_register, reg, node, &impl->registers) {
      fprintf(fp, "\t");
      print_register_decl(reg, fp);
   }

   nir_index_blocks(impl);

   foreach_list_typed(nir_cf_node, node, node, &impl->body) {
      print_cf_node(node, state, 1, fp);
   }

   fprintf(fp, "\tblock block_%u:\n}\n\n", impl->end_block->index);
}

static void
print_function_overload(nir_function_overload *overload,
                        print_var_state *state, FILE *fp)
{
   fprintf(fp, "decl_overload %s ", overload->function->name);

   for (unsigned i = 0; i < overload->num_params; i++) {
      if (i != 0)
         fprintf(fp, ", ");

      switch (overload->params[i].param_type) {
      case nir_parameter_in:
         fprintf(fp, "in ");
         break;
      case nir_parameter_out:
         fprintf(fp, "out ");
         break;
      case nir_parameter_inout:
         fprintf(fp, "inout ");
         break;
      default:
         unreachable("Invalid parameter type");
      }

      glsl_print_type(overload->params[i].type, fp);
   }

   if (overload->return_type != NULL) {
      if (overload->num_params != 0)
         fprintf(fp, ", ");
      fprintf(fp, "returning ");
      glsl_print_type(overload->return_type, fp);
   }

   fprintf(fp, "\n");

   if (overload->impl != NULL) {
      print_function_impl(overload->impl, state, fp);
      return;
   }
}

static void
print_function(nir_function *func, print_var_state *state, FILE *fp)
{
   foreach_list_typed(nir_function_overload, overload, node, &func->overload_list) {
      print_function_overload(overload, state, fp);
   }
}

static void
init_print_state(print_var_state *state)
{
   state->ht = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                                       _mesa_key_pointer_equal);
   state->syms = _mesa_set_create(NULL, _mesa_key_string_equal);
   state->index = 0;
}

static void
destroy_print_state(print_var_state *state)
{
   _mesa_hash_table_destroy(state->ht, NULL);
   _mesa_set_destroy(state->syms, NULL);
}

void
nir_print_shader(nir_shader *shader, FILE *fp)
{
   print_var_state state;
   init_print_state(&state);

   for (unsigned i = 0; i < shader->num_user_structures; i++) {
      glsl_print_struct(shader->user_structures[i], fp);
   }

   struct hash_entry *entry;

   hash_table_foreach(shader->uniforms, entry) {
      print_var_decl((nir_variable *) entry->data, &state, fp);
   }

   hash_table_foreach(shader->inputs, entry) {
      print_var_decl((nir_variable *) entry->data, &state, fp);
   }

   hash_table_foreach(shader->outputs, entry) {
      print_var_decl((nir_variable *) entry->data, &state, fp);
   }

   foreach_list_typed(nir_variable, var, node, &shader->globals) {
      print_var_decl(var, &state, fp);
   }

   foreach_list_typed(nir_variable, var, node, &shader->system_values) {
      print_var_decl(var, &state, fp);
   }

   foreach_list_typed(nir_register, reg, node, &shader->registers) {
      print_register_decl(reg, fp);
   }

   foreach_list_typed(nir_function, func, node, &shader->functions) {
      print_function(func, &state, fp);
   }

   destroy_print_state(&state);
}
