/*
 * Copyright © 2012 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "mtypes.h"
#include "context.h"
#include "glformats.h"
#include "macros.h"
#include "enums.h"
#include "fbobject.h"
#include "formatquery.h"
#include "teximage.h"
#include "texparam.h"

static bool
_is_renderable(struct gl_context *ctx, GLenum internalformat)
{
   /*  Section 4.4.4 on page 212 of the  GLES 3.0.4 spec says:
    *
    *     "An internal format is color-renderable if it is one of the
    *     formats from table 3.13 noted as color-renderable or if it
    *     is unsized format RGBA or RGB."
    *
    * Therefore, we must accept GL_RGB and GL_RGBA here.
    */
   if (internalformat != GL_RGB && internalformat != GL_RGBA &&
       _mesa_base_fbo_format(ctx, internalformat) == 0)
      return false;

   return true;
}

/* Handles the cases where either ARB_internalformat_query or
 * ARB_internalformat_query2 have to return an error.
 */
static bool
_legal_parameters(struct gl_context *ctx, GLenum target, GLenum internalformat,
                  GLenum pname, GLsizei bufSize, GLint *params)

{
   bool query2 = _mesa_has_ARB_internalformat_query2(ctx);

   /* The ARB_internalformat_query2 spec says:
    *
    *    "The INVALID_ENUM error is generated if the <target> parameter to
    *    GetInternalformati*v is not one of the targets listed in Table 6.xx.
    */
   switch(target){
   case GL_TEXTURE_1D:
   case GL_TEXTURE_1D_ARRAY:
   case GL_TEXTURE_2D:
   case GL_TEXTURE_2D_ARRAY:
   case GL_TEXTURE_3D:
   case GL_TEXTURE_CUBE_MAP:
   case GL_TEXTURE_CUBE_MAP_ARRAY:
   case GL_TEXTURE_RECTANGLE:
   case GL_TEXTURE_BUFFER:
      if (!query2) {
         /* The ARB_internalformat_query spec says:
          *
          *     "If the <target> parameter to GetInternalformativ is not one of
          *      TEXTURE_2D_MULTISAMPLE, TEXTURE_2D_MULTISAMPLE_ARRAY
          *      or RENDERBUFFER then an INVALID_ENUM error is generated.
          */
         _mesa_error(ctx, GL_INVALID_ENUM,
                     "glGetInternalformativ(target=%s)",
                     _mesa_enum_to_string(target));

         return false;
      }
      break;

   case GL_RENDERBUFFER:
      break;

   case GL_TEXTURE_2D_MULTISAMPLE:
   case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
      /* The non-existence of ARB_texture_multisample is treated in
       * ARB_internalformat_query implementation like an error.
       */
      if (!query2 &&
          !(_mesa_has_ARB_texture_multisample(ctx) || _mesa_is_gles31(ctx))) {
         _mesa_error(ctx, GL_INVALID_ENUM,
                     "glGetInternalformativ(target=%s)",
                     _mesa_enum_to_string(target));

         return false;
      }
      break;

   default:
      _mesa_error(ctx, GL_INVALID_ENUM,
                  "glGetInternalformativ(target=%s)",
                  _mesa_enum_to_string(target));
      return false;
   }


   /* The ARB_internalformat_query2 spec says:
    *
    *     "The INVALID_ENUM error is generated if the <pname> parameter is
    *     not one of the listed possibilities.
    */
   switch(pname){
   case GL_SAMPLES:
   case GL_NUM_SAMPLE_COUNTS:
      break;

   case GL_SRGB_DECODE_ARB:
      /* The ARB_internalformat_query2 spec says:
       *
       *     "If ARB_texture_sRGB_decode or EXT_texture_sRGB_decode or
       *     equivalent functionality is not supported, queries for the
       *     SRGB_DECODE_ARB <pname> set the INVALID_ENUM error.
       */
      if (!_mesa_has_EXT_texture_sRGB_decode(ctx)) {
         _mesa_error(ctx, GL_INVALID_ENUM,
                     "glGetInternalformativ(pname=%s)",
                     _mesa_enum_to_string(pname));
         return false;
      }
      /* fallthrough */
   case GL_INTERNALFORMAT_SUPPORTED:
   case GL_INTERNALFORMAT_PREFERRED:
   case GL_INTERNALFORMAT_RED_SIZE:
   case GL_INTERNALFORMAT_GREEN_SIZE:
   case GL_INTERNALFORMAT_BLUE_SIZE:
   case GL_INTERNALFORMAT_ALPHA_SIZE:
   case GL_INTERNALFORMAT_DEPTH_SIZE:
   case GL_INTERNALFORMAT_STENCIL_SIZE:
   case GL_INTERNALFORMAT_SHARED_SIZE:
   case GL_INTERNALFORMAT_RED_TYPE:
   case GL_INTERNALFORMAT_GREEN_TYPE:
   case GL_INTERNALFORMAT_BLUE_TYPE:
   case GL_INTERNALFORMAT_ALPHA_TYPE:
   case GL_INTERNALFORMAT_DEPTH_TYPE:
   case GL_INTERNALFORMAT_STENCIL_TYPE:
   case GL_MAX_WIDTH:
   case GL_MAX_HEIGHT:
   case GL_MAX_DEPTH:
   case GL_MAX_LAYERS:
   case GL_MAX_COMBINED_DIMENSIONS:
   case GL_COLOR_COMPONENTS:
   case GL_DEPTH_COMPONENTS:
   case GL_STENCIL_COMPONENTS:
   case GL_COLOR_RENDERABLE:
   case GL_DEPTH_RENDERABLE:
   case GL_STENCIL_RENDERABLE:
   case GL_FRAMEBUFFER_RENDERABLE:
   case GL_FRAMEBUFFER_RENDERABLE_LAYERED:
   case GL_FRAMEBUFFER_BLEND:
   case GL_READ_PIXELS:
   case GL_READ_PIXELS_FORMAT:
   case GL_READ_PIXELS_TYPE:
   case GL_TEXTURE_IMAGE_FORMAT:
   case GL_TEXTURE_IMAGE_TYPE:
   case GL_GET_TEXTURE_IMAGE_FORMAT:
   case GL_GET_TEXTURE_IMAGE_TYPE:
   case GL_MIPMAP:
   case GL_MANUAL_GENERATE_MIPMAP:
   case GL_AUTO_GENERATE_MIPMAP:
   case GL_COLOR_ENCODING:
   case GL_SRGB_READ:
   case GL_SRGB_WRITE:
   case GL_FILTER:
   case GL_VERTEX_TEXTURE:
   case GL_TESS_CONTROL_TEXTURE:
   case GL_TESS_EVALUATION_TEXTURE:
   case GL_GEOMETRY_TEXTURE:
   case GL_FRAGMENT_TEXTURE:
   case GL_COMPUTE_TEXTURE:
   case GL_TEXTURE_SHADOW:
   case GL_TEXTURE_GATHER:
   case GL_TEXTURE_GATHER_SHADOW:
   case GL_SHADER_IMAGE_LOAD:
   case GL_SHADER_IMAGE_STORE:
   case GL_SHADER_IMAGE_ATOMIC:
   case GL_IMAGE_TEXEL_SIZE:
   case GL_IMAGE_COMPATIBILITY_CLASS:
   case GL_IMAGE_PIXEL_FORMAT:
   case GL_IMAGE_PIXEL_TYPE:
   case GL_IMAGE_FORMAT_COMPATIBILITY_TYPE:
   case GL_SIMULTANEOUS_TEXTURE_AND_DEPTH_TEST:
   case GL_SIMULTANEOUS_TEXTURE_AND_STENCIL_TEST:
   case GL_SIMULTANEOUS_TEXTURE_AND_DEPTH_WRITE:
   case GL_SIMULTANEOUS_TEXTURE_AND_STENCIL_WRITE:
   case GL_TEXTURE_COMPRESSED:
   case GL_TEXTURE_COMPRESSED_BLOCK_WIDTH:
   case GL_TEXTURE_COMPRESSED_BLOCK_HEIGHT:
   case GL_TEXTURE_COMPRESSED_BLOCK_SIZE:
   case GL_CLEAR_BUFFER:
   case GL_TEXTURE_VIEW:
   case GL_VIEW_COMPATIBILITY_CLASS:
      /* The ARB_internalformat_query spec says:
       *
       *     "If the <pname> parameter to GetInternalformativ is not SAMPLES
       *     or NUM_SAMPLE_COUNTS, then an INVALID_ENUM error is generated."
       */
      if (!query2) {
         _mesa_error(ctx, GL_INVALID_ENUM,
                     "glGetInternalformativ(pname=%s)",
                     _mesa_enum_to_string(pname));

         return false;
      }
      break;

   default:
      _mesa_error(ctx, GL_INVALID_ENUM,
                  "glGetInternalformativ(pname=%s)",
                  _mesa_enum_to_string(pname));
      return false;
   }

   /* The ARB_internalformat_query spec says:
    *
    *     "If the <bufSize> parameter to GetInternalformativ is negative, then
    *     an INVALID_VALUE error is generated."
    *
    * Nothing is said in ARB_internalformat_query2 but we assume the same.
    */
   if (bufSize < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetInternalformativ(target=%s)",
                  _mesa_enum_to_string(target));
      return false;
   }

   /* The ARB_internalformat_query spec says:
    *
    *     "If the <internalformat> parameter to GetInternalformativ is not
    *     color-, depth- or stencil-renderable, then an INVALID_ENUM error is
    *     generated."
    */
   if (!query2 && !_is_renderable(ctx, internalformat)) {
      _mesa_error(ctx, GL_INVALID_ENUM,
                  "glGetInternalformativ(internalformat=%s)",
                  _mesa_enum_to_string(internalformat));
      return false;
   }

   return true;
}

