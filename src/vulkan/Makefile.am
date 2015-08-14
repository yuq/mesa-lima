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
	$(top_srcdir)/include/vulkan/vk_wsi_lunarg.h

lib_LTLIBRARIES = libvulkan.la

check_LTLIBRARIES = libvulkan-test.la

# The gallium includes are for the util/u_math.h include from main/macros.h

AM_CPPFLAGS = \
	$(INTEL_CFLAGS) \
	$(VALGRIND_CFLAGS) \
	$(DEFINES) \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/mapi \
	-I$(top_srcdir)/src/mesa \
	-I$(top_srcdir)/src/mesa/drivers/dri/common \
	-I$(top_srcdir)/src/mesa/drivers/dri/i965 \
	-I$(top_srcdir)/src/gallium/auxiliary \
	-I$(top_srcdir)/src/gallium/include \
	-I$(top_builddir)/src/vulkan

libvulkan_la_CFLAGS =							\
	-Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -O0 -g	\
	-Wstrict-prototypes -Wmissing-prototypes -Wno-override-init

libvulkan_la_CXXFLAGS =							\
	-Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -O0 -g

VULKAN_SOURCES =                                        \
	anv_allocator.c                                 \
	anv_cmd_buffer.c                                \
	anv_batch_chain.c                               \
	anv_compiler.cpp                                \
	anv_device.c                                    \
	anv_entrypoints.c                               \
	anv_entrypoints.h                               \
	anv_formats.c                                   \
	anv_image.c                                     \
	anv_intel.c                                     \
	anv_meta.c                                      \
	anv_pipeline.c                                  \
	anv_private.h                                   \
	anv_query.c                                     \
	anv_util.c                                      \
	anv_x11.c					\
	anv_gen8.c

libvulkan_la_SOURCES =                                  \
	$(VULKAN_SOURCES)                               \
	anv_gem.c

BUILT_SOURCES =                                         \
	anv_entrypoints.h                               \
	anv_entrypoints.c                               \
	anv_meta_spirv_autogen.h

anv_entrypoints.h : anv_entrypoints_gen.py $(vulkan_include_HEADERS)
	$(AM_V_GEN)cat $(vulkan_include_HEADERS) | $(PYTHON2) $< header > $@

anv_entrypoints.c : anv_entrypoints_gen.py $(vulkan_include_HEADERS)
	$(AM_V_GEN)cat $(vulkan_include_HEADERS) | $(PYTHON2) $< code > $@

%_spirv_autogen.h: %.c glsl_scraper.py
	$(AM_V_GEN) $(PYTHON2) $(srcdir)/glsl_scraper.py --glsl-only -o $@ $<

CLEANFILES = $(BUILT_SOURCES)

libvulkan_la_LIBADD = -lxcb -lxcb-dri3 \
	$(top_builddir)/src/mesa/drivers/dri/i965/libi965_compiler.la

# Libvulkan with dummy gem. Used for unit tests.

libvulkan_test_la_SOURCES =                             \
	$(VULKAN_SOURCES)                               \
	anv_gem_stubs.c

libvulkan_test_la_CFLAGS = $(libvulkan_la_CFLAGS)
libvulkan_test_la_CXXFLAGS = $(libvulkan_la_CXXFLAGS)
libvulkan_test_la_LIBADD = $(libvulkan_la_LIBADD)

include $(top_srcdir)/install-lib-links.mk
