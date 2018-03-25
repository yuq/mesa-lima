/*
 * Copyright (c) 2017 Etnaviv Project
 * Copyright (C) 2017 Zodiac Inflight Innovations
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#ifndef H_ETNAVIV_QUERY_PM
#define H_ETNAVIV_QUERY_PM

#include "etnaviv_query.h"

struct etna_screen;

#define ETNA_QUERY_HI_TOTAL_CYCLES                       (ETNA_PM_QUERY_BASE + 0)
#define ETNA_QUERY_HI_IDLE_CYCLES                        (ETNA_PM_QUERY_BASE + 1)
#define ETNA_QUERY_HI_AXI_CYCLES_READ_REQUEST_STALLED    (ETNA_PM_QUERY_BASE + 2)
#define ETNA_QUERY_HI_AXI_CYCLES_WRITE_REQUEST_STALLED   (ETNA_PM_QUERY_BASE + 3)
#define ETNA_QUERY_HI_AXI_CYCLES_WRITE_DATA_STALLED      (ETNA_PM_QUERY_BASE + 4)

#define ETNA_QUERY_PE_PIXEL_COUNT_KILLED_BY_COLOR_PIPE   (ETNA_PM_QUERY_BASE + 5)
#define ETNA_QUERY_PE_PIXEL_COUNT_KILLED_BY_DEPTH_PIPE   (ETNA_PM_QUERY_BASE + 6)
#define ETNA_QUERY_PE_PIXEL_COUNT_DRAWN_BY_COLOR_PIPE    (ETNA_PM_QUERY_BASE + 7)
#define ETNA_QUERY_PE_PIXEL_COUNT_DRAWN_BY_DEPTH_PIPE    (ETNA_PM_QUERY_BASE + 8)
#define ETNA_QUERY_PE_PIXELS_RENDERED_2D                 (ETNA_PM_QUERY_BASE + 9)

#define ETNA_QUERY_SH_SHADER_CYCLES                      (ETNA_PM_QUERY_BASE + 10)
#define ETNA_QUERY_SH_PS_INST_COUNTER                    (ETNA_PM_QUERY_BASE + 11)
#define ETNA_QUERY_SH_RENDERED_PIXEL_COUNTER             (ETNA_PM_QUERY_BASE + 12)
#define ETNA_QUERY_SH_VS_INST_COUNTER                    (ETNA_PM_QUERY_BASE + 13)
#define ETNA_QUERY_SH_RENDERED_VERTICE_COUNTER           (ETNA_PM_QUERY_BASE + 14)
#define ETNA_QUERY_SH_VTX_BRANCH_INST_COUNTER            (ETNA_PM_QUERY_BASE + 15)
#define ETNA_QUERY_SH_VTX_TEXLD_INST_COUNTER             (ETNA_PM_QUERY_BASE + 16)
#define ETNA_QUERY_SH_PXL_BRANCH_INST_COUNTER            (ETNA_PM_QUERY_BASE + 17)
#define ETNA_QUERY_SH_PXL_TEXLD_INST_COUNTER             (ETNA_PM_QUERY_BASE + 18)

struct etna_pm_query {
   struct etna_query base;
   struct etna_perfmon_signal *signal;
   struct etna_bo *bo;
   uint32_t *data;
   uint32_t sequence;
   bool ready;
};

static inline struct etna_pm_query *
etna_pm_query(struct etna_query *q)
{
   return (struct etna_pm_query *)q;
}

void
etna_pm_query_setup(struct etna_screen *screen);

struct etna_query *
etna_pm_create_query(struct etna_context *ctx, unsigned query_type);

int
etna_pm_get_driver_query_info(struct pipe_screen *pscreen, unsigned index,
                              struct pipe_driver_query_info *info);

#endif
