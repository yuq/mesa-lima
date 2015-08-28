#ifndef __NVC0_SCREEN_H__
#define __NVC0_SCREEN_H__

#include "nouveau_screen.h"
#include "nouveau_mm.h"
#include "nouveau_fence.h"
#include "nouveau_heap.h"

#include "nv_object.xml.h"

#include "nvc0/nvc0_winsys.h"
#include "nvc0/nvc0_stateobj.h"

#define NVC0_TIC_MAX_ENTRIES 2048
#define NVC0_TSC_MAX_ENTRIES 2048

/* doesn't count reserved slots (for auxiliary constants, immediates, etc.) */
#define NVC0_MAX_PIPE_CONSTBUFS         14
#define NVE4_MAX_PIPE_CONSTBUFS_COMPUTE  7

#define NVC0_MAX_SURFACE_SLOTS 16

#define NVC0_MAX_VIEWPORTS 16


struct nvc0_context;

struct nvc0_blitter;

struct nvc0_graph_state {
   bool flushed;
   bool rasterizer_discard;
   bool early_z_forced;
   bool prim_restart;
   uint32_t instance_elts; /* bitmask of per-instance elements */
   uint32_t instance_base;
   uint32_t constant_vbos;
   uint32_t constant_elts;
   int32_t index_bias;
   uint16_t scissor;
   uint8_t patch_vertices;
   uint8_t vbo_mode; /* 0 = normal, 1 = translate, 3 = translate, forced */
   uint8_t num_vtxbufs;
   uint8_t num_vtxelts;
   uint8_t num_textures[6];
   uint8_t num_samplers[6];
   uint8_t tls_required; /* bitmask of shader types using l[] */
   uint8_t c14_bound; /* whether immediate array constbuf is bound */
   uint8_t clip_enable;
   uint32_t clip_mode;
   uint32_t uniform_buffer_bound[5];
   struct nvc0_transform_feedback_state *tfb;
};

struct nvc0_screen {
   struct nouveau_screen base;

   struct nvc0_context *cur_ctx;
   struct nvc0_graph_state save_state;

   int num_occlusion_queries_active;

   struct nouveau_bo *text;
   struct nouveau_bo *parm;       /* for COMPUTE */
   struct nouveau_bo *uniform_bo; /* for 3D */
   struct nouveau_bo *tls;
   struct nouveau_bo *txc; /* TIC (offset 0) and TSC (65536) */
   struct nouveau_bo *poly_cache;

   uint16_t mp_count;
   uint16_t mp_count_compute; /* magic reg can make compute use fewer MPs */

   struct nouveau_heap *text_heap;
   struct nouveau_heap *lib_code; /* allocated from text_heap */

   struct nvc0_blitter *blitter;

   struct {
      void **entries;
      int next;
      uint32_t lock[NVC0_TIC_MAX_ENTRIES / 32];
   } tic;

   struct {
      void **entries;
      int next;
      uint32_t lock[NVC0_TSC_MAX_ENTRIES / 32];
   } tsc;

   struct {
      struct nouveau_bo *bo;
      uint32_t *map;
   } fence;

   struct {
      struct nvc0_program *prog; /* compute state object to read MP counters */
      struct pipe_query *mp_counter[8]; /* counter to query allocation */
      uint8_t num_mp_pm_active[2];
      bool mp_counters_enabled;
   } pm;

   struct nouveau_object *eng3d; /* sqrt(1/2)|kepler> + sqrt(1/2)|fermi> */
   struct nouveau_object *eng2d;
   struct nouveau_object *m2mf;
   struct nouveau_object *compute;
   struct nouveau_object *nvsw;
};

static inline struct nvc0_screen *
nvc0_screen(struct pipe_screen *screen)
{
   return (struct nvc0_screen *)screen;
}

/*
 * Performance counters groups:
 */
#define NVC0_QUERY_MP_COUNTER_GROUP 0
#define NVC0_QUERY_DRV_STAT_GROUP   1

/* Performance counter queries:
 */
