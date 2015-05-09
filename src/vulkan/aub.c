/*
 * Copyright © 2015 Intel Corporation
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

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/mman.h>

#include <drm.h>
#include <i915_drm.h>

#include "private.h"
#include "aub.h"

struct anv_aub_writer {
   FILE *file;
   uint32_t offset;
   int gen;
};

static void
aub_out(struct anv_aub_writer *writer, uint32_t data)
{
   fwrite(&data, 1, 4, writer->file);
}

static void
aub_out_data(struct anv_aub_writer *writer, const void *data, size_t size)
{
   fwrite(data, 1, size, writer->file);
}

static struct anv_aub_writer *
get_anv_aub_writer(struct anv_device *device)
{
   struct anv_aub_writer *writer = device->aub_writer;
   int entry = 0x200003;
   int i;
   int gtt_size = 0x10000;
   const char *filename;

   if (geteuid() != getuid())
      return NULL;

   if (writer)
      return writer;

   writer = malloc(sizeof(*writer));
   if (writer == NULL)
      return NULL;

   filename = "intel.aub";
   writer->gen = device->info.gen;
   writer->file = fopen(filename, "w+");
   if (!writer->file) {
      free(writer);
      return NULL;
   }

   /* Start allocating objects from just after the GTT. */
   writer->offset = gtt_size;

   /* Start with a (required) version packet. */
   aub_out(writer, CMD_AUB_HEADER | (13 - 2));
   aub_out(writer,
           (4 << AUB_HEADER_MAJOR_SHIFT) |
           (0 << AUB_HEADER_MINOR_SHIFT));
   for (i = 0; i < 8; i++) {
      aub_out(writer, 0); /* app name */
   }
   aub_out(writer, 0); /* timestamp */
   aub_out(writer, 0); /* timestamp */
   aub_out(writer, 0); /* comment len */

   /* Set up the GTT. The max we can handle is 256M */
   aub_out(writer, CMD_AUB_TRACE_HEADER_BLOCK | ((writer->gen >= 8 ? 6 : 5) - 2));
   aub_out(writer,
           AUB_TRACE_MEMTYPE_GTT_ENTRY |
           AUB_TRACE_TYPE_NOTYPE | AUB_TRACE_OP_DATA_WRITE);
   aub_out(writer, 0); /* subtype */
   aub_out(writer, 0); /* offset */
   aub_out(writer, gtt_size); /* size */
   if (writer->gen >= 8)
      aub_out(writer, 0);
   for (i = 0x000; i < gtt_size; i += 4, entry += 0x1000) {
      aub_out(writer, entry);
   }

   return device->aub_writer = writer;
}

void
anv_aub_writer_destroy(struct anv_aub_writer *writer)
{
   fclose(writer->file);
   free(writer);
}


/**
 * Break up large objects into multiple writes.  Otherwise a 128kb VBO
 * would overflow the 16 bits of size field in the packet header and
 * everything goes badly after that.
 */
static void
aub_write_trace_block(struct anv_aub_writer *writer, uint32_t type,
                      void *virtual, uint32_t size, uint32_t gtt_offset)
{
   uint32_t block_size;
   uint32_t offset;
   uint32_t subtype = 0;
   static const char null_block[8 * 4096];

   for (offset = 0; offset < size; offset += block_size) {
      block_size = size - offset;

      if (block_size > 8 * 4096)
         block_size = 8 * 4096;

      aub_out(writer,
              CMD_AUB_TRACE_HEADER_BLOCK |
              ((writer->gen >= 8 ? 6 : 5) - 2));
      aub_out(writer,
              AUB_TRACE_MEMTYPE_GTT |
              type | AUB_TRACE_OP_DATA_WRITE);
      aub_out(writer, subtype);
      aub_out(writer, gtt_offset + offset);
      aub_out(writer, ALIGN_U32(block_size, 4));
      if (writer->gen >= 8)
         aub_out(writer, 0);

      if (virtual)
         aub_out_data(writer, (char *) virtual + offset, block_size);
      else
         aub_out_data(writer, null_block, block_size);

      /* Pad to a multiple of 4 bytes. */
      aub_out_data(writer, null_block, -block_size & 3);
   }
}

/*
 * Make a ringbuffer on fly and dump it
 */
