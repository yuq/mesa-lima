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

#include "common/gen_decoder.h"
#include "gen_disasm.h"

#include <string.h>

void
gen_batch_decode_ctx_init(struct gen_batch_decode_ctx *ctx,
                          const struct gen_device_info *devinfo,
                          FILE *fp, enum gen_batch_decode_flags flags,
                          const char *xml_path,
                          struct gen_batch_decode_bo (*get_bo)(void *,
                                                               uint64_t),
                          void *user_data)
{
   memset(ctx, 0, sizeof(*ctx));

   ctx->get_bo = get_bo;
   ctx->user_data = user_data;
   ctx->fp = fp;
   ctx->flags = flags;

   if (xml_path == NULL)
      ctx->spec = gen_spec_load(devinfo);
   else
      ctx->spec = gen_spec_load_from_path(devinfo, xml_path);
   ctx->disasm = gen_disasm_create(devinfo);
}

void
gen_batch_decode_ctx_finish(struct gen_batch_decode_ctx *ctx)
{
   gen_spec_destroy(ctx->spec);
   gen_disasm_destroy(ctx->disasm);
}

#define CSI "\e["
#define BLUE_HEADER  CSI "0;44m"
#define GREEN_HEADER CSI "1;42m"
#define NORMAL       CSI "0m"

#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])

static void
ctx_print_group(struct gen_batch_decode_ctx *ctx,
                struct gen_group *group,
                uint64_t address, const void *map)
{
   gen_print_group(ctx->fp, group, address, map, 0,
                   (ctx->flags & GEN_BATCH_DECODE_IN_COLOR) != 0);
}

static struct gen_batch_decode_bo
ctx_get_bo(struct gen_batch_decode_ctx *ctx, uint64_t addr)
{
   if (gen_spec_get_gen(ctx->spec) >= gen_make_gen(8,0)) {
      /* On Broadwell and above, we have 48-bit addresses which consume two
       * dwords.  Some packets require that these get stored in a "canonical
       * form" which means that bit 47 is sign-extended through the upper
       * bits. In order to correctly handle those aub dumps, we need to mask
       * off the top 16 bits.
       */
      addr &= (~0ull >> 16);
   }

   struct gen_batch_decode_bo bo = ctx->get_bo(ctx->user_data, addr);

   if (gen_spec_get_gen(ctx->spec) >= gen_make_gen(8,0))
      bo.addr &= (~0ull >> 16);

   /* We may actually have an offset into the bo */
   if (bo.map != NULL) {
      assert(bo.addr <= addr);
      uint64_t offset = addr - bo.addr;
      bo.map += offset;
      bo.addr += offset;
      bo.size -= offset;
   }

   return bo;
}

static void
handle_state_base_address(struct gen_batch_decode_ctx *ctx, const uint32_t *p)
{
   struct gen_group *inst = gen_spec_find_instruction(ctx->spec, p);

   struct gen_field_iterator iter;
   gen_field_iterator_init(&iter, inst, p, 0, false);

   do {
      if (strcmp(iter.name, "Surface State Base Address") == 0) {
         ctx->surface_base = ctx_get_bo(ctx, iter.raw_value);
      } else if (strcmp(iter.name, "Dynamic State Base Address") == 0) {
         ctx->dynamic_base = ctx_get_bo(ctx, iter.raw_value);
      } else if (strcmp(iter.name, "Instruction Base Address") == 0) {
         ctx->instruction_base = ctx_get_bo(ctx, iter.raw_value);
      }
   } while (gen_field_iterator_next(&iter));
}

struct custom_decoder {
   const char *cmd_name;
   void (*decode)(struct gen_batch_decode_ctx *ctx, const uint32_t *p);
} custom_decoders[] = {
   { "STATE_BASE_ADDRESS", handle_state_base_address },
};

