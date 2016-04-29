/*
 Copyright (C) Intel Corp.  2006.  All Rights Reserved.
 Intel funded Tungsten Graphics to
 develop this 3D driver.

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sublicense, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:

 The above copyright notice and this permission notice (including the
 next paragraph) shall be included in all copies or substantial
 portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 **********************************************************************/
 /*
  * Authors:
  *   Keith Whitwell <keithw@vmware.com>
  */


#include "brw_context.h"
#include "brw_defines.h"
#include "brw_eu.h"

#include "util/ralloc.h"

/**
 * Converts a BRW_REGISTER_TYPE_* enum to a short string (F, UD, and so on).
 *
 * This is different than reg_encoding from brw_disasm.c in that it operates
 * on the abstract enum values, rather than the generation-specific encoding.
 */
const char *
brw_reg_type_letters(unsigned type)
{
   const char *names[] = {
      [BRW_REGISTER_TYPE_UD] = "UD",
      [BRW_REGISTER_TYPE_D]  = "D",
      [BRW_REGISTER_TYPE_UW] = "UW",
      [BRW_REGISTER_TYPE_W]  = "W",
      [BRW_REGISTER_TYPE_F]  = "F",
      [BRW_REGISTER_TYPE_UB] = "UB",
      [BRW_REGISTER_TYPE_B]  = "B",
      [BRW_REGISTER_TYPE_UV] = "UV",
      [BRW_REGISTER_TYPE_V]  = "V",
      [BRW_REGISTER_TYPE_VF] = "VF",
      [BRW_REGISTER_TYPE_DF] = "DF",
      [BRW_REGISTER_TYPE_HF] = "HF",
      [BRW_REGISTER_TYPE_UQ] = "UQ",
      [BRW_REGISTER_TYPE_Q]  = "Q",
   };
   assert(type <= BRW_REGISTER_TYPE_Q);
   return names[type];
}

/* Returns a conditional modifier that negates the condition. */
enum brw_conditional_mod
brw_negate_cmod(uint32_t cmod)
{
   switch (cmod) {
   case BRW_CONDITIONAL_Z:
      return BRW_CONDITIONAL_NZ;
   case BRW_CONDITIONAL_NZ:
      return BRW_CONDITIONAL_Z;
   case BRW_CONDITIONAL_G:
      return BRW_CONDITIONAL_LE;
   case BRW_CONDITIONAL_GE:
      return BRW_CONDITIONAL_L;
   case BRW_CONDITIONAL_L:
      return BRW_CONDITIONAL_GE;
   case BRW_CONDITIONAL_LE:
      return BRW_CONDITIONAL_G;
   default:
      return ~0;
   }
}

/* Returns the corresponding conditional mod for swapping src0 and
 * src1 in e.g. CMP.
 */
enum brw_conditional_mod
brw_swap_cmod(uint32_t cmod)
{
   switch (cmod) {
   case BRW_CONDITIONAL_Z:
   case BRW_CONDITIONAL_NZ:
      return cmod;
   case BRW_CONDITIONAL_G:
      return BRW_CONDITIONAL_L;
   case BRW_CONDITIONAL_GE:
      return BRW_CONDITIONAL_LE;
   case BRW_CONDITIONAL_L:
      return BRW_CONDITIONAL_G;
   case BRW_CONDITIONAL_LE:
      return BRW_CONDITIONAL_GE;
   default:
      return BRW_CONDITIONAL_NONE;
   }
}

/**
 * Get the least significant bit offset of the i+1-th component of immediate
 * type \p type.  For \p i equal to the two's complement of j, return the
 * offset of the j-th component starting from the end of the vector.  For
 * scalar register types return zero.
 */
static unsigned
imm_shift(enum brw_reg_type type, unsigned i)
{
   assert(type != BRW_REGISTER_TYPE_UV && type != BRW_REGISTER_TYPE_V &&
          "Not implemented.");

   if (type == BRW_REGISTER_TYPE_VF)
      return 8 * (i & 3);
   else
      return 0;
}

/**
 * Swizzle an arbitrary immediate \p x of the given type according to the
 * permutation specified as \p swz.
 */
