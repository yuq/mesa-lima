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

#define GLSL(src) "#version 330\n" #src

static void
anv_device_init_meta_clear_state(struct anv_device *device)
{
   VkPipelineIaStateCreateInfo ia_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_IA_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
      .disableVertexReuse = false,
      .primitiveRestartEnable = false,
      .primitiveRestartIndex = 0
   };

   /* We don't use a vertex shader for clearing, but instead build and pass
    * the VUEs directly to the rasterization backend.
    */
   static const char fs_source[] = GLSL(
      out vec4 f_color;
      flat in vec4 v_color;
      void main()
      {
         f_color = v_color;
      });

   VkShader fs;
   vkCreateShader((VkDevice) device,
                  &(VkShaderCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
                     .codeSize = sizeof(fs_source),
                     .pCode = fs_source,
                     .flags = 0
                  },
                  &fs);

   VkPipelineShaderStageCreateInfo fs_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &ia_create_info,
      .shader = {
         .stage = VK_SHADER_STAGE_FRAGMENT,
         .shader = fs,
         .linkConstBufferCount = 0,
         .pLinkConstBufferInfo = NULL,
         .pSpecializationInfo = NULL
      }
   };

   /* We use instanced rendering to clear multiple render targets. We have two
    * vertex buffers: the first vertex buffer holds per-vertex data and
    * provides the vertices for the clear rectangle. The second one holds
    * per-instance data, which consists of the VUE header (which selects the
    * layer) and the color (Vulkan supports per-RT clear colors).
    */
   VkPipelineVertexInputCreateInfo vi_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_CREATE_INFO,
      .pNext = &fs_create_info,
      .bindingCount = 2,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .strideInBytes = 8,
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         },
         {
            .binding = 1,
            .strideInBytes = 32,
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_INSTANCE
         },
      },
      .attributeCount = 3,
      .pVertexAttributeDescriptions = (VkVertexInputAttributeDescription[]) {
         {
            /* VUE Header */
            .location = 0,
            .binding = 1,
            .format = VK_FORMAT_R32G32B32A32_UINT,
            .offsetInBytes = 0
         },
         {
            /* Position */
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offsetInBytes = 0
         },
         {
            /* Color */
            .location = 2,
            .binding = 1,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offsetInBytes = 16
         }
      }
   };

   VkPipelineRsStateCreateInfo rs_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RS_STATE_CREATE_INFO,
      .pNext = &vi_create_info,
      .depthClipEnable = true,
      .rasterizerDiscardEnable = false,
      .fillMode = VK_FILL_MODE_SOLID,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_CCW
   };

   anv_pipeline_create((VkDevice) device,
                       &(VkGraphicsPipelineCreateInfo) {
                          .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                          .pNext = &rs_create_info,
                          .flags = 0,
                          .layout = 0
                       },
                       &(struct anv_pipeline_create_info) {
                          .use_repclear = true,
                          .disable_viewport = true,
                          .use_rectlist = true
                       },
                       &device->clear_state.pipeline);

   vkDestroyObject((VkDevice) device, VK_OBJECT_TYPE_SHADER, fs);

   vkCreateDynamicRasterState((VkDevice) device,
                              &(VkDynamicRsStateCreateInfo) {
                                 .sType = VK_STRUCTURE_TYPE_DYNAMIC_RS_STATE_CREATE_INFO,
                              },
                              &device->clear_state.rs_state);
}

struct anv_saved_state {
   struct {
      struct anv_buffer *buffer;
      VkDeviceSize offset;
   } vb[2];
   struct anv_pipeline *pipeline;
};

static void
anv_cmd_buffer_save(struct anv_cmd_buffer *cmd_buffer, struct anv_saved_state *state)
{
   memcpy(state->vb, cmd_buffer->vb, sizeof(state->vb));
   state->pipeline = cmd_buffer->pipeline;
}

static void
anv_cmd_buffer_restore(struct anv_cmd_buffer *cmd_buffer, struct anv_saved_state *state)
{
   memcpy(cmd_buffer->vb, state->vb, sizeof(state->vb));
   cmd_buffer->pipeline = state->pipeline;

   cmd_buffer->vb_dirty |= (1 << ARRAY_SIZE(state->vb)) - 1;
   cmd_buffer->dirty |= ANV_CMD_BUFFER_PIPELINE_DIRTY;
}

