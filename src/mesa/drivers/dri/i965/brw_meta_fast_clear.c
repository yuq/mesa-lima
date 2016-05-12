/*
 * Copyright © 2014 Intel Corporation
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

#include "main/mtypes.h"
#include "main/macros.h"
#include "main/context.h"
#include "main/objectlabel.h"
#include "main/shaderapi.h"
#include "main/shaderobj.h"
#include "main/arrayobj.h"
#include "main/bufferobj.h"
#include "main/buffers.h"
#include "main/blend.h"
#include "main/enable.h"
#include "main/depth.h"
#include "main/stencil.h"
#include "main/varray.h"
#include "main/uniforms.h"
#include "main/fbobject.h"
#include "main/framebuffer.h"
#include "main/renderbuffer.h"
#include "main/texobj.h"

#include "main/api_validate.h"
#include "main/state.h"

#include "util/format_srgb.h"

#include "vbo/vbo_context.h"

#include "drivers/common/meta.h"

#include "brw_defines.h"
#include "brw_context.h"
#include "brw_draw.h"
#include "brw_state.h"
#include "intel_fbo.h"
#include "intel_batchbuffer.h"

#include "brw_blorp.h"
#include "brw_meta_util.h"

struct brw_fast_clear_state {
   struct gl_buffer_object *buf_obj;
   struct gl_vertex_array_object *array_obj;
   struct gl_shader_program *shader_prog;
   GLuint vao;
   GLint color_location;
};

static bool
brw_fast_clear_init(struct brw_context *brw)
{
   struct brw_fast_clear_state *clear;
   struct gl_context *ctx = &brw->ctx;

   if (brw->fast_clear_state) {
      clear = brw->fast_clear_state;
      _mesa_BindVertexArray(clear->vao);
      return true;
   }

   brw->fast_clear_state = clear = malloc(sizeof *clear);
   if (clear == NULL)
      return false;

   memset(clear, 0, sizeof *clear);
   _mesa_GenVertexArrays(1, &clear->vao);
   _mesa_BindVertexArray(clear->vao);

   clear->buf_obj = ctx->Driver.NewBufferObject(ctx, 0xDEADBEEF);
   if (clear->buf_obj == NULL)
      return false;

   clear->array_obj = _mesa_lookup_vao(ctx, clear->vao);
   assert(clear->array_obj != NULL);

   _mesa_update_array_format(ctx, clear->array_obj, VERT_ATTRIB_GENERIC(0),
                             2, GL_FLOAT, GL_RGBA, GL_FALSE, GL_FALSE, GL_FALSE,
                             0, true);
   _mesa_bind_vertex_buffer(ctx, clear->array_obj, VERT_ATTRIB_GENERIC(0),
                            clear->buf_obj, 0, sizeof(float) * 2);
   _mesa_enable_vertex_array_attrib(ctx, clear->array_obj,
                                    VERT_ATTRIB_GENERIC(0));

   return true;
}

static void
brw_bind_rep_write_shader(struct brw_context *brw, float *color)
{
   const char *vs_source =
      "#extension GL_AMD_vertex_shader_layer : enable\n"
      "#extension GL_ARB_draw_instanced : enable\n"
      "#extension GL_ARB_explicit_attrib_location : enable\n"
      "layout(location = 0) in vec4 position;\n"
      "uniform int layer;\n"
      "void main()\n"
      "{\n"
      "#ifdef GL_AMD_vertex_shader_layer\n"
      "   gl_Layer = gl_InstanceID;\n"
      "#endif\n"
      "   gl_Position = position;\n"
      "}\n";
   const char *fs_source =
      "uniform vec4 color;\n"
      "void main()\n"
      "{\n"
      "   gl_FragColor = color;\n"
      "}\n";

   struct brw_fast_clear_state *clear = brw->fast_clear_state;
   struct gl_context *ctx = &brw->ctx;

   if (clear->shader_prog) {
      _mesa_meta_use_program(ctx, clear->shader_prog);
      _mesa_Uniform4fv(clear->color_location, 1, color);
      return;
   }

   _mesa_meta_compile_and_link_program(ctx, vs_source, fs_source,
                                       "meta repclear",
                                       &clear->shader_prog);

   clear->color_location =
      _mesa_program_resource_location(clear->shader_prog, GL_UNIFORM, "color");

   _mesa_meta_use_program(ctx, clear->shader_prog);
   _mesa_Uniform4fv(clear->color_location, 1, color);
}

void
brw_meta_fast_clear_free(struct brw_context *brw)
{
   struct brw_fast_clear_state *clear = brw->fast_clear_state;
   GET_CURRENT_CONTEXT(old_context);

   if (clear == NULL)
      return;

   _mesa_make_current(&brw->ctx, NULL, NULL);

   _mesa_DeleteVertexArrays(1, &clear->vao);
   _mesa_reference_buffer_object(&brw->ctx, &clear->buf_obj, NULL);
   _mesa_reference_shader_program(&brw->ctx, &clear->shader_prog, NULL);
   free(clear);

   if (old_context)
      _mesa_make_current(old_context, old_context->WinSysDrawBuffer, old_context->WinSysReadBuffer);
   else
      _mesa_make_current(NULL, NULL, NULL);
}

struct rect {
   unsigned x0, y0, x1, y1;
};

static void
brw_draw_rectlist(struct brw_context *brw, struct rect *rect, int num_instances)
{
   struct gl_context *ctx = &brw->ctx;
   struct brw_fast_clear_state *clear = brw->fast_clear_state;
   int start = 0, count = 3;
   struct _mesa_prim prim;
   float verts[6];

   verts[0] = rect->x1;
   verts[1] = rect->y1;
   verts[2] = rect->x0;
   verts[3] = rect->y1;
   verts[4] = rect->x0;
   verts[5] = rect->y0;

   /* upload new vertex data */
   _mesa_buffer_data(ctx, clear->buf_obj, GL_NONE, sizeof(verts), verts,
                     GL_DYNAMIC_DRAW, __func__);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   vbo_bind_arrays(ctx);

   memset(&prim, 0, sizeof prim);
   prim.begin = 1;
   prim.end = 1;
   prim.mode = BRW_PRIM_OFFSET + _3DPRIM_RECTLIST;
   prim.num_instances = num_instances;
   prim.start = start;
   prim.count = count;

   /* Make sure our internal prim value doesn't clash with a valid GL value. */
   assert(!_mesa_is_valid_prim_mode(ctx, prim.mode));

   brw_draw_prims(ctx, &prim, 1, NULL,
                  GL_TRUE, start, start + count - 1,
                  NULL, 0, NULL);
}

