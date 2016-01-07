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

#if ANV_GEN > 7 || ANV_IS_HASWELL
static const uint32_t vk_to_gen_swizzle_map[] = {
   [VK_COMPONENT_SWIZZLE_ZERO]                 = SCS_ZERO,
   [VK_COMPONENT_SWIZZLE_ONE]                  = SCS_ONE,
   [VK_COMPONENT_SWIZZLE_R]                    = SCS_RED,
   [VK_COMPONENT_SWIZZLE_G]                    = SCS_GREEN,
   [VK_COMPONENT_SWIZZLE_B]                    = SCS_BLUE,
   [VK_COMPONENT_SWIZZLE_A]                    = SCS_ALPHA
};

static inline uint32_t
vk_to_gen_swizzle(VkComponentSwizzle swizzle, VkComponentSwizzle component)
{
   if (swizzle == VK_COMPONENT_SWIZZLE_IDENTITY)
      return vk_to_gen_swizzle_map[component];
   else
      return vk_to_gen_swizzle_map[swizzle];
}
#endif

static const uint32_t vk_to_gen_tex_filter[] = {
   [VK_FILTER_NEAREST]                       = MAPFILTER_NEAREST,
   [VK_FILTER_LINEAR]                        = MAPFILTER_LINEAR
};

static const uint32_t vk_to_gen_mipmap_mode[] = {
   [VK_SAMPLER_MIPMAP_MODE_BASE]             = MIPFILTER_NONE,
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
