/*
 * Copyright (c) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "program/prog_statevars.h"
#include "nir_builder.h"

/* lower viewport transform into vertex shader
 *
 * This is needed for GPU like Mali400 GP which has no viewport transform hw.
 * Declare viewport transform parameters in uniform and use them to apply on
 * the gl_Position varying output.
 */

typedef struct {
   const nir_lower_viewport_transform_options *options;
   nir_shader   *shader;
   nir_builder   b;
   nir_variable *scale, *translate;
} lower_viewport_transform_state;

static nir_variable *
create_uniform(nir_shader *shader, const char *name, const int *tokens)
{
   nir_variable *var = nir_variable_create(
      shader, nir_var_uniform, glsl_vec_type(3), name);

   var->num_state_slots = 1;
   var->state_slots = ralloc_array(var, nir_state_slot, 1);
   memcpy(var->state_slots[0].tokens, tokens,
          sizeof(var->state_slots[0].tokens));
   return var;
}

static nir_ssa_def *
get_scale(lower_viewport_transform_state *state)
{
   if (!state->scale)
      state->scale = create_uniform(state->shader, "gl_viewportScale",
                                    state->options->scale);

   nir_ssa_def *def = nir_load_var(&state->b, state->scale);
   return nir_vec4(&state->b,
                   nir_channel(&state->b, def, 0),
                   nir_channel(&state->b, def, 1),
                   nir_channel(&state->b, def, 2),
                   nir_imm_float(&state->b, 1));
}

static nir_ssa_def *
get_translate(lower_viewport_transform_state *state)
{
   if (!state->translate)
      state->translate = create_uniform(state->shader, "gl_viewportTranslate",
                                        state->options->translate);

   nir_ssa_def *def = nir_load_var(&state->b, state->translate);
   return nir_vec4(&state->b,
                   nir_channel(&state->b, def, 0),
                   nir_channel(&state->b, def, 1),
                   nir_channel(&state->b, def, 2),
                   nir_imm_float(&state->b, 0));
}

static void
lower_viewport_transform_block(lower_viewport_transform_state *state, nir_block *block)
{
   nir_foreach_instr_safe(instr, block) {
      if (instr->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic == nir_intrinsic_store_var) {
            nir_deref_var *dvar = intr->variables[0];
            nir_variable *var = dvar->var;

            if (var->data.mode == nir_var_shader_out &&
                var->data.location == VARYING_SLOT_POS) {
               assert(intr->num_components == 4);

               state->b.cursor = nir_before_instr(instr);

               nir_ssa_def *def = nir_ssa_for_src(&state->b, intr->src[0], intr->num_components);

               /* homogenization */
               def = nir_fmul(&state->b, def,
                              nir_frcp(&state->b,
                                       nir_channel(&state->b, def, 3)));

               /* viewport transform*/
               def = nir_fmul(&state->b, def, get_scale(state));
               def = nir_fadd(&state->b, def, get_translate(state));

               nir_instr_rewrite_src(instr, &intr->src[0], nir_src_for_ssa(def));
            }
         }
      }
   }
}

static void
lower_viewport_transform_impl(lower_viewport_transform_state *state, nir_function_impl *impl)
{
   nir_builder_init(&state->b, impl);

   nir_foreach_block(block, impl) {
      lower_viewport_transform_block(state, block);
   }
   nir_metadata_preserve(impl, nir_metadata_block_index |
                               nir_metadata_dominance);
}

void
nir_lower_viewport_transform(nir_shader *shader,
                             const nir_lower_viewport_transform_options *options)
{
   lower_viewport_transform_state state = {
      .options = options,
      .shader = shader,
   };

   assert(shader->info.stage == MESA_SHADER_VERTEX);

   nir_foreach_function(function, shader) {
      if (function->impl)
         lower_viewport_transform_impl(&state, function->impl);
   }
}
