/*
 * Copyright (C) 2017 Lima Project
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifndef H_DRM_LIMA_PRIV
#define H_DRM_LIMA_PRIV

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "util/list.h"
#include "util/u_hash_table.h"

#define LIMA_PAGE_SIZE 4096

#define VOID2U64(x) ((uint64_t)(unsigned long)(x))

struct lima_va_hole {
   struct list_head list;
   uint64_t offset;
   uint64_t size;
};

struct lima_va_mgr {
   pthread_mutex_t lock;
   struct list_head va_holes;
};

struct lima_device {
   int fd;
   struct lima_va_mgr vamgr;

   pthread_mutex_t bo_table_mutex;
   struct util_hash_table *bo_handles;
   struct util_hash_table *bo_flink_names;
};

struct lima_bo {
   int refcnt;
   struct lima_device *dev;

   uint32_t size;
   uint32_t handle;
   uint64_t offset;
   void *map;
   uint32_t flink_name;
};

struct lima_submit {
   struct lima_device *dev;
   uint32_t pipe;
   uint32_t fence;

   struct drm_lima_gem_submit_bo *bos;
   uint32_t max_bos;
   uint32_t nr_bos;

   void *frame;
   uint32_t frame_size;
};

int lima_vamgr_init(struct lima_va_mgr *mgr);
void lima_vamgr_fini(struct lima_va_mgr *mgr);
int lima_get_absolute_timeout(uint64_t *timeout, bool relative);

#endif /* H_DRM_LIMA_PRIV */
