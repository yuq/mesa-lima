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

static void
gen7_emit_vertex_input(struct anv_pipeline *pipeline,
                       const VkPipelineVertexInputStateCreateInfo *info)
{
   const bool sgvs = pipeline->vs_prog_data.uses_vertexid ||
      pipeline->vs_prog_data.uses_instanceid;
   const uint32_t element_count = info->attributeCount + (sgvs ? 1 : 0);
   const uint32_t num_dwords = 1 + element_count * 2;
   uint32_t *p;

   if (info->attributeCount > 0) {
      p = anv_batch_emitn(&pipeline->batch, num_dwords,
                          GEN7_3DSTATE_VERTEX_ELEMENTS);
   }

   for (uint32_t i = 0; i < info->attributeCount; i++) {
      const VkVertexInputAttributeDescription *desc =
         &info->pVertexAttributeDescriptions[i];
      const struct anv_format *format = anv_format_for_vk_format(desc->format);

      struct GEN7_VERTEX_ELEMENT_STATE element = {
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
      GEN7_VERTEX_ELEMENT_STATE_pack(NULL, &p[1 + i * 2], &element);
   }

   if (sgvs) {
      struct GEN7_VERTEX_ELEMENT_STATE element = {
         .Valid = true,
         /* FIXME: Do we need to provide the base vertex as component 0 here
          * to support the correct base vertex ID? */
         .Component0Control = VFCOMP_STORE_0,
         .Component1Control = VFCOMP_STORE_0,
         .Component2Control = VFCOMP_STORE_VID,
         .Component3Control = VFCOMP_STORE_IID
      };
      GEN7_VERTEX_ELEMENT_STATE_pack(NULL, &p[1 + info->attributeCount * 2], &element);
   }
}

static const uint32_t vk_to_gen_cullmode[] = {
   [VK_CULL_MODE_NONE]                          = CULLMODE_NONE,
   [VK_CULL_MODE_FRONT]                         = CULLMODE_FRONT,
   [VK_CULL_MODE_BACK]                          = CULLMODE_BACK,
   [VK_CULL_MODE_FRONT_AND_BACK]                = CULLMODE_BOTH
};

static const uint32_t vk_to_gen_fillmode[] = {
   [VK_FILL_MODE_POINTS]                        = RASTER_POINT,
   [VK_FILL_MODE_WIREFRAME]                     = RASTER_WIREFRAME,
   [VK_FILL_MODE_SOLID]                         = RASTER_SOLID
};

static const uint32_t vk_to_gen_front_face[] = {
   [VK_FRONT_FACE_CCW]                          = CounterClockwise,
   [VK_FRONT_FACE_CW]                           = Clockwise
};

static void
gen7_emit_rs_state(struct anv_pipeline *pipeline,
                   const VkPipelineRasterStateCreateInfo *info,
                   const struct anv_graphics_pipeline_create_info *extra)
{
   struct GEN7_3DSTATE_SF sf = {
      GEN7_3DSTATE_SF_header,

      /* FIXME: Get this from pass info */
      .DepthBufferSurfaceFormat                 = D24_UNORM_X8_UINT,

      /* LegacyGlobalDepthBiasEnable */

      .StatisticsEnable                         = true,
      .FrontFaceFillMode                        = vk_to_gen_fillmode[info->fillMode],
      .BackFaceFillMode                         = vk_to_gen_fillmode[info->fillMode],
      .ViewTransformEnable                      = !(extra && extra->disable_viewport),
      .FrontWinding                             = vk_to_gen_front_face[info->frontFace],
      /* bool                                         AntiAliasingEnable; */

      .CullMode                                 = vk_to_gen_cullmode[info->cullMode],

      /* uint32_t                                     LineEndCapAntialiasingRegionWidth; */
      .ScissorRectangleEnable                   =  !(extra && extra->disable_scissor),

      /* uint32_t                                     MultisampleRasterizationMode; */
      /* bool                                         LastPixelEnable; */

      .TriangleStripListProvokingVertexSelect   = 0,
      .LineStripListProvokingVertexSelect       = 0,
      .TriangleFanProvokingVertexSelect         = 0,

      /* uint32_t                                     AALineDistanceMode; */
      /* uint32_t                                     VertexSubPixelPrecisionSelect; */
      .UsePointWidthState                       = !pipeline->writes_point_size,
      .PointWidth                               = 1.0,
   };

   GEN7_3DSTATE_SF_pack(NULL, &pipeline->gen7.sf, &sf);
}

static const uint32_t vk_to_gen_compare_op[] = {
   [VK_COMPARE_OP_NEVER]                        = PREFILTEROPNEVER,
   [VK_COMPARE_OP_LESS]                         = PREFILTEROPLESS,
   [VK_COMPARE_OP_EQUAL]                        = PREFILTEROPEQUAL,
   [VK_COMPARE_OP_LESS_EQUAL]                   = PREFILTEROPLEQUAL,
   [VK_COMPARE_OP_GREATER]                      = PREFILTEROPGREATER,
   [VK_COMPARE_OP_NOT_EQUAL]                    = PREFILTEROPNOTEQUAL,
   [VK_COMPARE_OP_GREATER_EQUAL]                = PREFILTEROPGEQUAL,
   [VK_COMPARE_OP_ALWAYS]                       = PREFILTEROPALWAYS,
};

static const uint32_t vk_to_gen_stencil_op[] = {
   [VK_STENCIL_OP_KEEP]                         = STENCILOP_KEEP,
   [VK_STENCIL_OP_ZERO]                         = STENCILOP_ZERO,
   [VK_STENCIL_OP_REPLACE]                      = STENCILOP_REPLACE,
   [VK_STENCIL_OP_INC_CLAMP]                    = STENCILOP_INCRSAT,
   [VK_STENCIL_OP_DEC_CLAMP]                    = STENCILOP_DECRSAT,
   [VK_STENCIL_OP_INVERT]                       = STENCILOP_INVERT,
   [VK_STENCIL_OP_INC_WRAP]                     = STENCILOP_INCR,
   [VK_STENCIL_OP_DEC_WRAP]                     = STENCILOP_DECR,
};

static const uint32_t vk_to_gen_blend_op[] = {
   [VK_BLEND_OP_ADD]                            = BLENDFUNCTION_ADD,
   [VK_BLEND_OP_SUBTRACT]                       = BLENDFUNCTION_SUBTRACT,
   [VK_BLEND_OP_REVERSE_SUBTRACT]               = BLENDFUNCTION_REVERSE_SUBTRACT,
   [VK_BLEND_OP_MIN]                            = BLENDFUNCTION_MIN,
   [VK_BLEND_OP_MAX]                            = BLENDFUNCTION_MAX,
};

static const uint32_t vk_to_gen_logic_op[] = {
   [VK_LOGIC_OP_COPY]                           = LOGICOP_COPY,
   [VK_LOGIC_OP_CLEAR]                          = LOGICOP_CLEAR,
   [VK_LOGIC_OP_AND]                            = LOGICOP_AND,
   [VK_LOGIC_OP_AND_REVERSE]                    = LOGICOP_AND_REVERSE,
   [VK_LOGIC_OP_AND_INVERTED]                   = LOGICOP_AND_INVERTED,
   [VK_LOGIC_OP_NOOP]                           = LOGICOP_NOOP,
   [VK_LOGIC_OP_XOR]                            = LOGICOP_XOR,
   [VK_LOGIC_OP_OR]                             = LOGICOP_OR,
   [VK_LOGIC_OP_NOR]                            = LOGICOP_NOR,
   [VK_LOGIC_OP_EQUIV]                          = LOGICOP_EQUIV,
   [VK_LOGIC_OP_INVERT]                         = LOGICOP_INVERT,
   [VK_LOGIC_OP_OR_REVERSE]                     = LOGICOP_OR_REVERSE,
   [VK_LOGIC_OP_COPY_INVERTED]                  = LOGICOP_COPY_INVERTED,
   [VK_LOGIC_OP_OR_INVERTED]                    = LOGICOP_OR_INVERTED,
   [VK_LOGIC_OP_NAND]                           = LOGICOP_NAND,
   [VK_LOGIC_OP_SET]                            = LOGICOP_SET,
};

static const uint32_t vk_to_gen_blend[] = {
   [VK_BLEND_ZERO]                              = BLENDFACTOR_ZERO,
   [VK_BLEND_ONE]                               = BLENDFACTOR_ONE,
   [VK_BLEND_SRC_COLOR]                         = BLENDFACTOR_SRC_COLOR,
   [VK_BLEND_ONE_MINUS_SRC_COLOR]               = BLENDFACTOR_INV_SRC_COLOR,
   [VK_BLEND_DEST_COLOR]                        = BLENDFACTOR_DST_COLOR,
   [VK_BLEND_ONE_MINUS_DEST_COLOR]              = BLENDFACTOR_INV_DST_COLOR,
   [VK_BLEND_SRC_ALPHA]                         = BLENDFACTOR_SRC_ALPHA,
   [VK_BLEND_ONE_MINUS_SRC_ALPHA]               = BLENDFACTOR_INV_SRC_ALPHA,
   [VK_BLEND_DEST_ALPHA]                        = BLENDFACTOR_DST_ALPHA,
   [VK_BLEND_ONE_MINUS_DEST_ALPHA]              = BLENDFACTOR_INV_DST_ALPHA,
   [VK_BLEND_CONSTANT_COLOR]                    = BLENDFACTOR_CONST_COLOR,
   [VK_BLEND_ONE_MINUS_CONSTANT_COLOR]          = BLENDFACTOR_INV_CONST_COLOR,
   [VK_BLEND_CONSTANT_ALPHA]                    = BLENDFACTOR_CONST_ALPHA,
   [VK_BLEND_ONE_MINUS_CONSTANT_ALPHA]          = BLENDFACTOR_INV_CONST_ALPHA,
   [VK_BLEND_SRC_ALPHA_SATURATE]                = BLENDFACTOR_SRC_ALPHA_SATURATE,
   [VK_BLEND_SRC1_COLOR]                        = BLENDFACTOR_SRC1_COLOR,
   [VK_BLEND_ONE_MINUS_SRC1_COLOR]              = BLENDFACTOR_INV_SRC1_COLOR,
   [VK_BLEND_SRC1_ALPHA]                        = BLENDFACTOR_SRC1_ALPHA,
   [VK_BLEND_ONE_MINUS_SRC1_ALPHA]              = BLENDFACTOR_INV_SRC1_ALPHA,
};

static void
gen7_emit_ds_state(struct anv_pipeline *pipeline,
                   const VkPipelineDepthStencilStateCreateInfo *info)
{
   if (info == NULL) {
      /* We're going to OR this together with the dynamic state.  We need
       * to make sure it's initialized to something useful.
       */
      memset(pipeline->gen7.depth_stencil_state, 0,
             sizeof(pipeline->gen7.depth_stencil_state));
      return;
   }

   bool has_stencil = false;  /* enable if subpass has stencil? */

   struct GEN7_DEPTH_STENCIL_STATE state = {
      /* Is this what we need to do? */
      .StencilBufferWriteEnable = has_stencil,

      .StencilTestEnable = info->stencilTestEnable,
      .StencilTestFunction = vk_to_gen_compare_op[info->front.stencilCompareOp],
      .StencilFailOp = vk_to_gen_stencil_op[info->front.stencilFailOp],
      .StencilPassDepthFailOp = vk_to_gen_stencil_op[info->front.stencilDepthFailOp],
      .StencilPassDepthPassOp = vk_to_gen_stencil_op[info->front.stencilPassOp],

      .DoubleSidedStencilEnable = true,

      .BackFaceStencilTestFunction = vk_to_gen_compare_op[info->back.stencilCompareOp],
      .BackfaceStencilFailOp = vk_to_gen_stencil_op[info->back.stencilFailOp],
      .BackfaceStencilPassDepthFailOp = vk_to_gen_stencil_op[info->back.stencilDepthFailOp],
      .BackfaceStencilPassDepthPassOp = vk_to_gen_stencil_op[info->back.stencilPassOp],

      .DepthTestEnable = info->depthTestEnable,
      .DepthTestFunction = vk_to_gen_compare_op[info->depthCompareOp],
      .DepthBufferWriteEnable = info->depthWriteEnable,
   };
   
   GEN7_DEPTH_STENCIL_STATE_pack(NULL, &pipeline->gen7.depth_stencil_state, &state);
}

static void
gen7_emit_cb_state(struct anv_pipeline *pipeline,
                   const VkPipelineColorBlendStateCreateInfo *info)
{
   struct anv_device *device = pipeline->device;

   /* FIXME-GEN7: All render targets share blend state settings on gen7, we
    * can't implement this.
    */
   const VkPipelineColorBlendAttachmentState *a = &info->pAttachments[0];

   uint32_t num_dwords = GEN7_BLEND_STATE_length;
   pipeline->blend_state =
      anv_state_pool_alloc(&device->dynamic_state_pool, num_dwords * 4, 64);

   struct GEN7_BLEND_STATE blend_state = {
      .ColorBufferBlendEnable = a->blendEnable,
      .IndependentAlphaBlendEnable = true, /* FIXME: yes? */
      .AlphaBlendFunction = vk_to_gen_blend_op[a->blendOpAlpha],

      .SourceAlphaBlendFactor = vk_to_gen_blend[a->srcBlendAlpha],
      .DestinationAlphaBlendFactor = vk_to_gen_blend[a->destBlendAlpha],

      .ColorBlendFunction = vk_to_gen_blend_op[a->blendOpColor],
      .SourceBlendFactor = vk_to_gen_blend[a->srcBlendColor],
      .DestinationBlendFactor = vk_to_gen_blend[a->destBlendColor],
      .AlphaToCoverageEnable = info->alphaToCoverageEnable,

#if 0
   bool                                         AlphaToOneEnable;
   bool                                         AlphaToCoverageDitherEnable;
#endif

      .WriteDisableAlpha = !(a->channelWriteMask & VK_CHANNEL_A_BIT),
      .WriteDisableRed = !(a->channelWriteMask & VK_CHANNEL_R_BIT),
      .WriteDisableGreen = !(a->channelWriteMask & VK_CHANNEL_G_BIT),
      .WriteDisableBlue = !(a->channelWriteMask & VK_CHANNEL_B_BIT),

      .LogicOpEnable = info->logicOpEnable,
      .LogicOpFunction = vk_to_gen_logic_op[info->logicOp],

#if 0
   bool                                         AlphaTestEnable;
   uint32_t                                     AlphaTestFunction;
   bool                                         ColorDitherEnable;
   uint32_t                                     XDitherOffset;
   uint32_t                                     YDitherOffset;
   uint32_t                                     ColorClampRange;
   bool                                         PreBlendColorClampEnable;
   bool                                         PostBlendColorClampEnable;
#endif
   };

   GEN7_BLEND_STATE_pack(NULL, pipeline->blend_state.map, &blend_state);

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_BLEND_STATE_POINTERS,
                  .BlendStatePointer = pipeline->blend_state.offset);
}

