/**************************************************************************
 *
 * Copyright © 2007 Red Hat Inc.
 * Copyright © 2007-2012 Intel Corporation
 * Copyright 2006 Tungsten Graphics, Inc., Bismarck, ND., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellström <thomas-at-tungstengraphics-dot-com>
 *          Keith Whitwell <keithw-at-tungstengraphics-dot-com>
 *	    Eric Anholt <eric@anholt.net>
 *	    Dave Airlie <airlied@linux.ie>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xf86drm.h>
#include <util/u_atomic.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>

#include "errno.h"
#ifndef ETIME
#define ETIME ETIMEDOUT
#endif
#include "common/gen_debug.h"
#include "common/gen_device_info.h"
#include "libdrm_macros.h"
#include "main/macros.h"
#include "util/macros.h"
#include "util/hash_table.h"
#include "util/list.h"
#include "brw_bufmgr.h"
#include "string.h"

#include "i915_drm.h"

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#include <memcheck.h>
#define VG(x) x
#else
#define VG(x)
#endif

#define memclear(s) memset(&s, 0, sizeof(s))

#define FILE_DEBUG_FLAG DEBUG_BUFMGR

static inline int
atomic_add_unless(int *v, int add, int unless)
{
   int c, old;
   c = p_atomic_read(v);
   while (c != unless && (old = p_atomic_cmpxchg(v, c, c + add)) != c)
      c = old;
   return c == unless;
}

struct _drm_bacon_context {
	unsigned int ctx_id;
	struct _drm_bacon_bufmgr *bufmgr;
};

struct bo_cache_bucket {
	struct list_head head;
	unsigned long size;
};

typedef struct _drm_bacon_bufmgr {
	int fd;

	pthread_mutex_t lock;

	/** Array of lists of cached gem objects of power-of-two sizes */
	struct bo_cache_bucket cache_bucket[14 * 4];
	int num_buckets;
	time_t time;

	struct hash_table *name_table;
	struct hash_table *handle_table;

	struct list_head vma_cache;
	int vma_count, vma_open, vma_max;

	unsigned int has_llc : 1;
	unsigned int bo_reuse : 1;
} drm_bacon_bufmgr;

static int
bo_set_tiling_internal(drm_bacon_bo *bo, uint32_t tiling_mode, uint32_t stride);

static void bo_free(drm_bacon_bo *bo);

static uint32_t
key_hash_uint(const void *key)
{
	return _mesa_hash_data(key, 4);
}

static bool
key_uint_equal(const void *a, const void *b)
{
	return *((unsigned *) a) == *((unsigned *) b);
}

static drm_bacon_bo *
hash_find_bo(struct hash_table *ht, unsigned int key)
{
	struct hash_entry *entry = _mesa_hash_table_search(ht, &key);
	return entry ? (drm_bacon_bo *) entry->data : NULL;
}

static unsigned long
bo_tile_size(drm_bacon_bufmgr *bufmgr, unsigned long size,
	     uint32_t *tiling_mode)
{
	if (*tiling_mode == I915_TILING_NONE)
		return size;

	/* 965+ just need multiples of page size for tiling */
	return ALIGN(size, 4096);
}

/*
 * Round a given pitch up to the minimum required for X tiling on a
 * given chip.  We use 512 as the minimum to allow for a later tiling
 * change.
 */
static unsigned long
bo_tile_pitch(drm_bacon_bufmgr *bufmgr,
	      unsigned long pitch, uint32_t *tiling_mode)
{
	unsigned long tile_width;

	/* If untiled, then just align it so that we can do rendering
	 * to it with the 3D engine.
	 */
	if (*tiling_mode == I915_TILING_NONE)
		return ALIGN(pitch, 64);

	if (*tiling_mode == I915_TILING_X)
		tile_width = 512;
	else
		tile_width = 128;

	/* 965 is flexible */
	return ALIGN(pitch, tile_width);
}

static struct bo_cache_bucket *
bucket_for_size(drm_bacon_bufmgr *bufmgr, unsigned long size)
{
	int i;

	for (i = 0; i < bufmgr->num_buckets; i++) {
		struct bo_cache_bucket *bucket =
		    &bufmgr->cache_bucket[i];
		if (bucket->size >= size) {
			return bucket;
		}
	}

	return NULL;
}

inline void
drm_bacon_bo_reference(drm_bacon_bo *bo)
{
	p_atomic_inc(&bo->refcount);
}

int
drm_bacon_bo_busy(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	struct drm_i915_gem_busy busy;
	int ret;

	memclear(busy);
	busy.handle = bo->gem_handle;

	ret = drmIoctl(bufmgr->fd, DRM_IOCTL_I915_GEM_BUSY, &busy);
	if (ret == 0) {
		bo->idle = !busy.busy;
		return busy.busy;
	} else {
		return false;
	}
	return (ret == 0 && busy.busy);
}

int
drm_bacon_bo_madvise(drm_bacon_bo *bo, int state)
{
	struct drm_i915_gem_madvise madv;

	memclear(madv);
	madv.handle = bo->gem_handle;
	madv.madv = state;
	madv.retained = 1;
	drmIoctl(bo->bufmgr->fd, DRM_IOCTL_I915_GEM_MADVISE, &madv);

	return madv.retained;
}

/* drop the oldest entries that have been purged by the kernel */
static void
drm_bacon_gem_bo_cache_purge_bucket(drm_bacon_bufmgr *bufmgr,
				    struct bo_cache_bucket *bucket)
{
	while (!list_empty(&bucket->head)) {
		drm_bacon_bo *bo;

		bo = LIST_ENTRY(drm_bacon_bo, bucket->head.next, head);
		if (drm_bacon_bo_madvise(bo, I915_MADV_DONTNEED))
			break;

		list_del(&bo->head);
		bo_free(bo);
	}
}

