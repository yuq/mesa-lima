/* Compile with:
 *
 * glsl_compiler --version 140 --dump-builder int64.glsl > builtin_int64.h
 *
 * Using version 1.40+ prevents built-in variables from being included.
 */
#version 140
#extension GL_MESA_shader_integer_functions: require

uvec2
umul64(uvec2 a, uvec2 b)
{
   uvec2 result;

   umulExtended(a.x, b.x, result.y, result.x);
   result.y += a.x * b.y + a.y * b.x;

   return result;
}

ivec2
sign64(ivec2 a)
{
   ivec2 result;

   result.y = a.y >> 31;
   result.x = result.y | int((a.x | a.y) != 0);

   return result;
}
