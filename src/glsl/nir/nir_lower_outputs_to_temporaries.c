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

/*
 * Implements a pass that lowers output variables to a temporary plus an
 * output variable with a single copy at each exit point of the shader.
 * This way the output variable is only ever written.
 *
 * Because valid NIR requires that output variables are never read, this
 * pass is more of a helper for NIR producers and must be run before the
 * shader is ever validated.
 */

#include "nir.h"

static void
emit_output_copies(nir_shader *shader, nir_variable *temp, nir_variable *output)
{
   nir_foreach_overload(shader, overload) {
      if (!overload->impl || strcmp(overload->function->name, "main"))
         continue;

      struct set_entry *block_entry;
      set_foreach(overload->impl->end_block->predecessors, block_entry) {
         struct nir_block *block = (void *)block_entry->key;

         nir_intrinsic_instr *copy =
            nir_intrinsic_instr_create(shader, nir_intrinsic_copy_var);
         copy->variables[0] = nir_deref_var_create(copy, output);
         copy->variables[1] = nir_deref_var_create(copy, temp);

         nir_instr_insert(nir_after_block_before_jump(block), &copy->instr);
      }
   }
}

void
nir_lower_outputs_to_temporaries(nir_shader *shader)
{
   struct exec_list old_outputs;

   exec_list_move_nodes_to(&shader->outputs, &old_outputs);

   /* Walk over all of the outputs turn each output into a temporary and
    * make a new variable for the actual output.
    */
   foreach_list_typed(nir_variable, var, node, &old_outputs) {
      nir_variable *output = ralloc(shader, nir_variable);
      memcpy(output, var, sizeof *output);

      /* The orignal is now the temporary */
      nir_variable *temp = var;

      /* Move the original name over to the new output */
      if (output->name)
         ralloc_steal(output, output->name);

      /* Give the output a new name with @out-temp appended */
      temp->name = ralloc_asprintf(var, "%s@out-temp", output->name);
      temp->data.mode = nir_var_global;
      temp->constant_initializer = NULL;

      exec_list_push_tail(&shader->outputs, &output->node);

      emit_output_copies(shader, temp, output);
   }

   exec_list_append(&shader->globals, &old_outputs);
}
