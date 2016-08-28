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

static void
do_buffer_copy(struct anv_cmd_buffer *cmd_buffer,
               struct anv_bo *src, uint64_t src_offset,
               struct anv_bo *dest, uint64_t dest_offset,
               int width, int height, int bs)
{
   struct anv_meta_blit2d_surf b_src = {
      .bo = src,
      .tiling = ISL_TILING_LINEAR,
      .base_offset = src_offset,
      .bs = bs,
      .pitch = width * bs,
   };
   struct anv_meta_blit2d_surf b_dst = {
      .bo = dest,
      .tiling = ISL_TILING_LINEAR,
      .base_offset = dest_offset,
      .bs = bs,
      .pitch = width * bs,
   };
   struct anv_meta_blit2d_rect rect = {
      .width = width,
      .height = height,
   };
   anv_meta_blit2d(cmd_buffer, &b_src, &b_dst, 1, &rect);
}

void anv_CmdCopyBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, src_buffer, srcBuffer);
   ANV_FROM_HANDLE(anv_buffer, dest_buffer, destBuffer);

   struct anv_meta_saved_state saved_state;

   anv_meta_begin_blit2d(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      uint64_t src_offset = src_buffer->offset + pRegions[r].srcOffset;
      uint64_t dest_offset = dest_buffer->offset + pRegions[r].dstOffset;
      uint64_t copy_size = pRegions[r].size;

      /* First, we compute the biggest format that can be used with the
       * given offsets and size.
       */
      int bs = 16;

      int fs = ffs(src_offset) - 1;
      if (fs != -1)
         bs = MIN2(bs, 1 << fs);
      assert(src_offset % bs == 0);

      fs = ffs(dest_offset) - 1;
      if (fs != -1)
         bs = MIN2(bs, 1 << fs);
      assert(dest_offset % bs == 0);

      fs = ffs(pRegions[r].size) - 1;
      if (fs != -1)
         bs = MIN2(bs, 1 << fs);
      assert(pRegions[r].size % bs == 0);

      /* This is maximum possible width/height our HW can handle */
      uint64_t max_surface_dim = 1 << 14;

      /* First, we make a bunch of max-sized copies */
      uint64_t max_copy_size = max_surface_dim * max_surface_dim * bs;
      while (copy_size >= max_copy_size) {
         do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
                        dest_buffer->bo, dest_offset,
                        max_surface_dim, max_surface_dim, bs);
         copy_size -= max_copy_size;
         src_offset += max_copy_size;
         dest_offset += max_copy_size;
      }

      uint64_t height = copy_size / (max_surface_dim * bs);
      assert(height < max_surface_dim);
      if (height != 0) {
         uint64_t rect_copy_size = height * max_surface_dim * bs;
         do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
                        dest_buffer->bo, dest_offset,
                        max_surface_dim, height, bs);
         copy_size -= rect_copy_size;
         src_offset += rect_copy_size;
         dest_offset += rect_copy_size;
      }

      if (copy_size != 0) {
         do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
                        dest_buffer->bo, dest_offset,
                        copy_size / bs, 1, bs);
      }
   }

   anv_meta_end_blit2d(cmd_buffer, &saved_state);
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
   struct anv_meta_saved_state saved_state;

   anv_meta_begin_blit2d(cmd_buffer, &saved_state);

   /* We can't quite grab a full block because the state stream needs a
    * little data at the top to build its linked list.
    */
   const uint32_t max_update_size =
      cmd_buffer->device->dynamic_state_block_pool.block_size - 64;

   assert(max_update_size < (1 << 14) * 4);

   while (dataSize) {
      const uint32_t copy_size = MIN2(dataSize, max_update_size);

      struct anv_state tmp_data =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, copy_size, 64);

      memcpy(tmp_data.map, pData, copy_size);

      int bs;
      if ((copy_size & 15) == 0 && (dstOffset & 15) == 0) {
         bs = 16;
      } else if ((copy_size & 7) == 0 && (dstOffset & 7) == 0) {
         bs = 8;
      } else {
         assert((copy_size & 3) == 0 && (dstOffset & 3) == 0);
         bs = 4;
      }

      do_buffer_copy(cmd_buffer,
                     &cmd_buffer->device->dynamic_state_block_pool.bo,
                     tmp_data.offset,
                     dst_buffer->bo, dst_buffer->offset + dstOffset,
                     copy_size / bs, 1, bs);

      dataSize -= copy_size;
      dstOffset += copy_size;
      pData = (void *)pData + copy_size;
   }

   anv_meta_end_blit2d(cmd_buffer, &saved_state);
}
