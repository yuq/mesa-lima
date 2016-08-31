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

#include "anv_private.h"

static bool
lookup_blorp_shader(struct blorp_context *blorp,
                    const void *key, uint32_t key_size,
                    uint32_t *kernel_out, void *prog_data_out)
{
   struct anv_device *device = blorp->driver_ctx;

   /* The blorp cache must be a real cache */
   assert(device->blorp_shader_cache.cache);

   struct anv_shader_bin *bin =
      anv_pipeline_cache_search(&device->blorp_shader_cache, key, key_size);
   if (!bin)
      return false;

   /* The cache already has a reference and it's not going anywhere so there
    * is no need to hold a second reference.
    */
   anv_shader_bin_unref(device, bin);

   *kernel_out = bin->kernel.offset;
   *(const struct brw_stage_prog_data **)prog_data_out =
      anv_shader_bin_get_prog_data(bin);

   return true;
}

static void
upload_blorp_shader(struct blorp_context *blorp,
                    const void *key, uint32_t key_size,
                    const void *kernel, uint32_t kernel_size,
                    const void *prog_data, uint32_t prog_data_size,
                    uint32_t *kernel_out, void *prog_data_out)
{
   struct anv_device *device = blorp->driver_ctx;

   /* The blorp cache must be a real cache */
   assert(device->blorp_shader_cache.cache);

   struct anv_pipeline_bind_map bind_map = {
      .surface_count = 0,
      .sampler_count = 0,
   };

   struct anv_shader_bin *bin =
      anv_pipeline_cache_upload_kernel(&device->blorp_shader_cache,
                                       key, key_size, kernel, kernel_size,
                                       prog_data, prog_data_size, &bind_map);

   /* The cache already has a reference and it's not going anywhere so there
    * is no need to hold a second reference.
    */
   anv_shader_bin_unref(device, bin);

   *kernel_out = bin->kernel.offset;
   *(const struct brw_stage_prog_data **)prog_data_out =
      anv_shader_bin_get_prog_data(bin);
}

void
anv_device_init_blorp(struct anv_device *device)
{
   anv_pipeline_cache_init(&device->blorp_shader_cache, device, true);
   blorp_init(&device->blorp, device, &device->isl_dev);
   device->blorp.compiler = device->instance->physicalDevice.compiler;
   device->blorp.mocs.tex = device->default_mocs;
   device->blorp.mocs.rb = device->default_mocs;
   device->blorp.mocs.vb = device->default_mocs;
   device->blorp.lookup_shader = lookup_blorp_shader;
   device->blorp.upload_shader = upload_blorp_shader;
   switch (device->info.gen) {
   case 7:
      if (device->info.is_haswell) {
         device->blorp.exec = gen75_blorp_exec;
      } else {
         device->blorp.exec = gen7_blorp_exec;
      }
      break;
   case 8:
      device->blorp.exec = gen8_blorp_exec;
      break;
   case 9:
      device->blorp.exec = gen9_blorp_exec;
      break;
   default:
      unreachable("Unknown hardware generation");
   }
}

void
anv_device_finish_blorp(struct anv_device *device)
{
   blorp_finish(&device->blorp);
   anv_pipeline_cache_finish(&device->blorp_shader_cache);
}

static void
get_blorp_surf_for_anv_buffer(struct anv_device *device,
                              struct anv_buffer *buffer, uint64_t offset,
                              uint32_t width, uint32_t height,
                              uint32_t row_pitch, enum isl_format format,
                              struct blorp_surf *blorp_surf,
                              struct isl_surf *isl_surf)
{
   *blorp_surf = (struct blorp_surf) {
      .surf = isl_surf,
      .addr = {
         .buffer = buffer->bo,
         .offset = buffer->offset + offset,
      },
   };

   isl_surf_init(&device->isl_dev, isl_surf,
                 .dim = ISL_SURF_DIM_2D,
                 .format = format,
                 .width = width,
                 .height = height,
                 .depth = 1,
                 .levels = 1,
                 .array_len = 1,
                 .samples = 1,
                 .min_pitch = row_pitch,
                 .usage = ISL_SURF_USAGE_TEXTURE_BIT |
                          ISL_SURF_USAGE_RENDER_TARGET_BIT,
                 .tiling_flags = ISL_TILING_LINEAR_BIT);
   assert(isl_surf->row_pitch == row_pitch);
}