static drm_bacon_bo *
bo_alloc_internal(drm_bacon_bufmgr *bufmgr,
		  const char *name,
		  unsigned long size,
		  unsigned long flags,
		  uint32_t tiling_mode,
		  unsigned long stride,
		  unsigned int alignment)
{
	drm_bacon_bo *bo;
	unsigned int page_size = getpagesize();
	int ret;
	struct bo_cache_bucket *bucket;
	bool alloc_from_cache;
	unsigned long bo_size;
	bool for_render = false;

	if (flags & BO_ALLOC_FOR_RENDER)
		for_render = true;

	/* Round the allocated size up to a power of two number of pages. */
	bucket = bucket_for_size(bufmgr, size);

	/* If we don't have caching at this size, don't actually round the
	 * allocation up.
	 */
	if (bucket == NULL) {
		bo_size = size;
		if (bo_size < page_size)
			bo_size = page_size;
	} else {
		bo_size = bucket->size;
	}

	pthread_mutex_lock(&bufmgr->lock);
	/* Get a buffer out of the cache if available */
retry:
	alloc_from_cache = false;
	if (bucket != NULL && !list_empty(&bucket->head)) {
		if (for_render) {
			/* Allocate new render-target BOs from the tail (MRU)
			 * of the list, as it will likely be hot in the GPU
			 * cache and in the aperture for us.
			 */
			bo = LIST_ENTRY(drm_bacon_bo, bucket->head.prev, head);
			list_del(&bo->head);
			alloc_from_cache = true;
			bo->align = alignment;
		} else {
			assert(alignment == 0);
			/* For non-render-target BOs (where we're probably
			 * going to map it first thing in order to fill it
			 * with data), check if the last BO in the cache is
			 * unbusy, and only reuse in that case. Otherwise,
			 * allocating a new buffer is probably faster than
			 * waiting for the GPU to finish.
			 */
			bo = LIST_ENTRY(drm_bacon_bo, bucket->head.next, head);
			if (!drm_bacon_bo_busy(bo)) {
				alloc_from_cache = true;
				list_del(&bo->head);
			}
		}

		if (alloc_from_cache) {
			if (!drm_bacon_bo_madvise(bo, I915_MADV_WILLNEED)) {
				bo_free(bo);
				drm_bacon_gem_bo_cache_purge_bucket(bufmgr,
								    bucket);
				goto retry;
			}

			if (bo_set_tiling_internal(bo, tiling_mode, stride)) {
				bo_free(bo);
				goto retry;
			}
		}
	}

	if (!alloc_from_cache) {
		struct drm_i915_gem_create create;

		bo = calloc(1, sizeof(*bo));
		if (!bo)
			goto err;

		/* bo_free calls list_del() for an uninitialized
		   list (vma_list), so better set the list head here */
		list_inithead(&bo->vma_list);

		bo->size = bo_size;

		memclear(create);
		create.size = bo_size;

		ret = drmIoctl(bufmgr->fd,
			       DRM_IOCTL_I915_GEM_CREATE,
			       &create);
		if (ret != 0) {
			free(bo);
			goto err;
		}

		bo->gem_handle = create.handle;
		_mesa_hash_table_insert(bufmgr->handle_table,
					&bo->gem_handle, bo);

		bo->bufmgr = bufmgr;
		bo->align = alignment;

		bo->tiling_mode = I915_TILING_NONE;
		bo->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		bo->stride = 0;

		if (bo_set_tiling_internal(bo, tiling_mode, stride))
			goto err_free;
	}

	bo->name = name;
	p_atomic_set(&bo->refcount, 1);
	bo->reusable = true;

	pthread_mutex_unlock(&bufmgr->lock);

	DBG("bo_create: buf %d (%s) %ldb\n",
	    bo->gem_handle, bo->name, size);

	return bo;

err_free:
	bo_free(bo);
err:
	pthread_mutex_unlock(&bufmgr->lock);
	return NULL;
}

drm_bacon_bo *
drm_bacon_bo_alloc_for_render(drm_bacon_bufmgr *bufmgr,
			      const char *name,
			      unsigned long size,
			      unsigned int alignment)
{
	return bo_alloc_internal(bufmgr, name, size, BO_ALLOC_FOR_RENDER,
				 I915_TILING_NONE, 0, alignment);
}

drm_bacon_bo *
drm_bacon_bo_alloc(drm_bacon_bufmgr *bufmgr,
		   const char *name,
		   unsigned long size,
		   unsigned int alignment)
{
	return bo_alloc_internal(bufmgr, name, size, 0, I915_TILING_NONE, 0, 0);
}

drm_bacon_bo *
drm_bacon_bo_alloc_tiled(drm_bacon_bufmgr *bufmgr, const char *name,
			 int x, int y, int cpp, uint32_t *tiling_mode,
			 unsigned long *pitch, unsigned long flags)
{
	unsigned long size, stride;
	uint32_t tiling;

	do {
		unsigned long aligned_y, height_alignment;

		tiling = *tiling_mode;

		/* If we're tiled, our allocations are in 8 or 32-row blocks,
		 * so failure to align our height means that we won't allocate
		 * enough pages.
		 *
		 * If we're untiled, we still have to align to 2 rows high
		 * because the data port accesses 2x2 blocks even if the
		 * bottom row isn't to be rendered, so failure to align means
		 * we could walk off the end of the GTT and fault.  This is
		 * documented on 965, and may be the case on older chipsets
		 * too so we try to be careful.
		 */
		aligned_y = y;
		height_alignment = 2;

		if (tiling == I915_TILING_X)
			height_alignment = 8;
		else if (tiling == I915_TILING_Y)
			height_alignment = 32;
		aligned_y = ALIGN(y, height_alignment);

		stride = x * cpp;
		stride = bo_tile_pitch(bufmgr, stride, tiling_mode);
		size = stride * aligned_y;
		size = bo_tile_size(bufmgr, size, tiling_mode);
	} while (*tiling_mode != tiling);
	*pitch = stride;

	if (tiling == I915_TILING_NONE)
		stride = 0;

	return bo_alloc_internal(bufmgr, name, size, flags, tiling, stride, 0);
}

/**
 * Returns a drm_bacon_bo wrapping the given buffer object handle.
 *
 * This can be used when one application needs to pass a buffer object
 * to another.
 */
