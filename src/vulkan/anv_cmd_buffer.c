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
 * This file contains all of the stuff for emitting commands into a command
 * buffer.  This includes implementations of most of the vkCmd*
 * entrypoints.  This file is concerned entirely with state emission and
 * not with the command buffer data structure itself.  As far as this file
 * is concerned, most of anv_cmd_buffer is magic.
 */

/* TODO: These are taken from GLES.  We should check the Vulkan spec */
const struct anv_dynamic_state default_dynamic_state = {
   .viewport = {
      .count = 0,
   },
   .scissor = {
      .count = 0,
   },
   .line_width = 1.0f,
   .depth_bias = {
      .bias = 0.0f,
      .clamp = 0.0f,
      .slope_scaled = 0.0f,
   },
   .blend_constants = { 0.0f, 0.0f, 0.0f, 0.0f },
   .depth_bounds = {
      .min = 0.0f,
      .max = 1.0f,
   },
   .stencil_compare_mask = {
      .front = ~0u,
      .back = ~0u,
   },
   .stencil_write_mask = {
      .front = ~0u,
      .back = ~0u,
   },
   .stencil_reference = {
      .front = 0u,
      .back = 0u,
   },
};

void
anv_dynamic_state_copy(struct anv_dynamic_state *dest,
                       const struct anv_dynamic_state *src,
                       uint32_t copy_mask)
{
   if (copy_mask & (1 << VK_DYNAMIC_STATE_VIEWPORT)) {
      dest->viewport.count = src->viewport.count;
      typed_memcpy(dest->viewport.viewports, src->viewport.viewports,
                   src->viewport.count);
   }

   if (copy_mask & (1 << VK_DYNAMIC_STATE_SCISSOR)) {
      dest->scissor.count = src->scissor.count;
      typed_memcpy(dest->scissor.scissors, src->scissor.scissors,
                   src->scissor.count);
   }

   if (copy_mask & (1 << VK_DYNAMIC_STATE_LINE_WIDTH))
      dest->line_width = src->line_width;

   if (copy_mask & (1 << VK_DYNAMIC_STATE_DEPTH_BIAS))
      dest->depth_bias = src->depth_bias;

   if (copy_mask & (1 << VK_DYNAMIC_STATE_BLEND_CONSTANTS))
      typed_memcpy(dest->blend_constants, src->blend_constants, 4);

   if (copy_mask & (1 << VK_DYNAMIC_STATE_DEPTH_BOUNDS))
      dest->depth_bounds = src->depth_bounds;

   if (copy_mask & (1 << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK))
      dest->stencil_compare_mask = src->stencil_compare_mask;

   if (copy_mask & (1 << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK))
      dest->stencil_write_mask = src->stencil_write_mask;

   if (copy_mask & (1 << VK_DYNAMIC_STATE_STENCIL_REFERENCE))
      dest->stencil_reference = src->stencil_reference;
}

static void
anv_cmd_state_init(struct anv_cmd_state *state)
{
   memset(&state->state_vf, 0, sizeof(state->state_vf));
   memset(&state->descriptors, 0, sizeof(state->descriptors));
   memset(&state->push_constants, 0, sizeof(state->push_constants));

   state->dirty = ~0;
   state->vb_dirty = 0;
   state->descriptors_dirty = 0;
   state->push_constants_dirty = 0;
   state->pipeline = NULL;
   state->dynamic = default_dynamic_state;

   state->gen7.index_buffer = NULL;
}

