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

#include "brw_fs_surface_builder.h"
#include "brw_fs.h"

using namespace brw;

namespace brw {
   namespace surface_access {
      namespace {
         /**
          * Generate a logical send opcode for a surface message and return
          * the result.
          */
         fs_reg
         emit_send(const fs_builder &bld, enum opcode opcode,
                   const fs_reg &addr, const fs_reg &src, const fs_reg &surface,
                   unsigned dims, unsigned arg, unsigned rsize,
                   brw_predicate pred = BRW_PREDICATE_NONE)
         {
            /* Reduce the dynamically uniform surface index to a single
             * scalar.
             */
            const fs_reg usurface = bld.emit_uniformize(surface);
            const fs_reg srcs[] = {
               addr, src, usurface, fs_reg(dims), fs_reg(arg)
            };
            const fs_reg dst = bld.vgrf(BRW_REGISTER_TYPE_UD, rsize);
            fs_inst *inst = bld.emit(opcode, dst, srcs, ARRAY_SIZE(srcs));

            inst->regs_written = rsize * bld.dispatch_width() / 8;
            inst->predicate = pred;
            return dst;
         }
      }

      /**
       * Emit an untyped surface read opcode.  \p dims determines the number
       * of components of the address and \p size the number of components of
       * the returned value.
       */
      fs_reg
      emit_untyped_read(const fs_builder &bld,
                        const fs_reg &surface, const fs_reg &addr,
                        unsigned dims, unsigned size,
                        brw_predicate pred)
      {
         return emit_send(bld, SHADER_OPCODE_UNTYPED_SURFACE_READ_LOGICAL,
                          addr, fs_reg(), surface, dims, size, size, pred);
      }

      /**
       * Emit an untyped surface write opcode.  \p dims determines the number
       * of components of the address and \p size the number of components of
       * the argument.
       */
      void
      emit_untyped_write(const fs_builder &bld, const fs_reg &surface,
                         const fs_reg &addr, const fs_reg &src,
                         unsigned dims, unsigned size,
                         brw_predicate pred)
      {
         emit_send(bld, SHADER_OPCODE_UNTYPED_SURFACE_WRITE_LOGICAL,
                   addr, src, surface, dims, size, 0, pred);
      }

      /**
       * Emit an untyped surface atomic opcode.  \p dims determines the number
       * of components of the address and \p rsize the number of components of
       * the returned value (either zero or one).
       */
      fs_reg
      emit_untyped_atomic(const fs_builder &bld,
                          const fs_reg &surface, const fs_reg &addr,
                          const fs_reg &src0, const fs_reg &src1,
                          unsigned dims, unsigned rsize, unsigned op,
                          brw_predicate pred)
      {
         /* FINISHME: Factor out this frequently recurring pattern into a
          * helper function.
          */
         const unsigned n = (src0.file != BAD_FILE) + (src1.file != BAD_FILE);
         const fs_reg srcs[] = { src0, src1 };
         const fs_reg tmp = bld.vgrf(BRW_REGISTER_TYPE_UD, n);
         bld.LOAD_PAYLOAD(tmp, srcs, n, 0);

         return emit_send(bld, SHADER_OPCODE_UNTYPED_ATOMIC_LOGICAL,
                          addr, tmp, surface, dims, op, rsize, pred);
      }

      /**
       * Emit a typed surface read opcode.  \p dims determines the number of
       * components of the address and \p size the number of components of the
       * returned value.
       */
      fs_reg
      emit_typed_read(const fs_builder &bld, const fs_reg &surface,
                      const fs_reg &addr, unsigned dims, unsigned size)
      {
         return emit_send(bld, SHADER_OPCODE_TYPED_SURFACE_READ_LOGICAL,
                          addr, fs_reg(), surface, dims, size, size);
      }

      /**
       * Emit a typed surface write opcode.  \p dims determines the number of
       * components of the address and \p size the number of components of the
       * argument.
       */
      void
      emit_typed_write(const fs_builder &bld, const fs_reg &surface,
                       const fs_reg &addr, const fs_reg &src,
                       unsigned dims, unsigned size)
      {
         emit_send(bld, SHADER_OPCODE_TYPED_SURFACE_WRITE_LOGICAL,
                   addr, src, surface, dims, size, 0);
      }

