/*
 * Copyright 2012 Vadim Girlin <vadimgirlin@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *      Vadim Girlin
 */

#include "r600_pipe.h"
#include "r600_isa.h"

static const struct fetch_op_info fetch_op_table[] = {
		{"VFETCH",                        { 0x000000,  0x000000,  0x000000,  0x000000 }, FF_VTX },
		{"SEMFETCH",                      { 0x000001,  0x000001,  0x000001,  0x000001 }, FF_VTX },

		{"READ_SCRATCH",                  {       -1,  0x000002,  0x000002,  0x000002 }, FF_VTX | FF_MEM },
		{"READ_REDUCT",                   {       -1,  0x000102,        -1,        -1 }, FF_VTX | FF_MEM },
		{"READ_MEM",                      {       -1,  0x000202,  0x000202,  0x000202 }, FF_VTX | FF_MEM },
		{"DS_LOCAL_WRITE",                {       -1,  0x000402,        -1,        -1 }, FF_VTX | FF_MEM },
		{"DS_LOCAL_READ",                 {       -1,  0x000502,        -1,        -1 }, FF_VTX | FF_MEM },

		{"GDS_ADD",                       {       -1,        -1,  0x020002,  0x020002 }, FF_GDS },
		{"GDS_SUB",                       {       -1,        -1,  0x020102,  0x020102 }, FF_GDS },
		{"GDS_RSUB",                      {       -1,        -1,  0x020202,  0x020202 }, FF_GDS },
		{"GDS_INC",                       {       -1,        -1,  0x020302,  0x020302 }, FF_GDS },
		{"GDS_DEC",                       {       -1,        -1,  0x020402,  0x020402 }, FF_GDS },
		{"GDS_MIN_INT",                   {       -1,        -1,  0x020502,  0x020502 }, FF_GDS },
		{"GDS_MAX_INT",                   {       -1,        -1,  0x020602,  0x020602 }, FF_GDS },
		{"GDS_MIN_UINT",                  {       -1,        -1,  0x020702,  0x020702 }, FF_GDS },
		{"GDS_MAX_UINT",                  {       -1,        -1,  0x020802,  0x020802 }, FF_GDS },
		{"GDS_AND",                       {       -1,        -1,  0x020902,  0x020902 }, FF_GDS },
		{"GDS_OR",                        {       -1,        -1,  0x020A02,  0x020A02 }, FF_GDS },
		{"GDS_XOR",                       {       -1,        -1,  0x020B02,  0x020B02 }, FF_GDS },
		{"GDS_MSKOR",                     {       -1,        -1,  0x030C02,  0x030C02 }, FF_GDS },
		{"GDS_WRITE",                     {       -1,        -1,  0x020D02,  0x020D02 }, FF_GDS },
		{"GDS_WRITE_REL",                 {       -1,        -1,  0x030E02,  0x030E02 }, FF_GDS },
		{"GDS_WRITE2",                    {       -1,        -1,  0x030F02,  0x030F02 }, FF_GDS },
		{"GDS_CMP_STORE",                 {       -1,        -1,  0x031002,  0x031002 }, FF_GDS },
		{"GDS_CMP_STORE_SPF",             {       -1,        -1,  0x031102,  0x031102 }, FF_GDS },
		{"GDS_BYTE_WRITE",                {       -1,        -1,  0x021202,  0x021202 }, FF_GDS },
		{"GDS_SHORT_WRITE",               {       -1,        -1,  0x021302,  0x021302 }, FF_GDS },
		{"GDS_ADD_RET",                   {       -1,        -1,  0x122002,  0x122002 }, FF_GDS },
		{"GDS_SUB_RET",                   {       -1,        -1,  0x122102,  0x122102 }, FF_GDS },
		{"GDS_RSUB_RET",                  {       -1,        -1,  0x122202,  0x122202 }, FF_GDS },
		{"GDS_INC_RET",                   {       -1,        -1,  0x122302,  0x122302 }, FF_GDS },
		{"GDS_DEC_RET",                   {       -1,        -1,  0x122402,  0x122402 }, FF_GDS },
		{"GDS_MIN_INT_RET",               {       -1,        -1,  0x122502,  0x122502 }, FF_GDS },
		{"GDS_MAX_INT_RET",               {       -1,        -1,  0x122602,  0x122602 }, FF_GDS },
		{"GDS_MIN_UINT_RET",              {       -1,        -1,  0x122702,  0x122702 }, FF_GDS },
		{"GDS_MAX_UINT_RET",              {       -1,        -1,  0x122802,  0x122802 }, FF_GDS },
		{"GDS_AND_RET",                   {       -1,        -1,  0x122902,  0x122902 }, FF_GDS },
		{"GDS_OR_RET",                    {       -1,        -1,  0x122A02,  0x122A02 }, FF_GDS },
		{"GDS_XOR_RET",                   {       -1,        -1,  0x122B02,  0x122B02 }, FF_GDS },
		{"GDS_MSKOR_RET",                 {       -1,        -1,  0x132C02,  0x132C02 }, FF_GDS },
		{"GDS_XCHG_RET",                  {       -1,        -1,  0x122D02,  0x122D02 }, FF_GDS },
		{"GDS_XCHG_REL_RET",              {       -1,        -1,  0x232E02,  0x232E02 }, FF_GDS },
		{"GDS_XCHG2_RET",                 {       -1,        -1,  0x232F02,  0x232F02 }, FF_GDS },
		{"GDS_CMP_XCHG_RET",              {       -1,        -1,  0x133002,  0x133002 }, FF_GDS },
		{"GDS_CMP_XCHG_SPF_RET",          {       -1,        -1,  0x133102,  0x133102 }, FF_GDS },
		{"GDS_READ_RET",                  {       -1,        -1,  0x113202,  0x113202 }, FF_GDS },
		{"GDS_READ_REL_RET",              {       -1,        -1,  0x213302,  0x213302 }, FF_GDS },
		{"GDS_READ2_RET",                 {       -1,        -1,  0x223402,  0x223402 }, FF_GDS },
		{"GDS_READWRITE_RET",             {       -1,        -1,  0x133502,  0x133502 }, FF_GDS },
		{"GDS_BYTE_READ_RET",             {       -1,        -1,  0x113602,  0x113602 }, FF_GDS },
		{"GDS_UBYTE_READ_RET",            {       -1,        -1,  0x113702,  0x113702 }, FF_GDS },
		{"GDS_SHORT_READ_RET",            {       -1,        -1,  0x113802,  0x113802 }, FF_GDS },
		{"GDS_USHORT_READ_RET",           {       -1,        -1,  0x113902,  0x113902 }, FF_GDS },
		{"GDS_ATOMIC_ORDERED_ALLOC",      {       -1,        -1,  0x113F02,  0x113F02 }, FF_GDS },

		{"TF_WRITE",                      {       -1,        -1,  0x020502,  0x020502 }, FF_GDS },

		{"DS_GLOBAL_WRITE",               {       -1,  0x000602,        -1,        -1 }, 0 },
		{"DS_GLOBAL_READ",                {       -1,  0x000702,        -1,        -1 }, 0 },

		{"LD",                            { 0x000003,  0x000003,  0x000003,  0x000003 }, 0 },
		{"LDFPTR",                        {       -1,        -1,  0x000103,  0x000103 }, 0 },
		{"GET_TEXTURE_RESINFO",           { 0x000004,  0x000004,  0x000004,  0x000004 }, 0 },
		{"GET_NUMBER_OF_SAMPLES",         { 0x000005,  0x000005,  0x000005,  0x000005 }, 0 },
		{"GET_LOD",                       { 0x000006,  0x000006,  0x000006,  0x000006 }, 0 },
		{"GET_GRADIENTS_H",               { 0x000007,  0x000007,  0x000007,  0x000007 }, FF_GETGRAD },
		{"GET_GRADIENTS_V",               { 0x000008,  0x000008,  0x000008,  0x000008 }, FF_GETGRAD },
		{"GET_GRADIENTS_H_FINE",          {       -1,        -1,  0x000107,  0x000107 }, FF_GETGRAD },
		{"GET_GRADIENTS_V_FINE",          {       -1,        -1,  0x000108,  0x000108 }, FF_GETGRAD },
		{"GET_LERP",                      { 0x000009,  0x000009,        -1,        -1 }, 0 },
		{"SET_TEXTURE_OFFSETS",           {       -1,        -1,  0x000009,  0x000009 }, FF_SET_TEXTURE_OFFSETS },
		{"KEEP_GRADIENTS",                {       -1,  0x00000A,  0x00000A,  0x00000A }, 0 },
		{"SET_GRADIENTS_H",               { 0x00000B,  0x00000B,  0x00000B,  0x00000B }, FF_SETGRAD },
		{"SET_GRADIENTS_V",               { 0x00000C,  0x00000C,  0x00000C,  0x00000C }, FF_SETGRAD },
		{"SET_GRADIENTS_H_COARSE",        {       -1,        -1,        -1,  0x00010B }, FF_SETGRAD },
		{"SET_GRADIENTS_V_COARSE",        {       -1,        -1,        -1,  0x00010C }, FF_SETGRAD },
		{"SET_GRADIENTS_H_PACKED_FINE",   {       -1,        -1,        -1,  0x00020B }, FF_SETGRAD },
		{"SET_GRADIENTS_V_PACKED_FINE",   {       -1,        -1,        -1,  0x00020C }, FF_SETGRAD },
		{"SET_GRADIENTS_H_PACKED_COARSE", {       -1,        -1,        -1,  0x00030B }, FF_SETGRAD },
		{"SET_GRADIENTS_V_PACKED_COARSE", {       -1,        -1,        -1,  0x00030C }, FF_SETGRAD },
		{"PASS",                          { 0x00000D,  0x00000D,  0x00000D,  0x00000D }, 0 }, /* ???? 700, eg, cm docs - marked as reserved */
		{"PASS1",                         {       -1,        -1,  0x00010D,  0x00010D }, 0 },
		{"PASS2",                         {       -1,        -1,  0x00020D,  0x00020D }, 0 },
		{"PASS3",                         {       -1,        -1,  0x00030D,  0x00030D }, 0 },
		{"SET_CUBEMAP_INDEX",             { 0x00000E,  0x00000E,        -1,        -1 }, 0 },
		{"GET_BUFFER_RESINFO",            {       -1,        -1,  0x00000E,  0x00000E }, FF_VTX },
		{"FETCH4",                        { 0x00000F,  0x00000F,        -1,        -1 }, 0 },

		{"SAMPLE",                        { 0x000010,  0x000010,  0x000010,  0x000010 }, FF_TEX },
		{"SAMPLE_L",                      { 0x000011,  0x000011,  0x000011,  0x000011 }, FF_TEX },
		{"SAMPLE_LB",                     { 0x000012,  0x000012,  0x000012,  0x000012 }, FF_TEX },
		{"SAMPLE_LZ",                     { 0x000013,  0x000013,  0x000013,  0x000013 }, FF_TEX },
		{"SAMPLE_G",                      { 0x000014,  0x000014,  0x000014,  0x000014 }, FF_TEX | FF_USEGRAD },
		{"SAMPLE_G_L",                    { 0x000015,  0x000015,        -1,        -1 }, FF_TEX | FF_USEGRAD},
		{"GATHER4",                       {       -1,        -1,  0x000015,  0x000015 }, FF_TEX },
		{"SAMPLE_G_LB",                   { 0x000016,  0x000016,  0x000016,  0x000016 }, FF_TEX | FF_USEGRAD},
		{"SAMPLE_G_LZ",                   { 0x000017,  0x000017,        -1,        -1 }, FF_TEX | FF_USEGRAD},
		{"GATHER4_O",                     {       -1,        -1,  0x000017,  0x000017 }, FF_TEX | FF_USE_TEXTURE_OFFSETS},
		{"SAMPLE_C",                      { 0x000018,  0x000018,  0x000018,  0x000018 }, FF_TEX },
		{"SAMPLE_C_L",                    { 0x000019,  0x000019,  0x000019,  0x000019 }, FF_TEX },
		{"SAMPLE_C_LB",                   { 0x00001A,  0x00001A,  0x00001A,  0x00001A }, FF_TEX },
		{"SAMPLE_C_LZ",                   { 0x00001B,  0x00001B,  0x00001B,  0x00001B }, FF_TEX },
		{"SAMPLE_C_G",                    { 0x00001C,  0x00001C,  0x00001C,  0x00001C }, FF_TEX | FF_USEGRAD},
		{"SAMPLE_C_G_L",                  { 0x00001D,  0x00001D,        -1,        -1 }, FF_TEX | FF_USEGRAD},
		{"GATHER4_C",                     {       -1,        -1,  0x00001D,  0x00001D }, FF_TEX },
		{"SAMPLE_C_G_LB",                 { 0x00001E,  0x00001E,  0x00001E,  0x00001E }, FF_TEX | FF_USEGRAD},
		{"SAMPLE_C_G_LZ",                 { 0x00001F,  0x00001F,        -1,        -1 }, FF_TEX | FF_USEGRAD},
		{"GATHER4_C_O",                   {       -1,        -1,  0x00001F,  0x00001F }, FF_TEX | FF_USE_TEXTURE_OFFSETS}
};

