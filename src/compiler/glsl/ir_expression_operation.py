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

class type(object):
   def __init__(self, c_type, union_field, glsl_type):
      self.c_type = c_type
      self.union_field = union_field
      self.glsl_type = glsl_type


class type_signature_iter(object):
   """Basic iterator for a set of type signatures.  Various kinds of sequences of
   types come in, and an iteration of type_signature objects come out.

   """

   def __init__(self, source_types, num_operands):
      """Initialize an iterator from a sequence of input types and a number
      operands.  This is for signatures where all the operands have the same
      type and the result type of the operation is the same as the input type.

      """
      self.dest_type = None
      self.source_types = source_types
      self.num_operands = num_operands
      self.i = 0

   def __init__(self, dest_type, source_types, num_operands):
      """Initialize an iterator from a result tpye, a sequence of input types and a
      number operands.  This is for signatures where all the operands have the
      same type but the result type of the operation is different from the
      input type.

      """
      self.dest_type = dest_type
      self.source_types = source_types
      self.num_operands = num_operands
      self.i = 0

   def __iter__(self):
      return self

   def next(self):
      if self.i < len(self.source_types):
         i = self.i
         self.i += 1

         if self.dest_type is None:
            dest_type = self.source_types[i]
         else:
            dest_type = self.dest_type

         return (dest_type, self.num_operands * (self.source_types[i],))
      else:
         raise StopIteration()


uint_type = type("unsigned", "u", "GLSL_TYPE_UINT")
int_type = type("int", "i", "GLSL_TYPE_INT")
float_type = type("float", "f", "GLSL_TYPE_FLOAT")
double_type = type("double", "d", "GLSL_TYPE_DOUBLE")
bool_type = type("bool", "b", "GLSL_TYPE_BOOL")

numeric_types = (uint_type, int_type, float_type, double_type)
integer_types = (uint_type, int_type)
real_types = (float_type, double_type)

# This template is for unary operations that can only have operands of a
# single type.  ir_unop_logic_not is an example.
constant_template0 = mako.template.Template("""\
   case ${op.get_enum_name()}:
      assert(op[0]->type->base_type == ${op.source_types[0].glsl_type});
      for (unsigned c = 0; c < op[0]->type->components(); c++)
         data.${op.source_types[0].union_field}[c] = ${op.get_c_expression(op.source_types)};
      break;""")

# This template is for unary operations that can have operands of a several
# different types.  ir_unop_bit_not is an example.
constant_template1 = mako.template.Template("""\
   case ${op.get_enum_name()}:
      switch (op[0]->type->base_type) {
    % for dst_type, src_types in op.signatures():
      case ${src_types[0].glsl_type}:
         for (unsigned c = 0; c < op[0]->type->components(); c++)
            data.${dst_type.union_field}[c] = ${op.get_c_expression(src_types)};
         break;
    % endfor
      default:
         assert(0);
      }
      break;""")

# This template is for unary operations that map an operand of one type to an
# operand of another type.  ir_unop_f2b is an example.
constant_template2 = mako.template.Template("""\
   case ${op.get_enum_name()}:
      assert(op[0]->type->base_type == ${op.source_types[0].glsl_type});
      for (unsigned c = 0; c < op[0]->type->components(); c++)
         data.${op.dest_type.union_field}[c] = ${op.get_c_expression(op.source_types)};
      break;""")


class operation(object):
   def __init__(self, name, num_operands, printable_name = None, source_types = None, dest_type = None, c_expression = None):
      self.name = name
      self.num_operands = num_operands

      if printable_name is None:
         self.printable_name = name
      else:
         self.printable_name = printable_name

      self.source_types = source_types
      self.dest_type = dest_type

      if c_expression is None:
         self.c_expression = None
      elif isinstance(c_expression, str):
         self.c_expression = {'default': c_expression}
      else:
         self.c_expression = c_expression


   def get_enum_name(self):
      return "ir_{}op_{}".format(("un", "bin", "tri", "quad")[self.num_operands-1], self.name)


   def get_template(self):
      if self.c_expression is None:
         return None

      if self.num_operands == 1:
         if self.dest_type is not None:
            return constant_template2.render(op=self)
         elif len(self.source_types) == 1:
            return constant_template0.render(op=self)
         else:
            return constant_template1.render(op=self)

      return None


   def get_c_expression(self, types):
      src0 = "op[0]->value.{}[c]".format(types[0].union_field)

      expr = self.c_expression[types[0].union_field] if types[0].union_field in self.c_expression else self.c_expression['default']

      return expr.format(src0=src0)


   def signatures(self):
      return type_signature_iter(self.dest_type, self.source_types, self.num_operands)


