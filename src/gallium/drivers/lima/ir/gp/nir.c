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

#include "util/ralloc.h"
#include "compiler/nir/nir.h"

#include "gpir.h"


static inline void *gpir_node_create_ssa(gpir_compiler *comp, gpir_op op, nir_ssa_def *ssa)
{
   return gpir_node_create(comp, op, ssa->index);
}

static inline void *gpir_node_create_reg(gpir_compiler *comp, gpir_op op, nir_reg_dest *reg)
{
   return gpir_node_create(comp, op, reg->reg->index + comp->reg_base);
}

static void *gpir_node_create_dest(gpir_compiler *comp, gpir_op op, nir_dest *dest)
{
   unsigned index = -1;

   if (dest) {
      if (dest->is_ssa)
         return gpir_node_create_ssa(comp, op, &dest->ssa);
      else
         return gpir_node_create_reg(comp, op, &dest->reg);
   }

   return gpir_node_create(comp, op, index);
}

static gpir_node *gpir_node_find(gpir_compiler *comp, nir_src *src)
{
   int index;
   gpir_node *node;

   if (src->is_ssa)
      index = src->ssa->index;
   else
      index = src->reg.reg->index + comp->reg_base;

   node = comp->var_nodes[index];
   assert(node);
   return node;
}

static int nir_to_gpir_opcodes[nir_num_opcodes] = {
   /* not supported */
   [0 ... nir_last_opcode] = -1,

   [nir_op_fmul] = gpir_op_mul,
   [nir_op_fadd] = gpir_op_add,
   [nir_op_fneg] = gpir_op_neg,
   [nir_op_fmax] = gpir_op_max,
   [nir_op_frcp] = gpir_op_rcp,
   [nir_op_frsq] = gpir_op_rsqrt,
};

static gpir_node *gpir_emit_alu(gpir_compiler *comp, nir_alu_instr *instr)
{
   int op = nir_to_gpir_opcodes[instr->op];

   if (op < 0) {
      gpir_error("unsupport nir_op %d\n", instr->op);
      return NULL;
   }

   gpir_alu_node *node = gpir_node_create_dest(comp, op, &instr->dest.dest);
   if (!node)
      return NULL;

   unsigned num_child = nir_op_infos[instr->op].num_inputs;
   assert(num_child <= ARRAY_SIZE(node->children));
   node->num_child = num_child;

   for (int i = 0; i < num_child; i++) {
      nir_alu_src *src = instr->src + i;
      node->children_negate[i] = src->negate;

      gpir_node *child = gpir_node_find(comp, &src->src);
      node->children[i] = child;

      gpir_node_add_child(&node->node, child);
   }

   return &node->node;
}

static gpir_node *gpir_emit_intrinsic(gpir_compiler *comp, nir_intrinsic_instr *instr)
{
   gpir_node *child;
   gpir_load_node *lnode;
   gpir_store_node *snode;

   switch (instr->intrinsic) {
   case nir_intrinsic_load_input:
      lnode = gpir_node_create_dest(comp, gpir_op_load_attribute, &instr->dest);
      if (!lnode)
         return NULL;

      lnode->index = nir_intrinsic_base(instr);
      lnode->component = nir_intrinsic_component(instr);

      return &lnode->node;

   case nir_intrinsic_load_uniform:
      lnode = gpir_node_create_dest(comp, gpir_op_load_uniform, &instr->dest);
      if (!lnode)
         return NULL;

      lnode->index = nir_intrinsic_base(instr);
      lnode->component = nir_intrinsic_component(instr);

      child = gpir_node_find(comp, instr->src);
      if (child->type == gpir_node_type_const) {
         gpir_const_node *c = gpir_node_to_const(child);
         lnode->index += c->value.i;
      }
      else
         gpir_node_add_read_after_write_dep(&lnode->node, child);

      return &lnode->node;

   case nir_intrinsic_store_output:
      snode = gpir_node_create_dest(comp, gpir_op_store_varying, NULL);
      if (!snode)
         return NULL;

      snode->index = nir_intrinsic_base(instr);
      snode->component = nir_intrinsic_component(instr);

      child = gpir_node_find(comp, instr->src);
      snode->child = child;
      gpir_node_add_child(&snode->node, child);

      return &snode->node;

   default:
      gpir_error("unsupport nir_intrinsic_instr %d\n", instr->intrinsic);
      return NULL;
   }
}