static const uint32_t vk_to_gen_primitive_type[] = {
   [VK_PRIMITIVE_TOPOLOGY_POINT_LIST]           = _3DPRIM_POINTLIST,
   [VK_PRIMITIVE_TOPOLOGY_LINE_LIST]            = _3DPRIM_LINELIST,
   [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP]           = _3DPRIM_LINESTRIP,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST]        = _3DPRIM_TRILIST,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP]       = _3DPRIM_TRISTRIP,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN]         = _3DPRIM_TRIFAN,
   [VK_PRIMITIVE_TOPOLOGY_LINE_LIST_ADJ]        = _3DPRIM_LINELIST_ADJ,
   [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_ADJ]       = _3DPRIM_LINESTRIP_ADJ,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_ADJ]    = _3DPRIM_TRILIST_ADJ,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_ADJ]   = _3DPRIM_TRISTRIP_ADJ,
   [VK_PRIMITIVE_TOPOLOGY_PATCH]                = _3DPRIM_PATCHLIST_1
};

static inline uint32_t
scratch_space(const struct brw_stage_prog_data *prog_data)
{
   return ffs(prog_data->total_scratch / 1024);
}

VkResult
gen7_graphics_pipeline_create(
    VkDevice                                    _device,
    const VkGraphicsPipelineCreateInfo*         pCreateInfo,
    const struct anv_graphics_pipeline_create_info *extra,
    VkPipeline*                                 pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_pipeline *pipeline;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
   
   pipeline = anv_device_alloc(device, sizeof(*pipeline), 8,
                               VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (pipeline == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   result = anv_pipeline_init(pipeline, device, pCreateInfo, extra);
   if (result != VK_SUCCESS) {
      anv_device_free(device, pipeline);
      return result;
   }

   assert(pCreateInfo->pVertexInputState);
   gen7_emit_vertex_input(pipeline, pCreateInfo->pVertexInputState);

   assert(pCreateInfo->pRasterState);
   gen7_emit_rs_state(pipeline, pCreateInfo->pRasterState, extra);

   gen7_emit_ds_state(pipeline, pCreateInfo->pDepthStencilState);

   gen7_emit_cb_state(pipeline, pCreateInfo->pColorBlendState);

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_VF_STATISTICS,
                   .StatisticsEnable = true);
   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_HS, .Enable = false);
   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_TE, .TEEnable = false);
   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_DS, .DSFunctionEnable = false);
   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_STREAMOUT, .SOFunctionEnable = false);

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_VS,
                  .ConstantBufferOffset = 0,
                  .ConstantBufferSize = 4);
   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_GS,
                  .ConstantBufferOffset = 4,
                  .ConstantBufferSize = 4);
   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_PS,
                  .ConstantBufferOffset = 8,
                  .ConstantBufferSize = 4);

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_AA_LINE_PARAMETERS);

   const VkPipelineRasterStateCreateInfo *rs_info = pCreateInfo->pRasterState;

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_CLIP,
      .FrontWinding                             = vk_to_gen_front_face[rs_info->frontFace],
      .CullMode                                 = vk_to_gen_cullmode[rs_info->cullMode],
      .ClipEnable                               = true,
      .APIMode                                  = APIMODE_OGL,
      .ViewportXYClipTestEnable                 = !(extra && extra->disable_viewport),
      .ClipMode                                 = CLIPMODE_NORMAL,
      .TriangleStripListProvokingVertexSelect   = 0,
      .LineStripListProvokingVertexSelect       = 0,
      .TriangleFanProvokingVertexSelect         = 0,
      .MinimumPointWidth                        = 0.125,
      .MaximumPointWidth                        = 255.875);

   uint32_t samples = 1;
   uint32_t log2_samples = __builtin_ffs(samples) - 1;

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_MULTISAMPLE,
      .PixelLocation                            = PIXLOC_CENTER,
      .NumberofMultisamples                     = log2_samples);

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_SAMPLE_MASK,
      .SampleMask                               = 0xff);

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_URB_VS,
      .VSURBStartingAddress                     = pipeline->urb.vs_start,
      .VSURBEntryAllocationSize                 = pipeline->urb.vs_size - 1,
      .VSNumberofURBEntries                     = pipeline->urb.nr_vs_entries);

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_URB_GS,
      .GSURBStartingAddress                     = pipeline->urb.gs_start,
      .GSURBEntryAllocationSize                 = pipeline->urb.gs_size - 1,
      .GSNumberofURBEntries                     = pipeline->urb.nr_gs_entries);

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_URB_HS,
      .HSURBStartingAddress                     = pipeline->urb.vs_start,
      .HSURBEntryAllocationSize                 = 0,
      .HSNumberofURBEntries                     = 0);

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_URB_DS,
      .DSURBStartingAddress                     = pipeline->urb.vs_start,
      .DSURBEntryAllocationSize                 = 0,
      .DSNumberofURBEntries                     = 0);

   const struct brw_vue_prog_data *vue_prog_data = &pipeline->vs_prog_data.base;
   /* The last geometry producing stage will set urb_offset and urb_length,
    * which we use in 3DSTATE_SBE. Skip the VUE header and position slots. */
   uint32_t urb_offset = 1;
   uint32_t urb_length = (vue_prog_data->vue_map.num_slots + 1) / 2 - urb_offset;