ir_expression_operation = [
   operation("bit_not", 1, printable_name="~", source_types=integer_types, c_expression="~ {src0}"),
   operation("logic_not", 1, printable_name="!", source_types=(bool_type,), c_expression="!{src0}"),
   operation("neg", 1),
   operation("abs", 1),
   operation("sign", 1),
   operation("rcp", 1),
   operation("rsq", 1),
   operation("sqrt", 1),
   operation("exp", 1, source_types=(float_type,), c_expression="expf({src0})"),         # Log base e on gentype
   operation("log", 1, source_types=(float_type,), c_expression="logf({src0})"),         # Natural log on gentype
   operation("exp2", 1, source_types=(float_type,), c_expression="exp2f({src0})"),
   operation("log2", 1, source_types=(float_type,), c_expression="log2f({src0})"),

   # Float-to-integer conversion.
   operation("f2i", 1, source_types=(float_type,), dest_type=int_type, c_expression="(int) {src0}"),
   # Float-to-unsigned conversion.
   operation("f2u", 1, source_types=(float_type,), dest_type=uint_type, c_expression="(unsigned) {src0}"),
   # Integer-to-float conversion.
   operation("i2f", 1, source_types=(int_type,), dest_type=float_type, c_expression="(float) {src0}"),
   # Float-to-boolean conversion
   operation("f2b", 1, source_types=(float_type,), dest_type=bool_type, c_expression="{src0} != 0.0F ? true : false"),
   # Boolean-to-float conversion
   operation("b2f", 1, source_types=(bool_type,), dest_type=float_type, c_expression="{src0} ? 1.0F : 0.0F"),
   # int-to-boolean conversion
   operation("i2b", 1),
   # Boolean-to-int conversion
   operation("b2i", 1, source_types=(bool_type,), dest_type=int_type, c_expression="{src0} ? 1 : 0"),
   # Unsigned-to-float conversion.
   operation("u2f", 1, source_types=(uint_type,), dest_type=float_type, c_expression="(float) {src0}"),
   # Integer-to-unsigned conversion.
   operation("i2u", 1, source_types=(int_type,), dest_type=uint_type, c_expression="{src0}"),
   # Unsigned-to-integer conversion.
   operation("u2i", 1, source_types=(uint_type,), dest_type=int_type, c_expression="{src0}"),
   # Double-to-float conversion.
   operation("d2f", 1, source_types=(double_type,), dest_type=float_type, c_expression="{src0}"),
   # Float-to-double conversion.
   operation("f2d", 1, source_types=(float_type,), dest_type=double_type, c_expression="{src0}"),
   # Double-to-integer conversion.
   operation("d2i", 1, source_types=(double_type,), dest_type=int_type, c_expression="{src0}"),
   # Integer-to-double conversion.
   operation("i2d", 1, source_types=(int_type,), dest_type=double_type, c_expression="{src0}"),
   # Double-to-unsigned conversion.
   operation("d2u", 1, source_types=(double_type,), dest_type=uint_type, c_expression="{src0}"),
   # Unsigned-to-double conversion.
   operation("u2d", 1, source_types=(uint_type,), dest_type=double_type, c_expression="{src0}"),
   # Double-to-boolean conversion.
   operation("d2b", 1, source_types=(double_type,), dest_type=bool_type, c_expression="{src0} != 0.0"),
   # 'Bit-identical int-to-float "conversion"
   operation("bitcast_i2f", 1, source_types=(int_type,), dest_type=float_type, c_expression="bitcast_u2f({src0})"),
   # 'Bit-identical float-to-int "conversion"
   operation("bitcast_f2i", 1, source_types=(float_type,), dest_type=int_type, c_expression="bitcast_f2u({src0})"),
   # 'Bit-identical uint-to-float "conversion"
   operation("bitcast_u2f", 1, source_types=(uint_type,), dest_type=float_type, c_expression="bitcast_u2f({src0})"),
   # 'Bit-identical float-to-uint "conversion"
   operation("bitcast_f2u", 1, source_types=(float_type,), dest_type=uint_type, c_expression="bitcast_f2u({src0})"),

   # Unary floating-point rounding operations.
   operation("trunc", 1),
   operation("ceil", 1),
   operation("floor", 1),
   operation("fract", 1),
   operation("round_even", 1),

   # Trigonometric operations.
   operation("sin", 1, source_types=(float_type,), c_expression="sinf({src0})"),
   operation("cos", 1, source_types=(float_type,), c_expression="cosf({src0})"),

   # Partial derivatives.
   operation("dFdx", 1, source_types=(float_type,), c_expression="0.0f"),
   operation("dFdx_coarse", 1, printable_name="dFdxCoarse", source_types=(float_type,), c_expression="0.0f"),
   operation("dFdx_fine", 1, printable_name="dFdxFine", source_types=(float_type,), c_expression="0.0f"),
   operation("dFdy", 1, source_types=(float_type,), c_expression="0.0f"),
   operation("dFdy_coarse", 1, printable_name="dFdyCoarse", source_types=(float_type,), c_expression="0.0f"),
   operation("dFdy_fine", 1, printable_name="dFdyFine", source_types=(float_type,), c_expression="0.0f"),

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
   operation("bitfield_reverse", 1, source_types=integer_types, c_expression="bitfield_reverse({src0})"),
   operation("bit_count", 1),
   operation("find_msb", 1),
   operation("find_lsb", 1),

   operation("saturate", 1, printable_name="sat", source_types=(float_type,), c_expression="CLAMP({src0}, 0.0f, 1.0f)"),

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

   constant_template = mako.template.Template("""\
   switch (this->operation) {
% for op in values:
    % if op.c_expression is not None:
${op.get_template()}

    % endif
% endfor
   default:
      /* FINISHME: Should handle all expression types. */
      return NULL;
   }
""")

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
   elif sys.argv[1] == "constant":
      print(constant_template.render(values=ir_expression_operation))
