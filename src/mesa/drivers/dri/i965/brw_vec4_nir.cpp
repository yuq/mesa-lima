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
 */

#include "brw_nir.h"
#include "brw_vec4.h"
#include "glsl/ir_uniform.h"

namespace brw {

void
vec4_visitor::emit_nir_code()
{
   nir_shader *nir = prog->nir;

   if (nir->num_inputs > 0)
      nir_setup_inputs(nir);

   if (nir->num_uniforms > 0)
      nir_setup_uniforms(nir);

   nir_setup_system_values(nir);

   /* get the main function and emit it */
   nir_foreach_overload(nir, overload) {
      assert(strcmp(overload->function->name, "main") == 0);
      assert(overload->impl);
      nir_emit_impl(overload->impl);
   }
}

static bool
setup_system_values_block(nir_block *block, void *void_visitor)
{
   /* @TODO: Not yet implemented */
   return true;
}

void
vec4_visitor::nir_setup_system_values(nir_shader *shader)
{
   nir_foreach_overload(shader, overload) {
      assert(strcmp(overload->function->name, "main") == 0);
      assert(overload->impl);
      nir_foreach_block(overload->impl, setup_system_values_block, this);
   }
}

void
vec4_visitor::nir_setup_inputs(nir_shader *shader)
{
   nir_inputs = ralloc_array(mem_ctx, src_reg, shader->num_inputs);

   foreach_list_typed(nir_variable, var, node, &shader->inputs) {
      int offset = var->data.driver_location;
      unsigned size = type_size(var->type);
      for (unsigned i = 0; i < size; i++) {
         src_reg src = src_reg(ATTR, var->data.location + i, var->type);
         nir_inputs[offset + i] = src;
      }
   }
}

void
vec4_visitor::nir_setup_uniforms(nir_shader *shader)
{
   /* @TODO: Not yet implemented */
}

void
vec4_visitor::nir_setup_uniform(nir_variable *var)
{
   /* @TODO: Not yet implemented */
}

void
vec4_visitor::nir_setup_builtin_uniform(nir_variable *var)
{
   /* @TODO: Not yet implemented */
}

void
vec4_visitor::nir_emit_impl(nir_function_impl *impl)
{
   /* @TODO: Not yet implemented */
}

void
vec4_visitor::nir_emit_cf_list(exec_list *list)
{
   exec_list_validate(list);
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_if:
         nir_emit_if(nir_cf_node_as_if(node));
         break;

      case nir_cf_node_loop:
         nir_emit_loop(nir_cf_node_as_loop(node));
         break;

      case nir_cf_node_block:
         nir_emit_block(nir_cf_node_as_block(node));
         break;

      default:
         unreachable("Invalid CFG node block");
      }
   }
}

void
vec4_visitor::nir_emit_if(nir_if *if_stmt)
{
   /* @TODO: Not yet implemented */
}

void
vec4_visitor::nir_emit_loop(nir_loop *loop)
{
   /* @TODO: Not yet implemented */
}

void
vec4_visitor::nir_emit_block(nir_block *block)
{
   nir_foreach_instr(block, instr) {
      nir_emit_instr(instr);
   }
}

void
vec4_visitor::nir_emit_instr(nir_instr *instr)
{
   this->base_ir = instr;

   switch (instr->type) {
   case nir_instr_type_load_const:
      nir_emit_load_const(nir_instr_as_load_const(instr));
      break;

   case nir_instr_type_intrinsic:
      nir_emit_intrinsic(nir_instr_as_intrinsic(instr));
      break;

   case nir_instr_type_alu:
      nir_emit_alu(nir_instr_as_alu(instr));
      break;

   case nir_instr_type_jump:
      nir_emit_jump(nir_instr_as_jump(instr));
      break;

   case nir_instr_type_tex:
      nir_emit_texture(nir_instr_as_tex(instr));
      break;

   default:
      fprintf(stderr, "VS instruction not yet implemented by NIR->vec4\n");
      break;
   }
}

void
vec4_visitor::nir_emit_load_const(nir_load_const_instr *instr)
{
   /* @TODO: Not yet implemented */
}

void
vec4_visitor::nir_emit_intrinsic(nir_intrinsic_instr *instr)
{
   switch (instr->intrinsic) {

   case nir_intrinsic_load_input_indirect:
      /* fallthrough */
   case nir_intrinsic_load_input:
      /* @TODO: Not yet implemented */
      break;

   case nir_intrinsic_store_output_indirect:
      /* fallthrough */
   case nir_intrinsic_store_output:
      /* @TODO: Not yet implemented */
      break;

   case nir_intrinsic_load_vertex_id:
      unreachable("should be lowered by lower_vertex_id()");

   case nir_intrinsic_load_vertex_id_zero_base:
      /* @TODO: Not yet implemented */
      break;

   case nir_intrinsic_load_base_vertex:
      /* @TODO: Not yet implemented */
      break;

   case nir_intrinsic_load_instance_id:
      /* @TODO: Not yet implemented */
      break;

   case nir_intrinsic_load_uniform_indirect:
      /* fallthrough */
   case nir_intrinsic_load_uniform:
      /* @TODO: Not yet implemented */
      break;

   case nir_intrinsic_atomic_counter_read:
   case nir_intrinsic_atomic_counter_inc:
   case nir_intrinsic_atomic_counter_dec:
      /* @TODO: Not yet implemented */
      break;

   case nir_intrinsic_load_ubo_indirect:
      /* fallthrough */
   case nir_intrinsic_load_ubo:
      /* @TODO: Not yet implemented */
      break;

   default:
      unreachable("Unknown intrinsic");
   }
}

void
vec4_visitor::nir_emit_alu(nir_alu_instr *instr)
{
   /* @TODO: Not yet implemented */
}

void
vec4_visitor::nir_emit_jump(nir_jump_instr *instr)
{
   /* @TODO: Not yet implemented */
}

void
vec4_visitor::nir_emit_texture(nir_tex_instr *instr)
{
   /* @TODO: Not yet implemented */
}

}
