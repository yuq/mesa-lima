ir_function_signature *
umul64(void *mem_ctx, builtin_available_predicate avail)
{
   ir_function_signature *const sig =
      new(mem_ctx) ir_function_signature(glsl_type::uvec2_type, avail);
   ir_factory body(&sig->body, mem_ctx);
   sig->is_defined = true;

   exec_list sig_parameters;

   ir_variable *const r0001 = new(mem_ctx) ir_variable(glsl_type::uvec2_type, "a", ir_var_function_in);
   sig_parameters.push_tail(r0001);
   ir_variable *const r0002 = new(mem_ctx) ir_variable(glsl_type::uvec2_type, "b", ir_var_function_in);
   sig_parameters.push_tail(r0002);
   ir_variable *const r0003 = new(mem_ctx) ir_variable(glsl_type::uvec2_type, "result", ir_var_auto);
   body.emit(r0003);
   body.emit(assign(r0003, imul_high(swizzle_x(r0001), swizzle_x(r0002)), 0x02));

   body.emit(assign(r0003, mul(swizzle_x(r0001), swizzle_x(r0002)), 0x01));

   ir_expression *const r0004 = mul(swizzle_x(r0001), swizzle_y(r0002));
   ir_expression *const r0005 = mul(swizzle_y(r0001), swizzle_x(r0002));
   ir_expression *const r0006 = add(r0004, r0005);
   body.emit(assign(r0003, add(swizzle_y(r0003), r0006), 0x02));

   body.emit(ret(r0003));

   sig->replace_parameters(&sig_parameters);
   return sig;
}
