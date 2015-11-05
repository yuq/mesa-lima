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

/** Vertex attributes for color clears.  */
struct color_clear_vattrs {
   struct anv_vue_header vue_header;
   float position[2]; /**< 3DPRIM_RECTLIST */
   VkClearColorValue color;
};

/** Vertex attributes for depthstencil clears.  */
struct depthstencil_clear_vattrs {
   struct anv_vue_header vue_header;
   float position[2]; /*<< 3DPRIM_RECTLIST */
};

static void
meta_clear_begin(struct anv_meta_saved_state *saved_state,
                 struct anv_cmd_buffer *cmd_buffer)
{
   anv_meta_save(saved_state, cmd_buffer,
                 (1 << VK_DYNAMIC_STATE_VIEWPORT) |
                 (1 << VK_DYNAMIC_STATE_SCISSOR) |
                 (1 << VK_DYNAMIC_STATE_STENCIL_REFERENCE));

   cmd_buffer->state.dynamic.viewport.count = 0;
   cmd_buffer->state.dynamic.scissor.count = 0;
}

static void
meta_clear_end(struct anv_meta_saved_state *saved_state,
               struct anv_cmd_buffer *cmd_buffer)
{
   anv_meta_restore(saved_state, cmd_buffer);
}

static void
build_color_shaders(struct nir_shader **out_vs,
                    struct nir_shader **out_fs)
{
   nir_builder vs_b;
   nir_builder fs_b;

   nir_builder_init_simple_shader(&vs_b, MESA_SHADER_VERTEX);
   nir_builder_init_simple_shader(&fs_b, MESA_SHADER_FRAGMENT);

   const struct glsl_type *position_type = glsl_vec4_type();
   const struct glsl_type *color_type = glsl_vec4_type();

   nir_variable *vs_in_pos =
      nir_variable_create(vs_b.shader, nir_var_shader_in, position_type,
                          "a_position");
   vs_in_pos->data.location = VERT_ATTRIB_GENERIC0;

   nir_variable *vs_out_pos =
      nir_variable_create(vs_b.shader, nir_var_shader_out, position_type,
                          "gl_Position");
   vs_out_pos->data.location = VARYING_SLOT_POS;

   nir_variable *vs_in_color =
      nir_variable_create(vs_b.shader, nir_var_shader_in, color_type,
                          "a_color");
   vs_in_color->data.location = VERT_ATTRIB_GENERIC1;

   nir_variable *vs_out_color =
      nir_variable_create(vs_b.shader, nir_var_shader_out, color_type,
                          "v_color");
   vs_out_color->data.location = VARYING_SLOT_VAR0;
   vs_out_color->data.interpolation = INTERP_QUALIFIER_FLAT;

   nir_variable *fs_in_color =
      nir_variable_create(fs_b.shader, nir_var_shader_in, color_type,
                          "v_color");
   fs_in_color->data.location = vs_out_color->data.location;
   fs_in_color->data.interpolation = vs_out_color->data.interpolation;

   nir_variable *fs_out_color =
      nir_variable_create(fs_b.shader, nir_var_shader_out, color_type,
                          "f_color");
   fs_out_color->data.location = FRAG_RESULT_DATA0;

   nir_copy_var(&vs_b, vs_out_pos, vs_in_pos);
   nir_copy_var(&vs_b, vs_out_color, vs_in_color);
   nir_copy_var(&fs_b, fs_out_color, fs_in_color);

   *out_vs = vs_b.shader;
   *out_fs = fs_b.shader;
}

