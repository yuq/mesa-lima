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

void
vec4_visitor::nir_setup_system_value_intrinsic(nir_intrinsic_instr *instr)
{
   dst_reg *reg;

   switch (instr->intrinsic) {
   case nir_intrinsic_load_vertex_id:
      unreachable("should be lowered by lower_vertex_id().");

   case nir_intrinsic_load_vertex_id_zero_base:
      reg = &this->nir_system_values[SYSTEM_VALUE_VERTEX_ID_ZERO_BASE];
      if (reg->file == BAD_FILE)
         *reg =
            *this->make_reg_for_system_value(SYSTEM_VALUE_VERTEX_ID_ZERO_BASE,
                                             glsl_type::int_type);
      break;

   case nir_intrinsic_load_base_vertex:
      reg = &this->nir_system_values[SYSTEM_VALUE_BASE_VERTEX];
      if (reg->file == BAD_FILE)
         *reg = *this->make_reg_for_system_value(SYSTEM_VALUE_BASE_VERTEX,
                                                 glsl_type::int_type);
      break;

   case nir_intrinsic_load_instance_id:
      reg = &this->nir_system_values[SYSTEM_VALUE_INSTANCE_ID];
      if (reg->file == BAD_FILE)
         *reg = *this->make_reg_for_system_value(SYSTEM_VALUE_INSTANCE_ID,
                                                 glsl_type::int_type);
      break;

   default:
      break;
   }
}

static bool
setup_system_values_block(nir_block *block, void *void_visitor)
{
   vec4_visitor *v = (vec4_visitor *)void_visitor;

   nir_foreach_instr(block, instr) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      v->nir_setup_system_value_intrinsic(intrin);
   }

   return true;
}

void
vec4_visitor::nir_setup_system_values(nir_shader *shader)
{
   nir_system_values = ralloc_array(mem_ctx, dst_reg, SYSTEM_VALUE_MAX);

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
   uniforms = 0;

   nir_uniform_driver_location =
      rzalloc_array(mem_ctx, unsigned, this->uniform_array_size);

   if (shader_prog) {
      foreach_list_typed(nir_variable, var, node, &shader->uniforms) {
         /* UBO's, atomics and samplers don't take up space in the
            uniform file */
         if (var->interface_type != NULL || var->type->contains_atomic() ||
             type_size(var->type) == 0) {
            continue;
         }

         assert(uniforms < uniform_array_size);
         this->uniform_size[uniforms] = type_size(var->type);

         if (strncmp(var->name, "gl_", 3) == 0)
            nir_setup_builtin_uniform(var);
         else
            nir_setup_uniform(var);
      }
   } else {
      /* ARB_vertex_program is not supported yet */
      assert("Not implemented");
   }
}

void
vec4_visitor::nir_setup_uniform(nir_variable *var)
{
   int namelen = strlen(var->name);

   /* The data for our (non-builtin) uniforms is stored in a series of
    * gl_uniform_driver_storage structs for each subcomponent that
    * glGetUniformLocation() could name.  We know it's been set up in the same
    * order we'd walk the type, so walk the list of storage and find anything
    * with our name, or the prefix of a component that starts with our name.
    */
    for (unsigned u = 0; u < shader_prog->NumUniformStorage; u++) {
       struct gl_uniform_storage *storage = &shader_prog->UniformStorage[u];

       if (storage->builtin)
          continue;

       if (strncmp(var->name, storage->name, namelen) != 0 ||
           (storage->name[namelen] != 0 &&
            storage->name[namelen] != '.' &&
            storage->name[namelen] != '[')) {
          continue;
       }

       gl_constant_value *components = storage->storage;
       unsigned vector_count = (MAX2(storage->array_elements, 1) *
                                storage->type->matrix_columns);

       for (unsigned s = 0; s < vector_count; s++) {
          assert(uniforms < uniform_array_size);
          uniform_vector_size[uniforms] = storage->type->vector_elements;

          int i;
          for (i = 0; i < uniform_vector_size[uniforms]; i++) {
             stage_prog_data->param[uniforms * 4 + i] = components;
             components++;
          }
          for (; i < 4; i++) {
             static const gl_constant_value zero = { 0.0 };
             stage_prog_data->param[uniforms * 4 + i] = &zero;
          }

          nir_uniform_driver_location[uniforms] = var->data.driver_location;
          uniforms++;
       }
    }
}