drm_bacon_bo *
drm_bacon_bo_gem_create_from_name(drm_bacon_bufmgr *bufmgr,
				  const char *name,
				  unsigned int handle)
{
	drm_bacon_bo *bo;
	int ret;
	struct drm_gem_open open_arg;
	struct drm_i915_gem_get_tiling get_tiling;

	/* At the moment most applications only have a few named bo.
	 * For instance, in a DRI client only the render buffers passed
	 * between X and the client are named. And since X returns the
	 * alternating names for the front/back buffer a linear search
	 * provides a sufficiently fast match.
	 */
	pthread_mutex_lock(&bufmgr->lock);
	bo = hash_find_bo(bufmgr->name_table, handle);
	if (bo) {
		drm_bacon_bo_reference(bo);
		goto out;
	}

	memclear(open_arg);
	open_arg.name = handle;
	ret = drmIoctl(bufmgr->fd,
		       DRM_IOCTL_GEM_OPEN,
		       &open_arg);
	if (ret != 0) {
		DBG("Couldn't reference %s handle 0x%08x: %s\n",
		    name, handle, strerror(errno));
		bo = NULL;
		goto out;
	}
        /* Now see if someone has used a prime handle to get this
         * object from the kernel before by looking through the list
         * again for a matching gem_handle
         */
	bo = hash_find_bo(bufmgr->handle_table, open_arg.handle);
	if (bo) {
		drm_bacon_bo_reference(bo);
		goto out;
	}

	bo = calloc(1, sizeof(*bo));
	if (!bo)
		goto out;

	p_atomic_set(&bo->refcount, 1);
	list_inithead(&bo->vma_list);

	bo->size = open_arg.size;
	bo->offset64 = 0;
	bo->virtual = NULL;
	bo->bufmgr = bufmgr;
	bo->gem_handle = open_arg.handle;
	bo->name = name;
	bo->global_name = handle;
	bo->reusable = false;

	_mesa_hash_table_insert(bufmgr->handle_table, &bo->gem_handle, bo);
	_mesa_hash_table_insert(bufmgr->name_table, &bo->global_name, bo);

	memclear(get_tiling);
	get_tiling.handle = bo->gem_handle;
	ret = drmIoctl(bufmgr->fd,
		       DRM_IOCTL_I915_GEM_GET_TILING,
		       &get_tiling);
	if (ret != 0)
		goto err_unref;

	bo->tiling_mode = get_tiling.tiling_mode;
	bo->swizzle_mode = get_tiling.swizzle_mode;
	/* XXX stride is unknown */
	DBG("bo_create_from_handle: %d (%s)\n", handle, bo->name);

out:
	pthread_mutex_unlock(&bufmgr->lock);
	return bo;

err_unref:
	bo_free(bo);
	pthread_mutex_unlock(&bufmgr->lock);
	return NULL;
}

static void
bo_free(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	struct drm_gem_close close;
	struct hash_entry *entry;
	int ret;

	list_del(&bo->vma_list);
	if (bo->mem_virtual) {
		VG(VALGRIND_FREELIKE_BLOCK(bo->mem_virtual, 0));
		drm_munmap(bo->mem_virtual, bo->size);
		bufmgr->vma_count--;
	}
	if (bo->wc_virtual) {
		VG(VALGRIND_FREELIKE_BLOCK(bo->wc_virtual, 0));
		drm_munmap(bo->wc_virtual, bo->size);
		bufmgr->vma_count--;
	}
	if (bo->gtt_virtual) {
		drm_munmap(bo->gtt_virtual, bo->size);
		bufmgr->vma_count--;
	}

	if (bo->global_name) {
		entry = _mesa_hash_table_search(bufmgr->name_table,
						&bo->global_name);
		_mesa_hash_table_remove(bufmgr->name_table, entry);
	}
	entry = _mesa_hash_table_search(bufmgr->handle_table,
					&bo->gem_handle);
	_mesa_hash_table_remove(bufmgr->handle_table, entry);

	/* Close this object */
	memclear(close);
	close.handle = bo->gem_handle;
	ret = drmIoctl(bufmgr->fd, DRM_IOCTL_GEM_CLOSE, &close);
	if (ret != 0) {
		DBG("DRM_IOCTL_GEM_CLOSE %d failed (%s): %s\n",
		    bo->gem_handle, bo->name, strerror(errno));
	}
	free(bo);
}

static void
bo_mark_mmaps_incoherent(drm_bacon_bo *bo)
{
#if HAVE_VALGRIND
	if (bo->mem_virtual)
		VALGRIND_MAKE_MEM_NOACCESS(bo->mem_virtual, bo->size);

	if (bo->wc_virtual)
		VALGRIND_MAKE_MEM_NOACCESS(bo->wc_virtual, bo->size);

	if (bo->gtt_virtual)
		VALGRIND_MAKE_MEM_NOACCESS(bo->gtt_virtual, bo->size);
#endif
}

/** Frees all cached buffers significantly older than @time. */
static void
cleanup_bo_cache(drm_bacon_bufmgr *bufmgr, time_t time)
{
	int i;

	if (bufmgr->time == time)
		return;

	for (i = 0; i < bufmgr->num_buckets; i++) {
		struct bo_cache_bucket *bucket =
		    &bufmgr->cache_bucket[i];

		while (!list_empty(&bucket->head)) {
			drm_bacon_bo *bo;

			bo = LIST_ENTRY(drm_bacon_bo, bucket->head.next, head);
			if (time - bo->free_time <= 1)
				break;

			list_del(&bo->head);

			bo_free(bo);
		}
	}

	bufmgr->time = time;
}

static void
bo_purge_vma_cache(drm_bacon_bufmgr *bufmgr)
{
	int limit;

	DBG("%s: cached=%d, open=%d, limit=%d\n", __FUNCTION__,
	    bufmgr->vma_count, bufmgr->vma_open, bufmgr->vma_max);

	if (bufmgr->vma_max < 0)
		return;

	/* We may need to evict a few entries in order to create new mmaps */
	limit = bufmgr->vma_max - 2*bufmgr->vma_open;
	if (limit < 0)
		limit = 0;

	while (bufmgr->vma_count > limit) {
		drm_bacon_bo *bo;

		bo = LIST_ENTRY(drm_bacon_bo, bufmgr->vma_cache.next, vma_list);
		assert(bo->map_count == 0);
		list_delinit(&bo->vma_list);

		if (bo->mem_virtual) {
			drm_munmap(bo->mem_virtual, bo->size);
			bo->mem_virtual = NULL;
			bufmgr->vma_count--;
		}
		if (bo->wc_virtual) {
			drm_munmap(bo->wc_virtual, bo->size);
			bo->wc_virtual = NULL;
			bufmgr->vma_count--;
		}
		if (bo->gtt_virtual) {
			drm_munmap(bo->gtt_virtual, bo->size);
			bo->gtt_virtual = NULL;
			bufmgr->vma_count--;
		}
	}
}

static void
bo_close_vma(drm_bacon_bufmgr *bufmgr, drm_bacon_bo *bo)
{
	bufmgr->vma_open--;
	list_addtail(&bo->vma_list, &bufmgr->vma_cache);
	if (bo->mem_virtual)
		bufmgr->vma_count++;
	if (bo->wc_virtual)
		bufmgr->vma_count++;
	if (bo->gtt_virtual)
		bufmgr->vma_count++;
	bo_purge_vma_cache(bufmgr);
}

