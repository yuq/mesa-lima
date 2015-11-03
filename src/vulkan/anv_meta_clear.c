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

#include "anv_meta.h"
#include "anv_meta_clear.h"
#include "anv_nir_builder.h"
#include "anv_private.h"

struct clear_instance_data {
   struct anv_vue_header vue_header;
   VkClearColorValue color;
};

static void
meta_emit_clear(struct anv_cmd_buffer *cmd_buffer,
                int num_instances,
                struct clear_instance_data *instance_data,
                VkClearDepthStencilValue ds_clear_value)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   struct anv_state state;
   uint32_t size;

   const float vertex_data[] = {
      /* Rect-list coordinates */
            0.0,        0.0, ds_clear_value.depth,
      fb->width,        0.0, ds_clear_value.depth,
      fb->width, fb->height, ds_clear_value.depth,

      /* Align to 16 bytes */
      0.0, 0.0, 0.0,
   };

   size = sizeof(vertex_data) + num_instances * sizeof(*instance_data);
   state = anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, size, 16);

   /* Copy in the vertex and instance data */
   memcpy(state.map, vertex_data, sizeof(vertex_data));
   memcpy(state.map + sizeof(vertex_data), instance_data,
          num_instances * sizeof(*instance_data));

   struct anv_buffer vertex_buffer = {
      .device = cmd_buffer->device,
      .size = size,
      .bo = &device->dynamic_state_block_pool.bo,
      .offset = state.offset
   };

   anv_CmdBindVertexBuffers(anv_cmd_buffer_to_handle(cmd_buffer), 0, 2,
      (VkBuffer[]) {
         anv_buffer_to_handle(&vertex_buffer),
         anv_buffer_to_handle(&vertex_buffer)
      },
      (VkDeviceSize[]) {
         0,
         sizeof(vertex_data)
      });

   if (cmd_buffer->state.pipeline != anv_pipeline_from_handle(device->meta_state.clear.pipeline))
      anv_CmdBindPipeline(anv_cmd_buffer_to_handle(cmd_buffer),
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          device->meta_state.clear.pipeline);

   ANV_CALL(CmdDraw)(anv_cmd_buffer_to_handle(cmd_buffer),
                     3, num_instances, 0, 0);
}

void
anv_cmd_buffer_clear_attachments(struct anv_cmd_buffer *cmd_buffer,
                                 struct anv_render_pass *pass,
                                 const VkClearValue *clear_values)
{
   struct anv_meta_saved_state saved_state;

   if (pass->has_stencil_clear_attachment)
      anv_finishme("stencil clear");

   /* FINISHME: Rethink how we count clear attachments in light of
    * 0.138.2 -> 0.170.2 diff.
    */
   if (pass->num_color_clear_attachments == 0 &&
       !pass->has_depth_clear_attachment)
      return;

   struct clear_instance_data instance_data[pass->num_color_clear_attachments];
   uint32_t color_attachments[pass->num_color_clear_attachments];
   uint32_t ds_attachment = VK_ATTACHMENT_UNUSED;
   VkClearDepthStencilValue ds_clear_value = {0};

   int layer = 0;
   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      const struct anv_render_pass_attachment *att = &pass->attachments[i];

      if (anv_format_is_color(att->format)) {
         if (att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            instance_data[layer] = (struct clear_instance_data) {
               .vue_header = {
                  .RTAIndex = i,
                  .ViewportIndex = 0,
                  .PointWidth = 0.0
               },
               .color = clear_values[i].color,
            };
            color_attachments[layer] = i;
            layer++;
         }
      } else {
         if (att->format->depth_format &&
             att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            assert(ds_attachment == VK_ATTACHMENT_UNUSED);
            ds_attachment = i;
            ds_clear_value = clear_values[ds_attachment].depthStencil;
         }

         if (att->format->has_stencil &&
             att->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            anv_finishme("stencil clear");
         }
      }
   }

   anv_meta_save(&saved_state, cmd_buffer,
                 (1 << VK_DYNAMIC_STATE_VIEWPORT));
   cmd_buffer->state.dynamic.viewport.count = 0;

   struct anv_subpass subpass = {
      .input_count = 0,
      .color_count = pass->num_color_clear_attachments,
      .color_attachments = color_attachments,
      .depth_stencil_attachment = ds_attachment,
   };

   anv_cmd_buffer_begin_subpass(cmd_buffer, &subpass);

   meta_emit_clear(cmd_buffer, pass->num_color_clear_attachments,
                   instance_data, ds_clear_value);

   anv_meta_restore(&saved_state, cmd_buffer);
}

