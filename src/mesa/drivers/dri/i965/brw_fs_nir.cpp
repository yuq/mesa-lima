/*
 * Copyright © 2010 Intel Corporation
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

#include "glsl/nir/glsl_to_nir.h"
#include "brw_fs.h"

void
fs_visitor::emit_nir_code()
{
   /* first, lower the GLSL IR shader to NIR */
   nir_shader *nir = glsl_to_nir(shader->base.ir, NULL, true);
   nir_validate_shader(nir);

   /* lower some of the GLSL-isms into NIR-isms - after this point, we no
    * longer have to deal with variables inside the shader
    */

   nir_lower_variables_scalar(nir, true, true, true, true);
   nir_validate_shader(nir);

   nir_lower_samplers(nir, shader_prog, shader->base.Program);
   nir_validate_shader(nir);

   nir_lower_system_values(nir);
   nir_validate_shader(nir);

   nir_lower_atomics(nir);
   nir_validate_shader(nir);

   nir_remove_dead_variables(nir);
   nir_opt_global_to_local(nir);
   nir_validate_shader(nir);

   if (1)
      nir_print_shader(nir, stderr);

   if (1) {
      nir_convert_to_ssa(nir);
      nir_validate_shader(nir);
      nir_copy_prop(nir);
      nir_validate_shader(nir);
      nir_opt_dce(nir);
      nir_validate_shader(nir);
      nir_opt_peephole_select(nir);
      nir_validate_shader(nir);
      nir_print_shader(nir, stderr);

      nir_convert_from_ssa(nir);
      nir_print_shader(nir, stderr);
      nir_validate_shader(nir);
      nir_lower_vec_to_movs(nir);
      nir_validate_shader(nir);
   }

   /* emit the arrays used for inputs and outputs - load/store intrinsics will
    * be converted to reads/writes of these arrays
    */

   if (nir->num_inputs > 0) {
      nir_inputs = fs_reg(GRF, virtual_grf_alloc(nir->num_inputs));
      nir_setup_inputs(nir);
   }

   if (nir->num_outputs > 0) {
      nir_outputs = fs_reg(GRF, virtual_grf_alloc(nir->num_outputs));
      nir_setup_outputs(nir);
   }

   if (nir->num_uniforms > 0) {
      nir_uniforms = fs_reg(UNIFORM, 0);
      nir_setup_uniforms(nir);
   }

   nir_setup_registers(&nir->registers);

   /* get the main function and emit it */
   nir_foreach_overload(nir, overload) {
      assert(strcmp(overload->function->name, "main") == 0);
      assert(overload->impl);
      nir_emit_impl(overload->impl);
   }

   ralloc_free(nir);
}

void
fs_visitor::nir_setup_inputs(nir_shader *shader)
{
   fs_reg varying = nir_inputs;

   struct hash_entry *entry;
   hash_table_foreach(shader->inputs, entry) {
      nir_variable *var = (nir_variable *) entry->data;
      varying.reg_offset = var->data.driver_location;

      fs_reg reg;
      if (!strcmp(var->name, "gl_FragCoord")) {
         reg = *emit_fragcoord_interpolation(var->data.pixel_center_integer,
                                             var->data.origin_upper_left);
         emit_percomp(MOV(varying, reg), 0xF);
      } else if (!strcmp(var->name, "gl_FrontFacing")) {
         reg = *emit_frontfacing_interpolation();
         emit(MOV(retype(varying, BRW_REGISTER_TYPE_UD), reg));
      } else {
         emit_general_interpolation(varying, var->name, var->type,
                                    (glsl_interp_qualifier) var->data.interpolation,
                                    var->data.location, var->data.centroid,
                                    var->data.sample);
      }
   }
}

void
fs_visitor::nir_setup_outputs(nir_shader *shader)
{
   brw_wm_prog_key *key = (brw_wm_prog_key*) this->key;
   fs_reg reg = nir_outputs;

   struct hash_entry *entry;
   hash_table_foreach(shader->outputs, entry) {
      nir_variable *var = (nir_variable *) entry->data;
      reg.reg_offset = var->data.driver_location;

      if (var->data.index > 0) {
         assert(var->data.location == FRAG_RESULT_DATA0);
         assert(var->data.index == 1);
         this->dual_src_output = reg;
         this->do_dual_src = true;
      } else if (var->data.location == FRAG_RESULT_COLOR) {
         /* Writing gl_FragColor outputs to all color regions. */
         for (unsigned int i = 0; i < MAX2(key->nr_color_regions, 1); i++) {
            this->outputs[i] = reg;
            this->output_components[i] = 4;
         }
      } else if (var->data.location == FRAG_RESULT_DEPTH) {
         this->frag_depth = reg;
      } else if (var->data.location == FRAG_RESULT_SAMPLE_MASK) {
         this->sample_mask = reg;
      } else {
         /* gl_FragData or a user-defined FS output */
         assert(var->data.location >= FRAG_RESULT_DATA0 &&
                var->data.location < FRAG_RESULT_DATA0 + BRW_MAX_DRAW_BUFFERS);

         int vector_elements =
            var->type->is_array() ? var->type->fields.array->vector_elements
                                  : var->type->vector_elements;

         /* General color output. */
         for (unsigned int i = 0; i < MAX2(1, var->type->length); i++) {
            int output = var->data.location - FRAG_RESULT_DATA0 + i;
            this->outputs[output] = reg;
            this->outputs[output].reg_offset += vector_elements * i;
            this->output_components[output] = vector_elements;
         }
      }
   }
}

void
fs_visitor::nir_setup_uniforms(nir_shader *shader)
{
   uniforms = shader->num_uniforms;
   param_size[0] = shader->num_uniforms;

   if (dispatch_width != 8)
      return;

   struct hash_entry *entry;
   hash_table_foreach(shader->uniforms, entry) {
      nir_variable *var = (nir_variable *) entry->data;

      /* UBO's and atomics don't take up space in the uniform file */

      if (var->interface_type != NULL || var->type->contains_atomic())
         continue;

      if (strncmp(var->name, "gl_", 3) == 0)
         nir_setup_builtin_uniform(var);
      else
         nir_setup_uniform(var);
   }
}

void
fs_visitor::nir_setup_uniform(nir_variable *var)
{
   int namelen = strlen(var->name);

   /* The data for our (non-builtin) uniforms is stored in a series of
      * gl_uniform_driver_storage structs for each subcomponent that
      * glGetUniformLocation() could name.  We know it's been set up in the
      * same order we'd walk the type, so walk the list of storage and find
      * anything with our name, or the prefix of a component that starts with
      * our name.
      */
   unsigned index = var->data.driver_location;
   for (unsigned u = 0; u < shader_prog->NumUserUniformStorage; u++) {
      struct gl_uniform_storage *storage = &shader_prog->UniformStorage[u];

      if (strncmp(var->name, storage->name, namelen) != 0 ||
         (storage->name[namelen] != 0 &&
         storage->name[namelen] != '.' &&
         storage->name[namelen] != '[')) {
         continue;
      }

      unsigned slots = storage->type->component_slots();
      if (storage->array_elements)
         slots *= storage->array_elements;

      for (unsigned i = 0; i < slots; i++) {
         stage_prog_data->param[index++] = &storage->storage[i];
      }
   }

   /* Make sure we actually initialized the right amount of stuff here. */
   assert(var->data.driver_location + var->type->component_slots() == index);
}

