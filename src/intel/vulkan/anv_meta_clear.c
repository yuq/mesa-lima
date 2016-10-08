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
#include "anv_private.h"
#include "nir/nir_builder.h"

#include "util/format_rgb9e5.h"

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
build_color_shaders(struct nir_shader **out_vs,
                    struct nir_shader **out_fs,
                    uint32_t frag_output)
{
   nir_builder vs_b;
   nir_builder fs_b;

   nir_builder_init_simple_shader(&vs_b, NULL, MESA_SHADER_VERTEX, NULL);
   nir_builder_init_simple_shader(&fs_b, NULL, MESA_SHADER_FRAGMENT, NULL);

   vs_b.shader->info.name = ralloc_strdup(vs_b.shader, "meta_clear_color_vs");
   fs_b.shader->info.name = ralloc_strdup(fs_b.shader, "meta_clear_color_fs");

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
   vs_out_color->data.interpolation = INTERP_MODE_FLAT;

   nir_variable *fs_in_color =
      nir_variable_create(fs_b.shader, nir_var_shader_in, color_type,
                          "v_color");
   fs_in_color->data.location = vs_out_color->data.location;
   fs_in_color->data.interpolation = vs_out_color->data.interpolation;

   nir_variable *fs_out_color =
      nir_variable_create(fs_b.shader, nir_var_shader_out, color_type,
                          "f_color");
   fs_out_color->data.location = FRAG_RESULT_DATA0 + frag_output;

   nir_copy_var(&vs_b, vs_out_pos, vs_in_pos);
   nir_copy_var(&vs_b, vs_out_color, vs_in_color);
   nir_copy_var(&fs_b, fs_out_color, fs_in_color);

   *out_vs = vs_b.shader;
   *out_fs = fs_b.shader;
}

static VkResult
create_pipeline(struct anv_device *device,
                uint32_t samples,
                struct nir_shader *vs_nir,
                struct nir_shader *fs_nir,
                const VkPipelineVertexInputStateCreateInfo *vi_state,
                const VkPipelineDepthStencilStateCreateInfo *ds_state,
                const VkPipelineColorBlendStateCreateInfo *cb_state,
                const VkAllocationCallbacks *alloc,
                bool use_repclear,
                struct anv_pipeline **pipeline)
{
   VkDevice device_h = anv_device_to_handle(device);
   VkResult result;

   struct anv_shader_module vs_m = { .nir = vs_nir };
   struct anv_shader_module fs_m = { .nir = fs_nir };

   VkPipeline pipeline_h = VK_NULL_HANDLE;
   result = anv_graphics_pipeline_create(device_h,
      VK_NULL_HANDLE,
      &(VkGraphicsPipelineCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = fs_nir ? 2 : 1,
         .pStages = (VkPipelineShaderStageCreateInfo[]) {
            {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_VERTEX_BIT,
               .module = anv_shader_module_to_handle(&vs_m),
               .pName = "main",
            },
            {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
               .module = anv_shader_module_to_handle(&fs_m),
               .pName = "main",
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
         .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .rasterizerDiscardEnable = false,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = false,
            .depthClampEnable = true,
         },
         .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = samples,
            .sampleShadingEnable = false,
            .pSampleMask = (VkSampleMask[]) { ~0 },
            .alphaToCoverageEnable = false,
            .alphaToOneEnable = false,
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
            .dynamicStateCount = 8,
            .pDynamicStates = (VkDynamicState[]) {
               /* Everything except stencil write mask */
               VK_DYNAMIC_STATE_VIEWPORT,
               VK_DYNAMIC_STATE_SCISSOR,
               VK_DYNAMIC_STATE_LINE_WIDTH,
               VK_DYNAMIC_STATE_DEPTH_BIAS,
               VK_DYNAMIC_STATE_BLEND_CONSTANTS,
               VK_DYNAMIC_STATE_DEPTH_BOUNDS,
               VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
               VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            },
         },
         .flags = 0,
         .renderPass = anv_render_pass_to_handle(&anv_meta_dummy_renderpass),
         .subpass = 0,
      },
      &(struct anv_graphics_pipeline_create_info) {
         .color_attachment_count = MAX_RTS,
         .use_repclear = use_repclear,
         .disable_vs = true,
         .use_rectlist = true
      },
      alloc,
      &pipeline_h);

   ralloc_free(vs_nir);
   ralloc_free(fs_nir);

   *pipeline = anv_pipeline_from_handle(pipeline_h);

   return result;
}

static VkResult
create_color_pipeline(struct anv_device *device,
                      uint32_t samples,
                      uint32_t frag_output,
                      struct anv_pipeline **pipeline)
{
   struct nir_shader *vs_nir;
   struct nir_shader *fs_nir;
   build_color_shaders(&vs_nir, &fs_nir, frag_output);

