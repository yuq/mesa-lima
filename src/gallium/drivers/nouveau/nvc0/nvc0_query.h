#ifndef __NVC0_QUERY_H__
#define __NVC0_QUERY_H__

#include "pipe/p_context.h"

#include "nouveau_context.h"
#include "nouveau_mm.h"

#define NVC0_QUERY_TFB_BUFFER_OFFSET (PIPE_QUERY_TYPES + 0)

struct nvc0_context;
struct nvc0_query;

struct nvc0_query_funcs {
   void (*destroy_query)(struct nvc0_context *, struct nvc0_query *);
   boolean (*begin_query)(struct nvc0_context *, struct nvc0_query *);
   void (*end_query)(struct nvc0_context *, struct nvc0_query *);
   boolean (*get_query_result)(struct nvc0_context *, struct nvc0_query *,
                               boolean, union pipe_query_result *);
};

struct nvc0_query {
   const struct nvc0_query_funcs *funcs;
   uint32_t *data;
   uint16_t type;
   uint16_t index;
   int8_t ctr[4];
   uint32_t sequence;
   struct nouveau_bo *bo;
   uint32_t base;
   uint32_t offset; /* base + i * rotate */
   uint8_t state;
   boolean is64bit;
   uint8_t rotate;
   int nesting; /* only used for occlusion queries */
   struct nouveau_mm_allocation *mm;
   struct nouveau_fence *fence;
};

static inline struct nvc0_query *
nvc0_query(struct pipe_query *pipe)
{
   return (struct nvc0_query *)pipe;
}

/*
 * Driver queries groups:
 */
#define NVC0_QUERY_MP_COUNTER_GROUP 0
#define NVC0_SW_QUERY_DRV_STAT_GROUP 1

/*
 * Performance counter queries:
 */
#define NVE4_HW_SM_QUERY(i)    (PIPE_QUERY_DRIVER_SPECIFIC + (i))
#define NVE4_HW_SM_QUERY_LAST   NVE4_HW_SM_QUERY(NVE4_HW_SM_QUERY_COUNT - 1)
enum nve4_pm_queries
{
   NVE4_HW_SM_QUERY_ACTIVE_CYCLES = 0,
   NVE4_HW_SM_QUERY_ACTIVE_WARPS,
   NVE4_HW_SM_QUERY_ATOM_COUNT,
   NVE4_HW_SM_QUERY_BRANCH,
   NVE4_HW_SM_QUERY_DIVERGENT_BRANCH,
   NVE4_HW_SM_QUERY_GLD_REQUEST,
   NVE4_HW_SM_QUERY_GLD_MEM_DIV_REPLAY,
   NVE4_HW_SM_QUERY_GST_TRANSACTIONS,
   NVE4_HW_SM_QUERY_GST_MEM_DIV_REPLAY,
   NVE4_HW_SM_QUERY_GRED_COUNT,
   NVE4_HW_SM_QUERY_GST_REQUEST,
   NVE4_HW_SM_QUERY_INST_EXECUTED,
   NVE4_HW_SM_QUERY_INST_ISSUED,
   NVE4_HW_SM_QUERY_INST_ISSUED1,
   NVE4_HW_SM_QUERY_INST_ISSUED2,
   NVE4_HW_SM_QUERY_L1_GLD_HIT,
   NVE4_HW_SM_QUERY_L1_GLD_MISS,
   NVE4_HW_SM_QUERY_L1_LOCAL_LD_HIT,
   NVE4_HW_SM_QUERY_L1_LOCAL_LD_MISS,
   NVE4_HW_SM_QUERY_L1_LOCAL_ST_HIT,
   NVE4_HW_SM_QUERY_L1_LOCAL_ST_MISS,
   NVE4_HW_SM_QUERY_L1_SHARED_LD_TRANSACTIONS,
   NVE4_HW_SM_QUERY_L1_SHARED_ST_TRANSACTIONS,
   NVE4_HW_SM_QUERY_LOCAL_LD,
   NVE4_HW_SM_QUERY_LOCAL_LD_TRANSACTIONS,
   NVE4_HW_SM_QUERY_LOCAL_ST,
   NVE4_HW_SM_QUERY_LOCAL_ST_TRANSACTIONS,
   NVE4_HW_SM_QUERY_PROF_TRIGGER_0,
   NVE4_HW_SM_QUERY_PROF_TRIGGER_1,
   NVE4_HW_SM_QUERY_PROF_TRIGGER_2,
   NVE4_HW_SM_QUERY_PROF_TRIGGER_3,
   NVE4_HW_SM_QUERY_PROF_TRIGGER_4,
   NVE4_HW_SM_QUERY_PROF_TRIGGER_5,
   NVE4_HW_SM_QUERY_PROF_TRIGGER_6,
   NVE4_HW_SM_QUERY_PROF_TRIGGER_7,
   NVE4_HW_SM_QUERY_SHARED_LD,
   NVE4_HW_SM_QUERY_SHARED_LD_REPLAY,
   NVE4_HW_SM_QUERY_SHARED_ST,
   NVE4_HW_SM_QUERY_SHARED_ST_REPLAY,
   NVE4_HW_SM_QUERY_SM_CTA_LAUNCHED,
   NVE4_HW_SM_QUERY_THREADS_LAUNCHED,
   NVE4_HW_SM_QUERY_UNCACHED_GLD_TRANSACTIONS,
   NVE4_HW_SM_QUERY_WARPS_LAUNCHED,
   NVE4_HW_SM_QUERY_METRIC_IPC,
   NVE4_HW_SM_QUERY_METRIC_IPAC,
   NVE4_HW_SM_QUERY_METRIC_IPEC,
   NVE4_HW_SM_QUERY_METRIC_MP_OCCUPANCY,
   NVE4_HW_SM_QUERY_METRIC_MP_EFFICIENCY,
   NVE4_HW_SM_QUERY_METRIC_INST_REPLAY_OHEAD,
   NVE4_HW_SM_QUERY_COUNT
};

