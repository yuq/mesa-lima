/*
 * Copyright © 2011 Red Hat All Rights Reserved.
 * Copyright © 2017 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS, AUTHORS
 * AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 */

#include "ac_surface.h"
#include "amdgpu_id.h"
#include "util/macros.h"
#include "util/u_math.h"

#include <stdio.h>
#include <stdlib.h>
#include <amdgpu.h>

#include "addrlib/addrinterface.h"

#ifndef CIASICIDGFXENGINE_SOUTHERNISLAND
#define CIASICIDGFXENGINE_SOUTHERNISLAND 0x0000000A
#endif

#ifndef CIASICIDGFXENGINE_ARCTICISLAND
#define CIASICIDGFXENGINE_ARCTICISLAND 0x0000000D
#endif

static void addrlib_family_rev_id(enum radeon_family family,
				  unsigned *addrlib_family,
				  unsigned *addrlib_revid)
{
	switch (family) {
	case CHIP_TAHITI:
		*addrlib_family = FAMILY_SI;
		*addrlib_revid = SI_TAHITI_P_A0;
		break;
	case CHIP_PITCAIRN:
		*addrlib_family = FAMILY_SI;
		*addrlib_revid = SI_PITCAIRN_PM_A0;
		break;
	case CHIP_VERDE:
		*addrlib_family = FAMILY_SI;
		*addrlib_revid = SI_CAPEVERDE_M_A0;
		break;
	case CHIP_OLAND:
		*addrlib_family = FAMILY_SI;
		*addrlib_revid = SI_OLAND_M_A0;
		break;
	case CHIP_HAINAN:
		*addrlib_family = FAMILY_SI;
		*addrlib_revid = SI_HAINAN_V_A0;
		break;
	case CHIP_BONAIRE:
		*addrlib_family = FAMILY_CI;
		*addrlib_revid = CI_BONAIRE_M_A0;
		break;
	case CHIP_KAVERI:
		*addrlib_family = FAMILY_KV;
		*addrlib_revid = KV_SPECTRE_A0;
		break;
	case CHIP_KABINI:
		*addrlib_family = FAMILY_KV;
		*addrlib_revid = KB_KALINDI_A0;
		break;
	case CHIP_HAWAII:
		*addrlib_family = FAMILY_CI;
		*addrlib_revid = CI_HAWAII_P_A0;
		break;
	case CHIP_MULLINS:
		*addrlib_family = FAMILY_KV;
		*addrlib_revid = ML_GODAVARI_A0;
		break;
	case CHIP_TONGA:
		*addrlib_family = FAMILY_VI;
		*addrlib_revid = VI_TONGA_P_A0;
		break;
	case CHIP_ICELAND:
		*addrlib_family = FAMILY_VI;
		*addrlib_revid = VI_ICELAND_M_A0;
		break;
	case CHIP_CARRIZO:
		*addrlib_family = FAMILY_CZ;
		*addrlib_revid = CARRIZO_A0;
		break;
	case CHIP_STONEY:
		*addrlib_family = FAMILY_CZ;
		*addrlib_revid = STONEY_A0;
		break;
	case CHIP_FIJI:
		*addrlib_family = FAMILY_VI;
		*addrlib_revid = VI_FIJI_P_A0;
		break;
	case CHIP_POLARIS10:
		*addrlib_family = FAMILY_VI;
		*addrlib_revid = VI_POLARIS10_P_A0;
		break;
	case CHIP_POLARIS11:
		*addrlib_family = FAMILY_VI;
		*addrlib_revid = VI_POLARIS11_M_A0;
		break;
	case CHIP_POLARIS12:
		*addrlib_family = FAMILY_VI;
		*addrlib_revid = VI_POLARIS12_V_A0;
		break;
	case CHIP_VEGA10:
		*addrlib_family = FAMILY_AI;
		*addrlib_revid = AI_VEGA10_P_A0;
		break;
	case CHIP_RAVEN:
		*addrlib_family = FAMILY_RV;
		*addrlib_revid = RAVEN_A0;
		break;
	default:
		fprintf(stderr, "amdgpu: Unknown family.\n");
	}
}

static void *ADDR_API allocSysMem(const ADDR_ALLOCSYSMEM_INPUT * pInput)
{
	return malloc(pInput->sizeInBytes);
}

static ADDR_E_RETURNCODE ADDR_API freeSysMem(const ADDR_FREESYSMEM_INPUT * pInput)
{
	free(pInput->pVirtAddr);
	return ADDR_OK;
}

ADDR_HANDLE amdgpu_addr_create(enum radeon_family family,
			       const struct amdgpu_gpu_info *info)
{
	ADDR_CREATE_INPUT addrCreateInput = {0};
	ADDR_CREATE_OUTPUT addrCreateOutput = {0};
	ADDR_REGISTER_VALUE regValue = {0};
	ADDR_CREATE_FLAGS createFlags = {{0}};
	ADDR_E_RETURNCODE addrRet;

	addrCreateInput.size = sizeof(ADDR_CREATE_INPUT);
	addrCreateOutput.size = sizeof(ADDR_CREATE_OUTPUT);

	regValue.gbAddrConfig = info->gb_addr_cfg;
	createFlags.value = 0;

	addrlib_family_rev_id(family, &addrCreateInput.chipFamily, &addrCreateInput.chipRevision);
	if (addrCreateInput.chipFamily == FAMILY_UNKNOWN)
		return NULL;

	if (addrCreateInput.chipFamily >= FAMILY_AI) {
		addrCreateInput.chipEngine = CIASICIDGFXENGINE_ARCTICISLAND;
		regValue.blockVarSizeLog2 = 0;
	} else {
		regValue.noOfBanks = info->mc_arb_ramcfg & 0x3;
		regValue.noOfRanks = (info->mc_arb_ramcfg & 0x4) >> 2;

		regValue.backendDisables = info->enabled_rb_pipes_mask;
		regValue.pTileConfig = info->gb_tile_mode;
		regValue.noOfEntries = ARRAY_SIZE(info->gb_tile_mode);
		if (addrCreateInput.chipFamily == FAMILY_SI) {
			regValue.pMacroTileConfig = NULL;
			regValue.noOfMacroEntries = 0;
		} else {
			regValue.pMacroTileConfig = info->gb_macro_tile_mode;
			regValue.noOfMacroEntries = ARRAY_SIZE(info->gb_macro_tile_mode);
		}

		createFlags.useTileIndex = 1;
		createFlags.useHtileSliceAlign = 1;

		addrCreateInput.chipEngine = CIASICIDGFXENGINE_SOUTHERNISLAND;
	}

	addrCreateInput.callbacks.allocSysMem = allocSysMem;
	addrCreateInput.callbacks.freeSysMem = freeSysMem;
	addrCreateInput.callbacks.debugPrint = 0;
	addrCreateInput.createFlags = createFlags;
	addrCreateInput.regValue = regValue;

	addrRet = AddrCreate(&addrCreateInput, &addrCreateOutput);
	if (addrRet != ADDR_OK)
		return NULL;

	return addrCreateOutput.hLib;
}
