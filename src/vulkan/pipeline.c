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

// Shader functions

VkResult anv_CreateShader(
    VkDevice                                    _device,
    const VkShaderCreateInfo*                   pCreateInfo,
    VkShader*                                   pShader)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_shader *shader;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_CREATE_INFO);

   shader = anv_device_alloc(device, sizeof(*shader) + pCreateInfo->codeSize, 8,
                               VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (shader == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   shader->size = pCreateInfo->codeSize;
   memcpy(shader->data, pCreateInfo->pCode, shader->size);

   *pShader = (VkShader) shader;

   return VK_SUCCESS;
}

// Pipeline functions

static void
emit_vertex_input(struct anv_pipeline *pipeline, VkPipelineVertexInputCreateInfo *info)
{
   const uint32_t num_dwords = 1 + info->attributeCount * 2;
   uint32_t *p;
   bool instancing_enable[32];

   pipeline->vb_used = 0;
   for (uint32_t i = 0; i < info->bindingCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &info->pVertexBindingDescriptions[i];

      pipeline->vb_used |= 1 << desc->binding;
      pipeline->binding_stride[desc->binding] = desc->strideInBytes;

      /* Step rate is programmed per vertex element (attribute), not
       * binding. Set up a map of which bindings step per instance, for
       * reference by vertex element setup. */
      switch (desc->stepRate) {
      default:
      case VK_VERTEX_INPUT_STEP_RATE_VERTEX:
         instancing_enable[desc->binding] = false;
         break;
      case VK_VERTEX_INPUT_STEP_RATE_INSTANCE:
         instancing_enable[desc->binding] = true;
         break;
      }
   }

   p = anv_batch_emitn(&pipeline->batch, num_dwords,
                       GEN8_3DSTATE_VERTEX_ELEMENTS);

   for (uint32_t i = 0; i < info->attributeCount; i++) {
      const VkVertexInputAttributeDescription *desc =
         &info->pVertexAttributeDescriptions[i];
      const struct anv_format *format = anv_format_for_vk_format(desc->format);

      struct GEN8_VERTEX_ELEMENT_STATE element = {
         .VertexBufferIndex = desc->binding,
         .Valid = true,
         .SourceElementFormat = format->surface_format,
         .EdgeFlagEnable = false,
         .SourceElementOffset = desc->offsetInBytes,
         .Component0Control = VFCOMP_STORE_SRC,
         .Component1Control = format->num_channels >= 2 ? VFCOMP_STORE_SRC : VFCOMP_STORE_0,
         .Component2Control = format->num_channels >= 3 ? VFCOMP_STORE_SRC : VFCOMP_STORE_0,
         .Component3Control = format->num_channels >= 4 ? VFCOMP_STORE_SRC : VFCOMP_STORE_1_FP
      };
      GEN8_VERTEX_ELEMENT_STATE_pack(NULL, &p[1 + i * 2], &element);

      anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VF_INSTANCING,
                     .InstancingEnable = instancing_enable[desc->binding],
                     .VertexElementIndex = i,
                     /* Vulkan so far doesn't have an instance divisor, so
                      * this is always 1 (ignored if not instancing). */
                     .InstanceDataStepRate = 1);
   }

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VF_SGVS,
                  .VertexIDEnable = pipeline->vs_prog_data.uses_vertexid,
                  .VertexIDComponentNumber = 2,
                  .VertexIDElementOffset = info->bindingCount,
                  .InstanceIDEnable = pipeline->vs_prog_data.uses_instanceid,
                  .InstanceIDComponentNumber = 3,
                  .InstanceIDElementOffset = info->bindingCount);
}

static void
emit_ia_state(struct anv_pipeline *pipeline,
              VkPipelineIaStateCreateInfo *info,
              const struct anv_pipeline_create_info *extra)
{
   static const uint32_t vk_to_gen_primitive_type[] = {
      [VK_PRIMITIVE_TOPOLOGY_POINT_LIST]        = _3DPRIM_POINTLIST,
      [VK_PRIMITIVE_TOPOLOGY_LINE_LIST]         = _3DPRIM_LINELIST,
      [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP]        = _3DPRIM_LINESTRIP,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST]     = _3DPRIM_TRILIST,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP]    = _3DPRIM_TRISTRIP,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN]      = _3DPRIM_TRIFAN,
      [VK_PRIMITIVE_TOPOLOGY_LINE_LIST_ADJ]     = _3DPRIM_LINELIST_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_ADJ]    = _3DPRIM_LINESTRIP_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_ADJ] = _3DPRIM_TRILIST_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_ADJ] = _3DPRIM_TRISTRIP_ADJ,
      [VK_PRIMITIVE_TOPOLOGY_PATCH]             = _3DPRIM_PATCHLIST_1
   };
   uint32_t topology = vk_to_gen_primitive_type[info->topology];

   if (extra && extra->use_rectlist)
      topology = _3DPRIM_RECTLIST;

   struct GEN8_3DSTATE_VF vf = {
      GEN8_3DSTATE_VF_header,
      .IndexedDrawCutIndexEnable = info->primitiveRestartEnable,
   };
   GEN8_3DSTATE_VF_pack(NULL, pipeline->state_vf, &vf);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VF_TOPOLOGY,
                  .PrimitiveTopologyType = topology);
}

