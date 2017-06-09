/*
 Copyright (C) Intel Corp.  2006.  All Rights Reserved.
 Intel funded Tungsten Graphics to
 develop this 3D driver.

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sublicense, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:

 The above copyright notice and this permission notice (including the
 next paragraph) shall be included in all copies or substantial
 portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 **********************************************************************/
 /*
  * Authors:
  *   Keith Whitwell <keithw@vmware.com>
  */



#include "main/mtypes.h"
#include "main/macros.h"
#include "main/fbobject.h"
#include "main/viewport.h"
#include "intel_batchbuffer.h"
#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "brw_util.h"

static void upload_sf_unit( struct brw_context *brw )
{
   struct gl_context *ctx = &brw->ctx;
   struct brw_sf_unit_state *sf;
   int chipset_max_threads;
   bool render_to_fbo = _mesa_is_user_fbo(ctx->DrawBuffer);

   sf = brw_state_batch(brw, sizeof(*sf), 64, &brw->sf.state_offset);

   memset(sf, 0, sizeof(*sf));

   /* BRW_NEW_PROGRAM_CACHE | BRW_NEW_SF_PROG_DATA */
   sf->thread0.grf_reg_count = ALIGN(brw->sf.prog_data->total_grf, 16) / 16 - 1;
   sf->thread0.kernel_start_pointer =
      brw_program_reloc(brw,
			brw->sf.state_offset +
			offsetof(struct brw_sf_unit_state, thread0),
			brw->sf.prog_offset +
			(sf->thread0.grf_reg_count << 1)) >> 6;

   sf->thread1.floating_point_mode = BRW_FLOATING_POINT_NON_IEEE_754;

   sf->thread3.dispatch_grf_start_reg = 3;
   sf->thread3.urb_entry_read_offset = BRW_SF_URB_ENTRY_READ_OFFSET;

   /* BRW_NEW_SF_PROG_DATA */
   sf->thread3.urb_entry_read_length = brw->sf.prog_data->urb_read_length;

   /* BRW_NEW_URB_FENCE */
   sf->thread4.nr_urb_entries = brw->urb.nr_sf_entries;
   sf->thread4.urb_entry_allocation_size = brw->urb.sfsize - 1;

   /* Each SF thread produces 1 PUE, and there can be up to 24 (Pre-Ironlake) or
    * 48 (Ironlake) threads.
    */
   if (brw->gen == 5)
      chipset_max_threads = 48;
   else
      chipset_max_threads = 24;

   /* BRW_NEW_URB_FENCE */
   sf->thread4.max_threads = MIN2(chipset_max_threads,
				  brw->urb.nr_sf_entries) - 1;

   /* BRW_NEW_SF_VP */
   sf->sf5.sf_viewport_state_offset = (brw->batch.bo->offset64 +
				       brw->sf.vp_offset) >> 5; /* reloc */

   sf->sf5.viewport_transform = 1;

   sf->sf6.scissor = 1;

   /* _NEW_POLYGON */
   if (brw->polygon_front_bit)
      sf->sf5.front_winding = BRW_FRONTWINDING_CW;
   else
      sf->sf5.front_winding = BRW_FRONTWINDING_CCW;

   /* _NEW_BUFFERS
    * The viewport is inverted for rendering to a FBO, and that inverts
    * polygon front/back orientation.
    */
   sf->sf5.front_winding ^= render_to_fbo;

   /* _NEW_POLYGON */
   switch (ctx->Polygon.CullFlag ? ctx->Polygon.CullFaceMode : GL_NONE) {
   case GL_FRONT:
      sf->sf6.cull_mode = BRW_CULLMODE_FRONT;
      break;
   case GL_BACK:
      sf->sf6.cull_mode = BRW_CULLMODE_BACK;
      break;
   case GL_FRONT_AND_BACK:
      sf->sf6.cull_mode = BRW_CULLMODE_BOTH;
      break;
   case GL_NONE:
      sf->sf6.cull_mode = BRW_CULLMODE_NONE;
      break;
   default:
      unreachable("not reached");
   }

   /* _NEW_LINE */
   sf->sf6.line_width = U_FIXED(brw_get_line_width(brw), 1);

   if (ctx->Line.SmoothFlag) {
      sf->sf6.aa_enable = 1;
      sf->sf6.line_endcap_aa_region_width = 1;
   }

   sf->sf6.point_rast_rule = BRW_RASTRULE_UPPER_RIGHT;

   /* _NEW_POINT */
   sf->sf7.sprite_point = ctx->Point.PointSprite;

   float point_sz;
   point_sz = CLAMP(ctx->Point.Size, ctx->Point.MinSize, ctx->Point.MaxSize);
   point_sz = CLAMP(point_sz, 0.125f, 255.875f);
   sf->sf7.point_size = U_FIXED(point_sz, 3);

   /* _NEW_PROGRAM | _NEW_POINT, BRW_NEW_VUE_MAP_GEOM_OUT */
   sf->sf7.use_point_size_state = use_state_point_size(brw);
   sf->sf7.aa_line_distance_mode = brw->is_g4x || brw->gen == 5;

   /* might be BRW_NEW_PRIMITIVE if we have to adjust pv for polygons:
    * _NEW_LIGHT
    */
   if (ctx->Light.ProvokingVertex != GL_FIRST_VERTEX_CONVENTION) {
      sf->sf7.trifan_pv = 2;
      sf->sf7.linestrip_pv = 1;
      sf->sf7.tristrip_pv = 2;
   } else {
      sf->sf7.trifan_pv = 1;
      sf->sf7.linestrip_pv = 0;
      sf->sf7.tristrip_pv = 0;
   }
   sf->sf7.line_last_pixel_enable = 0;

   /* Set bias for OpenGL rasterization rules:
    */
   sf->sf6.dest_org_vbias = 0x8;
   sf->sf6.dest_org_hbias = 0x8;

   /* STATE_PREFETCH command description describes this state as being
    * something loaded through the GPE (L2 ISC), so it's INSTRUCTION domain.
    */

   /* Emit SF viewport relocation */
   brw_emit_reloc(&brw->batch,
                  brw->sf.state_offset +
		  offsetof(struct brw_sf_unit_state, sf5),
                  brw->batch.bo,
                  brw->sf.vp_offset | sf->sf5.front_winding |
                  (sf->sf5.viewport_transform << 1),
                  I915_GEM_DOMAIN_INSTRUCTION, 0);

   brw->ctx.NewDriverState |= BRW_NEW_GEN4_UNIT_STATE;
}

const struct brw_tracked_state brw_sf_unit = {
   .dirty = {
      .mesa  = _NEW_BUFFERS |
               _NEW_LIGHT |
               _NEW_LINE |
               _NEW_POINT |
               _NEW_POLYGON |
               _NEW_PROGRAM,
      .brw   = BRW_NEW_BATCH |
               BRW_NEW_BLORP |
               BRW_NEW_PROGRAM_CACHE |
               BRW_NEW_SF_PROG_DATA |
               BRW_NEW_SF_VP |
               BRW_NEW_VUE_MAP_GEOM_OUT |
               BRW_NEW_URB_FENCE,
   },
   .emit = upload_sf_unit,
};
