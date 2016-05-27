# Copyright 2015 Intel Corporation
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

ISL_GEN_LIBS =                                           \
	libisl-gen7.la                                   \
	libisl-gen75.la                                  \
	libisl-gen8.la                                   \
	libisl-gen9.la                                   \
	$(NULL)

noinst_LTLIBRARIES = $(ISL_GEN_LIBS) libisl.la

# The gallium includes are for the util/u_math.h include from main/macros.h
AM_CPPFLAGS = \
	$(INTEL_CFLAGS) \
	$(VALGRIND_CFLAGS) \
	$(DEFINES) \
	-I$(top_srcdir)/include \
	-I$(top_builddir)/src \
	-I$(top_srcdir)/src \
	-I$(top_builddir)/src/intel \
	-I$(top_srcdir)/src/intel \
	-I$(top_srcdir)/src/mapi \
	-I$(top_srcdir)/src/mesa \
	-I$(top_srcdir)/src/mesa/drivers/dri/i965 \
	-I$(top_srcdir)/src/gallium/auxiliary \
	-I$(top_srcdir)/src/gallium/include

libisl_la_CFLAGS = $(CFLAGS) -Wno-override-init

libisl_la_LIBADD = $(ISL_GEN_LIBS)

libisl_la_SOURCES =                                     \
	isl.c                                           \
	isl.h                                           \
	isl_format.c                                    \
	isl_format_layout.c                             \
	isl_gen4.c                                      \
	isl_gen4.h                                      \
	isl_gen6.c                                      \
	isl_gen6.h                                      \
	isl_priv.h                                      \
	isl_storage_image.c                             \
	$(NULL)

libisl_gen7_la_SOURCES =                                \
	isl_gen7.c                                      \
	isl_gen7.h                                      \
        isl_surface_state.c                             \
	$(NULL)
libisl_gen7_la_CFLAGS = $(libisl_la_CFLAGS) -DGEN_VERSIONx10=70

libisl_gen75_la_SOURCES =                               \
        isl_surface_state.c                             \
	$(NULL)
libisl_gen75_la_CFLAGS = $(libisl_la_CFLAGS) -DGEN_VERSIONx10=75

libisl_gen8_la_SOURCES =                                \
	isl_gen8.c                                      \
	isl_gen8.h                                      \
        isl_surface_state.c                             \
	$(NULL)
libisl_gen8_la_CFLAGS = $(libisl_la_CFLAGS) -DGEN_VERSIONx10=80

libisl_gen9_la_SOURCES =                                \
	isl_gen9.c                                      \
	isl_gen9.h                                      \
        isl_surface_state.c                             \
	$(NULL)
libisl_gen9_la_CFLAGS = $(libisl_la_CFLAGS) -DGEN_VERSIONx10=90

BUILT_SOURCES =                                         \
	isl_format_layout.c

isl_format_layout.c: isl_format_layout_gen.bash \
                     isl_format_layout.csv
	$(AM_V_GEN)$(srcdir)/isl_format_layout_gen.bash \
	    <$(srcdir)/isl_format_layout.csv >$@

# ----------------------------------------------------------------------------
#  Tests
# ----------------------------------------------------------------------------

check_PROGRAMS = tests/isl_surf_get_image_offset_test

TESTS = $(check_PROGRAMS)

tests_isl_surf_get_image_offset_test_LDADD = \
	libisl.la \
	$(top_builddir)/src/mesa/drivers/dri/i965/libi965_compiler.la \
	-lm

# ----------------------------------------------------------------------------

EXTRA_DIST = \
	isl_format_layout_gen.bash \
	isl_format_layout.csv \
	README
