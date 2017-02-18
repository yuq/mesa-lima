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

#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"

VkResult genX(CreateQueryPool)(
    VkDevice                                    _device,
    const VkQueryPoolCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkQueryPool*                                pQueryPool)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_query_pool *pool;
   VkResult result;
   uint32_t slot_size;
   uint64_t size;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);

   switch (pCreateInfo->queryType) {
   case VK_QUERY_TYPE_OCCLUSION:
   case VK_QUERY_TYPE_TIMESTAMP:
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   default:
      assert(!"Invalid query type");
   }

   slot_size = sizeof(struct anv_query_pool_slot);
   pool = vk_alloc2(&device->alloc, pAllocator, sizeof(*pool), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pool->type = pCreateInfo->queryType;
   pool->slots = pCreateInfo->queryCount;

   size = pCreateInfo->queryCount * slot_size;
   result = anv_bo_init_new(&pool->bo, device, size);
   if (result != VK_SUCCESS)
      goto fail;

   pool->bo.map = anv_gem_mmap(device, pool->bo.gem_handle, 0, size, 0);

   *pQueryPool = anv_query_pool_to_handle(pool);

   return VK_SUCCESS;

 fail:
   vk_free2(&device->alloc, pAllocator, pool);

   return result;
}

void genX(DestroyQueryPool)(
    VkDevice                                    _device,
    VkQueryPool                                 _pool,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_query_pool, pool, _pool);

   if (!pool)
      return;

   anv_gem_munmap(pool->bo.map, pool->bo.size);
   anv_gem_close(device, pool->bo.gem_handle);
   vk_free2(&device->alloc, pAllocator, pool);
}

VkResult genX(GetQueryPoolResults)(
    VkDevice                                    _device,
    VkQueryPool                                 queryPool,
    uint32_t                                    firstQuery,
    uint32_t                                    queryCount,
    size_t                                      dataSize,
    void*                                       pData,
    VkDeviceSize                                stride,
    VkQueryResultFlags                          flags)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_query_pool, pool, queryPool);
   int64_t timeout = INT64_MAX;
   uint64_t result;
   int ret;

   assert(pool->type == VK_QUERY_TYPE_OCCLUSION ||
          pool->type == VK_QUERY_TYPE_TIMESTAMP);

   if (pData == NULL)
      return VK_SUCCESS;

   if (flags & VK_QUERY_RESULT_WAIT_BIT) {
      ret = anv_gem_wait(device, pool->bo.gem_handle, &timeout);
      if (ret == -1) {
         /* We don't know the real error. */
         return vk_errorf(VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "gem_wait failed %m");
      }
   }

   void *data_end = pData + dataSize;
   struct anv_query_pool_slot *slot = pool->bo.map;

   if (!device->info.has_llc)
      anv_invalidate_range(slot, MIN2(queryCount * sizeof(*slot), pool->bo.size));

   for (uint32_t i = 0; i < queryCount; i++) {
      switch (pool->type) {
      case VK_QUERY_TYPE_OCCLUSION: {
         result = slot[firstQuery + i].end - slot[firstQuery + i].begin;
         break;
      }
      case VK_QUERY_TYPE_PIPELINE_STATISTICS:
         unreachable("pipeline stats not supported");
      case VK_QUERY_TYPE_TIMESTAMP: {
         result = slot[firstQuery + i].begin;
         break;
      }
      default:
         unreachable("invalid pool type");
      }

      if (flags & VK_QUERY_RESULT_64_BIT) {
         uint64_t *dst = pData;
         dst[0] = result;
         if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
            dst[1] = slot[firstQuery + i].available;
      } else {
         uint32_t *dst = pData;
         if (result > UINT32_MAX)
            result = UINT32_MAX;
         dst[0] = result;
         if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
            dst[1] = slot[firstQuery + i].available;
      }

      pData += stride;
      if (pData >= data_end)
         break;
   }

   return VK_SUCCESS;
}

static void
emit_ps_depth_count(struct anv_cmd_buffer *cmd_buffer,
                    struct anv_bo *bo, uint32_t offset)
{
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.DestinationAddressType  = DAT_PPGTT;
      pc.PostSyncOperation       = WritePSDepthCount;
      pc.DepthStallEnable        = true;
      pc.Address                 = (struct anv_address) { bo, offset };

      if (GEN_GEN == 9 && cmd_buffer->device->info.gt == 4)
         pc.CommandStreamerStallEnable = true;
   }
}

static void
emit_query_availability(struct anv_cmd_buffer *cmd_buffer,
                        struct anv_bo *bo, uint32_t offset)
{
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.DestinationAddressType  = DAT_PPGTT;
      pc.PostSyncOperation       = WriteImmediateData;
      pc.Address                 = (struct anv_address) { bo, offset };
      pc.ImmediateData           = 1;
   }
}

