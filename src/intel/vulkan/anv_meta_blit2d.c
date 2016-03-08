/*
 * Copyright © 2016 Intel Corporation
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

#include "anv_meta.h"

static VkFormat
vk_format_for_size(int bs)
{
   /* The choice of UNORM and UINT formats is very intentional here.  Most of
    * the time, we want to use a UINT format to avoid any rounding error in
    * the blit.  For stencil blits, R8_UINT is required by the hardware.
    * (It's the only format allowed in conjunction with W-tiling.)  Also we
    * intentionally use the 4-channel formats whenever we can.  This is so
    * that, when we do a RGB <-> RGBX copy, the two formats will line up even
    * though one of them is 3/4 the size of the other.  The choice of UNORM
    * vs. UINT is also very intentional because Haswell doesn't handle 8 or
    * 16-bit RGB UINT formats at all so we have to use UNORM there.
    * Fortunately, the only time we should ever use two different formats in
    * the table below is for RGB -> RGBA blits and so we will never have any
    * UNORM/UINT mismatch.
    */
   switch (bs) {
   case 1: return VK_FORMAT_R8_UINT;
   case 2: return VK_FORMAT_R8G8_UINT;
   case 3: return VK_FORMAT_R8G8B8_UNORM;
   case 4: return VK_FORMAT_R8G8B8A8_UNORM;
   case 6: return VK_FORMAT_R16G16B16_UNORM;
   case 8: return VK_FORMAT_R16G16B16A16_UNORM;
   case 12: return VK_FORMAT_R32G32B32_UINT;
   case 16: return VK_FORMAT_R32G32B32A32_UINT;
   default:
      unreachable("Invalid format block size");
   }
}

void
anv_meta_end_blit2d(struct anv_cmd_buffer *cmd_buffer,
                    struct anv_meta_saved_state *save)
{
   anv_meta_restore(save, cmd_buffer);
}

void
anv_meta_begin_blit2d(struct anv_cmd_buffer *cmd_buffer,
                      struct anv_meta_saved_state *save)
{
   anv_meta_save(save, cmd_buffer,
                 (1 << VK_DYNAMIC_STATE_VIEWPORT));
}

void
anv_meta_blit2d(struct anv_cmd_buffer *cmd_buffer,
                struct anv_meta_blit2d_surf *src,
                struct anv_meta_blit2d_surf *dst,
                unsigned num_rects,
                struct anv_meta_blit2d_rect *rects)
{
   VkDevice vk_device = anv_device_to_handle(cmd_buffer->device);
   VkFormat src_format = vk_format_for_size(src->bs);
   VkFormat dst_format = vk_format_for_size(dst->bs);
   VkImageUsageFlags src_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
   VkImageUsageFlags dst_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   for (unsigned r = 0; r < num_rects; ++r) {

      /* Create VkImages */
      VkImageCreateInfo image_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = 0, /* TEMPLATE */
         .extent = {
            .width = 0, /* TEMPLATE */
            .height = 0, /* TEMPLATE */
            .depth = 1,
         },
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = 1,
         .tiling = 0, /* TEMPLATE */
         .usage = 0, /* TEMPLATE */
      };
      struct anv_image_create_info anv_image_info = {
         .vk_info = &image_info,
         .isl_tiling_flags = 0, /* TEMPLATE */
      };

      /* The image height is the rect height + src/dst y-offset from the
       * tile-aligned base address.
       */
      struct isl_tile_info tile_info;

      anv_image_info.isl_tiling_flags = 1 << src->tiling;
      image_info.tiling = anv_image_info.isl_tiling_flags ==
                          ISL_TILING_LINEAR_BIT ?
                          VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
      image_info.usage = src_usage;
      image_info.format = src_format,
      isl_tiling_get_info(&cmd_buffer->device->isl_dev, src->tiling, src->bs,
                          &tile_info);
      image_info.extent.height = rects[r].height +
                                 rects[r].src_y % tile_info.height;
      image_info.extent.width = src->pitch / src->bs;
      VkImage src_image;
      anv_image_create(vk_device, &anv_image_info,
                       &cmd_buffer->pool->alloc, &src_image);

      anv_image_info.isl_tiling_flags = 1 << dst->tiling;
      image_info.tiling = anv_image_info.isl_tiling_flags ==
                          ISL_TILING_LINEAR_BIT ?
                          VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
      image_info.usage = dst_usage;
      image_info.format = dst_format,
      isl_tiling_get_info(&cmd_buffer->device->isl_dev, dst->tiling, dst->bs,
                          &tile_info);
      image_info.extent.height = rects[r].height +
                                 rects[r].dst_y % tile_info.height;
      image_info.extent.width = dst->pitch / dst->bs;
      VkImage dst_image;
      anv_image_create(vk_device, &anv_image_info,
                       &cmd_buffer->pool->alloc, &dst_image);

      /* We could use a vk call to bind memory, but that would require
      * creating a dummy memory object etc. so there's really no point.
      */
      anv_image_from_handle(src_image)->bo = src->bo;
      anv_image_from_handle(src_image)->offset = src->base_offset;
      anv_image_from_handle(dst_image)->bo = dst->bo;
      anv_image_from_handle(dst_image)->offset = dst->base_offset;

      /* Create VkImageViews */
      VkImageViewCreateInfo iview_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = 0, /* TEMPLATE */
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = 0, /* TEMPLATE */
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
         },
      };
      uint32_t img_o = 0;

      iview_info.image = src_image;
      iview_info.format = src_format;
      VkOffset3D src_offset_el = {0};
      isl_surf_get_image_intratile_offset_el_xy(&cmd_buffer->device->isl_dev,
                                                &anv_image_from_handle(src_image)->
                                                   color_surface.isl,
                                                rects[r].src_x,
                                                rects[r].src_y,
                                                &img_o,
                                                (uint32_t*)&src_offset_el.x,
                                                (uint32_t*)&src_offset_el.y);

      struct anv_image_view src_iview;
      anv_image_view_init(&src_iview, cmd_buffer->device,
         &iview_info, cmd_buffer, img_o, src_usage);

      iview_info.image = dst_image;
      iview_info.format = dst_format;
      VkOffset3D dst_offset_el = {0};
      isl_surf_get_image_intratile_offset_el_xy(&cmd_buffer->device->isl_dev,
                                                &anv_image_from_handle(dst_image)->
                                                   color_surface.isl,
                                                rects[r].dst_x,
                                                rects[r].dst_y,
                                                &img_o,
                                                (uint32_t*)&dst_offset_el.x,
                                                (uint32_t*)&dst_offset_el.y);
      struct anv_image_view dst_iview;
      anv_image_view_init(&dst_iview, cmd_buffer->device,
         &iview_info, cmd_buffer, img_o, dst_usage);

      /* Perform blit */
      anv_meta_emit_blit(cmd_buffer,
                     anv_image_from_handle(src_image),
                     &src_iview,
                     src_offset_el,
                     (VkExtent3D){rects[r].width, rects[r].height, 1},
                     anv_image_from_handle(dst_image),
                     &dst_iview,
                     dst_offset_el,
                     (VkExtent3D){rects[r].width, rects[r].height, 1},
                     VK_FILTER_NEAREST);

      anv_DestroyImage(vk_device, src_image, &cmd_buffer->pool->alloc);
      anv_DestroyImage(vk_device, dst_image, &cmd_buffer->pool->alloc);
   }
}

