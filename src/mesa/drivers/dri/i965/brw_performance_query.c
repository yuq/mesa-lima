/*
 * Copyright © 2013 Intel Corporation
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

/**
 * \file brw_performance_query.c
 *
 * Implementation of the GL_INTEL_performance_query extension.
 *
 * Currently this driver only exposes the 64bit Pipeline Statistics
 * Registers for Gen6+, with support for Observability Counters to be
 * added later for Gen7.5+
 */

#include <limits.h>

#include <asm/unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "main/hash.h"
#include "main/macros.h"
#include "main/mtypes.h"
#include "main/performance_query.h"

#include "util/bitset.h"
#include "util/ralloc.h"

#include "brw_context.h"
#include "brw_defines.h"
#include "brw_performance_query.h"
#include "intel_batchbuffer.h"

#define FILE_DEBUG_FLAG DEBUG_PERFMON

/**
 * i965 representation of a performance query object.
 *
 * NB: We want to keep this structure relatively lean considering that
 * applications may expect to allocate enough objects to be able to
 * query around all draw calls in a frame.
 */
struct brw_perf_query_object
{
   struct gl_perf_query_object base;

   const struct brw_perf_query_info *query;

   struct {
      /**
       * BO containing starting and ending snapshots for the
       * statistics counters.
       */
      drm_intel_bo *bo;
   } pipeline_stats;
};

/** Downcasting convenience macro. */
static inline struct brw_perf_query_object *
brw_perf_query(struct gl_perf_query_object *o)
{
   return (struct brw_perf_query_object *) o;
}

#define STATS_BO_SIZE               4096
#define STATS_BO_END_OFFSET_BYTES   (STATS_BO_SIZE / 2)
#define MAX_STAT_COUNTERS           (STATS_BO_END_OFFSET_BYTES / 8)

/******************************************************************************/

static void
dump_perf_query_callback(GLuint id, void *query_void, void *brw_void)
{
   struct gl_perf_query_object *o = query_void;
   struct brw_perf_query_object *obj = query_void;

   switch (obj->query->kind) {
   case PIPELINE_STATS:
      DBG("%4d: %-6s %-8s BO: %-4s\n",
          id,
          o->Used ? "Dirty," : "New,",
          o->Active ? "Active," : (o->Ready ? "Ready," : "Pending,"),
          obj->pipeline_stats.bo ? "yes" : "no");
      break;
   }
}

static void
dump_perf_queries(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   DBG("Queries: (Open queries = %d)\n",
       brw->perfquery.n_active_pipeline_stats_queries);
   _mesa_HashWalk(ctx->PerfQuery.Objects, dump_perf_query_callback, brw);
}

/******************************************************************************/

/**
 * Driver hook for glGetPerfQueryInfoINTEL().
 */
static void
brw_get_perf_query_info(struct gl_context *ctx,
                        unsigned query_index,
                        const char **name,
                        GLuint *data_size,
                        GLuint *n_counters,
                        GLuint *n_active)
{
   struct brw_context *brw = brw_context(ctx);
   const struct brw_perf_query_info *query =
      &brw->perfquery.queries[query_index];

   *name = query->name;
   *data_size = query->data_size;
   *n_counters = query->n_counters;

   switch (query->kind) {
   case PIPELINE_STATS:
      *n_active = brw->perfquery.n_active_pipeline_stats_queries;
      break;
   }
}

/**
 * Driver hook for glGetPerfCounterInfoINTEL().
 */
static void
brw_get_perf_counter_info(struct gl_context *ctx,
                          unsigned query_index,
                          unsigned counter_index,
                          const char **name,
                          const char **desc,
                          GLuint *offset,
                          GLuint *data_size,
                          GLuint *type_enum,
                          GLuint *data_type_enum,
                          GLuint64 *raw_max)
{
   struct brw_context *brw = brw_context(ctx);
   const struct brw_perf_query_info *query =
      &brw->perfquery.queries[query_index];
   const struct brw_perf_query_counter *counter =
      &query->counters[counter_index];

   *name = counter->name;
   *desc = counter->desc;
   *offset = counter->offset;
   *data_size = counter->size;
   *type_enum = counter->type;
   *data_type_enum = counter->data_type;
   *raw_max = counter->raw_max;
}

