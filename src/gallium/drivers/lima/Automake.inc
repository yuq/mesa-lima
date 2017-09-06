if HAVE_GALLIUM_LIMA

TARGET_DRIVERS += lima
TARGET_CPPFLAGS += -DGALLIUM_LIMA
TARGET_LIB_DEPS += \
	$(top_builddir)/src/gallium/winsys/lima/drm/liblimadrm.la \
	$(top_builddir)/src/gallium/drivers/lima/liblima.la

endif
