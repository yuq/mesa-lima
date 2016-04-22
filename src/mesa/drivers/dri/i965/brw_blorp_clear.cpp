/*
 * Copyright © 2013 Intel Corporation
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

#include "main/teximage.h"
#include "main/blend.h"
#include "main/fbobject.h"
#include "main/renderbuffer.h"
#include "main/glformats.h"

#include "util/ralloc.h"

#include "intel_fbo.h"

#include "brw_blorp.h"
#include "brw_meta_util.h"
#include "brw_context.h"
#include "brw_eu.h"
#include "brw_state.h"

#define FILE_DEBUG_FLAG DEBUG_BLORP

struct brw_blorp_const_color_prog_key
{
   bool use_simd16_replicated_data;
   bool pad[3];
};

class brw_blorp_const_color_program
{
public:
   brw_blorp_const_color_program(struct brw_context *brw,
                                 const brw_blorp_const_color_prog_key *key);
   ~brw_blorp_const_color_program();

   const GLuint *compile(struct brw_context *brw, GLuint *program_size);

   brw_blorp_prog_data prog_data;

private:
   void alloc_regs();

   void *mem_ctx;
   const brw_blorp_const_color_prog_key *key;
   struct brw_codegen func;

   /* Thread dispatch header */
   struct brw_reg R0;

   /* Pixel X/Y coordinates (always in R1). */
   struct brw_reg R1;

   /* Register with push constants (a single vec4) */
   struct brw_reg clear_rgba;

   /* MRF used for render target writes */
   GLuint base_mrf;
};

brw_blorp_const_color_program::brw_blorp_const_color_program(
      struct brw_context *brw,
      const brw_blorp_const_color_prog_key *key)
   : mem_ctx(ralloc_context(NULL)),
     key(key),
     R0(),
     R1(),
     clear_rgba(),
     base_mrf(0)
{
   prog_data.first_curbe_grf = 0;
   prog_data.persample_msaa_dispatch = false;
   brw_init_codegen(brw->intelScreen->devinfo, &func, mem_ctx);
}

brw_blorp_const_color_program::~brw_blorp_const_color_program()
{
   ralloc_free(mem_ctx);
}

static void
brw_blorp_params_get_clear_kernel(struct brw_context *brw,
                                  struct brw_blorp_params *params,
                                  brw_blorp_const_color_prog_key *wm_prog_key)
{
   if (!brw_search_cache(&brw->cache, BRW_CACHE_BLORP_PROG,
                         wm_prog_key, sizeof(*wm_prog_key),
                         &params->wm_prog_kernel, &params->wm_prog_data)) {
      brw_blorp_const_color_program prog(brw, wm_prog_key);
      GLuint program_size;
      const GLuint *program = prog.compile(brw, &program_size);
      brw_upload_cache(&brw->cache, BRW_CACHE_BLORP_PROG,
                       wm_prog_key, sizeof(*wm_prog_key),
                       program, program_size,
                       &prog.prog_data, sizeof(prog.prog_data),
                       &params->wm_prog_kernel, &params->wm_prog_data);
   }
}

static bool
set_write_disables(const struct intel_renderbuffer *irb, 
                   const GLubyte *color_mask, bool *color_write_disable)
{
   /* Format information in the renderbuffer represents the requirements
    * given by the client. There are cases where the backing miptree uses,
    * for example, RGBA to represent RGBX. Since the client is only expecting
    * RGB we can treat alpha as not used and write whatever we like into it.
    */
   const GLenum base_format = irb->Base.Base._BaseFormat;
   const int components = _mesa_base_format_component_count(base_format);
   bool disables = false;

   assert(components > 0);

   for (int i = 0; i < components; i++) {
      color_write_disable[i] = !color_mask[i];
      disables = disables || !color_mask[i];
   }

   return disables;
}

void
brw_blorp_const_color_program::alloc_regs()
{
   int reg = 0;
   this->R0 = retype(brw_vec8_grf(reg++, 0), BRW_REGISTER_TYPE_UW);
   this->R1 = retype(brw_vec8_grf(reg++, 0), BRW_REGISTER_TYPE_UW);

   prog_data.first_curbe_grf = reg;
   clear_rgba = retype(brw_vec4_grf(reg++, 0), BRW_REGISTER_TYPE_F);
   reg += BRW_BLORP_NUM_PUSH_CONST_REGS;

   /* Make sure we didn't run out of registers */
   assert(reg <= GEN7_MRF_HACK_START);

   this->base_mrf = 2;
}

