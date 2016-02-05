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

#include "anv_meta.h"
#include "anv_private.h"
#include "nir/nir_builder.h"

struct anv_render_pass anv_meta_dummy_renderpass = {0};

static nir_shader *
build_nir_vertex_shader(bool attr_flat)
{
   nir_builder b;

   const struct glsl_type *vertex_type = glsl_vec4_type();

   nir_builder_init_simple_shader(&b, NULL, MESA_SHADER_VERTEX, NULL);
   b.shader->info.name = ralloc_strdup(b.shader, "meta_blit_vs");

   nir_variable *pos_in = nir_variable_create(b.shader, nir_var_shader_in,
                                              vertex_type, "a_pos");
   pos_in->data.location = VERT_ATTRIB_GENERIC0;
   nir_variable *pos_out = nir_variable_create(b.shader, nir_var_shader_out,
                                               vertex_type, "gl_Position");
   pos_out->data.location = VARYING_SLOT_POS;
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
build_nir_copy_fragment_shader(enum glsl_sampler_dim tex_dim)
{
   nir_builder b;

   nir_builder_init_simple_shader(&b, NULL, MESA_SHADER_FRAGMENT, NULL);
   b.shader->info.name = ralloc_strdup(b.shader, "meta_blit_fs");

   const struct glsl_type *color_type = glsl_vec4_type();

   nir_variable *tex_pos_in = nir_variable_create(b.shader, nir_var_shader_in,
                                                  glsl_vec4_type(), "v_attr");
   tex_pos_in->data.location = VARYING_SLOT_VAR0;

   /* Swizzle the array index which comes in as Z coordinate into the right
    * position.
    */
   unsigned swz[] = { 0, (tex_dim == GLSL_SAMPLER_DIM_1D ? 2 : 1), 2 };
   nir_ssa_def *const tex_pos =
      nir_swizzle(&b, nir_load_var(&b, tex_pos_in), swz,
                  (tex_dim == GLSL_SAMPLER_DIM_1D ? 2 : 3), false);

   const struct glsl_type *sampler_type =
      glsl_sampler_type(tex_dim, false, tex_dim != GLSL_SAMPLER_DIM_3D,
                        glsl_get_base_type(color_type));
   nir_variable *sampler = nir_variable_create(b.shader, nir_var_uniform,
                                               sampler_type, "s_tex");
   sampler->data.descriptor_set = 0;
   sampler->data.binding = 0;

   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
   tex->sampler_dim = tex_dim;
   tex->op = nir_texop_tex;
   tex->src[0].src_type = nir_tex_src_coord;
   tex->src[0].src = nir_src_for_ssa(tex_pos);
   tex->dest_type = nir_type_float; /* TODO */
   tex->is_array = glsl_sampler_type_is_array(sampler_type);
   tex->coord_components = tex_pos->num_components;
   tex->sampler = nir_deref_var_create(tex, sampler);

   nir_ssa_dest_init(&tex->instr, &tex->dest, 4, "tex");
   nir_builder_instr_insert(&b, &tex->instr);

   nir_variable *color_out = nir_variable_create(b.shader, nir_var_shader_out,
                                                 color_type, "f_color");
   color_out->data.location = FRAG_RESULT_DATA0;
   nir_store_var(&b, color_out, &tex->dest.ssa, 4);

   return b.shader;
}

void
anv_meta_save(struct anv_meta_saved_state *state,
              const struct anv_cmd_buffer *cmd_buffer,
              uint32_t dynamic_mask)
{
   state->old_pipeline = cmd_buffer->state.pipeline;
   state->old_descriptor_set0 = cmd_buffer->state.descriptors[0];
   memcpy(state->old_vertex_bindings, cmd_buffer->state.vertex_bindings,
          sizeof(state->old_vertex_bindings));

   state->dynamic_mask = dynamic_mask;
   anv_dynamic_state_copy(&state->dynamic, &cmd_buffer->state.dynamic,
                          dynamic_mask);
}

void
anv_meta_restore(const struct anv_meta_saved_state *state,
                 struct anv_cmd_buffer *cmd_buffer)
{
   cmd_buffer->state.pipeline = state->old_pipeline;
   cmd_buffer->state.descriptors[0] = state->old_descriptor_set0;
   memcpy(cmd_buffer->state.vertex_bindings, state->old_vertex_bindings,
          sizeof(state->old_vertex_bindings));

   cmd_buffer->state.vb_dirty |= (1 << ANV_META_VERTEX_BINDING_COUNT) - 1;
   cmd_buffer->state.dirty |= ANV_CMD_DIRTY_PIPELINE;
   cmd_buffer->state.descriptors_dirty |= VK_SHADER_STAGE_FRAGMENT_BIT;

   anv_dynamic_state_copy(&cmd_buffer->state.dynamic, &state->dynamic,
                          state->dynamic_mask);
   cmd_buffer->state.dirty |= state->dynamic_mask;

   /* Since we've used the pipeline with the VS disabled, set
    * need_query_wa. See CmdBeginQuery.
    */
   cmd_buffer->state.need_query_wa = true;
}

VkImageViewType
anv_meta_get_view_type(const struct anv_image *image)
{
   switch (image->type) {
   case VK_IMAGE_TYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
   case VK_IMAGE_TYPE_2D: return VK_IMAGE_VIEW_TYPE_2D;
   case VK_IMAGE_TYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
   default:
      unreachable("bad VkImageViewType");
   }
}

/**
 * When creating a destination VkImageView, this function provides the needed
 * VkImageViewCreateInfo::subresourceRange::baseArrayLayer.
 */
uint32_t
anv_meta_get_iview_layer(const struct anv_image *dest_image,
                         const VkImageSubresourceLayers *dest_subresource,
                         const VkOffset3D *dest_offset)
{
   switch (dest_image->type) {
   case VK_IMAGE_TYPE_1D:
   case VK_IMAGE_TYPE_2D:
      return dest_subresource->baseArrayLayer;
   case VK_IMAGE_TYPE_3D:
      /* HACK: Vulkan does not allow attaching a 3D image to a framebuffer,
       * but meta does it anyway. When doing so, we translate the
       * destination's z offset into an array offset.
       */
      return dest_offset->z;
   default:
      assert(!"bad VkImageType");
      return 0;
   }
}

static VkResult
anv_device_init_meta_blit_state(struct anv_device *device)
{
   VkResult result;

   result = anv_CreateRenderPass(anv_device_to_handle(device),
      &(VkRenderPassCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = &(VkAttachmentDescription) {
            .format = VK_FORMAT_UNDEFINED, /* Our shaders don't care */
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
         },
         .subpassCount = 1,
         .pSubpasses = &(VkSubpassDescription) {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .inputAttachmentCount = 0,
            .colorAttachmentCount = 1,
            .pColorAttachments = &(VkAttachmentReference) {
               .attachment = 0,
               .layout = VK_IMAGE_LAYOUT_GENERAL,
            },
            .pResolveAttachments = NULL,
            .pDepthStencilAttachment = &(VkAttachmentReference) {
               .attachment = VK_ATTACHMENT_UNUSED,
               .layout = VK_IMAGE_LAYOUT_GENERAL,
            },
            .preserveAttachmentCount = 1,
            .pPreserveAttachments = (uint32_t[]) { 0 },
         },
         .dependencyCount = 0,
      }, &device->meta_state.alloc, &device->meta_state.blit.render_pass);
   if (result != VK_SUCCESS)
      goto fail;

   /* We don't use a vertex shader for clearing, but instead build and pass
    * the VUEs directly to the rasterization backend.  However, we do need
    * to provide GLSL source for the vertex shader so that the compiler
    * does not dead-code our inputs.
    */
   struct anv_shader_module vs = {
      .nir = build_nir_vertex_shader(false),
   };

   struct anv_shader_module fs_1d = {
      .nir = build_nir_copy_fragment_shader(GLSL_SAMPLER_DIM_1D),
   };

   struct anv_shader_module fs_2d = {
      .nir = build_nir_copy_fragment_shader(GLSL_SAMPLER_DIM_2D),
   };

   struct anv_shader_module fs_3d = {
      .nir = build_nir_copy_fragment_shader(GLSL_SAMPLER_DIM_3D),
   };

   VkPipelineVertexInputStateCreateInfo vi_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 2,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .stride = 0,
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
         },
         {
            .binding = 1,
            .stride = 5 * sizeof(float),
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
            .offset = 0
         },
         {
            /* Position */
            .location = 1,
            .binding = 1,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = 0
         },
         {
            /* Texture Coordinate */
            .location = 2,
            .binding = 1,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = 8
         }
      }
   };

   VkDescriptorSetLayoutCreateInfo ds_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = (VkDescriptorSetLayoutBinding[]) {
         {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = NULL
         },
      }
   };
   result = anv_CreateDescriptorSetLayout(anv_device_to_handle(device),
                                          &ds_layout_info,
                                          &device->meta_state.alloc,
                                          &device->meta_state.blit.ds_layout);
   if (result != VK_SUCCESS)
      goto fail_render_pass;

   result = anv_CreatePipelineLayout(anv_device_to_handle(device),
      &(VkPipelineLayoutCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .setLayoutCount = 1,
         .pSetLayouts = &device->meta_state.blit.ds_layout,
      },
      &device->meta_state.alloc, &device->meta_state.blit.pipeline_layout);
   if (result != VK_SUCCESS)
      goto fail_descriptor_set_layout;

   VkPipelineShaderStageCreateInfo pipeline_shader_stages[] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = anv_shader_module_to_handle(&vs),
         .pName = "main",
         .pSpecializationInfo = NULL
      }, {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = VK_NULL_HANDLE, /* TEMPLATE VALUE! FILL ME IN! */
         .pName = "main",
         .pSpecializationInfo = NULL
      },
   };

   const VkGraphicsPipelineCreateInfo vk_pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = ARRAY_SIZE(pipeline_shader_stages),
      .pStages = pipeline_shader_stages,
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
      .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
         .rasterizerDiscardEnable = false,
         .polygonMode = VK_POLYGON_MODE_FILL,
         .cullMode = VK_CULL_MODE_NONE,
         .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE
      },
      .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = 1,
         .sampleShadingEnable = false,
         .pSampleMask = (VkSampleMask[]) { UINT32_MAX },
      },
      .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = (VkPipelineColorBlendAttachmentState []) {
            { .colorWriteMask =
                 VK_COLOR_COMPONENT_A_BIT |
                 VK_COLOR_COMPONENT_R_BIT |
                 VK_COLOR_COMPONENT_G_BIT |
                 VK_COLOR_COMPONENT_B_BIT },
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
      .layout = device->meta_state.blit.pipeline_layout,
      .renderPass = device->meta_state.blit.render_pass,
      .subpass = 0,
   };

   const struct anv_graphics_pipeline_create_info anv_pipeline_info = {
      .color_attachment_count = -1,
      .use_repclear = false,
      .disable_viewport = true,
      .disable_scissor = true,
      .disable_vs = true,
      .use_rectlist = true
   };

   pipeline_shader_stages[1].module = anv_shader_module_to_handle(&fs_1d);
   result = anv_graphics_pipeline_create(anv_device_to_handle(device),
      VK_NULL_HANDLE,
      &vk_pipeline_info, &anv_pipeline_info,
      &device->meta_state.alloc, &device->meta_state.blit.pipeline_1d_src);
   if (result != VK_SUCCESS)
      goto fail_pipeline_layout;

   pipeline_shader_stages[1].module = anv_shader_module_to_handle(&fs_2d);
   result = anv_graphics_pipeline_create(anv_device_to_handle(device),
      VK_NULL_HANDLE,
      &vk_pipeline_info, &anv_pipeline_info,
      &device->meta_state.alloc, &device->meta_state.blit.pipeline_2d_src);
   if (result != VK_SUCCESS)
      goto fail_pipeline_1d;

   pipeline_shader_stages[1].module = anv_shader_module_to_handle(&fs_3d);
   result = anv_graphics_pipeline_create(anv_device_to_handle(device),
      VK_NULL_HANDLE,
      &vk_pipeline_info, &anv_pipeline_info,
      &device->meta_state.alloc, &device->meta_state.blit.pipeline_3d_src);
   if (result != VK_SUCCESS)
      goto fail_pipeline_2d;

   ralloc_free(vs.nir);
   ralloc_free(fs_1d.nir);
   ralloc_free(fs_2d.nir);
   ralloc_free(fs_3d.nir);

   return VK_SUCCESS;

 fail_pipeline_2d:
   anv_DestroyPipeline(anv_device_to_handle(device),
                       device->meta_state.blit.pipeline_2d_src,
                       &device->meta_state.alloc);

 fail_pipeline_1d:
   anv_DestroyPipeline(anv_device_to_handle(device),
                       device->meta_state.blit.pipeline_1d_src,
                       &device->meta_state.alloc);

 fail_pipeline_layout:
   anv_DestroyPipelineLayout(anv_device_to_handle(device),
                             device->meta_state.blit.pipeline_layout,
                             &device->meta_state.alloc);
 fail_descriptor_set_layout:
   anv_DestroyDescriptorSetLayout(anv_device_to_handle(device),
                                  device->meta_state.blit.ds_layout,
                                  &device->meta_state.alloc);
 fail_render_pass:
   anv_DestroyRenderPass(anv_device_to_handle(device),
                         device->meta_state.blit.render_pass,
                         &device->meta_state.alloc);

   ralloc_free(vs.nir);
   ralloc_free(fs_1d.nir);
   ralloc_free(fs_2d.nir);
   ralloc_free(fs_3d.nir);
 fail:
   return result;
}

