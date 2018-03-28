/*
 * Copyright (C) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdlib.h>

#include "lima_screen.h"
#include "lima_vamgr.h"
#include "lima_util.h"

#include "util/u_math.h"

struct lima_va_hole {
   struct list_head list;
   uint64_t offset;
   uint64_t size;
};

bool lima_vamgr_init(struct lima_screen *screen)
{
   struct lima_va_hole *hole;

   list_inithead(&screen->va_holes);
   mtx_init(&screen->va_lock, mtx_plain);

   hole = malloc(sizeof(*hole));
   if (!hole)
      return false;

   hole->offset = screen->va_start;
   hole->size = screen->va_end;
   list_add(&hole->list, &screen->va_holes);
   return true;
}

void lima_vamgr_fini(struct lima_screen *screen)
{
   list_for_each_entry_safe(struct lima_va_hole, hole, &screen->va_holes, list) {
      list_del(&hole->list);
      free(hole);
   }
   mtx_destroy(&screen->va_lock);
}

bool lima_va_range_alloc(struct lima_screen *screen, uint32_t size, uint32_t *va)
{
   bool ret = false;

   size = align(size, LIMA_PAGE_SIZE);

   mtx_lock(&screen->va_lock);

   list_for_each_entry(struct lima_va_hole, hole, &screen->va_holes, list) {
      if (hole->size == size) {
         *va = hole->offset;
         ret = true;

         list_del(&hole->list);
         free(hole);
         break;
      }
      else if (hole->size > size) {
         *va = hole->offset;
         ret = true;

         hole->offset += size;
         hole->size -= size;
         break;
      }
   }

   mtx_unlock(&screen->va_lock);
   return ret;
}

static bool add_va_hole(struct list_head *head, uint32_t size, uint32_t va)
{
   struct lima_va_hole *hole;

   hole = malloc(sizeof(*hole));
   if (!hole)
      return false;

   hole->offset = va;
   hole->size = size;

   list_addtail(&hole->list, head);
   return true;
}

bool lima_va_range_free(struct lima_screen *screen, uint32_t size, uint32_t va)
{
   bool ret = true;

   va &= ~(LIMA_PAGE_SIZE - 1);
   size = align(size, LIMA_PAGE_SIZE);

   mtx_lock(&screen->va_lock);

   if (list_empty(&screen->va_holes))
      ret = add_va_hole(&screen->va_holes, size, va);
   else {
      list_for_each_entry(struct lima_va_hole, hole, &screen->va_holes, list) {
         if (va + size == hole->offset) {
            hole->offset = va;
            hole->size += size;
            break;
         }
         else if (va + size < hole->offset) {
            ret = add_va_hole(&hole->list, size, va);
            break;
         }
         else if (va == hole->offset + hole->size) {
            if (hole->list.next != &screen->va_holes) {
               struct lima_va_hole *next;
               next = list_first_entry(&hole->list, struct lima_va_hole, list);
               if (next->offset == va + size) {
                  size += next->size;
                  list_del(&next->list);
                  free(next);
               }
            }
            hole->size += size;
            break;
         }
         else if (va > hole->offset + hole->size) {
            if (hole->list.next == &screen->va_holes) {
               ret = add_va_hole(&screen->va_holes, size, va);
               break;
            }
         }
      }
   }

   mtx_unlock(&screen->va_lock);
   return ret;
}