const GLuint *
brw_blorp_const_color_program::compile(struct brw_context *brw,
                                       GLuint *program_size)
{
   /* Set up prog_data */
   memset(&prog_data, 0, sizeof(prog_data));
   prog_data.persample_msaa_dispatch = false;

   alloc_regs();

   brw_set_default_compression_control(&func, BRW_COMPRESSION_COMPRESSED);

   struct brw_reg mrf_rt_write =
      retype(vec16(brw_message_reg(base_mrf)), BRW_REGISTER_TYPE_F);

   uint32_t mlen, msg_type;
   if (key->use_simd16_replicated_data) {
      /* The message payload is a single register with the low 4 floats/ints
       * filled with the constant clear color.
       */
      brw_set_default_exec_size(&func, BRW_EXECUTE_4);
      brw_set_default_mask_control(&func, BRW_MASK_DISABLE);
      brw_MOV(&func, vec4(brw_message_reg(base_mrf)), clear_rgba);
      brw_set_default_mask_control(&func, BRW_MASK_ENABLE);
      brw_set_default_exec_size(&func, BRW_EXECUTE_16);

      msg_type = BRW_DATAPORT_RENDER_TARGET_WRITE_SIMD16_SINGLE_SOURCE_REPLICATED;
      mlen = 1;
   } else {
      brw_set_default_exec_size(&func, BRW_EXECUTE_16);
      for (int i = 0; i < 4; i++) {
         /* The message payload is pairs of registers for 16 pixels each of r,
          * g, b, and a.
          */
         brw_MOV(&func,
                 brw_message_reg(base_mrf + i * 2),
                 brw_vec1_grf(clear_rgba.nr, i));
      }

      msg_type = BRW_DATAPORT_RENDER_TARGET_WRITE_SIMD16_SINGLE_SOURCE;
      mlen = 8;
   }

   /* Now write to the render target and terminate the thread */
   brw_fb_WRITE(&func,
                16 /* dispatch_width */,
                base_mrf >= 0 ? brw_message_reg(base_mrf) : mrf_rt_write,
                brw_null_reg() /* header */,
                msg_type,
                BRW_BLORP_RENDERBUFFER_BINDING_TABLE_INDEX,
                mlen,
                0 /* response_length */,
                true /* eot */,
                true /* last render target */,
                false /* header present */);

   if (unlikely(INTEL_DEBUG & DEBUG_BLORP)) {
      fprintf(stderr, "Native code for BLORP clear:\n");
      brw_disassemble(brw->intelScreen->devinfo,
                      func.store, 0, func.next_insn_offset, stderr);
      fprintf(stderr, "\n");
   }

   brw_compact_instructions(&func, 0, 0, NULL);
   return brw_get_program(&func, program_size);
}


static bool
do_single_blorp_clear(struct brw_context *brw, struct gl_framebuffer *fb,
                      struct gl_renderbuffer *rb, unsigned buf,
                      bool partial_clear, bool encode_srgb, unsigned layer)
{
   struct gl_context *ctx = &brw->ctx;
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);
   mesa_format format = irb->mt->format;

   brw_blorp_params params;

   if (!encode_srgb && _mesa_get_format_color_encoding(format) == GL_SRGB)
      format = _mesa_get_srgb_format_linear(format);

   params.dst.set(brw, irb->mt, irb->mt_level, layer, format, true);

   /* Override the surface format according to the context's sRGB rules. */
   params.dst.brw_surfaceformat = brw->render_target_format[format];

   params.x0 = fb->_Xmin;
   params.x1 = fb->_Xmax;
   if (rb->Name != 0) {
      params.y0 = fb->_Ymin;
      params.y1 = fb->_Ymax;
   } else {
      params.y0 = rb->Height - fb->_Ymax;
      params.y1 = rb->Height - fb->_Ymin;
   }

   memcpy(&params.wm_push_consts.dst_x0,
          ctx->Color.ClearColor.f, sizeof(float) * 4);

   brw_blorp_const_color_prog_key wm_prog_key;
   memset(&wm_prog_key, 0, sizeof(wm_prog_key));

   wm_prog_key.use_simd16_replicated_data = true;

   /* From the SNB PRM (Vol4_Part1):
    *
    *     "Replicated data (Message Type = 111) is only supported when
    *      accessing tiled memory.  Using this Message Type to access linear
    *      (untiled) memory is UNDEFINED."
    */
   if (irb->mt->tiling == I915_TILING_NONE)
      wm_prog_key.use_simd16_replicated_data = false;

   /* Constant color writes ignore everyting in blend and color calculator
    * state.  This is not documented.
    */
   if (set_write_disables(irb, ctx->Color.ColorMask[buf],
                          params.color_write_disable))
      wm_prog_key.use_simd16_replicated_data = false;

   if (irb->mt->fast_clear_state != INTEL_FAST_CLEAR_STATE_NO_MCS &&
       !partial_clear && wm_prog_key.use_simd16_replicated_data &&
       brw_is_color_fast_clear_compatible(brw, irb->mt,
                                          &ctx->Color.ClearColor)) {
      memset(&params.wm_push_consts, 0xff, 4*sizeof(float));
      params.fast_clear_op = GEN7_PS_RENDER_TARGET_FAST_CLEAR_ENABLE;

      brw_get_fast_clear_rect(brw, fb, irb->mt, &params.x0, &params.y0,
                              &params.x1, &params.y1);
   } else {
      brw_meta_get_buffer_rect(fb, &params.x0, &params.y0,
                               &params.x1, &params.y1);
   }

   brw_blorp_params_get_clear_kernel(brw, &params, &wm_prog_key);

   const bool is_fast_clear =
      params.fast_clear_op == GEN7_PS_RENDER_TARGET_FAST_CLEAR_ENABLE;
   if (is_fast_clear) {
      /* Record the clear color in the miptree so that it will be
       * programmed in SURFACE_STATE by later rendering and resolve
       * operations.
       */
      brw_meta_set_fast_clear_color(brw, irb->mt, &ctx->Color.ClearColor);

      /* If the buffer is already in INTEL_FAST_CLEAR_STATE_CLEAR, the clear
       * is redundant and can be skipped.
       */
      if (irb->mt->fast_clear_state == INTEL_FAST_CLEAR_STATE_CLEAR)
         return true;

      /* If the MCS buffer hasn't been allocated yet, we need to allocate
       * it now.
       */
      if (!irb->mt->mcs_mt) {
         if (!intel_miptree_alloc_non_msrt_mcs(brw, irb->mt)) {
            /* MCS allocation failed--probably this will only happen in
             * out-of-memory conditions.  But in any case, try to recover
             * by falling back to a non-blorp clear technique.
             */
            return false;
         }
      }
   }

   const char *clear_type;
   if (is_fast_clear)
      clear_type = "fast";
   else if (wm_prog_key.use_simd16_replicated_data)
      clear_type = "replicated";
   else
      clear_type = "slow";

   DBG("%s (%s) to mt %p level %d layer %d\n", __FUNCTION__, clear_type,
       irb->mt, irb->mt_level, irb->mt_layer);

   brw_blorp_exec(brw, &params);

   if (is_fast_clear) {
      /* Now that the fast clear has occurred, put the buffer in
       * INTEL_FAST_CLEAR_STATE_CLEAR so that we won't waste time doing
       * redundant clears.
       */
      irb->mt->fast_clear_state = INTEL_FAST_CLEAR_STATE_CLEAR;
   }

   return true;
}


