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

static const uint8_t
anv_surftype(const struct anv_image *image, VkImageViewType view_type,
             bool storage)
{
   switch (view_type) {
   default:
      unreachable("bad VkImageViewType");
   case VK_IMAGE_VIEW_TYPE_1D:
   case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
      assert(image->type == VK_IMAGE_TYPE_1D);
      return SURFTYPE_1D;
   case VK_IMAGE_VIEW_TYPE_CUBE:
   case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
      assert(image->type == VK_IMAGE_TYPE_2D);
      return storage ? SURFTYPE_2D : SURFTYPE_CUBE;
   case VK_IMAGE_VIEW_TYPE_2D:
   case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
      assert(image->type == VK_IMAGE_TYPE_2D);
      return SURFTYPE_2D;
   case VK_IMAGE_VIEW_TYPE_3D:
      assert(image->type == VK_IMAGE_TYPE_3D);
      return SURFTYPE_3D;
   }
}

static enum isl_format
anv_surface_format(const struct anv_device *device, enum isl_format format,
                   bool storage)
{
   if (storage) {
      return isl_lower_storage_image_format(&device->isl_dev, format);
   } else {
      return format;
   }
}

#if GEN_GEN > 7 || GEN_IS_HASWELL
static const uint32_t vk_to_gen_swizzle[] = {
   [VK_COMPONENT_SWIZZLE_ZERO]                 = SCS_ZERO,
   [VK_COMPONENT_SWIZZLE_ONE]                  = SCS_ONE,
   [VK_COMPONENT_SWIZZLE_R]                    = SCS_RED,
   [VK_COMPONENT_SWIZZLE_G]                    = SCS_GREEN,
   [VK_COMPONENT_SWIZZLE_B]                    = SCS_BLUE,
   [VK_COMPONENT_SWIZZLE_A]                    = SCS_ALPHA
};
#endif

static inline uint32_t
vk_to_gen_tex_filter(VkFilter filter, bool anisotropyEnable)
{
   switch (filter) {
   default:
      assert(!"Invalid filter");
   case VK_FILTER_NEAREST:
      return MAPFILTER_NEAREST;
   case VK_FILTER_LINEAR:
      return anisotropyEnable ? MAPFILTER_ANISOTROPIC : MAPFILTER_LINEAR;
   }
}

static inline uint32_t
vk_to_gen_max_anisotropy(float ratio)
{
   return (anv_clamp_f(ratio, 2, 16) - 2) / 2;
}

static const uint32_t vk_to_gen_mipmap_mode[] = {
   [VK_SAMPLER_MIPMAP_MODE_NEAREST]          = MIPFILTER_NEAREST,
   [VK_SAMPLER_MIPMAP_MODE_LINEAR]           = MIPFILTER_LINEAR
};

static const uint32_t vk_to_gen_tex_address[] = {
   [VK_SAMPLER_ADDRESS_MODE_REPEAT]          = TCM_WRAP,
   [VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT] = TCM_MIRROR,
   [VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE]   = TCM_CLAMP,
   [VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE] = TCM_MIRROR_ONCE,
   [VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER] = TCM_CLAMP_BORDER,
};

static const uint32_t vk_to_gen_compare_op[] = {
   [VK_COMPARE_OP_NEVER]                     = PREFILTEROPNEVER,
   [VK_COMPARE_OP_LESS]                      = PREFILTEROPLESS,
   [VK_COMPARE_OP_EQUAL]                     = PREFILTEROPEQUAL,
   [VK_COMPARE_OP_LESS_OR_EQUAL]             = PREFILTEROPLEQUAL,
   [VK_COMPARE_OP_GREATER]                   = PREFILTEROPGREATER,
   [VK_COMPARE_OP_NOT_EQUAL]                 = PREFILTEROPNOTEQUAL,
   [VK_COMPARE_OP_GREATER_OR_EQUAL]          = PREFILTEROPGEQUAL,
   [VK_COMPARE_OP_ALWAYS]                    = PREFILTEROPALWAYS,
};
