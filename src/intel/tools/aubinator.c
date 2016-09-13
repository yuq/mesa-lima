/*
 * Copyright © 2016 Intel Corporation
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include "decoder.h"
#include "intel_aub.h"
#include "gen_disasm.h"

/* Below is the only command missing from intel_aub.h in libdrm
 * So, reuse intel_aub.h from libdrm and #define the
 * AUB_MI_BATCH_BUFFER_END as below
 */
#define AUB_MI_BATCH_BUFFER_END (0x0500 << 16)

#define CSI "\e["
#define HEADER CSI "37;44m"
#define NORMAL CSI "0m"
#define CLEAR_TO_EOL CSI "0K"

/* options */

static bool option_full_decode = true;
static bool option_print_offsets = true;
static enum { COLOR_AUTO, COLOR_ALWAYS, COLOR_NEVER } option_color;

/* state */

struct gen_disasm *disasm;

uint64_t gtt_size, gtt_end;
void *gtt;
uint64_t general_state_base;
uint64_t surface_state_base;
uint64_t dynamic_state_base;
uint64_t instruction_base;
uint64_t instruction_bound;

static inline uint32_t
field(uint32_t value, int start, int end)
{
   uint32_t mask;

   mask = ~0U >> (31 - end + start);

   return (value >> start) & mask;
}

struct brw_instruction;

static inline int
valid_offset(uint32_t offset)
{
   return offset < gtt_end;
}

static void
print_dword_val(struct gen_field_iterator *iter, uint64_t offset,
                int *dword_num)
{
   struct gen_field *f;

   f = iter->group->fields[iter->i - 1];
   const int dword = f->start / 32;

   if (*dword_num != dword) {
      printf("0x%08lx:  0x%08x : Dword %d\n",
             offset + 4 * dword,  iter->p[dword], dword);
      *dword_num = dword;
   }
}

static char *
print_iterator_values(struct gen_field_iterator *iter, int *idx)
{
    char *token = NULL;
    if (strstr(iter->value, "struct") == NULL) {
        printf("    %s: %s\n", iter->name, iter->value);
    } else {
        token = strtok(iter->value, " ");
        if (token != NULL) {
            token = strtok(NULL, " ");
            *idx = atoi(strtok(NULL, ">"));
        } else {
            token = NULL;
        }
        printf("    %s:<struct %s>\n", iter->name, token);
    }
    return token;
}

static void
decode_structure(struct gen_spec *spec, struct gen_group *strct,
                 const uint32_t *p)
{
   struct gen_field_iterator iter;
   char *token = NULL;
   int idx = 0, dword_num = 0;
   uint64_t offset = 0;

   if (option_print_offsets)
      offset = (void *) p - gtt;
   else
      offset = 0;

   gen_field_iterator_init(&iter, strct, p);
   while (gen_field_iterator_next(&iter)) {
      idx = 0;
      print_dword_val(&iter, offset, &dword_num);
      token = print_iterator_values(&iter, &idx);
      if (token != NULL) {
         struct gen_group *struct_val = gen_spec_find_struct(spec, token);
         decode_structure(spec, struct_val, &p[idx]);
         token = NULL;
      }
   }
}

static void
handle_struct_decode(struct gen_spec *spec, char *struct_name, uint32_t *p)
{
   if (struct_name == NULL)
      return;
   struct gen_group *struct_val = gen_spec_find_struct(spec, struct_name);
   decode_structure(spec, struct_val, p);
}

static void
dump_binding_table(struct gen_spec *spec, uint32_t offset)
{
   uint32_t *pointers, i;
   uint64_t start;
   struct gen_group *surface_state;

   surface_state = gen_spec_find_struct(spec, "RENDER_SURFACE_STATE");
   if (surface_state == NULL) {
      printf("did not find RENDER_SURFACE_STATE info\n");
      return;
   }

   start = surface_state_base + offset;
   pointers = gtt + start;
   for (i = 0; i < 16; i++) {
      if (pointers[i] == 0)
         continue;
      start = pointers[i] + surface_state_base;
      if (!valid_offset(start)) {
         printf("pointer %u: %08x <not valid>\n",
                i, pointers[i]);
         continue;
      } else {
         printf("pointer %u: %08x\n", i, pointers[i]);
      }

      decode_structure(spec, surface_state, gtt + start);
   }
}

static void
handle_3dstate_index_buffer(struct gen_spec *spec, uint32_t *p)
{
   void *start;
   uint32_t length, i, type, size;

   start = gtt + p[2];
   type = (p[1] >> 8) & 3;
   size = 1 << type;
   length = p[4] / size;
   if (length > 10)
      length = 10;

   printf("\t");

   for (i = 0; i < length; i++) {
      switch (type) {
      case 0:
         printf("%3d ", ((uint8_t *)start)[i]);
         break;
      case 1:
         printf("%3d ", ((uint16_t *)start)[i]);
         break;
      case 2:
         printf("%3d ", ((uint32_t *)start)[i]);
         break;
      }
   }
   if (length < p[4] / size)
      printf("...\n");
   else
      printf("\n");
}

