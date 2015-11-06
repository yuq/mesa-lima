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
#include "brw_nir.h"
#include "anv_nir.h"
#include "glsl/nir/nir_spirv.h"

/* Needed for SWIZZLE macros */
#include "program/prog_instruction.h"

// Shader functions

VkResult anv_CreateShaderModule(
    VkDevice                                    _device,
    const VkShaderModuleCreateInfo*             pCreateInfo,
    VkShaderModule*                             pShaderModule)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_shader_module *module;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
   assert(pCreateInfo->flags == 0);

   module = anv_device_alloc(device, sizeof(*module) + pCreateInfo->codeSize, 8,
                             VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (module == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   module->nir = NULL;
   module->size = pCreateInfo->codeSize;
   memcpy(module->data, pCreateInfo->pCode, module->size);

   *pShaderModule = anv_shader_module_to_handle(module);

   return VK_SUCCESS;
}

void anv_DestroyShaderModule(
    VkDevice                                    _device,
    VkShaderModule                              _module)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_shader_module, module, _module);

   anv_device_free(device, module);
}

VkResult anv_CreateShader(
    VkDevice                                    _device,
    const VkShaderCreateInfo*                   pCreateInfo,
    VkShader*                                   pShader)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_shader_module, module, pCreateInfo->module);
   struct anv_shader *shader;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_CREATE_INFO);
   assert(pCreateInfo->flags == 0);

   const char *name = pCreateInfo->pName ? pCreateInfo->pName : "main";
   size_t name_len = strlen(name);

   shader = anv_device_alloc(device, sizeof(*shader) + name_len + 1, 8,
                             VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (shader == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   shader->module = module,
   memcpy(shader->entrypoint, name, name_len + 1);

   *pShader = anv_shader_to_handle(shader);

   return VK_SUCCESS;
}

void anv_DestroyShader(
    VkDevice                                    _device,
    VkShader                                    _shader)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_shader, shader, _shader);

   anv_device_free(device, shader);
}

#define SPIR_V_MAGIC_NUMBER 0x07230203

static const gl_shader_stage vk_shader_stage_to_mesa_stage[] = {
   [VK_SHADER_STAGE_VERTEX] = MESA_SHADER_VERTEX,
   [VK_SHADER_STAGE_TESS_CONTROL] = -1,
   [VK_SHADER_STAGE_TESS_EVALUATION] = -1,
   [VK_SHADER_STAGE_GEOMETRY] = MESA_SHADER_GEOMETRY,
   [VK_SHADER_STAGE_FRAGMENT] = MESA_SHADER_FRAGMENT,
   [VK_SHADER_STAGE_COMPUTE] = MESA_SHADER_COMPUTE,
};

static bool
is_scalar_shader_stage(const struct brw_compiler *compiler, VkShaderStage stage)
{
   switch (stage) {
   case VK_SHADER_STAGE_VERTEX:
      return compiler->scalar_vs;
   case VK_SHADER_STAGE_GEOMETRY:
      return false;
   case VK_SHADER_STAGE_FRAGMENT:
   case VK_SHADER_STAGE_COMPUTE:
      return true;
   default:
      unreachable("Unsupported shader stage");
   }
}

/* Eventually, this will become part of anv_CreateShader.  Unfortunately,
 * we can't do that yet because we don't have the ability to copy nir.
 */
static nir_shader *
anv_shader_compile_to_nir(struct anv_device *device,
                          struct anv_shader *shader, VkShaderStage vk_stage)
{
   if (strcmp(shader->entrypoint, "main") != 0) {
      anv_finishme("Multiple shaders per module not really supported");
   }

   gl_shader_stage stage = vk_shader_stage_to_mesa_stage[vk_stage];
   const struct brw_compiler *compiler =
      device->instance->physicalDevice.compiler;
   const nir_shader_compiler_options *nir_options =
      compiler->glsl_compiler_options[stage].NirOptions;

   nir_shader *nir;
   if (shader->module->nir) {
      /* Some things such as our meta clear/blit code will give us a NIR
       * shader directly.  In that case, we just ignore the SPIR-V entirely
       * and just use the NIR shader */
      nir = shader->module->nir;
      nir->options = nir_options;
   } else {
      uint32_t *spirv = (uint32_t *) shader->module->data;
      assert(spirv[0] == SPIR_V_MAGIC_NUMBER);
      assert(shader->module->size % 4 == 0);

      nir = spirv_to_nir(spirv, shader->module->size / 4, stage, nir_options);
   }
   nir_validate_shader(nir);

   /* Vulkan uses the separate-shader linking model */
   nir->info.separate_shader = true;

   /* Make sure the provided shader has exactly one entrypoint and that the
    * name matches the name that came in from the VkShader.
    */
   nir_function_impl *entrypoint = NULL;
   nir_foreach_overload(nir, overload) {
      if (strcmp(shader->entrypoint, overload->function->name) == 0 &&
          overload->impl) {
         assert(entrypoint == NULL);
         entrypoint = overload->impl;
      }
   }
   assert(entrypoint != NULL);

   brw_preprocess_nir(nir, &device->info,
                      is_scalar_shader_stage(compiler, vk_stage));

   nir_shader_gather_info(nir, entrypoint);

   return nir;
}

VkResult anv_CreatePipelineCache(
    VkDevice                                    device,
    const VkPipelineCacheCreateInfo*            pCreateInfo,
    VkPipelineCache*                            pPipelineCache)
{
   pPipelineCache->handle = 1;

   stub_return(VK_SUCCESS);
}

void anv_DestroyPipelineCache(
    VkDevice                                    _device,
    VkPipelineCache                             _cache)
{
}

