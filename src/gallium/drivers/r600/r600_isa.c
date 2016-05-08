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

static const struct cf_op_info cf_op_table[] = {
		{"NOP",                           { 0x00, 0x00, 0x00, 0x00 },  0  },

		{"TEX",                           { 0x01, 0x01, 0x01, 0x01 },  CF_CLAUSE | CF_FETCH | CF_UNCOND }, /* merged with "TC" entry */
		{"VTX",                           { 0x02, 0x02, 0x02,   -1 },  CF_CLAUSE | CF_FETCH | CF_UNCOND }, /* merged with "VC" entry */
		{"VTX_TC",                        { 0x03, 0x03,   -1,   -1 },  CF_CLAUSE | CF_FETCH | CF_UNCOND },
		{"GDS",                           {   -1,   -1, 0x03, 0x03 },  CF_CLAUSE | CF_FETCH | CF_UNCOND },

		{"LOOP_START",                    { 0x04, 0x04, 0x04, 0x04 },  CF_LOOP | CF_LOOP_START },
		{"LOOP_END",                      { 0x05, 0x05, 0x05, 0x05 },  CF_LOOP  },
		{"LOOP_START_DX10",               { 0x06, 0x06, 0x06, 0x06 },  CF_LOOP | CF_LOOP_START },
		{"LOOP_START_NO_AL",              { 0x07, 0x07, 0x07, 0x07 },  CF_LOOP | CF_LOOP_START },
		{"LOOP_CONTINUE",                 { 0x08, 0x08, 0x08, 0x08 },  CF_LOOP  },
		{"LOOP_BREAK",                    { 0x09, 0x09, 0x09, 0x09 },  CF_LOOP  },
		{"JUMP",                          { 0x0A, 0x0A, 0x0A, 0x0A },  CF_BRANCH  },
		{"PUSH",                          { 0x0B, 0x0B, 0x0B, 0x0B },  CF_BRANCH  },
		{"PUSH_ELSE",                     { 0x0C, 0x0C,   -1,   -1 },  CF_BRANCH  },
		{"ELSE",                          { 0x0D, 0x0D, 0x0D, 0x0D },  CF_BRANCH  },
		{"POP",                           { 0x0E, 0x0E, 0x0E, 0x0E },  CF_BRANCH  },
		{"POP_JUMP",                      { 0x0F, 0x0F,   -1,   -1 },  CF_BRANCH  },
		{"POP_PUSH",                      { 0x10, 0x10,   -1,   -1 },  CF_BRANCH  },
		{"POP_PUSH_ELSE",                 { 0x11, 0x11,   -1,   -1 },  CF_BRANCH  },
		{"CALL",                          { 0x12, 0x12, 0x12, 0x12 },  CF_CALL  },
		{"CALL_FS",                       { 0x13, 0x13, 0x13, 0x13 },  CF_CALL  },
		{"RET",                           { 0x14, 0x14, 0x14, 0x14 },  0 },
		{"EMIT_VERTEX",                   { 0x15, 0x15, 0x15, 0x15 },  CF_EMIT | CF_UNCOND },
		{"EMIT_CUT_VERTEX",               { 0x16, 0x16, 0x16, 0x16 },  CF_EMIT | CF_UNCOND  },
		{"CUT_VERTEX",                    { 0x17, 0x17, 0x17, 0x17 },  CF_EMIT | CF_UNCOND  },
		{"KILL",                          { 0x18, 0x18, 0x18, 0x18 },  CF_UNCOND  },
		{"END_PROGRAM",                   { 0x19, 0x19, 0x19, 0x19 },  0  },  /* ??? "reserved" in isa docs */
		{"WAIT_ACK",                      {   -1, 0x1A, 0x1A, 0x1A },  0  },
		{"TEX_ACK",                       {   -1, 0x1B, 0x1B, 0x1B },  CF_CLAUSE | CF_FETCH | CF_ACK | CF_UNCOND },
		{"VTX_ACK",                       {   -1, 0x1C, 0x1C,   -1 },  CF_CLAUSE | CF_FETCH | CF_ACK | CF_UNCOND },
		{"VTX_TC_ACK",                    {   -1, 0x1D,   -1,   -1 },  CF_CLAUSE | CF_FETCH | CF_ACK | CF_UNCOND },
		{"JUMPTABLE",                     {   -1,   -1, 0x1D, 0x1D },  CF_BRANCH  },
		{"WAVE_SYNC",                     {   -1,   -1, 0x1E, 0x1E },  0  },
		{"HALT",                          {   -1,   -1, 0x1F, 0x1F },  0  },
		{"CF_END",                        {   -1,   -1,   -1, 0x20 },  0  },
		{"LDS_DEALLOC",                   {   -1,   -1,   -1, 0x21 },  0  },
		{"PUSH_WQM",                      {   -1,   -1,   -1, 0x22 },  CF_BRANCH  },
		{"POP_WQM",                       {   -1,   -1,   -1, 0x23 },  CF_BRANCH  },
		{"ELSE_WQM",                      {   -1,   -1,   -1, 0x24 },  CF_BRANCH  },
		{"JUMP_ANY",                      {   -1,   -1,   -1, 0x25 },  CF_BRANCH  },

		/* ??? next 5 added from CAYMAN ISA doc, not in the original table */
		{"REACTIVATE",                    {   -1,   -1,   -1, 0x26 },  0  },
		{"REACTIVATE_WQM",                {   -1,   -1,   -1, 0x27 },  0  },
		{"INTERRUPT",                     {   -1,   -1,   -1, 0x28 },  0  },
		{"INTERRUPT_AND_SLEEP",           {   -1,   -1,   -1, 0x29 },  0  },
		{"SET_PRIORITY",                  {   -1,   -1,   -1, 0x2A },  0  },

		{"MEM_STREAM0_BUF0",              {   -1,   -1, 0x40, 0x40 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM0_BUF1",              {   -1,   -1, 0x41, 0x41 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM0_BUF2",              {   -1,   -1, 0x42, 0x42 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM0_BUF3",              {   -1,   -1, 0x43, 0x43 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM1_BUF0",              {   -1,   -1, 0x44, 0x44 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM1_BUF1",              {   -1,   -1, 0x45, 0x45 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM1_BUF2",              {   -1,   -1, 0x46, 0x46 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM1_BUF3",              {   -1,   -1, 0x47, 0x47 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM2_BUF0",              {   -1,   -1, 0x48, 0x48 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM2_BUF1",              {   -1,   -1, 0x49, 0x49 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM2_BUF2",              {   -1,   -1, 0x4A, 0x4A },  CF_MEM | CF_STRM  },
		{"MEM_STREAM2_BUF3",              {   -1,   -1, 0x4B, 0x4B },  CF_MEM | CF_STRM  },
		{"MEM_STREAM3_BUF0",              {   -1,   -1, 0x4C, 0x4C },  CF_MEM | CF_STRM  },
		{"MEM_STREAM3_BUF1",              {   -1,   -1, 0x4D, 0x4D },  CF_MEM | CF_STRM  },
		{"MEM_STREAM3_BUF2",              {   -1,   -1, 0x4E, 0x4E },  CF_MEM | CF_STRM  },
		{"MEM_STREAM3_BUF3",              {   -1,   -1, 0x4F, 0x4F },  CF_MEM | CF_STRM  },

		{"MEM_STREAM0",                   { 0x20, 0x20,   -1,   -1 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM1",                   { 0x21, 0x21,   -1,   -1 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM2",                   { 0x22, 0x22,   -1,   -1 },  CF_MEM | CF_STRM  },
		{"MEM_STREAM3",                   { 0x23, 0x23,   -1,   -1 },  CF_MEM | CF_STRM  },

		{"MEM_SCRATCH",                   { 0x24, 0x24, 0x50, 0x50 },  CF_MEM  },
		{"MEM_REDUCT",                    { 0x25, 0x25,   -1,   -1 },  CF_MEM  },
		{"MEM_RING",                      { 0x26, 0x26, 0x52, 0x52 },  CF_MEM | CF_EMIT },

		{"EXPORT",                        { 0x27, 0x27, 0x53, 0x53 },  CF_EXP  },
		{"EXPORT_DONE",                   { 0x28, 0x28, 0x54, 0x54 },  CF_EXP  },

		{"MEM_EXPORT",                    {   -1, 0x3A, 0x55, 0x55 },  CF_MEM  },
		{"MEM_RAT",                       {   -1,   -1, 0x56, 0x56 },  CF_MEM | CF_RAT },
		{"MEM_RAT_NOCACHE",               {   -1,   -1, 0x57, 0x57 },  CF_MEM | CF_RAT },
		{"MEM_RING1",                     {   -1,   -1, 0x58, 0x58 },  CF_MEM | CF_EMIT },
		{"MEM_RING2",                     {   -1,   -1, 0x59, 0x59 },  CF_MEM | CF_EMIT },
		{"MEM_RING3",                     {   -1,   -1, 0x5A, 0x5A },  CF_MEM | CF_EMIT },
		{"MEM_MEM_COMBINED",              {   -1,   -1, 0x5B, 0x5B },  CF_MEM  },
		{"MEM_RAT_COMBINED_NOCACHE",      {   -1,   -1, 0x5C, 0x5C },  CF_MEM | CF_RAT },
		{"MEM_RAT_COMBINED",              {   -1,   -1,   -1, 0x5D },  CF_MEM | CF_RAT }, /* ??? not in cayman isa doc */

		{"EXPORT_DONE_END",               {   -1,   -1,   -1, 0x5E },  CF_EXP  },   /* ??? not in cayman isa doc */

		{"ALU",                           { 0x08, 0x08, 0x08, 0x08 },  CF_CLAUSE | CF_ALU  },
		{"ALU_PUSH_BEFORE",               { 0x09, 0x09, 0x09, 0x09 },  CF_CLAUSE | CF_ALU  },
		{"ALU_POP_AFTER",                 { 0x0A, 0x0A, 0x0A, 0x0A },  CF_CLAUSE | CF_ALU  },
		{"ALU_POP2_AFTER",                { 0x0B, 0x0B, 0x0B, 0x0B },  CF_CLAUSE | CF_ALU  },
		{"ALU_EXT",                       {   -1,   -1, 0x0C, 0x0C },  CF_CLAUSE | CF_ALU | CF_ALU_EXT  },
		{"ALU_CONTINUE",                  { 0x0D, 0x0D, 0x0D,   -1 },  CF_CLAUSE | CF_ALU  },
		{"ALU_BREAK",                     { 0x0E, 0x0E, 0x0E,   -1 },  CF_CLAUSE | CF_ALU  },
		{"ALU_ELSE_AFTER",                { 0x0F, 0x0F, 0x0F, 0x0F },  CF_CLAUSE | CF_ALU  },
		{"CF_NATIVE",                     { 0x00, 0x00, 0x00, 0x00 },  0  }
};

const struct fetch_op_info *
r600_isa_fetch(unsigned op) {
	assert (op < ARRAY_SIZE(fetch_op_table));
	return &fetch_op_table[op];
}

const struct cf_op_info *
r600_isa_cf(unsigned op) {
	assert (op < ARRAY_SIZE(cf_op_table));
	return &cf_op_table[op];
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



