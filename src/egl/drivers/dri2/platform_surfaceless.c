/*
 * Mesa 3-D graphics library
 *
 * Copyright (c) 2014 The Chromium OS Authors.
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <xf86drm.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "egl_dri2.h"
#include "egl_dri2_fallbacks.h"
#include "loader.h"

static __DRIimage*
surfaceless_alloc_image(struct dri2_egl_display *dri2_dpy,
                     struct dri2_egl_surface *dri2_surf)
{
   return dri2_dpy->image->createImage(
            dri2_dpy->dri_screen,
            dri2_surf->base.Width,
            dri2_surf->base.Height,
            dri2_surf->visual,
            0,
            NULL);
}

static void
surfaceless_free_images(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   if (dri2_surf->front) {
      dri2_dpy->image->destroyImage(dri2_surf->front);
      dri2_surf->front = NULL;
   }
}

static int
surfaceless_image_get_buffers(__DRIdrawable *driDrawable,
                        unsigned int format,
                        uint32_t *stamp,
                        void *loaderPrivate,
                        uint32_t buffer_mask,
                        struct __DRIimageList *buffers)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   buffers->image_mask = 0;
   buffers->front = NULL;
   buffers->back = NULL;

   /* The EGL 1.5 spec states that pbuffers are single-buffered. Specifically,
    * the spec states that they have a back buffer but no front buffer, in
    * contrast to pixmaps, which have a front buffer but no back buffer.
    *
    * Single-buffered surfaces with no front buffer confuse Mesa; so we deviate
    * from the spec, following the precedent of Mesa's EGL X11 platform. The
    * X11 platform correctly assigns pbuffers to single-buffered configs, but
    * assigns the pbuffer a front buffer instead of a back buffer.
    *
    * Pbuffers in the X11 platform mostly work today, so let's just copy its
    * behavior instead of trying to fix (and hence potentially breaking) the
    * world.
    */

   if (buffer_mask & __DRI_IMAGE_BUFFER_FRONT) {

      if (!dri2_surf->front)
         dri2_surf->front =
            surfaceless_alloc_image(dri2_dpy, dri2_surf);

      buffers->image_mask |= __DRI_IMAGE_BUFFER_FRONT;
      buffers->front = dri2_surf->front;
   }

   return 1;
}

static _EGLSurface *
dri2_surfaceless_create_surface(_EGLDriver *drv, _EGLDisplay *disp, EGLint type,
                        _EGLConfig *conf, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   const __DRIconfig *config;

   /* Make sure to calloc so all pointers
    * are originally NULL.
    */
   dri2_surf = calloc(1, sizeof *dri2_surf);

   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "eglCreatePbufferSurface");
      return NULL;
   }

   if (!_eglInitSurface(&dri2_surf->base, disp, type, conf, attrib_list))
      goto cleanup_surface;

   config = dri2_get_dri_config(dri2_conf, type,
                                dri2_surf->base.GLColorspace);

   if (!config)
      goto cleanup_surface;

   dri2_surf->dri_drawable =
      (*dri2_dpy->dri2->createNewDrawable)(dri2_dpy->dri_screen, config,
                                           dri2_surf);
   if (dri2_surf->dri_drawable == NULL) {
      _eglError(EGL_BAD_ALLOC, "dri2->createNewDrawable");
      goto cleanup_surface;
    }

   if (conf->RedSize == 5)
      dri2_surf->visual = __DRI_IMAGE_FORMAT_RGB565;
   else if (conf->AlphaSize == 0)
      dri2_surf->visual = __DRI_IMAGE_FORMAT_XRGB8888;
   else
      dri2_surf->visual = __DRI_IMAGE_FORMAT_ARGB8888;

   return &dri2_surf->base;

   cleanup_surface:
      free(dri2_surf);
      return NULL;
}

static EGLBoolean
surfaceless_destroy_surface(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   if (!_eglPutSurface(surf))
      return EGL_TRUE;

   surfaceless_free_images(dri2_surf);

   (*dri2_dpy->core->destroyDrawable)(dri2_surf->dri_drawable);

   free(dri2_surf);
   return EGL_TRUE;
}

static _EGLSurface *
dri2_surfaceless_create_pbuffer_surface(_EGLDriver *drv, _EGLDisplay *disp,
                                _EGLConfig *conf, const EGLint *attrib_list)
{
   return dri2_surfaceless_create_surface(drv, disp, EGL_PBUFFER_BIT, conf,
                                  attrib_list);
}

static EGLBoolean
surfaceless_add_configs_for_visuals(_EGLDriver *drv, _EGLDisplay *dpy)
{

   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);

   unsigned int visuals[3][4] = {
      { 0xff0000, 0xff00, 0xff, 0xff000000 },   // ARGB8888
      { 0xff0000, 0xff00, 0xff, 0x0 },          // RGB888
      { 0xf800, 0x7e0, 0x1f, 0x0  },            // RGB565
   };

   int count, i, j;
   unsigned int r, b, g, a;

   count = 0;
   for (i = 0; i < ARRAY_SIZE(visuals); i++) {
      for (j = 0; dri2_dpy->driver_configs[j]; j++) {
         const EGLint surface_type = EGL_PBUFFER_BIT;
         struct dri2_egl_config *dri2_conf;

         /* Determine driver supported masks */
         dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[j],
                                       __DRI_ATTRIB_RED_MASK, &r);
         dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[j],
                                       __DRI_ATTRIB_BLUE_MASK, &b);
         dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[j],
                                       __DRI_ATTRIB_GREEN_MASK, &g);
         dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[j],
                                       __DRI_ATTRIB_ALPHA_MASK, &a);

         /* Compare with advertised visuals */
         if (r ^ visuals[i][0] || g ^ visuals[i][1]
            || b ^ visuals[i][2] || a ^ visuals[i][3])
            continue;

         dri2_conf = dri2_add_config(dpy, dri2_dpy->driver_configs[j],
               count + 1, surface_type, NULL, visuals[i]);

         if (dri2_conf)
            count++;
      }
   }

   if (!count)
         _eglLog(_EGL_DEBUG, "Can't create surfaceless visuals");

   return (count != 0);
}

