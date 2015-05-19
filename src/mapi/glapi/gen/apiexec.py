# Copyright (C) 2015 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

class exec_info():
    """Information relating GL APIs to a function.

    Each of the four attributes of this class, compatibility, core, es1, and
    es2, specify the minimum API version where a function can possibly exist
    in Mesa.  The version is specified as an integer of (real GL version *
    10).  For example, glCreateProgram was added in OpenGL 2.0, so
    compatibility=20 and core=31.

    If the attribute is None, then it cannot be supported by that
    API.  For example, glNewList was removed from core profiles, so
    compatibility=10 and core=None.

    Each of the attributes that is not None must have a valid value.  The
    valid ranges are:

        compatiblity: [10, 30]
        core: [31, )
        es1: [10, 11]
        es2: [20, )

    These ranges are enforced by the constructor.
    """
    def __init__(self, compatibility=None, core=None, es1=None, es2=None):
        if compatibility is not None:
            assert isinstance(compatibility, int)
            assert compatibility >= 10
            assert compatibility <= 30

        if core is not None:
            assert isinstance(core, int)
            assert core >= 31

        if es1 is not None:
            assert isinstance(es1, int)
            assert es1 == 10 or es1 == 11

        if es2 is not None:
            assert isinstance(es2, int)
            assert es2 >= 20

        self.compatibility = compatibility
        self.core = core
        self.es1 = es1
        self.es2 = es2

functions = {
    # OpenGL 3.1 / GL_ARB_texture_buffer_object.  Mesa only exposes this
    # extension with core profile.
    "TexBuffer": exec_info(core=31),

    # OpenGL 3.2 / GL_ARB_geometry_shader4.  Mesa does not support
    # GL_ARB_geometry_shader4, so OpenGL 3.2 is required.
    "FramebufferTexture": exec_info(core=32),

    # OpenGL 4.0 / GL_ARB_gpu_shader_fp64.  The extension spec says:
    #
    #     "OpenGL 3.2 and GLSL 1.50 are required."
    "Uniform1d": exec_info(core=32),
    "Uniform2d": exec_info(core=32),
    "Uniform3d": exec_info(core=32),
    "Uniform4d": exec_info(core=32),
    "Uniform1dv": exec_info(core=32),
    "Uniform2dv": exec_info(core=32),
    "Uniform3dv": exec_info(core=32),
    "Uniform4dv": exec_info(core=32),
    "UniformMatrix2dv": exec_info(core=32),
    "UniformMatrix3dv": exec_info(core=32),
    "UniformMatrix4dv": exec_info(core=32),
    "UniformMatrix2x3dv": exec_info(core=32),
    "UniformMatrix2x4dv": exec_info(core=32),
    "UniformMatrix3x2dv": exec_info(core=32),
    "UniformMatrix3x4dv": exec_info(core=32),
    "UniformMatrix4x2dv": exec_info(core=32),
    "UniformMatrix4x3dv": exec_info(core=32),
    "GetUniformdv": exec_info(core=32),

    # OpenGL 4.1 / GL_ARB_vertex_attrib_64bit.  The extension spec says:
    #
    #     "OpenGL 3.0 and GLSL 1.30 are required.
    #
    #     ARB_gpu_shader_fp64 (or equivalent functionality) is required."
    #
    # For Mesa this effectively means OpenGL 3.2 is required.  It seems
    # unlikely that Mesa will ever get support for any of the NV extensions
    # that add "equivalent functionality."
    "VertexAttribL1d": exec_info(core=32),
    "VertexAttribL2d": exec_info(core=32),
    "VertexAttribL3d": exec_info(core=32),
    "VertexAttribL4d": exec_info(core=32),
    "VertexAttribL1dv": exec_info(core=32),
    "VertexAttribL2dv": exec_info(core=32),
    "VertexAttribL3dv": exec_info(core=32),
    "VertexAttribL4dv": exec_info(core=32),
    "VertexAttribLPointer": exec_info(core=32),
    "GetVertexAttribLdv": exec_info(core=32),

    # OpenGL 4.1 / GL_ARB_viewport_array.  The extension spec says:
    #
    #     "OpenGL 3.2 or the EXT_geometry_shader4 or ARB_geometry_shader4
    #     extensions are required."
    #
    # Mesa does not support either of the geometry shader extensions, so
    # OpenGL 3.2 is required.
    "ViewportArrayv": exec_info(core=32),
    "ViewportIndexedf": exec_info(core=32),
    "ViewportIndexedfv": exec_info(core=32),
    "ScissorArrayv": exec_info(core=32),
    "ScissorIndexed": exec_info(core=32),
    "ScissorIndexedv": exec_info(core=32),
    "DepthRangeArrayv": exec_info(core=32),
    "DepthRangeIndexed": exec_info(core=32),
    # GetFloati_v also GL_ARB_shader_atomic_counters
    # GetDoublei_v also GL_ARB_shader_atomic_counters

    # OpenGL 4.3 / GL_ARB_texture_buffer_range.  Mesa can expose the extension
    # with OpenGL 3.1.
    "TexBufferRange": exec_info(core=31),
}
