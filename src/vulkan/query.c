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

#include "private.h"

struct anv_query_pool_slot {
   uint64_t begin;
   uint64_t end;
   uint64_t available;
};

struct anv_query_pool {
   struct anv_object                            base;
   VkQueryType                                  type;
   uint32_t                                     slots;
   struct anv_bo                                bo;
};

static void
anv_query_pool_destroy(struct anv_device *device,
                       struct anv_object *object,
                       VkObjectType obj_type)
{
   struct anv_query_pool *pool = (struct anv_query_pool *) object;

   assert(obj_type == VK_OBJECT_TYPE_QUERY_POOL);

   anv_gem_munmap(pool->bo.map, pool->bo.size);
   anv_gem_close(device, pool->bo.gem_handle);
   anv_device_free(device, pool);
}

VkResult anv_CreateQueryPool(
    VkDevice                                    _device,
    const VkQueryPoolCreateInfo*                pCreateInfo,
    VkQueryPool*                                pQueryPool)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_query_pool *pool;
   VkResult result;
   size_t size;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
   
   switch (pCreateInfo->queryType) {
   case VK_QUERY_TYPE_OCCLUSION:
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      return VK_UNSUPPORTED;
   default:
      unreachable("");
   }

   pool = anv_device_alloc(device, sizeof(*pool), 8,
                            VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pool == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pool->base.destructor = anv_query_pool_destroy;

   pool->type = pCreateInfo->queryType;
   size = pCreateInfo->slots * sizeof(struct anv_query_pool_slot);
   result = anv_bo_init_new(&pool->bo, device, size);
   if (result != VK_SUCCESS)
      goto fail;

   pool->bo.map = anv_gem_mmap(device, pool->bo.gem_handle, 0, size);

   *pQueryPool = (VkQueryPool) pool;

   return VK_SUCCESS;

 fail:
   anv_device_free(device, pool);

   return result;
}

VkResult anv_GetQueryPoolResults(
    VkDevice                                    _device,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount,
    size_t*                                     pDataSize,
    void*                                       pData,
    VkQueryResultFlags                          flags)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_query_pool *pool = (struct anv_query_pool *) queryPool;
   struct anv_query_pool_slot *slot = pool->bo.map;
   int64_t timeout = INT64_MAX;
   uint32_t *dst32 = pData;
   uint64_t *dst64 = pData;
   uint64_t result;
   int ret;

   if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
      /* Where is the availabilty info supposed to go? */
      anv_finishme("VK_QUERY_RESULT_WITH_AVAILABILITY_BIT");
      return VK_UNSUPPORTED;
   }

   assert(pool->type == VK_QUERY_TYPE_OCCLUSION);

   if (flags & VK_QUERY_RESULT_64_BIT)
      *pDataSize = queryCount * sizeof(uint64_t);
   else
      *pDataSize = queryCount * sizeof(uint32_t);

   if (pData == NULL)
      return VK_SUCCESS;

   if (flags & VK_QUERY_RESULT_WAIT_BIT) {
      ret = anv_gem_wait(device, pool->bo.gem_handle, &timeout);
      if (ret == -1)
         return vk_error(VK_ERROR_UNKNOWN);
   }

   for (uint32_t i = 0; i < queryCount; i++) {
      result = slot[startQuery + i].end - slot[startQuery + i].begin;
      if (flags & VK_QUERY_RESULT_64_BIT) {
         *dst64++ = result;
      } else {
         if (result > UINT32_MAX)
            result = UINT32_MAX;
         *dst32++ = result;
      }
   }

   return VK_SUCCESS;
}

static void
anv_batch_emit_ps_depth_count(struct anv_batch *batch,
                              struct anv_bo *bo, uint32_t offset)
{
   anv_batch_emit(batch, GEN8_PIPE_CONTROL,
                  .DestinationAddressType = DAT_PPGTT,
                  .PostSyncOperation = WritePSDepthCount,
                  .Address = { bo, offset });  /* FIXME: This is only lower 32 bits */
}

void anv_CmdBeginQuery(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    slot,
    VkQueryControlFlags                         flags)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_query_pool *pool = (struct anv_query_pool *) queryPool;

   switch (pool->type) {
   case VK_QUERY_TYPE_OCCLUSION:
      anv_batch_emit_ps_depth_count(&cmd_buffer->batch, &pool->bo,
                                    slot * sizeof(struct anv_query_pool_slot));
      break;

   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
   default:
      unreachable("");
   }
}

void anv_CmdEndQuery(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    slot)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_query_pool *pool = (struct anv_query_pool *) queryPool;

   switch (pool->type) {
   case VK_QUERY_TYPE_OCCLUSION:
      anv_batch_emit_ps_depth_count(&cmd_buffer->batch, &pool->bo,
                                    slot * sizeof(struct anv_query_pool_slot) + 8);
      break;

   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
   default:
      unreachable("");
   }
}