static void
get_blorp_surf_for_anv_image(const struct anv_image *image,
                             VkImageAspectFlags aspect,
                             struct blorp_surf *blorp_surf)
{
   const struct anv_surface *surface =
      anv_image_get_surface_for_aspect_mask(image, aspect);

   *blorp_surf = (struct blorp_surf) {
      .surf = &surface->isl,
      .addr = {
         .buffer = image->bo,
         .offset = image->offset + surface->offset,
      },
   };
}

void anv_CmdCopyImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageCopy*                          pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer);

   for (unsigned r = 0; r < regionCount; r++) {
      VkOffset3D srcOffset =
         anv_sanitize_image_offset(src_image->type, pRegions[r].srcOffset);
      VkOffset3D dstOffset =
         anv_sanitize_image_offset(dst_image->type, pRegions[r].dstOffset);
      VkExtent3D extent =
         anv_sanitize_image_extent(src_image->type, pRegions[r].extent);

      unsigned dst_base_layer, layer_count;
      if (dst_image->type == VK_IMAGE_TYPE_3D) {
         dst_base_layer = pRegions[r].dstOffset.z;
         layer_count = pRegions[r].extent.depth;
      } else {
         dst_base_layer = pRegions[r].dstSubresource.baseArrayLayer;
         layer_count = pRegions[r].dstSubresource.layerCount;
      }

      unsigned src_base_layer;
      if (src_image->type == VK_IMAGE_TYPE_3D) {
         src_base_layer = pRegions[r].srcOffset.z;
      } else {
         src_base_layer = pRegions[r].srcSubresource.baseArrayLayer;
         assert(pRegions[r].srcSubresource.layerCount == layer_count);
      }

      assert(pRegions[r].srcSubresource.aspectMask ==
             pRegions[r].dstSubresource.aspectMask);

      uint32_t a;
      for_each_bit(a, pRegions[r].dstSubresource.aspectMask) {
         VkImageAspectFlagBits aspect = (1 << a);

         struct blorp_surf src_surf, dst_surf;
         get_blorp_surf_for_anv_image(src_image, aspect, &src_surf);
         get_blorp_surf_for_anv_image(dst_image, aspect, &dst_surf);

         for (unsigned i = 0; i < layer_count; i++) {
            blorp_copy(&batch, &src_surf, pRegions[r].srcSubresource.mipLevel,
                       src_base_layer + i,
                       &dst_surf, pRegions[r].dstSubresource.mipLevel,
                       dst_base_layer + i,
                       srcOffset.x, srcOffset.y,
                       dstOffset.x, dstOffset.y,
                       extent.width, extent.height);
         }
      }
   }

   blorp_batch_finish(&batch);
}

static void
copy_buffer_to_image(struct anv_cmd_buffer *cmd_buffer,
                     struct anv_buffer *anv_buffer,
                     struct anv_image *anv_image,
                     uint32_t regionCount,
                     const VkBufferImageCopy* pRegions,
                     bool buffer_to_image)
{
   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer);

   struct {
      struct blorp_surf surf;
      uint32_t level;
      VkOffset3D offset;
   } image, buffer, *src, *dst;

   buffer.level = 0;
   buffer.offset = (VkOffset3D) { 0, 0, 0 };

   if (buffer_to_image) {
      src = &buffer;
      dst = &image;
   } else {
      src = &image;
      dst = &buffer;
   }

   for (unsigned r = 0; r < regionCount; r++) {
      const VkImageAspectFlags aspect = pRegions[r].imageSubresource.aspectMask;

      get_blorp_surf_for_anv_image(anv_image, aspect, &image.surf);
      image.offset =
         anv_sanitize_image_offset(anv_image->type, pRegions[r].imageOffset);
      image.level = pRegions[r].imageSubresource.mipLevel;

      VkExtent3D extent =
         anv_sanitize_image_extent(anv_image->type, pRegions[r].imageExtent);
      if (anv_image->type != VK_IMAGE_TYPE_3D) {
         image.offset.z = pRegions[r].imageSubresource.baseArrayLayer;
         extent.depth = pRegions[r].imageSubresource.layerCount;
      }

      const enum isl_format buffer_format =
         anv_get_isl_format(&cmd_buffer->device->info, anv_image->vk_format,
                            aspect, VK_IMAGE_TILING_LINEAR);

      const VkExtent3D bufferImageExtent = {
         .width  = pRegions[r].bufferRowLength ?
                   pRegions[r].bufferRowLength : extent.width,
         .height = pRegions[r].bufferImageHeight ?
                   pRegions[r].bufferImageHeight : extent.height,
      };

      const struct isl_format_layout *buffer_fmtl =
         isl_format_get_layout(buffer_format);

      const uint32_t buffer_row_pitch =
         DIV_ROUND_UP(bufferImageExtent.width, buffer_fmtl->bw) *
         (buffer_fmtl->bpb / 8);

      const uint32_t buffer_layer_stride =
         DIV_ROUND_UP(bufferImageExtent.height, buffer_fmtl->bh) *
         buffer_row_pitch;

      struct isl_surf buffer_isl_surf;
      get_blorp_surf_for_anv_buffer(cmd_buffer->device,
                                    anv_buffer, pRegions[r].bufferOffset,
                                    extent.width, extent.height,
                                    buffer_row_pitch, buffer_format,
                                    &buffer.surf, &buffer_isl_surf);

      for (unsigned z = 0; z < extent.depth; z++) {
         blorp_copy(&batch, &src->surf, src->level, src->offset.z,
                    &dst->surf, dst->level, dst->offset.z,
                    src->offset.x, src->offset.y, dst->offset.x, dst->offset.y,
                    extent.width, extent.height);

         image.offset.z++;
         buffer.surf.addr.offset += buffer_layer_stride;
      }
   }

   blorp_batch_finish(&batch);
}