static void
meta_prepare_blit(struct anv_cmd_buffer *cmd_buffer,
                  struct anv_meta_saved_state *saved_state)
{
   anv_meta_save(saved_state, cmd_buffer,
                 (1 << VK_DYNAMIC_STATE_VIEWPORT));
}

struct blit_region {
   VkOffset3D src_offset;
   VkExtent3D src_extent;
   VkOffset3D dest_offset;
   VkExtent3D dest_extent;
};

/* Returns the user-provided VkBufferImageCopy::imageOffset in units of
 * elements rather than texels. One element equals one texel or one block
 * if Image is uncompressed or compressed, respectively.
 */
static struct VkOffset3D
meta_region_offset_el(const struct anv_image * image,
                      const struct VkOffset3D * offset)
{
   const struct isl_format_layout * isl_layout = image->format->isl_layout;
   return (VkOffset3D) {
      .x = offset->x / isl_layout->bw,
      .y = offset->y / isl_layout->bh,
      .z = offset->z / isl_layout->bd,
   };
}

/* Returns the user-provided VkBufferImageCopy::imageExtent in units of
 * elements rather than texels. One element equals one texel or one block
 * if Image is uncompressed or compressed, respectively.
 */
static struct VkExtent3D
meta_region_extent_el(const VkFormat format,
                      const struct VkExtent3D * extent)
{
   const struct isl_format_layout * isl_layout =
      anv_format_for_vk_format(format)->isl_layout;
   return (VkExtent3D) {
      .width  = DIV_ROUND_UP(extent->width , isl_layout->bw),
      .height = DIV_ROUND_UP(extent->height, isl_layout->bh),
      .depth  = DIV_ROUND_UP(extent->depth , isl_layout->bd),
   };
}