static VkResult
anv_cmd_buffer_ensure_push_constants_size(struct anv_cmd_buffer *cmd_buffer,
                                          VkShaderStage stage, uint32_t size)
{
   struct anv_push_constants **ptr = &cmd_buffer->state.push_constants[stage];

   if (*ptr == NULL) {
      *ptr = anv_device_alloc(cmd_buffer->device, size, 8,
                              VK_SYSTEM_ALLOC_TYPE_INTERNAL);
      if (*ptr == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
      (*ptr)->size = size;
   } else if ((*ptr)->size < size) {
      void *new_data = anv_device_alloc(cmd_buffer->device, size, 8,
                                        VK_SYSTEM_ALLOC_TYPE_INTERNAL);
      if (new_data == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      memcpy(new_data, *ptr, (*ptr)->size);
      anv_device_free(cmd_buffer->device, *ptr);

      *ptr = new_data;
      (*ptr)->size = size;
   }

   return VK_SUCCESS;
}

#define anv_cmd_buffer_ensure_push_constant_field(cmd_buffer, stage, field) \
   anv_cmd_buffer_ensure_push_constants_size(cmd_buffer, stage, \
      (offsetof(struct anv_push_constants, field) + \
       sizeof(cmd_buffer->state.push_constants[0]->field)))

VkResult anv_CreateCommandBuffer(
    VkDevice                                    _device,
    const VkCmdBufferCreateInfo*                pCreateInfo,
    VkCmdBuffer*                                pCmdBuffer)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_cmd_pool, pool, pCreateInfo->cmdPool);
   struct anv_cmd_buffer *cmd_buffer;
   VkResult result;

   cmd_buffer = anv_device_alloc(device, sizeof(*cmd_buffer), 8,
                                 VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   cmd_buffer->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   cmd_buffer->device = device;

   result = anv_cmd_buffer_init_batch_bo_chain(cmd_buffer);
   if (result != VK_SUCCESS)
      goto fail;

   anv_state_stream_init(&cmd_buffer->surface_state_stream,
                         &device->surface_state_block_pool);
   anv_state_stream_init(&cmd_buffer->dynamic_state_stream,
                         &device->dynamic_state_block_pool);

   cmd_buffer->level = pCreateInfo->level;
   cmd_buffer->opt_flags = 0;

   anv_cmd_state_init(&cmd_buffer->state);

   if (pool) {
      list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);
   } else {
      /* Init the pool_link so we can safefly call list_del when we destroy
       * the command buffer
       */
      list_inithead(&cmd_buffer->pool_link);
   }

   *pCmdBuffer = anv_cmd_buffer_to_handle(cmd_buffer);

   return VK_SUCCESS;

 fail: anv_device_free(device, cmd_buffer);

   return result;
}

void anv_DestroyCommandBuffer(
    VkDevice                                    _device,
    VkCmdBuffer                                 _cmd_buffer)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, _cmd_buffer);

   list_del(&cmd_buffer->pool_link);

   anv_cmd_buffer_fini_batch_bo_chain(cmd_buffer);

   anv_state_stream_finish(&cmd_buffer->surface_state_stream);
   anv_state_stream_finish(&cmd_buffer->dynamic_state_stream);
   anv_device_free(device, cmd_buffer);
}

VkResult anv_ResetCommandBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkCmdBufferResetFlags                       flags)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   anv_cmd_buffer_reset_batch_bo_chain(cmd_buffer);

   anv_cmd_state_init(&cmd_buffer->state);

   return VK_SUCCESS;
}

void
anv_cmd_buffer_emit_state_base_address(struct anv_cmd_buffer *cmd_buffer)
{
   switch (cmd_buffer->device->info.gen) {
   case 7:
      return gen7_cmd_buffer_emit_state_base_address(cmd_buffer);
   case 8:
      return gen8_cmd_buffer_emit_state_base_address(cmd_buffer);
   default:
      unreachable("unsupported gen\n");
   }
}

VkResult anv_BeginCommandBuffer(
    VkCmdBuffer                                 cmdBuffer,
    const VkCmdBufferBeginInfo*                 pBeginInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   anv_cmd_buffer_reset_batch_bo_chain(cmd_buffer);

   cmd_buffer->opt_flags = pBeginInfo->flags;

   if (cmd_buffer->level == VK_CMD_BUFFER_LEVEL_SECONDARY) {
      cmd_buffer->state.framebuffer =
         anv_framebuffer_from_handle(pBeginInfo->framebuffer);
      cmd_buffer->state.pass =
         anv_render_pass_from_handle(pBeginInfo->renderPass);

      struct anv_subpass *subpass =
         &cmd_buffer->state.pass->subpasses[pBeginInfo->subpass];

      anv_cmd_buffer_begin_subpass(cmd_buffer, subpass);
   }

   anv_cmd_buffer_emit_state_base_address(cmd_buffer);
   cmd_buffer->state.current_pipeline = UINT32_MAX;

   return VK_SUCCESS;
}

VkResult anv_EndCommandBuffer(
    VkCmdBuffer                                 cmdBuffer)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   struct anv_device *device = cmd_buffer->device;

   anv_cmd_buffer_end_batch_buffer(cmd_buffer);

   if (cmd_buffer->level == VK_CMD_BUFFER_LEVEL_PRIMARY) {
      /* The algorithm used to compute the validate list is not threadsafe as
       * it uses the bo->index field.  We have to lock the device around it.
       * Fortunately, the chances for contention here are probably very low.
       */
      pthread_mutex_lock(&device->mutex);
      anv_cmd_buffer_prepare_execbuf(cmd_buffer);
      pthread_mutex_unlock(&device->mutex);
   }

   return VK_SUCCESS;
}