static void
emit_rs_state(struct anv_pipeline *pipeline, VkPipelineRsStateCreateInfo *info,
              const struct anv_pipeline_create_info *extra)
{
   static const uint32_t vk_to_gen_cullmode[] = {
      [VK_CULL_MODE_NONE]                       = CULLMODE_NONE,
      [VK_CULL_MODE_FRONT]                      = CULLMODE_FRONT,
      [VK_CULL_MODE_BACK]                       = CULLMODE_BACK,
      [VK_CULL_MODE_FRONT_AND_BACK]             = CULLMODE_BOTH
   };

   static const uint32_t vk_to_gen_fillmode[] = {
      [VK_FILL_MODE_POINTS]                     = RASTER_POINT,
      [VK_FILL_MODE_WIREFRAME]                  = RASTER_WIREFRAME,
      [VK_FILL_MODE_SOLID]                      = RASTER_SOLID
   };

   static const uint32_t vk_to_gen_front_face[] = {
      [VK_FRONT_FACE_CCW]                       = CounterClockwise,
      [VK_FRONT_FACE_CW]                        = Clockwise
   };

   struct GEN8_3DSTATE_SF sf = {
      GEN8_3DSTATE_SF_header,
      .ViewportTransformEnable = !(extra && extra->disable_viewport),
      .TriangleStripListProvokingVertexSelect = 0,
      .LineStripListProvokingVertexSelect = 0,
      .TriangleFanProvokingVertexSelect = 0,
      .PointWidthSource = pipeline->writes_point_size ? Vertex : State,
      .PointWidth = 1.0,
   };

   /* FINISHME: bool32_t rasterizerDiscardEnable; */

   GEN8_3DSTATE_SF_pack(NULL, pipeline->state_sf, &sf);

   struct GEN8_3DSTATE_RASTER raster = {
      GEN8_3DSTATE_RASTER_header,
      .FrontWinding = vk_to_gen_front_face[info->frontFace],
      .CullMode = vk_to_gen_cullmode[info->cullMode],
      .FrontFaceFillMode = vk_to_gen_fillmode[info->fillMode],
      .BackFaceFillMode = vk_to_gen_fillmode[info->fillMode],
      .ScissorRectangleEnable = !(extra && extra->disable_scissor),
      .ViewportZClipTestEnable = info->depthClipEnable
   };

   GEN8_3DSTATE_RASTER_pack(NULL, pipeline->state_raster, &raster);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_SBE,
                  .ForceVertexURBEntryReadLength = false,
                  .ForceVertexURBEntryReadOffset = false,
                  .PointSpriteTextureCoordinateOrigin = UPPERLEFT,
                  .NumberofSFOutputAttributes =
                     pipeline->wm_prog_data.num_varying_inputs);

}

