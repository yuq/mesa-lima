/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
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

#include <stdlib.h>
#include <stdio.h>

#include "radv_debug.h"

bool
radv_init_trace(struct radv_device *device)
{
	struct radeon_winsys *ws = device->ws;

	device->trace_bo = ws->buffer_create(ws, 4096, 8, RADEON_DOMAIN_VRAM,
					     RADEON_FLAG_CPU_ACCESS);
	if (!device->trace_bo)
		return false;

	device->trace_id_ptr = ws->buffer_map(device->trace_bo);
	if (!device->trace_id_ptr)
		return false;

	return true;
}

void
radv_dump_trace(struct radv_device *device, struct radeon_winsys_cs *cs)
{
	const char *filename = getenv("RADV_TRACE_FILE");
	FILE *f = fopen(filename, "w");

	if (!f) {
		fprintf(stderr, "Failed to write trace dump to %s\n", filename);
		return;
	}

	fprintf(f, "Trace ID: %x\n", *device->trace_id_ptr);
	device->ws->cs_dump(cs, f, (const int*)device->trace_id_ptr, 2);
	fclose(f);
}
