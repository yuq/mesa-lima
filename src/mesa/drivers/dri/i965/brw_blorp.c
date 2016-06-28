/*
 * Copyright © 2012 Intel Corporation
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
#include "intel_batchbuffer.h"
#include "intel_fbo.h"

#include "brw_blorp.h"
#include "brw_compiler.h"
#include "brw_nir.h"
#include "brw_state.h"

#define FILE_DEBUG_FLAG DEBUG_BLORP

/**
 * A variant of isl_surf_get_image_offset_sa() specific to gen6 stencil and
 * HiZ surfaces.
 */
static void
get_image_offset_sa_gen6_stencil(const struct isl_surf *surf,
                                 uint32_t level, uint32_t logical_array_layer,
                                 uint32_t *x_offset_sa,
                                 uint32_t *y_offset_sa)
{
   assert(surf->tiling == ISL_TILING_W || surf->format == ISL_FORMAT_HIZ);
   assert(level < surf->levels);
   assert(logical_array_layer < surf->logical_level0_px.array_len);

   const struct isl_extent3d image_align_sa =
      isl_surf_get_image_alignment_sa(surf);

   const uint32_t W0 = surf->phys_level0_sa.width;
   const uint32_t H0 = surf->phys_level0_sa.height;

   uint32_t x = 0, y = 0;
   for (uint32_t l = 0; l < level; ++l) {
      if (l == 1) {
         uint32_t W = minify(W0, l);

         if (surf->samples > 1) {
            assert(surf->msaa_layout == ISL_MSAA_LAYOUT_INTERLEAVED);
            assert(surf->samples == 4);
            W = ALIGN(W, 2) * 2;
         }

         x += ALIGN(W, image_align_sa.w);
      } else {
         uint32_t H = minify(H0, l);

         if (surf->samples > 1) {
            assert(surf->msaa_layout == ISL_MSAA_LAYOUT_INTERLEAVED);
            assert(surf->samples == 4);
            H = ALIGN(H, 2) * 2;
         }

         y += ALIGN(H, image_align_sa.h) * surf->logical_level0_px.array_len;
      }
   }

   /* Now account for our location within the given LOD */
   uint32_t Hl = minify(H0, level);
   if (surf->samples > 1) {
      assert(surf->msaa_layout == ISL_MSAA_LAYOUT_INTERLEAVED);
      assert(surf->samples == 4);
      Hl = ALIGN(Hl, 2) * 2;
   }
   y += ALIGN(Hl, image_align_sa.h) * logical_array_layer;

   *x_offset_sa = x;
   *y_offset_sa = y;
}

void
blorp_get_image_offset_sa(struct isl_device *dev, const struct isl_surf *surf,
                          uint32_t level, uint32_t layer,
                          uint32_t *x_offset_sa,
                          uint32_t *y_offset_sa)
{
   if (ISL_DEV_GEN(dev) == 6 && surf->tiling == ISL_TILING_W) {
      get_image_offset_sa_gen6_stencil(surf, level, layer,
                                       x_offset_sa, y_offset_sa);
   } else {
      isl_surf_get_image_offset_sa(surf, level, layer, 0,
                                   x_offset_sa, y_offset_sa);
   }
}

