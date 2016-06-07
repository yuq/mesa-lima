/*
 * Copyright (c) 2014 Intel Corporation
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


#include "main/context.h"
#include "main/blend.h"
#include "main/mtypes.h"
#include "main/samplerobj.h"
#include "main/texformat.h"
#include "program/prog_parameter.h"

#include "intel_mipmap_tree.h"
#include "intel_batchbuffer.h"
#include "intel_tex.h"
#include "intel_fbo.h"
#include "intel_buffer_objects.h"

#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "brw_wm.h"

void
gen6_init_vtable_surface_functions(struct brw_context *brw)
{
   gen4_init_vtable_surface_functions(brw);
   brw->vtbl.update_renderbuffer_surface = brw_update_renderbuffer_surface;
}
