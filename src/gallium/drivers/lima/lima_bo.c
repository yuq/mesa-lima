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
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include "xf86drm.h"
#include "lima_priv.h"
#include "lima.h"
#include "lima_drm.h"

#include "util/u_atomic.h"
#include "os/os_mman.h"

static void lima_close_kms_handle(lima_device_handle dev, uint32_t handle)
{
   struct drm_gem_close args = {};

   args.handle = handle;
   drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &args);
}

int lima_bo_create(lima_device_handle dev, struct lima_bo_create_request *request,
                   lima_bo_handle *bo_handle)
{
   int err;
   struct lima_bo *bo;
   struct drm_lima_gem_create drm_request = {
      .size = request->size,
      .flags = request->flags,
   };

   bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;

   err = drmIoctl(dev->fd, DRM_IOCTL_LIMA_GEM_CREATE, &drm_request);
   if (err) {
      free(bo);
      return err;
   }

   bo->dev = dev;
   bo->size = drm_request.size;
   bo->handle = drm_request.handle;
   p_atomic_set(&bo->refcnt, 1);

   *bo_handle = bo;
   return 0;
}

int lima_bo_free(lima_bo_handle bo)
{

   if (!p_atomic_dec_zero(&bo->refcnt))
      return 0;

   pthread_mutex_lock(&bo->dev->bo_table_mutex);
   util_hash_table_remove(bo->dev->bo_handles, (void *)bo->handle);
   if (bo->flink_name)
      util_hash_table_remove(bo->dev->bo_flink_names, (void *)bo->flink_name);
   pthread_mutex_unlock(&bo->dev->bo_table_mutex);

   lima_close_kms_handle(bo->dev, bo->handle);
   free(bo);
   return 0;
}

void *lima_bo_map(lima_bo_handle bo)
{
   if (!bo->map) {
      if (!bo->offset) {
         struct drm_lima_gem_info req = {
            .handle = bo->handle,
         };

         if (drmIoctl(bo->dev->fd, DRM_IOCTL_LIMA_GEM_INFO, &req))
            return NULL;
         else
            bo->offset = req.offset;
      }

      bo->map = os_mmap(0, bo->size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, bo->dev->fd, bo->offset);
      if (bo->map == MAP_FAILED)
          bo->map = NULL;
   }

   return bo->map;
}

int lima_bo_unmap(lima_bo_handle bo)
{
   int err = 0;

   if (bo->map) {
      err = os_munmap(bo->map, bo->size);
      bo->map = NULL;
   }
   return err;
}

int lima_bo_va_map(lima_bo_handle bo, uint32_t va, uint32_t flags)
{
   struct drm_lima_gem_va req = {
      .handle = bo->handle,
      .op = LIMA_VA_OP_MAP,
      .flags = flags,
      .va = va,
   };

   return drmIoctl(bo->dev->fd, DRM_IOCTL_LIMA_GEM_VA, &req);
}

int lima_bo_va_unmap(lima_bo_handle bo, uint32_t va)
{
   struct drm_lima_gem_va req = {
      .handle = bo->handle,
      .op = LIMA_VA_OP_UNMAP,
      .flags = 0,
      .va = va,
   };

   return drmIoctl(bo->dev->fd, DRM_IOCTL_LIMA_GEM_VA, &req);
}

int lima_bo_export(lima_bo_handle bo, enum lima_bo_handle_type type,
                   uint32_t *handle)
{
   int err;

   switch (type) {
   case lima_bo_handle_type_gem_flink_name:
      if (!bo->flink_name) {
         struct drm_gem_flink flink = {
            .handle = bo->handle,
            .name = 0,
         };
         err = drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_FLINK, &flink);
         if (err)
            return err;

         bo->flink_name = flink.name;

         pthread_mutex_lock(&bo->dev->bo_table_mutex);
         util_hash_table_set(bo->dev->bo_flink_names, (void *)bo->flink_name, bo);
         pthread_mutex_unlock(&bo->dev->bo_table_mutex);
      }
      *handle = bo->flink_name;
      return 0;

   case lima_bo_handle_type_kms:
      pthread_mutex_lock(&bo->dev->bo_table_mutex);
      util_hash_table_set(bo->dev->bo_handles, (void *)bo->handle, bo);
      pthread_mutex_unlock(&bo->dev->bo_table_mutex);

      *handle = bo->handle;
      return 0;
   case lima_bo_handle_type_dma_buf_fd:
      /* unsupported yet */
      return -EINVAL;
   }

   return -EINVAL;
}

