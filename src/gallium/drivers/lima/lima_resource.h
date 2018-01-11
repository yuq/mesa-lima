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

#ifndef H_LIMA_RESOURCE
#define H_LIMA_RESOURCE

#include "pipe/p_state.h"

struct lima_screen;

struct lima_resource {
   struct pipe_resource base;

   struct renderonly_scanout *scanout;
   struct lima_bo *bo;
   uint32_t stride;
   uint32_t layer_stride;
};

struct lima_surface {
   struct pipe_surface base;
};

struct lima_transfer {
   struct pipe_transfer base;
};

static inline struct lima_resource *
lima_resource(struct pipe_resource *res)
{
   return (struct lima_resource *)res;
}

static inline struct lima_surface *
lima_surface(struct pipe_surface *surf)
{
   return (struct lima_surface *)surf;
}

static inline struct lima_transfer *
lima_transfer(struct pipe_transfer *trans)
{
   return (struct lima_transfer *)trans;
}

void
lima_resource_screen_init(struct lima_screen *screen);

void
lima_resource_context_init(struct lima_context *ctx);

#endif
