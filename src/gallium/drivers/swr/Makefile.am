# Copyright (C) 2015 Intel Corporation.   All Rights Reserved.
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

include Makefile.sources
include $(top_srcdir)/src/gallium/Automake.inc

AM_CXXFLAGS = $(GALLIUM_DRIVER_CFLAGS) $(SWR_CXX11_CXXFLAGS)

noinst_LTLIBRARIES = libmesaswr.la

libmesaswr_la_SOURCES = $(LOADER_SOURCES)

COMMON_CXXFLAGS = \
	-fno-strict-aliasing \
	$(GALLIUM_DRIVER_CFLAGS) \
	$(LLVM_CXXFLAGS) \
	$(SWR_CXX11_CXXFLAGS) \
	-I$(builddir)/rasterizer/codegen \
	-I$(builddir)/rasterizer/jitter \
	-I$(builddir)/rasterizer/archrast \
	-I$(srcdir)/rasterizer \
	-I$(srcdir)/rasterizer/core \
	-I$(srcdir)/rasterizer/codegen \
	-I$(srcdir)/rasterizer/jitter \
	-I$(srcdir)/rasterizer/archrast

COMMON_SOURCES = \
	$(CXX_SOURCES) \
	$(ARCHRAST_CXX_SOURCES) \
	$(COMMON_CXX_SOURCES) \
	$(CORE_CXX_SOURCES) \
	$(JITTER_CXX_SOURCES) \
	$(MEMORY_CXX_SOURCES) \
	$(BUILT_SOURCES)

BUILT_SOURCES = \
	swr_context_llvm.h \
	rasterizer/codegen/gen_knobs.cpp \
	rasterizer/codegen/gen_knobs.h \
	rasterizer/jitter/state_llvm.h \
	rasterizer/jitter/builder_x86.h \
	rasterizer/jitter/builder_x86.cpp \
	rasterizer/archrast/gen_ar_event.h \
	rasterizer/archrast/gen_ar_event.cpp \
	rasterizer/archrast/gen_ar_eventhandler.h \
	rasterizer/archrast/gen_ar_eventhandlerfile.h \
	rasterizer/core/BackendPixelRate0.cpp

MKDIR_GEN = $(AM_V_at)$(MKDIR_P) $(@D)
PYTHON_GEN = $(AM_V_GEN)$(PYTHON2) $(PYTHON_FLAGS)
swr_context_llvm.h: rasterizer/codegen/gen_llvm_types.py swr_context.h
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_llvm_types.py \
		--input $(srcdir)/swr_context.h \
		--output swr_context_llvm.h

rasterizer/codegen/gen_knobs.cpp: rasterizer/codegen/gen_knobs.py rasterizer/codegen/knob_defs.py rasterizer/codegen/templates/knobs.template
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_knobs.py \
		--input $(srcdir)/rasterizer/codegen/templates/knobs.template \
		--output rasterizer/codegen/gen_knobs.cpp \
		--gen_cpp

rasterizer/codegen/gen_knobs.h: rasterizer/codegen/gen_knobs.py rasterizer/codegen/knob_defs.py rasterizer/codegen/templates/knobs.template
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_knobs.py \
		--input $(srcdir)/rasterizer/codegen/templates/knobs.template \
		--output rasterizer/codegen/gen_knobs.h \
		--gen_h

rasterizer/jitter/state_llvm.h: rasterizer/codegen/gen_llvm_types.py rasterizer/core/state.h
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_llvm_types.py \
		--input $(srcdir)/rasterizer/core/state.h \
		--output rasterizer/jitter/state_llvm.h

rasterizer/jitter/builder_gen.h: rasterizer/codegen/gen_llvm_ir_macros.py
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_llvm_ir_macros.py \
		--input $(LLVM_INCLUDEDIR)/llvm/IR/IRBuilder.h \
		--output rasterizer/jitter/builder_gen.h \
		--gen_h

rasterizer/jitter/builder_gen.cpp: rasterizer/codegen/gen_llvm_ir_macros.py
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_llvm_ir_macros.py \
		--input $(LLVM_INCLUDEDIR)/llvm/IR/IRBuilder.h \
		--output rasterizer/jitter/builder_gen.cpp \
		--gen_cpp

