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
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include "xf86drm.h"
#include "lima_drm.h"

#include "util/u_hash_table.h"
#include "os/os_mman.h"

#include "state_tracker/drm_driver.h"

#include "lima_screen.h"
#include "lima_bo.h"
#include "lima_vamgr.h"
#include "lima_util.h"

#define PTR_TO_UINT(x) ((unsigned)((intptr_t)(x)))

static unsigned handle_hash(void *key)
{
    return PTR_TO_UINT(key);
}

static int handle_compare(void *key1, void *key2)
{
    return PTR_TO_UINT(key1) != PTR_TO_UINT(key2);
}

bool lima_bo_table_init(struct lima_screen *screen)
{
   screen->bo_handles = util_hash_table_create(handle_hash, handle_compare);
   if (!screen->bo_handles)
      return false;

   screen->bo_flink_names = util_hash_table_create(handle_hash, handle_compare);
   if (!screen->bo_flink_names)
      goto err_out0;

   mtx_init(&screen->bo_table_lock, mtx_plain);
   return true;

err_out0:
   util_hash_table_destroy(screen->bo_handles);
   return false;
}

void lima_bo_table_fini(struct lima_screen *screen)
{
   mtx_destroy(&screen->bo_table_lock);
   util_hash_table_destroy(screen->bo_handles);
   util_hash_table_destroy(screen->bo_flink_names);
}

static void lima_close_kms_handle(struct lima_screen *screen, uint32_t handle)
{
   struct drm_gem_close args = {
      .handle = handle,
   };

   drmIoctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &args);
}

struct lima_bo *lima_bo_create(struct lima_screen *screen,
                               uint32_t size, uint32_t flags,
                               bool need_map, bool need_va)
{
   struct lima_bo *bo;
   struct drm_lima_gem_create drm_request = {
      .size = size,
      .flags = flags,
   };

   if (!(bo = calloc(1, sizeof(*bo))))
       return NULL;

   if (drmIoctl(screen->fd, DRM_IOCTL_LIMA_GEM_CREATE, &drm_request))
      goto err_out0;

   bo->screen = screen;
   bo->size = drm_request.size;
   bo->handle = drm_request.handle;
   p_atomic_set(&bo->refcnt, 1);

   if (!lima_bo_update(bo, need_map, need_va))
      goto err_out1;

   return bo;

err_out1:
   lima_close_kms_handle(screen, bo->handle);
err_out0:
   free(bo);
   return NULL;
}

void lima_bo_free(struct lima_bo *bo)
{
   if (!p_atomic_dec_zero(&bo->refcnt))
      return;

   struct lima_screen *screen = bo->screen;
   mtx_lock(&screen->bo_table_lock);
   util_hash_table_remove(screen->bo_handles, (void *)bo->handle);
   if (bo->flink_name)
      util_hash_table_remove(screen->bo_flink_names, (void *)bo->flink_name);
   mtx_unlock(&screen->bo_table_lock);

   if (bo->va) {
      lima_bo_va_unmap(bo, bo->va);
      lima_va_range_free(bo->screen, bo->size, bo->va);
   }

   lima_close_kms_handle(screen, bo->handle);
   free(bo);
}

void *lima_bo_map(struct lima_bo *bo)
{
   if (!bo->map) {
      if (!bo->offset) {
         struct drm_lima_gem_info req = {
            .handle = bo->handle,
         };

         if (drmIoctl(bo->screen->fd, DRM_IOCTL_LIMA_GEM_INFO, &req))
            return NULL;

         bo->offset = req.offset;
      }

      bo->map = os_mmap(0, bo->size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, bo->screen->fd, bo->offset);
      if (bo->map == MAP_FAILED)
          bo->map = NULL;
   }

   return bo->map;
}

void lima_bo_unmap(struct lima_bo *bo)
{
   if (bo->map) {
      os_munmap(bo->map, bo->size);
      bo->map = NULL;
   }
}

bool lima_bo_va_map(struct lima_bo *bo, uint32_t va, uint32_t flags)
{
   struct drm_lima_gem_va req = {
      .handle = bo->handle,
      .op = LIMA_VA_OP_MAP,
      .flags = flags,
      .va = va,
   };

   return drmIoctl(bo->screen->fd, DRM_IOCTL_LIMA_GEM_VA, &req) == 0;
}

void lima_bo_va_unmap(struct lima_bo *bo, uint32_t va)
{
   struct drm_lima_gem_va req = {
      .handle = bo->handle,
      .op = LIMA_VA_OP_UNMAP,
      .flags = 0,
      .va = va,
   };

   drmIoctl(bo->screen->fd, DRM_IOCTL_LIMA_GEM_VA, &req);
}

bool lima_bo_update(struct lima_bo *bo, bool need_map, bool need_va)
{
   if (need_map && !bo->map) {
      bo->map = lima_bo_map(bo);
      if (!bo->map)
         return false;
   }

   if (need_va && !bo->va) {
      if (!lima_va_range_alloc(bo->screen, bo->size, &bo->va) ||
          !lima_bo_va_map(bo, bo->va, 0))
          return false;
   }

   return true;
}

