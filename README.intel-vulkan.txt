Intel's Open Source Vulkan Driver
Vulkan API Version: 1.0.2
SPIR-V Version: 1.0

Intro
=====
The Open Source Technology Center 3D graphics team at Intel has
been working on a Vulkan implementation based on the Mesa open source
OpenGL implementation.

The Mesa project source and our driver implementation is under the MIT
license [1], but is also covered by the Khronos IP framework as it
pertains to a specification under construction [2].

We welcome all feedback and contibutions, as long as the contributions
are MIT licensed and can be open sourced with the driver.

[1] https://opensource.org/licenses/MIT
[2] https://www.khronos.org/members/ip-framework


Maintainers
===========
Kristian Høgsberg Kristensen <kristian.h.kristensen@intel.com>
Jason Ekstrand <jason.ekstrand@intel.com>
Chad Versace <chad.versace@intel.com>


Supported Hardware
==================
- Broadwell & Sky Lake, main development focus
- Ivybridge
- Haswell
- Bay Trail
- Cherryview
- Broxton


Conformance
===========
First-wave conformance has been submitted for Broadwell, Sky Lake, and
Cherryview.  They all pass 100% of the mustpass tests as of January 30,
2016.


Supported OS Platforms
======================
 - Linux, tested on Fedora 22 with kernel >= 4.1
     - X11 with DRI3
     - Wayland
 - Android
     - TODO


Building and Installing
=======================
This driver is intended to be used directly from the build tree. Installing the
driver into a system location is not yet fully supported. If you require support
for system-wide installation, please contact a maintainer.

Throughout the instructions, MESA_TOP refers to the top of the Mesa repository.

First, install the usual dependencies needed to build Mesa.

        Fedora:
            $ sudo yum builddep mesa
        Ubunutu:
            $ FINISHME

Next, configure and build. The below commands will build Mesa in release mode.
If you wish to build Mesa in debug mode, add option '--enable-debug' to the
configure command.

        $ cd $MESA_TOP
        $ autoreconf -vfi
        $ ./configure --with-dri-drivers=i965 --with-gallium-drivers=
        $ make

To use the driver's libvulkan.so directly, without LunarG's loader, you must set
an environment variable before running your Vulkan application:

        $ export LD_LIBRARY_PATH="$MESA_TOP/lib"
        $ your-vk-app

Alternatively, to use the driver with LunarG's loader:

        $ export VK_ICD_FILENAMES="$MESA_TOP/src/vulkan/anv_icd.json"
        $ your-vk-app


File Structure and Naming
=========================
The core code of Intel's Mesa Vulkan driver lives in src/vulkan. Files prefixed
with "gen8" support Broadwell; files prefixed with "gen7" support Ivybridge;
files prefixed with "anv" are common to all hardware generations.

Mesa is an umbrella open source project containing many drivers for multiple
APIs. The codename for Intel's Mesa Vulkan driver is "Anvil", hence the filename
prefix "anv".


Feature Status
==============
The driver is still a work-in-progress. We do our best to keep the below list of
features up-to-date.

Supported Features:
  - Index buffers, instanced draw, indirect draw
  - Nested command buffers
  - Consumes SPIR-V (no GLSL "backdoor")
  - Fragment, vertex, geometry, and compute shaders
  - Uniform buffers, sampled images, dynamic uniform buffers
  - Shader storage buffers
  - Push constants
  - Color, depth and stencil attachments
  - 1D, 2D, 3D textures, texture arrays
  - Memory barrier
  - Optionally integrates with LunarGs loader
  - WSI extension for X11
  - Fences
  - Most copy/blit commands for color and depth buffers,
    vkCmdCopyImageToBuffer for stencil buffers
  - Occlution query and timestamps
  - VkkSemaphore and VkEvent
  - Shader specialization
  - Storage images
  - MSAA

Unsupported Features:
   - Tesselation shaders
   - Push constants in GS and VS on HSW and prior
   - Sparse resources
   - Input attachments
