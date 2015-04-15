/*
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

/* Update ir_expression::get_num_operands() and operator_strs when
 * updating this list.
 */
enum ir_expression_operation {
   ir_unop_bit_not,
   ir_unop_logic_not,
   ir_unop_neg,
   ir_unop_abs,
   ir_unop_sign,
   ir_unop_rcp,
   ir_unop_rsq,
   ir_unop_sqrt,
   ir_unop_exp,         /**< Log base e on gentype */
   ir_unop_log,	        /**< Natural log on gentype */
   ir_unop_exp2,
   ir_unop_log2,
   ir_unop_f2i,         /**< Float-to-integer conversion. */
   ir_unop_f2u,         /**< Float-to-unsigned conversion. */
   ir_unop_i2f,         /**< Integer-to-float conversion. */
   ir_unop_f2b,         /**< Float-to-boolean conversion */
   ir_unop_b2f,         /**< Boolean-to-float conversion */
   ir_unop_i2b,         /**< int-to-boolean conversion */
   ir_unop_b2i,         /**< Boolean-to-int conversion */
   ir_unop_u2f,         /**< Unsigned-to-float conversion. */
   ir_unop_i2u,         /**< Integer-to-unsigned conversion. */
   ir_unop_u2i,         /**< Unsigned-to-integer conversion. */
   ir_unop_d2f,         /**< Double-to-float conversion. */
   ir_unop_f2d,         /**< Float-to-double conversion. */
   ir_unop_d2i,         /**< Double-to-integer conversion. */
   ir_unop_i2d,         /**< Integer-to-double conversion. */
   ir_unop_d2u,         /**< Double-to-unsigned conversion. */
   ir_unop_u2d,         /**< Unsigned-to-double conversion. */
   ir_unop_d2b,         /**< Double-to-boolean conversion. */
   ir_unop_bitcast_i2f, /**< Bit-identical int-to-float "conversion" */
   ir_unop_bitcast_f2i, /**< Bit-identical float-to-int "conversion" */
   ir_unop_bitcast_u2f, /**< Bit-identical uint-to-float "conversion" */
   ir_unop_bitcast_f2u, /**< Bit-identical float-to-uint "conversion" */

   /**
    * \name Unary floating-point rounding operations.
    */
   /*@{*/
   ir_unop_trunc,
   ir_unop_ceil,
   ir_unop_floor,
   ir_unop_fract,
   ir_unop_round_even,
   /*@}*/

   /**
    * \name Trigonometric operations.
    */
   /*@{*/
   ir_unop_sin,
   ir_unop_cos,
   /*@}*/

   /**
    * \name Partial derivatives.
    */
   /*@{*/
   ir_unop_dFdx,
   ir_unop_dFdx_coarse,
   ir_unop_dFdx_fine,
   ir_unop_dFdy,
   ir_unop_dFdy_coarse,
   ir_unop_dFdy_fine,
   /*@}*/

   /**
    * \name Floating point pack and unpack operations.
    */
   /*@{*/
   ir_unop_pack_snorm_2x16,
   ir_unop_pack_snorm_4x8,
   ir_unop_pack_unorm_2x16,
   ir_unop_pack_unorm_4x8,
   ir_unop_pack_half_2x16,
   ir_unop_unpack_snorm_2x16,
   ir_unop_unpack_snorm_4x8,
   ir_unop_unpack_unorm_2x16,
   ir_unop_unpack_unorm_4x8,
   ir_unop_unpack_half_2x16,
   /*@}*/

   /**
    * \name Bit operations, part of ARB_gpu_shader5.
    */
   /*@{*/
   ir_unop_bitfield_reverse,
   ir_unop_bit_count,
   ir_unop_find_msb,
   ir_unop_find_lsb,
   /*@}*/

   ir_unop_saturate,

   /**
    * \name Double packing, part of ARB_gpu_shader_fp64.
    */
   /*@{*/
   ir_unop_pack_double_2x32,
   ir_unop_unpack_double_2x32,
   /*@}*/

   ir_unop_frexp_sig,
   ir_unop_frexp_exp,

   ir_unop_noise,

   ir_unop_subroutine_to_int,
   /**
    * Interpolate fs input at centroid
    *
    * operand0 is the fs input.
    */
   ir_unop_interpolate_at_centroid,

   /**
    * Ask the driver for the total size of a buffer block.
    *
    * operand0 is the ir_constant buffer block index in the linked shader.
    */
   ir_unop_get_buffer_size,