static gpir_node *gpir_emit_load_const(gpir_compiler *comp, nir_load_const_instr *instr)
{
   gpir_const_node *node = gpir_node_create_ssa(comp, gpir_op_const, &instr->def);
   if (!node)
      return NULL;

   assert(instr->def.bit_size == 32);
   assert(instr->def.num_components == 1);

   node->value.i = instr->value.i32[0];

   return &node->node;
}

static gpir_node *gpir_emit_ssa_undef(gpir_compiler *comp, nir_ssa_undef_instr *instr)
{
   gpir_error("nir_ssa_undef_instr not support\n");
   return NULL;
}

static gpir_node *gpir_emit_tex(gpir_compiler *comp, nir_tex_instr *instr)
{
   gpir_error("nir_jump_instr not support\n");
   return NULL;
}

static gpir_node *gpir_emit_jump(gpir_compiler *comp, nir_jump_instr *instr)
{
   gpir_error("nir_jump_instr not support\n");
   return NULL;
}

static gpir_node *gpir_emit_instr(gpir_compiler *comp, nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_alu:
      return gpir_emit_alu(comp, nir_instr_as_alu(instr));
   case nir_instr_type_intrinsic:
      return gpir_emit_intrinsic(comp, nir_instr_as_intrinsic(instr));
   case nir_instr_type_load_const:
      return gpir_emit_load_const(comp, nir_instr_as_load_const(instr));
   case nir_instr_type_ssa_undef:
      return gpir_emit_ssa_undef(comp, nir_instr_as_ssa_undef(instr));
   case nir_instr_type_tex:
      return gpir_emit_tex(comp, nir_instr_as_tex(instr));
   case nir_instr_type_jump:
      return gpir_emit_jump(comp, nir_instr_as_jump(instr));
   default:
      gpir_error("unknown NIR instr type %d\n", instr->type);
      return NULL;
   }
}

static gpir_block *gpir_block_create(gpir_compiler *comp)
{
   gpir_block *block = ralloc(comp, gpir_block);
   if (!block)
      return NULL;

   list_inithead(&block->node_list);
   util_dynarray_init(&block->instrs, block);

   return block;
}

static bool gpir_emit_block(gpir_compiler *comp, nir_block *nblock)
{
   gpir_block *block = gpir_block_create(comp);
   if (!block)
      return false;

   list_addtail(&block->list, &comp->block_list);
   block->comp = comp;

   nir_foreach_instr(instr, nblock) {
      gpir_node *node = gpir_emit_instr(comp, instr);
      if (node)
         list_addtail(&node->list, &block->node_list);
   }

   return true;
}

static bool gpir_emit_if(gpir_compiler *comp, nir_if *nif)
{
   gpir_error("if nir_cf_node not support\n");
   return false;
}

static bool gpir_emit_loop(gpir_compiler *comp, nir_loop *nloop)
{
   gpir_error("loop nir_cf_node not support\n");
   return false;
}

static bool gpir_emit_function(gpir_compiler *comp, nir_function_impl *nfunc)
{
   gpir_error("function nir_cf_node not support\n");
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
         gpir_error("unknown NIR node type %d\n", node->type);
         return false;
      }

      if (!ret)
         return false;
   }

   return true;
}

static gpir_compiler *gpir_compiler_create(void *prog, unsigned num_reg, unsigned num_ssa)
{
   gpir_compiler *comp = rzalloc_size(
      prog, sizeof(*comp) + (num_reg + num_ssa) * sizeof(gpir_node *));
   if (!comp)
      return NULL;

   list_inithead(&comp->block_list);
   comp->var_nodes = (gpir_node **)(comp + 1);
   comp->reg_base = num_ssa;
   comp->prog = prog;
   return comp;
}

bool gpir_compile_nir(struct lima_vs_shader_state *prog, struct nir_shader *nir)
{
   nir_function_impl *func = nir_shader_get_entrypoint(nir);
   gpir_compiler *comp = gpir_compiler_create(prog, func->reg_alloc, func->ssa_alloc);
   if (!comp)
      return false;

   comp->constant_base = nir->num_uniforms;

   if (!gpir_emit_cf_list(comp, &func->body))
      goto err_out0;

   gpir_node_print_prog(comp);

   if (!gpir_lower_prog(comp))
      goto err_out0;

   if (!gpir_schedule_prog(comp))
      goto err_out0;

   if (!gpir_codegen_prog(comp))
      goto err_out0;

   ralloc_free(comp);
   return true;

err_out0:
   ralloc_free(comp);
   return false;
}