uint32_t
brw_swizzle_immediate(enum brw_reg_type type, uint32_t x, unsigned swz)
{
   if (imm_shift(type, 1)) {
      const unsigned n = 32 / imm_shift(type, 1);
      uint32_t y = 0;

      for (unsigned i = 0; i < n; i++) {
         /* Shift the specified component all the way to the right and left to
          * discard any undesired L/MSBs, then shift it right into component i.
          */
         y |= x >> imm_shift(type, (i & ~3) + BRW_GET_SWZ(swz, i & 3))
                << imm_shift(type, ~0u)
                >> imm_shift(type, ~0u - i);
      }

      return y;
   } else {
      return x;
   }
}

void
brw_set_default_exec_size(struct brw_codegen *p, unsigned value)
{
   brw_inst_set_exec_size(p->devinfo, p->current, value);
}

void brw_set_default_predicate_control( struct brw_codegen *p, unsigned pc )
{
   brw_inst_set_pred_control(p->devinfo, p->current, pc);
}

void brw_set_default_predicate_inverse(struct brw_codegen *p, bool predicate_inverse)
{
   brw_inst_set_pred_inv(p->devinfo, p->current, predicate_inverse);
}

void brw_set_default_flag_reg(struct brw_codegen *p, int reg, int subreg)
{
   if (p->devinfo->gen >= 7)
      brw_inst_set_flag_reg_nr(p->devinfo, p->current, reg);

   brw_inst_set_flag_subreg_nr(p->devinfo, p->current, subreg);
}

void brw_set_default_access_mode( struct brw_codegen *p, unsigned access_mode )
{
   brw_inst_set_access_mode(p->devinfo, p->current, access_mode);
}

void
brw_set_default_compression_control(struct brw_codegen *p,
			    enum brw_compression compression_control)
{
   p->compressed = (compression_control == BRW_COMPRESSION_COMPRESSED);

   if (p->devinfo->gen >= 6) {
      /* Since we don't use the SIMD32 support in gen6, we translate
       * the pre-gen6 compression control here.
       */
      switch (compression_control) {
      case BRW_COMPRESSION_NONE:
	 /* This is the "use the first set of bits of dmask/vmask/arf
	  * according to execsize" option.
	  */
         brw_inst_set_qtr_control(p->devinfo, p->current, GEN6_COMPRESSION_1Q);
	 break;
      case BRW_COMPRESSION_2NDHALF:
	 /* For SIMD8, this is "use the second set of 8 bits." */
         brw_inst_set_qtr_control(p->devinfo, p->current, GEN6_COMPRESSION_2Q);
	 break;
      case BRW_COMPRESSION_COMPRESSED:
	 /* For SIMD16 instruction compression, use the first set of 16 bits
	  * since we don't do SIMD32 dispatch.
	  */
         brw_inst_set_qtr_control(p->devinfo, p->current, GEN6_COMPRESSION_1H);
	 break;
      default:
         unreachable("not reached");
      }
   } else {
      brw_inst_set_qtr_control(p->devinfo, p->current, compression_control);
   }
}

void brw_set_default_mask_control( struct brw_codegen *p, unsigned value )
{
   brw_inst_set_mask_control(p->devinfo, p->current, value);
}

void brw_set_default_saturate( struct brw_codegen *p, bool enable )
{
   brw_inst_set_saturate(p->devinfo, p->current, enable);
}

void brw_set_default_acc_write_control(struct brw_codegen *p, unsigned value)
{
   if (p->devinfo->gen >= 6)
      brw_inst_set_acc_wr_control(p->devinfo, p->current, value);
}

void brw_push_insn_state( struct brw_codegen *p )
{
   assert(p->current != &p->stack[BRW_EU_MAX_INSN_STACK-1]);
   memcpy(p->current + 1, p->current, sizeof(brw_inst));
   p->compressed_stack[p->current - p->stack] = p->compressed;
   p->current++;
}

void brw_pop_insn_state( struct brw_codegen *p )
{
   assert(p->current != p->stack);
   p->current--;
   p->compressed = p->compressed_stack[p->current - p->stack];
}


/***********************************************************************
 */
void
brw_init_codegen(const struct brw_device_info *devinfo,
                 struct brw_codegen *p, void *mem_ctx)
{
   memset(p, 0, sizeof(*p));

