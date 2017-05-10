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
#include <amdgpu_drm.h>

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

static int gfx6_compute_level(ADDR_HANDLE addrlib,
			      const struct ac_surf_config *config,
			      struct radeon_surf *surf, bool is_stencil,
			      unsigned level, bool compressed,
			      ADDR_COMPUTE_SURFACE_INFO_INPUT *AddrSurfInfoIn,
			      ADDR_COMPUTE_SURFACE_INFO_OUTPUT *AddrSurfInfoOut,
			      ADDR_COMPUTE_DCCINFO_INPUT *AddrDccIn,
			      ADDR_COMPUTE_DCCINFO_OUTPUT *AddrDccOut,
			      ADDR_COMPUTE_HTILE_INFO_INPUT *AddrHtileIn,
			      ADDR_COMPUTE_HTILE_INFO_OUTPUT *AddrHtileOut)
{
	struct legacy_surf_level *surf_level;
	ADDR_E_RETURNCODE ret;

	AddrSurfInfoIn->mipLevel = level;
	AddrSurfInfoIn->width = u_minify(config->info.width, level);
	AddrSurfInfoIn->height = u_minify(config->info.height, level);

	if (config->is_3d)
		AddrSurfInfoIn->numSlices = u_minify(config->info.depth, level);
	else if (config->is_cube)
		AddrSurfInfoIn->numSlices = 6;
	else
		AddrSurfInfoIn->numSlices = config->info.array_size;

	if (level > 0) {
		/* Set the base level pitch. This is needed for calculation
		 * of non-zero levels. */
		if (is_stencil)
			AddrSurfInfoIn->basePitch = surf->u.legacy.stencil_level[0].nblk_x;
		else
			AddrSurfInfoIn->basePitch = surf->u.legacy.level[0].nblk_x;

		/* Convert blocks to pixels for compressed formats. */
		if (compressed)
			AddrSurfInfoIn->basePitch *= surf->blk_w;
	}

	ret = AddrComputeSurfaceInfo(addrlib,
				     AddrSurfInfoIn,
				     AddrSurfInfoOut);
	if (ret != ADDR_OK) {
		return ret;
	}

	surf_level = is_stencil ? &surf->u.legacy.stencil_level[level] : &surf->u.legacy.level[level];
	surf_level->offset = align64(surf->surf_size, AddrSurfInfoOut->baseAlign);
	surf_level->slice_size = AddrSurfInfoOut->sliceSize;
	surf_level->nblk_x = AddrSurfInfoOut->pitch;
	surf_level->nblk_y = AddrSurfInfoOut->height;

	switch (AddrSurfInfoOut->tileMode) {
	case ADDR_TM_LINEAR_ALIGNED:
		surf_level->mode = RADEON_SURF_MODE_LINEAR_ALIGNED;
		break;
	case ADDR_TM_1D_TILED_THIN1:
		surf_level->mode = RADEON_SURF_MODE_1D;
		break;
	case ADDR_TM_2D_TILED_THIN1:
		surf_level->mode = RADEON_SURF_MODE_2D;
		break;
	default:
		assert(0);
	}

	if (is_stencil)
		surf->u.legacy.stencil_tiling_index[level] = AddrSurfInfoOut->tileIndex;
	else
		surf->u.legacy.tiling_index[level] = AddrSurfInfoOut->tileIndex;

	surf->surf_size = surf_level->offset + AddrSurfInfoOut->surfSize;

	/* Clear DCC fields at the beginning. */
	surf_level->dcc_offset = 0;

	/* The previous level's flag tells us if we can use DCC for this level. */
	if (AddrSurfInfoIn->flags.dccCompatible &&
	    (level == 0 || AddrDccOut->subLvlCompressible)) {
		AddrDccIn->colorSurfSize = AddrSurfInfoOut->surfSize;
		AddrDccIn->tileMode = AddrSurfInfoOut->tileMode;
		AddrDccIn->tileInfo = *AddrSurfInfoOut->pTileInfo;
		AddrDccIn->tileIndex = AddrSurfInfoOut->tileIndex;
		AddrDccIn->macroModeIndex = AddrSurfInfoOut->macroModeIndex;

		ret = AddrComputeDccInfo(addrlib,
					 AddrDccIn,
					 AddrDccOut);

		if (ret == ADDR_OK) {
			surf_level->dcc_offset = surf->dcc_size;
			surf_level->dcc_fast_clear_size = AddrDccOut->dccFastClearSize;
			surf->num_dcc_levels = level + 1;
			surf->dcc_size = surf_level->dcc_offset + AddrDccOut->dccRamSize;
			surf->dcc_alignment = MAX2(surf->dcc_alignment, AddrDccOut->dccRamBaseAlign);
		}
	}

	/* TC-compatible HTILE. */
	if (!is_stencil &&
	    AddrSurfInfoIn->flags.depth &&
	    AddrSurfInfoIn->flags.tcCompatible &&
	    surf_level->mode == RADEON_SURF_MODE_2D &&
	    level == 0) {
		AddrHtileIn->flags.tcCompatible = 1;
		AddrHtileIn->pitch = AddrSurfInfoOut->pitch;
		AddrHtileIn->height = AddrSurfInfoOut->height;
		AddrHtileIn->numSlices = AddrSurfInfoOut->depth;
		AddrHtileIn->blockWidth = ADDR_HTILE_BLOCKSIZE_8;
		AddrHtileIn->blockHeight = ADDR_HTILE_BLOCKSIZE_8;
		AddrHtileIn->pTileInfo = AddrSurfInfoOut->pTileInfo;
		AddrHtileIn->tileIndex = AddrSurfInfoOut->tileIndex;
		AddrHtileIn->macroModeIndex = AddrSurfInfoOut->macroModeIndex;

		ret = AddrComputeHtileInfo(addrlib,
					   AddrHtileIn,
					   AddrHtileOut);

		if (ret == ADDR_OK) {
			surf->htile_size = AddrHtileOut->htileBytes;
			surf->htile_alignment = AddrHtileOut->baseAlign;
		}
	}

	return 0;
}

