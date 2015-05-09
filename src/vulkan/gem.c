/*
 * Copyright © 2015 Intel Corporation
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
 */

#define _DEFAULT_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include "private.h"

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#include <memcheck.h>
#define VG(x) x
#else
#define VG(x)
#endif

#define VG_CLEAR(s) VG(memset(&s, 0, sizeof(s)))

static int
anv_ioctl(int fd, unsigned long request, void *arg)
{
   int ret;

   do {
      ret = ioctl(fd, request, arg);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   return ret;
}

/**
 * Wrapper around DRM_IOCTL_I915_GEM_CREATE.
 *
 * Return gem handle, or 0 on failure. Gem handles are never 0.
 */
uint32_t
anv_gem_create(struct anv_device *device, size_t size)
{
   struct drm_i915_gem_create gem_create;
   int ret;

   VG_CLEAR(gem_create);
   gem_create.size = size;

   ret = anv_ioctl(device->fd, DRM_IOCTL_I915_GEM_CREATE, &gem_create);
   if (ret != 0) {
      /* FIXME: What do we do if this fails? */
      return 0;
   }

   return gem_create.handle;
}

void
anv_gem_close(struct anv_device *device, int gem_handle)
{
   struct drm_gem_close close;

   VG_CLEAR(close);
   close.handle = gem_handle;
   anv_ioctl(device->fd, DRM_IOCTL_GEM_CLOSE, &close);
}

/**
 * Wrapper around DRM_IOCTL_I915_GEM_MMAP.
 */
void*
anv_gem_mmap(struct anv_device *device, uint32_t gem_handle,
             uint64_t offset, uint64_t size)
{
   struct drm_i915_gem_mmap gem_mmap;
   int ret;

   VG_CLEAR(gem_mmap);
   gem_mmap.handle = gem_handle;
   gem_mmap.offset = offset;
   gem_mmap.size = size;
   ret = anv_ioctl(device->fd, DRM_IOCTL_I915_GEM_MMAP, &gem_mmap);
   if (ret != 0) {
      /* FIXME: Is NULL the right error return? Cf MAP_INVALID */
      return NULL;
   }

   VG(VALGRIND_MALLOCLIKE_BLOCK(gem_mmap.addr_ptr, gem_mmap.size, 0, 1));
   return (void *)(uintptr_t) gem_mmap.addr_ptr;
}

/* This is just a wrapper around munmap, but it also notifies valgrind that
 * this map is no longer valid.  Pair this with anv_gem_mmap().
 */
void
anv_gem_munmap(void *p, uint64_t size)
{
   munmap(p, size);
   VG(VALGRIND_FREELIKE_BLOCK(p, 0));
}

int
anv_gem_userptr(struct anv_device *device, void *mem, size_t size)
{
   struct drm_i915_gem_userptr userptr;
   int ret;

   VG_CLEAR(userptr);
   userptr.user_ptr = (__u64)((unsigned long) mem);
   userptr.user_size = size;
   userptr.flags = 0;

   ret = anv_ioctl(device->fd, DRM_IOCTL_I915_GEM_USERPTR, &userptr);
   if (ret == -1)
      return 0;

   return userptr.handle;
}

/**
 * On error, \a timeout_ns holds the remaining time.
 */
int
anv_gem_wait(struct anv_device *device, int gem_handle, int64_t *timeout_ns)
{
   struct drm_i915_gem_wait wait;
   int ret;

   VG_CLEAR(wait);
   wait.bo_handle = gem_handle;
   wait.timeout_ns = *timeout_ns;
   wait.flags = 0;

   ret = anv_ioctl(device->fd, DRM_IOCTL_I915_GEM_WAIT, &wait);
   *timeout_ns = wait.timeout_ns;
   if (ret == -1)
      return -errno;

   return ret;
}

int
anv_gem_execbuffer(struct anv_device *device,
                   struct drm_i915_gem_execbuffer2 *execbuf)
{
   return anv_ioctl(device->fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf);
}

int
anv_gem_set_tiling(struct anv_device *device,
                   int gem_handle, uint32_t stride, uint32_t tiling)
{
   struct drm_i915_gem_set_tiling set_tiling;
   int ret;

   /* set_tiling overwrites the input on the error path, so we have to open
    * code anv_ioctl.
    */

   do {
      VG_CLEAR(set_tiling);
      set_tiling.handle = gem_handle;
      set_tiling.tiling_mode = I915_TILING_X;
      set_tiling.stride = stride;

      ret = ioctl(device->fd, DRM_IOCTL_I915_GEM_SET_TILING, &set_tiling);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   return ret;
}

int
anv_gem_get_param(int fd, uint32_t param)
{
   drm_i915_getparam_t gp;
   int ret, tmp;

   VG_CLEAR(gp);
   gp.param = param;
   gp.value = &tmp;
   ret = anv_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);
   if (ret == 0)
      return tmp;

   return 0;
}

int
anv_gem_create_context(struct anv_device *device)
{
   struct drm_i915_gem_context_create create;
   int ret;

   VG_CLEAR(create);

   ret = anv_ioctl(device->fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &create);
   if (ret == -1)
      return -1;

   return create.ctx_id;
}

int
anv_gem_destroy_context(struct anv_device *device, int context)
{
   struct drm_i915_gem_context_destroy destroy;

   VG_CLEAR(destroy);
   destroy.ctx_id = context;

   return anv_ioctl(device->fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &destroy);
}

int
anv_gem_get_aperture(struct anv_device *device, uint64_t *size)
{
   struct drm_i915_gem_get_aperture aperture;
   int ret;

   VG_CLEAR(aperture);
   ret = anv_ioctl(device->fd, DRM_IOCTL_I915_GEM_GET_APERTURE, &aperture);
   if (ret == -1)
      return -1;

   *size = aperture.aper_available_size;

   return 0;
}

int
anv_gem_handle_to_fd(struct anv_device *device, int gem_handle)
{
   struct drm_prime_handle args;
   int ret;

   VG_CLEAR(args);
   args.handle = gem_handle;
   args.flags = DRM_CLOEXEC;

   ret = anv_ioctl(device->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
   if (ret == -1)
      return -1;

   return args.fd;
}

int
anv_gem_fd_to_handle(struct anv_device *device, int fd)
{
   struct drm_prime_handle args;
   int ret;

   VG_CLEAR(args);
   args.fd = fd;

   ret = anv_ioctl(device->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &args);
   if (ret == -1)
      return 0;

   return args.handle;
}