#if 0 
   /* From gen7_vs_state.c */

   /**
    * From Graphics BSpec: 3D-Media-GPGPU Engine > 3D Pipeline Stages >
    * Geometry > Geometry Shader > State:
    *
    *     "Note: Because of corruption in IVB:GT2, software needs to flush the
    *     whole fixed function pipeline when the GS enable changes value in
    *     the 3DSTATE_GS."
    *
    * The hardware architects have clarified that in this context "flush the
    * whole fixed function pipeline" means to emit a PIPE_CONTROL with the "CS
    * Stall" bit set.
    */
   if (!brw->is_haswell && !brw->is_baytrail)
      gen7_emit_vs_workaround_flush(brw);
#endif

   if (pipeline->vs_vec4 == NO_KERNEL || (extra && extra->disable_vs))
      anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_VS, .VSFunctionEnable = false);
   else
      anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_VS,
         .KernelStartPointer                    = pipeline->vs_vec4,
         .ScratchSpaceBaseOffset                = pipeline->scratch_start[VK_SHADER_STAGE_VERTEX],
         .PerThreadScratchSpace                 = scratch_space(&vue_prog_data->base),

         .DispatchGRFStartRegisterforURBData    =
            vue_prog_data->base.dispatch_grf_start_reg,
         .VertexURBEntryReadLength              = vue_prog_data->urb_read_length,
         .VertexURBEntryReadOffset              = 0,

         .MaximumNumberofThreads                = device->info.max_vs_threads - 1,
         .StatisticsEnable                      = true,
         .VSFunctionEnable                      = true);

   const struct brw_gs_prog_data *gs_prog_data = &pipeline->gs_prog_data;

   if (pipeline->gs_vec4 == NO_KERNEL || (extra && extra->disable_vs)) {
      anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_GS, .GSEnable = false);
   } else {
      urb_offset = 1;
      urb_length = (gs_prog_data->base.vue_map.num_slots + 1) / 2 - urb_offset;

      anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_GS,
         .KernelStartPointer                    = pipeline->gs_vec4,
         .ScratchSpaceBasePointer               = pipeline->scratch_start[VK_SHADER_STAGE_GEOMETRY],
         .PerThreadScratchSpace                 = scratch_space(&gs_prog_data->base.base),

         .OutputVertexSize                      = gs_prog_data->output_vertex_size_hwords * 2 - 1,
         .OutputTopology                        = gs_prog_data->output_topology,
         .VertexURBEntryReadLength              = gs_prog_data->base.urb_read_length,
         .DispatchGRFStartRegisterforURBData    =
            gs_prog_data->base.base.dispatch_grf_start_reg,

         .MaximumNumberofThreads                = device->info.max_gs_threads - 1,
         /* This in the next dword on HSW. */
         .ControlDataFormat                     = gs_prog_data->control_data_format,
         .ControlDataHeaderSize                 = gs_prog_data->control_data_header_size_hwords,
         .InstanceControl                       = gs_prog_data->invocations - 1,
         .DispatchMode                          = gs_prog_data->base.dispatch_mode,
         .GSStatisticsEnable                    = true,
         .IncludePrimitiveID                    = gs_prog_data->include_primitive_id,
         .ReorderEnable                         = true,
         .GSEnable                              = true);
   }

   const struct brw_wm_prog_data *wm_prog_data = &pipeline->wm_prog_data;
   if (wm_prog_data->urb_setup[VARYING_SLOT_BFC0] != -1 ||
       wm_prog_data->urb_setup[VARYING_SLOT_BFC1] != -1)
      anv_finishme("two-sided color needs sbe swizzling setup");
   if (wm_prog_data->urb_setup[VARYING_SLOT_PRIMITIVE_ID] != -1)
      anv_finishme("primitive_id needs sbe swizzling setup");

   /* FIXME: generated header doesn't emit attr swizzle fields */
   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_SBE,
      .NumberofSFOutputAttributes               = pipeline->wm_prog_data.num_varying_inputs,
      .VertexURBEntryReadLength                 = urb_length,
      .VertexURBEntryReadOffset                 = urb_offset,
      .PointSpriteTextureCoordinateOrigin       = UPPERLEFT);

   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_PS,
      .KernelStartPointer0                      = pipeline->ps_ksp0,
      .ScratchSpaceBasePointer                  = pipeline->scratch_start[VK_SHADER_STAGE_FRAGMENT],
      .PerThreadScratchSpace                    = scratch_space(&wm_prog_data->base),
                  
      .MaximumNumberofThreads                   = device->info.max_wm_threads - 1,
      .PushConstantEnable                       = wm_prog_data->base.nr_params > 0,
      .AttributeEnable                          = wm_prog_data->num_varying_inputs > 0,
      .oMaskPresenttoRenderTarget               = wm_prog_data->uses_omask,

      .RenderTargetFastClearEnable              = false,
      .DualSourceBlendEnable                    = false,
      .RenderTargetResolveEnable                = false,

      .PositionXYOffsetSelect                   = wm_prog_data->uses_pos_offset ?
         POSOFFSET_SAMPLE : POSOFFSET_NONE,

      ._32PixelDispatchEnable                   = false,
      ._16PixelDispatchEnable                   = pipeline->ps_simd16 != NO_KERNEL,
      ._8PixelDispatchEnable                    = pipeline->ps_simd8 != NO_KERNEL,

      .DispatchGRFStartRegisterforConstantSetupData0 = pipeline->ps_grf_start0,
      .DispatchGRFStartRegisterforConstantSetupData1 = 0,
      .DispatchGRFStartRegisterforConstantSetupData2 = pipeline->ps_grf_start2,