static void
meta_emit_blit(struct anv_cmd_buffer *cmd_buffer,
               struct anv_image *src_image,
               struct anv_image_view *src_iview,
               VkOffset3D src_offset,
               VkExtent3D src_extent,
               struct anv_image *dest_image,
               struct anv_image_view *dest_iview,
               VkOffset3D dest_offset,
               VkExtent3D dest_extent,
               VkFilter blit_filter)
{
   struct anv_device *device = cmd_buffer->device;
   VkDescriptorPool dummy_desc_pool = (VkDescriptorPool)1;

   struct blit_vb_data {
      float pos[2];
      float tex_coord[3];
   } *vb_data;

   assert(src_image->samples == dest_image->samples);

   unsigned vb_size = sizeof(struct anv_vue_header) + 3 * sizeof(*vb_data);

   struct anv_state vb_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, vb_size, 16);
   memset(vb_state.map, 0, sizeof(struct anv_vue_header));
   vb_data = vb_state.map + sizeof(struct anv_vue_header);

   vb_data[0] = (struct blit_vb_data) {
      .pos = {
         dest_offset.x + dest_extent.width,
         dest_offset.y + dest_extent.height,
      },
      .tex_coord = {
         (float)(src_offset.x + src_extent.width) / (float)src_iview->extent.width,
         (float)(src_offset.y + src_extent.height) / (float)src_iview->extent.height,
         (float)src_offset.z / (float)src_iview->extent.depth,
      },
   };

   vb_data[1] = (struct blit_vb_data) {
      .pos = {
         dest_offset.x,
         dest_offset.y + dest_extent.height,
      },
      .tex_coord = {
         (float)src_offset.x / (float)src_iview->extent.width,
         (float)(src_offset.y + src_extent.height) / (float)src_iview->extent.height,
         (float)src_offset.z / (float)src_iview->extent.depth,
      },
   };

   vb_data[2] = (struct blit_vb_data) {
      .pos = {
         dest_offset.x,
         dest_offset.y,
      },
      .tex_coord = {
         (float)src_offset.x / (float)src_iview->extent.width,
         (float)src_offset.y / (float)src_iview->extent.height,
         (float)src_offset.z / (float)src_iview->extent.depth,
      },
   };

   anv_state_clflush(vb_state);

   struct anv_buffer vertex_buffer = {
      .device = device,
      .size = vb_size,
      .bo = &device->dynamic_state_block_pool.bo,
      .offset = vb_state.offset,
   };

   anv_CmdBindVertexBuffers(anv_cmd_buffer_to_handle(cmd_buffer), 0, 2,
      (VkBuffer[]) {
         anv_buffer_to_handle(&vertex_buffer),
         anv_buffer_to_handle(&vertex_buffer)
      },
      (VkDeviceSize[]) {
         0,
         sizeof(struct anv_vue_header),
      });

   VkSampler sampler;
   ANV_CALL(CreateSampler)(anv_device_to_handle(device),
      &(VkSamplerCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
         .magFilter = blit_filter,
         .minFilter = blit_filter,
      }, &cmd_buffer->pool->alloc, &sampler);

   VkDescriptorSet set;
   anv_AllocateDescriptorSets(anv_device_to_handle(device),
      &(VkDescriptorSetAllocateInfo) {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = dummy_desc_pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &device->meta_state.blit.ds_layout
      }, &set);
   anv_UpdateDescriptorSets(anv_device_to_handle(device),
      1, /* writeCount */
      (VkWriteDescriptorSet[]) {
         {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = (VkDescriptorImageInfo[]) {
               {
                  .sampler = sampler,
                  .imageView = anv_image_view_to_handle(src_iview),
                  .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
               },
            }
         }
      }, 0, NULL);

   VkFramebuffer fb;
   anv_CreateFramebuffer(anv_device_to_handle(device),
      &(VkFramebufferCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = (VkImageView[]) {
            anv_image_view_to_handle(dest_iview),
         },
         .width = dest_iview->extent.width,
         .height = dest_iview->extent.height,
         .layers = 1
      }, &cmd_buffer->pool->alloc, &fb);

   ANV_CALL(CmdBeginRenderPass)(anv_cmd_buffer_to_handle(cmd_buffer),
      &(VkRenderPassBeginInfo) {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = device->meta_state.blit.render_pass,
         .framebuffer = fb,
         .renderArea = {
            .offset = { dest_offset.x, dest_offset.y },
            .extent = { dest_extent.width, dest_extent.height },
         },
         .clearValueCount = 0,
         .pClearValues = NULL,
      }, VK_SUBPASS_CONTENTS_INLINE);

   VkPipeline pipeline;

   switch (src_image->type) {
   case VK_IMAGE_TYPE_1D:
      pipeline = device->meta_state.blit.pipeline_1d_src;
      break;
   case VK_IMAGE_TYPE_2D:
      pipeline = device->meta_state.blit.pipeline_2d_src;
      break;
   case VK_IMAGE_TYPE_3D:
      pipeline = device->meta_state.blit.pipeline_3d_src;
      break;
   default:
      unreachable(!"bad VkImageType");
   }

   if (cmd_buffer->state.pipeline != anv_pipeline_from_handle(pipeline)) {
      anv_CmdBindPipeline(anv_cmd_buffer_to_handle(cmd_buffer),
                          VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   }

   anv_CmdSetViewport(anv_cmd_buffer_to_handle(cmd_buffer), 0, 1,
                      &(VkViewport) {
                        .x = 0.0f,
                        .y = 0.0f,
                        .width = dest_iview->extent.width,
                        .height = dest_iview->extent.height,
                        .minDepth = 0.0f,
                        .maxDepth = 1.0f,
                      });

   anv_CmdBindDescriptorSets(anv_cmd_buffer_to_handle(cmd_buffer),
                             VK_PIPELINE_BIND_POINT_GRAPHICS,
                             device->meta_state.blit.pipeline_layout, 0, 1,
                             &set, 0, NULL);

   ANV_CALL(CmdDraw)(anv_cmd_buffer_to_handle(cmd_buffer), 3, 1, 0, 0);

   ANV_CALL(CmdEndRenderPass)(anv_cmd_buffer_to_handle(cmd_buffer));

   /* At the point where we emit the draw call, all data from the
    * descriptor sets, etc. has been used.  We are free to delete it.
    */
   anv_descriptor_set_destroy(device, anv_descriptor_set_from_handle(set));
   anv_DestroySampler(anv_device_to_handle(device), sampler,
                      &cmd_buffer->pool->alloc);
   anv_DestroyFramebuffer(anv_device_to_handle(device), fb,
                          &cmd_buffer->pool->alloc);
}