static inline uint64_t
get_qword(uint32_t *p)
{
   return ((uint64_t) p[1] << 32) | p[0];
}

static void
handle_state_base_address(struct gen_spec *spec, uint32_t *p)
{
   uint64_t mask = ~((1 << 12) - 1);

   if (gen_spec_get_gen(spec) >= gen_make_gen(8,0)) {
      if (p[1] & 1)
         general_state_base = get_qword(&p[1]) & mask;
      if (p[4] & 1)
         surface_state_base = get_qword(&p[4]) & mask;
      if (p[6] & 1)
         dynamic_state_base = get_qword(&p[6]) & mask;
      if (p[10] & 1)
         instruction_base = get_qword(&p[10]) & mask;
      if (p[15] & 1)
         instruction_bound = p[15] & mask;
   } else {
      if (p[2] & 1)
         surface_state_base = p[2] & mask;
      if (p[3] & 1)
         dynamic_state_base = p[3] & mask;
      if (p[5] & 1)
         instruction_base = p[5] & mask;
      if (p[9] & 1)
         instruction_bound = p[9] & mask;
   }
}

static void
dump_samplers(struct gen_spec *spec, uint32_t offset)
{
   uint32_t i;
   uint64_t start;
   struct gen_group *sampler_state;

   sampler_state = gen_spec_find_struct(spec, "SAMPLER_STATE");

   start = dynamic_state_base + offset;
   for (i = 0; i < 4; i++) {
      printf("sampler state %d\n", i);
      decode_structure(spec, sampler_state, gtt + start + i * 16);
   }
}

static void
handle_media_interface_descriptor_load(struct gen_spec *spec, uint32_t *p)
{
   int i, length = p[2] / 32;
   struct gen_group *descriptor_structure;
   uint32_t *descriptors;
   uint64_t start;
   struct brw_instruction *insns;

   descriptor_structure =
      gen_spec_find_struct(spec, "INTERFACE_DESCRIPTOR_DATA");
   if (descriptor_structure == NULL) {
      printf("did not find INTERFACE_DESCRIPTOR_DATA info\n");
      return;
   }

   start = dynamic_state_base + p[3];
   descriptors = gtt + start;
   for (i = 0; i < length; i++, descriptors += 8) {
      printf("descriptor %u: %08x\n", i, *descriptors);
      decode_structure(spec, descriptor_structure, descriptors);

      start = instruction_base + descriptors[0];
      if (!valid_offset(start)) {
         printf("kernel: %08lx <not valid>\n", start);
         continue;
      } else {
         printf("kernel: %08lx\n", start);
      }

      insns = (struct brw_instruction *) (gtt + start);
      gen_disasm_disassemble(disasm, insns, 0, stdout);

      dump_samplers(spec, descriptors[3] & ~0x1f);
      dump_binding_table(spec, descriptors[4] & ~0x1f);
   }
}

/* Heuristic to determine whether a uint32_t is probably actually a float
 * (http://stackoverflow.com/a/2953466)
 */

static bool
probably_float(uint32_t bits)
{
   int exp = ((bits & 0x7f800000U) >> 23) - 127;
   uint32_t mant = bits & 0x007fffff;

   /* +- 0.0 */
   if (exp == -127 && mant == 0)
      return true;

   /* +- 1 billionth to 1 billion */
   if (-30 <= exp && exp <= 30)
      return true;

   /* some value with only a few binary digits */
   if ((mant & 0x0000ffff) == 0)
      return true;

   return false;
}

static void
handle_3dstate_vertex_buffers(struct gen_spec *spec, uint32_t *p)
{
   uint32_t *end, *s, *dw, *dwend;
   uint64_t offset;
   int n, i, count, stride;

   end = (p[0] & 0xff) + p + 2;
   for (s = &p[1], n = 0; s < end; s += 4, n++) {
      if (gen_spec_get_gen(spec) >= gen_make_gen(8, 0)) {
         offset = *(uint64_t *) &s[1];
         dwend = gtt + offset + s[3];
      } else {
         offset = s[1];
         dwend = gtt + s[2] + 1;
      }

      stride = field(s[0], 0, 11);
      count = 0;
      printf("vertex buffer %d, size %d\n", n, s[3]);
      for (dw = gtt + offset, i = 0; dw < dwend && i < 256; dw++) {
         if (count == 0 && count % (8 * 4) == 0)
            printf("  ");

         if (probably_float(*dw))
            printf("  %8.2f", *(float *) dw);
         else
            printf("  0x%08x", *dw);

         i++;
         count += 4;

         if (count == stride) {
            printf("\n");
            count = 0;
         } else if (count % (8 * 4) == 0) {
            printf("\n");
         } else {
            printf(" ");
         }
      }
      if (count > 0 && count % (8 * 4) != 0)
         printf("\n");
   }
}