static void
emit_cb_state(struct anv_pipeline *pipeline, VkPipelineCbStateCreateInfo *info)
{
   struct anv_device *device = pipeline->device;

   static const uint32_t vk_to_gen_logic_op[] = {
      [VK_LOGIC_OP_COPY]                        = LOGICOP_COPY,
      [VK_LOGIC_OP_CLEAR]                       = LOGICOP_CLEAR,
      [VK_LOGIC_OP_AND]                         = LOGICOP_AND,
      [VK_LOGIC_OP_AND_REVERSE]                 = LOGICOP_AND_REVERSE,
      [VK_LOGIC_OP_AND_INVERTED]                = LOGICOP_AND_INVERTED,
      [VK_LOGIC_OP_NOOP]                        = LOGICOP_NOOP,
      [VK_LOGIC_OP_XOR]                         = LOGICOP_XOR,
      [VK_LOGIC_OP_OR]                          = LOGICOP_OR,
      [VK_LOGIC_OP_NOR]                         = LOGICOP_NOR,
      [VK_LOGIC_OP_EQUIV]                       = LOGICOP_EQUIV,
      [VK_LOGIC_OP_INVERT]                      = LOGICOP_INVERT,
      [VK_LOGIC_OP_OR_REVERSE]                  = LOGICOP_OR_REVERSE,
      [VK_LOGIC_OP_COPY_INVERTED]               = LOGICOP_COPY_INVERTED,
      [VK_LOGIC_OP_OR_INVERTED]                 = LOGICOP_OR_INVERTED,
      [VK_LOGIC_OP_NAND]                        = LOGICOP_NAND,
      [VK_LOGIC_OP_SET]                         = LOGICOP_SET,
   };

   static const uint32_t vk_to_gen_blend[] = {
      [VK_BLEND_ZERO]                           = BLENDFACTOR_ZERO,
      [VK_BLEND_ONE]                            = BLENDFACTOR_ONE,
      [VK_BLEND_SRC_COLOR]                      = BLENDFACTOR_SRC_COLOR,
      [VK_BLEND_ONE_MINUS_SRC_COLOR]            = BLENDFACTOR_INV_SRC_COLOR,
      [VK_BLEND_DEST_COLOR]                     = BLENDFACTOR_DST_COLOR,
      [VK_BLEND_ONE_MINUS_DEST_COLOR]           = BLENDFACTOR_INV_DST_COLOR,
      [VK_BLEND_SRC_ALPHA]                      = BLENDFACTOR_SRC_ALPHA,
      [VK_BLEND_ONE_MINUS_SRC_ALPHA]            = BLENDFACTOR_INV_SRC_ALPHA,
      [VK_BLEND_DEST_ALPHA]                     = BLENDFACTOR_DST_ALPHA,
      [VK_BLEND_ONE_MINUS_DEST_ALPHA]           = BLENDFACTOR_INV_DST_ALPHA,
      [VK_BLEND_CONSTANT_COLOR]                 = BLENDFACTOR_CONST_COLOR,
      [VK_BLEND_ONE_MINUS_CONSTANT_COLOR]       = BLENDFACTOR_INV_CONST_COLOR,
      [VK_BLEND_CONSTANT_ALPHA]                 = BLENDFACTOR_CONST_ALPHA,
      [VK_BLEND_ONE_MINUS_CONSTANT_ALPHA]       = BLENDFACTOR_INV_CONST_ALPHA,
      [VK_BLEND_SRC_ALPHA_SATURATE]             = BLENDFACTOR_SRC_ALPHA_SATURATE,
      [VK_BLEND_SRC1_COLOR]                     = BLENDFACTOR_SRC1_COLOR,
      [VK_BLEND_ONE_MINUS_SRC1_COLOR]           = BLENDFACTOR_INV_SRC1_COLOR,
      [VK_BLEND_SRC1_ALPHA]                     = BLENDFACTOR_SRC1_ALPHA,
      [VK_BLEND_ONE_MINUS_SRC1_ALPHA]           = BLENDFACTOR_INV_SRC1_ALPHA,
   };

   static const uint32_t vk_to_gen_blend_op[] = {
      [VK_BLEND_OP_ADD]                         = BLENDFUNCTION_ADD,
      [VK_BLEND_OP_SUBTRACT]                    = BLENDFUNCTION_SUBTRACT,
      [VK_BLEND_OP_REVERSE_SUBTRACT]            = BLENDFUNCTION_REVERSE_SUBTRACT,
      [VK_BLEND_OP_MIN]                         = BLENDFUNCTION_MIN,
      [VK_BLEND_OP_MAX]                         = BLENDFUNCTION_MAX,
   };

   uint32_t num_dwords = 1 + info->attachmentCount * 2;
   pipeline->blend_state =
      anv_state_pool_alloc(&device->dynamic_state_pool, num_dwords * 4, 64);

   struct GEN8_BLEND_STATE blend_state = {
      .AlphaToCoverageEnable = info->alphaToCoverageEnable,
   };

   uint32_t *state = pipeline->blend_state.map;
   GEN8_BLEND_STATE_pack(NULL, state, &blend_state);

   for (uint32_t i = 0; i < info->attachmentCount; i++) {
      const VkPipelineCbAttachmentState *a = &info->pAttachments[i];

      struct GEN8_BLEND_STATE_ENTRY entry = {
         .LogicOpEnable = info->logicOpEnable,
         .LogicOpFunction = vk_to_gen_logic_op[info->logicOp],
         .ColorBufferBlendEnable = a->blendEnable,
         .PreBlendSourceOnlyClampEnable = false,
         .PreBlendColorClampEnable = false,
         .PostBlendColorClampEnable = false,
         .SourceBlendFactor = vk_to_gen_blend[a->srcBlendColor],
         .DestinationBlendFactor = vk_to_gen_blend[a->destBlendColor],
         .ColorBlendFunction = vk_to_gen_blend_op[a->blendOpColor],
         .SourceAlphaBlendFactor = vk_to_gen_blend[a->srcBlendAlpha],
         .DestinationAlphaBlendFactor = vk_to_gen_blend[a->destBlendAlpha],
         .AlphaBlendFunction = vk_to_gen_blend_op[a->blendOpAlpha],
         .WriteDisableAlpha = !(a->channelWriteMask & VK_CHANNEL_A_BIT),
         .WriteDisableRed = !(a->channelWriteMask & VK_CHANNEL_R_BIT),
         .WriteDisableGreen = !(a->channelWriteMask & VK_CHANNEL_G_BIT),
         .WriteDisableBlue = !(a->channelWriteMask & VK_CHANNEL_B_BIT),
      };

      GEN8_BLEND_STATE_ENTRY_pack(NULL, state + i * 2 + 1, &entry);
   }

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_BLEND_STATE_POINTERS,
                  .BlendStatePointer = pipeline->blend_state.offset,
                  .BlendStatePointerValid = true);
}

