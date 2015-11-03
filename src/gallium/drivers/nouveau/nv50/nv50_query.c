/*
 * Copyright 2011 Nouveau Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Christoph Bumiller
 */

#define NV50_PUSH_EXPLICIT_SPACE_CHECKING

#include "nv50/nv50_context.h"
#include "nv50/nv50_query.h"
#include "nv50/nv50_query_hw.h"

static struct pipe_query *
nv50_create_query(struct pipe_context *pipe, unsigned type, unsigned index)
{
   struct nv50_context *nv50 = nv50_context(pipe);
   struct nv50_query *q;

   q = nv50_hw_create_query(nv50, type, index);
   return (struct pipe_query *)q;
}

static void
nv50_destroy_query(struct pipe_context *pipe, struct pipe_query *pq)
{
   struct nv50_query *q = nv50_query(pq);
   q->funcs->destroy_query(nv50_context(pipe), q);
}

static boolean
nv50_begin_query(struct pipe_context *pipe, struct pipe_query *pq)
{
   struct nv50_query *q = nv50_query(pq);
   return q->funcs->begin_query(nv50_context(pipe), q);
}

static void
nv50_end_query(struct pipe_context *pipe, struct pipe_query *pq)
{
   struct nv50_query *q = nv50_query(pq);
   q->funcs->end_query(nv50_context(pipe), q);
}

static boolean
nv50_get_query_result(struct pipe_context *pipe, struct pipe_query *pq,
                      boolean wait, union pipe_query_result *result)
{
   struct nv50_query *q = nv50_query(pq);
   return q->funcs->get_query_result(nv50_context(pipe), q, wait, result);
}

static void
nv50_render_condition(struct pipe_context *pipe,
                      struct pipe_query *pq,
                      boolean condition, uint mode)
{
   struct nv50_context *nv50 = nv50_context(pipe);
   struct nouveau_pushbuf *push = nv50->base.pushbuf;
   struct nv50_query *q = nv50_query(pq);
   struct nv50_hw_query *hq = nv50_hw_query(q);
   uint32_t cond;
   bool wait =
      mode != PIPE_RENDER_COND_NO_WAIT &&
      mode != PIPE_RENDER_COND_BY_REGION_NO_WAIT;

   if (!pq) {
      cond = NV50_3D_COND_MODE_ALWAYS;
   }
   else {
      /* NOTE: comparison of 2 queries only works if both have completed */
      switch (q->type) {
      case PIPE_QUERY_SO_OVERFLOW_PREDICATE:
         cond = condition ? NV50_3D_COND_MODE_EQUAL :
                            NV50_3D_COND_MODE_NOT_EQUAL;
         wait = true;
         break;
      case PIPE_QUERY_OCCLUSION_COUNTER:
      case PIPE_QUERY_OCCLUSION_PREDICATE:
         if (likely(!condition)) {
            if (unlikely(hq->nesting))
               cond = wait ? NV50_3D_COND_MODE_NOT_EQUAL :
                             NV50_3D_COND_MODE_ALWAYS;
            else
               cond = NV50_3D_COND_MODE_RES_NON_ZERO;
         } else {
            cond = wait ? NV50_3D_COND_MODE_EQUAL : NV50_3D_COND_MODE_ALWAYS;
         }
         break;
      default:
         assert(!"render condition query not a predicate");
         cond = NV50_3D_COND_MODE_ALWAYS;
         break;
      }
   }

   nv50->cond_query = pq;
   nv50->cond_cond = condition;
   nv50->cond_condmode = cond;
   nv50->cond_mode = mode;

   if (!pq) {
      PUSH_SPACE(push, 2);
      BEGIN_NV04(push, NV50_3D(COND_MODE), 1);
      PUSH_DATA (push, cond);
      return;
   }

   PUSH_SPACE(push, 9);

   if (wait) {
      BEGIN_NV04(push, SUBC_3D(NV50_GRAPH_SERIALIZE), 1);
      PUSH_DATA (push, 0);
   }

   PUSH_REFN (push, hq->bo, NOUVEAU_BO_GART | NOUVEAU_BO_RD);
   BEGIN_NV04(push, NV50_3D(COND_ADDRESS_HIGH), 3);
   PUSH_DATAh(push, hq->bo->offset + hq->offset);
   PUSH_DATA (push, hq->bo->offset + hq->offset);
   PUSH_DATA (push, cond);

   BEGIN_NV04(push, NV50_2D(COND_ADDRESS_HIGH), 2);
   PUSH_DATAh(push, hq->bo->offset + hq->offset);
   PUSH_DATA (push, hq->bo->offset + hq->offset);
}

void
nv50_init_query_functions(struct nv50_context *nv50)
{
   struct pipe_context *pipe = &nv50->base.pipe;

   pipe->create_query = nv50_create_query;
   pipe->destroy_query = nv50_destroy_query;
   pipe->begin_query = nv50_begin_query;
   pipe->end_query = nv50_end_query;
   pipe->get_query_result = nv50_get_query_result;
   pipe->render_condition = nv50_render_condition;
}
