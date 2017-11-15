if HAVE_GALLIUM_SUN4I

TARGET_DRIVERS += sun4i-drm
TARGET_CPPFLAGS += -DGALLIUM_SUN4I
TARGET_LIB_DEPS += \
    $(top_builddir)/src/gallium/winsys/sun4i/drm/libsun4idrm.la \
    $(LIBDRM_LIBS)

endif
