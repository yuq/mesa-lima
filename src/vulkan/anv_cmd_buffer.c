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

/** \file anv_cmd_buffer.c
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

VkResult
anv_reloc_list_init(struct anv_reloc_list *list, struct anv_device *device)
{
   list->num_relocs = 0;
   list->array_length = 256;
   list->relocs =
      anv_device_alloc(device, list->array_length * sizeof(*list->relocs), 8,
                       VK_SYSTEM_ALLOC_TYPE_INTERNAL);

   if (list->relocs == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   list->reloc_bos =
      anv_device_alloc(device, list->array_length * sizeof(*list->reloc_bos), 8,
                       VK_SYSTEM_ALLOC_TYPE_INTERNAL);

   if (list->relocs == NULL) {
      anv_device_free(device, list->relocs);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   return VK_SUCCESS;
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
   return anv_reloc_list_add(&batch->relocs, batch->device,
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
   anv_reloc_list_append(&batch->relocs, batch->device,
                         &other->relocs, offset);

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

   bbo->num_relocs = 0;
   bbo->prev_batch_bo = NULL;

   result = anv_bo_pool_alloc(&device->batch_bo_pool, &bbo->bo);
   if (result != VK_SUCCESS) {
      anv_device_free(device, bbo);
      return result;
   }

   *bbo_out = bbo;

   return VK_SUCCESS;
}

static void
anv_batch_bo_start(struct anv_batch_bo *bbo, struct anv_batch *batch,
                   size_t batch_padding)
{
   batch->next = batch->start = bbo->bo.map;
   batch->end = bbo->bo.map + bbo->bo.size - batch_padding;
   bbo->first_reloc = batch->relocs.num_relocs;
}

static void
anv_batch_bo_finish(struct anv_batch_bo *bbo, struct anv_batch *batch)
{
   /* Round batch up to an even number of dwords. */
   if ((batch->next - batch->start) & 4)
      anv_batch_emit(batch, GEN8_MI_NOOP);

   assert(batch->start == bbo->bo.map);
   bbo->length = batch->next - batch->start;
   VG(VALGRIND_CHECK_MEM_IS_DEFINED(batch->start, bbo->length));
   bbo->num_relocs = batch->relocs.num_relocs - bbo->first_reloc;
}

static void
anv_batch_bo_destroy(struct anv_batch_bo *bbo, struct anv_device *device)
{
   anv_bo_pool_free(&device->batch_bo_pool, &bbo->bo);
   anv_device_free(device, bbo);
}

/*-----------------------------------------------------------------------*
 * Functions related to anv_batch_bo
 *-----------------------------------------------------------------------*/

static VkResult
anv_cmd_buffer_chain_batch(struct anv_batch *batch, void *_data)
{
   struct anv_cmd_buffer *cmd_buffer = _data;

   struct anv_batch_bo *new_bbo, *old_bbo = cmd_buffer->last_batch_bo;

   VkResult result = anv_batch_bo_create(cmd_buffer->device, &new_bbo);
   if (result != VK_SUCCESS)
      return result;

   /* We set the end of the batch a little short so we would be sure we
    * have room for the chaining command.  Since we're about to emit the
    * chaining command, let's set it back where it should go.
    */
   batch->end += GEN8_MI_BATCH_BUFFER_START_length * 4;
   assert(batch->end == old_bbo->bo.map + old_bbo->bo.size);

   anv_batch_emit(batch, GEN8_MI_BATCH_BUFFER_START,
      GEN8_MI_BATCH_BUFFER_START_header,
      ._2ndLevelBatchBuffer = _1stlevelbatch,
      .AddressSpaceIndicator = ASI_PPGTT,
      .BatchBufferStartAddress = { &new_bbo->bo, 0 },
   );

   anv_batch_bo_finish(cmd_buffer->last_batch_bo, batch);

   new_bbo->prev_batch_bo = old_bbo;
   cmd_buffer->last_batch_bo = new_bbo;

   anv_batch_bo_start(new_bbo, batch, GEN8_MI_BATCH_BUFFER_START_length * 4);

   return VK_SUCCESS;
}

