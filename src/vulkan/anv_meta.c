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
#include "anv_meta_clear.h"
#include "anv_private.h"
#include "anv_nir_builder.h"

struct anv_render_pass anv_meta_dummy_renderpass = {0};

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
build_nir_copy_fragment_shader(enum glsl_sampler_dim tex_dim)
{
   nir_builder b;

   nir_builder_init_simple_shader(&b, MESA_SHADER_FRAGMENT);

   const struct glsl_type *color_type = glsl_vec4_type();

   nir_variable *tex_pos_in = nir_variable_create(b.shader, nir_var_shader_in,
                                                  glsl_vec4_type(), "v_attr");
   tex_pos_in->data.location = VARYING_SLOT_VAR0;

   const struct glsl_type *sampler_type =
      glsl_sampler_type(tex_dim, false, false, glsl_get_base_type(color_type));
   nir_variable *sampler = nir_variable_create(b.shader, nir_var_uniform,
                                               sampler_type, "s_tex");
   sampler->data.descriptor_set = 0;
   sampler->data.binding = 0;

   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
   tex->sampler_dim = tex_dim;
   tex->op = nir_texop_tex;
   tex->src[0].src_type = nir_tex_src_coord;
   tex->src[0].src = nir_src_for_ssa(nir_load_var(&b, tex_pos_in));
   tex->dest_type = nir_type_float; /* TODO */

   switch (tex_dim) {
   case GLSL_SAMPLER_DIM_2D:
      tex->coord_components = 2;
      break;
   case GLSL_SAMPLER_DIM_3D:
      tex->coord_components = 3;
      break;
   default:
      assert(!"Unsupported texture dimension");
   }

   tex->sampler = nir_deref_var_create(tex, sampler);

   nir_ssa_dest_init(&tex->instr, &tex->dest, 4, "tex");
   nir_builder_instr_insert(&b, &tex->instr);

   nir_variable *color_out = nir_variable_create(b.shader, nir_var_shader_out,
                                                 color_type, "f_color");
   color_out->data.location = FRAG_RESULT_DATA0;
   nir_store_var(&b, color_out, &tex->dest.ssa);

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
   cmd_buffer->state.descriptors_dirty |= VK_SHADER_STAGE_VERTEX_BIT;

   anv_dynamic_state_copy(&cmd_buffer->state.dynamic, &state->dynamic,
                          state->dynamic_mask);
   cmd_buffer->state.dirty |= state->dynamic_mask;
}

static VkImageViewType
meta_blit_get_src_image_view_type(const struct anv_image *src_image)
{
   switch (src_image->type) {
   case VK_IMAGE_TYPE_1D:
      return VK_IMAGE_VIEW_TYPE_1D;
   case VK_IMAGE_TYPE_2D:
      return VK_IMAGE_VIEW_TYPE_2D;
   case VK_IMAGE_TYPE_3D:
      return VK_IMAGE_VIEW_TYPE_3D;
   default:
      assert(!"bad VkImageType");
      return 0;
   }
}

