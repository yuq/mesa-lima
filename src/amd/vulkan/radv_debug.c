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

#include "sid.h"
#include "gfx9d.h"
#include "ac_debug.h"
#include "radv_debug.h"
#include "radv_shader.h"

#define TRACE_BO_SIZE 4096

/* Trace BO layout (offsets are 4 bytes):
 *
 * [0]: primary trace ID
 * [1]: secondary trace ID
 * [2-3]: 64-bit GFX pipeline pointer
 * [4-5]: 64-bit COMPUTE pipeline pointer
 */

bool
radv_init_trace(struct radv_device *device)
{
	struct radeon_winsys *ws = device->ws;

	device->trace_bo = ws->buffer_create(ws, TRACE_BO_SIZE, 8,
					     RADEON_DOMAIN_VRAM,
					     RADEON_FLAG_CPU_ACCESS);
	if (!device->trace_bo)
		return false;

	device->trace_id_ptr = ws->buffer_map(device->trace_bo);
	if (!device->trace_id_ptr)
		return false;

	memset(device->trace_id_ptr, 0, TRACE_BO_SIZE);

	ac_vm_fault_occured(device->physical_device->rad_info.chip_class,
			    &device->dmesg_timestamp, NULL);

	return true;
}

static void
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

static void
radv_dump_mmapped_reg(struct radv_device *device, FILE *f, unsigned offset)
{
	struct radeon_winsys *ws = device->ws;
	uint32_t value;

	if (ws->read_registers(ws, offset, 1, &value))
		ac_dump_reg(f, device->physical_device->rad_info.chip_class,
			    offset, value, ~0);
}

static void
radv_dump_debug_registers(struct radv_device *device, FILE *f)
{
	struct radeon_info *info = &device->physical_device->rad_info;

	if (info->drm_major == 2 && info->drm_minor < 42)
		return; /* no radeon support */

	fprintf(f, "Memory-mapped registers:\n");
	radv_dump_mmapped_reg(device, f, R_008010_GRBM_STATUS);

	/* No other registers can be read on DRM < 3.1.0. */
	if (info->drm_major < 3 || info->drm_minor < 1) {
		fprintf(f, "\n");
		return;
	}

	radv_dump_mmapped_reg(device, f, R_008008_GRBM_STATUS2);
	radv_dump_mmapped_reg(device, f, R_008014_GRBM_STATUS_SE0);
	radv_dump_mmapped_reg(device, f, R_008018_GRBM_STATUS_SE1);
	radv_dump_mmapped_reg(device, f, R_008038_GRBM_STATUS_SE2);
	radv_dump_mmapped_reg(device, f, R_00803C_GRBM_STATUS_SE3);
	radv_dump_mmapped_reg(device, f, R_00D034_SDMA0_STATUS_REG);
	radv_dump_mmapped_reg(device, f, R_00D834_SDMA1_STATUS_REG);
	if (info->chip_class <= VI) {
		radv_dump_mmapped_reg(device, f, R_000E50_SRBM_STATUS);
		radv_dump_mmapped_reg(device, f, R_000E4C_SRBM_STATUS2);
		radv_dump_mmapped_reg(device, f, R_000E54_SRBM_STATUS3);
	}
	radv_dump_mmapped_reg(device, f, R_008680_CP_STAT);
	radv_dump_mmapped_reg(device, f, R_008674_CP_STALLED_STAT1);
	radv_dump_mmapped_reg(device, f, R_008678_CP_STALLED_STAT2);
	radv_dump_mmapped_reg(device, f, R_008670_CP_STALLED_STAT3);
	radv_dump_mmapped_reg(device, f, R_008210_CP_CPC_STATUS);
	radv_dump_mmapped_reg(device, f, R_008214_CP_CPC_BUSY_STAT);
	radv_dump_mmapped_reg(device, f, R_008218_CP_CPC_STALLED_STAT1);
	radv_dump_mmapped_reg(device, f, R_00821C_CP_CPF_STATUS);
	radv_dump_mmapped_reg(device, f, R_008220_CP_CPF_BUSY_STAT);
	radv_dump_mmapped_reg(device, f, R_008224_CP_CPF_STALLED_STAT1);
	fprintf(f, "\n");
}

static void
radv_dump_shader(struct radv_pipeline *pipeline,
		 struct radv_shader_variant *shader, gl_shader_stage stage,
		 FILE *f)
{
	if (!shader)
		return;

	fprintf(f, "%s:\n%s\n\n", radv_get_shader_name(shader, stage),
		shader->disasm_string);

	radv_shader_dump_stats(pipeline->device, shader, stage, f);
}

