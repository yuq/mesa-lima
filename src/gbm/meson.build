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

inc_gbm = include_directories('.', 'backends/dri')

files_gbm = files(
  'main/backend.c',
  'main/backend.h',
  'main/gbm.c',
  'main/gbm.h',
  'main/gbmint.h',
)
deps_gbm = []
args_gbm = []
links_gbm = []
deps_gbm = []

if with_dri2
  files_gbm += files('backends/dri/gbm_dri.c', 'backends/dri/gbm_driint.h')
  deps_gbm += [dep_libdrm, dep_thread]
  args_gbm += '-DDEFAULT_DRIVER_DIR="@0@"'.format(dri_driver_dir)
endif
if with_platform_wayland
  deps_gbm += dep_wayland_server
  links_gbm += libwayland_drm
endif

# TODO: wayland support (requires egl)

libgbm = shared_library(
  'gbm',
  files_gbm,
  include_directories : [
    include_directories('main'), inc_include, inc_src, inc_loader,
    include_directories('../egl/wayland/wayland-drm')],
  c_args : args_gbm,
  link_args : [ld_args_gc_sections],
  link_with : [links_gbm, libloader, libmesa_util, libxmlconfig],
  dependencies : [deps_gbm, dep_dl],
  version : '1.0',
  install : true,
)

install_headers('main/gbm.h')

pkg.generate(
  name : 'gbm',
  filebase : 'gbm',
  description : 'Mesa gbm library',
  version : meson.project_version(),
  libraries : libgbm,
  libraries_private : '-ldl',  # FIXME: autotools lists this a incomplete
)

if with_tests
  gbm_symbols_check = find_program('gbm-symbols-check')
  test('gbm-symbols-check', gbm_symbols_check)
endif
