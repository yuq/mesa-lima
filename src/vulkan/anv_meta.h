/*
 * Copyright © 2015 Intel Corporation
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
 */

#pragma once

#include "anv_private.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_META_VERTEX_BINDING_COUNT 2

struct anv_meta_saved_state {
   struct anv_vertex_binding old_vertex_bindings[ANV_META_VERTEX_BINDING_COUNT];
   struct anv_descriptor_set *old_descriptor_set0;
   struct anv_pipeline *old_pipeline;
   uint32_t dynamic_flags;
   struct anv_dynamic_state dynamic;
};

void
anv_cmd_buffer_save(struct anv_cmd_buffer *cmd_buffer,
                    struct anv_meta_saved_state *state,
                    uint32_t dynamic_state);

void
anv_cmd_buffer_restore(struct anv_cmd_buffer *cmd_buffer,
                       const struct anv_meta_saved_state *state);

#ifdef __cplusplus
}
#endif
