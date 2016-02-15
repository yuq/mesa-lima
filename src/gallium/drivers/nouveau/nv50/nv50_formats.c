/*
 * Copyright 2010 Christoph Bumiller
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#if NOUVEAU_DRIVER == 0xc0
# include "nvc0/nvc0_screen.h"
# include "nvc0/nvc0_3d.xml.h"
#else
# include "nv50/nv50_screen.h"
# include "nv50/nv50_3d.xml.h"
#endif
#include "nv50/nv50_texture.xml.h"
#include "nv50/g80_defs.xml.h"

#include "pipe/p_defines.h"

/* Abbreviated usage masks:
 * T: texturing
 * R: render target
 * B: render target, blendable
 * C: render target (color), blendable only on nvc0
 * D: scanout/display target, blendable
 * Z: depth/stencil
 * I: image / surface, implies T
 */
#define U_T   PIPE_BIND_SAMPLER_VIEW
#define U_I   PIPE_BIND_SHADER_BUFFER | PIPE_BIND_SHADER_IMAGE | PIPE_BIND_COMPUTE_RESOURCE
#define U_TR  PIPE_BIND_RENDER_TARGET | U_T
#define U_IR  U_TR | U_I
#define U_TB  PIPE_BIND_BLENDABLE | U_TR
#define U_IB  PIPE_BIND_BLENDABLE | U_IR
#define U_TD  PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET | U_TB
#define U_TZ  PIPE_BIND_DEPTH_STENCIL | U_T
#if NOUVEAU_DRIVER == 0xc0
# define U_TC  U_TB
# define U_IC  U_IB
# define U_t   U_T
#else
# define U_TC  U_TR
# define U_IC  U_IR
# define U_t   0
#endif

#define G80_ZETA_FORMAT_NONE    0
#define G80_SURFACE_FORMAT_NONE 0