static struct dri2_egl_display_vtbl dri2_surfaceless_display_vtbl = {
   .create_pixmap_surface = dri2_fallback_create_pixmap_surface,
   .create_pbuffer_surface = dri2_surfaceless_create_pbuffer_surface,
   .destroy_surface = surfaceless_destroy_surface,
   .create_image = dri2_create_image_khr,
   .swap_interval = dri2_fallback_swap_interval,
   .swap_buffers_with_damage = dri2_fallback_swap_buffers_with_damage,
   .swap_buffers_region = dri2_fallback_swap_buffers_region,
   .post_sub_buffer = dri2_fallback_post_sub_buffer,
   .copy_buffers = dri2_fallback_copy_buffers,
   .query_buffer_age = dri2_fallback_query_buffer_age,
   .create_wayland_buffer_from_image = dri2_fallback_create_wayland_buffer_from_image,
   .get_sync_values = dri2_fallback_get_sync_values,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static void
surfaceless_flush_front_buffer(__DRIdrawable *driDrawable, void *loaderPrivate)
{
}

static __DRIbuffer *
surfaceless_get_buffers_with_format(__DRIdrawable * driDrawable,
                             int *width, int *height,
                             unsigned int *attachments, int count,
                             int *out_count, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   dri2_surf->buffer_count = 1;
   if (width)
      *width = dri2_surf->base.Width;
   if (height)
      *height = dri2_surf->base.Height;
   *out_count = dri2_surf->buffer_count;
   return dri2_surf->buffers;
}

static const __DRIimageLoaderExtension image_loader_extension = {
   .base             = { __DRI_IMAGE_LOADER, 1 },
   .getBuffers       = surfaceless_image_get_buffers,
   .flushFrontBuffer = surfaceless_flush_front_buffer,
};

#define DRM_RENDER_DEV_NAME  "%s/renderD%d"

EGLBoolean
dri2_initialize_surfaceless(_EGLDriver *drv, _EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy;
   const char* err;
   int i;
   int driver_loaded = 0;

   loader_set_logger(_eglLog);

   dri2_dpy = calloc(1, sizeof *dri2_dpy);
   if (!dri2_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   disp->DriverData = (void *) dri2_dpy;

   const int limit = 64;
   const int base = 128;
   for (i = 0; i < limit; ++i) {
      char *card_path;
      if (asprintf(&card_path, DRM_RENDER_DEV_NAME, DRM_DIR_NAME, base + i) < 0)
         continue;

      dri2_dpy->fd = loader_open_device(card_path);

      free(card_path);
      if (dri2_dpy->fd < 0)
         continue;

      dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd, 0);
      if (dri2_dpy->driver_name) {
         if (dri2_load_driver(disp)) {
            driver_loaded = 1;
            break;
         }
         free(dri2_dpy->driver_name);
      }
      close(dri2_dpy->fd);
   }

   if (!driver_loaded) {
      err = "DRI2: failed to load driver";
      goto cleanup_display;
   }

   dri2_dpy->dri2_loader_extension.base.name = __DRI_DRI2_LOADER;
   dri2_dpy->dri2_loader_extension.base.version = 3;
   dri2_dpy->dri2_loader_extension.getBuffers = NULL;
   dri2_dpy->dri2_loader_extension.flushFrontBuffer =
      surfaceless_flush_front_buffer;
   dri2_dpy->dri2_loader_extension.getBuffersWithFormat =
      surfaceless_get_buffers_with_format;

   dri2_dpy->extensions[0] = &image_loader_extension.base;
   dri2_dpy->extensions[1] = &image_lookup_extension.base;
   dri2_dpy->extensions[2] = &use_invalidate.base;
   dri2_dpy->extensions[3] = NULL;

   if (!dri2_create_screen(disp)) {
      err = "DRI2: failed to create screen";
      goto cleanup_driver;
   }

   if (!surfaceless_add_configs_for_visuals(drv, disp)) {
      err = "DRI2: failed to add configs";
      goto cleanup_screen;
   }

   disp->Extensions.KHR_image_base = EGL_TRUE;

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = &dri2_surfaceless_display_vtbl;

   return EGL_TRUE;

cleanup_screen:
   dri2_dpy->core->destroyScreen(dri2_dpy->dri_screen);

cleanup_driver:
   dlclose(dri2_dpy->driver);
   free(dri2_dpy->driver_name);
   close(dri2_dpy->fd);
cleanup_display:
   free(dri2_dpy);

   return _eglError(EGL_NOT_INITIALIZED, err);
}
