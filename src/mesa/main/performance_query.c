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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file performance_query.c
 * Core Mesa support for the INTEL_performance_query extension.
 *
 * In order to implement this extension, start by defining two enums:
 * one for Groups, and one for Counters.  These will be used as indexes into
 * arrays, so they should start at 0 and increment from there.
 *
 * Counter IDs need to be globally unique.  That is, you can't have counter 7
 * in group A and counter 7 in group B.  A global enum of all available
 * counters is a convenient way to guarantee this.
 */

#include <stdbool.h>
#include "glheader.h"
#include "context.h"
#include "enums.h"
#include "hash.h"
#include "macros.h"
#include "mtypes.h"
#include "performance_query.h"
#include "util/bitset.h"
#include "util/ralloc.h"

void
_mesa_init_performance_queries(struct gl_context *ctx)
{
   ctx->PerfQuery.Objects = _mesa_NewHashTable();
   ctx->PerfQuery.NumGroups = 0;
   ctx->PerfQuery.Groups = NULL;
}

static inline void
init_groups(struct gl_context *ctx)
{
   if (unlikely(!ctx->PerfMonitor.Groups))
      ctx->Driver.InitPerfMonitorGroups(ctx);
}

static struct gl_perf_monitor_object *
new_performance_query(struct gl_context *ctx, GLuint index)
{
   unsigned i;
   struct gl_perf_monitor_object *m = ctx->Driver.NewPerfMonitor(ctx);

   if (m == NULL)
      return NULL;

   m->Name = index;

   m->Active = false;

   m->ActiveGroups =
      rzalloc_array(NULL, unsigned, ctx->PerfQuery.NumGroups);

   m->ActiveCounters =
      ralloc_array(NULL, BITSET_WORD *, ctx->PerfQuery.NumGroups);

   if (m->ActiveGroups == NULL || m->ActiveCounters == NULL)
      goto fail;

   for (i = 0; i < ctx->PerfQuery.NumGroups; i++) {
      const struct gl_perf_monitor_group *g = &ctx->PerfQuery.Groups[i];

      m->ActiveCounters[i] = rzalloc_array(m->ActiveCounters, BITSET_WORD,
                                           BITSET_WORDS(g->NumCounters));
      if (m->ActiveCounters[i] == NULL)
         goto fail;
   }

   return m;

fail:
   ralloc_free(m->ActiveGroups);
   ralloc_free(m->ActiveCounters);
   ctx->Driver.DeletePerfMonitor(ctx, m);
   return NULL;
}

static void
free_performance_query(GLuint key, void *data, void *user)
{
   struct gl_perf_monitor_object *m = data;
   struct gl_context *ctx = user;

   ralloc_free(m->ActiveGroups);
   ralloc_free(m->ActiveCounters);
   ctx->Driver.DeletePerfMonitor(ctx, m);
}

void
_mesa_free_performance_queries(struct gl_context *ctx)
{
   _mesa_HashDeleteAll(ctx->PerfQuery.Objects,
                       free_performance_query, ctx);
   _mesa_DeleteHashTable(ctx->PerfQuery.Objects);
}

static inline struct gl_perf_monitor_object *
lookup_query(struct gl_context *ctx, GLuint id)
{
   return (struct gl_perf_monitor_object *)
      _mesa_HashLookup(ctx->PerfQuery.Objects, id);
}

static inline const struct gl_perf_monitor_group *
get_group(const struct gl_context *ctx, GLuint id)
{
   if (id >= ctx->PerfQuery.NumGroups)
      return NULL;

   return &ctx->PerfQuery.Groups[id];
}

static inline const struct gl_perf_monitor_counter *
get_counter(const struct gl_perf_monitor_group *group_obj, GLuint id)
{
   if (id >= group_obj->NumCounters)
      return NULL;

   return &group_obj->Counters[id];
}

/* For INTEL_performance_query, query id 0 is reserved to be invalid. We use
 * index to Groups array + 1 as the query id. Same applies to counter id.
 */
static inline GLuint
queryid_to_index(GLuint queryid)
{
   return queryid - 1;
}

static inline GLuint
index_to_queryid(GLuint index)
{
   return index + 1;
}

static inline bool
queryid_valid(const struct gl_context *ctx, GLuint queryid)
{
   return get_group(ctx, queryid_to_index(queryid)) != NULL;
}

static inline GLuint
counterid_to_index(GLuint counterid)
{
   return counterid - 1;
}

/*****************************************************************************/

/**
 * Returns how many bytes a counter's value takes up.
 */
