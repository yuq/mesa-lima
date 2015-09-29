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

#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <i915_drm.h>

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#include <memcheck.h>
#define VG(x) x
#define __gen_validate_value(x) VALGRIND_CHECK_MEM_IS_DEFINED(&(x), sizeof(x))
#else
#define VG(x)
#endif

#include "brw_device_info.h"
#include "util/macros.h"
#include "util/list.h"

#define VK_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_intel.h>
#include <vulkan/vk_ext_khr_swapchain.h>
#include <vulkan/vk_ext_khr_device_swapchain.h>

#include "anv_entrypoints.h"

#include "brw_context.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ICD_LOADER_MAGIC   0x01CDC0DE

typedef union _VK_LOADER_DATA {
  uintptr_t loaderMagic;
  void *loaderData;
} VK_LOADER_DATA;

#define anv_noreturn __attribute__((__noreturn__))
#define anv_printflike(a, b) __attribute__((__format__(__printf__, a, b)))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static inline uint32_t
align_u32(uint32_t v, uint32_t a)
{
   return (v + a - 1) & ~(a - 1);
}

static inline int32_t
align_i32(int32_t v, int32_t a)
{
   return (v + a - 1) & ~(a - 1);
}

/** Alignment must be a power of 2. */
static inline bool
anv_is_aligned(uintmax_t n, uintmax_t a)
{
   assert(a == (a & -a));
   return (n & (a - 1)) == 0;
}

static inline uint32_t
anv_minify(uint32_t n, uint32_t levels)
{
   if (unlikely(n == 0))
      return 0;
   else
      return MAX(n >> levels, 1);
}

static inline bool
anv_clear_mask(uint32_t *inout_mask, uint32_t clear_mask)
{
   if (*inout_mask & clear_mask) {
      *inout_mask &= ~clear_mask;
      return true;
   } else {
      return false;
   }
}

#define for_each_bit(b, dword)                          \
   for (uint32_t __dword = (dword);                     \
        (b) = __builtin_ffs(__dword) - 1, __dword;      \
        __dword &= ~(1 << (b)))

#define typed_memcpy(dest, src, count) ({ \
   static_assert(sizeof(*src) == sizeof(*dest), ""); \
   memcpy((dest), (src), (count) * sizeof(*(src))); \
})

/* Define no kernel as 1, since that's an illegal offset for a kernel */
#define NO_KERNEL 1

struct anv_common {
    VkStructureType                             sType;
    const void*                                 pNext;
};

/* Whenever we generate an error, pass it through this function. Useful for
 * debugging, where we can break on it. Only call at error site, not when
 * propagating errors. Might be useful to plug in a stack trace here.
 */

VkResult __vk_errorf(VkResult error, const char *file, int line, const char *format, ...);

#ifdef DEBUG
#define vk_error(error) __vk_errorf(error, __FILE__, __LINE__, NULL);
#define vk_errorf(error, format, ...) __vk_errorf(error, __FILE__, __LINE__, format, ## __VA_ARGS__);
#else
#define vk_error(error) error
#define vk_errorf(error, format, ...) error
#endif

void __anv_finishme(const char *file, int line, const char *format, ...)
   anv_printflike(3, 4);
void anv_loge(const char *format, ...) anv_printflike(1, 2);
void anv_loge_v(const char *format, va_list va);

/**
 * Print a FINISHME message, including its source location.
 */
#define anv_finishme(format, ...) \
   __anv_finishme(__FILE__, __LINE__, format, ##__VA_ARGS__);

/* A non-fatal assert.  Useful for debugging. */
#ifdef DEBUG
#define anv_assert(x) ({ \
   if (unlikely(!(x))) \
      fprintf(stderr, "%s:%d ASSERT: %s\n", __FILE__, __LINE__, #x); \
})
#else
#define anv_assert(x)
#endif

/**
 * If a block of code is annotated with anv_validate, then the block runs only
 * in debug builds.
 */
#ifdef DEBUG
#define anv_validate if (1)
#else
#define anv_validate if (0)
#endif

void anv_abortf(const char *format, ...) anv_noreturn anv_printflike(1, 2);
void anv_abortfv(const char *format, va_list va) anv_noreturn;

#define stub_return(v) \
   do { \
      anv_finishme("stub %s", __func__); \
      return (v); \
   } while (0)

#define stub() \
   do { \
      anv_finishme("stub %s", __func__); \
      return; \
   } while (0)

/**
 * A dynamically growable, circular buffer.  Elements are added at head and
 * removed from tail. head and tail are free-running uint32_t indices and we
 * only compute the modulo with size when accessing the array.  This way,
 * number of bytes in the queue is always head - tail, even in case of
 * wraparound.
 */

struct anv_vector {
   uint32_t head;
   uint32_t tail;
   uint32_t element_size;
   uint32_t size;
   void *data;
};

int anv_vector_init(struct anv_vector *queue, uint32_t element_size, uint32_t size);
void *anv_vector_add(struct anv_vector *queue);
void *anv_vector_remove(struct anv_vector *queue);

static inline int
anv_vector_length(struct anv_vector *queue)
{
   return (queue->head - queue->tail) / queue->element_size;
}

static inline void *
anv_vector_head(struct anv_vector *vector)
{
   assert(vector->tail < vector->head);
   return (void *)((char *)vector->data +
                   ((vector->head - vector->element_size) &
                    (vector->size - 1)));
}

static inline void *
anv_vector_tail(struct anv_vector *vector)
{
   return (void *)((char *)vector->data + (vector->tail & (vector->size - 1)));
}

static inline void
anv_vector_finish(struct anv_vector *queue)
{
   free(queue->data);
}

#define anv_vector_foreach(elem, queue)                                  \
   static_assert(__builtin_types_compatible_p(__typeof__(queue), struct anv_vector *), ""); \
   for (uint32_t __anv_vector_offset = (queue)->tail;                                \
        elem = (queue)->data + (__anv_vector_offset & ((queue)->size - 1)), __anv_vector_offset < (queue)->head; \
        __anv_vector_offset += (queue)->element_size)

struct anv_bo {
   int gem_handle;

   /* Index into the current validation list.  This is used by the
    * validation list building alrogithm to track which buffers are already
    * in the validation list so that we can ensure uniqueness.
    */
   uint32_t index;

