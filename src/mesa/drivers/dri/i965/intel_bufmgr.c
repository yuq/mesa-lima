/*
 * Copyright © 2007 Intel Corporation
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <drm.h>
#include <i915_drm.h>
#include "libdrm_macros.h"
#include "intel_bufmgr.h"
#include "intel_bufmgr_priv.h"
#include "xf86drm.h"

/** @file intel_bufmgr.c
 *
 * Convenience functions for buffer management methods.
 */

drm_bacon_bo *
drm_bacon_bo_alloc(drm_bacon_bufmgr *bufmgr, const char *name,
		   unsigned long size, unsigned int alignment)
{
	return bufmgr->bo_alloc(bufmgr, name, size, alignment);
}

drm_bacon_bo *
drm_bacon_bo_alloc_for_render(drm_bacon_bufmgr *bufmgr, const char *name,
			      unsigned long size, unsigned int alignment)
{
	return bufmgr->bo_alloc_for_render(bufmgr, name, size, alignment);
}

drm_bacon_bo *
drm_bacon_bo_alloc_userptr(drm_bacon_bufmgr *bufmgr,
			   const char *name, void *addr,
			   uint32_t tiling_mode,
			   uint32_t stride,
			   unsigned long size,
			   unsigned long flags)
{
	if (bufmgr->bo_alloc_userptr)
		return bufmgr->bo_alloc_userptr(bufmgr, name, addr, tiling_mode,
						stride, size, flags);
	return NULL;
}

drm_bacon_bo *
drm_bacon_bo_alloc_tiled(drm_bacon_bufmgr *bufmgr, const char *name,
                        int x, int y, int cpp, uint32_t *tiling_mode,
                        unsigned long *pitch, unsigned long flags)
{
	return bufmgr->bo_alloc_tiled(bufmgr, name, x, y, cpp,
				      tiling_mode, pitch, flags);
}

void
drm_bacon_bo_reference(drm_bacon_bo *bo)
{
	bo->bufmgr->bo_reference(bo);
}

void
drm_bacon_bo_unreference(drm_bacon_bo *bo)
{
	if (bo == NULL)
		return;

	bo->bufmgr->bo_unreference(bo);
}

int
drm_bacon_bo_map(drm_bacon_bo *buf, int write_enable)
{
	return buf->bufmgr->bo_map(buf, write_enable);
}

int
drm_bacon_bo_unmap(drm_bacon_bo *buf)
{
	return buf->bufmgr->bo_unmap(buf);
}

int
drm_bacon_bo_subdata(drm_bacon_bo *bo, unsigned long offset,
		     unsigned long size, const void *data)
{
	return bo->bufmgr->bo_subdata(bo, offset, size, data);
}

int
drm_bacon_bo_get_subdata(drm_bacon_bo *bo, unsigned long offset,
			 unsigned long size, void *data)
{
	int ret;
	if (bo->bufmgr->bo_get_subdata)
		return bo->bufmgr->bo_get_subdata(bo, offset, size, data);

	if (size == 0 || data == NULL)
		return 0;

	ret = drm_bacon_bo_map(bo, 0);
	if (ret)
		return ret;
	memcpy(data, (unsigned char *)bo->virtual + offset, size);
	drm_bacon_bo_unmap(bo);
	return 0;
}

void
drm_bacon_bo_wait_rendering(drm_bacon_bo *bo)
{
	bo->bufmgr->bo_wait_rendering(bo);
}

void
drm_bacon_bufmgr_destroy(drm_bacon_bufmgr *bufmgr)
{
	bufmgr->destroy(bufmgr);
}

int
drm_bacon_bo_exec(drm_bacon_bo *bo, int used,
		  drm_clip_rect_t * cliprects, int num_cliprects, int DR4)
{
	return bo->bufmgr->bo_exec(bo, used, cliprects, num_cliprects, DR4);
}

int
drm_bacon_bo_mrb_exec(drm_bacon_bo *bo, int used,
		drm_clip_rect_t *cliprects, int num_cliprects, int DR4,
		unsigned int rings)
{
	if (bo->bufmgr->bo_mrb_exec)
		return bo->bufmgr->bo_mrb_exec(bo, used,
					cliprects, num_cliprects, DR4,
					rings);

	switch (rings) {
	case I915_EXEC_DEFAULT:
	case I915_EXEC_RENDER:
		return bo->bufmgr->bo_exec(bo, used,
					   cliprects, num_cliprects, DR4);
	default:
		return -ENODEV;
	}
}

