/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#pragma once

#define _GLSL_SRC_VAR2(_line) _glsl_helpers_shader ## _line ## _glsl_src
#define _GLSL_SRC_VAR(_line) _GLSL_SRC_VAR2(_line)

#define GLSL_VK_SHADER(device, stage, ...) ({                           \
   VkShader __shader;                                                   \
   VkShaderCreateInfo __shader_create_info = {                          \
      .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,                    \
      .codeSize = sizeof(_GLSL_SRC_VAR(__LINE__)),                      \
      .pCode = _GLSL_SRC_VAR(__LINE__),                                 \
      .flags = (1 << 31) /* GLSL back-door hack */                      \
   };                                                                   \
   anv_CreateShader((VkDevice) device, &__shader_create_info, &__shader); \
   __shader;                                                            \
})
