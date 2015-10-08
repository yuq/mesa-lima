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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */


#ifndef BRW_CS_H
#define BRW_CS_H

#include "brw_program.h"

struct brw_cs_prog_key {
   uint32_t program_string_id;
   struct brw_sampler_prog_key_data tex;
};

#ifdef __cplusplus
extern "C" {
#endif

void
brw_upload_cs_prog(struct brw_context *brw);

struct nir_shader;

const unsigned *
brw_cs_emit(const struct brw_compiler *compiler, void *log_data,
            void *mem_ctx,
            const struct brw_cs_prog_key *key,
            struct brw_cs_prog_data *prog_data,
            const struct nir_shader *shader,
            int shader_time_index,
            unsigned *final_assembly_size,
            char **error_str);

void
brw_cs_fill_local_id_payload(const struct brw_cs_prog_data *cs_prog_data,
                             void *buffer, uint32_t threads, uint32_t stride);

#ifdef __cplusplus
}
#endif

#endif /* BRW_CS_H */