      /**
       * Emit a typed surface atomic opcode.  \p dims determines the number of
       * components of the address and \p rsize the number of components of
       * the returned value (either zero or one).
       */
      fs_reg
      emit_typed_atomic(const fs_builder &bld, const fs_reg &surface,
                        const fs_reg &addr,
                        const fs_reg &src0, const fs_reg &src1,
                        unsigned dims, unsigned rsize, unsigned op,
                        brw_predicate pred)
      {
         /* FINISHME: Factor out this frequently recurring pattern into a
          * helper function.
          */
         const unsigned n = (src0.file != BAD_FILE) + (src1.file != BAD_FILE);
         const fs_reg srcs[] = { src0, src1 };
         const fs_reg tmp = bld.vgrf(BRW_REGISTER_TYPE_UD, n);
         bld.LOAD_PAYLOAD(tmp, srcs, n, 0);

         return emit_send(bld, SHADER_OPCODE_TYPED_ATOMIC_LOGICAL,
                          addr, tmp, surface, dims, op, rsize);
      }
   }
}

namespace {
   namespace image_format_info {
      /**
       * Simple 4-tuple of scalars used to pass around per-color component
       * values.
       */
      struct color_u {
         color_u(unsigned x = 0) : r(x), g(x), b(x), a(x)
         {
         }

         color_u(unsigned r, unsigned g, unsigned b, unsigned a) :
            r(r), g(g), b(b), a(a)
         {
         }

         unsigned
         operator[](unsigned i) const
         {
            const unsigned xs[] = { r, g, b, a };
            return xs[i];
         }

         unsigned r, g, b, a;
      };

      /**
       * Return the per-channel bitfield widths for a given image format.
       */
      inline color_u
      get_bit_widths(mesa_format format)
      {
         return color_u(_mesa_get_format_bits(format, GL_RED_BITS),
                        _mesa_get_format_bits(format, GL_GREEN_BITS),
                        _mesa_get_format_bits(format, GL_BLUE_BITS),
                        _mesa_get_format_bits(format, GL_ALPHA_BITS));
      }

      /**
       * Return the per-channel bitfield shifts for a given image format.
       */
      inline color_u
      get_bit_shifts(mesa_format format)
      {
         const color_u widths = get_bit_widths(format);
         return color_u(0, widths.r, widths.r + widths.g,
                        widths.r + widths.g + widths.b);
      }

      /**
       * Return true if all present components have the same bit width.
       */
      inline bool
      is_homogeneous(mesa_format format)
      {
         const color_u widths = get_bit_widths(format);
         return ((widths.g == 0 || widths.g == widths.r) &&
                 (widths.b == 0 || widths.b == widths.r) &&
                 (widths.a == 0 || widths.a == widths.r));
      }

      /**
       * Return true if the format conversion boils down to a trivial copy.
       */
      inline bool
      is_conversion_trivial(const brw_device_info *devinfo, mesa_format format)
      {
         return (get_bit_widths(format).r == 32 && is_homogeneous(format)) ||
                 format == brw_lower_mesa_image_format(devinfo, format);
      }

      /**
       * Return true if the hardware natively supports some format with
       * compatible bitfield layout, but possibly different data types.
       */
      inline bool
      has_supported_bit_layout(const brw_device_info *devinfo,
                               mesa_format format)
      {
         const color_u widths = get_bit_widths(format);
         const color_u lower_widths = get_bit_widths(
            brw_lower_mesa_image_format(devinfo, format));

         return (widths.r == lower_widths.r &&
                 widths.g == lower_widths.g &&
                 widths.b == lower_widths.b &&
                 widths.a == lower_widths.a);
      }

      /**
       * Return true if we are required to spread individual components over
       * several components of the format used by the hardware (RG32 and
       * friends implemented as RGBA16UI).
       */
      inline bool
      has_split_bit_layout(const brw_device_info *devinfo, mesa_format format)
      {
         const mesa_format lower_format =
            brw_lower_mesa_image_format(devinfo, format);

         return (_mesa_format_num_components(format) <
                 _mesa_format_num_components(lower_format));
      }

      /**
       * Return true unless we have to fall back to untyped surface access.
       * Fail!
       */
      inline bool
      has_matching_typed_format(const brw_device_info *devinfo,
                                mesa_format format)
      {
         return (_mesa_get_format_bytes(format) <= 4 ||
                 (_mesa_get_format_bytes(format) <= 8 &&
                  (devinfo->gen >= 8 || devinfo->is_haswell)) ||
                 devinfo->gen >= 9);
      }