static void
bo_open_vma(drm_bacon_bufmgr *bufmgr, drm_bacon_bo *bo)
{
	bufmgr->vma_open++;
	list_del(&bo->vma_list);
	if (bo->mem_virtual)
		bufmgr->vma_count--;
	if (bo->wc_virtual)
		bufmgr->vma_count--;
	if (bo->gtt_virtual)
		bufmgr->vma_count--;
	bo_purge_vma_cache(bufmgr);
}

static void
bo_unreference_final(drm_bacon_bo *bo, time_t time)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	struct bo_cache_bucket *bucket;

	DBG("bo_unreference final: %d (%s)\n",
	    bo->gem_handle, bo->name);

	/* Clear any left-over mappings */
	if (bo->map_count) {
		DBG("bo freed with non-zero map-count %d\n", bo->map_count);
		bo->map_count = 0;
		bo_close_vma(bufmgr, bo);
		bo_mark_mmaps_incoherent(bo);
	}

	bucket = bucket_for_size(bufmgr, bo->size);
	/* Put the buffer into our internal cache for reuse if we can. */
	if (bufmgr->bo_reuse && bo->reusable && bucket != NULL &&
	    drm_bacon_bo_madvise(bo, I915_MADV_DONTNEED)) {
		bo->free_time = time;

		bo->name = NULL;

		list_addtail(&bo->head, &bucket->head);
	} else {
		bo_free(bo);
	}
}

void
drm_bacon_bo_unreference(drm_bacon_bo *bo)
{
	if (bo == NULL)
		return;

	assert(p_atomic_read(&bo->refcount) > 0);

	if (atomic_add_unless(&bo->refcount, -1, 1)) {
		drm_bacon_bufmgr *bufmgr = bo->bufmgr;
		struct timespec time;

		clock_gettime(CLOCK_MONOTONIC, &time);

		pthread_mutex_lock(&bufmgr->lock);

		if (p_atomic_dec_zero(&bo->refcount)) {
			bo_unreference_final(bo, time.tv_sec);
			cleanup_bo_cache(bufmgr, time.tv_sec);
		}

		pthread_mutex_unlock(&bufmgr->lock);
	}
}

int
drm_bacon_bo_map(drm_bacon_bo *bo, int write_enable)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	struct drm_i915_gem_set_domain set_domain;
	int ret;

	pthread_mutex_lock(&bufmgr->lock);

	if (bo->map_count++ == 0)
		bo_open_vma(bufmgr, bo);

	if (!bo->mem_virtual) {
		struct drm_i915_gem_mmap mmap_arg;

		DBG("bo_map: %d (%s), map_count=%d\n",
		    bo->gem_handle, bo->name, bo->map_count);

		memclear(mmap_arg);
		mmap_arg.handle = bo->gem_handle;
		mmap_arg.size = bo->size;
		ret = drmIoctl(bufmgr->fd,
			       DRM_IOCTL_I915_GEM_MMAP,
			       &mmap_arg);
		if (ret != 0) {
			ret = -errno;
			DBG("%s:%d: Error mapping buffer %d (%s): %s .\n",
			    __FILE__, __LINE__, bo->gem_handle,
			    bo->name, strerror(errno));
			if (--bo->map_count == 0)
				bo_close_vma(bufmgr, bo);
			pthread_mutex_unlock(&bufmgr->lock);
			return ret;
		}
		VG(VALGRIND_MALLOCLIKE_BLOCK(mmap_arg.addr_ptr, mmap_arg.size, 0, 1));
		bo->mem_virtual = (void *)(uintptr_t) mmap_arg.addr_ptr;
	}
	DBG("bo_map: %d (%s) -> %p\n", bo->gem_handle, bo->name,
	    bo->mem_virtual);
	bo->virtual = bo->mem_virtual;

	memclear(set_domain);
	set_domain.handle = bo->gem_handle;
	set_domain.read_domains = I915_GEM_DOMAIN_CPU;
	if (write_enable)
		set_domain.write_domain = I915_GEM_DOMAIN_CPU;
	else
		set_domain.write_domain = 0;
	ret = drmIoctl(bufmgr->fd,
		       DRM_IOCTL_I915_GEM_SET_DOMAIN,
		       &set_domain);
	if (ret != 0) {
		DBG("%s:%d: Error setting to CPU domain %d: %s\n",
		    __FILE__, __LINE__, bo->gem_handle,
		    strerror(errno));
	}

	bo_mark_mmaps_incoherent(bo);
	VG(VALGRIND_MAKE_MEM_DEFINED(bo->mem_virtual, bo->size));
	pthread_mutex_unlock(&bufmgr->lock);

	return 0;
}

static int
map_gtt(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	int ret;

	if (bo->map_count++ == 0)
		bo_open_vma(bufmgr, bo);

	/* Get a mapping of the buffer if we haven't before. */
	if (bo->gtt_virtual == NULL) {
		struct drm_i915_gem_mmap_gtt mmap_arg;

		DBG("bo_map_gtt: mmap %d (%s), map_count=%d\n",
		    bo->gem_handle, bo->name, bo->map_count);

		memclear(mmap_arg);
		mmap_arg.handle = bo->gem_handle;

		/* Get the fake offset back... */
		ret = drmIoctl(bufmgr->fd,
			       DRM_IOCTL_I915_GEM_MMAP_GTT,
			       &mmap_arg);
		if (ret != 0) {
			ret = -errno;
			DBG("%s:%d: Error preparing buffer map %d (%s): %s .\n",
			    __FILE__, __LINE__,
			    bo->gem_handle, bo->name,
			    strerror(errno));
			if (--bo->map_count == 0)
				bo_close_vma(bufmgr, bo);
			return ret;
		}

		/* and mmap it */
		bo->gtt_virtual = drm_mmap(0, bo->size, PROT_READ | PROT_WRITE,
					       MAP_SHARED, bufmgr->fd,
					       mmap_arg.offset);
		if (bo->gtt_virtual == MAP_FAILED) {
			bo->gtt_virtual = NULL;
			ret = -errno;
			DBG("%s:%d: Error mapping buffer %d (%s): %s .\n",
			    __FILE__, __LINE__,
			    bo->gem_handle, bo->name,
			    strerror(errno));
			if (--bo->map_count == 0)
				bo_close_vma(bufmgr, bo);
			return ret;
		}
	}

	bo->virtual = bo->gtt_virtual;

	DBG("bo_map_gtt: %d (%s) -> %p\n", bo->gem_handle, bo->name,
	    bo->gtt_virtual);

	return 0;
}