#define NVE4_PM_QUERY(i)    (PIPE_QUERY_DRIVER_SPECIFIC + (i))
#define NVE4_PM_QUERY_LAST   NVE4_PM_QUERY(NVE4_PM_QUERY_COUNT - 1)
enum nve4_pm_queries
{
    NVE4_PM_QUERY_PROF_TRIGGER_0 = 0,
    NVE4_PM_QUERY_PROF_TRIGGER_1,
    NVE4_PM_QUERY_PROF_TRIGGER_2,
    NVE4_PM_QUERY_PROF_TRIGGER_3,
    NVE4_PM_QUERY_PROF_TRIGGER_4,
    NVE4_PM_QUERY_PROF_TRIGGER_5,
    NVE4_PM_QUERY_PROF_TRIGGER_6,
    NVE4_PM_QUERY_PROF_TRIGGER_7,
    NVE4_PM_QUERY_LAUNCHED_WARPS,
    NVE4_PM_QUERY_LAUNCHED_THREADS,
    NVE4_PM_QUERY_LAUNCHED_CTA,
    NVE4_PM_QUERY_INST_ISSUED1,
    NVE4_PM_QUERY_INST_ISSUED2,
    NVE4_PM_QUERY_INST_EXECUTED,
    NVE4_PM_QUERY_LD_LOCAL,
    NVE4_PM_QUERY_ST_LOCAL,
    NVE4_PM_QUERY_LD_SHARED,
    NVE4_PM_QUERY_ST_SHARED,
    NVE4_PM_QUERY_L1_LOCAL_LOAD_HIT,
    NVE4_PM_QUERY_L1_LOCAL_LOAD_MISS,
    NVE4_PM_QUERY_L1_LOCAL_STORE_HIT,
    NVE4_PM_QUERY_L1_LOCAL_STORE_MISS,
    NVE4_PM_QUERY_GLD_REQUEST,
    NVE4_PM_QUERY_GST_REQUEST,
    NVE4_PM_QUERY_L1_GLOBAL_LOAD_HIT,
    NVE4_PM_QUERY_L1_GLOBAL_LOAD_MISS,
    NVE4_PM_QUERY_GLD_TRANSACTIONS_UNCACHED,
    NVE4_PM_QUERY_GST_TRANSACTIONS,
    NVE4_PM_QUERY_BRANCH,
    NVE4_PM_QUERY_BRANCH_DIVERGENT,
    NVE4_PM_QUERY_ACTIVE_WARPS,
    NVE4_PM_QUERY_ACTIVE_CYCLES,
    NVE4_PM_QUERY_INST_ISSUED,
    NVE4_PM_QUERY_ATOM_COUNT,
    NVE4_PM_QUERY_GRED_COUNT,
    NVE4_PM_QUERY_LD_SHARED_REPLAY,
    NVE4_PM_QUERY_ST_SHARED_REPLAY,
    NVE4_PM_QUERY_LD_LOCAL_TRANSACTIONS,
    NVE4_PM_QUERY_ST_LOCAL_TRANSACTIONS,
    NVE4_PM_QUERY_L1_LD_SHARED_TRANSACTIONS,
    NVE4_PM_QUERY_L1_ST_SHARED_TRANSACTIONS,
    NVE4_PM_QUERY_GLD_MEM_DIV_REPLAY,
    NVE4_PM_QUERY_GST_MEM_DIV_REPLAY,
    NVE4_PM_QUERY_METRIC_IPC,
    NVE4_PM_QUERY_METRIC_IPAC,
    NVE4_PM_QUERY_METRIC_IPEC,
    NVE4_PM_QUERY_METRIC_MP_OCCUPANCY,
    NVE4_PM_QUERY_METRIC_MP_EFFICIENCY,
    NVE4_PM_QUERY_METRIC_INST_REPLAY_OHEAD,
    NVE4_PM_QUERY_COUNT
};

