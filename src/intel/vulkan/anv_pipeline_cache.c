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

#include "util/mesa-sha1.h"
#include "util/debug.h"
#include "anv_private.h"

/* Remaining work:
 *
 * - Compact binding table layout so it's tight and not dependent on
 *   descriptor set layout.
 *
 * - Review prog_data struct for size and cacheability: struct
 *   brw_stage_prog_data has binding_table which uses a lot of uint32_t for 8
 *   bit quantities etc; param, pull_param, and image_params are pointers, we
 *   just need the compation map. use bit fields for all bools, eg
 *   dual_src_blend.
 */

void
anv_pipeline_cache_init(struct anv_pipeline_cache *cache,
                        struct anv_device *device)
{
   cache->device = device;
   anv_state_stream_init(&cache->program_stream,
                         &device->instruction_block_pool);
   pthread_mutex_init(&cache->mutex, NULL);

   cache->kernel_count = 0;
   cache->total_size = 0;
   cache->table_size = 1024;
   const size_t byte_size = cache->table_size * sizeof(cache->table[0]);
   cache->table = malloc(byte_size);

   /* We don't consider allocation failure fatal, we just start with a 0-sized
    * cache. */
   if (cache->table == NULL)
      cache->table_size = 0;
   else
      memset(cache->table, 0xff, byte_size);
}

void
anv_pipeline_cache_finish(struct anv_pipeline_cache *cache)
{
   anv_state_stream_finish(&cache->program_stream);
   pthread_mutex_destroy(&cache->mutex);
   free(cache->table);
}

struct cache_entry {
   unsigned char sha1[20];
   uint32_t prog_data_size;
   uint32_t kernel_size;
   char prog_data[0];

   /* kernel follows prog_data at next 64 byte aligned address */
};

void
anv_hash_shader(unsigned char *hash, const void *key, size_t key_size,
                struct anv_shader_module *module,
                const char *entrypoint,
                const VkSpecializationInfo *spec_info)
{
   struct mesa_sha1 *ctx;

   ctx = _mesa_sha1_init();
   _mesa_sha1_update(ctx, key, key_size);
   _mesa_sha1_update(ctx, module->sha1, sizeof(module->sha1));
   _mesa_sha1_update(ctx, entrypoint, strlen(entrypoint));
   /* hash in shader stage, pipeline layout? */
   if (spec_info) {
      _mesa_sha1_update(ctx, spec_info->pMapEntries,
                        spec_info->mapEntryCount * sizeof spec_info->pMapEntries[0]);
      _mesa_sha1_update(ctx, spec_info->pData, spec_info->dataSize);
   }
   _mesa_sha1_final(ctx, hash);
}

uint32_t
anv_pipeline_cache_search(struct anv_pipeline_cache *cache,
                          const unsigned char *sha1, void *prog_data)
{
   const uint32_t mask = cache->table_size - 1;
   const uint32_t start = (*(uint32_t *) sha1);

   for (uint32_t i = 0; i < cache->table_size; i++) {
      const uint32_t index = (start + i) & mask;
      const uint32_t offset = cache->table[index];

      if (offset == ~0)
         return NO_KERNEL;

      struct cache_entry *entry =
         cache->program_stream.block_pool->map + offset;
      if (memcmp(entry->sha1, sha1, sizeof(entry->sha1)) == 0) {
         if (prog_data)
            memcpy(prog_data, entry->prog_data, entry->prog_data_size);

         const uint32_t preamble_size =
            align_u32(sizeof(*entry) + entry->prog_data_size, 64);

         return offset + preamble_size;
      }
   }

   return NO_KERNEL;
}

static void
anv_pipeline_cache_add_entry(struct anv_pipeline_cache *cache,
                             struct cache_entry *entry, uint32_t entry_offset)
{
   const uint32_t mask = cache->table_size - 1;
   const uint32_t start = (*(uint32_t *) entry->sha1);

   /* We'll always be able to insert when we get here. */
   assert(cache->kernel_count < cache->table_size / 2);

