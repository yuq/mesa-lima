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
#include <string.h>

#include "gpir.h"

void gpir_instr_init(gpir_instr *instr)
{
   instr->alu_num_slot_free = 6;
   instr->reg_status = ~0ull;
}

static bool gpir_instr_insert_alu_check(gpir_instr *instr, gpir_node *node)
{
   /* check if this node is child of one store node.
    * complex1 won't be any of this instr's store node's child,
    * because it has two instr latency before store can use it.
    */
   for (int i = GPIR_INSTR_SLOT_STORE0; i < GPIR_INSTR_SLOT_STORE3; i++) {
      gpir_store_node *s = gpir_node_to_store(instr->slots[i]);
      if (s && s->child == node) {
         instr->alu_num_slot_needed_by_store--;
         instr->alu_num_slot_free--;
         return true;
      }
   }

   int consume_slot = node->op == gpir_op_complex1 ? 2 : 1;

   /* not a child of any store node, so must reserve alu slot for store node */
   if (instr->alu_num_slot_free - consume_slot <
       instr->alu_num_slot_needed_by_store)
      return false;

   instr->alu_num_slot_free -= consume_slot;
   return true;
}

static void gpir_instr_remove_alu(gpir_instr *instr, gpir_node *node)
{
   for (int i = GPIR_INSTR_SLOT_STORE0; i < GPIR_INSTR_SLOT_STORE3; i++) {
      gpir_store_node *s = gpir_node_to_store(instr->slots[i]);
      if (s && s->child == node) {
         instr->alu_num_slot_needed_by_store++;
         instr->alu_num_slot_free++;
         return;
      }
   }

   instr->alu_num_slot_free += node->op == gpir_op_complex1 ? 2 : 1;
}

static bool gpir_instr_insert_reg0_check(gpir_instr *instr, gpir_node *node)
{
   gpir_load_node *load = gpir_node_to_load(node);
   int i = node->sched_pos - GPIR_INSTR_SLOT_REG0_LOAD0;

   if (load->component != i)
      return false;

   if (instr->reg0_is_attr && node->op != gpir_op_load_attribute)
      return false;

   if (instr->reg0_use_count) {
       if (instr->reg0_index != load->index)
          return false;
   }
   else {
      instr->reg0_is_attr = node->op == gpir_op_load_attribute;
      instr->reg0_index = load->index;
   }

   instr->reg0_use_count++;
   return true;
}

static void gpir_instr_remove_reg0(gpir_instr *instr, gpir_node *node)
{
   instr->reg0_use_count--;
   if (!instr->reg0_use_count)
      instr->reg0_is_attr = false;
}

static bool gpir_instr_insert_reg1_check(gpir_instr *instr, gpir_node *node)
{
   gpir_load_node *load = gpir_node_to_load(node);
   int i = node->sched_pos - GPIR_INSTR_SLOT_REG1_LOAD0;

   if (load->component != i)
      return false;

   if (instr->reg1_use_count) {
       if (instr->reg1_index != load->index)
          return false;
   }
   else
      instr->reg1_index = load->index;

   instr->reg1_use_count++;
   return true;
}

static void gpir_instr_remove_reg1(gpir_instr *instr, gpir_node *node)
{
   instr->reg1_use_count--;
}

static bool gpir_instr_insert_mem_check(gpir_instr *instr, gpir_node *node)
{
   gpir_load_node *load = gpir_node_to_load(node);
   int i = node->sched_pos - GPIR_INSTR_SLOT_MEM_LOAD0;

   if (load->component != i)
      return false;

   if (instr->mem_is_temp && node->op != gpir_op_load_temp)
      return false;

   if (instr->mem_use_count) {
       if (instr->mem_index != load->index)
          return false;
   }
   else {
      instr->mem_is_temp = node->op == gpir_op_load_temp;
      instr->mem_index = load->index;
   }

   instr->mem_use_count++;
   return true;
}

static void gpir_instr_remove_mem(gpir_instr *instr, gpir_node *node)
{
   instr->mem_use_count--;
   if (!instr->mem_use_count)
      instr->mem_is_temp = false;
}

