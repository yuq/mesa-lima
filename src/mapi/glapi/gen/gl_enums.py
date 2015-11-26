#!/usr/bin/python2

# (C) Copyright Zack Rusin 2005. All Rights Reserved.
# Copyright (C) 2015 Intel Corporation
# Copyright (C) 2015 Broadcom Corporation
# 
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# on the rights to use, copy, modify, merge, publish, distribute, sub
# license, and/or sell copies of the Software, and to permit persons to whom
# the Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
# IBM AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
#
# Authors:
#    Zack Rusin <zack@kde.org>

import argparse

import license
import gl_XML
import xml.etree.ElementTree as ET
import sys, getopt
import re

class PrintGlEnums(gl_XML.gl_print_base):

    def __init__(self):
        gl_XML.gl_print_base.__init__(self)

        self.name = "gl_enums.py (from Mesa)"
        self.license = license.bsd_license_template % ( \
"""Copyright (C) 1999-2005 Brian Paul All Rights Reserved.""", "BRIAN PAUL")
        # Mapping from enum value to (name, priority) tuples.
        self.enum_table = {}
        # Mapping from enum name to value
        self.string_to_int = {}


    def printRealHeader(self):
        print '#include "main/glheader.h"'
        print '#include "main/enums.h"'
        print '#include "main/imports.h"'
        print '#include "main/mtypes.h"'
        print ''
        print 'typedef struct PACKED {'
        print '   uint16_t offset;'
        print '   int n;'
        print '} enum_elt;'
        print ''
        return

    def print_code(self):
        print """
typedef int (*cfunc)(const void *, const void *);

/**
 * Compare a key enum value to an element in the \c enum_string_table_offsets array.
 *
 * \c bsearch always passes the key as the first parameter and the pointer
 * to the array element as the second parameter.  We can elimiate some
 * extra work by taking advantage of that fact.
 *
 * \param a  Pointer to the desired enum name.
 * \param b  Pointer into the \c enum_string_table_offsets array.
 */
static int compar_nr( const int *a, enum_elt *b )
{
   return a[0] - b->n;
}


static char token_tmp[20];

const char *_mesa_enum_to_string( int nr )
{
   enum_elt *elt;

   STATIC_ASSERT(sizeof(enum_string_table) < (1 << 16));

   elt = bsearch(& nr, enum_string_table_offsets,
                 ARRAY_SIZE(enum_string_table_offsets),
                 sizeof(enum_string_table_offsets[0]),
                 (cfunc) compar_nr);

   if (elt != NULL) {
      return &enum_string_table[elt->offset];
   }
   else {
      /* this is not re-entrant safe, no big deal here */
      _mesa_snprintf(token_tmp, sizeof(token_tmp) - 1, "0x%x", nr);
      token_tmp[sizeof(token_tmp) - 1] = '\\0';
      return token_tmp;
   }
}

/**
 * Primitive names
 */
static const char *prim_names[PRIM_MAX+3] = {
   "GL_POINTS",
   "GL_LINES",
   "GL_LINE_LOOP",
   "GL_LINE_STRIP",
   "GL_TRIANGLES",
   "GL_TRIANGLE_STRIP",
   "GL_TRIANGLE_FAN",
   "GL_QUADS",
   "GL_QUAD_STRIP",
   "GL_POLYGON",
   "GL_LINES_ADJACENCY",
   "GL_LINE_STRIP_ADJACENCY",
   "GL_TRIANGLES_ADJACENCY",
   "GL_TRIANGLE_STRIP_ADJACENCY",
   "GL_PATCHES",
   "outside begin/end",
   "unknown state"
};


/* Get the name of an enum given that it is a primitive type.  Avoids
 * GL_FALSE/GL_POINTS ambiguity and others.
 */
const char *
_mesa_lookup_prim_by_nr(GLuint nr)
{
   if (nr < ARRAY_SIZE(prim_names))
      return prim_names[nr];
   else
      return "invalid mode";
}


"""
        return


    def printBody(self, xml):
        self.process_enums(xml)

        sorted_enum_values = sorted(self.enum_table.keys())
        string_offsets = {}
        i = 0;
        print '#if defined(__GNUC__)'
        print '# define LONGSTRING __extension__'
        print '#else'
        print '# define LONGSTRING'
        print '#endif'
        print ''
        print 'LONGSTRING static const char enum_string_table[] = '
        for enum in sorted_enum_values:
            (name, pri) = self.enum_table[enum]
            print '   "%s\\0"' % (name)
            string_offsets[ enum ] = i
            i += len(name) + 1

        print '   ;'
        print ''


        print 'static const enum_elt enum_string_table_offsets[%u] =' % (len(self.enum_table))
        print '{'
        for enum in sorted_enum_values:
            (name, pri) = self.enum_table[enum]
            print '   { %5u, 0x%08X }, /* %s */' % (string_offsets[enum], enum, name)
        print '};'
        print ''

        self.print_code()
        return

    def add_enum_provider(self, name, priority):
        # Skip some enums, to reduce the diffs from this commit.
        if name in ['GL_NEXT_BUFFER_NV',
                    # Mesa was choosing GL_LINES for this, which wasn't great.
                    'GL_TRUE',
                    # We're blacklisting 4.3, so also blacklist this
                    # to keep the 4.3/ARB_ssbo name for it.
                    'GL_MAX_COMBINED_IMAGE_UNITS_AND_FRAGMENT_OUTPUTS',
                    # GL 2.0 name when Mesa was using GLES 1.0.
                    'GL_BLEND_EQUATION_RGB',
                    # GL3.x compat names that Mesa was missing.
                    'GL_ALPHA_SNORM',
                    'GL_LUMINANCE_SNORM',
                    'GL_LUMINANCE_ALPHA_SNORM',
                    'GL_INTENSITY_SNORM',
                    'GL_ALPHA8_SNORM',
                    'GL_LUMINANCE8_SNORM',
                    'GL_LUMINANCE8_ALPHA8_SNORM',
                    'GL_INTENSITY8_SNORM',
                    'GL_ALPHA16_SNORM',
                    'GL_LUMINANCE16_SNORM',
                    'GL_LUMINANCE16_ALPHA16_SNORM',
                    'GL_INTENSITY16_SNORM',
                    # ARB_imaging names that Mesa was missing.
                    'GL_COLOR_INDEX1_EXT',
                    'GL_COLOR_INDEX2_EXT',
                    'GL_COLOR_INDEX4_EXT',
                    'GL_COLOR_INDEX8_EXT',
                    'GL_COLOR_INDEX12_EXT',
                    'GL_COLOR_INDEX16_EXT',
                    'GL_CONSTANT_BORDER',
                    'GL_REPLICATE_BORDER',
                    'GL_TABLE_TOO_LARGE',
                    # ARB_texture_view names that Mesa was missing.
                    'GL_TEXTURE_VIEW_MIN_LEVEL',
                    'GL_TEXTURE_VIEW_NUM_LEVELS',
                    'GL_TEXTURE_VIEW_MIN_LAYER',
                    'GL_TEXTURE_VIEW_NUM_LAYERS',
                    # GL4.2 BPTC names that Mesa was missing.
                    'GL_COMPRESSED_RGBA_BPTC_UNORM',
                    'GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM',
                    'GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT',
                    'GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT',
                    # Mesa was choosing the EXT names for these
                    # instead of core.
                    'GL_ALPHA_INTEGER',
                    'GL_PROGRAM_SEPARABLE',
                    'GL_PROGRAM_PIPELINE_BINDING',
                    'GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS',
                    # Mesa was choosing the ARB names for these instead of core.
                    'GL_TEXTURE_CUBE_MAP_ARRAY',
                    'GL_TEXTURE_BINDING_CUBE_MAP_ARRAY',
                    'GL_PROXY_TEXTURE_CUBE_MAP_ARRAY',
                    'GL_SAMPLER_CUBE_MAP_ARRAY',
                    'GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW',
                    'GL_INT_SAMPLER_CUBE_MAP_ARRAY',
                    'GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY',
                    'GL_TRANSFORM_FEEDBACK_PAUSED',
                    'GL_TRANSFORM_FEEDBACK_ACTIVE',
                    'GL_VERTEX_ATTRIB_ARRAY_DIVISOR',
                    # Mesa was choosing the ANGLE names for these
                    # instead of EXT.
                    'GL_COMPRESSED_RGBA_S3TC_DXT3_EXT',
                    'GL_COMPRESSED_RGBA_S3TC_DXT5_EXT',
                    ]:
            return

        # Mesa didn't know about the second half of this set of enums.
        m = re.match('GL_COLOR_ATTACHMENT([0-9]*)', name)
        if m and int(m.group(1)) >= 16:
            return

        value = self.string_to_int[name]

        # We don't want the weird GL_SKIP_COMPONENTS1_NV enums.
        if value < 0:
            return
        # We don't want the 64-bit GL_TIMEOUT_IGNORED "enums"
        if value > 0xffffffff:
            return

        # We don't want bitfields in the enum-to-string table --
        # individual bits have so many names, it's pointless.  Note
        # that we check for power-of-two, since some getters have
        # "_BITS" in their name, but none have a power-of-two enum
        # number.
        if not (value & (value - 1)) and '_BIT' in name:
            return

        # Also drop the GL_*_ATTRIB_BITS bitmasks.
        if value == 0xffffffff:
                return

        if value in self.enum_table:
            (n, p) = self.enum_table[value]
            if priority < p:
                self.enum_table[value] = (name, priority)
        else:
            self.enum_table[value] = (name, priority)

    def process_extension(self, extension):
        # Skip some of the extensions, to reduce the diffs from this commit.
        extension_name = extension.get('name')
        whitelist = ['GL_ANGLE_texture_compression_dxt3',
                     'GL_ANGLE_texture_compression_dxt5',
                     'GL_APPLE_flush_buffer_range',
                     'GL_APPLE_object_purgeable',
                     'GL_APPLE_texture_range',
                     'GL_SGIS_texture_color_mask',
                     'GL_SGIX_clipmap',
                     'GL_SGIX_texture_coordinate_clamp',
                     'GL_SGIX_fog_offset',
                     'GL_SGIX_texture_scale_bias',
                     'GL_SGIX_texture_lod_bias',
                     'GL_SGIX_shadow',
                     'GL_APPLE_ycbcr_422']
        whitelist_only_prefixes = ['GL_APPLE',
                                   'GL_ANGLE',
                                   'GL_ARM',
                                   'GL_DMP',
                                   'GL_FJ',
                                   'GL_INGR',
                                   'GL_IMG_',
                                   'GL_MESAX_',
                                   'GL_MALI_',
                                   'GL_NVX_',
                                   'GL_OML_',
                                   'GL_OVR_',
                                   'GL_PGI_',
                                   'GL_QCOM_',
                                   'GL_REND_',
                                   'GL_SGIS_',
                                   'GL_SGIX_',
                                   'GL_WIN_',
                                   'GL_VIV_']

        for prefix in whitelist_only_prefixes:
            if extension_name.startswith(prefix):
                if extension_name not in whitelist:
                    return

        if extension_name in ['GL_ATI_element_array',
                              'GL_ATI_meminfo',
                              'GL_ATI_text_fragment_shader',
                              'GL_ATI_pixel_format_float',
                              'GL_ATI_pn_triangles',
                              'GL_ATI_vertex_array_object',
                              'GL_ATI_vertex_streams',
                              'GL_AMD_blend_minmax_factor',
                              'GL_AMD_compressed_3DC_texture',
                              'GL_AMD_compressed_ATC_texture',
                              'GL_AMD_debug_output',
                              'GL_AMD_depth_clamp_separate',
                              'GL_AMD_gpu_shader_int64',
                              'GL_AMD_query_buffer_object',
                              'GL_AMD_interleaved_elements',
                              'GL_AMD_name_gen_delete',
                              'GL_AMD_occlusion_query_event',
                              'GL_AMD_program_binary_Z400',
                              'GL_AMD_sample_positions',
                              'GL_AMD_sparse_texture',
                              'GL_AMD_stencil_operation_extended',
                              'GL_AMD_transform_feedback4',
                              'GL_AMD_vertex_shader_tessellator',
                              'GL_ARB_bindless_texture',
                              'GL_ARB_cl_event',
                              'GL_ARB_compute_variable_group_size',
                              'GL_ARB_cull_distance',
                              'GL_ARB_enhanced_layouts',
                              'GL_ARB_indirect_parameters',
                              'GL_ARB_internalformat_query2',
                              'GL_ARB_query_buffer_object',
                              'GL_ARB_shading_language_include',
                              'GL_ARB_sparse_buffer',
                              'GL_ARB_sparse_texture',
                              'GL_ARB_texture_compression_bptc',
                              'GL_ARB_texture_mirror_clamp_to_edge',
                              'GL_ARB_texture_view',
                              'GL_ARB_transform_feedback_overflow_query',
                              'GL_EXT_422_pixels',
                              'GL_EXT_bindable_uniform',
                              'GL_EXT_cmyka',
                              'GL_EXT_coordinate_frame',
                              'GL_EXT_debug_label',
                              'GL_EXT_direct_state_access',
                              'GL_EXT_disjoint_timer_query',
                              'GL_EXT_geometry_shader',
                              'GL_EXT_light_texture',
                              'GL_EXT_robustness',
                              'GL_EXT_tessellation_shader',
                              'GL_EXT_texture_compression_latc',
                              'GL_EXT_texture_filter_minmax',
                              'GL_EXT_texture_sRGB_R8',
                              'GL_EXT_texture_sRGB_RG8',
                              'GL_EXT_framebuffer_multisample_blit_scaled',
                              'GL_EXT_multisample',
                              'GL_EXT_multisampled_render_to_texture',
                              'GL_EXT_multiview_draw_buffers',
                              'GL_EXT_pixel_transform',
                              'GL_EXT_primitive_bounding_box',
                              'GL_EXT_pvrtc_sRGB',
                              'GL_EXT_raster_multisample',
                              'GL_EXT_shader_framebuffer_fetch',
                              'GL_EXT_shader_pixel_local_storage',
                              'GL_EXT_sparse_texture',
                              'GL_EXT_stencil_clear_tag',
                              'GL_EXT_tesselation_shader',
                              'GL_EXT_texture_perturb_normal',
                              'GL_EXT_texture_sRGB',
                              'GL_EXT_texture_storage',
                              'GL_EXT_texture_view',
                              'GL_EXT_vertex_shader',
                              'GL_EXT_vertex_weighting',
                              'GL_EXT_x11_sync_object',
                              'GL_EXT_YUV_target',
                              'GL_IBM_cull_vertex',
                              'GL_IBM_static_data',
                              'GL_IBM_vertex_array_lists',
                              'GL_INTEL_map_texture',
                              'GL_INTEL_parallel_arrays',
                              'GL_HP_image_transform',
                              'GL_HP_texture_lighting',
                              'GL_KHR_blend_equation_advanced',
                              'GL_KHR_blend_equation_advanced_coherent',
                              'GL_KHR_robustness',
                              'GL_NV_blend_equation_advanced',
                              'GL_NV_blend_equation_advanced_coherent',
                              'GL_NV_command_list',
                              'GL_NV_compute_program5',
                              'GL_NV_conservative_raster',
                              'GL_NV_coverage_sample',
                              'GL_NV_deep_texture3D',
                              'GL_NV_depth_buffer_float',
                              'GL_NV_depth_nonlinear',
                              'GL_NV_evaluators',
                              'GL_NV_explicit_multisample',
                              'GL_NV_fence',
                              'GL_NV_fill_rectangle',
                              'GL_NV_float_buffer',
                              'GL_NV_fragment_coverage_to_color',
                              'GL_NV_framebuffer_mixed_samples',
                              'GL_NV_framebuffer_multisample_coverage',
                              'GL_NV_geometry_program4',
                              'GL_NV_gpu_program4',
                              'GL_NV_gpu_program5',
                              'GL_NV_gpu_shader5',
                              'GL_NV_internalformat_sample_query',
                              'GL_NV_multisample_coverage',
                              'GL_NV_parameter_buffer_object',
                              'GL_NV_path_rendering',
                              'GL_NV_path_rendering_shared_edge',
                              'GL_NV_pixel_data_range',
                              'GL_NV_present_video',
                              'GL_NV_register_combiners',
                              'GL_NV_register_combiners2',
                              'GL_NV_sample_locations',
                              'GL_NV_shader_buffer_load',
                              'GL_NV_shader_image_load_store',
                              'GL_NV_shader_thread_group',
                              'GL_NV_sRGB_formats',
                              'GL_NV_tessellation_program5',
                              'GL_NV_texgen_emboss',
                              'GL_NV_texture_multisample',
                              'GL_NV_texture_shader',
                              'GL_NV_texture_shader2',
                              'GL_NV_texture_shader3',
                              'GL_NV_transform_feedback',
                              'GL_NV_uniform_buffer_unified_memory',
                              'GL_NV_vertex_array_range',
                              'GL_NV_vertex_array_range2',
                              'GL_NV_vertex_attrib_integer_64bit',
                              'GL_NV_vertex_buffer_unified_memory',
                              'GL_NV_video_capture',
                              'GL_OES_geometry_shader',
                              'GL_OES_primitive_bounding_box',
                              'GL_OES_tessellation_shader',
                              'GL_OES_texture_compression_astc',
                              'GL_OES_texture_view',
                              'GL_SGI_color_table',
                              'GL_SUN_global_alpha',
                              'GL_SUN_triangle_list',
                              'GL_SUNX_constant_data',
                              'GL_EXT_index_func',
                              'GL_EXT_index_array_formats',
                              'GL_EXT_index_material']:
            return

        if extension.get('name').startswith('GL_ARB_'):
            extension_prio = 400
        elif extension.get('name').startswith('GL_EXT_'):
            extension_prio = 600
        else:
            extension_prio = 800

        for enum in extension.findall('require/enum'):
            self.add_enum_provider(enum.get('name'), extension_prio)

        # Part of 4.4, but Mesa added it early.
        self.add_enum_provider('GL_MAX_VERTEX_ATTRIB_STRIDE', 44)

    def process_enums(self, xml):
        # First, process the XML entries that define the hex values
        # for all of the enum names.
        for enum in xml.findall('enums/enum'):
            name = enum.get('name')
            value = int(enum.get('value'), base=16)

            # If the same name ever maps to multiple values, that can
            # confuse us.  GL_ACTIVE_PROGRAM_EXT is OK to lose because
            # we choose GL_ACTIVE PROGRAM instead.
            if name in self.string_to_int and name != "GL_ACTIVE_PROGRAM_EXT":
                print "#error Renumbering {0} from {1} to {2}".format(name, self.string_to_int[name], value)

            self.string_to_int[name] = value

        # Now, process all of the API versions and extensions that
        # provide enums, so we can decide what name to call any hex
        # value.
        for feature in xml.findall('feature'):
            feature_name = feature.get('name')
            # Skip some of the extensions, to reduce the diffs from this commit.
            if feature_name in ['GL_VERSION_4_3',
                                'GL_VERSION_4_4',
                                'GL_VERSION_4_5',
                                'GL_ES_VERSION_3_1']:
                continue

            # When an enum gets renamed in a newer version (generally
            # because of some generalization of the functionality),
            # prefer the newer name.  Also, prefer desktop GL names to
            # ES.
            m = re.match('GL_VERSION_([0-9])_([0-9])', feature_name)
            if m:
                feature_prio = 100 - int(m.group(1) + m.group(2))
            else:
                m = re.match('GL_ES_VERSION_([0-9])_([0-9])', feature_name)
                if m:
                    feature_prio = 200 - int(m.group(1) + m.group(2))
                else:
                    feature_prio = 200

            for enum in feature.findall('require/enum'):
                self.add_enum_provider(enum.get('name'), feature_prio)

        for extension in xml.findall('extensions/extension'):
            self.process_extension(extension)


def _parser():
    parser = argparse.ArgumentParser()
    parser.add_argument('-f', '--input_file',
                        required=True,
                        help="Choose an xml file to parse.")
    return parser.parse_args()


def main():
    args = _parser()
    xml = ET.parse(args.input_file)

    printer = PrintGlEnums()
    printer.Print(xml)


if __name__ == '__main__':
    main()