#define   G_009910_MICRO_TILE_MODE(x)          (((x) >> 0) & 0x03)
#define   G_009910_MICRO_TILE_MODE_NEW(x)      (((x) >> 22) & 0x07)

static void gfx6_set_micro_tile_mode(struct radeon_surf *surf,
				     const struct amdgpu_gpu_info *amdinfo)
{
	uint32_t tile_mode = amdinfo->gb_tile_mode[surf->u.legacy.tiling_index[0]];

	if (amdinfo->family_id >= AMDGPU_FAMILY_CI)
		surf->micro_tile_mode = G_009910_MICRO_TILE_MODE_NEW(tile_mode);
	else
		surf->micro_tile_mode = G_009910_MICRO_TILE_MODE(tile_mode);
}

static unsigned cik_get_macro_tile_index(struct radeon_surf *surf)
{
	unsigned index, tileb;

	tileb = 8 * 8 * surf->bpe;
	tileb = MIN2(surf->u.legacy.tile_split, tileb);

	for (index = 0; tileb > 64; index++)
		tileb >>= 1;

	assert(index < 16);
	return index;
}

/**
 * Fill in the tiling information in \p surf based on the given surface config.
 *
 * The following fields of \p surf must be initialized by the caller:
 * blk_w, blk_h, bpe, flags.
 */
int gfx6_compute_surface(ADDR_HANDLE addrlib,
			 const struct ac_surf_config *config,
			 enum radeon_surf_mode mode,
			 struct radeon_surf *surf)
{
	unsigned level;
	bool compressed;
	ADDR_COMPUTE_SURFACE_INFO_INPUT AddrSurfInfoIn = {0};
	ADDR_COMPUTE_SURFACE_INFO_OUTPUT AddrSurfInfoOut = {0};
	ADDR_COMPUTE_DCCINFO_INPUT AddrDccIn = {0};
	ADDR_COMPUTE_DCCINFO_OUTPUT AddrDccOut = {0};
	ADDR_COMPUTE_HTILE_INFO_INPUT AddrHtileIn = {0};
	ADDR_COMPUTE_HTILE_INFO_OUTPUT AddrHtileOut = {0};
	ADDR_TILEINFO AddrTileInfoIn = {0};
	ADDR_TILEINFO AddrTileInfoOut = {0};
	int r;