   /* Last known offset.  This value is provided by the kernel when we
    * execbuf and is used as the presumed offset for the next bunch of
    * relocations.
    */
   uint64_t offset;

   uint64_t size;
   void *map;
};

/* Represents a lock-free linked list of "free" things.  This is used by
 * both the block pool and the state pools.  Unfortunately, in order to
 * solve the ABA problem, we can't use a single uint32_t head.
 */
union anv_free_list {
   struct {
      int32_t offset;

      /* A simple count that is incremented every time the head changes. */
      uint32_t count;
   };
   uint64_t u64;
};

#define ANV_FREE_LIST_EMPTY ((union anv_free_list) { { 1, 0 } })

struct anv_block_state {
   union {
      struct {
         uint32_t next;
         uint32_t end;
      };
      uint64_t u64;
   };
};

struct anv_block_pool {
   struct anv_device *device;

   struct anv_bo bo;

   /* The offset from the start of the bo to the "center" of the block
    * pool.  Pointers to allocated blocks are given by
    * bo.map + center_bo_offset + offsets.
    */
   uint32_t center_bo_offset;

   /* Current memory map of the block pool.  This pointer may or may not
    * point to the actual beginning of the block pool memory.  If
    * anv_block_pool_alloc_back has ever been called, then this pointer
    * will point to the "center" position of the buffer and all offsets
    * (negative or positive) given out by the block pool alloc functions
    * will be valid relative to this pointer.
    *
    * In particular, map == bo.map + center_offset
    */
   void *map;
   int fd;

   /**
    * Array of mmaps and gem handles owned by the block pool, reclaimed when
    * the block pool is destroyed.
    */
   struct anv_vector mmap_cleanups;

   uint32_t block_size;

   union anv_free_list free_list;
   struct anv_block_state state;

   union anv_free_list back_free_list;
   struct anv_block_state back_state;
};

/* Block pools are backed by a fixed-size 2GB memfd */
#define BLOCK_POOL_MEMFD_SIZE (1ull << 32)

/* The center of the block pool is also the middle of the memfd.  This may
 * change in the future if we decide differently for some reason.
 */
#define BLOCK_POOL_MEMFD_CENTER (BLOCK_POOL_MEMFD_SIZE / 2)

static inline uint32_t
anv_block_pool_size(struct anv_block_pool *pool)
{
   return pool->state.end + pool->back_state.end;
}

struct anv_state {
   int32_t offset;
   uint32_t alloc_size;
   void *map;
};

struct anv_fixed_size_state_pool {
   size_t state_size;
   union anv_free_list free_list;
   struct anv_block_state block;
};

#define ANV_MIN_STATE_SIZE_LOG2 6
#define ANV_MAX_STATE_SIZE_LOG2 10

#define ANV_STATE_BUCKETS (ANV_MAX_STATE_SIZE_LOG2 - ANV_MIN_STATE_SIZE_LOG2)

struct anv_state_pool {
   struct anv_block_pool *block_pool;
   struct anv_fixed_size_state_pool buckets[ANV_STATE_BUCKETS];
};

struct anv_state_stream {
   struct anv_block_pool *block_pool;
   uint32_t next;
   uint32_t current_block;
   uint32_t end;
};

void anv_block_pool_init(struct anv_block_pool *pool,
                         struct anv_device *device, uint32_t block_size);
void anv_block_pool_finish(struct anv_block_pool *pool);
int32_t anv_block_pool_alloc(struct anv_block_pool *pool);
int32_t anv_block_pool_alloc_back(struct anv_block_pool *pool);
void anv_block_pool_free(struct anv_block_pool *pool, int32_t offset);
void anv_state_pool_init(struct anv_state_pool *pool,
                         struct anv_block_pool *block_pool);
void anv_state_pool_finish(struct anv_state_pool *pool);
struct anv_state anv_state_pool_alloc(struct anv_state_pool *pool,
                                      size_t state_size, size_t alignment);
void anv_state_pool_free(struct anv_state_pool *pool, struct anv_state state);
void anv_state_stream_init(struct anv_state_stream *stream,
                           struct anv_block_pool *block_pool);
void anv_state_stream_finish(struct anv_state_stream *stream);
struct anv_state anv_state_stream_alloc(struct anv_state_stream *stream,
                                        uint32_t size, uint32_t alignment);

/**
 * Implements a pool of re-usable BOs.  The interface is identical to that
 * of block_pool except that each block is its own BO.
 */
struct anv_bo_pool {
   struct anv_device *device;

   uint32_t bo_size;

   void *free_list;
};

void anv_bo_pool_init(struct anv_bo_pool *pool,
                      struct anv_device *device, uint32_t block_size);
void anv_bo_pool_finish(struct anv_bo_pool *pool);
VkResult anv_bo_pool_alloc(struct anv_bo_pool *pool, struct anv_bo *bo);
void anv_bo_pool_free(struct anv_bo_pool *pool, const struct anv_bo *bo);


void *anv_resolve_entrypoint(uint32_t index);

extern struct anv_dispatch_table dtable;

#define ANV_CALL(func) ({ \
   if (dtable.func == NULL) { \
      size_t idx = offsetof(struct anv_dispatch_table, func) / sizeof(void *); \
      dtable.entrypoints[idx] = anv_resolve_entrypoint(idx); \
   } \
   dtable.func; \
})


struct anv_physical_device {
    VK_LOADER_DATA                              _loader_data;

    struct anv_instance *                       instance;
    uint32_t                                    chipset_id;
    const char *                                path;
    const char *                                name;
    const struct brw_device_info *              info;
    uint64_t                                    aperture_size;
    struct brw_compiler *                       compiler;
};

bool anv_is_scalar_shader_stage(const struct brw_compiler *compiler,
                                VkShaderStage stage);

struct anv_instance {
    VK_LOADER_DATA                              _loader_data;

    void *                                      pAllocUserData;
    PFN_vkAllocFunction                         pfnAlloc;
    PFN_vkFreeFunction                          pfnFree;
    uint32_t                                    apiVersion;
    int                                         physicalDeviceCount;
    struct anv_physical_device                  physicalDevice;

    struct anv_wsi_implementation *             wsi_impl[VK_PLATFORM_NUM_KHR];
};

VkResult anv_init_wsi(struct anv_instance *instance);
void anv_finish_wsi(struct anv_instance *instance);