void
fs_visitor::nir_setup_builtin_uniform(nir_variable *var)
{
   const nir_state_slot *const slots = var->state_slots;
   assert(var->state_slots != NULL);

   unsigned uniform_index = var->data.driver_location;
   for (unsigned int i = 0; i < var->num_state_slots; i++) {
      /* This state reference has already been setup by ir_to_mesa, but we'll
       * get the same index back here.
       */
      int index = _mesa_add_state_reference(this->prog->Parameters,
                                            (gl_state_index *)slots[i].tokens);

      /* Add each of the unique swizzles of the element as a parameter.
       * This'll end up matching the expected layout of the
       * array/matrix/structure we're trying to fill in.
       */
      int last_swiz = -1;
      for (unsigned int j = 0; j < 4; j++) {
         int swiz = GET_SWZ(slots[i].swizzle, j);
         if (swiz == last_swiz)
            break;
         last_swiz = swiz;

         stage_prog_data->param[uniform_index++] =
            &prog->Parameters->ParameterValues[index][swiz];
      }
   }
}

void
fs_visitor::nir_setup_registers(exec_list *list)
{
   foreach_list_typed(nir_register, nir_reg, node, list) {
      unsigned array_elems =
         nir_reg->num_array_elems == 0 ? 1 : nir_reg->num_array_elems;
      unsigned size = array_elems * nir_reg->num_components;
      fs_reg *reg = new(mem_ctx) fs_reg(GRF, virtual_grf_alloc(size));
      _mesa_hash_table_insert(this->nir_reg_ht, nir_reg, reg);
   }
}

void
fs_visitor::nir_emit_impl(nir_function_impl *impl)
{
   nir_setup_registers(&impl->registers);
   nir_emit_cf_list(&impl->body);
}

void
fs_visitor::nir_emit_cf_list(exec_list *list)
{
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
fs_visitor::nir_emit_if(nir_if *if_stmt)
{
   if (brw->gen < 6) {
      no16("Can't support (non-uniform) control flow on SIMD16\n");
   }

   /* first, put the condition into f0 */
   fs_inst *inst = emit(MOV(reg_null_d,
                            retype(get_nir_src(if_stmt->condition),
                                   BRW_REGISTER_TYPE_UD)));
   inst->conditional_mod = BRW_CONDITIONAL_NZ;

   emit(IF(BRW_PREDICATE_NORMAL));

   nir_emit_cf_list(&if_stmt->then_list);

   /* note: if the else is empty, dead CF elimination will remove it */
   emit(BRW_OPCODE_ELSE);

   nir_emit_cf_list(&if_stmt->else_list);

   emit(BRW_OPCODE_ENDIF);

   try_replace_with_sel();
}

void
fs_visitor::nir_emit_loop(nir_loop *loop)
{
   if (brw->gen < 6) {
      no16("Can't support (non-uniform) control flow on SIMD16\n");
   }

   emit(BRW_OPCODE_DO);

   nir_emit_cf_list(&loop->body);

   emit(BRW_OPCODE_WHILE);
}

void
fs_visitor::nir_emit_block(nir_block *block)
{
   nir_foreach_instr(block, instr) {
      nir_emit_instr(instr);
   }
}

void
fs_visitor::nir_emit_instr(nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_alu:
      nir_emit_alu(nir_instr_as_alu(instr));
      break;

   case nir_instr_type_intrinsic:
      nir_emit_intrinsic(nir_instr_as_intrinsic(instr));
      break;

   case nir_instr_type_texture:
      nir_emit_texture(nir_instr_as_texture(instr));
      break;

   case nir_instr_type_load_const:
      nir_emit_load_const(nir_instr_as_load_const(instr));
      break;

   case nir_instr_type_jump:
      nir_emit_jump(nir_instr_as_jump(instr));
      break;

   default:
      unreachable("unknown instruction type");
   }
}

static brw_reg_type
brw_type_for_nir_type(nir_alu_type type)
{
   switch (type) {
   case nir_type_bool:
   case nir_type_unsigned:
      return BRW_REGISTER_TYPE_UD;
   case nir_type_int:
      return BRW_REGISTER_TYPE_D;
   case nir_type_float:
      return BRW_REGISTER_TYPE_F;
   default:
      unreachable("unknown type");
   }

   return BRW_REGISTER_TYPE_F;
}