void
brw_blorp_surface_info_init(struct brw_context *brw,
                            struct brw_blorp_surface_info *info,
                            struct intel_mipmap_tree *mt,
                            unsigned int level, unsigned int layer,
                            mesa_format format, bool is_render_target)
{
   /* Layer is a physical layer, so if this is a 2D multisample array texture
    * using INTEL_MSAA_LAYOUT_UMS or INTEL_MSAA_LAYOUT_CMS, then it had better
    * be a multiple of num_samples.
    */
   unsigned layer_multiplier = 1;
   if (mt->msaa_layout == INTEL_MSAA_LAYOUT_UMS ||
       mt->msaa_layout == INTEL_MSAA_LAYOUT_CMS) {
      assert(mt->num_samples <= 1 || layer % mt->num_samples == 0);
      layer_multiplier = MAX2(mt->num_samples, 1);
   }

   intel_miptree_check_level_layer(mt, level, layer);

   info->mt = mt;

   intel_miptree_get_isl_surf(brw, mt, &info->surf);

   if (mt->mcs_mt) {
      intel_miptree_get_aux_isl_surf(brw, mt, &info->aux_surf,
                                     &info->aux_usage);
   } else {
      info->aux_usage = ISL_AUX_USAGE_NONE;
   }

   info->view = (struct isl_view) {
      .usage = is_render_target ? ISL_SURF_USAGE_RENDER_TARGET_BIT :
                                  ISL_SURF_USAGE_TEXTURE_BIT,
      .format = ISL_FORMAT_UNSUPPORTED, /* Set later */
      .base_level = level,
      .levels = 1,
      .channel_select = {
         ISL_CHANNEL_SELECT_RED,
         ISL_CHANNEL_SELECT_GREEN,
         ISL_CHANNEL_SELECT_BLUE,
         ISL_CHANNEL_SELECT_ALPHA,
      },
   };

   if (!is_render_target &&
       (info->surf.dim == ISL_SURF_DIM_3D ||
        info->surf.msaa_layout == ISL_MSAA_LAYOUT_ARRAY)) {
      /* 3-D textures don't support base_array layer and neither do 2-D
       * multisampled textures on IVB so we need to pass it through the
       * sampler in those cases.  These are also two cases where we are
       * guaranteed that we won't be doing any funny surface hacks.
       */
      info->view.base_array_layer = 0;
      info->view.array_len = MAX2(info->surf.logical_level0_px.depth,
                                  info->surf.logical_level0_px.array_len);
      info->z_offset = layer / layer_multiplier;
   } else {
      info->view.base_array_layer = layer / layer_multiplier;
      info->view.array_len = 1;
      info->z_offset = 0;
   }

   info->level = level;
   info->layer = layer;

   if (format == MESA_FORMAT_NONE)
      format = mt->format;

   switch (format) {
   case MESA_FORMAT_S_UINT8:
      assert(info->surf.tiling == ISL_TILING_W);
      /* Prior to Broadwell, we can't render to R8_UINT */
      info->view.format = brw->gen >= 8 ? BRW_SURFACEFORMAT_R8_UINT :
                                          BRW_SURFACEFORMAT_R8_UNORM;
      break;
   case MESA_FORMAT_Z24_UNORM_X8_UINT:
      /* It would make sense to use BRW_SURFACEFORMAT_R24_UNORM_X8_TYPELESS
       * here, but unfortunately it isn't supported as a render target, which
       * would prevent us from blitting to 24-bit depth.
       *
       * The miptree consists of 32 bits per pixel, arranged as 24-bit depth
       * values interleaved with 8 "don't care" bits.  Since depth values don't
       * require any blending, it doesn't matter how we interpret the bit
       * pattern as long as we copy the right amount of data, so just map it
       * as 8-bit BGRA.
       */
      info->view.format = BRW_SURFACEFORMAT_B8G8R8A8_UNORM;
      break;
   case MESA_FORMAT_Z_FLOAT32:
      info->view.format = BRW_SURFACEFORMAT_R32_FLOAT;
      break;
   case MESA_FORMAT_Z_UNORM16:
      info->view.format = BRW_SURFACEFORMAT_R16_UNORM;
      break;
   default: {
      if (is_render_target) {
         assert(brw->format_supported_as_render_target[format]);
         info->view.format = brw->render_target_format[format];
      } else {
         info->view.format = brw_format_for_mesa_format(format);
      }
      break;
   }
   }
}


void
brw_blorp_params_init(struct brw_blorp_params *params)
{
   memset(params, 0, sizeof(*params));
   params->hiz_op = GEN6_HIZ_OP_NONE;
   params->fast_clear_op = 0;
   params->num_draw_buffers = 1;
   params->num_layers = 1;
}

void
brw_blorp_init_wm_prog_key(struct brw_wm_prog_key *wm_key)
{
   memset(wm_key, 0, sizeof(*wm_key));
   wm_key->nr_color_regions = 1;
   for (int i = 0; i < MAX_SAMPLERS; i++)
      wm_key->tex.swizzles[i] = SWIZZLE_XYZW;
}

static int
nir_uniform_type_size(const struct glsl_type *type)
{
   /* Only very basic types are allowed */
   assert(glsl_type_is_vector_or_scalar(type));
   assert(glsl_get_bit_size(type) == 32);

   return glsl_get_vector_elements(type) * 4;
}