struct anv_meta_state {
   struct {
      struct anv_pipeline *color_pipeline;
      struct anv_pipeline *depth_only_pipeline;
      struct anv_pipeline *stencil_only_pipeline;
      struct anv_pipeline *depthstencil_pipeline;
   } clear;

   struct {
      VkRenderPass render_pass;

      /** Pipeline that blits from a 2D image. */
      VkPipeline pipeline_2d_src;

      /** Pipeline that blits from a 3D image. */
      VkPipeline pipeline_3d_src;

      VkPipelineLayout                          pipeline_layout;
      VkDescriptorSetLayout                     ds_layout;
   } blit;
};

struct anv_queue {
    VK_LOADER_DATA                              _loader_data;

    struct anv_device *                         device;

    struct anv_state_pool *                     pool;
};

struct anv_device {
    VK_LOADER_DATA                              _loader_data;

    struct anv_instance *                       instance;
    uint32_t                                    chipset_id;
    struct brw_device_info                      info;
    int                                         context_id;
    int                                         fd;

    struct anv_bo_pool                          batch_bo_pool;

    struct anv_block_pool                       dynamic_state_block_pool;
    struct anv_state_pool                       dynamic_state_pool;

    struct anv_block_pool                       instruction_block_pool;
    struct anv_block_pool                       surface_state_block_pool;
    struct anv_state_pool                       surface_state_pool;

    struct anv_meta_state                       meta_state;

    struct anv_state                            border_colors;

    struct anv_queue                            queue;

    struct anv_block_pool                       scratch_block_pool;

    pthread_mutex_t                             mutex;
};

void *
anv_instance_alloc(struct anv_instance *        instance,
                   size_t                       size,
                   size_t                       alignment,
                   VkSystemAllocType            allocType);

void
anv_instance_free(struct anv_instance *         instance,
                  void *                        mem);

void *
anv_device_alloc(struct anv_device *            device,
                 size_t                         size,
                 size_t                         alignment,
                 VkSystemAllocType              allocType);

void
anv_device_free(struct anv_device *             device,
                void *                          mem);

void* anv_gem_mmap(struct anv_device *device,
                   uint32_t gem_handle, uint64_t offset, uint64_t size);
void anv_gem_munmap(void *p, uint64_t size);
uint32_t anv_gem_create(struct anv_device *device, size_t size);
void anv_gem_close(struct anv_device *device, int gem_handle);
int anv_gem_userptr(struct anv_device *device, void *mem, size_t size);
int anv_gem_wait(struct anv_device *device, int gem_handle, int64_t *timeout_ns);
int anv_gem_execbuffer(struct anv_device *device,
                       struct drm_i915_gem_execbuffer2 *execbuf);
int anv_gem_set_tiling(struct anv_device *device, int gem_handle,
                       uint32_t stride, uint32_t tiling);
int anv_gem_create_context(struct anv_device *device);
int anv_gem_destroy_context(struct anv_device *device, int context);
int anv_gem_get_param(int fd, uint32_t param);
int anv_gem_get_aperture(int fd, uint64_t *size);
int anv_gem_handle_to_fd(struct anv_device *device, int gem_handle);
int anv_gem_fd_to_handle(struct anv_device *device, int fd);
int anv_gem_userptr(struct anv_device *device, void *mem, size_t size);

VkResult anv_bo_init_new(struct anv_bo *bo, struct anv_device *device, uint64_t size);

struct anv_reloc_list {
   size_t                                       num_relocs;
   size_t                                       array_length;
   struct drm_i915_gem_relocation_entry *       relocs;
   struct anv_bo **                             reloc_bos;
};

VkResult anv_reloc_list_init(struct anv_reloc_list *list,
                             struct anv_device *device);
void anv_reloc_list_finish(struct anv_reloc_list *list,
                           struct anv_device *device);

uint64_t anv_reloc_list_add(struct anv_reloc_list *list,
                            struct anv_device *device,
                            uint32_t offset, struct anv_bo *target_bo,
                            uint32_t delta);

struct anv_batch_bo {
   /* Link in the anv_cmd_buffer.owned_batch_bos list */
   struct list_head                             link;

   struct anv_bo                                bo;

   /* Bytes actually consumed in this batch BO */
   size_t                                       length;

   /* Last seen surface state block pool bo offset */
   uint32_t                                     last_ss_pool_bo_offset;

   struct anv_reloc_list                        relocs;
};

struct anv_batch {
   struct anv_device *                          device;

   void *                                       start;
   void *                                       end;
   void *                                       next;

   struct anv_reloc_list *                      relocs;

   /* This callback is called (with the associated user data) in the event
    * that the batch runs out of space.
    */
   VkResult (*extend_cb)(struct anv_batch *, void *);
   void *                                       user_data;
};

void *anv_batch_emit_dwords(struct anv_batch *batch, int num_dwords);
void anv_batch_emit_batch(struct anv_batch *batch, struct anv_batch *other);
uint64_t anv_batch_emit_reloc(struct anv_batch *batch,
                              void *location, struct anv_bo *bo, uint32_t offset);

struct anv_address {
   struct anv_bo *bo;
   uint32_t offset;
};

#define __gen_address_type struct anv_address
#define __gen_user_data struct anv_batch

static inline uint64_t
__gen_combine_address(struct anv_batch *batch, void *location,
                      const struct anv_address address, uint32_t delta)
{
   if (address.bo == NULL) {
      return address.offset + delta;
   } else {
      assert(batch->start <= location && location < batch->end);

      return anv_batch_emit_reloc(batch, location, address.bo, address.offset + delta);
   }
}

#include "gen7_pack.h"
#include "gen75_pack.h"
#undef GEN8_3DSTATE_MULTISAMPLE
#include "gen8_pack.h"

#define anv_batch_emit(batch, cmd, ...) do {                            \
      void *__dst = anv_batch_emit_dwords(batch, cmd ## _length);       \
      struct cmd __template = {                                         \
         cmd ## _header,                                                \
         __VA_ARGS__                                                    \
      };                                                                \
      cmd ## _pack(batch, __dst, &__template);                          \
      VG(VALGRIND_CHECK_MEM_IS_DEFINED(__dst, cmd ## _length * 4));     \
   } while (0)

#define anv_batch_emitn(batch, n, cmd, ...) ({          \
      void *__dst = anv_batch_emit_dwords(batch, n);    \
      struct cmd __template = {                         \
         cmd ## _header,                                \
        .DwordLength = n - cmd ## _length_bias,         \
         __VA_ARGS__                                    \
      };                                                \
      cmd ## _pack(batch, __dst, &__template);          \
      __dst;                                            \
   })