void
fs_visitor::nir_emit_alu(nir_alu_instr *instr)
{
   struct brw_wm_prog_key *fs_key = (struct brw_wm_prog_key *) this->key;

   fs_reg op[3];
   fs_reg dest = get_nir_dest(instr->dest.dest);
   dest.type = brw_type_for_nir_type(nir_op_infos[instr->op].output_type);

   fs_reg result;
   if (instr->has_predicate) {
      result = fs_reg(GRF, virtual_grf_alloc(4));
      result.type = dest.type;
   } else {
      result = dest;
   }


   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++)
      op[i] = get_nir_alu_src(instr, i);

   switch (instr->op) {
   case nir_op_fmov:
   case nir_op_i2f:
   case nir_op_u2f: {
      fs_inst *inst = MOV(result, op[0]);
      inst->saturate = instr->dest.saturate;
      emit_percomp(inst, instr->dest.write_mask);
   }
      break;

   case nir_op_imov:
   case nir_op_f2i:
   case nir_op_f2u:
      emit_percomp(MOV(result, op[0]), instr->dest.write_mask);
      break;

   case nir_op_fsign: {
      /* AND(val, 0x80000000) gives the sign bit.
         *
         * Predicated OR ORs 1.0 (0x3f800000) with the sign bit if val is not
         * zero.
         */
      emit_percomp(CMP(reg_null_f, op[0], fs_reg(0.0f), BRW_CONDITIONAL_NZ),
                   instr->dest.write_mask);

      fs_reg result_int = retype(result, BRW_REGISTER_TYPE_UD);
      op[0].type = BRW_REGISTER_TYPE_UD;
      result.type = BRW_REGISTER_TYPE_UD;
      emit_percomp(AND(result_int, op[0], fs_reg(0x80000000u)),
                   instr->dest.write_mask);

      fs_inst *inst = OR(result_int, result_int, fs_reg(0x3f800000u));
      inst->predicate = BRW_PREDICATE_NORMAL;
      emit_percomp(inst, instr->dest.write_mask);
      if (instr->dest.saturate) {
         fs_inst *inst = MOV(result, result);
         inst->saturate = true;
         emit_percomp(inst, instr->dest.write_mask);
      }
      break;
   }

   case nir_op_isign: {
      /*  ASR(val, 31) -> negative val generates 0xffffffff (signed -1).
         *               -> non-negative val generates 0x00000000.
         *  Predicated OR sets 1 if val is positive.
         */
      emit_percomp(CMP(reg_null_d, op[0], fs_reg(0), BRW_CONDITIONAL_G),
                   instr->dest.write_mask);

      emit_percomp(ASR(result, op[0], fs_reg(31)), instr->dest.write_mask);

      fs_inst *inst = OR(result, result, fs_reg(1));
      inst->predicate = BRW_PREDICATE_NORMAL;
      emit_percomp(inst, instr->dest.write_mask);
      break;
   }

   case nir_op_frcp:
      emit_math_percomp(SHADER_OPCODE_RCP, result, op[0],
                        instr->dest.write_mask, instr->dest.saturate);
      break;

   case nir_op_fexp2:
      emit_math_percomp(SHADER_OPCODE_EXP2, result, op[0],
                        instr->dest.write_mask, instr->dest.saturate);
      break;

   case nir_op_flog2:
      emit_math_percomp(SHADER_OPCODE_LOG2, result, op[0],
                        instr->dest.write_mask, instr->dest.saturate);
      break;

   case nir_op_fexp:
   case nir_op_flog:
      unreachable("not reached: should be handled by ir_explog_to_explog2");

   case nir_op_fsin:
   case nir_op_fsin_reduced:
      emit_math_percomp(SHADER_OPCODE_SIN, result, op[0],
                        instr->dest.write_mask, instr->dest.saturate);
      break;

   case nir_op_fcos:
   case nir_op_fcos_reduced:
      emit_math_percomp(SHADER_OPCODE_COS, result, op[0],
                        instr->dest.write_mask, instr->dest.saturate);
      break;

   case nir_op_fddx:
      if (fs_key->high_quality_derivatives)
         emit_percomp(FS_OPCODE_DDX_FINE, result, op[0],
                      instr->dest.write_mask, instr->dest.saturate);
      else
         emit_percomp(FS_OPCODE_DDX_COARSE, result, op[0],
                      instr->dest.write_mask, instr->dest.saturate);
      break;
   case nir_op_fddx_fine:
      emit_percomp(FS_OPCODE_DDX_FINE, result, op[0],
                   instr->dest.write_mask, instr->dest.saturate);
      break;
   case nir_op_fddx_coarse:
      emit_percomp(FS_OPCODE_DDX_COARSE, result, op[0],
                   instr->dest.write_mask, instr->dest.saturate);
      break;
   case nir_op_fddy:
      if (fs_key->high_quality_derivatives)
         emit_percomp(FS_OPCODE_DDY_FINE, result, op[0],
                      fs_reg(fs_key->render_to_fbo),
                      instr->dest.write_mask, instr->dest.saturate);
      else
         emit_percomp(FS_OPCODE_DDY_COARSE, result, op[0],
                      fs_reg(fs_key->render_to_fbo),
                      instr->dest.write_mask, instr->dest.saturate);
      break;
   case nir_op_fddy_fine:
      emit_percomp(FS_OPCODE_DDY_FINE, result, op[0],
                   fs_reg(fs_key->render_to_fbo),
                   instr->dest.write_mask, instr->dest.saturate);
      break;
   case nir_op_fddy_coarse:
      emit_percomp(FS_OPCODE_DDY_COARSE, result, op[0],
                   fs_reg(fs_key->render_to_fbo),
                   instr->dest.write_mask, instr->dest.saturate);
      break;

   case nir_op_fadd:
   case nir_op_iadd: {
      fs_inst *inst = ADD(result, op[0], op[1]);
      inst->saturate = instr->dest.saturate;
      emit_percomp(inst, instr->dest.write_mask);
      break;
   }

   case nir_op_fmul: {
      fs_inst *inst = MUL(result, op[0], op[1]);
      inst->saturate = instr->dest.saturate;
      emit_percomp(MUL(result, op[0], op[1]), instr->dest.write_mask);
      break;
   }

   case nir_op_imul: {
      /* TODO put in the 16-bit constant optimization once we have SSA */

      if (brw->gen >= 7)
         no16("SIMD16 explicit accumulator operands unsupported\n");

      struct brw_reg acc = retype(brw_acc_reg(dispatch_width), result.type);

      emit_percomp(MUL(acc, op[0], op[1]), instr->dest.write_mask);
      emit_percomp(MACH(reg_null_d, op[0], op[1]), instr->dest.write_mask);
      emit_percomp(MOV(result, fs_reg(acc)), instr->dest.write_mask);
      break;
   }

   case nir_op_imul_high:
   case nir_op_umul_high: {
      if (brw->gen >= 7)
         no16("SIMD16 explicit accumulator operands unsupported\n");

      struct brw_reg acc = retype(brw_acc_reg(dispatch_width), result.type);

      emit_percomp(MUL(acc, op[0], op[1]), instr->dest.write_mask);
      emit_percomp(MACH(result, op[0], op[1]), instr->dest.write_mask);
      break;
   }

   case nir_op_idiv:
   case nir_op_udiv:
      emit_math_percomp(SHADER_OPCODE_INT_QUOTIENT, result, op[0], op[1],
                        instr->dest.write_mask);
      break;

   case nir_op_uadd_carry: {
      if (brw->gen >= 7)
         no16("SIMD16 explicit accumulator operands unsupported\n");

      struct brw_reg acc = retype(brw_acc_reg(dispatch_width),
                                  BRW_REGISTER_TYPE_UD);

      emit_percomp(ADDC(reg_null_ud, op[0], op[1]), instr->dest.write_mask);
      emit_percomp(MOV(result, fs_reg(acc)), instr->dest.write_mask);
      break;
   }

   case nir_op_usub_borrow: {
      if (brw->gen >= 7)
         no16("SIMD16 explicit accumulator operands unsupported\n");

      struct brw_reg acc = retype(brw_acc_reg(dispatch_width),
                                  BRW_REGISTER_TYPE_UD);

      emit_percomp(SUBB(reg_null_ud, op[0], op[1]), instr->dest.write_mask);
      emit_percomp(MOV(result, fs_reg(acc)), instr->dest.write_mask);
      break;
   }

   case nir_op_umod:
      emit_math_percomp(SHADER_OPCODE_INT_REMAINDER, result, op[0],
                        op[1], instr->dest.write_mask);
      break;

   case nir_op_flt:
   case nir_op_ilt:
   case nir_op_ult:
      emit_percomp(CMP(result, op[0], op[1], BRW_CONDITIONAL_L),
                   instr->dest.write_mask);
      break;

   case nir_op_fge:
   case nir_op_ige:
   case nir_op_uge:
      emit_percomp(CMP(result, op[0], op[1], BRW_CONDITIONAL_GE),
                   instr->dest.write_mask);
      break;

   case nir_op_feq:
   case nir_op_ieq:
      emit_percomp(CMP(result, op[0], op[1], BRW_CONDITIONAL_Z),
                   instr->dest.write_mask);
      break;

   case nir_op_fne:
   case nir_op_ine:
      emit_percomp(CMP(result, op[0], op[1], BRW_CONDITIONAL_NZ),
                   instr->dest.write_mask);
      break;

   case nir_op_ball_fequal2:
   case nir_op_ball_iequal2:
   case nir_op_ball_fequal3:
   case nir_op_ball_iequal3:
   case nir_op_ball_fequal4:
   case nir_op_ball_iequal4: {
      unsigned num_components = nir_op_infos[instr->op].input_sizes[0];
      fs_reg temp = fs_reg(GRF, virtual_grf_alloc(num_components));
      emit_percomp(CMP(temp, op[0], op[1], BRW_CONDITIONAL_Z),
                   (1 << num_components) - 1);
      emit_reduction(BRW_OPCODE_AND, result, temp, num_components);
      break;
   }

   case nir_op_bany_fnequal2:
   case nir_op_bany_inequal2:
   case nir_op_bany_fnequal3:
   case nir_op_bany_inequal3:
   case nir_op_bany_fnequal4:
   case nir_op_bany_inequal4: {
      unsigned num_components = nir_op_infos[instr->op].input_sizes[0];
      fs_reg temp = fs_reg(GRF, virtual_grf_alloc(num_components));
      temp.type = BRW_REGISTER_TYPE_UD;
      emit_percomp(CMP(temp, op[0], op[1], BRW_CONDITIONAL_NZ),
                   (1 << num_components) - 1);
      emit_reduction(BRW_OPCODE_OR, result, temp, num_components);
      break;
   }

   case nir_op_inot:
      emit_percomp(NOT(result, op[0]), instr->dest.write_mask);
      break;
   case nir_op_ixor:
      emit_percomp(XOR(result, op[0], op[1]), instr->dest.write_mask);
      break;
   case nir_op_ior:
      emit_percomp(OR(result, op[0], op[1]), instr->dest.write_mask);
      break;
   case nir_op_iand:
      emit_percomp(AND(result, op[0], op[1]), instr->dest.write_mask);
      break;

   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4: {
      unsigned num_components = nir_op_infos[instr->op].input_sizes[0];
      fs_reg temp = fs_reg(GRF, virtual_grf_alloc(num_components));
      emit_percomp(MUL(temp, op[0], op[1]), (1 << num_components) - 1);
      emit_reduction(BRW_OPCODE_ADD, result, temp, num_components);
      if (instr->dest.saturate) {
         fs_inst *inst = emit(MOV(result, result));
         inst->saturate = true;
      }
      break;
   }

   case nir_op_bany2:
   case nir_op_bany3:
   case nir_op_bany4: {
      unsigned num_components = nir_op_infos[instr->op].input_sizes[0];
      emit_reduction(BRW_OPCODE_OR, result, op[0], num_components);
      break;
   }

   case nir_op_ball2:
   case nir_op_ball3:
   case nir_op_ball4: {
      unsigned num_components = nir_op_infos[instr->op].input_sizes[0];
      emit_reduction(BRW_OPCODE_AND, result, op[0], num_components);
      break;
   }

   case nir_op_fnoise1_1:
   case nir_op_fnoise1_2:
   case nir_op_fnoise1_3:
   case nir_op_fnoise1_4:
   case nir_op_fnoise2_1:
   case nir_op_fnoise2_2:
   case nir_op_fnoise2_3:
   case nir_op_fnoise2_4:
   case nir_op_fnoise3_1:
   case nir_op_fnoise3_2:
   case nir_op_fnoise3_3:
   case nir_op_fnoise3_4:
   case nir_op_fnoise4_1:
   case nir_op_fnoise4_2:
   case nir_op_fnoise4_3:
   case nir_op_fnoise4_4:
      unreachable("not reached: should be handled by lower_noise");

   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
      unreachable("not reached: should be handled by lower_quadop_vector");

   case nir_op_ldexp:
      unreachable("not reached: should be handled by ldexp_to_arith()");

   case nir_op_fsqrt:
      emit_math_percomp(SHADER_OPCODE_SQRT, result, op[0],
                        instr->dest.write_mask, instr->dest.saturate);
      break;

   case nir_op_frsq:
      emit_math_percomp(SHADER_OPCODE_RSQ, result, op[0],
                        instr->dest.write_mask, instr->dest.saturate);
      break;

   case nir_op_b2i:
      emit_percomp(AND(result, op[0], fs_reg(1)), instr->dest.write_mask);
      break;
   case nir_op_b2f: {
      emit_percomp(AND(retype(result, BRW_REGISTER_TYPE_UD), op[0],
                       fs_reg(0x3f800000u)),
                   instr->dest.write_mask);
      break;
   }

   case nir_op_f2b:
      emit_percomp(CMP(result, op[0], fs_reg(0.0f), BRW_CONDITIONAL_NZ),
                   instr->dest.write_mask);
      break;
   case nir_op_i2b:
      emit_percomp(CMP(result, op[0], fs_reg(0), BRW_CONDITIONAL_NZ),
                   instr->dest.write_mask);
      break;

   case nir_op_ftrunc: {
      fs_inst *inst = RNDZ(result, op[0]);
      inst->saturate = instr->dest.saturate;
      emit_percomp(inst, instr->dest.write_mask);
      break;
   }
   case nir_op_fceil: {
      op[0].negate = !op[0].negate;
      fs_reg temp = fs_reg(this, glsl_type::vec4_type);
      emit_percomp(RNDD(temp, op[0]), instr->dest.write_mask);
      temp.negate = true;
      fs_inst *inst = MOV(result, temp);
      inst->saturate = instr->dest.saturate;
      emit_percomp(inst, instr->dest.write_mask);
      break;
   }
   case nir_op_ffloor: {
      fs_inst *inst = RNDD(result, op[0]);
      inst->saturate = instr->dest.saturate;
      emit_percomp(inst, instr->dest.write_mask);
      break;
   }
   case nir_op_ffract: {
      fs_inst *inst = FRC(result, op[0]);
      inst->saturate = instr->dest.saturate;
      emit_percomp(inst, instr->dest.write_mask);
      break;
   }
   case nir_op_fround_even: {
      fs_inst *inst = RNDE(result, op[0]);
      inst->saturate = instr->dest.saturate;
      emit_percomp(inst, instr->dest.write_mask);
      break;
   }

   case nir_op_fmin:
   case nir_op_imin:
   case nir_op_umin:
      if (brw->gen >= 6) {
         emit_percomp(BRW_OPCODE_SEL, result, op[0], op[1],
                      instr->dest.write_mask, instr->dest.saturate,
                      BRW_PREDICATE_NONE, BRW_CONDITIONAL_L);
      } else {
         emit_percomp(CMP(reg_null_d, op[0], op[1], BRW_CONDITIONAL_L),
                      instr->dest.write_mask);

         emit_percomp(BRW_OPCODE_SEL, result, op[0], op[1],
                      instr->dest.write_mask, instr->dest.saturate,
                      BRW_PREDICATE_NORMAL);
      }
      break;

   case nir_op_fmax:
   case nir_op_imax:
   case nir_op_umax:
      if (brw->gen >= 6) {
         emit_percomp(BRW_OPCODE_SEL, result, op[0], op[1],
                      instr->dest.write_mask, instr->dest.saturate,
                      BRW_PREDICATE_NONE, BRW_CONDITIONAL_GE);
      } else {
         emit_percomp(CMP(reg_null_d, op[0], op[1], BRW_CONDITIONAL_GE),
                      instr->dest.write_mask);

         emit_percomp(BRW_OPCODE_SEL, result, op[0], op[1],
                      instr->dest.write_mask, instr->dest.saturate,
                      BRW_PREDICATE_NORMAL);
      }
      break;

   case nir_op_pack_snorm_2x16:
   case nir_op_pack_snorm_4x8:
   case nir_op_pack_unorm_2x16:
   case nir_op_pack_unorm_4x8:
   case nir_op_unpack_snorm_2x16:
   case nir_op_unpack_snorm_4x8:
   case nir_op_unpack_unorm_2x16:
   case nir_op_unpack_unorm_4x8:
   case nir_op_unpack_half_2x16:
   case nir_op_pack_half_2x16:
      unreachable("not reached: should be handled by lower_packing_builtins");

   case nir_op_unpack_half_2x16_split_x:
      emit_percomp(FS_OPCODE_UNPACK_HALF_2x16_SPLIT_X, result, op[0],
                   instr->dest.write_mask, instr->dest.saturate);
      break;
   case nir_op_unpack_half_2x16_split_y:
      emit_percomp(FS_OPCODE_UNPACK_HALF_2x16_SPLIT_Y, result, op[0],
           instr->dest.write_mask, instr->dest.saturate);
      break;

   case nir_op_fpow:
      emit_percomp(SHADER_OPCODE_POW, result, op[0], op[1],
                   instr->dest.write_mask, instr->dest.saturate);
      break;

   case nir_op_bitfield_reverse:
      emit_percomp(BFREV(result, op[0]), instr->dest.write_mask);
      break;

   case nir_op_bit_count:
      emit_percomp(CBIT(result, op[0]), instr->dest.write_mask);
      break;

   case nir_op_ufind_msb:
   case nir_op_ifind_msb: {
      emit_percomp(FBH(retype(result, BRW_REGISTER_TYPE_UD), op[0]),
                   instr->dest.write_mask);

      /* FBH counts from the MSB side, while GLSL's findMSB() wants the count
       * from the LSB side. If FBH didn't return an error (0xFFFFFFFF), then
       * subtract the result from 31 to convert the MSB count into an LSB count.
       */

      emit_percomp(CMP(reg_null_d, result, fs_reg(-1), BRW_CONDITIONAL_NZ),
                   instr->dest.write_mask);
      fs_reg neg_result(result);
      neg_result.negate = true;
      fs_inst *inst = ADD(result, neg_result, fs_reg(31));
      inst->predicate = BRW_PREDICATE_NORMAL;
      emit_percomp(inst, instr->dest.write_mask);
      break;
   }

   case nir_op_find_lsb:
      emit_percomp(FBL(result, op[0]), instr->dest.write_mask);
      break;

   case nir_op_ubitfield_extract:
   case nir_op_ibitfield_extract:
      emit_percomp(BFE(result, op[2], op[1], op[0]), instr->dest.write_mask);
      break;
   case nir_op_bfm:
      emit_percomp(BFI1(result, op[0], op[1]), instr->dest.write_mask);
      break;
   case nir_op_bfi:
      emit_percomp(BFI2(result, op[0], op[1], op[2]), instr->dest.write_mask);
      break;

   case nir_op_bitfield_insert:
      unreachable("not reached: should be handled by "
                  "lower_instructions::bitfield_insert_to_bfm_bfi");

   case nir_op_ishl:
      emit_percomp(SHL(result, op[0], op[1]), instr->dest.write_mask);
      break;
   case nir_op_ishr:
      emit_percomp(ASR(result, op[0], op[1]), instr->dest.write_mask);
      break;
   case nir_op_ushr:
      emit_percomp(SHR(result, op[0], op[1]), instr->dest.write_mask);
      break;

   case nir_op_pack_half_2x16_split:
      emit_percomp(FS_OPCODE_PACK_HALF_2x16_SPLIT, result, op[0], op[1],
                   instr->dest.write_mask);
      break;

   case nir_op_ffma:
      emit_percomp(MAD(result, op[2], op[1], op[0]), instr->dest.write_mask);
      break;

   case nir_op_flrp:
      /* TODO emulate for gen < 6 */
      emit_percomp(LRP(result, op[2], op[1], op[0]), instr->dest.write_mask);
      break;

   case nir_op_bcsel:
      emit(CMP(reg_null_d, op[0], fs_reg(0), BRW_CONDITIONAL_NZ));
      emit_percomp(BRW_OPCODE_SEL, result, op[1], op[2],
                   instr->dest.write_mask, false, BRW_PREDICATE_NORMAL);
      break;

   default:
      unreachable("unhandled instruction");
   }

   /* emit a predicated move if there was predication */
   if (instr->has_predicate) {
      fs_inst *inst = emit(MOV(reg_null_d,
                               retype(get_nir_src(instr->predicate),
                                   BRW_REGISTER_TYPE_UD)));
      inst->conditional_mod = BRW_CONDITIONAL_NZ;
      inst = MOV(dest, result);
      inst->predicate = BRW_PREDICATE_NORMAL;
      emit_percomp(inst, instr->dest.write_mask);
   }
}