static const uint32_t vk_to_gen_compare_op[] = {
   [VK_COMPARE_OP_NEVER]                        = COMPAREFUNCTION_NEVER,
   [VK_COMPARE_OP_LESS]                         = COMPAREFUNCTION_LESS,
   [VK_COMPARE_OP_EQUAL]                        = COMPAREFUNCTION_EQUAL,
   [VK_COMPARE_OP_LESS_EQUAL]                   = COMPAREFUNCTION_LEQUAL,
   [VK_COMPARE_OP_GREATER]                      = COMPAREFUNCTION_GREATER,
   [VK_COMPARE_OP_NOT_EQUAL]                    = COMPAREFUNCTION_NOTEQUAL,
   [VK_COMPARE_OP_GREATER_EQUAL]                = COMPAREFUNCTION_GEQUAL,
   [VK_COMPARE_OP_ALWAYS]                       = COMPAREFUNCTION_ALWAYS,
};

static const uint32_t vk_to_gen_stencil_op[] = {
   [VK_STENCIL_OP_KEEP]                         = 0,
   [VK_STENCIL_OP_ZERO]                         = 0,
   [VK_STENCIL_OP_REPLACE]                      = 0,
   [VK_STENCIL_OP_INC_CLAMP]                    = 0,
   [VK_STENCIL_OP_DEC_CLAMP]                    = 0,
   [VK_STENCIL_OP_INVERT]                       = 0,
   [VK_STENCIL_OP_INC_WRAP]                     = 0,
   [VK_STENCIL_OP_DEC_WRAP]                     = 0
};

static void
emit_ds_state(struct anv_pipeline *pipeline, VkPipelineDsStateCreateInfo *info)
{
   if (info == NULL) {
      /* We're going to OR this together with the dynamic state.  We need
       * to make sure it's initialized to something useful.
       */
      memset(pipeline->state_wm_depth_stencil, 0,
             sizeof(pipeline->state_wm_depth_stencil));
      return;
   }

   /* bool32_t depthBoundsEnable;          // optional (depth_bounds_test) */

   struct GEN8_3DSTATE_WM_DEPTH_STENCIL wm_depth_stencil = {
      .DepthTestEnable = info->depthTestEnable,
      .DepthBufferWriteEnable = info->depthWriteEnable,
      .DepthTestFunction = vk_to_gen_compare_op[info->depthCompareOp],
      .DoubleSidedStencilEnable = true,

      .StencilTestEnable = info->stencilTestEnable,
      .StencilFailOp = vk_to_gen_stencil_op[info->front.stencilFailOp],
      .StencilPassDepthPassOp = vk_to_gen_stencil_op[info->front.stencilPassOp],
      .StencilPassDepthFailOp = vk_to_gen_stencil_op[info->front.stencilDepthFailOp],
      .StencilTestFunction = vk_to_gen_compare_op[info->front.stencilCompareOp],
      .BackfaceStencilFailOp = vk_to_gen_stencil_op[info->back.stencilFailOp],
      .BackfaceStencilPassDepthPassOp = vk_to_gen_stencil_op[info->back.stencilPassOp],
      .BackfaceStencilPassDepthFailOp =vk_to_gen_stencil_op[info->back.stencilDepthFailOp],
      .BackfaceStencilTestFunction = vk_to_gen_compare_op[info->back.stencilCompareOp],
   };

   GEN8_3DSTATE_WM_DEPTH_STENCIL_pack(NULL, pipeline->state_wm_depth_stencil, &wm_depth_stencil);
}

VkResult anv_CreateGraphicsPipeline(
    VkDevice                                    device,
    const VkGraphicsPipelineCreateInfo*         pCreateInfo,
    VkPipeline*                                 pPipeline)
{
   return anv_pipeline_create(device, pCreateInfo, NULL, pPipeline);
}

static void
anv_pipeline_destroy(struct anv_device *device,
                     struct anv_object *object,
                     VkObjectType obj_type)
{
   struct anv_pipeline *pipeline = (struct anv_pipeline*) object;

   assert(obj_type == VK_OBJECT_TYPE_PIPELINE);

   anv_compiler_free(pipeline);
   anv_reloc_list_finish(&pipeline->batch.relocs, pipeline->device);
   anv_state_stream_finish(&pipeline->program_stream);
   anv_state_pool_free(&device->dynamic_state_pool, pipeline->blend_state);
   anv_device_free(pipeline->device, pipeline);
}

