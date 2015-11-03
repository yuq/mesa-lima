#ifndef __NV50_QUERY_HW_H__
#define __NV50_QUERY_HW_H__

#include "nouveau_fence.h"
#include "nouveau_mm.h"

#include "nv50_query.h"

#define NVA0_HW_QUERY_STREAM_OUTPUT_BUFFER_OFFSET (PIPE_QUERY_TYPES + 0)

struct nv50_hw_query {
   struct nv50_query base;
   uint32_t *data;
   uint32_t sequence;
   struct nouveau_bo *bo;
   uint32_t base_offset;
   uint32_t offset; /* base + i * rotate */
   uint8_t state;
   bool is64bit;
   uint8_t rotate;
   int nesting; /* only used for occlusion queries */
   struct nouveau_mm_allocation *mm;
   struct nouveau_fence *fence;
};

static inline struct nv50_hw_query *
nv50_hw_query(struct nv50_query *q)
{
   return (struct nv50_hw_query *)q;
}

struct nv50_query *
nv50_hw_create_query(struct nv50_context *, unsigned, unsigned);
void
nv50_hw_query_pushbuf_submit(struct nouveau_pushbuf *, uint16_t,
                             struct nv50_query *, unsigned);
void
nv84_hw_query_fifo_wait(struct nouveau_pushbuf *, struct nv50_query *);

#endif