	AddrSurfInfoIn.size = sizeof(ADDR_COMPUTE_SURFACE_INFO_INPUT);
	AddrSurfInfoOut.size = sizeof(ADDR_COMPUTE_SURFACE_INFO_OUTPUT);
	AddrDccIn.size = sizeof(ADDR_COMPUTE_DCCINFO_INPUT);
	AddrDccOut.size = sizeof(ADDR_COMPUTE_DCCINFO_OUTPUT);
	AddrHtileIn.size = sizeof(ADDR_COMPUTE_HTILE_INFO_INPUT);
	AddrHtileOut.size = sizeof(ADDR_COMPUTE_HTILE_INFO_OUTPUT);
	AddrSurfInfoOut.pTileInfo = &AddrTileInfoOut;

	compressed = surf->blk_w == 4 && surf->blk_h == 4;

	/* MSAA and FMASK require 2D tiling. */
	if (config->info.samples > 1 ||
	    (surf->flags & RADEON_SURF_FMASK))
		mode = RADEON_SURF_MODE_2D;

	/* DB doesn't support linear layouts. */
	if (surf->flags & (RADEON_SURF_Z_OR_SBUFFER) &&
	    mode < RADEON_SURF_MODE_1D)
		mode = RADEON_SURF_MODE_1D;

	/* Set the requested tiling mode. */
	switch (mode) {
	case RADEON_SURF_MODE_LINEAR_ALIGNED:
		AddrSurfInfoIn.tileMode = ADDR_TM_LINEAR_ALIGNED;
		break;
	case RADEON_SURF_MODE_1D:
		AddrSurfInfoIn.tileMode = ADDR_TM_1D_TILED_THIN1;
		break;
	case RADEON_SURF_MODE_2D:
		AddrSurfInfoIn.tileMode = ADDR_TM_2D_TILED_THIN1;
		break;
	default:
		assert(0);
	}

	/* The format must be set correctly for the allocation of compressed
	 * textures to work. In other cases, setting the bpp is sufficient.
	 */
	if (compressed) {
		switch (surf->bpe) {
		case 8:
			AddrSurfInfoIn.format = ADDR_FMT_BC1;
			break;
		case 16:
			AddrSurfInfoIn.format = ADDR_FMT_BC3;
			break;
		default:
			assert(0);
		}
	}
	else {
		AddrDccIn.bpp = AddrSurfInfoIn.bpp = surf->bpe * 8;
	}

	AddrDccIn.numSamples = AddrSurfInfoIn.numSamples =
		config->info.samples ? config->info.samples : 1;
	AddrSurfInfoIn.tileIndex = -1;

	/* Set the micro tile type. */
	if (surf->flags & RADEON_SURF_SCANOUT)
		AddrSurfInfoIn.tileType = ADDR_DISPLAYABLE;
	else if (surf->flags & (RADEON_SURF_Z_OR_SBUFFER | RADEON_SURF_FMASK))
		AddrSurfInfoIn.tileType = ADDR_DEPTH_SAMPLE_ORDER;
	else
		AddrSurfInfoIn.tileType = ADDR_NON_DISPLAYABLE;

	AddrSurfInfoIn.flags.color = !(surf->flags & RADEON_SURF_Z_OR_SBUFFER);
	AddrSurfInfoIn.flags.depth = (surf->flags & RADEON_SURF_ZBUFFER) != 0;
	AddrSurfInfoIn.flags.cube = config->is_cube;
	AddrSurfInfoIn.flags.fmask = (surf->flags & RADEON_SURF_FMASK) != 0;
	AddrSurfInfoIn.flags.display = (surf->flags & RADEON_SURF_SCANOUT) != 0;
	AddrSurfInfoIn.flags.pow2Pad = config->info.levels > 1;
	AddrSurfInfoIn.flags.tcCompatible = (surf->flags & RADEON_SURF_TC_COMPATIBLE_HTILE) != 0;

	/* Only degrade the tile mode for space if TC-compatible HTILE hasn't been
	 * requested, because TC-compatible HTILE requires 2D tiling.
	 */
	AddrSurfInfoIn.flags.opt4Space = !AddrSurfInfoIn.flags.tcCompatible &&
					 !AddrSurfInfoIn.flags.fmask &&
					 config->info.samples <= 1 &&
					 (surf->flags & RADEON_SURF_OPTIMIZE_FOR_SPACE);