unsigned
_mesa_perf_query_counter_size(const struct gl_perf_monitor_counter *c)
{
   switch (c->Type) {
   case GL_FLOAT:
   case GL_PERCENTAGE_AMD:
      return sizeof(GLfloat);
   case GL_UNSIGNED_INT:
      return sizeof(GLuint);
   case GL_UNSIGNED_INT64_AMD:
      return sizeof(uint64_t);
   default:
      assert(!"Should not get here: invalid counter type");
      return 0;
   }
}

extern void GLAPIENTRY
_mesa_GetFirstPerfQueryIdINTEL(GLuint *queryId)
{
   GET_CURRENT_CONTEXT(ctx);
   unsigned numGroups;

   init_groups(ctx);

   /* The GL_INTEL_performance_query spec says:
    *
    *    "If queryId pointer is equal to 0, INVALID_VALUE error is generated."
    */
   if (!queryId) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetFirstPerfQueryIdINTEL(queryId == NULL)");
      return;
   }

   numGroups = ctx->PerfQuery.NumGroups;

   /* The GL_INTEL_performance_query spec says:
    *
    *    "If the given hardware platform doesn't support any performance
    *    queries, then the value of 0 is returned and INVALID_OPERATION error
    *    is raised."
    */
   if (numGroups == 0) {
      *queryId = 0;
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glGetFirstPerfQueryIdINTEL(no queries supported)");
      return;
   }

   *queryId = index_to_queryid(0);
}

extern void GLAPIENTRY
_mesa_GetNextPerfQueryIdINTEL(GLuint queryId, GLuint *nextQueryId)
{
   GET_CURRENT_CONTEXT(ctx);
   init_groups(ctx);

   /* The GL_INTEL_performance_query spec says:
    *
    *    "The result is passed in location pointed by nextQueryId. If query
    *    identified by queryId is the last query available the value of 0 is
    *    returned. If the specified performance query identifier is invalid
    *    then INVALID_VALUE error is generated. If nextQueryId pointer is
    *    equal to 0, an INVALID_VALUE error is generated.  Whenever error is
    *    generated, the value of 0 is returned."
    */

   if (!nextQueryId) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetNextPerfQueryIdINTEL(nextQueryId == NULL)");
      return;
   }

   if (!queryid_valid(ctx, queryId)) {
      *nextQueryId = 0;
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetNextPerfQueryIdINTEL(invalid query)");
      return;
   }

   ++queryId;

   if (!queryid_valid(ctx, queryId)) {
      *nextQueryId = 0;
   } else {
      *nextQueryId = queryId;
   }
}

extern void GLAPIENTRY
_mesa_GetPerfQueryIdByNameINTEL(char *queryName, GLuint *queryId)
{
   GET_CURRENT_CONTEXT(ctx);
   unsigned i;

   init_groups(ctx);

   /* The GL_INTEL_performance_query spec says:
    *
    *    "If queryName does not reference a valid query name, an INVALID_VALUE
    *    error is generated."
    */
   if (!queryName) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetPerfQueryIdByNameINTEL(queryName == NULL)");
      return;
   }

   /* The specification does not state that this produces an error. */
   if (!queryId) {
      _mesa_warning(ctx, "glGetPerfQueryIdByNameINTEL(queryId == NULL)");
      return;
   }

   for (i = 0; i < ctx->PerfQuery.NumGroups; ++i) {
      const struct gl_perf_monitor_group *group_obj = get_group(ctx, i);
      if (strcmp(group_obj->Name, queryName) == 0) {
         *queryId = index_to_queryid(i);
         return;
      }
   }

   _mesa_error(ctx, GL_INVALID_VALUE,
               "glGetPerfQueryIdByNameINTEL(invalid query name)");
}

