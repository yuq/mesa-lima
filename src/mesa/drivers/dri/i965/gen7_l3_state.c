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

/**
 * Program the hardware to use the specified L3 configuration.
 */
static void
setup_l3_config(struct brw_context *brw, const struct brw_l3_config *cfg)
{
   const bool has_dc = cfg->n[L3P_DC] || cfg->n[L3P_ALL];
   const bool has_is = cfg->n[L3P_IS] || cfg->n[L3P_RO] || cfg->n[L3P_ALL];
   const bool has_c = cfg->n[L3P_C] || cfg->n[L3P_RO] || cfg->n[L3P_ALL];
   const bool has_t = cfg->n[L3P_T] || cfg->n[L3P_RO] || cfg->n[L3P_ALL];
   const bool has_slm = cfg->n[L3P_SLM];

   /* According to the hardware docs, the L3 partitioning can only be changed
    * while the pipeline is completely drained and the caches are flushed,
    * which involves a first PIPE_CONTROL flush which stalls the pipeline and
    * initiates invalidation of the relevant caches...
    */
   brw_emit_pipe_control_flush(brw,
                               PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
                               PIPE_CONTROL_CONST_CACHE_INVALIDATE |
                               PIPE_CONTROL_INSTRUCTION_INVALIDATE |
                               PIPE_CONTROL_DATA_CACHE_INVALIDATE |
                               PIPE_CONTROL_NO_WRITE |
                               PIPE_CONTROL_CS_STALL);

   /* ...followed by a second stalling flush which guarantees that
    * invalidation is complete when the L3 configuration registers are
    * modified.
    */
   brw_emit_pipe_control_flush(brw,
                               PIPE_CONTROL_DATA_CACHE_INVALIDATE |
                               PIPE_CONTROL_NO_WRITE |
                               PIPE_CONTROL_CS_STALL);

   if (brw->gen >= 8) {
      assert(!cfg->n[L3P_IS] && !cfg->n[L3P_C] && !cfg->n[L3P_T]);

      BEGIN_BATCH(3);
      OUT_BATCH(MI_LOAD_REGISTER_IMM | (3 - 2));

      /* Set up the L3 partitioning. */
      OUT_BATCH(GEN8_L3CNTLREG);
      OUT_BATCH((has_slm ? GEN8_L3CNTLREG_SLM_ENABLE : 0) |
                SET_FIELD(cfg->n[L3P_URB], GEN8_L3CNTLREG_URB_ALLOC) |
                SET_FIELD(cfg->n[L3P_RO], GEN8_L3CNTLREG_RO_ALLOC) |
                SET_FIELD(cfg->n[L3P_DC], GEN8_L3CNTLREG_DC_ALLOC) |
                SET_FIELD(cfg->n[L3P_ALL], GEN8_L3CNTLREG_ALL_ALLOC));

      ADVANCE_BATCH();

   } else {
      assert(!cfg->n[L3P_ALL]);

      /* When enabled SLM only uses a portion of the L3 on half of the banks,
       * the matching space on the remaining banks has to be allocated to a
       * client (URB for all validated configurations) set to the
       * lower-bandwidth 2-bank address hashing mode.
       */
      const bool urb_low_bw = has_slm && !brw->is_baytrail;
      assert(!urb_low_bw || cfg->n[L3P_URB] == cfg->n[L3P_SLM]);

      /* Minimum number of ways that can be allocated to the URB. */
      const unsigned n0_urb = (brw->is_baytrail ? 32 : 0);
      assert(cfg->n[L3P_URB] >= n0_urb);

      BEGIN_BATCH(7);
      OUT_BATCH(MI_LOAD_REGISTER_IMM | (7 - 2));

      /* Demote any clients with no ways assigned to LLC. */
      OUT_BATCH(GEN7_L3SQCREG1);
      OUT_BATCH((brw->is_haswell ? HSW_L3SQCREG1_SQGHPCI_DEFAULT :
                 brw->is_baytrail ? VLV_L3SQCREG1_SQGHPCI_DEFAULT :
                 IVB_L3SQCREG1_SQGHPCI_DEFAULT) |
                (has_dc ? 0 : GEN7_L3SQCREG1_CONV_DC_UC) |
                (has_is ? 0 : GEN7_L3SQCREG1_CONV_IS_UC) |
                (has_c ? 0 : GEN7_L3SQCREG1_CONV_C_UC) |
                (has_t ? 0 : GEN7_L3SQCREG1_CONV_T_UC));

      /* Set up the L3 partitioning. */
      OUT_BATCH(GEN7_L3CNTLREG2);
      OUT_BATCH((has_slm ? GEN7_L3CNTLREG2_SLM_ENABLE : 0) |
                SET_FIELD(cfg->n[L3P_URB] - n0_urb, GEN7_L3CNTLREG2_URB_ALLOC) |
                (urb_low_bw ? GEN7_L3CNTLREG2_URB_LOW_BW : 0) |
                SET_FIELD(cfg->n[L3P_ALL], GEN7_L3CNTLREG2_ALL_ALLOC) |
                SET_FIELD(cfg->n[L3P_RO], GEN7_L3CNTLREG2_RO_ALLOC) |
                SET_FIELD(cfg->n[L3P_DC], GEN7_L3CNTLREG2_DC_ALLOC));
      OUT_BATCH(GEN7_L3CNTLREG3);
      OUT_BATCH(SET_FIELD(cfg->n[L3P_IS], GEN7_L3CNTLREG3_IS_ALLOC) |
                SET_FIELD(cfg->n[L3P_C], GEN7_L3CNTLREG3_C_ALLOC) |
                SET_FIELD(cfg->n[L3P_T], GEN7_L3CNTLREG3_T_ALLOC));

      ADVANCE_BATCH();

      if (brw->is_haswell && brw->intelScreen->cmd_parser_version >= 4) {
         /* Enable L3 atomics on HSW if we have a DC partition, otherwise keep
          * them disabled to avoid crashing the system hard.
          */
         BEGIN_BATCH(5);
         OUT_BATCH(MI_LOAD_REGISTER_IMM | (5 - 2));
         OUT_BATCH(HSW_SCRATCH1);
         OUT_BATCH(has_dc ? 0 : HSW_SCRATCH1_L3_ATOMIC_DISABLE);
         OUT_BATCH(HSW_ROW_CHICKEN3);
         OUT_BATCH(HSW_ROW_CHICKEN3_L3_ATOMIC_DISABLE << 16 |
                   (has_dc ? 0 : HSW_ROW_CHICKEN3_L3_ATOMIC_DISABLE));
         ADVANCE_BATCH();
      }
   }
}
