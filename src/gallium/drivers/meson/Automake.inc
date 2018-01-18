if HAVE_GALLIUM_MESON

TARGET_DRIVERS += meson
TARGET_CPPFLAGS += -DGALLIUM_MESON
TARGET_LIB_DEPS += \
    $(top_builddir)/src/gallium/winsys/meson/drm/libmesondrm.la \
    $(LIBDRM_LIBS)

endif