void anv_CmdBindPipeline(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    VkPipeline                                  _pipeline)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_pipeline, pipeline, _pipeline);

   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      cmd_buffer->state.compute_pipeline = pipeline;
      cmd_buffer->state.compute_dirty |= ANV_CMD_DIRTY_PIPELINE;
      cmd_buffer->state.push_constants_dirty |= VK_SHADER_STAGE_COMPUTE_BIT;
      break;

   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      cmd_buffer->state.pipeline = pipeline;
      cmd_buffer->state.vb_dirty |= pipeline->vb_used;
      cmd_buffer->state.dirty |= ANV_CMD_DIRTY_PIPELINE;
      cmd_buffer->state.push_constants_dirty |= pipeline->active_stages;

      /* Apply the dynamic state from the pipeline */
      cmd_buffer->state.dirty |= pipeline->dynamic_state_mask;
      anv_dynamic_state_copy(&cmd_buffer->state.dynamic,
                             &pipeline->dynamic_state,
                             pipeline->dynamic_state_mask);
      break;

   default:
      assert(!"invalid bind point");
      break;
   }
}

void anv_CmdSetViewport(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    viewportCount,
    const VkViewport*                           pViewports)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   cmd_buffer->state.dynamic.viewport.count = viewportCount;
   memcpy(cmd_buffer->state.dynamic.viewport.viewports,
          pViewports, viewportCount * sizeof(*pViewports));

   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_DYNAMIC_VIEWPORT;
}

void anv_CmdSetScissor(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    scissorCount,
    const VkRect2D*                             pScissors)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   cmd_buffer->state.dynamic.scissor.count = scissorCount;
   memcpy(cmd_buffer->state.dynamic.scissor.scissors,
          pScissors, scissorCount * sizeof(*pScissors));

   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_DYNAMIC_SCISSOR;
}

void anv_CmdSetLineWidth(
    VkCmdBuffer                                 cmdBuffer,
    float                                       lineWidth)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   cmd_buffer->state.dynamic.line_width = lineWidth;
   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_DYNAMIC_LINE_WIDTH;
}

void anv_CmdSetDepthBias(
    VkCmdBuffer                                 cmdBuffer,
    float                                       depthBias,
    float                                       depthBiasClamp,
    float                                       slopeScaledDepthBias)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   cmd_buffer->state.dynamic.depth_bias.bias = depthBias;
   cmd_buffer->state.dynamic.depth_bias.clamp = depthBiasClamp;
   cmd_buffer->state.dynamic.depth_bias.slope_scaled = slopeScaledDepthBias;

   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS;
}

void anv_CmdSetBlendConstants(
    VkCmdBuffer                                 cmdBuffer,
    const float                                 blendConst[4])
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   memcpy(cmd_buffer->state.dynamic.blend_constants,
          blendConst, sizeof(float) * 4);

   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_DYNAMIC_BLEND_CONSTANTS;
}

void anv_CmdSetDepthBounds(
    VkCmdBuffer                                 cmdBuffer,
    float                                       minDepthBounds,
    float                                       maxDepthBounds)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   cmd_buffer->state.dynamic.depth_bounds.min = minDepthBounds;
   cmd_buffer->state.dynamic.depth_bounds.max = maxDepthBounds;

   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_DYNAMIC_DEPTH_BOUNDS;
}

void anv_CmdSetStencilCompareMask(
    VkCmdBuffer                                 cmdBuffer,
    VkStencilFaceFlags                          faceMask,
    uint32_t                                    stencilCompareMask)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmd_buffer->state.dynamic.stencil_compare_mask.front = stencilCompareMask;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmd_buffer->state.dynamic.stencil_compare_mask.back = stencilCompareMask;

   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK;
}

void anv_CmdSetStencilWriteMask(
    VkCmdBuffer                                 cmdBuffer,
    VkStencilFaceFlags                          faceMask,
    uint32_t                                    stencilWriteMask)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmd_buffer->state.dynamic.stencil_write_mask.front = stencilWriteMask;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmd_buffer->state.dynamic.stencil_write_mask.back = stencilWriteMask;

   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK;
}

void anv_CmdSetStencilReference(
    VkCmdBuffer                                 cmdBuffer,
    VkStencilFaceFlags                          faceMask,
    uint32_t                                    stencilReference)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmd_buffer->state.dynamic.stencil_reference.front = stencilReference;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmd_buffer->state.dynamic.stencil_reference.back = stencilReference;

   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE;
}

