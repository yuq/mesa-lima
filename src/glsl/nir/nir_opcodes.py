#! /usr/bin/env python
#
# Copyright (C) 2014 Connor Abbott
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
#
# Authors:
#    Connor Abbott (cwabbott0@gmail.com)

# Class that represents all the information we have about the opcode
# NOTE: this must be kept in sync with nir_op_info

class Opcode(object):
   """Class that represents all the information we have about the opcode
   NOTE: this must be kept in sync with nir_op_info
   """
   def __init__(self, name, output_size, output_type, input_sizes,
                input_types, algebraic_properties):
      """Parameters:

      - name is the name of the opcode (prepend nir_op_ for the enum name)
      - all types are strings that get nir_type_ prepended to them
      - input_types is a list of types
      - algebraic_properties is a space-seperated string, where nir_op_is_ is
        prepended before each entry
      """
      assert isinstance(name, str)
      assert isinstance(output_size, int)
      assert isinstance(output_type, str)
      assert isinstance(input_sizes, list)
      assert isinstance(input_sizes[0], int)
      assert isinstance(input_types, list)
      assert isinstance(input_types[0], str)
      assert isinstance(algebraic_properties, str)
      assert len(input_sizes) == len(input_types)
      assert 0 <= output_size <= 4
      for size in input_sizes:
         assert 0 <= size <= 4
         if output_size != 0:
            assert size != 0
      self.name = name
      self.num_inputs = len(input_sizes)
      self.output_size = output_size
      self.output_type = output_type
      self.input_sizes = input_sizes
      self.input_types = input_types
      self.algebraic_properties = algebraic_properties

# helper variables for strings
tfloat = "float"
tint = "int"
tbool = "bool"
tunsigned = "unsigned"

commutative = "commutative "
associative = "associative "

# global dictionary of opcodes
opcodes = {}

def opcode(name, output_size, output_type, input_sizes, input_types,
           algebraic_properties):
   assert name not in opcodes
   opcodes[name] = Opcode(name, output_size, output_type, input_sizes,
                          input_types, algebraic_properties)

def unop_convert(name, in_type, out_type):
   opcode(name, 0, out_type, [0], [in_type], "")

def unop(name, ty):
   opcode(name, 0, ty, [0], [ty], "")

def unop_horiz(name, output_size, output_type, input_size, input_type):
   opcode(name, output_size, output_type, [input_size], [input_type], "")

def unop_reduce(name, output_size, output_type, input_type):
   unop_horiz(name + "2", output_size, output_type, 2, input_type)
   unop_horiz(name + "3", output_size, output_type, 3, input_type)
   unop_horiz(name + "4", output_size, output_type, 4, input_type)


# These two move instructions differ in what modifiers they support and what
# the negate modifier means. Otherwise, they are identical.
unop("fmov", tfloat)
unop("imov", tint)

unop("ineg", tint)
unop("fneg", tfloat)
unop("inot", tint) # invert every bit of the integer
unop("fnot", tfloat) # (src == 0.0) ? 1.0 : 0.0
unop("fsign", tfloat)
unop("isign", tint)
unop("iabs", tint)
unop("fabs", tfloat)
unop("fsat", tfloat)
unop("frcp", tfloat)
unop("frsq", tfloat)
unop("fsqrt", tfloat)
unop("fexp", tfloat) # < e^x
unop("flog", tfloat) # log base e
unop("fexp2", tfloat)
unop("flog2", tfloat)
unop_convert("f2i", tfloat, tint) # Float-to-integer conversion.
unop_convert("f2u", tfloat, tunsigned) # Float-to-unsigned conversion
unop_convert("i2f", tint, tfloat) # Integer-to-float conversion.
unop_convert("f2b", tfloat, tbool) # Float-to-boolean conversion
unop_convert("b2f", tbool, tfloat) # Boolean-to-float conversion
unop_convert("i2b", tint, tbool) # int-to-boolean conversion
unop_convert("b2i", tbool, tint) # Boolean-to-int conversion
unop_convert("u2f", tunsigned, tfloat) #Unsigned-to-float conversion.

unop_reduce("bany", 1, tbool, tbool) # returns ~0 if any component of src[0] != 0
unop_reduce("ball", 1, tbool, tbool) # returns ~0 if all components of src[0] != 0
unop_reduce("fany", 1, tfloat, tfloat) # returns 1.0 if any component of src[0] != 0
unop_reduce("fall", 1, tfloat, tfloat) # returns 1.0 if all components of src[0] != 0

