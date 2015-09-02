/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 * Copyright (C) 2009  VMware, Inc.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


/**
 * \file
 * \brief Extension handling
 */


#include "glheader.h"
#include "imports.h"
#include "context.h"
#include "extensions.h"
#include "macros.h"
#include "mtypes.h"

struct gl_extensions _mesa_extension_override_enables;
struct gl_extensions _mesa_extension_override_disables;
static char *extra_extensions = NULL;
static char *cant_disable_extensions = NULL;

enum {
   DISABLE = 0,
   GLL = 1 << API_OPENGL_COMPAT,       /* GL Legacy / Compatibility */
   GLC = 1 << API_OPENGL_CORE,  /* GL Core */
   GL  = (1 << API_OPENGL_COMPAT) | (1 << API_OPENGL_CORE),
   ES1 = 1 << API_OPENGLES,
   ES2 = 1 << API_OPENGLES2,
   ES3 = 1 << (API_OPENGL_LAST + 1),
   ES31 = 1 << (API_OPENGL_LAST + 2),
};

/**
 * \brief An element of the \c extension_table.
 */
struct extension {
   /** Name of extension, such as "GL_ARB_depth_clamp". */
   const char *name;

   /** Offset (in bytes) of the corresponding member in struct gl_extensions. */
   size_t offset;

   /** Set of API's in which the extension exists, as a bitset. */
   uint8_t api_set;

   /** Year the extension was proposed or approved.  Used to sort the 
    * extension string chronologically. */
   uint16_t year;
};


/**
 * Given a member \c x of struct gl_extensions, return offset of
 * \c x in bytes.
 */
#define o(x) offsetof(struct gl_extensions, x)


/**
 * \brief Table of supported OpenGL extensions for all API's.
 */
