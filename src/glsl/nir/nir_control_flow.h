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
 */

/** puts a control flow node immediately after another control flow node */
void nir_cf_node_insert_after(nir_cf_node *node, nir_cf_node *after);

/** puts a control flow node immediately before another control flow node */
void nir_cf_node_insert_before(nir_cf_node *node, nir_cf_node *before);

/** puts a control flow node at the beginning of a list from an if, loop, or function */
void nir_cf_node_insert_begin(struct exec_list *list, nir_cf_node *node);

/** puts a control flow node at the end of a list from an if, loop, or function */
void nir_cf_node_insert_end(struct exec_list *list, nir_cf_node *node);

/** removes a control flow node, doing any cleanup necessary */
void nir_cf_node_remove(nir_cf_node *node);

#ifdef __cplusplus
}
#endif