#define NVC0_PM_QUERY(i)    (PIPE_QUERY_DRIVER_SPECIFIC + 2048 + (i))
#define NVC0_PM_QUERY_LAST   NVC0_PM_QUERY(NVC0_PM_QUERY_COUNT - 1)
enum nvc0_pm_queries
{
    NVC0_PM_QUERY_INST_EXECUTED = 0,
    NVC0_PM_QUERY_BRANCH,
    NVC0_PM_QUERY_BRANCH_DIVERGENT,
    NVC0_PM_QUERY_ACTIVE_WARPS,
    NVC0_PM_QUERY_ACTIVE_CYCLES,
    NVC0_PM_QUERY_LAUNCHED_WARPS,
    NVC0_PM_QUERY_LAUNCHED_THREADS,
    NVC0_PM_QUERY_LD_SHARED,
    NVC0_PM_QUERY_ST_SHARED,
    NVC0_PM_QUERY_LD_LOCAL,
    NVC0_PM_QUERY_ST_LOCAL,
    NVC0_PM_QUERY_GRED_COUNT,
    NVC0_PM_QUERY_ATOM_COUNT,
    NVC0_PM_QUERY_GLD_REQUEST,
    NVC0_PM_QUERY_GST_REQUEST,
    NVC0_PM_QUERY_INST_ISSUED1_0,
    NVC0_PM_QUERY_INST_ISSUED1_1,
    NVC0_PM_QUERY_INST_ISSUED2_0,
    NVC0_PM_QUERY_INST_ISSUED2_1,
    NVC0_PM_QUERY_TH_INST_EXECUTED_0,
    NVC0_PM_QUERY_TH_INST_EXECUTED_1,
    NVC0_PM_QUERY_TH_INST_EXECUTED_2,
    NVC0_PM_QUERY_TH_INST_EXECUTED_3,
    NVC0_PM_QUERY_PROF_TRIGGER_0,
    NVC0_PM_QUERY_PROF_TRIGGER_1,
    NVC0_PM_QUERY_PROF_TRIGGER_2,
    NVC0_PM_QUERY_PROF_TRIGGER_3,
    NVC0_PM_QUERY_PROF_TRIGGER_4,
    NVC0_PM_QUERY_PROF_TRIGGER_5,
    NVC0_PM_QUERY_PROF_TRIGGER_6,
    NVC0_PM_QUERY_PROF_TRIGGER_7,
    NVC0_PM_QUERY_COUNT
};

/* Driver statistics queries:
 */
#define NVC0_QUERY_DRV_STAT(i)    (PIPE_QUERY_DRIVER_SPECIFIC + 1024 + (i))
#define NVC0_QUERY_DRV_STAT_LAST   NVC0_QUERY_DRV_STAT(NVC0_QUERY_DRV_STAT_COUNT - 1)
enum nvc0_drv_stats_queries
{
#ifdef NOUVEAU_ENABLE_DRIVER_STATISTICS
    NVC0_QUERY_DRV_STAT_TEX_OBJECT_CURRENT_COUNT = 0,
    NVC0_QUERY_DRV_STAT_TEX_OBJECT_CURRENT_BYTES,
    NVC0_QUERY_DRV_STAT_BUF_OBJECT_CURRENT_COUNT,
    NVC0_QUERY_DRV_STAT_BUF_OBJECT_CURRENT_BYTES_VID,
    NVC0_QUERY_DRV_STAT_BUF_OBJECT_CURRENT_BYTES_SYS,
    NVC0_QUERY_DRV_STAT_TEX_TRANSFERS_READ,
    NVC0_QUERY_DRV_STAT_TEX_TRANSFERS_WRITE,
    NVC0_QUERY_DRV_STAT_TEX_COPY_COUNT,
    NVC0_QUERY_DRV_STAT_TEX_BLIT_COUNT,
    NVC0_QUERY_DRV_STAT_TEX_CACHE_FLUSH_COUNT,
    NVC0_QUERY_DRV_STAT_BUF_TRANSFERS_READ,
    NVC0_QUERY_DRV_STAT_BUF_TRANSFERS_WRITE,
    NVC0_QUERY_DRV_STAT_BUF_READ_BYTES_STAGING_VID,
    NVC0_QUERY_DRV_STAT_BUF_WRITE_BYTES_DIRECT,
    NVC0_QUERY_DRV_STAT_BUF_WRITE_BYTES_STAGING_VID,
    NVC0_QUERY_DRV_STAT_BUF_WRITE_BYTES_STAGING_SYS,
    NVC0_QUERY_DRV_STAT_BUF_COPY_BYTES,
    NVC0_QUERY_DRV_STAT_BUF_NON_KERNEL_FENCE_SYNC_COUNT,
    NVC0_QUERY_DRV_STAT_ANY_NON_KERNEL_FENCE_SYNC_COUNT,
    NVC0_QUERY_DRV_STAT_QUERY_SYNC_COUNT,
    NVC0_QUERY_DRV_STAT_GPU_SERIALIZE_COUNT,
    NVC0_QUERY_DRV_STAT_DRAW_CALLS_ARRAY,
    NVC0_QUERY_DRV_STAT_DRAW_CALLS_INDEXED,
    NVC0_QUERY_DRV_STAT_DRAW_CALLS_FALLBACK_COUNT,
    NVC0_QUERY_DRV_STAT_USER_BUFFER_UPLOAD_BYTES,
    NVC0_QUERY_DRV_STAT_CONSTBUF_UPLOAD_COUNT,
    NVC0_QUERY_DRV_STAT_CONSTBUF_UPLOAD_BYTES,
    NVC0_QUERY_DRV_STAT_PUSHBUF_COUNT,
    NVC0_QUERY_DRV_STAT_RESOURCE_VALIDATE_COUNT,
#endif
    NVC0_QUERY_DRV_STAT_COUNT
};

