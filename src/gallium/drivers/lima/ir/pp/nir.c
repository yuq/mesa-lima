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

#include <string.h>

#include "util/ralloc.h"
#include "util/bitscan.h"
#include "compiler/nir/nir.h"
#include "ppir.h"
#include "interface.h"

static void *ppir_node_create_ssa(ppir_compiler *comp, ppir_op op, nir_ssa_def *ssa)
{
   ppir_node *node = ppir_node_create(comp, op, ssa->index, 0);
   if (!node)
      return NULL;

   ppir_dest *dest = ppir_node_get_dest(node);
   dest->type = ppir_target_ssa;
   dest->ssa.num_components = ssa->num_components;
   dest->ssa.live_in = INT_MAX;
   dest->ssa.live_out = 0;

   if (node->type == ppir_node_type_load ||
       node->type == ppir_node_type_store)
      dest->ssa.is_head = true;

   return node;
}

static void *ppir_node_create_reg(ppir_compiler *comp, ppir_op op,
                                  nir_reg_dest *reg, unsigned mask)
{
   ppir_node *node = ppir_node_create(comp, op, reg->reg->index, mask);
   if (!node)
      return NULL;

   ppir_dest *dest = ppir_node_get_dest(node);

   list_for_each_entry(ppir_reg, r, &comp->reg_list, list) {
      if (r->index == reg->reg->index) {
         dest->reg = r;
         break;
      }
   }

   dest->type = ppir_target_register;
   dest->write_mask = mask;

   if (node->type == ppir_node_type_load ||
       node->type == ppir_node_type_store)
      dest->reg->is_head = true;

   return node;
}

static void *ppir_node_create_dest(ppir_compiler *comp, ppir_op op,
                                   nir_dest *dest, unsigned mask)
{
   unsigned index = -1;

   if (dest) {
      if (dest->is_ssa)
         return ppir_node_create_ssa(comp, op, &dest->ssa);
      else
         return ppir_node_create_reg(comp, op, &dest->reg, mask);
   }

   return ppir_node_create(comp, op, index, 0);
}

static void ppir_node_add_src(ppir_compiler *comp, ppir_node *node,
                              ppir_src *ps, nir_src *ns)
{
   ppir_node *child = NULL;

   if (ns->is_ssa) {
      child = comp->var_nodes[ns->ssa->index];
      ppir_node_add_child(node, child);
   }
   else {
      unsigned mask;
      ppir_dest *dest = ppir_node_get_dest(node);
      nir_register *reg = ns->reg.reg;
      if (dest)
         mask = dest->write_mask;
      else
         mask = u_bit_consecutive(0, reg->num_components);

      while (mask) {
         int swizzle = ps->swizzle[u_bit_scan(&mask)];
         child = comp->var_nodes[(reg->index << 2) + comp->reg_base + swizzle];
         ppir_node_add_child(node, child);
      }
   }

   ppir_dest *dest = ppir_node_get_dest(child);
   ppir_node_target_assign(ps, dest);
}

static int nir_to_ppir_opcodes[nir_num_opcodes] = {
   /* not supported */
   [0 ... nir_last_opcode] = -1,

   [nir_op_imov] = ppir_op_mov,
   [nir_op_fmul] = ppir_op_mul,
   [nir_op_fadd] = ppir_op_add,
   [nir_op_fneg] = ppir_op_neg,
   [nir_op_fdot_replicated2] = ppir_op_dot2,
   [nir_op_fdot_replicated3] = ppir_op_dot3,
   [nir_op_fdot_replicated4] = ppir_op_dot4,
};

static ppir_node *ppir_emit_alu(ppir_compiler *comp, nir_alu_instr *instr)
{
   int op = nir_to_ppir_opcodes[instr->op];

   if (op < 0) {
      fprintf(stderr, "ppir: unsupport nir_op %d\n", instr->op);
      return NULL;
   }

   ppir_alu_node *node = ppir_node_create_dest(comp, op, &instr->dest.dest,
                                               instr->dest.write_mask);
   if (!node)
      return NULL;

   ppir_dest *pd = &node->dest;
   nir_alu_dest *nd = &instr->dest;
   if (nd->saturate)
      pd->modifier = ppir_outmod_clamp_fraction;

   unsigned num_child = nir_op_infos[instr->op].num_inputs;
   node->num_src = num_child;

   for (int i = 0; i < num_child; i++) {
      nir_alu_src *ns = instr->src + i;
      ppir_src *ps = node->src + i;
      memcpy(ps->swizzle, ns->swizzle, sizeof(ps->swizzle));
      ppir_node_add_src(comp, &node->node, ps, &ns->src);

      ps->absolute = ns->abs;
      ps->negate = ns->negate;
   }

   return &node->node;
}