static uint32_t
meta_blit_get_dest_view_base_array_slice(const struct anv_image *dest_image,
                                         const VkImageSubresourceCopy *dest_subresource,
                                         const VkOffset3D *dest_offset)
{
   switch (dest_image->type) {
   case VK_IMAGE_TYPE_1D:
   case VK_IMAGE_TYPE_2D:
      return dest_subresource->arrayLayer;
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

static void
anv_device_init_meta_blit_state(struct anv_device *device)
{
   anv_CreateRenderPass(anv_device_to_handle(device),
      &(VkRenderPassCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = &(VkAttachmentDescription) {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION,
            .format = VK_FORMAT_UNDEFINED, /* Our shaders don't care */
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
      }, &device->meta_state.blit.render_pass);

   /* We don't use a vertex shader for clearing, but instead build and pass
    * the VUEs directly to the rasterization backend.  However, we do need
    * to provide GLSL source for the vertex shader so that the compiler
    * does not dead-code our inputs.
    */
   struct anv_shader_module vsm = {
      .nir = build_nir_vertex_shader(false),
   };

   struct anv_shader_module fsm_2d = {
      .nir = build_nir_copy_fragment_shader(GLSL_SAMPLER_DIM_2D),
   };

   struct anv_shader_module fsm_3d = {
      .nir = build_nir_copy_fragment_shader(GLSL_SAMPLER_DIM_3D),
   };

   VkShader vs;
   anv_CreateShader(anv_device_to_handle(device),
      &(VkShaderCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
         .module = anv_shader_module_to_handle(&vsm),
         .pName = "main",
      }, &vs);

   VkShader fs_2d;
   anv_CreateShader(anv_device_to_handle(device),
      &(VkShaderCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
         .module = anv_shader_module_to_handle(&fsm_2d),
         .pName = "main",
      }, &fs_2d);

   VkShader fs_3d;
   anv_CreateShader(anv_device_to_handle(device),
      &(VkShaderCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
         .module = anv_shader_module_to_handle(&fsm_3d),
         .pName = "main",
      }, &fs_3d);

   VkPipelineVertexInputStateCreateInfo vi_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .bindingCount = 2,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .strideInBytes = 0,
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         },
         {
            .binding = 1,
            .strideInBytes = 5 * sizeof(float),
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
            .offsetInBytes = 0
         },
         {
            /* Position */
            .location = 1,
            .binding = 1,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offsetInBytes = 0
         },
         {
            /* Texture Coordinate */
            .location = 2,
            .binding = 1,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offsetInBytes = 8
         }
      }
   };

   VkDescriptorSetLayoutCreateInfo ds_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .count = 1,
      .pBinding = (VkDescriptorSetLayoutBinding[]) {
         {
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .arraySize = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = NULL
         },
      }
   };
   anv_CreateDescriptorSetLayout(anv_device_to_handle(device), &ds_layout_info,
                                 &device->meta_state.blit.ds_layout);

   anv_CreatePipelineLayout(anv_device_to_handle(device),
      &(VkPipelineLayoutCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .descriptorSetCount = 1,
         .pSetLayouts = &device->meta_state.blit.ds_layout,
      },
      &device->meta_state.blit.pipeline_layout);

   VkPipelineShaderStageCreateInfo pipeline_shader_stages[] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX,
         .shader = vs,
         .pSpecializationInfo = NULL
      }, {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT,
         .shader = {0}, /* TEMPLATE VALUE! FILL ME IN! */
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
      .layout = device->meta_state.blit.pipeline_layout,
      .renderPass = device->meta_state.blit.render_pass,
      .subpass = 0,
   };

   const struct anv_graphics_pipeline_create_info anv_pipeline_info = {
      .use_repclear = false,
      .disable_viewport = true,
      .disable_scissor = true,
      .disable_vs = true,
      .use_rectlist = true
   };

   pipeline_shader_stages[1].shader = fs_2d;
   anv_graphics_pipeline_create(anv_device_to_handle(device),
      &vk_pipeline_info, &anv_pipeline_info,
      &device->meta_state.blit.pipeline_2d_src);

   pipeline_shader_stages[1].shader = fs_3d;
   anv_graphics_pipeline_create(anv_device_to_handle(device),
      &vk_pipeline_info, &anv_pipeline_info,
      &device->meta_state.blit.pipeline_3d_src);

   anv_DestroyShader(anv_device_to_handle(device), vs);
   anv_DestroyShader(anv_device_to_handle(device), fs_2d);
   anv_DestroyShader(anv_device_to_handle(device), fs_3d);
   ralloc_free(vsm.nir);
   ralloc_free(fsm_2d.nir);
   ralloc_free(fsm_3d.nir);
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