static void
handle_3dstate_vs(struct gen_spec *spec, uint32_t *p)
{
   uint64_t start;
   struct brw_instruction *insns;
   int vs_enable;

   if (gen_spec_get_gen(spec) >= gen_make_gen(8, 0)) {
      start = get_qword(&p[1]);
      vs_enable = p[7] & 1;
   } else {
      start = p[1];
      vs_enable = p[5] & 1;
   }

   if (vs_enable) {
      printf("instruction_base %08lx, start %08lx\n",
             instruction_base, start);

      insns = (struct brw_instruction *) (gtt + instruction_base + start);
      gen_disasm_disassemble(disasm, insns, 0, stdout);
   }
}

static void
handle_3dstate_hs(struct gen_spec *spec, uint32_t *p)
{
   uint64_t start;
   struct brw_instruction *insns;
   int hs_enable;

   if (gen_spec_get_gen(spec) >= gen_make_gen(8, 0)) {
      start = get_qword(&p[4]);
   } else {
      start = p[4];
   }

   hs_enable = p[2] & 0x80000000;

   if (hs_enable) {
      printf("instruction_base %08lx, start %08lx\n",
             instruction_base, start);

      insns = (struct brw_instruction *) (gtt + instruction_base + start);
      gen_disasm_disassemble(disasm, insns, 0, stdout);
   }
}

static void
handle_3dstate_constant(struct gen_spec *spec, uint32_t *p)
{
   int i, j, length;
   uint32_t *dw;
   float *f;

   for (i = 0; i < 4; i++) {
      length = (p[1 + i / 2] >> (i & 1) * 16) & 0xffff;
      f = (float *) (gtt + p[3 + i * 2] + dynamic_state_base);
      dw = (uint32_t *) f;
      for (j = 0; j < length * 8; j++) {
         if (probably_float(dw[j]))
            printf("  %04.3f", f[j]);
         else
            printf("  0x%08x", dw[j]);

         if ((j & 7) == 7)
            printf("\n");
      }
   }
}

static void
handle_3dstate_ps(struct gen_spec *spec, uint32_t *p)
{
   uint32_t mask = ~((1 << 6) - 1);
   uint64_t start;
   struct brw_instruction *insns;
   static const char unused[] = "unused";
   static const char *pixel_type[3] = {"8 pixel", "16 pixel", "32 pixel"};
   const char *k0, *k1, *k2;
   uint32_t k_mask, k1_offset, k2_offset;

   if (gen_spec_get_gen(spec) >= gen_make_gen(8, 0)) {
      k_mask = p[6] & 7;
      k1_offset = 8;
      k2_offset = 10;
   } else {
      k_mask = p[4] & 7;
      k1_offset = 6;
      k2_offset = 7;
   }

#define DISPATCH_8 1
#define DISPATCH_16 2
#define DISPATCH_32 4

   switch (k_mask) {
   case DISPATCH_8:
      k0 = pixel_type[0];
      k1 = unused;
      k2 = unused;
      break;
   case DISPATCH_16:
      k0 = pixel_type[1];
      k1 = unused;
      k2 = unused;
      break;
   case DISPATCH_8 | DISPATCH_16:
      k0 = pixel_type[0];
      k1 = unused;
      k2 = pixel_type[1];
      break;
   case DISPATCH_32:
      k0 = pixel_type[2];
      k1 = unused;
      k2 = unused;
      break;
   case DISPATCH_16 | DISPATCH_32:
      k0 = unused;
      k1 = pixel_type[2];
      k2 = pixel_type[1];
      break;
   case DISPATCH_8 | DISPATCH_16 | DISPATCH_32:
      k0 = pixel_type[0];
      k1 = pixel_type[2];
      k2 = pixel_type[1];
      break;
   default:
      k0 = unused;
      k1 = unused;
      k2 = unused;
      break;
   }

   start = instruction_base + (p[1] & mask);
   printf("  Kernel[0] %s\n", k0);
   if (k0 != unused) {
      insns = (struct brw_instruction *) (gtt + start);
      gen_disasm_disassemble(disasm, insns, 0, stdout);
   }

   start = instruction_base + (p[k1_offset] & mask);
   printf("  Kernel[1] %s\n", k1);
   if (k1 != unused) {
      insns = (struct brw_instruction *) (gtt + start);
      gen_disasm_disassemble(disasm, insns, 0, stdout);
   }

   start = instruction_base + (p[k2_offset] & mask);
   printf("  Kernel[2] %s\n", k2);
   if (k2 != unused) {
      insns = (struct brw_instruction *) (gtt + start);
      gen_disasm_disassemble(disasm, insns, 0, stdout);
   }
}