extern void GLAPIENTRY
_mesa_GetPerfQueryInfoINTEL(GLuint queryId,
                            GLuint queryNameLength, char *queryName,
                            GLuint *dataSize, GLuint *noCounters,
                            GLuint *noActiveInstances,
                            GLuint *capsMask)
{
   GET_CURRENT_CONTEXT(ctx);
   unsigned i;

   const struct gl_perf_monitor_group *group_obj;

   init_groups(ctx);

   group_obj = get_group(ctx, queryid_to_index(queryId));
   if (group_obj == NULL) {
      /* The GL_INTEL_performance_query spec says:
       *
       *    "If queryId does not reference a valid query type, an
       *    INVALID_VALUE error is generated."
       */
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetPerfQueryInfoINTEL(invalid query)");
      return;
   }

   if (queryName) {
      strncpy(queryName, group_obj->Name, queryNameLength);

      /* No specification given about whether the string needs to be
       * zero-terminated. Zero-terminate the string always as we don't
       * otherwise communicate the length of the returned string.
       */
      if (queryNameLength > 0) {
         queryName[queryNameLength - 1] = '\0';
      }
   }

   if (dataSize) {
      unsigned size = 0;
      for (i = 0; i < group_obj->NumCounters; ++i) {
         /* What we get from the driver is group id (uint32_t) + counter id
          * (uint32_t) + value.
          */
         size += 2 * sizeof(uint32_t) + _mesa_perf_query_counter_size(&group_obj->Counters[i]);
      }
      *dataSize = size;
   }

   if (noCounters) {
      *noCounters = group_obj->NumCounters;
   }

   /* The GL_INTEL_performance_query spec says:
    *
    *    "-- the actual number of already created query instances in
    *    maxInstances location"
    *
    * 1) Typo in the specification, should be noActiveInstances.
    * 2) Another typo in the specification, maxInstances parameter is not listed
    *    in the declaration of this function in the list of new functions.
    */
   if (noActiveInstances) {
      *noActiveInstances = _mesa_HashNumEntries(ctx->PerfQuery.Objects);
   }

   if (capsMask) {
      /* TODO: This information not yet available in the monitor structs. For
       * now, we hardcode SINGLE_CONTEXT, since that's what the implementation
       * currently tries very hard to do.
       */
      *capsMask = GL_PERFQUERY_SINGLE_CONTEXT_INTEL;
   }
}

extern void GLAPIENTRY
_mesa_GetPerfCounterInfoINTEL(GLuint queryId, GLuint counterId,
                              GLuint counterNameLength, char *counterName,
                              GLuint counterDescLength, char *counterDesc,
                              GLuint *counterOffset, GLuint *counterDataSize, GLuint *counterTypeEnum,
                              GLuint *counterDataTypeEnum, GLuint64 *rawCounterMaxValue)
{
   GET_CURRENT_CONTEXT(ctx);

   const struct gl_perf_monitor_group *group_obj;
   const struct gl_perf_monitor_counter *counter_obj;
   unsigned counterIndex;
   unsigned i;

   init_groups(ctx);

   group_obj = get_group(ctx, queryid_to_index(queryId));

   /* The GL_INTEL_performance_query spec says:
    *
    *    "If the pair of queryId and counterId does not reference a valid
    *    counter, an INVALID_VALUE error is generated."
    */
   if (group_obj == NULL) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetPerfCounterInfoINTEL(invalid queryId)");
      return;
   }

   counterIndex = counterid_to_index(counterId);
   counter_obj = get_counter(group_obj, counterIndex);

   if (counter_obj == NULL) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetPerfCounterInfoINTEL(invalid counterId)");
      return;
   }

   if (counterName) {
      strncpy(counterName, counter_obj->Name, counterNameLength);

      /* No specification given about whether the string needs to be
       * zero-terminated. Zero-terminate the string always as we don't
       * otherwise communicate the length of the returned string.
       */
      if (counterNameLength > 0) {
         counterName[counterNameLength - 1] = '\0';
      }
   }

   if (counterDesc) {
      /* TODO: No separate description text at the moment. We pass the name
       * again for the moment.
       */
      strncpy(counterDesc, counter_obj->Name, counterDescLength);

      /* No specification given about whether the string needs to be
       * zero-terminated. Zero-terminate the string always as we don't
       * otherwise communicate the length of the returned string.
       */
      if (counterDescLength > 0) {
         counterDesc[counterDescLength - 1] = '\0';
      }
   }

   if (counterOffset) {
      unsigned offset = 0;
      for (i = 0; i < counterIndex; ++i) {
         /* What we get from the driver is group id (uint32_t) + counter id
          * (uint32_t) + value.
          */
         offset += 2 * sizeof(uint32_t) + _mesa_perf_query_counter_size(&group_obj->Counters[i]);
      }
      *counterOffset = 2 * sizeof(uint32_t) + offset;
   }

   if (counterDataSize) {
      *counterDataSize = _mesa_perf_query_counter_size(counter_obj);
   }

   if (counterTypeEnum) {
      /* TODO: Different counter types (semantic type, not data type) not
       * supported as of yet.
       */
      *counterTypeEnum = GL_PERFQUERY_COUNTER_RAW_INTEL;
   }

   if (counterDataTypeEnum) {
      switch (counter_obj->Type) {
      case GL_FLOAT:
      case GL_PERCENTAGE_AMD:
         *counterDataTypeEnum = GL_PERFQUERY_COUNTER_DATA_FLOAT_INTEL;
         break;
      case GL_UNSIGNED_INT:
         *counterDataTypeEnum = GL_PERFQUERY_COUNTER_DATA_UINT32_INTEL;
         break;
      case GL_UNSIGNED_INT64_AMD:
         *counterDataTypeEnum = GL_PERFQUERY_COUNTER_DATA_UINT64_INTEL;
         break;
      default:
         assert(!"Should not get here: invalid counter type");
         return;
      }
   }

   if (rawCounterMaxValue) {
      /* This value is (implicitly) specified to be used only with
       * GL_PERFQUERY_COUNTER_RAW_INTEL counters. When semantic types for
       * counters are added, that needs to be checked.
       */

      /* The GL_INTEL_performance_query spec says:
       *
       *    "for some raw counters for which the maximal value is
       *    deterministic, the maximal value of the counter in 1 second is
       *    returned in the location pointed by rawCounterMaxValue, otherwise,
       *    the location is written with the value of 0."
       *
       * The maximum value reported by the driver at the moment is not with
       * these semantics, so write 0 always to be safe.
       */
      *rawCounterMaxValue = 0;
   }
}