#define NVC0_HW_SM_QUERY(i)    (PIPE_QUERY_DRIVER_SPECIFIC + 2048 + (i))
#define NVC0_HW_SM_QUERY_LAST   NVC0_HW_SM_QUERY(NVC0_HW_SM_QUERY_COUNT - 1)
enum nvc0_pm_queries
{
   NVC0_HW_SM_QUERY_ACTIVE_CYCLES = 0,
   NVC0_HW_SM_QUERY_ACTIVE_WARPS,
   NVC0_HW_SM_QUERY_ATOM_COUNT,
   NVC0_HW_SM_QUERY_BRANCH,
   NVC0_HW_SM_QUERY_DIVERGENT_BRANCH,
   NVC0_HW_SM_QUERY_GLD_REQUEST,
   NVC0_HW_SM_QUERY_GRED_COUNT,
   NVC0_HW_SM_QUERY_GST_REQUEST,
   NVC0_HW_SM_QUERY_INST_EXECUTED,
   NVC0_HW_SM_QUERY_INST_ISSUED1_0,
   NVC0_HW_SM_QUERY_INST_ISSUED1_1,
   NVC0_HW_SM_QUERY_INST_ISSUED2_0,
   NVC0_HW_SM_QUERY_INST_ISSUED2_1,
   NVC0_HW_SM_QUERY_LOCAL_LD,
   NVC0_HW_SM_QUERY_LOCAL_ST,
   NVC0_HW_SM_QUERY_PROF_TRIGGER_0,
   NVC0_HW_SM_QUERY_PROF_TRIGGER_1,
   NVC0_HW_SM_QUERY_PROF_TRIGGER_2,
   NVC0_HW_SM_QUERY_PROF_TRIGGER_3,
   NVC0_HW_SM_QUERY_PROF_TRIGGER_4,
   NVC0_HW_SM_QUERY_PROF_TRIGGER_5,
   NVC0_HW_SM_QUERY_PROF_TRIGGER_6,
   NVC0_HW_SM_QUERY_PROF_TRIGGER_7,
   NVC0_HW_SM_QUERY_SHARED_LD,
   NVC0_HW_SM_QUERY_SHARED_ST,
   NVC0_HW_SM_QUERY_THREADS_LAUNCHED,
   NVC0_HW_SM_QUERY_TH_INST_EXECUTED_0,
   NVC0_HW_SM_QUERY_TH_INST_EXECUTED_1,
   NVC0_HW_SM_QUERY_TH_INST_EXECUTED_2,
   NVC0_HW_SM_QUERY_TH_INST_EXECUTED_3,
   NVC0_HW_SM_QUERY_WARPS_LAUNCHED,
   NVC0_HW_SM_QUERY_COUNT
};

void nvc0_init_query_functions(struct nvc0_context *);
void nvc0_query_pushbuf_submit(struct nouveau_pushbuf *, struct nvc0_query *,
                               unsigned);
void nvc0_query_fifo_wait(struct nouveau_pushbuf *, struct nvc0_query *);

#endif