rasterizer/jitter/builder_x86.h: rasterizer/codegen/gen_llvm_ir_macros.py
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_llvm_ir_macros.py \
		--output rasterizer/jitter/builder_x86.h \
		--gen_x86_h

rasterizer/jitter/builder_x86.cpp: rasterizer/codegen/gen_llvm_ir_macros.py
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_llvm_ir_macros.py \
		--output rasterizer/jitter/builder_x86.cpp \
		--gen_x86_cpp

rasterizer/archrast/gen_ar_event.h: rasterizer/codegen/gen_archrast.py rasterizer/codegen/templates/ar_event_h.template rasterizer/archrast/events.proto
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_archrast.py \
		--proto $(srcdir)/rasterizer/archrast/events.proto \
		--output rasterizer/archrast/gen_ar_event.h \
		--gen_event_h

rasterizer/archrast/gen_ar_event.cpp: rasterizer/codegen/gen_archrast.py rasterizer/codegen/templates/ar_event_cpp.template rasterizer/archrast/events.proto
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_archrast.py \
		--proto $(srcdir)/rasterizer/archrast/events.proto \
		--output rasterizer/archrast/gen_ar_event.cpp \
		--gen_event_cpp

rasterizer/archrast/gen_ar_eventhandler.h: rasterizer/codegen/gen_archrast.py rasterizer/codegen/templates/ar_eventhandler_h.template rasterizer/archrast/events.proto
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_archrast.py \
		--proto $(srcdir)/rasterizer/archrast/events.proto \
		--output rasterizer/archrast/gen_ar_eventhandler.h \
		--gen_eventhandler_h

rasterizer/archrast/gen_ar_eventhandlerfile.h: rasterizer/codegen/gen_archrast.py rasterizer/codegen/templates/ar_eventhandlerfile_h.template rasterizer/archrast/events.proto
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_archrast.py \
		--proto $(srcdir)/rasterizer/archrast/events.proto \
		--output rasterizer/archrast/gen_ar_eventhandlerfile.h \
		--gen_eventhandlerfile_h

# 5 SWR_MULTISAMPLE_TYPE_COUNT
# 2 SWR_MSAA_SAMPLE_PATTERN_COUNT
# 3 SWR_INPUT_COVERAGE_COUNT
# 2 centroid
# 2 forcedSampleCount
# 2 canEarlyZ
rasterizer/core/BackendPixelRate0.cpp: rasterizer/codegen/gen_backends.py rasterizer/codegen/templates/backend_template.cpp
	$(MKDIR_GEN)
	$(PYTHON_GEN) \
		$(srcdir)/rasterizer/codegen/gen_backends.py \
		--outdir rasterizer/core \
		--dim 5 2 3 2 2 2 \
		--split 0 \
		--cpp

COMMON_LIBADD = \
	$(top_builddir)/src/gallium/auxiliary/libgallium.la \
	$(top_builddir)/src/mesa/libmesagallium.la \
	$(LLVM_LIBS)

COMMON_LDFLAGS = \
	-shared \
	-module \
	-no-undefined \
	$(GC_SECTIONS) \
	$(NO_UNDEFINED) \
	$(LLVM_LDFLAGS)


# XXX: As we cannot use BUILT_SOURCES (the files will end up in the dist
# tarball) just annotate the dependency directly.
# As the single direct user of builder_gen.h is a header (builder.h) trace all
# the translusive users (one that use the latter header).
rasterizer/jitter/blend_jit.cpp: rasterizer/jitter/builder_gen.h
rasterizer/jitter/builder.cpp: rasterizer/jitter/builder_gen.h
rasterizer/jitter/builder_gen.cpp: rasterizer/jitter/builder_gen.h
rasterizer/jitter/builder_x86.cpp: rasterizer/jitter/builder_gen.h
rasterizer/jitter/builder_misc.cpp: rasterizer/jitter/builder_gen.h
rasterizer/jitter/fetch_jit.cpp: rasterizer/jitter/builder_gen.h
rasterizer/jitter/streamout_jit.cpp: rasterizer/jitter/builder_gen.h
swr_shader.cpp: rasterizer/jitter/builder_gen.h

