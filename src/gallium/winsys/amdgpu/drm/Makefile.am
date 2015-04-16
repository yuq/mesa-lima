include Makefile.sources
include $(top_srcdir)/src/gallium/Automake.inc

AM_CFLAGS = \
	$(GALLIUM_WINSYS_CFLAGS) \
	$(AMDGPU_CFLAGS)

AM_CXXFLAGS = $(AM_CFLAGS)

noinst_LTLIBRARIES = libamdgpuwinsys.la

libamdgpuwinsys_la_SOURCES = $(C_SOURCES)
