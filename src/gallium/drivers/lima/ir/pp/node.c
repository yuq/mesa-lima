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

#include <stdio.h>

#include "util/u_math.h"
#include "util/ralloc.h"
#include "util/hash_table.h"

#include "ppir.h"

const ppir_op_info ppir_op_infos[] = {
   [ppir_op_mov] = {
      .name = "mov",
      .type = ppir_node_type_alu,
   },
   [ppir_op_load_varying] = {
      .name = "ld_var",
      .type = ppir_node_type_load,
   },
   [ppir_op_const] = {
      .name = "const",
      .type = ppir_node_type_const,
   },
   [ppir_op_store_color] = {
      .name = "st_col",
      .type = ppir_node_type_store,
   },
};

void *ppir_node_create(ppir_compiler *comp, ppir_op op, int index)
{
   static const int node_size[] = {
      [ppir_node_type_alu] = sizeof(ppir_alu_node),
      [ppir_node_type_const] = sizeof(ppir_const_node),
      [ppir_node_type_load] = sizeof(ppir_load_node),
      [ppir_node_type_store] = sizeof(ppir_store_node),
   };

   ppir_node_type type = ppir_op_infos[op].type;
   int size = node_size[type];
   ppir_node *node = rzalloc_size(comp, size);
   if (!node)
      return NULL;

   node->preds = _mesa_set_create(node, _mesa_hash_pointer, _mesa_key_pointer_equal);
   if (!node->preds)
      goto err_out;
   node->succs = _mesa_set_create(node, _mesa_hash_pointer, _mesa_key_pointer_equal);
   if (!node->succs)
      goto err_out;

   if (index >= 0) {
      comp->var_nodes[index] = node;
      if (index < comp->reg_base)
         snprintf(node->name, sizeof(node->name), "ssa%d", index);
      else
         snprintf(node->name, sizeof(node->name), "reg%d", index - comp->reg_base);
   }
   else
      snprintf(node->name, sizeof(node->name), "new");

   node->op = op;
   node->type = type;
   node->index = comp->cur_index++;

   return node;

err_out:
   ralloc_free(node);
   return NULL;
}

static void ppir_node_create_dep(ppir_node *succ, ppir_node *pred,
                                 bool is_child_dep, bool is_offset)
{
   /* don't add duplicated dep */
   ppir_node_foreach_pred(succ, entry) {
      ppir_node *node = ppir_node_from_entry(entry, pred);
      if (node == pred)
         return;
   }

   ppir_dep_info *dep = ralloc(succ, ppir_dep_info);

   dep->pred = pred;
   dep->succ = succ;
   dep->is_child_dep = is_child_dep;
   dep->is_offset = is_offset;

   _mesa_set_add(succ->preds, dep);
   _mesa_set_add(pred->succs, dep);
}

void ppir_node_add_child(ppir_node *parent, ppir_node *child)
{
   ppir_node_create_dep(parent, child, true, false);
}

static void ppir_node_print_node(ppir_node *node, int space)
{
   for (int i = 0; i < space; i++)
      printf(" ");
   printf("%s%s %d %s\n", node->printed && !ppir_node_is_leaf(node) ? "+" : "",
          ppir_op_infos[node->op].name, node->index, node->name);

   if (!node->printed) {
      ppir_node_foreach_pred(node, entry) {
         ppir_node *pred = ppir_node_from_entry(entry, pred);
         ppir_node_print_node(pred, space + 2);
      }

      node->printed = true;
   }
}

void ppir_node_print_prog(ppir_compiler *comp)
{
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_node, node, &block->node_list, list) {
         node->printed = false;
      }
   }

   printf("========prog========\n");
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      printf("-------block------\n");
      list_for_each_entry(ppir_node, node, &block->node_list, list) {
         if (ppir_node_is_root(node))
            ppir_node_print_node(node, 0);
      }
   }
   printf("====================\n");
}