struct anv_state
anv_cmd_buffer_alloc_surface_state(struct anv_cmd_buffer *cmd_buffer,
                                   uint32_t size, uint32_t alignment)
{
   struct anv_state state;

   state.offset = align_u32(cmd_buffer->surface_next, alignment);
   if (state.offset + size > cmd_buffer->surface_batch_bo->bo.size)
      return (struct anv_state) { 0 };

   state.map = cmd_buffer->surface_batch_bo->bo.map + state.offset;
   state.alloc_size = size;
   cmd_buffer->surface_next = state.offset + size;

   assert(state.offset + size <= cmd_buffer->surface_batch_bo->bo.size);

   return state;
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
   struct anv_batch_bo *new_bbo, *old_bbo = cmd_buffer->surface_batch_bo;

   /* Finish off the old buffer */
   old_bbo->num_relocs =
      cmd_buffer->surface_relocs.num_relocs - old_bbo->first_reloc;
   old_bbo->length = cmd_buffer->surface_next;

   VkResult result = anv_batch_bo_create(cmd_buffer->device, &new_bbo);
   if (result != VK_SUCCESS)
      return result;

   new_bbo->first_reloc = cmd_buffer->surface_relocs.num_relocs;
   cmd_buffer->surface_next = 1;

   new_bbo->prev_batch_bo = old_bbo;
   cmd_buffer->surface_batch_bo = new_bbo;

   return VK_SUCCESS;
}

VkResult
anv_cmd_buffer_init_batch_bo_chain(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   VkResult result;

   result = anv_batch_bo_create(device, &cmd_buffer->last_batch_bo);
   if (result != VK_SUCCESS)
      return result;

   result = anv_reloc_list_init(&cmd_buffer->batch.relocs, device);
   if (result != VK_SUCCESS)
      goto fail_batch_bo;

   cmd_buffer->batch.device = device;
   cmd_buffer->batch.extend_cb = anv_cmd_buffer_chain_batch;
   cmd_buffer->batch.user_data = cmd_buffer;

   anv_batch_bo_start(cmd_buffer->last_batch_bo, &cmd_buffer->batch,
                      GEN8_MI_BATCH_BUFFER_START_length * 4);

   result = anv_batch_bo_create(device, &cmd_buffer->surface_batch_bo);
   if (result != VK_SUCCESS)
      goto fail_batch_relocs;
   cmd_buffer->surface_batch_bo->first_reloc = 0;

   result = anv_reloc_list_init(&cmd_buffer->surface_relocs, device);
   if (result != VK_SUCCESS)
      goto fail_ss_batch_bo;

   /* Start surface_next at 1 so surface offset 0 is invalid. */
   cmd_buffer->surface_next = 1;

   cmd_buffer->execbuf2.objects = NULL;
   cmd_buffer->execbuf2.bos = NULL;
   cmd_buffer->execbuf2.array_length = 0;

   return VK_SUCCESS;

 fail_ss_batch_bo:
   anv_batch_bo_destroy(cmd_buffer->surface_batch_bo, device);
 fail_batch_relocs:
   anv_reloc_list_finish(&cmd_buffer->batch.relocs, device);
 fail_batch_bo:
   anv_batch_bo_destroy(cmd_buffer->last_batch_bo, device);

   return result;
}

void
anv_cmd_buffer_fini_batch_bo_chain(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;

   /* Destroy all of the batch buffers */
   struct anv_batch_bo *bbo = cmd_buffer->last_batch_bo;
   while (bbo) {
      struct anv_batch_bo *prev = bbo->prev_batch_bo;
      anv_batch_bo_destroy(bbo, device);
      bbo = prev;
   }
   anv_reloc_list_finish(&cmd_buffer->batch.relocs, device);

   /* Destroy all of the surface state buffers */
   bbo = cmd_buffer->surface_batch_bo;
   while (bbo) {
      struct anv_batch_bo *prev = bbo->prev_batch_bo;
      anv_batch_bo_destroy(bbo, device);
      bbo = prev;
   }
   anv_reloc_list_finish(&cmd_buffer->surface_relocs, device);

   anv_device_free(device, cmd_buffer->execbuf2.objects);
   anv_device_free(device, cmd_buffer->execbuf2.bos);
}

void
anv_cmd_buffer_reset_batch_bo_chain(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;

   /* Delete all but the first batch bo */
   while (cmd_buffer->last_batch_bo->prev_batch_bo) {
      struct anv_batch_bo *prev = cmd_buffer->last_batch_bo->prev_batch_bo;
      anv_batch_bo_destroy(cmd_buffer->last_batch_bo, device);
      cmd_buffer->last_batch_bo = prev;
   }
   assert(cmd_buffer->last_batch_bo->prev_batch_bo == NULL);

   cmd_buffer->batch.relocs.num_relocs = 0;
   anv_batch_bo_start(cmd_buffer->last_batch_bo, &cmd_buffer->batch,
                      GEN8_MI_BATCH_BUFFER_START_length * 4);

   /* Delete all but the first batch bo */
   while (cmd_buffer->surface_batch_bo->prev_batch_bo) {
      struct anv_batch_bo *prev = cmd_buffer->surface_batch_bo->prev_batch_bo;
      anv_batch_bo_destroy(cmd_buffer->surface_batch_bo, device);
      cmd_buffer->surface_batch_bo = prev;
   }
   assert(cmd_buffer->surface_batch_bo->prev_batch_bo == NULL);

   cmd_buffer->surface_next = 1;
   cmd_buffer->surface_relocs.num_relocs = 0;
}