static nir_shader *
build_nir_vertex_shader(bool attr_flat)
{
   nir_builder b;

   const struct glsl_type *vertex_type = glsl_vec4_type();

   nir_builder_init_simple_shader(&b, MESA_SHADER_VERTEX);

   nir_variable *pos_in = nir_variable_create(b.shader, nir_var_shader_in,
                                              vertex_type, "a_pos");
   pos_in->data.location = VERT_ATTRIB_GENERIC0;
   nir_variable *pos_out = nir_variable_create(b.shader, nir_var_shader_out,
                                               vertex_type, "gl_Position");
   pos_in->data.location = VARYING_SLOT_POS;
   nir_copy_var(&b, pos_out, pos_in);

   /* Add one more pass-through attribute.  For clear shaders, this is used
    * to store the color and for blit shaders it's the texture coordinate.
    */
   const struct glsl_type *attr_type = glsl_vec4_type();
   nir_variable *attr_in = nir_variable_create(b.shader, nir_var_shader_in,
                                               attr_type, "a_attr");
   attr_in->data.location = VERT_ATTRIB_GENERIC1;
   nir_variable *attr_out = nir_variable_create(b.shader, nir_var_shader_out,
                                                attr_type, "v_attr");
   attr_out->data.location = VARYING_SLOT_VAR0;
   attr_out->data.interpolation = attr_flat ? INTERP_QUALIFIER_FLAT :
                                              INTERP_QUALIFIER_SMOOTH;
   nir_copy_var(&b, attr_out, attr_in);

   return b.shader;
}

static nir_shader *
build_nir_clear_fragment_shader(void)
{
   nir_builder b;

   const struct glsl_type *color_type = glsl_vec4_type();

   nir_builder_init_simple_shader(&b, MESA_SHADER_FRAGMENT);

   nir_variable *color_in = nir_variable_create(b.shader, nir_var_shader_in,
                                                color_type, "v_attr");
   color_in->data.location = VARYING_SLOT_VAR0;
   color_in->data.interpolation = INTERP_QUALIFIER_FLAT;
   nir_variable *color_out = nir_variable_create(b.shader, nir_var_shader_out,
                                                 color_type, "f_color");
   color_out->data.location = FRAG_RESULT_DATA0;
   nir_copy_var(&b, color_out, color_in);

   return b.shader;
}