   for (uint32_t i = 0; i < cache->table_size; i++) {
      const uint32_t index = (start + i) & mask;
      if (cache->table[index] == ~0) {
         cache->table[index] = entry_offset;
         break;
      }
   }

   /* We don't include the alignment padding bytes when we serialize, so
    * don't include taht in the the total size. */
   cache->total_size +=
      sizeof(*entry) + entry->prog_data_size + entry->kernel_size;
   cache->kernel_count++;
}

static VkResult
anv_pipeline_cache_grow(struct anv_pipeline_cache *cache)
{
   const uint32_t table_size = cache->table_size * 2;
   const uint32_t old_table_size = cache->table_size;
   const size_t byte_size = table_size * sizeof(cache->table[0]);
   uint32_t *table;
   uint32_t *old_table = cache->table;

   table = malloc(byte_size);
   if (table == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   cache->table = table;
   cache->table_size = table_size;
   cache->kernel_count = 0;
   cache->total_size = 0;

   memset(cache->table, 0xff, byte_size);
   for (uint32_t i = 0; i < old_table_size; i++) {
      const uint32_t offset = old_table[i];
      if (offset == ~0)
         continue;

      struct cache_entry *entry =
         cache->program_stream.block_pool->map + offset;
      anv_pipeline_cache_add_entry(cache, entry, offset);
   }

   free(old_table);

   return VK_SUCCESS;
}

uint32_t
anv_pipeline_cache_upload_kernel(struct anv_pipeline_cache *cache,
                                 const unsigned char *sha1,
                                 const void *kernel, size_t kernel_size,
                                 const void *prog_data, size_t prog_data_size)
{
   pthread_mutex_lock(&cache->mutex);
   struct cache_entry *entry;

   /* Meta pipelines don't have SPIR-V, so we can't hash them.
    * Consequentally, they just don't get cached.
    */
   const uint32_t preamble_size = sha1 ?
      align_u32(sizeof(*entry) + prog_data_size, 64) :
      0;

   const uint32_t size = preamble_size + kernel_size;

   assert(size < cache->program_stream.block_pool->block_size);
   const struct anv_state state =
      anv_state_stream_alloc(&cache->program_stream, size, 64);

   if (sha1 && env_var_as_boolean("ANV_ENABLE_PIPELINE_CACHE", false)) {
      assert(anv_pipeline_cache_search(cache, sha1, NULL) == NO_KERNEL);
      entry = state.map;
      memcpy(entry->sha1, sha1, sizeof(entry->sha1));
      entry->prog_data_size = prog_data_size;
      memcpy(entry->prog_data, prog_data, prog_data_size);
      entry->kernel_size = kernel_size;

      if (cache->kernel_count == cache->table_size / 2)
         anv_pipeline_cache_grow(cache);

      /* Failing to grow that hash table isn't fatal, but may mean we don't
       * have enough space to add this new kernel. Only add it if there's room.
       */
      if (cache->kernel_count < cache->table_size / 2)
         anv_pipeline_cache_add_entry(cache, entry, state.offset);
   }

   pthread_mutex_unlock(&cache->mutex);

   memcpy(state.map + preamble_size, kernel, kernel_size);

   if (!cache->device->info.has_llc)
      anv_state_clflush(state);

   return state.offset + preamble_size;
}

static void
anv_pipeline_cache_load(struct anv_pipeline_cache *cache,
                        const void *data, size_t size)
{
   struct anv_device *device = cache->device;
   uint8_t uuid[VK_UUID_SIZE];
   struct {
      uint32_t device_id;
      uint8_t uuid[VK_UUID_SIZE];
   } header;

   if (size < sizeof(header))
      return;
   memcpy(&header, data, sizeof(header));
   if (header.device_id != device->chipset_id)
      return;
   anv_device_get_cache_uuid(uuid);
   if (memcmp(header.uuid, uuid, VK_UUID_SIZE) != 0)
      return;

   const void *end = data + size;
   const void *p = data + sizeof(header);

   while (p < end) {
      /* The kernels aren't 64 byte aligned in the serialized format so
       * they're always right after the prog_data.
       */
      const struct cache_entry *entry = p;
      const void *kernel = &entry->prog_data[entry->prog_data_size];

      anv_pipeline_cache_upload_kernel(cache, entry->sha1,
                                       kernel, entry->kernel_size,
                                       entry->prog_data, entry->prog_data_size);
      p = kernel + entry->kernel_size;
   }
}

VkResult anv_CreatePipelineCache(
    VkDevice                                    _device,
    const VkPipelineCacheCreateInfo*            pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkPipelineCache*                            pPipelineCache)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_pipeline_cache *cache;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO);
   assert(pCreateInfo->flags == 0);

   cache = anv_alloc2(&device->alloc, pAllocator,
                       sizeof(*cache), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cache == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   anv_pipeline_cache_init(cache, device);

   if (pCreateInfo->initialDataSize > 0)
      anv_pipeline_cache_load(cache,
                              pCreateInfo->pInitialData,
                              pCreateInfo->initialDataSize);

   *pPipelineCache = anv_pipeline_cache_to_handle(cache);

   return VK_SUCCESS;
}