static VkResult
anv_cmd_buffer_add_bo(struct anv_cmd_buffer *cmd_buffer,
                      struct anv_bo *bo,
                      struct drm_i915_gem_relocation_entry *relocs,
                      size_t num_relocs)
{
   struct drm_i915_gem_exec_object2 *obj;

   if (bo->index < cmd_buffer->execbuf2.bo_count &&
       cmd_buffer->execbuf2.bos[bo->index] == bo)
      return VK_SUCCESS;

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

   if (relocs) {
      obj->relocation_count = num_relocs;
      obj->relocs_ptr = (uintptr_t) relocs;
   }

   return VK_SUCCESS;
}

static void
anv_cmd_buffer_add_validate_bos(struct anv_cmd_buffer *cmd_buffer,
                                struct anv_reloc_list *list)
{
   for (size_t i = 0; i < list->num_relocs; i++)
      anv_cmd_buffer_add_bo(cmd_buffer, list->reloc_bos[i], NULL, 0);
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
anv_cmd_buffer_emit_batch_buffer_end(struct anv_cmd_buffer *cmd_buffer)
{
   anv_batch_emit(&cmd_buffer->batch, GEN8_MI_BATCH_BUFFER_END);

   anv_batch_bo_finish(cmd_buffer->last_batch_bo, &cmd_buffer->batch);
   cmd_buffer->surface_batch_bo->num_relocs =
      cmd_buffer->surface_relocs.num_relocs - cmd_buffer->surface_batch_bo->first_reloc;
   cmd_buffer->surface_batch_bo->length = cmd_buffer->surface_next;
}

void
anv_cmd_buffer_prepare_execbuf(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_batch *batch = &cmd_buffer->batch;

   cmd_buffer->execbuf2.bo_count = 0;
   cmd_buffer->execbuf2.need_reloc = false;

   /* Add surface state bos first so we can add them with their relocs. */
   for (struct anv_batch_bo *bbo = cmd_buffer->surface_batch_bo;
        bbo != NULL; bbo = bbo->prev_batch_bo) {
      anv_cmd_buffer_add_bo(cmd_buffer, &bbo->bo,
                            &cmd_buffer->surface_relocs.relocs[bbo->first_reloc],
                            bbo->num_relocs);
   }

   /* Add all of the BOs referenced by surface state */
   anv_cmd_buffer_add_validate_bos(cmd_buffer, &cmd_buffer->surface_relocs);

   /* Add all but the first batch BO */
   struct anv_batch_bo *batch_bo = cmd_buffer->last_batch_bo;
   while (batch_bo->prev_batch_bo) {
      anv_cmd_buffer_add_bo(cmd_buffer, &batch_bo->bo,
                            &batch->relocs.relocs[batch_bo->first_reloc],
                            batch_bo->num_relocs);
      batch_bo = batch_bo->prev_batch_bo;
   }

   /* Add everything referenced by the batches */
   anv_cmd_buffer_add_validate_bos(cmd_buffer, &batch->relocs);

   /* Add the first batch bo last */
   assert(batch_bo->prev_batch_bo == NULL && batch_bo->first_reloc == 0);
   anv_cmd_buffer_add_bo(cmd_buffer, &batch_bo->bo,
                         &batch->relocs.relocs[batch_bo->first_reloc],
                         batch_bo->num_relocs);
   assert(batch_bo->bo.index == cmd_buffer->execbuf2.bo_count - 1);

   anv_cmd_buffer_process_relocs(cmd_buffer, &cmd_buffer->surface_relocs);
   anv_cmd_buffer_process_relocs(cmd_buffer, &batch->relocs);

   cmd_buffer->execbuf2.execbuf = (struct drm_i915_gem_execbuffer2) {
      .buffers_ptr = (uintptr_t) cmd_buffer->execbuf2.objects,
      .buffer_count = cmd_buffer->execbuf2.bo_count,
      .batch_start_offset = 0,
      .batch_len = batch->next - batch->start,
      .cliprects_ptr = 0,
      .num_cliprects = 0,
      .DR1 = 0,
      .DR4 = 0,
      .flags = I915_EXEC_HANDLE_LUT | I915_EXEC_RENDER,
      .rsvd1 = cmd_buffer->device->context_id,
      .rsvd2 = 0,
   };

   if (!cmd_buffer->execbuf2.need_reloc)
      cmd_buffer->execbuf2.execbuf.flags |= I915_EXEC_NO_RELOC;
}