/******************************************************************************/

/**
 * Emit MI_STORE_REGISTER_MEM commands to capture all of the
 * pipeline statistics for the performance query object.
 */
static void
snapshot_statistics_registers(struct brw_context *brw,
                              struct brw_perf_query_object *obj,
                              uint32_t offset_in_bytes)
{
   const struct brw_perf_query_info *query = obj->query;
   const int n_counters = query->n_counters;

   for (int i = 0; i < n_counters; i++) {
      const struct brw_perf_query_counter *counter = &query->counters[i];

      assert(counter->data_type == GL_PERFQUERY_COUNTER_DATA_UINT64_INTEL);

      brw_store_register_mem64(brw, obj->pipeline_stats.bo,
                               counter->pipeline_stat.reg,
                               offset_in_bytes + i * sizeof(uint64_t));
   }
}

/**
 * Driver hook for glBeginPerfQueryINTEL().
 */
static GLboolean
brw_begin_perf_query(struct gl_context *ctx,
                     struct gl_perf_query_object *o)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_perf_query_object *obj = brw_perf_query(o);
   const struct brw_perf_query_info *query = obj->query;

   /* We can assume the frontend hides mistaken attempts to Begin a
    * query object multiple times before its End. Similarly if an
    * application reuses a query object before results have arrived
    * the frontend will wait for prior results so we don't need
    * to support abandoning in-flight results.
    */
   assert(!o->Active);
   assert(!o->Used || o->Ready); /* no in-flight query to worry about */

   DBG("Begin(%d)\n", o->Id);

   /* XXX: We have to consider that the command parser unit that parses batch
    * buffer commands and is used to capture begin/end counter snapshots isn't
    * implicitly synchronized with what's currently running across other GPU
    * units (such as the EUs running shaders) that the performance counters are
    * associated with.
    *
    * The intention of performance queries is to measure the work associated
    * with commands between the begin/end delimiters and so for that to be the
    * case we need to explicitly synchronize the parsing of commands to capture
    * Begin/End counter snapshots with what's running across other parts of the
    * GPU.
    *
    * When the command parser reaches a Begin marker it effectively needs to
    * drain everything currently running on the GPU until the hardware is idle
    * before capturing the first snapshot of counters - otherwise the results
    * would also be measuring the effects of earlier commands.
    *
    * When the command parser reaches an End marker it needs to stall until
    * everything currently running on the GPU has finished before capturing the
    * end snapshot - otherwise the results won't be a complete representation
    * of the work.
    *
    * Theoretically there could be opportunities to minimize how much of the
    * GPU pipeline is drained, or that we stall for, when we know what specific
    * units the performance counters being queried relate to but we don't
    * currently attempt to be clever here.
    *
    * Note: with our current simple approach here then for back-to-back queries
    * we will redundantly emit duplicate commands to synchronize the command
    * streamer with the rest of the GPU pipeline, but we assume that in HW the
    * second synchronization is effectively a NOOP.
    *
    * N.B. The final results are based on deltas of counters between (inside)
    * Begin/End markers so even though the total wall clock time of the
    * workload is stretched by larger pipeline bubbles the bubbles themselves
    * are generally invisible to the query results. Whether that's a good or a
    * bad thing depends on the use case. For a lower real-time impact while
    * capturing metrics then periodic sampling may be a better choice than
    * INTEL_performance_query.
    *
    *
    * This is our Begin synchronization point to drain current work on the
    * GPU before we capture our first counter snapshot...
    */
   brw_emit_mi_flush(brw);

   switch (query->kind) {
   case PIPELINE_STATS:
      if (obj->pipeline_stats.bo) {
         drm_intel_bo_unreference(obj->pipeline_stats.bo);
         obj->pipeline_stats.bo = NULL;
      }

      obj->pipeline_stats.bo =
         drm_intel_bo_alloc(brw->bufmgr, "perf. query pipeline stats bo",
                            STATS_BO_SIZE, 64);

      /* Take starting snapshots. */
      snapshot_statistics_registers(brw, obj, 0);

      ++brw->perfquery.n_active_pipeline_stats_queries;
      break;
   }

   if (INTEL_DEBUG & DEBUG_PERFMON)
      dump_perf_queries(brw);

   return true;
}

