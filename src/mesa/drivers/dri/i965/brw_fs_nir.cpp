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

#include "glsl/ir.h"
#include "glsl/ir_optimization.h"
#include "glsl/nir/glsl_to_nir.h"
#include "main/shaderimage.h"
#include "program/prog_to_nir.h"
#include "brw_fs.h"
#include "brw_fs_surface_builder.h"
#include "brw_vec4_gs_visitor.h"
#include "brw_nir.h"
#include "brw_fs_surface_builder.h"
#include "brw_vec4_gs_visitor.h"

using namespace brw;
using namespace brw::surface_access;

void
fs_visitor::emit_nir_code()
{
   /* emit the arrays used for inputs and outputs - load/store intrinsics will
    * be converted to reads/writes of these arrays
    */
   nir_setup_inputs();
   nir_setup_outputs();
   nir_setup_uniforms();
   nir_emit_system_values();

   /* get the main function and emit it */
   nir_foreach_overload(nir, overload) {
      assert(strcmp(overload->function->name, "main") == 0);
      assert(overload->impl);
      nir_emit_impl(overload->impl);
   }
}

void
fs_visitor::nir_setup_inputs()
{
   if (stage != MESA_SHADER_FRAGMENT)
      return;

   nir_inputs = bld.vgrf(BRW_REGISTER_TYPE_F, nir->num_inputs);

   nir_foreach_variable(var, &nir->inputs) {
      fs_reg input = offset(nir_inputs, bld, var->data.driver_location);

      fs_reg reg;
      if (var->data.location == VARYING_SLOT_POS) {
         reg = *emit_fragcoord_interpolation(var->data.pixel_center_integer,
                                             var->data.origin_upper_left);
         emit_percomp(bld, fs_inst(BRW_OPCODE_MOV, bld.dispatch_width(),
                                   input, reg), 0xF);
      } else if (var->data.location == VARYING_SLOT_LAYER) {
         struct brw_reg reg = suboffset(interp_reg(VARYING_SLOT_LAYER, 1), 3);
         reg.type = BRW_REGISTER_TYPE_D;
         bld.emit(FS_OPCODE_CINTERP, retype(input, BRW_REGISTER_TYPE_D), reg);
      } else if (var->data.location == VARYING_SLOT_VIEWPORT) {
         struct brw_reg reg = suboffset(interp_reg(VARYING_SLOT_VIEWPORT, 2), 3);
         reg.type = BRW_REGISTER_TYPE_D;
         bld.emit(FS_OPCODE_CINTERP, retype(input, BRW_REGISTER_TYPE_D), reg);
      } else {
         emit_general_interpolation(input, var->name, var->type,
                                    (glsl_interp_qualifier) var->data.interpolation,
                                    var->data.location, var->data.centroid,
                                    var->data.sample);
      }
   }
}

void
fs_visitor::nir_setup_outputs()
{
   brw_wm_prog_key *key = (brw_wm_prog_key*) this->key;

   nir_outputs = bld.vgrf(BRW_REGISTER_TYPE_F, nir->num_outputs);

   nir_foreach_variable(var, &nir->outputs) {
      fs_reg reg = offset(nir_outputs, bld, var->data.driver_location);

      int vector_elements = var->type->without_array()->vector_elements;

      switch (stage) {
      case MESA_SHADER_VERTEX:
      case MESA_SHADER_GEOMETRY:
         for (unsigned int i = 0; i < ALIGN(type_size_scalar(var->type), 4) / 4; i++) {
            int output = var->data.location + i;
            this->outputs[output] = offset(reg, bld, 4 * i);
            this->output_components[output] = vector_elements;
         }
         break;
      case MESA_SHADER_FRAGMENT:
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
         } else if (var->data.location == FRAG_RESULT_STENCIL) {
            this->frag_stencil = reg;
         } else if (var->data.location == FRAG_RESULT_SAMPLE_MASK) {
            this->sample_mask = reg;
         } else {
            /* gl_FragData or a user-defined FS output */
            assert(var->data.location >= FRAG_RESULT_DATA0 &&
                   var->data.location < FRAG_RESULT_DATA0+BRW_MAX_DRAW_BUFFERS);

            /* General color output. */
            for (unsigned int i = 0; i < MAX2(1, var->type->length); i++) {
               int output = var->data.location - FRAG_RESULT_DATA0 + i;
               this->outputs[output] = offset(reg, bld, vector_elements * i);
               this->output_components[output] = vector_elements;
            }
         }
         break;
      default:
         unreachable("unhandled shader stage");
      }
   }
}

void
fs_visitor::nir_setup_uniforms()
{
   if (dispatch_width != 8)
      return;

   uniforms = nir->num_uniforms;

   nir_foreach_variable(var, &nir->uniforms) {
      /* UBO's and atomics don't take up space in the uniform file */
      if (var->interface_type != NULL || var->type->contains_atomic())
         continue;

      if (type_size_scalar(var->type) > 0)
         param_size[var->data.driver_location] = type_size_scalar(var->type);
   }
}

static bool
emit_system_values_block(nir_block *block, void *void_visitor)
{
   fs_visitor *v = (fs_visitor *)void_visitor;
   fs_reg *reg;

   nir_foreach_instr(block, instr) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_vertex_id:
         unreachable("should be lowered by lower_vertex_id().");

      case nir_intrinsic_load_vertex_id_zero_base:
         assert(v->stage == MESA_SHADER_VERTEX);
         reg = &v->nir_system_values[SYSTEM_VALUE_VERTEX_ID_ZERO_BASE];
         if (reg->file == BAD_FILE)
            *reg = *v->emit_vs_system_value(SYSTEM_VALUE_VERTEX_ID_ZERO_BASE);
         break;

      case nir_intrinsic_load_base_vertex:
         assert(v->stage == MESA_SHADER_VERTEX);
         reg = &v->nir_system_values[SYSTEM_VALUE_BASE_VERTEX];
         if (reg->file == BAD_FILE)
            *reg = *v->emit_vs_system_value(SYSTEM_VALUE_BASE_VERTEX);
         break;

      case nir_intrinsic_load_instance_id:
         assert(v->stage == MESA_SHADER_VERTEX);
         reg = &v->nir_system_values[SYSTEM_VALUE_INSTANCE_ID];
         if (reg->file == BAD_FILE)
            *reg = *v->emit_vs_system_value(SYSTEM_VALUE_INSTANCE_ID);
         break;

      case nir_intrinsic_load_invocation_id:
         assert(v->stage == MESA_SHADER_GEOMETRY);
         reg = &v->nir_system_values[SYSTEM_VALUE_INVOCATION_ID];
         if (reg->file == BAD_FILE) {
            const fs_builder abld = v->bld.annotate("gl_InvocationID", NULL);
            fs_reg g1(retype(brw_vec8_grf(1, 0), BRW_REGISTER_TYPE_UD));
            fs_reg iid = abld.vgrf(BRW_REGISTER_TYPE_UD, 1);
            abld.SHR(iid, g1, fs_reg(27u));
            *reg = iid;
         }
         break;

      case nir_intrinsic_load_sample_pos:
         assert(v->stage == MESA_SHADER_FRAGMENT);
         reg = &v->nir_system_values[SYSTEM_VALUE_SAMPLE_POS];
         if (reg->file == BAD_FILE)
            *reg = *v->emit_samplepos_setup();
         break;

      case nir_intrinsic_load_sample_id:
         assert(v->stage == MESA_SHADER_FRAGMENT);
         reg = &v->nir_system_values[SYSTEM_VALUE_SAMPLE_ID];
         if (reg->file == BAD_FILE)
            *reg = *v->emit_sampleid_setup();
         break;

      case nir_intrinsic_load_sample_mask_in:
         assert(v->stage == MESA_SHADER_FRAGMENT);
         assert(v->devinfo->gen >= 7);
         reg = &v->nir_system_values[SYSTEM_VALUE_SAMPLE_MASK_IN];
         if (reg->file == BAD_FILE)
            *reg = fs_reg(retype(brw_vec8_grf(v->payload.sample_mask_in_reg, 0),
                                 BRW_REGISTER_TYPE_D));
         break;

      case nir_intrinsic_load_local_invocation_id:
         assert(v->stage == MESA_SHADER_COMPUTE);
         reg = &v->nir_system_values[SYSTEM_VALUE_LOCAL_INVOCATION_ID];
         if (reg->file == BAD_FILE)
            *reg = *v->emit_cs_local_invocation_id_setup();
         break;

      case nir_intrinsic_load_work_group_id:
         assert(v->stage == MESA_SHADER_COMPUTE);
         reg = &v->nir_system_values[SYSTEM_VALUE_WORK_GROUP_ID];
         if (reg->file == BAD_FILE)
            *reg = *v->emit_cs_work_group_id_setup();
         break;

      default:
         break;
      }
   }

   return true;
}

void
fs_visitor::nir_emit_system_values()
{
   nir_system_values = ralloc_array(mem_ctx, fs_reg, SYSTEM_VALUE_MAX);
   nir_foreach_overload(nir, overload) {
      assert(strcmp(overload->function->name, "main") == 0);
      assert(overload->impl);
      nir_foreach_block(overload->impl, emit_system_values_block, this);
   }
}

void
fs_visitor::nir_emit_impl(nir_function_impl *impl)
{
   nir_locals = reralloc(mem_ctx, nir_locals, fs_reg, impl->reg_alloc);
   foreach_list_typed(nir_register, reg, node, &impl->registers) {
      unsigned array_elems =
         reg->num_array_elems == 0 ? 1 : reg->num_array_elems;
      unsigned size = array_elems * reg->num_components;
      nir_locals[reg->index] = bld.vgrf(BRW_REGISTER_TYPE_F, size);
   }

   nir_ssa_values = reralloc(mem_ctx, nir_ssa_values, fs_reg,
                             impl->ssa_alloc);

   nir_emit_cf_list(&impl->body);
}

void
fs_visitor::nir_emit_cf_list(exec_list *list)
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
fs_visitor::nir_emit_if(nir_if *if_stmt)
{
   /* first, put the condition into f0 */
   fs_inst *inst = bld.MOV(bld.null_reg_d(),
                            retype(get_nir_src(if_stmt->condition),
                                   BRW_REGISTER_TYPE_D));
   inst->conditional_mod = BRW_CONDITIONAL_NZ;

   bld.IF(BRW_PREDICATE_NORMAL);

   nir_emit_cf_list(&if_stmt->then_list);

   /* note: if the else is empty, dead CF elimination will remove it */
   bld.emit(BRW_OPCODE_ELSE);

   nir_emit_cf_list(&if_stmt->else_list);

   bld.emit(BRW_OPCODE_ENDIF);
}