fs_reg
fs_visitor::get_nir_src(nir_src src)
{
   struct hash_entry *entry =
      _mesa_hash_table_search(this->nir_reg_ht, src.reg.reg);
   fs_reg reg = *((fs_reg *) entry->data);
   /* to avoid floating-point denorm flushing problems, set the type by
    * default to D - instructions that need floating point semantics will set
    * this to F if they need to
    */
   reg.type = BRW_REGISTER_TYPE_D;
   reg.reg_offset = src.reg.base_offset;
   if (src.reg.indirect) {
      reg.reladdr = new(mem_ctx) fs_reg();
      *reg.reladdr = retype(get_nir_src(*src.reg.indirect),
                            BRW_REGISTER_TYPE_D);
   }

   return reg;
}

fs_reg
fs_visitor::get_nir_alu_src(nir_alu_instr *instr, unsigned src)
{
   fs_reg reg = get_nir_src(instr->src[src].src);

   reg.type = brw_type_for_nir_type(nir_op_infos[instr->op].input_types[src]);
   reg.abs = instr->src[src].abs;
   reg.negate = instr->src[src].negate;

   bool needs_swizzle = false;
   unsigned num_components = 0;
   for (unsigned i = 0; i < 4; i++) {
      if (!nir_alu_instr_channel_used(instr, src, i))
         continue;

      if (instr->src[src].swizzle[i] != i)
         needs_swizzle = true;

      num_components = i + 1;
   }

   if (needs_swizzle) {
      /* resolve the swizzle through MOV's */
      fs_reg new_reg = fs_reg(GRF, virtual_grf_alloc(num_components), reg.type);

      for (unsigned i = 0; i < 4; i++) {
         if (!nir_alu_instr_channel_used(instr, src, i))
            continue;

         emit(MOV(offset(new_reg, i),
                  offset(reg, instr->src[src].swizzle[i])));
      }

      return new_reg;
   }

   return reg;
}