/* Sets the appropriate "unsupported" response as defined by the
 * ARB_internalformat_query2 spec for each each <pname>.
 */
static void
_set_default_response(GLenum pname, GLint buffer[16])
{
   /* The ARB_internalformat_query2 defines which is the reponse best
    * representing "not supported" or "not applicable" for each <pname>.
    *
    *     " In general:
    *          - size- or count-based queries will return zero,
    *          - support-, format- or type-based queries will return NONE,
    *          - boolean-based queries will return FALSE, and
    *          - list-based queries return no entries."
    */
   switch(pname) {
   case GL_SAMPLES:
      break;

   case GL_MAX_COMBINED_DIMENSIONS:
   case GL_NUM_SAMPLE_COUNTS:
   case GL_INTERNALFORMAT_RED_SIZE:
   case GL_INTERNALFORMAT_GREEN_SIZE:
   case GL_INTERNALFORMAT_BLUE_SIZE:
   case GL_INTERNALFORMAT_ALPHA_SIZE:
   case GL_INTERNALFORMAT_DEPTH_SIZE:
   case GL_INTERNALFORMAT_STENCIL_SIZE:
   case GL_INTERNALFORMAT_SHARED_SIZE:
   case GL_MAX_WIDTH:
   case GL_MAX_HEIGHT:
   case GL_MAX_DEPTH:
   case GL_MAX_LAYERS:
   case GL_IMAGE_TEXEL_SIZE:
   case GL_TEXTURE_COMPRESSED_BLOCK_WIDTH:
   case GL_TEXTURE_COMPRESSED_BLOCK_HEIGHT:
   case GL_TEXTURE_COMPRESSED_BLOCK_SIZE:
      buffer[0] = 0;
      break;

   case GL_INTERNALFORMAT_PREFERRED:
   case GL_INTERNALFORMAT_RED_TYPE:
   case GL_INTERNALFORMAT_GREEN_TYPE:
   case GL_INTERNALFORMAT_BLUE_TYPE:
   case GL_INTERNALFORMAT_ALPHA_TYPE:
   case GL_INTERNALFORMAT_DEPTH_TYPE:
   case GL_INTERNALFORMAT_STENCIL_TYPE:
   case GL_FRAMEBUFFER_RENDERABLE:
   case GL_FRAMEBUFFER_RENDERABLE_LAYERED:
   case GL_FRAMEBUFFER_BLEND:
   case GL_READ_PIXELS:
   case GL_READ_PIXELS_FORMAT:
   case GL_READ_PIXELS_TYPE:
   case GL_TEXTURE_IMAGE_FORMAT:
   case GL_TEXTURE_IMAGE_TYPE:
   case GL_GET_TEXTURE_IMAGE_FORMAT:
   case GL_GET_TEXTURE_IMAGE_TYPE:
   case GL_MANUAL_GENERATE_MIPMAP:
   case GL_AUTO_GENERATE_MIPMAP:
   case GL_COLOR_ENCODING:
   case GL_SRGB_READ:
   case GL_SRGB_WRITE:
   case GL_SRGB_DECODE_ARB:
   case GL_FILTER:
   case GL_VERTEX_TEXTURE:
   case GL_TESS_CONTROL_TEXTURE:
   case GL_TESS_EVALUATION_TEXTURE:
   case GL_GEOMETRY_TEXTURE:
   case GL_FRAGMENT_TEXTURE:
   case GL_COMPUTE_TEXTURE:
   case GL_TEXTURE_SHADOW:
   case GL_TEXTURE_GATHER:
   case GL_TEXTURE_GATHER_SHADOW:
   case GL_SHADER_IMAGE_LOAD:
   case GL_SHADER_IMAGE_STORE:
   case GL_SHADER_IMAGE_ATOMIC:
   case GL_IMAGE_COMPATIBILITY_CLASS:
   case GL_IMAGE_PIXEL_FORMAT:
   case GL_IMAGE_PIXEL_TYPE:
   case GL_IMAGE_FORMAT_COMPATIBILITY_TYPE:
   case GL_SIMULTANEOUS_TEXTURE_AND_DEPTH_TEST:
   case GL_SIMULTANEOUS_TEXTURE_AND_STENCIL_TEST:
   case GL_SIMULTANEOUS_TEXTURE_AND_DEPTH_WRITE:
   case GL_SIMULTANEOUS_TEXTURE_AND_STENCIL_WRITE:
   case GL_CLEAR_BUFFER:
   case GL_TEXTURE_VIEW:
   case GL_VIEW_COMPATIBILITY_CLASS:
      buffer[0] = GL_NONE;
      break;

   case GL_INTERNALFORMAT_SUPPORTED:
   case GL_COLOR_COMPONENTS:
   case GL_DEPTH_COMPONENTS:
   case GL_STENCIL_COMPONENTS:
   case GL_COLOR_RENDERABLE:
   case GL_DEPTH_RENDERABLE:
   case GL_STENCIL_RENDERABLE:
   case GL_MIPMAP:
   case GL_TEXTURE_COMPRESSED:
      buffer[0] = GL_FALSE;
      break;

   default:
      unreachable("invalid 'pname'");
   }
}

