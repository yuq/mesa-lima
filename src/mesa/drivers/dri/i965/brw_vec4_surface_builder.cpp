/*
 * Copyright © 2013-2015 Intel Corporation
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

#include "brw_vec4_surface_builder.h"

using namespace brw;

namespace {
   namespace array_utils {
      /**
       * Copy one every \p src_stride logical components of the argument into
       * one every \p dst_stride logical components of the result.
       */
      src_reg
      emit_stride(const vec4_builder &bld, const src_reg &src, unsigned size,
                  unsigned dst_stride, unsigned src_stride)
      {
         if (src_stride == 1 && dst_stride == 1) {
            return src;
         } else {
            const dst_reg dst = bld.vgrf(src.type,
                                         DIV_ROUND_UP(size * dst_stride, 4));

            for (unsigned i = 0; i < size; ++i)
               bld.MOV(writemask(offset(dst, i * dst_stride / 4),
                                 1 << (i * dst_stride % 4)),
                       swizzle(offset(src, i * src_stride / 4),
                               brw_swizzle_for_mask(1 << (i * src_stride % 4))));

            return src_reg(dst);
         }
      }

      /**
       * Convert a VEC4 into an array of registers with the layout expected by
       * the recipient shared unit.  If \p has_simd4x2 is true the argument is
       * left unmodified in SIMD4x2 form, otherwise it will be rearranged into
       * a SIMD8 vector.
       */
      src_reg
      emit_insert(const vec4_builder &bld, const src_reg &src,
                  unsigned n, bool has_simd4x2)
      {
         if (src.file == BAD_FILE || n == 0) {
            return src_reg();

         } else {
            /* Pad unused components with zeroes. */
            const unsigned mask = (1 << n) - 1;
            const dst_reg tmp = bld.vgrf(src.type);

            bld.MOV(writemask(tmp, mask), src);
            if (n < 4)
               bld.MOV(writemask(tmp, ~mask), 0);

            return emit_stride(bld, src_reg(tmp), n, has_simd4x2 ? 1 : 4, 1);
         }
      }

      /**
       * Convert an array of registers back into a VEC4 according to the
       * layout expected from some shared unit.  If \p has_simd4x2 is true the
       * argument is left unmodified in SIMD4x2 form, otherwise it will be
       * rearranged from SIMD8 form.
       */
      src_reg
      emit_extract(const vec4_builder &bld, const src_reg src,
                   unsigned n, bool has_simd4x2)
      {
         if (src.file == BAD_FILE || n == 0) {
            return src_reg();

         } else {
            return emit_stride(bld, src, n, 1, has_simd4x2 ? 1 : 4);
         }
      }
   }
}
