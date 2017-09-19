/*
 * Copyright © 2010 Intel Corporation
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

#include <limits.h>
#include "main/compiler.h"
#include "compiler/glsl_types.h"
#include "loop_analysis.h"
#include "ir_hierarchical_visitor.h"


namespace {

class loop_control_visitor : public ir_hierarchical_visitor {
public:
   loop_control_visitor(loop_state *state)
   {
      this->state = state;
      this->progress = false;
   }

   virtual ir_visitor_status visit_leave(ir_loop *ir);

   loop_state *state;

   bool progress;
};

} /* anonymous namespace */

ir_visitor_status
loop_control_visitor::visit_leave(ir_loop *ir)
{
   loop_variable_state *const ls = this->state->get(ir);

   /* If we've entered a loop that hasn't been analyzed, something really,
    * really bad has happened.
    */
   if (ls == NULL) {
      assert(ls != NULL);
      return visit_continue;
   }

   if (ls->limiting_terminator != NULL) {
      /* If the limiting terminator has an iteration count of zero, then we've
       * proven that the loop cannot run, so delete it.
       */
      int iterations = ls->limiting_terminator->iterations;
      if (iterations == 0) {
         ir->remove();
         this->progress = true;
         return visit_continue;
      }
   }

   /* Remove the conditional break statements associated with all terminators
    * that are associated with a fixed iteration count, except for the one
    * associated with the limiting terminator--that one needs to stay, since
    * it terminates the loop.  Exception: if the loop still has a normative
    * bound, then that terminates the loop, so we don't even need the limiting
    * terminator.
    */
   foreach_in_list(loop_terminator, t, &ls->terminators) {
      if (t->iterations < 0)
         continue;

      if (t != ls->limiting_terminator) {
         t->ir->remove();

         assert(ls->num_loop_jumps > 0);
         ls->num_loop_jumps--;

         this->progress = true;
      }
   }

   return visit_continue;
}


bool
set_loop_controls(exec_list *instructions, loop_state *ls)
{
   loop_control_visitor v(ls);

   v.run(instructions);

   return v.progress;
}