extern void GLAPIENTRY
_mesa_CreatePerfQueryINTEL(GLuint queryId, GLuint *queryHandle)
{
   GET_CURRENT_CONTEXT(ctx);
   GLuint first;
   GLuint group;
   const struct gl_perf_monitor_group *group_obj;
   struct gl_perf_monitor_object *m;
   unsigned i;

   init_groups(ctx);

   /* This is not specified in the extension, but is the only sane thing to
    * do.
    */
   if (queryHandle == NULL) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glCreatePerfQueryINTEL(queryHandle == NULL)");
      return;
   }

   group = queryid_to_index(queryId);
   group_obj = get_group(ctx, group);

   /* The GL_INTEL_performance_query spec says:
    *
    *    "If queryId does not reference a valid query type, an INVALID_VALUE
    *    error is generated."
    */
   if (group_obj == NULL) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glCreatePerfQueryINTEL(invalid queryId)");
      return;
   }

   /* The query object created here is the counterpart of a `monitor' in
    * AMD_performance_monitor. This call is equivalent to calling
    * GenPerfMonitorsAMD and SelectPerfMonitorCountersAMD with a list of all
    * counters in a group.
    */

   /* We keep the query ids contiguous */
   first = _mesa_HashFindFreeKeyBlock(ctx->PerfQuery.Objects, 1);
   if (!first) {
      /* The GL_INTEL_performance_query spec says:
       *
       *    "If the query instance cannot be created due to exceeding the
       *    number of allowed instances or driver fails query creation due to
       *    an insufficient memory reason, an OUT_OF_MEMORY error is
       *    generated, and the location pointed by queryHandle returns NULL."
      */
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "glCreatePerfQueryINTEL");
      return;
   }

   m = new_performance_query(ctx, first);
   if (m == NULL) {
      _mesa_error_no_memory(__func__);
      return;
   }

   _mesa_HashInsert(ctx->PerfQuery.Objects, first, m);
   *queryHandle = first;

   ctx->Driver.ResetPerfMonitor(ctx, m);

   for (i = 0; i < group_obj->NumCounters; ++i) {
      ++m->ActiveGroups[group];
      /* Counters are a continuous range of integers, 0 to NumCounters (excl),
       * so i is the counter value to use here.
       */
      BITSET_SET(m->ActiveCounters[group], i);
   }
}

extern void GLAPIENTRY
_mesa_DeletePerfQueryINTEL(GLuint queryHandle)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_perf_monitor_object *m;

   /* The queryHandle is the counterpart to AMD_performance_monitor's monitor
    * id.
    */
   m = lookup_query(ctx, queryHandle);

   /* The GL_INTEL_performance_query spec says:
    *
    *    "If a query handle doesn't reference a previously created performance
    *    query instance, an INVALID_VALUE error is generated."
    */
   if (m == NULL) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glDeletePerfQueryINTEL(invalid queryHandle)");
      return;
   }

   /* Let the driver stop the query if it's active. */
   if (m->Active) {
      ctx->Driver.ResetPerfMonitor(ctx, m);
      m->Ended = false;
   }

   _mesa_HashRemove(ctx->PerfQuery.Objects, queryHandle);
   ralloc_free(m->ActiveGroups);
   ralloc_free(m->ActiveCounters);
   ctx->Driver.DeletePerfMonitor(ctx, m);
}