static struct anv_pipeline *
create_pipeline(struct anv_device *device,
                struct nir_shader *vs_nir,
                struct nir_shader *fs_nir,
                const VkPipelineVertexInputStateCreateInfo *vi_state,
                const VkPipelineDepthStencilStateCreateInfo *ds_state,
                const VkPipelineColorBlendStateCreateInfo *cb_state)
{
   VkDevice device_h = anv_device_to_handle(device);

   struct anv_shader_module vs_m = { .nir = vs_nir };
   struct anv_shader_module fs_m = { .nir = fs_nir };

   VkShader vs_h;
   ANV_CALL(CreateShader)(device_h,
      &(VkShaderCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
         .module = anv_shader_module_to_handle(&vs_m),
         .pName = "main",
      },
      &vs_h);

   VkShader fs_h;
   ANV_CALL(CreateShader)(device_h,
      &(VkShaderCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
         .module = anv_shader_module_to_handle(&fs_m),
         .pName = "main",
      },
      &fs_h);

   VkPipeline pipeline_h;
   anv_graphics_pipeline_create(device_h,
      &(VkGraphicsPipelineCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2,
         .pStages = (VkPipelineShaderStageCreateInfo[]) {
            {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_VERTEX,
               .shader = vs_h,
            },
            {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_FRAGMENT,
               .shader = fs_h,
            },
         },
         .pVertexInputState = vi_state,
         .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .primitiveRestartEnable = false,
         },
         .pViewportState = &(VkPipelineViewportStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = NULL, /* dynamic */
            .scissorCount = 1,
            .pScissors = NULL, /* dynamic */
         },
         .pRasterState = &(VkPipelineRasterStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTER_STATE_CREATE_INFO,
            .depthClipEnable = false,
            .rasterizerDiscardEnable = false,
            .fillMode = VK_FILL_MODE_SOLID,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CCW,
            .depthBiasEnable = false,
            .depthClipEnable = false,
         },
         .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterSamples = 1, /* FINISHME: Multisampling */
            .sampleShadingEnable = false,
            .pSampleMask = (VkSampleMask[]) { UINT32_MAX },
         },
         .pDepthStencilState = ds_state,
         .pColorBlendState = cb_state,
         .pDynamicState = &(VkPipelineDynamicStateCreateInfo) {
            /* The meta clear pipeline declares all state as dynamic.
             * As a consequence, vkCmdBindPipeline writes no dynamic state
             * to the cmd buffer. Therefore, at the end of the meta clear,
             * we need only restore dynamic state was vkCmdSet.
             */
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
      &pipeline_h);

   ANV_CALL(DestroyShader)(device_h, vs_h);
   ANV_CALL(DestroyShader)(device_h, fs_h);

   ralloc_free(vs_nir);
   ralloc_free(fs_nir);

   return anv_pipeline_from_handle(pipeline_h);
}

static void
init_color_pipeline(struct anv_device *device)
{
   struct nir_shader *vs_nir;
   struct nir_shader *fs_nir;
   build_color_shaders(&vs_nir, &fs_nir);

   const VkPipelineVertexInputStateCreateInfo vi_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .bindingCount = 1,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .strideInBytes = sizeof(struct color_clear_vattrs),
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         },
      },
      .attributeCount = 3,
      .pVertexAttributeDescriptions = (VkVertexInputAttributeDescription[]) {
         {
            /* VUE Header */
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_UINT,
            .offsetInBytes = offsetof(struct color_clear_vattrs, vue_header),
         },
         {
            /* Position */
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offsetInBytes = offsetof(struct color_clear_vattrs, position),
         },
         {
            /* Color */
            .location = 2,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offsetInBytes = offsetof(struct color_clear_vattrs, color),
         },
      },
   };

   const VkPipelineDepthStencilStateCreateInfo ds_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = false,
      .depthWriteEnable = false,
      .depthBoundsTestEnable = false,
      .stencilTestEnable = false,
   };

   const VkPipelineColorBlendStateCreateInfo cb_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .alphaToCoverageEnable = false,
      .alphaToOneEnable = false,
      .logicOpEnable = false,
      .attachmentCount = 1,
      .pAttachments = (VkPipelineColorBlendAttachmentState []) {
         {
            .blendEnable = false,
            .channelWriteMask = VK_CHANNEL_A_BIT |
                                VK_CHANNEL_R_BIT |
                                VK_CHANNEL_G_BIT |
                                VK_CHANNEL_B_BIT,
         },
      },
   };

   device->meta_state.clear.color_pipeline =
      create_pipeline(device, vs_nir, fs_nir, &vi_state, &ds_state,
                      &cb_state);
}