void anv_CmdBindDescriptorSets(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    VkPipelineLayout                            _layout,
    uint32_t                                    firstSet,
    uint32_t                                    setCount,
    const VkDescriptorSet*                      pDescriptorSets,
    uint32_t                                    dynamicOffsetCount,
    const uint32_t*                             pDynamicOffsets)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_pipeline_layout, layout, _layout);
   struct anv_descriptor_set_layout *set_layout;

   assert(firstSet + setCount < MAX_SETS);

   uint32_t dynamic_slot = 0;
   for (uint32_t i = 0; i < setCount; i++) {
      ANV_FROM_HANDLE(anv_descriptor_set, set, pDescriptorSets[i]);
      set_layout = layout->set[firstSet + i].layout;

      if (cmd_buffer->state.descriptors[firstSet + i] != set) {
         cmd_buffer->state.descriptors[firstSet + i] = set;
         cmd_buffer->state.descriptors_dirty |= set_layout->shader_stages;
      }

      if (set_layout->dynamic_offset_count > 0) {
         VkShaderStage s;
         for_each_bit(s, set_layout->shader_stages) {
            anv_cmd_buffer_ensure_push_constant_field(cmd_buffer, s,
                                                      dynamic_offsets);
            uint32_t *offsets =
               cmd_buffer->state.push_constants[s]->dynamic_offsets +
               layout->set[firstSet + i].dynamic_offset_start;

            typed_memcpy(offsets, pDynamicOffsets + dynamic_slot,
                         set_layout->dynamic_offset_count);

         }
         cmd_buffer->state.push_constants_dirty |= set_layout->shader_stages;

         dynamic_slot += set_layout->dynamic_offset_count;
      }
   }
}

void anv_CmdBindVertexBuffers(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    startBinding,
    uint32_t                                    bindingCount,
    const VkBuffer*                             pBuffers,
    const VkDeviceSize*                         pOffsets)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   struct anv_vertex_binding *vb = cmd_buffer->state.vertex_bindings;

   /* We have to defer setting up vertex buffer since we need the buffer
    * stride from the pipeline. */

   assert(startBinding + bindingCount < MAX_VBS);
   for (uint32_t i = 0; i < bindingCount; i++) {
      vb[startBinding + i].buffer = anv_buffer_from_handle(pBuffers[i]);
      vb[startBinding + i].offset = pOffsets[i];
      cmd_buffer->state.vb_dirty |= 1 << (startBinding + i);
   }
}

static void
add_surface_state_reloc(struct anv_cmd_buffer *cmd_buffer,
                        struct anv_state state, struct anv_bo *bo, uint32_t offset)
{
   /* The address goes in SURFACE_STATE dword 1 for gens < 8 and dwords 8 and
    * 9 for gen8+.  We only write the first dword for gen8+ here and rely on
    * the initial state to set the high bits to 0. */

   const uint32_t dword = cmd_buffer->device->info.gen < 8 ? 1 : 8;

   anv_reloc_list_add(&cmd_buffer->surface_relocs, cmd_buffer->device,
                      state.offset + dword * 4, bo, offset);
}

VkResult
anv_cmd_buffer_emit_binding_table(struct anv_cmd_buffer *cmd_buffer,
                                  VkShaderStage stage, struct anv_state *bt_state)
{
   struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   struct anv_subpass *subpass = cmd_buffer->state.subpass;
   struct anv_pipeline_layout *layout;
   uint32_t color_count, bias, state_offset;

   if (stage == VK_SHADER_STAGE_COMPUTE)
      layout = cmd_buffer->state.compute_pipeline->layout;
   else
      layout = cmd_buffer->state.pipeline->layout;

   if (stage == VK_SHADER_STAGE_FRAGMENT) {
      bias = MAX_RTS;
      color_count = subpass->color_count;
   } else {
      bias = 0;
      color_count = 0;
   }

   /* This is a little awkward: layout can be NULL but we still have to
    * allocate and set a binding table for the PS stage for render
    * targets. */
   uint32_t surface_count = layout ? layout->stage[stage].surface_count : 0;

   if (color_count + surface_count == 0)
      return VK_SUCCESS;

   *bt_state = anv_cmd_buffer_alloc_binding_table(cmd_buffer,
                                                  bias + surface_count,
                                                  &state_offset);
   uint32_t *bt_map = bt_state->map;

   if (bt_state->map == NULL)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   for (uint32_t a = 0; a < color_count; a++) {
      const struct anv_image_view *iview =
         fb->attachments[subpass->color_attachments[a]];

      bt_map[a] = iview->color_rt_surface_state.offset + state_offset;
      add_surface_state_reloc(cmd_buffer, iview->color_rt_surface_state,
                              iview->bo, iview->offset);
   }

   if (layout == NULL)
      return VK_SUCCESS;

   for (uint32_t s = 0; s < layout->stage[stage].surface_count; s++) {
      struct anv_pipeline_binding *binding =
         &layout->stage[stage].surface_to_descriptor[s];
      struct anv_descriptor_set *set =
         cmd_buffer->state.descriptors[binding->set];
      struct anv_descriptor *desc = &set->descriptors[binding->offset];

      const struct anv_state *surface_state;
      struct anv_bo *bo;
      uint32_t bo_offset;

      switch (desc->type) {
      case ANV_DESCRIPTOR_TYPE_EMPTY:
      case ANV_DESCRIPTOR_TYPE_SAMPLER:
         /* Nothing for us to do here */
         continue;
      case ANV_DESCRIPTOR_TYPE_BUFFER_VIEW:
         surface_state = &desc->buffer_view->surface_state;
         bo = desc->buffer_view->bo;
         bo_offset = desc->buffer_view->offset;
         break;
      case ANV_DESCRIPTOR_TYPE_BUFFER_AND_OFFSET: {
         struct anv_state state =
            anv_cmd_buffer_alloc_surface_state(cmd_buffer);
         anv_fill_buffer_surface_state(cmd_buffer->device, state.map,
                                       anv_format_for_vk_format(VK_FORMAT_R32G32B32A32_SFLOAT),
                                       desc->offset, desc->range);
         surface_state = &state;
         bo = desc->buffer_view->bo;
         bo_offset = desc->buffer_view->offset;
         break;
      }
      case ANV_DESCRIPTOR_TYPE_IMAGE_VIEW:
      case ANV_DESCRIPTOR_TYPE_IMAGE_VIEW_AND_SAMPLER:
         surface_state = &desc->image_view->nonrt_surface_state;
         bo = desc->image_view->bo;
         bo_offset = desc->image_view->offset;
         break;
      }

      bt_map[bias + s] = surface_state->offset + state_offset;
      add_surface_state_reloc(cmd_buffer, *surface_state, bo, bo_offset);
   }

   return VK_SUCCESS;
}