static const struct extension extension_table[] = {
#define EXT(name_str, driver_cap, api_flags, yyyy) \
        { .name = "GL_" #name_str, .offset = o(driver_cap), .api_set = api_flags, .year = yyyy},
   EXT(ARB_ES2_compatibility                   , ARB_ES2_compatibility                  , GL                   , 2009)
   EXT(ARB_ES3_compatibility                   , ARB_ES3_compatibility                  , GL                   , 2012)
   EXT(ARB_arrays_of_arrays                    , ARB_arrays_of_arrays                   , GL                   , 2012)
   EXT(ARB_base_instance                       , ARB_base_instance                      , GL                   , 2011)
   EXT(ARB_blend_func_extended                 , ARB_blend_func_extended                , GL                   , 2009)
   EXT(ARB_buffer_storage                      , ARB_buffer_storage                     , GL                   , 2013)
   EXT(ARB_clear_buffer_object                 , dummy_true                             , GL                   , 2012)
   EXT(ARB_clear_texture                       , ARB_clear_texture                      , GL                   , 2013)
   EXT(ARB_clip_control                        , ARB_clip_control                       , GL                   , 2014)
   EXT(ARB_color_buffer_float                  , ARB_color_buffer_float                 , GL                   , 2004)
   EXT(ARB_compressed_texture_pixel_storage    , dummy_true                             , GL                   , 2011)
   EXT(ARB_compute_shader                      , ARB_compute_shader                     , GL                   , 2012)
   EXT(ARB_conditional_render_inverted         , ARB_conditional_render_inverted        , GL                   , 2014)
   EXT(ARB_copy_buffer                         , dummy_true                             , GL                   , 2008)
   EXT(ARB_copy_image                          , ARB_copy_image                         , GL                   , 2012)
   EXT(ARB_conservative_depth                  , ARB_conservative_depth                 , GL                   , 2011)
   EXT(ARB_debug_output                        , dummy_true                             , GL                   , 2009)
   EXT(ARB_depth_buffer_float                  , ARB_depth_buffer_float                 , GL                   , 2008)
   EXT(ARB_depth_clamp                         , ARB_depth_clamp                        , GL                   , 2003)
   EXT(ARB_depth_texture                       , ARB_depth_texture                      , GLL                  , 2001)
   EXT(ARB_derivative_control                  , ARB_derivative_control                 , GL                   , 2014)
   EXT(ARB_direct_state_access                 , dummy_true                             , GLC                  , 2014)
   EXT(ARB_draw_buffers                        , dummy_true                             , GL                   , 2002)
   EXT(ARB_draw_buffers_blend                  , ARB_draw_buffers_blend                 , GL                   , 2009)
   EXT(ARB_draw_elements_base_vertex           , ARB_draw_elements_base_vertex          , GL                   , 2009)
   EXT(ARB_draw_indirect                       , ARB_draw_indirect                      , GLC                  , 2010)
   EXT(ARB_draw_instanced                      , ARB_draw_instanced                     , GL                   , 2008)
   EXT(ARB_enhanced_layouts                    , ARB_enhanced_layouts                   , GLC                  , 2013)
   EXT(ARB_explicit_attrib_location            , ARB_explicit_attrib_location           , GL                   , 2009)
   EXT(ARB_explicit_uniform_location           , ARB_explicit_uniform_location          , GL                   , 2012)
   EXT(ARB_fragment_coord_conventions          , ARB_fragment_coord_conventions         , GL                   , 2009)
   EXT(ARB_fragment_layer_viewport             , ARB_fragment_layer_viewport            , GLC                  , 2012)
   EXT(ARB_fragment_program                    , ARB_fragment_program                   , GLL                  , 2002)
   EXT(ARB_fragment_program_shadow             , ARB_fragment_program_shadow            , GLL                  , 2003)
   EXT(ARB_fragment_shader                     , ARB_fragment_shader                    , GL                   , 2002)
   EXT(ARB_framebuffer_no_attachments          , ARB_framebuffer_no_attachments         , GL                   , 2012)
   EXT(ARB_framebuffer_object                  , ARB_framebuffer_object                 , GL                   , 2005)
   EXT(ARB_framebuffer_sRGB                    , EXT_framebuffer_sRGB                   , GL                   , 1998)
   EXT(ARB_get_program_binary                  , dummy_true                             , GL                   , 2010)
   EXT(ARB_get_texture_sub_image               , dummy_true                             , GL                   , 2014)
   EXT(ARB_gpu_shader5                         , ARB_gpu_shader5                        , GLC                  , 2010)
   EXT(ARB_gpu_shader_fp64                     , ARB_gpu_shader_fp64                    , GLC                  , 2010)
   EXT(ARB_half_float_pixel                    , dummy_true                             , GL                   , 2003)
   EXT(ARB_half_float_vertex                   , ARB_half_float_vertex                  , GL                   , 2008)
   EXT(ARB_instanced_arrays                    , ARB_instanced_arrays                   , GL                   , 2008)
   EXT(ARB_internalformat_query                , ARB_internalformat_query               , GL                   , 2011)
   EXT(ARB_invalidate_subdata                  , dummy_true                             , GL                   , 2012)
   EXT(ARB_map_buffer_alignment                , dummy_true                             , GL                   , 2011)
   EXT(ARB_map_buffer_range                    , ARB_map_buffer_range                   , GL                   , 2008)
   EXT(ARB_multi_bind                          , dummy_true                             , GL                   , 2013)
   EXT(ARB_multi_draw_indirect                 , ARB_draw_indirect                      , GLC                  , 2012)
   EXT(ARB_multisample                         , dummy_true                             , GLL                  , 1994)
   EXT(ARB_multitexture                        , dummy_true                             , GLL                  , 1998)
   EXT(ARB_occlusion_query2                    , ARB_occlusion_query2                   , GL                   , 2003)
   EXT(ARB_occlusion_query                     , ARB_occlusion_query                    , GLL                  , 2001)
   EXT(ARB_pipeline_statistics_query           , ARB_pipeline_statistics_query          , GL                   , 2014)
   EXT(ARB_pixel_buffer_object                 , EXT_pixel_buffer_object                , GL                   , 2004)
   EXT(ARB_point_parameters                    , EXT_point_parameters                   , GLL                  , 1997)
   EXT(ARB_point_sprite                        , ARB_point_sprite                       , GL                   , 2003)
   EXT(ARB_program_interface_query             , dummy_true                             , GL                   , 2012)
   EXT(ARB_provoking_vertex                    , EXT_provoking_vertex                   , GL                   , 2009)
   EXT(ARB_robustness                          , dummy_true                             , GL                   , 2010)
   EXT(ARB_sample_shading                      , ARB_sample_shading                     , GL                   , 2009)
   EXT(ARB_sampler_objects                     , dummy_true                             , GL                   , 2009)
   EXT(ARB_seamless_cube_map                   , ARB_seamless_cube_map                  , GL                   , 2009)
   EXT(ARB_seamless_cubemap_per_texture        , AMD_seamless_cubemap_per_texture       , GL                   , 2013)
   EXT(ARB_separate_shader_objects             , dummy_true                             , GL                   , 2010)
   EXT(ARB_shader_atomic_counters              , ARB_shader_atomic_counters             , GL                   , 2011)
   EXT(ARB_shader_bit_encoding                 , ARB_shader_bit_encoding                , GL                   , 2010)
   EXT(ARB_shader_clock                        , ARB_shader_clock                       , GL                   , 2015)
   EXT(ARB_shader_image_load_store             , ARB_shader_image_load_store            , GL                   , 2011)
   EXT(ARB_shader_image_size                   , ARB_shader_image_size                  , GL                   , 2012)
   EXT(ARB_shader_objects                      , dummy_true                             , GL                   , 2002)
   EXT(ARB_shader_precision                    , ARB_shader_precision                   , GL                   , 2010)
   EXT(ARB_shader_stencil_export               , ARB_shader_stencil_export              , GL                   , 2009)
   EXT(ARB_shader_storage_buffer_object        , ARB_shader_storage_buffer_object       , GL                   , 2012)
   EXT(ARB_shader_subroutine                   , ARB_shader_subroutine                  , GLC                  , 2010)
   EXT(ARB_shader_texture_image_samples        , ARB_shader_texture_image_samples       , GL                   , 2014)
   EXT(ARB_shader_texture_lod                  , ARB_shader_texture_lod                 , GL                   , 2009)
   EXT(ARB_shading_language_100                , dummy_true                             , GLL                  , 2003)
   EXT(ARB_shading_language_packing            , ARB_shading_language_packing           , GL                   , 2011)
   EXT(ARB_shading_language_420pack            , ARB_shading_language_420pack           , GL                   , 2011)
   EXT(ARB_shadow                              , ARB_shadow                             , GLL                  , 2001)
   EXT(ARB_stencil_texturing                   , ARB_stencil_texturing                  , GL                   , 2012)
   EXT(ARB_sync                                , ARB_sync                               , GL                   , 2003)
   EXT(ARB_texture_barrier                     , NV_texture_barrier                     , GL                   , 2014)
   EXT(ARB_tessellation_shader                 , ARB_tessellation_shader                , GLC                  , 2009)
   EXT(ARB_texture_border_clamp                , ARB_texture_border_clamp               , GLL                  , 2000)
   EXT(ARB_texture_buffer_object               , ARB_texture_buffer_object              , GLC                  , 2008)
   EXT(ARB_texture_buffer_object_rgb32         , ARB_texture_buffer_object_rgb32        , GLC                  , 2009)
   EXT(ARB_texture_buffer_range                , ARB_texture_buffer_range               , GLC                  , 2012)
   EXT(ARB_texture_compression                 , dummy_true                             , GLL                  , 2000)
   EXT(ARB_texture_compression_bptc            , ARB_texture_compression_bptc           , GL                   , 2010)
   EXT(ARB_texture_compression_rgtc            , ARB_texture_compression_rgtc           , GL                   , 2004)
   EXT(ARB_texture_cube_map                    , ARB_texture_cube_map                   , GLL                  , 1999)
   EXT(ARB_texture_cube_map_array              , ARB_texture_cube_map_array             , GL                   , 2009)
   EXT(ARB_texture_env_add                     , dummy_true                             , GLL                  , 1999)
   EXT(ARB_texture_env_combine                 , ARB_texture_env_combine                , GLL                  , 2001)
   EXT(ARB_texture_env_crossbar                , ARB_texture_env_crossbar               , GLL                  , 2001)
   EXT(ARB_texture_env_dot3                    , ARB_texture_env_dot3                   , GLL                  , 2001)
   EXT(ARB_texture_float                       , ARB_texture_float                      , GL                   , 2004)
   EXT(ARB_texture_gather                      , ARB_texture_gather                     , GL                   , 2009)
   EXT(ARB_texture_mirrored_repeat             , dummy_true                             , GLL                  , 2001)
   EXT(ARB_texture_mirror_clamp_to_edge        , ARB_texture_mirror_clamp_to_edge       , GL                   , 2013)
   EXT(ARB_texture_multisample                 , ARB_texture_multisample                , GL                   , 2009)
   EXT(ARB_texture_non_power_of_two            , ARB_texture_non_power_of_two           , GL                   , 2003)
   EXT(ARB_texture_query_levels                , ARB_texture_query_levels               , GL                   , 2012)
   EXT(ARB_texture_query_lod                   , ARB_texture_query_lod                  , GL                   , 2009)
   EXT(ARB_texture_rectangle                   , NV_texture_rectangle                   , GL                   , 2004)
   EXT(ARB_texture_rgb10_a2ui                  , ARB_texture_rgb10_a2ui                 , GL                   , 2009)
   EXT(ARB_texture_rg                          , ARB_texture_rg                         , GL                   , 2008)
   EXT(ARB_texture_stencil8                    , ARB_texture_stencil8                   , GL                   , 2013)
   EXT(ARB_texture_storage                     , dummy_true                             , GL                   , 2011)
   EXT(ARB_texture_storage_multisample         , ARB_texture_multisample                , GL                   , 2012)
   EXT(ARB_texture_view                        , ARB_texture_view                       , GL                   , 2012)
   EXT(ARB_texture_swizzle                     , EXT_texture_swizzle                    , GL                   , 2008)
   EXT(ARB_timer_query                         , ARB_timer_query                        , GL                   , 2010)
   EXT(ARB_transform_feedback2                 , ARB_transform_feedback2                , GL                   , 2010)
   EXT(ARB_transform_feedback3                 , ARB_transform_feedback3                , GL                   , 2010)
   EXT(ARB_transform_feedback_instanced        , ARB_transform_feedback_instanced       , GL                   , 2011)
   EXT(ARB_transpose_matrix                    , dummy_true                             , GLL                  , 1999)
   EXT(ARB_uniform_buffer_object               , ARB_uniform_buffer_object              , GL                   , 2009)
   EXT(ARB_vertex_array_bgra                   , EXT_vertex_array_bgra                  , GL                   , 2008)
   EXT(ARB_vertex_array_object                 , dummy_true                             , GL                   , 2006)
   EXT(ARB_vertex_attrib_binding               , dummy_true                             , GL                   , 2012)
   EXT(ARB_vertex_buffer_object                , dummy_true                             , GLL                  , 2003)
   EXT(ARB_vertex_program                      , ARB_vertex_program                     , GLL                  , 2002)
   EXT(ARB_vertex_shader                       , ARB_vertex_shader                      , GL                   , 2002)
   EXT(ARB_vertex_attrib_64bit                 , ARB_vertex_attrib_64bit                , GLC                  , 2010)
   EXT(ARB_vertex_type_10f_11f_11f_rev         , ARB_vertex_type_10f_11f_11f_rev        , GL                   , 2013)
   EXT(ARB_vertex_type_2_10_10_10_rev          , ARB_vertex_type_2_10_10_10_rev         , GL                   , 2009)
   EXT(ARB_viewport_array                      , ARB_viewport_array                     , GLC                  , 2010)
   EXT(ARB_window_pos                          , dummy_true                             , GLL                  , 2001)

   EXT(EXT_abgr                                , dummy_true                             , GL                   , 1995)
   EXT(EXT_bgra                                , dummy_true                             , GLL                  , 1995)
   EXT(EXT_blend_color                         , EXT_blend_color                        , GLL                  , 1995)
   EXT(EXT_blend_equation_separate             , EXT_blend_equation_separate            , GL                   , 2003)
   EXT(EXT_blend_func_separate                 , EXT_blend_func_separate                , GLL                  , 1999)
   EXT(EXT_buffer_storage                      , ARB_buffer_storage                     , ES31                 , 2015)
   EXT(EXT_discard_framebuffer                 , dummy_true                             ,       ES1 | ES2      , 2009)
   EXT(EXT_blend_minmax                        , EXT_blend_minmax                       , GLL | ES1 | ES2      , 1995)
   EXT(EXT_blend_subtract                      , dummy_true                             , GLL                  , 1995)
   EXT(EXT_compiled_vertex_array               , dummy_true                             , GLL                  , 1996)
   EXT(EXT_copy_texture                        , dummy_true                             , GLL                  , 1995)
   EXT(EXT_depth_bounds_test                   , EXT_depth_bounds_test                  , GL                   , 2002)
   EXT(EXT_draw_buffers                        , dummy_true                             ,             ES2      , 2012)
   EXT(EXT_draw_buffers2                       , EXT_draw_buffers2                      , GL                   , 2006)
   EXT(EXT_draw_elements_base_vertex           , ARB_draw_elements_base_vertex          , ES2                  , 2014)
   EXT(EXT_draw_instanced                      , ARB_draw_instanced                     , GL                   , 2006)
   EXT(EXT_draw_range_elements                 , dummy_true                             , GLL                  , 1997)
   EXT(EXT_fog_coord                           , dummy_true                             , GLL                  , 1999)
   EXT(EXT_framebuffer_blit                    , dummy_true                             , GL                   , 2005)
   EXT(EXT_framebuffer_multisample             , EXT_framebuffer_multisample            , GL                   , 2005)
   EXT(EXT_framebuffer_multisample_blit_scaled , EXT_framebuffer_multisample_blit_scaled, GL                   , 2011)
   EXT(EXT_framebuffer_object                  , dummy_true                             , GLL                  , 2000)
   EXT(EXT_framebuffer_sRGB                    , EXT_framebuffer_sRGB                   , GL                   , 1998)
   EXT(EXT_gpu_program_parameters              , EXT_gpu_program_parameters             , GLL                  , 2006)
   EXT(EXT_gpu_shader4                         , EXT_gpu_shader4                        , GL                   , 2006)
   EXT(EXT_map_buffer_range                    , ARB_map_buffer_range                   ,       ES1 | ES2      , 2012)
   EXT(EXT_multi_draw_arrays                   , dummy_true                             , GLL | ES1 | ES2      , 1999)
   EXT(EXT_packed_depth_stencil                , dummy_true                             , GL                   , 2005)
   EXT(EXT_packed_float                        , EXT_packed_float                       , GL                   , 2004)
   EXT(EXT_packed_pixels                       , dummy_true                             , GLL                  , 1997)
   EXT(EXT_pixel_buffer_object                 , EXT_pixel_buffer_object                , GL                   , 2004)
   EXT(EXT_point_parameters                    , EXT_point_parameters                   , GLL                  , 1997)
   EXT(EXT_polygon_offset                      , dummy_true                             , GLL                  , 1995)
   EXT(EXT_polygon_offset_clamp                , EXT_polygon_offset_clamp               , GL                   , 2014)
   EXT(EXT_provoking_vertex                    , EXT_provoking_vertex                   , GL                   , 2009)
   EXT(EXT_rescale_normal                      , dummy_true                             , GLL                  , 1997)
   EXT(EXT_secondary_color                     , dummy_true                             , GLL                  , 1999)
   EXT(EXT_separate_shader_objects             , dummy_true                             ,            ES2       , 2013)
   EXT(EXT_separate_specular_color             , dummy_true                             , GLL                  , 1997)
   EXT(EXT_shader_integer_mix                  , EXT_shader_integer_mix                 , GL       | ES3       , 2013)
   EXT(EXT_shadow_funcs                        , ARB_shadow                             , GLL                  , 2002)
   EXT(EXT_stencil_two_side                    , EXT_stencil_two_side                   , GLL                  , 2001)
   EXT(EXT_stencil_wrap                        , dummy_true                             , GLL                  , 2002)
   EXT(EXT_subtexture                          , dummy_true                             , GLL                  , 1995)
   EXT(EXT_texture3D                           , EXT_texture3D                          , GLL                  , 1996)
   EXT(EXT_texture_array                       , EXT_texture_array                      , GL                   , 2006)
   EXT(EXT_texture_compression_dxt1            , ANGLE_texture_compression_dxt          , GL | ES1 | ES2       , 2004)
   EXT(ANGLE_texture_compression_dxt3          , ANGLE_texture_compression_dxt          , GL | ES1 | ES2       , 2011)
   EXT(ANGLE_texture_compression_dxt5          , ANGLE_texture_compression_dxt          , GL | ES1 | ES2       , 2011)
   EXT(EXT_texture_compression_latc            , EXT_texture_compression_latc           , GLL                  , 2006)
   EXT(EXT_texture_compression_rgtc            , ARB_texture_compression_rgtc           , GL                   , 2004)
   EXT(EXT_texture_compression_s3tc            , EXT_texture_compression_s3tc           , GL                   , 2000)
   EXT(EXT_texture_cube_map                    , ARB_texture_cube_map                   , GLL                  , 2001)
   EXT(EXT_texture_edge_clamp                  , dummy_true                             , GLL                  , 1997)
   EXT(EXT_texture_env_add                     , dummy_true                             , GLL                  , 1999)
   EXT(EXT_texture_env_combine                 , dummy_true                             , GLL                  , 2000)
   EXT(EXT_texture_env_dot3                    , EXT_texture_env_dot3                   , GLL                  , 2000)
   EXT(EXT_texture_filter_anisotropic          , EXT_texture_filter_anisotropic         , GL | ES1 | ES2       , 1999)
   EXT(EXT_texture_format_BGRA8888             , dummy_true                             ,      ES1 | ES2       , 2005)
   EXT(EXT_texture_rg                          , ARB_texture_rg                         ,            ES2       , 2011)
   EXT(EXT_read_format_bgra                    , dummy_true                             ,      ES1 | ES2       , 2009)
   EXT(EXT_texture_integer                     , EXT_texture_integer                    , GL                   , 2006)
   EXT(EXT_texture_lod_bias                    , dummy_true                             , GLL | ES1            , 1999)
   EXT(EXT_texture_mirror_clamp                , EXT_texture_mirror_clamp               , GL                   , 2004)
   EXT(EXT_texture_object                      , dummy_true                             , GLL                  , 1995)
   EXT(EXT_texture                             , dummy_true                             , GLL                  , 1996)
   EXT(EXT_texture_rectangle                   , NV_texture_rectangle                   , GLL                  , 2004)
   EXT(EXT_texture_shared_exponent             , EXT_texture_shared_exponent            , GL                   , 2004)
   EXT(EXT_texture_snorm                       , EXT_texture_snorm                      , GL                   , 2009)
   EXT(EXT_texture_sRGB                        , EXT_texture_sRGB                       , GL                   , 2004)
   EXT(EXT_texture_sRGB_decode                 , EXT_texture_sRGB_decode                , GL                   , 2006)
   EXT(EXT_texture_swizzle                     , EXT_texture_swizzle                    , GL                   , 2008)
   EXT(EXT_texture_type_2_10_10_10_REV         , dummy_true                             ,            ES2       , 2008)
   EXT(EXT_timer_query                         , EXT_timer_query                        , GL                   , 2006)
   EXT(EXT_transform_feedback                  , EXT_transform_feedback                 , GL                   , 2011)
   EXT(EXT_unpack_subimage                     , dummy_true                             ,            ES2       , 2011)
   EXT(EXT_vertex_array_bgra                   , EXT_vertex_array_bgra                  , GL                   , 2008)
   EXT(EXT_vertex_array                        , dummy_true                             , GLL                  , 1995)
   EXT(EXT_color_buffer_float                  , dummy_true                             ,                  ES3 , 2013)


   EXT(OES_blend_equation_separate             , EXT_blend_equation_separate            ,      ES1             , 2009)
   EXT(OES_blend_func_separate                 , EXT_blend_func_separate                ,      ES1             , 2009)
   EXT(OES_blend_subtract                      , dummy_true                             ,      ES1             , 2009)
   EXT(OES_byte_coordinates                    , dummy_true                             ,      ES1             , 2002)
   EXT(OES_compressed_ETC1_RGB8_texture        , OES_compressed_ETC1_RGB8_texture       ,      ES1 | ES2       , 2005)
   EXT(OES_compressed_paletted_texture         , dummy_true                             ,      ES1             , 2003)
   EXT(OES_depth24                             , dummy_true                             ,      ES1 | ES2       , 2005)
   EXT(OES_depth32                             , dummy_false                            ,       DISABLE        , 2005)
   EXT(OES_depth_texture                       , ARB_depth_texture                      ,            ES2       , 2006)
   EXT(OES_depth_texture_cube_map              , OES_depth_texture_cube_map             ,            ES2       , 2012)
   EXT(OES_draw_elements_base_vertex           , ARB_draw_elements_base_vertex          , ES2                  , 2014)
   EXT(OES_draw_texture                        , OES_draw_texture                       ,      ES1             , 2004)
   EXT(OES_EGL_sync                            , dummy_true                             ,      ES1 | ES2       , 2010)
   EXT(OES_EGL_image                           , OES_EGL_image                          , GL | ES1 | ES2       , 2006) /* FIXME: Mesa expects GL_OES_EGL_image to be available in OpenGL contexts. */
   EXT(OES_EGL_image_external                  , OES_EGL_image_external                 ,      ES1 | ES2       , 2010)
   EXT(OES_element_index_uint                  , dummy_true                             ,      ES1 | ES2       , 2005)
   EXT(OES_fbo_render_mipmap                   , dummy_true                             ,      ES1 | ES2       , 2005)
   EXT(OES_fixed_point                         , dummy_true                             ,      ES1             , 2002)
   EXT(OES_framebuffer_object                  , dummy_true                             ,      ES1             , 2005)
   EXT(OES_get_program_binary                  , dummy_true                             ,            ES2       , 2008)
   EXT(OES_mapbuffer                           , dummy_true                             ,      ES1 | ES2       , 2005)
   EXT(OES_packed_depth_stencil                , dummy_true                             ,      ES1 | ES2       , 2007)
   EXT(OES_point_size_array                    , dummy_true                             ,      ES1             , 2004)
   EXT(OES_point_sprite                        , ARB_point_sprite                       ,      ES1             , 2004)
   EXT(OES_query_matrix                        , dummy_true                             ,      ES1             , 2003)
   EXT(OES_read_format                         , dummy_true                             , GL | ES1             , 2003)
   EXT(OES_rgb8_rgba8                          , dummy_true                             ,      ES1 | ES2       , 2005)
   EXT(OES_single_precision                    , dummy_true                             ,      ES1             , 2003)
   EXT(OES_standard_derivatives                , OES_standard_derivatives               ,            ES2       , 2005)
   EXT(OES_stencil1                            , dummy_false                            ,       DISABLE        , 2005)
   EXT(OES_stencil4                            , dummy_false                            ,       DISABLE        , 2005)
   EXT(OES_stencil8                            , dummy_true                             ,      ES1 | ES2       , 2005)
   EXT(OES_stencil_wrap                        , dummy_true                             ,      ES1             , 2002)
   EXT(OES_surfaceless_context                 , dummy_true                             ,      ES1 | ES2       , 2012)
   EXT(OES_texture_3D                          , EXT_texture3D                          ,            ES2       , 2005)
   EXT(OES_texture_cube_map                    , ARB_texture_cube_map                   ,      ES1             , 2007)
   EXT(OES_texture_env_crossbar                , ARB_texture_env_crossbar               ,      ES1             , 2005)
   EXT(OES_texture_float                       , OES_texture_float                      ,            ES2       , 2005)
   EXT(OES_texture_float_linear                , OES_texture_float_linear               ,            ES2       , 2005)
   EXT(OES_texture_half_float                  , OES_texture_half_float                 ,            ES2       , 2005)
   EXT(OES_texture_half_float_linear           , OES_texture_half_float_linear          ,            ES2       , 2005)
   EXT(OES_texture_mirrored_repeat             , dummy_true                             ,      ES1             , 2005)
   EXT(OES_texture_storage_multisample_2d_array, ARB_texture_multisample                ,           ES31       , 2014)
   EXT(OES_texture_npot                        , ARB_texture_non_power_of_two           ,      ES1 | ES2       , 2005)
   EXT(OES_vertex_array_object                 , dummy_true                             ,      ES1 | ES2       , 2010)


   EXT(KHR_debug                               , dummy_true                             , GL                   , 2012)
   EXT(KHR_context_flush_control               , dummy_true                             , GL       | ES2       , 2014)
   EXT(KHR_texture_compression_astc_hdr        , KHR_texture_compression_astc_hdr       , GL       | ES2       , 2012)
   EXT(KHR_texture_compression_astc_ldr        , KHR_texture_compression_astc_ldr       , GL       | ES2       , 2012)


   EXT(3DFX_texture_compression_FXT1           , TDFX_texture_compression_FXT1          , GL                   , 1999)
   EXT(AMD_conservative_depth                  , ARB_conservative_depth                 , GL                   , 2009)
   EXT(AMD_draw_buffers_blend                  , ARB_draw_buffers_blend                 , GL                   , 2009)
   EXT(AMD_performance_monitor                 , AMD_performance_monitor                , GL                   , 2007)
   EXT(AMD_pinned_memory                       , AMD_pinned_memory                      , GL                   , 2013)
   EXT(AMD_seamless_cubemap_per_texture        , AMD_seamless_cubemap_per_texture       , GL                   , 2009)
   EXT(AMD_shader_stencil_export               , ARB_shader_stencil_export              , GL                   , 2009)
   EXT(AMD_shader_trinary_minmax               , dummy_true                             , GL                   , 2012)
   EXT(AMD_vertex_shader_layer                 , AMD_vertex_shader_layer                , GLC                  , 2012)
   EXT(AMD_vertex_shader_viewport_index        , AMD_vertex_shader_viewport_index       , GLC                  , 2012)
   EXT(APPLE_object_purgeable                  , APPLE_object_purgeable                 , GL                   , 2006)
   EXT(APPLE_packed_pixels                     , dummy_true                             , GLL                  , 2002)
   EXT(APPLE_texture_max_level                 , dummy_true                             ,      ES1 | ES2       , 2009)
   EXT(APPLE_vertex_array_object               , dummy_true                             , GLL                  , 2002)
   EXT(ATI_blend_equation_separate             , EXT_blend_equation_separate            , GL                   , 2003)
   EXT(ATI_draw_buffers                        , dummy_true                             , GLL                  , 2002)
   EXT(ATI_fragment_shader                     , ATI_fragment_shader                    , GLL                  , 2001)
   EXT(ATI_separate_stencil                    , ATI_separate_stencil                   , GLL                  , 2006)
   EXT(ATI_texture_compression_3dc             , ATI_texture_compression_3dc            , GLL                  , 2004)
   EXT(ATI_texture_env_combine3                , ATI_texture_env_combine3               , GLL                  , 2002)
   EXT(ATI_texture_float                       , ARB_texture_float                      , GL                   , 2002)
   EXT(ATI_texture_mirror_once                 , ATI_texture_mirror_once                , GL                   , 2006)
   EXT(IBM_multimode_draw_arrays               , dummy_true                             , GL                   , 1998)
   EXT(IBM_rasterpos_clip                      , dummy_true                             , GLL                  , 1996)
   EXT(IBM_texture_mirrored_repeat             , dummy_true                             , GLL                  , 1998)
   EXT(INGR_blend_func_separate                , EXT_blend_func_separate                , GLL                  , 1999)
   EXT(INTEL_performance_query                 , INTEL_performance_query                , GL       | ES2       , 2013)
   EXT(MESA_pack_invert                        , MESA_pack_invert                       , GL                   , 2002)
   EXT(MESA_texture_signed_rgba                , EXT_texture_snorm                      , GL                   , 2009)
   EXT(MESA_window_pos                         , dummy_true                             , GLL                  , 2000)
   EXT(MESA_ycbcr_texture                      , MESA_ycbcr_texture                     , GL                   , 2002)
   EXT(NV_blend_square                         , dummy_true                             , GLL                  , 1999)
   EXT(NV_conditional_render                   , NV_conditional_render                  , GL                   , 2008)
   EXT(NV_depth_clamp                          , ARB_depth_clamp                        , GL                   , 2001)
   EXT(NV_draw_buffers                         , dummy_true                             ,            ES2       , 2011)
   EXT(NV_fbo_color_attachments                , dummy_true                             ,            ES2       , 2010)
   EXT(NV_fog_distance                         , NV_fog_distance                        , GLL                  , 2001)
   EXT(NV_fragment_program_option              , NV_fragment_program_option             , GLL                  , 2005)
   EXT(NV_light_max_exponent                   , dummy_true                             , GLL                  , 1999)
   EXT(NV_packed_depth_stencil                 , dummy_true                             , GL                   , 2000)
   EXT(NV_point_sprite                         , NV_point_sprite                        , GL                   , 2001)
   EXT(NV_primitive_restart                    , NV_primitive_restart                   , GLL                  , 2002)
   EXT(NV_read_buffer                          , dummy_true                             ,            ES2       , 2011)
   EXT(NV_read_depth                           , dummy_true                             ,            ES2       , 2011)
   EXT(NV_read_depth_stencil                   , dummy_true                             ,            ES2       , 2011)
   EXT(NV_read_stencil                         , dummy_true                             ,            ES2       , 2011)
   EXT(NV_texgen_reflection                    , dummy_true                             , GLL                  , 1999)
   EXT(NV_texture_barrier                      , NV_texture_barrier                     , GL                   , 2009)
   EXT(NV_texture_env_combine4                 , NV_texture_env_combine4                , GLL                  , 1999)
   EXT(NV_texture_rectangle                    , NV_texture_rectangle                   , GLL                  , 2000)
   EXT(NV_vdpau_interop                        , NV_vdpau_interop                       , GL                   , 2010)
   EXT(S3_s3tc                                 , ANGLE_texture_compression_dxt          , GL                   , 1999)
   EXT(SGIS_generate_mipmap                    , dummy_true                             , GLL                  , 1997)
   EXT(SGIS_texture_border_clamp               , ARB_texture_border_clamp               , GLL                  , 1997)
   EXT(SGIS_texture_edge_clamp                 , dummy_true                             , GLL                  , 1997)
   EXT(SGIS_texture_lod                        , dummy_true                             , GLL                  , 1997)
   EXT(SUN_multi_draw_arrays                   , dummy_true                             , GLL                  , 1999)
#undef EXT
};


/**
 * Given an extension name, lookup up the corresponding member of struct
 * gl_extensions and return that member's offset (in bytes).  If the name is
 * not found in the \c extension_table, return 0.
 *
 * \param name Name of extension.
 * \return Offset of member in struct gl_extensions.
 */
static size_t
name_to_offset(const char* name)
{
   unsigned i;

   if (name == 0)
      return 0;

   for (i = 0; i < ARRAY_SIZE(extension_table); ++i) {
      if (strcmp(name, extension_table[i].name) == 0)
	 return extension_table[i].offset;
   }

   return 0;
}

/**
 * Overrides extensions in \c ctx based on the values in
 * _mesa_extension_override_enables and _mesa_extension_override_disables.
 */
static void
override_extensions_in_context(struct gl_context *ctx)
{
   unsigned i;
   const GLboolean *enables =
      (GLboolean*) &_mesa_extension_override_enables;
   const GLboolean *disables =
      (GLboolean*) &_mesa_extension_override_disables;
   GLboolean *ctx_ext = (GLboolean*)&ctx->Extensions;

   for (i = 0; i < ARRAY_SIZE(extension_table); ++i) {
      size_t offset = extension_table[i].offset;

      assert(!enables[offset] || !disables[offset]);
      if (enables[offset]) {
         ctx_ext[offset] = 1;
      } else if (disables[offset]) {
         ctx_ext[offset] = 0;
      }
   }
}


/**
 * Enable all extensions suitable for a software-only renderer.
 * This is a convenience function used by the XMesa, OSMesa, GGI drivers, etc.
 */
void
_mesa_enable_sw_extensions(struct gl_context *ctx)
{
   ctx->Extensions.ARB_depth_clamp = GL_TRUE;
   ctx->Extensions.ARB_depth_texture = GL_TRUE;
   ctx->Extensions.ARB_draw_elements_base_vertex = GL_TRUE;
   ctx->Extensions.ARB_draw_instanced = GL_TRUE;
   ctx->Extensions.ARB_explicit_attrib_location = GL_TRUE;
   ctx->Extensions.ARB_fragment_coord_conventions = GL_TRUE;
   ctx->Extensions.ARB_fragment_program = GL_TRUE;
   ctx->Extensions.ARB_fragment_program_shadow = GL_TRUE;
   ctx->Extensions.ARB_fragment_shader = GL_TRUE;
   ctx->Extensions.ARB_framebuffer_object = GL_TRUE;
   ctx->Extensions.ARB_half_float_vertex = GL_TRUE;
   ctx->Extensions.ARB_map_buffer_range = GL_TRUE;
   ctx->Extensions.ARB_occlusion_query = GL_TRUE;
   ctx->Extensions.ARB_occlusion_query2 = GL_TRUE;
   ctx->Extensions.ARB_point_sprite = GL_TRUE;
   ctx->Extensions.ARB_shadow = GL_TRUE;
   ctx->Extensions.ARB_texture_border_clamp = GL_TRUE;
   ctx->Extensions.ARB_texture_compression_bptc = GL_TRUE;
   ctx->Extensions.ARB_texture_cube_map = GL_TRUE;
   ctx->Extensions.ARB_texture_env_combine = GL_TRUE;
   ctx->Extensions.ARB_texture_env_crossbar = GL_TRUE;
   ctx->Extensions.ARB_texture_env_dot3 = GL_TRUE;
#ifdef TEXTURE_FLOAT_ENABLED
   ctx->Extensions.ARB_texture_float = GL_TRUE;
#endif
   ctx->Extensions.ARB_texture_mirror_clamp_to_edge = GL_TRUE;
   ctx->Extensions.ARB_texture_non_power_of_two = GL_TRUE;
   ctx->Extensions.ARB_texture_rg = GL_TRUE;
   ctx->Extensions.ARB_texture_compression_rgtc = GL_TRUE;
   ctx->Extensions.ARB_vertex_program = GL_TRUE;
   ctx->Extensions.ARB_vertex_shader = GL_TRUE;
   ctx->Extensions.ARB_sync = GL_TRUE;
   ctx->Extensions.APPLE_object_purgeable = GL_TRUE;
   ctx->Extensions.ATI_fragment_shader = GL_TRUE;
   ctx->Extensions.ATI_texture_compression_3dc = GL_TRUE;
   ctx->Extensions.ATI_texture_env_combine3 = GL_TRUE;
   ctx->Extensions.ATI_texture_mirror_once = GL_TRUE;
   ctx->Extensions.ATI_separate_stencil = GL_TRUE;
   ctx->Extensions.EXT_blend_color = GL_TRUE;
   ctx->Extensions.EXT_blend_equation_separate = GL_TRUE;
   ctx->Extensions.EXT_blend_func_separate = GL_TRUE;
   ctx->Extensions.EXT_blend_minmax = GL_TRUE;
   ctx->Extensions.EXT_depth_bounds_test = GL_TRUE;
   ctx->Extensions.EXT_draw_buffers2 = GL_TRUE;
   ctx->Extensions.EXT_pixel_buffer_object = GL_TRUE;
   ctx->Extensions.EXT_point_parameters = GL_TRUE;
   ctx->Extensions.EXT_provoking_vertex = GL_TRUE;
   ctx->Extensions.EXT_stencil_two_side = GL_TRUE;
   ctx->Extensions.EXT_texture_array = GL_TRUE;
   ctx->Extensions.EXT_texture_compression_latc = GL_TRUE;
   ctx->Extensions.EXT_texture_env_dot3 = GL_TRUE;
   ctx->Extensions.EXT_texture_filter_anisotropic = GL_TRUE;
   ctx->Extensions.EXT_texture_mirror_clamp = GL_TRUE;
   ctx->Extensions.EXT_texture_shared_exponent = GL_TRUE;
   ctx->Extensions.EXT_texture_sRGB = GL_TRUE;
   ctx->Extensions.EXT_texture_sRGB_decode = GL_TRUE;
   ctx->Extensions.EXT_texture_swizzle = GL_TRUE;
   /*ctx->Extensions.EXT_transform_feedback = GL_TRUE;*/
   ctx->Extensions.EXT_vertex_array_bgra = GL_TRUE;
   ctx->Extensions.MESA_pack_invert = GL_TRUE;
   ctx->Extensions.MESA_ycbcr_texture = GL_TRUE;
   ctx->Extensions.NV_conditional_render = GL_TRUE;
   ctx->Extensions.NV_point_sprite = GL_TRUE;
   ctx->Extensions.NV_texture_env_combine4 = GL_TRUE;
   ctx->Extensions.NV_texture_rectangle = GL_TRUE;
   ctx->Extensions.EXT_gpu_program_parameters = GL_TRUE;
   ctx->Extensions.OES_standard_derivatives = GL_TRUE;
   ctx->Extensions.TDFX_texture_compression_FXT1 = GL_TRUE;
   if (ctx->Mesa_DXTn) {
      ctx->Extensions.ANGLE_texture_compression_dxt = GL_TRUE;
      ctx->Extensions.EXT_texture_compression_s3tc = GL_TRUE;
   }
}

/**
 * Either enable or disable the named extension.
 * \return offset of extensions withint `ext' or 0 if extension is not known
 */
static size_t
set_extension(struct gl_extensions *ext, const char *name, GLboolean state)
{
   size_t offset;

   offset = name_to_offset(name);
   if (offset != 0 && (offset != o(dummy_true) || state != GL_FALSE)) {
      ((GLboolean *) ext)[offset] = state;
   }

   return offset;
}

/**
 * \brief Apply the \c MESA_EXTENSION_OVERRIDE environment variable.
 *
 * \c MESA_EXTENSION_OVERRIDE is a space-separated list of extensions to
 * enable or disable. The list is processed thus:
 *    - Enable recognized extension names that are prefixed with '+'.
 *    - Disable recognized extension names that are prefixed with '-'.
 *    - Enable recognized extension names that are not prefixed.
 *    - Collect unrecognized extension names in a new string.
 *
 * \c MESA_EXTENSION_OVERRIDE was previously parsed during
 * _mesa_one_time_init_extension_overrides. We just use the results of that
 * parsing in this function.
 *
 * \return Space-separated list of unrecognized extension names (which must
 *    be freed). Does not return \c NULL.
 */
static char *
get_extension_override( struct gl_context *ctx )
{
   override_extensions_in_context(ctx);

   if (cant_disable_extensions != NULL) {
      _mesa_problem(ctx,
                    "Trying to disable permanently enabled extensions: %s",
	            cant_disable_extensions);
   }

   if (extra_extensions == NULL) {
      return calloc(1, sizeof(char));
   } else {
      _mesa_problem(ctx, "Trying to enable unknown extensions: %s",
                    extra_extensions);
      return strdup(extra_extensions);
   }
}


/**
 * \brief Free extra_extensions and cant_disable_extensions strings
 *
 * These strings are allocated early during the first context creation by
 * _mesa_one_time_init_extension_overrides.
 */
static void
free_unknown_extensions_strings(void)
{
   free(extra_extensions);
   free(cant_disable_extensions);
}


/**
 * \brief Initialize extension override tables.
 *
 * This should be called one time early during first context initialization.
 */
void
_mesa_one_time_init_extension_overrides(void)
{
   const char *env_const = getenv("MESA_EXTENSION_OVERRIDE");
   char *env;
   char *ext;
   int len;
   size_t offset;

   atexit(free_unknown_extensions_strings);

   memset(&_mesa_extension_override_enables, 0, sizeof(struct gl_extensions));
   memset(&_mesa_extension_override_disables, 0, sizeof(struct gl_extensions));

   if (env_const == NULL) {
      return;
   }

   /* extra_exts: List of unrecognized extensions. */
   extra_extensions = calloc(ALIGN(strlen(env_const) + 2, 4), sizeof(char));
   cant_disable_extensions = calloc(ALIGN(strlen(env_const) + 2, 4), sizeof(char));

   /* Copy env_const because strtok() is destructive. */
   env = strdup(env_const);

   if (env == NULL || extra_extensions == NULL ||
           cant_disable_extensions == NULL) {
       free(env);
       free(extra_extensions);
       free(cant_disable_extensions);
       return;
   }

   for (ext = strtok(env, " "); ext != NULL; ext = strtok(NULL, " ")) {
      int enable;
      bool recognized;
      switch (ext[0]) {
      case '+':
         enable = 1;
         ++ext;
         break;
      case '-':
         enable = 0;
         ++ext;
         break;
      default:
         enable = 1;
         break;
      }

      offset = set_extension(&_mesa_extension_override_enables, ext, enable);
      if (offset != 0 && (offset != o(dummy_true) || enable != GL_FALSE)) {
         ((GLboolean *) &_mesa_extension_override_disables)[offset] = !enable;
         recognized = true;
      } else {
         recognized = false;
      }

      if (!recognized) {
         if (enable) {
            strcat(extra_extensions, ext);
            strcat(extra_extensions, " ");
         } else if (offset == o(dummy_true)) {
            strcat(cant_disable_extensions, ext);
            strcat(cant_disable_extensions, " ");
         }
      }
   }

   free(env);

   /* Remove trailing space, and free if unused. */
   len = strlen(extra_extensions);
   if (len == 0) {
      free(extra_extensions);
      extra_extensions = NULL;
   } else if (extra_extensions[len - 1] == ' ') {
      extra_extensions[len - 1] = '\0';
   }
   len = strlen(cant_disable_extensions);
   if (len == 0) {
      free(cant_disable_extensions);
      cant_disable_extensions = NULL;
   } else if (cant_disable_extensions[len - 1] == ' ') {
      cant_disable_extensions[len - 1] = '\0';
   }
}


/**
 * \brief Initialize extension tables and enable default extensions.
 *
 * This should be called during context initialization.
 * Note: Sets gl_extensions.dummy_true to true.
 */
void
_mesa_init_extensions(struct gl_extensions *extensions)
{
   GLboolean *base = (GLboolean *) extensions;
   GLboolean *sentinel = base + o(extension_sentinel);
   GLboolean *i;

   /* First, turn all extensions off. */
   for (i = base; i != sentinel; ++i)
      *i = GL_FALSE;

   /* Then, selectively turn default extensions on. */
   extensions->dummy_true = GL_TRUE;
   extensions->EXT_texture3D = GL_TRUE;
}


typedef unsigned short extension_index;


/**
 * Compare two entries of the extensions table.  Sorts first by year,
 * then by name.
 *
 * Arguments are indices into extension_table.
 */
static int
extension_compare(const void *p1, const void *p2)
{
   extension_index i1 = * (const extension_index *) p1;
   extension_index i2 = * (const extension_index *) p2;
   const struct extension *e1 = &extension_table[i1];
   const struct extension *e2 = &extension_table[i2];
   int res;

   res = (int)e1->year - (int)e2->year;

   if (res == 0) {
      res = strcmp(e1->name, e2->name);
   }

   return res;
}


/**
 * Construct the GL_EXTENSIONS string.  Called the first time that
 * glGetString(GL_EXTENSIONS) is called.
 */
GLubyte*
_mesa_make_extension_string(struct gl_context *ctx)
{
   /* The extension string. */
   char *exts = 0;
   /* Length of extension string. */
   size_t length = 0;
   /* Number of extensions */
   unsigned count;
   /* Indices of the extensions sorted by year */
   extension_index *extension_indices;
   /* String of extra extensions. */
   char *extra_extensions = get_extension_override(ctx);
   GLboolean *base = (GLboolean *) &ctx->Extensions;
   unsigned k;
   unsigned j;
   unsigned maxYear = ~0;
   unsigned api_set = (1 << ctx->API);
   if (_mesa_is_gles3(ctx))
      api_set |= ES3;
   if (_mesa_is_gles31(ctx))
      api_set |= ES31;

   /* Check if the MESA_EXTENSION_MAX_YEAR env var is set */
   {
      const char *env = getenv("MESA_EXTENSION_MAX_YEAR");
      if (env) {
         maxYear = atoi(env);
         _mesa_debug(ctx, "Note: limiting GL extensions to %u or earlier\n",
                     maxYear);
      }
   }

   /* Compute length of the extension string. */
   count = 0;
   for (k = 0; k < ARRAY_SIZE(extension_table); ++k) {
      const struct extension *i = extension_table + k;

      if (base[i->offset] &&
          i->year <= maxYear &&
          (i->api_set & api_set)) {
	 length += strlen(i->name) + 1; /* +1 for space */
	 ++count;
      }
   }
   if (extra_extensions != NULL)
      length += 1 + strlen(extra_extensions); /* +1 for space */

   exts = calloc(ALIGN(length + 1, 4), sizeof(char));
   if (exts == NULL) {
      free(extra_extensions);
      return NULL;
   }

   extension_indices = malloc(count * sizeof(extension_index));
   if (extension_indices == NULL) {
      free(exts);
      free(extra_extensions);
      return NULL;
   }

   /* Sort extensions in chronological order because certain old applications
    * (e.g., Quake3 demo) store the extension list in a static size buffer so
    * chronologically order ensure that the extensions that such applications
    * expect will fit into that buffer.
    */
   j = 0;
   for (k = 0; k < ARRAY_SIZE(extension_table); ++k) {
      const struct extension *i = extension_table + k;

      if (base[i->offset] &&
          i->year <= maxYear &&
          (i->api_set & api_set)) {
         extension_indices[j++] = k;
      }
   }
   assert(j == count);
   qsort(extension_indices, count,
         sizeof *extension_indices, extension_compare);

   /* Build the extension string.*/
   for (j = 0; j < count; ++j) {
      const struct extension *i = &extension_table[extension_indices[j]];
      assert(base[i->offset] && (i->api_set & api_set));
      strcat(exts, i->name);
      strcat(exts, " ");
   }
   free(extension_indices);
   if (extra_extensions != 0) {
      strcat(exts, extra_extensions);
      free(extra_extensions);
   }

   return (GLubyte *) exts;
}

/**
 * Return number of enabled extensions.
 */
GLuint
_mesa_get_extension_count(struct gl_context *ctx)
{
   GLboolean *base;
   unsigned k;
   unsigned api_set = (1 << ctx->API);
   if (_mesa_is_gles3(ctx))
      api_set |= ES3;
   if (_mesa_is_gles31(ctx))
      api_set |= ES31;

   /* only count once */
   if (ctx->Extensions.Count != 0)
      return ctx->Extensions.Count;

   base = (GLboolean *) &ctx->Extensions;
   for (k = 0; k < ARRAY_SIZE(extension_table); ++k) {
      const struct extension *i = extension_table + k;

      if (base[i->offset] && (i->api_set & api_set)) {
	 ctx->Extensions.Count++;
      }
   }
   return ctx->Extensions.Count;
}

/**
 * Return name of i-th enabled extension
 */
const GLubyte *
_mesa_get_enabled_extension(struct gl_context *ctx, GLuint index)
{
   const GLboolean *base;
   size_t n;
   unsigned k;
   unsigned api_set = (1 << ctx->API);
   if (_mesa_is_gles3(ctx))
      api_set |= ES3;
   if (_mesa_is_gles31(ctx))
      api_set |= ES31;

   base = (GLboolean*) &ctx->Extensions;
   n = 0;
   for (k = 0; k < ARRAY_SIZE(extension_table); ++k) {
      const struct extension *i = extension_table + k;

      if (base[i->offset] && (i->api_set & api_set)) {
         if (n == index)
            return (const GLubyte*) i->name;
         else
            ++n;
      }
   }

   return NULL;
}