static void
emit_load_color_clear(struct anv_cmd_buffer *cmd_buffer,
                      uint32_t attachment,
                      VkClearColorValue clear_value)
{
   struct anv_device *device = cmd_buffer->device;
   VkCmdBuffer cmd_buffer_h = anv_cmd_buffer_to_handle(cmd_buffer);
   const struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   VkPipeline pipeline_h =
      anv_pipeline_to_handle(device->meta_state.clear.color_pipeline);

   const struct color_clear_vattrs vertex_data[3] = {
      {
         .vue_header = { 0 },
         .position = { 0.0, 0.0 },
         .color = clear_value,
      },
      {
         .vue_header = { 0 },
         .position = { fb->width, 0.0 },
         .color = clear_value,
      },
      {
         .vue_header = { 0 },
         .position = { fb->width, fb->height },
         .color = clear_value,
      },
   };

   struct anv_state state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, sizeof(vertex_data), 16);
   memcpy(state.map, vertex_data, sizeof(vertex_data));

   struct anv_buffer vertex_buffer = {
      .device = device,
      .size = sizeof(vertex_data),
      .bo = &device->dynamic_state_block_pool.bo,
      .offset = state.offset,
   };

   anv_cmd_buffer_begin_subpass(cmd_buffer,
      &(struct anv_subpass) {
         .color_count = 1,
         .color_attachments = (uint32_t[]) { attachment },
         .depth_stencil_attachment = VK_ATTACHMENT_UNUSED,
      });

   ANV_CALL(CmdSetViewport)(cmd_buffer_h, 1,
      (VkViewport[]) {
         {
            .originX = 0,
            .originY = 0,
            .width = fb->width,
            .height = fb->height,
            .minDepth = 0.0,
            .maxDepth = 1.0,
         },
      });

   ANV_CALL(CmdSetScissor)(cmd_buffer_h, 1,
      (VkRect2D[]) {
         {
            .offset = { 0, 0 },
            .extent = { fb->width, fb->height },
         }
      });

   ANV_CALL(CmdBindVertexBuffers)(cmd_buffer_h, 0, 1,
      (VkBuffer[]) { anv_buffer_to_handle(&vertex_buffer) },
      (VkDeviceSize[]) { 0 });

   if (cmd_buffer->state.pipeline != device->meta_state.clear.color_pipeline) {
      ANV_CALL(CmdBindPipeline)(cmd_buffer_h, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_h);
   }

   ANV_CALL(CmdDraw)(cmd_buffer_h, 3, 1, 0, 0);
}


static void
build_depthstencil_shaders(struct nir_shader **out_vs,
                           struct nir_shader **out_fs)
{
   nir_builder vs_b;
   nir_builder fs_b;

   nir_builder_init_simple_shader(&vs_b, MESA_SHADER_VERTEX);
   nir_builder_init_simple_shader(&fs_b, MESA_SHADER_FRAGMENT);

   const struct glsl_type *position_type = glsl_vec4_type();

   nir_variable *vs_in_pos =
      nir_variable_create(vs_b.shader, nir_var_shader_in, position_type,
                          "a_position");
   vs_in_pos->data.location = VERT_ATTRIB_GENERIC0;

   nir_variable *vs_out_pos =
      nir_variable_create(vs_b.shader, nir_var_shader_out, position_type,
                          "gl_Position");
   vs_out_pos->data.location = VARYING_SLOT_POS;

   nir_copy_var(&vs_b, vs_out_pos, vs_in_pos);

   *out_vs = vs_b.shader;
   *out_fs = fs_b.shader;
}

