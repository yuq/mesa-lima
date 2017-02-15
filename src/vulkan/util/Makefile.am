vulkan_api_xml = $(top_srcdir)/src/vulkan/registry/vk.xml

AM_CPPFLAGS = \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/src

EXTRA_DIST = \
	gen_enum_to_str.py

BUILT_SOURCES = \
	vk_enum_to_str.c \
	vk_enum_to_str.h

vk_enum_to_str.c vk_enum_to_str.h: gen_enum_to_str.py $(vulkan_api_xml)
	$(AM_V_GEN)$(PYTHON2) $(srcdir)/gen_enum_to_str.py

noinst_LTLIBRARIES = libvulkan_util.la

libvulkan_util_la_SOURCES = \
	vk_enum_to_str.c \
	vk_enum_to_str.h

