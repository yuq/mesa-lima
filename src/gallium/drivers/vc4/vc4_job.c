/*
 * Copyright © 2014-2015 Broadcom
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

/** @file vc4_job.c
 *
 * Functions for submitting VC4 render jobs to the kernel.
 */

#include <xf86drm.h>
#include "vc4_context.h"

void
vc4_job_init(struct vc4_job *job)
{
        vc4_init_cl(job, &job->bcl);
        vc4_init_cl(job, &job->shader_rec);
        vc4_init_cl(job, &job->uniforms);
        vc4_init_cl(job, &job->bo_handles);
        vc4_init_cl(job, &job->bo_pointers);
        vc4_job_reset(job);
}

void
vc4_job_reset(struct vc4_job *job)
{
        struct vc4_bo **referenced_bos = job->bo_pointers.base;
        for (int i = 0; i < cl_offset(&job->bo_handles) / 4; i++) {
                vc4_bo_unreference(&referenced_bos[i]);
        }
        vc4_reset_cl(&job->bcl);
        vc4_reset_cl(&job->shader_rec);
        vc4_reset_cl(&job->uniforms);
        vc4_reset_cl(&job->bo_handles);
        vc4_reset_cl(&job->bo_pointers);
        job->shader_rec_count = 0;

        job->needs_flush = false;
        job->draw_calls_queued = 0;

        job->resolve = 0;
        job->cleared = 0;

        job->draw_min_x = ~0;
        job->draw_min_y = ~0;
        job->draw_max_x = 0;
        job->draw_max_y = 0;

        pipe_surface_reference(&job->color_write, NULL);
        pipe_surface_reference(&job->color_read, NULL);
        pipe_surface_reference(&job->msaa_color_write, NULL);
        pipe_surface_reference(&job->zs_write, NULL);
        pipe_surface_reference(&job->zs_read, NULL);
        pipe_surface_reference(&job->msaa_zs_write, NULL);
}

static void
vc4_submit_setup_rcl_surface(struct vc4_job *job,
                             struct drm_vc4_submit_rcl_surface *submit_surf,
                             struct pipe_surface *psurf,
                             bool is_depth, bool is_write)
{
        struct vc4_surface *surf = vc4_surface(psurf);

        if (!surf)
                return;

        struct vc4_resource *rsc = vc4_resource(psurf->texture);
        submit_surf->hindex = vc4_gem_hindex(job, rsc->bo);
        submit_surf->offset = surf->offset;

        if (psurf->texture->nr_samples <= 1) {
                if (is_depth) {
                        submit_surf->bits =
                                VC4_SET_FIELD(VC4_LOADSTORE_TILE_BUFFER_ZS,
                                              VC4_LOADSTORE_TILE_BUFFER_BUFFER);

                } else {
                        submit_surf->bits =
                                VC4_SET_FIELD(VC4_LOADSTORE_TILE_BUFFER_COLOR,
                                              VC4_LOADSTORE_TILE_BUFFER_BUFFER) |
                                VC4_SET_FIELD(vc4_rt_format_is_565(psurf->format) ?
                                              VC4_LOADSTORE_TILE_BUFFER_BGR565 :
                                              VC4_LOADSTORE_TILE_BUFFER_RGBA8888,
                                              VC4_LOADSTORE_TILE_BUFFER_FORMAT);
                }
                submit_surf->bits |=
                        VC4_SET_FIELD(surf->tiling,
                                      VC4_LOADSTORE_TILE_BUFFER_TILING);
        } else {
                assert(!is_write);
                submit_surf->flags |= VC4_SUBMIT_RCL_SURFACE_READ_IS_FULL_RES;
        }

        if (is_write)
                rsc->writes++;
}

static void
vc4_submit_setup_rcl_render_config_surface(struct vc4_job *job,
                                           struct drm_vc4_submit_rcl_surface *submit_surf,
                                           struct pipe_surface *psurf)
{
        struct vc4_surface *surf = vc4_surface(psurf);

        if (!surf)
                return;

        struct vc4_resource *rsc = vc4_resource(psurf->texture);
        submit_surf->hindex = vc4_gem_hindex(job, rsc->bo);
        submit_surf->offset = surf->offset;

        if (psurf->texture->nr_samples <= 1) {
                submit_surf->bits =
                        VC4_SET_FIELD(vc4_rt_format_is_565(surf->base.format) ?
                                      VC4_RENDER_CONFIG_FORMAT_BGR565 :
                                      VC4_RENDER_CONFIG_FORMAT_RGBA8888,
                                      VC4_RENDER_CONFIG_FORMAT) |
                        VC4_SET_FIELD(surf->tiling,
                                      VC4_RENDER_CONFIG_MEMORY_FORMAT);
        }

        rsc->writes++;
}

static void
vc4_submit_setup_rcl_msaa_surface(struct vc4_job *job,
                                  struct drm_vc4_submit_rcl_surface *submit_surf,
                                  struct pipe_surface *psurf)
{
        struct vc4_surface *surf = vc4_surface(psurf);

        if (!surf)
                return;

        struct vc4_resource *rsc = vc4_resource(psurf->texture);
        submit_surf->hindex = vc4_gem_hindex(job, rsc->bo);
        submit_surf->offset = surf->offset;
        submit_surf->bits = 0;
        rsc->writes++;
}

/**
 * Submits the job to the kernel and then reinitializes it.
 */