extern void GLAPIENTRY
_mesa_BeginPerfQueryINTEL(GLuint queryHandle)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_perf_monitor_object *m;

   /* The queryHandle is the counterpart to AMD_performance_monitor's monitor
    * id.
    */

   m = lookup_query(ctx, queryHandle);

   /* The GL_INTEL_performance_query spec says:
    *
    *    "If a query handle doesn't reference a previously created performance
    *    query instance, an INVALID_VALUE error is generated."
    */
   if (m == NULL) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glBeginPerfQueryINTEL(invalid queryHandle)");
      return;
   }

   /* The GL_INTEL_performance_query spec says:
    *
    *    "Note that some query types, they cannot be collected in the same
    *    time. Therefore calls of BeginPerfQueryINTEL() cannot be nested if
    *    they refer to queries of such different types. In such case
    *    INVALID_OPERATION error is generated."
    *
    * We also generate an INVALID_OPERATION error if the driver can't begin
    * a query for its own reasons, and for nesting the same query.
    */
   if (m->Active) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glBeginPerfQueryINTEL(already active)");
      return;
   }

   if (ctx->Driver.BeginPerfMonitor(ctx, m)) {
      m->Active = true;
      m->Ended = false;
   } else {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glBeginPerfQueryINTEL(driver unable to begin query)");
   }
}

extern void GLAPIENTRY
_mesa_EndPerfQueryINTEL(GLuint queryHandle)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_perf_monitor_object *m;

   /* The queryHandle is the counterpart to AMD_performance_monitor's monitor
    * id.
    */

   m = lookup_query(ctx, queryHandle);

   /* The GL_INTEL_performance_query spec says:
    *
    *    "If a performance query is not currently started, an
    *    INVALID_OPERATION error will be generated."
    *
    * The specification doesn't state that an invalid handle would be an
    * INVALID_VALUE error. Regardless, query for such a handle will not be
    * started, so we generate an INVALID_OPERATION in that case too.
    */
   if (m == NULL) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glEndPerfQueryINTEL(invalid queryHandle)");
      return;
   }

   if (!m->Active) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glEndPerfQueryINTEL(not active)");
      return;
   }

   ctx->Driver.EndPerfMonitor(ctx, m);

   m->Active = false;
   m->Ended = true;
}

extern void GLAPIENTRY
_mesa_GetPerfQueryDataINTEL(GLuint queryHandle, GLuint flags,
                            GLsizei dataSize, void *data, GLuint *bytesWritten)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_perf_monitor_object *m;
   bool result_available;

   /* The GL_INTEL_performance_query spec says:
    *
    *    "If bytesWritten or data pointers are NULL then an INVALID_VALUE
    *    error is generated."
    */
   if (!bytesWritten || !data) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetPerfQueryDataINTEL(bytesWritten or data is NULL)");
      return;
   }

   /* The queryHandle is the counterpart to AMD_performance_monitor's monitor
    * id.
    */

   m = lookup_query(ctx, queryHandle);

   /* The specification doesn't state that an invalid handle generates an
    * error. We could interpret that to mean the case should be handled as
    * "measurement not ready for this query", but what should be done if
    * `flags' equals PERFQUERY_WAIT_INTEL?
    *
    * To resolve this, we just generate an INVALID_VALUE from an invalid query
    * handle.
    */
   if (m == NULL) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetPerfQueryDataINTEL(invalid queryHandle)");
      return;
   }

   /* We need at least enough room for a single value. */
   if (dataSize < sizeof(GLuint)) {
      *bytesWritten = 0;
      return;
   }

   /* The GL_INTEL_performance_query spec says:
    *
    *    "The call may end without returning any data if they are not ready
    *    for reading as the measurement session is still pending (the
    *    EndPerfQueryINTEL() command processing is not finished by
    *    hardware). In this case location pointed by the bytesWritten
    *    parameter will be set to 0."
    *
    * If EndPerfQueryINTEL() is not called at all, we follow this.
    */
   if (!m->Ended) {
      *bytesWritten = 0;
      return;
   }

   result_available = ctx->Driver.IsPerfMonitorResultAvailable(ctx, m);

   if (!result_available) {
      if (flags == GL_PERFQUERY_FLUSH_INTEL) {
         ctx->Driver.Flush(ctx);
      } else if (flags == GL_PERFQUERY_WAIT_INTEL) {
         /* Assume Finish() is both enough and not too much to wait for
          * results. If results are still not available after Finish(), the
          * later code automatically bails out with 0 for bytesWritten.
          */
         ctx->Driver.Finish(ctx);
         result_available =
            ctx->Driver.IsPerfMonitorResultAvailable(ctx, m);
      }
   }

   if (result_available) {
      ctx->Driver.GetPerfMonitorResult(ctx, m, dataSize, data, (GLint*)bytesWritten);
   } else {
      *bytesWritten = 0;
   }
}