fs_reg
fs_visitor::get_nir_dest(nir_dest dest)
{
   struct hash_entry *entry =
      _mesa_hash_table_search(this->nir_reg_ht, dest.reg.reg);
   fs_reg reg = *((fs_reg *) entry->data);
   reg.reg_offset = dest.reg.base_offset;
   if (dest.reg.indirect) {
      reg.reladdr = new(mem_ctx) fs_reg();
      *reg.reladdr = retype(get_nir_src(*dest.reg.indirect),
                            BRW_REGISTER_TYPE_D);
   }

   return reg;
}

void
fs_visitor::emit_percomp(fs_inst *inst, unsigned wr_mask)
{
   for (unsigned i = 0; i < 4; i++) {
      if (!((wr_mask >> i) & 1))
         continue;

      fs_inst *new_inst = new(mem_ctx) fs_inst(*inst);
      new_inst->dst.reg_offset += i;
      for (unsigned j = 0; j < new_inst->sources; j++)
         if (inst->src[j].file == GRF)
            new_inst->src[j].reg_offset += i;

      emit(new_inst);
   }
}

void
fs_visitor::emit_percomp(enum opcode op, fs_reg dest, fs_reg src0,
                         unsigned wr_mask, bool saturate,
                         enum brw_predicate predicate,
                         enum brw_conditional_mod mod)
{
   for (unsigned i = 0; i < 4; i++) {
      if (!((wr_mask >> i) & 1))
         continue;

      fs_inst *new_inst = new(mem_ctx) fs_inst(op, dest, src0);
      new_inst->dst.reg_offset += i;
      for (unsigned j = 0; j < new_inst->sources; j++)
         if (new_inst->src[j].file == GRF)
            new_inst->src[j].reg_offset += i;

      new_inst->predicate = predicate;
      new_inst->conditional_mod = mod;
      new_inst->saturate = saturate;
      emit(new_inst);
   }
}

