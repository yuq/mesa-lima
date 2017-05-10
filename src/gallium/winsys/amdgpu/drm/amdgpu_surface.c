/*
 * Copyright © 2011 Red Hat All Rights Reserved.
 * Copyright © 2014 Advanced Micro Devices, Inc.
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

/* Contact:
 *     Marek Olšák <maraeo@gmail.com>
 */

#include "amdgpu_winsys.h"
#include "util/u_format.h"

static int amdgpu_surface_sanity(const struct pipe_resource *tex)
{
   /* all dimension must be at least 1 ! */
   if (!tex->width0 || !tex->height0 || !tex->depth0 ||
       !tex->array_size)
      return -EINVAL;

   switch (tex->nr_samples) {
   case 0:
   case 1:
   case 2:
   case 4:
   case 8:
      break;
   default:
      return -EINVAL;
   }

   switch (tex->target) {
   case PIPE_TEXTURE_1D:
      if (tex->height0 > 1)
         return -EINVAL;
      /* fall through */
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
      if (tex->depth0 > 1 || tex->array_size > 1)
         return -EINVAL;
      break;
   case PIPE_TEXTURE_3D:
      if (tex->array_size > 1)
         return -EINVAL;
      break;
   case PIPE_TEXTURE_1D_ARRAY:
      if (tex->height0 > 1)
         return -EINVAL;
      /* fall through */
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_CUBE_ARRAY:
      if (tex->depth0 > 1)
         return -EINVAL;
      break;
   default:
      return -EINVAL;
   }
   return 0;
}

static int gfx6_surface_init(struct radeon_winsys *rws,
                             const struct pipe_resource *tex,
                             unsigned flags, unsigned bpe,
                             enum radeon_surf_mode mode,
                             struct radeon_surf *surf)
{
   struct amdgpu_winsys *ws = (struct amdgpu_winsys*)rws;
   int r;

   r = amdgpu_surface_sanity(tex);
   if (r)
      return r;

   surf->blk_w = util_format_get_blockwidth(tex->format);
   surf->blk_h = util_format_get_blockheight(tex->format);
   surf->bpe = bpe;
   surf->flags = flags;

   struct ac_surf_config config;

   config.info.width = tex->width0;
   config.info.height = tex->height0;
   config.info.depth = tex->depth0;
   config.info.array_size = tex->array_size;
   config.info.samples = tex->nr_samples;
   config.info.levels = tex->last_level + 1;
   config.is_3d = !!(tex->target == PIPE_TEXTURE_3D);
   config.is_cube = !!(tex->target == PIPE_TEXTURE_CUBE);
   config.chip_class = ws->info.chip_class;
   config.num_tile_pipes = ws->info.num_tile_pipes;
   config.pipe_interleave_bytes = ws->info.pipe_interleave_bytes;
   config.amdinfo = &ws->amdinfo;

   return gfx6_compute_surface(ws->addrlib, &config, mode, surf);
}

/* This is only called when expecting a tiled layout. */
static int
gfx9_get_preferred_swizzle_mode(struct amdgpu_winsys *ws,
                                ADDR2_COMPUTE_SURFACE_INFO_INPUT *in,
                                bool is_fmask, AddrSwizzleMode *swizzle_mode)
{
   ADDR_E_RETURNCODE ret;
   ADDR2_GET_PREFERRED_SURF_SETTING_INPUT sin = {0};
   ADDR2_GET_PREFERRED_SURF_SETTING_OUTPUT sout = {0};

   sin.size = sizeof(ADDR2_GET_PREFERRED_SURF_SETTING_INPUT);
   sout.size = sizeof(ADDR2_GET_PREFERRED_SURF_SETTING_OUTPUT);

   sin.flags = in->flags;
   sin.resourceType = in->resourceType;
   sin.format = in->format;
   sin.resourceLoction = ADDR_RSRC_LOC_INVIS;
   /* TODO: We could allow some of these: */
   sin.forbiddenBlock.micro = 1; /* don't allow the 256B swizzle modes */
   sin.forbiddenBlock.var = 1; /* don't allow the variable-sized swizzle modes */
   sin.forbiddenBlock.linear = 1; /* don't allow linear swizzle modes */
   sin.bpp = in->bpp;
   sin.width = in->width;
   sin.height = in->height;
   sin.numSlices = in->numSlices;
   sin.numMipLevels = in->numMipLevels;
   sin.numSamples = in->numSamples;
   sin.numFrags = in->numFrags;

   if (is_fmask) {
      sin.flags.color = 0;
      sin.flags.fmask = 1;
   }

   ret = Addr2GetPreferredSurfaceSetting(ws->addrlib, &sin, &sout);
   if (ret != ADDR_OK)
      return ret;

   *swizzle_mode = sout.swizzleMode;
   return 0;
}