static void
meta_finish_blit(struct anv_cmd_buffer *cmd_buffer,
                 const struct anv_meta_saved_state *saved_state)
{
   anv_meta_restore(saved_state, cmd_buffer);
}

static VkFormat
vk_format_for_size(int bs)
{
   /* Note: We intentionally use the 4-channel formats whenever we can.
    * This is so that, when we do a RGB <-> RGBX copy, the two formats will
    * line up even though one of them is 3/4 the size of the other.
    */
   switch (bs) {
   case 1: return VK_FORMAT_R8_UINT;
   case 2: return VK_FORMAT_R8G8_UINT;
   case 3: return VK_FORMAT_R8G8B8_UINT;
   case 4: return VK_FORMAT_R8G8B8A8_UINT;
   case 6: return VK_FORMAT_R16G16B16_UINT;
   case 8: return VK_FORMAT_R16G16B16A16_UINT;
   case 12: return VK_FORMAT_R32G32B32_UINT;
   case 16: return VK_FORMAT_R32G32B32A32_UINT;
   default:
      unreachable("Invalid format block size");
   }
}

static void
do_buffer_copy(struct anv_cmd_buffer *cmd_buffer,
               struct anv_bo *src, uint64_t src_offset,
               struct anv_bo *dest, uint64_t dest_offset,
               int width, int height, VkFormat copy_format)
{
   VkDevice vk_device = anv_device_to_handle(cmd_buffer->device);

   VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = copy_format,
      .extent = {
         .width = width,
         .height = height,
         .depth = 1,
      },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = 1,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = 0,
      .flags = 0,
   };

   VkImage src_image;
   image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
   anv_CreateImage(vk_device, &image_info,
                   &cmd_buffer->pool->alloc, &src_image);

   VkImage dest_image;
   image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   anv_CreateImage(vk_device, &image_info,
                   &cmd_buffer->pool->alloc, &dest_image);

   /* We could use a vk call to bind memory, but that would require
    * creating a dummy memory object etc. so there's really no point.
    */
   anv_image_from_handle(src_image)->bo = src;
   anv_image_from_handle(src_image)->offset = src_offset;
   anv_image_from_handle(dest_image)->bo = dest;
   anv_image_from_handle(dest_image)->offset = dest_offset;

   struct anv_image_view src_iview;
   anv_image_view_init(&src_iview, cmd_buffer->device,
      &(VkImageViewCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = src_image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = copy_format,
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
         },
      },
      cmd_buffer, 0);

   struct anv_image_view dest_iview;
   anv_image_view_init(&dest_iview, cmd_buffer->device,
      &(VkImageViewCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = dest_image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = copy_format,
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
      },
      cmd_buffer, 0);

   meta_emit_blit(cmd_buffer,
                  anv_image_from_handle(src_image),
                  &src_iview,
                  (VkOffset3D) { 0, 0, 0 },
                  (VkExtent3D) { width, height, 1 },
                  anv_image_from_handle(dest_image),
                  &dest_iview,
                  (VkOffset3D) { 0, 0, 0 },
                  (VkExtent3D) { width, height, 1 },
                  VK_FILTER_NEAREST);

   anv_DestroyImage(vk_device, src_image, &cmd_buffer->pool->alloc);
   anv_DestroyImage(vk_device, dest_image, &cmd_buffer->pool->alloc);
}

