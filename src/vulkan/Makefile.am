# Copyright © 2015 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

SUBDIRS = . tests

vulkan_includedir = $(includedir)/vulkan

vulkan_include_HEADERS =				\
	$(top_srcdir)/include/vulkan/vk_platform.h	\
	$(top_srcdir)/include/vulkan/vulkan.h		\
	$(top_srcdir)/include/vulkan/vulkan_intel.h	\
	$(top_srcdir)/include/vulkan/vk_ext_khr_swapchain.h	\
	$(top_srcdir)/include/vulkan/vk_ext_khr_device_swapchain.h

lib_LTLIBRARIES = libvulkan.la

check_LTLIBRARIES = libvulkan-test.la

# The gallium includes are for the util/u_math.h include from main/macros.h

AM_CPPFLAGS = \
	$(INTEL_CFLAGS) \
	$(VALGRIND_CFLAGS) \
	$(DEFINES) \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/glsl/nir \
	-I$(top_srcdir)/src/mapi \
	-I$(top_srcdir)/src/mesa \
	-I$(top_srcdir)/src/mesa/drivers/dri/common \
	-I$(top_srcdir)/src/mesa/drivers/dri/i965 \
	-I$(top_srcdir)/src/gallium/auxiliary \
	-I$(top_srcdir)/src/gallium/include \
	-I$(top_builddir)/src/vulkan

libvulkan_la_CFLAGS =							\
	-Wall -Wno-unused-parameter -fvisibility=hidden -O0 -g		\
	-Wstrict-prototypes -Wmissing-prototypes -Wno-override-init

VULKAN_SOURCES =                                        \
	anv_allocator.c                                 \
	anv_cmd_buffer.c                                \
	anv_batch_chain.c                               \
	anv_device.c                                    \
        anv_dump.c                                      \
	anv_entrypoints.c                               \
	anv_entrypoints.h                               \
	anv_formats.c                                   \
	anv_image.c                                     \
	anv_intel.c                                     \
	anv_meta.c                                      \
	anv_nir_apply_dynamic_offsets.c                 \
	anv_nir_apply_pipeline_layout.c                 \
	anv_nir_lower_push_constants.c                  \
	anv_pass.c					\
	anv_pipeline.c                                  \
	anv_private.h                                   \
	anv_query.c                                     \
	anv_util.c                                      \
	anv_wsi.c					\
	anv_wsi_x11.c					\
	gen8_state.c					\
	gen8_cmd_buffer.c				\
	gen8_pipeline.c					\
	gen7_state.c					\
	gen7_cmd_buffer.c				\
	gen7_pipeline.c

BUILT_SOURCES =                                         \
	anv_entrypoints.h                               \
	anv_entrypoints.c

if HAVE_EGL_PLATFORM_WAYLAND
BUILT_SOURCES += \
	wayland-drm-protocol.c \
	wayland-drm-client-protocol.h

%-protocol.c : $(top_srcdir)/src/egl/wayland/wayland-drm/%.xml
	$(AM_V_GEN)$(WAYLAND_SCANNER) code < $< > $@

%-client-protocol.h : $(top_srcdir)/src/egl/wayland/wayland-drm/%.xml
	$(AM_V_GEN)$(WAYLAND_SCANNER) client-header < $< > $@

AM_CPPFLAGS += -I$(top_srcdir)/src/egl/wayland/wayland-drm
VULKAN_SOURCES += \
	wayland-drm-protocol.c \
	anv_wsi_wayland.c
libvulkan_la_CFLAGS += -DHAVE_WAYLAND_PLATFORM
endif

libvulkan_la_SOURCES =                                  \
	$(VULKAN_SOURCES)                               \
	anv_gem.c

anv_entrypoints.h : anv_entrypoints_gen.py $(vulkan_include_HEADERS)
	$(AM_V_GEN)cat $(vulkan_include_HEADERS) | $(PYTHON2) $< header > $@

anv_entrypoints.c : anv_entrypoints_gen.py $(vulkan_include_HEADERS)
	$(AM_V_GEN)cat $(vulkan_include_HEADERS) | $(PYTHON2) $< code > $@

CLEANFILES = $(BUILT_SOURCES)

libvulkan_la_LIBADD = $(WAYLAND_LIBS) -lxcb -lxcb-dri3 \
	$(top_builddir)/src/mesa/drivers/dri/i965/libi965_compiler.la \
	../mesa/libmesa.la \
	../mesa/drivers/dri/common/libdri_test_stubs.la \
	-lpthread -ldl -lstdc++

# Libvulkan with dummy gem. Used for unit tests.

libvulkan_test_la_SOURCES =                             \
	$(VULKAN_SOURCES)                               \
	anv_gem_stubs.c

libvulkan_test_la_CFLAGS = $(libvulkan_la_CFLAGS)
libvulkan_test_la_LIBADD = $(libvulkan_la_LIBADD)

include $(top_srcdir)/install-lib-links.mk