const unsigned *
brw_blorp_compile_nir_shader(struct brw_context *brw, struct nir_shader *nir,
                             const struct brw_wm_prog_key *wm_key,
                             bool use_repclear,
                             struct brw_blorp_prog_data *prog_data,
                             unsigned *program_size)
{
   const struct brw_compiler *compiler = brw->intelScreen->compiler;

   void *mem_ctx = ralloc_context(NULL);

   /* Calling brw_preprocess_nir and friends is destructive and, if cloning is
    * enabled, may end up completely replacing the nir_shader.  Therefore, we
    * own it and might as well put it in our context for easy cleanup.
    */
   ralloc_steal(mem_ctx, nir);
   nir->options =
      compiler->glsl_compiler_options[MESA_SHADER_FRAGMENT].NirOptions;

   struct brw_wm_prog_data wm_prog_data;
   memset(&wm_prog_data, 0, sizeof(wm_prog_data));

   wm_prog_data.base.nr_params = 0;
   wm_prog_data.base.param = NULL;

   /* BLORP always just uses the first two binding table entries */
   wm_prog_data.binding_table.render_target_start = 0;
   wm_prog_data.base.binding_table.texture_start = 1;

   nir = brw_preprocess_nir(compiler, nir);
   nir_remove_dead_variables(nir, nir_var_shader_in);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir)->impl);

   /* Uniforms are required to be lowered before going into compile_fs.  For
    * BLORP, we'll assume that whoever builds the shader sets the location
    * they want so we just need to lower them and figure out how many we have
    * in total.
    */
   nir->num_uniforms = 0;
   nir_foreach_variable(var, &nir->uniforms) {
      var->data.driver_location = var->data.location;
      unsigned end = var->data.location + nir_uniform_type_size(var->type);
      nir->num_uniforms = MAX2(nir->num_uniforms, end);
   }
   nir_lower_io(nir, nir_var_uniform, nir_uniform_type_size);

   const unsigned *program =
      brw_compile_fs(compiler, brw, mem_ctx, wm_key, &wm_prog_data, nir,
                     NULL, -1, -1, false, use_repclear, program_size, NULL);

   /* Copy the relavent bits of wm_prog_data over into the blorp prog data */
   prog_data->dispatch_8 = wm_prog_data.dispatch_8;
   prog_data->dispatch_16 = wm_prog_data.dispatch_16;
   prog_data->first_curbe_grf_0 = wm_prog_data.base.dispatch_grf_start_reg;
   prog_data->first_curbe_grf_2 = wm_prog_data.dispatch_grf_start_reg_2;
   prog_data->ksp_offset_2 = wm_prog_data.prog_offset_2;
   prog_data->persample_msaa_dispatch = wm_prog_data.persample_dispatch;
   prog_data->flat_inputs = wm_prog_data.flat_inputs;
   prog_data->num_varying_inputs = wm_prog_data.num_varying_inputs;
   prog_data->inputs_read = nir->info.inputs_read;

   assert(wm_prog_data.base.nr_params == 0);

   return program;
}

struct surface_state_info {
   unsigned num_dwords;
   unsigned ss_align; /* Required alignment of RENDER_SURFACE_STATE in bytes */
   unsigned reloc_dw;
   unsigned aux_reloc_dw;
   unsigned tex_mocs;
   unsigned rb_mocs;
};

static const struct surface_state_info surface_state_infos[] = {
   [6] = {6,  32, 1,  0},
   [7] = {8,  32, 1,  6,  GEN7_MOCS_L3, GEN7_MOCS_L3},
   [8] = {13, 64, 8,  10, BDW_MOCS_WB,  BDW_MOCS_PTE},
   [9] = {16, 64, 8,  10, SKL_MOCS_WB,  SKL_MOCS_PTE},
};

