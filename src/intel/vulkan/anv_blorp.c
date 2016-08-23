/*
 * Copyright © 2016 Intel Corporation
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

#include "anv_private.h"

static bool
lookup_blorp_shader(struct blorp_context *blorp,
                    const void *key, uint32_t key_size,
                    uint32_t *kernel_out, void *prog_data_out)
{
   struct anv_device *device = blorp->driver_ctx;

   /* The blorp cache must be a real cache */
   assert(device->blorp_shader_cache.cache);

   struct anv_shader_bin *bin =
      anv_pipeline_cache_search(&device->blorp_shader_cache, key, key_size);
   if (!bin)
      return false;

   /* The cache already has a reference and it's not going anywhere so there
    * is no need to hold a second reference.
    */
   anv_shader_bin_unref(device, bin);

   *kernel_out = bin->kernel.offset;
   *(const struct brw_stage_prog_data **)prog_data_out =
      anv_shader_bin_get_prog_data(bin);

   return true;
}

static void
upload_blorp_shader(struct blorp_context *blorp,
                    const void *key, uint32_t key_size,
                    const void *kernel, uint32_t kernel_size,
                    const void *prog_data, uint32_t prog_data_size,
                    uint32_t *kernel_out, void *prog_data_out)
{
   struct anv_device *device = blorp->driver_ctx;

   /* The blorp cache must be a real cache */
   assert(device->blorp_shader_cache.cache);

   struct anv_pipeline_bind_map bind_map = {
      .surface_count = 0,
      .sampler_count = 0,
   };

   struct anv_shader_bin *bin =
      anv_pipeline_cache_upload_kernel(&device->blorp_shader_cache,
                                       key, key_size, kernel, kernel_size,
                                       prog_data, prog_data_size, &bind_map);

   /* The cache already has a reference and it's not going anywhere so there
    * is no need to hold a second reference.
    */
   anv_shader_bin_unref(device, bin);

   *kernel_out = bin->kernel.offset;
   *(const struct brw_stage_prog_data **)prog_data_out =
      anv_shader_bin_get_prog_data(bin);
}

void
anv_device_init_blorp(struct anv_device *device)
{
   anv_pipeline_cache_init(&device->blorp_shader_cache, device, true);
   blorp_init(&device->blorp, device, &device->isl_dev);
   device->blorp.compiler = device->instance->physicalDevice.compiler;
   device->blorp.mocs.tex = device->default_mocs;
   device->blorp.mocs.rb = device->default_mocs;
   device->blorp.mocs.vb = device->default_mocs;
   device->blorp.lookup_shader = lookup_blorp_shader;
   device->blorp.upload_shader = upload_blorp_shader;
   switch (device->info.gen) {
   case 7:
      if (device->info.is_haswell) {
         device->blorp.exec = gen75_blorp_exec;
      } else {
         device->blorp.exec = gen7_blorp_exec;
      }
      break;
   case 8:
      device->blorp.exec = gen8_blorp_exec;
      break;
   case 9:
      device->blorp.exec = gen9_blorp_exec;
      break;
   default:
      unreachable("Unknown hardware generation");
   }
}

void
anv_device_finish_blorp(struct anv_device *device)
{
   blorp_finish(&device->blorp);
   anv_pipeline_cache_finish(&device->blorp_shader_cache);
}