const struct fetch_op_info *
r600_isa_fetch(unsigned op) {
	assert (op < ARRAY_SIZE(fetch_op_table));
	return &fetch_op_table[op];
}

int r600_isa_init(struct r600_context *ctx, struct r600_isa *isa) {
	unsigned i;

	assert(ctx->b.chip_class >= R600 && ctx->b.chip_class <= CAYMAN);
	isa->hw_class = ctx->b.chip_class - R600;

	/* reverse lookup maps are required for bytecode parsing */

	isa->alu_op2_map = calloc(256, sizeof(unsigned));
	if (!isa->alu_op2_map)
		return -1;
	isa->alu_op3_map = calloc(256, sizeof(unsigned));
	if (!isa->alu_op3_map)
		return -1;
	isa->fetch_map = calloc(256, sizeof(unsigned));
	if (!isa->fetch_map)
		return -1;
	isa->cf_map = calloc(256, sizeof(unsigned));
	if (!isa->cf_map)
		return -1;

	for (i = 0; i < TABLE_SIZE(alu_op_table); ++i) {
		const struct alu_op_info *op = &alu_op_table[i];
		unsigned opc;
		if (op->flags & AF_LDS || op->slots[isa->hw_class] == 0)
			continue;
		opc = op->opcode[isa->hw_class >> 1];
		assert(opc != -1);
		if (op->src_count == 3)
			isa->alu_op3_map[opc] = i + 1;
		else
			isa->alu_op2_map[opc] = i + 1;
	}

	for (i = 0; i < TABLE_SIZE(fetch_op_table); ++i) {
		const struct fetch_op_info *op = &fetch_op_table[i];
		unsigned opc = op->opcode[isa->hw_class];
		if ((op->flags & FF_GDS) || ((opc & 0xFF) != opc))
			continue; /* ignore GDS ops and INST_MOD versions for now */
		isa->fetch_map[opc] = i + 1;
	}

	for (i = 0; i < TABLE_SIZE(cf_op_table); ++i) {
		const struct cf_op_info *op = &cf_op_table[i];
		unsigned opc = op->opcode[isa->hw_class];
		if (opc == -1)
			continue;
		/* using offset for CF_ALU_xxx opcodes because they overlap with other
		 * CF opcodes (they use different encoding in hw) */
		if (op->flags & CF_ALU)
			opc += 0x80;
		isa->cf_map[opc] = i + 1;
	}

	return 0;
}

int r600_isa_destroy(struct r600_isa *isa) {

	if (!isa)
		return 0;

	free(isa->alu_op2_map);
	free(isa->alu_op3_map);
	free(isa->fetch_map);
	free(isa->cf_map);

	free(isa);
	return 0;
}