void
fs_visitor::nir_emit_loop(nir_loop *loop)
{
   bld.emit(BRW_OPCODE_DO);

   nir_emit_cf_list(&loop->body);

   bld.emit(BRW_OPCODE_WHILE);
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
   const fs_builder abld = bld.annotate(NULL, instr);

   switch (instr->type) {
   case nir_instr_type_alu:
      nir_emit_alu(abld, nir_instr_as_alu(instr));
      break;

   case nir_instr_type_intrinsic:
      nir_emit_intrinsic(abld, nir_instr_as_intrinsic(instr));
      break;

   case nir_instr_type_tex:
      nir_emit_texture(abld, nir_instr_as_tex(instr));
      break;

   case nir_instr_type_load_const:
      nir_emit_load_const(abld, nir_instr_as_load_const(instr));
      break;

   case nir_instr_type_ssa_undef:
      nir_emit_undef(abld, nir_instr_as_ssa_undef(instr));
      break;

   case nir_instr_type_jump:
      nir_emit_jump(abld, nir_instr_as_jump(instr));
      break;

   default:
      unreachable("unknown instruction type");
   }
}

bool
fs_visitor::optimize_frontfacing_ternary(nir_alu_instr *instr,
                                         const fs_reg &result)
{
   if (!instr->src[0].src.is_ssa ||
       instr->src[0].src.ssa->parent_instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *src0 =
      nir_instr_as_intrinsic(instr->src[0].src.ssa->parent_instr);

   if (src0->intrinsic != nir_intrinsic_load_front_face)
      return false;

   nir_const_value *value1 = nir_src_as_const_value(instr->src[1].src);
   if (!value1 || fabsf(value1->f[0]) != 1.0f)
      return false;

   nir_const_value *value2 = nir_src_as_const_value(instr->src[2].src);
   if (!value2 || fabsf(value2->f[0]) != 1.0f)
      return false;

   fs_reg tmp = vgrf(glsl_type::int_type);

   if (devinfo->gen >= 6) {
      /* Bit 15 of g0.0 is 0 if the polygon is front facing. */
      fs_reg g0 = fs_reg(retype(brw_vec1_grf(0, 0), BRW_REGISTER_TYPE_W));

      /* For (gl_FrontFacing ? 1.0 : -1.0), emit:
       *
       *    or(8)  tmp.1<2>W  g0.0<0,1,0>W  0x00003f80W
       *    and(8) dst<1>D    tmp<8,8,1>D   0xbf800000D
       *
       * and negate g0.0<0,1,0>W for (gl_FrontFacing ? -1.0 : 1.0).
       *
       * This negation looks like it's safe in practice, because bits 0:4 will
       * surely be TRIANGLES
       */

      if (value1->f[0] == -1.0f) {
         g0.negate = true;
      }

      tmp.type = BRW_REGISTER_TYPE_W;
      tmp.subreg_offset = 2;
      tmp.stride = 2;

      fs_inst *or_inst = bld.OR(tmp, g0, fs_reg(0x3f80));
      or_inst->src[1].type = BRW_REGISTER_TYPE_UW;

      tmp.type = BRW_REGISTER_TYPE_D;
      tmp.subreg_offset = 0;
      tmp.stride = 1;
   } else {
      /* Bit 31 of g1.6 is 0 if the polygon is front facing. */
      fs_reg g1_6 = fs_reg(retype(brw_vec1_grf(1, 6), BRW_REGISTER_TYPE_D));

      /* For (gl_FrontFacing ? 1.0 : -1.0), emit:
       *
       *    or(8)  tmp<1>D  g1.6<0,1,0>D  0x3f800000D
       *    and(8) dst<1>D  tmp<8,8,1>D   0xbf800000D
       *
       * and negate g1.6<0,1,0>D for (gl_FrontFacing ? -1.0 : 1.0).
       *
       * This negation looks like it's safe in practice, because bits 0:4 will
       * surely be TRIANGLES
       */

      if (value1->f[0] == -1.0f) {
         g1_6.negate = true;
      }

      bld.OR(tmp, g1_6, fs_reg(0x3f800000));
   }
   bld.AND(retype(result, BRW_REGISTER_TYPE_D), tmp, fs_reg(0xbf800000));

   return true;
}

void
fs_visitor::nir_emit_alu(const fs_builder &bld, nir_alu_instr *instr)
{
   struct brw_wm_prog_key *fs_key = (struct brw_wm_prog_key *) this->key;
   fs_inst *inst;

   fs_reg result = get_nir_dest(instr->dest.dest);
   result.type = brw_type_for_nir_type(nir_op_infos[instr->op].output_type);

   fs_reg op[4];
   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
      op[i] = get_nir_src(instr->src[i].src);
      op[i].type = brw_type_for_nir_type(nir_op_infos[instr->op].input_types[i]);
      op[i].abs = instr->src[i].abs;
      op[i].negate = instr->src[i].negate;
   }

   /* We get a bunch of mov's out of the from_ssa pass and they may still
    * be vectorized.  We'll handle them as a special-case.  We'll also
    * handle vecN here because it's basically the same thing.
    */
   switch (instr->op) {
   case nir_op_imov:
   case nir_op_fmov:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4: {
      fs_reg temp = result;
      bool need_extra_copy = false;
      for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
         if (!instr->src[i].src.is_ssa &&
             instr->dest.dest.reg.reg == instr->src[i].src.reg.reg) {
            need_extra_copy = true;
            temp = bld.vgrf(result.type, 4);
            break;
         }
      }

      for (unsigned i = 0; i < 4; i++) {
         if (!(instr->dest.write_mask & (1 << i)))
            continue;

         if (instr->op == nir_op_imov || instr->op == nir_op_fmov) {
            inst = bld.MOV(offset(temp, bld, i),
                           offset(op[0], bld, instr->src[0].swizzle[i]));
         } else {
            inst = bld.MOV(offset(temp, bld, i),
                           offset(op[i], bld, instr->src[i].swizzle[0]));
         }
         inst->saturate = instr->dest.saturate;
      }

      /* In this case the source and destination registers were the same,
       * so we need to insert an extra set of moves in order to deal with
       * any swizzling.
       */
      if (need_extra_copy) {
         for (unsigned i = 0; i < 4; i++) {
            if (!(instr->dest.write_mask & (1 << i)))
               continue;

            bld.MOV(offset(result, bld, i), offset(temp, bld, i));
         }
      }
      return;
   }
   default:
      break;
   }

   /* At this point, we have dealt with any instruction that operates on
    * more than a single channel.  Therefore, we can just adjust the source
    * and destination registers for that channel and emit the instruction.
    */
   unsigned channel = 0;
   if (nir_op_infos[instr->op].output_size == 0) {
      /* Since NIR is doing the scalarizing for us, we should only ever see
       * vectorized operations with a single channel.
       */
      assert(_mesa_bitcount(instr->dest.write_mask) == 1);
      channel = ffs(instr->dest.write_mask) - 1;

      result = offset(result, bld, channel);
   }

   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
      assert(nir_op_infos[instr->op].input_sizes[i] < 2);
      op[i] = offset(op[i], bld, instr->src[i].swizzle[channel]);
   }

   switch (instr->op) {
   case nir_op_i2f:
   case nir_op_u2f:
      inst = bld.MOV(result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_f2i:
   case nir_op_f2u:
      bld.MOV(result, op[0]);
      break;

   case nir_op_fsign: {
      /* AND(val, 0x80000000) gives the sign bit.
         *
         * Predicated OR ORs 1.0 (0x3f800000) with the sign bit if val is not
         * zero.
         */
      bld.CMP(bld.null_reg_f(), op[0], fs_reg(0.0f), BRW_CONDITIONAL_NZ);

      fs_reg result_int = retype(result, BRW_REGISTER_TYPE_UD);
      op[0].type = BRW_REGISTER_TYPE_UD;
      result.type = BRW_REGISTER_TYPE_UD;
      bld.AND(result_int, op[0], fs_reg(0x80000000u));

      inst = bld.OR(result_int, result_int, fs_reg(0x3f800000u));
      inst->predicate = BRW_PREDICATE_NORMAL;
      if (instr->dest.saturate) {
         inst = bld.MOV(result, result);
         inst->saturate = true;
      }
      break;
   }

   case nir_op_isign:
      /*  ASR(val, 31) -> negative val generates 0xffffffff (signed -1).
       *               -> non-negative val generates 0x00000000.
       *  Predicated OR sets 1 if val is positive.
       */
      bld.CMP(bld.null_reg_d(), op[0], fs_reg(0), BRW_CONDITIONAL_G);
      bld.ASR(result, op[0], fs_reg(31));
      inst = bld.OR(result, result, fs_reg(1));
      inst->predicate = BRW_PREDICATE_NORMAL;
      break;

   case nir_op_frcp:
      inst = bld.emit(SHADER_OPCODE_RCP, result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_fexp2:
      inst = bld.emit(SHADER_OPCODE_EXP2, result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_flog2:
      inst = bld.emit(SHADER_OPCODE_LOG2, result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_fsin:
      inst = bld.emit(SHADER_OPCODE_SIN, result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_fcos:
      inst = bld.emit(SHADER_OPCODE_COS, result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_fddx:
      if (fs_key->high_quality_derivatives) {
         inst = bld.emit(FS_OPCODE_DDX_FINE, result, op[0]);
      } else {
         inst = bld.emit(FS_OPCODE_DDX_COARSE, result, op[0]);
      }
      inst->saturate = instr->dest.saturate;
      break;
   case nir_op_fddx_fine:
      inst = bld.emit(FS_OPCODE_DDX_FINE, result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;
   case nir_op_fddx_coarse:
      inst = bld.emit(FS_OPCODE_DDX_COARSE, result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;
   case nir_op_fddy:
      if (fs_key->high_quality_derivatives) {
         inst = bld.emit(FS_OPCODE_DDY_FINE, result, op[0],
                         fs_reg(fs_key->render_to_fbo));
      } else {
         inst = bld.emit(FS_OPCODE_DDY_COARSE, result, op[0],
                         fs_reg(fs_key->render_to_fbo));
      }
      inst->saturate = instr->dest.saturate;
      break;
   case nir_op_fddy_fine:
      inst = bld.emit(FS_OPCODE_DDY_FINE, result, op[0],
                      fs_reg(fs_key->render_to_fbo));
      inst->saturate = instr->dest.saturate;
      break;
   case nir_op_fddy_coarse:
      inst = bld.emit(FS_OPCODE_DDY_COARSE, result, op[0],
                      fs_reg(fs_key->render_to_fbo));
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_fadd:
   case nir_op_iadd:
      inst = bld.ADD(result, op[0], op[1]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_fmul:
      inst = bld.MUL(result, op[0], op[1]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_imul:
      bld.MUL(result, op[0], op[1]);
      break;

   case nir_op_imul_high:
   case nir_op_umul_high:
      bld.emit(SHADER_OPCODE_MULH, result, op[0], op[1]);
      break;

   case nir_op_idiv:
   case nir_op_udiv:
      bld.emit(SHADER_OPCODE_INT_QUOTIENT, result, op[0], op[1]);
      break;

   case nir_op_uadd_carry:
      unreachable("Should have been lowered by carry_to_arith().");

   case nir_op_usub_borrow:
      unreachable("Should have been lowered by borrow_to_arith().");

   case nir_op_umod:
      bld.emit(SHADER_OPCODE_INT_REMAINDER, result, op[0], op[1]);
      break;

   case nir_op_flt:
   case nir_op_ilt:
   case nir_op_ult:
      bld.CMP(result, op[0], op[1], BRW_CONDITIONAL_L);
      break;

   case nir_op_fge:
   case nir_op_ige:
   case nir_op_uge:
      bld.CMP(result, op[0], op[1], BRW_CONDITIONAL_GE);
      break;

   case nir_op_feq:
   case nir_op_ieq:
      bld.CMP(result, op[0], op[1], BRW_CONDITIONAL_Z);
      break;

   case nir_op_fne:
   case nir_op_ine:
      bld.CMP(result, op[0], op[1], BRW_CONDITIONAL_NZ);
      break;

   case nir_op_inot:
      if (devinfo->gen >= 8) {
         op[0] = resolve_source_modifiers(op[0]);
      }
      bld.NOT(result, op[0]);
      break;
   case nir_op_ixor:
      if (devinfo->gen >= 8) {
         op[0] = resolve_source_modifiers(op[0]);
         op[1] = resolve_source_modifiers(op[1]);
      }
      bld.XOR(result, op[0], op[1]);
      break;
   case nir_op_ior:
      if (devinfo->gen >= 8) {
         op[0] = resolve_source_modifiers(op[0]);
         op[1] = resolve_source_modifiers(op[1]);
      }
      bld.OR(result, op[0], op[1]);
      break;
   case nir_op_iand:
      if (devinfo->gen >= 8) {
         op[0] = resolve_source_modifiers(op[0]);
         op[1] = resolve_source_modifiers(op[1]);
      }
      bld.AND(result, op[0], op[1]);
      break;

   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4:
   case nir_op_bany2:
   case nir_op_bany3:
   case nir_op_bany4:
   case nir_op_ball2:
   case nir_op_ball3:
   case nir_op_ball4:
   case nir_op_ball_fequal2:
   case nir_op_ball_iequal2:
   case nir_op_ball_fequal3:
   case nir_op_ball_iequal3:
   case nir_op_ball_fequal4:
   case nir_op_ball_iequal4:
   case nir_op_bany_fnequal2:
   case nir_op_bany_inequal2:
   case nir_op_bany_fnequal3:
   case nir_op_bany_inequal3:
   case nir_op_bany_fnequal4:
   case nir_op_bany_inequal4:
      unreachable("Lowered by nir_lower_alu_reductions");

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

   case nir_op_ldexp:
      unreachable("not reached: should be handled by ldexp_to_arith()");

   case nir_op_fsqrt:
      inst = bld.emit(SHADER_OPCODE_SQRT, result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_frsq:
      inst = bld.emit(SHADER_OPCODE_RSQ, result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_b2i:
   case nir_op_b2f:
      bld.MOV(result, negate(op[0]));
      break;

   case nir_op_f2b:
      bld.CMP(result, op[0], fs_reg(0.0f), BRW_CONDITIONAL_NZ);
      break;
   case nir_op_i2b:
      bld.CMP(result, op[0], fs_reg(0), BRW_CONDITIONAL_NZ);
      break;

   case nir_op_ftrunc:
      inst = bld.RNDZ(result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_fceil: {
      op[0].negate = !op[0].negate;
      fs_reg temp = vgrf(glsl_type::float_type);
      bld.RNDD(temp, op[0]);
      temp.negate = true;
      inst = bld.MOV(result, temp);
      inst->saturate = instr->dest.saturate;
      break;
   }
   case nir_op_ffloor:
      inst = bld.RNDD(result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;
   case nir_op_ffract:
      inst = bld.FRC(result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;
   case nir_op_fround_even:
      inst = bld.RNDE(result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_fmin:
   case nir_op_imin:
   case nir_op_umin:
      if (devinfo->gen >= 6) {
         inst = bld.emit(BRW_OPCODE_SEL, result, op[0], op[1]);
         inst->conditional_mod = BRW_CONDITIONAL_L;
      } else {
         bld.CMP(bld.null_reg_d(), op[0], op[1], BRW_CONDITIONAL_L);
         inst = bld.SEL(result, op[0], op[1]);
         inst->predicate = BRW_PREDICATE_NORMAL;
      }
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_fmax:
   case nir_op_imax:
   case nir_op_umax:
      if (devinfo->gen >= 6) {
         inst = bld.emit(BRW_OPCODE_SEL, result, op[0], op[1]);
         inst->conditional_mod = BRW_CONDITIONAL_GE;
      } else {
         bld.CMP(bld.null_reg_d(), op[0], op[1], BRW_CONDITIONAL_GE);
         inst = bld.SEL(result, op[0], op[1]);
         inst->predicate = BRW_PREDICATE_NORMAL;
      }
      inst->saturate = instr->dest.saturate;
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
      inst = bld.emit(FS_OPCODE_UNPACK_HALF_2x16_SPLIT_X, result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;
   case nir_op_unpack_half_2x16_split_y:
      inst = bld.emit(FS_OPCODE_UNPACK_HALF_2x16_SPLIT_Y, result, op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_fpow:
      inst = bld.emit(SHADER_OPCODE_POW, result, op[0], op[1]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_bitfield_reverse:
      bld.BFREV(result, op[0]);
      break;

   case nir_op_bit_count:
      bld.CBIT(result, op[0]);
      break;

   case nir_op_ufind_msb:
   case nir_op_ifind_msb: {
      bld.FBH(retype(result, BRW_REGISTER_TYPE_UD), op[0]);

      /* FBH counts from the MSB side, while GLSL's findMSB() wants the count
       * from the LSB side. If FBH didn't return an error (0xFFFFFFFF), then
       * subtract the result from 31 to convert the MSB count into an LSB count.
       */
      bld.CMP(bld.null_reg_d(), result, fs_reg(-1), BRW_CONDITIONAL_NZ);

      inst = bld.ADD(result, result, fs_reg(31));
      inst->predicate = BRW_PREDICATE_NORMAL;
      inst->src[0].negate = true;
      break;
   }

   case nir_op_find_lsb:
      bld.FBL(result, op[0]);
      break;

   case nir_op_ubitfield_extract:
   case nir_op_ibitfield_extract:
      bld.BFE(result, op[2], op[1], op[0]);
      break;
   case nir_op_bfm:
      bld.BFI1(result, op[0], op[1]);
      break;
   case nir_op_bfi:
      bld.BFI2(result, op[0], op[1], op[2]);
      break;

   case nir_op_bitfield_insert:
      unreachable("not reached: should be handled by "
                  "lower_instructions::bitfield_insert_to_bfm_bfi");

   case nir_op_ishl:
      bld.SHL(result, op[0], op[1]);
      break;
   case nir_op_ishr:
      bld.ASR(result, op[0], op[1]);
      break;
   case nir_op_ushr:
      bld.SHR(result, op[0], op[1]);
      break;

   case nir_op_pack_half_2x16_split:
      bld.emit(FS_OPCODE_PACK_HALF_2x16_SPLIT, result, op[0], op[1]);
      break;

   case nir_op_ffma:
      inst = bld.MAD(result, op[2], op[1], op[0]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_flrp:
      inst = bld.LRP(result, op[0], op[1], op[2]);
      inst->saturate = instr->dest.saturate;
      break;

   case nir_op_bcsel:
      if (optimize_frontfacing_ternary(instr, result))
         return;

      bld.CMP(bld.null_reg_d(), op[0], fs_reg(0), BRW_CONDITIONAL_NZ);
      inst = bld.SEL(result, op[1], op[2]);
      inst->predicate = BRW_PREDICATE_NORMAL;
      break;

   default:
      unreachable("unhandled instruction");
   }

   /* If we need to do a boolean resolve, replace the result with -(x & 1)
    * to sign extend the low bit to 0/~0
    */
   if (devinfo->gen <= 5 &&
       (instr->instr.pass_flags & BRW_NIR_BOOLEAN_MASK) == BRW_NIR_BOOLEAN_NEEDS_RESOLVE) {
      fs_reg masked = vgrf(glsl_type::int_type);
      bld.AND(masked, result, fs_reg(1));
      masked.negate = true;
      bld.MOV(retype(result, BRW_REGISTER_TYPE_D), masked);
   }
}

void
fs_visitor::nir_emit_load_const(const fs_builder &bld,
                                nir_load_const_instr *instr)
{
   fs_reg reg = bld.vgrf(BRW_REGISTER_TYPE_D, instr->def.num_components);

   for (unsigned i = 0; i < instr->def.num_components; i++)
      bld.MOV(offset(reg, bld, i), fs_reg(instr->value.i[i]));

   nir_ssa_values[instr->def.index] = reg;
}

void
fs_visitor::nir_emit_undef(const fs_builder &bld, nir_ssa_undef_instr *instr)
{
   nir_ssa_values[instr->def.index] = bld.vgrf(BRW_REGISTER_TYPE_D,
                                               instr->def.num_components);
}

static fs_reg
fs_reg_for_nir_reg(fs_visitor *v, nir_register *nir_reg,
                   unsigned base_offset, nir_src *indirect)
{
   fs_reg reg;

   assert(!nir_reg->is_global);

   reg = v->nir_locals[nir_reg->index];

   reg = offset(reg, v->bld, base_offset * nir_reg->num_components);
   if (indirect) {
      int multiplier = nir_reg->num_components * (v->dispatch_width / 8);

      reg.reladdr = new(v->mem_ctx) fs_reg(v->vgrf(glsl_type::int_type));
      v->bld.MUL(*reg.reladdr, v->get_nir_src(*indirect),
                 fs_reg(multiplier));
   }

   return reg;
}

fs_reg
fs_visitor::get_nir_src(nir_src src)
{
   fs_reg reg;
   if (src.is_ssa) {
      reg = nir_ssa_values[src.ssa->index];
   } else {
      reg = fs_reg_for_nir_reg(this, src.reg.reg, src.reg.base_offset,
                               src.reg.indirect);
   }

   /* to avoid floating-point denorm flushing problems, set the type by
    * default to D - instructions that need floating point semantics will set
    * this to F if they need to
    */
   return retype(reg, BRW_REGISTER_TYPE_D);
}

fs_reg
fs_visitor::get_nir_dest(nir_dest dest)
{
   if (dest.is_ssa) {
      nir_ssa_values[dest.ssa.index] = bld.vgrf(BRW_REGISTER_TYPE_F,
                                                dest.ssa.num_components);
      return nir_ssa_values[dest.ssa.index];
   }

   return fs_reg_for_nir_reg(this, dest.reg.reg, dest.reg.base_offset,
                             dest.reg.indirect);
}

fs_reg
fs_visitor::get_nir_image_deref(const nir_deref_var *deref)
{
   fs_reg image(UNIFORM, deref->var->data.driver_location,
                BRW_REGISTER_TYPE_UD);

   if (deref->deref.child) {
      const nir_deref_array *deref_array =
         nir_deref_as_array(deref->deref.child);
      assert(deref->deref.child->deref_type == nir_deref_type_array &&
             deref_array->deref.child == NULL);
      const unsigned size = glsl_get_length(deref->var->type);
      const unsigned base = MIN2(deref_array->base_offset, size - 1);

      image = offset(image, bld, base * BRW_IMAGE_PARAM_SIZE);

      if (deref_array->deref_array_type == nir_deref_array_type_indirect) {
         fs_reg *tmp = new(mem_ctx) fs_reg(vgrf(glsl_type::int_type));

         if (devinfo->gen == 7 && !devinfo->is_haswell) {
            /* IVB hangs when trying to access an invalid surface index with
             * the dataport.  According to the spec "if the index used to
             * select an individual element is negative or greater than or
             * equal to the size of the array, the results of the operation
             * are undefined but may not lead to termination" -- which is one
             * of the possible outcomes of the hang.  Clamp the index to
             * prevent access outside of the array bounds.
             */
            bld.emit_minmax(*tmp, retype(get_nir_src(deref_array->indirect),
                                         BRW_REGISTER_TYPE_UD),
                            fs_reg(size - base - 1), BRW_CONDITIONAL_L);
         } else {
            bld.MOV(*tmp, get_nir_src(deref_array->indirect));
         }

         bld.MUL(*tmp, *tmp, fs_reg(BRW_IMAGE_PARAM_SIZE));
         image.reladdr = tmp;
      }
   }

   return image;
}

void
fs_visitor::emit_percomp(const fs_builder &bld, const fs_inst &inst,
                         unsigned wr_mask)
{
   for (unsigned i = 0; i < 4; i++) {
      if (!((wr_mask >> i) & 1))
         continue;

      fs_inst *new_inst = new(mem_ctx) fs_inst(inst);
      new_inst->dst = offset(new_inst->dst, bld, i);
      for (unsigned j = 0; j < new_inst->sources; j++)
         if (new_inst->src[j].file == GRF)
            new_inst->src[j] = offset(new_inst->src[j], bld, i);

      bld.emit(new_inst);
   }
}

/**
 * Get the matching channel register datatype for an image intrinsic of the
 * specified GLSL image type.
 */
static brw_reg_type
get_image_base_type(const glsl_type *type)
{
   switch ((glsl_base_type)type->sampler_type) {
   case GLSL_TYPE_UINT:
      return BRW_REGISTER_TYPE_UD;
   case GLSL_TYPE_INT:
      return BRW_REGISTER_TYPE_D;
   case GLSL_TYPE_FLOAT:
      return BRW_REGISTER_TYPE_F;
   default:
      unreachable("Not reached.");
   }
}

/**
 * Get the appropriate atomic op for an image atomic intrinsic.
 */
static unsigned
get_image_atomic_op(nir_intrinsic_op op, const glsl_type *type)
{
   switch (op) {
   case nir_intrinsic_image_atomic_add:
      return BRW_AOP_ADD;
   case nir_intrinsic_image_atomic_min:
      return (get_image_base_type(type) == BRW_REGISTER_TYPE_D ?
              BRW_AOP_IMIN : BRW_AOP_UMIN);
   case nir_intrinsic_image_atomic_max:
      return (get_image_base_type(type) == BRW_REGISTER_TYPE_D ?
              BRW_AOP_IMAX : BRW_AOP_UMAX);
   case nir_intrinsic_image_atomic_and:
      return BRW_AOP_AND;
   case nir_intrinsic_image_atomic_or:
      return BRW_AOP_OR;
   case nir_intrinsic_image_atomic_xor:
      return BRW_AOP_XOR;
   case nir_intrinsic_image_atomic_exchange:
      return BRW_AOP_MOV;
   case nir_intrinsic_image_atomic_comp_swap:
      return BRW_AOP_CMPWR;
   default:
      unreachable("Not reachable.");
   }
}

static fs_inst *
emit_pixel_interpolater_send(const fs_builder &bld,
                             enum opcode opcode,
                             const fs_reg &dst,
                             const fs_reg &src,
                             const fs_reg &desc,
                             glsl_interp_qualifier interpolation)
{
   fs_inst *inst;
   fs_reg payload;
   int mlen;

   if (src.file == BAD_FILE) {
      /* Dummy payload */
      payload = bld.vgrf(BRW_REGISTER_TYPE_F, 1);
      mlen = 1;
   } else {
      payload = src;
      mlen = 2 * bld.dispatch_width() / 8;
   }

   inst = bld.emit(opcode, dst, payload, desc);
   inst->mlen = mlen;
   /* 2 floats per slot returned */
   inst->regs_written = 2 * bld.dispatch_width() / 8;
   inst->pi_noperspective = interpolation == INTERP_QUALIFIER_NOPERSPECTIVE;

   return inst;
}

/**
 * Computes 1 << x, given a D/UD register containing some value x.
 */
static fs_reg
intexp2(const fs_builder &bld, const fs_reg &x)
{
   assert(x.type == BRW_REGISTER_TYPE_UD || x.type == BRW_REGISTER_TYPE_D);

   fs_reg result = bld.vgrf(x.type, 1);
   fs_reg one = bld.vgrf(x.type, 1);

   bld.MOV(one, retype(fs_reg(1), one.type));
   bld.SHL(result, one, x);
   return result;
}

void
fs_visitor::emit_gs_end_primitive(const nir_src &vertex_count_nir_src)
{
   assert(stage == MESA_SHADER_GEOMETRY);

   struct brw_gs_prog_data *gs_prog_data =
      (struct brw_gs_prog_data *) prog_data;

   /* We can only do EndPrimitive() functionality when the control data
    * consists of cut bits.  Fortunately, the only time it isn't is when the
    * output type is points, in which case EndPrimitive() is a no-op.
    */
   if (gs_prog_data->control_data_format !=
       GEN7_GS_CONTROL_DATA_FORMAT_GSCTL_CUT) {
      return;
   }

   /* Cut bits use one bit per vertex. */
   assert(gs_compile->control_data_bits_per_vertex == 1);

   fs_reg vertex_count = get_nir_src(vertex_count_nir_src);
   vertex_count.type = BRW_REGISTER_TYPE_UD;

   /* Cut bit n should be set to 1 if EndPrimitive() was called after emitting
    * vertex n, 0 otherwise.  So all we need to do here is mark bit
    * (vertex_count - 1) % 32 in the cut_bits register to indicate that
    * EndPrimitive() was called after emitting vertex (vertex_count - 1);
    * vec4_gs_visitor::emit_control_data_bits() will take care of the rest.
    *
    * Note that if EndPrimitive() is called before emitting any vertices, this
    * will cause us to set bit 31 of the control_data_bits register to 1.
    * That's fine because:
    *
    * - If max_vertices < 32, then vertex number 31 (zero-based) will never be
    *   output, so the hardware will ignore cut bit 31.
    *
    * - If max_vertices == 32, then vertex number 31 is guaranteed to be the
    *   last vertex, so setting cut bit 31 has no effect (since the primitive
    *   is automatically ended when the GS terminates).
    *
    * - If max_vertices > 32, then the ir_emit_vertex visitor will reset the
    *   control_data_bits register to 0 when the first vertex is emitted.
    */

   const fs_builder abld = bld.annotate("end primitive");

   /* control_data_bits |= 1 << ((vertex_count - 1) % 32) */
   fs_reg prev_count = bld.vgrf(BRW_REGISTER_TYPE_UD, 1);
   abld.ADD(prev_count, vertex_count, fs_reg(0xffffffffu));
   fs_reg mask = intexp2(abld, prev_count);
   /* Note: we're relying on the fact that the GEN SHL instruction only pays
    * attention to the lower 5 bits of its second source argument, so on this
    * architecture, 1 << (vertex_count - 1) is equivalent to 1 <<
    * ((vertex_count - 1) % 32).
    */
   abld.OR(this->control_data_bits, this->control_data_bits, mask);
}

void
fs_visitor::emit_gs_control_data_bits(const fs_reg &vertex_count)
{
   assert(stage == MESA_SHADER_GEOMETRY);
   assert(gs_compile->control_data_bits_per_vertex != 0);

   struct brw_gs_prog_data *gs_prog_data =
      (struct brw_gs_prog_data *) prog_data;

   const fs_builder abld = bld.annotate("emit control data bits");
   const fs_builder fwa_bld = bld.exec_all();

   /* We use a single UD register to accumulate control data bits (32 bits
    * for each of the SIMD8 channels).  So we need to write a DWord (32 bits)
    * at a time.
    *
    * Unfortunately, the URB_WRITE_SIMD8 message uses 128-bit (OWord) offsets.
    * We have select a 128-bit group via the Global and Per-Slot Offsets, then
    * use the Channel Mask phase to enable/disable which DWord within that
    * group to write.  (Remember, different SIMD8 channels may have emitted
    * different numbers of vertices, so we may need per-slot offsets.)
    *
    * Channel masking presents an annoying problem: we may have to replicate
    * the data up to 4 times:
    *
    * Msg = Handles, Per-Slot Offsets, Channel Masks, Data, Data, Data, Data.
    *
    * To avoid penalizing shaders that emit a small number of vertices, we
    * can avoid these sometimes: if the size of the control data header is
    * <= 128 bits, then there is only 1 OWord.  All SIMD8 channels will land
    * land in the same 128-bit group, so we can skip per-slot offsets.
    *
    * Similarly, if the control data header is <= 32 bits, there is only one
    * DWord, so we can skip channel masks.
    */
   enum opcode opcode = SHADER_OPCODE_URB_WRITE_SIMD8;

   fs_reg channel_mask, per_slot_offset;

   if (gs_compile->control_data_header_size_bits > 32) {
      opcode = SHADER_OPCODE_URB_WRITE_SIMD8_MASKED;
      channel_mask = vgrf(glsl_type::uint_type);
   }

   if (gs_compile->control_data_header_size_bits > 128) {
      opcode = SHADER_OPCODE_URB_WRITE_SIMD8_MASKED_PER_SLOT;
      per_slot_offset = vgrf(glsl_type::uint_type);
   }

   /* Figure out which DWord we're trying to write to using the formula:
    *
    *    dword_index = (vertex_count - 1) * bits_per_vertex / 32
    *
    * Since bits_per_vertex is a power of two, and is known at compile
    * time, this can be optimized to:
    *
    *    dword_index = (vertex_count - 1) >> (6 - log2(bits_per_vertex))
    */
   if (opcode != SHADER_OPCODE_URB_WRITE_SIMD8) {
      fs_reg dword_index = bld.vgrf(BRW_REGISTER_TYPE_UD, 1);
      fs_reg prev_count = bld.vgrf(BRW_REGISTER_TYPE_UD, 1);
      abld.ADD(prev_count, vertex_count, fs_reg(0xffffffffu));
      unsigned log2_bits_per_vertex =
         _mesa_fls(gs_compile->control_data_bits_per_vertex);
      abld.SHR(dword_index, prev_count, fs_reg(6u - log2_bits_per_vertex));

      if (per_slot_offset.file != BAD_FILE) {
         /* Set the per-slot offset to dword_index / 4, so that we'll write to
          * the appropriate OWord within the control data header.
          */
         abld.SHR(per_slot_offset, dword_index, fs_reg(2u));
      }

      /* Set the channel masks to 1 << (dword_index % 4), so that we'll
       * write to the appropriate DWORD within the OWORD.
       */
      fs_reg channel = bld.vgrf(BRW_REGISTER_TYPE_UD, 1);
      fwa_bld.AND(channel, dword_index, fs_reg(3u));
      channel_mask = intexp2(fwa_bld, channel);
      /* Then the channel masks need to be in bits 23:16. */
      fwa_bld.SHL(channel_mask, channel_mask, fs_reg(16u));
   }

   /* Store the control data bits in the message payload and send it. */
   int mlen = 2;
   if (channel_mask.file != BAD_FILE)
      mlen += 4; /* channel masks, plus 3 extra copies of the data */
   if (per_slot_offset.file != BAD_FILE)
      mlen++;

   fs_reg payload = bld.vgrf(BRW_REGISTER_TYPE_UD, mlen);
   fs_reg *sources = ralloc_array(mem_ctx, fs_reg, mlen);
   int i = 0;
   sources[i++] = fs_reg(retype(brw_vec8_grf(1, 0), BRW_REGISTER_TYPE_UD));
   if (per_slot_offset.file != BAD_FILE)
      sources[i++] = per_slot_offset;
   if (channel_mask.file != BAD_FILE)
      sources[i++] = channel_mask;
   while (i < mlen) {
      sources[i++] = this->control_data_bits;
   }

   abld.LOAD_PAYLOAD(payload, sources, mlen, mlen);
   fs_inst *inst = abld.emit(opcode, reg_undef, payload);
   inst->mlen = mlen;
   /* We need to increment Global Offset by 256-bits to make room for
    * Broadwell's extra "Vertex Count" payload at the beginning of the
    * URB entry.  Since this is an OWord message, Global Offset is counted
    * in 128-bit units, so we must set it to 2.
    */
   if (gs_prog_data->static_vertex_count == -1)
      inst->offset = 2;
}

void
fs_visitor::set_gs_stream_control_data_bits(const fs_reg &vertex_count,
                                            unsigned stream_id)
{
   /* control_data_bits |= stream_id << ((2 * (vertex_count - 1)) % 32) */

   /* Note: we are calling this *before* increasing vertex_count, so
    * this->vertex_count == vertex_count - 1 in the formula above.
    */

   /* Stream mode uses 2 bits per vertex */
   assert(gs_compile->control_data_bits_per_vertex == 2);

   /* Must be a valid stream */
   assert(stream_id >= 0 && stream_id < MAX_VERTEX_STREAMS);

   /* Control data bits are initialized to 0 so we don't have to set any
    * bits when sending vertices to stream 0.
    */
   if (stream_id == 0)
      return;

   const fs_builder abld = bld.annotate("set stream control data bits", NULL);

   /* reg::sid = stream_id */
   fs_reg sid = bld.vgrf(BRW_REGISTER_TYPE_UD, 1);
   abld.MOV(sid, fs_reg(stream_id));

   /* reg:shift_count = 2 * (vertex_count - 1) */
   fs_reg shift_count = bld.vgrf(BRW_REGISTER_TYPE_UD, 1);
   abld.SHL(shift_count, vertex_count, fs_reg(1u));

   /* Note: we're relying on the fact that the GEN SHL instruction only pays
    * attention to the lower 5 bits of its second source argument, so on this
    * architecture, stream_id << 2 * (vertex_count - 1) is equivalent to
    * stream_id << ((2 * (vertex_count - 1)) % 32).
    */
   fs_reg mask = bld.vgrf(BRW_REGISTER_TYPE_UD, 1);
   abld.SHL(mask, sid, shift_count);
   abld.OR(this->control_data_bits, this->control_data_bits, mask);
}

void
fs_visitor::emit_gs_vertex(const nir_src &vertex_count_nir_src,
                           unsigned stream_id)
{
   assert(stage == MESA_SHADER_GEOMETRY);

   struct brw_gs_prog_data *gs_prog_data =
      (struct brw_gs_prog_data *) prog_data;

   fs_reg vertex_count = get_nir_src(vertex_count_nir_src);
   vertex_count.type = BRW_REGISTER_TYPE_UD;

   /* Haswell and later hardware ignores the "Render Stream Select" bits
    * from the 3DSTATE_STREAMOUT packet when the SOL stage is disabled,
    * and instead sends all primitives down the pipeline for rasterization.
    * If the SOL stage is enabled, "Render Stream Select" is honored and
    * primitives bound to non-zero streams are discarded after stream output.
    *
    * Since the only purpose of primives sent to non-zero streams is to
    * be recorded by transform feedback, we can simply discard all geometry
    * bound to these streams when transform feedback is disabled.
    */
   if (stream_id > 0 && !nir->info.has_transform_feedback_varyings)
      return;

   /* If we're outputting 32 control data bits or less, then we can wait
    * until the shader is over to output them all.  Otherwise we need to
    * output them as we go.  Now is the time to do it, since we're about to
    * output the vertex_count'th vertex, so it's guaranteed that the
    * control data bits associated with the (vertex_count - 1)th vertex are
    * correct.
    */
   if (gs_compile->control_data_header_size_bits > 32) {
      const fs_builder abld =
         bld.annotate("emit vertex: emit control data bits");

      /* Only emit control data bits if we've finished accumulating a batch
       * of 32 bits.  This is the case when:
       *
       *     (vertex_count * bits_per_vertex) % 32 == 0
       *
       * (in other words, when the last 5 bits of vertex_count *
       * bits_per_vertex are 0).  Assuming bits_per_vertex == 2^n for some
       * integer n (which is always the case, since bits_per_vertex is
       * always 1 or 2), this is equivalent to requiring that the last 5-n
       * bits of vertex_count are 0:
       *
       *     vertex_count & (2^(5-n) - 1) == 0
       *
       * 2^(5-n) == 2^5 / 2^n == 32 / bits_per_vertex, so this is
       * equivalent to:
       *
       *     vertex_count & (32 / bits_per_vertex - 1) == 0
       *
       * TODO: If vertex_count is an immediate, we could do some of this math
       *       at compile time...
       */
      fs_inst *inst =
         abld.AND(bld.null_reg_d(), vertex_count,
                  fs_reg(32u / gs_compile->control_data_bits_per_vertex - 1u));
      inst->conditional_mod = BRW_CONDITIONAL_Z;

      abld.IF(BRW_PREDICATE_NORMAL);
      /* If vertex_count is 0, then no control data bits have been
       * accumulated yet, so we can skip emitting them.
       */
      abld.CMP(bld.null_reg_d(), vertex_count, fs_reg(0u),
               BRW_CONDITIONAL_NEQ);
      abld.IF(BRW_PREDICATE_NORMAL);
      emit_gs_control_data_bits(vertex_count);
      abld.emit(BRW_OPCODE_ENDIF);

      /* Reset control_data_bits to 0 so we can start accumulating a new
       * batch.
       *
       * Note: in the case where vertex_count == 0, this neutralizes the
       * effect of any call to EndPrimitive() that the shader may have
       * made before outputting its first vertex.
       */
      inst = abld.MOV(this->control_data_bits, fs_reg(0u));
      inst->force_writemask_all = true;
      abld.emit(BRW_OPCODE_ENDIF);
   }

   emit_urb_writes(vertex_count);

   /* In stream mode we have to set control data bits for all vertices
    * unless we have disabled control data bits completely (which we do
    * do for GL_POINTS outputs that don't use streams).
    */
   if (gs_compile->control_data_header_size_bits > 0 &&
       gs_prog_data->control_data_format ==
          GEN7_GS_CONTROL_DATA_FORMAT_GSCTL_SID) {
      set_gs_stream_control_data_bits(vertex_count, stream_id);
   }
}

void
fs_visitor::emit_gs_input_load(const fs_reg &dst,
                               const nir_src &vertex_src,
                               unsigned input_offset,
                               unsigned num_components)
{
   const brw_vue_prog_data *vue_prog_data = (const brw_vue_prog_data *) prog_data;
   const unsigned vertex = nir_src_as_const_value(vertex_src)->u[0];

   const unsigned array_stride = vue_prog_data->urb_read_length * 8;

   const bool pushed = 4 * input_offset < array_stride;

   if (input_offset == 0) {
      /* This is the VUE header, containing VARYING_SLOT_LAYER [.y],
       * VARYING_SLOT_VIEWPORT [.z], and VARYING_SLOT_PSIZ [.w].
       * Only gl_PointSize is available as a GS input, so they must
       * be asking for that input.
       */
      if (pushed) {
         bld.MOV(dst, fs_reg(ATTR, array_stride * vertex + 3, dst.type));
      } else {
         fs_reg tmp = bld.vgrf(dst.type, 4);
         fs_inst *inst = bld.emit(SHADER_OPCODE_URB_READ_SIMD8, tmp,
                                  fs_reg(vertex), fs_reg(0));
         inst->regs_written = 4;
         bld.MOV(dst, offset(tmp, bld, 3));
      }
   } else {
      if (pushed) {
         int index = vertex * array_stride + 4 * input_offset;
         for (unsigned i = 0; i < num_components; i++) {
            bld.MOV(offset(dst, bld, i), fs_reg(ATTR, index + i, dst.type));
         }
      } else {
         fs_inst *inst = bld.emit(SHADER_OPCODE_URB_READ_SIMD8, dst,
                                  fs_reg(vertex), fs_reg(input_offset));
         inst->regs_written = num_components;
      }
   }
}

void
fs_visitor::nir_emit_intrinsic(const fs_builder &bld, nir_intrinsic_instr *instr)
{
   fs_reg dest;
   if (nir_intrinsic_infos[instr->intrinsic].has_dest)
      dest = get_nir_dest(instr->dest);

   bool has_indirect = false;

   switch (instr->intrinsic) {
   case nir_intrinsic_discard:
   case nir_intrinsic_discard_if: {
      /* We track our discarded pixels in f0.1.  By predicating on it, we can
       * update just the flag bits that aren't yet discarded.  If there's no
       * condition, we emit a CMP of g0 != g0, so all currently executing
       * channels will get turned off.
       */
      fs_inst *cmp;
      if (instr->intrinsic == nir_intrinsic_discard_if) {
         cmp = bld.CMP(bld.null_reg_f(), get_nir_src(instr->src[0]),
                       fs_reg(0), BRW_CONDITIONAL_Z);
      } else {
         fs_reg some_reg = fs_reg(retype(brw_vec8_grf(0, 0),
                                       BRW_REGISTER_TYPE_UW));
         cmp = bld.CMP(bld.null_reg_f(), some_reg, some_reg, BRW_CONDITIONAL_NZ);
      }
      cmp->predicate = BRW_PREDICATE_NORMAL;
      cmp->flag_subreg = 1;

      if (devinfo->gen >= 6) {
         emit_discard_jump();
      }
      break;
   }

   case nir_intrinsic_atomic_counter_inc:
   case nir_intrinsic_atomic_counter_dec:
   case nir_intrinsic_atomic_counter_read: {
      using namespace surface_access;

      /* Get the arguments of the atomic intrinsic. */
      const fs_reg offset = get_nir_src(instr->src[0]);
      const unsigned surface = (stage_prog_data->binding_table.abo_start +
                                instr->const_index[0]);
      fs_reg tmp;

      /* Emit a surface read or atomic op. */
      switch (instr->intrinsic) {
      case nir_intrinsic_atomic_counter_read:
         tmp = emit_untyped_read(bld, fs_reg(surface), offset, 1, 1);
         break;

      case nir_intrinsic_atomic_counter_inc:
         tmp = emit_untyped_atomic(bld, fs_reg(surface), offset, fs_reg(),
                                   fs_reg(), 1, 1, BRW_AOP_INC);
         break;

      case nir_intrinsic_atomic_counter_dec:
         tmp = emit_untyped_atomic(bld, fs_reg(surface), offset, fs_reg(),
                                   fs_reg(), 1, 1, BRW_AOP_PREDEC);
         break;

      default:
         unreachable("Unreachable");
      }

      /* Assign the result. */
      bld.MOV(retype(dest, BRW_REGISTER_TYPE_UD), tmp);

      /* Mark the surface as used. */
      brw_mark_surface_used(stage_prog_data, surface);
      break;
   }

   case nir_intrinsic_image_load:
   case nir_intrinsic_image_store:
   case nir_intrinsic_image_atomic_add:
   case nir_intrinsic_image_atomic_min:
   case nir_intrinsic_image_atomic_max:
   case nir_intrinsic_image_atomic_and:
   case nir_intrinsic_image_atomic_or:
   case nir_intrinsic_image_atomic_xor:
   case nir_intrinsic_image_atomic_exchange:
   case nir_intrinsic_image_atomic_comp_swap: {
      using namespace image_access;

      /* Get the referenced image variable and type. */
      const nir_variable *var = instr->variables[0]->var;
      const glsl_type *type = var->type->without_array();
      const brw_reg_type base_type = get_image_base_type(type);

      /* Get some metadata from the image intrinsic. */
      const nir_intrinsic_info *info = &nir_intrinsic_infos[instr->intrinsic];
      const unsigned arr_dims = type->sampler_array ? 1 : 0;
      const unsigned surf_dims = type->coordinate_components() - arr_dims;
      const mesa_format format =
         (var->data.image.write_only ? MESA_FORMAT_NONE :
          _mesa_get_shader_image_format(var->data.image.format));

      /* Get the arguments of the image intrinsic. */
      const fs_reg image = get_nir_image_deref(instr->variables[0]);
      const fs_reg addr = retype(get_nir_src(instr->src[0]),
                                 BRW_REGISTER_TYPE_UD);
      const fs_reg src0 = (info->num_srcs >= 3 ?
                           retype(get_nir_src(instr->src[2]), base_type) :
                           fs_reg());
      const fs_reg src1 = (info->num_srcs >= 4 ?
                           retype(get_nir_src(instr->src[3]), base_type) :
                           fs_reg());
      fs_reg tmp;

      /* Emit an image load, store or atomic op. */
      if (instr->intrinsic == nir_intrinsic_image_load)
         tmp = emit_image_load(bld, image, addr, surf_dims, arr_dims, format);

      else if (instr->intrinsic == nir_intrinsic_image_store)
         emit_image_store(bld, image, addr, src0, surf_dims, arr_dims, format);

      else
         tmp = emit_image_atomic(bld, image, addr, src0, src1,
                                 surf_dims, arr_dims, info->dest_components,
                                 get_image_atomic_op(instr->intrinsic, type));

      /* Assign the result. */
      for (unsigned c = 0; c < info->dest_components; ++c)
         bld.MOV(offset(retype(dest, base_type), bld, c),
                 offset(tmp, bld, c));
      break;
   }

   case nir_intrinsic_memory_barrier: {
      const fs_reg tmp = bld.vgrf(BRW_REGISTER_TYPE_UD, 16 / dispatch_width);
      bld.emit(SHADER_OPCODE_MEMORY_FENCE, tmp)
         ->regs_written = 2;
      break;
   }

   case nir_intrinsic_shader_clock: {
      /* We cannot do anything if there is an event, so ignore it for now */
      fs_reg shader_clock = get_timestamp(bld);
      const fs_reg srcs[] = { shader_clock.set_smear(0), shader_clock.set_smear(1) };

      bld.LOAD_PAYLOAD(dest, srcs, ARRAY_SIZE(srcs), 0);
      break;
   }

   case nir_intrinsic_image_size: {
      /* Get the referenced image variable and type. */
      const nir_variable *var = instr->variables[0]->var;
      const glsl_type *type = var->type->without_array();

      /* Get the size of the image. */
      const fs_reg image = get_nir_image_deref(instr->variables[0]);
      const fs_reg size = offset(image, bld, BRW_IMAGE_PARAM_SIZE_OFFSET);

      /* For 1DArray image types, the array index is stored in the Z component.
       * Fix this by swizzling the Z component to the Y component.
       */
      const bool is_1d_array_image =
                  type->sampler_dimensionality == GLSL_SAMPLER_DIM_1D &&
                  type->sampler_array;

      /* For CubeArray images, we should count the number of cubes instead
       * of the number of faces. Fix it by dividing the (Z component) by 6.
       */
      const bool is_cube_array_image =
                  type->sampler_dimensionality == GLSL_SAMPLER_DIM_CUBE &&
                  type->sampler_array;

      /* Copy all the components. */
      const nir_intrinsic_info *info = &nir_intrinsic_infos[instr->intrinsic];
      for (unsigned c = 0; c < info->dest_components; ++c) {
         if ((int)c >= type->coordinate_components()) {
             bld.MOV(offset(retype(dest, BRW_REGISTER_TYPE_D), bld, c),
                     fs_reg(1));
         } else if (c == 1 && is_1d_array_image) {
            bld.MOV(offset(retype(dest, BRW_REGISTER_TYPE_D), bld, c),
                    offset(size, bld, 2));
         } else if (c == 2 && is_cube_array_image) {
            bld.emit(SHADER_OPCODE_INT_QUOTIENT,
                     offset(retype(dest, BRW_REGISTER_TYPE_D), bld, c),
                     offset(size, bld, c), fs_reg(6));
         } else {
            bld.MOV(offset(retype(dest, BRW_REGISTER_TYPE_D), bld, c),
                    offset(size, bld, c));
         }
       }

      break;
   }

   case nir_intrinsic_image_samples:
      /* The driver does not support multi-sampled images. */
      bld.MOV(retype(dest, BRW_REGISTER_TYPE_D), fs_reg(1));
      break;

   case nir_intrinsic_load_front_face:
      bld.MOV(retype(dest, BRW_REGISTER_TYPE_D),
              *emit_frontfacing_interpolation());
      break;

   case nir_intrinsic_load_vertex_id:
      unreachable("should be lowered by lower_vertex_id()");

   case nir_intrinsic_load_primitive_id:
      assert(stage == MESA_SHADER_GEOMETRY);
      assert(((struct brw_gs_prog_data *)prog_data)->include_primitive_id);
      bld.MOV(retype(dest, BRW_REGISTER_TYPE_UD),
              retype(fs_reg(brw_vec8_grf(2, 0)), BRW_REGISTER_TYPE_UD));
      break;

   case nir_intrinsic_load_vertex_id_zero_base:
   case nir_intrinsic_load_base_vertex:
   case nir_intrinsic_load_instance_id:
   case nir_intrinsic_load_invocation_id:
   case nir_intrinsic_load_sample_mask_in:
   case nir_intrinsic_load_sample_id: {
      gl_system_value sv = nir_system_value_from_intrinsic(instr->intrinsic);
      fs_reg val = nir_system_values[sv];
      assert(val.file != BAD_FILE);
      dest.type = val.type;
      bld.MOV(dest, val);
      break;
   }

   case nir_intrinsic_load_sample_pos: {
      fs_reg sample_pos = nir_system_values[SYSTEM_VALUE_SAMPLE_POS];
      assert(sample_pos.file != BAD_FILE);
      dest.type = sample_pos.type;
      bld.MOV(dest, sample_pos);
      bld.MOV(offset(dest, bld, 1), offset(sample_pos, bld, 1));
      break;
   }

   case nir_intrinsic_load_uniform_indirect:
      has_indirect = true;
      /* fallthrough */
   case nir_intrinsic_load_uniform: {
      fs_reg uniform_reg(UNIFORM, instr->const_index[0]);
      uniform_reg.reg_offset = instr->const_index[1];

      for (unsigned j = 0; j < instr->num_components; j++) {
         fs_reg src = offset(retype(uniform_reg, dest.type), bld, j);
         if (has_indirect)
            src.reladdr = new(mem_ctx) fs_reg(get_nir_src(instr->src[0]));

         bld.MOV(dest, src);
         dest = offset(dest, bld, 1);
      }
      break;
   }

   case nir_intrinsic_load_ubo_indirect:
      has_indirect = true;
      /* fallthrough */
   case nir_intrinsic_load_ubo: {
      nir_const_value *const_index = nir_src_as_const_value(instr->src[0]);
      fs_reg surf_index;

      if (const_index) {
         surf_index = fs_reg(stage_prog_data->binding_table.ubo_start +
                             const_index->u[0]);
      } else {
         /* The block index is not a constant. Evaluate the index expression
          * per-channel and add the base UBO index; we have to select a value
          * from any live channel.
          */
         surf_index = vgrf(glsl_type::uint_type);
         bld.ADD(surf_index, get_nir_src(instr->src[0]),
                 fs_reg(stage_prog_data->binding_table.ubo_start));
         surf_index = bld.emit_uniformize(surf_index);

         /* Assume this may touch any UBO. It would be nice to provide
          * a tighter bound, but the array information is already lowered away.
          */
         brw_mark_surface_used(prog_data,
                               stage_prog_data->binding_table.ubo_start +
                               nir->info.num_ubos - 1);
      }

      if (has_indirect) {
         /* Turn the byte offset into a dword offset. */
         fs_reg base_offset = vgrf(glsl_type::int_type);
         bld.SHR(base_offset, retype(get_nir_src(instr->src[1]),
                                     BRW_REGISTER_TYPE_D),
                 fs_reg(2));

         unsigned vec4_offset = instr->const_index[0] / 4;
         for (int i = 0; i < instr->num_components; i++)
            VARYING_PULL_CONSTANT_LOAD(bld, offset(dest, bld, i), surf_index,
                                       base_offset, vec4_offset + i);
      } else {
         fs_reg packed_consts = vgrf(glsl_type::float_type);
         packed_consts.type = dest.type;

         fs_reg const_offset_reg((unsigned) instr->const_index[0] & ~15);
         bld.emit(FS_OPCODE_UNIFORM_PULL_CONSTANT_LOAD, packed_consts,
                  surf_index, const_offset_reg);

         for (unsigned i = 0; i < instr->num_components; i++) {
            packed_consts.set_smear(instr->const_index[0] % 16 / 4 + i);

            /* The std140 packing rules don't allow vectors to cross 16-byte
             * boundaries, and a reg is 32 bytes.
             */
            assert(packed_consts.subreg_offset < 32);

            bld.MOV(dest, packed_consts);
            dest = offset(dest, bld, 1);
         }
      }
      break;
   }

   case nir_intrinsic_load_ssbo_indirect:
      has_indirect = true;
      /* fallthrough */
   case nir_intrinsic_load_ssbo: {
      assert(devinfo->gen >= 7);

      nir_const_value *const_uniform_block =
         nir_src_as_const_value(instr->src[0]);

      fs_reg surf_index;
      if (const_uniform_block) {
         unsigned index = stage_prog_data->binding_table.ssbo_start +
                          const_uniform_block->u[0];
         surf_index = fs_reg(index);
         brw_mark_surface_used(prog_data, index);
      } else {
         surf_index = vgrf(glsl_type::uint_type);
         bld.ADD(surf_index, get_nir_src(instr->src[0]),
                 fs_reg(stage_prog_data->binding_table.ssbo_start));

         /* Assume this may touch any UBO. It would be nice to provide
          * a tighter bound, but the array information is already lowered away.
          */
         brw_mark_surface_used(prog_data,
                               stage_prog_data->binding_table.ssbo_start +
                               nir->info.num_ssbos - 1);
      }

      /* Get the offset to read from */
      fs_reg offset_reg;
      if (has_indirect) {
         offset_reg = get_nir_src(instr->src[1]);
      } else {
         offset_reg = fs_reg(instr->const_index[0]);
      }

      /* Read the vector */
      fs_reg read_result = emit_untyped_read(bld, surf_index, offset_reg,
                                             1 /* dims */,
                                             instr->num_components,
                                             BRW_PREDICATE_NONE);
      read_result.type = dest.type;
      for (int i = 0; i < instr->num_components; i++)
         bld.MOV(offset(dest, bld, i), offset(read_result, bld, i));

      break;
   }

   case nir_intrinsic_load_input_indirect:
      has_indirect = true;
      /* fallthrough */
   case nir_intrinsic_load_input: {
      unsigned index = 0;
      for (unsigned j = 0; j < instr->num_components; j++) {
         fs_reg src;
         if (stage == MESA_SHADER_VERTEX) {
            src = offset(fs_reg(ATTR, instr->const_index[0], dest.type), bld, index);
         } else {
            src = offset(retype(nir_inputs, dest.type), bld,
                         instr->const_index[0] + index);
         }
         if (has_indirect)
            src.reladdr = new(mem_ctx) fs_reg(get_nir_src(instr->src[0]));
         index++;

         bld.MOV(dest, src);
         dest = offset(dest, bld, 1);
      }
      break;
   }

   case nir_intrinsic_load_per_vertex_input_indirect:
      assert(!"Not allowed");
      /* fallthrough */
   case nir_intrinsic_load_per_vertex_input:
      emit_gs_input_load(dest, instr->src[0], instr->const_index[0],
                         instr->num_components);
      break;

   /* Handle ARB_gpu_shader5 interpolation intrinsics
    *
    * It's worth a quick word of explanation as to why we handle the full
    * variable-based interpolation intrinsic rather than a lowered version
    * with like we do for other inputs.  We have to do that because the way
    * we set up inputs doesn't allow us to use the already setup inputs for
    * interpolation.  At the beginning of the shader, we go through all of
    * the input variables and do the initial interpolation and put it in
    * the nir_inputs array based on its location as determined in
    * nir_lower_io.  If the input isn't used, dead code cleans up and
    * everything works fine.  However, when we get to the ARB_gpu_shader5
    * interpolation intrinsics, we need to reinterpolate the input
    * differently.  If we used an intrinsic that just had an index it would
    * only give us the offset into the nir_inputs array.  However, this is
    * useless because that value is post-interpolation and we need
    * pre-interpolation.  In order to get the actual location of the bits
    * we get from the vertex fetching hardware, we need the variable.
    */
   case nir_intrinsic_interp_var_at_centroid:
   case nir_intrinsic_interp_var_at_sample:
   case nir_intrinsic_interp_var_at_offset: {
      assert(stage == MESA_SHADER_FRAGMENT);

      ((struct brw_wm_prog_data *) prog_data)->pulls_bary = true;

      fs_reg dst_xy = bld.vgrf(BRW_REGISTER_TYPE_F, 2);
      const glsl_interp_qualifier interpolation =
         (glsl_interp_qualifier) instr->variables[0]->var->data.interpolation;

      switch (instr->intrinsic) {
      case nir_intrinsic_interp_var_at_centroid:
         emit_pixel_interpolater_send(bld,
                                      FS_OPCODE_INTERPOLATE_AT_CENTROID,
                                      dst_xy,
                                      fs_reg(), /* src */
                                      fs_reg(0u),
                                      interpolation);
         break;

      case nir_intrinsic_interp_var_at_sample: {
         nir_const_value *const_sample = nir_src_as_const_value(instr->src[0]);

         if (const_sample) {
            unsigned msg_data = const_sample->i[0] << 4;

            emit_pixel_interpolater_send(bld,
                                         FS_OPCODE_INTERPOLATE_AT_SAMPLE,
                                         dst_xy,
                                         fs_reg(), /* src */
                                         fs_reg(msg_data),
                                         interpolation);
         } else {
            const fs_reg sample_src = retype(get_nir_src(instr->src[0]),
                                             BRW_REGISTER_TYPE_UD);

            if (nir_src_is_dynamically_uniform(instr->src[0])) {
               const fs_reg sample_id = bld.emit_uniformize(sample_src);
               const fs_reg msg_data = vgrf(glsl_type::uint_type);
               bld.exec_all().group(1, 0).SHL(msg_data, sample_id, fs_reg(4u));
               emit_pixel_interpolater_send(bld,
                                            FS_OPCODE_INTERPOLATE_AT_SAMPLE,
                                            dst_xy,
                                            fs_reg(), /* src */
                                            msg_data,
                                            interpolation);
            } else {
               /* Make a loop that sends a message to the pixel interpolater
                * for the sample number in each live channel. If there are
                * multiple channels with the same sample number then these
                * will be handled simultaneously with a single interation of
                * the loop.
                */
               bld.emit(BRW_OPCODE_DO);

               /* Get the next live sample number into sample_id_reg */
               const fs_reg sample_id = bld.emit_uniformize(sample_src);

               /* Set the flag register so that we can perform the send
                * message on all channels that have the same sample number
                */
               bld.CMP(bld.null_reg_ud(),
                       sample_src, sample_id,
                       BRW_CONDITIONAL_EQ);
               const fs_reg msg_data = vgrf(glsl_type::uint_type);
               bld.exec_all().group(1, 0).SHL(msg_data, sample_id, fs_reg(4u));
               fs_inst *inst =
                  emit_pixel_interpolater_send(bld,
                                               FS_OPCODE_INTERPOLATE_AT_SAMPLE,
                                               dst_xy,
                                               fs_reg(), /* src */
                                               msg_data,
                                               interpolation);
               set_predicate(BRW_PREDICATE_NORMAL, inst);

               /* Continue the loop if there are any live channels left */
               set_predicate_inv(BRW_PREDICATE_NORMAL,
                                 true, /* inverse */
                                 bld.emit(BRW_OPCODE_WHILE));
            }
         }

         break;
      }

      case nir_intrinsic_interp_var_at_offset: {
         nir_const_value *const_offset = nir_src_as_const_value(instr->src[0]);

         if (const_offset) {
            unsigned off_x = MIN2((int)(const_offset->f[0] * 16), 7) & 0xf;
            unsigned off_y = MIN2((int)(const_offset->f[1] * 16), 7) & 0xf;

            emit_pixel_interpolater_send(bld,
                                         FS_OPCODE_INTERPOLATE_AT_SHARED_OFFSET,
                                         dst_xy,
                                         fs_reg(), /* src */
                                         fs_reg(off_x | (off_y << 4)),
                                         interpolation);
         } else {
            fs_reg src = vgrf(glsl_type::ivec2_type);
            fs_reg offset_src = retype(get_nir_src(instr->src[0]),
                                       BRW_REGISTER_TYPE_F);
            for (int i = 0; i < 2; i++) {
               fs_reg temp = vgrf(glsl_type::float_type);
               bld.MUL(temp, offset(offset_src, bld, i), fs_reg(16.0f));
               fs_reg itemp = vgrf(glsl_type::int_type);
               bld.MOV(itemp, temp);  /* float to int */

               /* Clamp the upper end of the range to +7/16.
                * ARB_gpu_shader5 requires that we support a maximum offset
                * of +0.5, which isn't representable in a S0.4 value -- if
                * we didn't clamp it, we'd end up with -8/16, which is the
                * opposite of what the shader author wanted.
                *
                * This is legal due to ARB_gpu_shader5's quantization
                * rules:
                *
                * "Not all values of <offset> may be supported; x and y
                * offsets may be rounded to fixed-point values with the
                * number of fraction bits given by the
                * implementation-dependent constant
                * FRAGMENT_INTERPOLATION_OFFSET_BITS"
                */
               set_condmod(BRW_CONDITIONAL_L,
                           bld.SEL(offset(src, bld, i), itemp, fs_reg(7)));
            }

            const enum opcode opcode = FS_OPCODE_INTERPOLATE_AT_PER_SLOT_OFFSET;
            emit_pixel_interpolater_send(bld,
                                         opcode,
                                         dst_xy,
                                         src,
                                         fs_reg(0u),
                                         interpolation);
         }
         break;
      }

      default:
         unreachable("Invalid intrinsic");
      }

      for (unsigned j = 0; j < instr->num_components; j++) {
         fs_reg src = interp_reg(instr->variables[0]->var->data.location, j);
         src.type = dest.type;

         bld.emit(FS_OPCODE_LINTERP, dest, dst_xy, src);
         dest = offset(dest, bld, 1);
      }
      break;
   }

   case nir_intrinsic_store_ssbo_indirect:
      has_indirect = true;
      /* fallthrough */
   case nir_intrinsic_store_ssbo: {
      assert(devinfo->gen >= 7);

      /* Block index */
      fs_reg surf_index;
      nir_const_value *const_uniform_block =
         nir_src_as_const_value(instr->src[1]);
      if (const_uniform_block) {
         unsigned index = stage_prog_data->binding_table.ssbo_start +
                          const_uniform_block->u[0];
         surf_index = fs_reg(index);
         brw_mark_surface_used(prog_data, index);
      } else {
         surf_index = vgrf(glsl_type::uint_type);
         bld.ADD(surf_index, get_nir_src(instr->src[1]),
                  fs_reg(stage_prog_data->binding_table.ssbo_start));

         brw_mark_surface_used(prog_data,
                               stage_prog_data->binding_table.ssbo_start +
                               nir->info.num_ssbos - 1);
      }

      /* Value */
      fs_reg val_reg = get_nir_src(instr->src[0]);

      /* Writemask */
      unsigned writemask = instr->const_index[1];

      /* Combine groups of consecutive enabled channels in one write
       * message. We use ffs to find the first enabled channel and then ffs on
       * the bit-inverse, down-shifted writemask to determine the length of
       * the block of enabled bits.
       */
      while (writemask) {
         unsigned first_component = ffs(writemask) - 1;
         unsigned length = ffs(~(writemask >> first_component)) - 1;
         fs_reg offset_reg;

         if (!has_indirect) {
            offset_reg = fs_reg(instr->const_index[0] + 4 * first_component);
         } else {
            offset_reg = vgrf(glsl_type::uint_type);
            bld.ADD(offset_reg,
                    retype(get_nir_src(instr->src[2]), BRW_REGISTER_TYPE_UD),
                    fs_reg(4 * first_component));
         }

         emit_untyped_write(bld, surf_index, offset_reg,
                            offset(val_reg, bld, first_component),
                            1 /* dims */, length,
                            BRW_PREDICATE_NONE);

         /* Clear the bits in the writemask that we just wrote, then try
          * again to see if more channels are left.
          */
         writemask &= (15 << (first_component + length));
      }
      break;
   }

   case nir_intrinsic_store_output_indirect:
      has_indirect = true;
      /* fallthrough */
   case nir_intrinsic_store_output: {
      fs_reg src = get_nir_src(instr->src[0]);
      unsigned index = 0;
      for (unsigned j = 0; j < instr->num_components; j++) {
         fs_reg new_dest = offset(retype(nir_outputs, src.type), bld,
                                  instr->const_index[0] + index);
         if (has_indirect)
            src.reladdr = new(mem_ctx) fs_reg(get_nir_src(instr->src[1]));
         index++;
         bld.MOV(new_dest, src);
         src = offset(src, bld, 1);
      }
      break;
   }

   case nir_intrinsic_barrier:
      emit_barrier();
      if (stage == MESA_SHADER_COMPUTE)
         ((struct brw_cs_prog_data *) prog_data)->uses_barrier = true;
      break;

   case nir_intrinsic_load_local_invocation_id:
   case nir_intrinsic_load_work_group_id: {
      gl_system_value sv = nir_system_value_from_intrinsic(instr->intrinsic);
      fs_reg val = nir_system_values[sv];
      assert(val.file != BAD_FILE);
      dest.type = val.type;
      for (unsigned i = 0; i < 3; i++)
         bld.MOV(offset(dest, bld, i), offset(val, bld, i));
      break;
   }

   case nir_intrinsic_ssbo_atomic_add:
      nir_emit_ssbo_atomic(bld, BRW_AOP_ADD, instr);
      break;
   case nir_intrinsic_ssbo_atomic_imin:
      nir_emit_ssbo_atomic(bld, BRW_AOP_IMIN, instr);
      break;
   case nir_intrinsic_ssbo_atomic_umin:
      nir_emit_ssbo_atomic(bld, BRW_AOP_UMIN, instr);
      break;
   case nir_intrinsic_ssbo_atomic_imax:
      nir_emit_ssbo_atomic(bld, BRW_AOP_IMAX, instr);
      break;
   case nir_intrinsic_ssbo_atomic_umax:
      nir_emit_ssbo_atomic(bld, BRW_AOP_UMAX, instr);
      break;
   case nir_intrinsic_ssbo_atomic_and:
      nir_emit_ssbo_atomic(bld, BRW_AOP_AND, instr);
      break;
   case nir_intrinsic_ssbo_atomic_or:
      nir_emit_ssbo_atomic(bld, BRW_AOP_OR, instr);
      break;
   case nir_intrinsic_ssbo_atomic_xor:
      nir_emit_ssbo_atomic(bld, BRW_AOP_XOR, instr);
      break;
   case nir_intrinsic_ssbo_atomic_exchange:
      nir_emit_ssbo_atomic(bld, BRW_AOP_MOV, instr);
      break;
   case nir_intrinsic_ssbo_atomic_comp_swap:
      nir_emit_ssbo_atomic(bld, BRW_AOP_CMPWR, instr);
      break;

   case nir_intrinsic_get_buffer_size: {
      nir_const_value *const_uniform_block = nir_src_as_const_value(instr->src[0]);
      unsigned ssbo_index = const_uniform_block ? const_uniform_block->u[0] : 0;
      int reg_width = dispatch_width / 8;

      /* Set LOD = 0 */
      fs_reg source = fs_reg(0);

      int mlen = 1 * reg_width;
      fs_reg src_payload = fs_reg(GRF, alloc.allocate(mlen),
                                  BRW_REGISTER_TYPE_UD);
      bld.LOAD_PAYLOAD(src_payload, &source, 1, 0);

      fs_reg surf_index = fs_reg(prog_data->binding_table.ssbo_start + ssbo_index);
      fs_inst *inst = bld.emit(FS_OPCODE_GET_BUFFER_SIZE, dest,
                               src_payload, surf_index);
      inst->header_size = 0;
      inst->mlen = mlen;
      bld.emit(inst);
      break;
   }

   case nir_intrinsic_load_num_work_groups: {
      assert(devinfo->gen >= 7);
      assert(stage == MESA_SHADER_COMPUTE);

      struct brw_cs_prog_data *cs_prog_data =
         (struct brw_cs_prog_data *) prog_data;
      const unsigned surface =
         cs_prog_data->binding_table.work_groups_start;

      cs_prog_data->uses_num_work_groups = true;

      fs_reg surf_index = fs_reg(surface);
      brw_mark_surface_used(prog_data, surface);

      /* Read the 3 GLuint components of gl_NumWorkGroups */
      for (unsigned i = 0; i < 3; i++) {
         fs_reg read_result =
            emit_untyped_read(bld, surf_index,
                              fs_reg(i << 2),
                              1 /* dims */, 1 /* size */,
                              BRW_PREDICATE_NONE);
         read_result.type = dest.type;
         bld.MOV(dest, read_result);
         dest = offset(dest, bld, 1);
      }
      break;
   }

   case nir_intrinsic_emit_vertex_with_counter:
      emit_gs_vertex(instr->src[0], instr->const_index[0]);
      break;

   case nir_intrinsic_end_primitive_with_counter:
      emit_gs_end_primitive(instr->src[0]);
      break;

   case nir_intrinsic_set_vertex_count:
      bld.MOV(this->final_gs_vertex_count, get_nir_src(instr->src[0]));
      break;

   default:
      unreachable("unknown intrinsic");
   }
}

void
fs_visitor::nir_emit_ssbo_atomic(const fs_builder &bld,
                                 int op, nir_intrinsic_instr *instr)
{
   fs_reg dest;
   if (nir_intrinsic_infos[instr->intrinsic].has_dest)
      dest = get_nir_dest(instr->dest);

   fs_reg surface;
   nir_const_value *const_surface = nir_src_as_const_value(instr->src[0]);
   if (const_surface) {
      unsigned surf_index = stage_prog_data->binding_table.ssbo_start +
                            const_surface->u[0];
      surface = fs_reg(surf_index);
      brw_mark_surface_used(prog_data, surf_index);
   } else {
      surface = vgrf(glsl_type::uint_type);
      bld.ADD(surface, get_nir_src(instr->src[0]),
              fs_reg(stage_prog_data->binding_table.ssbo_start));

      /* Assume this may touch any SSBO. This is the same we do for other
       * UBO/SSBO accesses with non-constant surface.
       */
      brw_mark_surface_used(prog_data,
                            stage_prog_data->binding_table.ssbo_start +
                            nir->info.num_ssbos - 1);
   }

   fs_reg offset = get_nir_src(instr->src[1]);
   fs_reg data1 = get_nir_src(instr->src[2]);
   fs_reg data2;
   if (op == BRW_AOP_CMPWR)
      data2 = get_nir_src(instr->src[3]);

   /* Emit the actual atomic operation operation */

   fs_reg atomic_result =
      surface_access::emit_untyped_atomic(bld, surface, offset,
                                          data1, data2,
                                          1 /* dims */, 1 /* rsize */,
                                          op,
                                          BRW_PREDICATE_NONE);
   dest.type = atomic_result.type;
   bld.MOV(dest, atomic_result);
}

void
fs_visitor::nir_emit_texture(const fs_builder &bld, nir_tex_instr *instr)
{
   unsigned sampler = instr->sampler_index;
   fs_reg sampler_reg(sampler);

   int gather_component = instr->component;

   bool is_rect = instr->sampler_dim == GLSL_SAMPLER_DIM_RECT;

   bool is_cube_array = instr->sampler_dim == GLSL_SAMPLER_DIM_CUBE &&
                        instr->is_array;

   int lod_components = 0;
   int UNUSED offset_components = 0;

   fs_reg coordinate, shadow_comparitor, lod, lod2, sample_index, mcs, tex_offset;

   for (unsigned i = 0; i < instr->num_srcs; i++) {
      fs_reg src = get_nir_src(instr->src[i].src);
      switch (instr->src[i].src_type) {
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
         tex_offset = retype(src, BRW_REGISTER_TYPE_D);
         if (instr->is_array)
            offset_components = instr->coord_components - 1;
         else
            offset_components = instr->coord_components;
         break;
      case nir_tex_src_projector:
         unreachable("should be lowered");

      case nir_tex_src_sampler_offset: {
         /* Figure out the highest possible sampler index and mark it as used */
         uint32_t max_used = sampler + instr->sampler_array_size - 1;
         if (instr->op == nir_texop_tg4 && devinfo->gen < 8) {
            max_used += stage_prog_data->binding_table.gather_texture_start;
         } else {
            max_used += stage_prog_data->binding_table.texture_start;
         }
         brw_mark_surface_used(prog_data, max_used);

         /* Emit code to evaluate the actual indexing expression */
         sampler_reg = vgrf(glsl_type::uint_type);
         bld.ADD(sampler_reg, src, fs_reg(sampler));
         sampler_reg = bld.emit_uniformize(sampler_reg);
         break;
      }

      default:
         unreachable("unknown texture source");
      }
   }

   if (instr->op == nir_texop_txf_ms) {
      if (devinfo->gen >= 7 &&
          key_tex->compressed_multisample_layout_mask & (1 << sampler)) {
         mcs = emit_mcs_fetch(coordinate, instr->coord_components, sampler_reg);
      } else {
         mcs = fs_reg(0u);
      }
   }

   for (unsigned i = 0; i < 3; i++) {
      if (instr->const_offset[i] != 0) {
         assert(offset_components == 0);
         tex_offset = fs_reg(brw_texture_offset(instr->const_offset, 3));
         break;
      }
   }

   enum glsl_base_type dest_base_type =
     brw_glsl_base_type_for_nir_type (instr->dest_type);

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
   case nir_texop_texture_samples: {
      fs_reg dst = retype(get_nir_dest(instr->dest), BRW_REGISTER_TYPE_D);
      fs_inst *inst = bld.emit(SHADER_OPCODE_SAMPLEINFO, dst,
                               bld.vgrf(BRW_REGISTER_TYPE_D, 1),
                               sampler_reg);
      inst->mlen = 1;
      inst->header_size = 1;
      inst->base_mrf = -1;
      return;
   }
   default:
      unreachable("unknown texture opcode");
   }

   emit_texture(op, dest_type, coordinate, instr->coord_components,
                shadow_comparitor, lod, lod2, lod_components, sample_index,
                tex_offset, mcs, gather_component,
                is_cube_array, is_rect, sampler, sampler_reg);

   fs_reg dest = get_nir_dest(instr->dest);
   dest.type = this->result.type;
   unsigned num_components = nir_tex_instr_dest_size(instr);
   emit_percomp(bld, fs_inst(BRW_OPCODE_MOV, bld.dispatch_width(),
                             dest, this->result),
                (1 << num_components) - 1);
}

void
fs_visitor::nir_emit_jump(const fs_builder &bld, nir_jump_instr *instr)
{
   switch (instr->type) {
   case nir_jump_break:
      bld.emit(BRW_OPCODE_BREAK);
      break;
   case nir_jump_continue:
      bld.emit(BRW_OPCODE_CONTINUE);
      break;
   case nir_jump_return:
   default:
      unreachable("unknown jump");
   }
}