void anv_CmdCopyBufferToImage(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, src_buffer, srcBuffer);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   copy_buffer_to_image(cmd_buffer, src_buffer, dst_image,
                        regionCount, pRegions, true);
}

void anv_CmdCopyImageToBuffer(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkBuffer                                    dstBuffer,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);

   copy_buffer_to_image(cmd_buffer, dst_buffer, src_image,
                        regionCount, pRegions, false);
}

static bool
flip_coords(unsigned *src0, unsigned *src1, unsigned *dst0, unsigned *dst1)
{
   bool flip = false;
   if (*src0 > *src1) {
      unsigned tmp = *src0;
      *src0 = *src1;
      *src1 = tmp;
      flip = !flip;
   }

   if (*dst0 > *dst1) {
      unsigned tmp = *dst0;
      *dst0 = *dst1;
      *dst1 = tmp;
      flip = !flip;
   }

   return flip;
}

void anv_CmdBlitImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageBlit*                          pRegions,
    VkFilter                                    filter)

{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   struct blorp_surf src, dst;

   uint32_t gl_filter;
   switch (filter) {
   case VK_FILTER_NEAREST:
      gl_filter = 0x2600; /* GL_NEAREST */
      break;
   case VK_FILTER_LINEAR:
      gl_filter = 0x2601; /* GL_LINEAR */
      break;
   default:
      unreachable("Invalid filter");
   }

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer);

   for (unsigned r = 0; r < regionCount; r++) {
      const VkImageSubresourceLayers *src_res = &pRegions[r].srcSubresource;
      const VkImageSubresourceLayers *dst_res = &pRegions[r].dstSubresource;

      get_blorp_surf_for_anv_image(src_image, src_res->aspectMask, &src);
      get_blorp_surf_for_anv_image(dst_image, dst_res->aspectMask, &dst);

      struct anv_format src_format =
         anv_get_format(&cmd_buffer->device->info, src_image->vk_format,
                        src_res->aspectMask, src_image->tiling);
      struct anv_format dst_format =
         anv_get_format(&cmd_buffer->device->info, dst_image->vk_format,
                        dst_res->aspectMask, dst_image->tiling);

      unsigned dst_start, dst_end;
      if (dst_image->type == VK_IMAGE_TYPE_3D) {
         assert(dst_res->baseArrayLayer == 0);
         dst_start = pRegions[r].dstOffsets[0].z;
         dst_end = pRegions[r].dstOffsets[1].z;
      } else {
         dst_start = dst_res->baseArrayLayer;
         dst_end = dst_start + dst_res->layerCount;
      }

      unsigned src_start, src_end;
      if (src_image->type == VK_IMAGE_TYPE_3D) {
         assert(src_res->baseArrayLayer == 0);
         src_start = pRegions[r].srcOffsets[0].z;
         src_end = pRegions[r].srcOffsets[1].z;
      } else {
         src_start = src_res->baseArrayLayer;
         src_end = src_start + src_res->layerCount;
      }

      bool flip_z = flip_coords(&src_start, &src_end, &dst_start, &dst_end);
      float src_z_step = (float)(src_end + 1 - src_start) /
                         (float)(dst_end + 1 - dst_start);

      if (flip_z) {
         src_start = src_end;
         src_z_step *= -1;
      }

      unsigned src_x0 = pRegions[r].srcOffsets[0].x;
      unsigned src_x1 = pRegions[r].srcOffsets[1].x;
      unsigned dst_x0 = pRegions[r].dstOffsets[0].x;
      unsigned dst_x1 = pRegions[r].dstOffsets[1].x;
      bool flip_x = flip_coords(&src_x0, &src_x1, &dst_x0, &dst_x1);

      unsigned src_y0 = pRegions[r].srcOffsets[0].y;
      unsigned src_y1 = pRegions[r].srcOffsets[1].y;
      unsigned dst_y0 = pRegions[r].dstOffsets[0].y;
      unsigned dst_y1 = pRegions[r].dstOffsets[1].y;
      bool flip_y = flip_coords(&src_y0, &src_y1, &dst_y0, &dst_y1);

      const unsigned num_layers = dst_end - dst_start;
      for (unsigned i = 0; i < num_layers; i++) {
         unsigned dst_z = dst_start + i;
         unsigned src_z = src_start + i * src_z_step;

         blorp_blit(&batch, &src, src_res->mipLevel, src_z,
                    src_format.isl_format, src_format.swizzle,
                    &dst, dst_res->mipLevel, dst_z,
                    dst_format.isl_format, dst_format.swizzle,
                    src_x0, src_y0, src_x1, src_y1,
                    dst_x0, dst_y0, dst_x1, dst_y1,
                    gl_filter, flip_x, flip_y);
      }

   }

   blorp_batch_finish(&batch);
}

