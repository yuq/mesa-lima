/*
 * Copyright © 2008-2012 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

/**
 * @file brw_bufmgr.h
 *
 * Public definitions of Intel-specific bufmgr functions.
 */

#ifndef INTEL_BUFMGR_H
#define INTEL_BUFMGR_H

#include <stdio.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__cplusplus)
extern "C" {
#endif

struct drm_clip_rect;

typedef struct _drm_bacon_bufmgr drm_bacon_bufmgr;
typedef struct _drm_bacon_context drm_bacon_context;
typedef struct _drm_bacon_bo drm_bacon_bo;

struct _drm_bacon_bo {
	/**
	 * Size in bytes of the buffer object.
	 *
	 * The size may be larger than the size originally requested for the
	 * allocation, such as being aligned to page size.
	 */
	unsigned long size;

	/**
	 * Alignment requirement for object
	 *
	 * Used for GTT mapping & pinning the object.
	 */
	unsigned long align;

	/**
	 * Deprecated field containing (possibly the low 32-bits of) the last
	 * seen virtual card address.  Use offset64 instead.
	 */
	unsigned long offset;

	/**
	 * Virtual address for accessing the buffer data.  Only valid while
	 * mapped.
	 */
#ifdef __cplusplus
	void *virt;
#else
	void *virtual;
#endif

	/** Buffer manager context associated with this buffer object */
	drm_bacon_bufmgr *bufmgr;

	/**
	 * MM-specific handle for accessing object
	 */
	int handle;

	/**
	 * Last seen card virtual address (offset from the beginning of the
	 * aperture) for the object.  This should be used to fill relocation
	 * entries when calling drm_bacon_bo_emit_reloc()
	 */
	uint64_t offset64;
};

#define BO_ALLOC_FOR_RENDER (1<<0)

drm_bacon_bo *drm_bacon_bo_alloc(drm_bacon_bufmgr *bufmgr, const char *name,
				 unsigned long size, unsigned int alignment);
drm_bacon_bo *drm_bacon_bo_alloc_for_render(drm_bacon_bufmgr *bufmgr,
					    const char *name,
					    unsigned long size,
					    unsigned int alignment);
drm_bacon_bo *drm_bacon_bo_alloc_userptr(drm_bacon_bufmgr *bufmgr,
					const char *name,
					void *addr, uint32_t tiling_mode,
					uint32_t stride, unsigned long size,
					unsigned long flags);
drm_bacon_bo *drm_bacon_bo_alloc_tiled(drm_bacon_bufmgr *bufmgr,
				       const char *name,
				       int x, int y, int cpp,
				       uint32_t *tiling_mode,
				       unsigned long *pitch,
				       unsigned long flags);
void drm_bacon_bo_reference(drm_bacon_bo *bo);
void drm_bacon_bo_unreference(drm_bacon_bo *bo);
int drm_bacon_bo_map(drm_bacon_bo *bo, int write_enable);
int drm_bacon_bo_unmap(drm_bacon_bo *bo);

int drm_bacon_bo_subdata(drm_bacon_bo *bo, unsigned long offset,
			 unsigned long size, const void *data);
int drm_bacon_bo_get_subdata(drm_bacon_bo *bo, unsigned long offset,
			     unsigned long size, void *data);
void drm_bacon_bo_wait_rendering(drm_bacon_bo *bo);

void drm_bacon_bufmgr_set_debug(drm_bacon_bufmgr *bufmgr, int enable_debug);
void drm_bacon_bufmgr_destroy(drm_bacon_bufmgr *bufmgr);
int drm_bacon_bo_exec(drm_bacon_bo *bo, int used,
		      struct drm_clip_rect *cliprects, int num_cliprects, int DR4);
int drm_bacon_bo_mrb_exec(drm_bacon_bo *bo, int used,
			struct drm_clip_rect *cliprects, int num_cliprects, int DR4,
			unsigned int flags);
int drm_bacon_bufmgr_check_aperture_space(drm_bacon_bo ** bo_array, int count);

int drm_bacon_bo_emit_reloc(drm_bacon_bo *bo, uint32_t offset,
			    drm_bacon_bo *target_bo, uint32_t target_offset,
			    uint32_t read_domains, uint32_t write_domain);
int drm_bacon_bo_emit_reloc_fence(drm_bacon_bo *bo, uint32_t offset,
				  drm_bacon_bo *target_bo,
				  uint32_t target_offset,
				  uint32_t read_domains, uint32_t write_domain);
int drm_bacon_bo_pin(drm_bacon_bo *bo, uint32_t alignment);
int drm_bacon_bo_unpin(drm_bacon_bo *bo);
int drm_bacon_bo_set_tiling(drm_bacon_bo *bo, uint32_t * tiling_mode,
			    uint32_t stride);
int drm_bacon_bo_get_tiling(drm_bacon_bo *bo, uint32_t * tiling_mode,
			    uint32_t * swizzle_mode);
int drm_bacon_bo_flink(drm_bacon_bo *bo, uint32_t * name);
int drm_bacon_bo_busy(drm_bacon_bo *bo);
int drm_bacon_bo_madvise(drm_bacon_bo *bo, int madv);
int drm_bacon_bo_use_48b_address_range(drm_bacon_bo *bo, uint32_t enable);
int drm_bacon_bo_set_softpin_offset(drm_bacon_bo *bo, uint64_t offset);

int drm_bacon_bo_disable_reuse(drm_bacon_bo *bo);
int drm_bacon_bo_is_reusable(drm_bacon_bo *bo);
int drm_bacon_bo_references(drm_bacon_bo *bo, drm_bacon_bo *target_bo);

/* drm_bacon_bufmgr_gem.c */
drm_bacon_bufmgr *drm_bacon_bufmgr_gem_init(int fd, int batch_size);
drm_bacon_bo *drm_bacon_bo_gem_create_from_name(drm_bacon_bufmgr *bufmgr,
						const char *name,
						unsigned int handle);
void drm_bacon_bufmgr_gem_enable_reuse(drm_bacon_bufmgr *bufmgr);
void drm_bacon_bufmgr_gem_enable_fenced_relocs(drm_bacon_bufmgr *bufmgr);
void drm_bacon_bufmgr_gem_set_vma_cache_size(drm_bacon_bufmgr *bufmgr,
					     int limit);
int drm_bacon_gem_bo_map_unsynchronized(drm_bacon_bo *bo);
int drm_bacon_gem_bo_map_gtt(drm_bacon_bo *bo);
int drm_bacon_gem_bo_unmap_gtt(drm_bacon_bo *bo);

#define HAVE_DRM_INTEL_GEM_BO_DISABLE_IMPLICIT_SYNC 1
int drm_bacon_bufmgr_gem_can_disable_implicit_sync(drm_bacon_bufmgr *bufmgr);
void drm_bacon_gem_bo_disable_implicit_sync(drm_bacon_bo *bo);
void drm_bacon_gem_bo_enable_implicit_sync(drm_bacon_bo *bo);

void *drm_bacon_gem_bo_map__cpu(drm_bacon_bo *bo);
void *drm_bacon_gem_bo_map__gtt(drm_bacon_bo *bo);
void *drm_bacon_gem_bo_map__wc(drm_bacon_bo *bo);

int drm_bacon_gem_bo_get_reloc_count(drm_bacon_bo *bo);
void drm_bacon_gem_bo_clear_relocs(drm_bacon_bo *bo, int start);
void drm_bacon_gem_bo_start_gtt_access(drm_bacon_bo *bo, int write_enable);

int drm_bacon_get_pipe_from_crtc_id(drm_bacon_bufmgr *bufmgr, int crtc_id);

int drm_bacon_bufmgr_gem_get_devid(drm_bacon_bufmgr *bufmgr);
int drm_bacon_gem_bo_wait(drm_bacon_bo *bo, int64_t timeout_ns);

drm_bacon_context *drm_bacon_gem_context_create(drm_bacon_bufmgr *bufmgr);
int drm_bacon_gem_context_get_id(drm_bacon_context *ctx,
                                 uint32_t *ctx_id);
void drm_bacon_gem_context_destroy(drm_bacon_context *ctx);
int drm_bacon_gem_bo_context_exec(drm_bacon_bo *bo, drm_bacon_context *ctx,
				  int used, unsigned int flags);
int drm_bacon_gem_bo_fence_exec(drm_bacon_bo *bo,
				drm_bacon_context *ctx,
				int used,
				int in_fence,
				int *out_fence,
				unsigned int flags);

int drm_bacon_bo_gem_export_to_prime(drm_bacon_bo *bo, int *prime_fd);
drm_bacon_bo *drm_bacon_bo_gem_create_from_prime(drm_bacon_bufmgr *bufmgr,
						int prime_fd, int size);

int drm_bacon_reg_read(drm_bacon_bufmgr *bufmgr,
		       uint32_t offset,
		       uint64_t *result);

int drm_bacon_get_reset_stats(drm_bacon_context *ctx,
			      uint32_t *reset_count,
			      uint32_t *active,
			      uint32_t *pending);

int drm_bacon_get_subslice_total(int fd, unsigned int *subslice_total);
int drm_bacon_get_eu_total(int fd, unsigned int *eu_total);

int drm_bacon_get_pooled_eu(int fd);
int drm_bacon_get_min_eu_in_pool(int fd);

/** @{ */

#if defined(__cplusplus)
}
#endif

#endif /* INTEL_BUFMGR_H */
