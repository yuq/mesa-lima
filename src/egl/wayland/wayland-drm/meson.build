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

wayland_drm_protocol_c = custom_target(
  'wayland-drm-protocol.c',
  input : 'wayland-drm.xml',
  output : 'wayland-drm-protocol.c',
  command : [prog_wl_scanner, 'code', '@INPUT@', '@OUTPUT@'],
)

wayland_drm_client_protocol_h = custom_target(
  'wayland-drm-client-protocol.h',
  input : 'wayland-drm.xml',
  output : 'wayland-drm-client-protocol.h',
  command : [prog_wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
