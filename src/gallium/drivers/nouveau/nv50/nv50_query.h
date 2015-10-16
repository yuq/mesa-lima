#ifndef __NV50_QUERY_H__
#define __NV50_QUERY_H__

#include "pipe/p_context.h"

#include "nouveau_context.h"
#include "nouveau_mm.h"

#define NVA0_QUERY_STREAM_OUTPUT_BUFFER_OFFSET (PIPE_QUERY_TYPES + 0)

struct nv50_query {
   uint32_t *data;
   uint16_t type;
   uint16_t index;
   uint32_t sequence;
   struct nouveau_bo *bo;
   uint32_t base;
   uint32_t offset; /* base + i * 32 */
   uint8_t state;
   bool is64bit;
   int nesting; /* only used for occlusion queries */
   struct nouveau_mm_allocation *mm;
   struct nouveau_fence *fence;
};

static inline struct nv50_query *
nv50_query(struct pipe_query *pipe)
{
   return (struct nv50_query *)pipe;
}

void nv50_init_query_functions(struct nv50_context *);
void nv50_query_pushbuf_submit(struct nouveau_pushbuf *, uint16_t,
                               struct nv50_query *, unsigned result_offset);
void nv84_query_fifo_wait(struct nouveau_pushbuf *, struct nv50_query *);
void nva0_so_target_save_offset(struct pipe_context *,
                                struct pipe_stream_output_target *,
                                unsigned, bool);

#endif