void
drm_bacon_bufmgr_set_debug(drm_bacon_bufmgr *bufmgr, int enable_debug)
{
	bufmgr->debug = enable_debug;
}

int
drm_bacon_bufmgr_check_aperture_space(drm_bacon_bo ** bo_array, int count)
{
	return bo_array[0]->bufmgr->check_aperture_space(bo_array, count);
}

int
drm_bacon_bo_flink(drm_bacon_bo *bo, uint32_t * name)
{
	if (bo->bufmgr->bo_flink)
		return bo->bufmgr->bo_flink(bo, name);

	return -ENODEV;
}

int
drm_bacon_bo_emit_reloc(drm_bacon_bo *bo, uint32_t offset,
			drm_bacon_bo *target_bo, uint32_t target_offset,
			uint32_t read_domains, uint32_t write_domain)
{
	return bo->bufmgr->bo_emit_reloc(bo, offset,
					 target_bo, target_offset,
					 read_domains, write_domain);
}

/* For fence registers, not GL fences */
int
drm_bacon_bo_emit_reloc_fence(drm_bacon_bo *bo, uint32_t offset,
			      drm_bacon_bo *target_bo, uint32_t target_offset,
			      uint32_t read_domains, uint32_t write_domain)
{
	return bo->bufmgr->bo_emit_reloc_fence(bo, offset,
					       target_bo, target_offset,
					       read_domains, write_domain);
}


int
drm_bacon_bo_pin(drm_bacon_bo *bo, uint32_t alignment)
{
	if (bo->bufmgr->bo_pin)
		return bo->bufmgr->bo_pin(bo, alignment);

	return -ENODEV;
}

int
drm_bacon_bo_unpin(drm_bacon_bo *bo)
{
	if (bo->bufmgr->bo_unpin)
		return bo->bufmgr->bo_unpin(bo);

	return -ENODEV;
}

int
drm_bacon_bo_set_tiling(drm_bacon_bo *bo, uint32_t * tiling_mode,
			uint32_t stride)
{
	if (bo->bufmgr->bo_set_tiling)
		return bo->bufmgr->bo_set_tiling(bo, tiling_mode, stride);

	*tiling_mode = I915_TILING_NONE;
	return 0;
}

int
drm_bacon_bo_get_tiling(drm_bacon_bo *bo, uint32_t * tiling_mode,
			uint32_t * swizzle_mode)
{
	if (bo->bufmgr->bo_get_tiling)
		return bo->bufmgr->bo_get_tiling(bo, tiling_mode, swizzle_mode);

	*tiling_mode = I915_TILING_NONE;
	*swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
	return 0;
}

int
drm_bacon_bo_set_softpin_offset(drm_bacon_bo *bo, uint64_t offset)
{
	if (bo->bufmgr->bo_set_softpin_offset)
		return bo->bufmgr->bo_set_softpin_offset(bo, offset);

	return -ENODEV;
}

int
drm_bacon_bo_disable_reuse(drm_bacon_bo *bo)
{
	if (bo->bufmgr->bo_disable_reuse)
		return bo->bufmgr->bo_disable_reuse(bo);
	return 0;
}

int
drm_bacon_bo_is_reusable(drm_bacon_bo *bo)
{
	if (bo->bufmgr->bo_is_reusable)
		return bo->bufmgr->bo_is_reusable(bo);
	return 0;
}

int
drm_bacon_bo_busy(drm_bacon_bo *bo)
{
	if (bo->bufmgr->bo_busy)
		return bo->bufmgr->bo_busy(bo);
	return 0;
}

int
drm_bacon_bo_madvise(drm_bacon_bo *bo, int madv)
{
	if (bo->bufmgr->bo_madvise)
		return bo->bufmgr->bo_madvise(bo, madv);
	return -1;
}

int
drm_bacon_bo_use_48b_address_range(drm_bacon_bo *bo, uint32_t enable)
{
	if (bo->bufmgr->bo_use_48b_address_range) {
		bo->bufmgr->bo_use_48b_address_range(bo, enable);
		return 0;
	}

	return -ENODEV;
}

int
drm_bacon_bo_references(drm_bacon_bo *bo, drm_bacon_bo *target_bo)
{
	return bo->bufmgr->bo_references(bo, target_bo);
}

int
drm_bacon_get_pipe_from_crtc_id(drm_bacon_bufmgr *bufmgr, int crtc_id)
{
	if (bufmgr->get_pipe_from_crtc_id)
		return bufmgr->get_pipe_from_crtc_id(bufmgr, crtc_id);
	return -1;
}