int nvc0_screen_get_driver_query_info(struct pipe_screen *, unsigned,
                                      struct pipe_driver_query_info *);

int nvc0_screen_get_driver_query_group_info(struct pipe_screen *, unsigned,
                                            struct pipe_driver_query_group_info *);

bool nvc0_blitter_create(struct nvc0_screen *);
void nvc0_blitter_destroy(struct nvc0_screen *);

void nvc0_screen_make_buffers_resident(struct nvc0_screen *);

int nvc0_screen_tic_alloc(struct nvc0_screen *, void *);
int nvc0_screen_tsc_alloc(struct nvc0_screen *, void *);

int nve4_screen_compute_setup(struct nvc0_screen *, struct nouveau_pushbuf *);
int nvc0_screen_compute_setup(struct nvc0_screen *, struct nouveau_pushbuf *);

bool nvc0_screen_resize_tls_area(struct nvc0_screen *, uint32_t lpos,
                                 uint32_t lneg, uint32_t cstack);

static inline void
nvc0_resource_fence(struct nv04_resource *res, uint32_t flags)
{
   struct nvc0_screen *screen = nvc0_screen(res->base.screen);

   if (res->mm) {
      nouveau_fence_ref(screen->base.fence.current, &res->fence);
      if (flags & NOUVEAU_BO_WR)
         nouveau_fence_ref(screen->base.fence.current, &res->fence_wr);
   }
}

static inline void
nvc0_resource_validate(struct nv04_resource *res, uint32_t flags)
{
   if (likely(res->bo)) {
      if (flags & NOUVEAU_BO_WR)
         res->status |= NOUVEAU_BUFFER_STATUS_GPU_WRITING |
            NOUVEAU_BUFFER_STATUS_DIRTY;
      if (flags & NOUVEAU_BO_RD)
         res->status |= NOUVEAU_BUFFER_STATUS_GPU_READING;

      nvc0_resource_fence(res, flags);
   }
}

struct nvc0_format {
   uint32_t rt;
   uint32_t tic;
   uint32_t vtx;
   uint32_t usage;
};

extern const struct nvc0_format nvc0_format_table[];

static inline void
nvc0_screen_tic_unlock(struct nvc0_screen *screen, struct nv50_tic_entry *tic)
{
   if (tic->id >= 0)
      screen->tic.lock[tic->id / 32] &= ~(1 << (tic->id % 32));
}

static inline void
nvc0_screen_tsc_unlock(struct nvc0_screen *screen, struct nv50_tsc_entry *tsc)
{
   if (tsc->id >= 0)
      screen->tsc.lock[tsc->id / 32] &= ~(1 << (tsc->id % 32));
}

static inline void
nvc0_screen_tic_free(struct nvc0_screen *screen, struct nv50_tic_entry *tic)
{
   if (tic->id >= 0) {
      screen->tic.entries[tic->id] = NULL;
      screen->tic.lock[tic->id / 32] &= ~(1 << (tic->id % 32));
   }
}

static inline void
nvc0_screen_tsc_free(struct nvc0_screen *screen, struct nv50_tsc_entry *tsc)
{
   if (tsc->id >= 0) {
      screen->tsc.entries[tsc->id] = NULL;
      screen->tsc.lock[tsc->id / 32] &= ~(1 << (tsc->id % 32));
   }
}

#endif