static void
meta_emit_blit(struct anv_cmd_buffer *cmd_buffer,
               struct anv_image *src_image,
               struct anv_image_view *src_iview,
               VkOffset3D src_offset,
               VkExtent3D src_extent,
               struct anv_image *dest_image,
               struct anv_image_view *dest_iview,
               VkOffset3D dest_offset,
               VkExtent3D dest_extent)
{
   struct anv_device *device = cmd_buffer->device;
   VkDescriptorPool dummy_desc_pool = { .handle = 1 };

   struct blit_vb_data {
      float pos[2];
      float tex_coord[3];
   } *vb_data;

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
         (float)(src_offset.z + src_extent.depth) / (float)src_iview->extent.depth,
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
         (float)(src_offset.z + src_extent.depth) / (float)src_iview->extent.depth,
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

   VkDescriptorSet set;
   anv_AllocDescriptorSets(anv_device_to_handle(device), dummy_desc_pool,
                           VK_DESCRIPTOR_SET_USAGE_ONE_SHOT,
                           1, &device->meta_state.blit.ds_layout, &set);
   anv_UpdateDescriptorSets(anv_device_to_handle(device),
      1, /* writeCount */
      (VkWriteDescriptorSet[]) {
         {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .destSet = set,
            .destBinding = 0,
            .destArrayElement = 0,
            .count = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pDescriptors = (VkDescriptorInfo[]) {
               {
                  .imageView = anv_image_view_to_handle(src_iview),
                  .imageLayout = VK_IMAGE_LAYOUT_GENERAL
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
      }, &fb);

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
      }, VK_RENDER_PASS_CONTENTS_INLINE);

   VkPipeline pipeline;

   switch (src_image->type) {
   case VK_IMAGE_TYPE_1D:
      anv_finishme("VK_IMAGE_TYPE_1D");
      pipeline = device->meta_state.blit.pipeline_2d_src;
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

   anv_CmdSetViewport(anv_cmd_buffer_to_handle(cmd_buffer), 1,
                      &(VkViewport) {
                        .originX = 0.0f,
                        .originY = 0.0f,
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
   anv_DestroyFramebuffer(anv_device_to_handle(device), fb);
}

static void
meta_finish_blit(struct anv_cmd_buffer *cmd_buffer,
                 const struct anv_meta_saved_state *saved_state)
{
   anv_meta_restore(saved_state, cmd_buffer);
}

static VkFormat
vk_format_for_cpp(int cpp)
{
   switch (cpp) {
   case 1: return VK_FORMAT_R8_UINT;
   case 2: return VK_FORMAT_R8G8_UINT;
   case 3: return VK_FORMAT_R8G8B8_UINT;
   case 4: return VK_FORMAT_R8G8B8A8_UINT;
   case 6: return VK_FORMAT_R16G16B16_UINT;
   case 8: return VK_FORMAT_R16G16B16A16_UINT;
   case 12: return VK_FORMAT_R32G32B32_UINT;
   case 16: return VK_FORMAT_R32G32B32A32_UINT;
   default:
      unreachable("Invalid format cpp");
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
      .arraySize = 1,
      .samples = 1,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = 0,
      .flags = 0,
   };

   VkImage src_image;
   image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
   anv_CreateImage(vk_device, &image_info, &src_image);

   VkImage dest_image;
   image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   anv_CreateImage(vk_device, &image_info, &dest_image);

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
         .channels = {
            VK_CHANNEL_SWIZZLE_R,
            VK_CHANNEL_SWIZZLE_G,
            VK_CHANNEL_SWIZZLE_B,
            VK_CHANNEL_SWIZZLE_A
         },
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .mipLevels = 1,
            .baseArrayLayer = 0,
            .arraySize = 1
         },
      },
      cmd_buffer);

   struct anv_image_view dest_iview;
   anv_image_view_init(&dest_iview, cmd_buffer->device,
      &(VkImageViewCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = dest_image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = copy_format,
         .channels = {
            .r = VK_CHANNEL_SWIZZLE_R,
            .g = VK_CHANNEL_SWIZZLE_G,
            .b = VK_CHANNEL_SWIZZLE_B,
            .a = VK_CHANNEL_SWIZZLE_A,
         },
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .mipLevels = 1,
            .baseArrayLayer = 0,
            .arraySize = 1,
         },
      },
      cmd_buffer);

   meta_emit_blit(cmd_buffer,
                  anv_image_from_handle(src_image),
                  &src_iview,
                  (VkOffset3D) { 0, 0, 0 },
                  (VkExtent3D) { width, height, 1 },
                  anv_image_from_handle(dest_image),
                  &dest_iview,
                  (VkOffset3D) { 0, 0, 0 },
                  (VkExtent3D) { width, height, 1 });

   anv_DestroyImage(vk_device, src_image);
   anv_DestroyImage(vk_device, dest_image);
}

