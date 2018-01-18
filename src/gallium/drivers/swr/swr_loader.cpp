/****************************************************************************
 * Copyright (C) 2016 Intel Corporation.   All Rights Reserved.
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
 ***************************************************************************/

#include "util/u_cpu_detect.h"
#include "util/u_dl.h"
#include "swr_public.h"
#include "swr_screen.h"

#include <stdio.h>

// Helper function to resolve the backend filename based on architecture
inline void get_swr_arch_filename(const char arch[], char filename[])
{
#ifdef HAVE_SWR_BUILTIN
   strcpy(filename , "builtin");
#else
   sprintf(filename, "%sswr%s%s", UTIL_DL_PREFIX, arch, UTIL_DL_EXT);
#endif
}

struct pipe_screen *
swr_create_screen(struct sw_winsys *winsys)
{
   char filename[256] = { 0 };
   bool found = false;
   bool is_knl = false;
   PFNSwrGetInterface pfnSwrGetInterface = nullptr;

   util_cpu_detect();

   if (!found && util_cpu_caps.has_avx512f && util_cpu_caps.has_avx512er) {
      fprintf(stderr, "SWR detected KNL instruction support ");
#ifndef HAVE_SWR_KNL
      fprintf(stderr, "(skipping not built).\n");
#else
      get_swr_arch_filename("KNL", filename);
      found = true;
      is_knl = true;
#endif
   }

   if (!found && util_cpu_caps.has_avx512f && util_cpu_caps.has_avx512bw) {
      fprintf(stderr, "SWR detected SKX instruction support ");
#ifndef HAVE_SWR_SKX
      fprintf(stderr, "(skipping not built).\n");
#else
      get_swr_arch_filename("SKX", filename);
      found = true;
#endif
   }

   if (!found && util_cpu_caps.has_avx2) {
      fprintf(stderr, "SWR detected AVX2 instruction support ");
#ifndef HAVE_SWR_AVX2
      fprintf(stderr, "(skipping not built).\n");
#else
      get_swr_arch_filename("AVX2", filename);
      found = true;
#endif
   }

   if (!found && util_cpu_caps.has_avx) {
      fprintf(stderr, "SWR detected AVX instruction support ");
#ifndef HAVE_SWR_AVX
      fprintf(stderr, "(skipping not built).\n");
#else
      get_swr_arch_filename("AVX", filename);
      found = true;
#endif
   }

   if (!found) {
      fprintf(stderr, "SWR could not detect a supported CPU architecture.\n");
      exit(-1);
   }

   fprintf(stderr, "(using %s).\n", filename);

#ifdef HAVE_SWR_BUILTIN
   pfnSwrGetInterface = SwrGetInterface;
#else
   util_dl_library *pLibrary = util_dl_open(filename);
   if (!pLibrary) {
      fprintf(stderr, "SWR library load failure: %s\n", util_dl_error());
      exit(-1);
   }

   util_dl_proc pApiProc = util_dl_get_proc_address(pLibrary, "SwrGetInterface");
   if (!pApiProc) {
      fprintf(stderr, "SWR library search failure: %s\n", util_dl_error());
      exit(-1);
   }

   pfnSwrGetInterface = (PFNSwrGetInterface)pApiProc;
#endif

   struct pipe_screen *screen = swr_create_screen_internal(winsys);
   swr_screen(screen)->is_knl = is_knl;
   swr_screen(screen)->pfnSwrGetInterface = pfnSwrGetInterface;

   return screen;
}


#ifdef _WIN32
// swap function called from libl_gdi.c

void
swr_gdi_swap(struct pipe_screen *screen,
             struct pipe_resource *res,
             void *hDC)
{
   screen->flush_frontbuffer(screen,
                             res,
                             0, 0,
                             hDC,
                             NULL);
}

#endif /* _WIN32 */