static bool gpir_instr_insert_store_check(gpir_instr *instr, gpir_node *node)
{
   gpir_store_node *store = gpir_node_to_store(node);
   int i = node->sched_pos - GPIR_INSTR_SLOT_STORE0;

   if (store->component != i)
      return false;

   i >>= 1;
   switch (instr->store_content[i]) {
   case GPIR_INSTR_STORE_NONE:
      /* store temp has only one address reg for two store unit */
      if (node->op == gpir_op_store_temp &&
          instr->store_content[!i] == GPIR_INSTR_STORE_TEMP &&
          instr->store_index[!i] != store->index)
         return false;
      break;

   case GPIR_INSTR_STORE_VARYING:
      if (node->op != gpir_op_store_varying ||
          instr->store_index[i] != store->index)
         return false;
      break;

   case GPIR_INSTR_STORE_REG:
      if (node->op != gpir_op_store_reg ||
          instr->store_index[i] != store->index)
         return false;
      break;

   case GPIR_INSTR_STORE_TEMP:
      if (node->op != gpir_op_store_temp ||
          instr->store_index[i] != store->index)
         return false;
      break;
   }

   /* check if any store node has the same child as this node */
   for (int j = GPIR_INSTR_SLOT_STORE0; j <= GPIR_INSTR_SLOT_STORE3; j++) {
      gpir_store_node *s = gpir_node_to_store(instr->slots[j]);
      if (s && s->child == store->child)
         goto out;
   }

   /* check if the child is alrady in this instr's alu slot,
    * this may happen when store an scheduled alu node to reg
    */
   for (int j = GPIR_INSTR_SLOT_MUL0; j <= GPIR_INSTR_SLOT_PASS; j++) {
      if (store->child == instr->slots[j])
         goto out;
   }

   /* no store node has the same child as this node, and child is not
    * already in this instr's alu slot, so instr must have some free
    * alu slot to insert this node's child
    */
   if (!instr->alu_num_slot_free)
      return false;

   instr->alu_num_slot_needed_by_store++;

out:
   if (instr->store_content[i] == GPIR_INSTR_STORE_NONE) {
      if (node->op == gpir_op_store_varying)
         instr->store_content[i] = GPIR_INSTR_STORE_VARYING;
      else if (node->op == gpir_op_store_reg)
         instr->store_content[i] = GPIR_INSTR_STORE_REG;
      else
         instr->store_content[i] = GPIR_INSTR_STORE_TEMP;

      instr->store_index[i] = store->index;
   }
   return true;
}

static void gpir_instr_remove_store(gpir_instr *instr, gpir_node *node)
{
   gpir_store_node *store = gpir_node_to_store(node);
   int component = node->sched_pos - GPIR_INSTR_SLOT_STORE0;
   int other_slot = GPIR_INSTR_SLOT_STORE0 + (component ^ 1);

   for (int j = GPIR_INSTR_SLOT_STORE0; j <= GPIR_INSTR_SLOT_STORE3; j++) {
      gpir_store_node *s = gpir_node_to_store(instr->slots[j]);
      if (s && s->child == store->child)
         goto out;
   }

   for (int j = GPIR_INSTR_SLOT_MUL0; j <= GPIR_INSTR_SLOT_PASS; j++) {
      if (store->child == instr->slots[j])
         goto out;
   }

   instr->alu_num_slot_needed_by_store--;

out:
   if (!instr->slots[other_slot])
      instr->store_content[component >> 1] = GPIR_INSTR_STORE_NONE;
}

bool gpir_instr_try_insert_node(gpir_instr *instr, gpir_node *node)
{
   if (instr->slots[node->sched_pos])
      return false;

   if (node->op == gpir_op_complex1) {
      if (instr->slots[GPIR_INSTR_SLOT_MUL1])
         return false;
   }

   if (node->sched_pos >= GPIR_INSTR_SLOT_MUL0 &&
       node->sched_pos <= GPIR_INSTR_SLOT_PASS) {
      if (!gpir_instr_insert_alu_check(instr, node))
         return false;
   }
   else if (node->sched_pos >= GPIR_INSTR_SLOT_REG0_LOAD0 &&
            node->sched_pos <= GPIR_INSTR_SLOT_REG0_LOAD3) {
      if (!gpir_instr_insert_reg0_check(instr, node))
         return false;
   }
   else if (node->sched_pos >= GPIR_INSTR_SLOT_REG1_LOAD0 &&
            node->sched_pos <= GPIR_INSTR_SLOT_REG1_LOAD3) {
      if (!gpir_instr_insert_reg1_check(instr, node))
         return false;
   }
   else if (node->sched_pos >= GPIR_INSTR_SLOT_MEM_LOAD0 &&
            node->sched_pos <= GPIR_INSTR_SLOT_MEM_LOAD3) {
      if (!gpir_instr_insert_mem_check(instr, node))
         return false;
   }
   else if (node->sched_pos >= GPIR_INSTR_SLOT_STORE0 &&
            node->sched_pos <= GPIR_INSTR_SLOT_STORE3) {
      if (!gpir_instr_insert_store_check(instr, node))
         return false;
   }

   instr->slots[node->sched_pos] = node;

   if (node->op == gpir_op_complex1)
      instr->slots[GPIR_INSTR_SLOT_MUL1] = node;

   return true;
}

