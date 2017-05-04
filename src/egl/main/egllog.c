/**************************************************************************
 *
 * Copyright 2008 VMware, Inc.
 * Copyright 2009-2010 Chia-I Wu <olvaffe@gmail.com>
 * Copyright 2010 LunarG, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


/**
 * Logging facility for debug/info messages.
 * _EGL_FATAL messages are printed to stderr
 * The EGL_LOG_LEVEL var controls the output of other warning/info/debug msgs.
 */


#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "c11/threads.h"

#include "egllog.h"

#ifdef HAVE_ANDROID_PLATFORM
#define LOG_TAG "EGL-MAIN"
#include <cutils/log.h>

/* support versions < JellyBean */
#ifndef ALOGW
#define ALOGW LOGW
#endif
#ifndef ALOGD
#define ALOGD LOGD
#endif
#ifndef ALOGI
#define ALOGI LOGI
#endif

#endif /* HAVE_ANDROID_PLATFORM */

#define MAXSTRING 1000
#define FALLBACK_LOG_LEVEL _EGL_WARNING


static struct {
   mtx_t mutex;

   EGLBoolean initialized;
   EGLint level;
} logging = {
   _MTX_INITIALIZER_NP,
   EGL_FALSE,
   FALLBACK_LOG_LEVEL,
};

static const char *level_strings[] = {
   /* the order is important */
   "fatal",
   "warning",
   "info",
   "debug",
   NULL
};


/**
 * The default logger.  It prints the message to stderr.
 */
static void
_eglDefaultLogger(EGLint level, const char *msg)
{
#ifdef HAVE_ANDROID_PLATFORM
   switch (level) {
   case _EGL_DEBUG:
      ALOGD("%s", msg);
      break;
   case _EGL_INFO:
      ALOGI("%s", msg);
      break;
   case _EGL_WARNING:
      ALOGW("%s", msg);
      break;
   case _EGL_FATAL:
      LOG_FATAL("%s", msg);
      break;
   default:
      break;
   }
#else
   fprintf(stderr, "libEGL %s: %s\n", level_strings[level], msg);
#endif /* HAVE_ANDROID_PLATFORM */
}


/**
 * Initialize the logging facility.
 */
static void
_eglInitLogger(void)
{
   const char *log_env;
   EGLint i, level = -1;

   if (logging.initialized)
      return;

   log_env = getenv("EGL_LOG_LEVEL");
   if (log_env) {
      for (i = 0; level_strings[i]; i++) {
         if (strcasecmp(log_env, level_strings[i]) == 0) {
            level = i;
            break;
         }
      }
   }

   logging.level = (level >= 0) ? level : FALLBACK_LOG_LEVEL;
   logging.initialized = EGL_TRUE;

   /* it is fine to call _eglLog now */
   if (log_env && level < 0) {
      _eglLog(_EGL_WARNING,
              "Unrecognized EGL_LOG_LEVEL environment variable value. "
              "Expected one of \"fatal\", \"warning\", \"info\", \"debug\". "
              "Got \"%s\". Falling back to \"%s\".",
              log_env, level_strings[FALLBACK_LOG_LEVEL]);
   }
}


/**
 * Log a message with message logger.
 * \param level one of _EGL_FATAL, _EGL_WARNING, _EGL_INFO, _EGL_DEBUG.
 */
void
_eglLog(EGLint level, const char *fmtStr, ...)
{
   va_list args;
   char msg[MAXSTRING];
   int ret;

   /* one-time initialization; a little race here is fine */
   if (!logging.initialized)
      _eglInitLogger();
   if (level > logging.level || level < 0)
      return;

   mtx_lock(&logging.mutex);

   va_start(args, fmtStr);
   ret = vsnprintf(msg, MAXSTRING, fmtStr, args);
   if (ret < 0 || ret >= MAXSTRING)
      strcpy(msg, "<message truncated>");
   va_end(args);

   _eglDefaultLogger(level, msg);

   mtx_unlock(&logging.mutex);

   if (level == _EGL_FATAL)
      exit(1); /* or abort()? */
}