static ppir_node *ppir_emit_intrinsic(ppir_compiler *comp, nir_intrinsic_instr *instr)
{
   unsigned mask = 0;
   ppir_load_node *lnode;
   ppir_store_node *snode;

   switch (instr->intrinsic) {
   case nir_intrinsic_load_input:
      if (!instr->dest.is_ssa)
         mask = u_bit_consecutive(0, instr->num_components);

      lnode = ppir_node_create_dest(comp, ppir_op_load_varying, &instr->dest, mask);
      if (!lnode)
         return NULL;

      lnode->num_components = instr->num_components;
      lnode->index = nir_intrinsic_base(instr);
      return &lnode->node;

   case nir_intrinsic_load_uniform:
      if (!instr->dest.is_ssa)
         mask = u_bit_consecutive(0, instr->num_components);

      lnode = ppir_node_create_dest(comp, ppir_op_load_uniform, &instr->dest, mask);
      if (!lnode)
         return NULL;

      lnode->num_components = instr->num_components;
      lnode->index = nir_intrinsic_base(instr);

      if (instr->src->is_ssa) {
         ppir_node *child = comp->var_nodes[instr->src->ssa->index];
         if (child->type == ppir_node_type_const) {
            ppir_const_node *c = ppir_node_to_const(child);
            assert(c->constant.num == 1);
            lnode->index += c->constant.value[0].i;
         }
      }

      return &lnode->node;

   case nir_intrinsic_store_output:
      snode = ppir_node_create_dest(comp, ppir_op_store_color, NULL, 0);
      if (!snode)
         return NULL;

      snode->index = nir_intrinsic_base(instr);

      for (int i = 0; i < 4; i++)
         snode->src.swizzle[i] = i;
      ppir_node_add_src(comp, &snode->node, &snode->src, instr->src);

      return &snode->node;

   default:
      fprintf(stderr, "ppir: unsupport nir_intrinsic_instr %d\n", instr->intrinsic);
      return NULL;
   }
}

static ppir_node *ppir_emit_load_const(ppir_compiler *comp, nir_load_const_instr *instr)
{
   ppir_const_node *node = ppir_node_create_ssa(comp, ppir_op_const, &instr->def);
   if (!node)
      return NULL;

   assert(instr->def.bit_size == 32);

   for (int i = 0; i < instr->def.num_components; i++)
      node->constant.value[i].i = instr->value.i32[i];
   node->constant.num = instr->def.num_components;

   return &node->node;
}

static ppir_node *ppir_emit_ssa_undef(ppir_compiler *comp, nir_ssa_undef_instr *instr)
{
   fprintf(stderr, "ppir: nir_ssa_undef_instr not support\n");
   return NULL;
}

static ppir_node *ppir_emit_tex(ppir_compiler *comp, nir_tex_instr *instr)
{
   fprintf(stderr, "ppir: nir_tex_instr not support\n");
   return NULL;
}

static ppir_node *ppir_emit_jump(ppir_compiler *comp, nir_jump_instr *instr)
{
   fprintf(stderr, "ppir: nir_jump_instr not support\n");
   return NULL;
}

static ppir_node *ppir_emit_instr(ppir_compiler *comp, nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_alu:
      return ppir_emit_alu(comp, nir_instr_as_alu(instr));
   case nir_instr_type_intrinsic:
      return ppir_emit_intrinsic(comp, nir_instr_as_intrinsic(instr));
   case nir_instr_type_load_const:
      return ppir_emit_load_const(comp, nir_instr_as_load_const(instr));
   case nir_instr_type_ssa_undef:
      return ppir_emit_ssa_undef(comp, nir_instr_as_ssa_undef(instr));
   case nir_instr_type_tex:
      return ppir_emit_tex(comp, nir_instr_as_tex(instr));
   case nir_instr_type_jump:
      return ppir_emit_jump(comp, nir_instr_as_jump(instr));
   default:
      fprintf(stderr, "ppir: unknown NIR instr type %d\n", instr->type);
      return NULL;
   }
}