VkResult
anv_cmd_buffer_emit_samplers(struct anv_cmd_buffer *cmd_buffer,
                             VkShaderStage stage, struct anv_state *state)
{
   struct anv_pipeline_layout *layout;
   uint32_t sampler_count;

   if (stage == VK_SHADER_STAGE_COMPUTE)
      layout = cmd_buffer->state.compute_pipeline->layout;
   else
      layout = cmd_buffer->state.pipeline->layout;

   sampler_count = layout ? layout->stage[stage].sampler_count : 0;
   if (sampler_count == 0)
      return VK_SUCCESS;

   uint32_t size = sampler_count * 16;
   *state = anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, size, 32);

   if (state->map == NULL)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   for (uint32_t s = 0; s < layout->stage[stage].sampler_count; s++) {
      struct anv_pipeline_binding *binding =
         &layout->stage[stage].sampler_to_descriptor[s];
      struct anv_descriptor_set *set =
         cmd_buffer->state.descriptors[binding->set];
      struct anv_descriptor *desc = &set->descriptors[binding->offset];

      if (desc->type != ANV_DESCRIPTOR_TYPE_SAMPLER &&
          desc->type != ANV_DESCRIPTOR_TYPE_IMAGE_VIEW_AND_SAMPLER)
         continue;

      struct anv_sampler *sampler = desc->sampler;

      /* FIXME: We shouldn't have to do this */
      if (sampler == NULL)
         continue;

      memcpy(state->map + (s * 16),
             sampler->state, sizeof(sampler->state));
   }

   return VK_SUCCESS;
}

static VkResult
flush_descriptor_set(struct anv_cmd_buffer *cmd_buffer, VkShaderStage stage)
{
   struct anv_state surfaces = { 0, }, samplers = { 0, };
   VkResult result;

   result = anv_cmd_buffer_emit_samplers(cmd_buffer, stage, &samplers);
   if (result != VK_SUCCESS)
      return result;
   result = anv_cmd_buffer_emit_binding_table(cmd_buffer, stage, &surfaces);
   if (result != VK_SUCCESS)
      return result;

   static const uint32_t sampler_state_opcodes[] = {
      [VK_SHADER_STAGE_VERTEX]                  = 43,
      [VK_SHADER_STAGE_TESS_CONTROL]            = 44, /* HS */
      [VK_SHADER_STAGE_TESS_EVALUATION]         = 45, /* DS */
      [VK_SHADER_STAGE_GEOMETRY]                = 46,
      [VK_SHADER_STAGE_FRAGMENT]                = 47,
      [VK_SHADER_STAGE_COMPUTE]                 = 0,
   };

   static const uint32_t binding_table_opcodes[] = {
      [VK_SHADER_STAGE_VERTEX]                  = 38,
      [VK_SHADER_STAGE_TESS_CONTROL]            = 39,
      [VK_SHADER_STAGE_TESS_EVALUATION]         = 40,
      [VK_SHADER_STAGE_GEOMETRY]                = 41,
      [VK_SHADER_STAGE_FRAGMENT]                = 42,
      [VK_SHADER_STAGE_COMPUTE]                 = 0,
   };

   if (samplers.alloc_size > 0) {
      anv_batch_emit(&cmd_buffer->batch,
                     GEN7_3DSTATE_SAMPLER_STATE_POINTERS_VS,
                     ._3DCommandSubOpcode  = sampler_state_opcodes[stage],
                     .PointertoVSSamplerState = samplers.offset);
   }

   if (surfaces.alloc_size > 0) {
      anv_batch_emit(&cmd_buffer->batch,
                     GEN7_3DSTATE_BINDING_TABLE_POINTERS_VS,
                     ._3DCommandSubOpcode  = binding_table_opcodes[stage],
                     .PointertoVSBindingTable = surfaces.offset);
   }

   return VK_SUCCESS;
}