static bool
_is_target_supported(struct gl_context *ctx, GLenum target)
{
   /* The ARB_internalformat_query2 spec says:
    *
    *     "if a particular type of <target> is not supported by the
    *     implementation the "unsupported" answer should be given.
    *     This is not an error."
    */
   switch(target){
   case GL_TEXTURE_2D:
   case GL_TEXTURE_3D:
      break;

   case GL_TEXTURE_1D:
      if (!_mesa_is_desktop_gl(ctx))
         return false;
      break;

   case GL_TEXTURE_1D_ARRAY:
      if (!_mesa_has_EXT_texture_array(ctx))
         return false;
      break;

   case GL_TEXTURE_2D_ARRAY:
      if (!(_mesa_has_EXT_texture_array(ctx) || _mesa_is_gles3(ctx)))
         return false;
      break;

   case GL_TEXTURE_CUBE_MAP:
      if (!_mesa_has_ARB_texture_cube_map(ctx))
         return false;
      break;

   case GL_TEXTURE_CUBE_MAP_ARRAY:
      if (!_mesa_has_ARB_texture_cube_map_array(ctx))
         return false;
      break;

   case GL_TEXTURE_RECTANGLE:
      if (!_mesa_has_NV_texture_rectangle(ctx))
          return false;
      break;

   case GL_TEXTURE_BUFFER:
      if (!_mesa_has_ARB_texture_buffer_object(ctx))
         return false;
      break;

   case GL_RENDERBUFFER:
      if (!(_mesa_has_ARB_framebuffer_object(ctx) ||
            _mesa_is_gles3(ctx)))
         return false;
      break;

   case GL_TEXTURE_2D_MULTISAMPLE:
   case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
      if (!(_mesa_has_ARB_texture_multisample(ctx) ||
            _mesa_is_gles31(ctx)))
         return false;
      break;

   default:
      unreachable("invalid target");
   }

   return true;
}