static ppir_block *ppir_block_create(ppir_compiler *comp)
{
   ppir_block *block = ralloc(comp, ppir_block);
   if (!block)
      return NULL;

   list_inithead(&block->node_list);
   list_inithead(&block->instr_list);

   return block;
}

static bool ppir_emit_block(ppir_compiler *comp, nir_block *nblock)
{
   ppir_block *block = ppir_block_create(comp);
   if (!block)
      return false;

   list_addtail(&block->list, &comp->block_list);
   block->comp = comp;

   nir_foreach_instr(instr, nblock) {
      ppir_node *node = ppir_emit_instr(comp, instr);
      if (node)
         list_addtail(&node->list, &block->node_list);
   }

   return true;
}

static bool ppir_emit_if(ppir_compiler *comp, nir_if *nif)
{
   fprintf(stderr, "ppir: if nir_cf_node not support\n");
   return false;
}

static bool ppir_emit_loop(ppir_compiler *comp, nir_loop *nloop)
{
   fprintf(stderr, "ppir: loop nir_cf_node not support\n");
   return false;
}

static bool ppir_emit_function(ppir_compiler *comp, nir_function_impl *nfunc)
{
   fprintf(stderr, "ppir: function nir_cf_node not support\n");
   return false;
}

static bool ppir_emit_cf_list(ppir_compiler *comp, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      bool ret;

      switch (node->type) {
      case nir_cf_node_block:
         ret = ppir_emit_block(comp, nir_cf_node_as_block(node));
         break;
      case nir_cf_node_if:
         ret = ppir_emit_if(comp, nir_cf_node_as_if(node));
         break;
      case nir_cf_node_loop:
         ret = ppir_emit_loop(comp, nir_cf_node_as_loop(node));
         break;
      case nir_cf_node_function:
         ret = ppir_emit_function(comp, nir_cf_node_as_function(node));
         break;
      default:
         fprintf(stderr, "ppir: unknown NIR node type %d\n", node->type);
         return false;
      }

      if (!ret)
         return false;
   }

   return true;
}

static ppir_compiler *ppir_compiler_create(void *prog, unsigned num_reg, unsigned num_ssa)
{
   ppir_compiler *comp = rzalloc_size(
      prog, sizeof(*comp) + ((num_reg << 2) + num_ssa) * sizeof(ppir_node *));
   if (!comp)
      return NULL;

   list_inithead(&comp->block_list);
   list_inithead(&comp->reg_list);

   comp->cur_reg_index = num_reg;
   comp->var_nodes = (ppir_node **)(comp + 1);
   comp->reg_base = num_ssa;
   comp->prog = prog;
   return comp;
}

bool ppir_compile_nir(struct lima_fs_shader_state *prog, nir_shader *nir,
                      struct ra_regs *ra)
{
   nir_function_impl *func = nir_shader_get_entrypoint(nir);
   ppir_compiler *comp = ppir_compiler_create(prog, func->reg_alloc, func->ssa_alloc);
   if (!comp)
      return false;

   comp->ra = ra;

   foreach_list_typed(nir_register, reg, node, &func->registers) {
      ppir_reg *r = ralloc(comp, ppir_reg);
      if (!r)
         return false;

      r->index = reg->index;
      r->num_components = reg->num_components;
      r->live_in = INT_MAX;
      r->live_out = 0;
      list_addtail(&r->list, &comp->reg_list);
   }

   if (!ppir_emit_cf_list(comp, &func->body))
      goto err_out0;
   ppir_node_print_prog(comp);

   if (!ppir_lower_prog(comp))
      goto err_out0;

   if (!ppir_schedule_prog(comp))
      goto err_out0;

   if (!ppir_regalloc_prog(comp))
      goto err_out0;

   if (!ppir_codegen_prog(comp))
      goto err_out0;

   ralloc_free(comp);
   return true;

err_out0:
   ralloc_free(comp);
   return false;
}

