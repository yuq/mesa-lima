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

class operation(object):
   def __init__(self, name, num_operands, printable_name = None):
      self.name = name
      self.num_operands = num_operands

      if printable_name is None:
         self.printable_name = name
      else:
         self.printable_name = printable_name


   def get_enum_name(self):
      return "ir_{}op_{}".format(("un", "bin", "tri", "quad")[self.num_operands-1], self.name)


ir_expression_operation = [
   operation("bit_not", 1, printable_name="~"),
   operation("logic_not", 1, printable_name="!"),
   operation("neg", 1),
   operation("abs", 1),
   operation("sign", 1),
   operation("rcp", 1),
   operation("rsq", 1),
   operation("sqrt", 1),
   operation("exp", 1),         # Log base e on gentype
   operation("log", 1),         # Natural log on gentype
   operation("exp2", 1),
   operation("log2", 1),
   operation("f2i", 1),         # Float-to-integer conversion.
   operation("f2u", 1),         # Float-to-unsigned conversion.
   operation("i2f", 1),         # Integer-to-float conversion.
   operation("f2b", 1),         # Float-to-boolean conversion
   operation("b2f", 1),         # Boolean-to-float conversion
   operation("i2b", 1),         # int-to-boolean conversion
   operation("b2i", 1),         # Boolean-to-int conversion
   operation("u2f", 1),         # Unsigned-to-float conversion.
   operation("i2u", 1),         # Integer-to-unsigned conversion.
   operation("u2i", 1),         # Unsigned-to-integer conversion.
   operation("d2f", 1),         # Double-to-float conversion.
   operation("f2d", 1),         # Float-to-double conversion.
   operation("d2i", 1),         # Double-to-integer conversion.
   operation("i2d", 1),         # Integer-to-double conversion.
   operation("d2u", 1),         # Double-to-unsigned conversion.
   operation("u2d", 1),         # Unsigned-to-double conversion.
   operation("d2b", 1),         # Double-to-boolean conversion.
   operation("bitcast_i2f", 1), # 'Bit-identical int-to-float "conversion"
   operation("bitcast_f2i", 1), # 'Bit-identical float-to-int "conversion"
   operation("bitcast_u2f", 1), # 'Bit-identical uint-to-float "conversion"
   operation("bitcast_f2u", 1), # 'Bit-identical float-to-uint "conversion"

   # Unary floating-point rounding operations.
   operation("trunc", 1),
   operation("ceil", 1),
   operation("floor", 1),
   operation("fract", 1),
   operation("round_even", 1),

   # Trigonometric operations.
   operation("sin", 1),
   operation("cos", 1),

   # Partial derivatives.
   operation("dFdx", 1),
   operation("dFdx_coarse", 1, printable_name="dFdxCoarse"),
   operation("dFdx_fine", 1, printable_name="dFdxFine"),
   operation("dFdy", 1),
   operation("dFdy_coarse", 1, printable_name="dFdyCoarse"),
   operation("dFdy_fine", 1, printable_name="dFdyFine"),

   # Floating point pack and unpack operations.
   operation("pack_snorm_2x16", 1, printable_name="packSnorm2x16"),
   operation("pack_snorm_4x8", 1, printable_name="packSnorm4x8"),
   operation("pack_unorm_2x16", 1, printable_name="packUnorm2x16"),
   operation("pack_unorm_4x8", 1, printable_name="packUnorm4x8"),
   operation("pack_half_2x16", 1, printable_name="packHalf2x16"),
   operation("unpack_snorm_2x16", 1, printable_name="unpackSnorm2x16"),
   operation("unpack_snorm_4x8", 1, printable_name="unpackSnorm4x8"),
   operation("unpack_unorm_2x16", 1, printable_name="unpackUnorm2x16"),
   operation("unpack_unorm_4x8", 1, printable_name="unpackUnorm4x8"),
   operation("unpack_half_2x16", 1, printable_name="unpackHalf2x16"),

   # Bit operations, part of ARB_gpu_shader5.
   operation("bitfield_reverse", 1),
   operation("bit_count", 1),
   operation("find_msb", 1),
   operation("find_lsb", 1),

   operation("saturate", 1, printable_name="sat"),

   # Double packing, part of ARB_gpu_shader_fp64.
   operation("pack_double_2x32", 1, printable_name="packDouble2x32"),
   operation("unpack_double_2x32", 1, printable_name="unpackDouble2x32"),

   operation("frexp_sig", 1),
   operation("frexp_exp", 1),

   operation("noise", 1),

   operation("subroutine_to_int", 1),

   # Interpolate fs input at centroid
   #
   # operand0 is the fs input.
   operation("interpolate_at_centroid", 1),

   # Ask the driver for the total size of a buffer block.
   # operand0 is the ir_constant buffer block index in the linked shader.
   operation("get_buffer_size", 1),

   # Calculate length of an unsized array inside a buffer block.
   # This opcode is going to be replaced in a lowering pass inside
   # the linker.
   #
   # operand0 is the unsized array's ir_value for the calculation
   # of its length.
   operation("ssbo_unsized_array_length", 1),

   # Vote among threads on the value of the boolean argument.
   operation("vote_any", 1),
   operation("vote_all", 1),
   operation("vote_eq", 1),

   operation("add", 2, printable_name="+"),
   operation("sub", 2, printable_name="-"),
   # "Floating-point or low 32-bit integer multiply."
   operation("mul", 2, printable_name="*"),
   operation("imul_high", 2),       # Calculates the high 32-bits of a 64-bit multiply.
   operation("div", 2, printable_name="/"),

   # Returns the carry resulting from the addition of the two arguments.
   operation("carry", 2),

   # Returns the borrow resulting from the subtraction of the second argument
   # from the first argument.
   operation("borrow", 2),

   # Either (vector % vector) or (vector % scalar)
   operation("mod", 2, printable_name="%"),

   # Binary comparison operators which return a boolean vector.
   # The type of both operands must be equal.
   operation("less", 2, printable_name="<"),
   operation("greater", 2, printable_name=">"),
   operation("lequal", 2, printable_name="<="),
   operation("gequal", 2, printable_name=">="),
   operation("equal", 2, printable_name="=="),
   operation("nequal", 2, printable_name="!="),

   # Returns single boolean for whether all components of operands[0]
   # equal the components of operands[1].
   operation("all_equal", 2),

   # Returns single boolean for whether any component of operands[0]
   # is not equal to the corresponding component of operands[1].
   operation("any_nequal", 2),

   # Bit-wise binary operations.
   operation("lshift", 2, printable_name="<<"),
   operation("rshift", 2, printable_name=">>"),
   operation("bit_and", 2, printable_name="&"),
   operation("bit_xor", 2, printable_name="^"),
   operation("bit_or", 2, printable_name="|"),

   operation("logic_and", 2, printable_name="&&"),
   operation("logic_xor", 2, printable_name="^^"),
   operation("logic_or", 2, printable_name="||"),

   operation("dot", 2),
   operation("min", 2),
   operation("max", 2),

   operation("pow", 2),

   # Load a value the size of a given GLSL type from a uniform block.
   #
   # operand0 is the ir_constant uniform block index in the linked shader.
   # operand1 is a byte offset within the uniform block.
   operation("ubo_load", 2),

   # Multiplies a number by two to a power, part of ARB_gpu_shader5.
   operation("ldexp", 2),

   # Extract a scalar from a vector
   #
   # operand0 is the vector
   # operand1 is the index of the field to read from operand0
   operation("vector_extract", 2),

   # Interpolate fs input at offset
   #
   # operand0 is the fs input
   # operand1 is the offset from the pixel center
   operation("interpolate_at_offset", 2),

   # Interpolate fs input at sample position
   #
   # operand0 is the fs input
   # operand1 is the sample ID
   operation("interpolate_at_sample", 2),

   # Fused floating-point multiply-add, part of ARB_gpu_shader5.
   operation("fma", 3),

   operation("lrp", 3),

   # Conditional Select
   #
   # A vector conditional select instruction (like ?:, but operating per-
   # component on vectors).
   #
   # See also lower_instructions_visitor::ldexp_to_arith
   operation("csel", 3),

   operation("bitfield_extract", 3),

   # Generate a value with one field of a vector changed
   #
   # operand0 is the vector
   # operand1 is the value to write into the vector result
   # operand2 is the index in operand0 to be modified
   operation("vector_insert", 3),

   operation("bitfield_insert", 4),

   operation("vector", 4),
]


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
   ${item.get_enum_name()},
% endfor

   /* Sentinels marking the last of each kind of operation. */
% for item in lasts:
   ir_last_${("un", "bin", "tri", "quad")[item.num_operands - 1]}op = ${item.get_enum_name()},
% endfor
   ir_last_opcode = ir_quadop_${lasts[3].name}
};""")

   strings_template = mako.template.Template(copyright + """
const char *const ir_expression_operation_strings[] = {
% for item in values:
   "${item.printable_name}",
% endfor
};""")

   if sys.argv[1] == "enum":
      lasts = [None, None, None, None]
      for item in reversed(ir_expression_operation):
         i = item.num_operands - 1
         if lasts[i] is None:
            lasts[i] = item

      print(enum_template.render(values=ir_expression_operation,
                                 lasts=lasts))
   elif sys.argv[1] == "strings":
      print(strings_template.render(values=ir_expression_operation))
