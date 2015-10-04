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

#define NVC0_PUSH_EXPLICIT_SPACE_CHECKING

#include "nvc0/nvc0_context.h"
#include "nvc0/nvc0_query.h"
#include "nvc0/nvc0_query_sw.h"
#include "nvc0/nvc0_query_hw.h"

static struct pipe_query *
nvc0_create_query(struct pipe_context *pipe, unsigned type, unsigned index)
{
   struct nvc0_context *nvc0 = nvc0_context(pipe);
   struct nvc0_query *q;

   q = nvc0_sw_create_query(nvc0, type, index);
   if (!q)
      q = nvc0_hw_create_query(nvc0, type, index);

   return (struct pipe_query *)q;
}

static void
nvc0_destroy_query(struct pipe_context *pipe, struct pipe_query *pq)
{
   struct nvc0_query *q = nvc0_query(pq);
   q->funcs->destroy_query(nvc0_context(pipe), q);
}

static boolean
nvc0_begin_query(struct pipe_context *pipe, struct pipe_query *pq)
{
   struct nvc0_query *q = nvc0_query(pq);
   return q->funcs->begin_query(nvc0_context(pipe), q);
}

static void
nvc0_end_query(struct pipe_context *pipe, struct pipe_query *pq)
{
   struct nvc0_query *q = nvc0_query(pq);
   q->funcs->end_query(nvc0_context(pipe), q);
}

static boolean
nvc0_get_query_result(struct pipe_context *pipe, struct pipe_query *pq,
                      boolean wait, union pipe_query_result *result)
{
   struct nvc0_query *q = nvc0_query(pq);
   return q->funcs->get_query_result(nvc0_context(pipe), q, wait, result);
}

static void
nvc0_render_condition(struct pipe_context *pipe,
                      struct pipe_query *pq,
                      boolean condition, uint mode)
{
   struct nvc0_context *nvc0 = nvc0_context(pipe);
   struct nouveau_pushbuf *push = nvc0->base.pushbuf;
   struct nvc0_query *q = nvc0_query(pq);
   struct nvc0_hw_query *hq = nvc0_hw_query(q);
   uint32_t cond;
   bool wait =
      mode != PIPE_RENDER_COND_NO_WAIT &&
      mode != PIPE_RENDER_COND_BY_REGION_NO_WAIT;

   if (!pq) {
      cond = NVC0_3D_COND_MODE_ALWAYS;
   }
   else {
      /* NOTE: comparison of 2 queries only works if both have completed */
      switch (q->type) {
      case PIPE_QUERY_SO_OVERFLOW_PREDICATE:
         cond = condition ? NVC0_3D_COND_MODE_EQUAL :
                          NVC0_3D_COND_MODE_NOT_EQUAL;
         wait = true;
         break;
      case PIPE_QUERY_OCCLUSION_COUNTER:
      case PIPE_QUERY_OCCLUSION_PREDICATE:
         if (likely(!condition)) {
            if (unlikely(hq->nesting))
               cond = wait ? NVC0_3D_COND_MODE_NOT_EQUAL :
                             NVC0_3D_COND_MODE_ALWAYS;
            else
               cond = NVC0_3D_COND_MODE_RES_NON_ZERO;
         } else {
            cond = wait ? NVC0_3D_COND_MODE_EQUAL : NVC0_3D_COND_MODE_ALWAYS;
         }
         break;
      default:
         assert(!"render condition query not a predicate");
         cond = NVC0_3D_COND_MODE_ALWAYS;
         break;
      }
   }

   nvc0->cond_query = pq;
   nvc0->cond_cond = condition;
   nvc0->cond_condmode = cond;
   nvc0->cond_mode = mode;

   if (!pq) {
      PUSH_SPACE(push, 1);
      IMMED_NVC0(push, NVC0_3D(COND_MODE), cond);
      return;
   }

   if (wait)
      nvc0_hw_query_fifo_wait(push, q);

   PUSH_SPACE(push, 7);
   PUSH_REFN (push, hq->bo, NOUVEAU_BO_GART | NOUVEAU_BO_RD);
   BEGIN_NVC0(push, NVC0_3D(COND_ADDRESS_HIGH), 3);
   PUSH_DATAh(push, hq->bo->offset + hq->offset);
   PUSH_DATA (push, hq->bo->offset + hq->offset);
   PUSH_DATA (push, cond);
   BEGIN_NVC0(push, NVC0_2D(COND_ADDRESS_HIGH), 2);
   PUSH_DATAh(push, hq->bo->offset + hq->offset);
   PUSH_DATA (push, hq->bo->offset + hq->offset);
}

/* === DRIVER STATISTICS === */

#ifdef NOUVEAU_ENABLE_DRIVER_STATISTICS