static void
handle_3dstate_binding_table_pointers(struct gen_spec *spec, uint32_t *p)
{
   dump_binding_table(spec, p[1]);
}

static void
handle_3dstate_sampler_state_pointers(struct gen_spec *spec, uint32_t *p)
{
   dump_samplers(spec, p[1]);
}

static void
handle_3dstate_viewport_state_pointers_cc(struct gen_spec *spec, uint32_t *p)
{
   uint64_t start;
   struct gen_group *cc_viewport;

   cc_viewport = gen_spec_find_struct(spec, "CC_VIEWPORT");

   start = dynamic_state_base + (p[1] & ~0x1fu);
   for (uint32_t i = 0; i < 4; i++) {
      printf("viewport %d\n", i);
      decode_structure(spec, cc_viewport, gtt + start + i * 8);
   }
}

static void
handle_3dstate_viewport_state_pointers_sf_clip(struct gen_spec *spec,
                                               uint32_t *p)
{
   uint64_t start;
   struct gen_group *sf_clip_viewport;

   sf_clip_viewport = gen_spec_find_struct(spec, "SF_CLIP_VIEWPORT");

   start = dynamic_state_base + (p[1] & ~0x3fu);
   for (uint32_t i = 0; i < 4; i++) {
      printf("viewport %d\n", i);
      decode_structure(spec, sf_clip_viewport, gtt + start + i * 64);
   }
}

static void
handle_3dstate_blend_state_pointers(struct gen_spec *spec, uint32_t *p)
{
   uint64_t start;
   struct gen_group *blend_state;

   blend_state = gen_spec_find_struct(spec, "BLEND_STATE");

   start = dynamic_state_base + (p[1] & ~0x3fu);
   decode_structure(spec, blend_state, gtt + start);
}

static void
handle_3dstate_cc_state_pointers(struct gen_spec *spec, uint32_t *p)
{
   uint64_t start;
   struct gen_group *cc_state;

   cc_state = gen_spec_find_struct(spec, "COLOR_CALC_STATE");

   start = dynamic_state_base + (p[1] & ~0x3fu);
   decode_structure(spec, cc_state, gtt + start);
}

static void
handle_3dstate_scissor_state_pointers(struct gen_spec *spec, uint32_t *p)
{
   uint64_t start;
   struct gen_group *scissor_rect;

   scissor_rect = gen_spec_find_struct(spec, "SCISSOR_RECT");

   start = dynamic_state_base + (p[1] & ~0x1fu);
   decode_structure(spec, scissor_rect, gtt + start);
}

#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])

#define STATE_BASE_ADDRESS                  0x61010000

#define MEDIA_INTERFACE_DESCRIPTOR_LOAD     0x70020000

#define _3DSTATE_INDEX_BUFFER               0x780a0000
#define _3DSTATE_VERTEX_BUFFERS             0x78080000

#define _3DSTATE_VS                         0x78100000
#define _3DSTATE_GS                         0x78110000
#define _3DSTATE_HS                         0x781b0000
#define _3DSTATE_DS                         0x781d0000

#define _3DSTATE_CONSTANT_VS                0x78150000
#define _3DSTATE_CONSTANT_GS                0x78160000
#define _3DSTATE_CONSTANT_PS                0x78170000
#define _3DSTATE_CONSTANT_HS                0x78190000
#define _3DSTATE_CONSTANT_DS                0x781A0000

#define _3DSTATE_PS                         0x78200000

#define _3DSTATE_BINDING_TABLE_POINTERS_VS  0x78260000
#define _3DSTATE_BINDING_TABLE_POINTERS_HS  0x78270000
#define _3DSTATE_BINDING_TABLE_POINTERS_DS  0x78280000
#define _3DSTATE_BINDING_TABLE_POINTERS_GS  0x78290000
#define _3DSTATE_BINDING_TABLE_POINTERS_PS  0x782a0000

#define _3DSTATE_SAMPLER_STATE_POINTERS_VS  0x782b0000
#define _3DSTATE_SAMPLER_STATE_POINTERS_GS  0x782e0000
#define _3DSTATE_SAMPLER_STATE_POINTERS_PS  0x782f0000

#define _3DSTATE_VIEWPORT_STATE_POINTERS_CC 0x78230000
#define _3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP 0x78210000
#define _3DSTATE_BLEND_STATE_POINTERS       0x78240000
#define _3DSTATE_CC_STATE_POINTERS          0x780e0000
#define _3DSTATE_SCISSOR_STATE_POINTERS     0x780f0000