void
anv_cmd_buffer_clear(struct anv_cmd_buffer *cmd_buffer,
                     struct anv_render_pass *pass)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_framebuffer *fb = cmd_buffer->framebuffer;
   struct anv_saved_state saved_state;
   struct anv_state state;
   uint32_t size;

   struct instance_data {
      struct {
         uint32_t Reserved;
         uint32_t RTAIndex;
         uint32_t ViewportIndex;
         float PointWidth;
      } vue_header;
      float color[4];
   } *instance_data;

   const float vertex_data[] = {
      /* Rect-list coordinates */
            0.0,        0.0,
      fb->width,        0.0,
      fb->width, fb->height,

      /* Align to 16 bytes */
            0.0,        0.0,
   };

   size = sizeof(vertex_data) + pass->num_clear_layers * sizeof(instance_data[0]);
   state = anv_state_stream_alloc(&cmd_buffer->surface_state_stream, size, 16);

   memcpy(state.map, vertex_data, sizeof(vertex_data));
   instance_data = state.map + sizeof(vertex_data);

   for (uint32_t i = 0; i < pass->num_layers; i++) {
      if (pass->layers[i].color_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         *instance_data++ = (struct instance_data) {
            .vue_header = {
               .RTAIndex = i,
               .ViewportIndex = 0,
               .PointWidth = 0.0
            },
            .color = {
               pass->layers[i].clear_color.color.floatColor[0],
               pass->layers[i].clear_color.color.floatColor[1],
               pass->layers[i].clear_color.color.floatColor[2],
               pass->layers[i].clear_color.color.floatColor[3],
            }
         };
      }
   }

   struct anv_buffer vertex_buffer = {
      .device = cmd_buffer->device,
      .size = size,
      .bo = &device->surface_state_block_pool.bo,
      .offset = state.offset
   };

   anv_cmd_buffer_save(cmd_buffer, &saved_state);

   vkCmdBindVertexBuffers((VkCmdBuffer) cmd_buffer, 0, 2,
                          (VkBuffer[]) {
                             (VkBuffer) &vertex_buffer,
                             (VkBuffer) &vertex_buffer
                          },
                          (VkDeviceSize[]) {
                             0,
                             sizeof(vertex_data)
                          });

   if ((VkPipeline) cmd_buffer->pipeline != device->clear_state.pipeline)
      vkCmdBindPipeline((VkCmdBuffer) cmd_buffer,
                        VK_PIPELINE_BIND_POINT_GRAPHICS, device->clear_state.pipeline);

   /* We don't need anything here, only set if not already set. */
   if (cmd_buffer->rs_state == NULL)
      vkCmdBindDynamicStateObject((VkCmdBuffer) cmd_buffer,
                                  VK_STATE_BIND_POINT_RASTER,
                                  device->clear_state.rs_state);

   if (cmd_buffer->vp_state == NULL)
      vkCmdBindDynamicStateObject((VkCmdBuffer) cmd_buffer,
                                  VK_STATE_BIND_POINT_VIEWPORT,
                                  cmd_buffer->framebuffer->vp_state);

   vkCmdDraw((VkCmdBuffer) cmd_buffer, 0, 3, 0, pass->num_clear_layers);

   /* Restore API state */
   anv_cmd_buffer_restore(cmd_buffer, &saved_state);

}

void VKAPI vkCmdCopyBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{
   stub();
}

void VKAPI vkCmdCopyImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageCopy*                          pRegions)
{
   stub();
}

void VKAPI vkCmdBlitImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageBlit*                          pRegions)
{
   stub();
}

void VKAPI vkCmdCopyBufferToImage(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    srcBuffer,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   stub();
}

void VKAPI vkCmdCopyImageToBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   stub();
}

void VKAPI vkCmdCloneImageData(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout)
{
   stub();
}

void VKAPI vkCmdUpdateBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                dataSize,
    const uint32_t*                             pData)
{
   stub();
}

void VKAPI vkCmdFillBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                fillSize,
    uint32_t                                    data)
{
   stub();
}

void VKAPI vkCmdClearColorImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     image,
    VkImageLayout                               imageLayout,
    const VkClearColor*                         color,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
   stub();
}

void VKAPI vkCmdClearDepthStencil(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     image,
    VkImageLayout                               imageLayout,
    float                                       depth,
    uint32_t                                    stencil,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
   stub();
}

void VKAPI vkCmdResolveImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageResolve*                       pRegions)
{
   stub();
}

void
anv_device_init_meta(struct anv_device *device)
{
   anv_device_init_meta_clear_state(device);
}