VkResult
anv_pipeline_create(
    VkDevice                                    _device,
    const VkGraphicsPipelineCreateInfo*         pCreateInfo,
    const struct anv_pipeline_create_info *     extra,
    VkPipeline*                                 pPipeline)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_pipeline *pipeline;
   const struct anv_common *common;
   VkPipelineShaderStageCreateInfo *shader_create_info;
   VkPipelineIaStateCreateInfo *ia_info = NULL;
   VkPipelineRsStateCreateInfo *rs_info = NULL;
   VkPipelineDsStateCreateInfo *ds_info = NULL;
   VkPipelineCbStateCreateInfo *cb_info = NULL;
   VkPipelineVertexInputCreateInfo *vi_info = NULL;
   VkResult result;
   uint32_t offset, length;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
   
   pipeline = anv_device_alloc(device, sizeof(*pipeline), 8,
                               VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pipeline == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->base.destructor = anv_pipeline_destroy;
   pipeline->device = device;
   pipeline->layout = (struct anv_pipeline_layout *) pCreateInfo->layout;
   memset(pipeline->shaders, 0, sizeof(pipeline->shaders));

   result = anv_reloc_list_init(&pipeline->batch.relocs, device);
   if (result != VK_SUCCESS) {
      anv_device_free(device, pipeline);
      return result;
   }
   pipeline->batch.next = pipeline->batch.start = pipeline->batch_data;
   pipeline->batch.end = pipeline->batch.start + sizeof(pipeline->batch_data);

   anv_state_stream_init(&pipeline->program_stream,
                         &device->instruction_block_pool);

   for (common = pCreateInfo->pNext; common; common = common->pNext) {
      switch (common->sType) {
      case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_CREATE_INFO:
         vi_info = (VkPipelineVertexInputCreateInfo *) common;
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_IA_STATE_CREATE_INFO:
         ia_info = (VkPipelineIaStateCreateInfo *) common;
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_TESS_STATE_CREATE_INFO:
         anv_finishme("VK_STRUCTURE_TYPE_PIPELINE_TESS_STATE_CREATE_INFO");
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_VP_STATE_CREATE_INFO:
         anv_finishme("VK_STRUCTURE_TYPE_PIPELINE_VP_STATE_CREATE_INFO");
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_RS_STATE_CREATE_INFO:
         rs_info = (VkPipelineRsStateCreateInfo *) common;
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_MS_STATE_CREATE_INFO:
         anv_finishme("VK_STRUCTURE_TYPE_PIPELINE_MS_STATE_CREATE_INFO");
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_CB_STATE_CREATE_INFO:
         cb_info = (VkPipelineCbStateCreateInfo *) common;
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_DS_STATE_CREATE_INFO:
         ds_info = (VkPipelineDsStateCreateInfo *) common;
         break;
      case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO:
         shader_create_info = (VkPipelineShaderStageCreateInfo *) common;
         pipeline->shaders[shader_create_info->shader.stage] =
            (struct anv_shader *) shader_create_info->shader.shader;
         break;
      default:
         break;
      }
   }

   pipeline->use_repclear = extra && extra->use_repclear;

   anv_compiler_run(device->compiler, pipeline);

   /* FIXME: The compiler dead-codes FS inputs when we don't have a VS, so we
    * hard code this to num_attributes - 2. This is because the attributes
    * include VUE header and position, which aren't counted as varying
    * inputs. */
   if (pipeline->vs_simd8 == NO_KERNEL)
      pipeline->wm_prog_data.num_varying_inputs = vi_info->attributeCount - 2;

   assert(vi_info);
   emit_vertex_input(pipeline, vi_info);
   assert(ia_info);
   emit_ia_state(pipeline, ia_info, extra);
   assert(rs_info);
   emit_rs_state(pipeline, rs_info, extra);
   emit_ds_state(pipeline, ds_info);
   emit_cb_state(pipeline, cb_info);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VF_STATISTICS,
                   .StatisticsEnable = true);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_HS, .Enable = false);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_TE, .TEEnable = false);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_DS, .FunctionEnable = false);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_STREAMOUT, .SOFunctionEnable = false);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_VS,
                  .ConstantBufferOffset = 0,
                  .ConstantBufferSize = 4);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_GS,
                  .ConstantBufferOffset = 4,
                  .ConstantBufferSize = 4);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_PS,
                  .ConstantBufferOffset = 8,
                  .ConstantBufferSize = 4);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_WM_CHROMAKEY,
                  .ChromaKeyKillEnable = false);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_SBE_SWIZ);
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_AA_LINE_PARAMETERS);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_CLIP,
                  .ClipEnable = true,
                  .ViewportXYClipTestEnable = !(extra && extra->disable_viewport),
                  .MinimumPointWidth = 0.125,
                  .MaximumPointWidth = 255.875);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_WM,
                  .StatisticsEnable = true,
                  .LineEndCapAntialiasingRegionWidth = _05pixels,
                  .LineAntialiasingRegionWidth = _10pixels,
                  .EarlyDepthStencilControl = NORMAL,
                  .ForceThreadDispatchEnable = NORMAL,
                  .PointRasterizationRule = RASTRULE_UPPER_RIGHT,
                  .BarycentricInterpolationMode =
                     pipeline->wm_prog_data.barycentric_interp_modes);

   uint32_t samples = 1;
   uint32_t log2_samples = __builtin_ffs(samples) - 1;
   bool enable_sampling = samples > 1 ? true : false;

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_MULTISAMPLE,
                  .PixelPositionOffsetEnable = enable_sampling,
                  .PixelLocation = CENTER,
                  .NumberofMultisamples = log2_samples);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_SAMPLE_MASK,
                  .SampleMask = 0xffff);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_URB_VS,
                  .VSURBStartingAddress = pipeline->urb.vs_start,
                  .VSURBEntryAllocationSize = pipeline->urb.vs_size - 1,
                  .VSNumberofURBEntries = pipeline->urb.nr_vs_entries);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_URB_GS,
                  .GSURBStartingAddress = pipeline->urb.gs_start,
                  .GSURBEntryAllocationSize = pipeline->urb.gs_size - 1,
                  .GSNumberofURBEntries = pipeline->urb.nr_gs_entries);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_URB_HS,
                  .HSURBStartingAddress = pipeline->urb.vs_start,
                  .HSURBEntryAllocationSize = 0,
                  .HSNumberofURBEntries = 0);

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_URB_DS,
                  .DSURBStartingAddress = pipeline->urb.vs_start,
                  .DSURBEntryAllocationSize = 0,
                  .DSNumberofURBEntries = 0);

   const struct brw_gs_prog_data *gs_prog_data = &pipeline->gs_prog_data;
   offset = 1;
   length = (gs_prog_data->base.vue_map.num_slots + 1) / 2 - offset;

   if (pipeline->gs_vec4 == NO_KERNEL)
      anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_GS, .Enable = false);
   else
      anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_GS,
                     .SingleProgramFlow = false,
                     .KernelStartPointer = pipeline->gs_vec4,
                     .VectorMaskEnable = Vmask,
                     .SamplerCount = 0,
                     .BindingTableEntryCount = 0,
                     .ExpectedVertexCount = pipeline->gs_vertex_count,
                        
                     .ScratchSpaceBasePointer = pipeline->scratch_start[VK_SHADER_STAGE_GEOMETRY],
                     .PerThreadScratchSpace = ffs(gs_prog_data->base.base.total_scratch / 2048),

                     .OutputVertexSize = gs_prog_data->output_vertex_size_hwords * 2 - 1,
                     .OutputTopology = gs_prog_data->output_topology,
                     .VertexURBEntryReadLength = gs_prog_data->base.urb_read_length,
                     .DispatchGRFStartRegisterForURBData =
                        gs_prog_data->base.base.dispatch_grf_start_reg,

                     .MaximumNumberofThreads = device->info.max_gs_threads,
                     .ControlDataHeaderSize = gs_prog_data->control_data_header_size_hwords,
                     //pipeline->gs_prog_data.dispatch_mode |
                     .StatisticsEnable = true,
                     .IncludePrimitiveID = gs_prog_data->include_primitive_id,
                     .ReorderMode = TRAILING,
                     .Enable = true,

                     .ControlDataFormat = gs_prog_data->control_data_format,

                     /* FIXME: mesa sets this based on ctx->Transform.ClipPlanesEnabled:
                      * UserClipDistanceClipTestEnableBitmask_3DSTATE_GS(v)
                      * UserClipDistanceCullTestEnableBitmask(v)
                      */

                     .VertexURBEntryOutputReadOffset = offset,
                     .VertexURBEntryOutputLength = length);

   const struct brw_vue_prog_data *vue_prog_data = &pipeline->vs_prog_data.base;
   /* Skip the VUE header and position slots */
   offset = 1;
   length = (vue_prog_data->vue_map.num_slots + 1) / 2 - offset;

   if (pipeline->vs_simd8 == NO_KERNEL || (extra && extra->disable_vs))
      anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VS,
                     .FunctionEnable = false,
                     .VertexURBEntryOutputReadOffset = 1,
                     /* Even if VS is disabled, SBE still gets the amount of
                      * vertex data to read from this field. We use attribute
                      * count - 1, as we don't count the VUE header here. */
                     .VertexURBEntryOutputLength =
                        DIV_ROUND_UP(vi_info->attributeCount - 1, 2));
   else
      anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_VS,
                     .KernelStartPointer = pipeline->vs_simd8,
                     .SingleVertexDispatch = Multiple,
                     .VectorMaskEnable = Dmask,
                     .SamplerCount = 0,
                     .BindingTableEntryCount =
                     vue_prog_data->base.binding_table.size_bytes / 4,
                     .ThreadDispatchPriority = Normal,
                     .FloatingPointMode = IEEE754,
                     .IllegalOpcodeExceptionEnable = false,
                     .AccessesUAV = false,
                     .SoftwareExceptionEnable = false,

                     .ScratchSpaceBasePointer = pipeline->scratch_start[VK_SHADER_STAGE_VERTEX],
                     .PerThreadScratchSpace = ffs(vue_prog_data->base.total_scratch / 2048),

                     .DispatchGRFStartRegisterForURBData =
                     vue_prog_data->base.dispatch_grf_start_reg,
                     .VertexURBEntryReadLength = vue_prog_data->urb_read_length,
                     .VertexURBEntryReadOffset = 0,

                     .MaximumNumberofThreads = device->info.max_vs_threads - 1,
                     .StatisticsEnable = false,
                     .SIMD8DispatchEnable = true,
                     .VertexCacheDisable = false,
                     .FunctionEnable = true,

                     .VertexURBEntryOutputReadOffset = offset,
                     .VertexURBEntryOutputLength = length,
                     .UserClipDistanceClipTestEnableBitmask = 0,
                     .UserClipDistanceCullTestEnableBitmask = 0);

   const struct brw_wm_prog_data *wm_prog_data = &pipeline->wm_prog_data;
   uint32_t ksp0, ksp2, grf_start0, grf_start2;

   ksp2 = 0;
   grf_start2 = 0;
   if (pipeline->ps_simd8 != NO_KERNEL) {
      ksp0 = pipeline->ps_simd8;
      grf_start0 = wm_prog_data->base.dispatch_grf_start_reg;
      if (pipeline->ps_simd16 != NO_KERNEL) {
         ksp2 = pipeline->ps_simd16;
         grf_start2 = wm_prog_data->dispatch_grf_start_reg_16;
      }
   } else if (pipeline->ps_simd16 != NO_KERNEL) {
      ksp0 = pipeline->ps_simd16;
      grf_start0 = wm_prog_data->dispatch_grf_start_reg_16;
   } else {
      unreachable("no ps shader");
   }

   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_PS,
                  .KernelStartPointer0 = ksp0,
   
                  .SingleProgramFlow = false,
                  .VectorMaskEnable = true,
                  .SamplerCount = 1,

                  .ScratchSpaceBasePointer = pipeline->scratch_start[VK_SHADER_STAGE_FRAGMENT],
                  .PerThreadScratchSpace = ffs(wm_prog_data->base.total_scratch / 2048),
                  
                  .MaximumNumberofThreadsPerPSD = 64 - 2,
                  .PositionXYOffsetSelect = wm_prog_data->uses_pos_offset ?
                     POSOFFSET_SAMPLE: POSOFFSET_NONE,
                  .PushConstantEnable = wm_prog_data->base.nr_params > 0,
                  ._8PixelDispatchEnable = pipeline->ps_simd8 != NO_KERNEL,
                  ._16PixelDispatchEnable = pipeline->ps_simd16 != NO_KERNEL,
                  ._32PixelDispatchEnable = false,

                  .DispatchGRFStartRegisterForConstantSetupData0 = grf_start0,
                  .DispatchGRFStartRegisterForConstantSetupData1 = 0,
                  .DispatchGRFStartRegisterForConstantSetupData2 = grf_start2,

                  .KernelStartPointer1 = 0,
                  .KernelStartPointer2 = ksp2);

   bool per_sample_ps = false;
   anv_batch_emit(&pipeline->batch, GEN8_3DSTATE_PS_EXTRA,
                  .PixelShaderValid = true,
                  .PixelShaderKillsPixel = wm_prog_data->uses_kill,
                  .PixelShaderComputedDepthMode = wm_prog_data->computed_depth_mode,
                  .AttributeEnable = wm_prog_data->num_varying_inputs > 0,
                  .oMaskPresenttoRenderTarget = wm_prog_data->uses_omask,
                  .PixelShaderIsPerSample = per_sample_ps);

   *pPipeline = (VkPipeline) pipeline;

   return VK_SUCCESS;
}