struct custom_handler {
   uint32_t opcode;
   void (*handle)(struct gen_spec *spec, uint32_t *p);
} custom_handlers[] = {
   { STATE_BASE_ADDRESS, handle_state_base_address },
   { MEDIA_INTERFACE_DESCRIPTOR_LOAD, handle_media_interface_descriptor_load },
   { _3DSTATE_VERTEX_BUFFERS, handle_3dstate_vertex_buffers },
   { _3DSTATE_INDEX_BUFFER, handle_3dstate_index_buffer },
   { _3DSTATE_VS, handle_3dstate_vs },
   { _3DSTATE_GS, handle_3dstate_vs },
   { _3DSTATE_DS, handle_3dstate_vs },
   { _3DSTATE_HS, handle_3dstate_hs },
   { _3DSTATE_CONSTANT_VS, handle_3dstate_constant },
   { _3DSTATE_CONSTANT_GS, handle_3dstate_constant },
   { _3DSTATE_CONSTANT_PS, handle_3dstate_constant },
   { _3DSTATE_CONSTANT_HS, handle_3dstate_constant },
   { _3DSTATE_CONSTANT_DS, handle_3dstate_constant },
   { _3DSTATE_PS, handle_3dstate_ps },

   { _3DSTATE_BINDING_TABLE_POINTERS_VS, handle_3dstate_binding_table_pointers },
   { _3DSTATE_BINDING_TABLE_POINTERS_HS, handle_3dstate_binding_table_pointers },
   { _3DSTATE_BINDING_TABLE_POINTERS_DS, handle_3dstate_binding_table_pointers },
   { _3DSTATE_BINDING_TABLE_POINTERS_GS, handle_3dstate_binding_table_pointers },
   { _3DSTATE_BINDING_TABLE_POINTERS_PS, handle_3dstate_binding_table_pointers },

   { _3DSTATE_SAMPLER_STATE_POINTERS_VS, handle_3dstate_sampler_state_pointers },
   { _3DSTATE_SAMPLER_STATE_POINTERS_GS, handle_3dstate_sampler_state_pointers },
   { _3DSTATE_SAMPLER_STATE_POINTERS_PS, handle_3dstate_sampler_state_pointers },

   { _3DSTATE_VIEWPORT_STATE_POINTERS_CC, handle_3dstate_viewport_state_pointers_cc },
   { _3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP, handle_3dstate_viewport_state_pointers_sf_clip },
   { _3DSTATE_BLEND_STATE_POINTERS, handle_3dstate_blend_state_pointers },
   { _3DSTATE_CC_STATE_POINTERS, handle_3dstate_cc_state_pointers },
   { _3DSTATE_SCISSOR_STATE_POINTERS, handle_3dstate_scissor_state_pointers }
};

static void
parse_commands(struct gen_spec *spec, uint32_t *cmds, int size, int engine)
{
   uint32_t *p, *end = cmds + size / 4;
   unsigned int length, i;
   struct gen_group *inst;

   for (p = cmds; p < end; p += length) {
      inst = gen_spec_find_instruction(spec, p);
      if (inst == NULL) {
         printf("unknown instruction %08x\n", p[0]);
         length = (p[0] & 0xff) + 2;
         continue;
      }
      length = gen_group_get_length(inst, p);

      const char *color, *reset_color = CLEAR_TO_EOL NORMAL;
      uint64_t offset;

      if (option_full_decode)
         color = HEADER;
      else
         color = NORMAL;

      if (option_color == COLOR_NEVER) {
         color = "";
         reset_color = "";
      }

      if (option_print_offsets)
         offset = (void *) p - gtt;
      else
         offset = 0;

      printf("%s0x%08lx:  0x%08x:  %s%s\n",
             color, offset, p[0],
             gen_group_get_name(inst), reset_color);

      if (option_full_decode) {
         struct gen_field_iterator iter;
         char *token = NULL;
         int idx = 0, dword_num = 0;
         gen_field_iterator_init(&iter, inst, p);
         while (gen_field_iterator_next(&iter)) {
            idx = 0;
            print_dword_val(&iter, offset, &dword_num);
            if (dword_num > 0)
                token = print_iterator_values(&iter, &idx);
            if (token != NULL) {
                printf("0x%08lx:  0x%08x : Dword %d\n",
                       offset + 4 * idx, p[idx], idx);
                handle_struct_decode(spec,token, &p[idx]);
                token = NULL;
            }
         }

         for (i = 0; i < ARRAY_LENGTH(custom_handlers); i++) {
            if (gen_group_get_opcode(inst) ==
                custom_handlers[i].opcode)
               custom_handlers[i].handle(spec, p);
         }
      }

      if ((p[0] & 0xffff0000) == AUB_MI_BATCH_BUFFER_START) {
         uint64_t start;
         if (gen_spec_get_gen(spec) >= gen_make_gen(8,0))
            start = get_qword(&p[1]);
         else
            start = p[1];

         parse_commands(spec, gtt + start, 1 << 20, engine);
      } else if ((p[0] & 0xffff0000) == AUB_MI_BATCH_BUFFER_END) {
         break;
      }
   }
}