   const VkPipelineVertexInputStateCreateInfo vi_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .stride = sizeof(struct color_clear_vattrs),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
         },
      },
      .vertexAttributeDescriptionCount = 3,
      .pVertexAttributeDescriptions = (VkVertexInputAttributeDescription[]) {
         {
            /* VUE Header */
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_UINT,
            .offset = offsetof(struct color_clear_vattrs, vue_header),
         },
         {
            /* Position */
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = offsetof(struct color_clear_vattrs, position),
         },
         {
            /* Color */
            .location = 2,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset = offsetof(struct color_clear_vattrs, color),
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

   VkPipelineColorBlendAttachmentState blend_attachment_state[MAX_RTS] = { 0 };
   blend_attachment_state[frag_output] = (VkPipelineColorBlendAttachmentState) {
      .blendEnable = false,
      .colorWriteMask = VK_COLOR_COMPONENT_A_BIT |
                        VK_COLOR_COMPONENT_R_BIT |
                        VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT,
   };

   const VkPipelineColorBlendStateCreateInfo cb_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .logicOpEnable = false,
      .attachmentCount = MAX_RTS,
      .pAttachments = blend_attachment_state
   };

   /* Use the repclear shader.  Since the NIR shader we are providing has
    * exactly one output, that output will get compacted down to binding
    * table entry 0.  The hard-coded repclear shader is then exactly what
    * we want regardless of what attachment we are actually clearing.
    */
   return
      create_pipeline(device, samples, vs_nir, fs_nir, &vi_state, &ds_state,
                      &cb_state, &device->meta_state.alloc,
                      /*use_repclear*/ true, pipeline);
}

static void
destroy_pipeline(struct anv_device *device, struct anv_pipeline *pipeline)
{
   if (!pipeline)
      return;

   ANV_CALL(DestroyPipeline)(anv_device_to_handle(device),
                             anv_pipeline_to_handle(pipeline),
                             &device->meta_state.alloc);
}

void
anv_device_finish_meta_clear_state(struct anv_device *device)
{
   struct anv_meta_state *state = &device->meta_state;

   for (uint32_t i = 0; i < ARRAY_SIZE(state->clear); ++i) {
      for (uint32_t j = 0; j < ARRAY_SIZE(state->clear[i].color_pipelines); ++j) {
         destroy_pipeline(device, state->clear[i].color_pipelines[j]);
      }

      destroy_pipeline(device, state->clear[i].depth_only_pipeline);
      destroy_pipeline(device, state->clear[i].stencil_only_pipeline);
      destroy_pipeline(device, state->clear[i].depthstencil_pipeline);
   }
}

static void
build_depthstencil_shader(struct nir_shader **out_vs)
{
   nir_builder vs_b;

   nir_builder_init_simple_shader(&vs_b, NULL, MESA_SHADER_VERTEX, NULL);

   vs_b.shader->info.name = ralloc_strdup(vs_b.shader, "meta_clear_depthstencil_vs");

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
}

static VkResult
create_depthstencil_pipeline(struct anv_device *device,
                             VkImageAspectFlags aspects,
                             uint32_t samples,
                             struct anv_pipeline **pipeline)
{
   struct nir_shader *vs_nir;

   build_depthstencil_shader(&vs_nir);

   const VkPipelineVertexInputStateCreateInfo vi_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .stride = sizeof(struct depthstencil_clear_vattrs),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
         },
      },
      .vertexAttributeDescriptionCount = 2,
      .pVertexAttributeDescriptions = (VkVertexInputAttributeDescription[]) {
         {
            /* VUE Header */
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_UINT,
            .offset = offsetof(struct depthstencil_clear_vattrs, vue_header),
         },
         {
            /* Position */
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = offsetof(struct depthstencil_clear_vattrs, position),
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
         .passOp = VK_STENCIL_OP_REPLACE,
         .compareOp = VK_COMPARE_OP_ALWAYS,
         .writeMask = UINT32_MAX,
         .reference = 0, /* dynamic */
      },
      .back = { 0 /* dont care */ },
   };

   const VkPipelineColorBlendStateCreateInfo cb_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .logicOpEnable = false,
      .attachmentCount = 0,
      .pAttachments = NULL,
   };

   return create_pipeline(device, samples, vs_nir, NULL, &vi_state, &ds_state,
                          &cb_state, &device->meta_state.alloc,
                          /*use_repclear*/ true, pipeline);
}

VkResult
anv_device_init_meta_clear_state(struct anv_device *device)
{
   VkResult res;
   struct anv_meta_state *state = &device->meta_state;

   zero(device->meta_state.clear);

   for (uint32_t i = 0; i < ARRAY_SIZE(state->clear); ++i) {
      uint32_t samples = 1 << i;

      for (uint32_t j = 0; j < ARRAY_SIZE(state->clear[i].color_pipelines); ++j) {
         res = create_color_pipeline(device, samples, /* frag_output */ j,
                                     &state->clear[i].color_pipelines[j]);
         if (res != VK_SUCCESS)
            goto fail;
      }

      res = create_depthstencil_pipeline(device,
                                         VK_IMAGE_ASPECT_DEPTH_BIT, samples,
                                         &state->clear[i].depth_only_pipeline);
      if (res != VK_SUCCESS)
         goto fail;

      res = create_depthstencil_pipeline(device,
                                         VK_IMAGE_ASPECT_STENCIL_BIT, samples,
                                         &state->clear[i].stencil_only_pipeline);
      if (res != VK_SUCCESS)
         goto fail;

      res = create_depthstencil_pipeline(device,
                                         VK_IMAGE_ASPECT_DEPTH_BIT |
                                         VK_IMAGE_ASPECT_STENCIL_BIT, samples,
                                         &state->clear[i].depthstencil_pipeline);
      if (res != VK_SUCCESS)
         goto fail;
   }

   return VK_SUCCESS;

fail:
   anv_device_finish_meta_clear_state(device);
   return res;
}
