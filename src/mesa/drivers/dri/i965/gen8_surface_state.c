/*
 * Copyright © 2012 Intel Corporation
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

#include "main/blend.h"
#include "main/mtypes.h"
#include "main/samplerobj.h"
#include "main/texformat.h"
#include "main/teximage.h"
#include "program/prog_parameter.h"
#include "program/prog_instruction.h"

#include "intel_mipmap_tree.h"
#include "intel_batchbuffer.h"
#include "intel_tex.h"
#include "intel_fbo.h"
#include "intel_buffer_objects.h"
#include "intel_image.h"

#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "brw_wm.h"
#include "isl/isl.h"

static uint32_t *
gen8_allocate_surface_state(struct brw_context *brw,
                            uint32_t *out_offset, int index)
{
   int dwords = brw->gen >= 9 ? 16 : 13;
   uint32_t *surf = __brw_state_batch(brw, AUB_TRACE_SURFACE_STATE,
                                      dwords * 4, 64, index, out_offset);
   memset(surf, 0, dwords * 4);
   return surf;
}

static void
gen8_emit_buffer_surface_state(struct brw_context *brw,
                               uint32_t *out_offset,
                               drm_intel_bo *bo,
                               unsigned buffer_offset,
                               unsigned surface_format,
                               unsigned buffer_size,
                               unsigned pitch,
                               bool rw)
{
   const unsigned mocs = brw->gen >= 9 ? SKL_MOCS_WB : BDW_MOCS_WB;
   uint32_t *surf = gen8_allocate_surface_state(brw, out_offset, -1);

   surf[0] = BRW_SURFACE_BUFFER << BRW_SURFACE_TYPE_SHIFT |
             surface_format << BRW_SURFACE_FORMAT_SHIFT |
             BRW_SURFACE_RC_READ_WRITE;
   surf[1] = SET_FIELD(mocs, GEN8_SURFACE_MOCS);

   surf[2] = SET_FIELD((buffer_size - 1) & 0x7f, GEN7_SURFACE_WIDTH) |
             SET_FIELD(((buffer_size - 1) >> 7) & 0x3fff, GEN7_SURFACE_HEIGHT);
   if (surface_format == BRW_SURFACEFORMAT_RAW)
      surf[3] = SET_FIELD(((buffer_size - 1) >> 21) & 0x3ff, BRW_SURFACE_DEPTH);
   else
      surf[3] = SET_FIELD(((buffer_size - 1) >> 21) & 0x3f, BRW_SURFACE_DEPTH);
   surf[3] |= (pitch - 1);
   surf[7] = SET_FIELD(HSW_SCS_RED,   GEN7_SURFACE_SCS_R) |
             SET_FIELD(HSW_SCS_GREEN, GEN7_SURFACE_SCS_G) |
             SET_FIELD(HSW_SCS_BLUE,  GEN7_SURFACE_SCS_B) |
             SET_FIELD(HSW_SCS_ALPHA, GEN7_SURFACE_SCS_A);
   /* reloc */
   *((uint64_t *) &surf[8]) = (bo ? bo->offset64 : 0) + buffer_offset;

   /* Emit relocation to surface contents. */
   if (bo) {
      drm_intel_bo_emit_reloc(brw->batch.bo, *out_offset + 8 * 4,
                              bo, buffer_offset, I915_GEM_DOMAIN_SAMPLER,
                              rw ? I915_GEM_DOMAIN_SAMPLER : 0);
   }
}

/**
 * Creates a null surface.
 *
 * This is used when the shader doesn't write to any color output.  An FB
 * write to target 0 will still be emitted, because that's how the thread is
 * terminated (and computed depth is returned), so we need to have the
 * hardware discard the target 0 color output..
 */
static void
gen8_emit_null_surface_state(struct brw_context *brw,
                             unsigned width,
                             unsigned height,
                             unsigned samples,
                             uint32_t *out_offset)
{
   uint32_t *surf = gen8_allocate_surface_state(brw, out_offset, -1);

   surf[0] = BRW_SURFACE_NULL << BRW_SURFACE_TYPE_SHIFT |
             BRW_SURFACEFORMAT_B8G8R8A8_UNORM << BRW_SURFACE_FORMAT_SHIFT |
             GEN8_SURFACE_TILING_Y;
   surf[2] = SET_FIELD(width - 1, GEN7_SURFACE_WIDTH) |
             SET_FIELD(height - 1, GEN7_SURFACE_HEIGHT);
}

void
gen8_init_vtable_surface_functions(struct brw_context *brw)
{
   brw->vtbl.update_texture_surface = brw_update_texture_surface;
   brw->vtbl.update_renderbuffer_surface = brw_update_renderbuffer_surface;
   brw->vtbl.emit_null_surface_state = gen8_emit_null_surface_state;
   brw->vtbl.emit_buffer_surface_state = gen8_emit_buffer_surface_state;
}