void anv_CmdResetQueryPool(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount)
{
   stub();
}

#define TIMESTAMP 0x2358

void anv_CmdWriteTimestamp(
    VkCmdBuffer                                 cmdBuffer,
    VkTimestampType                             timestampType,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_buffer *buffer = (struct anv_buffer *) destBuffer;
   struct anv_bo *bo = buffer->bo;

   switch (timestampType) {
   case VK_TIMESTAMP_TYPE_TOP:
      anv_batch_emit(&cmd_buffer->batch, GEN8_MI_STORE_REGISTER_MEM,
                     .RegisterAddress = TIMESTAMP,
                     .MemoryAddress = { bo, buffer->offset + destOffset });
      anv_batch_emit(&cmd_buffer->batch, GEN8_MI_STORE_REGISTER_MEM,
                     .RegisterAddress = TIMESTAMP + 4,
                     .MemoryAddress = { bo, buffer->offset + destOffset + 4 });
      break;

   case VK_TIMESTAMP_TYPE_BOTTOM:
      anv_batch_emit(&cmd_buffer->batch, GEN8_PIPE_CONTROL,
                     .DestinationAddressType = DAT_PPGTT,
                     .PostSyncOperation = WriteTimestamp,
                     .Address = /* FIXME: This is only lower 32 bits */
                        { bo, buffer->offset + destOffset });
      break;

   default:
      break;
   }
}

#define alu_opcode(v)   __gen_field((v),  20, 31)
#define alu_operand1(v) __gen_field((v),  10, 19)
#define alu_operand2(v) __gen_field((v),   0,  9)
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
   anv_batch_emit(batch, GEN8_MI_LOAD_REGISTER_MEM,
                  .RegisterAddress = reg,
                  .MemoryAddress = { bo, offset });
   anv_batch_emit(batch, GEN8_MI_LOAD_REGISTER_MEM,
                  .RegisterAddress = reg + 4,
                  .MemoryAddress = { bo, offset + 4 });
}

void anv_CmdCopyQueryPoolResults(
    VkCmdBuffer                                 cmdBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                destStride,
    VkQueryResultFlags                          flags)
{
   struct anv_cmd_buffer *cmd_buffer = (struct anv_cmd_buffer *) cmdBuffer;
   struct anv_query_pool *pool = (struct anv_query_pool *) queryPool;
   struct anv_buffer *buffer = (struct anv_buffer *) destBuffer;
   uint32_t slot_offset, dst_offset;

   if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
      /* Where is the availabilty info supposed to go? */
      anv_finishme("VK_QUERY_RESULT_WITH_AVAILABILITY_BIT");
      return;
   }

   assert(pool->type == VK_QUERY_TYPE_OCCLUSION);

   /* FIXME: If we're not waiting, should we just do this on the CPU? */
   if (flags & VK_QUERY_RESULT_WAIT_BIT)
      anv_batch_emit(&cmd_buffer->batch, GEN8_PIPE_CONTROL,
                     .CommandStreamerStallEnable = true,
                     .StallAtPixelScoreboard = true);

   dst_offset = buffer->offset + destOffset;
   for (uint32_t i = 0; i < queryCount; i++) {

      slot_offset = (startQuery + i) * sizeof(struct anv_query_pool_slot);

      emit_load_alu_reg_u64(&cmd_buffer->batch, CS_GPR(0), &pool->bo, slot_offset);
      emit_load_alu_reg_u64(&cmd_buffer->batch, CS_GPR(1), &pool->bo, slot_offset + 8);

      /* FIXME: We need to clamp the result for 32 bit. */

      uint32_t *dw = anv_batch_emitn(&cmd_buffer->batch, 5, GEN8_MI_MATH);
      dw[1] = alu(OPCODE_LOAD, OPERAND_SRCA, OPERAND_R1);
      dw[2] = alu(OPCODE_LOAD, OPERAND_SRCB, OPERAND_R0);
      dw[3] = alu(OPCODE_SUB, 0, 0);
      dw[4] = alu(OPCODE_STORE, OPERAND_R2, OPERAND_ACCU);

      anv_batch_emit(&cmd_buffer->batch, GEN8_MI_STORE_REGISTER_MEM,
                     .RegisterAddress = CS_GPR(2),
                     /* FIXME: This is only lower 32 bits */
                     .MemoryAddress = { buffer->bo, dst_offset });

      if (flags & VK_QUERY_RESULT_64_BIT)
         anv_batch_emit(&cmd_buffer->batch, GEN8_MI_STORE_REGISTER_MEM,
                        .RegisterAddress = CS_GPR(2) + 4,
                        /* FIXME: This is only lower 32 bits */
                        .MemoryAddress = { buffer->bo, dst_offset + 4 });

      dst_offset += destStride;
   }
}