uint32_t
brw_blorp_emit_surface_state(struct brw_context *brw,
                             const struct brw_blorp_surface_info *surface,
                             uint32_t read_domains, uint32_t write_domain,
                             bool is_render_target)
{
   const struct surface_state_info ss_info = surface_state_infos[brw->gen];

   struct isl_surf surf = surface->surf;

   if (surf.dim == ISL_SURF_DIM_1D &&
       surf.dim_layout == ISL_DIM_LAYOUT_GEN4_2D) {
      assert(surf.logical_level0_px.height == 1);
      surf.dim = ISL_SURF_DIM_2D;
   }

   union isl_color_value clear_color = { .u32 = { 0, 0, 0, 0 } };

   const struct isl_surf *aux_surf = NULL;
   uint64_t aux_offset = 0;
   if (surface->mt->mcs_mt &&
       (is_render_target ||
        surface->mt->fast_clear_state != INTEL_FAST_CLEAR_STATE_RESOLVED)) {
      aux_surf = &surface->aux_surf;
      assert(surface->mt->mcs_mt->offset == 0);
      aux_offset = surface->mt->mcs_mt->bo->offset64;

      /* We only really need a clear color if we also have an auxiliary
       * surface.  Without one, it does nothing.
       */
      clear_color = intel_miptree_get_isl_clear_color(brw, surface->mt);
   }

   uint32_t surf_offset;
   uint32_t *dw = brw_state_batch(brw, AUB_TRACE_SURFACE_STATE,
                                  ss_info.num_dwords * 4, ss_info.ss_align,
                                  &surf_offset);

   const uint32_t mocs = is_render_target ? ss_info.rb_mocs : ss_info.tex_mocs;

   isl_surf_fill_state(&brw->isl_dev, dw, .surf = &surf, .view = &surface->view,
                       .address = surface->mt->bo->offset64 + surface->bo_offset,
                       .aux_surf = aux_surf, .aux_usage = surface->aux_usage,
                       .aux_address = aux_offset,
                       .mocs = mocs, .clear_color = clear_color,
                       .x_offset_sa = surface->tile_x_sa,
                       .y_offset_sa = surface->tile_y_sa);

   /* Emit relocation to surface contents */
   drm_intel_bo_emit_reloc(brw->batch.bo,
                           surf_offset + ss_info.reloc_dw * 4,
                           surface->mt->bo,
                           dw[ss_info.reloc_dw] - surface->mt->bo->offset64,
                           read_domains, write_domain);

   if (aux_surf) {
      /* On gen7 and prior, the bottom 12 bits of the MCS base address are
       * used to store other information.  This should be ok, however, because
       * surface buffer addresses are always 4K page alinged.
       */
      assert((aux_offset & 0xfff) == 0);
      drm_intel_bo_emit_reloc(brw->batch.bo,
                              surf_offset + ss_info.aux_reloc_dw * 4,
                              surface->mt->mcs_mt->bo,
                              dw[ss_info.aux_reloc_dw] & 0xfff,
                              read_domains, write_domain);
   }

   return surf_offset;
}

/**
 * Perform a HiZ or depth resolve operation.
 *
 * For an overview of HiZ ops, see the following sections of the Sandy Bridge
 * PRM, Volume 1, Part 2:
 *   - 7.5.3.1 Depth Buffer Clear
 *   - 7.5.3.2 Depth Buffer Resolve
 *   - 7.5.3.3 Hierarchical Depth Buffer Resolve
 */
void
intel_hiz_exec(struct brw_context *brw, struct intel_mipmap_tree *mt,
	       unsigned int level, unsigned int layer, enum gen6_hiz_op op)
{
   const char *opname = NULL;

   switch (op) {
   case GEN6_HIZ_OP_DEPTH_RESOLVE:
      opname = "depth resolve";
      break;
   case GEN6_HIZ_OP_HIZ_RESOLVE:
      opname = "hiz ambiguate";
      break;
   case GEN6_HIZ_OP_DEPTH_CLEAR:
      opname = "depth clear";
      break;
   case GEN6_HIZ_OP_NONE:
      opname = "noop?";
      break;
   }

   DBG("%s %s to mt %p level %d layer %d\n",
       __func__, opname, mt, level, layer);

   if (brw->gen >= 8) {
      gen8_hiz_exec(brw, mt, level, layer, op);
   } else {
      gen6_blorp_hiz_exec(brw, mt, level, layer, op);
   }
}

void
brw_blorp_exec(struct brw_context *brw, const struct brw_blorp_params *params)
{
   struct gl_context *ctx = &brw->ctx;
   const uint32_t estimated_max_batch_usage = brw->gen >= 8 ? 1800 : 1500;
   bool check_aperture_failed_once = false;

   /* Flush the sampler and render caches.  We definitely need to flush the
    * sampler cache so that we get updated contents from the render cache for
    * the glBlitFramebuffer() source.  Also, we are sometimes warned in the
    * docs to flush the cache between reinterpretations of the same surface
    * data with different formats, which blorp does for stencil and depth
    * data.
    */
   brw_emit_mi_flush(brw);

   brw_select_pipeline(brw, BRW_RENDER_PIPELINE);

retry:
   intel_batchbuffer_require_space(brw, estimated_max_batch_usage, RENDER_RING);
   intel_batchbuffer_save_state(brw);
   drm_intel_bo *saved_bo = brw->batch.bo;
   uint32_t saved_used = USED_BATCH(brw->batch);
   uint32_t saved_state_batch_offset = brw->batch.state_batch_offset;

   switch (brw->gen) {
   case 6:
      gen6_blorp_exec(brw, params);
      break;
   case 7:
      gen7_blorp_exec(brw, params);
      break;
   case 8:
   case 9:
      gen8_blorp_exec(brw, params);
      break;
   default:
      /* BLORP is not supported before Gen6. */
      unreachable("not reached");
   }

   /* Make sure we didn't wrap the batch unintentionally, and make sure we
    * reserved enough space that a wrap will never happen.
    */
   assert(brw->batch.bo == saved_bo);
   assert((USED_BATCH(brw->batch) - saved_used) * 4 +
          (saved_state_batch_offset - brw->batch.state_batch_offset) <
          estimated_max_batch_usage);
   /* Shut up compiler warnings on release build */
   (void)saved_bo;
   (void)saved_used;
   (void)saved_state_batch_offset;

   /* Check if the blorp op we just did would make our batch likely to fail to
    * map all the BOs into the GPU at batch exec time later.  If so, flush the
    * batch and try again with nothing else in the batch.
    */
   if (dri_bufmgr_check_aperture_space(&brw->batch.bo, 1)) {
      if (!check_aperture_failed_once) {
         check_aperture_failed_once = true;
         intel_batchbuffer_reset_to_saved(brw);
         intel_batchbuffer_flush(brw);
         goto retry;
      } else {
         int ret = intel_batchbuffer_flush(brw);
         WARN_ONCE(ret == -ENOSPC,
                   "i965: blorp emit exceeded available aperture space\n");
      }
   }

   if (unlikely(brw->always_flush_batch))
      intel_batchbuffer_flush(brw);

   /* We've smashed all state compared to what the normal 3D pipeline
    * rendering tracks for GL.
    */
   brw->ctx.NewDriverState |= BRW_NEW_BLORP;
   brw->no_depth_or_stencil = false;
   brw->ib.type = -1;

   /* Flush the sampler cache so any texturing from the destination is
    * coherent.
    */
   brw_emit_mi_flush(brw);
}

