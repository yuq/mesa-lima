/****************************************************************************
 * Copyright (C) 2015 Intel Corporation.   All Rights Reserved.
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
 ***************************************************************************/

#include "pipe/p_defines.h"
#include "util/u_memory.h"
#include "os/os_time.h"
#include "swr_context.h"
#include "swr_fence.h"
#include "swr_query.h"
#include "swr_screen.h"
#include "swr_state.h"


static struct swr_query *
swr_query(struct pipe_query *p)
{
   return (struct swr_query *)p;
}

static struct pipe_query *
swr_create_query(struct pipe_context *pipe, unsigned type, unsigned index)
{
   struct swr_query *pq;

   assert(type < PIPE_QUERY_TYPES);
   assert(index < MAX_SO_STREAMS);

   pq = CALLOC_STRUCT(swr_query);

   if (pq) {
      pq->type = type;
      pq->index = index;
   }

   return (struct pipe_query *)pq;
}


static void
swr_destroy_query(struct pipe_context *pipe, struct pipe_query *q)
{
   struct swr_query *pq = swr_query(q);

   if (pq->fence) {
      if (!swr_is_fence_pending(pq->fence)) {
         swr_fence_submit(swr_context(pipe), pq->fence);
         swr_fence_finish(pipe->screen, pq->fence, 0);
      }
      swr_fence_reference(pipe->screen, &pq->fence, NULL);
   }

   FREE(pq);
}


// XXX Create a fence callback, rather than stalling SwrWaitForIdle
static void
swr_gather_stats(struct pipe_context *pipe, struct swr_query *pq)
{
   struct swr_context *ctx = swr_context(pipe);

   assert(pq->result);
   union pipe_query_result *result = pq->result;
   boolean enable_stats = pq->enable_stats;
   SWR_STATS swr_stats = {0};

   if (pq->fence) {
      if (!swr_is_fence_pending(pq->fence)) {
         swr_fence_submit(ctx, pq->fence);
         swr_fence_finish(pipe->screen, pq->fence, 0);
      }
      swr_fence_reference(pipe->screen, &pq->fence, NULL);
   }

   /*
    * These queries don't need SWR Stats enabled in the core
    * Set and return.
    */
   switch (pq->type) {
   case PIPE_QUERY_TIMESTAMP:
   case PIPE_QUERY_TIME_ELAPSED:
      result->u64 = swr_get_timestamp(pipe->screen);
      return;
      break;
   case PIPE_QUERY_TIMESTAMP_DISJOINT:
      /* nothing to do here */
      return;
      break;
   case PIPE_QUERY_GPU_FINISHED:
      result->b = TRUE; /* XXX TODO Add an api func to SWR to compare drawId
                           vs LastRetiredId? */
      return;
      break;
   default:
      /* Any query that needs SwrCore stats */
      break;
   }

   /*
    * All other results are collected from SwrCore counters
    */

   /* XXX, Should turn this into a fence callback and skip the stall */
   SwrGetStats(ctx->swrContext, &swr_stats);
   /* SwrGetStats returns immediately, wait for collection */
   SwrWaitForIdle(ctx->swrContext);

   switch (pq->type) {
   case PIPE_QUERY_OCCLUSION_PREDICATE:
   case PIPE_QUERY_OCCLUSION_COUNTER:
      result->u64 = swr_stats.DepthPassCount;
      break;
   case PIPE_QUERY_PRIMITIVES_GENERATED:
      result->u64 = swr_stats.IaPrimitives;
      break;
   case PIPE_QUERY_PRIMITIVES_EMITTED:
      result->u64 = swr_stats.SoNumPrimsWritten[pq->index];
      break;
   case PIPE_QUERY_SO_STATISTICS:
   case PIPE_QUERY_SO_OVERFLOW_PREDICATE: {
      struct pipe_query_data_so_statistics *so_stats = &result->so_statistics;
      so_stats->num_primitives_written =
         swr_stats.SoNumPrimsWritten[pq->index];
      so_stats->primitives_storage_needed =
         swr_stats.SoPrimStorageNeeded[pq->index];
   } break;
   case PIPE_QUERY_PIPELINE_STATISTICS: {
      struct pipe_query_data_pipeline_statistics *p_stats =
         &result->pipeline_statistics;
      p_stats->ia_vertices = swr_stats.IaVertices;
      p_stats->ia_primitives = swr_stats.IaPrimitives;
      p_stats->vs_invocations = swr_stats.VsInvocations;
      p_stats->gs_invocations = swr_stats.GsInvocations;
      p_stats->gs_primitives = swr_stats.GsPrimitives;
      p_stats->c_invocations = swr_stats.CPrimitives;
      p_stats->c_primitives = swr_stats.CPrimitives;
      p_stats->ps_invocations = swr_stats.PsInvocations;
      p_stats->hs_invocations = swr_stats.HsInvocations;
      p_stats->ds_invocations = swr_stats.DsInvocations;
      p_stats->cs_invocations = swr_stats.CsInvocations;
   } break;
   default:
      assert(0 && "Unsupported query");
      break;
   }

   /* Only change stat collection if there are no active queries */
   if (ctx->active_queries == 0)
      SwrEnableStats(ctx->swrContext, enable_stats);
}