int
drm_bacon_gem_bo_map_gtt(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	struct drm_i915_gem_set_domain set_domain;
	int ret;

	pthread_mutex_lock(&bufmgr->lock);

	ret = map_gtt(bo);
	if (ret) {
		pthread_mutex_unlock(&bufmgr->lock);
		return ret;
	}

	/* Now move it to the GTT domain so that the GPU and CPU
	 * caches are flushed and the GPU isn't actively using the
	 * buffer.
	 *
	 * The pagefault handler does this domain change for us when
	 * it has unbound the BO from the GTT, but it's up to us to
	 * tell it when we're about to use things if we had done
	 * rendering and it still happens to be bound to the GTT.
	 */
	memclear(set_domain);
	set_domain.handle = bo->gem_handle;
	set_domain.read_domains = I915_GEM_DOMAIN_GTT;
	set_domain.write_domain = I915_GEM_DOMAIN_GTT;
	ret = drmIoctl(bufmgr->fd,
		       DRM_IOCTL_I915_GEM_SET_DOMAIN,
		       &set_domain);
	if (ret != 0) {
		DBG("%s:%d: Error setting domain %d: %s\n",
		    __FILE__, __LINE__, bo->gem_handle,
		    strerror(errno));
	}

	bo_mark_mmaps_incoherent(bo);
	VG(VALGRIND_MAKE_MEM_DEFINED(bo->gtt_virtual, bo->size));
	pthread_mutex_unlock(&bufmgr->lock);

	return 0;
}

/**
 * Performs a mapping of the buffer object like the normal GTT
 * mapping, but avoids waiting for the GPU to be done reading from or
 * rendering to the buffer.
 *
 * This is used in the implementation of GL_ARB_map_buffer_range: The
 * user asks to create a buffer, then does a mapping, fills some
 * space, runs a drawing command, then asks to map it again without
 * synchronizing because it guarantees that it won't write over the
 * data that the GPU is busy using (or, more specifically, that if it
 * does write over the data, it acknowledges that rendering is
 * undefined).
 */

int
drm_bacon_gem_bo_map_unsynchronized(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	int ret;

	/* If the CPU cache isn't coherent with the GTT, then use a
	 * regular synchronized mapping.  The problem is that we don't
	 * track where the buffer was last used on the CPU side in
	 * terms of drm_bacon_bo_map vs drm_bacon_gem_bo_map_gtt, so
	 * we would potentially corrupt the buffer even when the user
	 * does reasonable things.
	 */
	if (!bufmgr->has_llc)
		return drm_bacon_gem_bo_map_gtt(bo);

	pthread_mutex_lock(&bufmgr->lock);

	ret = map_gtt(bo);
	if (ret == 0) {
		bo_mark_mmaps_incoherent(bo);
		VG(VALGRIND_MAKE_MEM_DEFINED(bo->gtt_virtual, bo->size));
	}

	pthread_mutex_unlock(&bufmgr->lock);

	return ret;
}

int
drm_bacon_bo_unmap(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	int ret = 0;

	if (bo == NULL)
		return 0;

	pthread_mutex_lock(&bufmgr->lock);

	if (bo->map_count <= 0) {
		DBG("attempted to unmap an unmapped bo\n");
		pthread_mutex_unlock(&bufmgr->lock);
		/* Preserve the old behaviour of just treating this as a
		 * no-op rather than reporting the error.
		 */
		return 0;
	}

	/* We need to unmap after every innovation as we cannot track
	 * an open vma for every bo as that will exhaust the system
	 * limits and cause later failures.
	 */
	if (--bo->map_count == 0) {
		bo_close_vma(bufmgr, bo);
		bo_mark_mmaps_incoherent(bo);
		bo->virtual = NULL;
	}
	pthread_mutex_unlock(&bufmgr->lock);

	return ret;
}

int
drm_bacon_bo_subdata(drm_bacon_bo *bo, unsigned long offset,
		     unsigned long size, const void *data)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	struct drm_i915_gem_pwrite pwrite;
	int ret;

	memclear(pwrite);
	pwrite.handle = bo->gem_handle;
	pwrite.offset = offset;
	pwrite.size = size;
	pwrite.data_ptr = (uint64_t) (uintptr_t) data;
	ret = drmIoctl(bufmgr->fd,
		       DRM_IOCTL_I915_GEM_PWRITE,
		       &pwrite);
	if (ret != 0) {
		ret = -errno;
		DBG("%s:%d: Error writing data to buffer %d: (%d %d) %s .\n",
		    __FILE__, __LINE__, bo->gem_handle, (int)offset,
		    (int)size, strerror(errno));
	}

	return ret;
}

int
drm_bacon_bo_get_subdata(drm_bacon_bo *bo, unsigned long offset,
			 unsigned long size, void *data)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	struct drm_i915_gem_pread pread;
	int ret;

	memclear(pread);
	pread.handle = bo->gem_handle;
	pread.offset = offset;
	pread.size = size;
	pread.data_ptr = (uint64_t) (uintptr_t) data;
	ret = drmIoctl(bufmgr->fd,
		       DRM_IOCTL_I915_GEM_PREAD,
		       &pread);
	if (ret != 0) {
		ret = -errno;
		DBG("%s:%d: Error reading data from buffer %d: (%d %d) %s .\n",
		    __FILE__, __LINE__, bo->gem_handle, (int)offset,
		    (int)size, strerror(errno));
	}

	return ret;
}

/** Waits for all GPU rendering with the object to have completed. */
void
drm_bacon_bo_wait_rendering(drm_bacon_bo *bo)
{
	drm_bacon_gem_bo_start_gtt_access(bo, 1);
}