	/* DCC notes:
	 * - If we add MSAA support, keep in mind that CB can't decompress 8bpp
	 *   with samples >= 4.
	 * - Mipmapped array textures have low performance (discovered by a closed
	 *   driver team).
	 */
	AddrSurfInfoIn.flags.dccCompatible =
		config->chip_class >= VI &&
		!(surf->flags & RADEON_SURF_Z_OR_SBUFFER) &&
		!(surf->flags & RADEON_SURF_DISABLE_DCC) &&
		!compressed && AddrDccIn.numSamples <= 1 &&
		((config->info.array_size == 1 && config->info.depth == 1) ||
		 config->info.levels == 1);

	AddrSurfInfoIn.flags.noStencil = (surf->flags & RADEON_SURF_SBUFFER) == 0;
	AddrSurfInfoIn.flags.compressZ = AddrSurfInfoIn.flags.depth;

	/* noStencil = 0 can result in a depth part that is incompatible with
	 * mipmapped texturing. So set noStencil = 1 when mipmaps are requested (in
	 * this case, we may end up setting stencil_adjusted).
	 *
	 * TODO: update addrlib to a newer version, remove this, and
	 * use flags.matchStencilTileCfg = 1 as an alternative fix.
	 */
	if (config->info.levels > 1)
		AddrSurfInfoIn.flags.noStencil = 1;

	/* Set preferred macrotile parameters. This is usually required
	 * for shared resources. This is for 2D tiling only. */
	if (AddrSurfInfoIn.tileMode >= ADDR_TM_2D_TILED_THIN1 &&
	    surf->u.legacy.bankw && surf->u.legacy.bankh &&
	    surf->u.legacy.mtilea && surf->u.legacy.tile_split) {
		assert(!(surf->flags & RADEON_SURF_FMASK));

		/* If any of these parameters are incorrect, the calculation
		 * will fail. */
		AddrTileInfoIn.banks = surf->u.legacy.num_banks;
		AddrTileInfoIn.bankWidth = surf->u.legacy.bankw;
		AddrTileInfoIn.bankHeight = surf->u.legacy.bankh;
		AddrTileInfoIn.macroAspectRatio = surf->u.legacy.mtilea;
		AddrTileInfoIn.tileSplitBytes = surf->u.legacy.tile_split;
		AddrTileInfoIn.pipeConfig = surf->u.legacy.pipe_config + 1; /* +1 compared to GB_TILE_MODE */
		AddrSurfInfoIn.flags.opt4Space = 0;
		AddrSurfInfoIn.pTileInfo = &AddrTileInfoIn;

		/* If AddrSurfInfoIn.pTileInfo is set, Addrlib doesn't set
		 * the tile index, because we are expected to know it if
		 * we know the other parameters.
		 *
		 * This is something that can easily be fixed in Addrlib.
		 * For now, just figure it out here.
		 * Note that only 2D_TILE_THIN1 is handled here.
		 */
		assert(!(surf->flags & RADEON_SURF_Z_OR_SBUFFER));
		assert(AddrSurfInfoIn.tileMode == ADDR_TM_2D_TILED_THIN1);

		if (config->chip_class == SI) {
			if (AddrSurfInfoIn.tileType == ADDR_DISPLAYABLE) {
				if (surf->bpe == 2)
					AddrSurfInfoIn.tileIndex = 11; /* 16bpp */
				else
					AddrSurfInfoIn.tileIndex = 12; /* 32bpp */
			} else {
				if (surf->bpe == 1)
					AddrSurfInfoIn.tileIndex = 14; /* 8bpp */
				else if (surf->bpe == 2)
					AddrSurfInfoIn.tileIndex = 15; /* 16bpp */
				else if (surf->bpe == 4)
					AddrSurfInfoIn.tileIndex = 16; /* 32bpp */
				else
					AddrSurfInfoIn.tileIndex = 17; /* 64bpp (and 128bpp) */
			}
		} else {
			/* CIK - VI */
			if (AddrSurfInfoIn.tileType == ADDR_DISPLAYABLE)
				AddrSurfInfoIn.tileIndex = 10; /* 2D displayable */
			else
				AddrSurfInfoIn.tileIndex = 14; /* 2D non-displayable */

			/* Addrlib doesn't set this if tileIndex is forced like above. */
			AddrSurfInfoOut.macroModeIndex = cik_get_macro_tile_index(surf);
		}
	}