#define GEN_ENGINE_RENDER 1
#define GEN_ENGINE_BLITTER 2

static void
handle_trace_block(struct gen_spec *spec, uint32_t *p)
{
   int operation = p[1] & AUB_TRACE_OPERATION_MASK;
   int type = p[1] & AUB_TRACE_TYPE_MASK;
   int address_space = p[1] & AUB_TRACE_ADDRESS_SPACE_MASK;
   uint64_t offset = p[3];
   uint32_t size = p[4];
   int header_length = p[0] & 0xffff;
   uint32_t *data = p + header_length + 2;
   int engine = GEN_ENGINE_RENDER;

   if (gen_spec_get_gen(spec) >= gen_make_gen(8,0))
      offset += (uint64_t) p[5] << 32;

   switch (operation) {
   case AUB_TRACE_OP_DATA_WRITE:
      if (address_space != AUB_TRACE_MEMTYPE_GTT)
         break;
      if (gtt_size < offset + size) {
         fprintf(stderr, "overflow gtt space: %s", strerror(errno));
         exit(EXIT_FAILURE);
      }
      memcpy((char *) gtt + offset, data, size);
      if (gtt_end < offset + size)
         gtt_end = offset + size;
      break;
   case AUB_TRACE_OP_COMMAND_WRITE:
      switch (type) {
      case AUB_TRACE_TYPE_RING_PRB0:
         engine = GEN_ENGINE_RENDER;
         break;
      case AUB_TRACE_TYPE_RING_PRB2:
         engine = GEN_ENGINE_BLITTER;
         break;
      default:
         printf("command write to unknown ring %d\n", type);
         break;
      }

      parse_commands(spec, data, size, engine);
      gtt_end = 0;
      break;
   }
}

struct aub_file {
   char *filename;
   int fd;
   struct stat sb;
   uint32_t *map, *end, *cursor;
};

static struct aub_file *
aub_file_open(const char *filename)
{
   struct aub_file *file;

   file = malloc(sizeof *file);
   file->filename = strdup(filename);
   file->fd = open(file->filename, O_RDONLY);
   if (file->fd == -1) {
      fprintf(stderr, "open %s failed: %s", file->filename, strerror(errno));
      exit(EXIT_FAILURE);
   }

   if (fstat(file->fd, &file->sb) == -1) {
      fprintf(stderr, "stat failed: %s", strerror(errno));
      exit(EXIT_FAILURE);
   }

   file->map = mmap(NULL, file->sb.st_size,
                    PROT_READ, MAP_SHARED, file->fd, 0);
   if (file->map == MAP_FAILED) {
      fprintf(stderr, "mmap failed: %s", strerror(errno));
      exit(EXIT_FAILURE);
   }

   file->cursor = file->map;
   file->end = file->map + file->sb.st_size / 4;

   /* mmap a terabyte for our gtt space. */
   gtt_size = 1ul << 40;
   gtt = mmap(NULL, gtt_size, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS |  MAP_NORESERVE, -1, 0);
   if (gtt == MAP_FAILED) {
      fprintf(stderr, "failed to alloc gtt space: %s", strerror(errno));
      exit(1);
   }

   return file;
}

#define TYPE(dw)       (((dw) >> 29) & 7)
#define OPCODE(dw)     (((dw) >> 23) & 0x3f)
#define SUBOPCODE(dw)  (((dw) >> 16) & 0x7f)

#define MAKE_HEADER(type, opcode, subopcode) \
                   (((type) << 29) | ((opcode) << 23) | ((subopcode) << 16))

#define TYPE_AUB            0x7

/* Classic AUB opcodes */
#define OPCODE_AUB          0x01
#define SUBOPCODE_HEADER    0x05
#define SUBOPCODE_BLOCK     0x41
#define SUBOPCODE_BMP       0x1e

/* Newer version AUB opcode */
#define OPCODE_NEW_AUB      0x2e
#define SUBOPCODE_VERSION   0x00
#define SUBOPCODE_REG_WRITE 0x03
#define SUBOPCODE_MEM_POLL  0x05
#define SUBOPCODE_MEM_WRITE 0x06

#define MAKE_GEN(major, minor) ( ((major) << 8) | (minor) )