#define anv_batch_emit_merge(batch, dwords0, dwords1)                   \
   do {                                                                 \
      uint32_t *dw;                                                     \
                                                                        \
      assert(ARRAY_SIZE(dwords0) == ARRAY_SIZE(dwords1));               \
      dw = anv_batch_emit_dwords((batch), ARRAY_SIZE(dwords0));         \
      for (uint32_t i = 0; i < ARRAY_SIZE(dwords0); i++)                \
         dw[i] = (dwords0)[i] | (dwords1)[i];                           \
      VG(VALGRIND_CHECK_MEM_IS_DEFINED(dw, ARRAY_SIZE(dwords0) * 4));\
   } while (0)

static const struct GEN7_MEMORY_OBJECT_CONTROL_STATE GEN7_MOCS = {
   .GraphicsDataTypeGFDT                        = 0,
   .LLCCacheabilityControlLLCCC                 = 0,
   .L3CacheabilityControlL3CC                   = 1
};

#define GEN8_MOCS {                                     \
      .MemoryTypeLLCeLLCCacheabilityControl = WB,       \
      .TargetCache = L3DefertoPATforLLCeLLCselection,   \
      .AgeforQUADLRU = 0                                \
   }

struct anv_device_memory {
   struct anv_bo                                bo;
   VkDeviceSize                                 map_size;
   void *                                       map;
};

/**
 * Header for Vertex URB Entry (VUE)
 */
struct anv_vue_header {
   uint32_t Reserved;
   uint32_t RTAIndex; /* RenderTargetArrayIndex */
   uint32_t ViewportIndex;
   float PointWidth;
};

struct anv_descriptor_set_binding_layout {
   /* Number of array elements in this binding */
   uint16_t array_size;

   /* Index into the flattend descriptor set */
   uint16_t descriptor_index;

   /* Index into the dynamic state array for a dynamic buffer */
   int16_t dynamic_offset_index;

   struct {
      /* Index into the binding table for the associated surface */
      int16_t surface_index;

      /* Index into the sampler table for the associated sampler */
      int16_t sampler_index;
   } stage[VK_SHADER_STAGE_NUM];

   /* Immutable samplers (or NULL if no immutable samplers) */
   struct anv_sampler **immutable_samplers;
};

struct anv_descriptor_set_layout {
   /* Number of bindings in this descriptor set */
   uint16_t binding_count;

   /* Total size of the descriptor set with room for all array entries */
   uint16_t size;

   /* Shader stages affected by this descriptor set */
   uint16_t shader_stages;

   /* Number of dynamic offsets used by this descriptor set */
   uint16_t dynamic_offset_count;

   /* Bindings in this descriptor set */
   struct anv_descriptor_set_binding_layout binding[0];
};

struct anv_descriptor {
   VkDescriptorType type;

   union {
      struct {
         union {
            struct anv_image_view *image_view;
         };
         struct anv_sampler *sampler;
      };

      struct {
         struct anv_buffer *buffer;
         uint64_t offset;
         uint64_t range;
      };
   };
};

struct anv_descriptor_set {
   const struct anv_descriptor_set_layout *layout;
   struct anv_descriptor descriptors[0];
};

VkResult
anv_descriptor_set_create(struct anv_device *device,
                          const struct anv_descriptor_set_layout *layout,
                          struct anv_descriptor_set **out_set);

void
anv_descriptor_set_destroy(struct anv_device *device,
                           struct anv_descriptor_set *set);

#define MAX_VBS         32
#define MAX_SETS         8
#define MAX_RTS          8
#define MAX_VIEWPORTS   16
#define MAX_SCISSORS    16
#define MAX_PUSH_CONSTANTS_SIZE 128
#define MAX_DYNAMIC_BUFFERS 16
#define MAX_IMAGES 8

struct anv_pipeline_binding {
   /* The descriptor set this surface corresponds to */
   uint16_t set;

   /* Offset into the descriptor set */
   uint16_t offset;
};

struct anv_pipeline_layout {
   struct {
      struct anv_descriptor_set_layout *layout;
      uint32_t dynamic_offset_start;
      struct {
         uint32_t surface_start;
         uint32_t sampler_start;
      } stage[VK_SHADER_STAGE_NUM];
   } set[MAX_SETS];

   uint32_t num_sets;

   struct {
      bool has_dynamic_offsets;
      uint32_t surface_count;
      struct anv_pipeline_binding *surface_to_descriptor;
      uint32_t sampler_count;
      struct anv_pipeline_binding *sampler_to_descriptor;
   } stage[VK_SHADER_STAGE_NUM];

   struct anv_pipeline_binding entries[0];
};

struct anv_buffer {
   struct anv_device *                          device;
   VkDeviceSize                                 size;

   /* Set when bound */
   struct anv_bo *                              bo;
   VkDeviceSize                                 offset;   
};

enum anv_cmd_dirty_bits {
   ANV_CMD_DIRTY_DYNAMIC_VIEWPORT                  = 1 << 0, /* VK_DYNAMIC_STATE_VIEWPORT */
   ANV_CMD_DIRTY_DYNAMIC_SCISSOR                   = 1 << 1, /* VK_DYNAMIC_STATE_SCISSOR */
   ANV_CMD_DIRTY_DYNAMIC_LINE_WIDTH                = 1 << 2, /* VK_DYNAMIC_STATE_LINE_WIDTH */
   ANV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS                = 1 << 3, /* VK_DYNAMIC_STATE_DEPTH_BIAS */
   ANV_CMD_DIRTY_DYNAMIC_BLEND_CONSTANTS           = 1 << 4, /* VK_DYNAMIC_STATE_BLEND_CONSTANTS */
   ANV_CMD_DIRTY_DYNAMIC_DEPTH_BOUNDS              = 1 << 5, /* VK_DYNAMIC_STATE_DEPTH_BOUNDS */
   ANV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK      = 1 << 6, /* VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK */
   ANV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK        = 1 << 7, /* VK_DYNAMIC_STATE_STENCIL_WRITE_MASK */
   ANV_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE         = 1 << 8, /* VK_DYNAMIC_STATE_STENCIL_REFERENCE */
   ANV_CMD_DIRTY_DYNAMIC_ALL                       = (1 << 9) - 1,
   ANV_CMD_DIRTY_PIPELINE                          = 1 << 9,
   ANV_CMD_DIRTY_INDEX_BUFFER                      = 1 << 10,
};
typedef uint32_t anv_cmd_dirty_mask_t;