extern "C" {
bool
brw_blorp_clear_color(struct brw_context *brw, struct gl_framebuffer *fb,
                      GLbitfield mask, bool partial_clear, bool encode_srgb)
{
   for (unsigned buf = 0; buf < fb->_NumColorDrawBuffers; buf++) {
      struct gl_renderbuffer *rb = fb->_ColorDrawBuffers[buf];
      struct intel_renderbuffer *irb = intel_renderbuffer(rb);

      /* Only clear the buffers present in the provided mask */
      if (((1 << fb->_ColorDrawBufferIndexes[buf]) & mask) == 0)
         continue;

      /* If this is an ES2 context or GL_ARB_ES2_compatibility is supported,
       * the framebuffer can be complete with some attachments missing.  In
       * this case the _ColorDrawBuffers pointer will be NULL.
       */
      if (rb == NULL)
         continue;

      if (fb->MaxNumLayers > 0) {
         unsigned layer_multiplier =
            (irb->mt->msaa_layout == INTEL_MSAA_LAYOUT_UMS ||
             irb->mt->msaa_layout == INTEL_MSAA_LAYOUT_CMS) ?
            irb->mt->num_samples : 1;
         unsigned num_layers = irb->layer_count;
         for (unsigned layer = 0; layer < num_layers; layer++) {
            if (!do_single_blorp_clear(
                    brw, fb, rb, buf, partial_clear, encode_srgb,
                    irb->mt_layer + layer * layer_multiplier)) {
               return false;
            }
         }
      } else {
         unsigned layer = irb->mt_layer;
         if (!do_single_blorp_clear(brw, fb, rb, buf, partial_clear,
                                    encode_srgb, layer))
            return false;
      }

      irb->need_downsample = true;
   }

   return true;
}

void
brw_blorp_resolve_color(struct brw_context *brw, struct intel_mipmap_tree *mt)
{
   DBG("%s to mt %p\n", __FUNCTION__, mt);

   const mesa_format format = _mesa_get_srgb_format_linear(mt->format);

   brw_blorp_params params;

   params.dst.set(brw, mt, 0 /* level */, 0 /* layer */, format, true);

   brw_get_resolve_rect(brw, mt, &params.x0, &params.y0,
                        &params.x1, &params.y1);

   params.fast_clear_op = GEN7_PS_RENDER_TARGET_RESOLVE_ENABLE;

   /* Note: there is no need to initialize push constants because it doesn't
    * matter what data gets dispatched to the render target.  However, we must
    * ensure that the fragment shader delivers the data using the "replicated
    * color" message.
    */
   brw_blorp_const_color_prog_key wm_prog_key;
   memset(&wm_prog_key, 0, sizeof(wm_prog_key));
   wm_prog_key.use_simd16_replicated_data = true;

   brw_blorp_params_get_clear_kernel(brw, &params, &wm_prog_key);

   brw_blorp_exec(brw, &params);
   mt->fast_clear_state = INTEL_FAST_CLEAR_STATE_RESOLVED;
}

} /* extern "C" */