static void
do_buffer_copy(struct blorp_batch *batch,
               struct anv_bo *src, uint64_t src_offset,
               struct anv_bo *dst, uint64_t dst_offset,
               int width, int height, int block_size)
{
   struct anv_device *device = batch->blorp->driver_ctx;

   /* The actual format we pick doesn't matter as blorp will throw it away.
    * The only thing that actually matters is the size.
    */
   enum isl_format format;
   switch (block_size) {
   case 1:  format = ISL_FORMAT_R8_UINT;              break;
   case 2:  format = ISL_FORMAT_R8G8_UINT;            break;
   case 4:  format = ISL_FORMAT_R8G8B8A8_UNORM;       break;
   case 8:  format = ISL_FORMAT_R16G16B16A16_UNORM;   break;
   case 16: format = ISL_FORMAT_R32G32B32A32_UINT;    break;
   default:
      unreachable("Not a power-of-two format size");
   }

   struct isl_surf surf;
   isl_surf_init(&device->isl_dev, &surf,
                 .dim = ISL_SURF_DIM_2D,
                 .format = format,
                 .width = width,
                 .height = height,
                 .depth = 1,
                 .levels = 1,
                 .array_len = 1,
                 .samples = 1,
                 .usage = ISL_SURF_USAGE_TEXTURE_BIT |
                          ISL_SURF_USAGE_RENDER_TARGET_BIT,
                 .tiling_flags = ISL_TILING_LINEAR_BIT);
   assert(surf.row_pitch == width * block_size);

   struct blorp_surf src_blorp_surf = {
      .surf = &surf,
      .addr = {
         .buffer = src,
         .offset = src_offset,
      },
   };

   struct blorp_surf dst_blorp_surf = {
      .surf = &surf,
      .addr = {
         .buffer = dst,
         .offset = dst_offset,
      },
   };

   blorp_copy(batch, &src_blorp_surf, 0, 0, &dst_blorp_surf, 0, 0,
              0, 0, 0, 0, width, height);
}

/**
 * Returns the greatest common divisor of a and b that is a power of two.
 */
static inline uint64_t
gcd_pow2_u64(uint64_t a, uint64_t b)
{
   assert(a > 0 || b > 0);

   unsigned a_log2 = ffsll(a) - 1;
   unsigned b_log2 = ffsll(b) - 1;

   /* If either a or b is 0, then a_log2 or b_log2 till be UINT_MAX in which
    * case, the MIN2() will take the other one.  If both are 0 then we will
    * hit the assert above.
    */
   return 1 << MIN2(a_log2, b_log2);
}

/* This is maximum possible width/height our HW can handle */
#define MAX_SURFACE_DIM (1ull << 14)