void anv_CmdCopyBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_buffer, src_buffer, srcBuffer);
   ANV_FROM_HANDLE(anv_buffer, dest_buffer, destBuffer);

   struct anv_meta_saved_state saved_state;

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      uint64_t src_offset = src_buffer->offset + pRegions[r].srcOffset;
      uint64_t dest_offset = dest_buffer->offset + pRegions[r].destOffset;
      uint64_t copy_size = pRegions[r].copySize;

      /* First, we compute the biggest format that can be used with the
       * given offsets and size.
       */
      int cpp = 16;

      int fs = ffs(src_offset) - 1;
      if (fs != -1)
         cpp = MIN2(cpp, 1 << fs);
      assert(src_offset % cpp == 0);

      fs = ffs(dest_offset) - 1;
      if (fs != -1)
         cpp = MIN2(cpp, 1 << fs);
      assert(dest_offset % cpp == 0);

      fs = ffs(pRegions[r].copySize) - 1;
      if (fs != -1)
         cpp = MIN2(cpp, 1 << fs);
      assert(pRegions[r].copySize % cpp == 0);

      VkFormat copy_format = vk_format_for_cpp(cpp);

      /* This is maximum possible width/height our HW can handle */
      uint64_t max_surface_dim = 1 << 14;

      /* First, we make a bunch of max-sized copies */
      uint64_t max_copy_size = max_surface_dim * max_surface_dim * cpp;
      while (copy_size > max_copy_size) {
         do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
                        dest_buffer->bo, dest_offset,
                        max_surface_dim, max_surface_dim, copy_format);
         copy_size -= max_copy_size;
         src_offset += max_copy_size;
         dest_offset += max_copy_size;
      }

      uint64_t height = copy_size / (max_surface_dim * cpp);
      assert(height < max_surface_dim);
      if (height != 0) {
         uint64_t rect_copy_size = height * max_surface_dim * cpp;
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
                        copy_size / cpp, 1, copy_format);
      }
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdCopyImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageCopy*                          pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dest_image, destImage);

   const VkImageViewType src_iview_type =
      meta_blit_get_src_image_view_type(src_image);

   struct anv_meta_saved_state saved_state;

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      struct anv_image_view src_iview;
      anv_image_view_init(&src_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = srcImage,
            .viewType = src_iview_type,
            .format = src_image->format->vk_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspectMask = 1 << pRegions[r].srcSubresource.aspect,
               .baseMipLevel = pRegions[r].srcSubresource.mipLevel,
               .mipLevels = 1,
               .baseArrayLayer = pRegions[r].srcSubresource.arrayLayer,
               .arraySize = 1
            },
         },
         cmd_buffer);

      const VkOffset3D dest_offset = {
         .x = pRegions[r].destOffset.x,
         .y = pRegions[r].destOffset.y,
         .z = 0,
      };

      const uint32_t dest_array_slice =
         meta_blit_get_dest_view_base_array_slice(dest_image,
                                                  &pRegions[r].destSubresource,
                                                  &pRegions[r].destOffset);

      if (pRegions[r].srcSubresource.arraySize > 1)
         anv_finishme("FINISHME: copy multiple array layers");

      if (pRegions[r].extent.depth > 1)
         anv_finishme("FINISHME: copy multiple depth layers");

      struct anv_image_view dest_iview;
      anv_image_view_init(&dest_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = destImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = dest_image->format->vk_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .baseMipLevel = pRegions[r].destSubresource.mipLevel,
               .mipLevels = 1,
               .baseArrayLayer = dest_array_slice,
               .arraySize = 1
            },
         },
         cmd_buffer);

      meta_emit_blit(cmd_buffer,
                     src_image, &src_iview,
                     pRegions[r].srcOffset,
                     pRegions[r].extent,
                     dest_image, &dest_iview,
                     dest_offset,
                     pRegions[r].extent);
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdBlitImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageBlit*                          pRegions,
    VkTexFilter                                 filter)