void
fs_visitor::emit_percomp(enum opcode op, fs_reg dest, fs_reg src0, fs_reg src1,
                         unsigned wr_mask, bool saturate,
                         enum brw_predicate predicate,
                         enum brw_conditional_mod mod)
{
   for (unsigned i = 0; i < 4; i++) {
      if (!((wr_mask >> i) & 1))
         continue;

      fs_inst *new_inst = new(mem_ctx) fs_inst(op, dest, src0, src1);
      new_inst->dst.reg_offset += i;
      for (unsigned j = 0; j < new_inst->sources; j++)
         if (new_inst->src[j].file == GRF)
            new_inst->src[j].reg_offset += i;

      new_inst->predicate = predicate;
      new_inst->conditional_mod = mod;
      new_inst->saturate = saturate;
      emit(new_inst);
   }
}

void
fs_visitor::emit_math_percomp(enum opcode op, fs_reg dest, fs_reg src0,
                              unsigned wr_mask, bool saturate)
{
   for (unsigned i = 0; i < 4; i++) {
      if (!((wr_mask >> i) & 1))
         continue;

      fs_reg new_dest = dest;
      new_dest.reg_offset += i;
      fs_reg new_src0 = src0;
      if (src0.file == GRF)
         new_src0.reg_offset += i;

      fs_inst *new_inst = emit_math(op, new_dest, new_src0);
      new_inst->saturate = saturate;
   }
}

void
fs_visitor::emit_math_percomp(enum opcode op, fs_reg dest, fs_reg src0,
                              fs_reg src1, unsigned wr_mask,
                              bool saturate)
{
   for (unsigned i = 0; i < 4; i++) {
      if (!((wr_mask >> i) & 1))
         continue;

      fs_reg new_dest = dest;
      new_dest.reg_offset += i;
      fs_reg new_src0 = src0;
      if (src0.file == GRF)
         new_src0.reg_offset += i;
      fs_reg new_src1 = src1;
      if (src1.file == GRF)
         new_src1.reg_offset += i;

      fs_inst *new_inst = emit_math(op, new_dest, new_src0, new_src1);
      new_inst->saturate = saturate;
   }
}

void
fs_visitor::emit_reduction(enum opcode op, fs_reg dest, fs_reg src,
                           unsigned num_components)
{
   fs_reg src0 = src;
   fs_reg src1 = src;
   src1.reg_offset++;

   if (num_components == 2) {
      emit(op, dest, src0, src1);
      return;
   }

   fs_reg temp1 = fs_reg(GRF, virtual_grf_alloc(1));
   temp1.type = src.type;
   emit(op, temp1, src0, src1);

   fs_reg src2 = src;
   src2.reg_offset += 2;

   if (num_components == 3) {
      emit(op, dest, temp1, src2);
      return;
   }

   assert(num_components == 4);

   fs_reg src3 = src;
   src3.reg_offset += 3;
   fs_reg temp2 = fs_reg(GRF, virtual_grf_alloc(1));
   temp2.type = src.type;

   emit(op, temp2, src2, src3);
   emit(op, dest, temp1, temp2);
}