/**
 * Waits on a BO for the given amount of time.
 *
 * @bo: buffer object to wait for
 * @timeout_ns: amount of time to wait in nanoseconds.
 *   If value is less than 0, an infinite wait will occur.
 *
 * Returns 0 if the wait was successful ie. the last batch referencing the
 * object has completed within the allotted time. Otherwise some negative return
 * value describes the error. Of particular interest is -ETIME when the wait has
 * failed to yield the desired result.
 *
 * Similar to drm_bacon_gem_bo_wait_rendering except a timeout parameter allows
 * the operation to give up after a certain amount of time. Another subtle
 * difference is the internal locking semantics are different (this variant does
 * not hold the lock for the duration of the wait). This makes the wait subject
 * to a larger userspace race window.
 *
 * The implementation shall wait until the object is no longer actively
 * referenced within a batch buffer at the time of the call. The wait will
 * not guarantee that the buffer is re-issued via another thread, or an flinked
 * handle. Userspace must make sure this race does not occur if such precision
 * is important.
 *
 * Note that some kernels have broken the inifite wait for negative values
 * promise, upgrade to latest stable kernels if this is the case.
 */
int
drm_bacon_gem_bo_wait(drm_bacon_bo *bo, int64_t timeout_ns)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	struct drm_i915_gem_wait wait;
	int ret;

	memclear(wait);
	wait.bo_handle = bo->gem_handle;
	wait.timeout_ns = timeout_ns;
	ret = drmIoctl(bufmgr->fd, DRM_IOCTL_I915_GEM_WAIT, &wait);
	if (ret == -1)
		return -errno;

	return ret;
}

/**
 * Sets the object to the GTT read and possibly write domain, used by the X
 * 2D driver in the absence of kernel support to do drm_bacon_gem_bo_map_gtt().
 *
 * In combination with drm_bacon_gem_bo_pin() and manual fence management, we
 * can do tiled pixmaps this way.
 */
void
drm_bacon_gem_bo_start_gtt_access(drm_bacon_bo *bo, int write_enable)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	struct drm_i915_gem_set_domain set_domain;
	int ret;

	memclear(set_domain);
	set_domain.handle = bo->gem_handle;
	set_domain.read_domains = I915_GEM_DOMAIN_GTT;
	set_domain.write_domain = write_enable ? I915_GEM_DOMAIN_GTT : 0;
	ret = drmIoctl(bufmgr->fd,
		       DRM_IOCTL_I915_GEM_SET_DOMAIN,
		       &set_domain);
	if (ret != 0) {
		DBG("%s:%d: Error setting memory domains %d (%08x %08x): %s .\n",
		    __FILE__, __LINE__, bo->gem_handle,
		    set_domain.read_domains, set_domain.write_domain,
		    strerror(errno));
	}
}

void
drm_bacon_bufmgr_destroy(drm_bacon_bufmgr *bufmgr)
{
	pthread_mutex_destroy(&bufmgr->lock);

	/* Free any cached buffer objects we were going to reuse */
	for (int i = 0; i < bufmgr->num_buckets; i++) {
		struct bo_cache_bucket *bucket =
		    &bufmgr->cache_bucket[i];
		drm_bacon_bo *bo;

		while (!list_empty(&bucket->head)) {
			bo = LIST_ENTRY(drm_bacon_bo, bucket->head.next, head);
			list_del(&bo->head);

			bo_free(bo);
		}
	}

	_mesa_hash_table_destroy(bufmgr->name_table, NULL);
	_mesa_hash_table_destroy(bufmgr->handle_table, NULL);

	free(bufmgr);
}

static int
bo_set_tiling_internal(drm_bacon_bo *bo, uint32_t tiling_mode, uint32_t stride)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;
	struct drm_i915_gem_set_tiling set_tiling;
	int ret;

	if (bo->global_name == 0 &&
	    tiling_mode == bo->tiling_mode &&
	    stride == bo->stride)
		return 0;

	memset(&set_tiling, 0, sizeof(set_tiling));
	do {
		/* set_tiling is slightly broken and overwrites the
		 * input on the error path, so we have to open code
		 * rmIoctl.
		 */
		set_tiling.handle = bo->gem_handle;
		set_tiling.tiling_mode = tiling_mode;
		set_tiling.stride = stride;

		ret = ioctl(bufmgr->fd,
			    DRM_IOCTL_I915_GEM_SET_TILING,
			    &set_tiling);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	if (ret == -1)
		return -errno;

	bo->tiling_mode = set_tiling.tiling_mode;
	bo->swizzle_mode = set_tiling.swizzle_mode;
	bo->stride = set_tiling.stride;
	return 0;
}

int
drm_bacon_bo_set_tiling(drm_bacon_bo *bo, uint32_t * tiling_mode,
			uint32_t stride)
{
	int ret;

	/* Linear buffers have no stride. By ensuring that we only ever use
	 * stride 0 with linear buffers, we simplify our code.
	 */
	if (*tiling_mode == I915_TILING_NONE)
		stride = 0;

	ret = bo_set_tiling_internal(bo, *tiling_mode, stride);

	*tiling_mode = bo->tiling_mode;
	return ret;
}

int
drm_bacon_bo_get_tiling(drm_bacon_bo *bo, uint32_t * tiling_mode,
			uint32_t *swizzle_mode)
{
	*tiling_mode = bo->tiling_mode;
	*swizzle_mode = bo->swizzle_mode;
	return 0;
}

drm_bacon_bo *
drm_bacon_bo_gem_create_from_prime(drm_bacon_bufmgr *bufmgr, int prime_fd, int size)
{
	int ret;
	uint32_t handle;
	drm_bacon_bo *bo;
	struct drm_i915_gem_get_tiling get_tiling;

	pthread_mutex_lock(&bufmgr->lock);
	ret = drmPrimeFDToHandle(bufmgr->fd, prime_fd, &handle);
	if (ret) {
		DBG("create_from_prime: failed to obtain handle from fd: %s\n", strerror(errno));
		pthread_mutex_unlock(&bufmgr->lock);
		return NULL;
	}

	/*
	 * See if the kernel has already returned this buffer to us. Just as
	 * for named buffers, we must not create two bo's pointing at the same
	 * kernel object
	 */
	bo = hash_find_bo(bufmgr->handle_table, handle);
	if (bo) {
		drm_bacon_bo_reference(bo);
		goto out;
	}

	bo = calloc(1, sizeof(*bo));
	if (!bo)
		goto out;

	p_atomic_set(&bo->refcount, 1);
	list_inithead(&bo->vma_list);

	/* Determine size of bo.  The fd-to-handle ioctl really should
	 * return the size, but it doesn't.  If we have kernel 3.12 or
	 * later, we can lseek on the prime fd to get the size.  Older
	 * kernels will just fail, in which case we fall back to the
	 * provided (estimated or guess size). */
	ret = lseek(prime_fd, 0, SEEK_END);
	if (ret != -1)
		bo->size = ret;
	else
		bo->size = size;

	bo->bufmgr = bufmgr;

	bo->gem_handle = handle;
	_mesa_hash_table_insert(bufmgr->handle_table,
				&bo->gem_handle, bo);

	bo->name = "prime";
	bo->reusable = false;

	memclear(get_tiling);
	get_tiling.handle = bo->gem_handle;
	if (drmIoctl(bufmgr->fd,
		     DRM_IOCTL_I915_GEM_GET_TILING,
		     &get_tiling))
		goto err;

	bo->tiling_mode = get_tiling.tiling_mode;
	bo->swizzle_mode = get_tiling.swizzle_mode;
	/* XXX stride is unknown */

out:
	pthread_mutex_unlock(&bufmgr->lock);
	return bo;

err:
	bo_free(bo);
	pthread_mutex_unlock(&bufmgr->lock);
	return NULL;
}