void
vec4_visitor::nir_setup_builtin_uniform(nir_variable *var)
{
   const nir_state_slot *const slots = var->state_slots;
   assert(var->state_slots != NULL);

   for (unsigned int i = 0; i < var->num_state_slots; i++) {
      /* This state reference has already been setup by ir_to_mesa,
       * but we'll get the same index back here.  We can reference
       * ParameterValues directly, since unlike brw_fs.cpp, we never
       * add new state references during compile.
       */
      int index = _mesa_add_state_reference(this->prog->Parameters,
					    (gl_state_index *)slots[i].tokens);
      gl_constant_value *values =
         &this->prog->Parameters->ParameterValues[index][0];

      assert(uniforms < uniform_array_size);

      for (unsigned j = 0; j < 4; j++)
         stage_prog_data->param[uniforms * 4 + j] =
            &values[GET_SWZ(slots[i].swizzle, j)];

      this->uniform_vector_size[uniforms] =
         (var->type->is_scalar() || var->type->is_vector() ||
          var->type->is_matrix() ? var->type->vector_elements : 4);

      nir_uniform_driver_location[uniforms] = var->data.driver_location;
      uniforms++;
   }
}

void
vec4_visitor::nir_emit_impl(nir_function_impl *impl)
{
   nir_locals = ralloc_array(mem_ctx, dst_reg, impl->reg_alloc);

   foreach_list_typed(nir_register, reg, node, &impl->registers) {
      unsigned array_elems =
         reg->num_array_elems == 0 ? 1 : reg->num_array_elems;

      nir_locals[reg->index] = dst_reg(GRF, alloc.allocate(array_elems));
   }

   nir_ssa_values = ralloc_array(mem_ctx, dst_reg, impl->ssa_alloc);

   nir_emit_cf_list(&impl->body);
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
   /* First, put the condition in f0 */
   src_reg condition = get_nir_src(if_stmt->condition, BRW_REGISTER_TYPE_D, 1);
   vec4_instruction *inst = emit(MOV(dst_null_d(), condition));
   inst->conditional_mod = BRW_CONDITIONAL_NZ;

   emit(IF(BRW_PREDICATE_NORMAL));

   nir_emit_cf_list(&if_stmt->then_list);

   /* note: if the else is empty, dead CF elimination will remove it */
   emit(BRW_OPCODE_ELSE);

   nir_emit_cf_list(&if_stmt->else_list);

   emit(BRW_OPCODE_ENDIF);
}

