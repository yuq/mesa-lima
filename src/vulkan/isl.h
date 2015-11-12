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

/**
 * @file
 * @brief Intel Surface Layout
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hardware enumeration SURFACE_FORMAT.
 *
 * For the official list, see Broadwell PRM: Volume 2b: Command Reference:
 * Enumerations: SURFACE_FORMAT.
 */
enum isl_format {
   ISL_FORMAT_R32G32B32A32_FLOAT =                               0,
   ISL_FORMAT_R32G32B32A32_SINT =                                1,
   ISL_FORMAT_R32G32B32A32_UINT =                                2,
   ISL_FORMAT_R32G32B32A32_UNORM =                               3,
   ISL_FORMAT_R32G32B32A32_SNORM =                               4,
   ISL_FORMAT_R64G64_FLOAT =                                     5,
   ISL_FORMAT_R32G32B32X32_FLOAT =                               6,
   ISL_FORMAT_R32G32B32A32_SSCALED =                             7,
   ISL_FORMAT_R32G32B32A32_USCALED =                             8,
   ISL_FORMAT_R32G32B32A32_SFIXED =                             32,
   ISL_FORMAT_R64G64_PASSTHRU =                                 33,
   ISL_FORMAT_R32G32B32_FLOAT =                                 64,
   ISL_FORMAT_R32G32B32_SINT =                                  65,
   ISL_FORMAT_R32G32B32_UINT =                                  66,
   ISL_FORMAT_R32G32B32_UNORM =                                 67,
   ISL_FORMAT_R32G32B32_SNORM =                                 68,
   ISL_FORMAT_R32G32B32_SSCALED =                               69,
   ISL_FORMAT_R32G32B32_USCALED =                               70,
   ISL_FORMAT_R32G32B32_SFIXED =                                80,
   ISL_FORMAT_R16G16B16A16_UNORM =                             128,
   ISL_FORMAT_R16G16B16A16_SNORM =                             129,
   ISL_FORMAT_R16G16B16A16_SINT =                              130,
   ISL_FORMAT_R16G16B16A16_UINT =                              131,
   ISL_FORMAT_R16G16B16A16_FLOAT =                             132,
   ISL_FORMAT_R32G32_FLOAT =                                   133,
   ISL_FORMAT_R32G32_SINT =                                    134,
   ISL_FORMAT_R32G32_UINT =                                    135,
   ISL_FORMAT_R32_FLOAT_X8X24_TYPELESS =                       136,
   ISL_FORMAT_X32_TYPELESS_G8X24_UINT =                        137,
   ISL_FORMAT_L32A32_FLOAT =                                   138,
   ISL_FORMAT_R32G32_UNORM =                                   139,
   ISL_FORMAT_R32G32_SNORM =                                   140,
   ISL_FORMAT_R64_FLOAT =                                      141,
   ISL_FORMAT_R16G16B16X16_UNORM =                             142,
   ISL_FORMAT_R16G16B16X16_FLOAT =                             143,
   ISL_FORMAT_A32X32_FLOAT =                                   144,
   ISL_FORMAT_L32X32_FLOAT =                                   145,
   ISL_FORMAT_I32X32_FLOAT =                                   146,
   ISL_FORMAT_R16G16B16A16_SSCALED =                           147,
   ISL_FORMAT_R16G16B16A16_USCALED =                           148,
   ISL_FORMAT_R32G32_SSCALED =                                 149,
   ISL_FORMAT_R32G32_USCALED =                                 150,
   ISL_FORMAT_R32G32_SFIXED =                                  160,
   ISL_FORMAT_R64_PASSTHRU =                                   161,
   ISL_FORMAT_B8G8R8A8_UNORM =                                 192,
   ISL_FORMAT_B8G8R8A8_UNORM_SRGB =                            193,
   ISL_FORMAT_R10G10B10A2_UNORM =                              194,
   ISL_FORMAT_R10G10B10A2_UNORM_SRGB =                         195,
   ISL_FORMAT_R10G10B10A2_UINT =                               196,
   ISL_FORMAT_R10G10B10_SNORM_A2_UNORM =                       197,
   ISL_FORMAT_R8G8B8A8_UNORM =                                 199,
   ISL_FORMAT_R8G8B8A8_UNORM_SRGB =                            200,
   ISL_FORMAT_R8G8B8A8_SNORM =                                 201,
   ISL_FORMAT_R8G8B8A8_SINT =                                  202,
   ISL_FORMAT_R8G8B8A8_UINT =                                  203,
   ISL_FORMAT_R16G16_UNORM =                                   204,
   ISL_FORMAT_R16G16_SNORM =                                   205,
   ISL_FORMAT_R16G16_SINT =                                    206,
   ISL_FORMAT_R16G16_UINT =                                    207,
   ISL_FORMAT_R16G16_FLOAT =                                   208,
   ISL_FORMAT_B10G10R10A2_UNORM =                              209,
   ISL_FORMAT_B10G10R10A2_UNORM_SRGB =                         210,
   ISL_FORMAT_R11G11B10_FLOAT =                                211,
   ISL_FORMAT_R32_SINT =                                       214,
   ISL_FORMAT_R32_UINT =                                       215,
   ISL_FORMAT_R32_FLOAT =                                      216,
   ISL_FORMAT_R24_UNORM_X8_TYPELESS =                          217,
   ISL_FORMAT_X24_TYPELESS_G8_UINT =                           218,
   ISL_FORMAT_L32_UNORM =                                      221,
   ISL_FORMAT_A32_UNORM =                                      222,
   ISL_FORMAT_L16A16_UNORM =                                   223,
   ISL_FORMAT_I24X8_UNORM =                                    224,
   ISL_FORMAT_L24X8_UNORM =                                    225,
   ISL_FORMAT_A24X8_UNORM =                                    226,
   ISL_FORMAT_I32_FLOAT =                                      227,
   ISL_FORMAT_L32_FLOAT =                                      228,
   ISL_FORMAT_A32_FLOAT =                                      229,
   ISL_FORMAT_X8B8_UNORM_G8R8_SNORM =                          230,
   ISL_FORMAT_A8X8_UNORM_G8R8_SNORM =                          231,
   ISL_FORMAT_B8X8_UNORM_G8R8_SNORM =                          232,
   ISL_FORMAT_B8G8R8X8_UNORM =                                 233,
   ISL_FORMAT_B8G8R8X8_UNORM_SRGB =                            234,
   ISL_FORMAT_R8G8B8X8_UNORM =                                 235,
   ISL_FORMAT_R8G8B8X8_UNORM_SRGB =                            236,
   ISL_FORMAT_R9G9B9E5_SHAREDEXP =                             237,
   ISL_FORMAT_B10G10R10X2_UNORM =                              238,
   ISL_FORMAT_L16A16_FLOAT =                                   240,
   ISL_FORMAT_R32_UNORM =                                      241,
   ISL_FORMAT_R32_SNORM =                                      242,
   ISL_FORMAT_R10G10B10X2_USCALED =                            243,
   ISL_FORMAT_R8G8B8A8_SSCALED =                               244,
   ISL_FORMAT_R8G8B8A8_USCALED =                               245,
   ISL_FORMAT_R16G16_SSCALED =                                 246,
   ISL_FORMAT_R16G16_USCALED =                                 247,
   ISL_FORMAT_R32_SSCALED =                                    248,
   ISL_FORMAT_R32_USCALED =                                    249,
   ISL_FORMAT_B5G6R5_UNORM =                                   256,
   ISL_FORMAT_B5G6R5_UNORM_SRGB =                              257,
   ISL_FORMAT_B5G5R5A1_UNORM =                                 258,
   ISL_FORMAT_B5G5R5A1_UNORM_SRGB =                            259,
   ISL_FORMAT_B4G4R4A4_UNORM =                                 260,
   ISL_FORMAT_B4G4R4A4_UNORM_SRGB =                            261,
   ISL_FORMAT_R8G8_UNORM =                                     262,
   ISL_FORMAT_R8G8_SNORM =                                     263,
   ISL_FORMAT_R8G8_SINT =                                      264,
   ISL_FORMAT_R8G8_UINT =                                      265,
   ISL_FORMAT_R16_UNORM =                                      266,
   ISL_FORMAT_R16_SNORM =                                      267,
   ISL_FORMAT_R16_SINT =                                       268,
   ISL_FORMAT_R16_UINT =                                       269,
   ISL_FORMAT_R16_FLOAT =                                      270,
   ISL_FORMAT_A8P8_UNORM_PALETTE0 =                            271,
   ISL_FORMAT_A8P8_UNORM_PALETTE1 =                            272,
   ISL_FORMAT_I16_UNORM =                                      273,
   ISL_FORMAT_L16_UNORM =                                      274,
   ISL_FORMAT_A16_UNORM =                                      275,
   ISL_FORMAT_L8A8_UNORM =                                     276,
   ISL_FORMAT_I16_FLOAT =                                      277,
   ISL_FORMAT_L16_FLOAT =                                      278,
   ISL_FORMAT_A16_FLOAT =                                      279,
   ISL_FORMAT_L8A8_UNORM_SRGB =                                280,
   ISL_FORMAT_R5G5_SNORM_B6_UNORM =                            281,
   ISL_FORMAT_B5G5R5X1_UNORM =                                 282,
   ISL_FORMAT_B5G5R5X1_UNORM_SRGB =                            283,
   ISL_FORMAT_R8G8_SSCALED =                                   284,
   ISL_FORMAT_R8G8_USCALED =                                   285,
   ISL_FORMAT_R16_SSCALED =                                    286,
   ISL_FORMAT_R16_USCALED =                                    287,
   ISL_FORMAT_P8A8_UNORM_PALETTE0 =                            290,
   ISL_FORMAT_P8A8_UNORM_PALETTE1 =                            291,
   ISL_FORMAT_A1B5G5R5_UNORM =                                 292,
   ISL_FORMAT_A4B4G4R4_UNORM =                                 293,
   ISL_FORMAT_L8A8_UINT =                                      294,
   ISL_FORMAT_L8A8_SINT =                                      295,
   ISL_FORMAT_R8_UNORM =                                       320,
   ISL_FORMAT_R8_SNORM =                                       321,
   ISL_FORMAT_R8_SINT =                                        322,
   ISL_FORMAT_R8_UINT =                                        323,
   ISL_FORMAT_A8_UNORM =                                       324,
   ISL_FORMAT_I8_UNORM =                                       325,
   ISL_FORMAT_L8_UNORM =                                       326,
   ISL_FORMAT_P4A4_UNORM_PALETTE0 =                            327,
   ISL_FORMAT_A4P4_UNORM_PALETTE0 =                            328,
   ISL_FORMAT_R8_SSCALED =                                     329,
   ISL_FORMAT_R8_USCALED =                                     330,
   ISL_FORMAT_P8_UNORM_PALETTE0 =                              331,
   ISL_FORMAT_L8_UNORM_SRGB =                                  332,
   ISL_FORMAT_P8_UNORM_PALETTE1 =                              333,
   ISL_FORMAT_P4A4_UNORM_PALETTE1 =                            334,
   ISL_FORMAT_A4P4_UNORM_PALETTE1 =                            335,
   ISL_FORMAT_Y8_UNORM =                                       336,
   ISL_FORMAT_L8_UINT =                                        338,
   ISL_FORMAT_L8_SINT =                                        339,
   ISL_FORMAT_I8_UINT =                                        340,
   ISL_FORMAT_I8_SINT =                                        341,
   ISL_FORMAT_DXT1_RGB_SRGB =                                  384,
   ISL_FORMAT_R1_UNORM =                                       385,
   ISL_FORMAT_YCRCB_NORMAL =                                   386,
   ISL_FORMAT_YCRCB_SWAPUVY =                                  387,
   ISL_FORMAT_P2_UNORM_PALETTE0 =                              388,
   ISL_FORMAT_P2_UNORM_PALETTE1 =                              389,
   ISL_FORMAT_BC1_UNORM =                                      390,
   ISL_FORMAT_BC2_UNORM =                                      391,
   ISL_FORMAT_BC3_UNORM =                                      392,
   ISL_FORMAT_BC4_UNORM =                                      393,
   ISL_FORMAT_BC5_UNORM =                                      394,
   ISL_FORMAT_BC1_UNORM_SRGB =                                 395,
   ISL_FORMAT_BC2_UNORM_SRGB =                                 396,
   ISL_FORMAT_BC3_UNORM_SRGB =                                 397,
   ISL_FORMAT_MONO8 =                                          398,
   ISL_FORMAT_YCRCB_SWAPUV =                                   399,
   ISL_FORMAT_YCRCB_SWAPY =                                    400,
   ISL_FORMAT_DXT1_RGB =                                       401,
   ISL_FORMAT_FXT1 =                                           402,
   ISL_FORMAT_R8G8B8_UNORM =                                   403,
   ISL_FORMAT_R8G8B8_SNORM =                                   404,
   ISL_FORMAT_R8G8B8_SSCALED =                                 405,
   ISL_FORMAT_R8G8B8_USCALED =                                 406,
   ISL_FORMAT_R64G64B64A64_FLOAT =                             407,
   ISL_FORMAT_R64G64B64_FLOAT =                                408,
   ISL_FORMAT_BC4_SNORM =                                      409,
   ISL_FORMAT_BC5_SNORM =                                      410,
   ISL_FORMAT_R16G16B16_FLOAT =                                411,
   ISL_FORMAT_R16G16B16_UNORM =                                412,
   ISL_FORMAT_R16G16B16_SNORM =                                413,
   ISL_FORMAT_R16G16B16_SSCALED =                              414,
   ISL_FORMAT_R16G16B16_USCALED =                              415,
   ISL_FORMAT_BC6H_SF16 =                                      417,
   ISL_FORMAT_BC7_UNORM =                                      418,
   ISL_FORMAT_BC7_UNORM_SRGB =                                 419,
   ISL_FORMAT_BC6H_UF16 =                                      420,
   ISL_FORMAT_PLANAR_420_8 =                                   421,
   ISL_FORMAT_R8G8B8_UNORM_SRGB =                              424,
   ISL_FORMAT_ETC1_RGB8 =                                      425,
   ISL_FORMAT_ETC2_RGB8 =                                      426,
   ISL_FORMAT_EAC_R11 =                                        427,
   ISL_FORMAT_EAC_RG11 =                                       428,
   ISL_FORMAT_EAC_SIGNED_R11 =                                 429,
   ISL_FORMAT_EAC_SIGNED_RG11 =                                430,
   ISL_FORMAT_ETC2_SRGB8 =                                     431,
   ISL_FORMAT_R16G16B16_UINT =                                 432,
   ISL_FORMAT_R16G16B16_SINT =                                 433,
   ISL_FORMAT_R32_SFIXED =                                     434,
   ISL_FORMAT_R10G10B10A2_SNORM =                              435,
   ISL_FORMAT_R10G10B10A2_USCALED =                            436,
   ISL_FORMAT_R10G10B10A2_SSCALED =                            437,
   ISL_FORMAT_R10G10B10A2_SINT =                               438,
   ISL_FORMAT_B10G10R10A2_SNORM =                              439,
   ISL_FORMAT_B10G10R10A2_USCALED =                            440,
   ISL_FORMAT_B10G10R10A2_SSCALED =                            441,
   ISL_FORMAT_B10G10R10A2_UINT =                               442,
   ISL_FORMAT_B10G10R10A2_SINT =                               443,
   ISL_FORMAT_R64G64B64A64_PASSTHRU =                          444,
   ISL_FORMAT_R64G64B64_PASSTHRU =                             445,
   ISL_FORMAT_ETC2_RGB8_PTA =                                  448,
   ISL_FORMAT_ETC2_SRGB8_PTA =                                 449,
   ISL_FORMAT_ETC2_EAC_RGBA8 =                                 450,
   ISL_FORMAT_ETC2_EAC_SRGB8_A8 =                              451,
   ISL_FORMAT_R8G8B8_UINT =                                    456,
   ISL_FORMAT_R8G8B8_SINT =                                    457,
   ISL_FORMAT_RAW =                                            511,

