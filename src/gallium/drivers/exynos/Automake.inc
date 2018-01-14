if HAVE_GALLIUM_EXYNOS

TARGET_DRIVERS += exynos
TARGET_CPPFLAGS += -DGALLIUM_EXYNOS
TARGET_LIB_DEPS += \
    $(top_builddir)/src/gallium/winsys/exynos/drm/libexynosdrm.la \
    $(LIBDRM_LIBS)

endif
