/*
 * Copyright © 2017 Red Hat
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
#include "nir.h"
#include "ac_shader_info.h"
#include "ac_nir_to_llvm.h"
static void
gather_intrinsic_info(nir_intrinsic_instr *instr, struct ac_shader_info *info)
{
	switch (instr->intrinsic) {
	case nir_intrinsic_interp_var_at_sample:
		info->ps.needs_sample_positions = true;
		break;
	default:
		break;
	}
}

static void
gather_info_block(nir_block *block, struct ac_shader_info *info)
{
	nir_foreach_instr(instr, block) {
		switch (instr->type) {
		case nir_instr_type_intrinsic:
			gather_intrinsic_info(nir_instr_as_intrinsic(instr), info);
			break;
		default:
			break;
		}
	}
}

void
ac_nir_shader_info_pass(struct nir_shader *nir,
			const struct ac_nir_compiler_options *options,
			struct ac_shader_info *info)
{
	struct nir_function *func = (struct nir_function *)exec_list_get_head(&nir->functions);
	nir_foreach_block(block, func->impl) {
		gather_info_block(block, info);
	}
}
