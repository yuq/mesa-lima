/*
 * Copyright (c) 2015 Intel Corporation
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

#include "brw_context.h"
#include "brw_defines.h"
#include "brw_state.h"
#include "intel_batchbuffer.h"

/**
 * Chunk of L3 cache reserved for some specific purpose.
 */
enum brw_l3_partition {
   /** Shared local memory. */
   L3P_SLM = 0,
   /** Unified return buffer. */
   L3P_URB,
   /** Union of DC and RO. */
   L3P_ALL,
   /** Data cluster RW partition. */
   L3P_DC,
   /** Union of IS, C and T. */
   L3P_RO,
   /** Instruction and state cache. */
   L3P_IS,
   /** Constant cache. */
   L3P_C,
   /** Texture cache. */
   L3P_T,
   /** Number of supported L3 partitions. */
   NUM_L3P
};

/**
 * L3 configuration represented as the number of ways allocated for each
 * partition.  \sa get_l3_way_size().
 */
struct brw_l3_config {
   unsigned n[NUM_L3P];
};

/**
 * IVB/HSW validated L3 configurations.
 */
static const struct brw_l3_config ivb_l3_configs[] = {
   /* SLM URB ALL DC  RO  IS   C   T */
   {{  0, 32,  0,  0, 32,  0,  0,  0 }},
   {{  0, 32,  0, 16, 16,  0,  0,  0 }},
   {{  0, 32,  0,  4,  0,  8,  4, 16 }},
   {{  0, 28,  0,  8,  0,  8,  4, 16 }},
   {{  0, 28,  0, 16,  0,  8,  4,  8 }},
   {{  0, 28,  0,  8,  0, 16,  4,  8 }},
   {{  0, 28,  0,  0,  0, 16,  4, 16 }},
   {{  0, 32,  0,  0,  0, 16,  0, 16 }},
   {{  0, 28,  0,  4, 32,  0,  0,  0 }},
   {{ 16, 16,  0, 16, 16,  0,  0,  0 }},
   {{ 16, 16,  0,  8,  0,  8,  8,  8 }},
   {{ 16, 16,  0,  4,  0,  8,  4, 16 }},
   {{ 16, 16,  0,  4,  0, 16,  4,  8 }},
   {{ 16, 16,  0,  0, 32,  0,  0,  0 }},
   {{ 0 }}
};

/**
 * VLV validated L3 configurations.
 */
static const struct brw_l3_config vlv_l3_configs[] = {
   /* SLM URB ALL DC  RO  IS   C   T */
   {{  0, 64,  0,  0, 32,  0,  0,  0 }},
   {{  0, 80,  0,  0, 16,  0,  0,  0 }},
   {{  0, 80,  0,  8,  8,  0,  0,  0 }},
   {{  0, 64,  0, 16, 16,  0,  0,  0 }},
   {{  0, 60,  0,  4, 32,  0,  0,  0 }},
   {{ 32, 32,  0, 16, 16,  0,  0,  0 }},
   {{ 32, 40,  0,  8, 16,  0,  0,  0 }},
   {{ 32, 40,  0, 16,  8,  0,  0,  0 }},
   {{ 0 }}
};

/**
 * BDW validated L3 configurations.
 */
static const struct brw_l3_config bdw_l3_configs[] = {
   /* SLM URB ALL DC  RO  IS   C   T */
   {{  0, 48, 48,  0,  0,  0,  0,  0 }},
   {{  0, 48,  0, 16, 32,  0,  0,  0 }},
   {{  0, 32,  0, 16, 48,  0,  0,  0 }},
   {{  0, 32,  0,  0, 64,  0,  0,  0 }},
   {{  0, 32, 64,  0,  0,  0,  0,  0 }},
   {{ 24, 16, 48,  0,  0,  0,  0,  0 }},
   {{ 24, 16,  0, 16, 32,  0,  0,  0 }},
   {{ 24, 16,  0, 32, 16,  0,  0,  0 }},
   {{ 0 }}
};

/**
 * CHV/SKL validated L3 configurations.
 */
static const struct brw_l3_config chv_l3_configs[] = {
   /* SLM URB ALL DC  RO  IS   C   T */
   {{  0, 48, 48,  0,  0,  0,  0,  0 }},
   {{  0, 48,  0, 16, 32,  0,  0,  0 }},
   {{  0, 32,  0, 16, 48,  0,  0,  0 }},
   {{  0, 32,  0,  0, 64,  0,  0,  0 }},
   {{  0, 32, 64,  0,  0,  0,  0,  0 }},
   {{ 32, 16, 48,  0,  0,  0,  0,  0 }},
   {{ 32, 16,  0, 16, 32,  0,  0,  0 }},
   {{ 32, 16,  0, 32, 16,  0,  0,  0 }},
   {{ 0 }}
};

/**
 * Return a zero-terminated array of validated L3 configurations for the
 * specified device.
 */
static const struct brw_l3_config *
get_l3_configs(const struct brw_device_info *devinfo)
{
   switch (devinfo->gen) {
   case 7:
      return (devinfo->is_baytrail ? vlv_l3_configs : ivb_l3_configs);

   case 8:
      return (devinfo->is_cherryview ? chv_l3_configs : bdw_l3_configs);

   case 9:
      return chv_l3_configs;

   default:
      unreachable("Not implemented");
   }
}

/**
 * Return the size of an L3 way in KB.
 */
static unsigned
get_l3_way_size(const struct brw_device_info *devinfo)
{
   if (devinfo->is_baytrail)
      return 2;

   else if (devinfo->is_cherryview || devinfo->gt == 1)
      return 4;

   else
      return 8 * devinfo->num_slices;
}
