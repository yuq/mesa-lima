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

#include "compiler/nir/nir.h"
#include "gpir.h"

static bool gpir_emit_block(gpir_prog *prog, nir_block *block)
{
   return false;
}

static bool gpir_emit_if(gpir_prog *prog, nir_if *nif)
{
   return false;
}

static bool gpir_emit_loop(gpir_prog *prog, nir_loop *loop)
{
   return false;
}

static bool gpir_emit_function(gpir_prog *prog, nir_function_impl *func)
{
   return false;
}

static bool gpir_emit_cf_list(gpir_prog *prog, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      bool ret;

      switch (node->type) {
      case nir_cf_node_block:
         ret = gpir_emit_block(prog, nir_cf_node_as_block(node));
         break;
      case nir_cf_node_if:
         ret = gpir_emit_if(prog, nir_cf_node_as_if(node));
         break;
      case nir_cf_node_loop:
         ret = gpir_emit_loop(prog, nir_cf_node_as_loop(node));
         break;
      case nir_cf_node_function:
         ret = gpir_emit_function(prog, nir_cf_node_as_function(node));
         break;
      default:
         fprintf(stderr, "Unknown NIR node type\n");
         return false;
      }

      if (!ret)
         return false;
   }

   return true;
}

gpir_prog *nir_to_gpir(nir_shader *nir)
{
   gpir_prog *prog = gpir_prog_create();
   if (!prog)
      return NULL;

   nir_function_impl *func = nir_shader_get_entrypoint(nir);
   if (!gpir_emit_cf_list(prog, &func->body)) {
      gpir_prog_delete(prog);
      return NULL;
   }

   return prog;
}