   /**
    * Calculate length of an unsized array inside a buffer block.
    * This opcode is going to be replaced in a lowering pass inside
    * the linker.
    *
    * operand0 is the unsized array's ir_value for the calculation
    * of its length.
    */
   ir_unop_ssbo_unsized_array_length,

   /**
    * Vote among threads on the value of the boolean argument.
    */
   ir_unop_vote_any,
   ir_unop_vote_all,
   ir_unop_vote_eq,

   /**
    * A sentinel marking the last of the unary operations.
    */
   ir_last_unop = ir_unop_vote_eq,

   ir_binop_add,
   ir_binop_sub,
   ir_binop_mul,       /**< Floating-point or low 32-bit integer multiply. */
   ir_binop_imul_high, /**< Calculates the high 32-bits of a 64-bit multiply. */
   ir_binop_div,

   /**
    * Returns the carry resulting from the addition of the two arguments.
    */
   /*@{*/
   ir_binop_carry,
   /*@}*/

   /**
    * Returns the borrow resulting from the subtraction of the second argument
    * from the first argument.
    */
   /*@{*/
   ir_binop_borrow,
   /*@}*/

   /**
    * Takes one of two combinations of arguments:
    *
    * - mod(vecN, vecN)
    * - mod(vecN, float)
    *
    * Does not take integer types.
    */
   ir_binop_mod,

   /**
    * \name Binary comparison operators which return a boolean vector.
    * The type of both operands must be equal.
    */
   /*@{*/
   ir_binop_less,
   ir_binop_greater,
   ir_binop_lequal,
   ir_binop_gequal,
   ir_binop_equal,
   ir_binop_nequal,
   /**
    * Returns single boolean for whether all components of operands[0]
    * equal the components of operands[1].
    */
   ir_binop_all_equal,
   /**
    * Returns single boolean for whether any component of operands[0]
    * is not equal to the corresponding component of operands[1].
    */
   ir_binop_any_nequal,
   /*@}*/

   /**
    * \name Bit-wise binary operations.
    */
   /*@{*/
   ir_binop_lshift,
   ir_binop_rshift,
   ir_binop_bit_and,
   ir_binop_bit_xor,
   ir_binop_bit_or,
   /*@}*/

   ir_binop_logic_and,
   ir_binop_logic_xor,
   ir_binop_logic_or,

   ir_binop_dot,
   ir_binop_min,
   ir_binop_max,

   ir_binop_pow,

   /**
    * Load a value the size of a given GLSL type from a uniform block.
    *
    * operand0 is the ir_constant uniform block index in the linked shader.
    * operand1 is a byte offset within the uniform block.
    */
   ir_binop_ubo_load,

   /**
    * \name Multiplies a number by two to a power, part of ARB_gpu_shader5.
    */
   /*@{*/
   ir_binop_ldexp,
   /*@}*/

   /**
    * Extract a scalar from a vector
    *
    * operand0 is the vector
    * operand1 is the index of the field to read from operand0
    */
   ir_binop_vector_extract,

   /**
    * Interpolate fs input at offset
    *
    * operand0 is the fs input
    * operand1 is the offset from the pixel center
    */
   ir_binop_interpolate_at_offset,

   /**
    * Interpolate fs input at sample position
    *
    * operand0 is the fs input
    * operand1 is the sample ID
    */
   ir_binop_interpolate_at_sample,

   /**
    * A sentinel marking the last of the binary operations.
    */
   ir_last_binop = ir_binop_interpolate_at_sample,

   /**
    * \name Fused floating-point multiply-add, part of ARB_gpu_shader5.
    */
   /*@{*/
   ir_triop_fma,
   /*@}*/

   ir_triop_lrp,

   /**
    * \name Conditional Select
    *
    * A vector conditional select instruction (like ?:, but operating per-
    * component on vectors).
    *
    * \see lower_instructions_visitor::ldexp_to_arith
    */
   /*@{*/
   ir_triop_csel,
   /*@}*/

   ir_triop_bitfield_extract,

   /**
    * Generate a value with one field of a vector changed
    *
    * operand0 is the vector
    * operand1 is the value to write into the vector result
    * operand2 is the index in operand0 to be modified
    */
   ir_triop_vector_insert,

   /**
    * A sentinel marking the last of the ternary operations.
    */
   ir_last_triop = ir_triop_vector_insert,

   ir_quadop_bitfield_insert,

   ir_quadop_vector,

   /**
    * A sentinel marking the last of the ternary operations.
    */
   ir_last_quadop = ir_quadop_vector,

   /**
    * A sentinel marking the last of all operations.
    */
   ir_last_opcode = ir_quadop_vector
};
