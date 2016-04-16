/*
 * Copyright 2015 Intel Corporation
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice (including the next
 *  paragraph) shall be included in all copies or substantial portions of the
 *  Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 *  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

#include <assert.h>

#include "isl.h"

static inline bool
isl_format_has_channel_type(enum isl_format fmt, enum isl_base_type type)
{
   const struct isl_format_layout *fmtl = isl_format_get_layout(fmt);

   return fmtl->channels.r.type == type ||
          fmtl->channels.g.type == type ||
          fmtl->channels.b.type == type ||
          fmtl->channels.a.type == type ||
          fmtl->channels.l.type == type ||
          fmtl->channels.i.type == type ||
          fmtl->channels.p.type == type;
}

bool
isl_format_has_uint_channel(enum isl_format fmt)
{
   return isl_format_has_channel_type(fmt, ISL_UINT);
}

bool
isl_format_has_sint_channel(enum isl_format fmt)
{
   return isl_format_has_channel_type(fmt, ISL_SINT);
}

enum isl_format
isl_format_rgb_to_rgba(enum isl_format rgb)
{
   assert(isl_format_is_rgb(rgb));

   switch (rgb) {
   case ISL_FORMAT_R32G32B32_FLOAT:    return ISL_FORMAT_R32G32B32A32_FLOAT;
   case ISL_FORMAT_R32G32B32_SINT:     return ISL_FORMAT_R32G32B32A32_SINT;
   case ISL_FORMAT_R32G32B32_UINT:     return ISL_FORMAT_R32G32B32A32_UINT;
   case ISL_FORMAT_R32G32B32_UNORM:    return ISL_FORMAT_R32G32B32A32_UNORM;
   case ISL_FORMAT_R32G32B32_SNORM:    return ISL_FORMAT_R32G32B32A32_SNORM;
   case ISL_FORMAT_R32G32B32_SSCALED:  return ISL_FORMAT_R32G32B32A32_SSCALED;
   case ISL_FORMAT_R32G32B32_USCALED:  return ISL_FORMAT_R32G32B32A32_USCALED;
   case ISL_FORMAT_R32G32B32_SFIXED:   return ISL_FORMAT_R32G32B32A32_SFIXED;
   case ISL_FORMAT_R8G8B8_UNORM:       return ISL_FORMAT_R8G8B8A8_UNORM;
   case ISL_FORMAT_R8G8B8_SNORM:       return ISL_FORMAT_R8G8B8A8_SNORM;
   case ISL_FORMAT_R8G8B8_SSCALED:     return ISL_FORMAT_R8G8B8A8_SSCALED;
   case ISL_FORMAT_R8G8B8_USCALED:     return ISL_FORMAT_R8G8B8A8_USCALED;
   case ISL_FORMAT_R16G16B16_FLOAT:    return ISL_FORMAT_R16G16B16A16_FLOAT;
   case ISL_FORMAT_R16G16B16_UNORM:    return ISL_FORMAT_R16G16B16A16_UNORM;
   case ISL_FORMAT_R16G16B16_SNORM:    return ISL_FORMAT_R16G16B16A16_SNORM;
   case ISL_FORMAT_R16G16B16_SSCALED:  return ISL_FORMAT_R16G16B16A16_SSCALED;
   case ISL_FORMAT_R16G16B16_USCALED:  return ISL_FORMAT_R16G16B16A16_USCALED;
   case ISL_FORMAT_R8G8B8_UNORM_SRGB:  return ISL_FORMAT_R8G8B8A8_UNORM_SRGB;
   case ISL_FORMAT_R16G16B16_UINT:     return ISL_FORMAT_R16G16B16A16_UINT;
   case ISL_FORMAT_R16G16B16_SINT:     return ISL_FORMAT_R16G16B16A16_SINT;
   case ISL_FORMAT_R8G8B8_UINT:        return ISL_FORMAT_R8G8B8A8_UINT;
   case ISL_FORMAT_R8G8B8_SINT:        return ISL_FORMAT_R8G8B8A8_SINT;
   default:
      return ISL_FORMAT_UNSUPPORTED;
   }
}

enum isl_format
isl_format_rgb_to_rgbx(enum isl_format rgb)
{
   assert(isl_format_is_rgb(rgb));

   switch (rgb) {
   case ISL_FORMAT_R32G32B32_FLOAT:
      return ISL_FORMAT_R32G32B32X32_FLOAT;
   case ISL_FORMAT_R16G16B16_UNORM:
      return ISL_FORMAT_R16G16B16X16_UNORM;
   case ISL_FORMAT_R16G16B16_FLOAT:
      return ISL_FORMAT_R16G16B16X16_FLOAT;
   case ISL_FORMAT_R8G8B8_UNORM:
      return ISL_FORMAT_R8G8B8X8_UNORM;
   case ISL_FORMAT_R8G8B8_UNORM_SRGB:
      return ISL_FORMAT_R8G8B8X8_UNORM_SRGB;
   default:
      return ISL_FORMAT_UNSUPPORTED;
   }
}