VkResult anv_CreateGraphicsPipelineDerivative(
    VkDevice                                    device,
    const VkGraphicsPipelineCreateInfo*         pCreateInfo,
    VkPipeline                                  basePipeline,
    VkPipeline*                                 pPipeline)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_CreateComputePipeline(
    VkDevice                                    _device,
    const VkComputePipelineCreateInfo*          pCreateInfo,
    VkPipeline*                                 pPipeline)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_pipeline *pipeline;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);

   pipeline = anv_device_alloc(device, sizeof(*pipeline), 8,
                               VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pipeline == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->base.destructor = anv_pipeline_destroy;
   pipeline->device = device;
   pipeline->layout = (struct anv_pipeline_layout *) pCreateInfo->layout;

   result = anv_reloc_list_init(&pipeline->batch.relocs, device);
   if (result != VK_SUCCESS) {
      anv_device_free(device, pipeline);
      return result;
   }
   pipeline->batch.next = pipeline->batch.start = pipeline->batch_data;
   pipeline->batch.end = pipeline->batch.start + sizeof(pipeline->batch_data);

   anv_state_stream_init(&pipeline->program_stream,
                         &device->instruction_block_pool);

   memset(pipeline->shaders, 0, sizeof(pipeline->shaders));

   pipeline->shaders[VK_SHADER_STAGE_COMPUTE] =
      (struct anv_shader *) pCreateInfo->cs.shader;

   pipeline->use_repclear = false;

   anv_compiler_run(device->compiler, pipeline);

   const struct brw_cs_prog_data *cs_prog_data = &pipeline->cs_prog_data;

   anv_batch_emit(&pipeline->batch, GEN8_MEDIA_VFE_STATE,
                  .ScratchSpaceBasePointer = pipeline->scratch_start[VK_SHADER_STAGE_FRAGMENT],
                  .PerThreadScratchSpace = ffs(cs_prog_data->base.total_scratch / 2048),
                  .ScratchSpaceBasePointerHigh = 0,
                  .StackSize = 0,

                  .MaximumNumberofThreads = device->info.max_cs_threads - 1,
                  .NumberofURBEntries = 2,
                  .ResetGatewayTimer = true,
                  .BypassGatewayControl = true,
                  .URBEntryAllocationSize = 2,
                  .CURBEAllocationSize = 0);

   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;
   uint32_t group_size = prog_data->local_size[0] *
      prog_data->local_size[1] * prog_data->local_size[2];
   pipeline->cs_thread_width_max = DIV_ROUND_UP(group_size, prog_data->simd_size);
   uint32_t remainder = group_size & (prog_data->simd_size - 1);

   if (remainder > 0)
      pipeline->cs_right_mask = ~0u >> (32 - remainder);
   else
      pipeline->cs_right_mask = ~0u >> (32 - prog_data->simd_size);


   *pPipeline = (VkPipeline) pipeline;

   return VK_SUCCESS;
}