static struct anv_pipeline *
create_depthstencil_pipeline(struct anv_device *device,
                             VkImageAspectFlags aspects)
{
   struct nir_shader *vs_nir;
   struct nir_shader *fs_nir;

   build_depthstencil_shaders(&vs_nir, &fs_nir);

   const VkPipelineVertexInputStateCreateInfo vi_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .bindingCount = 1,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .strideInBytes = sizeof(struct depthstencil_clear_vattrs),
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         },
      },
      .attributeCount = 2,
      .pVertexAttributeDescriptions = (VkVertexInputAttributeDescription[]) {
         {
            /* VUE Header */
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_UINT,
            .offsetInBytes = offsetof(struct depthstencil_clear_vattrs, vue_header),
         },
         {
            /* Position */
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offsetInBytes = offsetof(struct depthstencil_clear_vattrs, position),
         },
      },
   };

   const VkPipelineDepthStencilStateCreateInfo ds_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT),
      .depthCompareOp = VK_COMPARE_OP_ALWAYS,
      .depthWriteEnable = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT),
      .depthBoundsTestEnable = false,
      .stencilTestEnable = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT),
      .front = {
         .stencilPassOp = VK_STENCIL_OP_REPLACE,
         .stencilCompareOp = VK_COMPARE_OP_ALWAYS,
         .stencilWriteMask = UINT32_MAX,
         .stencilReference = 0, /* dynamic */
      },
      .back = { 0 /* dont care */ },
   };

   const VkPipelineColorBlendStateCreateInfo cb_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .alphaToCoverageEnable = false,
      .alphaToOneEnable = false,
      .logicOpEnable = false,
      .attachmentCount = 0,
      .pAttachments = NULL,
   };

   return create_pipeline(device, vs_nir, fs_nir, &vi_state, &ds_state,
                          &cb_state);
}

static void
emit_load_depthstencil_clear(struct anv_cmd_buffer *cmd_buffer,
                             uint32_t attachment,
                             VkImageAspectFlags aspects,
                             VkClearDepthStencilValue clear_value)
{
   struct anv_device *device = cmd_buffer->device;
   VkCmdBuffer cmd_buffer_h = anv_cmd_buffer_to_handle(cmd_buffer);
   const struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;

   const struct depthstencil_clear_vattrs vertex_data[3] = {
      {
         .vue_header = { 0 },
         .position = { 0.0, 0.0 },
      },
      {
         .vue_header = { 0 },
         .position = { fb->width, 0.0 },
      },
      {
         .vue_header = { 0 },
         .position = { fb->width, fb->height },
      },
   };

   struct anv_state state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, sizeof(vertex_data), 16);
   memcpy(state.map, vertex_data, sizeof(vertex_data));

   struct anv_buffer vertex_buffer = {
      .device = device,
      .size = sizeof(vertex_data),
      .bo = &device->dynamic_state_block_pool.bo,
      .offset = state.offset,
   };

   anv_cmd_buffer_begin_subpass(cmd_buffer,
      &(struct anv_subpass) {
         .color_count = 0,
         .depth_stencil_attachment = attachment,
      });

   ANV_CALL(CmdSetViewport)(cmd_buffer_h, 1,
      (VkViewport[]) {
         {
            .originX = 0,
            .originY = 0,
            .width = fb->width,
            .height = fb->height,

            /* Ignored when clearing only stencil. */
            .minDepth = clear_value.depth,
            .maxDepth = clear_value.depth,
         },
      });

   ANV_CALL(CmdSetScissor)(cmd_buffer_h, 1,
      (VkRect2D[]) {
         {
            .offset = { 0, 0 },
            .extent = { fb->width, fb->height },
         }
      });

   if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      ANV_CALL(CmdSetStencilReference)(cmd_buffer_h, VK_STENCIL_FACE_FRONT_BIT,
                                       clear_value.stencil);
   }

   ANV_CALL(CmdBindVertexBuffers)(cmd_buffer_h, 0, 1,
      (VkBuffer[]) { anv_buffer_to_handle(&vertex_buffer) },
      (VkDeviceSize[]) { 0 });

   struct anv_pipeline *pipeline;
   switch (aspects) {
   case VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT:
      pipeline = device->meta_state.clear.depthstencil_pipeline;
      break;
   case VK_IMAGE_ASPECT_DEPTH_BIT:
      pipeline = device->meta_state.clear.depth_only_pipeline;
      break;
   case VK_IMAGE_ASPECT_STENCIL_BIT:
      pipeline = device->meta_state.clear.stencil_only_pipeline;
      break;
   default:
      unreachable("expected depth or stencil aspect");
   }

   if (cmd_buffer->state.pipeline != pipeline) {
      ANV_CALL(CmdBindPipeline)(cmd_buffer_h, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                anv_pipeline_to_handle(pipeline));
   }

   ANV_CALL(CmdDraw)(cmd_buffer_h, 3, 1, 0, 0);
}

