#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define VK_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <poll.h>
#include <libpng16/png.h>

static void
fail_if(int cond, const char *format, ...)
{
   va_list args;

   if (!cond)
      return;

   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);

   exit(1);
}

static void
write_png(char *path, int32_t width, int32_t height, int32_t stride, void *pixels)
{
   FILE *f = NULL;
   png_structp png_writer = NULL;
   png_infop png_info = NULL;

   uint8_t *rows[height];

   for (int32_t y = 0; y < height; y++)
      rows[y] = pixels + y * stride;

   f = fopen(path, "wb");
   fail_if(!f, "failed to open file for writing: %s", path);

   png_writer = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                        NULL, NULL, NULL);
   fail_if (!png_writer, "failed to create png writer");

   png_info = png_create_info_struct(png_writer);
   fail_if(!png_info, "failed to create png writer info");

   png_init_io(png_writer, f);
   png_set_IHDR(png_writer, png_info,
                width, height,
                8, PNG_COLOR_TYPE_RGBA,
                PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                PNG_FILTER_TYPE_DEFAULT);
   png_write_info(png_writer, png_info);
   png_set_rows(png_writer, png_info, rows);
   png_write_png(png_writer, png_info, PNG_TRANSFORM_IDENTITY, NULL);

   png_destroy_write_struct(&png_writer, &png_info);

   fclose(f);
}

static void *
test_alloc(void*                                       pUserData,
           size_t                                      size,
           size_t                                      alignment,
           VkSystemAllocType                           allocType)
{
   return malloc(size);
}

static void
test_free(void*                                       pUserData,
          void*                                       pMem)
{
   free(pMem);
}

#define GLSL(src) "#version 330\n" #src