#define SF_A(sz) NV50_TIC_0_FMT_##sz
#define SF_C(sz) NVC0_TIC_0_FMT_##sz
#define SF(c, pf, sf, r, g, b, a, t0, t1, t2, t3, sz, u)                \
   [PIPE_FORMAT_##pf] = {                                               \
      sf,                                                               \
      (NV50_TIC_MAP_##r << NV50_TIC_0_MAPR__SHIFT) |                    \
      (NV50_TIC_MAP_##g << NV50_TIC_0_MAPG__SHIFT) |                    \
      (NV50_TIC_MAP_##b << NV50_TIC_0_MAPB__SHIFT) |                    \
      (NV50_TIC_MAP_##a << NV50_TIC_0_MAPA__SHIFT) |                    \
      (NV50_TIC_TYPE_##t0 << NV50_TIC_0_TYPE0__SHIFT) |                 \
      (NV50_TIC_TYPE_##t1 << NV50_TIC_0_TYPE1__SHIFT) |                 \
      (NV50_TIC_TYPE_##t2 << NV50_TIC_0_TYPE2__SHIFT) |                 \
      (NV50_TIC_TYPE_##t3 << NV50_TIC_0_TYPE3__SHIFT) |                 \
      SF_##c(sz), U_##u                                                 \
   }

#define C4(c, p, n, r, g, b, a, t, s, u)                                \
   SF(c, p, G80_SURFACE_FORMAT_##n, r, g, b, a, t, t, t, t, s, u)

#define ZX(c, p, n, r, g, b, a, t, s, u)                                \
   SF(c, p, G80_ZETA_FORMAT_##n,                                        \
      r, g, b, ONE_FLOAT, t, UINT, UINT, UINT, s, u)
#define ZS(c, p, n, r, g, b, a, t, s, u)                                \
   SF(c, p, G80_ZETA_FORMAT_##n,                                        \
      r, g, b, ONE_FLOAT, t, UINT, UINT, UINT, s, u)
#define SZ(c, p, n, r, g, b, a, t, s, u)                                \
   SF(c, p, G80_ZETA_FORMAT_##n,                                        \
      r, g, b, ONE_FLOAT, UINT, t, UINT, UINT, s, u)
#define SX(c, p, r, s, u)                                               \
   SF(c, p, G80_ZETA_FORMAT_NONE,                                       \
      r, r, r, r, UINT, UINT, UINT, UINT, s, u)

#define F3(c, p, n, r, g, b, a, t, s, u)         \
   C4(c, p, n, r, g, b, ONE_FLOAT, t, s, u)
#define I3(c, p, n, r, g, b, a, t, s, u)         \
   C4(c, p, n, r, g, b, ONE_INT, t, s, u)

#define F2(c, p, n, r, g, b, a, t, s, u)         \
   C4(c, p, n, r, g, ZERO, ONE_FLOAT, t, s, u)
#define I2(c, p, n, r, g, b, a, t, s, u)         \
   C4(c, p, n, r, g, ZERO, ONE_INT, t, s, u)

#define F1(c, p, n, r, g, b, a, t, s, u)         \
   C4(c, p, n, r, ZERO, ZERO, ONE_FLOAT, t, s, u)
#define I1(c, p, n, r, g, b, a, t, s, u)         \
   C4(c, p, n, r, ZERO, ZERO, ONE_INT, t, s, u)

#define A1(c, p, n, r, g, b, a, t, s, u)         \
   C4(c, p, n, ZERO, ZERO, ZERO, a, t, s, u)

#if NOUVEAU_DRIVER == 0xc0
const struct nvc0_format nvc0_format_table[PIPE_FORMAT_COUNT] =
#else
const struct nv50_format nv50_format_table[PIPE_FORMAT_COUNT] =
#endif
{
   C4(A, B8G8R8A8_UNORM, BGRA8_UNORM, C2, C1, C0, C3, UNORM, 8_8_8_8, TD),
   F3(A, B8G8R8X8_UNORM, BGRX8_UNORM, C2, C1, C0, xx, UNORM, 8_8_8_8, TD),
   C4(A, B8G8R8A8_SRGB, BGRA8_SRGB, C2, C1, C0, C3, UNORM, 8_8_8_8, TD),
   F3(A, B8G8R8X8_SRGB, BGRX8_SRGB, C2, C1, C0, xx, UNORM, 8_8_8_8, TD),
   C4(A, R8G8B8A8_UNORM, RGBA8_UNORM, C0, C1, C2, C3, UNORM, 8_8_8_8, IB),
   F3(A, R8G8B8X8_UNORM, RGBX8_UNORM, C0, C1, C2, xx, UNORM, 8_8_8_8, TB),
   C4(A, R8G8B8A8_SRGB, RGBA8_SRGB, C0, C1, C2, C3, UNORM, 8_8_8_8, TB),
   F3(A, R8G8B8X8_SRGB, RGBX8_SRGB, C0, C1, C2, xx, UNORM, 8_8_8_8, TB),

   ZX(A, Z16_UNORM, Z16_UNORM, C0, C0, C0, xx, UNORM, Z16, TZ),
   ZX(A, Z32_FLOAT, Z32_FLOAT, C0, C0, C0, xx, FLOAT, Z32, TZ),
   ZX(A, Z24X8_UNORM, Z24_X8_UNORM, C0, C0, C0, xx, UNORM, Z24_X8, TZ),
   SZ(A, X8Z24_UNORM, S8_Z24_UNORM, C1, C1, C1, xx, UNORM, S8_Z24, TZ),
   ZS(A, Z24_UNORM_S8_UINT, Z24_S8_UNORM, C0, C0, C0, xx, UNORM, Z24_S8, TZ),
   SZ(A, S8_UINT_Z24_UNORM, S8_Z24_UNORM, C1, C1, C1, xx, UNORM, S8_Z24, TZ),
   ZS(A, Z32_FLOAT_S8X24_UINT, Z32_S8_X24_FLOAT, C0, C0, C0, xx, FLOAT, Z32_S8_X24, TZ),

   SX(A, S8_UINT, C0, 8, T),
   SX(A, X24S8_UINT, C1, Z24_S8, T),
   SX(A, S8X24_UINT, C0, S8_Z24, T),
   SX(A, X32_S8X24_UINT, C1, Z32_S8_X24, T),

   F3(A, B5G6R5_UNORM, B5G6R5_UNORM, C2, C1, C0, xx, UNORM, 5_6_5, TD),
   C4(A, B5G5R5A1_UNORM, BGR5_A1_UNORM, C2, C1, C0, C3, UNORM, 5_5_5_1, TD),
   F3(A, B5G5R5X1_UNORM, BGR5_X1_UNORM, C2, C1, C0, xx, UNORM, 5_5_5_1, TD),
   C4(A, B4G4R4A4_UNORM, NONE, C2, C1, C0, C3, UNORM, 4_4_4_4, T),
   F3(A, B4G4R4X4_UNORM, NONE, C2, C1, C0, xx, UNORM, 4_4_4_4, T),
   F3(A, R9G9B9E5_FLOAT, NONE, C0, C1, C2, xx, FLOAT, 9_9_9_E5, T),

   C4(A, R10G10B10A2_UNORM, RGB10_A2_UNORM, C0, C1, C2, C3, UNORM, 10_10_10_2, IB),
   C4(A, B10G10R10A2_UNORM, BGR10_A2_UNORM, C2, C1, C0, C3, UNORM, 10_10_10_2, TD),
   C4(A, R10G10B10A2_SNORM, NONE, C0, C1, C2, C3, SNORM, 10_10_10_2, T),
   C4(A, B10G10R10A2_SNORM, NONE, C2, C1, C0, C3, SNORM, 10_10_10_2, T),
   C4(A, R10G10B10A2_UINT, RGB10_A2_UINT, C0, C1, C2, C3, UINT, 10_10_10_2, TR),
   C4(A, B10G10R10A2_UINT, RGB10_A2_UINT, C2, C1, C0, C3, UINT, 10_10_10_2, T),

   F3(A, R11G11B10_FLOAT, R11G11B10_FLOAT, C0, C1, C2, xx, FLOAT, 11_11_10, IB),

   F3(A, L8_UNORM, R8_UNORM, C0, C0, C0, xx, UNORM, 8, TB),
   F3(A, L8_SRGB, R8_UNORM, C0, C0, C0, xx, UNORM, 8, TB),
   F3(A, L8_SNORM, R8_SNORM, C0, C0, C0, xx, SNORM, 8, TC),
   I3(A, L8_SINT, R8_SINT, C0, C0, C0, xx, SINT, 8, TR),
   I3(A, L8_UINT, R8_UINT, C0, C0, C0, xx, UINT, 8, TR),
   F3(A, L16_UNORM, R16_UNORM, C0, C0, C0, xx, UNORM, 16, TC),
   F3(A, L16_SNORM, R16_SNORM, C0, C0, C0, xx, SNORM, 16, TC),
   F3(A, L16_FLOAT, R16_FLOAT, C0, C0, C0, xx, FLOAT, 16, TB),
   I3(A, L16_SINT, R16_SINT, C0, C0, C0, xx, SINT, 16, TR),
   I3(A, L16_UINT, R16_UINT, C0, C0, C0, xx, UINT, 16, TR),
   F3(A, L32_FLOAT, R32_FLOAT, C0, C0, C0, xx, FLOAT, 32, TB),
   I3(A, L32_SINT, R32_SINT, C0, C0, C0, xx, SINT, 32, TR),
   I3(A, L32_UINT, R32_UINT, C0, C0, C0, xx, UINT, 32, TR),

   C4(A, I8_UNORM, R8_UNORM, C0, C0, C0, C0, UNORM, 8, TR),
   C4(A, I8_SNORM, R8_SNORM, C0, C0, C0, C0, SNORM, 8, TR),
   C4(A, I8_SINT, R8_SINT, C0, C0, C0, C0, SINT, 8, TR),
   C4(A, I8_UINT, R8_UINT, C0, C0, C0, C0, UINT, 8, TR),
   C4(A, I16_UNORM, R16_UNORM, C0, C0, C0, C0, UNORM, 16, TR),
   C4(A, I16_SNORM, R16_SNORM, C0, C0, C0, C0, SNORM, 16, TR),
   C4(A, I16_FLOAT, R16_FLOAT, C0, C0, C0, C0, FLOAT, 16, TR),
   C4(A, I16_SINT, R16_SINT, C0, C0, C0, C0, SINT, 16, TR),
   C4(A, I16_UINT, R16_UINT, C0, C0, C0, C0, UINT, 16, TR),
   C4(A, I32_FLOAT, R32_FLOAT, C0, C0, C0, C0, FLOAT, 32, TR),
   C4(A, I32_SINT, R32_SINT, C0, C0, C0, C0, SINT, 32, TR),
   C4(A, I32_UINT, R32_UINT, C0, C0, C0, C0, UINT, 32, TR),

   A1(A, A8_UNORM, A8_UNORM, xx, xx, xx, C0, UNORM, 8, TB),
   A1(A, A8_SNORM, R8_SNORM, xx, xx, xx, C0, SNORM, 8, T),
   A1(A, A8_SINT, R8_SINT, xx, xx, xx, C0, SINT, 8, T),
   A1(A, A8_UINT, R8_UINT, xx, xx, xx, C0, UINT, 8, T),
   A1(A, A16_UNORM, R16_UNORM, xx, xx, xx, C0, UNORM, 16, T),
   A1(A, A16_SNORM, R16_SNORM, xx, xx, xx, C0, SNORM, 16, T),
   A1(A, A16_FLOAT, R16_FLOAT, xx, xx, xx, C0, FLOAT, 16, T),
   A1(A, A16_SINT, R16_SINT, xx, xx, xx, C0, SINT, 16, T),
   A1(A, A16_UINT, R16_UINT, xx, xx, xx, C0, UINT, 16, T),
   A1(A, A32_FLOAT, R32_FLOAT, xx, xx, xx, C0, FLOAT, 32, T),
   A1(A, A32_SINT, R32_SINT, xx, xx, xx, C0, SINT, 32, T),
   A1(A, A32_UINT, R32_UINT, xx, xx, xx, C0, UINT, 32, T),

   C4(A, L4A4_UNORM, NONE, C0, C0, C0, C1, UNORM, 4_4, T),
   C4(A, L8A8_UNORM, RG8_UNORM, C0, C0, C0, C1, UNORM, 8_8, T),
   C4(A, L8A8_SNORM, RG8_SNORM, C0, C0, C0, C1, SNORM, 8_8, T),
   C4(A, L8A8_SRGB, RG8_UNORM, C0, C0, C0, C1, UNORM, 8_8, T),
   C4(A, L8A8_SINT, RG8_SINT, C0, C0, C0, C1, SINT, 8_8, T),
   C4(A, L8A8_UINT, RG8_UINT, C0, C0, C0, C1, UINT, 8_8, T),
   C4(A, L16A16_UNORM, RG16_UNORM, C0, C0, C0, C1, UNORM, 16_16, T),
   C4(A, L16A16_SNORM, RG16_SNORM, C0, C0, C0, C1, SNORM, 16_16, T),
   C4(A, L16A16_FLOAT, RG16_FLOAT, C0, C0, C0, C1, FLOAT, 16_16, T),
   C4(A, L16A16_SINT, RG16_SINT, C0, C0, C0, C1, SINT, 16_16, T),
   C4(A, L16A16_UINT, RG16_UINT, C0, C0, C0, C1, UINT, 16_16, T),
   C4(A, L32A32_FLOAT, RG32_FLOAT, C0, C0, C0, C1, FLOAT, 32_32, T),
   C4(A, L32A32_SINT, RG32_SINT, C0, C0, C0, C1, SINT, 32_32, T),
   C4(A, L32A32_UINT, RG32_UINT, C0, C0, C0, C1, UINT, 32_32, T),

   F3(A, DXT1_RGB,   NONE, C0, C1, C2, xx, UNORM, DXT1, T),
   F3(A, DXT1_SRGB,  NONE, C0, C1, C2, xx, UNORM, DXT1, T),
   C4(A, DXT1_RGBA,  NONE, C0, C1, C2, C3, UNORM, DXT1, T),
   C4(A, DXT1_SRGBA, NONE, C0, C1, C2, C3, UNORM, DXT1, T),
   C4(A, DXT3_RGBA,  NONE, C0, C1, C2, C3, UNORM, DXT3, T),
   C4(A, DXT3_SRGBA, NONE, C0, C1, C2, C3, UNORM, DXT3, T),
   C4(A, DXT5_RGBA,  NONE, C0, C1, C2, C3, UNORM, DXT5, T),
   C4(A, DXT5_SRGBA, NONE, C0, C1, C2, C3, UNORM, DXT5, T),

   F1(A, RGTC1_UNORM, NONE, C0, xx, xx, xx, UNORM, RGTC1, T),
   F1(A, RGTC1_SNORM, NONE, C0, xx, xx, xx, SNORM, RGTC1, T),
   F2(A, RGTC2_UNORM, NONE, C0, C1, xx, xx, UNORM, RGTC2, T),
   F2(A, RGTC2_SNORM, NONE, C0, C1, xx, xx, SNORM, RGTC2, T),
   F3(A, LATC1_UNORM, NONE, C0, C0, C0, xx, UNORM, RGTC1, T),
   F3(A, LATC1_SNORM, NONE, C0, C0, C0, xx, SNORM, RGTC1, T),
   C4(A, LATC2_UNORM, NONE, C0, C0, C0, C1, UNORM, RGTC2, T),
   C4(A, LATC2_SNORM, NONE, C0, C0, C0, C1, SNORM, RGTC2, T),

   C4(C, BPTC_RGBA_UNORM, NONE, C0, C1, C2, C3, UNORM, BPTC, t),
   C4(C, BPTC_SRGBA,      NONE, C0, C1, C2, C3, UNORM, BPTC, t),
   F3(C, BPTC_RGB_FLOAT,  NONE, C0, C1, C2, xx, FLOAT, BPTC_FLOAT, t),
   F3(C, BPTC_RGB_UFLOAT, NONE, C0, C1, C2, xx, FLOAT, BPTC_UFLOAT, t),

   C4(A, R32G32B32A32_FLOAT, RGBA32_FLOAT, C0, C1, C2, C3, FLOAT, 32_32_32_32, IB),
   C4(A, R32G32B32A32_UNORM, NONE, C0, C1, C2, C3, UNORM, 32_32_32_32, T),
   C4(A, R32G32B32A32_SNORM, NONE, C0, C1, C2, C3, SNORM, 32_32_32_32, T),
   C4(A, R32G32B32A32_SINT, RGBA32_SINT, C0, C1, C2, C3, SINT, 32_32_32_32, IR),
   C4(A, R32G32B32A32_UINT, RGBA32_UINT, C0, C1, C2, C3, UINT, 32_32_32_32, IR),
   F3(A, R32G32B32X32_FLOAT, RGBX32_FLOAT, C0, C1, C2, xx, FLOAT, 32_32_32_32, TB),
   I3(A, R32G32B32X32_SINT, RGBX32_SINT, C0, C1, C2, xx, SINT, 32_32_32_32, TR),
   I3(A, R32G32B32X32_UINT, RGBX32_UINT, C0, C1, C2, xx, UINT, 32_32_32_32, TR),

   F3(C, R32G32B32_FLOAT, NONE, C0, C1, C2, xx, FLOAT, 32_32_32, t),
   I3(C, R32G32B32_SINT, NONE, C0, C1, C2, xx, SINT, 32_32_32, t),
   I3(C, R32G32B32_UINT, NONE, C0, C1, C2, xx, UINT, 32_32_32, t),

   F2(A, R32G32_FLOAT, RG32_FLOAT, C0, C1, xx, xx, FLOAT, 32_32, IB),
   F2(A, R32G32_UNORM, NONE, C0, C1, xx, xx, UNORM, 32_32, T),
   F2(A, R32G32_SNORM, NONE, C0, C1, xx, xx, SNORM, 32_32, T),
   I2(A, R32G32_SINT, RG32_SINT, C0, C1, xx, xx, SINT, 32_32, IR),
   I2(A, R32G32_UINT, RG32_UINT, C0, C1, xx, xx, UINT, 32_32, IR),

   F1(A, R32_FLOAT, R32_FLOAT, C0, xx, xx, xx, FLOAT, 32, IB),
   F1(A, R32_UNORM, NONE, C0, xx, xx, xx, UNORM, 32, T),
   F1(A, R32_SNORM, NONE, C0, xx, xx, xx, SNORM, 32, T),
   I1(A, R32_SINT, R32_SINT, C0, xx, xx, xx, SINT, 32, IR),
   I1(A, R32_UINT, R32_UINT, C0, xx, xx, xx, UINT, 32, IR),

   C4(A, R16G16B16A16_FLOAT, RGBA16_FLOAT, C0, C1, C2, C3, FLOAT, 16_16_16_16, IB),
   C4(A, R16G16B16A16_UNORM, RGBA16_UNORM, C0, C1, C2, C3, UNORM, 16_16_16_16, IC),
   C4(A, R16G16B16A16_SNORM, RGBA16_SNORM, C0, C1, C2, C3, SNORM, 16_16_16_16, IC),
   C4(A, R16G16B16A16_SINT, RGBA16_SINT, C0, C1, C2, C3, SINT, 16_16_16_16, IR),
   C4(A, R16G16B16A16_UINT, RGBA16_UINT, C0, C1, C2, C3, UINT, 16_16_16_16, IR),
   F3(A, R16G16B16X16_FLOAT, RGBX16_FLOAT, C0, C1, C2, xx, FLOAT, 16_16_16_16, TB),
   F3(A, R16G16B16X16_UNORM, RGBA16_UNORM, C0, C1, C2, xx, UNORM, 16_16_16_16, T),
   F3(A, R16G16B16X16_SNORM, RGBA16_SNORM, C0, C1, C2, xx, SNORM, 16_16_16_16, T),
   I3(A, R16G16B16X16_SINT, RGBA16_SINT, C0, C1, C2, xx, SINT, 16_16_16_16, T),
   I3(A, R16G16B16X16_UINT, RGBA16_UINT, C0, C1, C2, xx, UINT, 16_16_16_16, T),

   F2(A, R16G16_FLOAT, RG16_FLOAT, C0, C1, xx, xx, FLOAT, 16_16, IB),
   F2(A, R16G16_UNORM, RG16_UNORM, C0, C1, xx, xx, UNORM, 16_16, IC),
   F2(A, R16G16_SNORM, RG16_SNORM, C0, C1, xx, xx, SNORM, 16_16, IC),
   I2(A, R16G16_SINT, RG16_SINT, C0, C1, xx, xx, SINT, 16_16, IR),
   I2(A, R16G16_UINT, RG16_UINT, C0, C1, xx, xx, UINT, 16_16, IR),

   F1(A, R16_FLOAT, R16_FLOAT, C0, xx, xx, xx, FLOAT, 16, IB),
   F1(A, R16_UNORM, R16_UNORM, C0, xx, xx, xx, UNORM, 16, IC),
   F1(A, R16_SNORM, R16_SNORM, C0, xx, xx, xx, SNORM, 16, IC),
   I1(A, R16_SINT, R16_SINT, C0, xx, xx, xx, SINT, 16, IR),
   I1(A, R16_UINT, R16_UINT, C0, xx, xx, xx, UINT, 16, IR),

   C4(A, R8G8B8A8_SNORM, RGBA8_SNORM, C0, C1, C2, C3, SNORM, 8_8_8_8, IC),
   C4(A, R8G8B8A8_SINT, RGBA8_SINT, C0, C1, C2, C3, SINT, 8_8_8_8, IR),
   C4(A, R8G8B8A8_UINT, RGBA8_UINT, C0, C1, C2, C3, UINT, 8_8_8_8, IR),
   F3(A, R8G8B8X8_SNORM, RGBA8_SNORM, C0, C1, C2, xx, SNORM, 8_8_8_8, T),
   I3(A, R8G8B8X8_SINT, RGBA8_SINT, C0, C1, C2, xx, SINT, 8_8_8_8, T),
   I3(A, R8G8B8X8_UINT, RGBA8_UINT, C0, C1, C2, xx, UINT, 8_8_8_8, T),

   F2(A, R8G8_UNORM, RG8_UNORM, C0, C1, xx, xx, UNORM, 8_8, IB),
   F2(A, R8G8_SNORM, RG8_SNORM, C0, C1, xx, xx, SNORM, 8_8, IC),
   I2(A, R8G8_SINT, RG8_SINT, C0, C1, xx, xx, SINT, 8_8, IR),
   I2(A, R8G8_UINT, RG8_UINT, C0, C1, xx, xx, UINT, 8_8, IR),

   F1(A, R8_UNORM, R8_UNORM, C0, xx, xx, xx, UNORM, 8, IB),
   F1(A, R8_SNORM, R8_SNORM, C0, xx, xx, xx, SNORM, 8, IC),
   I1(A, R8_SINT, R8_SINT, C0, xx, xx, xx, SINT, 8, IR),
   I1(A, R8_UINT, R8_UINT, C0, xx, xx, xx, UINT, 8, IR),

   F3(A, R8G8_B8G8_UNORM, NONE, C0, C1, C2, xx, UNORM, U8_YA8_V8_YB8, T),
   F3(A, G8R8_B8R8_UNORM, NONE, C1, C0, C2, xx, UNORM, U8_YA8_V8_YB8, T),
   F3(A, G8R8_G8B8_UNORM, NONE, C0, C1, C2, xx, UNORM, YA8_U8_YB8_V8, T),
   F3(A, R8G8_R8B8_UNORM, NONE, C1, C0, C2, xx, UNORM, YA8_U8_YB8_V8, T),

   F1(A, R1_UNORM, BITMAP, C0, xx, xx, xx, UNORM, BITMAP, T),

   C4(A, R4A4_UNORM, NONE, C0, ZERO, ZERO, C1, UNORM, 4_4, T),
   C4(A, R8A8_UNORM, NONE, C0, ZERO, ZERO, C1, UNORM, 8_8, T),
   C4(A, A4R4_UNORM, NONE, C1, ZERO, ZERO, C0, UNORM, 4_4, T),
   C4(A, A8R8_UNORM, NONE, C1, ZERO, ZERO, C0, UNORM, 8_8, T),

   SF(A, R8SG8SB8UX8U_NORM, 0, C0, C1, C2, ONE_FLOAT, SNORM, SNORM, UNORM, UNORM, 8_8_8_8, T),
   SF(A, R5SG5SB6U_NORM, 0, C0, C1, C2, ONE_FLOAT, SNORM, SNORM, UNORM, UNORM, 5_5_6, T),
};

#if NOUVEAU_DRIVER == 0xc0
# define NVXX_3D_VAF_SIZE(s) NVC0_3D_VERTEX_ATTRIB_FORMAT_SIZE_##s
# define NVXX_3D_VAF_TYPE(t) NVC0_3D_VERTEX_ATTRIB_FORMAT_TYPE_##t
#else
# define NVXX_3D_VAF_SIZE(s) NV50_3D_VERTEX_ARRAY_ATTRIB_FORMAT_##s
# define NVXX_3D_VAF_TYPE(t) NV50_3D_VERTEX_ARRAY_ATTRIB_TYPE_##t
#endif

#define VF_x(pf, type, size, bgra) 0
#define VF_A(pf, type, size, bgra)                                      \
      NVXX_3D_VAF_SIZE(size) | NVXX_3D_VAF_TYPE(type) | (bgra << 31)
#define VF(c, pf, type, size, bgra)                                     \
   [PIPE_FORMAT_##pf] = {                                               \
      VF_##c(pf, type, size, bgra),                                     \
      PIPE_BIND_VERTEX_BUFFER                                           \
   }

#if NOUVEAU_DRIVER == 0xc0
const struct nvc0_vertex_format nvc0_vertex_format[PIPE_FORMAT_COUNT] =
#else
const struct nv50_vertex_format nv50_vertex_format[PIPE_FORMAT_COUNT] =
#endif
{
   VF(A, B8G8R8A8_UNORM, UNORM, 8_8_8_8, 1),
   VF(A, R8G8B8A8_UNORM, UNORM, 8_8_8_8, 0),

   VF(A, R10G10B10A2_UNORM, UNORM, 10_10_10_2, 0),
   VF(A, B10G10R10A2_UNORM, UNORM, 10_10_10_2, 1),
   VF(A, R10G10B10A2_SNORM, SNORM, 10_10_10_2, 0),
   VF(A, B10G10R10A2_SNORM, SNORM, 10_10_10_2, 1),
   VF(A, R10G10B10A2_UINT, UINT, 10_10_10_2, 0),
   VF(A, B10G10R10A2_UINT, UINT, 10_10_10_2, 0),

   VF(A, R11G11B10_FLOAT, FLOAT, 11_11_10, 0),

   VF(A, R32G32B32A32_FLOAT, FLOAT, 32_32_32_32, 0),
   VF(A, R32G32B32A32_UNORM, UNORM, 32_32_32_32, 0),
   VF(A, R32G32B32A32_SNORM, SNORM, 32_32_32_32, 0),
   VF(A, R32G32B32A32_SINT, SINT, 32_32_32_32, 0),
   VF(A, R32G32B32A32_UINT, UINT, 32_32_32_32, 0),

   VF(A, R32G32_FLOAT, FLOAT, 32_32, 0),
   VF(A, R32G32_UNORM, UNORM, 32_32, 0),
   VF(A, R32G32_SNORM, SNORM, 32_32, 0),
   VF(A, R32G32_SINT, SINT, 32_32, 0),
   VF(A, R32G32_UINT, UINT, 32_32, 0),

   VF(A, R32_FLOAT, FLOAT, 32, 0),
   VF(A, R32_UNORM, UNORM, 32, 0),
   VF(A, R32_SNORM, SNORM, 32, 0),
   VF(A, R32_SINT, SINT, 32, 0),
   VF(A, R32_UINT, UINT, 32, 0),

   VF(A, R16G16B16A16_FLOAT, FLOAT, 16_16_16_16, 0),
   VF(A, R16G16B16A16_UNORM, UNORM, 16_16_16_16, 0),
   VF(A, R16G16B16A16_SNORM, SNORM, 16_16_16_16, 0),
   VF(A, R16G16B16A16_SINT, SINT, 16_16_16_16, 0),
   VF(A, R16G16B16A16_UINT, UINT, 16_16_16_16, 0),

   VF(A, R16G16_FLOAT, FLOAT, 16_16, 0),
   VF(A, R16G16_UNORM, UNORM, 16_16, 0),
   VF(A, R16G16_SNORM, SNORM, 16_16, 0),
   VF(A, R16G16_SINT, SINT, 16_16, 0),
   VF(A, R16G16_UINT, UINT, 16_16, 0),

   VF(A, R16_FLOAT, FLOAT, 16, 0),
   VF(A, R16_UNORM, UNORM, 16, 0),
   VF(A, R16_SNORM, SNORM, 16, 0),
   VF(A, R16_SINT, SINT, 16, 0),
   VF(A, R16_UINT, UINT, 16, 0),

   VF(A, R8G8B8A8_SNORM, SNORM, 8_8_8_8, 0),
   VF(A, R8G8B8A8_SINT, SINT, 8_8_8_8, 0),
   VF(A, R8G8B8A8_UINT, UINT, 8_8_8_8, 0),

   VF(A, R8G8_UNORM, UNORM, 8_8, 0),
   VF(A, R8G8_SNORM, SNORM, 8_8, 0),
   VF(A, R8G8_SINT, SINT, 8_8, 0),
   VF(A, R8G8_UINT, UINT, 8_8, 0),

   VF(A, R8_UNORM, UNORM, 8, 0),
   VF(A, R8_SNORM, SNORM, 8, 0),
   VF(A, R8_SINT, SINT, 8, 0),
   VF(A, R8_UINT, UINT, 8, 0),

   VF(A, R32G32B32A32_SSCALED, SSCALED, 32_32_32_32, 0),
   VF(A, R32G32B32A32_USCALED, USCALED, 32_32_32_32, 0),
   VF(A, R32G32B32_FLOAT, FLOAT, 32_32_32, 0),
   VF(A, R32G32B32_UNORM, UNORM, 32_32_32, 0),
   VF(A, R32G32B32_SNORM, SNORM, 32_32_32, 0),
   VF(A, R32G32B32_SINT, SINT, 32_32_32, 0),
   VF(A, R32G32B32_UINT, UINT, 32_32_32, 0),
   VF(A, R32G32B32_SSCALED, SSCALED, 32_32_32, 0),
   VF(A, R32G32B32_USCALED, USCALED, 32_32_32, 0),
   VF(A, R32G32_SSCALED, SSCALED, 32_32, 0),
   VF(A, R32G32_USCALED, USCALED, 32_32, 0),
   VF(A, R32_SSCALED, SSCALED, 32, 0),
   VF(A, R32_USCALED, USCALED, 32, 0),

   VF(A, R16G16B16A16_SSCALED, SSCALED, 16_16_16_16, 0),
   VF(A, R16G16B16A16_USCALED, USCALED, 16_16_16_16, 0),
   VF(A, R16G16B16_FLOAT, FLOAT, 16_16_16, 0),
   VF(A, R16G16B16_UNORM, UNORM, 16_16_16, 0),
   VF(A, R16G16B16_SNORM, SNORM, 16_16_16, 0),
   VF(A, R16G16B16_SINT, SINT, 16_16_16, 0),
   VF(A, R16G16B16_UINT, UINT, 16_16_16, 0),
   VF(A, R16G16B16_SSCALED, SSCALED, 16_16_16, 0),
   VF(A, R16G16B16_USCALED, USCALED, 16_16_16, 0),
   VF(A, R16G16_SSCALED, SSCALED, 16_16, 0),
   VF(A, R16G16_USCALED, USCALED, 16_16, 0),
   VF(A, R16_SSCALED, SSCALED, 16, 0),
   VF(A, R16_USCALED, USCALED, 16, 0),

   VF(A, R10G10B10A2_USCALED, USCALED, 10_10_10_2, 0),
   VF(A, R10G10B10A2_SSCALED, SSCALED, 10_10_10_2, 0),
   VF(A, B10G10R10A2_USCALED, USCALED, 10_10_10_2, 1),
   VF(A, B10G10R10A2_SSCALED, SSCALED, 10_10_10_2, 1),

   VF(A, R8G8B8A8_SSCALED, SSCALED, 8_8_8_8, 0),
   VF(A, R8G8B8A8_USCALED, USCALED, 8_8_8_8, 0),
   VF(A, R8G8B8_UNORM, UNORM, 8_8_8, 0),
   VF(A, R8G8B8_SNORM, SNORM, 8_8_8, 0),
   VF(A, R8G8B8_SINT, SINT, 8_8_8, 0),
   VF(A, R8G8B8_UINT, UINT, 8_8_8, 0),
   VF(A, R8G8B8_SSCALED, SSCALED, 8_8_8, 0),
   VF(A, R8G8B8_USCALED, USCALED, 8_8_8, 0),
   VF(A, R8G8_SSCALED, SSCALED, 8_8, 0),
   VF(A, R8G8_USCALED, USCALED, 8_8, 0),
   VF(A, R8_SSCALED, SSCALED, 8, 0),
   VF(A, R8_USCALED, USCALED, 8, 0),

   /* FIXED types: not supported natively, converted on VBO push */

   VF(x, R32G32B32A32_FIXED, xx, xx, xx),
   VF(x, R32G32B32_FIXED, xx, xx, xx),
   VF(x, R32G32_FIXED, xx, xx, xx),
   VF(x, R32_FIXED, xx, xx, xx),

   VF(x, R64G64B64A64_FLOAT, xx, xx, xx),
   VF(x, R64G64B64_FLOAT, xx, xx, xx),
   VF(x, R64G64_FLOAT, xx, xx, xx),
   VF(x, R64_FLOAT, xx, xx, xx),
};
