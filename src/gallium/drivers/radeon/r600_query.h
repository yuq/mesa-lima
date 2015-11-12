/*
 * Copyright 2015 Advanced Micro Devices, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *  Nicolai Hähnle <nicolai.haehnle@amd.com>
 *
 */

#ifndef R600_QUERY_H
#define R600_QUERY_H

#include "pipe/p_defines.h"

#define R600_QUERY_DRAW_CALLS		(PIPE_QUERY_DRIVER_SPECIFIC + 0)
#define R600_QUERY_REQUESTED_VRAM	(PIPE_QUERY_DRIVER_SPECIFIC + 1)
#define R600_QUERY_REQUESTED_GTT	(PIPE_QUERY_DRIVER_SPECIFIC + 2)
#define R600_QUERY_BUFFER_WAIT_TIME	(PIPE_QUERY_DRIVER_SPECIFIC + 3)
#define R600_QUERY_NUM_CS_FLUSHES	(PIPE_QUERY_DRIVER_SPECIFIC + 4)
#define R600_QUERY_NUM_BYTES_MOVED	(PIPE_QUERY_DRIVER_SPECIFIC + 5)
#define R600_QUERY_VRAM_USAGE		(PIPE_QUERY_DRIVER_SPECIFIC + 6)
#define R600_QUERY_GTT_USAGE		(PIPE_QUERY_DRIVER_SPECIFIC + 7)
#define R600_QUERY_GPU_TEMPERATURE	(PIPE_QUERY_DRIVER_SPECIFIC + 8)
#define R600_QUERY_CURRENT_GPU_SCLK	(PIPE_QUERY_DRIVER_SPECIFIC + 9)
#define R600_QUERY_CURRENT_GPU_MCLK	(PIPE_QUERY_DRIVER_SPECIFIC + 10)
#define R600_QUERY_GPU_LOAD		(PIPE_QUERY_DRIVER_SPECIFIC + 11)
#define R600_QUERY_NUM_COMPILATIONS	(PIPE_QUERY_DRIVER_SPECIFIC + 12)
#define R600_QUERY_NUM_SHADERS_CREATED	(PIPE_QUERY_DRIVER_SPECIFIC + 13)
#define R600_QUERY_FIRST_PERFCOUNTER	(PIPE_QUERY_DRIVER_SPECIFIC + 100)

#endif /* R600_QUERY_H */
