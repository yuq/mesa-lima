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
	$(top_srcdir)/include/vulkan/vulkan_intel.h

# Used when generating entrypoints to filter out unwanted extensions
VULKAN_ENTRYPOINT_CPPFLAGS = \
   -I$(top_srcdir)/include/vulkan \
   -DVK_USE_PLATFORM_XCB_KHR

lib_LTLIBRARIES = libvulkan_intel.la

check_LTLIBRARIES = libvulkan-test.la

PER_GEN_LIBS = \
   libanv-gen7.la \
   libanv-gen75.la \
   libanv-gen8.la \
   libanv-gen9.la

noinst_LTLIBRARIES = $(PER_GEN_LIBS)

# The gallium includes are for the util/u_math.h include from main/macros.h

AM_CPPFLAGS = \
	$(INTEL_CFLAGS) \
	$(VALGRIND_CFLAGS) \
	$(DEFINES) \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/compiler \
	-I$(top_srcdir)/src/mapi \
	-I$(top_srcdir)/src/mesa \
	-I$(top_srcdir)/src/mesa/drivers/dri/common \
	-I$(top_srcdir)/src/mesa/drivers/dri/i965 \
	-I$(top_srcdir)/src/gallium/auxiliary \
	-I$(top_srcdir)/src/gallium/include \
	-I$(top_srcdir)/src/intel/ \
	-I$(top_builddir)/src \
	-I$(top_builddir)/src/compiler \
	-I$(top_builddir)/src/compiler/nir \
	-I$(top_builddir)/src/intel

libvulkan_intel_la_CFLAGS = $(CFLAGS) -DVK_USE_PLATFORM_XCB_KHR -Wno-override-init -msse2

VULKAN_SOURCES =                                        \
	anv_allocator.c                                 \
	anv_cmd_buffer.c                                \
	anv_batch_chain.c                               \
	anv_descriptor_set.c                            \
	anv_device.c                                    \
        anv_dump.c                                      \
	anv_entrypoints.c                               \
	anv_entrypoints.h                               \
	anv_formats.c                                   \
	anv_image.c                                     \
	anv_intel.c                                     \
	anv_meta.c                                      \
	anv_meta_blit.c                                 \
	anv_meta_blit2d.c                               \
	anv_meta_clear.c                                \
	anv_meta_copy.c                                 \
	anv_meta_resolve.c                              \
	anv_nir_apply_dynamic_offsets.c                 \
	anv_nir_apply_pipeline_layout.c                 \
	anv_nir_lower_push_constants.c                  \
	anv_pass.c					\
	anv_pipeline.c                                  \
	anv_pipeline_cache.c                            \
	anv_private.h                                   \
	anv_query.c                                     \
	anv_util.c                                      \
	anv_wsi.c					\
	anv_wsi_x11.c

BUILT_SOURCES =                                         \
	anv_entrypoints.h                               \
	anv_entrypoints.c

libanv_gen7_la_SOURCES =                                \
	genX_cmd_buffer.c				\
	genX_pipeline.c					\
	gen7_cmd_buffer.c				\
	gen7_pipeline.c                                 \
	genX_state.c
libanv_gen7_la_CFLAGS = $(libvulkan_intel_la_CFLAGS) -DGEN_VERSIONx10=70

libanv_gen75_la_SOURCES =                               \
	genX_cmd_buffer.c				\
	genX_pipeline.c					\
	gen7_cmd_buffer.c				\
	gen7_pipeline.c                                 \
	genX_state.c
libanv_gen75_la_CFLAGS = $(libvulkan_intel_la_CFLAGS) -DGEN_VERSIONx10=75

libanv_gen8_la_SOURCES =                               	\
	genX_cmd_buffer.c				\
	genX_pipeline.c					\
	gen8_cmd_buffer.c				\
	gen8_pipeline.c					\
	genX_state.c
libanv_gen8_la_CFLAGS = $(libvulkan_intel_la_CFLAGS) -DGEN_VERSIONx10=80

libanv_gen9_la_SOURCES =                               	\
	genX_cmd_buffer.c				\
	genX_pipeline.c					\
	gen8_cmd_buffer.c				\
	gen8_pipeline.c					\
	genX_state.c
libanv_gen9_la_CFLAGS = $(libvulkan_intel_la_CFLAGS) -DGEN_VERSIONx10=90

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
libvulkan_intel_la_CFLAGS += -DHAVE_WAYLAND_PLATFORM -DVK_USE_PLATFORM_WAYLAND_KHR

VULKAN_ENTRYPOINT_CPPFLAGS += -DVK_USE_PLATFORM_WAYLAND_KHR
endif

libvulkan_intel_la_SOURCES =                            \
	$(VULKAN_SOURCES)                               \
	anv_gem.c

anv_entrypoints.h : anv_entrypoints_gen.py $(vulkan_include_HEADERS)
	$(AM_V_GEN) cat $(vulkan_include_HEADERS) | $(CPP) $(VULKAN_ENTRYPOINT_CPPFLAGS) - | $(PYTHON2) $< header > $@

anv_entrypoints.c : anv_entrypoints_gen.py $(vulkan_include_HEADERS)
	$(AM_V_GEN) cat $(vulkan_include_HEADERS) | $(CPP) $(VULKAN_ENTRYPOINT_CPPFLAGS) - | $(PYTHON2) $< code > $@

CLEANFILES = $(BUILT_SOURCES)

libvulkan_intel_la_LIBADD = $(WAYLAND_LIBS) \
	-lxcb -lxcb-dri3 -lxcb-present -lxcb-sync -lxshmfence \
	$(top_builddir)/src/intel/isl/libisl.la \
	$(top_builddir)/src/mesa/drivers/dri/i965/libi965_compiler.la \
	$(top_builddir)/src/mesa/libmesa.la \
	$(top_builddir)/src/mesa/drivers/dri/common/libdri_test_stubs.la \
	-lpthread -ldl -lstdc++ \
        $(PER_GEN_LIBS)

libvulkan_intel_la_LDFLAGS = \
        -module -avoid-version -shared -shrext .so


# Generate icd files. It would be nice to just be able to add these to
# AC_CONFIG_FILES, but @libdir@ typically expands to '${exec_prefix}/lib64',
# which we can't put in the icd file. When running sed from the Makefile we
# can use ${libdir}, which expands completely and we avoid putting Makefile
# variables in the icd file.

icdconfdir=$(sysconfdir)/vulkan/icd.d
icdconf_DATA = intel_icd.json
noinst_DATA = dev_icd.json

%.json : %.json.in
	$(AM_V_GEN) $(SED) \
		-e "s#@build_libdir@#${abs_top_builddir}/${LIB_DIR}#" \
		-e "s#@install_libdir@#${libdir}#" < $< > $@


# Libvulkan with dummy gem. Used for unit tests.

libvulkan_test_la_SOURCES =                             \
	$(VULKAN_SOURCES)                               \
	anv_gem_stubs.c

libvulkan_test_la_CFLAGS =                              \
	-I$(top_srcdir)/src/intel/vulkan                \
	$(libvulkan_intel_la_CFLAGS)

libvulkan_test_la_LIBADD = $(libvulkan_intel_la_LIBADD)

include $(top_srcdir)/install-lib-links.mk

install-data-local:
	$(INSTALL_DATA) -D $(srcdir)/intel_icd.json $(DESTDIR)/$(VULKAN_ICD_INSTALL_DIR)/intel_icd.json