void
anv_flush_descriptor_sets(struct anv_cmd_buffer *cmd_buffer)
{
   VkShaderStage s;
   VkShaderStageFlags dirty = cmd_buffer->state.descriptors_dirty &
                              cmd_buffer->state.pipeline->active_stages;

   VkResult result = VK_SUCCESS;
   for_each_bit(s, dirty) {
      result = flush_descriptor_set(cmd_buffer, s);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      assert(result == VK_ERROR_OUT_OF_DEVICE_MEMORY);

      result = anv_cmd_buffer_new_binding_table_block(cmd_buffer);
      assert(result == VK_SUCCESS);

      /* Re-emit state base addresses so we get the new surface state base
       * address before we start emitting binding tables etc.
       */
      anv_cmd_buffer_emit_state_base_address(cmd_buffer);

      /* Re-emit all active binding tables */
      for_each_bit(s, cmd_buffer->state.pipeline->active_stages) {
         result = flush_descriptor_set(cmd_buffer, s);

         /* It had better succeed this time */
         assert(result == VK_SUCCESS);
      }
   }

   cmd_buffer->state.descriptors_dirty &= ~cmd_buffer->state.pipeline->active_stages;
}

struct anv_state
anv_cmd_buffer_emit_dynamic(struct anv_cmd_buffer *cmd_buffer,
                             uint32_t *a, uint32_t dwords, uint32_t alignment)
{
   struct anv_state state;

   state = anv_cmd_buffer_alloc_dynamic_state(cmd_buffer,
                                              dwords * 4, alignment);
   memcpy(state.map, a, dwords * 4);

   VG(VALGRIND_CHECK_MEM_IS_DEFINED(state.map, dwords * 4));

   return state;
}

struct anv_state
anv_cmd_buffer_merge_dynamic(struct anv_cmd_buffer *cmd_buffer,
                             uint32_t *a, uint32_t *b,
                             uint32_t dwords, uint32_t alignment)
{
   struct anv_state state;
   uint32_t *p;

   state = anv_cmd_buffer_alloc_dynamic_state(cmd_buffer,
                                              dwords * 4, alignment);
   p = state.map;
   for (uint32_t i = 0; i < dwords; i++)
      p[i] = a[i] | b[i];

   VG(VALGRIND_CHECK_MEM_IS_DEFINED(p, dwords * 4));

   return state;
}

void
anv_cmd_buffer_begin_subpass(struct anv_cmd_buffer *cmd_buffer,
                             struct anv_subpass *subpass)
{
   switch (cmd_buffer->device->info.gen) {
   case 7:
      gen7_cmd_buffer_begin_subpass(cmd_buffer, subpass);
      break;
   case 8:
      gen8_cmd_buffer_begin_subpass(cmd_buffer, subpass);
      break;
   default:
      unreachable("unsupported gen\n");
   }
}

static void
emit_viewport_state(struct anv_cmd_buffer *cmd_buffer,
                    uint32_t count, const VkViewport *viewports)
{
   struct anv_state sf_clip_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, count * 64, 64);
   struct anv_state cc_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, count * 8, 32);

   for (uint32_t i = 0; i < count; i++) {
      const VkViewport *vp = &viewports[i];

      /* The gen7 state struct has just the matrix and guardband fields, the
       * gen8 struct adds the min/max viewport fields. */
      struct GEN8_SF_CLIP_VIEWPORT sf_clip_viewport = {
         .ViewportMatrixElementm00 = vp->width / 2,
         .ViewportMatrixElementm11 = vp->height / 2,
         .ViewportMatrixElementm22 = (vp->maxDepth - vp->minDepth) / 2,
         .ViewportMatrixElementm30 = vp->originX + vp->width / 2,
         .ViewportMatrixElementm31 = vp->originY + vp->height / 2,
         .ViewportMatrixElementm32 = (vp->maxDepth + vp->minDepth) / 2,
         .XMinClipGuardband = -1.0f,
         .XMaxClipGuardband = 1.0f,
         .YMinClipGuardband = -1.0f,
         .YMaxClipGuardband = 1.0f,
         .XMinViewPort = vp->originX,
         .XMaxViewPort = vp->originX + vp->width - 1,
         .YMinViewPort = vp->originY,
         .YMaxViewPort = vp->originY + vp->height - 1,
      };

      struct GEN7_CC_VIEWPORT cc_viewport = {
         .MinimumDepth = vp->minDepth,
         .MaximumDepth = vp->maxDepth
      };

      GEN8_SF_CLIP_VIEWPORT_pack(NULL, sf_clip_state.map + i * 64,
                                 &sf_clip_viewport);
      GEN7_CC_VIEWPORT_pack(NULL, cc_state.map + i * 32, &cc_viewport);
   }

   anv_batch_emit(&cmd_buffer->batch,
                  GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_CC,
                  .CCViewportPointer = cc_state.offset);
   anv_batch_emit(&cmd_buffer->batch,
                  GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP,
                  .SFClipViewportPointer = sf_clip_state.offset);
}

