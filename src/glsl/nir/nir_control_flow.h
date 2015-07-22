/*
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Connor Abbott (cwabbott0@gmail.com)
 *
 */

#include "nir.h"

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** NIR Control Flow Modification
 *
 * This file contains various API's that make modifying control flow in NIR,
 * while maintaining the invariants checked by the validator, much easier.
 * There are two parts to this:
 *
 * 1. Inserting control flow (if's and loops) in various places, for creating
 *    IR either from scratch or as part of some lowering pass.
 * 2. Taking existing pieces of the IR and either moving them around or
 *    deleting them.
 */

/* Helper struct for representing a point to extract/insert. Helps reduce the
 * combinatorial explosion of possible points to extract.
 */

typedef enum {
   nir_cursor_before_block,
   nir_cursor_after_block,
   nir_cursor_before_instr,
   nir_cursor_after_instr,
} nir_cursor_option;

typedef struct {
   nir_cursor_option option;
   union {
      nir_block *block;
      nir_instr *instr;
   };
} nir_cursor;

static inline nir_cursor
nir_before_block(nir_block *block)
{
   nir_cursor cursor;
   cursor.option = nir_cursor_before_block;
   cursor.block = block;
   return cursor;
}

static inline nir_cursor
nir_after_block(nir_block *block)
{
   nir_cursor cursor;
   cursor.option = nir_cursor_after_block;
   cursor.block = block;
   return cursor;
}

static inline nir_cursor
nir_before_instr(nir_instr *instr)
{
   nir_cursor cursor;
   cursor.option = nir_cursor_before_instr;
   cursor.instr = instr;
   return cursor;
}

static inline nir_cursor
nir_after_instr(nir_instr *instr)
{
   nir_cursor cursor;
   cursor.option = nir_cursor_after_instr;
   cursor.instr = instr;
   return cursor;
}

static inline nir_cursor
nir_before_cf_node(nir_cf_node *node)
{
   if (node->type == nir_cf_node_block)
      return nir_before_block(nir_cf_node_as_block(node));

   return nir_after_block(nir_cf_node_as_block(nir_cf_node_prev(node)));
}

static inline nir_cursor
nir_after_cf_node(nir_cf_node *node)
{
   if (node->type == nir_cf_node_block)
      return nir_after_block(nir_cf_node_as_block(node));

   return nir_before_block(nir_cf_node_as_block(nir_cf_node_next(node)));
}

static inline nir_cursor
nir_before_cf_list(struct exec_list *cf_list)
{
   nir_cf_node *first_node = exec_node_data(nir_cf_node,
                                            exec_list_get_head(cf_list), node);
   return nir_before_cf_node(first_node);
}

static inline nir_cursor
nir_after_cf_list(struct exec_list *cf_list)
{
   nir_cf_node *last_node = exec_node_data(nir_cf_node,
                                           exec_list_get_tail(cf_list), node);
   return nir_after_cf_node(last_node);
}

/** Control flow insertion. */

/** puts a control flow node where the cursor is */
void nir_cf_node_insert(nir_cursor cursor, nir_cf_node *node);

/** puts a control flow node immediately after another control flow node */
static inline void
nir_cf_node_insert_after(nir_cf_node *node, nir_cf_node *after)
{
   nir_cf_node_insert(nir_after_cf_node(node), after);
}

/** puts a control flow node immediately before another control flow node */
static inline void
nir_cf_node_insert_before(nir_cf_node *node, nir_cf_node *before)
{
   nir_cf_node_insert(nir_before_cf_node(node), before);
}

/** puts a control flow node at the beginning of a list from an if, loop, or function */
static inline void
nir_cf_node_insert_begin(struct exec_list *list, nir_cf_node *node)
{
   nir_cf_node_insert(nir_before_cf_list(list), node);
}

/** puts a control flow node at the end of a list from an if, loop, or function */
static inline void
nir_cf_node_insert_end(struct exec_list *list, nir_cf_node *node)
{
   nir_cf_node_insert(nir_after_cf_list(list), node);
}


/** Control flow motion.
 *
 * These functions let you take a part of a control flow list (basically
 * equivalent to a series of statement in GLSL) and "extract" it from the IR,
 * so that it's a free-floating piece of IR that can be either re-inserted
 * somewhere else or deleted entirely. A few notes on using it:
 *
 * 1. Phi nodes are considered attached to the piece of control flow that
 *    their sources come from. There are three places where phi nodes can
 *    occur, which are the three places where a block can have multiple
 *    predecessors:
 *
 *    1) After an if statement, if neither branch ends in a jump.
 *    2) After a loop, if there are multiple break's.
 *    3) At the beginning of a loop.
 *
 *    For #1, the phi node is considered to be part of the if, and for #2 and
 *    #3 the phi node is considered to be part of the loop. This allows us to
 *    keep phi's intact, but it means that phi nodes cannot be separated from
 *    the control flow they come from. For example, extracting an if without
 *    extracting all the phi nodes after it is not allowed, and neither is
 *    extracting only some of the phi nodes at the beginning of a block. It
 *    also means that extracting from the beginning of a basic block actually
 *    means extracting from the first non-phi instruction, since there's no
 *    situation where extracting phi nodes without extracting what comes
 *    before them makes any sense.
 *
 * 2. Phi node sources are guaranteed to remain valid, meaning that they still
 *    correspond one-to-one with the predecessors of the basic block they're
 *    part of. In addition, the original sources will be preserved unless they
 *    correspond to a break or continue that was deleted. However, no attempt
 *    is made to ensure that SSA form is maintained. In particular, it is
 *    *not* guaranteed that definitions of SSA values will dominate all their
 *    uses after all is said and done. Either the caller must ensure that this
 *    is the case, or it must insert extra phi nodes to restore SSA.
 *
 * 3. It is invalid to move a piece of IR with a break/continue outside of the
 *    loop it references. Doing this will result in invalid
 *    successors/predecessors and phi node sources.
 *
 * 4. It is invalid to move a piece of IR from one function implementation to
 *    another.
 *
 * 5. Extracting a control flow list will leave lots of dangling references to
 *    and from other pieces of the IR. It also leaves things in a not 100%
 *    consistent state. This means that some things (e.g. inserting
 *    instructions) might not work reliably on the extracted control flow. It
 *    also means that extracting control flow without re-inserting it or
 *    deleting it is a Bad Thing (tm).
 */

typedef struct {
   struct exec_list list;
   nir_function_impl *impl; /* for cleaning up if the list is deleted */
} nir_cf_list;

void nir_cf_extract(nir_cf_list *extracted, nir_cursor begin, nir_cursor end);

void nir_cf_reinsert(nir_cf_list *cf_list, nir_cursor cursor);

void nir_cf_delete(nir_cf_list *cf_list);

static inline void
nir_cf_list_extract(nir_cf_list *extracted, struct exec_list *cf_list)
{
   nir_cf_extract(extracted, nir_before_cf_list(cf_list),
                  nir_after_cf_list(cf_list));
}

/** removes a control flow node, doing any cleanup necessary */
static inline void
nir_cf_node_remove(nir_cf_node *node)
{
   nir_cf_list list;
   nir_cf_extract(&list, nir_before_cf_node(node), nir_after_cf_node(node));
   nir_cf_delete(&list);
}

#ifdef __cplusplus
}
#endif