static boolean
swr_get_query_result(struct pipe_context *pipe,
                     struct pipe_query *q,
                     boolean wait,
                     union pipe_query_result *result)
{
   struct swr_context *ctx = swr_context(pipe);
   struct swr_query *pq = swr_query(q);

   if (pq->fence) {
      if (!swr_is_fence_pending(pq->fence)) {
         swr_fence_submit(ctx, pq->fence);
         if (!wait)
            return FALSE;
         swr_fence_finish(pipe->screen, pq->fence, 0);
      }
      swr_fence_reference(pipe->screen, &pq->fence, NULL);
   }

   /* XXX: Need to handle counter rollover */

   switch (pq->type) {
   /* Booleans */
   case PIPE_QUERY_OCCLUSION_PREDICATE:
      result->b = pq->end.u64 != pq->start.u64 ? TRUE : FALSE;
      break;
   case PIPE_QUERY_GPU_FINISHED:
      result->b = pq->end.b;
      break;
   /* Counters */
   case PIPE_QUERY_OCCLUSION_COUNTER:
   case PIPE_QUERY_TIMESTAMP:
   case PIPE_QUERY_TIME_ELAPSED:
   case PIPE_QUERY_PRIMITIVES_GENERATED:
   case PIPE_QUERY_PRIMITIVES_EMITTED:
      result->u64 = pq->end.u64 - pq->start.u64;
      break;
   /* Structures */
   case PIPE_QUERY_SO_STATISTICS: {
      struct pipe_query_data_so_statistics *so_stats = &result->so_statistics;
      struct pipe_query_data_so_statistics *start = &pq->start.so_statistics;
      struct pipe_query_data_so_statistics *end = &pq->end.so_statistics;
      so_stats->num_primitives_written =
         end->num_primitives_written - start->num_primitives_written;
      so_stats->primitives_storage_needed =
         end->primitives_storage_needed - start->primitives_storage_needed;
   } break;
   case PIPE_QUERY_TIMESTAMP_DISJOINT: {
      /* os_get_time_nano returns nanoseconds */
      result->timestamp_disjoint.frequency = UINT64_C(1000000000);
      result->timestamp_disjoint.disjoint = FALSE;
   } break;
   case PIPE_QUERY_PIPELINE_STATISTICS: {
      struct pipe_query_data_pipeline_statistics *p_stats =
         &result->pipeline_statistics;
      struct pipe_query_data_pipeline_statistics *start =
         &pq->start.pipeline_statistics;
      struct pipe_query_data_pipeline_statistics *end =
         &pq->end.pipeline_statistics;
      p_stats->ia_vertices = end->ia_vertices - start->ia_vertices;
      p_stats->ia_primitives = end->ia_primitives - start->ia_primitives;
      p_stats->vs_invocations = end->vs_invocations - start->vs_invocations;
      p_stats->gs_invocations = end->gs_invocations - start->gs_invocations;
      p_stats->gs_primitives = end->gs_primitives - start->gs_primitives;
      p_stats->c_invocations = end->c_invocations - start->c_invocations;
      p_stats->c_primitives = end->c_primitives - start->c_primitives;
      p_stats->ps_invocations = end->ps_invocations - start->ps_invocations;
      p_stats->hs_invocations = end->hs_invocations - start->hs_invocations;
      p_stats->ds_invocations = end->ds_invocations - start->ds_invocations;
      p_stats->cs_invocations = end->cs_invocations - start->cs_invocations;
   } break;
   case PIPE_QUERY_SO_OVERFLOW_PREDICATE: {
      struct pipe_query_data_so_statistics *start = &pq->start.so_statistics;
      struct pipe_query_data_so_statistics *end = &pq->end.so_statistics;
      uint64_t num_primitives_written =
         end->num_primitives_written - start->num_primitives_written;
      uint64_t primitives_storage_needed =
         end->primitives_storage_needed - start->primitives_storage_needed;
      result->b = num_primitives_written > primitives_storage_needed;
   } break;
   default:
      assert(0 && "Unsupported query");
      break;
   }