      /**
       * Return true if the hardware returns garbage in the unused high bits
       * of each component.  This may happen on IVB because we rely on the
       * undocumented behavior that typed reads from surfaces of the
       * unsupported R8 and R16 formats return useful data in their least
       * significant bits.
       */
      inline bool
      has_undefined_high_bits(const brw_device_info *devinfo,
                              mesa_format format)
      {
         const mesa_format lower_format =
            brw_lower_mesa_image_format(devinfo, format);

         return (devinfo->gen == 7 && !devinfo->is_haswell &&
                 (lower_format == MESA_FORMAT_R_UINT16 ||
                  lower_format == MESA_FORMAT_R_UINT8));
      }

      /**
       * Return true if the format represents values as signed integers
       * requiring sign extension when unpacking.
       */
      inline bool
      needs_sign_extension(mesa_format format)
      {
         return (_mesa_get_format_datatype(format) == GL_SIGNED_NORMALIZED ||
                 _mesa_get_format_datatype(format) == GL_INT);
      }
   }

   namespace image_validity {
      /**
       * Check whether there is an image bound at the given index and write
       * the comparison result to f0.0.  Returns an appropriate predication
       * mode to use on subsequent image operations.
       */
      brw_predicate
      emit_surface_check(const fs_builder &bld, const fs_reg &image)
      {
         const brw_device_info *devinfo = bld.shader->devinfo;
         const fs_reg size = offset(image, bld, BRW_IMAGE_PARAM_SIZE_OFFSET);

         if (devinfo->gen == 7 && !devinfo->is_haswell) {
            /* Check the first component of the size field to find out if the
             * image is bound.  Necessary on IVB for typed atomics because
             * they don't seem to respect null surfaces and will happily
             * corrupt or read random memory when no image is bound.
             */
            bld.CMP(bld.null_reg_ud(),
                    retype(size, BRW_REGISTER_TYPE_UD),
                    fs_reg(0), BRW_CONDITIONAL_NZ);

            return BRW_PREDICATE_NORMAL;
         } else {
            /* More recent platforms implement compliant behavior when a null
             * surface is bound.
             */
            return BRW_PREDICATE_NONE;
         }
      }

      /**
       * Check whether the provided coordinates are within the image bounds
       * and write the comparison result to f0.0.  Returns an appropriate
       * predication mode to use on subsequent image operations.
       */
      brw_predicate
      emit_bounds_check(const fs_builder &bld, const fs_reg &image,
                        const fs_reg &addr, unsigned dims)
      {
         const fs_reg size = offset(image, bld, BRW_IMAGE_PARAM_SIZE_OFFSET);

         for (unsigned c = 0; c < dims; ++c)
            set_predicate(c == 0 ? BRW_PREDICATE_NONE : BRW_PREDICATE_NORMAL,
                          bld.CMP(bld.null_reg_ud(),
                                  offset(retype(addr, BRW_REGISTER_TYPE_UD), bld, c),
                                  offset(size, bld, c),
                                  BRW_CONDITIONAL_L));

         return BRW_PREDICATE_NORMAL;
      }
   }

   namespace image_coordinates {
      /**
       * Return the total number of coordinates needed to address a texel of
       * the surface, which may be more than the sum of \p surf_dims and \p
       * arr_dims if padding is required.
       */
      unsigned
      num_image_coordinates(const fs_builder &bld,
                            unsigned surf_dims, unsigned arr_dims,
                            mesa_format format)
      {
         /* HSW in vec4 mode and our software coordinate handling for untyped
          * reads want the array index to be at the Z component.
          */
         const bool array_index_at_z =
            !image_format_info::has_matching_typed_format(
               bld.shader->devinfo, format);
         const unsigned zero_dims =
            ((surf_dims == 1 && arr_dims == 1 && array_index_at_z) ? 1 : 0);

         return surf_dims + zero_dims + arr_dims;
      }