/**
 * Convert the given color to a bitfield suitable for ORing into DWORD 7 of
 * SURFACE_STATE (DWORD 12-15 on SKL+).
 *
 * Returned boolean tells if the given color differs from the stored.
 */
bool
brw_meta_set_fast_clear_color(struct brw_context *brw,
                              struct intel_mipmap_tree *mt,
                              const union gl_color_union *color)
{
   union gl_color_union override_color = *color;

   /* The sampler doesn't look at the format of the surface when the fast
    * clear color is used so we need to implement luminance, intensity and
    * missing components manually.
    */
   switch (_mesa_get_format_base_format(mt->format)) {
   case GL_INTENSITY:
      override_color.ui[3] = override_color.ui[0];
      /* flow through */
   case GL_LUMINANCE:
   case GL_LUMINANCE_ALPHA:
      override_color.ui[1] = override_color.ui[0];
      override_color.ui[2] = override_color.ui[0];
      break;
   default:
      for (int i = 0; i < 3; i++) {
         if (!_mesa_format_has_color_component(mt->format, i))
            override_color.ui[i] = 0;
      }
      break;
   }

   if (!_mesa_format_has_color_component(mt->format, 3)) {
      if (_mesa_is_format_integer_color(mt->format))
         override_color.ui[3] = 1;
      else
         override_color.f[3] = 1.0f;
   }

   /* Handle linear→SRGB conversion */
   if (brw->ctx.Color.sRGBEnabled &&
       _mesa_get_srgb_format_linear(mt->format) != mt->format) {
      for (int i = 0; i < 3; i++) {
         override_color.f[i] =
            util_format_linear_to_srgb_float(override_color.f[i]);
      }
   }

   bool updated;
   if (brw->gen >= 9) {
      updated = memcmp(&mt->gen9_fast_clear_color, &override_color,
                       sizeof(mt->gen9_fast_clear_color));
      mt->gen9_fast_clear_color = override_color;
   } else {
      const uint32_t old_color_value = mt->fast_clear_color_value;

      mt->fast_clear_color_value = 0;
      for (int i = 0; i < 4; i++) {
         /* Testing for non-0 works for integer and float colors */
         if (override_color.f[i] != 0.0f) {
             mt->fast_clear_color_value |=
                1 << (GEN7_SURFACE_CLEAR_COLOR_SHIFT + (3 - i));
         }
      }

      updated = (old_color_value != mt->fast_clear_color_value);
   }

   return updated;
}