   return TRUE;
}

static boolean
swr_begin_query(struct pipe_context *pipe, struct pipe_query *q)
{
   struct swr_context *ctx = swr_context(pipe);
   struct swr_query *pq = swr_query(q);

   /* Initialize Results */
   memset(&pq->start, 0, sizeof(pq->start));
   memset(&pq->end, 0, sizeof(pq->end));

   /* Gather start stats and enable SwrCore counters */
   pq->result = &pq->start;
   pq->enable_stats = TRUE;
   swr_gather_stats(pipe, pq);
   ctx->active_queries++;

   /* override start timestamp to 0 for TIMESTAMP query */
   if (pq->type == PIPE_QUERY_TIMESTAMP)
      pq->start.u64 = 0;

   return true;
}

static bool
swr_end_query(struct pipe_context *pipe, struct pipe_query *q)
{
   struct swr_context *ctx = swr_context(pipe);
   struct swr_query *pq = swr_query(q);

   assert(ctx->active_queries
          && "swr_end_query, there are no active queries!");
   ctx->active_queries--;

   /* Gather end stats and disable SwrCore counters */
   pq->result = &pq->end;
   pq->enable_stats = FALSE;
   swr_gather_stats(pipe, pq);
   return true;
}


boolean
swr_check_render_cond(struct pipe_context *pipe)
{
   struct swr_context *ctx = swr_context(pipe);
   boolean b, wait;
   uint64_t result;

   if (!ctx->render_cond_query)
      return TRUE; /* no query predicate, draw normally */

   wait = (ctx->render_cond_mode == PIPE_RENDER_COND_WAIT
           || ctx->render_cond_mode == PIPE_RENDER_COND_BY_REGION_WAIT);

   b = pipe->get_query_result(
      pipe, ctx->render_cond_query, wait, (union pipe_query_result *)&result);
   if (b)
      return ((!result) == ctx->render_cond_cond);
   else
      return TRUE;
}


static void
swr_set_active_query_state(struct pipe_context *pipe, boolean enable)
{
}

void
swr_query_init(struct pipe_context *pipe)
{
   struct swr_context *ctx = swr_context(pipe);

   pipe->create_query = swr_create_query;
   pipe->destroy_query = swr_destroy_query;
   pipe->begin_query = swr_begin_query;
   pipe->end_query = swr_end_query;
   pipe->get_query_result = swr_get_query_result;
   pipe->set_active_query_state = swr_set_active_query_state;

   ctx->active_queries = 0;
}