int
drm_bacon_bo_gem_export_to_prime(drm_bacon_bo *bo, int *prime_fd)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;

	if (drmPrimeHandleToFD(bufmgr->fd, bo->gem_handle,
			       DRM_CLOEXEC, prime_fd) != 0)
		return -errno;

	bo->reusable = false;

	return 0;
}

int
drm_bacon_bo_flink(drm_bacon_bo *bo, uint32_t *name)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;

	if (!bo->global_name) {
		struct drm_gem_flink flink;

		memclear(flink);
		flink.handle = bo->gem_handle;
		if (drmIoctl(bufmgr->fd, DRM_IOCTL_GEM_FLINK, &flink))
			return -errno;

		pthread_mutex_lock(&bufmgr->lock);
		if (!bo->global_name) {
			bo->global_name = flink.name;
			bo->reusable = false;

			_mesa_hash_table_insert(bufmgr->name_table,
						&bo->global_name, bo);
		}
		pthread_mutex_unlock(&bufmgr->lock);
	}

	*name = bo->global_name;
	return 0;
}

/**
 * Enables unlimited caching of buffer objects for reuse.
 *
 * This is potentially very memory expensive, as the cache at each bucket
 * size is only bounded by how many buffers of that size we've managed to have
 * in flight at once.
 */
void
drm_bacon_bufmgr_gem_enable_reuse(drm_bacon_bufmgr *bufmgr)
{
	bufmgr->bo_reuse = true;
}

/*
 * Disable buffer reuse for objects which are shared with the kernel
 * as scanout buffers
 */
int
drm_bacon_bo_disable_reuse(drm_bacon_bo *bo)
{
	bo->reusable = false;
	return 0;
}

int
drm_bacon_bo_is_reusable(drm_bacon_bo *bo)
{
	return bo->reusable;
}

static void
add_bucket(drm_bacon_bufmgr *bufmgr, int size)
{
	unsigned int i = bufmgr->num_buckets;

	assert(i < ARRAY_SIZE(bufmgr->cache_bucket));

	list_inithead(&bufmgr->cache_bucket[i].head);
	bufmgr->cache_bucket[i].size = size;
	bufmgr->num_buckets++;
}

static void
init_cache_buckets(drm_bacon_bufmgr *bufmgr)
{
	unsigned long size, cache_max_size = 64 * 1024 * 1024;

	/* OK, so power of two buckets was too wasteful of memory.
	 * Give 3 other sizes between each power of two, to hopefully
	 * cover things accurately enough.  (The alternative is
	 * probably to just go for exact matching of sizes, and assume
	 * that for things like composited window resize the tiled
	 * width/height alignment and rounding of sizes to pages will
	 * get us useful cache hit rates anyway)
	 */
	add_bucket(bufmgr, 4096);
	add_bucket(bufmgr, 4096 * 2);
	add_bucket(bufmgr, 4096 * 3);

	/* Initialize the linked lists for BO reuse cache. */
	for (size = 4 * 4096; size <= cache_max_size; size *= 2) {
		add_bucket(bufmgr, size);

		add_bucket(bufmgr, size + size * 1 / 4);
		add_bucket(bufmgr, size + size * 2 / 4);
		add_bucket(bufmgr, size + size * 3 / 4);
	}
}

void
drm_bacon_bufmgr_gem_set_vma_cache_size(drm_bacon_bufmgr *bufmgr, int limit)
{
	bufmgr->vma_max = limit;

	bo_purge_vma_cache(bufmgr);
}

drm_bacon_context *
drm_bacon_gem_context_create(drm_bacon_bufmgr *bufmgr)
{
	struct drm_i915_gem_context_create create;
	drm_bacon_context *context = NULL;
	int ret;

	context = calloc(1, sizeof(*context));
	if (!context)
		return NULL;

	memclear(create);
	ret = drmIoctl(bufmgr->fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &create);
	if (ret != 0) {
		DBG("DRM_IOCTL_I915_GEM_CONTEXT_CREATE failed: %s\n",
		    strerror(errno));
		free(context);
		return NULL;
	}

	context->ctx_id = create.ctx_id;
	context->bufmgr = bufmgr;

	return context;
}

int
drm_bacon_gem_context_get_id(drm_bacon_context *ctx, uint32_t *ctx_id)
{
	if (ctx == NULL)
		return -EINVAL;

	*ctx_id = ctx->ctx_id;

	return 0;
}

void
drm_bacon_gem_context_destroy(drm_bacon_context *ctx)
{
	struct drm_i915_gem_context_destroy destroy;
	int ret;

	if (ctx == NULL)
		return;

	memclear(destroy);

	destroy.ctx_id = ctx->ctx_id;
	ret = drmIoctl(ctx->bufmgr->fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY,
		       &destroy);
	if (ret != 0)
		fprintf(stderr, "DRM_IOCTL_I915_GEM_CONTEXT_DESTROY failed: %s\n",
			strerror(errno));

	free(ctx);
}

int
drm_bacon_get_reset_stats(drm_bacon_context *ctx,
			  uint32_t *reset_count,
			  uint32_t *active,
			  uint32_t *pending)
{
	struct drm_i915_reset_stats stats;
	int ret;

	if (ctx == NULL)
		return -EINVAL;

	memclear(stats);

	stats.ctx_id = ctx->ctx_id;
	ret = drmIoctl(ctx->bufmgr->fd,
		       DRM_IOCTL_I915_GET_RESET_STATS,
		       &stats);
	if (ret == 0) {
		if (reset_count != NULL)
			*reset_count = stats.reset_count;

		if (active != NULL)
			*active = stats.batch_active;

		if (pending != NULL)
			*pending = stats.batch_pending;
	}

	return ret;
}

