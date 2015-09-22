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

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "anv_private.h"

/** \file anv_batch_chain.c
 *
 * This file contains functions related to anv_cmd_buffer as a data
 * structure.  This involves everything required to create and destroy
 * the actual batch buffers as well as link them together and handle
 * relocations and surface state.  It specifically does *not* contain any
 * handling of actual vkCmd calls beyond vkCmdExecuteCommands.
 */

/*-----------------------------------------------------------------------*
 * Functions related to anv_reloc_list
 *-----------------------------------------------------------------------*/

static VkResult
anv_reloc_list_init_clone(struct anv_reloc_list *list,
                          struct anv_device *device,
                          const struct anv_reloc_list *other_list)
{
   if (other_list) {
      list->num_relocs = other_list->num_relocs;
      list->array_length = other_list->array_length;
   } else {
      list->num_relocs = 0;
      list->array_length = 256;
   }

   list->relocs =
      anv_device_alloc(device, list->array_length * sizeof(*list->relocs), 8,
                       VK_SYSTEM_ALLOC_TYPE_INTERNAL);

   if (list->relocs == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   list->reloc_bos =
      anv_device_alloc(device, list->array_length * sizeof(*list->reloc_bos), 8,
                       VK_SYSTEM_ALLOC_TYPE_INTERNAL);

   if (list->reloc_bos == NULL) {
      anv_device_free(device, list->relocs);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   if (other_list) {
      memcpy(list->relocs, other_list->relocs,
             list->array_length * sizeof(*list->relocs));
      memcpy(list->reloc_bos, other_list->reloc_bos,
             list->array_length * sizeof(*list->reloc_bos));
   }

   return VK_SUCCESS;
}

VkResult
anv_reloc_list_init(struct anv_reloc_list *list, struct anv_device *device)
{
   return anv_reloc_list_init_clone(list, device, NULL);
}

void
anv_reloc_list_finish(struct anv_reloc_list *list, struct anv_device *device)
{
   anv_device_free(device, list->relocs);
   anv_device_free(device, list->reloc_bos);
}

static VkResult
anv_reloc_list_grow(struct anv_reloc_list *list, struct anv_device *device,
                    size_t num_additional_relocs)
{
   if (list->num_relocs + num_additional_relocs <= list->array_length)
      return VK_SUCCESS;

   size_t new_length = list->array_length * 2;
   while (new_length < list->num_relocs + num_additional_relocs)
      new_length *= 2;

   struct drm_i915_gem_relocation_entry *new_relocs =
      anv_device_alloc(device, new_length * sizeof(*list->relocs), 8,
                       VK_SYSTEM_ALLOC_TYPE_INTERNAL);
   if (new_relocs == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   struct anv_bo **new_reloc_bos =
      anv_device_alloc(device, new_length * sizeof(*list->reloc_bos), 8,
                       VK_SYSTEM_ALLOC_TYPE_INTERNAL);
   if (new_relocs == NULL) {
      anv_device_free(device, new_relocs);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   memcpy(new_relocs, list->relocs, list->num_relocs * sizeof(*list->relocs));
   memcpy(new_reloc_bos, list->reloc_bos,
          list->num_relocs * sizeof(*list->reloc_bos));

   anv_device_free(device, list->relocs);
   anv_device_free(device, list->reloc_bos);

   list->array_length = new_length;
   list->relocs = new_relocs;
   list->reloc_bos = new_reloc_bos;

   return VK_SUCCESS;
}

uint64_t
anv_reloc_list_add(struct anv_reloc_list *list, struct anv_device *device,
                   uint32_t offset, struct anv_bo *target_bo, uint32_t delta)
{
   struct drm_i915_gem_relocation_entry *entry;
   int index;

   anv_reloc_list_grow(list, device, 1);
   /* TODO: Handle failure */

   /* XXX: Can we use I915_EXEC_HANDLE_LUT? */
   index = list->num_relocs++;
   list->reloc_bos[index] = target_bo;
   entry = &list->relocs[index];
   entry->target_handle = target_bo->gem_handle;
   entry->delta = delta;
   entry->offset = offset;
   entry->presumed_offset = target_bo->offset;
   entry->read_domains = 0;
   entry->write_domain = 0;

   return target_bo->offset + delta;
}

static void
anv_reloc_list_append(struct anv_reloc_list *list, struct anv_device *device,
                      struct anv_reloc_list *other, uint32_t offset)
{
   anv_reloc_list_grow(list, device, other->num_relocs);
   /* TODO: Handle failure */

   memcpy(&list->relocs[list->num_relocs], &other->relocs[0],
          other->num_relocs * sizeof(other->relocs[0]));
   memcpy(&list->reloc_bos[list->num_relocs], &other->reloc_bos[0],
          other->num_relocs * sizeof(other->reloc_bos[0]));

   for (uint32_t i = 0; i < other->num_relocs; i++)
      list->relocs[i + list->num_relocs].offset += offset;

   list->num_relocs += other->num_relocs;
}

/*-----------------------------------------------------------------------*
 * Functions related to anv_batch
 *-----------------------------------------------------------------------*/

void *
anv_batch_emit_dwords(struct anv_batch *batch, int num_dwords)
{
   if (batch->next + num_dwords * 4 > batch->end)
      batch->extend_cb(batch, batch->user_data);

   void *p = batch->next;

   batch->next += num_dwords * 4;
   assert(batch->next <= batch->end);

   return p;
}

uint64_t
anv_batch_emit_reloc(struct anv_batch *batch,
                     void *location, struct anv_bo *bo, uint32_t delta)
{
   return anv_reloc_list_add(batch->relocs, batch->device,
                             location - batch->start, bo, delta);
}

void
anv_batch_emit_batch(struct anv_batch *batch, struct anv_batch *other)
{
   uint32_t size, offset;

   size = other->next - other->start;
   assert(size % 4 == 0);

   if (batch->next + size > batch->end)
      batch->extend_cb(batch, batch->user_data);

   assert(batch->next + size <= batch->end);

   VG(VALGRIND_CHECK_MEM_IS_DEFINED(other->start, size));
   memcpy(batch->next, other->start, size);

   offset = batch->next - batch->start;
   anv_reloc_list_append(batch->relocs, batch->device,
                         other->relocs, offset);

   batch->next += size;
}

/*-----------------------------------------------------------------------*
 * Functions related to anv_batch_bo
 *-----------------------------------------------------------------------*/

static VkResult
anv_batch_bo_create(struct anv_device *device, struct anv_batch_bo **bbo_out)
{
   VkResult result;

   struct anv_batch_bo *bbo =
      anv_device_alloc(device, sizeof(*bbo), 8, VK_SYSTEM_ALLOC_TYPE_INTERNAL);
   if (bbo == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   result = anv_bo_pool_alloc(&device->batch_bo_pool, &bbo->bo);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   result = anv_reloc_list_init(&bbo->relocs, device);
   if (result != VK_SUCCESS)
      goto fail_bo_alloc;

   *bbo_out = bbo;

   return VK_SUCCESS;

 fail_bo_alloc:
   anv_bo_pool_free(&device->batch_bo_pool, &bbo->bo);
 fail_alloc:
   anv_device_free(device, bbo);

   return result;
}

static VkResult
anv_batch_bo_clone(struct anv_device *device,
                   const struct anv_batch_bo *other_bbo,
                   struct anv_batch_bo **bbo_out)
{
   VkResult result;

   struct anv_batch_bo *bbo =
      anv_device_alloc(device, sizeof(*bbo), 8, VK_SYSTEM_ALLOC_TYPE_INTERNAL);
   if (bbo == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   result = anv_bo_pool_alloc(&device->batch_bo_pool, &bbo->bo);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   result = anv_reloc_list_init_clone(&bbo->relocs, device, &other_bbo->relocs);
   if (result != VK_SUCCESS)
      goto fail_bo_alloc;

   bbo->length = other_bbo->length;
   memcpy(bbo->bo.map, other_bbo->bo.map, other_bbo->length);

   *bbo_out = bbo;

   return VK_SUCCESS;

 fail_bo_alloc:
   anv_bo_pool_free(&device->batch_bo_pool, &bbo->bo);
 fail_alloc:
   anv_device_free(device, bbo);

   return result;
}

static void
anv_batch_bo_start(struct anv_batch_bo *bbo, struct anv_batch *batch,
                   size_t batch_padding)
{
   batch->next = batch->start = bbo->bo.map;
   batch->end = bbo->bo.map + bbo->bo.size - batch_padding;
   batch->relocs = &bbo->relocs;
   bbo->relocs.num_relocs = 0;
}

static void
anv_batch_bo_continue(struct anv_batch_bo *bbo, struct anv_batch *batch,
                      size_t batch_padding)
{
   batch->start = bbo->bo.map;
   batch->next = bbo->bo.map + bbo->length;
   batch->end = bbo->bo.map + bbo->bo.size - batch_padding;
   batch->relocs = &bbo->relocs;
}

static void
anv_batch_bo_finish(struct anv_batch_bo *bbo, struct anv_batch *batch)
{
   assert(batch->start == bbo->bo.map);
   bbo->length = batch->next - batch->start;
   VG(VALGRIND_CHECK_MEM_IS_DEFINED(batch->start, bbo->length));
}

static void
anv_batch_bo_destroy(struct anv_batch_bo *bbo, struct anv_device *device)
{
   anv_reloc_list_finish(&bbo->relocs, device);
   anv_bo_pool_free(&device->batch_bo_pool, &bbo->bo);
   anv_device_free(device, bbo);
}

static VkResult
anv_batch_bo_list_clone(const struct list_head *list, struct anv_device *device,
                        struct list_head *new_list)
{
   VkResult result = VK_SUCCESS;

   list_inithead(new_list);

   struct anv_batch_bo *prev_bbo = NULL;
   list_for_each_entry(struct anv_batch_bo, bbo, list, link) {
      struct anv_batch_bo *new_bbo;
      result = anv_batch_bo_clone(device, bbo, &new_bbo);
      if (result != VK_SUCCESS)
         break;
      list_addtail(&new_bbo->link, new_list);

      if (prev_bbo) {
         /* As we clone this list of batch_bo's, they chain one to the
          * other using MI_BATCH_BUFFER_START commands.  We need to fix up
          * those relocations as we go.  Fortunately, this is pretty easy
          * as it will always be the last relocation in the list.
          */
         uint32_t last_idx = prev_bbo->relocs.num_relocs - 1;
         assert(prev_bbo->relocs.reloc_bos[last_idx] == &bbo->bo);
         prev_bbo->relocs.reloc_bos[last_idx] = &new_bbo->bo;
      }

      prev_bbo = new_bbo;
   }

   if (result != VK_SUCCESS) {
      list_for_each_entry_safe(struct anv_batch_bo, bbo, new_list, link)
         anv_batch_bo_destroy(bbo, device);
   }

   return result;
}

/*-----------------------------------------------------------------------*
 * Functions related to anv_batch_bo
 *-----------------------------------------------------------------------*/

static inline struct anv_batch_bo *
anv_cmd_buffer_current_batch_bo(struct anv_cmd_buffer *cmd_buffer)
{
   return LIST_ENTRY(struct anv_batch_bo, cmd_buffer->batch_bos.prev, link);
}

static inline struct anv_batch_bo *
anv_cmd_buffer_current_surface_bbo(struct anv_cmd_buffer *cmd_buffer)
{
   return LIST_ENTRY(struct anv_batch_bo, cmd_buffer->surface_bos.prev, link);
}

struct anv_reloc_list *
anv_cmd_buffer_current_surface_relocs(struct anv_cmd_buffer *cmd_buffer)
{
   return &anv_cmd_buffer_current_surface_bbo(cmd_buffer)->relocs;
}

struct anv_address
anv_cmd_buffer_surface_base_address(struct anv_cmd_buffer *cmd_buffer)
{
   return (struct anv_address) {
      .bo = &anv_cmd_buffer_current_surface_bbo(cmd_buffer)->bo,
      .offset = 0,
   };
}

static void
emit_batch_buffer_start(struct anv_batch *batch, struct anv_bo *bo, uint32_t offset)
{
   /* In gen8+ the address field grew to two dwords to accomodate 48 bit
    * offsets. The high 16 bits are in the last dword, so we can use the gen8
    * version in either case, as long as we set the instruction length in the
    * header accordingly.  This means that we always emit three dwords here
    * and all the padding and adjustment we do in this file works for all
    * gens.
    */

   const uint32_t gen7_length =
      GEN7_MI_BATCH_BUFFER_START_length - GEN7_MI_BATCH_BUFFER_START_length_bias;
   const uint32_t gen8_length =
      GEN8_MI_BATCH_BUFFER_START_length - GEN8_MI_BATCH_BUFFER_START_length_bias;

   anv_batch_emit(batch, GEN8_MI_BATCH_BUFFER_START,
      .DwordLength = batch->device->info.gen < 8 ? gen7_length : gen8_length,
      ._2ndLevelBatchBuffer = _1stlevelbatch,
      .AddressSpaceIndicator = ASI_PPGTT,
      .BatchBufferStartAddress = { bo, offset });
}

static void
cmd_buffer_chain_to_batch_bo(struct anv_cmd_buffer *cmd_buffer,
                             struct anv_batch_bo *bbo)
{
   struct anv_batch *batch = &cmd_buffer->batch;
   struct anv_batch_bo *current_bbo =
      anv_cmd_buffer_current_batch_bo(cmd_buffer);

   /* We set the end of the batch a little short so we would be sure we
    * have room for the chaining command.  Since we're about to emit the
    * chaining command, let's set it back where it should go.
    */
   batch->end += GEN8_MI_BATCH_BUFFER_START_length * 4;
   assert(batch->end == current_bbo->bo.map + current_bbo->bo.size);

   emit_batch_buffer_start(batch, &bbo->bo, 0);

   anv_batch_bo_finish(current_bbo, batch);
}

static VkResult
anv_cmd_buffer_chain_batch(struct anv_batch *batch, void *_data)
{
   struct anv_cmd_buffer *cmd_buffer = _data;
   struct anv_batch_bo *new_bbo;

   VkResult result = anv_batch_bo_create(cmd_buffer->device, &new_bbo);
   if (result != VK_SUCCESS)
      return result;

   struct anv_batch_bo **seen_bbo = anv_vector_add(&cmd_buffer->seen_bbos);
   if (seen_bbo == NULL) {
      anv_batch_bo_destroy(new_bbo, cmd_buffer->device);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   *seen_bbo = new_bbo;

   cmd_buffer_chain_to_batch_bo(cmd_buffer, new_bbo);

   list_addtail(&new_bbo->link, &cmd_buffer->batch_bos);

   anv_batch_bo_start(new_bbo, batch, GEN8_MI_BATCH_BUFFER_START_length * 4);

   return VK_SUCCESS;
}

struct anv_state
anv_cmd_buffer_alloc_surface_state(struct anv_cmd_buffer *cmd_buffer,
                                   uint32_t size, uint32_t alignment)
{
   struct anv_bo *surface_bo =
      &anv_cmd_buffer_current_surface_bbo(cmd_buffer)->bo;
   struct anv_state state;

   state.offset = align_u32(cmd_buffer->surface_next, alignment);
   if (state.offset + size > surface_bo->size)
      return (struct anv_state) { 0 };

   state.map = surface_bo->map + state.offset;
   state.alloc_size = size;
   cmd_buffer->surface_next = state.offset + size;

   assert(state.offset + size <= surface_bo->size);

   return state;
}

struct anv_state
anv_cmd_buffer_alloc_binding_table(struct anv_cmd_buffer *cmd_buffer,
                                   uint32_t entries)
{
   return anv_cmd_buffer_alloc_surface_state(cmd_buffer, entries * 4, 32);
}

struct anv_state
anv_cmd_buffer_alloc_dynamic_state(struct anv_cmd_buffer *cmd_buffer,
                                   uint32_t size, uint32_t alignment)
{
   return anv_state_stream_alloc(&cmd_buffer->dynamic_state_stream,
                                 size, alignment);
}

VkResult
anv_cmd_buffer_new_surface_state_bo(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_batch_bo *new_bbo, *old_bbo =
      anv_cmd_buffer_current_surface_bbo(cmd_buffer);

   /* Finish off the old buffer */
   old_bbo->length = cmd_buffer->surface_next;

   VkResult result = anv_batch_bo_create(cmd_buffer->device, &new_bbo);
   if (result != VK_SUCCESS)
      return result;

   struct anv_batch_bo **seen_bbo = anv_vector_add(&cmd_buffer->seen_bbos);
   if (seen_bbo == NULL) {
      anv_batch_bo_destroy(new_bbo, cmd_buffer->device);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   *seen_bbo = new_bbo;

   cmd_buffer->surface_next = 1;

   list_addtail(&new_bbo->link, &cmd_buffer->surface_bos);

   return VK_SUCCESS;
}

VkResult
anv_cmd_buffer_init_batch_bo_chain(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_batch_bo *batch_bo, *surface_bbo;
   struct anv_device *device = cmd_buffer->device;
   VkResult result;

   list_inithead(&cmd_buffer->batch_bos);
   list_inithead(&cmd_buffer->surface_bos);

   result = anv_batch_bo_create(device, &batch_bo);
   if (result != VK_SUCCESS)
      return result;

   list_addtail(&batch_bo->link, &cmd_buffer->batch_bos);

   cmd_buffer->batch.device = device;
   cmd_buffer->batch.extend_cb = anv_cmd_buffer_chain_batch;
   cmd_buffer->batch.user_data = cmd_buffer;

   anv_batch_bo_start(batch_bo, &cmd_buffer->batch,
                      GEN8_MI_BATCH_BUFFER_START_length * 4);

   result = anv_batch_bo_create(device, &surface_bbo);
   if (result != VK_SUCCESS)
      goto fail_batch_bo;

   list_addtail(&surface_bbo->link, &cmd_buffer->surface_bos);

   int success = anv_vector_init(&cmd_buffer->seen_bbos,
                                 sizeof(struct anv_bo *),
                                 8 * sizeof(struct anv_bo *));
   if (!success)
      goto fail_surface_bo;

   *(struct anv_batch_bo **)anv_vector_add(&cmd_buffer->seen_bbos) = batch_bo;
   *(struct anv_batch_bo **)anv_vector_add(&cmd_buffer->seen_bbos) = surface_bbo;

   /* Start surface_next at 1 so surface offset 0 is invalid. */
   cmd_buffer->surface_next = 1;

   cmd_buffer->execbuf2.objects = NULL;
   cmd_buffer->execbuf2.bos = NULL;
   cmd_buffer->execbuf2.array_length = 0;

   return VK_SUCCESS;

 fail_surface_bo:
   anv_batch_bo_destroy(surface_bbo, device);
 fail_batch_bo:
   anv_batch_bo_destroy(batch_bo, device);

   return result;
}

void
anv_cmd_buffer_fini_batch_bo_chain(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;

   anv_vector_finish(&cmd_buffer->seen_bbos);

   /* Destroy all of the batch buffers */
   list_for_each_entry_safe(struct anv_batch_bo, bbo,
                            &cmd_buffer->batch_bos, link) {
      anv_batch_bo_destroy(bbo, device);
   }

   /* Destroy all of the surface state buffers */
   list_for_each_entry_safe(struct anv_batch_bo, bbo,
                            &cmd_buffer->surface_bos, link) {
      anv_batch_bo_destroy(bbo, device);
   }

   anv_device_free(device, cmd_buffer->execbuf2.objects);
   anv_device_free(device, cmd_buffer->execbuf2.bos);
}

void
anv_cmd_buffer_reset_batch_bo_chain(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;

   /* Delete all but the first batch bo */
   assert(!list_empty(&cmd_buffer->batch_bos));
   while (cmd_buffer->batch_bos.next != cmd_buffer->batch_bos.prev) {
      struct anv_batch_bo *bbo = anv_cmd_buffer_current_batch_bo(cmd_buffer);
      list_del(&bbo->link);
      anv_batch_bo_destroy(bbo, device);
   }
   assert(!list_empty(&cmd_buffer->batch_bos));

   anv_batch_bo_start(anv_cmd_buffer_current_batch_bo(cmd_buffer),
                      &cmd_buffer->batch,
                      GEN8_MI_BATCH_BUFFER_START_length * 4);

   /* Delete all but the first batch bo */
   assert(!list_empty(&cmd_buffer->batch_bos));
   while (cmd_buffer->surface_bos.next != cmd_buffer->surface_bos.prev) {
      struct anv_batch_bo *bbo = anv_cmd_buffer_current_surface_bbo(cmd_buffer);
      list_del(&bbo->link);
      anv_batch_bo_destroy(bbo, device);
   }
   assert(!list_empty(&cmd_buffer->batch_bos));

   anv_cmd_buffer_current_surface_bbo(cmd_buffer)->relocs.num_relocs = 0;

   cmd_buffer->surface_next = 1;

   /* Reset the list of seen buffers */
   cmd_buffer->seen_bbos.head = 0;
   cmd_buffer->seen_bbos.tail = 0;

   *(struct anv_batch_bo **)anv_vector_add(&cmd_buffer->seen_bbos) =
      anv_cmd_buffer_current_batch_bo(cmd_buffer);
   *(struct anv_batch_bo **)anv_vector_add(&cmd_buffer->seen_bbos) =
      anv_cmd_buffer_current_surface_bbo(cmd_buffer);
}

void
anv_cmd_buffer_end_batch_buffer(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_batch_bo *batch_bo = anv_cmd_buffer_current_batch_bo(cmd_buffer);
   struct anv_batch_bo *surface_bbo =
      anv_cmd_buffer_current_surface_bbo(cmd_buffer);

   if (cmd_buffer->level == VK_CMD_BUFFER_LEVEL_PRIMARY) {
      anv_batch_emit(&cmd_buffer->batch, GEN7_MI_BATCH_BUFFER_END);

      /* Round batch up to an even number of dwords. */
      if ((cmd_buffer->batch.next - cmd_buffer->batch.start) & 4)
         anv_batch_emit(&cmd_buffer->batch, GEN7_MI_NOOP);

      cmd_buffer->exec_mode = ANV_CMD_BUFFER_EXEC_MODE_PRIMARY;
   }

   anv_batch_bo_finish(batch_bo, &cmd_buffer->batch);

   surface_bbo->length = cmd_buffer->surface_next;

   if (cmd_buffer->level == VK_CMD_BUFFER_LEVEL_SECONDARY) {
      /* If this is a secondary command buffer, we need to determine the
       * mode in which it will be executed with vkExecuteCommands.  We
       * determine this statically here so that this stays in sync with the
       * actual ExecuteCommands implementation.
       */
      if ((cmd_buffer->batch_bos.next == cmd_buffer->batch_bos.prev) &&
          (anv_cmd_buffer_current_batch_bo(cmd_buffer)->length <
           ANV_CMD_BUFFER_BATCH_SIZE / 2)) {
         /* If the secondary has exactly one batch buffer in its list *and*
          * that batch buffer is less than half of the maximum size, we're
          * probably better of simply copying it into our batch.
          */
         cmd_buffer->exec_mode = ANV_CMD_BUFFER_EXEC_MODE_EMIT;
      } else if (cmd_buffer->opt_flags &
                 VK_CMD_BUFFER_OPTIMIZE_NO_SIMULTANEOUS_USE_BIT) {
         cmd_buffer->exec_mode = ANV_CMD_BUFFER_EXEC_MODE_CHAIN;

         /* When we chain, we need to add an MI_BATCH_BUFFER_START command
          * with its relocation.  In order to handle this we'll increment here
          * so we can unconditionally decrement right before adding the
          * MI_BATCH_BUFFER_START command.
          */
         anv_cmd_buffer_current_batch_bo(cmd_buffer)->relocs.num_relocs++;
         cmd_buffer->batch.next += GEN8_MI_BATCH_BUFFER_START_length * 4;
      } else {
         cmd_buffer->exec_mode = ANV_CMD_BUFFER_EXEC_MODE_COPY_AND_CHAIN;
      }
   }
}

static inline VkResult
anv_cmd_buffer_add_seen_bbos(struct anv_cmd_buffer *cmd_buffer,
                             struct list_head *list)
{
   list_for_each_entry(struct anv_batch_bo, bbo, list, link) {
      struct anv_batch_bo **bbo_ptr = anv_vector_add(&cmd_buffer->seen_bbos);
      if (bbo_ptr == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      *bbo_ptr = bbo;
   }

   return VK_SUCCESS;
}

void
anv_cmd_buffer_add_secondary(struct anv_cmd_buffer *primary,
                             struct anv_cmd_buffer *secondary)
{
   switch (secondary->exec_mode) {
   case ANV_CMD_BUFFER_EXEC_MODE_EMIT:
      anv_batch_emit_batch(&primary->batch, &secondary->batch);
      break;
   case ANV_CMD_BUFFER_EXEC_MODE_CHAIN: {
      struct anv_batch_bo *first_bbo =
         list_first_entry(&secondary->batch_bos, struct anv_batch_bo, link);
      struct anv_batch_bo *last_bbo =
         list_last_entry(&secondary->batch_bos, struct anv_batch_bo, link);

      emit_batch_buffer_start(&primary->batch, &first_bbo->bo, 0);

      struct anv_batch_bo *this_bbo = anv_cmd_buffer_current_batch_bo(primary);
      assert(primary->batch.start == this_bbo->bo.map);
      uint32_t offset = primary->batch.next - primary->batch.start;

      /* Roll back the previous MI_BATCH_BUFFER_START and its relocation so we
       * can emit a new command and relocation for the current splice.  In
       * order to handle the initial-use case, we incremented next and
       * num_relocs in end_batch_buffer() so we can alyways just subtract
       * here.
       */
      last_bbo->relocs.num_relocs--;
      secondary->batch.next -= GEN8_MI_BATCH_BUFFER_START_length * 4;
      emit_batch_buffer_start(&secondary->batch, &this_bbo->bo, offset);
      anv_cmd_buffer_add_seen_bbos(primary, &secondary->batch_bos);
      break;
   }
   case ANV_CMD_BUFFER_EXEC_MODE_COPY_AND_CHAIN: {
      struct list_head copy_list;
      VkResult result = anv_batch_bo_list_clone(&secondary->batch_bos,
                                                secondary->device,
                                                &copy_list);
      if (result != VK_SUCCESS)
         return; /* FIXME */

      anv_cmd_buffer_add_seen_bbos(primary, &copy_list);

      struct anv_batch_bo *first_bbo =
         list_first_entry(&copy_list, struct anv_batch_bo, link);
      struct anv_batch_bo *last_bbo =
         list_last_entry(&copy_list, struct anv_batch_bo, link);

      cmd_buffer_chain_to_batch_bo(primary, first_bbo);

      list_splicetail(&copy_list, &primary->batch_bos);

      anv_batch_bo_continue(last_bbo, &primary->batch,
                            GEN8_MI_BATCH_BUFFER_START_length * 4);

      anv_cmd_buffer_emit_state_base_address(primary);
      break;
   }
   default:
      assert(!"Invalid execution mode");
   }

   /* Mark the surface buffer from the secondary as seen */
   anv_cmd_buffer_add_seen_bbos(primary, &secondary->surface_bos);
}

static VkResult
anv_cmd_buffer_add_bo(struct anv_cmd_buffer *cmd_buffer,
                      struct anv_bo *bo,
                      struct anv_reloc_list *relocs)
{
   struct drm_i915_gem_exec_object2 *obj = NULL;

   if (bo->index < cmd_buffer->execbuf2.bo_count &&
       cmd_buffer->execbuf2.bos[bo->index] == bo)
      obj = &cmd_buffer->execbuf2.objects[bo->index];

   if (obj == NULL) {
      /* We've never seen this one before.  Add it to the list and assign
       * an id that we can use later.
       */
      if (cmd_buffer->execbuf2.bo_count >= cmd_buffer->execbuf2.array_length) {
         uint32_t new_len = cmd_buffer->execbuf2.objects ?
                            cmd_buffer->execbuf2.array_length * 2 : 64;

         struct drm_i915_gem_exec_object2 *new_objects =
            anv_device_alloc(cmd_buffer->device, new_len * sizeof(*new_objects),
                             8, VK_SYSTEM_ALLOC_TYPE_INTERNAL);
         if (new_objects == NULL)
            return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

         struct anv_bo **new_bos =
            anv_device_alloc(cmd_buffer->device, new_len * sizeof(*new_bos),
                             8, VK_SYSTEM_ALLOC_TYPE_INTERNAL);
         if (new_objects == NULL) {
            anv_device_free(cmd_buffer->device, new_objects);
            return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
         }

         if (cmd_buffer->execbuf2.objects) {
            memcpy(new_objects, cmd_buffer->execbuf2.objects,
                   cmd_buffer->execbuf2.bo_count * sizeof(*new_objects));
            memcpy(new_bos, cmd_buffer->execbuf2.bos,
                   cmd_buffer->execbuf2.bo_count * sizeof(*new_bos));
         }

         cmd_buffer->execbuf2.objects = new_objects;
         cmd_buffer->execbuf2.bos = new_bos;
         cmd_buffer->execbuf2.array_length = new_len;
      }

      assert(cmd_buffer->execbuf2.bo_count < cmd_buffer->execbuf2.array_length);

      bo->index = cmd_buffer->execbuf2.bo_count++;
      obj = &cmd_buffer->execbuf2.objects[bo->index];
      cmd_buffer->execbuf2.bos[bo->index] = bo;

      obj->handle = bo->gem_handle;
      obj->relocation_count = 0;
      obj->relocs_ptr = 0;
      obj->alignment = 0;
      obj->offset = bo->offset;
      obj->flags = 0;
      obj->rsvd1 = 0;
      obj->rsvd2 = 0;
   }

   if (relocs != NULL && obj->relocation_count == 0) {
      /* This is the first time we've ever seen a list of relocations for
       * this BO.  Go ahead and set the relocations and then walk the list
       * of relocations and add them all.
       */
      obj->relocation_count = relocs->num_relocs;
      obj->relocs_ptr = (uintptr_t) relocs->relocs;

      for (size_t i = 0; i < relocs->num_relocs; i++)
         anv_cmd_buffer_add_bo(cmd_buffer, relocs->reloc_bos[i], NULL);
   }

   return VK_SUCCESS;
}

static void
anv_cmd_buffer_process_relocs(struct anv_cmd_buffer *cmd_buffer,
                              struct anv_reloc_list *list)
{
   struct anv_bo *bo;

   /* If the kernel supports I915_EXEC_NO_RELOC, it will compare offset in
    * struct drm_i915_gem_exec_object2 against the bos current offset and if
    * all bos haven't moved it will skip relocation processing alltogether.
    * If I915_EXEC_NO_RELOC is not supported, the kernel ignores the incoming
    * value of offset so we can set it either way.  For that to work we need
    * to make sure all relocs use the same presumed offset.
    */

   for (size_t i = 0; i < list->num_relocs; i++) {
      bo = list->reloc_bos[i];
      if (bo->offset != list->relocs[i].presumed_offset)
         cmd_buffer->execbuf2.need_reloc = true;

      list->relocs[i].target_handle = bo->index;
   }
}

void
anv_cmd_buffer_prepare_execbuf(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_batch *batch = &cmd_buffer->batch;

   cmd_buffer->execbuf2.bo_count = 0;
   cmd_buffer->execbuf2.need_reloc = false;

   /* First, we walk over all of the bos we've seen and add them and their
    * relocations to the validate list.
    */
   struct anv_batch_bo **bbo;
   anv_vector_foreach(bbo, &cmd_buffer->seen_bbos)
      anv_cmd_buffer_add_bo(cmd_buffer, &(*bbo)->bo, &(*bbo)->relocs);

   struct anv_batch_bo *first_batch_bo =
      list_first_entry(&cmd_buffer->batch_bos, struct anv_batch_bo, link);

   /* The kernel requires that the last entry in the validation list be the
    * batch buffer to execute.  We can simply swap the element
    * corresponding to the first batch_bo in the chain with the last
    * element in the list.
    */
   if (first_batch_bo->bo.index != cmd_buffer->execbuf2.bo_count - 1) {
      uint32_t idx = first_batch_bo->bo.index;
      uint32_t last_idx = cmd_buffer->execbuf2.bo_count - 1;

      struct drm_i915_gem_exec_object2 tmp_obj =
         cmd_buffer->execbuf2.objects[idx];
      assert(cmd_buffer->execbuf2.bos[idx] == &first_batch_bo->bo);

      cmd_buffer->execbuf2.objects[idx] = cmd_buffer->execbuf2.objects[last_idx];
      cmd_buffer->execbuf2.bos[idx] = cmd_buffer->execbuf2.bos[last_idx];
      cmd_buffer->execbuf2.bos[idx]->index = idx;

      cmd_buffer->execbuf2.objects[last_idx] = tmp_obj;
      cmd_buffer->execbuf2.bos[last_idx] = &first_batch_bo->bo;
      first_batch_bo->bo.index = last_idx;
   }

   /* Now we go through and fixup all of the relocation lists to point to
    * the correct indices in the object array.  We have to do this after we
    * reorder the list above as some of the indices may have changed.
    */
   anv_vector_foreach(bbo, &cmd_buffer->seen_bbos)
      anv_cmd_buffer_process_relocs(cmd_buffer, &(*bbo)->relocs);

   cmd_buffer->execbuf2.execbuf = (struct drm_i915_gem_execbuffer2) {
      .buffers_ptr = (uintptr_t) cmd_buffer->execbuf2.objects,
      .buffer_count = cmd_buffer->execbuf2.bo_count,
      .batch_start_offset = 0,
      .batch_len = batch->next - batch->start,
      .cliprects_ptr = 0,
      .num_cliprects = 0,
      .DR1 = 0,
      .DR4 = 0,
      .flags = I915_EXEC_HANDLE_LUT | I915_EXEC_RENDER |
               I915_EXEC_CONSTANTS_REL_GENERAL,
      .rsvd1 = cmd_buffer->device->context_id,
      .rsvd2 = 0,
   };

   if (!cmd_buffer->execbuf2.need_reloc)
      cmd_buffer->execbuf2.execbuf.flags |= I915_EXEC_NO_RELOC;
}