void genX(CmdResetQueryPool)(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    firstQuery,
    uint32_t                                    queryCount)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_query_pool, pool, queryPool);

   for (uint32_t i = 0; i < queryCount; i++) {
      switch (pool->type) {
      case VK_QUERY_TYPE_OCCLUSION:
      case VK_QUERY_TYPE_TIMESTAMP: {
         anv_batch_emit(&cmd_buffer->batch, GENX(MI_STORE_DATA_IMM), sdm) {
            sdm.Address = (struct anv_address) {
               .bo = &pool->bo,
               .offset = (firstQuery + i) * sizeof(struct anv_query_pool_slot) +
                         offsetof(struct anv_query_pool_slot, available),
            };
            sdm.DataDWord0 = 0;
            sdm.DataDWord1 = 0;
         }
         break;
      }
      default:
         assert(!"Invalid query type");
      }
   }
}

void genX(CmdBeginQuery)(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    query,
    VkQueryControlFlags                         flags)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_query_pool, pool, queryPool);

   /* Workaround: When meta uses the pipeline with the VS disabled, it seems
    * that the pipelining of the depth write breaks. What we see is that
    * samples from the render pass clear leaks into the first query
    * immediately after the clear. Doing a pipecontrol with a post-sync
    * operation and DepthStallEnable seems to work around the issue.
    */
   if (cmd_buffer->state.need_query_wa) {
      cmd_buffer->state.need_query_wa = false;
      anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
         pc.DepthCacheFlushEnable   = true;
         pc.DepthStallEnable        = true;
      }
   }

   switch (pool->type) {
   case VK_QUERY_TYPE_OCCLUSION:
      emit_ps_depth_count(cmd_buffer, &pool->bo,
                          query * sizeof(struct anv_query_pool_slot));
      break;

   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
   default:
      unreachable("");
   }
}

void genX(CmdEndQuery)(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    query)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_query_pool, pool, queryPool);

   switch (pool->type) {
   case VK_QUERY_TYPE_OCCLUSION:
      emit_ps_depth_count(cmd_buffer, &pool->bo,
                          query * sizeof(struct anv_query_pool_slot) + 8);

      emit_query_availability(cmd_buffer, &pool->bo,
                              query * sizeof(struct anv_query_pool_slot) + 16);
      break;

   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
   default:
      unreachable("");
   }
}

#define TIMESTAMP 0x2358

void genX(CmdWriteTimestamp)(
    VkCommandBuffer                             commandBuffer,
    VkPipelineStageFlagBits                     pipelineStage,
    VkQueryPool                                 queryPool,
    uint32_t                                    query)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_query_pool, pool, queryPool);
   uint32_t offset = query * sizeof(struct anv_query_pool_slot);

   assert(pool->type == VK_QUERY_TYPE_TIMESTAMP);

   switch (pipelineStage) {
   case VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_STORE_REGISTER_MEM), srm) {
         srm.RegisterAddress  = TIMESTAMP;
         srm.MemoryAddress    = (struct anv_address) { &pool->bo, offset };
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_STORE_REGISTER_MEM), srm) {
         srm.RegisterAddress  = TIMESTAMP + 4;
         srm.MemoryAddress    = (struct anv_address) { &pool->bo, offset + 4 };
      }
      break;

   default:
      /* Everything else is bottom-of-pipe */
      anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
         pc.DestinationAddressType  = DAT_PPGTT;
         pc.PostSyncOperation       = WriteTimestamp;
         pc.Address = (struct anv_address) { &pool->bo, offset };

         if (GEN_GEN == 9 && cmd_buffer->device->info.gt == 4)
            pc.CommandStreamerStallEnable = true;
      }
      break;
   }

   emit_query_availability(cmd_buffer, &pool->bo, query + 16);
}

#if GEN_GEN > 7 || GEN_IS_HASWELL

#define alu_opcode(v)   __gen_uint((v),  20, 31)
#define alu_operand1(v) __gen_uint((v),  10, 19)
#define alu_operand2(v) __gen_uint((v),   0,  9)
#define alu(opcode, operand1, operand2) \
   alu_opcode(opcode) | alu_operand1(operand1) | alu_operand2(operand2)

#define OPCODE_NOOP      0x000
#define OPCODE_LOAD      0x080
#define OPCODE_LOADINV   0x480
#define OPCODE_LOAD0     0x081
#define OPCODE_LOAD1     0x481
#define OPCODE_ADD       0x100
#define OPCODE_SUB       0x101
#define OPCODE_AND       0x102
#define OPCODE_OR        0x103
#define OPCODE_XOR       0x104
#define OPCODE_STORE     0x180
#define OPCODE_STOREINV  0x580