	surf->num_dcc_levels = 0;
	surf->surf_size = 0;
	surf->dcc_size = 0;
	surf->dcc_alignment = 1;
	surf->htile_size = 0;
	surf->htile_alignment = 1;

	/* Calculate texture layout information. */
	for (level = 0; level < config->info.levels; level++) {
		r = gfx6_compute_level(addrlib, config, surf, false, level, compressed,
				       &AddrSurfInfoIn, &AddrSurfInfoOut,
				       &AddrDccIn, &AddrDccOut, &AddrHtileIn, &AddrHtileOut);
		if (r)
			return r;

		if (level == 0) {
			surf->surf_alignment = AddrSurfInfoOut.baseAlign;
			surf->u.legacy.pipe_config = AddrSurfInfoOut.pTileInfo->pipeConfig - 1;
			gfx6_set_micro_tile_mode(surf, config->amdinfo);

			/* For 2D modes only. */
			if (AddrSurfInfoOut.tileMode >= ADDR_TM_2D_TILED_THIN1) {
				surf->u.legacy.bankw = AddrSurfInfoOut.pTileInfo->bankWidth;
				surf->u.legacy.bankh = AddrSurfInfoOut.pTileInfo->bankHeight;
				surf->u.legacy.mtilea = AddrSurfInfoOut.pTileInfo->macroAspectRatio;
				surf->u.legacy.tile_split = AddrSurfInfoOut.pTileInfo->tileSplitBytes;
				surf->u.legacy.num_banks = AddrSurfInfoOut.pTileInfo->banks;
				surf->u.legacy.macro_tile_index = AddrSurfInfoOut.macroModeIndex;
			} else {
				surf->u.legacy.macro_tile_index = 0;
			}
		}
	}

	/* Calculate texture layout information for stencil. */
	if (surf->flags & RADEON_SURF_SBUFFER) {
		AddrSurfInfoIn.bpp = 8;
		AddrSurfInfoIn.flags.depth = 0;
		AddrSurfInfoIn.flags.stencil = 1;
		AddrSurfInfoIn.flags.tcCompatible = 0;
		/* This will be ignored if AddrSurfInfoIn.pTileInfo is NULL. */
		AddrTileInfoIn.tileSplitBytes = surf->u.legacy.stencil_tile_split;

		for (level = 0; level < config->info.levels; level++) {
			r = gfx6_compute_level(addrlib, config, surf, true, level, compressed,
					       &AddrSurfInfoIn, &AddrSurfInfoOut,
					       &AddrDccIn, &AddrDccOut,
					       NULL, NULL);
			if (r)
				return r;

			/* DB uses the depth pitch for both stencil and depth. */
			if (surf->u.legacy.stencil_level[level].nblk_x !=
			    surf->u.legacy.level[level].nblk_x)
				surf->u.legacy.stencil_adjusted = true;

			if (level == 0) {
				/* For 2D modes only. */
				if (AddrSurfInfoOut.tileMode >= ADDR_TM_2D_TILED_THIN1) {
					surf->u.legacy.stencil_tile_split =
						AddrSurfInfoOut.pTileInfo->tileSplitBytes;
				}
			}
		}
	}

	/* Recalculate the whole DCC miptree size including disabled levels.
	 * This is what addrlib does, but calling addrlib would be a lot more
	 * complicated.
	 */
	if (surf->dcc_size && config->info.levels > 1) {
		surf->dcc_size = align64(surf->surf_size >> 8,
					 config->pipe_interleave_bytes *
					 config->num_tile_pipes);
	}

	/* Make sure HTILE covers the whole miptree, because the shader reads
	 * TC-compatible HTILE even for levels where it's disabled by DB.
	 */
	if (surf->htile_size && config->info.levels > 1)
		surf->htile_size *= 2;

	surf->is_linear = surf->u.legacy.level[0].mode == RADEON_SURF_MODE_LINEAR_ALIGNED;
	return 0;
}
