/*
 * Copyright (C) 2018 Lima Project
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

#ifndef H_LIMA_SUBMIT
#define H_LIMA_SUBMIT

#include <stdbool.h>
#include <stdint.h>

struct lima_screen;
struct lima_bo;

struct lima_submit {
   struct lima_screen *screen;
   uint32_t pipe;
   uint32_t fence;

   struct drm_lima_gem_submit_bo *bos;
   uint32_t max_bos;
   uint32_t nr_bos;

   void *frame;
   uint32_t frame_size;
};

struct lima_submit *lima_submit_create(struct lima_screen *screen, uint32_t pipe);
void lima_submit_delete(struct lima_submit *submit);
bool lima_submit_add_bo(struct lima_submit *submit, struct lima_bo *bo, uint32_t flags);

static inline void lima_submit_set_frame(struct lima_submit *submit,
                                         void *frame, uint32_t size)
{
   submit->frame = frame;
   submit->frame_size = size;
}

bool lima_submit_start(struct lima_submit *submit);
bool lima_submit_wait(struct lima_submit *submit, uint64_t timeout_ns, bool relative);

#endif