   p->devinfo = devinfo;
   /*
    * Set the initial instruction store array size to 1024, if found that
    * isn't enough, then it will double the store size at brw_next_insn()
    * until out of memory.
    */
   p->store_size = 1024;
   p->store = rzalloc_array(mem_ctx, brw_inst, p->store_size);
   p->nr_insn = 0;
   p->current = p->stack;
   p->compressed = false;
   memset(p->current, 0, sizeof(p->current[0]));

   p->mem_ctx = mem_ctx;

   /* Some defaults?
    */
   brw_set_default_exec_size(p, BRW_EXECUTE_8);
   brw_set_default_mask_control(p, BRW_MASK_ENABLE); /* what does this do? */
   brw_set_default_saturate(p, 0);
   brw_set_default_compression_control(p, BRW_COMPRESSION_NONE);

   /* Set up control flow stack */
   p->if_stack_depth = 0;
   p->if_stack_array_size = 16;
   p->if_stack = rzalloc_array(mem_ctx, int, p->if_stack_array_size);

   p->loop_stack_depth = 0;
   p->loop_stack_array_size = 16;
   p->loop_stack = rzalloc_array(mem_ctx, int, p->loop_stack_array_size);
   p->if_depth_in_loop = rzalloc_array(mem_ctx, int, p->loop_stack_array_size);

   brw_init_compaction_tables(devinfo);
}


const unsigned *brw_get_program( struct brw_codegen *p,
			       unsigned *sz )
{
   *sz = p->next_insn_offset;
   return (const unsigned *)p->store;
}

void
brw_disassemble(const struct brw_device_info *devinfo,
                void *assembly, int start, int end, FILE *out)
{
   bool dump_hex = (INTEL_DEBUG & DEBUG_HEX) != 0;

   for (int offset = start; offset < end;) {
      brw_inst *insn = assembly + offset;
      brw_inst uncompacted;
      bool compacted = brw_inst_cmpt_control(devinfo, insn);
      if (0)
         fprintf(out, "0x%08x: ", offset);

      if (compacted) {
         brw_compact_inst *compacted = (void *)insn;
	 if (dump_hex) {
	    fprintf(out, "0x%08x 0x%08x                       ",
		    ((uint32_t *)insn)[1],
		    ((uint32_t *)insn)[0]);
	 }

	 brw_uncompact_instruction(devinfo, &uncompacted, compacted);
	 insn = &uncompacted;
	 offset += 8;
      } else {
	 if (dump_hex) {
	    fprintf(out, "0x%08x 0x%08x 0x%08x 0x%08x ",
		    ((uint32_t *)insn)[3],
		    ((uint32_t *)insn)[2],
		    ((uint32_t *)insn)[1],
		    ((uint32_t *)insn)[0]);
	 }
	 offset += 16;
      }

      brw_disassemble_inst(out, devinfo, insn, compacted);
   }
}

enum gen {
   GEN4  = (1 << 0),
   GEN45 = (1 << 1),
   GEN5  = (1 << 2),
   GEN6  = (1 << 3),
   GEN7  = (1 << 4),
   GEN75 = (1 << 5),
   GEN8  = (1 << 6),
   GEN9  = (1 << 7),
   GEN_ALL = ~0
};

#define GEN_GE(gen) (~((gen) - 1) | gen)
#define GEN_LE(gen) (((gen) - 1) | gen)