static void
create_pipeline(VkDevice device, VkPipeline *pipeline,
                VkPipelineLayout pipeline_layout)
{
   VkPipelineIaStateCreateInfo ia_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_IA_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
      .disableVertexReuse = false,
      .primitiveRestartEnable = false,
      .primitiveRestartIndex = 0
   };

   static const char vs_source[] = GLSL(
      layout(location = 0) in vec4 a_position;
      layout(location = 1) in vec4 a_color;
      layout(set = 0, index = 0) uniform block1 {
         vec4 color;
      } u1;
      layout(set = 0, index = 1) uniform block2 {
         vec4 color;
      } u2;
      layout(set = 1, index = 0) uniform block3 {
         vec4 color;
      } u3;
      out vec4 v_color;
      void main()
      {
        gl_Position = a_position;
        v_color = a_color + u1.color + u2.color + u3.color;
      });

   static const char fs_source[] = GLSL(
      out vec4 f_color;
      in vec4 v_color;
      layout(set = 0, index = 0) uniform sampler2D tex;
      void main()
      {
         f_color = v_color + texture2D(tex, vec2(0.1, 0.1));
      });

   VkShader vs;
   vkCreateShader(device,
                  &(VkShaderCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
                     .codeSize = sizeof(vs_source),
                     .pCode = vs_source,
                     .flags = 0
                  },
                  &vs);

   VkShader fs;
   vkCreateShader(device,
                  &(VkShaderCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
                     .codeSize = sizeof(fs_source),
                     .pCode = fs_source,
                     .flags = 0
                  },
                  &fs);

   VkPipelineShaderStageCreateInfo vs_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &ia_create_info,
      .shader = {
         .stage = VK_SHADER_STAGE_VERTEX,
         .shader = vs,
         .linkConstBufferCount = 0,
         .pLinkConstBufferInfo = NULL,
         .pSpecializationInfo = NULL
      }
   };

   VkPipelineShaderStageCreateInfo fs_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &vs_create_info,
      .shader = {
         .stage = VK_SHADER_STAGE_FRAGMENT,
         .shader = fs,
         .linkConstBufferCount = 0,
         .pLinkConstBufferInfo = NULL,
         .pSpecializationInfo = NULL
      }
   };

   VkPipelineVertexInputCreateInfo vi_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_CREATE_INFO,
      .pNext = &fs_create_info,
      .bindingCount = 2,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .strideInBytes = 16,
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         },
         {
            .binding = 1,
            .strideInBytes = 0,
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         }
      },
      .attributeCount = 2,
      .pVertexAttributeDescriptions = (VkVertexInputAttributeDescription[]) {
         {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offsetInBytes = 0
         },
         {
            .location = 1,
            .binding = 1,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offsetInBytes = 0
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

   vkCreateGraphicsPipeline(device,
                            &(VkGraphicsPipelineCreateInfo) {
                               .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                               .pNext = &rs_create_info,
                               .flags = 0,
                               .layout = pipeline_layout
                            },
                            pipeline);


   vkDestroyObject(device, VK_OBJECT_TYPE_SHADER, fs);
   vkDestroyObject(device, VK_OBJECT_TYPE_SHADER, vs);
}

int main(int argc, char *argv[])
{
   VkInstance instance;
   vkCreateInstance(&(VkInstanceCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pAllocCb = &(VkAllocCallbacks) {
            .pUserData = NULL,
            .pfnAlloc = test_alloc,
            .pfnFree = test_free
         },
         .pAppInfo = &(VkApplicationInfo) {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pAppName = "vk",
            .apiVersion = 1
         }
      },
      &instance);

   uint32_t count = 1;
   VkPhysicalDevice physicalDevices[1];
   vkEnumeratePhysicalDevices(instance, &count, physicalDevices);
   printf("%d physical devices\n", count);

   VkPhysicalDeviceProperties properties;
   size_t size = sizeof(properties);
   vkGetPhysicalDeviceInfo(physicalDevices[0],
                           VK_PHYSICAL_DEVICE_INFO_TYPE_PROPERTIES,
                           &size, &properties);
   printf("vendor id %04x, device name %s\n",
          properties.vendorId, properties.deviceName);

   VkDevice device;
   vkCreateDevice(physicalDevices[0],
                  &(VkDeviceCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                     .queueRecordCount = 1,
                     .pRequestedQueues = &(VkDeviceQueueCreateInfo) {
                        .queueNodeIndex = 0,
                        .queueCount = 1                       
                     }
                  },
                  &device);

   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);

   VkCmdBuffer cmdBuffer;
   vkCreateCommandBuffer(device,
                         &(VkCmdBufferCreateInfo) {
                            .sType = VK_STRUCTURE_TYPE_CMD_BUFFER_CREATE_INFO,
                            .queueNodeIndex = 0,
                            .flags = 0
                         },
                         &cmdBuffer);


   VkDescriptorSetLayout set_layout[2];
   vkCreateDescriptorSetLayout(device,
                               &(VkDescriptorSetLayoutCreateInfo) {
                                  .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                  .count = 2,
                                  .pBinding = (VkDescriptorSetLayoutBinding[]) {
                                     {
                                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                        .count = 2,
                                        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                                        .pImmutableSamplers = NULL
                                     },
                                     {
                                        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                        .count = 1,
                                        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                        .pImmutableSamplers = NULL
                                     }
                                  }
                               },
                               &set_layout[0]);

   vkCreateDescriptorSetLayout(device,
                               &(VkDescriptorSetLayoutCreateInfo) {
                                  .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                  .count = 1,
                                  .pBinding = (VkDescriptorSetLayoutBinding[]) {
                                     {
                                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                        .count = 1,
                                        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                                        .pImmutableSamplers = NULL
                                     }
                                  }
                               },
                               &set_layout[1]);

   VkPipelineLayout pipeline_layout;
   vkCreatePipelineLayout(device,
                          &(VkPipelineLayoutCreateInfo) {
                             .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                             .descriptorSetCount = 2,
                             .pSetLayouts = set_layout,
                          },
                          &pipeline_layout);

   VkPipeline pipeline;
   create_pipeline(device, &pipeline, pipeline_layout);

   VkDescriptorSet set[2];
   vkAllocDescriptorSets(device, 0 /* pool */,
                         VK_DESCRIPTOR_SET_USAGE_STATIC,
                         2, set_layout, set, &count);

   VkBuffer buffer;
   vkCreateBuffer(device,
                  &(VkBufferCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                     .size = 1024,
                     .usage = VK_BUFFER_USAGE_GENERAL,
                     .flags = 0
                  },
                  &buffer);

   VkMemoryRequirements buffer_requirements;
   size = sizeof(buffer_requirements);
   vkGetObjectInfo(device, VK_OBJECT_TYPE_BUFFER, buffer,
                   VK_OBJECT_INFO_TYPE_MEMORY_REQUIREMENTS,
                   &size, &buffer_requirements);

   int32_t width = 256, height = 256;

   VkImage rt;
   vkCreateImage(device,
                 &(VkImageCreateInfo) {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .imageType = VK_IMAGE_TYPE_2D,
                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                    .extent = { .width = width, .height = height, .depth = 1 },
                    .mipLevels = 1,
                    .arraySize = 1,
                    .samples = 1,
                    .tiling = VK_IMAGE_TILING_LINEAR,
                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                    .flags = 0,
                 },
                 &rt);

   VkMemoryRequirements rt_requirements;
   size = sizeof(rt_requirements);
   vkGetObjectInfo(device, VK_OBJECT_TYPE_IMAGE, rt,
                   VK_OBJECT_INFO_TYPE_MEMORY_REQUIREMENTS,
                   &size, &rt_requirements);

   VkBuffer vertex_buffer;
   vkCreateBuffer(device,
                  &(VkBufferCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                     .size = 1024,
                     .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     .flags = 0
                  },
                  &vertex_buffer);

   VkMemoryRequirements vb_requirements;
   size = sizeof(vb_requirements);
   vkGetObjectInfo(device, VK_OBJECT_TYPE_BUFFER, vertex_buffer,
                   VK_OBJECT_INFO_TYPE_MEMORY_REQUIREMENTS,
                   &size, &vb_requirements);

   printf("buffer size: %lu, buffer alignment: %lu\n",
          buffer_requirements.size, buffer_requirements.alignment);
   printf("rt size: %lu, rt alignment: %lu\n",
          rt_requirements.size, rt_requirements.alignment);
   printf("vb size: %lu vb alignment: %lu\n",
          vb_requirements.size, vb_requirements.alignment);

   size_t mem_size = rt_requirements.size + 2048 + 16 * 16 * 4;
   VkDeviceMemory mem;
   vkAllocMemory(device,
                 &(VkMemoryAllocInfo) {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO,
                    .allocationSize = mem_size,
                    .memProps = VK_MEMORY_PROPERTY_HOST_DEVICE_COHERENT_BIT,
                    .memPriority = VK_MEMORY_PRIORITY_NORMAL
                 },
                 &mem);

   void *map;
   vkMapMemory(device, mem, 0, mem_size, 0, &map);
   memset(map, 192, mem_size);

   vkQueueBindObjectMemory(queue, VK_OBJECT_TYPE_BUFFER,
                           buffer,
                           0, /* allocation index; for objects which need to bind to multiple mems */
                           mem, 128);

   float color[12] = {
      0.0, 0.2, 0.0, 0.0,
      0.0, 0.0, 0.5, 0.0,
      0.0, 0.0, 0.5, 0.5
   };
   memcpy(map + 128 + 16, color, sizeof(color));
   VkBufferView buffer_view[3];
   vkCreateBufferView(device,
                      &(VkBufferViewCreateInfo) {
                         .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                         .buffer = buffer,
                         .viewType = VK_BUFFER_VIEW_TYPE_RAW,
                         .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                         .offset = 16,
                         .range = 64
                      },
                      &buffer_view[0]);

   vkCreateBufferView(device,
                      &(VkBufferViewCreateInfo) {
                         .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                         .buffer = buffer,
                         .viewType = VK_BUFFER_VIEW_TYPE_RAW,
                         .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                         .offset = 32,
                         .range = 64
                      },
                      &buffer_view[1]);

   vkCreateBufferView(device,
                      &(VkBufferViewCreateInfo) {
                         .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                         .buffer = buffer,
                         .viewType = VK_BUFFER_VIEW_TYPE_RAW,
                         .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                         .offset = 48,
                         .range = 64
                      },
                      &buffer_view[2]);

   vkQueueBindObjectMemory(queue, VK_OBJECT_TYPE_BUFFER,
                           vertex_buffer,
                           0, /* allocation index; for objects which need to bind to multiple mems */
                           mem, 1024);
   static const float vertex_data[] = {
      /* Triangle coordinates */
      -0.5, -0.5, 0.0, 1.0,
       0.5, -0.5, 0.0, 1.0,
       0.0,  0.5, 0.0, 1.0,
      /* Color */
       1.0,  0.0, 0.0, 0.2,
   };
   memcpy(map + 1024, vertex_data, sizeof(vertex_data));

   VkDynamicVpState vp_state;
   vkCreateDynamicViewportState(device,
                                &(VkDynamicVpStateCreateInfo) {
                                   .sType = VK_STRUCTURE_TYPE_DYNAMIC_VP_STATE_CREATE_INFO,
                                   .viewportAndScissorCount = 2,
                                   .pViewports = (VkViewport[]) {
                                      {
                                         .originX = 0,
                                         .originY = 0,
                                         .width = width,
                                         .height = height,
                                         .minDepth = 0,
                                         .maxDepth = 1
                                      },
                                      {
                                         .originX = -10,
                                         .originY = -10,
                                         .width = 20,
                                         .height = 20,
                                         .minDepth = -1,
                                         .maxDepth = 1
                                      },
                                   },
                                   .pScissors = (VkRect[]) {
                                      { {  0,  0 }, { width, height } },
                                      { { 10, 10 }, { 236, 236 } }
                                   }
                                },
                                &vp_state);

   VkDynamicRsState rs_state;
   vkCreateDynamicRasterState(device,
                              &(VkDynamicRsStateCreateInfo) {
                                 .sType = VK_STRUCTURE_TYPE_DYNAMIC_RS_STATE_CREATE_INFO,
                              },
                              &rs_state);
   
   /* FIXME: Need to query memory info before binding to memory */
   vkQueueBindObjectMemory(queue, VK_OBJECT_TYPE_IMAGE,
                           rt,
                           0, /* allocation index; for objects which need to bind to multiple mems */
                           mem, 2048);

   const uint32_t texture_width = 16, texture_height = 16;
   VkImage texture;
   vkCreateImage(device,
                 &(VkImageCreateInfo) {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .imageType = VK_IMAGE_TYPE_2D,
                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                    .extent = { .width = texture_width, .height = texture_height, .depth = 1 },
                    .mipLevels = 1,
                    .arraySize = 1,
                    .samples = 1,
                    .tiling = VK_IMAGE_TILING_LINEAR,
                    .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                    .flags = 0,
                 },
                 &texture);

   VkImageView image_view;
   vkCreateImageView(device,
                     &(VkImageViewCreateInfo) {
                        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                        .image = texture,
                        .viewType = VK_IMAGE_VIEW_TYPE_2D,
                        .format = VK_FORMAT_R8G8B8A8_UNORM,
                        .channels = {
                           VK_CHANNEL_SWIZZLE_R,
                           VK_CHANNEL_SWIZZLE_G,
                           VK_CHANNEL_SWIZZLE_B,
                           VK_CHANNEL_SWIZZLE_A
                        },
                        .subresourceRange = {
                           .aspect = VK_IMAGE_ASPECT_COLOR,
                           .baseMipLevel = 0,
                           .mipLevels = 1,
                           .baseArraySlice = 0,
                           .arraySize = 1
                        },
                        .minLod = 0
                      },
                      &image_view);

   vkQueueBindObjectMemory(queue, VK_OBJECT_TYPE_IMAGE,
                           texture,
                           0, /* allocation index; for objects which need to bind to multiple mems */
                           mem, 2048 + 256 * 256 * 4);

   vkUpdateDescriptors(device, set[0], 2,
                       (const void * []) {
                          &(VkUpdateBuffers) {
                             .sType = VK_STRUCTURE_TYPE_UPDATE_BUFFERS,
                             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                             .arrayIndex = 0,
                             .binding = 0,
                             .count = 2,
                             .pBufferViews = (VkBufferViewAttachInfo[]) {
                                {
                                   .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_ATTACH_INFO,
                                   .view = buffer_view[0]
                                },
                                {
                                   .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_ATTACH_INFO,
                                   .view = buffer_view[1]
                                }
                             }
                          },
                          &(VkUpdateImages) {
                             .sType = VK_STRUCTURE_TYPE_UPDATE_IMAGES,
                             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                             .binding = 2,
                             .count = 1,
                             .pImageViews = (VkImageViewAttachInfo[]) {
                                {
                                   .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_ATTACH_INFO,
                                   .view = image_view,
                                   .layout = VK_IMAGE_LAYOUT_GENERAL,
                                }
                             }
                          }
                       });

   vkUpdateDescriptors(device, set[1], 1,
                       (const void * []) {
                          &(VkUpdateBuffers) {
                             .sType = VK_STRUCTURE_TYPE_UPDATE_BUFFERS,
                             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                             .arrayIndex = 0,
                             .count = 1,
                             .pBufferViews = (VkBufferViewAttachInfo[]) {
                                {
                                   .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_ATTACH_INFO,
                                   .view = buffer_view[2]
                                }
                             }
                          }
                       });

   VkColorAttachmentView view;
   vkCreateColorAttachmentView(device,
                               &(VkColorAttachmentViewCreateInfo) {
                                  .sType = VK_STRUCTURE_TYPE_COLOR_ATTACHMENT_VIEW_CREATE_INFO,
                                  .image = rt,
                                  .format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .mipLevel = 0,
                                  .baseArraySlice = 0,
                                  .arraySize = 1,
                                  .msaaResolveImage = 0,
                                  .msaaResolveSubResource = { 0, }
                               },
                               &view);
   
   VkFramebuffer framebuffer;
   vkCreateFramebuffer(device,
                       &(VkFramebufferCreateInfo) {
                          .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = (VkColorAttachmentBindInfo[]) {
                             {
                                .view = view,
                                .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                             }
                          },
                          .pDepthStencilAttachment = NULL,
                          .sampleCount = 1,
                          .width = width,
                          .height = height,
                          .layers = 1
                       },
                       &framebuffer);

   VkRenderPass pass;
   vkCreateRenderPass(device,
                      &(VkRenderPassCreateInfo) {
                         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                         .renderArea = { { 0, 0 }, { width, height } },
                         .colorAttachmentCount = 1,
                         .extent = { },
                         .sampleCount = 1,
                         .layers = 1,
                         .pColorFormats = (VkFormat[]) { VK_FORMAT_R8G8B8A8_UNORM },
                         .pColorLayouts = (VkImageLayout[]) { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
                         .pColorLoadOps = (VkAttachmentLoadOp[]) { VK_ATTACHMENT_LOAD_OP_CLEAR },
                         .pColorStoreOps = (VkAttachmentStoreOp[]) { VK_ATTACHMENT_STORE_OP_STORE },
                         .pColorLoadClearValues = (VkClearColor[]) {
                            { .color = { .floatColor = { 1.0, 0.0, 0.0, 1.0 } }, .useRawValue = false }
                         },
                         .depthStencilFormat = VK_FORMAT_UNDEFINED,
                      },
                      &pass);

   vkBeginCommandBuffer(cmdBuffer,
                        &(VkCmdBufferBeginInfo) {
                           .sType = VK_STRUCTURE_TYPE_CMD_BUFFER_BEGIN_INFO,
                           .flags = 0
                        });

   vkCmdBeginRenderPass(cmdBuffer,
                        &(VkRenderPassBegin) {
                           .renderPass = pass,
                           .framebuffer = framebuffer
                        });

   vkCmdBindVertexBuffers(cmdBuffer, 0, 2,
                          (VkBuffer[]) { vertex_buffer, vertex_buffer },
                          (VkDeviceSize[]) { 0, 3 * 4 * sizeof(float) });

   vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

   vkCmdBindDescriptorSets(cmdBuffer,
                           VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 1,
                           &set[0], 0, NULL);
   vkCmdBindDescriptorSets(cmdBuffer,
                           VK_PIPELINE_BIND_POINT_GRAPHICS, 1, 1,
                           &set[1], 0, NULL);

   vkCmdBindDynamicStateObject(cmdBuffer,
                               VK_STATE_BIND_POINT_VIEWPORT, vp_state);
   vkCmdBindDynamicStateObject(cmdBuffer,
                               VK_STATE_BIND_POINT_RASTER, rs_state);

   vkCmdWriteTimestamp(cmdBuffer, VK_TIMESTAMP_TYPE_TOP, buffer, 0);
   vkCmdWriteTimestamp(cmdBuffer, VK_TIMESTAMP_TYPE_BOTTOM, buffer, 8);

   vkCmdDraw(cmdBuffer, 0, 3, 0, 1);

   vkCmdEndRenderPass(cmdBuffer, pass);

   vkEndCommandBuffer(cmdBuffer);

   vkQueueSubmit(queue, 1, &cmdBuffer, 0);

   vkQueueWaitIdle(queue);

   write_png("vk.png", width, height, 1024, map + 2048);

   vkDestroyObject(device, VK_OBJECT_TYPE_IMAGE, texture);
   vkDestroyObject(device, VK_OBJECT_TYPE_IMAGE, rt);
   vkDestroyObject(device, VK_OBJECT_TYPE_BUFFER, buffer);
   vkDestroyObject(device, VK_OBJECT_TYPE_COMMAND_BUFFER, cmdBuffer);
   vkDestroyObject(device, VK_OBJECT_TYPE_PIPELINE, pipeline);

   vkDestroyDevice(device);

   vkDestroyInstance(instance);

   return 0;
}