#define OPERAND_R0   0x00
#define OPERAND_R1   0x01
#define OPERAND_R2   0x02
#define OPERAND_R3   0x03
#define OPERAND_R4   0x04
#define OPERAND_SRCA 0x20
#define OPERAND_SRCB 0x21
#define OPERAND_ACCU 0x31
#define OPERAND_ZF   0x32
#define OPERAND_CF   0x33

#define CS_GPR(n) (0x2600 + (n) * 8)

static void
emit_load_alu_reg_u64(struct anv_batch *batch, uint32_t reg,
                      struct anv_bo *bo, uint32_t offset)
{
   anv_batch_emit(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
      lrm.RegisterAddress  = reg,
      lrm.MemoryAddress    = (struct anv_address) { bo, offset };
   }
   anv_batch_emit(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
      lrm.RegisterAddress  = reg + 4;
      lrm.MemoryAddress    = (struct anv_address) { bo, offset + 4 };
   }
}

static void
store_query_result(struct anv_batch *batch, uint32_t reg,
                   struct anv_bo *bo, uint32_t offset, VkQueryResultFlags flags)
{
   anv_batch_emit(batch, GENX(MI_STORE_REGISTER_MEM), srm) {
      srm.RegisterAddress  = reg;
      srm.MemoryAddress    = (struct anv_address) { bo, offset };
   }

   if (flags & VK_QUERY_RESULT_64_BIT) {
      anv_batch_emit(batch, GENX(MI_STORE_REGISTER_MEM), srm) {
         srm.RegisterAddress  = reg + 4;
         srm.MemoryAddress    = (struct anv_address) { bo, offset + 4 };
      }
   }
}

void genX(CmdCopyQueryPoolResults)(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    firstQuery,
    uint32_t                                    queryCount,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                destStride,
    VkQueryResultFlags                          flags)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_query_pool, pool, queryPool);
   ANV_FROM_HANDLE(anv_buffer, buffer, destBuffer);
   uint32_t slot_offset, dst_offset;

   if (flags & VK_QUERY_RESULT_WAIT_BIT) {
      anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
         pc.CommandStreamerStallEnable = true;
         pc.StallAtPixelScoreboard     = true;
      }
   }

   dst_offset = buffer->offset + destOffset;
   for (uint32_t i = 0; i < queryCount; i++) {

      slot_offset = (firstQuery + i) * sizeof(struct anv_query_pool_slot);
      switch (pool->type) {
      case VK_QUERY_TYPE_OCCLUSION:
         emit_load_alu_reg_u64(&cmd_buffer->batch,
                               CS_GPR(0), &pool->bo, slot_offset);
         emit_load_alu_reg_u64(&cmd_buffer->batch,
                               CS_GPR(1), &pool->bo, slot_offset + 8);

         /* FIXME: We need to clamp the result for 32 bit. */

         uint32_t *dw = anv_batch_emitn(&cmd_buffer->batch, 5, GENX(MI_MATH));
         dw[1] = alu(OPCODE_LOAD, OPERAND_SRCA, OPERAND_R1);
         dw[2] = alu(OPCODE_LOAD, OPERAND_SRCB, OPERAND_R0);
         dw[3] = alu(OPCODE_SUB, 0, 0);
         dw[4] = alu(OPCODE_STORE, OPERAND_R2, OPERAND_ACCU);
         break;

      case VK_QUERY_TYPE_TIMESTAMP:
         emit_load_alu_reg_u64(&cmd_buffer->batch,
                               CS_GPR(2), &pool->bo, slot_offset);
         break;

      default:
         unreachable("unhandled query type");
      }

      store_query_result(&cmd_buffer->batch,
                         CS_GPR(2), buffer->bo, dst_offset, flags);

      if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
         emit_load_alu_reg_u64(&cmd_buffer->batch, CS_GPR(0),
                               &pool->bo, slot_offset + 16);
         if (flags & VK_QUERY_RESULT_64_BIT)
            store_query_result(&cmd_buffer->batch,
                               CS_GPR(0), buffer->bo, dst_offset + 8, flags);
         else
            store_query_result(&cmd_buffer->batch,
                               CS_GPR(0), buffer->bo, dst_offset + 4, flags);
      }

      dst_offset += destStride;
   }
}

#else
void genX(CmdCopyQueryPoolResults)(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    firstQuery,
    uint32_t                                    queryCount,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                destStride,
    VkQueryResultFlags                          flags)
{
   anv_finishme("Queries not yet supported on Ivy Bridge");
}
#endif
