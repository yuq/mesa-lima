if HAVE_GALLIUM_ROCKCHIP

TARGET_DRIVERS += rockchip
TARGET_CPPFLAGS += -DGALLIUM_ROCKCHIP
TARGET_LIB_DEPS += \
    $(top_builddir)/src/gallium/winsys/rockchip/drm/librockchipdrm.la \
    $(LIBDRM_LIBS)

endif