static void
radv_dump_shaders(struct radv_pipeline *pipeline,
		  struct radv_shader_variant *compute_shader, FILE *f)
{
	unsigned mask;

	/* Dump active graphics shaders. */
	mask = pipeline->active_stages;
	while (mask) {
		int stage = u_bit_scan(&mask);

		radv_dump_shader(pipeline, pipeline->shaders[stage], stage, f);
	}

	radv_dump_shader(pipeline, compute_shader, MESA_SHADER_COMPUTE, f);
}

static void
radv_dump_graphics_state(struct radv_pipeline *graphics_pipeline,
			 struct radv_pipeline *compute_pipeline, FILE *f)
{
	struct radv_shader_variant *compute_shader =
		compute_pipeline ? compute_pipeline->shaders[MESA_SHADER_COMPUTE] : NULL;

	if (!graphics_pipeline)
		return;

	radv_dump_shaders(graphics_pipeline, compute_shader, f);
}

static void
radv_dump_compute_state(struct radv_pipeline *compute_pipeline, FILE *f)
{
	if (!compute_pipeline)
		return;

	radv_dump_shaders(compute_pipeline,
			  compute_pipeline->shaders[MESA_SHADER_COMPUTE], f);
}

static struct radv_pipeline *
radv_get_saved_graphics_pipeline(struct radv_device *device)
{
	uint64_t *ptr = (uint64_t *)device->trace_id_ptr;

	return (struct radv_pipeline *)ptr[1];
}

static struct radv_pipeline *
radv_get_saved_compute_pipeline(struct radv_device *device)
{
	uint64_t *ptr = (uint64_t *)device->trace_id_ptr;

	return (struct radv_pipeline *)ptr[2];
}

static bool
radv_gpu_hang_occured(struct radv_queue *queue, enum ring_type ring)
{
	struct radeon_winsys *ws = queue->device->ws;

	if (!ws->ctx_wait_idle(queue->hw_ctx, ring, queue->queue_idx))
		return true;

	return false;
}

void
radv_check_gpu_hangs(struct radv_queue *queue, struct radeon_winsys_cs *cs)
{
	struct radv_pipeline *graphics_pipeline, *compute_pipeline;
	struct radv_device *device = queue->device;
	enum ring_type ring;
	uint64_t addr;

	ring = radv_queue_family_to_ring(queue->queue_family_index);

	bool hang_occurred = radv_gpu_hang_occured(queue, ring);
	bool vm_fault_occurred = false;
	if (queue->device->instance->debug_flags & RADV_DEBUG_VM_FAULTS)
		vm_fault_occurred = ac_vm_fault_occured(device->physical_device->rad_info.chip_class,
		                                        &device->dmesg_timestamp, &addr);
	if (!hang_occurred && !vm_fault_occurred)
		return;

	graphics_pipeline = radv_get_saved_graphics_pipeline(device);
	compute_pipeline = radv_get_saved_compute_pipeline(device);

	if (vm_fault_occurred) {
		fprintf(stderr, "VM fault report.\n\n");
		fprintf(stderr, "Failing VM page: 0x%08"PRIx64"\n\n", addr);
	}

	radv_dump_debug_registers(device, stderr);

	switch (ring) {
	case RING_GFX:
		radv_dump_graphics_state(graphics_pipeline, compute_pipeline,
					 stderr);
		break;
	case RING_COMPUTE:
		radv_dump_compute_state(compute_pipeline, stderr);
		break;
	default:
		assert(0);
		break;
	}

	radv_dump_trace(queue->device, cs);
	abort();
}

void
radv_print_spirv(struct radv_shader_module *module, FILE *fp)
{
	char path[] = "/tmp/fileXXXXXX";
	char line[2048], command[128];
	FILE *p;
	int fd;

	/* Dump the binary into a temporary file. */
	fd = mkstemp(path);
	if (fd < 0)
		return;

	if (write(fd, module->data, module->size) == -1)
		goto fail;

	sprintf(command, "spirv-dis %s", path);

	/* Disassemble using spirv-dis if installed. */
	p = popen(command, "r");
	if (p) {
		while (fgets(line, sizeof(line), p))
			fprintf(fp, "%s", line);
		pclose(p);
	}

fail:
	close(fd);
	unlink(path);
}