{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dest_image, destImage);

   const VkImageViewType src_iview_type =
      meta_blit_get_src_image_view_type(src_image);

   struct anv_meta_saved_state saved_state;

   anv_finishme("respect VkTexFilter");

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      struct anv_image_view src_iview;
      anv_image_view_init(&src_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = srcImage,
            .viewType = src_iview_type,
            .format = src_image->format->vk_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspectMask = 1 << pRegions[r].srcSubresource.aspect,
               .baseMipLevel = pRegions[r].srcSubresource.mipLevel,
               .mipLevels = 1,
               .baseArrayLayer = pRegions[r].srcSubresource.arrayLayer,
               .arraySize = 1
            },
         },
         cmd_buffer);

      const VkOffset3D dest_offset = {
         .x = pRegions[r].destOffset.x,
         .y = pRegions[r].destOffset.y,
         .z = 0,
      };

      const uint32_t dest_array_slice =
         meta_blit_get_dest_view_base_array_slice(dest_image,
                                                  &pRegions[r].destSubresource,
                                                  &pRegions[r].destOffset);

      if (pRegions[r].srcSubresource.arraySize > 1)
         anv_finishme("FINISHME: copy multiple array layers");

      if (pRegions[r].destExtent.depth > 1)
         anv_finishme("FINISHME: copy multiple depth layers");

      struct anv_image_view dest_iview;
      anv_image_view_init(&dest_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = destImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = dest_image->format->vk_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .baseMipLevel = pRegions[r].destSubresource.mipLevel,
               .mipLevels = 1,
               .baseArrayLayer = dest_array_slice,
               .arraySize = 1
            },
         },
         cmd_buffer);

      meta_emit_blit(cmd_buffer,
                     src_image, &src_iview,
                     pRegions[r].srcOffset,
                     pRegions[r].srcExtent,
                     dest_image, &dest_iview,
                     dest_offset,
                     pRegions[r].destExtent);
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

static VkImage
make_image_for_buffer(VkDevice vk_device, VkBuffer vk_buffer, VkFormat format,
                      VkImageUsageFlags usage,
                      const VkBufferImageCopy *copy)
{
   ANV_FROM_HANDLE(anv_buffer, buffer, vk_buffer);

   VkExtent3D extent = copy->imageExtent;
   if (copy->bufferRowLength)
      extent.width = copy->bufferRowLength;
   if (copy->bufferImageHeight)
      extent.height = copy->bufferImageHeight;
   extent.depth = 1;

   VkImage vk_image;
   VkResult result = anv_CreateImage(vk_device,
      &(VkImageCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = format,
         .extent = extent,
         .mipLevels = 1,
         .arraySize = 1,
         .samples = 1,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = usage,
         .flags = 0,
      }, &vk_image);
   assert(result == VK_SUCCESS);

   ANV_FROM_HANDLE(anv_image, image, vk_image);

   /* We could use a vk call to bind memory, but that would require
    * creating a dummy memory object etc. so there's really no point.
    */
   image->bo = buffer->bo;
   image->offset = buffer->offset + copy->bufferOffset;

   return anv_image_to_handle(image);
}

