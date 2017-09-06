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

#include "xf86drm.h"
#include "lima_priv.h"
#include "lima.h"
#include "lima_drm.h"

#include "util/u_atomic.h"

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
   int err;
   struct drm_gem_close req = {
      .handle = bo->handle,
   };

   if (!p_atomic_dec_zero(&bo->refcnt))
      return 0;

   pthread_mutex_lock(&bo->dev->bo_table_mutex);
   util_hash_table_remove(bo->dev->bo_handles, (void *)bo->handle);
   if (bo->flink_name)
      util_hash_table_remove(bo->dev->bo_flink_names, (void *)bo->flink_name);
   pthread_mutex_unlock(&bo->dev->bo_table_mutex);

   err = drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_CLOSE, &req);
   free(bo);
   return err;
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

      drmAddress address;
      if (!drmMap(bo->dev->fd, bo->offset, bo->size, &address))
         bo->map = address;
   }

   return bo->map;
}

int lima_bo_unmap(lima_bo_handle bo)
{
   int err = 0;

   if (bo->map) {
      err = drmUnmap(bo->map, bo->size);
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
   }

   return -EINVAL;
}

int lima_bo_import(lima_device_handle dev, enum lima_bo_handle_type type,
                   uint32_t handle, struct lima_bo_import_result *result)
{
   int err;
   lima_bo_handle bo = NULL;
   struct drm_gem_open req = {0};

   pthread_mutex_lock(&dev->bo_table_mutex);
   switch (type) {
   case lima_bo_handle_type_gem_flink_name:
      bo = util_hash_table_get(dev->bo_flink_names, (void *)handle);
      break;
   case lima_bo_handle_type_kms:
      bo = util_hash_table_get(dev->bo_handles, (void *)handle);
      break;
   }
   pthread_mutex_unlock(&dev->bo_table_mutex);

   if (bo) {
      p_atomic_inc(&bo->refcnt);
      result->bo = bo;
      result->size = bo->size;
      return 0;
   }

   bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;

   bo->dev = dev;
   p_atomic_set(&bo->refcnt, 1);

   switch (type) {
   case lima_bo_handle_type_gem_flink_name:
      req.name = handle;
      err = drmIoctl(dev->fd, DRM_IOCTL_GEM_OPEN, &req);
      if (err) {
         free(bo);
         return err;
      }
      bo->handle = req.handle;
      bo->flink_name = handle;
      bo->size = req.size;

      pthread_mutex_lock(&dev->bo_table_mutex);
      util_hash_table_set(bo->dev->bo_flink_names, (void *)bo->flink_name, bo);
      pthread_mutex_unlock(&dev->bo_table_mutex);
      break;
   case lima_bo_handle_type_kms:
      /* not possible */
      free(bo);
      return -EINVAL;
   }

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
