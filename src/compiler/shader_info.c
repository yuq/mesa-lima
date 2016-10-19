/*
 * Copyright © 2016 Intel Corporation
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
 */

#include "mesa/main/mtypes.h"

void
copy_shader_info(const struct gl_shader_program *shader_prog,
                 struct gl_linked_shader *sh)
{
   shader_info *info = &sh->Program->info;

   info->inputs_read = sh->Program->InputsRead;
   info->double_inputs_read = sh->Program->DoubleInputsRead;
   info->outputs_written = sh->Program->OutputsWritten;
   info->outputs_read = sh->Program->OutputsRead;
   info->patch_inputs_read = sh->Program->PatchInputsRead;
   info->patch_outputs_written = sh->Program->PatchOutputsWritten;
   info->system_values_read = sh->Program->SystemValuesRead;
   info->uses_texture_gather = sh->Program->UsesGather;

   switch (sh->Stage) {
   case MESA_SHADER_FRAGMENT: {
      struct gl_fragment_program *fp =
         (struct gl_fragment_program *)sh->Program;

      info->fs.uses_discard = fp->UsesKill;
      info->fs.uses_sample_qualifier = fp->IsSample != 0;
      info->fs.early_fragment_tests = sh->info.EarlyFragmentTests;
      info->fs.depth_layout = fp->FragDepthLayout;
      break;
   }

   default:
      break; /* No stage-specific info */
   }
}