void
gen6_blorp_hiz_exec(struct brw_context *brw, struct intel_mipmap_tree *mt,
                    unsigned int level, unsigned int layer, enum gen6_hiz_op op)
{
   struct brw_blorp_params params;
   brw_blorp_params_init(&params);

   params.hiz_op = op;

   brw_blorp_surface_info_init(brw, &params.depth, mt, level, layer,
                               mt->format, true);

   /* Align the rectangle primitive to 8x4 pixels.
    *
    * During fast depth clears, the emitted rectangle primitive  must be
    * aligned to 8x4 pixels.  From the Ivybridge PRM, Vol 2 Part 1 Section
    * 11.5.3.1 Depth Buffer Clear (and the matching section in the Sandybridge
    * PRM):
    *     If Number of Multisamples is NUMSAMPLES_1, the rectangle must be
    *     aligned to an 8x4 pixel block relative to the upper left corner
    *     of the depth buffer [...]
    *
    * For hiz resolves, the rectangle must also be 8x4 aligned. Item
    * WaHizAmbiguate8x4Aligned from the Haswell workarounds page and the
    * Ivybridge simulator require the alignment.
    *
    * To be safe, let's just align the rect for all hiz operations and all
    * hardware generations.
    *
    * However, for some miptree slices of a Z24 texture, emitting an 8x4
    * aligned rectangle that covers the slice may clobber adjacent slices if
    * we strictly adhered to the texture alignments specified in the PRM.  The
    * Ivybridge PRM, Section "Alignment Unit Size", states that
    * SURFACE_STATE.Surface_Horizontal_Alignment should be 4 for Z24 surfaces,
    * not 8. But commit 1f112cc increased the alignment from 4 to 8, which
    * prevents the clobbering.
    */
   params.x1 = minify(params.depth.surf.logical_level0_px.width,
                      params.depth.view.base_level);
   params.y1 = minify(params.depth.surf.logical_level0_px.height,
                      params.depth.view.base_level);
   params.x1 = ALIGN(params.x1, 8);
   params.y1 = ALIGN(params.y1, 4);

   if (params.depth.view.base_level == 0) {
      /* TODO: What about MSAA? */
      params.depth.surf.logical_level0_px.width = params.x1;
      params.depth.surf.logical_level0_px.height = params.y1;
   }

   params.dst.surf.samples = params.depth.surf.samples;
   params.dst.surf.logical_level0_px = params.depth.surf.logical_level0_px;

   assert(intel_miptree_level_has_hiz(mt, level));

   switch (mt->format) {
   case MESA_FORMAT_Z_UNORM16:
      params.depth_format = BRW_DEPTHFORMAT_D16_UNORM;
      break;
   case MESA_FORMAT_Z_FLOAT32:
      params.depth_format = BRW_DEPTHFORMAT_D32_FLOAT;
      break;
   case MESA_FORMAT_Z24_UNORM_X8_UINT:
      params.depth_format = BRW_DEPTHFORMAT_D24_UNORM_X8_UINT;
      break;
   default:
      unreachable("not reached");
   }

   brw_blorp_exec(brw, &params);
}