# Unary floating-point rounding operations.


unop("ftrunc", tfloat)
unop("fceil", tfloat)
unop("ffloor", tfloat)
unop("ffract", tfloat)
unop("fround_even", tfloat)


# Trigonometric operations.


unop("fsin", tfloat)
unop("fcos", tfloat)
unop("fsin_reduced", tfloat)
unop("fcos_reduced", tfloat)


# Partial derivatives.


unop("fddx", tfloat)
unop("fddy", tfloat)
unop("fddx_fine", tfloat)
unop("fddy_fine", tfloat)
unop("fddx_coarse", tfloat)
unop("fddy_coarse", tfloat)


# Floating point pack and unpack operations.


unop_horiz("pack_snorm_2x16", 1, tunsigned, 2, tfloat)
unop_horiz("pack_snorm_4x8", 1, tunsigned, 4, tfloat)
unop_horiz("pack_unorm_2x16", 1, tunsigned, 2, tfloat)
unop_horiz("pack_unorm_4x8", 1, tunsigned, 4, tfloat)
unop_horiz("pack_half_2x16", 1, tunsigned, 2, tfloat)
unop_horiz("unpack_snorm_2x16", 2, tfloat, 1, tunsigned)
unop_horiz("unpack_snorm_4x8", 4, tfloat, 1, tunsigned)
unop_horiz("unpack_unorm_2x16", 2, tfloat, 1, tunsigned)
unop_horiz("unpack_unorm_4x8", 4, tfloat, 1, tunsigned)
unop_horiz("unpack_half_2x16", 2, tfloat, 1, tunsigned)


# Lowered floating point unpacking operations.


unop_horiz("unpack_half_2x16_split_x", 1, tfloat, 1, tunsigned)
unop_horiz("unpack_half_2x16_split_y", 1, tfloat, 1, tunsigned)


# Bit operations, part of ARB_gpu_shader5.


unop("bitfield_reverse", tunsigned)
unop("bit_count", tunsigned)
unop_convert("ufind_msb", tunsigned, tint)
unop("ifind_msb", tint)
unop("find_lsb", tint)


for i in xrange(1, 5):
   for j in xrange(1, 5):
      unop_horiz("fnoise{0}_{1}".format(i, j), i, tfloat, j, tfloat)

def binop_convert(name, out_type, in_type, alg_props):
   opcode(name, 0, out_type, [0, 0], [in_type, in_type], alg_props)

def binop(name, ty, alg_props):
   binop_convert(name, ty, ty, alg_props)

def binop_compare(name, ty, alg_props):
   binop_convert(name, ty, tbool, alg_props)

def binop_horiz(name, out_size, out_type, src1_size, src1_type, src2_size,
                src2_type):
   opcode(name, out_size, out_type, [src1_size, src2_size], [src1_type, src2_type], "")

def binop_reduce(name, output_size, output_type, src_type):
   opcode(name + "2",output_size, output_type,
          [2, 2], [src_type, src_type], commutative)
   opcode(name + "3", output_size, output_type,
          [3, 3], [src_type, src_type], commutative)
   opcode(name + "4", output_size, output_type,
          [4, 4], [src_type, src_type], commutative)

binop("fadd", tfloat, commutative + associative)
binop("iadd", tint, commutative + associative)
binop("fsub", tfloat, "")
binop("isub", tint, "")

binop("fmul", tfloat, commutative + associative)
# low 32-bits of signed/unsigned integer multiply
binop("imul", tint, commutative + associative)
# high 32-bits of signed integer multiply
binop("imul_high", tint, commutative)
# high 32-bits of unsigned integer multiply
binop("umul_high", tunsigned, commutative)

binop("fdiv", tfloat, "")
binop("idiv", tint, "")
binop("udiv", tunsigned, "")

# returns a boolean representing the carry resulting from the addition of
# the two unsigned arguments.

binop_convert("uadd_carry", tbool, tunsigned,
              commutative)

# returns a boolean representing the borrow resulting from the subtraction
# of the two unsigned arguments.

binop_convert("usub_borrow", tbool, tunsigned, "")

binop("fmod", tfloat, "")
binop("umod", tunsigned, "")

#
# Comparisons
#


