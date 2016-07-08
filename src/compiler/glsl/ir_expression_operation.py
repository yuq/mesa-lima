#! /usr/bin/env python
#
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

import mako.template
import sys

ir_expression_operation = [
   # Name        operands  string
   ("bit_not", 1, "~"),
   ("logic_not", 1, "!"),
   ("neg", 1, None),
   ("abs", 1, None),
   ("sign", 1, None),
   ("rcp", 1, None),
   ("rsq", 1, None),
   ("sqrt", 1, None),
   ("exp", 1, None),         # Log base e on gentype
   ("log", 1, None),         # Natural log on gentype
   ("exp2", 1, None),
   ("log2", 1, None),
   ("f2i", 1, None),         # Float-to-integer conversion.
   ("f2u", 1, None),         # Float-to-unsigned conversion.
   ("i2f", 1, None),         # Integer-to-float conversion.
   ("f2b", 1, None),         # Float-to-boolean conversion
   ("b2f", 1, None),         # Boolean-to-float conversion
   ("i2b", 1, None),         # int-to-boolean conversion
   ("b2i", 1, None),         # Boolean-to-int conversion
   ("u2f", 1, None),         # Unsigned-to-float conversion.
   ("i2u", 1, None),         # Integer-to-unsigned conversion.
   ("u2i", 1, None),         # Unsigned-to-integer conversion.
   ("d2f", 1, None),         # Double-to-float conversion.
   ("f2d", 1, None),         # Float-to-double conversion.
   ("d2i", 1, None),         # Double-to-integer conversion.
   ("i2d", 1, None),         # Integer-to-double conversion.
   ("d2u", 1, None),         # Double-to-unsigned conversion.
   ("u2d", 1, None),         # Unsigned-to-double conversion.
   ("d2b", 1, None),         # Double-to-boolean conversion.
   ("bitcast_i2f", 1, None), # 'Bit-identical int-to-float "conversion"
   ("bitcast_f2i", 1, None), # 'Bit-identical float-to-int "conversion"
   ("bitcast_u2f", 1, None), # 'Bit-identical uint-to-float "conversion"
   ("bitcast_f2u", 1, None), # 'Bit-identical float-to-uint "conversion"

   # Unary floating-point rounding operations.
   ("trunc", 1, None),
   ("ceil", 1, None),
   ("floor", 1, None),
   ("fract", 1, None),
   ("round_even", 1, None),

   # Trigonometric operations.
   ("sin", 1, None),
   ("cos", 1, None),

   # Partial derivatives.
   ("dFdx", 1, None),
   ("dFdx_coarse", 1, "dFdxCoarse"),
   ("dFdx_fine", 1, "dFdxFine"),
   ("dFdy", 1, None),
   ("dFdy_coarse", 1, "dFdyCoarse"),
   ("dFdy_fine", 1, "dFdyFine"),

   # Floating point pack and unpack operations.
   ("pack_snorm_2x16", 1, "packSnorm2x16"),
   ("pack_snorm_4x8", 1, "packSnorm4x8"),
   ("pack_unorm_2x16", 1, "packUnorm2x16"),
   ("pack_unorm_4x8", 1, "packUnorm4x8"),
   ("pack_half_2x16", 1, "packHalf2x16"),
   ("unpack_snorm_2x16", 1, "unpackSnorm2x16"),
   ("unpack_snorm_4x8", 1, "unpackSnorm4x8"),
   ("unpack_unorm_2x16", 1, "unpackUnorm2x16"),
   ("unpack_unorm_4x8", 1, "unpackUnorm4x8"),
   ("unpack_half_2x16", 1, "unpackHalf2x16"),

   # Bit operations, part of ARB_gpu_shader5.
   ("bitfield_reverse", 1, None),
   ("bit_count", 1, None),
   ("find_msb", 1, None),
   ("find_lsb", 1, None),

   ("saturate", 1, "sat"),

   # Double packing, part of ARB_gpu_shader_fp64.
   ("pack_double_2x32", 1, "packDouble2x32"),
   ("unpack_double_2x32", 1, "unpackDouble2x32"),

   ("frexp_sig", 1, None),
   ("frexp_exp", 1, None),

   ("noise", 1, None),

   ("subroutine_to_int", 1, None),

   # Interpolate fs input at centroid
   #
   # operand0 is the fs input.
   ("interpolate_at_centroid", 1, None),

   # Ask the driver for the total size of a buffer block.
   # operand0 is the ir_constant buffer block index in the linked shader.
   ("get_buffer_size", 1, None),

   # Calculate length of an unsized array inside a buffer block.
   # This opcode is going to be replaced in a lowering pass inside
   # the linker.
   #
   # operand0 is the unsized array's ir_value for the calculation
   # of its length.
   ("ssbo_unsized_array_length", 1, None),

   # Vote among threads on the value of the boolean argument.
   ("vote_any", 1, None),
   ("vote_all", 1, None),
   ("vote_eq", 1, None),

   ("add", 2, "+"),
   ("sub", 2, "-"),
   ("mul", 2, "*"),        # "Floating-point or low 32-bit integer multiply."
   ("imul_high", 2, None), # Calculates the high 32-bits of a 64-bit multiply.
   ("div", 2, "/"),

   # Returns the carry resulting from the addition of the two arguments.
   ("carry", 2, None),

   # Returns the borrow resulting from the subtraction of the second argument
   # from the first argument.
   ("borrow", 2, None),

   # Takes one of two combinations of arguments:
   #
   # - mod(vecN, vecN)
   # - mod(vecN, float)
   #
   # Does not take integer types.
   ("mod", 2, "%"),

   # Binary comparison operators which return a boolean vector.
   # The type of both operands must be equal.
   ("less", 2, "<"),
   ("greater", 2, ">"),
   ("lequal", 2, "<="),
   ("gequal", 2, ">="),
   ("equal", 2, "=="),
   ("nequal", 2, "!="),

   # Returns single boolean for whether all components of operands[0]
   # equal the components of operands[1].
   ("all_equal", 2, None),

   # Returns single boolean for whether any component of operands[0]
   # is not equal to the corresponding component of operands[1].
   ("any_nequal", 2, None),

   # Bit-wise binary operations.
   ("lshift", 2, "<<"),
   ("rshift", 2, ">>"),
   ("bit_and", 2, "&"),
   ("bit_xor", 2, "^"),
   ("bit_or", 2, "|"),

   ("logic_and", 2, "&&"),
   ("logic_xor", 2, "^^"),
   ("logic_or", 2, "||"),

   ("dot", 2, None),
   ("min", 2, None),
   ("max", 2, None),

   ("pow", 2, None),

   # Load a value the size of a given GLSL type from a uniform block.
   #
   # operand0 is the ir_constant uniform block index in the linked shader.
   # operand1 is a byte offset within the uniform block.
   ("ubo_load", 2, None),

   # Multiplies a number by two to a power, part of ARB_gpu_shader5.
   ("ldexp", 2, None),

   # Extract a scalar from a vector
   #
   # operand0 is the vector
   # operand1 is the index of the field to read from operand0
   ("vector_extract", 2, None),

   # Interpolate fs input at offset
   #
   # operand0 is the fs input
   # operand1 is the offset from the pixel center
   ("interpolate_at_offset", 2, None),

   # Interpolate fs input at sample position
   #
   # operand0 is the fs input
   # operand1 is the sample ID
   ("interpolate_at_sample", 2, None),

   # Fused floating-point multiply-add, part of ARB_gpu_shader5.
   ("fma", 3, None),

   ("lrp", 3, None),

   # Conditional Select
   #
   # A vector conditional select instruction (like ?:, but operating per-
   # component on vectors).
   #
   # See also lower_instructions_visitor::ldexp_to_arith
   ("csel", 3, None),

   ("bitfield_extract", 3, None),

   # Generate a value with one field of a vector changed
   #
   # operand0 is the vector
   # operand1 is the value to write into the vector result
   # operand2 is the index in operand0 to be modified
   ("vector_insert", 3, None),

   ("bitfield_insert", 4, None),

   ("vector", 4, None),
]

