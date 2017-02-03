/*
 * Copyright © 2017 Broadcom
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

#include "util/u_format.h"
#include "vc5_context.h"
#include "vc5_tiling.h"
#include "broadcom/cle/v3d_packet_v33_pack.h"

void
vc5_emit_rcl(struct vc5_job *job)
{
        uint32_t min_x_tile = job->draw_min_x / job->tile_width;
        uint32_t min_y_tile = job->draw_min_y / job->tile_height;
        uint32_t max_x_tile = (job->draw_max_x - 1) / job->tile_width;
        uint32_t max_y_tile = (job->draw_max_y - 1) / job->tile_height;

        /* The RCL list should be empty. */
        assert(!job->rcl.bo);

        vc5_cl_ensure_space(&job->rcl,
                            256 +
                            (64 *
                             (max_x_tile - min_x_tile + 1) *
                             (max_y_tile - min_y_tile + 1)), 1);

        job->submit.rcl_start = job->rcl.bo->offset;
        vc5_job_add_bo(job, job->rcl.bo);

        int nr_cbufs = 0;
        for (int i = 0; i < VC5_MAX_DRAW_BUFFERS; i++) {
                if (job->cbufs[i])
                        nr_cbufs = i + 1;
        }

        /* Comon config must be the first TILE_RENDERING_MODE_CONFIGURATION
         * and Z_STENCIL_CLEAR_VALUES must be last.  The ones in between are
         * optional updates to the previous HW state.
         */
        cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_COMMON_CONFIGURATION,
                config) {
                config.enable_z_store = job->resolve & PIPE_CLEAR_DEPTH;
                config.enable_stencil_store = job->resolve & PIPE_CLEAR_STENCIL;

                config.early_z_disable = !job->uses_early_z;

                config.image_width_pixels = job->draw_width;
                config.image_height_pixels = job->draw_height;

                config.number_of_render_targets_minus_1 =
                        MAX2(nr_cbufs, 1) - 1;

                config.maximum_bpp_of_all_render_targets = job->internal_bpp;
        }

        for (int i = 0; i < nr_cbufs; i++) {
                cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_RENDER_TARGET_CONFIG, rt) {
                        struct pipe_surface *psurf = job->cbufs[i];
                        if (!psurf)
                                continue;

                        struct vc5_surface *surf = vc5_surface(psurf);
                        struct vc5_resource *rsc = vc5_resource(psurf->texture);
                        rt.address = cl_address(rsc->bo, surf->offset);
                        rt.internal_type = surf->internal_type;
                        rt.output_image_format = surf->format;
                        rt.memory_format = surf->tiling;
                        rt.internal_bpp = surf->internal_bpp;
                        rt.render_target_number = i;

                        if (job->resolve & PIPE_CLEAR_COLOR0 << i)
                                rsc->writes++;
                }
        }

        /* TODO: Don't bother emitting if we don't load/clear Z/S. */
        if (job->zsbuf) {
                struct pipe_surface *psurf = job->zsbuf;
                struct vc5_surface *surf = vc5_surface(psurf);
                struct vc5_resource *rsc = vc5_resource(psurf->texture);

                cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_Z_STENCIL_CONFIG, zs) {
                        zs.address = cl_address(rsc->bo, surf->offset);

                        zs.internal_type = surf->internal_type;
                        zs.output_image_format = surf->format;

                        struct vc5_resource_slice *slice = &rsc->slices[psurf->u.tex.level];
                        /* XXX */
                        zs.padded_height_of_output_image_in_uif_blocks =
                                (slice->size / slice->stride) / (2 * vc5_utile_height(rsc->cpp));

                        assert(surf->tiling != VC5_TILING_RASTER);
                        zs.memory_format = surf->tiling;
                }

                if (job->resolve & PIPE_CLEAR_DEPTHSTENCIL)
                        rsc->writes++;
        }

        cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_CLEAR_COLORS_PART1,
                clear) {
                clear.clear_color_low_32_bits = job->clear_color[0];
        };

        /* Ends rendering mode config. */
        cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_Z_STENCIL_CLEAR_VALUES,
                clear) {
                clear.z_s_clear_value = job->clear_zs;
        };

        /* Always set initial block size before the first branch, which needs
         * to match the value from binning mode config.
         */
        cl_emit(&job->rcl, TILE_LIST_INITIAL_BLOCK_SIZE, init) {
                init.use_auto_chained_tile_lists = true;
                init.size_of_first_block_in_chained_tile_lists =
                        TILE_ALLOCATION_BLOCK_SIZE_64B;
        }

        cl_emit(&job->rcl, WAIT_ON_SEMAPHORE, sem);

        /* Start by clearing the tile buffer. */
        cl_emit(&job->rcl, TILE_COORDINATES, coords) {
                coords.tile_column_number = 0;
                coords.tile_row_number = 0;
        }

        cl_emit(&job->rcl, STORE_TILE_BUFFER_GENERAL, store) {
                store.buffer_to_store = NONE;
        }

        cl_emit(&job->rcl, FLUSH_VCD_CACHE, flush);

        const uint32_t pipe_clear_color_buffers = (PIPE_CLEAR_COLOR0 |
                                                   PIPE_CLEAR_COLOR1 |
                                                   PIPE_CLEAR_COLOR2 |
                                                   PIPE_CLEAR_COLOR3);
        const uint32_t first_color_buffer_bit = (ffs(PIPE_CLEAR_COLOR0) - 1);

        for (int y = min_y_tile; y <= max_y_tile; y++) {
                for (int x = min_x_tile; x <= max_x_tile; x++) {
                        uint32_t read_but_not_cleared = job->resolve & ~job->cleared;

                        /* The initial reload will be queued until we get the
                         * tile coordinates.
                         */
                        if (read_but_not_cleared) {
                                cl_emit(&job->rcl, RELOAD_TILE_COLOUR_BUFFER, load) {
                                        load.disable_colour_buffer_load =
                                                (~read_but_not_cleared & pipe_clear_color_buffers) >>
                                                first_color_buffer_bit;
                                        load.enable_z_load =
                                                read_but_not_cleared & PIPE_CLEAR_DEPTH;
                                        load.enable_stencil_load =
                                                read_but_not_cleared & PIPE_CLEAR_STENCIL;
                                }
                        }

                        /* Tile Coordinates triggers the reload and sets where
                         * the stores go. There must be one per store packet.
                         */
                        cl_emit(&job->rcl, TILE_COORDINATES, coords) {
                                coords.tile_column_number = x;
                                coords.tile_row_number = y;
                        }

                        cl_emit(&job->rcl, BRANCH_TO_AUTO_CHAINED_SUB_LIST, branch) {
                                uint32_t bin_tile_stride =
                                        (align(job->draw_width,
                                               job->tile_width) /
                                         job->tile_width);
                                uint32_t bin_index =
                                        (y * bin_tile_stride + x);
                                branch.address = cl_address(job->tile_alloc,
                                                            64 * bin_index);
                        }

                        cl_emit(&job->rcl, STORE_MULTI_SAMPLE_RESOLVED_TILE_COLOR_BUFFER_EXTENDED, store) {
                                uint32_t color_write_enables =
                                        job->resolve >> first_color_buffer_bit;

                                store.disable_color_buffer_write = (~color_write_enables) & 0xf;
                                store.enable_z_write = job->resolve & PIPE_CLEAR_DEPTH;
                                store.enable_stencil_write = job->resolve & PIPE_CLEAR_STENCIL;

                                store.disable_colour_buffers_clear_on_write =
                                        (job->cleared & pipe_clear_color_buffers) == 0;
                                store.disable_z_buffer_clear_on_write =
                                        !(job->cleared & PIPE_CLEAR_DEPTH);
                                store.disable_stencil_buffer_clear_on_write =
                                        !(job->cleared & PIPE_CLEAR_STENCIL);

                                store.last_tile_of_frame = (x == max_x_tile &&
                                                            y == max_y_tile);
                        };
                }
        }
}