void anv_DestroyPipelineCache(
    VkDevice                                    _device,
    VkPipelineCache                             _cache,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_pipeline_cache, cache, _cache);

   anv_pipeline_cache_finish(cache);

   anv_free2(&device->alloc, pAllocator, cache);
}

VkResult anv_GetPipelineCacheData(
    VkDevice                                    _device,
    VkPipelineCache                             _cache,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_pipeline_cache, cache, _cache);

   const size_t size = 4 + VK_UUID_SIZE + cache->total_size;

   if (pData == NULL) {
      *pDataSize = size;
      return VK_SUCCESS;
   }

   if (*pDataSize < size) {
      *pDataSize = 0;
      return VK_INCOMPLETE;
   }

   void *p = pData;
   memcpy(p, &device->chipset_id, sizeof(device->chipset_id));
   p += sizeof(device->chipset_id);

   anv_device_get_cache_uuid(p);
   p += VK_UUID_SIZE;

   struct cache_entry *entry;
   for (uint32_t i = 0; i < cache->table_size; i++) {
      if (cache->table[i] == ~0)
         continue;

      entry = cache->program_stream.block_pool->map + cache->table[i];

      memcpy(p, entry, sizeof(*entry) + entry->prog_data_size);
      p += sizeof(*entry) + entry->prog_data_size;

      void *kernel = (void *) entry +
         align_u32(sizeof(*entry) + entry->prog_data_size, 64);

      memcpy(p, kernel, entry->kernel_size);
      p += entry->kernel_size;
   }

   return VK_SUCCESS;
}

static void
anv_pipeline_cache_merge(struct anv_pipeline_cache *dst,
                         struct anv_pipeline_cache *src)
{
   for (uint32_t i = 0; i < src->table_size; i++) {
      if (src->table[i] == ~0)
         continue;

      struct cache_entry *entry =
         src->program_stream.block_pool->map + src->table[i];

      if (anv_pipeline_cache_search(dst, entry->sha1, NULL) != NO_KERNEL)
         continue;

      const void *kernel = (void *) entry +
         align_u32(sizeof(*entry) + entry->prog_data_size, 64);
      anv_pipeline_cache_upload_kernel(dst, entry->sha1,
                                       kernel, entry->kernel_size,
                                       entry->prog_data, entry->prog_data_size);
   }
}

VkResult anv_MergePipelineCaches(
    VkDevice                                    _device,
    VkPipelineCache                             destCache,
    uint32_t                                    srcCacheCount,
    const VkPipelineCache*                      pSrcCaches)
{
   ANV_FROM_HANDLE(anv_pipeline_cache, dst, destCache);

   for (uint32_t i = 0; i < srcCacheCount; i++) {
      ANV_FROM_HANDLE(anv_pipeline_cache, src, pSrcCaches[i]);

      anv_pipeline_cache_merge(dst, src);
   }

   return VK_SUCCESS;
}
