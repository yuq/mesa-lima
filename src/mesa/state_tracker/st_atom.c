/**************************************************************************
 * 
 * Copyright 2003 VMware, Inc.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/


#include <stdio.h>
#include "main/glheader.h"
#include "main/context.h"

#include "pipe/p_defines.h"
#include "st_context.h"
#include "st_atom.h"
#include "st_program.h"
#include "st_manager.h"


/* The list state update functions. */
static const struct st_tracked_state *atoms[] =
{
#define ST_STATE(FLAG, st_update) &st_update,
#include "st_atom_list.h"
#undef ST_STATE
};


void st_init_atoms( struct st_context *st )
{
   STATIC_ASSERT(ARRAY_SIZE(atoms) <= 64);
}


void st_destroy_atoms( struct st_context *st )
{
   /* no-op */
}


/* Too complex to figure out, just check every time:
 */
static void check_program_state( struct st_context *st )
{
   struct gl_context *ctx = st->ctx;

   if (ctx->VertexProgram._Current != &st->vp->Base)
      st->dirty |= ST_NEW_VERTEX_PROGRAM;

   if (ctx->FragmentProgram._Current != &st->fp->Base)
      st->dirty |= ST_NEW_FRAGMENT_PROGRAM;

   if (ctx->GeometryProgram._Current != &st->gp->Base)
      st->dirty |= ST_NEW_GEOMETRY_PROGRAM;
}

static void check_attrib_edgeflag(struct st_context *st)
{
   const struct gl_client_array **arrays = st->ctx->Array._DrawArrays;
   GLboolean vertdata_edgeflags, edgeflag_culls_prims, edgeflags_enabled;

   if (!arrays)
      return;

   edgeflags_enabled = st->ctx->Polygon.FrontMode != GL_FILL ||
                       st->ctx->Polygon.BackMode != GL_FILL;

   vertdata_edgeflags = edgeflags_enabled &&
                        arrays[VERT_ATTRIB_EDGEFLAG]->StrideB != 0;
   if (vertdata_edgeflags != st->vertdata_edgeflags) {
      st->vertdata_edgeflags = vertdata_edgeflags;
      st->dirty |= ST_NEW_VERTEX_PROGRAM;
   }

   edgeflag_culls_prims = edgeflags_enabled && !vertdata_edgeflags &&
                          !st->ctx->Current.Attrib[VERT_ATTRIB_EDGEFLAG][0];
   if (edgeflag_culls_prims != st->edgeflag_culls_prims) {
      st->edgeflag_culls_prims = edgeflag_culls_prims;
      st->dirty |= ST_NEW_RASTERIZER;
   }
}


/***********************************************************************
 * Update all derived state:
 */

void st_validate_state( struct st_context *st, enum st_pipeline pipeline )
{
   uint64_t dirty, pipeline_mask;
   uint32_t dirty_lo, dirty_hi;

   /* Get Mesa driver state. */
   st->dirty |= st->ctx->NewDriverState & ST_ALL_STATES_MASK;
   st->ctx->NewDriverState = 0;

   /* Get pipeline state. */
   switch (pipeline) {
   case ST_PIPELINE_RENDER:
      check_attrib_edgeflag(st);
      check_program_state(st);
      st_manager_validate_framebuffers(st);

      pipeline_mask = ST_PIPELINE_RENDER_STATE_MASK;
      break;
   case ST_PIPELINE_COMPUTE:
      pipeline_mask = ST_PIPELINE_COMPUTE_STATE_MASK;
      break;
   default:
      unreachable("Invalid pipeline specified");
   }

   dirty = st->dirty & pipeline_mask;
   if (!dirty)
      return;

   dirty_lo = dirty;
   dirty_hi = dirty >> 32;

   /* Update states.
    *
    * Don't use u_bit_scan64, it may be slower on 32-bit.
    */
   while (dirty_lo)
      atoms[u_bit_scan(&dirty_lo)]->update(st);
   while (dirty_hi)
      atoms[32 + u_bit_scan(&dirty_hi)]->update(st);

   /* Clear the render or compute state bits. */
   st->dirty &= ~pipeline_mask;
}