static int gfx9_compute_miptree(struct amdgpu_winsys *ws,
                                struct radeon_surf *surf, bool compressed,
                                ADDR2_COMPUTE_SURFACE_INFO_INPUT *in)
{
   ADDR2_MIP_INFO mip_info[RADEON_SURF_MAX_LEVELS] = {};
   ADDR2_COMPUTE_SURFACE_INFO_OUTPUT out = {0};
   ADDR_E_RETURNCODE ret;

   out.size = sizeof(ADDR2_COMPUTE_SURFACE_INFO_OUTPUT);
   out.pMipInfo = mip_info;

   ret = Addr2ComputeSurfaceInfo(ws->addrlib, in, &out);
   if (ret != ADDR_OK)
      return ret;

   if (in->flags.stencil) {
      surf->u.gfx9.stencil.swizzle_mode = in->swizzleMode;
      surf->u.gfx9.stencil.epitch = out.epitchIsHeight ? out.mipChainHeight - 1 :
                                                         out.mipChainPitch - 1;
      surf->surf_alignment = MAX2(surf->surf_alignment, out.baseAlign);
      surf->u.gfx9.stencil_offset = align(surf->surf_size, out.baseAlign);
      surf->surf_size = surf->u.gfx9.stencil_offset + out.surfSize;
      return 0;
   }

   surf->u.gfx9.surf.swizzle_mode = in->swizzleMode;
   surf->u.gfx9.surf.epitch = out.epitchIsHeight ? out.mipChainHeight - 1 :
                                                   out.mipChainPitch - 1;

   /* CMASK fast clear uses these even if FMASK isn't allocated.
    * FMASK only supports the Z swizzle modes, whose numbers are multiples of 4.
    */
   surf->u.gfx9.fmask.swizzle_mode = surf->u.gfx9.surf.swizzle_mode & ~0x3;
   surf->u.gfx9.fmask.epitch = surf->u.gfx9.surf.epitch;

   surf->u.gfx9.surf_slice_size = out.sliceSize;
   surf->u.gfx9.surf_pitch = out.pitch;
   surf->u.gfx9.surf_height = out.height;
   surf->surf_size = out.surfSize;
   surf->surf_alignment = out.baseAlign;

   if (in->swizzleMode == ADDR_SW_LINEAR) {
      for (unsigned i = 0; i < in->numMipLevels; i++)
         surf->u.gfx9.offset[i] = mip_info[i].offset;
   }

   if (in->flags.depth) {
      assert(in->swizzleMode != ADDR_SW_LINEAR);

      /* HTILE */
      ADDR2_COMPUTE_HTILE_INFO_INPUT hin = {0};
      ADDR2_COMPUTE_HTILE_INFO_OUTPUT hout = {0};

      hin.size = sizeof(ADDR2_COMPUTE_HTILE_INFO_INPUT);
      hout.size = sizeof(ADDR2_COMPUTE_HTILE_INFO_OUTPUT);

      hin.hTileFlags.pipeAligned = 1;
      hin.hTileFlags.rbAligned = 1;
      hin.depthFlags = in->flags;
      hin.swizzleMode = in->swizzleMode;
      hin.unalignedWidth = in->width;
      hin.unalignedHeight = in->height;
      hin.numSlices = in->numSlices;
      hin.numMipLevels = in->numMipLevels;

      ret = Addr2ComputeHtileInfo(ws->addrlib, &hin, &hout);
      if (ret != ADDR_OK)
         return ret;

      surf->u.gfx9.htile.rb_aligned = hin.hTileFlags.rbAligned;
      surf->u.gfx9.htile.pipe_aligned = hin.hTileFlags.pipeAligned;
      surf->htile_size = hout.htileBytes;
      surf->htile_alignment = hout.baseAlign;
   } else {
      /* DCC */
      if (!(surf->flags & RADEON_SURF_DISABLE_DCC) &&
          !(surf->flags & RADEON_SURF_SCANOUT) &&
          !compressed &&
          in->swizzleMode != ADDR_SW_LINEAR &&
          /* TODO: We could support DCC with MSAA. */
          in->numSamples == 1) {
         ADDR2_COMPUTE_DCCINFO_INPUT din = {0};
         ADDR2_COMPUTE_DCCINFO_OUTPUT dout = {0};

         din.size = sizeof(ADDR2_COMPUTE_DCCINFO_INPUT);
         dout.size = sizeof(ADDR2_COMPUTE_DCCINFO_OUTPUT);

         din.dccKeyFlags.pipeAligned = 1;
         din.dccKeyFlags.rbAligned = 1;
         din.colorFlags = in->flags;
         din.resourceType = in->resourceType;
         din.swizzleMode = in->swizzleMode;
         din.bpp = in->bpp;
         din.unalignedWidth = in->width;
         din.unalignedHeight = in->height;
         din.numSlices = in->numSlices;
         din.numFrags = in->numFrags;
         din.numMipLevels = in->numMipLevels;
         din.dataSurfaceSize = out.surfSize;

         ret = Addr2ComputeDccInfo(ws->addrlib, &din, &dout);
         if (ret != ADDR_OK)
            return ret;

         surf->u.gfx9.dcc.rb_aligned = din.dccKeyFlags.rbAligned;
         surf->u.gfx9.dcc.pipe_aligned = din.dccKeyFlags.pipeAligned;
         surf->u.gfx9.dcc_pitch_max = dout.pitch - 1;
         surf->dcc_size = dout.dccRamSize;
         surf->dcc_alignment = dout.dccRamBaseAlign;
      }

      /* FMASK */
      if (in->numSamples > 1) {
         ADDR2_COMPUTE_FMASK_INFO_INPUT fin = {0};
         ADDR2_COMPUTE_FMASK_INFO_OUTPUT fout = {0};

         fin.size = sizeof(ADDR2_COMPUTE_FMASK_INFO_INPUT);
         fout.size = sizeof(ADDR2_COMPUTE_FMASK_INFO_OUTPUT);

         ret = gfx9_get_preferred_swizzle_mode(ws, in, true, &fin.swizzleMode);
         if (ret != ADDR_OK)
            return ret;

         fin.unalignedWidth = in->width;
         fin.unalignedHeight = in->height;
         fin.numSlices = in->numSlices;
         fin.numSamples = in->numSamples;
         fin.numFrags = in->numFrags;

         ret = Addr2ComputeFmaskInfo(ws->addrlib, &fin, &fout);
         if (ret != ADDR_OK)
            return ret;

         surf->u.gfx9.fmask.swizzle_mode = fin.swizzleMode;
         surf->u.gfx9.fmask.epitch = fout.pitch - 1;
         surf->u.gfx9.fmask_size = fout.fmaskBytes;
         surf->u.gfx9.fmask_alignment = fout.baseAlign;
      }

      /* CMASK */
      if (in->swizzleMode != ADDR_SW_LINEAR) {
         ADDR2_COMPUTE_CMASK_INFO_INPUT cin = {0};
         ADDR2_COMPUTE_CMASK_INFO_OUTPUT cout = {0};

         cin.size = sizeof(ADDR2_COMPUTE_CMASK_INFO_INPUT);
         cout.size = sizeof(ADDR2_COMPUTE_CMASK_INFO_OUTPUT);

         cin.cMaskFlags.pipeAligned = 1;
         cin.cMaskFlags.rbAligned = 1;
         cin.colorFlags = in->flags;
         cin.resourceType = in->resourceType;
         cin.unalignedWidth = in->width;
         cin.unalignedHeight = in->height;
         cin.numSlices = in->numSlices;

         if (in->numSamples > 1)
            cin.swizzleMode = surf->u.gfx9.fmask.swizzle_mode;
         else
            cin.swizzleMode = in->swizzleMode;

         ret = Addr2ComputeCmaskInfo(ws->addrlib, &cin, &cout);
         if (ret != ADDR_OK)
            return ret;

         surf->u.gfx9.cmask.rb_aligned = cin.cMaskFlags.rbAligned;
         surf->u.gfx9.cmask.pipe_aligned = cin.cMaskFlags.pipeAligned;
         surf->u.gfx9.cmask_size = cout.cmaskBytes;
         surf->u.gfx9.cmask_alignment = cout.baseAlign;
      }
   }

   return 0;
}