void anv_CmdCopyBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, src_buffer, srcBuffer);
   ANV_FROM_HANDLE(anv_buffer, dest_buffer, destBuffer);

   struct anv_meta_saved_state saved_state;

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      uint64_t src_offset = src_buffer->offset + pRegions[r].srcOffset;
      uint64_t dest_offset = dest_buffer->offset + pRegions[r].dstOffset;
      uint64_t copy_size = pRegions[r].size;

      /* First, we compute the biggest format that can be used with the
       * given offsets and size.
       */
      int bs = 16;

      int fs = ffs(src_offset) - 1;
      if (fs != -1)
         bs = MIN2(bs, 1 << fs);
      assert(src_offset % bs == 0);

      fs = ffs(dest_offset) - 1;
      if (fs != -1)
         bs = MIN2(bs, 1 << fs);
      assert(dest_offset % bs == 0);

      fs = ffs(pRegions[r].size) - 1;
      if (fs != -1)
         bs = MIN2(bs, 1 << fs);
      assert(pRegions[r].size % bs == 0);

      VkFormat copy_format = vk_format_for_size(bs);

      /* This is maximum possible width/height our HW can handle */
      uint64_t max_surface_dim = 1 << 14;

      /* First, we make a bunch of max-sized copies */
      uint64_t max_copy_size = max_surface_dim * max_surface_dim * bs;
      while (copy_size >= max_copy_size) {
         do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
                        dest_buffer->bo, dest_offset,
                        max_surface_dim, max_surface_dim, copy_format);
         copy_size -= max_copy_size;
         src_offset += max_copy_size;
         dest_offset += max_copy_size;
      }

      uint64_t height = copy_size / (max_surface_dim * bs);
      assert(height < max_surface_dim);
      if (height != 0) {
         uint64_t rect_copy_size = height * max_surface_dim * bs;
         do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
                        dest_buffer->bo, dest_offset,
                        max_surface_dim, height, copy_format);
         copy_size -= rect_copy_size;
         src_offset += rect_copy_size;
         dest_offset += rect_copy_size;
      }

      if (copy_size != 0) {
         do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
                        dest_buffer->bo, dest_offset,
                        copy_size / bs, 1, copy_format);
      }
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdUpdateBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    VkDeviceSize                                dataSize,
    const uint32_t*                             pData)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);
   struct anv_meta_saved_state saved_state;

   meta_prepare_blit(cmd_buffer, &saved_state);

   /* We can't quite grab a full block because the state stream needs a
    * little data at the top to build its linked list.
    */
   const uint32_t max_update_size =
      cmd_buffer->device->dynamic_state_block_pool.block_size - 64;

   assert(max_update_size < (1 << 14) * 4);

   while (dataSize) {
      const uint32_t copy_size = MIN2(dataSize, max_update_size);

      struct anv_state tmp_data =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, copy_size, 64);

      memcpy(tmp_data.map, pData, copy_size);

      VkFormat format;
      int bs;
      if ((copy_size & 15) == 0 && (dstOffset & 15) == 0) {
         format = VK_FORMAT_R32G32B32A32_UINT;
         bs = 16;
      } else if ((copy_size & 7) == 0 && (dstOffset & 7) == 0) {
         format = VK_FORMAT_R32G32_UINT;
         bs = 8;
      } else {
         assert((copy_size & 3) == 0 && (dstOffset & 3) == 0);
         format = VK_FORMAT_R32_UINT;
         bs = 4;
      }

      do_buffer_copy(cmd_buffer,
                     &cmd_buffer->device->dynamic_state_block_pool.bo,
                     tmp_data.offset,
                     dst_buffer->bo, dst_buffer->offset + dstOffset,
                     copy_size / bs, 1, format);

      dataSize -= copy_size;
      dstOffset += copy_size;
      pData = (void *)pData + copy_size;
   }
}

static VkFormat
choose_iview_format(struct anv_image *image, VkImageAspectFlagBits aspect)
{
   assert(__builtin_popcount(aspect) == 1);

   struct isl_surf *surf =
      &anv_image_get_surface_for_aspect_mask(image, aspect)->isl;

   /* vkCmdCopyImage behaves like memcpy. Therefore we choose identical UINT
    * formats for the source and destination image views.
    *
    * From the Vulkan spec (2015-12-30):
    *
    *    vkCmdCopyImage performs image copies in a similar manner to a host
    *    memcpy. It does not perform general-purpose conversions such as
    *    scaling, resizing, blending, color-space conversion, or format
    *    conversions.  Rather, it simply copies raw image data. vkCmdCopyImage
    *    can copy between images with different formats, provided the formats
    *    are compatible as defined below.
    *
    *    [The spec later defines compatibility as having the same number of
    *    bytes per block].
    */
   return vk_format_for_size(isl_format_layouts[surf->format].bs);
}

