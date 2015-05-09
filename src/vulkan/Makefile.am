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

lib_LTLIBRARIES = libvulkan.la

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
	-I$(top_srcdir)/src/gallium/include

libvulkan_la_CFLAGS =							\
	-Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -O0 -g	\
	-Wstrict-prototypes -Wmissing-prototypes -Wno-override-init

libvulkan_la_CXXFLAGS =							\
	-Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -O0 -g

libvulkan_la_SOURCES =				\
	private.h				\
	gem.c					\
	device.c				\
	aub.c					\
	allocator.c				\
	util.c					\
	pipeline.c				\
	image.c					\
	meta.c					\
	intel.c					\
	compiler.cpp

bin_PROGRAMS = vk

vk_SOURCES = vk.c
vk_LDADD = libvulkan.la -lpng16

libvulkan_la_LIBADD = -lxcb -lxcb-dri3 \
	$(top_builddir)/src/mesa/drivers/dri/i965/libi965_compiler.la

include $(top_srcdir)/install-lib-links.mk
