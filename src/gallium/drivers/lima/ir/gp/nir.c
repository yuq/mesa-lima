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
#include "compiler/nir/nir.h"
#include "gpir.h"

static gpir_node *gpir_emit_alu(gpir_compiler *comp, nir_alu_instr *instr)
{
   return NULL;
}

static gpir_node *gpir_emit_intrinsic(gpir_compiler *comp, nir_intrinsic_instr *instr)
{
   return NULL;
}

static gpir_node *gpir_emit_load_const(gpir_compiler *comp, nir_load_const_instr *instr)
{
   return NULL;
}

static gpir_node *gpir_emit_ssa_undef(gpir_compiler *comp, nir_ssa_undef_instr *instr)
{
   fprintf(stderr, "gpir: nir_ssa_undef_instr not support\n");
   return NULL;
}

static gpir_node *gpir_emit_tex(gpir_compiler *comp, nir_tex_instr *instr)
{
   fprintf(stderr, "gpir: nir_jump_instr not support\n");
   return NULL;
}

static gpir_node *gpir_emit_jump(gpir_compiler *comp, nir_jump_instr *instr)
{
   fprintf(stderr, "gpir: nir_jump_instr not support\n");
   return NULL;
}

static gpir_node *gpir_emit_instr(gpir_compiler *comp, nir_instr *instr)
{
   gpir_node *node;

   switch (instr->type) {
   case nir_instr_type_alu:
      node = gpir_emit_alu(comp, nir_instr_as_alu(instr));
      break;
   case nir_instr_type_intrinsic:
      node = gpir_emit_intrinsic(comp, nir_instr_as_intrinsic(instr));
      break;
   case nir_instr_type_load_const:
      node = gpir_emit_load_const(comp, nir_instr_as_load_const(instr));
      break;
   case nir_instr_type_ssa_undef:
      node = gpir_emit_ssa_undef(comp, nir_instr_as_ssa_undef(instr));
      break;
   case nir_instr_type_tex:
      node = gpir_emit_tex(comp, nir_instr_as_tex(instr));
      break;
   case nir_instr_type_jump:
      node = gpir_emit_jump(comp, nir_instr_as_jump(instr));
      break;
   default:
      fprintf(stderr, "gpir: unknown NIR instr type %d\n", instr->type);
      node = NULL;
      break;
   }
   return node;
}

static gpir_block *gpir_block_create(void)
{
   gpir_block *block = MALLOC(sizeof(*block));
   if (!block)
      return NULL;

   list_inithead(&block->node_list);
   return block;
}

static void gpir_block_delete(gpir_block *block)
{
   list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
      FREE(node);
   }
   FREE(block);
}

static bool gpir_emit_block(gpir_compiler *comp, nir_block *nblock)
{
   gpir_block *block = gpir_block_create();
   if (!block)
      return false;

   list_addtail(&block->list, &comp->block_list);

   nir_foreach_instr(instr, nblock) {
      gpir_node *node = gpir_emit_instr(comp, instr);
      if (!node)
         return false;
      list_addtail(&node->list, &block->node_list);
   }

   return true;
}

static bool gpir_emit_if(gpir_compiler *comp, nir_if *nif)
{
   fprintf(stderr, "gpir: if nir_cf_node not support\n");
   return false;
}

static bool gpir_emit_loop(gpir_compiler *comp, nir_loop *nloop)
{
   fprintf(stderr, "gpir: loop nir_cf_node not support\n");
   return false;
}

static bool gpir_emit_function(gpir_compiler *comp, nir_function_impl *nfunc)
{
   fprintf(stderr, "gpir: function nir_cf_node not support\n");
   return false;
}

static bool gpir_emit_cf_list(gpir_compiler *comp, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      bool ret;

      switch (node->type) {
      case nir_cf_node_block:
         ret = gpir_emit_block(comp, nir_cf_node_as_block(node));
         break;
      case nir_cf_node_if:
         ret = gpir_emit_if(comp, nir_cf_node_as_if(node));
         break;
      case nir_cf_node_loop:
         ret = gpir_emit_loop(comp, nir_cf_node_as_loop(node));
         break;
      case nir_cf_node_function:
         ret = gpir_emit_function(comp, nir_cf_node_as_function(node));
         break;
      default:
         fprintf(stderr, "gpir: unknown NIR node type %d\n", node->type);
         return false;
      }

      if (!ret)
         return false;
   }

   return true;
}

static gpir_compiler *gpir_compiler_create(unsigned num_reg, unsigned num_ssa)
{
   gpir_compiler *comp = CALLOC(1, sizeof(*comp) + (num_reg + num_ssa) * sizeof(gpir_node *));
   if (!comp)
      return NULL;

   list_inithead(&comp->block_list);
   comp->var_nodes = (gpir_node **)(comp + 1);
   comp->reg_base = num_ssa;
   return comp;
}

static void gpir_compiler_delete(gpir_compiler *comp)
{
   list_for_each_entry_safe(gpir_block, block, &comp->block_list, list) {
      gpir_block_delete(block);
   }
   FREE(comp);
}

gpir_prog *gpir_compile_nir(nir_shader *nir)
{
   nir_function_impl *func = nir_shader_get_entrypoint(nir);
   gpir_compiler *comp = gpir_compiler_create(func->reg_alloc, func->ssa_alloc);
   if (!comp)
      return NULL;

   if (!gpir_emit_cf_list(comp, &func->body)) {
      gpir_compiler_delete(comp);
      return NULL;
   }

   gpir_compiler_delete(comp);
   return NULL;
}