bool lima_bo_export(struct lima_bo *bo, struct winsys_handle *handle)
{
   struct lima_screen *screen = bo->screen;

   switch (handle->type) {
   case DRM_API_HANDLE_TYPE_SHARED:
      if (!bo->flink_name) {
         struct drm_gem_flink flink = {
            .handle = bo->handle,
            .name = 0,
         };
         if (drmIoctl(screen->fd, DRM_IOCTL_GEM_FLINK, &flink))
            return false;

         bo->flink_name = flink.name;

         mtx_lock(&screen->bo_table_lock);
         util_hash_table_set(screen->bo_flink_names, (void *)bo->flink_name, bo);
         mtx_unlock(&screen->bo_table_lock);
      }
      handle->handle = bo->flink_name;
      return true;

   case DRM_API_HANDLE_TYPE_KMS:
      mtx_lock(&screen->bo_table_lock);
      util_hash_table_set(screen->bo_handles, (void *)bo->handle, bo);
      mtx_unlock(&screen->bo_table_lock);

      handle->handle = bo->handle;
      return true;

   case DRM_API_HANDLE_TYPE_FD:
      if (drmPrimeHandleToFD(screen->fd, bo->handle, DRM_CLOEXEC,
                             (int*)&handle->handle))
         return false;

      mtx_lock(&screen->bo_table_lock);
      util_hash_table_set(screen->bo_handles, (void *)bo->handle, bo);
      mtx_unlock(&screen->bo_table_lock);
      return true;

   default:
      return false;
   }
}

struct lima_bo *lima_bo_import(struct lima_screen *screen,
                               struct winsys_handle *handle)
{
   struct lima_bo *bo = NULL;
   struct drm_gem_open req = {0};
   uint32_t dma_buf_size = 0;
   unsigned h = handle->handle;

   mtx_lock(&screen->bo_table_lock);

   /* Convert a DMA buf handle to a KMS handle now. */
   if (handle->type == DRM_API_HANDLE_TYPE_FD) {
      uint32_t prime_handle;
      off_t size;

      /* Get a KMS handle. */
      if (drmPrimeFDToHandle(screen->fd, h, &prime_handle)) {
         mtx_unlock(&screen->bo_table_lock);
         return NULL;
      }

      /* Query the buffer size. */
      size = lseek(h, 0, SEEK_END);
      if (size == (off_t)-1) {
         mtx_unlock(&screen->bo_table_lock);
         lima_close_kms_handle(screen, prime_handle);
         return NULL;
      }
      lseek(h, 0, SEEK_SET);

      dma_buf_size = size;
      h = prime_handle;
   }

   switch (handle->type) {
   case DRM_API_HANDLE_TYPE_SHARED:
      bo = util_hash_table_get(screen->bo_flink_names, (void *)h);
      break;
   case DRM_API_HANDLE_TYPE_KMS:
   case DRM_API_HANDLE_TYPE_FD:
      bo = util_hash_table_get(screen->bo_handles, (void *)h);
      break;
   default:
      mtx_unlock(&screen->bo_table_lock);
      return NULL;
   }

   if (bo) {
      p_atomic_inc(&bo->refcnt);
      mtx_unlock(&screen->bo_table_lock);
      return bo;
   }

   if (!(bo = calloc(1, sizeof(*bo)))) {
      mtx_unlock(&screen->bo_table_lock);
      if (handle->type == DRM_API_HANDLE_TYPE_FD)
         lima_close_kms_handle(screen, h);
      return NULL;
   }

   bo->screen = screen;
   p_atomic_set(&bo->refcnt, 1);

   switch (handle->type) {
   case DRM_API_HANDLE_TYPE_SHARED:
      req.name = h;
      if (drmIoctl(screen->fd, DRM_IOCTL_GEM_OPEN, &req)) {
         mtx_unlock(&screen->bo_table_lock);
         free(bo);
         return NULL;
      }
      bo->handle = req.handle;
      bo->flink_name = h;
      bo->size = req.size;

      util_hash_table_set(screen->bo_flink_names, (void *)bo->flink_name, bo);
      break;
   case DRM_API_HANDLE_TYPE_FD:
      bo->handle = h;
      bo->size = dma_buf_size;
      break;
   default:
      /* not possible */
      assert(0);
   }

   util_hash_table_set(screen->bo_handles, (void*)bo->handle, bo);
   mtx_unlock(&screen->bo_table_lock);

   return bo;
}

bool lima_bo_wait(struct lima_bo *bo, uint32_t op, uint64_t timeout_ns, bool relative)
{
   struct drm_lima_gem_wait req = {
      .handle = bo->handle,
      .op = op,
      .timeout_ns = timeout_ns,
   };

   if (!lima_get_absolute_timeout(&req.timeout_ns, relative))
      return false;

   return drmIoctl(bo->screen->fd, DRM_IOCTL_LIMA_GEM_WAIT, &req) == 0;
}