void anv_CmdCopyBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    dstBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, src_buffer, srcBuffer);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer);

   for (unsigned r = 0; r < regionCount; r++) {
      uint64_t src_offset = src_buffer->offset + pRegions[r].srcOffset;
      uint64_t dst_offset = dst_buffer->offset + pRegions[r].dstOffset;
      uint64_t copy_size = pRegions[r].size;

      /* First, we compute the biggest format that can be used with the
       * given offsets and size.
       */
      int bs = 16;
      bs = gcd_pow2_u64(bs, src_offset);
      bs = gcd_pow2_u64(bs, dst_offset);
      bs = gcd_pow2_u64(bs, pRegions[r].size);

      /* First, we make a bunch of max-sized copies */
      uint64_t max_copy_size = MAX_SURFACE_DIM * MAX_SURFACE_DIM * bs;
      while (copy_size >= max_copy_size) {
         do_buffer_copy(&batch, src_buffer->bo, src_offset,
                        dst_buffer->bo, dst_offset,
                        MAX_SURFACE_DIM, MAX_SURFACE_DIM, bs);
         copy_size -= max_copy_size;
         src_offset += max_copy_size;
         dst_offset += max_copy_size;
      }

      /* Now make a max-width copy */
      uint64_t height = copy_size / (MAX_SURFACE_DIM * bs);
      assert(height < MAX_SURFACE_DIM);
      if (height != 0) {
         uint64_t rect_copy_size = height * MAX_SURFACE_DIM * bs;
         do_buffer_copy(&batch, src_buffer->bo, src_offset,
                        dst_buffer->bo, dst_offset,
                        MAX_SURFACE_DIM, height, bs);
         copy_size -= rect_copy_size;
         src_offset += rect_copy_size;
         dst_offset += rect_copy_size;
      }

      /* Finally, make a small copy to finish it off */
      if (copy_size != 0) {
         do_buffer_copy(&batch, src_buffer->bo, src_offset,
                        dst_buffer->bo, dst_offset,
                        copy_size / bs, 1, bs);
      }
   }

   blorp_batch_finish(&batch);
}

void anv_CmdUpdateBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    VkDeviceSize                                dataSize,
    const uint32_t*                             pData)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer);

   /* We can't quite grab a full block because the state stream needs a
    * little data at the top to build its linked list.
    */
   const uint32_t max_update_size =
      cmd_buffer->device->dynamic_state_block_pool.block_size - 64;

   assert(max_update_size < MAX_SURFACE_DIM * 4);

   while (dataSize) {
      const uint32_t copy_size = MIN2(dataSize, max_update_size);

      struct anv_state tmp_data =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, copy_size, 64);

      memcpy(tmp_data.map, pData, copy_size);

      int bs = 16;
      bs = gcd_pow2_u64(bs, dstOffset);
      bs = gcd_pow2_u64(bs, copy_size);

      do_buffer_copy(&batch,
                     &cmd_buffer->device->dynamic_state_block_pool.bo,
                     tmp_data.offset,
                     dst_buffer->bo, dst_buffer->offset + dstOffset,
                     copy_size / bs, 1, bs);

      dataSize -= copy_size;
      dstOffset += copy_size;
      pData = (void *)pData + copy_size;
   }

   blorp_batch_finish(&batch);
}

void anv_CmdClearColorImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     _image,
    VkImageLayout                               imageLayout,
    const VkClearColorValue*                    pColor,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, image, _image);

   static const bool color_write_disable[4] = { false, false, false, false };

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer);

   union isl_color_value clear_color;
   memcpy(clear_color.u32, pColor->uint32, sizeof(pColor->uint32));

   struct blorp_surf surf;
   get_blorp_surf_for_anv_image(image, VK_IMAGE_ASPECT_COLOR_BIT, &surf);

   for (unsigned r = 0; r < rangeCount; r++) {
      if (pRanges[r].aspectMask == 0)
         continue;

      assert(pRanges[r].aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);

      struct anv_format src_format =
         anv_get_format(&cmd_buffer->device->info, image->vk_format,
                        VK_IMAGE_ASPECT_COLOR_BIT, image->tiling);

      unsigned base_layer = pRanges[r].baseArrayLayer;
      unsigned layer_count = pRanges[r].layerCount;

      for (unsigned i = 0; i < pRanges[r].levelCount; i++) {
         const unsigned level = pRanges[r].baseMipLevel + i;
         const unsigned level_width = anv_minify(image->extent.width, level);
         const unsigned level_height = anv_minify(image->extent.height, level);

         if (image->type == VK_IMAGE_TYPE_3D) {
            base_layer = 0;
            layer_count = anv_minify(image->extent.depth, level);
         }

         blorp_clear(&batch, &surf,
                     src_format.isl_format, src_format.swizzle,
                     level, base_layer, layer_count,
                     0, 0, level_width, level_height,
                     clear_color, color_write_disable);
      }
   }

   blorp_batch_finish(&batch);
}