#if 0
   /* Haswell requires the sample mask to be set in this packet as well as
    * in 3DSTATE_SAMPLE_MASK; the values should match. */
   /* _NEW_BUFFERS, _NEW_MULTISAMPLE */
#endif

      .KernelStartPointer1                      = 0,
      .KernelStartPointer2                      = pipeline->ps_ksp2);

   /* FIXME-GEN7: This needs a lot more work, cf gen7 upload_wm_state(). */
   anv_batch_emit(&pipeline->batch, GEN7_3DSTATE_WM,
      .StatisticsEnable                         = true,
      .ThreadDispatchEnable                     = true,
      .LineEndCapAntialiasingRegionWidth        = _05pixels,
      .LineAntialiasingRegionWidth              = _10pixels,
      .EarlyDepthStencilControl                 = NORMAL,
      .PointRasterizationRule                   = RASTRULE_UPPER_RIGHT,
      .PixelShaderComputedDepthMode             = wm_prog_data->computed_depth_mode,
      .BarycentricInterpolationMode             = wm_prog_data->barycentric_interp_modes);

   *pPipeline = anv_pipeline_to_handle(pipeline);

   return VK_SUCCESS;
}

VkResult gen7_compute_pipeline_create(
    VkDevice                                    _device,
    const VkComputePipelineCreateInfo*          pCreateInfo,
    VkPipeline*                                 pPipeline)
{
   anv_finishme("primitive_id needs sbe swizzling setup");

   return vk_error(VK_ERROR_UNAVAILABLE);
}