/**
 * Driver hook for glEndPerfQueryINTEL().
 */
static void
brw_end_perf_query(struct gl_context *ctx,
                     struct gl_perf_query_object *o)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_perf_query_object *obj = brw_perf_query(o);

   DBG("End(%d)\n", o->Id);

   /* Ensure that the work associated with the queried commands will have
    * finished before taking our query end counter readings.
    *
    * For more details see comment in brw_begin_perf_query for
    * corresponding flush.
    */
   brw_emit_mi_flush(brw);

   switch (obj->query->kind) {
   case PIPELINE_STATS:
      snapshot_statistics_registers(brw, obj,
                                    STATS_BO_END_OFFSET_BYTES);
      --brw->perfquery.n_active_pipeline_stats_queries;
      break;
   }
}

static void
brw_wait_perf_query(struct gl_context *ctx, struct gl_perf_query_object *o)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_perf_query_object *obj = brw_perf_query(o);
   drm_intel_bo *bo = NULL;

   assert(!o->Ready);

   switch (obj->query->kind) {
   case PIPELINE_STATS:
      bo = obj->pipeline_stats.bo;
      break;
   }

   if (bo == NULL)
      return;

   /* If the current batch references our results bo then we need to
    * flush first... */
   if (drm_intel_bo_references(brw->batch.bo, bo))
      intel_batchbuffer_flush(brw);

   if (unlikely(brw->perf_debug)) {
      if (drm_intel_bo_busy(bo))
         perf_debug("Stalling GPU waiting for a performance query object.\n");
   }

   drm_intel_bo_wait_rendering(bo);
}

static GLboolean
brw_is_perf_query_ready(struct gl_context *ctx,
                        struct gl_perf_query_object *o)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_perf_query_object *obj = brw_perf_query(o);

   if (o->Ready)
      return true;

   switch (obj->query->kind) {
   case PIPELINE_STATS:
      return (obj->pipeline_stats.bo &&
              !drm_intel_bo_references(brw->batch.bo, obj->pipeline_stats.bo) &&
              !drm_intel_bo_busy(obj->pipeline_stats.bo));
   }

   unreachable("missing ready check for unknown query kind");
   return false;
}

static int
get_pipeline_stats_data(struct brw_context *brw,
                        struct brw_perf_query_object *obj,
                        size_t data_size,
                        uint8_t *data)

{
   const struct brw_perf_query_info *query = obj->query;
   int n_counters = obj->query->n_counters;
   uint8_t *p = data;

   drm_intel_bo_map(obj->pipeline_stats.bo, false);
   uint64_t *start = obj->pipeline_stats.bo->virtual;
   uint64_t *end = start + (STATS_BO_END_OFFSET_BYTES / sizeof(uint64_t));

   for (int i = 0; i < n_counters; i++) {
      const struct brw_perf_query_counter *counter = &query->counters[i];
      uint64_t value = end[i] - start[i];

      if (counter->pipeline_stat.numerator !=
          counter->pipeline_stat.denominator) {
         value *= counter->pipeline_stat.numerator;
         value /= counter->pipeline_stat.denominator;
      }

      *((uint64_t *)p) = value;
      p += 8;
   }

   drm_intel_bo_unmap(obj->pipeline_stats.bo);

   return p - data;
}

/**
 * Driver hook for glGetPerfQueryDataINTEL().
 */
static void
brw_get_perf_query_data(struct gl_context *ctx,
                        struct gl_perf_query_object *o,
                        GLsizei data_size,
                        GLuint *data,
                        GLuint *bytes_written)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_perf_query_object *obj = brw_perf_query(o);
   int written = 0;

   assert(brw_is_perf_query_ready(ctx, o));

   DBG("GetData(%d)\n", o->Id);

   if (INTEL_DEBUG & DEBUG_PERFMON)
      dump_perf_queries(brw);

   /* We expect that the frontend only calls this hook when it knows
    * that results are available.
    */
   assert(o->Ready);

   switch (obj->query->kind) {
   case PIPELINE_STATS:
      written = get_pipeline_stats_data(brw, obj, data_size, (uint8_t *)data);
      break;
   }

   if (bytes_written)
      *bytes_written = written;
}

