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

/** @file brw_eu_validate.c
 *
 * This file implements a pass that validates shader assembly.
 */

#include "brw_eu.h"

/* We're going to do lots of string concatenation, so this should help. */
struct string {
   char *str;
   size_t len;
};

static void
cat(struct string *dest, const struct string src)
{
   dest->str = realloc(dest->str, dest->len + src.len + 1);
   memcpy(dest->str + dest->len, src.str, src.len);
   dest->str[dest->len + src.len] = '\0';
   dest->len = dest->len + src.len;
}
#define CAT(dest, src) cat(&dest, (struct string){src, strlen(src)})

#define error(str) "\tERROR: " str "\n"

#define ERROR(msg) ERROR_IF(true, msg)
#define ERROR_IF(cond, msg)          \
   do {                              \
      if (cond) {                    \
         CAT(error_msg, error(msg)); \
      }                              \
   } while(0)

#define CHECK(func, args...)                             \
   do {                                                  \
      struct string __msg = func(devinfo, inst, ##args); \
      if (__msg.str) {                                   \
         cat(&error_msg, __msg);                         \
         free(__msg.str);                                \
      }                                                  \
   } while (0)

static bool
inst_is_send(const struct gen_device_info *devinfo, const brw_inst *inst)
{
   switch (brw_inst_opcode(devinfo, inst)) {
   case BRW_OPCODE_SEND:
   case BRW_OPCODE_SENDC:
   case BRW_OPCODE_SENDS:
   case BRW_OPCODE_SENDSC:
      return true;
   default:
      return false;
   }
}

static bool
dst_is_null(const struct gen_device_info *devinfo, const brw_inst *inst)
{
   return brw_inst_dst_reg_file(devinfo, inst) == BRW_ARCHITECTURE_REGISTER_FILE &&
          brw_inst_dst_da_reg_nr(devinfo, inst) == BRW_ARF_NULL;
}

static bool
src0_is_null(const struct gen_device_info *devinfo, const brw_inst *inst)
{
   return brw_inst_src0_reg_file(devinfo, inst) == BRW_ARCHITECTURE_REGISTER_FILE &&
          brw_inst_src0_da_reg_nr(devinfo, inst) == BRW_ARF_NULL;
}

static bool
src1_is_null(const struct gen_device_info *devinfo, const brw_inst *inst)
{
   return brw_inst_src1_reg_file(devinfo, inst) == BRW_ARCHITECTURE_REGISTER_FILE &&
          brw_inst_src1_da_reg_nr(devinfo, inst) == BRW_ARF_NULL;
}

static bool
src0_is_grf(const struct gen_device_info *devinfo, const brw_inst *inst)
{
   return brw_inst_src0_reg_file(devinfo, inst) == BRW_GENERAL_REGISTER_FILE;
}

static unsigned
num_sources_from_inst(const struct gen_device_info *devinfo,
                      const brw_inst *inst)
{
   const struct opcode_desc *desc =
      brw_opcode_desc(devinfo, brw_inst_opcode(devinfo, inst));
   unsigned math_function;

   if (brw_inst_opcode(devinfo, inst) == BRW_OPCODE_MATH) {
      math_function = brw_inst_math_function(devinfo, inst);
   } else if (devinfo->gen < 6 &&
              brw_inst_opcode(devinfo, inst) == BRW_OPCODE_SEND) {
      if (brw_inst_sfid(devinfo, inst) == BRW_SFID_MATH) {
         /* src1 must be a descriptor (including the information to determine
          * that the SEND is doing an extended math operation), but src0 can
          * actually be null since it serves as the source of the implicit GRF
          * to MRF move.
          *
          * If we stop using that functionality, we'll have to revisit this.
          */
         return 2;
      } else {
         /* Send instructions are allowed to have null sources since they use
          * the base_mrf field to specify which message register source.
          */
         return 0;
      }
   } else {
      return desc->nsrc;
   }

   switch (math_function) {
   case BRW_MATH_FUNCTION_INV:
   case BRW_MATH_FUNCTION_LOG:
   case BRW_MATH_FUNCTION_EXP:
   case BRW_MATH_FUNCTION_SQRT:
   case BRW_MATH_FUNCTION_RSQ:
   case BRW_MATH_FUNCTION_SIN:
   case BRW_MATH_FUNCTION_COS:
   case BRW_MATH_FUNCTION_SINCOS:
   case GEN8_MATH_FUNCTION_INVM:
   case GEN8_MATH_FUNCTION_RSQRTM:
      return 1;
   case BRW_MATH_FUNCTION_FDIV:
   case BRW_MATH_FUNCTION_POW:
   case BRW_MATH_FUNCTION_INT_DIV_QUOTIENT_AND_REMAINDER:
   case BRW_MATH_FUNCTION_INT_DIV_QUOTIENT:
   case BRW_MATH_FUNCTION_INT_DIV_REMAINDER:
      return 2;
   default:
      unreachable("not reached");
   }
}

static struct string
sources_not_null(const struct gen_device_info *devinfo,
                 const brw_inst *inst)
{
   unsigned num_sources = num_sources_from_inst(devinfo, inst);
   struct string error_msg = { .str = NULL, .len = 0 };

   /* Nothing to test. 3-src instructions can only have GRF sources, and
    * there's no bit to control the file.
    */
   if (num_sources == 3)
      return (struct string){};

   if (num_sources >= 1)
      ERROR_IF(src0_is_null(devinfo, inst), "src0 is null");

   if (num_sources == 2)
      ERROR_IF(src1_is_null(devinfo, inst), "src1 is null");

   return error_msg;
}

static struct string
send_restrictions(const struct gen_device_info *devinfo,
                  const brw_inst *inst)
{
   struct string error_msg = { .str = NULL, .len = 0 };

   if (brw_inst_opcode(devinfo, inst) == BRW_OPCODE_SEND) {
      ERROR_IF(brw_inst_src0_address_mode(devinfo, inst) != BRW_ADDRESS_DIRECT,
               "send must use direct addressing");

      if (devinfo->gen >= 7) {
         ERROR_IF(!src0_is_grf(devinfo, inst), "send from non-GRF");
         ERROR_IF(brw_inst_eot(devinfo, inst) &&
                  brw_inst_src0_da_reg_nr(devinfo, inst) < 112,
                  "send with EOT must use g112-g127");
      }
   }

   return error_msg;
}

static bool
is_unsupported_inst(const struct gen_device_info *devinfo,
                    const brw_inst *inst)
{
   return brw_opcode_desc(devinfo, brw_inst_opcode(devinfo, inst)) == NULL;
}

static unsigned
execution_type_for_type(unsigned type, bool is_immediate)
{
   /* The meaning of the type bits is dependent on whether the operand is an
    * immediate, so normalize them first.
    */
   if (is_immediate) {
      switch (type) {
      case BRW_HW_REG_IMM_TYPE_UV:
      case BRW_HW_REG_IMM_TYPE_V:
         type = BRW_HW_REG_TYPE_W;
         break;
      case BRW_HW_REG_IMM_TYPE_VF:
         type = BRW_HW_REG_TYPE_F;
         break;
      case GEN8_HW_REG_IMM_TYPE_DF:
         type = GEN7_HW_REG_NON_IMM_TYPE_DF;
         break;
      case GEN8_HW_REG_IMM_TYPE_HF:
         type = GEN8_HW_REG_NON_IMM_TYPE_HF;
         break;
      default:
         break;
      }
   }

   switch (type) {
   case BRW_HW_REG_TYPE_UD:
   case BRW_HW_REG_TYPE_D:
      return BRW_HW_REG_TYPE_D;
   case BRW_HW_REG_TYPE_UW:
   case BRW_HW_REG_TYPE_W:
   case BRW_HW_REG_NON_IMM_TYPE_UB:
   case BRW_HW_REG_NON_IMM_TYPE_B:
      return BRW_HW_REG_TYPE_W;
   case GEN8_HW_REG_TYPE_UQ:
   case GEN8_HW_REG_TYPE_Q:
      return GEN8_HW_REG_TYPE_Q;
   case BRW_HW_REG_TYPE_F:
   case GEN7_HW_REG_NON_IMM_TYPE_DF:
   case GEN8_HW_REG_NON_IMM_TYPE_HF:
      return type;
   default:
      unreachable("not reached");
   }
}

/**
 * Returns the execution type of an instruction \p inst
 */
static unsigned
execution_type(const struct gen_device_info *devinfo, const brw_inst *inst)
{
   unsigned num_sources = num_sources_from_inst(devinfo, inst);
   unsigned src0_exec_type, src1_exec_type;
   unsigned src0_type = brw_inst_src0_reg_type(devinfo, inst);
   unsigned src1_type = brw_inst_src1_reg_type(devinfo, inst);

   bool src0_is_immediate =
      brw_inst_src0_reg_file(devinfo, inst) == BRW_IMMEDIATE_VALUE;
   bool src1_is_immediate =
      brw_inst_src1_reg_file(devinfo, inst) == BRW_IMMEDIATE_VALUE;

   /* Execution data type is independent of destination data type, except in
    * mixed F/HF instructions on CHV and SKL+.
    */
   unsigned dst_exec_type = brw_inst_dst_reg_type(devinfo, inst);

   src0_exec_type = execution_type_for_type(src0_type, src0_is_immediate);
   if (num_sources == 1) {
      if ((devinfo->gen >= 9 || devinfo->is_cherryview) &&
          src0_exec_type == GEN8_HW_REG_NON_IMM_TYPE_HF) {
         return dst_exec_type;
      }
      return src0_exec_type;
   }

   src1_exec_type = execution_type_for_type(src1_type, src1_is_immediate);
   if (src0_exec_type == src1_exec_type)
      return src0_exec_type;

   /* Mixed operand types where one is float is float on Gen < 6
    * (and not allowed on later platforms)
    */
   if (devinfo->gen < 6 &&
       (src0_exec_type == BRW_HW_REG_TYPE_F ||
        src1_exec_type == BRW_HW_REG_TYPE_F))
      return BRW_HW_REG_TYPE_F;

   if (src0_exec_type == GEN8_HW_REG_TYPE_Q ||
       src1_exec_type == GEN8_HW_REG_TYPE_Q)
      return GEN8_HW_REG_TYPE_Q;

   if (src0_exec_type == BRW_HW_REG_TYPE_D ||
       src1_exec_type == BRW_HW_REG_TYPE_D)
      return BRW_HW_REG_TYPE_D;

   if (src0_exec_type == BRW_HW_REG_TYPE_W ||
       src1_exec_type == BRW_HW_REG_TYPE_W)
      return BRW_HW_REG_TYPE_W;

   if (src0_exec_type == GEN7_HW_REG_NON_IMM_TYPE_DF ||
       src1_exec_type == GEN7_HW_REG_NON_IMM_TYPE_DF)
      return GEN7_HW_REG_NON_IMM_TYPE_DF;

   if (devinfo->gen >= 9 || devinfo->is_cherryview) {
      if (dst_exec_type == BRW_HW_REG_TYPE_F ||
          src0_exec_type == BRW_HW_REG_TYPE_F ||
          src1_exec_type == BRW_HW_REG_TYPE_F) {
         return BRW_HW_REG_TYPE_F;
      } else {
         return GEN8_HW_REG_NON_IMM_TYPE_HF;
      }
   }

   assert(src0_exec_type == BRW_HW_REG_TYPE_F);
   return BRW_HW_REG_TYPE_F;
}

/**
 * Checks restrictions listed in "General Restrictions Based on Operand Types"
 * in the "Register Region Restrictions" section.
 */
static struct string
general_restrictions_based_on_operand_types(const struct gen_device_info *devinfo,
                                            const brw_inst *inst)
{
   const struct opcode_desc *desc =
      brw_opcode_desc(devinfo, brw_inst_opcode(devinfo, inst));
   unsigned num_sources = num_sources_from_inst(devinfo, inst);
   unsigned exec_size = 1 << brw_inst_exec_size(devinfo, inst);
   struct string error_msg = { .str = NULL, .len = 0 };

   if (num_sources == 3)
      return (struct string){};

   if (inst_is_send(devinfo, inst))
      return (struct string){};

   if (exec_size == 1)
      return (struct string){};

   if (desc->ndst == 0)
      return (struct string){};

   /* The PRMs say:
    *
    *    Where n is the largest element size in bytes for any source or
    *    destination operand type, ExecSize * n must be <= 64.
    *
    * But we do not attempt to enforce it, because it is implied by other
    * rules:
    *
    *    - that the destination stride must match the execution data type
    *    - sources may not span more than two adjacent GRF registers
    *    - destination may not span more than two adjacent GRF registers
    *
    * In fact, checking it would weaken testing of the other rules.
    */

   if (num_sources == 3)
      return (struct string){};

   if (exec_size == 1)
      return (struct string){};

   if (inst_is_send(devinfo, inst))
      return (struct string){};

   if (desc->ndst == 0)
      return (struct string){};

   /* FINISHME: check special cases for byte operations */
   if (brw_inst_dst_reg_type(devinfo, inst) == BRW_HW_REG_NON_IMM_TYPE_B ||
       brw_inst_dst_reg_type(devinfo, inst) == BRW_HW_REG_NON_IMM_TYPE_UB)
      return (struct string){};

   unsigned exec_type = execution_type(devinfo, inst);
   unsigned exec_type_size =
      brw_hw_reg_type_to_size(devinfo, exec_type, BRW_GENERAL_REGISTER_FILE);
   unsigned dst_type_size = brw_element_size(devinfo, inst, dst);

   if (exec_type_size > dst_type_size) {
      unsigned dst_stride = 1 << (brw_inst_dst_hstride(devinfo, inst) - 1);
      ERROR_IF(dst_stride * dst_type_size != exec_type_size,
               "Destination stride must be equal to the ratio of the sizes of "
               "the execution data type to the destination type");

      if (brw_inst_access_mode(devinfo, inst) == BRW_ALIGN_1 &&
          brw_inst_dst_address_mode(devinfo, inst) == BRW_ADDRESS_DIRECT) {
         unsigned subreg = brw_inst_dst_da1_subreg_nr(devinfo, inst);
         ERROR_IF(subreg % exec_type_size != 0,
                  "Destination subreg must be aligned to the size of the "
                  "execution data type");
      }
   }

   return error_msg;
}

/**
 * Checks restrictions listed in "General Restrictions on Regioning Parameters"
 * in the "Register Region Restrictions" section.
 */
static struct string
general_restrictions_on_region_parameters(const struct gen_device_info *devinfo,
                                          const brw_inst *inst)
{
   const struct opcode_desc *desc =
      brw_opcode_desc(devinfo, brw_inst_opcode(devinfo, inst));
   unsigned num_sources = num_sources_from_inst(devinfo, inst);
   unsigned exec_size = 1 << brw_inst_exec_size(devinfo, inst);
   struct string error_msg = { .str = NULL, .len = 0 };

   if (num_sources == 3)
      return (struct string){};

   if (brw_inst_access_mode(devinfo, inst) == BRW_ALIGN_16) {
      if (desc->ndst != 0 && !dst_is_null(devinfo, inst))
         ERROR_IF(brw_inst_dst_hstride(devinfo, inst) != BRW_HORIZONTAL_STRIDE_1,
                  "Destination Horizontal Stride must be 1");

      if (num_sources >= 1) {
         if (devinfo->is_haswell || devinfo->gen >= 8) {
            ERROR_IF(brw_inst_src0_reg_file(devinfo, inst) != BRW_IMMEDIATE_VALUE &&
                     brw_inst_src0_vstride(devinfo, inst) != BRW_VERTICAL_STRIDE_0 &&
                     brw_inst_src0_vstride(devinfo, inst) != BRW_VERTICAL_STRIDE_2 &&
                     brw_inst_src0_vstride(devinfo, inst) != BRW_VERTICAL_STRIDE_4,
                     "In Align16 mode, only VertStride of 0, 2, or 4 is allowed");
         } else {
            ERROR_IF(brw_inst_src0_reg_file(devinfo, inst) != BRW_IMMEDIATE_VALUE &&
                     brw_inst_src0_vstride(devinfo, inst) != BRW_VERTICAL_STRIDE_0 &&
                     brw_inst_src0_vstride(devinfo, inst) != BRW_VERTICAL_STRIDE_4,
                     "In Align16 mode, only VertStride of 0 or 4 is allowed");
         }
      }

      if (num_sources == 2) {
         if (devinfo->is_haswell || devinfo->gen >= 8) {
            ERROR_IF(brw_inst_src1_reg_file(devinfo, inst) != BRW_IMMEDIATE_VALUE &&
                     brw_inst_src1_vstride(devinfo, inst) != BRW_VERTICAL_STRIDE_0 &&
                     brw_inst_src1_vstride(devinfo, inst) != BRW_VERTICAL_STRIDE_2 &&
                     brw_inst_src1_vstride(devinfo, inst) != BRW_VERTICAL_STRIDE_4,
                     "In Align16 mode, only VertStride of 0, 2, or 4 is allowed");
         } else {
            ERROR_IF(brw_inst_src1_reg_file(devinfo, inst) != BRW_IMMEDIATE_VALUE &&
                     brw_inst_src1_vstride(devinfo, inst) != BRW_VERTICAL_STRIDE_0 &&
                     brw_inst_src1_vstride(devinfo, inst) != BRW_VERTICAL_STRIDE_4,
                     "In Align16 mode, only VertStride of 0 or 4 is allowed");
         }
      }

      return error_msg;
   }

   for (unsigned i = 0; i < num_sources; i++) {
      unsigned vstride, width, hstride, element_size, subreg;

#define DO_SRC(n)                                                              \
      if (brw_inst_src ## n ## _reg_file(devinfo, inst) ==                     \
          BRW_IMMEDIATE_VALUE)                                                 \
         continue;                                                             \
                                                                               \
      vstride = brw_inst_src ## n ## _vstride(devinfo, inst) ?                 \
                (1 << (brw_inst_src ## n ## _vstride(devinfo, inst) - 1)) : 0; \
      width = 1 << brw_inst_src ## n ## _width(devinfo, inst);                 \
      hstride = brw_inst_src ## n ## _hstride(devinfo, inst) ?                 \
                (1 << (brw_inst_src ## n ## _hstride(devinfo, inst) - 1)) : 0; \
      element_size = brw_element_size(devinfo, inst, src ## n);                \
      subreg = brw_inst_src ## n ## _da1_subreg_nr(devinfo, inst)

      if (i == 0) {
         DO_SRC(0);
      } else if (i == 1) {
         DO_SRC(1);
      }
#undef DO_SRC

      /* ExecSize must be greater than or equal to Width. */
      ERROR_IF(exec_size < width, "ExecSize must be greater than or equal "
                                  "to Width");

      /* If ExecSize = Width and HorzStride ≠ 0,
       * VertStride must be set to Width * HorzStride.
       */
      if (exec_size == width && hstride != 0) {
         ERROR_IF(vstride != width * hstride,
                  "If ExecSize = Width and HorzStride ≠ 0, "
                  "VertStride must be set to Width * HorzStride");
      }

      /* If Width = 1, HorzStride must be 0 regardless of the values of
       * ExecSize and VertStride.
       */
      if (width == 1) {
         ERROR_IF(hstride != 0,
                  "If Width = 1, HorzStride must be 0 regardless "
                  "of the values of ExecSize and VertStride");
      }

      /* If ExecSize = Width = 1, both VertStride and HorzStride must be 0. */
      if (exec_size == 1 && width == 1) {
         ERROR_IF(vstride != 0 || hstride != 0,
                  "If ExecSize = Width = 1, both VertStride "
                  "and HorzStride must be 0");
      }

      /* If VertStride = HorzStride = 0, Width must be 1 regardless of the
       * value of ExecSize.
       */
      if (vstride == 0 && hstride == 0) {
         ERROR_IF(width != 1,
                  "If VertStride = HorzStride = 0, Width must be "
                  "1 regardless of the value of ExecSize");
      }

      /* VertStride must be used to cross GRF register boundaries. This rule
       * implies that elements within a 'Width' cannot cross GRF boundaries.
       */
      const uint64_t mask = (1 << element_size) - 1;
      unsigned rowbase = subreg;

      for (int y = 0; y < exec_size / width; y++) {
         uint64_t access_mask = 0;
         unsigned offset = rowbase;

         for (int x = 0; x < width; x++) {
            access_mask |= mask << offset;
            offset += hstride * element_size;
         }

         rowbase += vstride * element_size;

         if ((uint32_t)access_mask != 0 && (access_mask >> 32) != 0) {
            ERROR("VertStride must be used to cross GRF register boundaries");
            break;
         }
      }
   }

   /* Dst.HorzStride must not be 0. */
   if (desc->ndst != 0 && !dst_is_null(devinfo, inst)) {
      ERROR_IF(brw_inst_dst_hstride(devinfo, inst) == BRW_HORIZONTAL_STRIDE_0,
               "Destination Horizontal Stride must not be 0");
   }

   return error_msg;
}

bool
brw_validate_instructions(const struct brw_codegen *p, int start_offset,
                          struct annotation_info *annotation)
{
   const struct gen_device_info *devinfo = p->devinfo;
   const void *store = p->store;
   bool valid = true;

   for (int src_offset = start_offset; src_offset < p->next_insn_offset;
        src_offset += sizeof(brw_inst)) {
      struct string error_msg = { .str = NULL, .len = 0 };
      const brw_inst *inst = store + src_offset;

      if (is_unsupported_inst(devinfo, inst)) {
         ERROR("Instruction not supported on this Gen");
      } else {
         CHECK(sources_not_null);
         CHECK(send_restrictions);
         CHECK(general_restrictions_based_on_operand_types);
         CHECK(general_restrictions_on_region_parameters);
      }

      if (error_msg.str && annotation) {
         annotation_insert_error(annotation, src_offset, error_msg.str);
      }
      valid = valid && error_msg.len == 0;
      free(error_msg.str);
   }

   return valid;
}