static void
aub_build_dump_ringbuffer(struct anv_aub_writer *writer,
                          uint32_t batch_offset, uint32_t offset,
                          int ring_flag)
{
   uint32_t ringbuffer[4096];
   int ring = AUB_TRACE_TYPE_RING_PRB0; /* The default ring */
   int ring_count = 0;

   if (ring_flag == I915_EXEC_BSD)
      ring = AUB_TRACE_TYPE_RING_PRB1;
   else if (ring_flag == I915_EXEC_BLT)
      ring = AUB_TRACE_TYPE_RING_PRB2;

   /* Make a ring buffer to execute our batchbuffer. */
   memset(ringbuffer, 0, sizeof(ringbuffer));
   if (writer->gen >= 8) {
      ringbuffer[ring_count++] = AUB_MI_BATCH_BUFFER_START | (3 - 2);
      ringbuffer[ring_count++] = batch_offset;
      ringbuffer[ring_count++] = 0;
   } else {
      ringbuffer[ring_count++] = AUB_MI_BATCH_BUFFER_START;
      ringbuffer[ring_count++] = batch_offset;
   }

   /* Write out the ring.  This appears to trigger execution of
    * the ring in the simulator.
    */
   aub_out(writer,
           CMD_AUB_TRACE_HEADER_BLOCK |
           ((writer->gen >= 8 ? 6 : 5) - 2));
   aub_out(writer,
           AUB_TRACE_MEMTYPE_GTT | ring | AUB_TRACE_OP_COMMAND_WRITE);
   aub_out(writer, 0); /* general/surface subtype */
   aub_out(writer, offset);
   aub_out(writer, ring_count * 4);
   if (writer->gen >= 8)
      aub_out(writer, 0);

   /* FIXME: Need some flush operations here? */
   aub_out_data(writer, ringbuffer, ring_count * 4);
}

struct aub_bo {
   uint32_t offset;
   void *map;
   void *relocated;
};

static void
relocate_bo(struct anv_bo *bo, struct anv_reloc_list *list, struct aub_bo *bos)
{
   struct aub_bo *aub_bo = &bos[bo->index];
   struct drm_i915_gem_relocation_entry *reloc;
   uint32_t *dw;

   aub_bo->relocated = malloc(bo->size);
   memcpy(aub_bo->relocated, aub_bo->map, bo->size);
   for (size_t i = 0; i < list->num_relocs; i++) {
      reloc = &list->relocs[i];
      assert(reloc->offset < bo->size);
      dw = aub_bo->relocated + reloc->offset;
      *dw = bos[reloc->target_handle].offset + reloc->delta;
   }
}

void
anv_cmd_buffer_dump(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_batch *batch = &cmd_buffer->batch;
   struct anv_aub_writer *writer;
   struct anv_bo *bo;
   uint32_t ring_flag = 0;
   uint32_t offset, length;
   struct aub_bo *aub_bos;

   writer = get_anv_aub_writer(device);
   if (writer == NULL)
      return;

   aub_bos = malloc(cmd_buffer->bo_count * sizeof(aub_bos[0]));
   offset = writer->offset;
   for (uint32_t i = 0; i < cmd_buffer->bo_count; i++) {
      bo = cmd_buffer->exec2_bos[i];
      if (bo->map)
         aub_bos[i].map = bo->map;
      else
         aub_bos[i].map = anv_gem_mmap(device, bo->gem_handle, 0, bo->size);
      aub_bos[i].relocated = aub_bos[i].map;
      aub_bos[i].offset = offset;
      offset = ALIGN_U32(offset + bo->size + 4095, 4096);
   }

   relocate_bo(&batch->bo, &batch->cmd_relocs, aub_bos);
   relocate_bo(&device->surface_state_block_pool.bo,
               &batch->surf_relocs, aub_bos);

   for (uint32_t i = 0; i < cmd_buffer->bo_count; i++) {
      bo = cmd_buffer->exec2_bos[i];
      if (i == cmd_buffer->bo_count - 1) {
         length = batch->next - batch->bo.map;
         aub_write_trace_block(writer, AUB_TRACE_TYPE_BATCH,
                               aub_bos[i].relocated,
                               length, aub_bos[i].offset);
      } else {
         aub_write_trace_block(writer, AUB_TRACE_TYPE_NOTYPE,
                               aub_bos[i].relocated,
                               bo->size, aub_bos[i].offset);
      }
      if (aub_bos[i].relocated != aub_bos[i].map)
         free(aub_bos[i].relocated);
      if (aub_bos[i].map != bo->map)
         anv_gem_munmap(aub_bos[i].map, bo->size);
   }

   /* Dump ring buffer */
   aub_build_dump_ringbuffer(writer, aub_bos[batch->bo.index].offset,
                             offset, ring_flag);

   free(aub_bos);

   fflush(writer->file);
}
