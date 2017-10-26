# Copyright © 2017 Dylan Baker
# Copyright © 2017 Intel Corporation

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

inc_gallium_drivers = include_directories('drivers')
inc_gallium_winsys = include_directories('winsys')

subdir('auxiliary')
subdir('auxiliary/pipe-loader')
subdir('drivers/ddebug')
subdir('drivers/noop')
subdir('drivers/trace')
subdir('drivers/rbug')
subdir('winsys/sw/null')
subdir('winsys/sw/dri')
subdir('winsys/sw/kms-dri')
subdir('winsys/sw/wrapper')
if with_gallium_softpipe
  subdir('drivers/softpipe')
  if with_llvm
    subdir('drivers/llvmpipe')
  endif
endif
if with_gallium_r300 or with_gallium_radeonsi or with_gallium_r600
  subdir('winsys/radeon/drm')
endif
if with_gallium_r300
  subdir('drivers/r300')
endif
if with_gallium_r600
  subdir('drivers/r600')
endif
if with_gallium_radeonsi
  subdir('drivers/radeon')
  subdir('drivers/radeonsi')
  subdir('winsys/amdgpu/drm')
endif
if with_gallium_nouveau
  subdir('drivers/nouveau')
  subdir('winsys/nouveau/drm')
endif
if with_gallium_freedreno
  subdir('drivers/freedreno')
  subdir('winsys/freedreno/drm')
endif
if with_gallium_pl111
  subdir('winsys/pl111/drm')
endif
if with_gallium_vc4
  subdir('drivers/vc4')
  subdir('winsys/vc4/drm')
endif
if with_gallium_vc5
  subdir('drivers/vc5')
  subdir('winsys/vc5/drm')
endif
if with_gallium_etnaviv
  subdir('drivers/etnaviv')
  subdir('winsys/etnaviv/drm')
endif
if with_gallium_imx
  subdir('winsys/imx/drm')
endif
if with_gallium_i915
  subdir('winsys/i915/drm')
  subdir('drivers/i915')
endif
if with_gallium_svga
  subdir('drivers/svga')
  subdir('winsys/svga/drm')
endif
if with_gallium_virgl
  subdir('drivers/virgl')
  subdir('winsys/virgl/drm')
  subdir('winsys/virgl/vtest')
endif
if with_dri
  subdir('state_trackers/dri')
endif
if with_osmesa == 'gallium'
  subdir('state_trackers/osmesa')
endif
if with_glx == 'gallium-xlib'
  subdir('winsys/sw/xlib')
  subdir('state_trackers/glx/xlib')
endif
# TODO: SWR
# TODO: clover
if with_dri
  subdir('targets/dri')
endif
if with_osmesa == 'gallium'
  subdir('targets/osmesa')
endif
if with_glx == 'gallium-xlib'
  subdir('targets/libgl-xlib')
endif
# TODO: OMX
# TODO: VA
# TODO: vdpau
# TODO: xa
# TODO: xvmc
# TODO: nine
# TODO: tests