struct anv_vertex_binding {
   struct anv_buffer *                          buffer;
   VkDeviceSize                                 offset;
};

struct anv_push_constants {
   /* Current allocated size of this push constants data structure.
    * Because a decent chunk of it may not be used (images on SKL, for
    * instance), we won't actually allocate the entire structure up-front.
    */
   uint32_t size;

   /* Push constant data provided by the client through vkPushConstants */
   uint8_t client_data[MAX_PUSH_CONSTANTS_SIZE];

   /* Our hardware only provides zero-based vertex and instance id so, in
    * order to satisfy the vulkan requirements, we may have to push one or
    * both of these into the shader.
    */
   uint32_t base_vertex;
   uint32_t base_instance;

   /* Offsets and ranges for dynamically bound buffers */
   struct {
      uint32_t offset;
      uint32_t range;
   } dynamic[MAX_DYNAMIC_BUFFERS];

   /* Image data for image_load_store on pre-SKL */
   struct brw_image_param images[MAX_IMAGES];
};

struct anv_dynamic_state {
   struct {
      uint32_t                                  count;
      VkViewport                                viewports[MAX_VIEWPORTS];
   } viewport;

   struct {
      uint32_t                                  count;
      VkRect2D                                  scissors[MAX_SCISSORS];
   } scissor;

   float                                        line_width;

   struct {
      float                                     bias;
      float                                     clamp;
      float                                     slope_scaled;
   } depth_bias;

   float                                        blend_constants[4];

   struct {
      float                                     min;
      float                                     max;
   } depth_bounds;

   struct {
      uint32_t                                  front;
      uint32_t                                  back;
   } stencil_compare_mask;

   struct {
      uint32_t                                  front;
      uint32_t                                  back;
   } stencil_write_mask;

   struct {
      uint32_t                                  front;
      uint32_t                                  back;
   } stencil_reference;
};

extern const struct anv_dynamic_state default_dynamic_state;

void anv_dynamic_state_copy(struct anv_dynamic_state *dest,
                            const struct anv_dynamic_state *src,
                            uint32_t copy_mask);

/** State required while building cmd buffer */
struct anv_cmd_state {
   uint32_t                                     current_pipeline;
   uint32_t                                     vb_dirty;
   anv_cmd_dirty_mask_t                         dirty;
   anv_cmd_dirty_mask_t                         compute_dirty;
   VkShaderStageFlags                           descriptors_dirty;
   VkShaderStageFlags                           push_constants_dirty;
   uint32_t                                     scratch_size;
   struct anv_pipeline *                        pipeline;
   struct anv_pipeline *                        compute_pipeline;
   struct anv_framebuffer *                     framebuffer;
   struct anv_render_pass *                     pass;
   struct anv_subpass *                         subpass;
   uint32_t                                     state_vf[GEN8_3DSTATE_VF_length];
   struct anv_vertex_binding                    vertex_bindings[MAX_VBS];
   struct anv_descriptor_set *                  descriptors[MAX_SETS];
   struct anv_push_constants *                  push_constants[VK_SHADER_STAGE_NUM];
   struct anv_dynamic_state                     dynamic;

   struct {
      struct anv_buffer *                       index_buffer;
      uint32_t                                  index_type; /**< 3DSTATE_INDEX_BUFFER.IndexFormat */
      uint32_t                                  index_offset;
   } gen7;
};

struct anv_cmd_pool {
   struct list_head                             cmd_buffers;
};

#define ANV_CMD_BUFFER_BATCH_SIZE 8192

enum anv_cmd_buffer_exec_mode {
   ANV_CMD_BUFFER_EXEC_MODE_PRIMARY,
   ANV_CMD_BUFFER_EXEC_MODE_EMIT,
   ANV_CMD_BUFFER_EXEC_MODE_CHAIN,
   ANV_CMD_BUFFER_EXEC_MODE_COPY_AND_CHAIN,
};

struct anv_cmd_buffer {
   VK_LOADER_DATA                               _loader_data;

   struct anv_device *                          device;

   struct list_head                             pool_link;

   struct anv_batch                             batch;

   /* Fields required for the actual chain of anv_batch_bo's.
    *
    * These fields are initialized by anv_cmd_buffer_init_batch_bo_chain().
    */
   struct list_head                             batch_bos;
   enum anv_cmd_buffer_exec_mode                exec_mode;

   /* A vector of anv_batch_bo pointers for every batch or surface buffer
    * referenced by this command buffer
    *
    * initialized by anv_cmd_buffer_init_batch_bo_chain()
    */
   struct anv_vector                            seen_bbos;

   /* A vector of int32_t's for every block of binding tables.
    *
    * initialized by anv_cmd_buffer_init_batch_bo_chain()
    */
   struct anv_vector                            bt_blocks;
   uint32_t                                     bt_next;
   struct anv_reloc_list                        surface_relocs;

   /* Information needed for execbuf
    *
    * These fields are generated by anv_cmd_buffer_prepare_execbuf().
    */
   struct {
      struct drm_i915_gem_execbuffer2           execbuf;

      struct drm_i915_gem_exec_object2 *        objects;
      uint32_t                                  bo_count;
      struct anv_bo **                          bos;

      /* Allocated length of the 'objects' and 'bos' arrays */
      uint32_t                                  array_length;

      bool                                      need_reloc;
   } execbuf2;

   /* Serial for tracking buffer completion */
   uint32_t                                     serial;

   /* Stream objects for storing temporary data */
   struct anv_state_stream                      surface_state_stream;
   struct anv_state_stream                      dynamic_state_stream;

   VkCmdBufferOptimizeFlags                     opt_flags;
   VkCmdBufferLevel                             level;

   struct anv_cmd_state                         state;
};

VkResult anv_cmd_buffer_init_batch_bo_chain(struct anv_cmd_buffer *cmd_buffer);
void anv_cmd_buffer_fini_batch_bo_chain(struct anv_cmd_buffer *cmd_buffer);
void anv_cmd_buffer_reset_batch_bo_chain(struct anv_cmd_buffer *cmd_buffer);
void anv_cmd_buffer_end_batch_buffer(struct anv_cmd_buffer *cmd_buffer);
void anv_cmd_buffer_add_secondary(struct anv_cmd_buffer *primary,
                                  struct anv_cmd_buffer *secondary);