void
anv_device_init_meta_clear_state(struct anv_device *device)
{
   struct anv_shader_module vsm = {
      .nir = build_nir_vertex_shader(true),
   };

   struct anv_shader_module fsm = {
      .nir = build_nir_clear_fragment_shader(),
   };

   VkShader vs;
   anv_CreateShader(anv_device_to_handle(device),
      &(VkShaderCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
         .module = anv_shader_module_to_handle(&vsm),
         .pName = "main",
      }, &vs);

   VkShader fs;
   anv_CreateShader(anv_device_to_handle(device),
      &(VkShaderCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
         .module = anv_shader_module_to_handle(&fsm),
         .pName = "main",
      }, &fs);

   /* We use instanced rendering to clear multiple render targets. We have two
    * vertex buffers: the first vertex buffer holds per-vertex data and
    * provides the vertices for the clear rectangle. The second one holds
    * per-instance data, which consists of the VUE header (which selects the
    * layer) and the color (Vulkan supports per-RT clear colors).
    */
   VkPipelineVertexInputStateCreateInfo vi_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .bindingCount = 2,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .strideInBytes = 12,
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
            .format = VK_FORMAT_R32G32B32_SFLOAT,
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

   anv_graphics_pipeline_create(anv_device_to_handle(device),
      &(VkGraphicsPipelineCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,

         .stageCount = 2,
         .pStages = (VkPipelineShaderStageCreateInfo[]) {
            {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_VERTEX,
               .shader = vs,
               .pSpecializationInfo = NULL
            }, {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_FRAGMENT,
               .shader = fs,
               .pSpecializationInfo = NULL,
            }
         },
         .pVertexInputState = &vi_create_info,
         .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .primitiveRestartEnable = false,
         },
         .pViewportState = &(VkPipelineViewportStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
         },
         .pRasterState = &(VkPipelineRasterStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTER_STATE_CREATE_INFO,
            .depthClipEnable = true,
            .rasterizerDiscardEnable = false,
            .fillMode = VK_FILL_MODE_SOLID,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CCW
         },
         .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterSamples = 1,
            .sampleShadingEnable = false,
            .pSampleMask = (VkSampleMask[]) { UINT32_MAX },
         },
         .pDepthStencilState = &(VkPipelineDepthStencilStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = true,
            .depthWriteEnable = true,
            .depthCompareOp = VK_COMPARE_OP_ALWAYS,
            .depthBoundsTestEnable = false,
            .stencilTestEnable = true,
            .front = (VkStencilOpState) {
               .stencilPassOp = VK_STENCIL_OP_REPLACE,
               .stencilCompareOp = VK_COMPARE_OP_ALWAYS,
            },
            .back = (VkStencilOpState) {
               .stencilPassOp = VK_STENCIL_OP_REPLACE,
               .stencilCompareOp = VK_COMPARE_OP_ALWAYS,
            },
         },
         .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = (VkPipelineColorBlendAttachmentState []) {
               { .channelWriteMask = VK_CHANNEL_A_BIT |
                    VK_CHANNEL_R_BIT | VK_CHANNEL_G_BIT | VK_CHANNEL_B_BIT },
            }
         },
         .pDynamicState = &(VkPipelineDynamicStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 9,
            .pDynamicStates = (VkDynamicState[]) {
               VK_DYNAMIC_STATE_VIEWPORT,
               VK_DYNAMIC_STATE_SCISSOR,
               VK_DYNAMIC_STATE_LINE_WIDTH,
               VK_DYNAMIC_STATE_DEPTH_BIAS,
               VK_DYNAMIC_STATE_BLEND_CONSTANTS,
               VK_DYNAMIC_STATE_DEPTH_BOUNDS,
               VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
               VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
               VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            },
         },
         .flags = 0,
         .renderPass = anv_render_pass_to_handle(&anv_meta_dummy_renderpass),
         .subpass = 0,
      },
      &(struct anv_graphics_pipeline_create_info) {
         .use_repclear = true,
         .disable_viewport = true,
         .disable_vs = true,
         .use_rectlist = true
      },
      &device->meta_state.clear.pipeline);

   anv_DestroyShader(anv_device_to_handle(device), vs);
   anv_DestroyShader(anv_device_to_handle(device), fs);
   ralloc_free(vsm.nir);
   ralloc_free(fsm.nir);
}