CLEANFILES = \
	rasterizer/jitter/builder_gen.h \
	rasterizer/jitter/builder_gen.cpp

# XXX: Due to the funky dependencies above, the builder_x86.cpp file gets
# generated (copied) into builddir when building from release tarball.
# Add a temporary workaround to remove it, until the above issue is resolved.
distclean-local:
	( test $(top_srcdir) != $(top_builddir) && \
		rm $(builddir)/rasterizer/jitter/builder_x86.cpp ) || true

lib_LTLIBRARIES = libswrAVX.la libswrAVX2.la

libswrAVX_la_CXXFLAGS = \
	$(SWR_AVX_CXXFLAGS) \
	-DKNOB_ARCH=KNOB_ARCH_AVX \
	$(COMMON_CXXFLAGS)

libswrAVX_la_SOURCES = \
	$(COMMON_SOURCES)

# XXX: Don't ship these generated sources for now, since they are specific
# to the LLVM version they are generated from. Thus a release tarball
# containing the said files, generated against eg. LLVM 3.8 will fail to build
# on systems with other versions of LLVM eg. 3.7 or 3.6.
# Move these back to BUILT_SOURCES once that is resolved.
nodist_libswrAVX_la_SOURCES = \
	rasterizer/jitter/builder_gen.h \
	rasterizer/jitter/builder_gen.cpp

libswrAVX_la_LIBADD = \
	$(COMMON_LIBADD)

libswrAVX_la_LDFLAGS = \
	$(COMMON_LDFLAGS)

libswrAVX2_la_CXXFLAGS = \
	$(SWR_AVX2_CXXFLAGS) \
	-DKNOB_ARCH=KNOB_ARCH_AVX2 \
	$(COMMON_CXXFLAGS)

libswrAVX2_la_SOURCES = \
	$(COMMON_SOURCES)

# XXX: Don't ship these generated sources for now, since they are specific
# to the LLVM version they are generated from. Thus a release tarball
# containing the said files, generated against eg. LLVM 3.8 will fail to build
# on systems with other versions of LLVM eg. 3.7 or 3.6.
# Move these back to BUILT_SOURCES once that is resolved.
nodist_libswrAVX2_la_SOURCES = \
	rasterizer/jitter/builder_gen.h \
	rasterizer/jitter/builder_gen.cpp

libswrAVX2_la_LIBADD = \
	$(COMMON_LIBADD)

libswrAVX2_la_LDFLAGS = \
	$(COMMON_LDFLAGS)

include $(top_srcdir)/install-gallium-links.mk

EXTRA_DIST = \
	SConscript \
	rasterizer/archrast/events.proto \
	rasterizer/codegen/gen_llvm_ir_macros.py \
	rasterizer/codegen/gen_llvm_types.py \
	rasterizer/codegen/gen_archrast.py \
	rasterizer/codegen/gen_backends.py \
	rasterizer/codegen/gen_knobs.py \
	rasterizer/codegen/knob_defs.py \
	rasterizer/codegen/mako/ast.py \
	rasterizer/codegen/mako/_ast_util.py \
	rasterizer/codegen/mako/cache.py \
	rasterizer/codegen/mako/cmd.py \
	rasterizer/codegen/mako/codegen.py \
	rasterizer/codegen/mako/compat.py \
	rasterizer/codegen/mako/exceptions.py \
	rasterizer/codegen/mako/filters.py \
	rasterizer/codegen/mako/__init__.py \
	rasterizer/codegen/mako/lexer.py \
	rasterizer/codegen/mako/lookup.py \
	rasterizer/codegen/mako/parsetree.py \
	rasterizer/codegen/mako/pygen.py \
	rasterizer/codegen/mako/pyparser.py \
	rasterizer/codegen/mako/runtime.py \
	rasterizer/codegen/mako/template.py \
	rasterizer/codegen/mako/util.py \
	rasterizer/codegen/templates/knobs.template \
	rasterizer/codegen/templates/ar_event_h.template \
	rasterizer/codegen/templates/ar_event_cpp.template \
	rasterizer/codegen/templates/ar_eventhandler_h.template \
	rasterizer/codegen/templates/ar_eventhandlerfile_h.template \
	rasterizer/codegen/templates/backend_template.cpp