void anv_cmd_buffer_prepare_execbuf(struct anv_cmd_buffer *cmd_buffer);

VkResult anv_cmd_buffer_emit_binding_table(struct anv_cmd_buffer *cmd_buffer,
                                           unsigned stage, struct anv_state *bt_state);
VkResult anv_cmd_buffer_emit_samplers(struct anv_cmd_buffer *cmd_buffer,
                                      unsigned stage, struct anv_state *state);
void anv_flush_descriptor_sets(struct anv_cmd_buffer *cmd_buffer);

struct anv_state anv_cmd_buffer_emit_dynamic(struct anv_cmd_buffer *cmd_buffer,
                                             uint32_t *a, uint32_t dwords,
                                             uint32_t alignment);
struct anv_state anv_cmd_buffer_merge_dynamic(struct anv_cmd_buffer *cmd_buffer,
                                              uint32_t *a, uint32_t *b,
                                              uint32_t dwords, uint32_t alignment);
void anv_cmd_buffer_begin_subpass(struct anv_cmd_buffer *cmd_buffer,
                                  struct anv_subpass *subpass);

struct anv_address
anv_cmd_buffer_surface_base_address(struct anv_cmd_buffer *cmd_buffer);
struct anv_state
anv_cmd_buffer_alloc_binding_table(struct anv_cmd_buffer *cmd_buffer,
                                   uint32_t entries, uint32_t *state_offset);
struct anv_state
anv_cmd_buffer_alloc_surface_state(struct anv_cmd_buffer *cmd_buffer);
struct anv_state
anv_cmd_buffer_alloc_dynamic_state(struct anv_cmd_buffer *cmd_buffer,
                                   uint32_t size, uint32_t alignment);

VkResult
anv_cmd_buffer_new_binding_table_block(struct anv_cmd_buffer *cmd_buffer);

void anv_cmd_buffer_emit_viewport(struct anv_cmd_buffer *cmd_buffer);
void anv_cmd_buffer_emit_scissor(struct anv_cmd_buffer *cmd_buffer);

void gen7_cmd_buffer_emit_state_base_address(struct anv_cmd_buffer *cmd_buffer);
void gen8_cmd_buffer_emit_state_base_address(struct anv_cmd_buffer *cmd_buffer);

void anv_cmd_buffer_emit_state_base_address(struct anv_cmd_buffer *cmd_buffer);

void gen7_cmd_buffer_begin_subpass(struct anv_cmd_buffer *cmd_buffer,
                                   struct anv_subpass *subpass);

void gen8_cmd_buffer_begin_subpass(struct anv_cmd_buffer *cmd_buffer,
                                   struct anv_subpass *subpass);

void anv_cmd_buffer_begin_subpass(struct anv_cmd_buffer *cmd_buffer,
                                  struct anv_subpass *subpass);

struct anv_state
anv_cmd_buffer_push_constants(struct anv_cmd_buffer *cmd_buffer,
                              VkShaderStage stage);

void anv_cmd_buffer_clear_attachments(struct anv_cmd_buffer *cmd_buffer,
                                      struct anv_render_pass *pass,
                                      const VkClearValue *clear_values);
const struct anv_image_view *
anv_cmd_buffer_get_depth_stencil_view(const struct anv_cmd_buffer *cmd_buffer);

void anv_cmd_buffer_dump(struct anv_cmd_buffer *cmd_buffer);

struct anv_fence {
   struct anv_bo bo;
   struct drm_i915_gem_execbuffer2 execbuf;
   struct drm_i915_gem_exec_object2 exec2_objects[1];
   bool ready;
};

struct nir_shader;

struct anv_shader_module {
   struct nir_shader *                          nir;

   uint32_t                                     size;
   char                                         data[0];
};

struct anv_shader {
   struct anv_shader_module *                   module;
   char                                         entrypoint[0];
};

struct anv_pipeline {
   struct anv_device *                          device;
   struct anv_batch                             batch;
   uint32_t                                     batch_data[512];
   struct anv_reloc_list                        batch_relocs;
   uint32_t                                     dynamic_state_mask;
   struct anv_dynamic_state                     dynamic_state;

   struct anv_pipeline_layout *                 layout;
   bool                                         use_repclear;

   struct brw_vs_prog_data                      vs_prog_data;
   struct brw_wm_prog_data                      wm_prog_data;
   struct brw_gs_prog_data                      gs_prog_data;
   struct brw_cs_prog_data                      cs_prog_data;
   bool                                         writes_point_size;
   struct brw_stage_prog_data *                 prog_data[VK_SHADER_STAGE_NUM];
   uint32_t                                     scratch_start[VK_SHADER_STAGE_NUM];
   uint32_t                                     total_scratch;
   struct {
      uint32_t                                  vs_start;
      uint32_t                                  vs_size;
      uint32_t                                  nr_vs_entries;
      uint32_t                                  gs_start;
      uint32_t                                  gs_size;
      uint32_t                                  nr_gs_entries;
   } urb;

   VkShaderStageFlags                           active_stages;
   struct anv_state_stream                      program_stream;
   struct anv_state                             blend_state;
   uint32_t                                     vs_simd8;
   uint32_t                                     vs_vec4;
   uint32_t                                     ps_simd8;
   uint32_t                                     ps_simd16;
   uint32_t                                     ps_ksp0;
   uint32_t                                     ps_ksp2;
   uint32_t                                     ps_grf_start0;
   uint32_t                                     ps_grf_start2;
   uint32_t                                     gs_vec4;
   uint32_t                                     gs_vertex_count;
   uint32_t                                     cs_simd;

   uint32_t                                     vb_used;
   uint32_t                                     binding_stride[MAX_VBS];
   bool                                         instancing_enable[MAX_VBS];
   bool                                         primitive_restart;
   uint32_t                                     topology;

   uint32_t                                     cs_thread_width_max;
   uint32_t                                     cs_right_mask;

   struct {
      uint32_t                                  sf[GEN7_3DSTATE_SF_length];
      uint32_t                                  depth_stencil_state[GEN7_DEPTH_STENCIL_STATE_length];
   } gen7;

