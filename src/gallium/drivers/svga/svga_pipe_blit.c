/**********************************************************
 * Copyright 2008-2009 VMware, Inc.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************/

#include "svga_resource_texture.h"
#include "svga_context.h"
#include "svga_debug.h"
#include "svga_cmd.h"
#include "svga_surface.h"

//#include "util/u_blit_sw.h"
#include "util/u_format.h"
#include "util/u_surface.h"

#define FILE_DEBUG_FLAG DEBUG_BLIT


/**
 * Copy an image between textures with the vgpu10 CopyRegion command.
 */
static void
copy_region_vgpu10(struct svga_context *svga, struct pipe_resource *src_tex,
                    unsigned src_x, unsigned src_y, unsigned src_z,
                    unsigned src_level, unsigned src_face,
                    struct pipe_resource *dst_tex,
                    unsigned dst_x, unsigned dst_y, unsigned dst_z,
                    unsigned dst_level, unsigned dst_face,
                    unsigned width, unsigned height, unsigned depth)
{
   enum pipe_error ret;
   uint32 srcSubResource, dstSubResource;
   struct svga_texture *dtex, *stex;
   SVGA3dCopyBox box;
   int i, num_layers = 1;

   stex = svga_texture(src_tex);
   dtex = svga_texture(dst_tex);

   box.x = dst_x;
   box.y = dst_y;
   box.z = dst_z;
   box.w = width;
   box.h = height;
   box.d = depth;
   box.srcx = src_x;
   box.srcy = src_y;
   box.srcz = src_z;

   if (src_tex->target == PIPE_TEXTURE_1D_ARRAY ||
       src_tex->target == PIPE_TEXTURE_2D_ARRAY) {
      /* copy layer by layer */
      box.z = 0;
      box.d = 1;
      box.srcz = 0;

      num_layers = depth;
      src_face = src_z;
      dst_face = dst_z;
   }

   /* loop over array layers */
   for (i = 0; i < num_layers; i++) {
      srcSubResource = (src_face + i) * (src_tex->last_level + 1) + src_level;
      dstSubResource = (dst_face + i) * (dst_tex->last_level + 1) + dst_level;

      ret = SVGA3D_vgpu10_PredCopyRegion(svga->swc,
                                         dtex->handle, dstSubResource,
                                         stex->handle, srcSubResource, &box);
      if (ret != PIPE_OK) {
         svga_context_flush(svga, NULL);
         ret = SVGA3D_vgpu10_PredCopyRegion(svga->swc,
                                            dtex->handle, dstSubResource,
                                            stex->handle, srcSubResource, &box);
         assert(ret == PIPE_OK);
      }

      svga_define_texture_level(dtex, dst_face + i, dst_level);
   }
}


static void
svga_resource_copy_region(struct pipe_context *pipe,
                          struct pipe_resource *dst_tex,
                          unsigned dst_level,
                          unsigned dstx, unsigned dsty, unsigned dstz,
                          struct pipe_resource *src_tex,
                          unsigned src_level,
                          const struct pipe_box *src_box)
{
   struct svga_context *svga = svga_context(pipe);
   struct svga_texture *stex, *dtex;
   unsigned dst_face_layer, dst_z, src_face_layer, src_z;

   /* Emit buffered drawing commands, and any back copies.
    */
   svga_surfaces_flush( svga );

   /* Fallback for buffers. */
   if (dst_tex->target == PIPE_BUFFER && src_tex->target == PIPE_BUFFER) {
      util_resource_copy_region(pipe, dst_tex, dst_level, dstx, dsty, dstz,
                                src_tex, src_level, src_box);
      return;
   }

   stex = svga_texture(src_tex);
   dtex = svga_texture(dst_tex);

   if (src_tex->target == PIPE_TEXTURE_CUBE ||
       src_tex->target == PIPE_TEXTURE_1D_ARRAY) {
      src_face_layer = src_box->z;
      src_z = 0;
      assert(src_box->depth == 1);
   }
   else {
      src_face_layer = 0;
      src_z = src_box->z;
   }

   /* different src/dst type???*/
   if (dst_tex->target == PIPE_TEXTURE_CUBE ||
       dst_tex->target == PIPE_TEXTURE_1D_ARRAY) {
      dst_face_layer = dstz;
      dst_z = 0;
      assert(src_box->depth == 1);
   }
   else {
      dst_face_layer = 0;
      dst_z = dstz;
   }

   svga_texture_copy_handle(svga,
                            stex->handle,
                            src_box->x, src_box->y, src_z,
                            src_level, src_face_layer,
                            dtex->handle,
                            dstx, dsty, dst_z,
                            dst_level, dst_face_layer,
                            src_box->width, src_box->height, src_box->depth);

   /* Mark the destination image as being defined */
   svga_define_texture_level(dtex, dst_face_layer, dst_level);
}


/**
 * The state tracker implements some resource copies with blits (for
 * GL_ARB_copy_image).  This function checks if we should really do the blit
 * with a VGPU10 CopyRegion command or software fallback (for incompatible
 * src/dst formats).
 */