      /**
       * Transform image coordinates into the form expected by the
       * implementation.
       */
      fs_reg
      emit_image_coordinates(const fs_builder &bld, const fs_reg &addr,
                             unsigned surf_dims, unsigned arr_dims,
                             mesa_format format)
      {
         const unsigned dims =
            num_image_coordinates(bld, surf_dims, arr_dims, format);

         if (dims > surf_dims + arr_dims) {
            assert(surf_dims == 1 && arr_dims == 1 && dims == 3);
            /* The array index is required to be passed in as the Z component,
             * insert a zero at the Y component to shift it to the right
             * position.
             *
             * FINISHME: Factor out this frequently recurring pattern into a
             * helper function.
             */
            const fs_reg srcs[] = { addr, fs_reg(0), offset(addr, bld, 1) };
            const fs_reg dst = bld.vgrf(addr.type, dims);
            bld.LOAD_PAYLOAD(dst, srcs, dims, 0);
            return dst;
         } else {
            return addr;
         }
      }

      /**
       * Calculate the offset in memory of the texel given by \p coord.
       *
       * This is meant to be used with untyped surface messages to access a
       * tiled surface, what involves taking into account the tiling and
       * swizzling modes of the surface manually so it will hopefully not
       * happen very often.
       *
       * The tiling algorithm implemented here matches either the X or Y
       * tiling layouts supported by the hardware depending on the tiling
       * coefficients passed to the program as uniforms.  See Volume 1 Part 2
       * Section 4.5 "Address Tiling Function" of the IVB PRM for an in-depth
       * explanation of the hardware tiling format.
       */
      fs_reg
      emit_address_calculation(const fs_builder &bld, const fs_reg &image,
                               const fs_reg &coord, unsigned dims)
      {
         const brw_device_info *devinfo = bld.shader->devinfo;
         const fs_reg off = offset(image, bld, BRW_IMAGE_PARAM_OFFSET_OFFSET);
         const fs_reg stride = offset(image, bld, BRW_IMAGE_PARAM_STRIDE_OFFSET);
         const fs_reg tile = offset(image, bld, BRW_IMAGE_PARAM_TILING_OFFSET);
         const fs_reg swz = offset(image, bld, BRW_IMAGE_PARAM_SWIZZLING_OFFSET);
         const fs_reg addr = bld.vgrf(BRW_REGISTER_TYPE_UD, 2);
         const fs_reg tmp = bld.vgrf(BRW_REGISTER_TYPE_UD, 2);
         const fs_reg minor = bld.vgrf(BRW_REGISTER_TYPE_UD, 2);
         const fs_reg major = bld.vgrf(BRW_REGISTER_TYPE_UD, 2);
         const fs_reg dst = bld.vgrf(BRW_REGISTER_TYPE_UD);

         /* Shift the coordinates by the fixed surface offset.  It may be
          * non-zero if the image is a single slice of a higher-dimensional
          * surface, or if a non-zero mipmap level of the surface is bound to
          * the pipeline.  The offset needs to be applied here rather than at
          * surface state set-up time because the desired slice-level may
          * start mid-tile, so simply shifting the surface base address
          * wouldn't give a well-formed tiled surface in the general case.
          */
         for (unsigned c = 0; c < 2; ++c)
            bld.ADD(offset(addr, bld, c), offset(off, bld, c),
                    (c < dims ?
                     offset(retype(coord, BRW_REGISTER_TYPE_UD), bld, c) :
                     fs_reg(0)));

         /* The layout of 3-D textures in memory is sort-of like a tiling
          * format.  At each miplevel, the slices are arranged in rows of
          * 2^level slices per row.  The slice row is stored in tmp.y and
          * the slice within the row is stored in tmp.x.
          *
          * The layout of 2-D array textures and cubemaps is much simpler:
          * Depending on whether the ARYSPC_LOD0 layout is in use it will be
          * stored in memory as an array of slices, each one being a 2-D
          * arrangement of miplevels, or as a 2D arrangement of miplevels,
          * each one being an array of slices.  In either case the separation
          * between slices of the same LOD is equal to the qpitch value
          * provided as stride.w.
          *
          * This code can be made to handle either 2D arrays and 3D textures
          * by passing in the miplevel as tile.z for 3-D textures and 0 in
          * tile.z for 2-D array textures.
          *
          * See Volume 1 Part 1 of the Gen7 PRM, sections 6.18.4.7 "Surface
          * Arrays" and 6.18.6 "3D Surfaces" for a more extensive discussion
          * of the hardware 3D texture and 2D array layouts.
          */
         if (dims > 2) {
            /* Decompose z into a major (tmp.y) and a minor (tmp.x)
             * index.
             */
            bld.BFE(offset(tmp, bld, 0), offset(tile, bld, 2), fs_reg(0),
                    offset(retype(coord, BRW_REGISTER_TYPE_UD), bld, 2));
            bld.SHR(offset(tmp, bld, 1),
                    offset(retype(coord, BRW_REGISTER_TYPE_UD), bld, 2),
                    offset(tile, bld, 2));

            /* Take into account the horizontal (tmp.x) and vertical (tmp.y)
             * slice offset.
             */
            for (unsigned c = 0; c < 2; ++c) {
               bld.MUL(offset(tmp, bld, c),
                       offset(stride, bld, 2 + c), offset(tmp, bld, c));
               bld.ADD(offset(addr, bld, c),
                       offset(addr, bld, c), offset(tmp, bld, c));
            }
         }

         if (dims > 1) {
            /* Calculate the major/minor x and y indices.  In order to
             * accommodate both X and Y tiling, the Y-major tiling format is
             * treated as being a bunch of narrow X-tiles placed next to each
             * other.  This means that the tile width for Y-tiling is actually
             * the width of one sub-column of the Y-major tile where each 4K
             * tile has 8 512B sub-columns.
             *
             * The major Y value is the row of tiles in which the pixel lives.
             * The major X value is the tile sub-column in which the pixel
             * lives; for X tiling, this is the same as the tile column, for Y
             * tiling, each tile has 8 sub-columns.  The minor X and Y indices
             * are the position within the sub-column.
             */
            for (unsigned c = 0; c < 2; ++c) {
               /* Calculate the minor x and y indices. */
               bld.BFE(offset(minor, bld, c), offset(tile, bld, c),
                       fs_reg(0), offset(addr, bld, c));

               /* Calculate the major x and y indices. */
               bld.SHR(offset(major, bld, c),
                       offset(addr, bld, c), offset(tile, bld, c));
            }

            /* Calculate the texel index from the start of the tile row and
             * the vertical coordinate of the row.
             * Equivalent to:
             *   tmp.x = (major.x << tile.y << tile.x) +
             *           (minor.y << tile.x) + minor.x
             *   tmp.y = major.y << tile.y
             */
            bld.SHL(tmp, major, offset(tile, bld, 1));
            bld.ADD(tmp, tmp, offset(minor, bld, 1));
            bld.SHL(tmp, tmp, offset(tile, bld, 0));
            bld.ADD(tmp, tmp, minor);
            bld.SHL(offset(tmp, bld, 1),
                    offset(major, bld, 1), offset(tile, bld, 1));

            /* Add it to the start of the tile row. */
            bld.MUL(offset(tmp, bld, 1),
                    offset(tmp, bld, 1), offset(stride, bld, 1));
            bld.ADD(tmp, tmp, offset(tmp, bld, 1));

            /* Multiply by the Bpp value. */
            bld.MUL(dst, tmp, stride);

            if (devinfo->gen < 8 && !devinfo->is_baytrail) {
               /* Take into account the two dynamically specified shifts.
                * Both need are used to implement swizzling of X-tiled
                * surfaces.  For Y-tiled surfaces only one bit needs to be
                * XOR-ed with bit 6 of the memory address, so a swz value of
                * 0xff (actually interpreted as 31 by the hardware) will be
                * provided to cause the relevant bit of tmp.y to be zero and
                * turn the first XOR into the identity.  For linear surfaces
                * or platforms lacking address swizzling both shifts will be
                * 0xff causing the relevant bits of both tmp.x and .y to be
                * zero, what effectively disables swizzling.
                */
               for (unsigned c = 0; c < 2; ++c)
                  bld.SHR(offset(tmp, bld, c), dst, offset(swz, bld, c));

               /* XOR tmp.x and tmp.y with bit 6 of the memory address. */
               bld.XOR(tmp, tmp, offset(tmp, bld, 1));
               bld.AND(tmp, tmp, fs_reg(1 << 6));
               bld.XOR(dst, dst, tmp);
            }

         } else {
            /* Multiply by the Bpp/stride value.  Note that the addr.y may be
             * non-zero even if the image is one-dimensional because a
             * vertical offset may have been applied above to select a
             * non-zero slice or level of a higher-dimensional texture.
             */
            bld.MUL(offset(addr, bld, 1),
                    offset(addr, bld, 1), offset(stride, bld, 1));
            bld.ADD(addr, addr, offset(addr, bld, 1));
            bld.MUL(dst, addr, stride);
         }

         return dst;
      }
   }
}