   struct {
      uint32_t                                  sf[GEN8_3DSTATE_SF_length];
      uint32_t                                  vf[GEN8_3DSTATE_VF_length];
      uint32_t                                  raster[GEN8_3DSTATE_RASTER_length];
      uint32_t                                  wm_depth_stencil[GEN8_3DSTATE_WM_DEPTH_STENCIL_length];
   } gen8;
};

struct anv_graphics_pipeline_create_info {
   bool                                         use_repclear;
   bool                                         disable_viewport;
   bool                                         disable_scissor;
   bool                                         disable_vs;
   bool                                         use_rectlist;
};

VkResult
anv_pipeline_init(struct anv_pipeline *pipeline, struct anv_device *device,
                  const VkGraphicsPipelineCreateInfo *pCreateInfo,
                  const struct anv_graphics_pipeline_create_info *extra);

VkResult
anv_pipeline_compile_cs(struct anv_pipeline *pipeline,
                        const VkComputePipelineCreateInfo *info,
                        struct anv_shader *shader);

VkResult
anv_graphics_pipeline_create(VkDevice device,
                             const VkGraphicsPipelineCreateInfo *pCreateInfo,
                             const struct anv_graphics_pipeline_create_info *extra,
                             VkPipeline *pPipeline);

VkResult
gen7_graphics_pipeline_create(VkDevice _device,
                              const VkGraphicsPipelineCreateInfo *pCreateInfo,
                              const struct anv_graphics_pipeline_create_info *extra,
                              VkPipeline *pPipeline);

VkResult
gen8_graphics_pipeline_create(VkDevice _device,
                              const VkGraphicsPipelineCreateInfo *pCreateInfo,
                              const struct anv_graphics_pipeline_create_info *extra,
                              VkPipeline *pPipeline);
VkResult
gen7_compute_pipeline_create(VkDevice _device,
                             const VkComputePipelineCreateInfo *pCreateInfo,
                             VkPipeline *pPipeline);

VkResult
gen8_compute_pipeline_create(VkDevice _device,
                             const VkComputePipelineCreateInfo *pCreateInfo,
                             VkPipeline *pPipeline);

struct anv_format {
   const VkFormat vk_format;
   const char *name;
   uint16_t surface_format; /**< RENDER_SURFACE_STATE.SurfaceFormat */
   uint8_t bs; /**< Block size (in bytes) of anv_format::surface_format. */
   uint8_t bw; /**< Block width of anv_format::surface_format. */
   uint8_t bh; /**< Block height of anv_format::surface_format. */
   uint8_t bd; /**< Block depth of anv_format::surface_format. */
   uint8_t num_channels;
   uint16_t depth_format; /**< 3DSTATE_DEPTH_BUFFER.SurfaceFormat */
   bool has_stencil;
};

/**
 * Stencil formats are often a special case. To reduce the number of lookups
 * into the VkFormat-to-anv_format translation table when working with
 * stencil, here is the handle to the table's entry for VK_FORMAT_S8_UINT.
 */
extern const struct anv_format *const anv_format_s8_uint;

const struct anv_format *
anv_format_for_vk_format(VkFormat format);

static inline bool
anv_format_is_color(const struct anv_format *format)
{
   return !format->depth_format && !format->has_stencil;
}

static inline bool
anv_format_is_depth_or_stencil(const struct anv_format *format)
{
   return format->depth_format || format->has_stencil;
}

struct anv_image_view_info {
   uint8_t surface_type; /**< RENDER_SURFACE_STATE.SurfaceType */
   bool is_array:1; /**< RENDER_SURFACE_STATE.SurfaceArray */
   bool is_cube:1; /**< RENDER_SURFACE_STATE.CubeFaceEnable* */
};

struct anv_image_view_info
anv_image_view_info_for_vk_image_view_type(VkImageViewType type);

/**
 * A proxy for the color surfaces, depth surfaces, and stencil surfaces.
 */
struct anv_surface {
   /**
    * Offset from VkImage's base address, as bound by vkBindImageMemory().
    */
   uint32_t offset;

   uint32_t stride; /**< RENDER_SURFACE_STATE.SurfacePitch */
   uint16_t qpitch; /**< RENDER_SURFACE_STATE.QPitch */

   /**
    * \name Alignment of miptree images, in units of pixels.
    *
    * These fields contain the real alignment values, not the values to be
    * given to the GPU.  For example, if h_align is 4, then program the GPU
    * with HALIGN_4.
    * \{
    */
   uint8_t h_align; /**< RENDER_SURFACE_STATE.SurfaceHorizontalAlignment */
   uint8_t v_align; /**< RENDER_SURFACE_STATE.SurfaceVerticalAlignment */
   /** \} */

   uint8_t tile_mode; /**< RENDER_SURFACE_STATE.TileMode */
};

struct anv_image {
   VkImageType type;
   const struct anv_format *format;
   VkExtent3D extent;
   uint32_t levels;
   uint32_t array_size;
   VkImageUsageFlags usage; /**< Superset of VkImageCreateInfo::usage. */

   VkDeviceSize size;
   uint32_t alignment;

   /* Set when bound */
   struct anv_bo *bo;
   VkDeviceSize offset;

   uint8_t surface_type; /**< RENDER_SURFACE_STATE.SurfaceType */

   bool needs_nonrt_surface_state:1;
   bool needs_color_rt_surface_state:1;

   /**
    * Image subsurfaces
    *
    * For each foo, anv_image::foo_surface is valid if and only if
    * anv_image::format has a foo aspect.
    *
    * The hardware requires that the depth buffer and stencil buffer be
    * separate surfaces.  From Vulkan's perspective, though, depth and stencil
    * reside in the same VkImage.  To satisfy both the hardware and Vulkan, we
    * allocate the depth and stencil buffers as separate surfaces in the same
    * bo.
    */
   union {
      struct anv_surface color_surface;

      struct {
         struct anv_surface depth_surface;
         struct anv_surface stencil_surface;
      };
   };
};

struct anv_image_view {
   const struct anv_image *image; /**< VkImageViewCreateInfo::image */
   const struct anv_format *format; /**< VkImageViewCreateInfo::format */
   struct anv_bo *bo;
   uint32_t offset; /**< Offset into bo. */
   VkExtent3D extent; /**< Extent of VkImageViewCreateInfo::baseMipLevel. */