static bool
can_blit_via_copy_region_vgpu10(struct svga_context *svga,
                                const struct pipe_blit_info *blit_info)
{
   struct svga_texture *dtex, *stex;

   if (!svga_have_vgpu10(svga))
      return false;

   stex = svga_texture(blit_info->src.resource);
   dtex = svga_texture(blit_info->src.resource);

   // can't copy within one resource
   if (stex->handle == dtex->handle)
      return false;

   // can't copy between different resource types
   if (blit_info->src.resource->target != blit_info->dst.resource->target)
      return false;

   // check that the blit src/dst regions are same size, no flipping, etc.
   if (blit_info->src.box.width != blit_info->dst.box.width ||
       blit_info->src.box.height != blit_info->dst.box.height)
      return false;

   // depth/stencil copies not supported at this time
   if (blit_info->mask != PIPE_MASK_RGBA)
      return false;

   if (blit_info->alpha_blend || blit_info->render_condition_enable ||
       blit_info->scissor_enable)
      return false;

   // check that src/dst surface formats are compatible for the VGPU device.
   return util_is_format_compatible(
        util_format_description(blit_info->src.resource->format),
        util_format_description(blit_info->dst.resource->format));
}


static void
svga_blit(struct pipe_context *pipe,
          const struct pipe_blit_info *blit_info)
{
   struct svga_context *svga = svga_context(pipe);

   if (!svga_have_vgpu10(svga) &&
       blit_info->src.resource->nr_samples > 1 &&
       blit_info->dst.resource->nr_samples <= 1 &&
       !util_format_is_depth_or_stencil(blit_info->src.resource->format) &&
       !util_format_is_pure_integer(blit_info->src.resource->format)) {
      debug_printf("svga: color resolve unimplemented\n");
      return;
   }

   if (can_blit_via_copy_region_vgpu10(svga, blit_info)) {
      unsigned src_face, src_z, dst_face, dst_z;

      if (blit_info->src.resource->target == PIPE_TEXTURE_CUBE) {
         src_face = blit_info->src.box.z;
         src_z = 0;
         assert(blit_info->src.box.depth == 1);
      }
      else {
         src_face = 0;
         src_z = blit_info->src.box.z;
      }

      if (blit_info->dst.resource->target == PIPE_TEXTURE_CUBE) {
         dst_face = blit_info->dst.box.z;
         dst_z = 0;
         assert(blit_info->src.box.depth == 1);
      }
      else {
         dst_face = 0;
         dst_z = blit_info->dst.box.z;
      }

      copy_region_vgpu10(svga,
                         blit_info->src.resource,
                         blit_info->src.box.x, blit_info->src.box.y, src_z,
                         blit_info->src.level, src_face,
                         blit_info->dst.resource,
                         blit_info->dst.box.x, blit_info->dst.box.y, dst_z,
                         blit_info->dst.level, dst_face,
                         blit_info->src.box.width, blit_info->src.box.height,
                         blit_info->src.box.depth);
      return;
   }

   if (util_try_blit_via_copy_region(pipe, blit_info)) {
      return; /* done */
   }

   if ((blit_info->mask & PIPE_MASK_S) ||
       !util_blitter_is_blit_supported(svga->blitter, blit_info)) {
      debug_printf("svga: blit unsupported %s -> %s\n",
                   util_format_short_name(blit_info->src.resource->format),
                   util_format_short_name(blit_info->dst.resource->format));
      return;
   }

   /* XXX turn off occlusion and streamout queries */

   util_blitter_save_vertex_buffer_slot(svga->blitter, svga->curr.vb);
   util_blitter_save_vertex_elements(svga->blitter, (void*)svga->curr.velems);
   util_blitter_save_vertex_shader(svga->blitter, svga->curr.vs);
   util_blitter_save_geometry_shader(svga->blitter, svga->curr.user_gs);
   util_blitter_save_so_targets(svga->blitter, svga->num_so_targets,
                     (struct pipe_stream_output_target**)svga->so_targets);
   util_blitter_save_rasterizer(svga->blitter, (void*)svga->curr.rast);
   util_blitter_save_viewport(svga->blitter, &svga->curr.viewport);
   util_blitter_save_scissor(svga->blitter, &svga->curr.scissor);
   util_blitter_save_fragment_shader(svga->blitter, svga->curr.fs);
   util_blitter_save_blend(svga->blitter, (void*)svga->curr.blend);
   util_blitter_save_depth_stencil_alpha(svga->blitter,
                                         (void*)svga->curr.depth);
   util_blitter_save_stencil_ref(svga->blitter, &svga->curr.stencil_ref);
   util_blitter_save_sample_mask(svga->blitter, svga->curr.sample_mask);
   util_blitter_save_framebuffer(svga->blitter, &svga->curr.framebuffer);
   util_blitter_save_fragment_sampler_states(svga->blitter,
                     svga->curr.num_samplers[PIPE_SHADER_FRAGMENT],
                     (void**)svga->curr.sampler[PIPE_SHADER_FRAGMENT]);
   util_blitter_save_fragment_sampler_views(svga->blitter,
                     svga->curr.num_sampler_views[PIPE_SHADER_FRAGMENT],
                     svga->curr.sampler_views[PIPE_SHADER_FRAGMENT]);
   /*util_blitter_save_render_condition(svga->blitter, svga->render_cond_query,
                                      svga->render_cond_cond, svga->render_cond_mode);*/
   util_blitter_blit(svga->blitter, blit_info);
}


static void
svga_flush_resource(struct pipe_context *pipe,
                    struct pipe_resource *resource)
{
}


void
svga_init_blit_functions(struct svga_context *svga)
{
   svga->pipe.resource_copy_region = svga_resource_copy_region;
   svga->pipe.blit = svga_blit;
   svga->pipe.flush_resource = svga_flush_resource;
}