static struct gl_perf_query_object *
brw_new_perf_query_object(struct gl_context *ctx, unsigned query_index)
{
   struct brw_context *brw = brw_context(ctx);
   const struct brw_perf_query_info *query =
      &brw->perfquery.queries[query_index];
   struct brw_perf_query_object *obj =
      calloc(1, sizeof(struct brw_perf_query_object));

   if (!obj)
      return NULL;

   obj->query = query;

   return &obj->base;
}

/**
 * Driver hook for glDeletePerfQueryINTEL().
 */
static void
brw_delete_perf_query(struct gl_context *ctx,
                      struct gl_perf_query_object *o)
{
   struct brw_perf_query_object *obj = brw_perf_query(o);

   /* We can assume that the frontend waits for a query to complete
    * before ever calling into here, so we don't have to worry about
    * deleting an in-flight query object.
    */
   assert(!o->Active);
   assert(!o->Used || o->Ready);

   DBG("Delete(%d)\n", o->Id);

   switch (obj->query->kind) {
   case PIPELINE_STATS:
      if (obj->pipeline_stats.bo) {
         drm_intel_bo_unreference(obj->pipeline_stats.bo);
         obj->pipeline_stats.bo = NULL;
      }
      break;
   }

   free(obj);
}

/******************************************************************************/

static struct brw_perf_query_info *
append_query_info(struct brw_context *brw)
{
   brw->perfquery.queries =
      reralloc(brw, brw->perfquery.queries,
               struct brw_perf_query_info, ++brw->perfquery.n_queries);

   return &brw->perfquery.queries[brw->perfquery.n_queries - 1];
}

static void
add_stat_reg(struct brw_perf_query_info *query,
             uint32_t reg,
             uint32_t numerator,
             uint32_t denominator,
             const char *name,
             const char *description)
{
   struct brw_perf_query_counter *counter;

   assert(query->n_counters < MAX_STAT_COUNTERS);

   counter = &query->counters[query->n_counters];
   counter->name = name;
   counter->desc = description;
   counter->type = GL_PERFQUERY_COUNTER_RAW_INTEL;
   counter->data_type = GL_PERFQUERY_COUNTER_DATA_UINT64_INTEL;
   counter->size = sizeof(uint64_t);
   counter->offset = sizeof(uint64_t) * query->n_counters;
   counter->pipeline_stat.reg = reg;
   counter->pipeline_stat.numerator = numerator;
   counter->pipeline_stat.denominator = denominator;

   query->n_counters++;
}

static void
add_basic_stat_reg(struct brw_perf_query_info *query,
                   uint32_t reg, const char *name)
{
   add_stat_reg(query, reg, 1, 1, name, name);
}

