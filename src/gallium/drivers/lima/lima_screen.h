/*
 * Copyright (c) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef H_LIMA_SCREEN
#define H_LIMA_SCREEN

#include "util/slab.h"

#include "pipe/p_screen.h"

#include "lima.h"

#define LIMA_MAX_MIP_LEVELS 12

struct ra_regs;

struct lima_screen {
   struct pipe_screen base;
   struct renderonly *ro;

   int refcnt;
   void *winsys_priv;

   struct lima_device_info info;
   lima_device_handle dev;
   int fd;

   struct slab_parent_pool transfer_pool;

   struct ra_regs *pp_ra;
};

static inline struct lima_screen *
lima_screen(struct pipe_screen *pscreen)
{
   return (struct lima_screen *)pscreen;
}

struct pipe_screen *
lima_screen_create(int fd, struct renderonly *ro);

#endif