static const uint32_t fast_clear_color[4] = { ~0, ~0, ~0, ~0 };

static void
set_fast_clear_op(struct brw_context *brw, uint32_t op)
{
   /* Set op and dirty BRW_NEW_FRAGMENT_PROGRAM to make sure we re-emit
    * 3DSTATE_PS.
    */
   brw->wm.fast_clear_op = op;
   brw->ctx.NewDriverState |= BRW_NEW_FRAGMENT_PROGRAM;
}

static void
use_rectlist(struct brw_context *brw, bool enable)
{
   /* Set custom state to let us use _3DPRIM_RECTLIST and the replicated
    * rendertarget write.  When we enable reclist mode, we disable the
    * viewport transform, disable clipping, enable the rep16 write
    * optimization and disable simd8 dispatch in the PS.
    */
   brw->sf.viewport_transform_enable = !enable;
   brw->use_rep_send = enable;
   brw->no_simd8 = enable;

   /* Dirty state to make sure we reemit the state packages affected by the
    * custom state.  We dirty BRW_NEW_FRAGMENT_PROGRAM to emit 3DSTATE_PS for
    * disabling simd8 dispatch, _NEW_LIGHT to emit 3DSTATE_SF for disabling
    * the viewport transform and 3DSTATE_CLIP to disable clipping for the
    * reclist primitive.  This is a little messy - it would be nicer to
    * BRW_NEW_FAST_CLEAR flag or so, but we're out of brw state bits.  Dirty
    * _NEW_BUFFERS to make sure we emit new SURFACE_STATE with the new fast
    * clear color value.
    */
   brw->NewGLState |= _NEW_LIGHT | _NEW_BUFFERS;
   brw->ctx.NewDriverState |= BRW_NEW_FRAGMENT_PROGRAM;
}

/**
 * Individually fast clear each color buffer attachment. On previous gens this
 * isn't required. The motivation for this comes from one line (which seems to
 * be specific to SKL+). The list item is in section titled _MCS Buffer for
 * Render Target(s)_
 *
 *   "Since only one RT is bound with a clear pass, only one RT can be cleared
 *   at a time. To clear multiple RTs, multiple clear passes are required."
 *
 * The code follows the same idea as the resolve code which creates a fake FBO
 * to avoid interfering with too much of the GL state.
 */
static void
fast_clear_attachments(struct brw_context *brw,
                       struct gl_framebuffer *fb,
                       uint32_t fast_clear_buffers,
                       struct rect fast_clear_rect)
{
   struct gl_context *ctx = &brw->ctx;
   const bool srgb_enabled = ctx->Color.sRGBEnabled;

   assert(brw->gen >= 9);

   /* Make sure the GL_FRAMEBUFFER_SRGB is disabled during fast clear so that
    * the surface state will always be uploaded with a linear buffer. SRGB
    * buffers are not supported on Gen9 because they are not marked as
    * losslessly compressible. This shouldn't matter for the fast clear
    * because the color is not written to the framebuffer yet so the hardware
    * doesn't need to do any SRGB conversion.
    */
   if (srgb_enabled)
      _mesa_set_framebuffer_srgb(ctx, GL_FALSE);

   brw_bind_rep_write_shader(brw, (float *) fast_clear_color);

   /* SKL+ also has a resolve mode for compressed render targets and thus more
    * bits to let us select the type of resolve.  For fast clear resolves, it
    * turns out we can use the same value as pre-SKL though.
    */
   set_fast_clear_op(brw, GEN7_PS_RENDER_TARGET_FAST_CLEAR_ENABLE);

