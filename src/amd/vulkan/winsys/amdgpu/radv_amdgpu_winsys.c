/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * based on amdgpu winsys.
 * Copyright © 2011 Marek Olšák <maraeo@gmail.com>
 * Copyright © 2015 Advanced Micro Devices, Inc.
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
#include "radv_amdgpu_winsys.h"
#include "radv_amdgpu_winsys_public.h"
#include "radv_amdgpu_surface.h"
#include "radv_debug.h"
#include "amdgpu_id.h"
#include "xf86drm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <amdgpu_drm.h>
#include <assert.h>
#include "radv_amdgpu_cs.h"
#include "radv_amdgpu_bo.h"
#include "radv_amdgpu_surface.h"

static bool
do_winsys_init(struct radv_amdgpu_winsys *ws, int fd)
{
	if (!ac_query_gpu_info(fd, ws->dev, &ws->info, &ws->amdinfo))
		goto fail;

	if (ws->info.chip_class >= GFX9) {
		fprintf(stderr, "radv: GFX9 is not supported.\n");
		goto fail;
	}

	/* family and rev_id are for addrlib */
	switch (ws->info.family) {
	case CHIP_TAHITI:
		ws->family = FAMILY_SI;
		ws->rev_id = SI_TAHITI_P_A0;
		break;
	case CHIP_PITCAIRN:
		ws->family = FAMILY_SI;
		ws->rev_id = SI_PITCAIRN_PM_A0;
	  break;
	case CHIP_VERDE:
		ws->family = FAMILY_SI;
		ws->rev_id = SI_CAPEVERDE_M_A0;
		break;
	case CHIP_OLAND:
		ws->family = FAMILY_SI;
		ws->rev_id = SI_OLAND_M_A0;
		break;
	case CHIP_HAINAN:
		ws->family = FAMILY_SI;
		ws->rev_id = SI_HAINAN_V_A0;
		break;
	case CHIP_BONAIRE:
		ws->family = FAMILY_CI;
		ws->rev_id = CI_BONAIRE_M_A0;
		break;
	case CHIP_KAVERI:
		ws->family = FAMILY_KV;
		ws->rev_id = KV_SPECTRE_A0;
		break;
	case CHIP_KABINI:
		ws->family = FAMILY_KV;
		ws->rev_id = KB_KALINDI_A0;
		break;
	case CHIP_HAWAII:
		ws->family = FAMILY_CI;
		ws->rev_id = CI_HAWAII_P_A0;
		break;
	case CHIP_MULLINS:
		ws->family = FAMILY_KV;
		ws->rev_id = ML_GODAVARI_A0;
		break;
	case CHIP_TONGA:
		ws->family = FAMILY_VI;
		ws->rev_id = VI_TONGA_P_A0;
		break;
	case CHIP_ICELAND:
		ws->family = FAMILY_VI;
		ws->rev_id = VI_ICELAND_M_A0;
		break;
	case CHIP_CARRIZO:
		ws->family = FAMILY_CZ;
		ws->rev_id = CARRIZO_A0;
		break;
	case CHIP_STONEY:
		ws->family = FAMILY_CZ;
		ws->rev_id = STONEY_A0;
		break;
	case CHIP_FIJI:
		ws->family = FAMILY_VI;
		ws->rev_id = VI_FIJI_P_A0;
		break;
	case CHIP_POLARIS10:
		ws->family = FAMILY_VI;
		ws->rev_id = VI_POLARIS10_P_A0;
		break;
	case CHIP_POLARIS11:
		ws->family = FAMILY_VI;
		ws->rev_id = VI_POLARIS11_M_A0;
		break;
	case CHIP_POLARIS12:
		ws->family = FAMILY_VI;
		ws->rev_id = VI_POLARIS12_V_A0;
		break;
	default:
		fprintf(stderr, "amdgpu: Unknown family.\n");
		goto fail;
	}

	ws->addrlib = radv_amdgpu_addr_create(&ws->amdinfo, ws->family, ws->rev_id, ws->info.chip_class);
	if (!ws->addrlib) {
		fprintf(stderr, "amdgpu: Cannot create addrlib.\n");
		goto fail;
	}

	ws->info.num_sdma_rings = MIN2(ws->info.num_sdma_rings, MAX_RINGS_PER_TYPE);
	ws->info.num_compute_rings = MIN2(ws->info.num_compute_rings, MAX_RINGS_PER_TYPE);

	ws->use_ib_bos = ws->info.chip_class >= CIK;
	return true;
fail:
	return false;
}

static void radv_amdgpu_winsys_query_info(struct radeon_winsys *rws,
                                     struct radeon_info *info)
{
	*info = ((struct radv_amdgpu_winsys *)rws)->info;
}

static void radv_amdgpu_winsys_destroy(struct radeon_winsys *rws)
{
	struct radv_amdgpu_winsys *ws = (struct radv_amdgpu_winsys*)rws;

	AddrDestroy(ws->addrlib);
	amdgpu_device_deinitialize(ws->dev);
	FREE(rws);
}

struct radeon_winsys *
radv_amdgpu_winsys_create(int fd, uint32_t debug_flags)
{
	uint32_t drm_major, drm_minor, r;
	amdgpu_device_handle dev;
	struct radv_amdgpu_winsys *ws;

	r = amdgpu_device_initialize(fd, &drm_major, &drm_minor, &dev);
	if (r)
		return NULL;

	ws = calloc(1, sizeof(struct radv_amdgpu_winsys));
	if (!ws)
		goto fail;

	ws->dev = dev;
	ws->info.drm_major = drm_major;
	ws->info.drm_minor = drm_minor;
	if (!do_winsys_init(ws, fd))
		goto winsys_fail;

	ws->debug_all_bos = !!(debug_flags & RADV_DEBUG_ALL_BOS);
	if (debug_flags & RADV_DEBUG_NO_IBS)
		ws->use_ib_bos = false;

	LIST_INITHEAD(&ws->global_bo_list);
	pthread_mutex_init(&ws->global_bo_list_lock, NULL);
	ws->base.query_info = radv_amdgpu_winsys_query_info;
	ws->base.destroy = radv_amdgpu_winsys_destroy;
	radv_amdgpu_bo_init_functions(ws);
	radv_amdgpu_cs_init_functions(ws);
	radv_amdgpu_surface_init_functions(ws);

	return &ws->base;

winsys_fail:
	free(ws);
fail:
	amdgpu_device_deinitialize(dev);
	return NULL;
}