void
fs_visitor::nir_emit_intrinsic(nir_intrinsic_instr *instr)
{
   fs_reg dest;
   if (nir_intrinsic_infos[instr->intrinsic].has_dest)
      dest = get_nir_dest(instr->dest);
   if (instr->has_predicate) {
      fs_inst *inst = emit(MOV(reg_null_d,
                               retype(get_nir_src(instr->predicate),
                                      BRW_REGISTER_TYPE_UD)));
      inst->conditional_mod = BRW_CONDITIONAL_NZ;
   }

   switch (instr->intrinsic) {
   case nir_intrinsic_discard: {
      /* We track our discarded pixels in f0.1.  By predicating on it, we can
       * update just the flag bits that aren't yet discarded.  By emitting a
       * CMP of g0 != g0, all our currently executing channels will get turned
       * off.
       */
      fs_reg some_reg = fs_reg(retype(brw_vec8_grf(0, 0),
                                    BRW_REGISTER_TYPE_UW));
      fs_inst *cmp = emit(CMP(reg_null_f, some_reg, some_reg,
                              BRW_CONDITIONAL_NZ));
      cmp->predicate = BRW_PREDICATE_NORMAL;
      cmp->flag_subreg = 1;

      if (brw->gen >= 6) {
         /* For performance, after a discard, jump to the end of the shader.
         * Only jump if all relevant channels have been discarded.
         */
         fs_inst *discard_jump = emit(FS_OPCODE_DISCARD_JUMP);
         discard_jump->flag_subreg = 1;

         discard_jump->predicate = (dispatch_width == 8)
                                 ? BRW_PREDICATE_ALIGN1_ANY8H
                                 : BRW_PREDICATE_ALIGN1_ANY16H;
         discard_jump->predicate_inverse = true;
      }

      break;
   }

   case nir_intrinsic_atomic_counter_inc:
   case nir_intrinsic_atomic_counter_dec:
   case nir_intrinsic_atomic_counter_read: {
      unsigned surf_index = prog_data->binding_table.abo_start +
                            (unsigned) instr->const_index[0];
      fs_reg offset = fs_reg(get_nir_src(instr->src[0]));

      switch (instr->intrinsic) {
         case nir_intrinsic_atomic_counter_inc:
            emit_untyped_atomic(BRW_AOP_INC, surf_index, dest, offset,
                                fs_reg(), fs_reg());
            break;
         case nir_intrinsic_atomic_counter_dec:
            emit_untyped_atomic(BRW_AOP_PREDEC, surf_index, dest, offset,
                                fs_reg(), fs_reg());
            break;
         case nir_intrinsic_atomic_counter_read:
            emit_untyped_surface_read(surf_index, dest, offset);
            break;
         default:
            unreachable("Unreachable");
      }
      break;
   }

   case nir_intrinsic_load_front_face:
      assert(!"TODO");

   case nir_intrinsic_load_sample_mask_in: {
      assert(brw->gen >= 7);
      fs_reg reg = fs_reg(retype(brw_vec8_grf(payload.sample_mask_in_reg, 0),
                          BRW_REGISTER_TYPE_D));
      dest.type = reg.type;
      fs_inst *inst = MOV(dest, reg);
      if (instr->has_predicate)
         inst->predicate = BRW_PREDICATE_NORMAL;
      emit(inst);
      break;
   }

   case nir_intrinsic_load_sample_pos: {
      fs_reg *reg = emit_samplepos_setup();
      dest.type = reg->type;
      emit(MOV(dest, *reg));
      emit(MOV(offset(dest, 1), offset(*reg, 1)));
      break;
   }

   case nir_intrinsic_load_sample_id: {
      fs_reg *reg = emit_sampleid_setup();
      dest.type = reg->type;
      emit(MOV(dest, *reg));
      break;
   }

   case nir_intrinsic_load_uniform_vec1:
   case nir_intrinsic_load_uniform_vec2:
   case nir_intrinsic_load_uniform_vec3:
   case nir_intrinsic_load_uniform_vec4: {
      unsigned index = 0;
      for (int i = 0; i < instr->const_index[1]; i++) {
         for (unsigned j = 0;
            j < nir_intrinsic_infos[instr->intrinsic].dest_components; j++) {
            fs_reg src = nir_uniforms;
            src.reg_offset = instr->const_index[0] + index;
            src.type = dest.type;
            index++;

            fs_inst *inst = MOV(dest, src);
            if (instr->has_predicate)
               inst->predicate = BRW_PREDICATE_NORMAL;
            emit(inst);
            dest.reg_offset++;
         }
      }
      break;
   }

   case nir_intrinsic_load_uniform_vec1_indirect:
   case nir_intrinsic_load_uniform_vec2_indirect:
   case nir_intrinsic_load_uniform_vec3_indirect:
   case nir_intrinsic_load_uniform_vec4_indirect: {
      unsigned index = 0;
      for (int i = 0; i < instr->const_index[1]; i++) {
         for (unsigned j = 0;
            j < nir_intrinsic_infos[instr->intrinsic].dest_components; j++) {
            fs_reg src = nir_uniforms;
            src.reg_offset = instr->const_index[0] + index;
            src.reladdr = new(mem_ctx) fs_reg(get_nir_src(instr->src[0]));
            src.reladdr->type = BRW_REGISTER_TYPE_D;
            src.type = dest.type;
            index++;

            fs_inst *inst = MOV(dest, src);
            if (instr->has_predicate)
               inst->predicate = BRW_PREDICATE_NORMAL;
            emit(inst);
            dest.reg_offset++;
         }
      }
      break;
   }

   case nir_intrinsic_load_ubo_vec1:
   case nir_intrinsic_load_ubo_vec2:
   case nir_intrinsic_load_ubo_vec3:
   case nir_intrinsic_load_ubo_vec4: {
      fs_reg surf_index = fs_reg(prog_data->binding_table.ubo_start +
                                 (unsigned) instr->const_index[0]);
      fs_reg packed_consts = fs_reg(this, glsl_type::float_type);
      packed_consts.type = dest.type;

      fs_reg const_offset_reg = fs_reg((unsigned) instr->const_index[1] & ~15);
      emit(new(mem_ctx) fs_inst(FS_OPCODE_UNIFORM_PULL_CONSTANT_LOAD,
                                packed_consts, surf_index, const_offset_reg));

      for (unsigned i = 0;
           i < nir_intrinsic_infos[instr->intrinsic].dest_components; i++) {
         packed_consts.set_smear(instr->const_index[1] % 16 / 4 + i);

         /* The std140 packing rules don't allow vectors to cross 16-byte
          * boundaries, and a reg is 32 bytes.
          */
         assert(packed_consts.subreg_offset < 32);

         fs_inst *inst = MOV(dest, packed_consts);
         if (instr->has_predicate)
               inst->predicate = BRW_PREDICATE_NORMAL;
         emit(inst);

         dest.reg_offset++;
      }
      break;
   }

   case nir_intrinsic_load_ubo_vec1_indirect:
   case nir_intrinsic_load_ubo_vec2_indirect:
   case nir_intrinsic_load_ubo_vec3_indirect:
   case nir_intrinsic_load_ubo_vec4_indirect: {
      fs_reg surf_index = fs_reg(prog_data->binding_table.ubo_start +
                                 instr->const_index[0]);
      /* Turn the byte offset into a dword offset. */
      unsigned base_offset = instr->const_index[1] / 4;
      fs_reg offset = fs_reg(this, glsl_type::int_type);
      emit(SHR(offset, retype(get_nir_src(instr->src[0]), BRW_REGISTER_TYPE_D),
               fs_reg(2)));

      for (unsigned i = 0;
           i < nir_intrinsic_infos[instr->intrinsic].dest_components; i++) {
         exec_list list = VARYING_PULL_CONSTANT_LOAD(dest, surf_index,
                                                     offset, base_offset + i);
         fs_inst *last_inst = (fs_inst *) list.get_tail();
         if (instr->has_predicate)
               last_inst->predicate = BRW_PREDICATE_NORMAL;
         emit(list);

         dest.reg_offset++;
      }
      break;
   }

   case nir_intrinsic_load_input_vec1:
   case nir_intrinsic_load_input_vec2:
   case nir_intrinsic_load_input_vec3:
   case nir_intrinsic_load_input_vec4: {
      unsigned index = 0;
      for (int i = 0; i < instr->const_index[1]; i++) {
         for (unsigned j = 0;
            j < nir_intrinsic_infos[instr->intrinsic].dest_components; j++) {
            fs_reg src = nir_inputs;
            src.reg_offset = instr->const_index[0] + index;
            src.type = dest.type;
            index++;

            fs_inst *inst = MOV(dest, src);
            if (instr->has_predicate)
               inst->predicate = BRW_PREDICATE_NORMAL;
            emit(inst);
            dest.reg_offset++;
         }
      }
      break;
   }

   case nir_intrinsic_load_input_vec1_indirect:
   case nir_intrinsic_load_input_vec2_indirect:
   case nir_intrinsic_load_input_vec3_indirect:
   case nir_intrinsic_load_input_vec4_indirect: {
      unsigned index = 0;
      for (int i = 0; i < instr->const_index[1]; i++) {
         for (unsigned j = 0;
            j < nir_intrinsic_infos[instr->intrinsic].dest_components; j++) {
            fs_reg src = nir_inputs;
            src.reg_offset = instr->const_index[0] + index;
            src.reladdr = new(mem_ctx) fs_reg(get_nir_src(instr->src[0]));
            src.reladdr->type = BRW_REGISTER_TYPE_D;
            src.type = dest.type;
            index++;

            fs_inst *inst = MOV(dest, src);
            if (instr->has_predicate)
               inst->predicate = BRW_PREDICATE_NORMAL;
            emit(inst);
            dest.reg_offset++;
         }
      }
      break;
   }

   case nir_intrinsic_store_output_vec1:
   case nir_intrinsic_store_output_vec2:
   case nir_intrinsic_store_output_vec3:
   case nir_intrinsic_store_output_vec4: {
      fs_reg src = get_nir_src(instr->src[0]);
      unsigned index = 0;
      for (int i = 0; i < instr->const_index[1]; i++) {
         for (unsigned j = 0;
            j < nir_intrinsic_infos[instr->intrinsic].src_components[0]; j++) {
            fs_reg new_dest = nir_outputs;
            new_dest.reg_offset = instr->const_index[0] + index;
            new_dest.type = src.type;
            index++;
            fs_inst *inst = MOV(new_dest, src);
            if (instr->has_predicate)
               inst->predicate = BRW_PREDICATE_NORMAL;
            emit(inst);
            src.reg_offset++;
         }
      }
      break;
   }

   case nir_intrinsic_store_output_vec1_indirect:
   case nir_intrinsic_store_output_vec2_indirect:
   case nir_intrinsic_store_output_vec3_indirect:
   case nir_intrinsic_store_output_vec4_indirect: {
      fs_reg src = get_nir_src(instr->src[0]);
      fs_reg indirect = get_nir_src(instr->src[1]);
      unsigned index = 0;
      for (int i = 0; i < instr->const_index[1]; i++) {
         for (unsigned j = 0;
            j < nir_intrinsic_infos[instr->intrinsic].src_components[0]; j++) {
            fs_reg new_dest = nir_outputs;
            new_dest.reg_offset = instr->const_index[0] + index;
            new_dest.reladdr = new(mem_ctx) fs_reg(indirect);
            new_dest.type = src.type;
            index++;
            fs_inst *inst = MOV(new_dest, src);
            if (instr->has_predicate)
               inst->predicate = BRW_PREDICATE_NORMAL;
            emit(MOV(new_dest, src));
            src.reg_offset++;
         }
      }
      break;
   }

   default:
      unreachable("unknown intrinsic");
   }
}