   while (fast_clear_buffers) {
      int index = ffs(fast_clear_buffers) - 1;

      fast_clear_buffers &= ~(1 << index);

      _mesa_meta_drawbuffers_from_bitfield(1 << index);

      brw_draw_rectlist(brw, &fast_clear_rect, MAX2(1, fb->MaxNumLayers));

      /* Now set the mcs we cleared to INTEL_FAST_CLEAR_STATE_CLEAR so we'll
       * resolve them eventually.
       */
      struct gl_renderbuffer *rb = fb->_ColorDrawBuffers[0];
      struct intel_renderbuffer *irb = intel_renderbuffer(rb);
      irb->mt->fast_clear_state = INTEL_FAST_CLEAR_STATE_CLEAR;
   }

   set_fast_clear_op(brw, 0);

   if (srgb_enabled)
      _mesa_set_framebuffer_srgb(ctx, GL_TRUE);
}

bool
brw_meta_fast_clear(struct brw_context *brw, struct gl_framebuffer *fb,
                    GLbitfield buffers, bool partial_clear)
{
   struct gl_context *ctx = &brw->ctx;
   enum { FAST_CLEAR, REP_CLEAR, PLAIN_CLEAR } clear_type;
   GLbitfield plain_clear_buffers, meta_save, rep_clear_buffers, fast_clear_buffers;
   struct rect fast_clear_rect, clear_rect;
   int layers;

   fast_clear_buffers = rep_clear_buffers = plain_clear_buffers = 0;

   /* First we loop through the color draw buffers and determine which ones
    * can be fast cleared, which ones can use the replicated write and which
    * ones have to fall back to regular color clear.
    */
   for (unsigned buf = 0; buf < fb->_NumColorDrawBuffers; buf++) {
      struct gl_renderbuffer *rb = fb->_ColorDrawBuffers[buf];
      struct intel_renderbuffer *irb = intel_renderbuffer(rb);
      int index = fb->_ColorDrawBufferIndexes[buf];

      /* Only clear the buffers present in the provided mask */
      if (((1 << index) & buffers) == 0)
         continue;

      /* If this is an ES2 context or GL_ARB_ES2_compatibility is supported,
       * the framebuffer can be complete with some attachments missing.  In
       * this case the _ColorDrawBuffers pointer will be NULL.
       */
      if (rb == NULL)
         continue;

      clear_type = FAST_CLEAR;

      /* We don't have fast clear until gen7. */
      if (brw->gen < 7)
         clear_type = REP_CLEAR;

      if (irb->mt->fast_clear_state == INTEL_FAST_CLEAR_STATE_NO_MCS)
         clear_type = REP_CLEAR;

      /* We can't do scissored fast clears because of the restrictions on the
       * fast clear rectangle size.
       */
      if (partial_clear)
         clear_type = REP_CLEAR;

      /* Fast clear is only supported for colors where all components are
       * either 0 or 1.
       */
      if (!brw_is_color_fast_clear_compatible(brw, irb->mt,
                                              &ctx->Color.ClearColor))
         clear_type = REP_CLEAR;

      /* From the SNB PRM (Vol4_Part1):
       *
       *     "Replicated data (Message Type = 111) is only supported when
       *      accessing tiled memory.  Using this Message Type to access
       *      linear (untiled) memory is UNDEFINED."
       */
      if (irb->mt->tiling == I915_TILING_NONE) {
         perf_debug("Falling back to plain clear because %dx%d buffer is untiled\n",
                    irb->mt->logical_width0, irb->mt->logical_height0);
         clear_type = PLAIN_CLEAR;
      }

      /* Constant color writes ignore everything in blend and color calculator
       * state.  This is not documented.
       */
      GLubyte *color_mask = ctx->Color.ColorMask[buf];
      for (int i = 0; i < 4; i++) {
         if (_mesa_format_has_color_component(irb->mt->format, i) &&
             !(i == 3 && irb->Base.Base._BaseFormat == GL_RGB) &&
             !color_mask[i]) {
            perf_debug("Falling back to plain clear on %dx%d buffer because of color mask\n",
                       irb->mt->logical_width0, irb->mt->logical_height0);
            clear_type = PLAIN_CLEAR;
         }
      }

      /* Allocate the MCS for non MSRT surfaces now if we're doing a fast
       * clear and we don't have the MCS yet.  On failure, fall back to
       * replicated clear.
       */
      if (clear_type == FAST_CLEAR && irb->mt->mcs_mt == NULL)
         if (!intel_miptree_alloc_non_msrt_mcs(brw, irb->mt))
            clear_type = REP_CLEAR;

      switch (clear_type) {
      case FAST_CLEAR:
         brw_meta_set_fast_clear_color(brw, irb->mt, &ctx->Color.ClearColor);
         irb->need_downsample = true;

         /* If the buffer is already in INTEL_FAST_CLEAR_STATE_CLEAR, the
          * clear is redundant and can be skipped.  Only skip after we've
          * updated the fast clear color above though.
          */
         if (irb->mt->fast_clear_state == INTEL_FAST_CLEAR_STATE_CLEAR)
            continue;

         /* Set fast_clear_state to RESOLVED so we don't try resolve them when
          * we draw, in case the mt is also bound as a texture.
          */
         irb->mt->fast_clear_state = INTEL_FAST_CLEAR_STATE_RESOLVED;
         irb->need_downsample = true;
         fast_clear_buffers |= 1 << index;
         brw_get_fast_clear_rect(brw, fb, irb->mt,
                                 &fast_clear_rect.x0, &fast_clear_rect.y0,
                                 &fast_clear_rect.x1, &fast_clear_rect.y1);
         break;

      case REP_CLEAR:
         rep_clear_buffers |= 1 << index;
         brw_meta_get_buffer_rect(fb,
                                  &clear_rect.x0, &clear_rect.y0,
                                  &clear_rect.x1, &clear_rect.y1);
         break;

      case PLAIN_CLEAR:
         plain_clear_buffers |= 1 << index;
         brw_meta_get_buffer_rect(fb,
                                  &clear_rect.x0, &clear_rect.y0,
                                  &clear_rect.x1, &clear_rect.y1);
         continue;
      }
   }

   assert((fast_clear_buffers & rep_clear_buffers) == 0);

   if (!(fast_clear_buffers | rep_clear_buffers)) {
      if (plain_clear_buffers)
         /* If we only have plain clears, skip the meta save/restore. */
         goto out;
      else
         /* Nothing left to do.  This happens when we hit the redundant fast
          * clear case above and nothing else.
          */
         return true;
   }

   meta_save =
      MESA_META_ALPHA_TEST |
      MESA_META_BLEND |
      MESA_META_DEPTH_TEST |
      MESA_META_RASTERIZATION |
      MESA_META_SHADER |
      MESA_META_STENCIL_TEST |
      MESA_META_VERTEX |
      MESA_META_VIEWPORT |
      MESA_META_CLIP |
      MESA_META_CLAMP_FRAGMENT_COLOR |
      MESA_META_MULTISAMPLE |
      MESA_META_OCCLUSION_QUERY |
      MESA_META_DRAW_BUFFERS;

   _mesa_meta_begin(ctx, meta_save);

   if (!brw_fast_clear_init(brw)) {
      /* This is going to be hard to recover from, most likely out of memory.
       * Bail and let meta try and (probably) fail for us.
       */
      plain_clear_buffers = buffers;
      goto bail_to_meta;
   }

   /* Clears never have the color clamped. */
   if (ctx->Extensions.ARB_color_buffer_float)
      _mesa_ClampColor(GL_CLAMP_FRAGMENT_COLOR, GL_FALSE);

   _mesa_set_enable(ctx, GL_DEPTH_TEST, GL_FALSE);
   _mesa_DepthMask(GL_FALSE);
   _mesa_set_enable(ctx, GL_STENCIL_TEST, GL_FALSE);

   use_rectlist(brw, true);

   layers = MAX2(1, fb->MaxNumLayers);

   if (brw->gen >= 9 && fast_clear_buffers) {
      fast_clear_attachments(brw, fb, fast_clear_buffers, fast_clear_rect);
   } else if (fast_clear_buffers) {
      _mesa_meta_drawbuffers_from_bitfield(fast_clear_buffers);
      brw_bind_rep_write_shader(brw, (float *) fast_clear_color);
      set_fast_clear_op(brw, GEN7_PS_RENDER_TARGET_FAST_CLEAR_ENABLE);
      brw_draw_rectlist(brw, &fast_clear_rect, layers);
      set_fast_clear_op(brw, 0);

      /* Now set the mcs we cleared to INTEL_FAST_CLEAR_STATE_CLEAR so we'll
       * resolve them eventually.
       */
      for (unsigned buf = 0; buf < fb->_NumColorDrawBuffers; buf++) {
         struct gl_renderbuffer *rb = fb->_ColorDrawBuffers[buf];
         struct intel_renderbuffer *irb = intel_renderbuffer(rb);
         int index = fb->_ColorDrawBufferIndexes[buf];

         if ((1 << index) & fast_clear_buffers)
            irb->mt->fast_clear_state = INTEL_FAST_CLEAR_STATE_CLEAR;
      }
   }

   if (rep_clear_buffers) {
      _mesa_meta_drawbuffers_from_bitfield(rep_clear_buffers);
      brw_bind_rep_write_shader(brw, ctx->Color.ClearColor.f);
      brw_draw_rectlist(brw, &clear_rect, layers);
   }

 bail_to_meta:
   /* Dirty _NEW_BUFFERS so we reemit SURFACE_STATE which sets the fast clear
    * color before resolve and sets irb->mt->fast_clear_state to UNRESOLVED if
    * we render to it.
    */
   brw->NewGLState |= _NEW_BUFFERS;


   /* Set the custom state back to normal and dirty the same bits as above */
   use_rectlist(brw, false);

   _mesa_meta_end(ctx);

   /* From BSpec: Render Target Fast Clear:
    *
    *     After Render target fast clear, pipe-control with color cache
    *     write-flush must be issued before sending any DRAW commands on that
    *     render target.
    */
   brw_emit_mi_flush(brw);

   /* If we had to fall back to plain clear for any buffers, clear those now
    * by calling into meta.
    */
 out:
   if (plain_clear_buffers)
      _mesa_meta_glsl_Clear(&brw->ctx, plain_clear_buffers);

   return true;
}

