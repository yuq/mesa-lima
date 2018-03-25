/*
 * Copyright (c) 2017 Etnaviv Project
 * Copyright (C) 2017 Zodiac Inflight Innovations
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "etnaviv_context.h"
#include "etnaviv_query_pm.h"
#include "etnaviv_screen.h"

struct etna_perfmon_source
{
   const char *domain;
   const char *signal;
};

struct etna_perfmon_config
{
   const char *name;
   unsigned type;
   const struct etna_perfmon_source *source;
};

static const struct etna_perfmon_config query_config[] = {
};

static const struct etna_perfmon_config *
etna_pm_query_config(unsigned type)
{
   for (unsigned i = 0; i < ARRAY_SIZE(query_config); i++)
      if (query_config[i].type == type)
         return &query_config[i];

   return NULL;
}

static struct etna_perfmon_signal *
etna_pm_query_signal(struct etna_perfmon *perfmon,
                     const struct etna_perfmon_source *source)
{
   struct etna_perfmon_domain *domain;

   domain = etna_perfmon_get_dom_by_name(perfmon, source->domain);
   if (!domain)
      return NULL;

   return etna_perfmon_get_sig_by_name(domain, source->signal);
}

static inline bool
etna_pm_cfg_supported(struct etna_perfmon *perfmon,
                      const struct etna_perfmon_config *cfg)
{
   struct etna_perfmon_signal *signal = etna_pm_query_signal(perfmon, cfg->source);

   return !!signal;
}

static inline void
etna_pm_add_signal(struct etna_pm_query *pq, struct etna_perfmon *perfmon,
                   const struct etna_perfmon_config *cfg)
{
   struct etna_perfmon_signal *signal = etna_pm_query_signal(perfmon, cfg->source);

   pq->signal = signal;
}

static bool
realloc_query_bo(struct etna_context *ctx, struct etna_pm_query *pq)
{
   if (pq->bo)
      etna_bo_del(pq->bo);

   pq->bo = etna_bo_new(ctx->screen->dev, 64, DRM_ETNA_GEM_CACHE_WC);
   if (unlikely(!pq->bo))
      return false;

   pq->data = etna_bo_map(pq->bo);

   return true;
}

static void
etna_pm_query_get(struct etna_cmd_stream *stream, struct etna_query *q,
                  unsigned flags)
{
   struct etna_pm_query *pq = etna_pm_query(q);
   unsigned offset;
   assert(flags);

   if (flags == ETNA_PM_PROCESS_PRE)
      offset = 2;
   else
      offset = 3;

   struct etna_perf p = {
      .flags = flags,
      .sequence = pq->sequence,
      .bo = pq->bo,
      .signal = pq->signal,
      .offset = offset
   };

   etna_cmd_stream_perf(stream, &p);
}

static inline void
etna_pm_query_update(struct etna_query *q)
{
   struct etna_pm_query *pq = etna_pm_query(q);

   if (pq->data[0] == pq->sequence)
      pq->ready = true;
}

static void
etna_pm_destroy_query(struct etna_context *ctx, struct etna_query *q)
{
   struct etna_pm_query *pq = etna_pm_query(q);

   etna_bo_del(pq->bo);
   FREE(pq);
}

static boolean
etna_pm_begin_query(struct etna_context *ctx, struct etna_query *q)
{
   struct etna_pm_query *pq = etna_pm_query(q);

   pq->ready = false;
   pq->sequence++;

   etna_pm_query_get(ctx->stream, q, ETNA_PM_PROCESS_PRE);

   return true;
}

static void
etna_pm_end_query(struct etna_context *ctx, struct etna_query *q)
{
   etna_pm_query_get(ctx->stream, q, ETNA_PM_PROCESS_POST);
}

static boolean
etna_pm_get_query_result(struct etna_context *ctx, struct etna_query *q,
                         boolean wait, union pipe_query_result *result)
{
   struct etna_pm_query *pq = etna_pm_query(q);

   etna_pm_query_update(q);

   if (!pq->ready) {
      if (!wait)
         return false;

      if (!etna_bo_cpu_prep(pq->bo, DRM_ETNA_PREP_READ))
         return false;

      pq->ready = true;
      etna_bo_cpu_fini(pq->bo);
   }

   result->u32 = pq->data[2] - pq->data[1];

   return true;
}

static const struct etna_query_funcs hw_query_funcs = {
   .destroy_query = etna_pm_destroy_query,
   .begin_query = etna_pm_begin_query,
   .end_query = etna_pm_end_query,
   .get_query_result = etna_pm_get_query_result,
};

struct etna_query *
etna_pm_create_query(struct etna_context *ctx, unsigned query_type)
{
   struct etna_perfmon *perfmon = ctx->screen->perfmon;
   const struct etna_perfmon_config *cfg;
   struct etna_pm_query *pq;
   struct etna_query *q;

   cfg = etna_pm_query_config(query_type);
   if (!cfg)
      return NULL;

   if (!etna_pm_cfg_supported(perfmon, cfg))
      return NULL;

   pq = CALLOC_STRUCT(etna_pm_query);
   if (!pq)
      return NULL;

   if (!realloc_query_bo(ctx, pq)) {
      FREE(pq);
      return NULL;
   }

   q = &pq->base;
   q->funcs = &hw_query_funcs;
   q->type = query_type;

   etna_pm_add_signal(pq, perfmon, cfg);

   return q;
}

void
etna_pm_query_setup(struct etna_screen *screen)
{
   screen->perfmon = etna_perfmon_create(screen->pipe);

   if (!screen->perfmon)
      return;

   for (unsigned i = 0; i < ARRAY_SIZE(query_config); i++) {
      const struct etna_perfmon_config *cfg = &query_config[i];

      if (!etna_pm_cfg_supported(screen->perfmon, cfg))
         continue;

      util_dynarray_append(&screen->supported_pm_queries, unsigned, i);
   }
}

int
etna_pm_get_driver_query_info(struct pipe_screen *pscreen, unsigned index,
                              struct pipe_driver_query_info *info)
{
   const struct etna_screen *screen = etna_screen(pscreen);
   const unsigned num = screen->supported_pm_queries.size / sizeof(unsigned);
   unsigned i;

   if (!info)
      return num;

   if (index >= num)
      return 0;

   i = *util_dynarray_element(&screen->supported_pm_queries, unsigned, index);
   assert(i < ARRAY_SIZE(query_config));

   info->name = query_config[i].name;
   info->query_type = query_config[i].type;
   info->group_id = 0;

   return 1;
}