struct {
   const char *name;
   uint32_t gen;
} device_map[] = {
   { "bwr", MAKE_GEN(4, 0) },
   { "cln", MAKE_GEN(4, 0) },
   { "blc", MAKE_GEN(4, 0) },
   { "ctg", MAKE_GEN(4, 0) },
   { "el", MAKE_GEN(4, 0) },
   { "il", MAKE_GEN(4, 0) },
   { "sbr", MAKE_GEN(6, 0) },
   { "ivb", MAKE_GEN(7, 0) },
   { "lrb2", MAKE_GEN(0, 0) },
   { "hsw", MAKE_GEN(7, 5) },
   { "vlv", MAKE_GEN(7, 0) },
   { "bdw", MAKE_GEN(8, 0) },
   { "skl", MAKE_GEN(9, 0) },
   { "chv", MAKE_GEN(8, 0) },
   { "bxt", MAKE_GEN(9, 0) }
};

static void
aub_file_decode_batch(struct aub_file *file, struct gen_spec *spec)
{
   uint32_t *p, h, device, data_type;
   int header_length, payload_size, bias;

   p = file->cursor;
   h = *p;
   header_length = h & 0xffff;

   switch (OPCODE(h)) {
   case OPCODE_AUB:
      bias = 2;
      break;
   case OPCODE_NEW_AUB:
      bias = 1;
      break;
   default:
      printf("unknown opcode %d at %ld/%ld\n",
             OPCODE(h), file->cursor - file->map,
             file->end - file->map);
      file->cursor = file->end;
      return;
   }

   payload_size = 0;
   switch (h & 0xffff0000) {
   case MAKE_HEADER(TYPE_AUB, OPCODE_AUB, SUBOPCODE_HEADER):
      payload_size = p[12];
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_AUB, SUBOPCODE_BLOCK):
      payload_size = p[4];
      handle_trace_block(spec, p);
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_AUB, SUBOPCODE_BMP):
      break;

   case MAKE_HEADER(TYPE_AUB, OPCODE_NEW_AUB, SUBOPCODE_VERSION):
      printf("version block: dw1 %08x\n", p[1]);
      device = (p[1] >> 8) & 0xff;
      printf("  device %s\n", device_map[device].name);
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_NEW_AUB, SUBOPCODE_REG_WRITE):
      printf("register write block: (dwords %d)\n", h & 0xffff);
      printf("  reg 0x%x, data 0x%x\n", p[1], p[5]);
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_NEW_AUB, SUBOPCODE_MEM_WRITE):
      printf("memory write block (dwords %d):\n", h & 0xffff);
      printf("  address 0x%lx\n", *(uint64_t *) &p[1]);
      data_type = (p[3] >> 20) & 0xff;
      if (data_type != 0)
         printf("  data type 0x%x\n", data_type);
      printf("  address space 0x%x\n", (p[3] >> 28) & 0xf);
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_NEW_AUB, SUBOPCODE_MEM_POLL):
      printf("memory poll block (dwords %d):\n", h & 0xffff);
      break;
   default:
      printf("unknown block type=0x%x, opcode=0x%x, "
             "subopcode=0x%x (%08x)\n", TYPE(h), OPCODE(h), SUBOPCODE(h), h);
      break;
   }
   file->cursor = p + header_length + bias + payload_size / 4;
}

static int
aub_file_more_stuff(struct aub_file *file)
{
   return file->cursor < file->end;
}

static void
setup_pager(void)
{
   int fds[2];
   pid_t pid;

   if (!isatty(1))
      return;

   if (pipe(fds) == -1)
      return;

   pid = fork();
   if (pid == -1)
      return;

   if (pid == 0) {
      close(fds[1]);
      dup2(fds[0], 0);
      execlp("less", "less", "-rFi", NULL);
   }

   close(fds[0]);
   dup2(fds[1], 1);
   close(fds[1]);
}

static void
print_help(const char *progname, FILE *file)
{
   fprintf(file,
           "Usage: %s [OPTION]... FILE\n"
           "Decode aub file contents.\n\n"
           "A valid --gen option must be provided.\n\n"
           "      --help          display this help and exit\n"
           "      --gen=platform  decode for given platform (ivb, byt, hsw, bdw, chv, skl, kbl or bxt)\n"
           "      --headers       decode only command headers\n"
           "      --color[=WHEN]  colorize the output; WHEN can be 'auto' (default\n"
           "                        if omitted), 'always', or 'never'\n"
           "      --no-pager      don't launch pager\n"
           "      --no-offsets    don't print instruction offsets\n",
           progname);
}

static bool
is_prefix(const char *arg, const char *prefix, const char **value)
{
   int l = strlen(prefix);

   if (strncmp(arg, prefix, l) == 0 && (arg[l] == '\0' || arg[l] == '=')) {
      if (arg[l] == '=')
         *value = arg + l + 1;
      else
         *value = NULL;

      return true;
   }

   return false;
}

