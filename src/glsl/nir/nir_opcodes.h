/*
 * Copyright © 2014 Intel Corporation
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
 *
 * Authors:
 *    Connor Abbott (cwabbott0@gmail.com)
 *
 */

/**
 * This header file defines all the available opcodes in one place. It expands
 * to a list of macros of the form:
 *
 * OPCODE(name, num_inputs, per_component, output_size, output_type,
 *        input_sizes, input_types)
 *
 * Which should correspond one-to-one with the nir_op_info structure. It is
 * included in both ir.h to create the nir_op enum (with members of the form
 * nir_op_(name)) and and in opcodes.c to create nir_op_infos, which is a
 * const array of nir_op_info structures for each opcode.
 */

#define ARR(...) { __VA_ARGS__ }

#define UNOP(name, type) OPCODE(name, 1, false, 0, type, ARR(0), ARR(type))
#define UNOP_CONVERT(name, in_type, out_type) \
   OPCODE(name, 1, false, 0, out_type, ARR(0), ARR(in_type))
#define UNOP_HORIZ(name, output_size, output_type, input_size, input_type) \
   OPCODE(name, 1, true, output_size, output_type, ARR(input_size), \
          ARR(input_type))

#define UNOP_REDUCE(name, output_size, output_type, input_type) \
   UNOP_HORIZ(name##2, output_size, output_type, 2, input_type) \
   UNOP_HORIZ(name##3, output_size, output_type, 3, input_type) \
   UNOP_HORIZ(name##4, output_size, output_type, 4, input_type)

/**
 * These two move instructions differ in what modifiers they support and what
 * the negate modifier means. Otherwise, they are identical.
 */
UNOP(fmov, nir_type_float)
UNOP(imov, nir_type_int)

UNOP(inot, nir_type_int) /* invert every bit of the integer */
UNOP(fnot, nir_type_float) /* (src == 0.0) ? 1.0 : 0.0 */
UNOP(fsign, nir_type_float)
UNOP(isign, nir_type_int)
UNOP(frcp, nir_type_float)
UNOP(frsq, nir_type_float)
UNOP(fsqrt, nir_type_float)
UNOP(fexp, nir_type_float) /* < e^x */
UNOP(flog, nir_type_float) /* log base e */
UNOP(fexp2, nir_type_float)
UNOP(flog2, nir_type_float)
UNOP_CONVERT(f2i, nir_type_float, nir_type_int)       /**< Float-to-integer conversion. */
UNOP_CONVERT(f2u, nir_type_float, nir_type_unsigned)  /**< Float-to-unsigned conversion. */
UNOP_CONVERT(i2f, nir_type_int, nir_type_float)       /**< Integer-to-float conversion. */
UNOP_CONVERT(f2b, nir_type_float, nir_type_bool)      /**< Float-to-boolean conversion */
UNOP_CONVERT(b2f, nir_type_bool, nir_type_float)      /**< Boolean-to-float conversion */
UNOP_CONVERT(i2b, nir_type_int, nir_type_bool)        /**< int-to-boolean conversion */
UNOP_CONVERT(b2i, nir_type_bool, nir_type_int)        /**< Boolean-to-int conversion */
UNOP_CONVERT(u2f, nir_type_unsigned, nir_type_float)  /**< Unsigned-to-float conversion. */

UNOP_REDUCE(bany, 1, nir_type_bool, nir_type_bool) /* returns ~0 if any component of src[0] != 0 */
UNOP_REDUCE(ball, 1, nir_type_bool, nir_type_bool) /* returns ~0 if all components of src[0] != 0 */
UNOP_REDUCE(fany, 1, nir_type_float, nir_type_float) /* returns 1.0 if any component of src[0] != 0 */
UNOP_REDUCE(fall, 1, nir_type_float, nir_type_float) /* returns 1.0 if all components of src[0] != 0 */

/**
 * \name Unary floating-point rounding operations.
 */
/*@{*/
UNOP(ftrunc, nir_type_float)
UNOP(fceil, nir_type_float)
UNOP(ffloor, nir_type_float)
UNOP(ffract, nir_type_float)
UNOP(fround_even, nir_type_float)
/*@}*/

/**
 * \name Trigonometric operations.
 */
/*@{*/
UNOP(fsin, nir_type_float)
UNOP(fcos, nir_type_float)
UNOP(fsin_reduced, nir_type_float)
UNOP(fcos_reduced, nir_type_float)
/*@}*/

/**
 * \name Partial derivatives.
 */
/*@{*/
UNOP(fddx, nir_type_float)
UNOP(fddy, nir_type_float)
UNOP(fddx_fine, nir_type_float)
UNOP(fddy_fine, nir_type_float)
UNOP(fddx_coarse, nir_type_float)
UNOP(fddy_coarse, nir_type_float)
/*@}*/

/**
 * \name Floating point pack and unpack operations.
 */
/*@{*/
UNOP_HORIZ(pack_snorm_2x16, 1, nir_type_unsigned, 2, nir_type_float)
UNOP_HORIZ(pack_snorm_4x8, 1, nir_type_unsigned, 4, nir_type_float)
UNOP_HORIZ(pack_unorm_2x16, 1, nir_type_unsigned, 2, nir_type_float)
UNOP_HORIZ(pack_unorm_4x8, 1, nir_type_unsigned, 4, nir_type_float)
UNOP_HORIZ(pack_half_2x16, 1, nir_type_unsigned, 2, nir_type_float)
UNOP_HORIZ(unpack_snorm_2x16, 2, nir_type_float, 1, nir_type_unsigned)
UNOP_HORIZ(unpack_snorm_4x8, 4, nir_type_float, 1, nir_type_unsigned)
UNOP_HORIZ(unpack_unorm_2x16, 2, nir_type_float, 1, nir_type_unsigned)
UNOP_HORIZ(unpack_unorm_4x8, 4, nir_type_float, 1, nir_type_unsigned)
UNOP_HORIZ(unpack_half_2x16, 2, nir_type_float, 1, nir_type_unsigned)
/*@}*/

/**
 * \name Lowered floating point unpacking operations.
 */
/*@{*/
UNOP_HORIZ(unpack_half_2x16_split_x, 1, nir_type_float, 1, nir_type_unsigned)
UNOP_HORIZ(unpack_half_2x16_split_y, 1, nir_type_float, 1, nir_type_unsigned)
/*@}*/

/**
 * \name Bit operations, part of ARB_gpu_shader5.
 */
/*@{*/
UNOP(bitfield_reverse, nir_type_unsigned)
UNOP(bit_count, nir_type_unsigned)
UNOP(find_msb, nir_type_unsigned)
UNOP(find_lsb, nir_type_unsigned)
/*@}*/

UNOP_HORIZ(fnoise1_1, 1, nir_type_float, 1, nir_type_float)
UNOP_HORIZ(fnoise1_2, 1, nir_type_float, 2, nir_type_float)
UNOP_HORIZ(fnoise1_3, 1, nir_type_float, 3, nir_type_float)
UNOP_HORIZ(fnoise1_4, 1, nir_type_float, 4, nir_type_float)
UNOP_HORIZ(fnoise2_1, 2, nir_type_float, 1, nir_type_float)
UNOP_HORIZ(fnoise2_2, 2, nir_type_float, 2, nir_type_float)
UNOP_HORIZ(fnoise2_3, 2, nir_type_float, 3, nir_type_float)
UNOP_HORIZ(fnoise2_4, 2, nir_type_float, 4, nir_type_float)
UNOP_HORIZ(fnoise3_1, 3, nir_type_float, 1, nir_type_float)
UNOP_HORIZ(fnoise3_2, 3, nir_type_float, 2, nir_type_float)
UNOP_HORIZ(fnoise3_3, 3, nir_type_float, 3, nir_type_float)
UNOP_HORIZ(fnoise3_4, 3, nir_type_float, 4, nir_type_float)
UNOP_HORIZ(fnoise4_1, 4, nir_type_float, 1, nir_type_float)
UNOP_HORIZ(fnoise4_2, 4, nir_type_float, 2, nir_type_float)
UNOP_HORIZ(fnoise4_3, 4, nir_type_float, 3, nir_type_float)
UNOP_HORIZ(fnoise4_4, 4, nir_type_float, 4, nir_type_float)

#define BINOP(name, type) \
   OPCODE(name, 2, true, 0, type, ARR(0, 0), ARR(type, type))
#define BINOP_CONVERT(name, out_type, in_type) \
   OPCODE(name, 2, true, 0, out_type, ARR(0, 0), ARR(in_type, in_type))
#define BINOP_COMPARE(name, type) BINOP_CONVERT(name, nir_type_bool, type)
#define BINOP_HORIZ(name, output_size, output_type, src1_size, src1_type, \
                    src2_size, src2_type) \
   OPCODE(name, 2, true, output_size, output_type, ARR(src1_size, src2_size), \
          ARR(src1_type, src2_type))
#define BINOP_REDUCE(name, output_size, output_type, src_type) \
   BINOP_HORIZ(name##2, output_size, output_type, 2, src_type, 2, src_type) \
   BINOP_HORIZ(name##3, output_size, output_type, 3, src_type, 3, src_type) \
   BINOP_HORIZ(name##4, output_size, output_type, 4, src_type, 4, src_type) \

BINOP(fadd, nir_type_float)
BINOP(iadd, nir_type_int)
BINOP(fsub, nir_type_float)
BINOP(isub, nir_type_int)

BINOP(fmul, nir_type_float)
BINOP(imul, nir_type_int) /* low 32-bits of signed/unsigned integer multiply */
BINOP(imul_high, nir_type_int) /* high 32-bits of signed integer multiply */
BINOP(umul_high, nir_type_unsigned) /* high 32-bits of unsigned integer multiply */

BINOP(fdiv, nir_type_float)
BINOP(idiv, nir_type_int)
BINOP(udiv, nir_type_unsigned)

/**
 * returns a boolean representing the carry resulting from the addition of
 * the two unsigned arguments.
 */
BINOP_CONVERT(uadd_carry, nir_type_bool, nir_type_unsigned)

/**
 * returns a boolean representing the borrow resulting from the subtraction
 * of the two unsigned arguments.
 */
BINOP_CONVERT(usub_borrow, nir_type_bool, nir_type_unsigned)

BINOP(fmod, nir_type_float)
BINOP(umod, nir_type_unsigned)

/**
 * \name comparisons
 */
/*@{*/

/**
 * these integer-aware comparisons return a boolean (0 or ~0)
 */
BINOP_COMPARE(flt, nir_type_float)
BINOP_COMPARE(fge, nir_type_float)
BINOP_COMPARE(feq, nir_type_float)
BINOP_COMPARE(fne, nir_type_float)
BINOP_COMPARE(ilt, nir_type_int)
BINOP_COMPARE(ige, nir_type_int)
BINOP_COMPARE(ieq, nir_type_int)
BINOP_COMPARE(ine, nir_type_int)
BINOP_COMPARE(ult, nir_type_unsigned)
BINOP_COMPARE(uge, nir_type_unsigned)

/** integer-aware GLSL-style comparisons that compare floats and ints */
BINOP_REDUCE(ball_fequal,  1, nir_type_bool, nir_type_float)
BINOP_REDUCE(bany_fnequal, 1, nir_type_bool, nir_type_float)
BINOP_REDUCE(ball_iequal,  1, nir_type_bool, nir_type_int)
BINOP_REDUCE(bany_inequal, 1, nir_type_bool, nir_type_int)

/** non-integer-aware GLSL-style comparisons that return 0.0 or 1.0 */
BINOP_REDUCE(fall_equal,  1, nir_type_float, nir_type_float)
BINOP_REDUCE(fany_nequal, 1, nir_type_float, nir_type_float)

/**
 * These comparisons for integer-less hardware return 1.0 and 0.0 for true
 * and false respectively
 */
BINOP(slt, nir_type_float) /* Set on Less Than */
BINOP(sge, nir_type_float) /* Set on Greater Than or Equal */
BINOP(seq, nir_type_float) /* Set on Equal */
BINOP(sne, nir_type_float) /* Set on Not Equal */

/*@}*/

BINOP(ishl, nir_type_int)
BINOP(ishr, nir_type_int)
BINOP(ushr, nir_type_unsigned)

/**
 * \name bitwise logic operators
 *
 * These are also used as boolean and, or, xor for hardware supporting
 * integers.
 */
/*@{*/
BINOP(iand, nir_type_unsigned)
BINOP(ior, nir_type_unsigned)
BINOP(ixor, nir_type_unsigned)
/*@{*/

/**
 * \name floating point logic operators
 *
 * These use (src != 0.0) for testing the truth of the input, and output 1.0
 * for true and 0.0 for false
 */
BINOP(fand, nir_type_float)
BINOP(for, nir_type_float)
BINOP(fxor, nir_type_float)

BINOP_REDUCE(fdot, 1, nir_type_float, nir_type_float)

BINOP(fmin, nir_type_float)
BINOP(imin, nir_type_int)
BINOP(umin, nir_type_unsigned)
BINOP(fmax, nir_type_float)
BINOP(imax, nir_type_int)
BINOP(umax, nir_type_unsigned)

BINOP(fpow, nir_type_float)

BINOP_HORIZ(pack_half_2x16_split, 1, nir_type_unsigned, 1, nir_type_float, 1, nir_type_float)

BINOP(bfm, nir_type_unsigned)

BINOP(ldexp, nir_type_unsigned)

/**
 * Combines the first component of each input to make a 2-component vector.
 */
BINOP_HORIZ(vec2, 2, nir_type_unsigned, 1, nir_type_unsigned, 1, nir_type_unsigned)

#define TRIOP(name, type) \
   OPCODE(name, 3, true, 0, type, ARR(0, 0, 0), ARR(type, type, type))
#define TRIOP_HORIZ(name, output_size, src1_size, src2_size, src3_size) \
   OPCODE(name, 3, false, output_size, nir_type_unsigned, \
   ARR(src1_size, src2_size, src3_size), \
   ARR(nir_type_unsigned, nir_type_unsigned, nir_type_unsigned))

TRIOP(ffma, nir_type_float)

TRIOP(flrp, nir_type_float)

/**
 * \name Conditional Select
 *
 * A vector conditional select instruction (like ?:, but operating per-
 * component on vectors). There are two versions, one for floating point
 * bools (0.0 vs 1.0) and one for integer bools (0 vs ~0).
 */

OPCODE(fcsel, 3, true, 0, nir_type_float, ARR(1, 0, 0),
       ARR(nir_type_float, nir_type_float, nir_type_float))
OPCODE(bcsel, 3, true, 0, nir_type_unsigned, ARR(1, 0, 0),
       ARR(nir_type_bool, nir_type_unsigned, nir_type_unsigned))

TRIOP(bfi, nir_type_unsigned)

TRIOP(ubitfield_extract, nir_type_unsigned)
OPCODE(ibitfield_extract, 3, true, 0, nir_type_int, ARR(0, 0, 0),
       ARR(nir_type_int, nir_type_unsigned, nir_type_unsigned))

/**
 * Combines the first component of each input to make a 3-component vector.
 */
TRIOP_HORIZ(vec3, 3, 1, 1, 1)

#define QUADOP(name) \
   OPCODE(name, 4, true, 0, nir_type_unsigned, ARR(0, 0, 0, 0), \
   ARR(nir_type_unsigned, nir_type_unsigned, nir_type_unsigned, nir_type_unsigned))
#define QUADOP_HORIZ(name, output_size, src1_size, src2_size, src3_size, \
                     src4_size) \
   OPCODE(name, 4, false, output_size, nir_type_unsigned, \
          ARR(src1_size, src2_size, src3_size, src4_size), \
          ARR(nir_type_unsigned, nir_type_unsigned, nir_type_unsigned, nir_type_unsigned))

QUADOP(bitfield_insert)

QUADOP_HORIZ(vec4, 4, 1, 1, 1, 1)

LAST_OPCODE(vec4)