# these integer-aware comparisons return a boolean (0 or ~0)

binop_compare("flt", tfloat, "")
binop_compare("fge", tfloat, "")
binop_compare("feq", tfloat, commutative)
binop_compare("fne", tfloat, commutative)
binop_compare("ilt", tint, "")
binop_compare("ige", tint, "")
binop_compare("ieq", tint, commutative)
binop_compare("ine", tint, commutative)
binop_compare("ult", tunsigned, "")
binop_compare("uge", tunsigned, "")

# integer-aware GLSL-style comparisons that compare floats and ints

binop_reduce("ball_fequal",  1, tbool, tfloat)
binop_reduce("bany_fnequal", 1, tbool, tfloat)
binop_reduce("ball_iequal",  1, tbool, tint)
binop_reduce("bany_inequal", 1, tbool, tint)

# non-integer-aware GLSL-style comparisons that return 0.0 or 1.0

binop_reduce("fall_equal",  1, tfloat, tfloat)
binop_reduce("fany_nequal", 1, tfloat, tfloat)

# These comparisons for integer-less hardware return 1.0 and 0.0 for true
# and false respectively

binop("slt", tfloat, "") # Set on Less Than
binop("sge", tfloat, "") # Set on Greater Than or Equal
binop("seq", tfloat, commutative) # Set on Equal
binop("sne", tfloat, commutative) # Set on Not Equal


binop("ishl", tint, "")
binop("ishr", tint, "")
binop("ushr", tunsigned, "")

# bitwise logic operators
#
# These are also used as boolean and, or, xor for hardware supporting
# integers.


binop("iand", tunsigned, commutative + associative)
binop("ior", tunsigned, commutative + associative)
binop("ixor", tunsigned, commutative + associative)


# floating point logic operators
#
# These use (src != 0.0) for testing the truth of the input, and output 1.0
# for true and 0.0 for false

binop("fand", tfloat, commutative)
binop("for", tfloat, commutative)
binop("fxor", tfloat, commutative)

binop_reduce("fdot", 1, tfloat, tfloat)

binop("fmin", tfloat, "")
binop("imin", tint, commutative + associative)
binop("umin", tunsigned, commutative + associative)
binop("fmax", tfloat, "")
binop("imax", tint, commutative + associative)
binop("umax", tunsigned, commutative + associative)

binop("fpow", tfloat, "")

binop_horiz("pack_half_2x16_split", 1, tunsigned, 1, tfloat, 1, tfloat)

binop("bfm", tunsigned, "")

binop("ldexp", tunsigned, "")

# Combines the first component of each input to make a 2-component vector.

binop_horiz("vec2", 2, tunsigned, 1, tunsigned, 1, tunsigned)

def triop(name, ty):
   opcode(name, 0, ty, [0, 0, 0], [ty, ty, ty], "")
def triop_horiz(name, output_size, src1_size, src2_size, src3_size):
   opcode(name, output_size, tunsigned,
   [src1_size, src2_size, src3_size],
   [tunsigned, tunsigned, tunsigned], "")

# fma(a, b, c) = (a# b) + c
triop("ffma", tfloat)

triop("flrp", tfloat)

# Conditional Select
#
# A vector conditional select instruction (like ?:, but operating per-
# component on vectors). There are two versions, one for floating point
# bools (0.0 vs 1.0) and one for integer bools (0 vs ~0).


triop("fcsel", tfloat)
opcode("bcsel", 0, tunsigned, [0, 0, 0],
       [tbool, tunsigned, tunsigned], "")

triop("bfi", tunsigned)

triop("ubitfield_extract", tunsigned)
opcode("ibitfield_extract", 0, tint, [0, 0, 0],
       [tint, tunsigned, tunsigned], "")

# Combines the first component of each input to make a 3-component vector.

triop_horiz("vec3", 3, 1, 1, 1)

def quadop(name):
   opcode(name, 0, tunsigned, [0, 0, 0, 0],
          [tunsigned, tunsigned, tunsigned, tunsigned],
          "")
def quadop_horiz(name, output_size, src1_size, src2_size, src3_size, src4_size):
   opcode(name, output_size, tunsigned,
          [src1_size, src2_size, src3_size, src4_size],
          [tunsigned, tunsigned, tunsigned, tunsigned],
          "")

quadop("bitfield_insert")

quadop_horiz("vec4", 4, 1, 1, 1, 1)