void
anv_cmd_buffer_emit_viewport(struct anv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->state.dynamic.viewport.count > 0) {
      emit_viewport_state(cmd_buffer, cmd_buffer->state.dynamic.viewport.count,
                          cmd_buffer->state.dynamic.viewport.viewports);
   } else {
      /* If viewport count is 0, this is taken to mean "use the default" */
      emit_viewport_state(cmd_buffer, 1,
                          &(VkViewport) {
                             .originX = 0.0f,
                             .originY = 0.0f,
                             .width = cmd_buffer->state.framebuffer->width,
                             .height = cmd_buffer->state.framebuffer->height,
                             .minDepth = 0.0f,
                             .maxDepth = 1.0f,
                          });
   }
}

static inline int64_t
clamp_int64(int64_t x, int64_t min, int64_t max)
{
   if (x < min)
      return min;
   else if (x < max)
      return x;
   else
      return max;
}

static void
emit_scissor_state(struct anv_cmd_buffer *cmd_buffer,
                   uint32_t count, const VkRect2D *scissors)
{
   struct anv_state scissor_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, count * 32, 32);

   for (uint32_t i = 0; i < count; i++) {
      const VkRect2D *s = &scissors[i];

      /* Since xmax and ymax are inclusive, we have to have xmax < xmin or
       * ymax < ymin for empty clips.  In case clip x, y, width height are all
       * 0, the clamps below produce 0 for xmin, ymin, xmax, ymax, which isn't
       * what we want. Just special case empty clips and produce a canonical
       * empty clip. */
      static const struct GEN7_SCISSOR_RECT empty_scissor = {
         .ScissorRectangleYMin = 1,
         .ScissorRectangleXMin = 1,
         .ScissorRectangleYMax = 0,
         .ScissorRectangleXMax = 0
      };

      const int max = 0xffff;
      struct GEN7_SCISSOR_RECT scissor = {
         /* Do this math using int64_t so overflow gets clamped correctly. */
         .ScissorRectangleYMin = clamp_int64(s->offset.y, 0, max),
         .ScissorRectangleXMin = clamp_int64(s->offset.x, 0, max),
         .ScissorRectangleYMax = clamp_int64((uint64_t) s->offset.y + s->extent.height - 1, 0, max),
         .ScissorRectangleXMax = clamp_int64((uint64_t) s->offset.x + s->extent.width - 1, 0, max)
      };

      if (s->extent.width <= 0 || s->extent.height <= 0) {
         GEN7_SCISSOR_RECT_pack(NULL, scissor_state.map + i * 32,
                                &empty_scissor);
      } else {
         GEN7_SCISSOR_RECT_pack(NULL, scissor_state.map + i * 32, &scissor);
      }
   }

   anv_batch_emit(&cmd_buffer->batch, GEN8_3DSTATE_SCISSOR_STATE_POINTERS,
                  .ScissorRectPointer = scissor_state.offset);
}

void
anv_cmd_buffer_emit_scissor(struct anv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->state.dynamic.scissor.count > 0) {
      emit_scissor_state(cmd_buffer, cmd_buffer->state.dynamic.scissor.count,
                         cmd_buffer->state.dynamic.scissor.scissors);
   } else {
      /* Emit a default scissor based on the currently bound framebuffer */
      emit_scissor_state(cmd_buffer, 1,
                         &(VkRect2D) {
                            .offset = { .x = 0, .y = 0, },
                            .extent = {
                               .width = cmd_buffer->state.framebuffer->width,
                               .height = cmd_buffer->state.framebuffer->height,
                            },
                         });
   }
}

void anv_CmdSetEvent(
    VkCmdBuffer                                 cmdBuffer,
    VkEvent                                     event,
    VkPipelineStageFlags                        stageMask)
{
   stub();
}