int main(int argc, char *argv[])
{
   struct gen_spec *spec;
   struct aub_file *file;
   int i, pci_id = 0;
   bool found_arg_gen = false, pager = true;
   int gen_major, gen_minor;
   const char *value;
   char gen_file[256], gen_val[24];

   if (argc == 1) {
      print_help(argv[0], stderr);
      exit(EXIT_FAILURE);
   }

   for (i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--no-pager") == 0) {
         pager = false;
      } else if (strcmp(argv[i], "--no-offsets") == 0) {
         option_print_offsets = false;
      } else if (is_prefix(argv[i], "--gen", &value)) {
         if (value == NULL) {
            fprintf(stderr, "option '--gen' requires an argument\n");
            exit(EXIT_FAILURE);
         }
         found_arg_gen = true;
         gen_major = 0;
         gen_minor = 0;
         snprintf(gen_val, sizeof(gen_val), "%s", value);
      } else if (strcmp(argv[i], "--headers") == 0) {
         option_full_decode = false;
      } else if (is_prefix(argv[i], "--color", &value)) {
         if (value == NULL || strcmp(value, "always") == 0)
            option_color = COLOR_ALWAYS;
         else if (strcmp(value, "never") == 0)
            option_color = COLOR_NEVER;
         else if (strcmp(value, "auto") == 0)
            option_color = COLOR_AUTO;
         else {
            fprintf(stderr, "invalid value for --color: %s", value);
            exit(EXIT_FAILURE);
         }
      } else if (strcmp(argv[i], "--help") == 0) {
         print_help(argv[0], stdout);
         exit(EXIT_SUCCESS);
      } else {
         if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            exit(EXIT_FAILURE);
         }
         break;
      }
   }

   if (!found_arg_gen) {
      fprintf(stderr, "argument --gen is required\n");
      exit(EXIT_FAILURE);
   }

   if (strstr(gen_val, "ivb") != NULL) {
      /* Intel(R) Ivybridge Mobile GT2 */
      pci_id = 0x0166;
      gen_major = 7;
      gen_minor = 0;
   } else if (strstr(gen_val, "hsw") != NULL) {
      /* Intel(R) Haswell Mobile GT2 */
      pci_id = 0x0416;
      gen_major = 7;
      gen_minor = 5;
   } else if (strstr(gen_val, "byt") != NULL) {
      /* Intel(R) Bay Trail */
      pci_id = 0x0155;
      gen_major = 7;
      gen_minor = 5;
   } else if (strstr(gen_val, "bdw") != NULL) {
      /* Intel(R) HD Graphics 5500 (Broadwell GT2) */
      pci_id = 0x1616;
      gen_major = 8;
      gen_minor = 0;
   }  else if (strstr(gen_val, "chv") != NULL) {
      /* Intel(R) HD Graphics (Cherryview) */
      pci_id = 0x22B3;
      gen_major = 8;
      gen_minor = 0;
   } else if (strstr(gen_val, "skl") != NULL) {
      /* Intel(R) HD Graphics 530 (Skylake GT2) */
      pci_id = 0x1912;
      gen_major = 9;
      gen_minor = 0;
   } else if (strstr(gen_val, "kbl") != NULL) {
      /* Intel(R) Kabylake GT2 */
      pci_id = 0x591D;
      gen_major = 9;
      gen_minor = 0;
   } else if (strstr(gen_val, "bxt") != NULL) {
      /* Intel(R) HD Graphics (Broxton) */
      pci_id = 0x0A84;
      gen_major = 9;
      gen_minor = 0;
   } else {
      fprintf(stderr, "can't parse gen: %s, expected ivb, byt, hsw, "
                             "bdw, chv, skl, kbl or bxt\n", gen_val);
      exit(EXIT_FAILURE);
   }

   /* Do this before we redirect stdout to pager. */
   if (option_color == COLOR_AUTO)
      option_color = isatty(1) ? COLOR_ALWAYS : COLOR_NEVER;

   if (isatty(1) && pager)
      setup_pager();

   if (gen_minor > 0) {
      snprintf(gen_file, sizeof(gen_file), "../genxml/gen%d%d.xml",
               gen_major, gen_minor);
   } else {
      snprintf(gen_file, sizeof(gen_file), "../genxml/gen%d.xml", gen_major);
   }

   spec = gen_spec_load(gen_file);
   disasm = gen_disasm_create(pci_id);

   if (argv[i] == NULL) {
       print_help(argv[0], stderr);
       exit(EXIT_FAILURE);
   } else {
       file = aub_file_open(argv[i]);
   }

   while (aub_file_more_stuff(file))
      aub_file_decode_batch(file, spec);

   fflush(stdout);
   /* close the stdout which is opened to write the output */
   close(1);

   wait(NULL);

   return EXIT_SUCCESS;
}