static void
init_pipeline_statistic_query_registers(struct brw_context *brw)
{
   struct brw_perf_query_info *query = append_query_info(brw);

   query->kind = PIPELINE_STATS;
   query->name = "Pipeline Statistics Registers";
   query->n_counters = 0;
   query->counters =
      rzalloc_array(brw, struct brw_perf_query_counter, MAX_STAT_COUNTERS);

   add_basic_stat_reg(query, IA_VERTICES_COUNT,
                      "N vertices submitted");
   add_basic_stat_reg(query, IA_PRIMITIVES_COUNT,
                      "N primitives submitted");
   add_basic_stat_reg(query, VS_INVOCATION_COUNT,
                      "N vertex shader invocations");

   if (brw->gen == 6) {
      add_stat_reg(query, GEN6_SO_PRIM_STORAGE_NEEDED, 1, 1,
                   "SO_PRIM_STORAGE_NEEDED",
                   "N geometry shader stream-out primitives (total)");
      add_stat_reg(query, GEN6_SO_NUM_PRIMS_WRITTEN, 1, 1,
                   "SO_NUM_PRIMS_WRITTEN",
                   "N geometry shader stream-out primitives (written)");
   } else {
      add_stat_reg(query, GEN7_SO_PRIM_STORAGE_NEEDED(0), 1, 1,
                   "SO_PRIM_STORAGE_NEEDED (Stream 0)",
                   "N stream-out (stream 0) primitives (total)");
      add_stat_reg(query, GEN7_SO_PRIM_STORAGE_NEEDED(1), 1, 1,
                   "SO_PRIM_STORAGE_NEEDED (Stream 1)",
                   "N stream-out (stream 1) primitives (total)");
      add_stat_reg(query, GEN7_SO_PRIM_STORAGE_NEEDED(2), 1, 1,
                   "SO_PRIM_STORAGE_NEEDED (Stream 2)",
                   "N stream-out (stream 2) primitives (total)");
      add_stat_reg(query, GEN7_SO_PRIM_STORAGE_NEEDED(3), 1, 1,
                   "SO_PRIM_STORAGE_NEEDED (Stream 3)",
                   "N stream-out (stream 3) primitives (total)");
      add_stat_reg(query, GEN7_SO_NUM_PRIMS_WRITTEN(0), 1, 1,
                   "SO_NUM_PRIMS_WRITTEN (Stream 0)",
                   "N stream-out (stream 0) primitives (written)");
      add_stat_reg(query, GEN7_SO_NUM_PRIMS_WRITTEN(1), 1, 1,
                   "SO_NUM_PRIMS_WRITTEN (Stream 1)",
                   "N stream-out (stream 1) primitives (written)");
      add_stat_reg(query, GEN7_SO_NUM_PRIMS_WRITTEN(2), 1, 1,
                   "SO_NUM_PRIMS_WRITTEN (Stream 2)",
                   "N stream-out (stream 2) primitives (written)");
      add_stat_reg(query, GEN7_SO_NUM_PRIMS_WRITTEN(3), 1, 1,
                   "SO_NUM_PRIMS_WRITTEN (Stream 3)",
                   "N stream-out (stream 3) primitives (written)");
   }

   add_basic_stat_reg(query, HS_INVOCATION_COUNT,
                      "N TCS shader invocations");
   add_basic_stat_reg(query, DS_INVOCATION_COUNT,
                      "N TES shader invocations");

   add_basic_stat_reg(query, GS_INVOCATION_COUNT,
                      "N geometry shader invocations");
   add_basic_stat_reg(query, GS_PRIMITIVES_COUNT,
                      "N geometry shader primitives emitted");

   add_basic_stat_reg(query, CL_INVOCATION_COUNT,
                      "N primitives entering clipping");
   add_basic_stat_reg(query, CL_PRIMITIVES_COUNT,
                      "N primitives leaving clipping");

   if (brw->is_haswell || brw->gen == 8)
      add_stat_reg(query, PS_INVOCATION_COUNT, 1, 4,
                   "N fragment shader invocations",
                   "N fragment shader invocations");
   else
      add_basic_stat_reg(query, PS_INVOCATION_COUNT,
                         "N fragment shader invocations");

   add_basic_stat_reg(query, PS_DEPTH_COUNT, "N z-pass fragments");

   if (brw->gen >= 7)
      add_basic_stat_reg(query, CS_INVOCATION_COUNT,
                         "N compute shader invocations");

   query->data_size = sizeof(uint64_t) * query->n_counters;
}

static unsigned
brw_init_perf_query_info(struct gl_context *ctx)
{
   struct brw_context *brw = brw_context(ctx);

   if (brw->perfquery.n_queries)
      return brw->perfquery.n_queries;

   init_pipeline_statistic_query_registers(brw);

   return brw->perfquery.n_queries;
}

void
brw_init_performance_queries(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;

   ctx->Driver.InitPerfQueryInfo = brw_init_perf_query_info;
   ctx->Driver.GetPerfQueryInfo = brw_get_perf_query_info;
   ctx->Driver.GetPerfCounterInfo = brw_get_perf_counter_info;
   ctx->Driver.NewPerfQueryObject = brw_new_perf_query_object;
   ctx->Driver.DeletePerfQuery = brw_delete_perf_query;
   ctx->Driver.BeginPerfQuery = brw_begin_perf_query;
   ctx->Driver.EndPerfQuery = brw_end_perf_query;
   ctx->Driver.WaitPerfQuery = brw_wait_perf_query;
   ctx->Driver.IsPerfQueryReady = brw_is_perf_query_ready;
   ctx->Driver.GetPerfQueryData = brw_get_perf_query_data;
}