size_t anv_GetPipelineCacheSize(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache)
{
   stub_return(0);
}

VkResult anv_GetPipelineCacheData(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache,
    void*                                       pData)
{
   stub_return(VK_UNSUPPORTED);
}

VkResult anv_MergePipelineCaches(
    VkDevice                                    device,
    VkPipelineCache                             destCache,
    uint32_t                                    srcCacheCount,
    const VkPipelineCache*                      pSrcCaches)
{
   stub_return(VK_UNSUPPORTED);
}

void anv_DestroyPipeline(
    VkDevice                                    _device,
    VkPipeline                                  _pipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_pipeline, pipeline, _pipeline);

   anv_reloc_list_finish(&pipeline->batch_relocs, pipeline->device);
   anv_state_stream_finish(&pipeline->program_stream);
   anv_state_pool_free(&device->dynamic_state_pool, pipeline->blend_state);
   anv_device_free(pipeline->device, pipeline);
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

static void
populate_sampler_prog_key(const struct brw_device_info *devinfo,
                          struct brw_sampler_prog_key_data *key)
{
   /* XXX: Handle texture swizzle on HSW- */
   for (int i = 0; i < MAX_SAMPLERS; i++) {
      /* Assume color sampler, no swizzling. (Works for BDW+) */
      key->swizzles[i] = SWIZZLE_XYZW;
   }
}

static void
populate_vs_prog_key(const struct brw_device_info *devinfo,
                     struct brw_vs_prog_key *key)
{
   memset(key, 0, sizeof(*key));

   populate_sampler_prog_key(devinfo, &key->tex);

   /* XXX: Handle vertex input work-arounds */

   /* XXX: Handle sampler_prog_key */
}

static void
populate_gs_prog_key(const struct brw_device_info *devinfo,
                     struct brw_gs_prog_key *key)
{
   memset(key, 0, sizeof(*key));

   populate_sampler_prog_key(devinfo, &key->tex);
}

static void
populate_wm_prog_key(const struct brw_device_info *devinfo,
                     const VkGraphicsPipelineCreateInfo *info,
                     struct brw_wm_prog_key *key)
{
   ANV_FROM_HANDLE(anv_render_pass, render_pass, info->renderPass);

   memset(key, 0, sizeof(*key));

   populate_sampler_prog_key(devinfo, &key->tex);

   /* TODO: Fill out key->input_slots_valid */

   /* Vulkan doesn't specify a default */
   key->high_quality_derivatives = false;

   /* XXX Vulkan doesn't appear to specify */
   key->clamp_fragment_color = false;

   /* Vulkan always specifies upper-left coordinates */
   key->drawable_height = 0;
   key->render_to_fbo = false;

   key->nr_color_regions = render_pass->subpasses[info->subpass].color_count;

   key->replicate_alpha = key->nr_color_regions > 1 &&
                          info->pColorBlendState->alphaToCoverageEnable;

   if (info->pMultisampleState && info->pMultisampleState->rasterSamples > 1) {
      /* We should probably pull this out of the shader, but it's fairly
       * harmless to compute it and then let dead-code take care of it.
       */
      key->persample_shading = info->pMultisampleState->sampleShadingEnable;
      if (key->persample_shading)
         key->persample_2x = info->pMultisampleState->rasterSamples == 2;

      key->compute_pos_offset = info->pMultisampleState->sampleShadingEnable;
      key->compute_sample_id = info->pMultisampleState->sampleShadingEnable;
   }
}

static void
populate_cs_prog_key(const struct brw_device_info *devinfo,
                     struct brw_cs_prog_key *key)
{
   memset(key, 0, sizeof(*key));

   populate_sampler_prog_key(devinfo, &key->tex);
}

static nir_shader *
anv_pipeline_compile(struct anv_pipeline *pipeline,
                     struct anv_shader *shader,
                     VkShaderStage stage,
                     struct brw_stage_prog_data *prog_data)
{
   const struct brw_compiler *compiler =
      pipeline->device->instance->physicalDevice.compiler;

   nir_shader *nir = anv_shader_compile_to_nir(pipeline->device, shader, stage);
   if (nir == NULL)
      return NULL;

   anv_nir_lower_push_constants(nir, is_scalar_shader_stage(compiler, stage));

   /* Figure out the number of parameters */
   prog_data->nr_params = 0;

   if (nir->num_uniforms > 0) {
      /* If the shader uses any push constants at all, we'll just give
       * them the maximum possible number
       */
      prog_data->nr_params += MAX_PUSH_CONSTANTS_SIZE / sizeof(float);
   }

   if (pipeline->layout && pipeline->layout->stage[stage].has_dynamic_offsets)
      prog_data->nr_params += MAX_DYNAMIC_BUFFERS * 2;

   if (prog_data->nr_params > 0) {
      prog_data->param = (const gl_constant_value **)
         anv_device_alloc(pipeline->device,
                          prog_data->nr_params * sizeof(gl_constant_value *),
                          8, VK_SYSTEM_ALLOC_TYPE_INTERNAL_SHADER);

      /* We now set the param values to be offsets into a
       * anv_push_constant_data structure.  Since the compiler doesn't
       * actually dereference any of the gl_constant_value pointers in the
       * params array, it doesn't really matter what we put here.
       */
      struct anv_push_constants *null_data = NULL;
      if (nir->num_uniforms > 0) {
         /* Fill out the push constants section of the param array */
         for (unsigned i = 0; i < MAX_PUSH_CONSTANTS_SIZE / sizeof(float); i++)
            prog_data->param[i] = (const gl_constant_value *)
               &null_data->client_data[i * sizeof(float)];
      }
   }