static bool
_is_resource_supported(struct gl_context *ctx, GLenum target,
                       GLenum internalformat, GLenum pname)
{
   /* From the ARB_internalformat_query2 spec:
    *
    * In the following descriptions, the term /resource/ is used to generically
    * refer to an object of the appropriate type that has been created with
    * <internalformat> and <target>.  If the particular <target> and
    * <internalformat> combination do not make sense, ... the "unsupported"
    * answer should be given. This is not an error.
    */

   /* In the ARB_internalformat_query2 spec wording, some <pnames> do not care
    * about the /resource/ being supported or not, we return 'true' for those.
    */
   switch (pname) {
   case GL_INTERNALFORMAT_SUPPORTED:
   case GL_INTERNALFORMAT_PREFERRED:
   case GL_COLOR_COMPONENTS:
   case GL_DEPTH_COMPONENTS:
   case GL_STENCIL_COMPONENTS:
   case GL_COLOR_RENDERABLE:
   case GL_DEPTH_RENDERABLE:
   case GL_STENCIL_RENDERABLE:
      return true;
   default:
      break;
   }

   switch(target){
   case GL_TEXTURE_1D:
   case GL_TEXTURE_1D_ARRAY:
   case GL_TEXTURE_2D:
   case GL_TEXTURE_2D_ARRAY:
   case GL_TEXTURE_3D:
   case GL_TEXTURE_CUBE_MAP:
   case GL_TEXTURE_CUBE_MAP_ARRAY:
   case GL_TEXTURE_RECTANGLE:
      /* Based on what Mesa does for glTexImage1D/2D/3D and
       * glCompressedTexImage1D/2D/3D functions.
       */
      if (_mesa_base_tex_format(ctx, internalformat) < 0)
         return false;

      /* additional checks for depth textures */
      if (!_mesa_legal_texture_base_format_for_target(ctx, target, internalformat))
         return false;

      /* additional checks for compressed textures */
      if (_mesa_is_compressed_format(ctx, internalformat) &&
          (!_mesa_target_can_be_compressed(ctx, target, internalformat, NULL) ||
           _mesa_format_no_online_compression(ctx, internalformat)))
         return false;

      break;
   case GL_TEXTURE_2D_MULTISAMPLE:
   case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
      /* Based on what Mesa does for glTexImage2D/3DMultisample,
       * glTexStorage2D/3DMultisample and
       * glTextureStorage2D/3DMultisample functions.
       */
      if (!_mesa_is_renderable_texture_format(ctx, internalformat))
         return false;

      break;
   case GL_TEXTURE_BUFFER:
      /* Based on what Mesa does for the glTexBuffer function. */
      if (_mesa_validate_texbuffer_format(ctx, internalformat) ==
          MESA_FORMAT_NONE)
         return false;

      break;
   case GL_RENDERBUFFER:
      /* Based on what Mesa does for glRenderbufferStorage(Multisample) and
       * glNamedRenderbufferStorage functions.
       */
      if (!_mesa_base_fbo_format(ctx, internalformat))
         return false;

      break;
   default:
      unreachable("bad target");
   }

   return true;
}

