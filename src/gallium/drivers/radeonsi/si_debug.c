/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *      Marek Olšák <maraeo@gmail.com>
 */

#include "si_pipe.h"
#include "si_shader.h"
#include "sid.h"


static void si_dump_shader(struct si_shader_selector *sel, const char *name,
			   FILE *f)
{
	if (!sel || !sel->current)
		return;

	fprintf(f, "%s shader disassembly:\n", name);
	si_dump_shader_key(sel->type, &sel->current->key, f);
	fprintf(f, "%s\n\n", sel->current->binary.disasm_string);
}

static void si_dump_debug_state(struct pipe_context *ctx, FILE *f,
				unsigned flags)
{
	struct si_context *sctx = (struct si_context*)ctx;

	si_dump_shader(sctx->vs_shader, "Vertex", f);
	si_dump_shader(sctx->tcs_shader, "Tessellation control", f);
	si_dump_shader(sctx->tes_shader, "Tessellation evaluation", f);
	si_dump_shader(sctx->gs_shader, "Geometry", f);
	si_dump_shader(sctx->ps_shader, "Fragment", f);
	fprintf(f, "Done.\n");
}

void si_init_debug_functions(struct si_context *sctx)
{
	sctx->b.b.dump_debug_state = si_dump_debug_state;
}