static void
resolve_image(struct blorp_batch *batch,
              const struct anv_image *src_image,
              uint32_t src_level, uint32_t src_layer,
              const struct anv_image *dst_image,
              uint32_t dst_level, uint32_t dst_layer,
              VkImageAspectFlags aspect_mask,
              uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y,
              uint32_t width, uint32_t height)
{
   assert(src_image->type == VK_IMAGE_TYPE_2D);
   assert(src_image->samples > 1);
   assert(dst_image->type == VK_IMAGE_TYPE_2D);
   assert(dst_image->samples == 1);

   uint32_t a;
   for_each_bit(a, aspect_mask) {
      VkImageAspectFlagBits aspect = 1 << a;

      struct blorp_surf src_surf, dst_surf;
      get_blorp_surf_for_anv_image(src_image, aspect, &src_surf);
      get_blorp_surf_for_anv_image(dst_image, aspect, &dst_surf);

      blorp_blit(batch,
                 &src_surf, src_level, src_layer,
                 ISL_FORMAT_UNSUPPORTED, ISL_SWIZZLE_IDENTITY,
                 &dst_surf, dst_level, dst_layer,
                 ISL_FORMAT_UNSUPPORTED, ISL_SWIZZLE_IDENTITY,
                 src_x, src_y, src_x + width, src_y + height,
                 dst_x, dst_y, dst_x + width, dst_y + height,
                 0x2600 /* GL_NEAREST */, false, false);
   }
}

void anv_CmdResolveImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageResolve*                       pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer);

   for (uint32_t r = 0; r < regionCount; r++) {
      assert(pRegions[r].srcSubresource.aspectMask ==
             pRegions[r].dstSubresource.aspectMask);
      assert(pRegions[r].srcSubresource.layerCount ==
             pRegions[r].dstSubresource.layerCount);

      const uint32_t layer_count = pRegions[r].dstSubresource.layerCount;

      for (uint32_t layer = 0; layer < layer_count; layer++) {
         resolve_image(&batch,
                       src_image, pRegions[r].srcSubresource.mipLevel,
                       pRegions[r].srcSubresource.baseArrayLayer + layer,
                       dst_image, pRegions[r].dstSubresource.mipLevel,
                       pRegions[r].dstSubresource.baseArrayLayer + layer,
                       pRegions[r].dstSubresource.aspectMask,
                       pRegions[r].srcOffset.x, pRegions[r].srcOffset.y,
                       pRegions[r].dstOffset.x, pRegions[r].dstOffset.y,
                       pRegions[r].extent.width, pRegions[r].extent.height);
      }
   }

   blorp_batch_finish(&batch);
}

void
anv_cmd_buffer_resolve_subpass(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   struct anv_subpass *subpass = cmd_buffer->state.subpass;

   /* FINISHME(perf): Skip clears for resolve attachments.
    *
    * From the Vulkan 1.0 spec:
    *
    *    If the first use of an attachment in a render pass is as a resolve
    *    attachment, then the loadOp is effectively ignored as the resolve is
    *    guaranteed to overwrite all pixels in the render area.
    */

   if (!subpass->has_resolve)
      return;

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer);

   for (uint32_t i = 0; i < subpass->color_count; ++i) {
      uint32_t src_att = subpass->color_attachments[i];
      uint32_t dst_att = subpass->resolve_attachments[i];

      if (dst_att == VK_ATTACHMENT_UNUSED)
         continue;

      struct anv_image_view *src_iview = fb->attachments[src_att];
      struct anv_image_view *dst_iview = fb->attachments[dst_att];

      const VkRect2D render_area = cmd_buffer->state.render_area;

      assert(src_iview->aspect_mask == dst_iview->aspect_mask);
      resolve_image(&batch, src_iview->image,
                    src_iview->base_mip, src_iview->base_layer,
                    dst_iview->image,
                    dst_iview->base_mip, dst_iview->base_layer,
                    src_iview->aspect_mask,
                    render_area.offset.x, render_area.offset.y,
                    render_area.offset.x, render_area.offset.y,
                    render_area.extent.width, render_area.extent.height);
   }

   blorp_batch_finish(&batch);
}