void
fs_visitor::nir_emit_texture(nir_tex_instr *instr)
{
   brw_wm_prog_key *key = (brw_wm_prog_key*) this->key;
   unsigned sampler = instr->sampler_index;

   /* FINISHME: We're failing to recompile our programs when the sampler is
    * updated.  This only matters for the texture rectangle scale parameters
    * (pre-gen6, or gen6+ with GL_CLAMP).
    */
   int texunit = prog->SamplerUnits[sampler];

   int gather_component = instr->component;

   bool is_rect = instr->sampler_dim == GLSL_SAMPLER_DIM_RECT;

   bool is_cube_array = instr->sampler_dim == GLSL_SAMPLER_DIM_CUBE &&
                        instr->is_array;

   int lod_components, offset_components = 0;

   fs_reg coordinate, shadow_comparitor, lod, lod2, sample_index, mcs, offset;

   for (unsigned i = 0; i < instr->num_srcs; i++) {
      fs_reg src = get_nir_src(instr->src[i]);
      switch (instr->src_type[i]) {
      case nir_tex_src_bias:
         lod = retype(src, BRW_REGISTER_TYPE_F);
         break;
      case nir_tex_src_comparitor:
         shadow_comparitor = retype(src, BRW_REGISTER_TYPE_F);
         break;
      case nir_tex_src_coord:
         switch (instr->op) {
         case nir_texop_txf:
         case nir_texop_txf_ms:
            coordinate = retype(src, BRW_REGISTER_TYPE_D);
            break;
         default:
            coordinate = retype(src, BRW_REGISTER_TYPE_F);
            break;
         }
         break;
      case nir_tex_src_ddx:
         lod = retype(src, BRW_REGISTER_TYPE_F);
         lod_components = nir_tex_instr_src_size(instr, i);
         break;
      case nir_tex_src_ddy:
         lod2 = retype(src, BRW_REGISTER_TYPE_F);
         break;
      case nir_tex_src_lod:
         switch (instr->op) {
         case nir_texop_txs:
            lod = retype(src, BRW_REGISTER_TYPE_UD);
            break;
         case nir_texop_txf:
            lod = retype(src, BRW_REGISTER_TYPE_D);
            break;
         default:
            lod = retype(src, BRW_REGISTER_TYPE_F);
            break;
         }
         break;
      case nir_tex_src_ms_index:
         sample_index = retype(src, BRW_REGISTER_TYPE_UD);
         break;
      case nir_tex_src_offset:
         offset = retype(src, BRW_REGISTER_TYPE_D);
         if (instr->is_array)
            offset_components = instr->coord_components - 1;
         else
            offset_components = instr->coord_components;
         break;
      case nir_tex_src_projector:
         unreachable("should be lowered");
      case nir_tex_src_sampler_index:
         unreachable("not yet supported");
      default:
         unreachable("unknown texture source");
      }
   }

   if (instr->op == nir_texop_txf_ms) {
      if (brw->gen >= 7 && key->tex.compressed_multisample_layout_mask & (1<<sampler))
         mcs = emit_mcs_fetch(coordinate, instr->coord_components, fs_reg(sampler));
      else
         mcs = fs_reg(0u);
   }

   for (unsigned i = 0; i < 3; i++) {
      if (instr->const_offset[i] != 0) {
         assert(offset_components == 0);
         offset = fs_reg(brw_texture_offset(ctx, instr->const_offset, 3));
         break;
      }
   }

   enum glsl_base_type dest_base_type;
   switch (instr->dest_type) {
   case nir_type_float:
      dest_base_type = GLSL_TYPE_FLOAT;
      break;
   case nir_type_int:
      dest_base_type = GLSL_TYPE_INT;
      break;
   case nir_type_unsigned:
      dest_base_type = GLSL_TYPE_UINT;
      break;
   default:
      unreachable("bad type");
   }

   const glsl_type *dest_type =
      glsl_type::get_instance(dest_base_type, nir_tex_instr_dest_size(instr),
                              1);

   ir_texture_opcode op;
   switch (instr->op) {
   case nir_texop_lod: op = ir_lod; break;
   case nir_texop_query_levels: op = ir_query_levels; break;
   case nir_texop_tex: op = ir_tex; break;
   case nir_texop_tg4: op = ir_tg4; break;
   case nir_texop_txb: op = ir_txb; break;
   case nir_texop_txd: op = ir_txd; break;
   case nir_texop_txf: op = ir_txf; break;
   case nir_texop_txf_ms: op = ir_txf_ms; break;
   case nir_texop_txl: op = ir_txl; break;
   case nir_texop_txs: op = ir_txs; break;
   default:
      unreachable("unknown texture opcode");
   }

   emit_texture(op, dest_type, coordinate, instr->coord_components,
                shadow_comparitor, lod, lod2, lod_components, sample_index,
                offset, offset_components, mcs, gather_component,
                is_cube_array, is_rect, sampler, fs_reg(sampler), texunit);

   fs_reg dest = get_nir_dest(instr->dest);
   dest.type = this->result.type;
   unsigned num_components = nir_tex_instr_dest_size(instr);
   emit_percomp(MOV(dest, this->result), (1 << num_components) - 1);
}

void
fs_visitor::nir_emit_load_const(nir_load_const_instr *instr)
{
   fs_reg dest = get_nir_dest(instr->dest);
   dest.type = BRW_REGISTER_TYPE_UD;
   if (instr->array_elems == 0) {
      for (unsigned i = 0; i < instr->num_components; i++) {
         emit(MOV(dest, fs_reg(instr->value.u[i])));
         dest.reg_offset++;
      }
   } else {
      for (unsigned i = 0; i < instr->array_elems; i++) {
         for (unsigned j = 0; j < instr->num_components; j++) {
            emit(MOV(dest, fs_reg(instr->array[i].u[j])));
            dest.reg_offset++;
         }
      }
   }
}

void
fs_visitor::nir_emit_jump(nir_jump_instr *instr)
{
   switch (instr->type) {
   case nir_jump_break:
      emit(BRW_OPCODE_BREAK);
      break;
   case nir_jump_continue:
      emit(BRW_OPCODE_CONTINUE);
      break;
   case nir_jump_return:
   default:
      unreachable("unknown jump");
   }
}