void
vc4_job_submit(struct vc4_context *vc4, struct vc4_job *job)
{
        if (!job->needs_flush)
                return;

        /* The RCL setup would choke if the draw bounds cause no drawing, so
         * just drop the drawing if that's the case.
         */
        if (job->draw_max_x <= job->draw_min_x ||
            job->draw_max_y <= job->draw_min_y) {
                vc4_job_reset(job);
                return;
        }

        if (vc4_debug & VC4_DEBUG_CL) {
                fprintf(stderr, "BCL:\n");
                vc4_dump_cl(job->bcl.base, cl_offset(&job->bcl), false);
        }

        if (cl_offset(&job->bcl) > 0) {
                /* Increment the semaphore indicating that binning is done and
                 * unblocking the render thread.  Note that this doesn't act
                 * until the FLUSH completes.
                 */
                cl_ensure_space(&job->bcl, 8);
                struct vc4_cl_out *bcl = cl_start(&job->bcl);
                cl_u8(&bcl, VC4_PACKET_INCREMENT_SEMAPHORE);
                /* The FLUSH caps all of our bin lists with a
                 * VC4_PACKET_RETURN.
                 */
                cl_u8(&bcl, VC4_PACKET_FLUSH);
                cl_end(&job->bcl, bcl);
        }
        struct drm_vc4_submit_cl submit = {
                .color_read.hindex = ~0,
                .zs_read.hindex = ~0,
                .color_write.hindex = ~0,
                .msaa_color_write.hindex = ~0,
                .zs_write.hindex = ~0,
                .msaa_zs_write.hindex = ~0,
        };

        cl_ensure_space(&job->bo_handles, 6 * sizeof(uint32_t));
        cl_ensure_space(&job->bo_pointers, 6 * sizeof(struct vc4_bo *));

        if (job->resolve & PIPE_CLEAR_COLOR) {
                if (!(job->cleared & PIPE_CLEAR_COLOR)) {
                        vc4_submit_setup_rcl_surface(job, &submit.color_read,
                                                     job->color_read,
                                                     false, false);
                }
                vc4_submit_setup_rcl_render_config_surface(job,
                                                           &submit.color_write,
                                                           job->color_write);
                vc4_submit_setup_rcl_msaa_surface(job,
                                                  &submit.msaa_color_write,
                                                  job->msaa_color_write);
        }
        if (job->resolve & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL)) {
                if (!(job->cleared & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL))) {
                        vc4_submit_setup_rcl_surface(job, &submit.zs_read,
                                                     job->zs_read, true, false);
                }
                vc4_submit_setup_rcl_surface(job, &submit.zs_write,
                                             job->zs_write, true, true);
                vc4_submit_setup_rcl_msaa_surface(job, &submit.msaa_zs_write,
                                                  job->msaa_zs_write);
        }

        if (job->msaa) {
                /* This bit controls how many pixels the general
                 * (i.e. subsampled) loads/stores are iterating over
                 * (multisample loads replicate out to the other samples).
                 */
                submit.color_write.bits |= VC4_RENDER_CONFIG_MS_MODE_4X;
                /* Controls whether color_write's
                 * VC4_PACKET_STORE_MS_TILE_BUFFER does 4x decimation
                 */
                submit.color_write.bits |= VC4_RENDER_CONFIG_DECIMATE_MODE_4X;
        }

        submit.bo_handles = (uintptr_t)job->bo_handles.base;
        submit.bo_handle_count = cl_offset(&job->bo_handles) / 4;
        submit.bin_cl = (uintptr_t)job->bcl.base;
        submit.bin_cl_size = cl_offset(&job->bcl);
        submit.shader_rec = (uintptr_t)job->shader_rec.base;
        submit.shader_rec_size = cl_offset(&job->shader_rec);
        submit.shader_rec_count = job->shader_rec_count;
        submit.uniforms = (uintptr_t)job->uniforms.base;
        submit.uniforms_size = cl_offset(&job->uniforms);

        assert(job->draw_min_x != ~0 && job->draw_min_y != ~0);
        submit.min_x_tile = job->draw_min_x / job->tile_width;
        submit.min_y_tile = job->draw_min_y / job->tile_height;
        submit.max_x_tile = (job->draw_max_x - 1) / job->tile_width;
        submit.max_y_tile = (job->draw_max_y - 1) / job->tile_height;
        submit.width = job->draw_width;
        submit.height = job->draw_height;
        if (job->cleared) {
                submit.flags |= VC4_SUBMIT_CL_USE_CLEAR_COLOR;
                submit.clear_color[0] = job->clear_color[0];
                submit.clear_color[1] = job->clear_color[1];
                submit.clear_z = job->clear_depth;
                submit.clear_s = job->clear_stencil;
        }

        if (!(vc4_debug & VC4_DEBUG_NORAST)) {
                int ret;

#ifndef USE_VC4_SIMULATOR
                ret = drmIoctl(vc4->fd, DRM_IOCTL_VC4_SUBMIT_CL, &submit);
#else
                ret = vc4_simulator_flush(vc4, &submit);
#endif
                static bool warned = false;
                if (ret && !warned) {
                        fprintf(stderr, "Draw call returned %s.  "
                                        "Expect corruption.\n", strerror(errno));
                        warned = true;
                } else if (!ret) {
                        vc4->last_emit_seqno = submit.seqno;
                }
        }

        if (vc4->last_emit_seqno - vc4->screen->finished_seqno > 5) {
                if (!vc4_wait_seqno(vc4->screen,
                                    vc4->last_emit_seqno - 5,
                                    PIPE_TIMEOUT_INFINITE,
                                    "job throttling")) {
                        fprintf(stderr, "Job throttling failed\n");
                }
        }

        if (vc4_debug & VC4_DEBUG_ALWAYS_SYNC) {
                if (!vc4_wait_seqno(vc4->screen, vc4->last_emit_seqno,
                                    PIPE_TIMEOUT_INFINITE, "sync")) {
                        fprintf(stderr, "Wait failed.\n");
                        abort();
                }
        }

        vc4_job_reset(vc4->job);
}