   /* Set up dynamic offsets */
   anv_nir_apply_dynamic_offsets(pipeline, nir, prog_data);

   /* Apply the actual pipeline layout to UBOs, SSBOs, and textures */
   anv_nir_apply_pipeline_layout(nir, pipeline->layout);

   /* All binding table offsets provided by apply_pipeline_layout() are
    * relative to the start of the bindint table (plus MAX_RTS for VS).
    */
   unsigned bias = stage == VK_SHADER_STAGE_FRAGMENT ? MAX_RTS : 0;
   prog_data->binding_table.size_bytes = 0;
   prog_data->binding_table.texture_start = bias;
   prog_data->binding_table.ubo_start = bias;
   prog_data->binding_table.image_start = bias;

   /* Finish the optimization and compilation process */
   brw_postprocess_nir(nir, &pipeline->device->info,
                       is_scalar_shader_stage(compiler, stage));

   /* nir_lower_io will only handle the push constants; we need to set this
    * to the full number of possible uniforms.
    */
   nir->num_uniforms = prog_data->nr_params;

   return nir;
}

static uint32_t
anv_pipeline_upload_kernel(struct anv_pipeline *pipeline,
                           const void *data, size_t size)
{
   struct anv_state state =
      anv_state_stream_alloc(&pipeline->program_stream, size, 64);

   assert(size < pipeline->program_stream.block_pool->block_size);

   memcpy(state.map, data, size);

   return state.offset;
}
static void
anv_pipeline_add_compiled_stage(struct anv_pipeline *pipeline,
                                VkShaderStage stage,
                                struct brw_stage_prog_data *prog_data)
{
   struct brw_device_info *devinfo = &pipeline->device->info;
   uint32_t max_threads[] = {
      [VK_SHADER_STAGE_VERTEX]                  = devinfo->max_vs_threads,
      [VK_SHADER_STAGE_TESS_CONTROL]            = 0,
      [VK_SHADER_STAGE_TESS_EVALUATION]         = 0,
      [VK_SHADER_STAGE_GEOMETRY]                = devinfo->max_gs_threads,
      [VK_SHADER_STAGE_FRAGMENT]                = devinfo->max_wm_threads,
      [VK_SHADER_STAGE_COMPUTE]                 = devinfo->max_cs_threads,
   };

   pipeline->prog_data[stage] = prog_data;
   pipeline->active_stages |= 1 << stage;
   pipeline->scratch_start[stage] = pipeline->total_scratch;
   pipeline->total_scratch =
      align_u32(pipeline->total_scratch, 1024) +
      prog_data->total_scratch * max_threads[stage];
}

static VkResult
anv_pipeline_compile_vs(struct anv_pipeline *pipeline,
                        const VkGraphicsPipelineCreateInfo *info,
                        struct anv_shader *shader)
{
   const struct brw_compiler *compiler =
      pipeline->device->instance->physicalDevice.compiler;
   struct brw_vs_prog_data *prog_data = &pipeline->vs_prog_data;
   struct brw_vs_prog_key key;

   populate_vs_prog_key(&pipeline->device->info, &key);

   /* TODO: Look up shader in cache */

   memset(prog_data, 0, sizeof(*prog_data));

