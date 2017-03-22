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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__cplusplus)
extern "C" {
#endif

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

/**
 * Allocate a buffer object.
 *
 * Buffer objects are not necessarily initially mapped into CPU virtual
 * address space or graphics device aperture.  They must be mapped
 * using bo_map() or drm_bacon_gem_bo_map_gtt() to be used by the CPU.
 */
drm_bacon_bo *drm_bacon_bo_alloc(drm_bacon_bufmgr *bufmgr, const char *name,
				 unsigned long size, unsigned int alignment);
/**
 * Allocate a buffer object, hinting that it will be used as a
 * render target.
 *
 * This is otherwise the same as bo_alloc.
 */
drm_bacon_bo *drm_bacon_bo_alloc_for_render(drm_bacon_bufmgr *bufmgr,
					    const char *name,
					    unsigned long size,
					    unsigned int alignment);

bool drm_bacon_has_userptr(drm_bacon_bufmgr *bufmgr);

/**
 * Allocate a buffer object from an existing user accessible
 * address malloc'd with the provided size.
 * Alignment is used when mapping to the gtt.
 * Flags may be I915_VMAP_READ_ONLY or I915_USERPTR_UNSYNCHRONIZED
 */
drm_bacon_bo *drm_bacon_bo_alloc_userptr(drm_bacon_bufmgr *bufmgr,
					const char *name,
					void *addr, uint32_t tiling_mode,
					uint32_t stride, unsigned long size,
					unsigned long flags);
/**
 * Allocate a tiled buffer object.
 *
 * Alignment for tiled objects is set automatically; the 'flags'
 * argument provides a hint about how the object will be used initially.
 *
 * Valid tiling formats are:
 *  I915_TILING_NONE
 *  I915_TILING_X
 *  I915_TILING_Y
 *
 * Note the tiling format may be rejected; callers should check the
 * 'tiling_mode' field on return, as well as the pitch value, which
 * may have been rounded up to accommodate for tiling restrictions.
 */
drm_bacon_bo *drm_bacon_bo_alloc_tiled(drm_bacon_bufmgr *bufmgr,
				       const char *name,
				       int x, int y, int cpp,
				       uint32_t *tiling_mode,
				       unsigned long *pitch,
				       unsigned long flags);

/** Takes a reference on a buffer object */
void drm_bacon_bo_reference(drm_bacon_bo *bo);

/**
 * Releases a reference on a buffer object, freeing the data if
 * no references remain.
 */
void drm_bacon_bo_unreference(drm_bacon_bo *bo);

/**
 * Maps the buffer into userspace.
 *
 * This function will block waiting for any existing execution on the
 * buffer to complete, first.  The resulting mapping is available at
 * buf->virtual.
 */
int drm_bacon_bo_map(drm_bacon_bo *bo, int write_enable);

/**
 * Reduces the refcount on the userspace mapping of the buffer
 * object.
 */
int drm_bacon_bo_unmap(drm_bacon_bo *bo);

/** Write data into an object. */
int drm_bacon_bo_subdata(drm_bacon_bo *bo, unsigned long offset,
			 unsigned long size, const void *data);
/** Read data from an object. */
int drm_bacon_bo_get_subdata(drm_bacon_bo *bo, unsigned long offset,
			     unsigned long size, void *data);
/**
 * Waits for rendering to an object by the GPU to have completed.
 *
 * This is not required for any access to the BO by bo_map,
 * bo_subdata, etc.  It is merely a way for the driver to implement
 * glFinish.
 */
void drm_bacon_bo_wait_rendering(drm_bacon_bo *bo);

/**
 * Tears down the buffer manager instance.
 */
void drm_bacon_bufmgr_destroy(drm_bacon_bufmgr *bufmgr);

/** Executes the command buffer pointed to by bo. */
int drm_bacon_bo_exec(drm_bacon_bo *bo, int used);

/** Executes the command buffer pointed to by bo on the selected ring buffer */
int drm_bacon_bo_mrb_exec(drm_bacon_bo *bo, int used, unsigned int flags);
int drm_bacon_bufmgr_check_aperture_space(drm_bacon_bo ** bo_array, int count);

/**
 * Add relocation entry in reloc_buf, which will be updated with the
 * target buffer's real offset on on command submission.
 *
 * Relocations remain in place for the lifetime of the buffer object.
 *
 * \param bo Buffer to write the relocation into.
 * \param offset Byte offset within reloc_bo of the pointer to
 *                      target_bo.
 * \param target_bo Buffer whose offset should be written into the
 *                  relocation entry.
 * \param target_offset Constant value to be added to target_bo's
 *                      offset in relocation entry.
 * \param read_domains GEM read domains which the buffer will be
 *                      read into by the command that this relocation
 *                      is part of.
 * \param write_domains GEM read domains which the buffer will be
 *                      dirtied in by the command that this
 *                      relocation is part of.
 */
int drm_bacon_bo_emit_reloc(drm_bacon_bo *bo, uint32_t offset,
			    drm_bacon_bo *target_bo, uint32_t target_offset,
			    uint32_t read_domains, uint32_t write_domain);

/**
 * Ask that the buffer be placed in tiling mode
 *
 * \param buf Buffer to set tiling mode for
 * \param tiling_mode desired, and returned tiling mode
 */
int drm_bacon_bo_set_tiling(drm_bacon_bo *bo, uint32_t * tiling_mode,
			    uint32_t stride);
/**
 * Get the current tiling (and resulting swizzling) mode for the bo.
 *
 * \param buf Buffer to get tiling mode for
 * \param tiling_mode returned tiling mode
 * \param swizzle_mode returned swizzling mode
 */
int drm_bacon_bo_get_tiling(drm_bacon_bo *bo, uint32_t * tiling_mode,
			    uint32_t * swizzle_mode);

/**
 * Create a visible name for a buffer which can be used by other apps
 *
 * \param buf Buffer to create a name for
 * \param name Returned name
 */
int drm_bacon_bo_flink(drm_bacon_bo *bo, uint32_t * name);

/**
 * Returns 1 if mapping the buffer for write could cause the process
 * to block, due to the object being active in the GPU.
 */
int drm_bacon_bo_busy(drm_bacon_bo *bo);

/**
 * Specify the volatility of the buffer.
 * \param bo Buffer to create a name for
 * \param madv The purgeable status
 *
 * Use I915_MADV_DONTNEED to mark the buffer as purgeable, and it will be
 * reclaimed under memory pressure. If you subsequently require the buffer,
 * then you must pass I915_MADV_WILLNEED to mark the buffer as required.
 *
 * Returns 1 if the buffer was retained, or 0 if it was discarded whilst
 * marked as I915_MADV_DONTNEED.
 */
int drm_bacon_bo_madvise(drm_bacon_bo *bo, int madv);

/**
 * Set the offset at which this buffer will be softpinned
 * \param bo Buffer to set the softpin offset for
 * \param offset Softpin offset
 */
int drm_bacon_bo_set_softpin_offset(drm_bacon_bo *bo, uint64_t offset);

/**
 * Disable buffer reuse for buffers which will be shared in some way,
 * as with scanout buffers. When the buffer reference count goes to
 * zero, it will be freed and not placed in the reuse list.
 *
 * \param bo Buffer to disable reuse for
 */
int drm_bacon_bo_disable_reuse(drm_bacon_bo *bo);

/**
 * Query whether a buffer is reusable.
 *
 * \param bo Buffer to query
 */
int drm_bacon_bo_is_reusable(drm_bacon_bo *bo);

/** Returns true if target_bo is in the relocation tree rooted at bo. */
int drm_bacon_bo_references(drm_bacon_bo *bo, drm_bacon_bo *target_bo);

/* drm_bacon_bufmgr_gem.c */
drm_bacon_bufmgr *drm_bacon_bufmgr_gem_init(int fd, int batch_size);
drm_bacon_bo *drm_bacon_bo_gem_create_from_name(drm_bacon_bufmgr *bufmgr,
						const char *name,
						unsigned int handle);
void drm_bacon_bufmgr_gem_enable_reuse(drm_bacon_bufmgr *bufmgr);
void drm_bacon_bufmgr_gem_set_vma_cache_size(drm_bacon_bufmgr *bufmgr,
					     int limit);
int drm_bacon_gem_bo_map_unsynchronized(drm_bacon_bo *bo);
int drm_bacon_gem_bo_map_gtt(drm_bacon_bo *bo);

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

/** @{ */

#if defined(__cplusplus)
}
#endif

#endif /* INTEL_BUFMGR_H */