static bool
_is_internalformat_supported(struct gl_context *ctx, GLenum target,
                             GLenum internalformat)
{
   /* From the ARB_internalformat_query2 specification:
    *
    *     "- INTERNALFORMAT_SUPPORTED: If <internalformat> is an internal format
    *     that is supported by the implementation in at least some subset of
    *     possible operations, TRUE is written to <params>.  If <internalformat>
    *     if not a valid token for any internal format usage, FALSE is returned.
    *
    *     <internalformats> that must be supported (in GL 4.2 or later) include
    *      the following:
    *         - "sized internal formats" from Table 3.12, 3.13, and 3.15,
    *         - any specific "compressed internal format" from Table 3.14,
    *         - any "image unit format" from Table 3.21.
    *         - any generic "compressed internal format" from Table 3.14, if the
    *         implementation accepts it for any texture specification commands, and
    *         - unsized or base internal format, if the implementation accepts
    *         it for texture or image specification.
    */
   GLint buffer[1];

   /* At this point a internalformat is valid if it is valid as a texture or
    * as a renderbuffer format. The checks are different because those methods
    * return different values when passing non supported internalformats */
   if (_mesa_base_tex_format(ctx, internalformat) < 0 &&
       _mesa_base_fbo_format(ctx, internalformat) == 0)
      return false;

   /* Let the driver have the final word */
   ctx->Driver.QueryInternalFormat(ctx, target, internalformat,
                                   GL_INTERNALFORMAT_SUPPORTED, buffer);

   return (buffer[0] == GL_TRUE);
}

/* default implementation of QueryInternalFormat driverfunc, for
 * drivers not implementing ARB_internalformat_query2.
 */
void
_mesa_query_internal_format_default(struct gl_context *ctx, GLenum target,
                                    GLenum internalFormat, GLenum pname,
                                    GLint *params)
{
   (void) ctx;
   (void) target;
   (void) internalFormat;

   switch (pname) {
   case GL_SAMPLES:
   case GL_NUM_SAMPLE_COUNTS:
      params[0] = 1;
      break;

   case GL_INTERNALFORMAT_SUPPORTED:
      params[0] = GL_TRUE;
      break;

   case GL_INTERNALFORMAT_PREFERRED:
      params[0] = internalFormat;
      break;

   default:
      _set_default_response(pname, params);
      break;
   }
}