static void
init_depthstencil_pipelines(struct anv_device *device)
{
   device->meta_state.clear.depth_only_pipeline =
      create_depthstencil_pipeline(device, VK_IMAGE_ASPECT_DEPTH_BIT);

   device->meta_state.clear.stencil_only_pipeline =
      create_depthstencil_pipeline(device, VK_IMAGE_ASPECT_STENCIL_BIT);

   device->meta_state.clear.depthstencil_pipeline =
      create_depthstencil_pipeline(device, VK_IMAGE_ASPECT_DEPTH_BIT |
                                           VK_IMAGE_ASPECT_STENCIL_BIT);
}

void
anv_device_init_meta_clear_state(struct anv_device *device)
{
   init_color_pipeline(device);
   init_depthstencil_pipelines(device);
}

void
anv_device_finish_meta_clear_state(struct anv_device *device)
{
   VkDevice device_h = anv_device_to_handle(device);

   ANV_CALL(DestroyPipeline)(device_h,
      anv_pipeline_to_handle(device->meta_state.clear.color_pipeline));
   ANV_CALL(DestroyPipeline)(device_h,
      anv_pipeline_to_handle(device->meta_state.clear.depth_only_pipeline));
   ANV_CALL(DestroyPipeline)(device_h,
      anv_pipeline_to_handle(device->meta_state.clear.stencil_only_pipeline));
   ANV_CALL(DestroyPipeline)(device_h,
      anv_pipeline_to_handle(device->meta_state.clear.depthstencil_pipeline));
}

void
anv_cmd_buffer_clear_attachments(struct anv_cmd_buffer *cmd_buffer,
                                 struct anv_render_pass *pass,
                                 const VkClearValue *clear_values)
{
   struct anv_meta_saved_state saved_state;

   meta_clear_begin(&saved_state, cmd_buffer);

   for (uint32_t a = 0; a < pass->attachment_count; ++a) {
      struct anv_render_pass_attachment *att = &pass->attachments[a];

      if (anv_format_is_color(att->format)) {
         if (att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            emit_load_color_clear(cmd_buffer, a, clear_values[a].color);
         }
      } else {
         VkImageAspectFlags aspects = 0;

         if (att->format->depth_format &&
             att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
         }

         if (att->format->has_stencil &&
             att->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
         }

         emit_load_depthstencil_clear(cmd_buffer, a, aspects,
                                      clear_values[a].depthStencil);
      }
   }

   meta_clear_end(&saved_state, cmd_buffer);
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

   meta_clear_begin(&saved_state, cmd_buffer);

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
                  .pClearValues = (VkClearValue[]) {
                     { .color = *pColor },
                  },
               }, VK_RENDER_PASS_CONTENTS_INLINE);

            ANV_CALL(CmdEndRenderPass)(anv_cmd_buffer_to_handle(cmd_buffer));
         }
      }
   }

   meta_clear_end(&saved_state, cmd_buffer);
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