void gpir_instr_remove_node(gpir_instr *instr, gpir_node *node)
{
   if (node->sched_pos >= GPIR_INSTR_SLOT_MUL0 &&
       node->sched_pos <= GPIR_INSTR_SLOT_PASS)
      gpir_instr_remove_alu(instr, node);
   else if (node->sched_pos >= GPIR_INSTR_SLOT_REG0_LOAD0 &&
            node->sched_pos <= GPIR_INSTR_SLOT_REG0_LOAD3)
      gpir_instr_remove_reg0(instr, node);
   else if (node->sched_pos >= GPIR_INSTR_SLOT_REG1_LOAD0 &&
            node->sched_pos <= GPIR_INSTR_SLOT_REG1_LOAD3)
      gpir_instr_remove_reg1(instr, node);
   else if (node->sched_pos >= GPIR_INSTR_SLOT_MEM_LOAD0 &&
            node->sched_pos <= GPIR_INSTR_SLOT_MEM_LOAD3)
      gpir_instr_remove_mem(instr, node);
   else if (node->sched_pos >= GPIR_INSTR_SLOT_STORE0 &&
            node->sched_pos <= GPIR_INSTR_SLOT_STORE3)
      gpir_instr_remove_store(instr, node);

   instr->slots[node->sched_pos] = NULL;

   if (node->op == gpir_op_complex1)
      instr->slots[GPIR_INSTR_SLOT_MUL1] = NULL;
}

void gpir_instr_print_prog(gpir_compiler *comp)
{
   struct {
      int len;
      char *name;
   } fields[] = {
      [GPIR_INSTR_SLOT_MUL0] = { 4, "mul0" },
      [GPIR_INSTR_SLOT_MUL1] = { 4, "mul1" },
      [GPIR_INSTR_SLOT_ADD0] = { 4, "add0" },
      [GPIR_INSTR_SLOT_ADD1] = { 4, "add1" },
      [GPIR_INSTR_SLOT_REG0_LOAD3] = { 15, "load0" },
      [GPIR_INSTR_SLOT_REG1_LOAD3] = { 15, "load1" },
      [GPIR_INSTR_SLOT_MEM_LOAD3] = { 15, "load2" },
      [GPIR_INSTR_SLOT_BRANCH] = { 4, "bnch" },
      [GPIR_INSTR_SLOT_STORE3] = { 15, "store" },
      [GPIR_INSTR_SLOT_COMPLEX] = { 4, "cmpl" },
      [GPIR_INSTR_SLOT_PASS] = { 4, "pass" },
   };

   printf("========prog instr========\n");
   printf("     ");
   for (int i = 0; i < GPIR_INSTR_SLOT_NUM; i++) {
      if (fields[i].len)
         printf("%-*s ", fields[i].len, fields[i].name);
   }
   printf("\n");

   int index = 0;
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      printf("-------block instr------\n");
      for (int i = gpir_instr_array_n(&block->instrs) - 1; i >= 0; i--) {
         printf("%03d: ", index++);

         gpir_instr *instr = gpir_instr_array_e(&block->instrs, i);
         char buff[16] = "null";
         int start = 0;
         for (int j = 0; j < GPIR_INSTR_SLOT_NUM; j++) {
            gpir_node *node = instr->slots[j];
            if (fields[j].len) {
               if (node)
                  snprintf(buff + start, sizeof(buff) - start, "%d", node->index);
               printf("%-*s ", fields[j].len, buff);

               strcpy(buff, "null");
               start = 0;
            }
            else {
               if (node)
                  start += snprintf(buff + start, sizeof(buff) - start, "%d", node->index);
               start += snprintf(buff + start, sizeof(buff) - start, "|");
            }
         }
         printf("\n");
      }
   }
   printf("==========================\n");
}