void GLAPIENTRY
_mesa_GetInternalformativ(GLenum target, GLenum internalformat, GLenum pname,
                          GLsizei bufSize, GLint *params)
{
   GLint buffer[16];
   GET_CURRENT_CONTEXT(ctx);

   ASSERT_OUTSIDE_BEGIN_END(ctx);

   /* ARB_internalformat_query is also mandatory for ARB_internalformat_query2 */
   if (!(_mesa_has_ARB_internalformat_query(ctx) ||
         _mesa_is_gles3(ctx))) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "glGetInternalformativ");
      return;
   }

   assert(ctx->Driver.QueryInternalFormat != NULL);

   if (!_legal_parameters(ctx, target, internalformat, pname, bufSize, params))
      return;

   /* initialize the contents of the temporary buffer */
   memcpy(buffer, params, MIN2(bufSize, 16) * sizeof(GLint));

   /* Use the 'unsupported' response defined by the spec for every pname
    * as the default answer.
    */
   _set_default_response(pname, buffer);

   if (!_is_target_supported(ctx, target) ||
       !_is_internalformat_supported(ctx, target, internalformat) ||
       !_is_resource_supported(ctx, target, internalformat, pname))
      goto end;

   switch (pname) {
   case GL_SAMPLES:
      /* fall-through */
   case GL_NUM_SAMPLE_COUNTS:
      /* The ARB_internalformat_query2 sets the response as 'unsupported' for
       * SAMPLES and NUM_SAMPLE_COUNTS:
       *
       *     "If <internalformat> is not color-renderable, depth-renderable, or
       *     stencil-renderable (as defined in section 4.4.4), or if <target>
       *     does not support multiple samples (ie other than
       *     TEXTURE_2D_MULTISAMPLE,  TEXTURE_2D_MULTISAMPLE_ARRAY,
       *     or RENDERBUFFER)."
       */
      if ((target != GL_RENDERBUFFER &&
           target != GL_TEXTURE_2D_MULTISAMPLE &&
           target != GL_TEXTURE_2D_MULTISAMPLE_ARRAY) ||
          !_is_renderable(ctx, internalformat))
         goto end;

      /* The GL ES 3.0 specification, section 6.1.15 page 236 says:
       *
       *     "Since multisampling is not supported for signed and unsigned
       *     integer internal formats, the value of NUM_SAMPLE_COUNTS will be
       *     zero for such formats.
       */
      if (pname == GL_NUM_SAMPLE_COUNTS && ctx->API == API_OPENGLES2 &&
          ctx->Version == 30 && _mesa_is_enum_format_integer(internalformat)) {
         goto end;
      }

      ctx->Driver.QueryInternalFormat(ctx, target, internalformat, pname,
                                      buffer);
      break;

   case GL_INTERNALFORMAT_SUPPORTED:
      /* Having a supported <internalformat> is implemented as a prerequisite
       * for all the <pnames>. Thus,  if we reach this point, the internalformat is
       * supported.
       */
      buffer[0] = GL_TRUE;
      break;

   case GL_INTERNALFORMAT_PREFERRED:
      /* The ARB_internalformat_query2 spec says:
       *
       *     "- INTERNALFORMAT_PREFERRED: The implementation-preferred internal
       *     format for representing resources of the specified <internalformat> is
       *     returned in <params>.
       *
       * Therefore, we let the driver answer.
       */
      ctx->Driver.QueryInternalFormat(ctx, target, internalformat, pname,
                                      buffer);
      break;

   case GL_INTERNALFORMAT_RED_SIZE:
   case GL_INTERNALFORMAT_GREEN_SIZE:
   case GL_INTERNALFORMAT_BLUE_SIZE:
   case GL_INTERNALFORMAT_ALPHA_SIZE:
   case GL_INTERNALFORMAT_DEPTH_SIZE:
   case GL_INTERNALFORMAT_STENCIL_SIZE:
   case GL_INTERNALFORMAT_SHARED_SIZE:
   case GL_INTERNALFORMAT_RED_TYPE:
   case GL_INTERNALFORMAT_GREEN_TYPE:
   case GL_INTERNALFORMAT_BLUE_TYPE:
   case GL_INTERNALFORMAT_ALPHA_TYPE:
   case GL_INTERNALFORMAT_DEPTH_TYPE:
   case GL_INTERNALFORMAT_STENCIL_TYPE: {
      GLint baseformat;
      mesa_format texformat;

      if (target != GL_RENDERBUFFER) {
         if (!_mesa_legal_get_tex_level_parameter_target(ctx, target, true))
            goto end;

         baseformat = _mesa_base_tex_format(ctx, internalformat);
      } else {
         baseformat = _mesa_base_fbo_format(ctx, internalformat);
      }

      /* Let the driver choose the texture format.
       *
       * Disclaimer: I am considering that drivers use for renderbuffers the
       * same format-choice logic as for textures.
       */
      texformat = ctx->Driver.ChooseTextureFormat(ctx, target, internalformat,
                                                  GL_NONE /*format */, GL_NONE /* type */);

      if (texformat == MESA_FORMAT_NONE || baseformat <= 0)
         goto end;

      /* Implementation based on what Mesa does for glGetTexLevelParameteriv
       * and glGetRenderbufferParameteriv functions.
       */
      if (pname == GL_INTERNALFORMAT_SHARED_SIZE) {
         if (_mesa_has_EXT_texture_shared_exponent(ctx) &&
             target != GL_TEXTURE_BUFFER &&
             target != GL_RENDERBUFFER &&
             texformat == MESA_FORMAT_R9G9B9E5_FLOAT) {
            buffer[0] = 5;
         }
         goto end;
      }

      if (!_mesa_base_format_has_channel(baseformat, pname))
         goto end;

      switch (pname) {
      case GL_INTERNALFORMAT_DEPTH_SIZE:
         if (!_mesa_has_ARB_depth_texture(ctx) &&
             target != GL_RENDERBUFFER &&
             target != GL_TEXTURE_BUFFER)
            goto end;
         /* fallthrough */
      case GL_INTERNALFORMAT_RED_SIZE:
      case GL_INTERNALFORMAT_GREEN_SIZE:
      case GL_INTERNALFORMAT_BLUE_SIZE:
      case GL_INTERNALFORMAT_ALPHA_SIZE:
      case GL_INTERNALFORMAT_STENCIL_SIZE:
         buffer[0] = _mesa_get_format_bits(texformat, pname);
         break;

      case GL_INTERNALFORMAT_DEPTH_TYPE:
         if (!_mesa_has_ARB_texture_float(ctx))
            goto end;
         /* fallthrough */
      case GL_INTERNALFORMAT_RED_TYPE:
      case GL_INTERNALFORMAT_GREEN_TYPE:
      case GL_INTERNALFORMAT_BLUE_TYPE:
      case GL_INTERNALFORMAT_ALPHA_TYPE:
      case GL_INTERNALFORMAT_STENCIL_TYPE:
         buffer[0]  = _mesa_get_format_datatype(texformat);
         break;

      default:
         break;

      }
      break;
   }

   case GL_MAX_WIDTH:
      /* @TODO */
      break;

   case GL_MAX_HEIGHT:
      /* @TODO */
      break;

   case GL_MAX_DEPTH:
      /* @TODO */
      break;

   case GL_MAX_LAYERS:
      /* @TODO */
      break;

   case GL_MAX_COMBINED_DIMENSIONS:
      /* @TODO */
      break;

   case GL_COLOR_COMPONENTS:
      /* @TODO */
      break;

   case GL_DEPTH_COMPONENTS:
      /* @TODO */
      break;

   case GL_STENCIL_COMPONENTS:
      /* @TODO */
      break;

   case GL_COLOR_RENDERABLE:
      /* @TODO */
      break;

   case GL_DEPTH_RENDERABLE:
      /* @TODO */
      break;

   case GL_STENCIL_RENDERABLE:
      /* @TODO */
      break;

   case GL_FRAMEBUFFER_RENDERABLE:
      /* @TODO */
      break;

   case GL_FRAMEBUFFER_RENDERABLE_LAYERED:
      /* @TODO */
      break;

   case GL_FRAMEBUFFER_BLEND:
      /* @TODO */
      break;

   case GL_READ_PIXELS:
      /* @TODO */
      break;

   case GL_READ_PIXELS_FORMAT:
      /* @TODO */
      break;

   case GL_READ_PIXELS_TYPE:
      /* @TODO */
      break;

   case GL_TEXTURE_IMAGE_FORMAT:
      /* @TODO */
      break;

   case GL_TEXTURE_IMAGE_TYPE:
      /* @TODO */
      break;

   case GL_GET_TEXTURE_IMAGE_FORMAT:
      /* @TODO */
      break;

   case GL_GET_TEXTURE_IMAGE_TYPE:
      /* @TODO */
      break;

   case GL_MIPMAP:
      /* @TODO */
      break;

   case GL_MANUAL_GENERATE_MIPMAP:
      /* @TODO */
      break;

   case GL_AUTO_GENERATE_MIPMAP:
      /* @TODO */
      break;

   case GL_COLOR_ENCODING:
      /* @TODO */
      break;

   case GL_SRGB_READ:
      /* @TODO */
      break;

   case GL_SRGB_WRITE:
      /* @TODO */
      break;

   case GL_SRGB_DECODE_ARB:
      /* @TODO */
      break;

   case GL_FILTER:
      /* @TODO */
      break;

   case GL_VERTEX_TEXTURE:
      /* @TODO */
      break;

   case GL_TESS_CONTROL_TEXTURE:
      /* @TODO */
      break;

   case GL_TESS_EVALUATION_TEXTURE:
      /* @TODO */
      break;

   case GL_GEOMETRY_TEXTURE:
      /* @TODO */
      break;

   case GL_FRAGMENT_TEXTURE:
      /* @TODO */
      break;

   case GL_COMPUTE_TEXTURE:
      /* @TODO */
      break;

   case GL_TEXTURE_SHADOW:
      /* @TODO */
      break;

   case GL_TEXTURE_GATHER:
      /* @TODO */
      break;

   case GL_TEXTURE_GATHER_SHADOW:
      /* @TODO */
      break;

   case GL_SHADER_IMAGE_LOAD:
      /* @TODO */
      break;

   case GL_SHADER_IMAGE_STORE:
      /* @TODO */
      break;

   case GL_SHADER_IMAGE_ATOMIC:
      /* @TODO */
      break;

   case GL_IMAGE_TEXEL_SIZE:
      /* @TODO */
      break;

   case GL_IMAGE_COMPATIBILITY_CLASS:
      /* @TODO */
      break;

   case GL_IMAGE_PIXEL_FORMAT:
      /* @TODO */
      break;

   case GL_IMAGE_PIXEL_TYPE:
      /* @TODO */
      break;

   case GL_IMAGE_FORMAT_COMPATIBILITY_TYPE:
      /* @TODO */
      break;

   case GL_SIMULTANEOUS_TEXTURE_AND_DEPTH_TEST:
      /* @TODO */
      break;

   case GL_SIMULTANEOUS_TEXTURE_AND_STENCIL_TEST:
      /* @TODO */
      break;

   case GL_SIMULTANEOUS_TEXTURE_AND_DEPTH_WRITE:
      /* @TODO */
      break;

   case GL_SIMULTANEOUS_TEXTURE_AND_STENCIL_WRITE:
      /* @TODO */
      break;

   case GL_TEXTURE_COMPRESSED:
      /* @TODO */
      break;

   case GL_TEXTURE_COMPRESSED_BLOCK_WIDTH:
      /* @TODO */
      break;

   case GL_TEXTURE_COMPRESSED_BLOCK_HEIGHT:
      /* @TODO */
      break;

   case GL_TEXTURE_COMPRESSED_BLOCK_SIZE:
      /* @TODO */
      break;

   case GL_CLEAR_BUFFER:
      /* @TODO */
      break;

   case GL_TEXTURE_VIEW:
      /* @TODO */
      break;

   case GL_VIEW_COMPATIBILITY_CLASS:
      /* @TODO */
      break;

   default:
      unreachable("bad param");
   }

 end:
   if (bufSize != 0 && params == NULL) {
      /* Emit a warning to aid application debugging, but go ahead and do the
       * memcpy (and probably crash) anyway.
       */
      _mesa_warning(ctx,
                    "glGetInternalformativ(bufSize = %d, but params = NULL)",
                    bufSize);
   }

   /* Copy the data from the temporary buffer to the buffer supplied by the
    * application.  Clamp the size of the copy to the size supplied by the
    * application.
    */
   memcpy(params, buffer, MIN2(bufSize, 16) * sizeof(GLint));

   return;
}

void GLAPIENTRY
_mesa_GetInternalformati64v(GLenum target, GLenum internalformat,
                            GLenum pname, GLsizei bufSize, GLint64 *params)
{
   GLint params32[16];
   unsigned i;

   GET_CURRENT_CONTEXT(ctx);

   ASSERT_OUTSIDE_BEGIN_END(ctx);

   if (!_mesa_has_ARB_internalformat_query2(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "glGetInternalformati64v");
      return;
   }

   _mesa_GetInternalformativ(target, internalformat, pname, bufSize, params32);

   for (i = 0; i < bufSize; i++)
      params[i] = params32[i];
}