int lima_bo_import(lima_device_handle dev, enum lima_bo_handle_type type,
                   uint32_t handle, struct lima_bo_import_result *result)
{
   int err;
   lima_bo_handle bo = NULL;
   struct drm_gem_open req = {0};
   uint64_t dma_buf_size = 0;

   pthread_mutex_lock(&dev->bo_table_mutex);

   /* Convert a DMA buf handle to a KMS handle now. */
   if (type == lima_bo_handle_type_dma_buf_fd) {
      uint32_t prime_handle;
      off_t size;

      /* Get a KMS handle. */
      err = drmPrimeFDToHandle(dev->fd, handle, &prime_handle);
      if (err) {
         pthread_mutex_unlock(&dev->bo_table_mutex);
         return err;
      }

      /* Query the buffer size. */
      size = lseek(handle, 0, SEEK_END);
      if (size == (off_t)-1) {
         pthread_mutex_unlock(&dev->bo_table_mutex);
         lima_close_kms_handle(dev, prime_handle);
         return -errno;
      }
      lseek(handle, 0, SEEK_SET);

      dma_buf_size = size;
      handle = prime_handle;
   }

   switch (type) {
   case lima_bo_handle_type_gem_flink_name:
      bo = util_hash_table_get(dev->bo_flink_names, (void *)handle);
      break;
   case lima_bo_handle_type_kms:
      bo = util_hash_table_get(dev->bo_handles, (void *)handle);
      break;
   case lima_bo_handle_type_dma_buf_fd:
      bo = util_hash_table_get(dev->bo_handles, (void *)handle);
      break;
   default:
      pthread_mutex_unlock(&dev->bo_table_mutex);
      return -EINVAL;
   }

   if (bo) {
      p_atomic_inc(&bo->refcnt);
      result->bo = bo;
      result->size = bo->size;
      pthread_mutex_unlock(&dev->bo_table_mutex);
      return 0;
   }

   bo = calloc(1, sizeof(*bo));
   if (!bo) {
      pthread_mutex_unlock(&dev->bo_table_mutex);
      if (type == lima_bo_handle_type_dma_buf_fd) {
         lima_close_kms_handle(dev, handle);
      }
      return -ENOMEM;
   }
   bo->dev = dev;
   p_atomic_set(&bo->refcnt, 1);

   switch (type) {
   case lima_bo_handle_type_gem_flink_name:
      req.name = handle;
      err = drmIoctl(dev->fd, DRM_IOCTL_GEM_OPEN, &req);
      if (err) {
         free(bo);
         pthread_mutex_unlock(&dev->bo_table_mutex);
         return err;
      }
      bo->handle = req.handle;
      bo->flink_name = handle;
      bo->size = req.size;

      util_hash_table_set(bo->dev->bo_flink_names, (void *)bo->flink_name, bo);
      pthread_mutex_unlock(&dev->bo_table_mutex);
      break;
   case lima_bo_handle_type_kms:
      /* not possible */
      free(bo);
      pthread_mutex_unlock(&dev->bo_table_mutex);
      return -EINVAL;
   case lima_bo_handle_type_dma_buf_fd:
      bo->handle = handle;
      bo->size = dma_buf_size;
      break;
   }
   util_hash_table_set(dev->bo_handles, (void*)bo->handle, bo);
   pthread_mutex_unlock(&dev->bo_table_mutex);

   result->bo = bo;
   result->size = bo->size;
   return 0;
}

int lima_bo_wait(lima_bo_handle bo, uint32_t op, uint64_t timeout_ns, bool relative)
{
   struct drm_lima_gem_wait req = {
      .handle = bo->handle,
      .op = op,
      .timeout_ns = timeout_ns,
   };
   int err;

   err = lima_get_absolute_timeout(&req.timeout_ns, relative);
   if (err)
      return err;

   return drmIoctl(bo->dev->fd, DRM_IOCTL_LIMA_GEM_WAIT, &req);
}