static VkFormat
choose_buffer_format(VkFormat format, VkImageAspectFlagBits aspect)
{
   assert(__builtin_popcount(aspect) == 1);

   /* vkCmdCopy* commands behave like memcpy. Therefore we choose
    * compatable UINT formats for the source and destination image views.
    *
    * For the buffer, we go back to the original image format and get a
    * the format as if it were linear.  This way, for RGB formats, we get
    * an RGB format here even if the tiled image is RGBA. XXX: This doesn't
    * work if the buffer is the destination.
    */
   enum isl_format linear_format = anv_get_isl_format(format, aspect,
                                                      VK_IMAGE_TILING_LINEAR,
                                                      NULL);

   return vk_format_for_size(isl_format_layouts[linear_format].bs);
}

void anv_CmdCopyImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageCopy*                          pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dest_image, destImage);
   struct anv_meta_saved_state saved_state;

   /* From the Vulkan 1.0 spec:
    *
    *    vkCmdCopyImage can be used to copy image data between multisample
    *    images, but both images must have the same number of samples.
    */
   assert(src_image->samples == dest_image->samples);

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      assert(pRegions[r].srcSubresource.aspectMask ==
             pRegions[r].dstSubresource.aspectMask);

      VkImageAspectFlags aspect = pRegions[r].srcSubresource.aspectMask;

      VkFormat src_format = choose_iview_format(src_image, aspect);
      VkFormat dst_format = choose_iview_format(dest_image, aspect);

      struct anv_image_view src_iview;
      anv_image_view_init(&src_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = srcImage,
            .viewType = anv_meta_get_view_type(src_image),
            .format = src_format,
            .subresourceRange = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .baseMipLevel = pRegions[r].srcSubresource.mipLevel,
               .levelCount = 1,
               .baseArrayLayer = pRegions[r].srcSubresource.baseArrayLayer,
               .layerCount = pRegions[r].dstSubresource.layerCount,
            },
         },
         cmd_buffer, 0);

      const VkOffset3D dest_offset = {
         .x = pRegions[r].dstOffset.x,
         .y = pRegions[r].dstOffset.y,
         .z = 0,
      };

      unsigned num_slices;
      if (src_image->type == VK_IMAGE_TYPE_3D) {
         assert(pRegions[r].srcSubresource.layerCount == 1 &&
                pRegions[r].dstSubresource.layerCount == 1);
         num_slices = pRegions[r].extent.depth;
      } else {
         assert(pRegions[r].srcSubresource.layerCount ==
                pRegions[r].dstSubresource.layerCount);
         assert(pRegions[r].extent.depth == 1);
         num_slices = pRegions[r].dstSubresource.layerCount;
      }

      const uint32_t dest_base_array_slice =
         anv_meta_get_iview_layer(dest_image, &pRegions[r].dstSubresource,
                                  &pRegions[r].dstOffset);

      for (unsigned slice = 0; slice < num_slices; slice++) {
         VkOffset3D src_offset = pRegions[r].srcOffset;
         src_offset.z += slice;

         struct anv_image_view dest_iview;
         anv_image_view_init(&dest_iview, cmd_buffer->device,
            &(VkImageViewCreateInfo) {
               .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
               .image = destImage,
               .viewType = anv_meta_get_view_type(dest_image),
               .format = dst_format,
               .subresourceRange = {
                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                  .baseMipLevel = pRegions[r].dstSubresource.mipLevel,
                  .levelCount = 1,
                  .baseArrayLayer = dest_base_array_slice + slice,
                  .layerCount = 1
               },
            },
            cmd_buffer, 0);

         meta_emit_blit(cmd_buffer,
                        src_image, &src_iview,
                        src_offset,
                        pRegions[r].extent,
                        dest_image, &dest_iview,
                        dest_offset,
                        pRegions[r].extent,
                        VK_FILTER_NEAREST);
      }
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdBlitImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageBlit*                          pRegions,
    VkFilter                                    filter)

{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dest_image, destImage);
   struct anv_meta_saved_state saved_state;

   /* From the Vulkan 1.0 spec:
    *
    *    vkCmdBlitImage must not be used for multisampled source or
    *    destination images. Use vkCmdResolveImage for this purpose.
    */
   assert(src_image->samples == 1);
   assert(dest_image->samples == 1);

   anv_finishme("respect VkFilter");

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      struct anv_image_view src_iview;
      anv_image_view_init(&src_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = srcImage,
            .viewType = anv_meta_get_view_type(src_image),
            .format = src_image->vk_format,
            .subresourceRange = {
               .aspectMask = pRegions[r].srcSubresource.aspectMask,
               .baseMipLevel = pRegions[r].srcSubresource.mipLevel,
               .levelCount = 1,
               .baseArrayLayer = pRegions[r].srcSubresource.baseArrayLayer,
               .layerCount = 1
            },
         },
         cmd_buffer, 0);

      const VkOffset3D dest_offset = {
         .x = pRegions[r].dstOffsets[0].x,
         .y = pRegions[r].dstOffsets[0].y,
         .z = 0,
      };

      if (pRegions[r].dstOffsets[1].x < pRegions[r].dstOffsets[0].x ||
          pRegions[r].dstOffsets[1].y < pRegions[r].dstOffsets[0].y ||
          pRegions[r].srcOffsets[1].x < pRegions[r].srcOffsets[0].x ||
          pRegions[r].srcOffsets[1].y < pRegions[r].srcOffsets[0].y)
         anv_finishme("FINISHME: Allow flipping in blits");

      const VkExtent3D dest_extent = {
         .width = pRegions[r].dstOffsets[1].x - pRegions[r].dstOffsets[0].x,
         .height = pRegions[r].dstOffsets[1].y - pRegions[r].dstOffsets[0].y,
      };

      const VkExtent3D src_extent = {
         .width = pRegions[r].srcOffsets[1].x - pRegions[r].srcOffsets[0].x,
         .height = pRegions[r].srcOffsets[1].y - pRegions[r].srcOffsets[0].y,
      };

      const uint32_t dest_array_slice =
         anv_meta_get_iview_layer(dest_image, &pRegions[r].dstSubresource,
                                  &pRegions[r].dstOffsets[0]);

      if (pRegions[r].srcSubresource.layerCount > 1)
         anv_finishme("FINISHME: copy multiple array layers");

      if (pRegions[r].srcOffsets[0].z + 1 != pRegions[r].srcOffsets[1].z ||
          pRegions[r].dstOffsets[0].z + 1 != pRegions[r].dstOffsets[1].z)
         anv_finishme("FINISHME: copy multiple depth layers");

      struct anv_image_view dest_iview;
      anv_image_view_init(&dest_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = destImage,
            .viewType = anv_meta_get_view_type(dest_image),
            .format = dest_image->vk_format,
            .subresourceRange = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .baseMipLevel = pRegions[r].dstSubresource.mipLevel,
               .levelCount = 1,
               .baseArrayLayer = dest_array_slice,
               .layerCount = 1
            },
         },
         cmd_buffer, 0);

      meta_emit_blit(cmd_buffer,
                     src_image, &src_iview,
                     pRegions[r].srcOffsets[0], src_extent,
                     dest_image, &dest_iview,
                     dest_offset, dest_extent,
                     filter);
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

