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

AM_CXXFLAGS = $(GALLIUM_DRIVER_CFLAGS) -std=c++11

noinst_LTLIBRARIES = libmesaswr.la

libmesaswr_la_SOURCES = $(LOADER_SOURCES)

COMMON_CXXFLAGS = \
	$(GALLIUM_DRIVER_CFLAGS) \
	$(LLVM_CXXFLAGS) \
	-std=c++11 \
	-I$(builddir)/rasterizer/scripts \
	-I$(builddir)/rasterizer/jitter \
	-I$(srcdir)/rasterizer \
	-I$(srcdir)/rasterizer/core \
	-I$(srcdir)/rasterizer/jitter

COMMON_SOURCES = \
	$(CXX_SOURCES) \
	$(COMMON_CXX_SOURCES) \
	$(CORE_CXX_SOURCES) \
	$(JITTER_CXX_SOURCES) \
	$(MEMORY_CXX_SOURCES) \
	$(BUILT_SOURCES)

BUILT_SOURCES = \
	swr_context_llvm.h \
	rasterizer/scripts/gen_knobs.cpp \
	rasterizer/scripts/gen_knobs.h \
	rasterizer/jitter/state_llvm.h \
	rasterizer/jitter/builder_gen.h \
	rasterizer/jitter/builder_gen.cpp \
	rasterizer/jitter/builder_x86.h \
	rasterizer/jitter/builder_x86.cpp

swr_context_llvm.h: rasterizer/jitter/scripts/gen_llvm_types.py swr_context.h
	$(PYTHON2) $(PYTHON_FLAGS) \
		$(srcdir)/rasterizer/jitter/scripts/gen_llvm_types.py \
		--input $(srcdir)/swr_context.h \
		--output swr_context_llvm.h

rasterizer/scripts/gen_knobs.cpp rasterizer/scripts/gen_knobs.h: rasterizer/scripts/gen_knobs.py rasterizer/scripts/knob_defs.py rasterizer/scripts/templates/knobs.template
	$(PYTHON2) $(PYTHON_FLAGS) \
		$(srcdir)/rasterizer/scripts/gen_knobs.py \
		rasterizer/scripts

rasterizer/jitter/state_llvm.h: rasterizer/jitter/scripts/gen_llvm_types.py rasterizer/core/state.h
	$(PYTHON2) $(PYTHON_FLAGS) \
		$(srcdir)/rasterizer/jitter/scripts/gen_llvm_types.py \
		--input $(srcdir)/rasterizer/core/state.h \
		--output rasterizer/jitter/state_llvm.h

rasterizer/jitter/builder_gen.h: rasterizer/jitter/scripts/gen_llvm_ir_macros.py $(LLVM_INCLUDEDIR)/llvm/IR/IRBuilder.h
	$(PYTHON2) $(PYTHON_FLAGS) \
		$(srcdir)/rasterizer/jitter/scripts/gen_llvm_ir_macros.py \
		--input $(LLVM_INCLUDEDIR)/llvm/IR/IRBuilder.h \
		--output rasterizer/jitter/builder_gen.h \
		--gen_h

rasterizer/jitter/builder_gen.cpp: rasterizer/jitter/scripts/gen_llvm_ir_macros.py $(LLVM_INCLUDEDIR)/llvm/IR/IRBuilder.h
	$(PYTHON2) $(PYTHON_FLAGS) \
		$(srcdir)/rasterizer/jitter/scripts/gen_llvm_ir_macros.py \
		--input $(LLVM_INCLUDEDIR)/llvm/IR/IRBuilder.h \
		--output rasterizer/jitter/builder_gen.cpp \
		--gen_cpp

rasterizer/jitter/builder_x86.h: rasterizer/jitter/scripts/gen_llvm_ir_macros.py
	$(PYTHON2) $(PYTHON_FLAGS) \
		$(srcdir)/rasterizer/jitter/scripts/gen_llvm_ir_macros.py \
		--output rasterizer/jitter/builder_x86.h \
		--gen_x86_h

rasterizer/jitter/builder_x86.cpp: rasterizer/jitter/scripts/gen_llvm_ir_macros.py
	$(PYTHON2) $(PYTHON_FLAGS) \
		$(srcdir)/rasterizer/jitter/scripts/gen_llvm_ir_macros.py \
		--output rasterizer/jitter/builder_x86.cpp \
		--gen_x86_cpp


COMMON_LIBADD = \
	$(top_builddir)/src/gallium/auxiliary/libgallium.la \
	$(top_builddir)/src/mesa/libmesagallium.la \
	$(LLVM_LIBS)

COMMON_LDFLAGS = $(LLVM_LDFLAGS)

lib_LTLIBRARIES = libswrAVX.la libswrAVX2.la

libswrAVX_la_CXXFLAGS = \
	-march=core-avx-i \
	-DKNOB_ARCH=KNOB_ARCH_AVX \
	$(COMMON_CXXFLAGS)

libswrAVX_la_SOURCES = \
	$(COMMON_SOURCES)

libswrAVX_la_LIBADD = \
	$(COMMON_LIBADD)

libswrAVX_la_LDFLAGS = \
	$(COMMON_LDFLAGS)

libswrAVX2_la_CXXFLAGS = \
	-march=core-avx2 \
	-DKNOB_ARCH=KNOB_ARCH_AVX2 \
	$(COMMON_CXXFLAGS)

libswrAVX2_la_SOURCES = \
	$(COMMON_SOURCES)

libswrAVX2_la_LIBADD = \
	$(COMMON_LIBADD)

libswrAVX2_la_LDFLAGS = \
	$(COMMON_LDFLAGS)

include $(top_srcdir)/install-gallium-links.mk
