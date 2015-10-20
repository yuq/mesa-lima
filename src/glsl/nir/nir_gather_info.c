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

#include "nir.h"

static void
gather_intrinsic_info(nir_intrinsic_instr *instr, nir_shader *shader)
{
   switch (instr->intrinsic) {
   case nir_intrinsic_discard:
      assert(shader->stage == MESA_SHADER_FRAGMENT);
      shader->info.fs.uses_discard = true;
      break;

   case nir_intrinsic_load_front_face:
   case nir_intrinsic_load_vertex_id:
   case nir_intrinsic_load_vertex_id_zero_base:
   case nir_intrinsic_load_base_vertex:
   case nir_intrinsic_load_instance_id:
   case nir_intrinsic_load_sample_id:
   case nir_intrinsic_load_sample_pos:
   case nir_intrinsic_load_sample_mask_in:
   case nir_intrinsic_load_primitive_id:
   case nir_intrinsic_load_invocation_id:
   case nir_intrinsic_load_local_invocation_id:
   case nir_intrinsic_load_work_group_id:
   case nir_intrinsic_load_num_work_groups:
      shader->info.system_values_read |=
         (1 << nir_system_value_from_intrinsic(instr->intrinsic));
      break;
   default:
      break;
   }
}

static void
gather_tex_info(nir_tex_instr *instr, nir_shader *shader)
{
   if (instr->op == nir_texop_tg4)
      shader->info.uses_texture_gather = true;
}

static bool
gather_info_block(nir_block *block, void *shader)
{
   nir_foreach_instr(block, instr) {
      switch (instr->type) {
      case nir_instr_type_intrinsic:
         gather_intrinsic_info(nir_instr_as_intrinsic(instr), shader);
         break;
      case nir_instr_type_tex:
         gather_tex_info(nir_instr_as_tex(instr), shader);
         break;
      case nir_instr_type_call:
         assert(!"nir_shader_gather_info only works if functions are inlined");
         break;
      default:
         break;
      }
   }

   return true;
}

void
nir_shader_gather_info(nir_shader *shader, nir_function_impl *entrypoint)
{
   shader->info.inputs_read = 0;
   foreach_list_typed(nir_variable, var, node, &shader->inputs)
      shader->info.inputs_read |= (1ull << var->data.location);

   shader->info.outputs_written = 0;
   foreach_list_typed(nir_variable, var, node, &shader->outputs)
      shader->info.outputs_written |= (1ull << var->data.location);

   shader->info.system_values_read = 0;
   foreach_list_typed(nir_variable, var, node, &shader->system_values)
      shader->info.system_values_read |= (1ull << var->data.location);

   nir_foreach_block(entrypoint, gather_info_block, shader);
}
