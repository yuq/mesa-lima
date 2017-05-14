/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
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

#include <errno.h>

#include "radv_private.h"
#include "addrlib/addrinterface.h"
#include "util/bitset.h"
#include "radv_amdgpu_winsys.h"
#include "radv_amdgpu_surface.h"
#include "sid.h"

#include "ac_surface.h"

static int radv_amdgpu_surface_sanity(const struct ac_surf_info *surf_info,
				      const struct radeon_surf *surf)
{
	unsigned type = RADEON_SURF_GET(surf->flags, TYPE);

	if (!(surf->flags & RADEON_SURF_HAS_TILE_MODE_INDEX))
		return -EINVAL;

	/* all dimension must be at least 1 ! */
	if (!surf_info->width || !surf_info->height || !surf_info->depth ||
	    !surf_info->array_size)
		return -EINVAL;

	if (!surf->blk_w || !surf->blk_h)
		return -EINVAL;

	switch (surf_info->samples) {
	case 1:
	case 2:
	case 4:
	case 8:
		break;
	default:
		return -EINVAL;
	}

	switch (type) {
	case RADEON_SURF_TYPE_1D:
		if (surf_info->height > 1)
			return -EINVAL;
		/* fall through */
	case RADEON_SURF_TYPE_2D:
	case RADEON_SURF_TYPE_CUBEMAP:
		if (surf_info->depth > 1 || surf_info->array_size > 1)
			return -EINVAL;
		break;
	case RADEON_SURF_TYPE_3D:
		if (surf_info->array_size > 1)
			return -EINVAL;
		break;
	case RADEON_SURF_TYPE_1D_ARRAY:
		if (surf_info->height > 1)
			return -EINVAL;
		/* fall through */
	case RADEON_SURF_TYPE_2D_ARRAY:
		if (surf_info->depth > 1)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int radv_compute_level(ADDR_HANDLE addrlib,
			      const struct ac_surf_info *surf_info,
                              struct radeon_surf *surf, bool is_stencil,
                              unsigned level, unsigned type, bool compressed,
                              ADDR_COMPUTE_SURFACE_INFO_INPUT *AddrSurfInfoIn,
                              ADDR_COMPUTE_SURFACE_INFO_OUTPUT *AddrSurfInfoOut,
                              ADDR_COMPUTE_DCCINFO_INPUT *AddrDccIn,
                              ADDR_COMPUTE_DCCINFO_OUTPUT *AddrDccOut)
{
	struct legacy_surf_level *surf_level;
	ADDR_E_RETURNCODE ret;

	AddrSurfInfoIn->mipLevel = level;
	AddrSurfInfoIn->width = u_minify(surf_info->width, level);
	AddrSurfInfoIn->height = u_minify(surf_info->height, level);

	if (type == RADEON_SURF_TYPE_3D)
		AddrSurfInfoIn->numSlices = u_minify(surf_info->depth, level);
	else if (type == RADEON_SURF_TYPE_CUBEMAP)
		AddrSurfInfoIn->numSlices = 6;
	else
		AddrSurfInfoIn->numSlices = surf_info->array_size;

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
	if (ret != ADDR_OK)
		return ret;

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

	if (!is_stencil && AddrSurfInfoIn->flags.depth &&
	    surf_level->mode == RADEON_SURF_MODE_2D && level == 0) {
		ADDR_COMPUTE_HTILE_INFO_INPUT AddrHtileIn = {0};
		ADDR_COMPUTE_HTILE_INFO_OUTPUT AddrHtileOut = {0};
		AddrHtileIn.flags.tcCompatible = AddrSurfInfoIn->flags.tcCompatible;
		AddrHtileIn.pitch = AddrSurfInfoOut->pitch;
		AddrHtileIn.height = AddrSurfInfoOut->height;
		AddrHtileIn.numSlices = AddrSurfInfoOut->depth;
		AddrHtileIn.blockWidth = ADDR_HTILE_BLOCKSIZE_8;
		AddrHtileIn.blockHeight = ADDR_HTILE_BLOCKSIZE_8;
		AddrHtileIn.pTileInfo = AddrSurfInfoOut->pTileInfo;
		AddrHtileIn.tileIndex = AddrSurfInfoOut->tileIndex;
		AddrHtileIn.macroModeIndex = AddrSurfInfoOut->macroModeIndex;

		ret = AddrComputeHtileInfo(addrlib,
		                           &AddrHtileIn,
		                           &AddrHtileOut);

		if (ret == ADDR_OK) {
			surf->htile_size = AddrHtileOut.htileBytes;
			surf->htile_slice_size = AddrHtileOut.sliceSize;
			surf->htile_alignment = AddrHtileOut.baseAlign;
		}
	}
	return 0;
}

static void radv_set_micro_tile_mode(struct radeon_surf *surf,
                                     struct radeon_info *info)
{
	uint32_t tile_mode = info->si_tile_mode_array[surf->u.legacy.tiling_index[0]];

	if (info->chip_class >= CIK)
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

static int radv_amdgpu_winsys_surface_init(struct radeon_winsys *_ws,
					   const struct ac_surf_info *surf_info,
					   struct radeon_surf *surf)
{
	struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
	unsigned level, mode, type;
	bool compressed;
	ADDR_COMPUTE_SURFACE_INFO_INPUT AddrSurfInfoIn = {0};
	ADDR_COMPUTE_SURFACE_INFO_OUTPUT AddrSurfInfoOut = {0};
	ADDR_COMPUTE_DCCINFO_INPUT AddrDccIn = {0};
	ADDR_COMPUTE_DCCINFO_OUTPUT AddrDccOut = {0};
	ADDR_TILEINFO AddrTileInfoIn = {0};
	ADDR_TILEINFO AddrTileInfoOut = {0};
	int r;
	uint32_t last_level = surf_info->levels - 1;

	r = radv_amdgpu_surface_sanity(surf_info, surf);
	if (r)
		return r;

	AddrSurfInfoIn.size = sizeof(ADDR_COMPUTE_SURFACE_INFO_INPUT);
	AddrSurfInfoOut.size = sizeof(ADDR_COMPUTE_SURFACE_INFO_OUTPUT);
	AddrDccIn.size = sizeof(ADDR_COMPUTE_DCCINFO_INPUT);
	AddrDccOut.size = sizeof(ADDR_COMPUTE_DCCINFO_OUTPUT);
	AddrSurfInfoOut.pTileInfo = &AddrTileInfoOut;

	type = RADEON_SURF_GET(surf->flags, TYPE);
	mode = RADEON_SURF_GET(surf->flags, MODE);
	compressed = surf->blk_w == 4 && surf->blk_h == 4;

	/* MSAA and FMASK require 2D tiling. */
	if (surf_info->samples > 1 ||
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
	 * textures to work. In other cases, setting the bpp is sufficient. */
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
	} else {
		AddrDccIn.bpp = AddrSurfInfoIn.bpp = surf->bpe * 8;
	}

	AddrDccIn.numSamples = AddrSurfInfoIn.numSamples = surf_info->samples;
	AddrSurfInfoIn.tileIndex = -1;

	/* Set the micro tile type. */
	if (surf->flags & RADEON_SURF_SCANOUT)
		AddrSurfInfoIn.tileType = ADDR_DISPLAYABLE;
	else if (surf->flags & RADEON_SURF_Z_OR_SBUFFER)
		AddrSurfInfoIn.tileType = ADDR_DEPTH_SAMPLE_ORDER;
	else
		AddrSurfInfoIn.tileType = ADDR_NON_DISPLAYABLE;

	AddrSurfInfoIn.flags.color = !(surf->flags & RADEON_SURF_Z_OR_SBUFFER);
	AddrSurfInfoIn.flags.depth = (surf->flags & RADEON_SURF_ZBUFFER) != 0;
	AddrSurfInfoIn.flags.fmask = (surf->flags & RADEON_SURF_FMASK) != 0;
	AddrSurfInfoIn.flags.cube = type == RADEON_SURF_TYPE_CUBEMAP;
	AddrSurfInfoIn.flags.display = (surf->flags & RADEON_SURF_SCANOUT) != 0;
	AddrSurfInfoIn.flags.pow2Pad = last_level > 0;
	AddrSurfInfoIn.flags.opt4Space = !AddrSurfInfoIn.flags.fmask;

	/* DCC notes:
	 * - If we add MSAA support, keep in mind that CB can't decompress 8bpp
	 *   with samples >= 4.
	 * - Mipmapped array textures have low performance (discovered by a closed
	 *   driver team).
	 */
	AddrSurfInfoIn.flags.dccCompatible = !(surf->flags & RADEON_SURF_Z_OR_SBUFFER) &&
		!(surf->flags & RADEON_SURF_DISABLE_DCC) &&
		!compressed && AddrDccIn.numSamples <= 1 &&
		((surf_info->array_size == 1 && surf_info->depth == 1) ||
		 last_level == 0);

	AddrSurfInfoIn.flags.noStencil = (surf->flags & RADEON_SURF_SBUFFER) == 0;
	AddrSurfInfoIn.flags.compressZ = AddrSurfInfoIn.flags.depth;

	/* noStencil = 0 can result in a depth part that is incompatible with
	 * mipmapped texturing. So set noStencil = 1 when mipmaps are requested (in
	 * this case, we may end up setting stencil_adjusted).
	 *
	 * TODO: update addrlib to a newer version, remove this, and
	 * use flags.matchStencilTileCfg = 1 as an alternative fix.
	 */
	if (last_level > 0)
		AddrSurfInfoIn.flags.noStencil = 1;

	/* Set preferred macrotile parameters. This is usually required
	 * for shared resources. This is for 2D tiling only. */
	if (AddrSurfInfoIn.tileMode >= ADDR_TM_2D_TILED_THIN1 &&
	    surf->u.legacy.bankw && surf->u.legacy.bankh && surf->u.legacy.mtilea &&
	    surf->u.legacy.tile_split) {
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

		if (ws->info.chip_class == SI) {
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
			if (AddrSurfInfoIn.tileType == ADDR_DISPLAYABLE)
				AddrSurfInfoIn.tileIndex = 10; /* 2D displayable */
			else
				AddrSurfInfoIn.tileIndex = 14; /* 2D non-displayable */
			AddrSurfInfoOut.macroModeIndex = cik_get_macro_tile_index(surf);
		}
	}

	surf->surf_size = 0;
	surf->num_dcc_levels = 0;
	surf->dcc_size = 0;
	surf->dcc_alignment = 1;
	surf->htile_size = surf->htile_slice_size = 0;
	surf->htile_alignment = 1;

	/* Calculate texture layout information. */
	for (level = 0; level <= last_level; level++) {
		r = radv_compute_level(ws->addrlib, surf_info, surf, false, level, type, compressed,
				       &AddrSurfInfoIn, &AddrSurfInfoOut, &AddrDccIn, &AddrDccOut);
		if (r)
			break;

		if (level == 0) {
			surf->surf_alignment = AddrSurfInfoOut.baseAlign;
			surf->u.legacy.pipe_config = AddrSurfInfoOut.pTileInfo->pipeConfig - 1;
			radv_set_micro_tile_mode(surf, &ws->info);

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
		/* This will be ignored if AddrSurfInfoIn.pTileInfo is NULL. */
		AddrTileInfoIn.tileSplitBytes = surf->u.legacy.stencil_tile_split;

		for (level = 0; level <= last_level; level++) {
			r = radv_compute_level(ws->addrlib, surf_info, surf, true, level, type, compressed,
					       &AddrSurfInfoIn, &AddrSurfInfoOut, &AddrDccIn, &AddrDccOut);
			if (r)
				return r;

			/* DB uses the depth pitch for both stencil and depth. */
			if (surf->u.legacy.stencil_level[level].nblk_x != surf->u.legacy.level[level].nblk_x)
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
#if 0
	if (surf->dcc_size && last_level > 0) {
		surf->dcc_size = align64(surf->bo_size >> 8,
					 ws->info.pipe_interleave_bytes *
					 ws->info.num_tile_pipes);
	}
#endif
	return 0;
}

static int radv_amdgpu_winsys_surface_best(struct radeon_winsys *rws,
					   struct radeon_surf *surf)
{
	return 0;
}

void radv_amdgpu_surface_init_functions(struct radv_amdgpu_winsys *ws)
{
	ws->base.surface_init = radv_amdgpu_winsys_surface_init;
	ws->base.surface_best = radv_amdgpu_winsys_surface_best;
}