def name_from_item(item):
   return "ir_{}op_{}".format(("un", "bin", "tri", "quad")[item[1]-1], item[0])

if __name__ == "__main__":
   copyright = """/*
 * Copyright (C) 2010 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
"""
   enum_template = mako.template.Template(copyright + """
enum ir_expression_operation {
% for item in values:
   ${name_from_item(item)},
% endfor

   /* Sentinels marking the last of each kind of operation. */
% for (name, i) in lasts:
   ir_last_${("un", "bin", "tri", "quad")[i]}op = ${name_from_item((name, i+1))},
% endfor
   ir_last_opcode = ir_quadop_${lasts[3][0]}
};""")

   strings_template = mako.template.Template(copyright + """
const char *const ir_expression_operation_strings[] = {
% for item in values:
   "${item[2] if item[2] is not None else item[0]}",
% endfor
};""")

   if sys.argv[1] == "enum":
      lasts = [None, None, None, None]
      for item in reversed(ir_expression_operation):
         i = item[1] - 1
         if lasts[i] is None:
            lasts[i] = (item[0], i)

      print(enum_template.render(values=ir_expression_operation,
                                 lasts=lasts,
                                 name_from_item=name_from_item))
   elif sys.argv[1] == "strings":
      print(strings_template.render(values=ir_expression_operation,
                                    name_from_item=name_from_item))
