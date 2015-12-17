/*
 * Copyright 2015 Intel Corporation
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

#include "isl.h"
#include "brw_compiler.h"

bool
isl_is_storage_image_format(enum isl_format format)
{
   /* XXX: Maybe we should put this in the CSV? */

   switch (format) {
   case ISL_FORMAT_R32G32B32A32_UINT:
   case ISL_FORMAT_R32G32B32A32_SINT:
   case ISL_FORMAT_R32G32B32A32_FLOAT:
   case ISL_FORMAT_R32_UINT:
   case ISL_FORMAT_R32_SINT:
   case ISL_FORMAT_R32_FLOAT:
   case ISL_FORMAT_R16G16B16A16_UINT:
   case ISL_FORMAT_R16G16B16A16_SINT:
   case ISL_FORMAT_R16G16B16A16_FLOAT:
   case ISL_FORMAT_R32G32_UINT:
   case ISL_FORMAT_R32G32_SINT:
   case ISL_FORMAT_R32G32_FLOAT:
   case ISL_FORMAT_R8G8B8A8_UINT:
   case ISL_FORMAT_R8G8B8A8_SINT:
   case ISL_FORMAT_R16G16_UINT:
   case ISL_FORMAT_R16G16_SINT:
   case ISL_FORMAT_R16G16_FLOAT:
   case ISL_FORMAT_R8G8_UINT:
   case ISL_FORMAT_R8G8_SINT:
   case ISL_FORMAT_R16_UINT:
   case ISL_FORMAT_R16_FLOAT:
   case ISL_FORMAT_R16_SINT:
   case ISL_FORMAT_R8_UINT:
   case ISL_FORMAT_R8_SINT:
   case ISL_FORMAT_R10G10B10A2_UINT:
   case ISL_FORMAT_R10G10B10A2_UNORM:
   case ISL_FORMAT_R11G11B10_FLOAT:
   case ISL_FORMAT_R16G16B16A16_UNORM:
   case ISL_FORMAT_R16G16B16A16_SNORM:
   case ISL_FORMAT_R8G8B8A8_UNORM:
   case ISL_FORMAT_R8G8B8A8_SNORM:
   case ISL_FORMAT_R16G16_UNORM:
   case ISL_FORMAT_R16G16_SNORM:
   case ISL_FORMAT_R8G8_UNORM:
   case ISL_FORMAT_R8G8_SNORM:
   case ISL_FORMAT_R16_UNORM:
   case ISL_FORMAT_R16_SNORM:
   case ISL_FORMAT_R8_UNORM:
   case ISL_FORMAT_R8_SNORM:
      return true;
   default:
      return false;
   }
}