void anv_CmdCopyBufferToImage(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    srcBuffer,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_image, dest_image, destImage);
   VkDevice vk_device = anv_device_to_handle(cmd_buffer->device);
   const VkFormat orig_format = dest_image->format->vk_format;
   struct anv_meta_saved_state saved_state;

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      VkFormat proxy_format = orig_format;
      VkImageAspect proxy_aspect = pRegions[r].imageSubresource.aspect;

      if (orig_format == VK_FORMAT_S8_UINT) {
         proxy_format = VK_FORMAT_R8_UINT;
         proxy_aspect = VK_IMAGE_ASPECT_COLOR;
      }

      VkImage srcImage = make_image_for_buffer(vk_device, srcBuffer,
            proxy_format, VK_IMAGE_USAGE_SAMPLED_BIT, &pRegions[r]);

      struct anv_image_view src_iview;
      anv_image_view_init(&src_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = srcImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = proxy_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspectMask = 1 << proxy_aspect,
               .baseMipLevel = 0,
               .mipLevels = 1,
               .baseArrayLayer = 0,
               .arraySize = 1
            },
         },
         cmd_buffer);

      const VkOffset3D dest_offset = {
         .x = pRegions[r].imageOffset.x,
         .y = pRegions[r].imageOffset.y,
         .z = 0,
      };

      const uint32_t dest_array_slice =
         meta_blit_get_dest_view_base_array_slice(dest_image,
                                                  &pRegions[r].imageSubresource,
                                                  &pRegions[r].imageOffset);

      if (pRegions[r].imageExtent.depth > 1)
         anv_finishme("FINISHME: copy multiple depth layers");

      struct anv_image_view dest_iview;
      anv_image_view_init(&dest_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = anv_image_to_handle(dest_image),
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = proxy_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .baseMipLevel = pRegions[r].imageSubresource.mipLevel,
               .mipLevels = 1,
               .baseArrayLayer = dest_array_slice,
               .arraySize = 1
            },
         },
         cmd_buffer);

      meta_emit_blit(cmd_buffer,
                     anv_image_from_handle(srcImage),
                     &src_iview,
                     (VkOffset3D) { 0, 0, 0 },
                     pRegions[r].imageExtent,
                     dest_image,
                     &dest_iview,
                     dest_offset,
                     pRegions[r].imageExtent);

      anv_DestroyImage(vk_device, srcImage);
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdCopyImageToBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   VkDevice vk_device = anv_device_to_handle(cmd_buffer->device);
   struct anv_meta_saved_state saved_state;

   const VkImageViewType src_iview_type =
      meta_blit_get_src_image_view_type(src_image);

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      if (pRegions[r].imageSubresource.arraySize > 1)
         anv_finishme("FINISHME: copy multiple array layers");

      if (pRegions[r].imageExtent.depth > 1)
         anv_finishme("FINISHME: copy multiple depth layers");

      struct anv_image_view src_iview;
      anv_image_view_init(&src_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = srcImage,
            .viewType = src_iview_type,
            .format = src_image->format->vk_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspectMask = 1 << pRegions[r].imageSubresource.aspect,
               .baseMipLevel = pRegions[r].imageSubresource.mipLevel,
               .mipLevels = 1,
               .baseArrayLayer = pRegions[r].imageSubresource.arrayLayer,
               .arraySize = 1
            },
         },
         cmd_buffer);

      VkFormat dest_format = src_image->format->vk_format;
      if (dest_format == VK_FORMAT_S8_UINT) {
         dest_format = VK_FORMAT_R8_UINT;
      }

      VkImage destImage = make_image_for_buffer(vk_device, destBuffer,
            dest_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &pRegions[r]);

      struct anv_image_view dest_iview;
      anv_image_view_init(&dest_iview, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = destImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = dest_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .baseMipLevel = 0,
               .mipLevels = 1,
               .baseArrayLayer = 0,
               .arraySize = 1
            },
         },
         cmd_buffer);

      meta_emit_blit(cmd_buffer,
                     anv_image_from_handle(srcImage),
                     &src_iview,
                     pRegions[r].imageOffset,
                     pRegions[r].imageExtent,
                     anv_image_from_handle(destImage),
                     &dest_iview,
                     (VkOffset3D) { 0, 0, 0 },
                     pRegions[r].imageExtent);

      anv_DestroyImage(vk_device, destImage);
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdUpdateBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                dataSize,
    const uint32_t*                             pData)
{
   stub();
}

void anv_CmdFillBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                fillSize,
    uint32_t                                    data)
{
   stub();
}

void anv_CmdResolveImage(
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
   anv_device_init_meta_blit_state(device);
}

void
anv_device_finish_meta(struct anv_device *device)
{
   /* Clear */
   anv_DestroyPipeline(anv_device_to_handle(device),
                       device->meta_state.clear.pipeline);

   /* Blit */
   anv_DestroyRenderPass(anv_device_to_handle(device),
                         device->meta_state.blit.render_pass);
   anv_DestroyPipeline(anv_device_to_handle(device),
                       device->meta_state.blit.pipeline_2d_src);
   anv_DestroyPipeline(anv_device_to_handle(device),
                       device->meta_state.blit.pipeline_3d_src);
   anv_DestroyPipelineLayout(anv_device_to_handle(device),
                             device->meta_state.blit.pipeline_layout);
   anv_DestroyDescriptorSetLayout(anv_device_to_handle(device),
                                  device->meta_state.blit.ds_layout);
}
