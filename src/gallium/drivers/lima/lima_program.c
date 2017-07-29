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

#include "util/u_memory.h"
#include "tgsi/tgsi_dump.h"
#include "compiler/nir/nir.h"
#include "nir/tgsi_to_nir.h"

#include "pipe/p_state.h"

#include "lima_context.h"
#include "lima_program.h"
#include "ir/gp/gpir.h"


static const nir_shader_compiler_options vs_nir_options = {
   .lower_fpow = true,
   .lower_ffract = true,
   .lower_fdiv = true,
   .lower_fsqrt = true,
};

static const nir_shader_compiler_options fs_nir_options = {0};

const void *
lima_program_get_compiler_options(enum pipe_shader_type shader)
{
   switch (shader) {
   case PIPE_SHADER_VERTEX:
      return &vs_nir_options;
   case PIPE_SHADER_FRAGMENT:
      return &fs_nir_options;
   default:
      return NULL;
   }
}

void
lima_program_optimize_nir(struct nir_shader *s)
{
   bool progress;

   do {
      progress = false;

      NIR_PASS_V(s, nir_lower_vars_to_ssa);
      NIR_PASS(progress, s, nir_lower_alu_to_scalar);
      NIR_PASS(progress, s, nir_lower_phis_to_scalar);
      NIR_PASS(progress, s, nir_copy_prop);
      NIR_PASS(progress, s, nir_opt_remove_phis);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_dead_cf);
      NIR_PASS(progress, s, nir_opt_cse);
      NIR_PASS(progress, s, nir_opt_peephole_select, 8);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_undef);
      NIR_PASS(progress, s, nir_opt_loop_unroll,
               nir_var_shader_in |
               nir_var_shader_out |
               nir_var_local);
   } while (progress);

   NIR_PASS_V(s, nir_lower_locals_to_regs);
   NIR_PASS_V(s, nir_convert_from_ssa, true);
   NIR_PASS_V(s, nir_remove_dead_variables, nir_var_local);
   nir_sweep(s);
}

static void *
lima_create_fs_state(struct pipe_context *pctx,
                     const struct pipe_shader_state *cso)
{
   struct lima_fs_shader_state *so = CALLOC_STRUCT(lima_fs_shader_state);

   if (!so)
      return NULL;

   printf("dummy %s\n", __func__);

   assert(cso->type == PIPE_SHADER_IR_TGSI);

   tgsi_dump(cso->tokens, 0);

   static uint32_t fs[] = {
      0x00021025, 0x0000014c, 0x03c007cf, 0x00000000, /* 0x00000000 */
      0x00000000,
   };

   so->shader = fs;
   so->shader_size = sizeof(fs);

   return so;
}

static void
lima_bind_fs_state(struct pipe_context *pctx, void *hwcso)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   ctx->fs = hwcso;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_SHADER_FRAG;
}

static void
lima_delete_fs_state(struct pipe_context *pctx, void *hwcso)
{
   struct lima_fs_shader_state *so = hwcso;

   FREE(so);
}

static void *
lima_create_vs_state(struct pipe_context *pctx,
                     const struct pipe_shader_state *cso)
{
   struct lima_vs_shader_state *so = CALLOC_STRUCT(lima_vs_shader_state);

   if (!so)
      return NULL;

   printf("dummy %s\n", __func__);

   assert(cso->type == PIPE_SHADER_IR_NIR);

   nir_shader *nir = cso->ir.nir;
   lima_program_optimize_nir(nir);
   nir_print_shader(nir, stdout);
   gpir_compile_nir(nir);

/*
  uniform.load(2), acc[1].pass(uniform.x);
  uniform.load(0), attribute.load(0), mul[0].mul(attribute.z, uniform.z), mul[1].mul(attribute.y, uniform.y);
  uniform.load(0), attribute.load(0), mul[0].mul(attribute.x, uniform.x), mul[1].mul(mul[0].out[1], acc[1].out[2]), acc[1].pass(acc[1].out[2]);
  uniform.load(1), mul[0].mul(mul[1].out[2], acc[1].out[1]), mul[1].mul(mul[0].out[1], acc[1].out[1]), acc[1].add(mul[1].out[1], uniform.z);
  uniform.load(1), mul[1].pass(acc[1].out[2]), acc[0].add(mul[0].out[1], uniform.y), acc[1].add(mul[1].out[1], uniform.x), complex.pass(acc[1].out[1]), store[0].varying(0, acc[1].out, acc[0].out), store[1].varying(0, complex.out, mul[1].out);

  void main()
  {
  004: varying[0].x = (((attribute[0].x * temp[0].x) * temp[2].x) + temp[1].x);
  004: varying[0].y = (((attribute[0].y * temp[0].y) * temp[2].x) + temp[1].y);
  004: varying[0].z = (((attribute[0].z * temp[0].z) * temp[2].x) + temp[1].z);
  004: varying[0].w = temp[2].x;
  }

  sx * x * s = vx
  sy * y * s = vy
  sz * z * s = vz
  s = vw
 */
   static uint32_t vs[] = {
      0xad4ad6b5, 0x0380a2cc, 0x0007ff80, 0x000ad500, /* 0x00000000 */
      0xad4685c2, 0x438002b5, 0x0007ff80, 0x000ad500, /* 0x00000010 */
      0xad4cc980, 0x438022d9, 0x0007ff80, 0x000ad500, /* 0x00000020 */
      0xad48ca3b, 0x038041d3, 0x0007ff80, 0x000ad500, /* 0x00000030 */
      0x6c8b66b5, 0x03804193, 0x4243c080, 0x000ac508, /* 0x00000040 */
   };

   so->shader = vs;
   so->shader_size = sizeof(vs);

   return so;
}

static void
lima_bind_vs_state(struct pipe_context *pctx, void *hwcso)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   ctx->vs = hwcso;
   ctx->dirty |= LIMA_CONTEXT_DIRTY_SHADER_VERT;
}

static void
lima_delete_vs_state(struct pipe_context *pctx, void *hwcso)
{
   struct lima_vs_shader_state *so = hwcso;

   FREE(so);
}

void
lima_program_init(struct lima_context *ctx)
{
   ctx->base.create_fs_state = lima_create_fs_state;
   ctx->base.bind_fs_state = lima_bind_fs_state;
   ctx->base.delete_fs_state = lima_delete_fs_state;

   ctx->base.create_vs_state = lima_create_vs_state;
   ctx->base.bind_vs_state = lima_bind_vs_state;
   ctx->base.delete_vs_state = lima_delete_vs_state;
}