enum isl_format
isl_lower_storage_image_format(const struct isl_device *dev,
                               enum isl_format format)
{
   switch (format) {
   /* These are never lowered.  Up to BDW we'll have to fall back to untyped
    * surface access for 128bpp formats.
    */
   case ISL_FORMAT_R32G32B32A32_UINT:
   case ISL_FORMAT_R32G32B32A32_SINT:
   case ISL_FORMAT_R32G32B32A32_FLOAT:
   case ISL_FORMAT_R32_UINT:
   case ISL_FORMAT_R32_SINT:
   case ISL_FORMAT_R32_FLOAT:
      return format;

   /* From HSW to BDW the only 64bpp format supported for typed access is
    * RGBA_UINT16.  IVB falls back to untyped.
    */
   case ISL_FORMAT_R16G16B16A16_UINT:
   case ISL_FORMAT_R16G16B16A16_SINT:
   case ISL_FORMAT_R16G16B16A16_FLOAT:
   case ISL_FORMAT_R32G32_UINT:
   case ISL_FORMAT_R32G32_SINT:
   case ISL_FORMAT_R32G32_FLOAT:
      return (ISL_DEV_GEN(dev) >= 9 ? format :
              ISL_DEV_GEN(dev) >= 8 || dev->info->is_haswell ?
              ISL_FORMAT_R16G16B16A16_UINT :
              ISL_FORMAT_R32G32_UINT);

   /* Up to BDW no SINT or FLOAT formats of less than 32 bits per component
    * are supported.  IVB doesn't support formats with more than one component
    * for typed access.  For 8 and 16 bpp formats IVB relies on the
    * undocumented behavior that typed reads from R_UINT8 and R_UINT16
    * surfaces actually do a 32-bit misaligned read.  The alternative would be
    * to use two surface state entries with different formats for each image,
    * one for reading (using R_UINT32) and another one for writing (using
    * R_UINT8 or R_UINT16), but that would complicate the shaders we generate
    * even more.
    */
   case ISL_FORMAT_R8G8B8A8_UINT:
   case ISL_FORMAT_R8G8B8A8_SINT:
      return (ISL_DEV_GEN(dev) >= 9 ? format :
              ISL_DEV_GEN(dev) >= 8 || dev->info->is_haswell ?
              ISL_FORMAT_R8G8B8A8_UINT : ISL_FORMAT_R32_UINT);

   case ISL_FORMAT_R16G16_UINT:
   case ISL_FORMAT_R16G16_SINT:
   case ISL_FORMAT_R16G16_FLOAT:
      return (ISL_DEV_GEN(dev) >= 9 ? format :
              ISL_DEV_GEN(dev) >= 8 || dev->info->is_haswell ?
              ISL_FORMAT_R16G16_UINT : ISL_FORMAT_R32_UINT);

   case ISL_FORMAT_R8G8_UINT:
   case ISL_FORMAT_R8G8_SINT:
      return (ISL_DEV_GEN(dev) >= 9 ? format :
              ISL_DEV_GEN(dev) >= 8 || dev->info->is_haswell ?
              ISL_FORMAT_R8G8_UINT : ISL_FORMAT_R16_UINT);

   case ISL_FORMAT_R16_UINT:
   case ISL_FORMAT_R16_FLOAT:
   case ISL_FORMAT_R16_SINT:
      return (ISL_DEV_GEN(dev) >= 9 ? format : ISL_FORMAT_R16_UINT);

   case ISL_FORMAT_R8_UINT:
   case ISL_FORMAT_R8_SINT:
      return (ISL_DEV_GEN(dev) >= 9 ? format : ISL_FORMAT_R8_UINT);

   /* Neither the 2/10/10/10 nor the 11/11/10 packed formats are supported
    * by the hardware.
    */
   case ISL_FORMAT_R10G10B10A2_UINT:
   case ISL_FORMAT_R10G10B10A2_UNORM:
   case ISL_FORMAT_R11G11B10_FLOAT:
      return ISL_FORMAT_R32_UINT;

   /* No normalized fixed-point formats are supported by the hardware. */
   case ISL_FORMAT_R16G16B16A16_UNORM:
   case ISL_FORMAT_R16G16B16A16_SNORM:
      return (ISL_DEV_GEN(dev) >= 8 || dev->info->is_haswell ?
              ISL_FORMAT_R16G16B16A16_UINT :
              ISL_FORMAT_R32G32_UINT);

   case ISL_FORMAT_R8G8B8A8_UNORM:
   case ISL_FORMAT_R8G8B8A8_SNORM:
      return (ISL_DEV_GEN(dev) >= 8 || dev->info->is_haswell ?
              ISL_FORMAT_R8G8B8A8_UINT : ISL_FORMAT_R32_UINT);

   case ISL_FORMAT_R16G16_UNORM:
   case ISL_FORMAT_R16G16_SNORM:
      return (ISL_DEV_GEN(dev) >= 8 || dev->info->is_haswell ?
              ISL_FORMAT_R16G16_UINT : ISL_FORMAT_R32_UINT);

   case ISL_FORMAT_R8G8_UNORM:
   case ISL_FORMAT_R8G8_SNORM:
      return (ISL_DEV_GEN(dev) >= 8 || dev->info->is_haswell ?
              ISL_FORMAT_R8G8_UINT : ISL_FORMAT_R16_UINT);

   case ISL_FORMAT_R16_UNORM:
   case ISL_FORMAT_R16_SNORM:
      return ISL_FORMAT_R16_UINT;

   case ISL_FORMAT_R8_UNORM:
   case ISL_FORMAT_R8_SNORM:
      return ISL_FORMAT_R8_UINT;

   default:
      assert(!"Unknown image format");
      return ISL_FORMAT_UNSUPPORTED;
   }
}