static struct anv_image *
make_image_for_buffer(VkDevice vk_device, VkBuffer vk_buffer, VkFormat format,
                      VkImageUsageFlags usage,
                      VkImageType image_type,
                      const VkAllocationCallbacks *alloc,
                      const VkBufferImageCopy *copy)
{
   ANV_FROM_HANDLE(anv_buffer, buffer, vk_buffer);

   VkExtent3D extent = copy->imageExtent;
   if (copy->bufferRowLength)
      extent.width = copy->bufferRowLength;
   if (copy->bufferImageHeight)
      extent.height = copy->bufferImageHeight;
   extent.depth = 1;
   extent = meta_region_extent_el(format, &extent);

   VkImageAspectFlags aspect = copy->imageSubresource.aspectMask;
   VkFormat buffer_format = choose_buffer_format(format, aspect);

   VkImage vk_image;
   VkResult result = anv_CreateImage(vk_device,
      &(VkImageCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = buffer_format,
         .extent = extent,
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = 1,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = usage,
         .flags = 0,
      }, alloc, &vk_image);
   assert(result == VK_SUCCESS);

   ANV_FROM_HANDLE(anv_image, image, vk_image);

   /* We could use a vk call to bind memory, but that would require
    * creating a dummy memory object etc. so there's really no point.
    */
   image->bo = buffer->bo;
   image->offset = buffer->offset + copy->bufferOffset;

   return image;
}

void anv_CmdCopyBufferToImage(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, dest_image, destImage);
   VkDevice vk_device = anv_device_to_handle(cmd_buffer->device);
   struct anv_meta_saved_state saved_state;

   /* The Vulkan 1.0 spec says "dstImage must have a sample count equal to
    * VK_SAMPLE_COUNT_1_BIT."
    */
   assert(dest_image->samples == 1);

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      VkImageAspectFlags aspect = pRegions[r].imageSubresource.aspectMask;

      VkFormat image_format = choose_iview_format(dest_image, aspect);

      struct anv_image *src_image =
         make_image_for_buffer(vk_device, srcBuffer, dest_image->vk_format,
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                               dest_image->type, &cmd_buffer->pool->alloc,
                               &pRegions[r]);

      const uint32_t dest_base_array_slice =
         anv_meta_get_iview_layer(dest_image, &pRegions[r].imageSubresource,
                                  &pRegions[r].imageOffset);

      unsigned num_slices_3d = pRegions[r].imageExtent.depth;
      unsigned num_slices_array = pRegions[r].imageSubresource.layerCount;
      unsigned slice_3d = 0;
      unsigned slice_array = 0;
      while (slice_3d < num_slices_3d && slice_array < num_slices_array) {
         struct anv_image_view src_iview;
         anv_image_view_init(&src_iview, cmd_buffer->device,
            &(VkImageViewCreateInfo) {
               .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
               .image = anv_image_to_handle(src_image),
               .viewType = VK_IMAGE_VIEW_TYPE_2D,
               .format = src_image->vk_format,
               .subresourceRange = {
                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                  .baseMipLevel = 0,
                  .levelCount = 1,
                  .baseArrayLayer = 0,
                  .layerCount = 1,
               },
            },
            cmd_buffer, 0);

         uint32_t img_x = 0;
         uint32_t img_y = 0;
         uint32_t img_o = 0;
         if (isl_format_is_compressed(dest_image->format->surface_format))
            isl_surf_get_image_intratile_offset_el(&cmd_buffer->device->isl_dev,
                                                   &dest_image->color_surface.isl,
                                                   pRegions[r].imageSubresource.mipLevel,
                                                   pRegions[r].imageSubresource.baseArrayLayer + slice_array,
                                                   pRegions[r].imageOffset.z + slice_3d,
                                                   &img_o, &img_x, &img_y);

         VkOffset3D dest_offset_el = meta_region_offset_el(dest_image, & pRegions[r].imageOffset);
         dest_offset_el.x += img_x;
         dest_offset_el.y += img_y;
         dest_offset_el.z = 0;

         struct anv_image_view dest_iview;
         anv_image_view_init(&dest_iview, cmd_buffer->device,
            &(VkImageViewCreateInfo) {
               .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
               .image = anv_image_to_handle(dest_image),
               .viewType = anv_meta_get_view_type(dest_image),
               .format = image_format,
               .subresourceRange = {
                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                  .baseMipLevel = pRegions[r].imageSubresource.mipLevel,
                  .levelCount = 1,
                  .baseArrayLayer = dest_base_array_slice +
                                    slice_array + slice_3d,
                  .layerCount = 1
               },
            },
            cmd_buffer, img_o);

         const VkExtent3D img_extent_el = meta_region_extent_el(dest_image->vk_format,
                                                      &pRegions[r].imageExtent);

         meta_emit_blit(cmd_buffer,
                        src_image,
                        &src_iview,
                        (VkOffset3D){0, 0, 0},
                        img_extent_el,
                        dest_image,
                        &dest_iview,
                        dest_offset_el,
                        img_extent_el,
                        VK_FILTER_NEAREST);

         /* Once we've done the blit, all of the actual information about
          * the image is embedded in the command buffer so we can just
          * increment the offset directly in the image effectively
          * re-binding it to different backing memory.
          */
         src_image->offset += src_image->extent.width *
                              src_image->extent.height *
                              src_image->format->isl_layout->bs;

         if (dest_image->type == VK_IMAGE_TYPE_3D)
            slice_3d++;
         else
            slice_array++;
      }

      anv_DestroyImage(vk_device, anv_image_to_handle(src_image),
                       &cmd_buffer->pool->alloc);
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdCopyImageToBuffer(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   VkDevice vk_device = anv_device_to_handle(cmd_buffer->device);
   struct anv_meta_saved_state saved_state;


   /* The Vulkan 1.0 spec says "srcImage must have a sample count equal to
    * VK_SAMPLE_COUNT_1_BIT."
    */
   assert(src_image->samples == 1);

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      VkImageAspectFlags aspect = pRegions[r].imageSubresource.aspectMask;

      VkFormat image_format = choose_iview_format(src_image, aspect);

      struct anv_image_view src_iview;
      anv_image_view_init(&src_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = srcImage,
            .viewType = anv_meta_get_view_type(src_image),
            .format = image_format,
            .subresourceRange = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .baseMipLevel = pRegions[r].imageSubresource.mipLevel,
               .levelCount = 1,
               .baseArrayLayer = pRegions[r].imageSubresource.baseArrayLayer,
               .layerCount = pRegions[r].imageSubresource.layerCount,
            },
         },
         cmd_buffer, 0);

      struct anv_image *dest_image =
         make_image_for_buffer(vk_device, destBuffer, src_image->vk_format,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                               src_image->type, &cmd_buffer->pool->alloc,
                               &pRegions[r]);

      unsigned num_slices;
      if (src_image->type == VK_IMAGE_TYPE_3D) {
         assert(pRegions[r].imageSubresource.layerCount == 1);
         num_slices = pRegions[r].imageExtent.depth;
      } else {
         assert(pRegions[r].imageExtent.depth == 1);
         num_slices = pRegions[r].imageSubresource.layerCount;
      }

      for (unsigned slice = 0; slice < num_slices; slice++) {
         VkOffset3D src_offset = pRegions[r].imageOffset;
         src_offset.z += slice;

         struct anv_image_view dest_iview;
         anv_image_view_init(&dest_iview, cmd_buffer->device,
            &(VkImageViewCreateInfo) {
               .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
               .image = anv_image_to_handle(dest_image),
               .viewType = VK_IMAGE_VIEW_TYPE_2D,
               .format = dest_image->vk_format,
               .subresourceRange = {
                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                  .baseMipLevel = 0,
                  .levelCount = 1,
                  .baseArrayLayer = 0,
                  .layerCount = 1
               },
            },
            cmd_buffer, 0);

         meta_emit_blit(cmd_buffer,
                        anv_image_from_handle(srcImage),
                        &src_iview,
                        src_offset,
                        pRegions[r].imageExtent,
                        dest_image,
                        &dest_iview,
                        (VkOffset3D) { 0, 0, 0 },
                        pRegions[r].imageExtent,
                        VK_FILTER_NEAREST);

         /* Once we've done the blit, all of the actual information about
          * the image is embedded in the command buffer so we can just
          * increment the offset directly in the image effectively
          * re-binding it to different backing memory.
          */
         dest_image->offset += dest_image->extent.width *
                               dest_image->extent.height *
                               src_image->format->isl_layout->bs;
      }

      anv_DestroyImage(vk_device, anv_image_to_handle(dest_image),
                       &cmd_buffer->pool->alloc);
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

