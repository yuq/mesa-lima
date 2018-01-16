COPYRIGHT = """\
/*
 * Copyright 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
"""

import argparse
import xml.etree.cElementTree as et

from mako.template import Template

from anv_extensions import *

def _init_exts_from_xml(xml):
    """ Walk the Vulkan XML and fill out extra extension information. """

    xml = et.parse(xml)

    ext_name_map = {}
    for ext in EXTENSIONS:
        ext_name_map[ext.name] = ext

    for ext_elem in xml.findall('.extensions/extension'):
        ext_name = ext_elem.attrib['name']
        if ext_name not in ext_name_map:
            continue

        # Workaround for VK_ANDROID_native_buffer. Its <extension> element in
        # vk.xml lists it as supported="disabled" and provides only a stub
        # definition.  Its <extension> element in Mesa's custom
        # vk_android_native_buffer.xml, though, lists it as
        # supported='android-vendor' and fully defines the extension. We want
        # to skip the <extension> element in vk.xml.
        if ext_elem.attrib['supported'] == 'disabled':
            assert ext_name == 'VK_ANDROID_native_buffer'
            continue

        ext = ext_name_map[ext_name]
        ext.type = ext_elem.attrib['type']

_TEMPLATE = Template(COPYRIGHT + """
#include "anv_private.h"

#include "vk_util.h"

/* Convert the VK_USE_PLATFORM_* defines to booleans */
%for platform in ['ANDROID', 'WAYLAND', 'XCB', 'XLIB']:
#ifdef VK_USE_PLATFORM_${platform}_KHR
#   undef VK_USE_PLATFORM_${platform}_KHR
#   define VK_USE_PLATFORM_${platform}_KHR true
#else
#   define VK_USE_PLATFORM_${platform}_KHR false
#endif
%endfor

/* And ANDROID too */
#ifdef ANDROID
#   undef ANDROID
#   define ANDROID true
#else
#   define ANDROID false
#endif

#define ANV_HAS_SURFACE (VK_USE_PLATFORM_WAYLAND_KHR || \\
                         VK_USE_PLATFORM_XCB_KHR || \\
                         VK_USE_PLATFORM_XLIB_KHR)

bool
anv_instance_extension_supported(const char *name)
{
%for ext in instance_extensions:
    if (strcmp(name, "${ext.name}") == 0)
        return ${ext.enable};
%endfor
    return false;
}

VkResult anv_EnumerateInstanceExtensionProperties(
    const char*                                 pLayerName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties)
{
    VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);

%for ext in instance_extensions:
    if (${ext.enable}) {
        vk_outarray_append(&out, prop) {
            *prop = (VkExtensionProperties) {
                .extensionName = "${ext.name}",
                .specVersion = ${ext.ext_version},
            };
        }
    }
%endfor

    return vk_outarray_status(&out);
}

uint32_t
anv_physical_device_api_version(struct anv_physical_device *dev)
{
    return ${MAX_API_VERSION.c_vk_version()};
}

bool
anv_physical_device_extension_supported(struct anv_physical_device *device,
                                        const char *name)
{
%for ext in device_extensions:
    if (strcmp(name, "${ext.name}") == 0)
        return ${ext.enable};
%endfor
    return false;
}

VkResult anv_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice                            physicalDevice,
    const char*                                 pLayerName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties)
{
    ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);
    VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);
    (void)device;

%for ext in device_extensions:
    if (${ext.enable}) {
        vk_outarray_append(&out, prop) {
            *prop = (VkExtensionProperties) {
                .extensionName = "${ext.name}",
                .specVersion = ${ext.ext_version},
            };
        }
    }
%endfor

    return vk_outarray_status(&out);
}
""")

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', help='Output C file.', required=True)
    parser.add_argument('--xml',
                        help='Vulkan API XML file.',
                        required=True,
                        action='append',
                        dest='xml_files')
    args = parser.parse_args()

    for filename in args.xml_files:
        _init_exts_from_xml(filename)

    for ext in EXTENSIONS:
        assert ext.type == 'instance' or ext.type == 'device'

    template_env = {
        'MAX_API_VERSION': MAX_API_VERSION,
        'instance_extensions': [e for e in EXTENSIONS if e.type == 'instance'],
        'device_extensions': [e for e in EXTENSIONS if e.type == 'device'],
    }

    with open(args.out, 'w') as f:
        f.write(_TEMPLATE.render(**template_env))