static const char *nvc0_sw_query_drv_stat_names[] =
{
   "drv-tex_obj_current_count",
   "drv-tex_obj_current_bytes",
   "drv-buf_obj_current_count",
   "drv-buf_obj_current_bytes_vid",
   "drv-buf_obj_current_bytes_sys",
   "drv-tex_transfers_rd",
   "drv-tex_transfers_wr",
   "drv-tex_copy_count",
   "drv-tex_blit_count",
   "drv-tex_cache_flush_count",
   "drv-buf_transfers_rd",
   "drv-buf_transfers_wr",
   "drv-buf_read_bytes_staging_vid",
   "drv-buf_write_bytes_direct",
   "drv-buf_write_bytes_staging_vid",
   "drv-buf_write_bytes_staging_sys",
   "drv-buf_copy_bytes",
   "drv-buf_non_kernel_fence_sync_count",
   "drv-any_non_kernel_fence_sync_count",
   "drv-query_sync_count",
   "drv-gpu_serialize_count",
   "drv-draw_calls_array",
   "drv-draw_calls_indexed",
   "drv-draw_calls_fallback_count",
   "drv-user_buffer_upload_bytes",
   "drv-constbuf_upload_count",
   "drv-constbuf_upload_bytes",
   "drv-pushbuf_count",
   "drv-resource_validate_count"
};

#endif /* NOUVEAU_ENABLE_DRIVER_STATISTICS */

/* === PERFORMANCE MONITORING COUNTERS for NVE4+ === */

/* NOTE: intentionally using the same names as NV */
static const char *nve4_pm_query_names[] =
{
   /* MP counters */
   "active_cycles",
   "active_warps",
   "atom_count",
   "branch",
   "divergent_branch",
   "gld_request",
   "global_ld_mem_divergence_replays",
   "global_store_transaction",
   "global_st_mem_divergence_replays",
   "gred_count",
   "gst_request",
   "inst_executed",
   "inst_issued",
   "inst_issued1",
   "inst_issued2",
   "l1_global_load_hit",
   "l1_global_load_miss",
   "l1_local_load_hit",
   "l1_local_load_miss",
   "l1_local_store_hit",
   "l1_local_store_miss",
   "l1_shared_load_transactions",
   "l1_shared_store_transactions",
   "local_load",
   "local_load_transactions",
   "local_store",
   "local_store_transactions",
   "prof_trigger_00",
   "prof_trigger_01",
   "prof_trigger_02",
   "prof_trigger_03",
   "prof_trigger_04",
   "prof_trigger_05",
   "prof_trigger_06",
   "prof_trigger_07",
   "shared_load",
   "shared_load_replay",
   "shared_store",
   "shared_store_replay",
   "sm_cta_launched",
   "threads_launched",
   "uncached_global_load_transaction",
   "warps_launched",
   /* metrics, i.e. functions of the MP counters */
   "metric-ipc",                   /* inst_executed, clock */
   "metric-ipac",                  /* inst_executed, active_cycles */
   "metric-ipec",                  /* inst_executed, (bool)inst_executed */
   "metric-achieved_occupancy",    /* active_warps, active_cycles */
   "metric-sm_efficiency",         /* active_cycles, clock */
   "metric-inst_replay_overhead"   /* inst_issued, inst_executed */
};

/* === PERFORMANCE MONITORING COUNTERS for NVC0:NVE4 === */
static const char *nvc0_pm_query_names[] =
{
   /* MP counters */
   "active_cycles",
   "active_warps",
   "atom_count",
   "branch",
   "divergent_branch",
   "gld_request",
   "gred_count",
   "gst_request",
   "inst_executed",
   "inst_issued1_0",
   "inst_issued1_1",
   "inst_issued2_0",
   "inst_issued2_1",
   "local_load",
   "local_store",
   "prof_trigger_00",
   "prof_trigger_01",
   "prof_trigger_02",
   "prof_trigger_03",
   "prof_trigger_04",
   "prof_trigger_05",
   "prof_trigger_06",
   "prof_trigger_07",
   "shared_load",
   "shared_store",
   "threads_launched",
   "thread_inst_executed_0",
   "thread_inst_executed_1",
   "thread_inst_executed_2",
   "thread_inst_executed_3",
   "warps_launched",
};

int
nvc0_screen_get_driver_query_info(struct pipe_screen *pscreen,
                                  unsigned id,
                                  struct pipe_driver_query_info *info)
{
   struct nvc0_screen *screen = nvc0_screen(pscreen);
   int count = 0;

   count += NVC0_SW_QUERY_DRV_STAT_COUNT;

   if (screen->base.device->drm_version >= 0x01000101) {
      if (screen->compute) {
         if (screen->base.class_3d == NVE4_3D_CLASS) {
            count += NVE4_HW_SM_QUERY_COUNT;
         } else
         if (screen->base.class_3d < NVE4_3D_CLASS) {
            /* NVC0_COMPUTE is not always enabled */
            count += NVC0_HW_SM_QUERY_COUNT;
         }
      }
   }

   if (!info)
      return count;