static void *
meta_alloc(void* _device, size_t size, size_t alignment,
           VkSystemAllocationScope allocationScope)
{
   struct anv_device *device = _device;
   return device->alloc.pfnAllocation(device->alloc.pUserData, size, alignment,
                                      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
}

static void *
meta_realloc(void* _device, void *original, size_t size, size_t alignment,
             VkSystemAllocationScope allocationScope)
{
   struct anv_device *device = _device;
   return device->alloc.pfnReallocation(device->alloc.pUserData, original,
                                        size, alignment,
                                        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
}

static void
meta_free(void* _device, void *data)
{
   struct anv_device *device = _device;
   return device->alloc.pfnFree(device->alloc.pUserData, data);
}

VkResult
anv_device_init_meta(struct anv_device *device)
{
   VkResult result;

   device->meta_state.alloc = (VkAllocationCallbacks) {
      .pUserData = device,
      .pfnAllocation = meta_alloc,
      .pfnReallocation = meta_realloc,
      .pfnFree = meta_free,
   };

   result = anv_device_init_meta_clear_state(device);
   if (result != VK_SUCCESS)
      goto fail_clear;

   result = anv_device_init_meta_resolve_state(device);
   if (result != VK_SUCCESS)
      goto fail_resolve;

   result = anv_device_init_meta_blit_state(device);
   if (result != VK_SUCCESS)
      goto fail_blit;

   return VK_SUCCESS;

fail_blit:
   anv_device_finish_meta_resolve_state(device);
fail_resolve:
   anv_device_finish_meta_clear_state(device);
fail_clear:
   return result;
}

void
anv_device_finish_meta(struct anv_device *device)
{
   anv_device_finish_meta_resolve_state(device);
   anv_device_finish_meta_clear_state(device);

   /* Blit */
   anv_DestroyRenderPass(anv_device_to_handle(device),
                         device->meta_state.blit.render_pass,
                         &device->meta_state.alloc);
   anv_DestroyPipeline(anv_device_to_handle(device),
                       device->meta_state.blit.pipeline_1d_src,
                       &device->meta_state.alloc);
   anv_DestroyPipeline(anv_device_to_handle(device),
                       device->meta_state.blit.pipeline_2d_src,
                       &device->meta_state.alloc);
   anv_DestroyPipeline(anv_device_to_handle(device),
                       device->meta_state.blit.pipeline_3d_src,
                       &device->meta_state.alloc);
   anv_DestroyPipelineLayout(anv_device_to_handle(device),
                             device->meta_state.blit.pipeline_layout,
                             &device->meta_state.alloc);
   anv_DestroyDescriptorSetLayout(anv_device_to_handle(device),
                                  device->meta_state.blit.ds_layout,
                                  &device->meta_state.alloc);
}
