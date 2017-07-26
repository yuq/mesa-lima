/*
 * Copyright © 2017 Intel Corporation
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

#include "brw_reg.h"
#include "brw_eu_defines.h"
#include "common/gen_device_info.h"

#define INVALID (-1)

static const struct {
   enum hw_reg_type reg_type;
   enum hw_imm_type imm_type;
} gen4_hw_type[] = {
   [BRW_REGISTER_TYPE_DF] = { GEN7_HW_REG_TYPE_DF, GEN8_HW_IMM_TYPE_DF },
   [BRW_REGISTER_TYPE_F]  = { BRW_HW_REG_TYPE_F,   BRW_HW_IMM_TYPE_F   },
   [BRW_REGISTER_TYPE_HF] = { GEN8_HW_REG_TYPE_HF, GEN8_HW_IMM_TYPE_HF },
   [BRW_REGISTER_TYPE_VF] = { INVALID,             BRW_HW_IMM_TYPE_VF  },

   [BRW_REGISTER_TYPE_Q]  = { GEN8_HW_REG_TYPE_Q,  GEN8_HW_IMM_TYPE_Q  },
   [BRW_REGISTER_TYPE_UQ] = { GEN8_HW_REG_TYPE_UQ, GEN8_HW_IMM_TYPE_UQ },
   [BRW_REGISTER_TYPE_D]  = { BRW_HW_REG_TYPE_D,   BRW_HW_IMM_TYPE_D   },
   [BRW_REGISTER_TYPE_UD] = { BRW_HW_REG_TYPE_UD,  BRW_HW_IMM_TYPE_UD  },
   [BRW_REGISTER_TYPE_W]  = { BRW_HW_REG_TYPE_W,   BRW_HW_IMM_TYPE_W   },
   [BRW_REGISTER_TYPE_UW] = { BRW_HW_REG_TYPE_UW,  BRW_HW_IMM_TYPE_UW  },
   [BRW_REGISTER_TYPE_B]  = { BRW_HW_REG_TYPE_B,   INVALID             },
   [BRW_REGISTER_TYPE_UB] = { BRW_HW_REG_TYPE_UB,  INVALID             },
   [BRW_REGISTER_TYPE_V]  = { INVALID,             BRW_HW_IMM_TYPE_V   },
   [BRW_REGISTER_TYPE_UV] = { INVALID,             BRW_HW_IMM_TYPE_UV  },
};

/**
 * Convert a brw_reg_type enumeration value into the hardware representation.
 *
 * The hardware encoding may depend on whether the value is an immediate.
 */
unsigned
brw_reg_type_to_hw_type(const struct gen_device_info *devinfo,
                        enum brw_reg_file file,
                        enum brw_reg_type type)
{
   assert(type < ARRAY_SIZE(gen4_hw_type));

   if (file == BRW_IMMEDIATE_VALUE) {
      assert(gen4_hw_type[type].imm_type != (enum hw_imm_type)INVALID);
      return gen4_hw_type[type].imm_type;
   } else {
      assert(gen4_hw_type[type].reg_type != (enum hw_reg_type)INVALID);
      return gen4_hw_type[type].reg_type;
   }
}

/**
 * Convert the hardware representation into a brw_reg_type enumeration value.
 *
 * The hardware encoding may depend on whether the value is an immediate.
 */
enum brw_reg_type
brw_hw_type_to_reg_type(const struct gen_device_info *devinfo,
                        enum brw_reg_file file, unsigned hw_type)
{
   if (file == BRW_IMMEDIATE_VALUE) {
      for (enum brw_reg_type i = 0; i <= BRW_REGISTER_TYPE_LAST; i++) {
         if (gen4_hw_type[i].imm_type == hw_type) {
            return i;
         }
      }
   } else {
      for (enum brw_reg_type i = 0; i <= BRW_REGISTER_TYPE_LAST; i++) {
         if (gen4_hw_type[i].reg_type == hw_type) {
            return i;
         }
      }
   }
   unreachable("not reached");
}

/**
 * Return the element size given a hardware register type and file.
 *
 * The hardware encoding may depend on whether the value is an immediate.
 */
unsigned
brw_hw_reg_type_to_size(const struct gen_device_info *devinfo,
                        enum brw_reg_file file,
                        unsigned hw_type)
{
   static const unsigned type_size[] = {
      [BRW_REGISTER_TYPE_DF] = 8,
      [BRW_REGISTER_TYPE_F]  = 4,
      [BRW_REGISTER_TYPE_HF] = 2,
      [BRW_REGISTER_TYPE_VF] = 4,

      [BRW_REGISTER_TYPE_Q]  = 8,
      [BRW_REGISTER_TYPE_UQ] = 8,
      [BRW_REGISTER_TYPE_D]  = 4,
      [BRW_REGISTER_TYPE_UD] = 4,
      [BRW_REGISTER_TYPE_W]  = 2,
      [BRW_REGISTER_TYPE_UW] = 2,
      [BRW_REGISTER_TYPE_B]  = 1,
      [BRW_REGISTER_TYPE_UB] = 1,
      [BRW_REGISTER_TYPE_V]  = 2,
      [BRW_REGISTER_TYPE_UV] = 2,
   };
   enum brw_reg_type type = brw_hw_type_to_reg_type(devinfo, file, hw_type);
   return type_size[type];
}
