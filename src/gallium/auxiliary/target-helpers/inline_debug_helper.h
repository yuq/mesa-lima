
#ifndef INLINE_DEBUG_HELPER_H
#define INLINE_DEBUG_HELPER_H

#include "pipe/p_compiler.h"
#include "util/u_debug.h"
#include "util/u_tests.h"


/* Helper function to wrap a screen with
 * one or more debug driver: rbug, trace.
 */

#include "ddebug/dd_public.h"
#include "trace/tr_public.h"
#include "rbug/rbug_public.h"
#include "noop/noop_public.h"

static inline struct pipe_screen *
debug_screen_wrap(struct pipe_screen *screen)
{
   screen = ddebug_screen_create(screen);
   screen = rbug_screen_create(screen);
   screen = trace_screen_create(screen);
   screen = noop_screen_create(screen);

   if (debug_get_bool_option("GALLIUM_TESTS", FALSE))
      util_run_tests(screen);

   return screen;
}

#endif