   nir_shader *nir = anv_pipeline_compile(pipeline, shader,
                                          VK_SHADER_STAGE_VERTEX,
                                          &prog_data->base.base);
   if (nir == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   void *mem_ctx = ralloc_context(NULL);

   if (shader->module->nir == NULL)
      ralloc_steal(mem_ctx, nir);

   prog_data->inputs_read = nir->info.inputs_read;
   pipeline->writes_point_size = nir->info.outputs_written & VARYING_SLOT_PSIZ;

   brw_compute_vue_map(&pipeline->device->info,
                       &prog_data->base.vue_map,
                       nir->info.outputs_written,
                       nir->info.separate_shader);

   unsigned code_size;
   const unsigned *shader_code =
      brw_compile_vs(compiler, NULL, mem_ctx, &key, prog_data, nir,
                     NULL, false, -1, &code_size, NULL);
   if (shader_code == NULL) {
      ralloc_free(mem_ctx);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   const uint32_t offset =
      anv_pipeline_upload_kernel(pipeline, shader_code, code_size);
   if (prog_data->base.dispatch_mode == DISPATCH_MODE_SIMD8) {
      pipeline->vs_simd8 = offset;
      pipeline->vs_vec4 = NO_KERNEL;
   } else {
      pipeline->vs_simd8 = NO_KERNEL;
      pipeline->vs_vec4 = offset;
   }

   ralloc_free(mem_ctx);

   anv_pipeline_add_compiled_stage(pipeline, VK_SHADER_STAGE_VERTEX,
                                   &prog_data->base.base);

   return VK_SUCCESS;
}

static VkResult
anv_pipeline_compile_gs(struct anv_pipeline *pipeline,
                        const VkGraphicsPipelineCreateInfo *info,
                        struct anv_shader *shader)
{
   const struct brw_compiler *compiler =
      pipeline->device->instance->physicalDevice.compiler;
   struct brw_gs_prog_data *prog_data = &pipeline->gs_prog_data;
   struct brw_gs_prog_key key;

   populate_gs_prog_key(&pipeline->device->info, &key);

   /* TODO: Look up shader in cache */

   memset(prog_data, 0, sizeof(*prog_data));

   nir_shader *nir = anv_pipeline_compile(pipeline, shader,
                                          VK_SHADER_STAGE_GEOMETRY,
                                          &prog_data->base.base);
   if (nir == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   void *mem_ctx = ralloc_context(NULL);

   if (shader->module->nir == NULL)
      ralloc_steal(mem_ctx, nir);

   brw_compute_vue_map(&pipeline->device->info,
                       &prog_data->base.vue_map,
                       nir->info.outputs_written,
                       nir->info.separate_shader);

   unsigned code_size;
   const unsigned *shader_code =
      brw_compile_gs(compiler, NULL, mem_ctx, &key, prog_data, nir,
                     NULL, -1, &code_size, NULL);
   if (shader_code == NULL) {
      ralloc_free(mem_ctx);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* TODO: SIMD8 GS */
   pipeline->gs_vec4 =
      anv_pipeline_upload_kernel(pipeline, shader_code, code_size);
   pipeline->gs_vertex_count = nir->info.gs.vertices_in;

   ralloc_free(mem_ctx);

   anv_pipeline_add_compiled_stage(pipeline, VK_SHADER_STAGE_GEOMETRY,
                                   &prog_data->base.base);

   return VK_SUCCESS;
}

static VkResult
anv_pipeline_compile_fs(struct anv_pipeline *pipeline,
                        const VkGraphicsPipelineCreateInfo *info,
                        struct anv_shader *shader)
{
   const struct brw_compiler *compiler =
      pipeline->device->instance->physicalDevice.compiler;
   struct brw_wm_prog_data *prog_data = &pipeline->wm_prog_data;
   struct brw_wm_prog_key key;

   populate_wm_prog_key(&pipeline->device->info, info, &key);

   if (pipeline->use_repclear)
      key.nr_color_regions = 1;

   /* TODO: Look up shader in cache */

   memset(prog_data, 0, sizeof(*prog_data));

   prog_data->binding_table.render_target_start = 0;

   nir_shader *nir = anv_pipeline_compile(pipeline, shader,
                                          VK_SHADER_STAGE_FRAGMENT,
                                          &prog_data->base);
   if (nir == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   void *mem_ctx = ralloc_context(NULL);

   if (shader->module->nir == NULL)
      ralloc_steal(mem_ctx, nir);

   unsigned code_size;
   const unsigned *shader_code =
      brw_compile_fs(compiler, NULL, mem_ctx, &key, prog_data, nir,
                     NULL, -1, -1, pipeline->use_repclear, &code_size, NULL);
   if (shader_code == NULL) {
      ralloc_free(mem_ctx);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   uint32_t offset = anv_pipeline_upload_kernel(pipeline,
                                                shader_code, code_size);
   if (prog_data->no_8)
      pipeline->ps_simd8 = NO_KERNEL;
   else
      pipeline->ps_simd8 = offset;

   if (prog_data->no_8 || prog_data->prog_offset_16) {
      pipeline->ps_simd16 = offset + prog_data->prog_offset_16;
   } else {
      pipeline->ps_simd16 = NO_KERNEL;
   }

   pipeline->ps_ksp2 = 0;
   pipeline->ps_grf_start2 = 0;
   if (pipeline->ps_simd8 != NO_KERNEL) {
      pipeline->ps_ksp0 = pipeline->ps_simd8;
      pipeline->ps_grf_start0 = prog_data->base.dispatch_grf_start_reg;
      if (pipeline->ps_simd16 != NO_KERNEL) {
         pipeline->ps_ksp2 = pipeline->ps_simd16;
         pipeline->ps_grf_start2 = prog_data->dispatch_grf_start_reg_16;
      }
   } else if (pipeline->ps_simd16 != NO_KERNEL) {
      pipeline->ps_ksp0 = pipeline->ps_simd16;
      pipeline->ps_grf_start0 = prog_data->dispatch_grf_start_reg_16;
   }

   ralloc_free(mem_ctx);

   anv_pipeline_add_compiled_stage(pipeline, VK_SHADER_STAGE_FRAGMENT,
                                   &prog_data->base);

   return VK_SUCCESS;
}

VkResult
anv_pipeline_compile_cs(struct anv_pipeline *pipeline,
                        const VkComputePipelineCreateInfo *info,
                        struct anv_shader *shader)
{
   const struct brw_compiler *compiler =
      pipeline->device->instance->physicalDevice.compiler;
   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;
   struct brw_cs_prog_key key;

   populate_cs_prog_key(&pipeline->device->info, &key);

   /* TODO: Look up shader in cache */

   memset(prog_data, 0, sizeof(*prog_data));

   nir_shader *nir = anv_pipeline_compile(pipeline, shader,
                                          VK_SHADER_STAGE_COMPUTE,
                                          &prog_data->base);
   if (nir == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   void *mem_ctx = ralloc_context(NULL);

   if (shader->module->nir == NULL)
      ralloc_steal(mem_ctx, nir);

   unsigned code_size;
   const unsigned *shader_code =
      brw_compile_cs(compiler, NULL, mem_ctx, &key, prog_data, nir,
                     -1, &code_size, NULL);
   if (shader_code == NULL) {
      ralloc_free(mem_ctx);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   pipeline->cs_simd = anv_pipeline_upload_kernel(pipeline,
                                                  shader_code, code_size);
   ralloc_free(mem_ctx);

   anv_pipeline_add_compiled_stage(pipeline, VK_SHADER_STAGE_COMPUTE,
                                   &prog_data->base);

   return VK_SUCCESS;
}

static const int gen8_push_size = 32 * 1024;

static void
gen7_compute_urb_partition(struct anv_pipeline *pipeline)
{
   const struct brw_device_info *devinfo = &pipeline->device->info;
   bool vs_present = pipeline->active_stages & VK_SHADER_STAGE_VERTEX_BIT;
   unsigned vs_size = vs_present ? pipeline->vs_prog_data.base.urb_entry_size : 1;
   unsigned vs_entry_size_bytes = vs_size * 64;
   bool gs_present = pipeline->active_stages & VK_SHADER_STAGE_GEOMETRY_BIT;
   unsigned gs_size = gs_present ? pipeline->gs_prog_data.base.urb_entry_size : 1;
   unsigned gs_entry_size_bytes = gs_size * 64;

   /* From p35 of the Ivy Bridge PRM (section 1.7.1: 3DSTATE_URB_GS):
    *
    *     VS Number of URB Entries must be divisible by 8 if the VS URB Entry
    *     Allocation Size is less than 9 512-bit URB entries.
    *
    * Similar text exists for GS.
    */
   unsigned vs_granularity = (vs_size < 9) ? 8 : 1;
   unsigned gs_granularity = (gs_size < 9) ? 8 : 1;

   /* URB allocations must be done in 8k chunks. */
   unsigned chunk_size_bytes = 8192;

   /* Determine the size of the URB in chunks. */
   unsigned urb_chunks = devinfo->urb.size * 1024 / chunk_size_bytes;

   /* Reserve space for push constants */
   unsigned push_constant_bytes = gen8_push_size;
   unsigned push_constant_chunks =
      push_constant_bytes / chunk_size_bytes;

   /* Initially, assign each stage the minimum amount of URB space it needs,
    * and make a note of how much additional space it "wants" (the amount of
    * additional space it could actually make use of).
    */

   /* VS has a lower limit on the number of URB entries */
   unsigned vs_chunks =
      ALIGN(devinfo->urb.min_vs_entries * vs_entry_size_bytes,
            chunk_size_bytes) / chunk_size_bytes;
   unsigned vs_wants =
      ALIGN(devinfo->urb.max_vs_entries * vs_entry_size_bytes,
            chunk_size_bytes) / chunk_size_bytes - vs_chunks;

   unsigned gs_chunks = 0;
   unsigned gs_wants = 0;
   if (gs_present) {
      /* There are two constraints on the minimum amount of URB space we can
       * allocate:
       *
       * (1) We need room for at least 2 URB entries, since we always operate
       * the GS in DUAL_OBJECT mode.
       *
       * (2) We can't allocate less than nr_gs_entries_granularity.
       */
      gs_chunks = ALIGN(MAX2(gs_granularity, 2) * gs_entry_size_bytes,
                        chunk_size_bytes) / chunk_size_bytes;
      gs_wants =
         ALIGN(devinfo->urb.max_gs_entries * gs_entry_size_bytes,
               chunk_size_bytes) / chunk_size_bytes - gs_chunks;
   }

   /* There should always be enough URB space to satisfy the minimum
    * requirements of each stage.
    */
   unsigned total_needs = push_constant_chunks + vs_chunks + gs_chunks;
   assert(total_needs <= urb_chunks);

   /* Mete out remaining space (if any) in proportion to "wants". */
   unsigned total_wants = vs_wants + gs_wants;
   unsigned remaining_space = urb_chunks - total_needs;
   if (remaining_space > total_wants)
      remaining_space = total_wants;
   if (remaining_space > 0) {
      unsigned vs_additional = (unsigned)
         round(vs_wants * (((double) remaining_space) / total_wants));
      vs_chunks += vs_additional;
      remaining_space -= vs_additional;
      gs_chunks += remaining_space;
   }

   /* Sanity check that we haven't over-allocated. */
   assert(push_constant_chunks + vs_chunks + gs_chunks <= urb_chunks);

   /* Finally, compute the number of entries that can fit in the space
    * allocated to each stage.
    */
   unsigned nr_vs_entries = vs_chunks * chunk_size_bytes / vs_entry_size_bytes;
   unsigned nr_gs_entries = gs_chunks * chunk_size_bytes / gs_entry_size_bytes;

   /* Since we rounded up when computing *_wants, this may be slightly more
    * than the maximum allowed amount, so correct for that.
    */
   nr_vs_entries = MIN2(nr_vs_entries, devinfo->urb.max_vs_entries);
   nr_gs_entries = MIN2(nr_gs_entries, devinfo->urb.max_gs_entries);

   /* Ensure that we program a multiple of the granularity. */
   nr_vs_entries = ROUND_DOWN_TO(nr_vs_entries, vs_granularity);
   nr_gs_entries = ROUND_DOWN_TO(nr_gs_entries, gs_granularity);

   /* Finally, sanity check to make sure we have at least the minimum number
    * of entries needed for each stage.
    */
   assert(nr_vs_entries >= devinfo->urb.min_vs_entries);
   if (gs_present)
      assert(nr_gs_entries >= 2);

   /* Lay out the URB in the following order:
    * - push constants
    * - VS
    * - GS
    */
   pipeline->urb.vs_start = push_constant_chunks;
   pipeline->urb.vs_size = vs_size;
   pipeline->urb.nr_vs_entries = nr_vs_entries;

   pipeline->urb.gs_start = push_constant_chunks + vs_chunks;
   pipeline->urb.gs_size = gs_size;
   pipeline->urb.nr_gs_entries = nr_gs_entries;
}

static void
anv_pipeline_init_dynamic_state(struct anv_pipeline *pipeline,
                                const VkGraphicsPipelineCreateInfo *pCreateInfo)
{
   anv_cmd_dirty_mask_t states = ANV_CMD_DIRTY_DYNAMIC_ALL;
   ANV_FROM_HANDLE(anv_render_pass, pass, pCreateInfo->renderPass);
   struct anv_subpass *subpass = &pass->subpasses[pCreateInfo->subpass];

   pipeline->dynamic_state = default_dynamic_state;

   if (pCreateInfo->pDynamicState) {
      /* Remove all of the states that are marked as dynamic */
      uint32_t count = pCreateInfo->pDynamicState->dynamicStateCount;
      for (uint32_t s = 0; s < count; s++)
         states &= ~(1 << pCreateInfo->pDynamicState->pDynamicStates[s]);
   }

   struct anv_dynamic_state *dynamic = &pipeline->dynamic_state;

   dynamic->viewport.count = pCreateInfo->pViewportState->viewportCount;
   if (states & (1 << VK_DYNAMIC_STATE_VIEWPORT)) {
      typed_memcpy(dynamic->viewport.viewports,
                   pCreateInfo->pViewportState->pViewports,
                   pCreateInfo->pViewportState->viewportCount);
   }

   dynamic->scissor.count = pCreateInfo->pViewportState->scissorCount;
   if (states & (1 << VK_DYNAMIC_STATE_SCISSOR)) {
      typed_memcpy(dynamic->scissor.scissors,
                   pCreateInfo->pViewportState->pScissors,
                   pCreateInfo->pViewportState->scissorCount);
   }

   if (states & (1 << VK_DYNAMIC_STATE_LINE_WIDTH)) {
      assert(pCreateInfo->pRasterState);
      dynamic->line_width = pCreateInfo->pRasterState->lineWidth;
   }

   if (states & (1 << VK_DYNAMIC_STATE_DEPTH_BIAS)) {
      assert(pCreateInfo->pRasterState);
      dynamic->depth_bias.bias = pCreateInfo->pRasterState->depthBias;
      dynamic->depth_bias.clamp = pCreateInfo->pRasterState->depthBiasClamp;
      dynamic->depth_bias.slope_scaled =
         pCreateInfo->pRasterState->slopeScaledDepthBias;
   }

   if (states & (1 << VK_DYNAMIC_STATE_BLEND_CONSTANTS)) {
      assert(pCreateInfo->pColorBlendState);
      typed_memcpy(dynamic->blend_constants,
                   pCreateInfo->pColorBlendState->blendConst, 4);
   }

   /* If there is no depthstencil attachment, then don't read
    * pDepthStencilState. The Vulkan spec states that pDepthStencilState may
    * be NULL in this case. Even if pDepthStencilState is non-NULL, there is
    * no need to override the depthstencil defaults in
    * anv_pipeline::dynamic_state when there is no depthstencil attachment.
    *
    * From the Vulkan spec (20 Oct 2015, git-aa308cb):
    *
    *    pDepthStencilState [...] may only be NULL if renderPass and subpass
    *    specify a subpass that has no depth/stencil attachment.
    */
   if (subpass->depth_stencil_attachment != VK_ATTACHMENT_UNUSED) {
      if (states & (1 << VK_DYNAMIC_STATE_DEPTH_BOUNDS)) {
         assert(pCreateInfo->pDepthStencilState);
         dynamic->depth_bounds.min =
            pCreateInfo->pDepthStencilState->minDepthBounds;
         dynamic->depth_bounds.max =
            pCreateInfo->pDepthStencilState->maxDepthBounds;
      }

      if (states & (1 << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) {
         assert(pCreateInfo->pDepthStencilState);
         dynamic->stencil_compare_mask.front =
            pCreateInfo->pDepthStencilState->front.stencilCompareMask;
         dynamic->stencil_compare_mask.back =
            pCreateInfo->pDepthStencilState->back.stencilCompareMask;
      }

      if (states & (1 << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) {
         assert(pCreateInfo->pDepthStencilState);
         dynamic->stencil_write_mask.front =
            pCreateInfo->pDepthStencilState->front.stencilWriteMask;
         dynamic->stencil_write_mask.back =
            pCreateInfo->pDepthStencilState->back.stencilWriteMask;
      }

      if (states & (1 << VK_DYNAMIC_STATE_STENCIL_REFERENCE)) {
         assert(pCreateInfo->pDepthStencilState);
         dynamic->stencil_reference.front =
            pCreateInfo->pDepthStencilState->front.stencilReference;
         dynamic->stencil_reference.back =
            pCreateInfo->pDepthStencilState->back.stencilReference;
      }
   }

   pipeline->dynamic_state_mask = states;
}

static void
anv_pipeline_validate_create_info(const VkGraphicsPipelineCreateInfo *info)
{
   struct anv_render_pass *renderpass = NULL;
   struct anv_subpass *subpass = NULL;

   /* Assert that all required members of VkGraphicsPipelineCreateInfo are
    * present, as explained by the Vulkan (20 Oct 2015, git-aa308cb), Section
    * 4.2 Graphics Pipeline.
    */
   assert(info->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);

   renderpass = anv_render_pass_from_handle(info->renderPass);
   assert(renderpass);

   if (renderpass != &anv_meta_dummy_renderpass) {
      assert(info->subpass < renderpass->subpass_count);
      subpass = &renderpass->subpasses[info->subpass];
   }

   assert(info->stageCount >= 1);
   assert(info->pVertexInputState);
   assert(info->pInputAssemblyState);
   assert(info->pViewportState);
   assert(info->pRasterState);
   assert(info->pMultisampleState);

   if (subpass && subpass->depth_stencil_attachment != VK_ATTACHMENT_UNUSED)
      assert(info->pDepthStencilState);

   if (subpass && subpass->color_count > 0)
      assert(info->pColorBlendState);

   for (uint32_t i = 0; i < info->stageCount; ++i) {
      switch (info->pStages[i].stage) {
      case VK_SHADER_STAGE_TESS_CONTROL:
      case VK_SHADER_STAGE_TESS_EVALUATION:
         assert(info->pTessellationState);
         break;
      default:
         break;
      }
   }
}

VkResult
anv_pipeline_init(struct anv_pipeline *pipeline, struct anv_device *device,
                  const VkGraphicsPipelineCreateInfo *pCreateInfo,
                  const struct anv_graphics_pipeline_create_info *extra)
{
   VkResult result;

   anv_validate {
      anv_pipeline_validate_create_info(pCreateInfo);
   }

   pipeline->device = device;
   pipeline->layout = anv_pipeline_layout_from_handle(pCreateInfo->layout);

   result = anv_reloc_list_init(&pipeline->batch_relocs, device);
   if (result != VK_SUCCESS) {
      anv_device_free(device, pipeline);
      return result;
   }
   pipeline->batch.next = pipeline->batch.start = pipeline->batch_data;
   pipeline->batch.end = pipeline->batch.start + sizeof(pipeline->batch_data);
   pipeline->batch.relocs = &pipeline->batch_relocs;

   anv_state_stream_init(&pipeline->program_stream,
                         &device->instruction_block_pool);

   anv_pipeline_init_dynamic_state(pipeline, pCreateInfo);

   if (pCreateInfo->pTessellationState)
      anv_finishme("VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO");
   if (pCreateInfo->pMultisampleState &&
       pCreateInfo->pMultisampleState->rasterSamples > 1)
      anv_finishme("VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO");

   pipeline->use_repclear = extra && extra->use_repclear;
   pipeline->writes_point_size = false;

   /* When we free the pipeline, we detect stages based on the NULL status
    * of various prog_data pointers.  Make them NULL by default.
    */
   memset(pipeline->prog_data, 0, sizeof(pipeline->prog_data));
   memset(pipeline->scratch_start, 0, sizeof(pipeline->scratch_start));

   pipeline->vs_simd8 = NO_KERNEL;
   pipeline->vs_vec4 = NO_KERNEL;
   pipeline->gs_vec4 = NO_KERNEL;

   pipeline->active_stages = 0;
   pipeline->total_scratch = 0;

   for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
      ANV_FROM_HANDLE(anv_shader, shader, pCreateInfo->pStages[i].shader);

      switch (pCreateInfo->pStages[i].stage) {
      case VK_SHADER_STAGE_VERTEX:
         anv_pipeline_compile_vs(pipeline, pCreateInfo, shader);
         break;
      case VK_SHADER_STAGE_GEOMETRY:
         anv_pipeline_compile_gs(pipeline, pCreateInfo, shader);
         break;
      case VK_SHADER_STAGE_FRAGMENT:
         anv_pipeline_compile_fs(pipeline, pCreateInfo, shader);
         break;
      default:
         anv_finishme("Unsupported shader stage");
      }
   }

   if (!(pipeline->active_stages & VK_SHADER_STAGE_VERTEX_BIT)) {
      /* Vertex is only optional if disable_vs is set */
      assert(extra->disable_vs);
      memset(&pipeline->vs_prog_data, 0, sizeof(pipeline->vs_prog_data));
   }

   gen7_compute_urb_partition(pipeline);

   const VkPipelineVertexInputStateCreateInfo *vi_info =
      pCreateInfo->pVertexInputState;
   pipeline->vb_used = 0;
   for (uint32_t i = 0; i < vi_info->bindingCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &vi_info->pVertexBindingDescriptions[i];

      pipeline->vb_used |= 1 << desc->binding;
      pipeline->binding_stride[desc->binding] = desc->strideInBytes;

      /* Step rate is programmed per vertex element (attribute), not
       * binding. Set up a map of which bindings step per instance, for
       * reference by vertex element setup. */
      switch (desc->stepRate) {
      default:
      case VK_VERTEX_INPUT_STEP_RATE_VERTEX:
         pipeline->instancing_enable[desc->binding] = false;
         break;
      case VK_VERTEX_INPUT_STEP_RATE_INSTANCE:
         pipeline->instancing_enable[desc->binding] = true;
         break;
      }
   }

   const VkPipelineInputAssemblyStateCreateInfo *ia_info =
      pCreateInfo->pInputAssemblyState;
   pipeline->primitive_restart = ia_info->primitiveRestartEnable;
   pipeline->topology = vk_to_gen_primitive_type[ia_info->topology];

   if (extra && extra->use_rectlist)
      pipeline->topology = _3DPRIM_RECTLIST;

   return VK_SUCCESS;
}

VkResult
anv_graphics_pipeline_create(
   VkDevice _device,
   const VkGraphicsPipelineCreateInfo *pCreateInfo,
   const struct anv_graphics_pipeline_create_info *extra,
   VkPipeline *pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   switch (device->info.gen) {
   case 7:
      return gen7_graphics_pipeline_create(_device, pCreateInfo, extra, pPipeline);
   case 8:
      return gen8_graphics_pipeline_create(_device, pCreateInfo, extra, pPipeline);
   default:
      unreachable("unsupported gen\n");
   }
}

VkResult anv_CreateGraphicsPipelines(
    VkDevice                                    _device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    count,
    const VkGraphicsPipelineCreateInfo*         pCreateInfos,
    VkPipeline*                                 pPipelines)
{
   VkResult result = VK_SUCCESS;

   unsigned i = 0;
   for (; i < count; i++) {
      result = anv_graphics_pipeline_create(_device, &pCreateInfos[i],
                                            NULL, &pPipelines[i]);
      if (result != VK_SUCCESS) {
         for (unsigned j = 0; j < i; j++) {
            anv_DestroyPipeline(_device, pPipelines[j]);
         }

         return result;
      }
   }

   return VK_SUCCESS;
}

static VkResult anv_compute_pipeline_create(
    VkDevice                                    _device,
    const VkComputePipelineCreateInfo*          pCreateInfo,
    VkPipeline*                                 pPipeline)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   switch (device->info.gen) {
   case 7:
      return gen7_compute_pipeline_create(_device, pCreateInfo, pPipeline);
   case 8:
      return gen8_compute_pipeline_create(_device, pCreateInfo, pPipeline);
   default:
      unreachable("unsupported gen\n");
   }
}

VkResult anv_CreateComputePipelines(
    VkDevice                                    _device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    count,
    const VkComputePipelineCreateInfo*          pCreateInfos,
    VkPipeline*                                 pPipelines)
{
   VkResult result = VK_SUCCESS;

   unsigned i = 0;
   for (; i < count; i++) {
      result = anv_compute_pipeline_create(_device, &pCreateInfos[i],
                                           &pPipelines[i]);
      if (result != VK_SUCCESS) {
         for (unsigned j = 0; j < i; j++) {
            anv_DestroyPipeline(_device, pPipelines[j]);
         }

         return result;
      }
   }

   return VK_SUCCESS;
}

// Pipeline layout functions

VkResult anv_CreatePipelineLayout(
    VkDevice                                    _device,
    const VkPipelineLayoutCreateInfo*           pCreateInfo,
    VkPipelineLayout*                           pPipelineLayout)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_pipeline_layout l, *layout;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);

   l.num_sets = pCreateInfo->descriptorSetCount;

   unsigned dynamic_offset_count = 0;

   memset(l.stage, 0, sizeof(l.stage));
   for (uint32_t set = 0; set < pCreateInfo->descriptorSetCount; set++) {
      ANV_FROM_HANDLE(anv_descriptor_set_layout, set_layout,
                      pCreateInfo->pSetLayouts[set]);
      l.set[set].layout = set_layout;

      l.set[set].dynamic_offset_start = dynamic_offset_count;
      for (uint32_t b = 0; b < set_layout->binding_count; b++) {
         if (set_layout->binding[b].dynamic_offset_index >= 0)
            dynamic_offset_count += set_layout->binding[b].array_size;
      }

      for (VkShaderStage s = 0; s < VK_SHADER_STAGE_NUM; s++) {
         l.set[set].stage[s].surface_start = l.stage[s].surface_count;
         l.set[set].stage[s].sampler_start = l.stage[s].sampler_count;

         for (uint32_t b = 0; b < set_layout->binding_count; b++) {
            unsigned array_size = set_layout->binding[b].array_size;

            if (set_layout->binding[b].stage[s].surface_index >= 0) {
               l.stage[s].surface_count += array_size;

               if (set_layout->binding[b].dynamic_offset_index >= 0)
                  l.stage[s].has_dynamic_offsets = true;
            }

            if (set_layout->binding[b].stage[s].sampler_index >= 0)
               l.stage[s].sampler_count += array_size;
         }
      }
   }

   unsigned num_bindings = 0;
   for (VkShaderStage s = 0; s < VK_SHADER_STAGE_NUM; s++)
      num_bindings += l.stage[s].surface_count + l.stage[s].sampler_count;

   size_t size = sizeof(*layout) + num_bindings * sizeof(layout->entries[0]);

   layout = anv_device_alloc(device, size, 8, VK_SYSTEM_ALLOC_TYPE_API_OBJECT);
   if (layout == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Now we can actually build our surface and sampler maps */
   struct anv_pipeline_binding *entry = layout->entries;
   for (VkShaderStage s = 0; s < VK_SHADER_STAGE_NUM; s++) {
      l.stage[s].surface_to_descriptor = entry;
      entry += l.stage[s].surface_count;
      l.stage[s].sampler_to_descriptor = entry;
      entry += l.stage[s].sampler_count;

      int surface = 0;
      int sampler = 0;
      for (uint32_t set = 0; set < pCreateInfo->descriptorSetCount; set++) {
         struct anv_descriptor_set_layout *set_layout = l.set[set].layout;

         for (uint32_t b = 0; b < set_layout->binding_count; b++) {
            unsigned array_size = set_layout->binding[b].array_size;
            unsigned set_offset = set_layout->binding[b].descriptor_index;

            if (set_layout->binding[b].stage[s].surface_index >= 0) {
               assert(surface == l.set[set].stage[s].surface_start +
                                 set_layout->binding[b].stage[s].surface_index);
               for (unsigned i = 0; i < array_size; i++) {
                  l.stage[s].surface_to_descriptor[surface + i].set = set;
                  l.stage[s].surface_to_descriptor[surface + i].offset = set_offset + i;
               }
               surface += array_size;
            }

            if (set_layout->binding[b].stage[s].sampler_index >= 0) {
               assert(sampler == l.set[set].stage[s].sampler_start +
                                 set_layout->binding[b].stage[s].sampler_index);
               for (unsigned i = 0; i < array_size; i++) {
                  l.stage[s].sampler_to_descriptor[sampler + i].set = set;
                  l.stage[s].sampler_to_descriptor[sampler + i].offset = set_offset + i;
               }
               sampler += array_size;
            }
         }
      }
   }

   /* Finally, we're done setting it up, copy into the allocated version */
   *layout = l;

   *pPipelineLayout = anv_pipeline_layout_to_handle(layout);

   return VK_SUCCESS;
}

void anv_DestroyPipelineLayout(
    VkDevice                                    _device,
    VkPipelineLayout                            _pipelineLayout)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_pipeline_layout, pipeline_layout, _pipelineLayout);

   anv_device_free(device, pipeline_layout);
}