void
brw_meta_resolve_color(struct brw_context *brw,
                       struct intel_mipmap_tree *mt)
{
   struct gl_context *ctx = &brw->ctx;
   struct gl_framebuffer *drawFb;
   struct gl_renderbuffer *rb;
   struct rect rect;

   brw_emit_mi_flush(brw);

   drawFb = ctx->Driver.NewFramebuffer(ctx, 0xDEADBEEF);
   if (drawFb == NULL) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "in %s", __func__);
      return;
   }

   _mesa_meta_begin(ctx, MESA_META_ALL);

   rb = brw_get_rb_for_slice(brw, mt, 0, 0, false);

   _mesa_bind_framebuffers(ctx, drawFb, ctx->ReadBuffer);
   _mesa_framebuffer_renderbuffer(ctx, ctx->DrawBuffer, GL_COLOR_ATTACHMENT0,
                                  rb);
   _mesa_DrawBuffer(GL_COLOR_ATTACHMENT0);

   brw_fast_clear_init(brw);

   use_rectlist(brw, true);

   brw_bind_rep_write_shader(brw, (float *) fast_clear_color);

   /* SKL+ also has a resolve mode for compressed render targets and thus more
    * bits to let us select the type of resolve.  For fast clear resolves, it
    * turns out we can use the same value as pre-SKL though.
    */
   if (intel_miptree_is_lossless_compressed(brw, mt))
      set_fast_clear_op(brw, GEN9_PS_RENDER_TARGET_RESOLVE_FULL);
   else
      set_fast_clear_op(brw, GEN7_PS_RENDER_TARGET_RESOLVE_ENABLE);

   mt->fast_clear_state = INTEL_FAST_CLEAR_STATE_RESOLVED;
   brw_get_resolve_rect(brw, mt, &rect.x0, &rect.y0, &rect.x1, &rect.y1);

   brw_draw_rectlist(brw, &rect, 1);

   set_fast_clear_op(brw, 0);
   use_rectlist(brw, false);

   _mesa_reference_renderbuffer(&rb, NULL);
   _mesa_reference_framebuffer(&drawFb, NULL);

   _mesa_meta_end(ctx);

   /* We're typically called from intel_update_state() and we're supposed to
    * return with the state all updated to what it was before
    * brw_meta_resolve_color() was called.  The meta rendering will have
    * messed up the state and we need to call _mesa_update_state() again to
    * get back to where we were supposed to be when resolve was called.
    */
   if (ctx->NewState)
      _mesa_update_state(ctx);
}