void anv_CmdClearColorImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     _image,
    VkImageLayout                               imageLayout,
    const VkClearColorValue*                    pColor,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_image, image, _image);
   struct anv_meta_saved_state saved_state;

   anv_meta_save(&saved_state, cmd_buffer,
                 (1 << VK_DYNAMIC_STATE_VIEWPORT));
   cmd_buffer->state.dynamic.viewport.count = 0;

   for (uint32_t r = 0; r < rangeCount; r++) {
      for (uint32_t l = 0; l < pRanges[r].mipLevels; l++) {
         for (uint32_t s = 0; s < pRanges[r].arraySize; s++) {
            struct anv_image_view iview;
            anv_image_view_init(&iview, cmd_buffer->device,
               &(VkImageViewCreateInfo) {
                  .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                  .image = _image,
                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                  .format = image->format->vk_format,
                  .channels = {
                     VK_CHANNEL_SWIZZLE_R,
                     VK_CHANNEL_SWIZZLE_G,
                     VK_CHANNEL_SWIZZLE_B,
                     VK_CHANNEL_SWIZZLE_A
                  },
                  .subresourceRange = {
                     .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                     .baseMipLevel = pRanges[r].baseMipLevel + l,
                     .mipLevels = 1,
                     .baseArrayLayer = pRanges[r].baseArrayLayer + s,
                     .arraySize = 1
                  },
               },
               cmd_buffer);

            VkFramebuffer fb;
            anv_CreateFramebuffer(anv_device_to_handle(cmd_buffer->device),
               &(VkFramebufferCreateInfo) {
                  .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                  .attachmentCount = 1,
                  .pAttachments = (VkImageView[]) {
                     anv_image_view_to_handle(&iview),
                  },
                  .width = iview.extent.width,
                  .height = iview.extent.height,
                  .layers = 1
               }, &fb);

            VkRenderPass pass;
            anv_CreateRenderPass(anv_device_to_handle(cmd_buffer->device),
               &(VkRenderPassCreateInfo) {
                  .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                  .attachmentCount = 1,
                  .pAttachments = &(VkAttachmentDescription) {
                     .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION,
                     .format = iview.format->vk_format,
                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                     .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
                     .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
                  },
                  .subpassCount = 1,
                  .pSubpasses = &(VkSubpassDescription) {
                     .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION,
                     .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                     .inputCount = 0,
                     .colorCount = 1,
                     .pColorAttachments = &(VkAttachmentReference) {
                        .attachment = 0,
                        .layout = VK_IMAGE_LAYOUT_GENERAL,
                     },
                     .pResolveAttachments = NULL,
                     .depthStencilAttachment = (VkAttachmentReference) {
                        .attachment = VK_ATTACHMENT_UNUSED,
                        .layout = VK_IMAGE_LAYOUT_GENERAL,
                     },
                     .preserveCount = 1,
                     .pPreserveAttachments = &(VkAttachmentReference) {
                        .attachment = 0,
                        .layout = VK_IMAGE_LAYOUT_GENERAL,
                     },
                  },
                  .dependencyCount = 0,
               }, &pass);

            ANV_CALL(CmdBeginRenderPass)(anv_cmd_buffer_to_handle(cmd_buffer),
               &(VkRenderPassBeginInfo) {
                  .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                  .renderArea = {
                     .offset = { 0, 0, },
                     .extent = {
                        .width = iview.extent.width,
                        .height = iview.extent.height,
                     },
                  },
                  .renderPass = pass,
                  .framebuffer = fb,
                  .clearValueCount = 1,
                  .pClearValues = NULL,
               }, VK_RENDER_PASS_CONTENTS_INLINE);

            struct clear_instance_data instance_data = {
               .vue_header = {
                  .RTAIndex = 0,
                  .ViewportIndex = 0,
                  .PointWidth = 0.0
               },
               .color = *pColor,
            };

            meta_emit_clear(cmd_buffer, 1, &instance_data,
                            (VkClearDepthStencilValue) {0});

            ANV_CALL(CmdEndRenderPass)(anv_cmd_buffer_to_handle(cmd_buffer));
         }
      }
   }

   anv_meta_restore(&saved_state, cmd_buffer);
}

void anv_CmdClearDepthStencilImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     image,
    VkImageLayout                               imageLayout,
    const VkClearDepthStencilValue*             pDepthStencil,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
   stub();
}

void anv_CmdClearColorAttachment(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    colorAttachment,
    VkImageLayout                               imageLayout,
    const VkClearColorValue*                    pColor,
    uint32_t                                    rectCount,
    const VkRect3D*                             pRects)
{
   stub();
}

void anv_CmdClearDepthStencilAttachment(
    VkCmdBuffer                                 cmdBuffer,
    VkImageAspectFlags                          aspectMask,
    VkImageLayout                               imageLayout,
    const VkClearDepthStencilValue*             pDepthStencil,
    uint32_t                                    rectCount,
    const VkRect3D*                             pRects)
{
   stub();
}