VkResult anv_StorePipeline(
    VkDevice                                    device,
    VkPipeline                                  pipeline,
    size_t*                                     pDataSize,
    void*                                       pData)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_LoadPipeline(
    VkDevice                                    device,
    size_t                                      dataSize,
    const void*                                 pData,
    VkPipeline*                                 pPipeline)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_LoadPipelineDerivative(
    VkDevice                                    device,
    size_t                                      dataSize,
    const void*                                 pData,
    VkPipeline                                  basePipeline,
    VkPipeline*                                 pPipeline)
{
   stub_return(VK_UNSUPPORTED);
}

// Pipeline layout functions

VkResult anv_CreatePipelineLayout(
    VkDevice                                    _device,
    const VkPipelineLayoutCreateInfo*           pCreateInfo,
    VkPipelineLayout*                           pPipelineLayout)
{
   struct anv_device *device = (struct anv_device *) _device;
   struct anv_pipeline_layout *layout;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);

   layout = anv_device_alloc(device, sizeof(*layout), 8,
                             VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (layout == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   layout->num_sets = pCreateInfo->descriptorSetCount;

   uint32_t surface_start[VK_SHADER_STAGE_NUM] = { 0, };
   uint32_t sampler_start[VK_SHADER_STAGE_NUM] = { 0, };

   for (uint32_t s = 0; s < VK_SHADER_STAGE_NUM; s++) {
      layout->stage[s].surface_count = 0;
      layout->stage[s].sampler_count = 0;
   }

   for (uint32_t i = 0; i < pCreateInfo->descriptorSetCount; i++) {
      struct anv_descriptor_set_layout *set_layout =
         (struct anv_descriptor_set_layout *) pCreateInfo->pSetLayouts[i];

      layout->set[i].layout = set_layout;
      for (uint32_t s = 0; s < VK_SHADER_STAGE_NUM; s++) {
         layout->set[i].surface_start[s] = surface_start[s];
         surface_start[s] += set_layout->stage[s].surface_count;
         layout->set[i].sampler_start[s] = sampler_start[s];
         sampler_start[s] += set_layout->stage[s].sampler_count;

         layout->stage[s].surface_count += set_layout->stage[s].surface_count;
         layout->stage[s].sampler_count += set_layout->stage[s].sampler_count;
      }
   }

   *pPipelineLayout = (VkPipelineLayout) layout;

   return VK_SUCCESS;
}