   /* Init default values. */
   info->name = "this_is_not_the_query_you_are_looking_for";
   info->query_type = 0xdeadd01d;
   info->max_value.u64 = 0;
   info->type = PIPE_DRIVER_QUERY_TYPE_UINT64;
   info->group_id = -1;

#ifdef NOUVEAU_ENABLE_DRIVER_STATISTICS
   if (id < NVC0_SW_QUERY_DRV_STAT_COUNT) {
      info->name = nvc0_sw_query_drv_stat_names[id];
      info->query_type = NVC0_SW_QUERY_DRV_STAT(id);
      info->max_value.u64 = 0;
      if (strstr(info->name, "bytes"))
         info->type = PIPE_DRIVER_QUERY_TYPE_BYTES;
      info->group_id = NVC0_SW_QUERY_DRV_STAT_GROUP;
      return 1;
   } else
#endif
   if (id < count) {
      if (screen->compute) {
         if (screen->base.class_3d == NVE4_3D_CLASS) {
            info->name = nve4_pm_query_names[id - NVC0_SW_QUERY_DRV_STAT_COUNT];
            info->query_type = NVE4_HW_SM_QUERY(id - NVC0_SW_QUERY_DRV_STAT_COUNT);
            info->max_value.u64 =
               (id < NVE4_HW_SM_QUERY_METRIC_MP_OCCUPANCY) ? 0 : 100;
            info->group_id = NVC0_QUERY_MP_COUNTER_GROUP;
            return 1;
         } else
         if (screen->base.class_3d < NVE4_3D_CLASS) {
            info->name = nvc0_pm_query_names[id - NVC0_SW_QUERY_DRV_STAT_COUNT];
            info->query_type = NVC0_HW_SM_QUERY(id - NVC0_SW_QUERY_DRV_STAT_COUNT);
            info->group_id = NVC0_QUERY_MP_COUNTER_GROUP;
            return 1;
         }
      }
   }
   /* user asked for info about non-existing query */
   return 0;
}

int
nvc0_screen_get_driver_query_group_info(struct pipe_screen *pscreen,
                                        unsigned id,
                                        struct pipe_driver_query_group_info *info)
{
   struct nvc0_screen *screen = nvc0_screen(pscreen);
   int count = 0;

#ifdef NOUVEAU_ENABLE_DRIVER_STATISTICS
   count++;
#endif

   if (screen->base.device->drm_version >= 0x01000101) {
      if (screen->compute) {
         if (screen->base.class_3d == NVE4_3D_CLASS) {
            count++;
         } else
         if (screen->base.class_3d < NVE4_3D_CLASS) {
            count++; /* NVC0_COMPUTE is not always enabled */
         }
      }
   }

   if (!info)
      return count;

   if (id == NVC0_QUERY_MP_COUNTER_GROUP) {
      if (screen->compute) {
         info->name = "MP counters";
         info->type = PIPE_DRIVER_QUERY_GROUP_TYPE_GPU;

         if (screen->base.class_3d == NVE4_3D_CLASS) {
            info->num_queries = NVE4_HW_SM_QUERY_COUNT;

             /* On NVE4+, each multiprocessor have 8 hardware counters separated
              * in two distinct domains, but we allow only one active query
              * simultaneously because some of them use more than one hardware
              * counter and this will result in an undefined behaviour. */
             info->max_active_queries = 1; /* TODO: handle multiple hw counters */
             return 1;
         } else
         if (screen->base.class_3d < NVE4_3D_CLASS) {
            info->num_queries = NVC0_HW_SM_QUERY_COUNT;

            /* On NVC0:NVE4, each multiprocessor have 8 hardware counters
             * in a single domain. */
            info->max_active_queries = 8;
            return 1;
         }
      }
   }
#ifdef NOUVEAU_ENABLE_DRIVER_STATISTICS
   else if (id == NVC0_SW_QUERY_DRV_STAT_GROUP) {
      info->name = "Driver statistics";
      info->type = PIPE_DRIVER_QUERY_GROUP_TYPE_CPU;
      info->max_active_queries = NVC0_SW_QUERY_DRV_STAT_COUNT;
      info->num_queries = NVC0_SW_QUERY_DRV_STAT_COUNT;
      return 1;
   }
#endif

   /* user asked for info about non-existing query group */
   info->name = "this_is_not_the_query_group_you_are_looking_for";
   info->max_active_queries = 0;
   info->num_queries = 0;
   info->type = 0;
   return 0;
}

void
nvc0_init_query_functions(struct nvc0_context *nvc0)
{
   struct pipe_context *pipe = &nvc0->base.pipe;

   pipe->create_query = nvc0_create_query;
   pipe->destroy_query = nvc0_destroy_query;
   pipe->begin_query = nvc0_begin_query;
   pipe->end_query = nvc0_end_query;
   pipe->get_query_result = nvc0_get_query_result;
   pipe->render_condition = nvc0_render_condition;
}