   /** RENDER_SURFACE_STATE when using image as a color render target. */
   struct anv_state color_rt_surface_state;

   /** RENDER_SURFACE_STATE when using image as a non render target. */
   struct anv_state nonrt_surface_state;
};

struct anv_image_create_info {
   const VkImageCreateInfo *vk_info;
   bool force_tile_mode;
   uint8_t tile_mode;
   uint32_t stride;
};

VkResult anv_image_create(VkDevice _device,
                          const struct anv_image_create_info *info,
                          VkImage *pImage);

struct anv_surface *
anv_image_get_surface_for_aspect_mask(struct anv_image *image,
                                      VkImageAspectFlags aspect_mask);

void anv_image_view_init(struct anv_image_view *view,
                         struct anv_device *device,
                         const VkImageViewCreateInfo* pCreateInfo,
                         struct anv_cmd_buffer *cmd_buffer);

void
gen7_image_view_init(struct anv_image_view *iview,
                     struct anv_device *device,
                     const VkImageViewCreateInfo* pCreateInfo,
                     struct anv_cmd_buffer *cmd_buffer);

void
gen8_image_view_init(struct anv_image_view *iview,
                     struct anv_device *device,
                     const VkImageViewCreateInfo* pCreateInfo,
                     struct anv_cmd_buffer *cmd_buffer);

void anv_fill_buffer_surface_state(struct anv_device *device, void *state,
                                   const struct anv_format *format,
                                   uint32_t offset, uint32_t range,
                                   uint32_t stride);

void gen7_fill_buffer_surface_state(void *state, const struct anv_format *format,
                                    uint32_t offset, uint32_t range,
                                    uint32_t stride);
void gen8_fill_buffer_surface_state(void *state, const struct anv_format *format,
                                    uint32_t offset, uint32_t range,
                                    uint32_t stride);

struct anv_sampler {
   uint32_t state[4];
};

struct anv_framebuffer {
   uint32_t                                     width;
   uint32_t                                     height;
   uint32_t                                     layers;

   uint32_t                                     attachment_count;
   const struct anv_image_view *           attachments[0];
};

struct anv_subpass {
   uint32_t                                     input_count;
   uint32_t *                                   input_attachments;
   uint32_t                                     color_count;
   uint32_t *                                   color_attachments;
   uint32_t *                                   resolve_attachments;
   uint32_t                                     depth_stencil_attachment;
};

struct anv_render_pass_attachment {
   const struct anv_format                      *format;
   uint32_t                                     samples;
   VkAttachmentLoadOp                           load_op;
   VkAttachmentLoadOp                           stencil_load_op;
};

struct anv_render_pass {
   uint32_t                                     attachment_count;
   uint32_t                                     subpass_count;
   struct anv_render_pass_attachment *          attachments;
   struct anv_subpass                           subpasses[0];
};

extern struct anv_render_pass anv_meta_dummy_renderpass;

struct anv_query_pool_slot {
   uint64_t begin;
   uint64_t end;
   uint64_t available;
};

struct anv_query_pool {
   VkQueryType                                  type;
   uint32_t                                     slots;
   struct anv_bo                                bo;
};

void anv_device_init_meta(struct anv_device *device);
void anv_device_finish_meta(struct anv_device *device);

void *anv_lookup_entrypoint(const char *name);

void anv_dump_image_to_ppm(struct anv_device *device,
                           struct anv_image *image, unsigned miplevel,
                           unsigned array_layer, const char *filename);

#define ANV_DEFINE_HANDLE_CASTS(__anv_type, __VkType)                      \
                                                                           \
   static inline struct __anv_type *                                       \
   __anv_type ## _from_handle(__VkType _handle)                            \
   {                                                                       \
      return (struct __anv_type *) _handle;                                \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __anv_type ## _to_handle(struct __anv_type *_obj)                       \
   {                                                                       \
      return (__VkType) _obj;                                              \
   }

#define ANV_DEFINE_NONDISP_HANDLE_CASTS(__anv_type, __VkType)              \
                                                                           \
   static inline struct __anv_type *                                       \
   __anv_type ## _from_handle(__VkType _handle)                            \
   {                                                                       \
      return (struct __anv_type *) _handle.handle;                         \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __anv_type ## _to_handle(struct __anv_type *_obj)                       \
   {                                                                       \
      return (__VkType) { .handle = (uint64_t) _obj };                     \
   }

#define ANV_FROM_HANDLE(__anv_type, __name, __handle) \
   struct __anv_type *__name = __anv_type ## _from_handle(__handle)

ANV_DEFINE_HANDLE_CASTS(anv_cmd_buffer, VkCmdBuffer)
ANV_DEFINE_HANDLE_CASTS(anv_device, VkDevice)
ANV_DEFINE_HANDLE_CASTS(anv_instance, VkInstance)
ANV_DEFINE_HANDLE_CASTS(anv_physical_device, VkPhysicalDevice)
ANV_DEFINE_HANDLE_CASTS(anv_queue, VkQueue)

ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_cmd_pool, VkCmdPool)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_buffer, VkBuffer)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_descriptor_set, VkDescriptorSet)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_descriptor_set_layout, VkDescriptorSetLayout)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_device_memory, VkDeviceMemory)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_fence, VkFence)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_framebuffer, VkFramebuffer)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_image, VkImage)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_image_view, VkImageView);
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_pipeline, VkPipeline)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_pipeline_layout, VkPipelineLayout)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_query_pool, VkQueryPool)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_render_pass, VkRenderPass)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_sampler, VkSampler)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_shader, VkShader)
ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_shader_module, VkShaderModule)

#define ANV_DEFINE_STRUCT_CASTS(__anv_type, __VkType) \
   \
   static inline const __VkType * \
   __anv_type ## _to_ ## __VkType(const struct __anv_type *__anv_obj) \
   { \
      return (const __VkType *) __anv_obj; \
   }

#define ANV_COMMON_TO_STRUCT(__VkType, __vk_name, __common_name) \
   const __VkType *__vk_name = anv_common_to_ ## __VkType(__common_name)

ANV_DEFINE_STRUCT_CASTS(anv_common, VkMemoryBarrier)
ANV_DEFINE_STRUCT_CASTS(anv_common, VkBufferMemoryBarrier)
ANV_DEFINE_STRUCT_CASTS(anv_common, VkImageMemoryBarrier)

#ifdef __cplusplus
}
#endif