const struct inst_info inst_info[128] = {
   [BRW_OPCODE_ILLEGAL] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_MOV] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_SEL] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_MOVI] = {
      .gens = GEN_GE(GEN45),
   },
   [BRW_OPCODE_NOT] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_AND] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_OR] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_XOR] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_SHR] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_SHL] = {
      .gens = GEN_ALL,
   },
   /* BRW_OPCODE_DIM / BRW_OPCODE_SMOV */
   /* Reserved - 11 */
   [BRW_OPCODE_ASR] = {
      .gens = GEN_ALL,
   },
   /* Reserved - 13-15 */
   [BRW_OPCODE_CMP] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_CMPN] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_CSEL] = {
      .gens = GEN_GE(GEN8),
   },
   [BRW_OPCODE_F32TO16] = {
      .gens = GEN7 | GEN75,
   },
   [BRW_OPCODE_F16TO32] = {
      .gens = GEN7 | GEN75,
   },
   /* Reserved - 21-22 */
   [BRW_OPCODE_BFREV] = {
      .gens = GEN_GE(GEN7),
   },
   [BRW_OPCODE_BFE] = {
      .gens = GEN_GE(GEN7),
   },
   [BRW_OPCODE_BFI1] = {
      .gens = GEN_GE(GEN7),
   },
   [BRW_OPCODE_BFI2] = {
      .gens = GEN_GE(GEN7),
   },
   /* Reserved - 27-31 */
   [BRW_OPCODE_JMPI] = {
      .gens = GEN_ALL,
   },
   /* BRW_OPCODE_BRD */
   [BRW_OPCODE_IF] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_IFF] = { /* also BRW_OPCODE_BRC */
      .gens = GEN_LE(GEN5),
   },
   [BRW_OPCODE_ELSE] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_ENDIF] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_DO] = { /* also BRW_OPCODE_CASE */
      .gens = GEN_LE(GEN5),
   },
   [BRW_OPCODE_WHILE] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_BREAK] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_CONTINUE] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_HALT] = {
      .gens = GEN_ALL,
   },
   /* BRW_OPCODE_CALLA */
   /* BRW_OPCODE_MSAVE / BRW_OPCODE_CALL */
   /* BRW_OPCODE_MREST / BRW_OPCODE_RET */
   /* BRW_OPCODE_PUSH / BRW_OPCODE_FORK / BRW_OPCODE_GOTO */
   /* BRW_OPCODE_POP */
   [BRW_OPCODE_WAIT] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_SEND] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_SENDC] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_SENDS] = {
      .gens = GEN_GE(GEN9),
   },
   [BRW_OPCODE_SENDSC] = {
      .gens = GEN_GE(GEN9),
   },
   /* Reserved 53-55 */
   [BRW_OPCODE_MATH] = {
      .gens = GEN_GE(GEN6),
   },
   /* Reserved 57-63 */
   [BRW_OPCODE_ADD] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_MUL] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_AVG] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_FRC] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_RNDU] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_RNDD] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_RNDE] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_RNDZ] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_MAC] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_MACH] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_LZD] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_FBH] = {
      .gens = GEN_GE(GEN7),
   },
   [BRW_OPCODE_FBL] = {
      .gens = GEN_GE(GEN7),
   },
   [BRW_OPCODE_CBIT] = {
      .gens = GEN_GE(GEN7),
   },
   [BRW_OPCODE_ADDC] = {
      .gens = GEN_GE(GEN7),
   },
   [BRW_OPCODE_SUBB] = {
      .gens = GEN_GE(GEN7),
   },
   [BRW_OPCODE_SAD2] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_SADA2] = {
      .gens = GEN_ALL,
   },
   /* Reserved 82-83 */
   [BRW_OPCODE_DP4] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_DPH] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_DP3] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_DP2] = {
      .gens = GEN_ALL,
   },
   /* Reserved 88 */
   [BRW_OPCODE_LINE] = {
      .gens = GEN_ALL,
   },
   [BRW_OPCODE_PLN] = {
      .gens = GEN_GE(GEN45),
   },
   [BRW_OPCODE_MAD] = {
      .gens = GEN_GE(GEN6),
   },
   [BRW_OPCODE_LRP] = {
      .gens = GEN_GE(GEN6),
   },
   /* Reserved 93-124 */
   /* BRW_OPCODE_NENOP */
   [BRW_OPCODE_NOP] = {
      .gens = GEN_ALL,
   },
};

int
gen_from_devinfo(const struct brw_device_info *devinfo)
{
   switch (devinfo->gen) {
   case 4: return devinfo->is_g4x ? GEN45 : GEN4;
   case 5: return GEN5;
   case 6: return GEN6;
   case 7: return devinfo->is_haswell ? GEN75 : GEN7;
   case 8: return GEN8;
   case 9: return GEN9;
   default:
      unreachable("not reached");
   }
}

/* Return the matching opcode_desc for the specified opcode number and
 * hardware generation, or NULL if the opcode is not supported by the device.
 * XXX -- Actually check whether the opcode is supported.
 */
const struct opcode_desc *
brw_opcode_desc(const struct brw_device_info *devinfo, enum opcode opcode)
{
   if (opcode >= ARRAY_SIZE(opcode_descs))
      return NULL;

   if (opcode_descs[opcode].name)
      return &opcode_descs[opcode];
   else
      return NULL;
}
