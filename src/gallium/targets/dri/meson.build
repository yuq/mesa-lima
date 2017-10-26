# Copyright © 2017 Dylan Baker

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# TODO: support non-static targets
# Static targets are always enabled in autotools (unless you modify
# configure.ac)

gallium_dri_c_args = [
  '-DGALLIUM_DDEBUG',
  '-DGALLIUM_NOOP',
  '-DGALLIUM_RBUG',
  '-DGALLIUME_TRACE',
]
gallium_dri_ld_args = []
gallium_dri_link_with = []
gallium_dri_depends = []
gallium_dri_link_depends = []
gallium_dri_drivers = []
gallium_dri_sources = []

if with_ld_version_script
  gallium_dri_ld_args += ['-Wl,--version-script', join_paths(meson.current_source_dir(), 'dri.sym')]
  gallium_dri_link_depends += files('dri.sym')
endif
if with_ld_dynamic_list
  gallium_dri_ld_args += ['-Wl,--dynamic-list', join_paths(meson.current_source_dir(), '../dri-vdpau.dyn')]
  gallium_dri_link_depends += files('../dri-vdpau.dyn')
endif

if with_dri
  gallium_dri_link_with += libswdri
endif
if with_gallium_drisw_kms
  gallium_dri_link_with += libswkmsdri
endif

if with_gallium_pl111
  gallium_dri_c_args += '-DGALLIUM_PL111'
  gallium_dri_link_with += [libpl111winsys]
  gallium_dri_drivers += 'pl111_dri.so'
endif
if with_gallium_radeonsi
  gallium_dri_c_args += '-DGALLIUM_RADEONSI'
  gallium_dri_link_with += [
    libradeonsi, libamdgpuwinsys, libradeon, libamd_common,
  ]
  gallium_dri_drivers += 'radeonsi_dri.so'
  gallium_dri_sources += [si_driinfo_h]
endif
if with_gallium_nouveau
  gallium_dri_c_args += '-DGALLIUM_NOUVEAU'
  gallium_dri_link_with += [libnouveauwinsys, libnouveau]
  gallium_dri_drivers += 'nouveau_dri.so'
endif
if with_gallium_freedreno
  gallium_dri_c_args += '-DGALLIUM_FREEDRENO'
  gallium_dri_link_with += [libfreedrenowinsys, libfreedreno]
  gallium_dri_drivers += 'msm_dri.so'
  gallium_dri_drivers += 'kgsl_dri.so'
endif
if with_gallium_softpipe
  gallium_dri_c_args += '-DGALLIUM_SOFTPIPE'
  gallium_dri_link_with += libsoftpipe
  gallium_dri_drivers += 'swrast_dri.so'
  if with_gallium_drisw_kms
    gallium_dri_drivers += 'kms_swrast_dri.so'
  endif
  if with_llvm
    gallium_dri_c_args += '-DGALLIUM_LLVMPIPE'
    gallium_dri_link_with += libllvmpipe
  endif
endif
if with_gallium_vc4
  gallium_dri_c_args += '-DGALLIUM_VC4'
  gallium_dri_link_with += [libvc4, libvc4winsys]
  gallium_dri_drivers += 'vc4_dri.so'
endif
if with_gallium_vc5
  gallium_dri_c_args += '-DGALLIUM_VC5'
  gallium_dri_link_with += [libvc5, libvc5winsys, libbroadcom_vc5]
  gallium_dri_drivers += 'vc5_dri.so'
endif
if with_gallium_etnaviv
  gallium_dri_c_args += '-DGALLIUM_ETNAVIV'
  gallium_dri_link_with += [libetnaviv, libetnavivdrm]
  gallium_dri_drivers += 'etnaviv_dri.so'
endif
if with_gallium_imx
  gallium_dri_c_args += '-DGALLIUM_IMX'
  gallium_dri_link_with += libimxdrm
  gallium_dri_drivers += 'imx-drm_dri.so'
endif
if with_gallium_i915
  gallium_dri_c_args += '-DGALLIUM_I915'
  gallium_dri_link_with += [libi915, libi915drm]
  gallium_dri_drivers += 'i915_dri.so'
endif
if with_gallium_r300
  gallium_dri_c_args += '-DGALLIUM_R300'
  gallium_dri_link_with += libr300
  gallium_dri_drivers += 'r300_dri.so'
endif
if with_gallium_r600
  gallium_dri_c_args += '-DGALLIUM_R600'
  gallium_dri_link_with += libr600
  gallium_dri_drivers += 'r600_dri.so'
endif
if with_gallium_svga
  gallium_dri_c_args += '-DGALLIUM_VMWGFX'
  gallium_dri_link_with += [libsvga, libsvgadrm]
  gallium_dri_drivers += 'vmwgfx_dri.so'
endif
if with_gallium_virgl
  gallium_dri_c_args += '-DGALLIUM_VIRGL'
  gallium_dri_link_with += [libvirgl, libvirgldrm, libvirglvtest]
  gallium_dri_drivers += 'virtio_gpu_dri.so'
endif

if with_gallium_radeonsi or with_gallium_r300 or with_gallium_r600
  gallium_dri_link_with += libradeonwinsys
endif

if with_gallium_vc4 or with_gallium_vc5
  gallium_dri_link_with += libbroadcom_cle
endif

if with_gallium_vc4 or with_gallium_vc5 or with_gallium_radeonsi
  gallium_dri_link_with += libnir
endif

libgallium_dri = shared_library(
  'gallium_dri',
  [files('target.c'), gallium_dri_sources],
  include_directories : [
    inc_common, inc_util, inc_dri_common, inc_gallium_drivers,
    inc_gallium_winsys, include_directories('../../state_trackers/dri'),
  ],
  c_args : [c_vis_args, gallium_dri_c_args],
  cpp_args : [cpp_vis_args],
  link_args : [ld_args_gc_sections, gallium_dri_ld_args],
  link_depends : gallium_dri_link_depends,
  link_with : [
    libmesa_gallium, libdricommon, libmegadriver_stub, libdri, libgalliumvl,
    libgallium, libddebug, libnoop, librbug, libtrace, libglapi,
    libpipe_loader_static, libws_null, libwsw,  gallium_dri_link_with,
  ],
  dependencies : [
    gallium_dri_depends, dep_selinux, dep_expat, dep_libdrm, dep_llvm,
    dep_thread,
  ],
)

meson.add_install_script(
  join_paths(meson.source_root(), 'bin/install_megadrivers.py'),
  libgallium_dri.full_path(),
  dri_drivers_path,
  gallium_dri_drivers,
)
