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

/** @file vc4_tiling_lt.c
 *
 * Helper functions from vc4_tiling.c that will be compiled for using NEON
 * assembly or not.
 */

#include <string.h>
#include "pipe/p_state.h"
#include "vc4_tiling.h"

/** Returns the stride in bytes of a 64-byte microtile. */
static uint32_t
vc4_utile_stride(int cpp)
{
        switch (cpp) {
        case 1:
                return 8;
        case 2:
        case 4:
        case 8:
                return 16;
        default:
                unreachable("bad cpp");
        }
}

static void
vc4_load_utile(void *dst, void *src, uint32_t dst_stride, uint32_t cpp)
{
        uint32_t src_stride = vc4_utile_stride(cpp);

        for (uint32_t src_offset = 0; src_offset < 64; src_offset += src_stride) {
                memcpy(dst, src + src_offset, src_stride);
                dst += dst_stride;
        }
}

static void
vc4_store_utile(void *dst, void *src, uint32_t src_stride, uint32_t cpp)
{
        uint32_t dst_stride = vc4_utile_stride(cpp);

        for (uint32_t dst_offset = 0; dst_offset < 64; dst_offset += dst_stride) {
                memcpy(dst + dst_offset, src, dst_stride);
                src += src_stride;
        }
}

void
vc4_load_lt_image(void *dst, uint32_t dst_stride,
                  void *src, uint32_t src_stride,
                  int cpp, const struct pipe_box *box)
{
        uint32_t utile_w = vc4_utile_width(cpp);
        uint32_t utile_h = vc4_utile_height(cpp);
        uint32_t xstart = box->x;
        uint32_t ystart = box->y;

        for (uint32_t y = 0; y < box->height; y += utile_h) {
                for (int x = 0; x < box->width; x += utile_w) {
                        vc4_load_utile(dst + (dst_stride * y +
                                              x * cpp),
                                       src + ((ystart + y) * src_stride +
                                              (xstart + x) * 64 / utile_w),
                                       dst_stride, cpp);
                }
        }
}

void
vc4_store_lt_image(void *dst, uint32_t dst_stride,
                   void *src, uint32_t src_stride,
                   int cpp, const struct pipe_box *box)
{
        uint32_t utile_w = vc4_utile_width(cpp);
        uint32_t utile_h = vc4_utile_height(cpp);
        uint32_t xstart = box->x;
        uint32_t ystart = box->y;

        for (uint32_t y = 0; y < box->height; y += utile_h) {
                for (int x = 0; x < box->width; x += utile_w) {
                        vc4_store_utile(dst + ((ystart + y) * dst_stride +
                                               (xstart + x) * 64 / utile_w),
                                        src + (src_stride * y +
                                               x * cpp),
                                        src_stride, cpp);
                }
        }
}