void anv_CmdResetEvent(
    VkCmdBuffer                                 cmdBuffer,
    VkEvent                                     event,
    VkPipelineStageFlags                        stageMask)
{
   stub();
}

void anv_CmdWaitEvents(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    eventCount,
    const VkEvent*                              pEvents,
    VkPipelineStageFlags                        srcStageMask,
    VkPipelineStageFlags                        destStageMask,
    uint32_t                                    memBarrierCount,
    const void* const*                          ppMemBarriers)
{
   stub();
}

struct anv_state
anv_cmd_buffer_push_constants(struct anv_cmd_buffer *cmd_buffer,
                              VkShaderStage stage)
{
   struct anv_push_constants *data =
      cmd_buffer->state.push_constants[stage];
   struct brw_stage_prog_data *prog_data =
      cmd_buffer->state.pipeline->prog_data[stage];

   /* If we don't actually have any push constants, bail. */
   if (data == NULL || prog_data->nr_params == 0)
      return (struct anv_state) { .offset = 0 };

   struct anv_state state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer,
                                         prog_data->nr_params * sizeof(float),
                                         32 /* bottom 5 bits MBZ */);

   /* Walk through the param array and fill the buffer with data */
   uint32_t *u32_map = state.map;
   for (unsigned i = 0; i < prog_data->nr_params; i++) {
      uint32_t offset = (uintptr_t)prog_data->param[i];
      u32_map[i] = *(uint32_t *)((uint8_t *)data + offset);
   }

   return state;
}

void anv_CmdPushConstants(
    VkCmdBuffer                                 cmdBuffer,
    VkPipelineLayout                            layout,
    VkShaderStageFlags                          stageFlags,
    uint32_t                                    start,
    uint32_t                                    length,
    const void*                                 values)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   VkShaderStage stage;

   for_each_bit(stage, stageFlags) {
      anv_cmd_buffer_ensure_push_constant_field(cmd_buffer, stage, client_data);

      memcpy(cmd_buffer->state.push_constants[stage]->client_data + start,
             values, length);
   }

   cmd_buffer->state.push_constants_dirty |= stageFlags;
}

void anv_CmdExecuteCommands(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    cmdBuffersCount,
    const VkCmdBuffer*                          pCmdBuffers)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, primary, cmdBuffer);

   assert(primary->level == VK_CMD_BUFFER_LEVEL_PRIMARY);

   anv_assert(primary->state.subpass == &primary->state.pass->subpasses[0]);

   for (uint32_t i = 0; i < cmdBuffersCount; i++) {
      ANV_FROM_HANDLE(anv_cmd_buffer, secondary, pCmdBuffers[i]);

      assert(secondary->level == VK_CMD_BUFFER_LEVEL_SECONDARY);

      anv_cmd_buffer_add_secondary(primary, secondary);
   }
}

VkResult anv_CreateCommandPool(
    VkDevice                                    _device,
    const VkCmdPoolCreateInfo*                  pCreateInfo,
    VkCmdPool*                                  pCmdPool)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_cmd_pool *pool;

   pool = anv_device_alloc(device, sizeof(*pool), 8,
                           VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pool == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   list_inithead(&pool->cmd_buffers);

   *pCmdPool = anv_cmd_pool_to_handle(pool);

   return VK_SUCCESS;
}

void anv_DestroyCommandPool(
    VkDevice                                    _device,
    VkCmdPool                                   cmdPool)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_cmd_pool, pool, cmdPool);

   anv_ResetCommandPool(_device, cmdPool, 0);

   anv_device_free(device, pool);
}

VkResult anv_ResetCommandPool(
    VkDevice                                    device,
    VkCmdPool                                   cmdPool,
    VkCmdPoolResetFlags                         flags)
{
   ANV_FROM_HANDLE(anv_cmd_pool, pool, cmdPool);

   list_for_each_entry_safe(struct anv_cmd_buffer, cmd_buffer,
                            &pool->cmd_buffers, pool_link) {
      anv_DestroyCommandBuffer(device, anv_cmd_buffer_to_handle(cmd_buffer));
   }

   return VK_SUCCESS;
}

/**
 * Return NULL if the current subpass has no depthstencil attachment.
 */
const struct anv_image_view *
anv_cmd_buffer_get_depth_stencil_view(const struct anv_cmd_buffer *cmd_buffer)
{
   const struct anv_subpass *subpass = cmd_buffer->state.subpass;
   const struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;

   if (subpass->depth_stencil_attachment == VK_ATTACHMENT_UNUSED)
      return NULL;

   const struct anv_image_view *iview =
      fb->attachments[subpass->depth_stencil_attachment];

   assert(anv_format_is_depth_or_stencil(iview->format));

   return iview;
}