static int gfx9_surface_init(struct radeon_winsys *rws,
                             const struct pipe_resource *tex,
                             unsigned flags, unsigned bpe,
                             enum radeon_surf_mode mode,
                             struct radeon_surf *surf)
{
   struct amdgpu_winsys *ws = (struct amdgpu_winsys*)rws;
   bool compressed;
   ADDR2_COMPUTE_SURFACE_INFO_INPUT AddrSurfInfoIn = {0};
   int r;

   assert(!(flags & RADEON_SURF_FMASK));

   r = amdgpu_surface_sanity(tex);
   if (r)
      return r;

   AddrSurfInfoIn.size = sizeof(ADDR2_COMPUTE_SURFACE_INFO_INPUT);

   surf->blk_w = util_format_get_blockwidth(tex->format);
   surf->blk_h = util_format_get_blockheight(tex->format);
   surf->bpe = bpe;
   surf->flags = flags;

   compressed = surf->blk_w == 4 && surf->blk_h == 4;

   /* The format must be set correctly for the allocation of compressed
    * textures to work. In other cases, setting the bpp is sufficient. */
   if (compressed) {
      switch (bpe) {
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
      AddrSurfInfoIn.bpp = bpe * 8;
   }

   AddrSurfInfoIn.flags.color = !(flags & RADEON_SURF_Z_OR_SBUFFER);
   AddrSurfInfoIn.flags.depth = (flags & RADEON_SURF_ZBUFFER) != 0;
   AddrSurfInfoIn.flags.display = (flags & RADEON_SURF_SCANOUT) != 0;
   AddrSurfInfoIn.flags.texture = 1;
   AddrSurfInfoIn.flags.opt4space = 1;

   AddrSurfInfoIn.numMipLevels = tex->last_level + 1;
   AddrSurfInfoIn.numSamples = tex->nr_samples ? tex->nr_samples : 1;
   AddrSurfInfoIn.numFrags = AddrSurfInfoIn.numSamples;

   switch (tex->target) {
   /* GFX9 doesn't support 1D depth textures, so allocate all 1D textures
    * as 2D to avoid having shader variants for 1D vs 2D, so all shaders
    * must sample 1D textures as 2D. */
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_1D_ARRAY:
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_CUBE_ARRAY:
   case PIPE_TEXTURE_3D:
      if (tex->target == PIPE_TEXTURE_3D)
         AddrSurfInfoIn.resourceType = ADDR_RSRC_TEX_3D;
      else
         AddrSurfInfoIn.resourceType = ADDR_RSRC_TEX_2D;

      AddrSurfInfoIn.width = tex->width0;
      AddrSurfInfoIn.height = tex->height0;

      if (tex->target == PIPE_TEXTURE_3D)
         AddrSurfInfoIn.numSlices = tex->depth0;
      else if (tex->target == PIPE_TEXTURE_CUBE)
         AddrSurfInfoIn.numSlices = 6;
      else
         AddrSurfInfoIn.numSlices = tex->array_size;

      switch (mode) {
      case RADEON_SURF_MODE_LINEAR_ALIGNED:
         assert(tex->nr_samples <= 1);
         assert(!(flags & RADEON_SURF_Z_OR_SBUFFER));
         AddrSurfInfoIn.swizzleMode = ADDR_SW_LINEAR;
         break;

      case RADEON_SURF_MODE_1D:
      case RADEON_SURF_MODE_2D:
         r = gfx9_get_preferred_swizzle_mode(ws, &AddrSurfInfoIn, false,
                                             &AddrSurfInfoIn.swizzleMode);
         if (r)
            return r;
         break;

      default:
         assert(0);
      }
      break;

   default:
      assert(0);
   }

   surf->u.gfx9.resource_type = AddrSurfInfoIn.resourceType;

   surf->surf_size = 0;
   surf->dcc_size = 0;
   surf->htile_size = 0;
   surf->u.gfx9.surf_offset = 0;
   surf->u.gfx9.stencil_offset = 0;
   surf->u.gfx9.fmask_size = 0;
   surf->u.gfx9.cmask_size = 0;

   /* Calculate texture layout information. */
   r = gfx9_compute_miptree(ws, surf, compressed, &AddrSurfInfoIn);
   if (r)
      return r;

   /* Calculate texture layout information for stencil. */
   if (flags & RADEON_SURF_SBUFFER) {
      AddrSurfInfoIn.bpp = 8;
      AddrSurfInfoIn.flags.depth = 0;
      AddrSurfInfoIn.flags.stencil = 1;

      r = gfx9_compute_miptree(ws, surf, compressed, &AddrSurfInfoIn);
      if (r)
         return r;
   }

   surf->is_linear = surf->u.gfx9.surf.swizzle_mode == ADDR_SW_LINEAR;
   surf->num_dcc_levels = surf->dcc_size ? tex->last_level + 1 : 0;

   switch (surf->u.gfx9.surf.swizzle_mode) {
   /* S = standard. */
   case ADDR_SW_256B_S:
   case ADDR_SW_4KB_S:
   case ADDR_SW_64KB_S:
   case ADDR_SW_VAR_S:
   case ADDR_SW_64KB_S_T:
   case ADDR_SW_4KB_S_X:
   case ADDR_SW_64KB_S_X:
   case ADDR_SW_VAR_S_X:
      surf->micro_tile_mode = RADEON_MICRO_MODE_THIN;
      break;

   /* D = display. */
   case ADDR_SW_LINEAR:
   case ADDR_SW_256B_D:
   case ADDR_SW_4KB_D:
   case ADDR_SW_64KB_D:
   case ADDR_SW_VAR_D:
   case ADDR_SW_64KB_D_T:
   case ADDR_SW_4KB_D_X:
   case ADDR_SW_64KB_D_X:
   case ADDR_SW_VAR_D_X:
      surf->micro_tile_mode = RADEON_MICRO_MODE_DISPLAY;
      break;

   /* R = rotated. */
   case ADDR_SW_256B_R:
   case ADDR_SW_4KB_R:
   case ADDR_SW_64KB_R:
   case ADDR_SW_VAR_R:
   case ADDR_SW_64KB_R_T:
   case ADDR_SW_4KB_R_X:
   case ADDR_SW_64KB_R_X:
   case ADDR_SW_VAR_R_X:
      surf->micro_tile_mode = RADEON_MICRO_MODE_ROTATED;
      break;

   /* Z = depth. */
   case ADDR_SW_4KB_Z:
   case ADDR_SW_64KB_Z:
   case ADDR_SW_VAR_Z:
   case ADDR_SW_64KB_Z_T:
   case ADDR_SW_4KB_Z_X:
   case ADDR_SW_64KB_Z_X:
   case ADDR_SW_VAR_Z_X:
      surf->micro_tile_mode = RADEON_MICRO_MODE_DEPTH;
      break;

   default:
      assert(0);
   }

   return 0;
}

void amdgpu_surface_init_functions(struct amdgpu_winsys *ws)
{
   if (ws->info.chip_class >= GFX9)
      ws->base.surface_init = gfx9_surface_init;
   else
      ws->base.surface_init = gfx6_surface_init;
}