static inline uint64_t
get_address(struct gen_spec *spec, const uint32_t *p)
{
   /* Addresses are always guaranteed to be page-aligned and sometimes
    * hardware packets have extra stuff stuffed in the bottom 12 bits.
    */
   uint64_t addr = p[0] & ~0xfffu;

   if (gen_spec_get_gen(spec) >= gen_make_gen(8,0)) {
      /* On Broadwell and above, we have 48-bit addresses which consume two
       * dwords.  Some packets require that these get stored in a "canonical
       * form" which means that bit 47 is sign-extended through the upper
       * bits. In order to correctly handle those aub dumps, we need to mask
       * off the top 16 bits.
       */
      addr |= ((uint64_t)p[1] & 0xffff) << 32;
   }

   return addr;
}

void
gen_print_batch(struct gen_batch_decode_ctx *ctx,
                const uint32_t *batch, uint32_t batch_size,
                uint64_t batch_addr)
{
   const uint32_t *p, *end = batch + batch_size;
   int length;
   struct gen_group *inst;

   for (p = batch; p < end; p += length) {
      inst = gen_spec_find_instruction(ctx->spec, p);
      length = gen_group_get_length(inst, p);
      assert(inst == NULL || length > 0);
      length = MAX2(1, length);
      if (inst == NULL) {
         fprintf(ctx->fp, "unknown instruction %08x\n", p[0]);
         continue;
      }

      const char *color, *reset_color;
      uint64_t offset;

      const char *inst_name = gen_group_get_name(inst);
      if (ctx->flags & GEN_BATCH_DECODE_IN_COLOR) {
         reset_color = NORMAL;
         if (ctx->flags & GEN_BATCH_DECODE_FULL) {
            if (strcmp(inst_name, "MI_BATCH_BUFFER_START") == 0 ||
                strcmp(inst_name, "MI_BATCH_BUFFER_END") == 0)
               color = GREEN_HEADER;
            else
               color = BLUE_HEADER;
         } else {
            color = NORMAL;
         }
      } else {
         color = "";
         reset_color = "";
      }

      if (ctx->flags & GEN_BATCH_DECODE_OFFSETS)
         offset = batch_addr + ((char *)p - (char *)batch);
      else
         offset = 0;

      fprintf(ctx->fp, "%s0x%08"PRIx64":  0x%08x:  %-80s%s\n",
              color, offset, p[0], inst_name, reset_color);

      if (ctx->flags & GEN_BATCH_DECODE_FULL) {
         ctx_print_group(ctx, inst, offset, p);

         for (int i = 0; i < ARRAY_LENGTH(custom_decoders); i++) {
            if (strcmp(inst_name, custom_decoders[i].cmd_name) == 0) {
               custom_decoders[i].decode(ctx, p);
               break;
            }
         }
      }

      if (strcmp(inst_name, "MI_BATCH_BUFFER_START") == 0) {
         struct gen_batch_decode_bo next_batch;
         bool second_level;
         struct gen_field_iterator iter;
         gen_field_iterator_init(&iter, inst, p, 0, false);
         do {
            if (strcmp(iter.name, "Batch Buffer Start Address") == 0) {
               next_batch = ctx_get_bo(ctx, iter.raw_value);
            } else if (strcmp(iter.name, "Second Level Batch Buffer") == 0) {
               second_level = iter.raw_value;
            }
         } while (gen_field_iterator_next(&iter));

         if (next_batch.map == NULL) {
            fprintf(ctx->fp, "Secondary batch at 0x%08"PRIx64" unavailable",
                    next_batch.addr);
         }

         if (second_level) {
            /* MI_BATCH_BUFFER_START with "2nd Level Batch Buffer" set acts
             * like a subroutine call.  Commands that come afterwards get
             * processed once the 2nd level batch buffer returns with
             * MI_BATCH_BUFFER_END.
             */
            if (next_batch.map) {
               gen_print_batch(ctx, next_batch.map, next_batch.size,
                               next_batch.addr);
            }
         } else {
            /* MI_BATCH_BUFFER_START with "2nd Level Batch Buffer" unset acts
             * like a goto.  Nothing after it will ever get processed.  In
             * order to prevent the recursion from growing, we just reset the
             * loop and continue;
             */
            if (next_batch.map) {
               p = next_batch.map;
               end = next_batch.map + next_batch.size;
               length = 0;
               continue;
            } else {
               /* Nothing we can do */
               break;
            }
         }
      } else if (strcmp(inst_name, "MI_BATCH_BUFFER_END") == 0) {
         break;
      }
   }
}