int
drm_bacon_reg_read(drm_bacon_bufmgr *bufmgr,
		   uint32_t offset,
		   uint64_t *result)
{
	struct drm_i915_reg_read reg_read;
	int ret;

	memclear(reg_read);
	reg_read.offset = offset;

	ret = drmIoctl(bufmgr->fd, DRM_IOCTL_I915_REG_READ, &reg_read);

	*result = reg_read.val;
	return ret;
}

void *drm_bacon_gem_bo_map__gtt(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;

	if (bo->gtt_virtual)
		return bo->gtt_virtual;

	pthread_mutex_lock(&bufmgr->lock);
	if (bo->gtt_virtual == NULL) {
		struct drm_i915_gem_mmap_gtt mmap_arg;
		void *ptr;

		DBG("bo_map_gtt: mmap %d (%s), map_count=%d\n",
		    bo->gem_handle, bo->name, bo->map_count);

		if (bo->map_count++ == 0)
			bo_open_vma(bufmgr, bo);

		memclear(mmap_arg);
		mmap_arg.handle = bo->gem_handle;

		/* Get the fake offset back... */
		ptr = MAP_FAILED;
		if (drmIoctl(bufmgr->fd,
			     DRM_IOCTL_I915_GEM_MMAP_GTT,
			     &mmap_arg) == 0) {
			/* and mmap it */
			ptr = drm_mmap(0, bo->size, PROT_READ | PROT_WRITE,
				       MAP_SHARED, bufmgr->fd,
				       mmap_arg.offset);
		}
		if (ptr == MAP_FAILED) {
			if (--bo->map_count == 0)
				bo_close_vma(bufmgr, bo);
			ptr = NULL;
		}

		bo->gtt_virtual = ptr;
	}
	pthread_mutex_unlock(&bufmgr->lock);

	return bo->gtt_virtual;
}

void *drm_bacon_gem_bo_map__cpu(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;

	if (bo->mem_virtual)
		return bo->mem_virtual;

	pthread_mutex_lock(&bufmgr->lock);
	if (!bo->mem_virtual) {
		struct drm_i915_gem_mmap mmap_arg;

		if (bo->map_count++ == 0)
			bo_open_vma(bufmgr, bo);

		DBG("bo_map: %d (%s), map_count=%d\n",
		    bo->gem_handle, bo->name, bo->map_count);

		memclear(mmap_arg);
		mmap_arg.handle = bo->gem_handle;
		mmap_arg.size = bo->size;
		if (drmIoctl(bufmgr->fd,
			     DRM_IOCTL_I915_GEM_MMAP,
			     &mmap_arg)) {
			DBG("%s:%d: Error mapping buffer %d (%s): %s .\n",
			    __FILE__, __LINE__, bo->gem_handle,
			    bo->name, strerror(errno));
			if (--bo->map_count == 0)
				bo_close_vma(bufmgr, bo);
		} else {
			VG(VALGRIND_MALLOCLIKE_BLOCK(mmap_arg.addr_ptr, mmap_arg.size, 0, 1));
			bo->mem_virtual = (void *)(uintptr_t) mmap_arg.addr_ptr;
		}
	}
	pthread_mutex_unlock(&bufmgr->lock);

	return bo->mem_virtual;
}

void *drm_bacon_gem_bo_map__wc(drm_bacon_bo *bo)
{
	drm_bacon_bufmgr *bufmgr = bo->bufmgr;

	if (bo->wc_virtual)
		return bo->wc_virtual;

	pthread_mutex_lock(&bufmgr->lock);
	if (!bo->wc_virtual) {
		struct drm_i915_gem_mmap mmap_arg;

		if (bo->map_count++ == 0)
			bo_open_vma(bufmgr, bo);

		DBG("bo_map: %d (%s), map_count=%d\n",
		    bo->gem_handle, bo->name, bo->map_count);

		memclear(mmap_arg);
		mmap_arg.handle = bo->gem_handle;
		mmap_arg.size = bo->size;
		mmap_arg.flags = I915_MMAP_WC;
		if (drmIoctl(bufmgr->fd,
			     DRM_IOCTL_I915_GEM_MMAP,
			     &mmap_arg)) {
			DBG("%s:%d: Error mapping buffer %d (%s): %s .\n",
			    __FILE__, __LINE__, bo->gem_handle,
			    bo->name, strerror(errno));
			if (--bo->map_count == 0)
				bo_close_vma(bufmgr, bo);
		} else {
			VG(VALGRIND_MALLOCLIKE_BLOCK(mmap_arg.addr_ptr, mmap_arg.size, 0, 1));
			bo->wc_virtual = (void *)(uintptr_t) mmap_arg.addr_ptr;
		}
	}
	pthread_mutex_unlock(&bufmgr->lock);

	return bo->wc_virtual;
}

/**
 * Initializes the GEM buffer manager, which uses the kernel to allocate, map,
 * and manage map buffer objections.
 *
 * \param fd File descriptor of the opened DRM device.
 */
drm_bacon_bufmgr *
drm_bacon_bufmgr_gem_init(struct gen_device_info *devinfo,
                          int fd, int batch_size)
{
	drm_bacon_bufmgr *bufmgr;

	bufmgr = calloc(1, sizeof(*bufmgr));
	if (bufmgr == NULL)
		return NULL;

	/* Handles to buffer objects belong to the device fd and are not
	 * reference counted by the kernel.  If the same fd is used by
	 * multiple parties (threads sharing the same screen bufmgr, or
	 * even worse the same device fd passed to multiple libraries)
	 * ownership of those handles is shared by those independent parties.
	 *
	 * Don't do this! Ensure that each library/bufmgr has its own device
	 * fd so that its namespace does not clash with another.
	 */
	bufmgr->fd = fd;

	if (pthread_mutex_init(&bufmgr->lock, NULL) != 0) {
		free(bufmgr);
		return NULL;
	}

	bufmgr->has_llc = devinfo->has_llc;

	init_cache_buckets(bufmgr);

	list_inithead(&bufmgr->vma_cache);
	bufmgr->vma_max = -1; /* unlimited by default */

	bufmgr->name_table =
		_mesa_hash_table_create(NULL, key_hash_uint, key_uint_equal);
	bufmgr->handle_table =
		_mesa_hash_table_create(NULL, key_hash_uint, key_uint_equal);

	return bufmgr;
}
