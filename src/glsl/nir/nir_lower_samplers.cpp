/*
 * Copyright (C) 2005-2007  Brian Paul   All Rights Reserved.
 * Copyright (C) 2008  VMware, Inc.   All Rights Reserved.
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "nir.h"
#include "../program.h"
#include "program/hash_table.h"
#include "ir_uniform.h"

extern "C" {
#include "main/compiler.h"
#include "main/mtypes.h"
#include "program/prog_parameter.h"
#include "program/program.h"
}

static unsigned
get_deref_name_offset(nir_deref_var *deref_var,
                      gl_shader_program *shader_program, char **name,
                      void *mem_ctx)
{
   nir_deref *deref = &deref_var->deref;
   nir_deref_array *deref_array;
   nir_deref_struct *deref_struct;

   *name = ralloc_strdup(mem_ctx, deref_var->var->name);

   while (deref->child != NULL) {
      deref = deref->child;
      switch (deref->deref_type) {
         case nir_deref_type_array:
            deref_array = nir_deref_as_array(deref);
            if (deref_array->has_indirect) {
               /* GLSL 1.10 and 1.20 allowed variable sampler array indices,
                * while GLSL 1.30 requires that the array indices be
                * constant integer expressions.  We don't expect any driver
                * to actually work with a really variable array index, so
                * all that would work would be an unrolled loop counter that
                * ends up being constant.
                */
               ralloc_strcat(&shader_program->InfoLog,
                           "warning: Variable sampler array index unsupported.\n"
                           "This feature of the language was removed in GLSL 1.20 "
                           "and is unlikely to be supported for 1.10 in Mesa.\n");
            }
            if (deref->child == NULL) {
               return deref_array->base_offset;
            }
            ralloc_asprintf_append(name, "[%u]", deref_array->base_offset);
            break;

         case nir_deref_type_struct:
            deref_struct = nir_deref_as_struct(deref);
            ralloc_asprintf_append(name, ".%s", deref_struct->elem);
            break;

         default:
            assert(0);
            break;
      }
   }

   return 0;
}

static unsigned
get_sampler_index(nir_deref_var *sampler,
                  struct gl_shader_program *shader_program,
                  const struct gl_program *prog)
{
   void *mem_ctx = ralloc_context(NULL);
   char *name;
   unsigned offset = get_deref_name_offset(sampler, shader_program, &name,
                                           mem_ctx);

   GLuint shader = _mesa_program_enum_to_shader_stage(prog->Target);

   unsigned location;
   if (!shader_program->UniformHash->get(location, name)) {
      linker_error(shader_program,
                   "failed to find sampler named %s.\n", name);
      return 0;
   }

   if (!shader_program->UniformStorage[location].sampler[shader].active) {
      assert(0 && "cannot return a sampler");
      linker_error(shader_program,
                   "cannot return a sampler named %s, because it is not "
                   "used in this shader stage. This is a driver bug.\n",
                   name);
      return 0;
   }

   ralloc_free(mem_ctx);

   return shader_program->UniformStorage[location].sampler[shader].index +
          offset;
}

static void
lower_sampler(nir_tex_instr *instr, struct gl_shader_program *shader_program,
              const struct gl_program *prog)
{
   if (instr->sampler) {
      instr->sampler_index = get_sampler_index(instr->sampler, shader_program,
                                             prog);
      instr->sampler = NULL;
   }
}

typedef struct {
   struct gl_shader_program *shader_program;
   struct gl_program *prog;
} lower_state;

static bool
lower_block_cb(nir_block *block, void *_state)
{
   lower_state *state = (lower_state *) _state;

   nir_foreach_instr(block, instr) {
      if (instr->type == nir_instr_type_texture) {
         nir_tex_instr *tex_instr = nir_instr_as_texture(instr);
         lower_sampler(tex_instr, state->shader_program, state->prog);
      }
   }

   return true;
}

static void
lower_impl(nir_function_impl *impl, struct gl_shader_program *shader_program,
           struct gl_program *prog)
{
   lower_state state;
   state.shader_program = shader_program;
   state.prog = prog;
   nir_foreach_block(impl, lower_block_cb, &state);
}

extern "C" void
nir_lower_samplers(nir_shader *shader, struct gl_shader_program *shader_program,
                   struct gl_program *prog)
{
   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         lower_impl(overload->impl, shader_program, prog);
   }
}