void
vec4_visitor::nir_emit_loop(nir_loop *loop)
{
   emit(BRW_OPCODE_DO);

   nir_emit_cf_list(&loop->body);

   emit(BRW_OPCODE_WHILE);
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

static dst_reg
dst_reg_for_nir_reg(vec4_visitor *v, nir_register *nir_reg,
                    unsigned base_offset, nir_src *indirect)
{
   dst_reg reg;

   reg = v->nir_locals[nir_reg->index];
   reg = offset(reg, base_offset);
   if (indirect) {
      reg.reladdr =
         new(v->mem_ctx) src_reg(v->get_nir_src(*indirect,
                                                BRW_REGISTER_TYPE_D,
                                                1));
   }
   return reg;
}

dst_reg
vec4_visitor::get_nir_dest(nir_dest dest)
{
   assert(!dest.is_ssa);
   return dst_reg_for_nir_reg(this, dest.reg.reg, dest.reg.base_offset,
                              dest.reg.indirect);
}

dst_reg
vec4_visitor::get_nir_dest(nir_dest dest, enum brw_reg_type type)
{
   return retype(get_nir_dest(dest), type);
}

dst_reg
vec4_visitor::get_nir_dest(nir_dest dest, nir_alu_type type)
{
   return get_nir_dest(dest, brw_type_for_nir_type(type));
}

src_reg
vec4_visitor::get_nir_src(nir_src src, enum brw_reg_type type,
                          unsigned num_components)
{
   dst_reg reg;

   if (src.is_ssa) {
      assert(src.ssa != NULL);
      reg = nir_ssa_values[src.ssa->index];
   }
   else {
     reg = dst_reg_for_nir_reg(this, src.reg.reg, src.reg.base_offset,
                               src.reg.indirect);
   }

   reg = retype(reg, type);

   src_reg reg_as_src = src_reg(reg);
   reg_as_src.swizzle = brw_swizzle_for_size(num_components);
   return reg_as_src;
}

src_reg
vec4_visitor::get_nir_src(nir_src src, nir_alu_type type,
                          unsigned num_components)
{
   return get_nir_src(src, brw_type_for_nir_type(type), num_components);
}

src_reg
vec4_visitor::get_nir_src(nir_src src, unsigned num_components)
{
   /* if type is not specified, default to signed int */
   return get_nir_src(src, nir_type_int, num_components);
}

void
vec4_visitor::nir_emit_load_const(nir_load_const_instr *instr)
{
   dst_reg reg = dst_reg(GRF, alloc.allocate(1));
   reg.type =  BRW_REGISTER_TYPE_F;

   /* @FIXME: consider emitting vector operations to save some MOVs in
    * cases where the components are representable in 8 bits.
    * By now, we emit a MOV for each component.
    */
   for (unsigned i = 0; i < instr->def.num_components; ++i) {
      reg.writemask = 1 << i;
      emit(MOV(reg, src_reg(instr->value.f[i])));
   }

   /* Set final writemask */
   reg.writemask = brw_writemask_for_size(instr->def.num_components);

   nir_ssa_values[instr->def.index] = reg;
}

void
vec4_visitor::nir_emit_intrinsic(nir_intrinsic_instr *instr)
{
   dst_reg dest;
   src_reg src;

   bool has_indirect = false;

   switch (instr->intrinsic) {

   case nir_intrinsic_load_input_indirect:
      has_indirect = true;
      /* fallthrough */
   case nir_intrinsic_load_input: {
      int offset = instr->const_index[0];
      src = nir_inputs[offset];

      if (has_indirect) {
         dest.reladdr = new(mem_ctx) src_reg(get_nir_src(instr->src[0],
                                                         BRW_REGISTER_TYPE_D,
                                                         1));
      }
      dest = get_nir_dest(instr->dest, src.type);
      dest.writemask = brw_writemask_for_size(instr->num_components);

      emit(MOV(dest, src));
      break;
   }

   case nir_intrinsic_store_output_indirect:
      has_indirect = true;
      /* fallthrough */
   case nir_intrinsic_store_output: {
      int varying = instr->const_index[0];

      src = get_nir_src(instr->src[0], BRW_REGISTER_TYPE_F,
                        instr->num_components);
      dest = dst_reg(src);

      if (has_indirect) {
         dest.reladdr = new(mem_ctx) src_reg(get_nir_src(instr->src[1],
                                                         BRW_REGISTER_TYPE_D,
                                                         1));
      }
      output_reg[varying] = dest;
      break;
   }

   case nir_intrinsic_load_vertex_id:
      unreachable("should be lowered by lower_vertex_id()");

   case nir_intrinsic_load_vertex_id_zero_base: {
      src_reg vertex_id =
         src_reg(nir_system_values[SYSTEM_VALUE_VERTEX_ID_ZERO_BASE]);
      assert(vertex_id.file != BAD_FILE);
      dest = get_nir_dest(instr->dest, vertex_id.type);
      emit(MOV(dest, vertex_id));
      break;
   }

   case nir_intrinsic_load_base_vertex: {
      src_reg base_vertex =
         src_reg(nir_system_values[SYSTEM_VALUE_BASE_VERTEX]);
      assert(base_vertex.file != BAD_FILE);
      dest = get_nir_dest(instr->dest, base_vertex.type);
      emit(MOV(dest, base_vertex));
      break;
   }

   case nir_intrinsic_load_instance_id: {
      src_reg instance_id =
         src_reg(nir_system_values[SYSTEM_VALUE_INSTANCE_ID]);
      assert(instance_id.file != BAD_FILE);
      dest = get_nir_dest(instr->dest, instance_id.type);
      emit(MOV(dest, instance_id));
      break;
   }

   case nir_intrinsic_load_uniform_indirect:
      has_indirect = true;
      /* fallthrough */
   case nir_intrinsic_load_uniform: {
      int uniform = instr->const_index[0];

      dest = get_nir_dest(instr->dest);

      if (has_indirect) {
         /* Split addressing into uniform and offset */
         int offset = uniform - nir_uniform_driver_location[uniform];
         assert(offset >= 0);

         uniform -= offset;
         assert(uniform >= 0);

         src = src_reg(dst_reg(UNIFORM, uniform));
         src.reg_offset = offset;
         src_reg tmp = get_nir_src(instr->src[0], BRW_REGISTER_TYPE_D, 1);
         src.reladdr = new(mem_ctx) src_reg(tmp);
      } else {
         src = src_reg(dst_reg(UNIFORM, uniform));
      }

      emit(MOV(dest, src));
      break;
   }

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