   /* Hardware doesn't understand this out-of-band value */
   ISL_FORMAT_UNSUPPORTED =                             UINT16_MAX,
};

enum isl_base_type {
   ISL_VOID,
   ISL_RAW,
   ISL_UNORM,
   ISL_SNORM,
   ISL_UFLOAT,
   ISL_SFLOAT,
   ISL_UFIXED,
   ISL_SFIXED,
   ISL_UINT,
   ISL_SINT,
   ISL_USCALED,
   ISL_SSCALED,
};

enum isl_colorspace {
   ISL_COLORSPACE_NONE = 0,
   ISL_COLORSPACE_LINEAR,
   ISL_COLORSPACE_SRGB,
   ISL_COLORSPACE_YUV,
};

/**
 * Texture compression mode
 */
enum isl_txc {
   ISL_TXC_NONE = 0,
   ISL_TXC_DXT1,
   ISL_TXC_DXT3,
   ISL_TXC_DXT5,
   ISL_TXC_FXT1,
   ISL_TXC_RGTC1,
   ISL_TXC_RGTC2,
   ISL_TXC_BPTC,
   ISL_TXC_ETC1,
   ISL_TXC_ETC2,
};

struct isl_channel_layout {
   enum isl_base_type type;
   uint8_t bits; /**< Size in bits */
};

struct isl_format_layout {
   enum isl_format format;

   uint16_t bpb; /**< Bits per block */
   uint8_t bs; /**< Block size, in bytes, rounded towards 0 */
   uint8_t bw; /**< Block width, in pixels */
   uint8_t bh; /**< Block height, in pixels */
   uint8_t bd; /**< Block depth, in pixels */

   struct {
      struct isl_channel_layout r; /**< Red channel */
      struct isl_channel_layout g; /**< Green channel */
      struct isl_channel_layout b; /**< Blue channel */
      struct isl_channel_layout a; /**< Alpha channel */
      struct isl_channel_layout l; /**< Luminance channel */
      struct isl_channel_layout i; /**< Intensity channel */
      struct isl_channel_layout p; /**< Palette channel */
   } channels;

   enum isl_colorspace colorspace;
   enum isl_txc txc;
};

extern const struct isl_format_layout isl_format_layouts[];

#ifdef __cplusplus
}
#endif
