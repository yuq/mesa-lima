/*
 * Copyright © 2017 Gert Wollny
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

#include <tgsi/tgsi_ureg.h>
#include <tgsi/tgsi_info.h>
#include <mesa/program/prog_instruction.h>

#include <gtest/gtest.h>
#include <utility>
#include <algorithm>
#include <iostream>

#include "st_tests_common.h"

using std::vector;
using std::pair;
using std::make_pair;
using std::transform;
using std::copy;


TEST_F(LifetimeEvaluatorExactTest, SimpleMoveAdd)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_UADD, {out0}, {1,in0}, {}},
      { TGSI_OPCODE_END}
   };
   run(code, temp_lt_expect({{-1,-1}, {0,1}}));
}

TEST_F(LifetimeEvaluatorExactTest, SimpleMoveAddMove)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_UADD, {2}, {1,in0}, {}},
      { TGSI_OPCODE_MOV, {out0}, {2}, {}},
      { TGSI_OPCODE_END}
   };
   run(code, temp_lt_expect({{-1, -1}, {0,1}, {1,2}}));
}

/* Test whether the texoffst are actually visited by the
 * merge algorithm. Note that it is of no importance
 * what instruction is actually used, the MockShader class
 * does not consider the details of the operation, only
 * the number of arguments is of importance.
 */
TEST_F(LifetimeEvaluatorExactTest, SimpleOpWithTexoffset)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_MOV, {2}, {in1}, {}},
      { TGSI_OPCODE_TEX, {out0}, {in0}, {1,2}},
      { TGSI_OPCODE_END}
   };
   run(code, temp_lt_expect({{-1, -1}, {0,2}, {1,2}}));
}

/* Simple register access involving a loop
 * 1: must life up to then end of the loop
 * 2: only needs to life from write to read
 * 3: only needs to life from write to read outside the loop
 */
TEST_F(LifetimeEvaluatorExactTest, SimpleMoveInLoop)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_UADD, {2}, {1,in0}, {}},
      {   TGSI_OPCODE_UADD, {3}, {1,2}, {}},
      {   TGSI_OPCODE_UADD, {3}, {3,in1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {3}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,5}, {2,3}, {3,6}}));
}

/* In loop if/else value written only in one path, and read later
 * - value must survive the whole loop.
 */
TEST_F(LifetimeEvaluatorExactTest, MoveInIfInLoop)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {in1}, {}},
      {     TGSI_OPCODE_UADD, {2}, {1,in0}, {}},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_UADD, {3}, {1,2}, {}},
      {   TGSI_OPCODE_UADD, {3}, {3,in1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {3}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,7}, {1,7}, {5,8}}));
}

/* A non-dominant write within an IF can be ignored (if it is read
 * later)
 */
TEST_F(LifetimeEvaluatorExactTest, NonDominantWriteinIfInLoop)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {   TGSI_OPCODE_IF, {}, {in1}, {}},
      {     TGSI_OPCODE_MOV, {1}, {in1}, {}},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_UADD, {2}, {1,in1}, {}},
      {   TGSI_OPCODE_IF, {}, {2}, {}},
      {     TGSI_OPCODE_BRK},
      {   TGSI_OPCODE_ENDIF},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {2}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {1,5}, {5,10}}));
}

/* In Nested loop if/else value written only in one path, and read later
 * - value must survive the outer loop.
 */
TEST_F(LifetimeEvaluatorExactTest, MoveInIfInNestedLoop)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_BGNLOOP },
      {     TGSI_OPCODE_IF, {}, {in1}, {} },
      {       TGSI_OPCODE_UADD, {2}, {1,in0}, {}},
      {     TGSI_OPCODE_ENDIF},
      {     TGSI_OPCODE_UADD, {3}, {1,2}, {}},
      {   TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {3}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,8}, {1,8}, {6,9}}));
}

/* In loop if/else value written in both path, and read later
 * - value must survive from first write to last read in loop
 * for now we only check that the minimum life time is correct.
 */
TEST_F(LifetimeEvaluatorAtLeastTest, WriteInIfAndElseInLoop)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {1}, {}},
      {     TGSI_OPCODE_UADD, {2}, {1,in0}, {}},
      {   TGSI_OPCODE_ELSE },
      {     TGSI_OPCODE_MOV, {2}, {1}, {}},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_UADD, {3}, {1,2}, {}},
      {   TGSI_OPCODE_UADD, {3}, {3,in1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {3}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,9}, {3,7}, {7,10}}));
}

/* In loop if/else value written in both path, read in else path
 * before write and also read later
 * - value must survive the whole loop
 */
TEST_F(LifetimeEvaluatorExactTest, WriteInIfAndElseReadInElseInLoop)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {1}, {}},
      {     TGSI_OPCODE_UADD, {2}, {1,in0}, {}},
      {   TGSI_OPCODE_ELSE },
      {     TGSI_OPCODE_ADD, {2}, {1,2}, {}},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_UADD, {3}, {1,2}, {}},
      {   TGSI_OPCODE_UADD, {3}, {3,in1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {3}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,9}, {1,9}, {7,10}}));
}

/* In loop if/else read in one path before written in the same loop
 * - value must survive the whole loop
 */
TEST_F(LifetimeEvaluatorExactTest, ReadInIfInLoopBeforeWrite)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_UADD, {2}, {1,3}, {}},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_UADD, {3}, {1,2}, {}},
      {   TGSI_OPCODE_UADD, {3}, {3,in1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {3}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,7}, {1,7}, {1,8}}));
}

/* In loop if/else read in one path before written in the same loop
 * read after the loop, value must survivethe whole loop and
 * to the read.
 */
TEST_F(LifetimeEvaluatorExactTest, ReadInLoopInIfBeforeWriteAndLifeToTheEnd)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_MUL, {1}, {1,in1}, {}},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_UADD, {1}, {1,in1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,6}}));
}

/* In loop if/else read in one path before written in the same loop
 * read after the loop, value must survivethe whole loop and
 * to the read.
 */
TEST_F(LifetimeEvaluatorExactTest, ReadInLoopBeforeWriteAndLifeToTheEnd)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_MUL, {1}, {1,in1}, {}},
      {   TGSI_OPCODE_UADD, {1}, {1,in1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,4}}));
}


/* Write in nested ifs in loop, for now we do test whether the
 * life time is at least what is required, but we know that the
 * implementation doesn't do a full check and sets larger boundaries
 */
TEST_F(LifetimeEvaluatorAtLeastTest, NestedIfInLoopAlwaysWriteButNotPropagated)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_IF, {}, {in0}, {}},
      {       TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {     TGSI_OPCODE_ELSE},
      {       TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {     TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_ELSE},
      {     TGSI_OPCODE_IF, {}, {in0}, {}},
      {       TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {     TGSI_OPCODE_ELSE},
      {       TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {     TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {3,14}}));
}

/* The value is written in a loop and in a nested if, but
 * not in all code paths, hence the value must survive the loop.
 */
TEST_F(LifetimeEvaluatorExactTest, NestedIfInLoopWriteNotAlways)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_IF, {}, {in0}, {}},
      {       TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {     TGSI_OPCODE_ELSE},
      {       TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {     TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_ELSE},
      {     TGSI_OPCODE_IF, {}, {in0}, {}},
      {       TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {     TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,13}}));
}

/* A continue in the loop is not relevant */
TEST_F(LifetimeEvaluatorExactTest, LoopWithWriteAfterContinue)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_CONT},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {4,6}}));
}

/* Temporary used to in case must live up to the case
 * statement where it is used, the switch we only keep
 * for the actual SWITCH opcode like it is in tgsi_exec.c, the
 * only current use case.
 */
TEST_F(LifetimeEvaluatorExactTest, UseSwitchCase)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_MOV, {2}, {in1}, {}},
      { TGSI_OPCODE_MOV, {3}, {in2}, {}},
      { TGSI_OPCODE_SWITCH, {}, {3}, {}},
      {   TGSI_OPCODE_CASE, {}, {2}, {}},
      {   TGSI_OPCODE_CASE, {}, {1}, {}},
      {   TGSI_OPCODE_BRK},
      {   TGSI_OPCODE_DEFAULT},
      { TGSI_OPCODE_ENDSWITCH},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,5}, {1,4}, {2,3}}));
}

/* With two destinations, if one result is thrown away, the
 * register must be kept past the writing instructions.
 */
TEST_F(LifetimeEvaluatorExactTest, WriteTwoOnlyUseOne)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_DFRACEXP , {1,2}, {in0}, {}},
      { TGSI_OPCODE_ADD , {3}, {2,in0}, {}},
      { TGSI_OPCODE_MOV, {out1}, {3}, {}},
      { TGSI_OPCODE_END},

   };
   run (code, temp_lt_expect({{-1,-1}, {0,1}, {0,1}, {1,2}}));
}

/* If a break is in the loop, all variables written after the
 * break and used outside the loop must be maintained for the
 * whole loop
 */
TEST_F(LifetimeEvaluatorExactTest, LoopWithWriteAfterBreak)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_BRK},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,6}}));
}

/* If a break is in the loop, all variables written after the
 * break and used outside the loop must be maintained for the
 * whole loop. The first break in the loop is the defining one.
 */
TEST_F(LifetimeEvaluatorExactTest, LoopWithWriteAfterBreak2Breaks)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_BRK},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {   TGSI_OPCODE_BRK},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,7}}));
}

/* Loop with a break at the beginning and read/write in the post
 * break loop scope. The value written and read within the loop
 * can be limited to [write, read], but the value read outside the
 * loop must survive the whole loop. This is the typical code for
 * while and for loops, where the breaking condition is tested at
 * the beginning.
 */
TEST_F(LifetimeEvaluatorExactTest, LoopWithWriteAndReadAfterBreak)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_BRK},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {   TGSI_OPCODE_MOV, {2}, {1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {2}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {4,5}, {0,7}}));
}

/* Same as above, just make sure that the life time of the local variable
 * in the outer loop (3) is not accidently promoted to the whole loop.
 */
TEST_F(LifetimeEvaluatorExactTest, NestedLoopWithWriteAndReadAfterBreak)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_IF, {}, {in1}, {}},
      {     TGSI_OPCODE_BRK},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_BGNLOOP},
      {     TGSI_OPCODE_IF, {}, {in0}, {}},
      {       TGSI_OPCODE_BRK},
      {     TGSI_OPCODE_ENDIF},
      {     TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {     TGSI_OPCODE_MOV, {2}, {1}, {}},
      {   TGSI_OPCODE_ENDLOOP },
      {   TGSI_OPCODE_ADD, {3}, {2,in0}, {}},
      {   TGSI_OPCODE_ADD, {4}, {3,in2}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {4}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {8,9}, {0,13}, {11,12}, {0,14}}));
}

/* If a break is in the loop inside a switch case, make sure it is
 * interpreted as breaking that inner loop, i.e. the variable has to
 * survive the loop.
 */
TEST_F(LifetimeEvaluatorExactTest, LoopWithWriteAfterBreakInSwitchInLoop)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_SWITCH, {}, {in1}, {}},
      {  TGSI_OPCODE_CASE, {}, {in1}, {}},
      {   TGSI_OPCODE_BGNLOOP },
      {    TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_BRK},
      {    TGSI_OPCODE_ENDIF},
      {    TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {   TGSI_OPCODE_ENDLOOP },
      {  TGSI_OPCODE_DEFAULT, {}, {}, {}},
      { TGSI_OPCODE_ENDSWITCH, {}, {}, {}},
      { TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {2,10}}));
}

/* Value written conditionally in one loop and read in another loop,
 * and both of these loops are within yet another loop. Here the value
 * has to survive the outer loop.
 */
TEST_F(LifetimeEvaluatorExactTest, LoopsWithDifferntScopesConditionalWrite)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {  TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {   TGSI_OPCODE_ENDIF},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_BGNLOOP },
      {     TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,7}}));
}

/* Value written and read in one loop and last read in another loop,
 * Here the value has to survive both loops.
 */
TEST_F(LifetimeEvaluatorExactTest, LoopsWithDifferntScopesFirstReadBeforeWrite)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_MUL, {1}, {1,in0}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,5}}));
}


/* Value is written in one switch code path within a loop
 * must survive the full loop.
 */
TEST_F(LifetimeEvaluatorExactTest, LoopWithWriteInSwitch)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_SWITCH, {}, {in0}, {} },
      {    TGSI_OPCODE_CASE, {}, {in0}, {} },
      {     TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {    TGSI_OPCODE_BRK },
      {   TGSI_OPCODE_DEFAULT },
      {   TGSI_OPCODE_BRK },
      {   TGSI_OPCODE_ENDSWITCH },
      {   TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,9}}));
}

/* Value written in one case, and read in other,in loop
 * - must survive the loop.
 */
TEST_F(LifetimeEvaluatorExactTest, LoopWithReadWriteInSwitchDifferentCase)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_SWITCH, {}, {in0}, {} },
      {    TGSI_OPCODE_CASE, {}, {in0}, {} },
      {     TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {    TGSI_OPCODE_BRK },
      {   TGSI_OPCODE_DEFAULT },
      {     TGSI_OPCODE_MOV, {out0}, {1}, {}},
      {   TGSI_OPCODE_BRK },
      {   TGSI_OPCODE_ENDSWITCH },
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,9}}));
}

/* Value written in one case, and read in other,in loop
 * - must survive the loop, even if the write case falls through.
 */
TEST_F(LifetimeEvaluatorExactTest, LoopWithReadWriteInSwitchDifferentCaseFallThrough)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_SWITCH, {}, {in0}, {} },
      {    TGSI_OPCODE_CASE, {}, {in0}, {} },
      {     TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {   TGSI_OPCODE_DEFAULT },
      {     TGSI_OPCODE_MOV, {out0}, {1}, {}},
      {   TGSI_OPCODE_BRK },
      {   TGSI_OPCODE_ENDSWITCH },
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,8}}));
}


/* Here we read and write from an to the same temp in the same instruction,
 * but the read is conditional (select operation), hence the lifetime must
 * start with the first write.
 */
TEST_F(LifetimeEvaluatorExactTest, WriteSelectFromSelf)
{
   const vector<FakeCodeline> code = {
      {TGSI_OPCODE_USEQ, {5}, {in0,in1}, {}},
      {TGSI_OPCODE_UCMP, {1}, {5,in1,1}, {}},
      {TGSI_OPCODE_UCMP, {1}, {5,in1,1}, {}},
      {TGSI_OPCODE_UCMP, {1}, {5,in1,1}, {}},
      {TGSI_OPCODE_UCMP, {1}, {5,in1,1}, {}},
      {TGSI_OPCODE_FSLT, {2}, {1,in1}, {}},
      {TGSI_OPCODE_UIF, {}, {2}, {}},
      {  TGSI_OPCODE_MOV, {3}, {in1}, {}},
      {TGSI_OPCODE_ELSE},
      {  TGSI_OPCODE_MOV, {4}, {in1}, {}},
      {  TGSI_OPCODE_MOV, {4}, {4}, {}},
      {  TGSI_OPCODE_MOV, {3}, {4}, {}},
      {TGSI_OPCODE_ENDIF},
      {TGSI_OPCODE_MOV, {out1}, {3}, {}},
      {TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {1,5}, {5,6}, {7,13}, {9,11}, {0,4}}));
}

/* This test checks wheter the ENDSWITCH is handled properly if the
 * last switch case/default doesn't stop with a BRK.
 */
TEST_F(LifetimeEvaluatorExactTest, LoopRWInSwitchCaseLastCaseWithoutBreak)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_SWITCH, {}, {in0}, {} },
      {    TGSI_OPCODE_CASE, {}, {in0}, {} },
      {     TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {    TGSI_OPCODE_BRK },
      {   TGSI_OPCODE_DEFAULT },
      {     TGSI_OPCODE_MOV, {out0}, {1}, {}},
      {   TGSI_OPCODE_ENDSWITCH },
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,8}}));
}

/* Value read/write in same case, stays there */
TEST_F(LifetimeEvaluatorExactTest, LoopWithReadWriteInSwitchSameCase)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_SWITCH, {}, {in0}, {} },
      {    TGSI_OPCODE_CASE, {}, {in0}, {} },
      {     TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {     TGSI_OPCODE_MOV, {out0}, {1}, {}},
      {    TGSI_OPCODE_BRK },
      {   TGSI_OPCODE_DEFAULT },
      {   TGSI_OPCODE_BRK },
      {   TGSI_OPCODE_ENDSWITCH },
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {3,4}}));
}

/* Value read/write in all cases, should only live from first
 * write to last read, but currently the whole loop is used.
 */
TEST_F(LifetimeEvaluatorAtLeastTest, LoopWithReadWriteInSwitchSameCase)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_SWITCH, {}, {in0}, {}},
      {    TGSI_OPCODE_CASE, {}, {in0}, {} },
      {     TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {    TGSI_OPCODE_BRK },
      {   TGSI_OPCODE_DEFAULT },
      {     TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {   TGSI_OPCODE_BRK },
      {   TGSI_OPCODE_ENDSWITCH },
      {   TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {3,9}}));
}

/* First read before first write with nested loops */
TEST_F(LifetimeEvaluatorExactTest, LoopsWithDifferentScopesCondReadBeforeWrite)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_BGNLOOP },
      {    TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_MOV, {out0}, {1}, {}},
      {    TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_ENDLOOP },
      {   TGSI_OPCODE_BGNLOOP },
      {     TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {   TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,9}}));
}

/* First read before first write wiredness with nested loops.
 * Here the first read of 2 is logically before the first, dominant
 * write, therfore, the 2 has to survive both loops.
 */
TEST_F(LifetimeEvaluatorExactTest, FirstWriteAtferReadInNestedLoop)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_BGNLOOP },
      {     TGSI_OPCODE_MUL, {2}, {2,1}, {}},
      {     TGSI_OPCODE_MOV, {3}, {2}, {}},
      {   TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_ADD, {1}, {1,in1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_MOV, {out0}, {3}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,7}, {1,7}, {4,8}}));
}


#define DST(X, W) vector<pair<int,int>>(1, make_pair(X, W))
#define SRC(X, S) vector<pair<int, const char *>>(1, make_pair(X, S))
#define SRC2(X, S, Y, T) vector<pair<int, const char *>>({make_pair(X, S), make_pair(Y, T)})

/* Partial write to components: one component was written unconditionally
 * but another conditionally, temporary must survive the whole loop.
 * Test series for all components.
 */
TEST_F(LifetimeEvaluatorExactTest, LoopWithConditionalComponentWrite_X)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP},
      {   TGSI_OPCODE_MOV, DST(1, WRITEMASK_Y), SRC(in1, "x"), {}, SWZ()},
      {   TGSI_OPCODE_IF, {}, SRC(in0, "xxxx"), {}, SWZ()},
      {     TGSI_OPCODE_MOV, DST(1, WRITEMASK_X), SRC(in1, "y"), {}, SWZ()},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_MOV, DST(2, WRITEMASK_XY), SRC(1, "xy"), {}, SWZ()},
      { TGSI_OPCODE_ENDLOOP},
      { TGSI_OPCODE_MOV, DST(out0, WRITEMASK_XYZW), SRC(2, "xyxy"), {}, SWZ()},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,6}, {5,7}}));
}

TEST_F(LifetimeEvaluatorExactTest, LoopWithConditionalComponentWrite_Y)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP},
      {   TGSI_OPCODE_MOV, DST(1, WRITEMASK_X), SRC(in1, "x"), {}, SWZ()},
      {   TGSI_OPCODE_IF, {}, SRC(in0, "xxxx"), {}, SWZ()},
      {     TGSI_OPCODE_MOV, DST(1, WRITEMASK_Y), SRC(in1, "y"), {}, SWZ()},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_MOV, DST(2, WRITEMASK_XY), SRC(1, "xy"), {}, SWZ()},
      { TGSI_OPCODE_ENDLOOP},
      { TGSI_OPCODE_MOV, DST(out0, WRITEMASK_XYZW), SRC(2, "xyxy"), {}, SWZ()},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,6}, {5,7}}));
}

TEST_F(LifetimeEvaluatorExactTest, LoopWithConditionalComponentWrite_Z)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP},
      {   TGSI_OPCODE_MOV, DST(1, WRITEMASK_X), SRC(in1, "x"), {}, SWZ()},
      {   TGSI_OPCODE_IF, {}, SRC(in0, "xxxx"), {}, SWZ()},
      {     TGSI_OPCODE_MOV, DST(1, WRITEMASK_Z), SRC(in1, "y"), {}, SWZ()},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_MOV, DST(2, WRITEMASK_XY), SRC(1, "xz"), {}, SWZ()},
      { TGSI_OPCODE_ENDLOOP},
      { TGSI_OPCODE_MOV, DST(out0, WRITEMASK_XYZW), SRC(2, "xyxy"), {}, SWZ()},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,6}, {5,7}}));
}

TEST_F(LifetimeEvaluatorExactTest, LoopWithConditionalComponentWrite_W)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP},
      {   TGSI_OPCODE_MOV, DST(1, WRITEMASK_X), SRC(in1, "x"), {}, SWZ()},
      {   TGSI_OPCODE_IF, {}, SRC(in0, "xxxx"), {}, SWZ()},
      {     TGSI_OPCODE_MOV, DST(1, WRITEMASK_W), SRC(in1, "y"), {}, SWZ()},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_MOV, DST(2, WRITEMASK_XY), SRC(1, "xw"), {}, SWZ()},
      { TGSI_OPCODE_ENDLOOP},
      { TGSI_OPCODE_MOV, DST(out0, WRITEMASK_XYZW), SRC(2, "xyxy"), {}, SWZ()},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,6}, {5,7}}));
}

TEST_F(LifetimeEvaluatorExactTest, LoopWithConditionalComponentWrite_X_Read_Y_Before)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP},
      {   TGSI_OPCODE_MOV, DST(1, WRITEMASK_X), SRC(in1, "x"), {}, SWZ()},
      {   TGSI_OPCODE_IF, {}, SRC(in0, "xxxx"), {}, SWZ()},
      {     TGSI_OPCODE_MOV, DST(2, WRITEMASK_XYZW), SRC(1, "yyyy"), {}, SWZ()},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_MOV, DST(1, WRITEMASK_YZW), SRC(2, "yyzw"), {}, SWZ()},
      { TGSI_OPCODE_ENDLOOP},
      { TGSI_OPCODE_ADD, DST(out0, WRITEMASK_XYZW),
                         SRC2(2, "yyzw", 1, "xyxy"), {}, SWZ()},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,7}, {0,7}}));
}

/* The variable is conditionally read before first written, so
 * it has to surive all the loops.
 */
TEST_F(LifetimeEvaluatorExactTest, FRaWSameInstructionInLoopAndCondition)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_BGNLOOP },
      {     TGSI_OPCODE_IF, {}, {in0}, {} },
      {       TGSI_OPCODE_ADD, {1}, {1,in0}, {}},
      {     TGSI_OPCODE_ENDIF},
      {     TGSI_OPCODE_MOV, {1}, {in1}, {}},
      {  TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END},

   };
   run (code, temp_lt_expect({{-1,-1}, {0,7}}));
}

/* If unconditionally first written and read in the same
 * instruction, then the register must be kept for the
 * one write, but not more (undefined behaviour)
 */
TEST_F(LifetimeEvaluatorExactTest, FRaWSameInstruction)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_ADD, {1}, {1,in0}, {}},
      { TGSI_OPCODE_END},

   };
   run (code, temp_lt_expect({{-1,-1}, {0,1}}));
}

/* If unconditionally written and read in the same
 * instruction, various times then the register must be
 * kept past the last write, but not longer (undefined behaviour)
 */
TEST_F(LifetimeEvaluatorExactTest, FRaWSameInstructionMoreThenOnce)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_ADD, {1}, {1,in0}, {}},
      { TGSI_OPCODE_ADD, {1}, {1,in0}, {}},
      { TGSI_OPCODE_MOV, {out0}, {in0}, {}},
      { TGSI_OPCODE_END},

   };
   run (code, temp_lt_expect({{-1,-1}, {0,2}}));
}

/* Register is only written. This should not happen,
 * but to handle the case we want the register to life
 * at least one instruction
 */
TEST_F(LifetimeEvaluatorExactTest, WriteOnly)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,1}}));
}

/* Register is read in IF.
 */
TEST_F(LifetimeEvaluatorExactTest, SimpleReadForIf)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_ADD, {out0}, {in0,in1}, {}},
      { TGSI_OPCODE_IF, {}, {1}, {}},
      { TGSI_OPCODE_ENDIF}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,2}}));
}

TEST_F(LifetimeEvaluatorExactTest, WriteTwoReadOne)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_DFRACEXP , {1,2}, {in0}, {}},
      { TGSI_OPCODE_ADD , {3}, {2,in0}, {}},
      { TGSI_OPCODE_MOV, {out1}, {3}, {}},
      { TGSI_OPCODE_END},
   };
   run (code, temp_lt_expect({{-1,-1}, {0,1}, {0,1}, {1,2}}));
}

TEST_F(LifetimeEvaluatorExactTest, ReadOnly)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_END},
   };
   run (code, temp_lt_expect({{-1,-1}, {-1,-1}}));
}

/* Test handling of missing END marker
*/
TEST_F(LifetimeEvaluatorExactTest, SomeScopesAndNoEndProgramId)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_IF, {}, {1}, {}},
      { TGSI_OPCODE_MOV, {2}, {1}, {}},
      { TGSI_OPCODE_ENDIF},
      { TGSI_OPCODE_IF, {}, {1}, {}},
      { TGSI_OPCODE_MOV, {out0}, {2}, {}},
      { TGSI_OPCODE_ENDIF},
   };
   run (code, temp_lt_expect({{-1,-1}, {0,4}, {2,5}}));
}

TEST_F(LifetimeEvaluatorExactTest, SerialReadWrite)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_MOV, {2}, {1}, {}},
      { TGSI_OPCODE_MOV, {3}, {2}, {}},
      { TGSI_OPCODE_MOV, {out0}, {3}, {}},
      { TGSI_OPCODE_END},
   };
   run (code, temp_lt_expect({{-1,-1}, {0,1}, {1,2}, {2,3}}));
}

/* Check that two destination registers are used */
TEST_F(LifetimeEvaluatorExactTest, TwoDestRegisters)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_DFRACEXP , {1,2}, {in0}, {}},
      { TGSI_OPCODE_ADD, {out0}, {1,2}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,1}, {0,1}}));
}

/* Check that writing within a loop in a conditional is propagated
 * to the outer loop.
 */
TEST_F(LifetimeEvaluatorExactTest, WriteInLoopInConditionalReadOutside)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP},
      {   TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_BGNLOOP},
      {       TGSI_OPCODE_MOV, {1}, {in1}, {}},
      {     TGSI_OPCODE_ENDLOOP},
      {   TGSI_OPCODE_ENDIF},
      {   TGSI_OPCODE_ADD, {2}, {1,in1}, {}},
      { TGSI_OPCODE_ENDLOOP},
      { TGSI_OPCODE_MOV, {out0}, {2}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,7}, {6,8}}));
}

/* Check that a register written in a loop that is inside a conditional
 * is not propagated past that loop if last read is also within the
 * conditional
*/
TEST_F(LifetimeEvaluatorExactTest, WriteInLoopInCondReadInCondOutsideLoop)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP},
      {   TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_BGNLOOP},
      {       TGSI_OPCODE_MUL, {1}, {in2,in1}, {}},
      {     TGSI_OPCODE_ENDLOOP},
      {     TGSI_OPCODE_ADD, {2}, {1,in1}, {}},
      {   TGSI_OPCODE_ENDIF},
      { TGSI_OPCODE_ENDLOOP},
      { TGSI_OPCODE_MOV, {out0}, {2}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {3,5}, {0,8}}));
}

/* Check that a register read before written in a loop that is
 * inside a conditional is propagated to the outer loop.
 */
TEST_F(LifetimeEvaluatorExactTest, ReadWriteInLoopInCondReadInCondOutsideLoop)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP},
      {   TGSI_OPCODE_IF, {}, {in0}, {}},
      {     TGSI_OPCODE_BGNLOOP},
      {       TGSI_OPCODE_MUL, {1}, {1,in1}, {}},
      {     TGSI_OPCODE_ENDLOOP},
      {     TGSI_OPCODE_ADD, {2}, {1,in1}, {}},
      {   TGSI_OPCODE_ENDIF},
      { TGSI_OPCODE_ENDLOOP},
      { TGSI_OPCODE_MOV, {out0}, {2}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,7}, {0,8}}));
}

/* With two destinations if one value is thrown away, we must
 * ensure that the two output registers don't merge. In this test
 * case the last access for 2 and 3 is in line 4, but 4 can only
 * be merged with 3 because it is read,2 on the other hand is written
 * to, and merging it with 4 would result in a bug.
 */
TEST_F(LifetimeEvaluatorExactTest, WritePastLastRead2)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_MOV, {2}, {in0}, {}},
      { TGSI_OPCODE_ADD, {3}, {1,2}, {}},
      { TGSI_OPCODE_DFRACEXP , {2,4}, {3}, {}},
      { TGSI_OPCODE_MOV, {out1}, {4}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,2}, {1,4}, {2,3}, {3,4}}));
}

/* Check that three source registers are used */
TEST_F(LifetimeEvaluatorExactTest, ThreeSourceRegisters)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_DFRACEXP , {1,2}, {in0}, {}},
      { TGSI_OPCODE_ADD , {3}, {in0,in1}, {}},
      { TGSI_OPCODE_MAD, {out0}, {1,2,3}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,2}, {0,2}, {1,2}}));
}

/* Check minimal lifetime for registers only written to */
TEST_F(LifetimeEvaluatorExactTest, OverwriteWrittenOnlyTemps)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV , {1}, {in0}, {}},
      { TGSI_OPCODE_MOV , {2}, {in1}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,1}, {1,2}}));
}

/* Same register is only written twice. This should not happen,
 * but to handle the case we want the register to life
 * at least past the last write instruction
 */
TEST_F(LifetimeEvaluatorExactTest, WriteOnlyTwiceSame)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,2}}));
}

/* Dead code elimination should catch and remove the case
 * when a variable is written after its last read, but
 * we want the code to be aware of this case.
 * The life time of this uselessly written variable is set
 * to the instruction after the write, because
 * otherwise it could be re-used too early.
 */
TEST_F(LifetimeEvaluatorExactTest, WritePastLastRead)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_MOV, {1}, {in0}, {}},
      { TGSI_OPCODE_MOV, {2}, {1}, {}},
      { TGSI_OPCODE_MOV, {1}, {2}, {}},
      { TGSI_OPCODE_END},

   };
   run (code, temp_lt_expect({{-1,-1}, {0,3}, {1,2}}));
}

/* If a break is in the loop, all variables written after the
 * break and used outside the loop the variable must survive the
 * outer loop
 */
TEST_F(LifetimeEvaluatorExactTest, NestedLoopWithWriteAfterBreak)
{
   const vector<FakeCodeline> code = {
      { TGSI_OPCODE_BGNLOOP },
      {   TGSI_OPCODE_BGNLOOP },
      {     TGSI_OPCODE_IF, {}, {in0}, {}},
      {       TGSI_OPCODE_BRK},
      {     TGSI_OPCODE_ENDIF},
      {     TGSI_OPCODE_MOV, {1}, {in0}, {}},
      {   TGSI_OPCODE_ENDLOOP },
      {   TGSI_OPCODE_MOV, {out0}, {1}, {}},
      { TGSI_OPCODE_ENDLOOP },
      { TGSI_OPCODE_END}
   };
   run (code, temp_lt_expect({{-1,-1}, {0,8}}));
}

/* Test remapping table of registers. The tests don't assume
 * that the sorting algorithm used to sort the lifetimes
 * based on their 'begin' is stable.
 */
TEST_F(RegisterRemappingTest, RegisterRemapping1)
{
   vector<lifetime> lt({{-1,-1},
                        {0,1},
                        {0,2},
                        {1,2},
                        {2,10},
                        {3,5},
                        {5,10}
                       });

   vector<int> expect({0,1,2,1,1,2,2});
   run(lt, expect);
}

TEST_F(RegisterRemappingTest, RegisterRemapping2)
{
   vector<lifetime> lt({{-1,-1},
                        {0,1},
                        {0,2},
                        {3,4},
                        {4,5},
                       });
   vector<int> expect({0,1,2,1,1});
   run(lt, expect);
}

TEST_F(RegisterRemappingTest, RegisterRemappingMergeAllToOne)
{
   vector<lifetime> lt({{-1,-1},
                        {0,1},
                        {1,2},
                        {2,3},
                        {3,4},
                       });
   vector<int> expect({0,1,1,1,1});
   run(lt, expect);
}

TEST_F(RegisterRemappingTest, RegisterRemappingIgnoreUnused)
{
   vector<lifetime> lt({{-1,-1},
                        {0,1},
                        {1,2},
                        {2,3},
                        {-1,-1},
                        {3,4},
                       });
   vector<int> expect({0,1,1,1,4,1});
   run(lt, expect);
}

TEST_F(RegisterRemappingTest, RegisterRemappingMergeZeroLifetimeRegisters)
{
   vector<lifetime> lt({{-1,-1},
                        {0,1},
                        {1,2},
                        {2,3},
                        {3,3},
                        {3,4},
                       });
   vector<int> expect({0,1,1,1,1,1});
   run(lt, expect);
}

TEST_F(RegisterLifetimeAndRemappingTest, LifetimeAndRemapping)
{
   const vector<FakeCodeline> code = {
      {TGSI_OPCODE_USEQ, {5}, {in0,in1}, {}},
      {TGSI_OPCODE_UCMP, {1}, {5,in1,1}, {}},
      {TGSI_OPCODE_UCMP, {1}, {5,in1,1}, {}},
      {TGSI_OPCODE_UCMP, {1}, {5,in1,1}, {}},
      {TGSI_OPCODE_UCMP, {1}, {5,in1,1}, {}},
      {TGSI_OPCODE_FSLT, {2}, {1,in1}, {}},
      {TGSI_OPCODE_UIF, {}, {2}, {}},
      {  TGSI_OPCODE_MOV, {3}, {in1}, {}},
      {TGSI_OPCODE_ELSE},
      {  TGSI_OPCODE_MOV, {4}, {in1}, {}},
      {  TGSI_OPCODE_MOV, {4}, {4}, {}},
      {  TGSI_OPCODE_MOV, {3}, {4}, {}},
      {TGSI_OPCODE_ENDIF},
      {TGSI_OPCODE_MOV, {out1}, {3}, {}},
      {TGSI_OPCODE_END}
   };
   run (code, vector<int>({0,1,5,5,1,5}));
}

TEST_F(RegisterLifetimeAndRemappingTest, LifetimeAndRemappingWithUnusedReadOnlyIgnored)
{
   const vector<FakeCodeline> code = {
      {TGSI_OPCODE_USEQ, {1}, {in0,in1}, {}},
      {TGSI_OPCODE_UCMP, {2}, {1,in1,2}, {}},
      {TGSI_OPCODE_UCMP, {4}, {2,in1,1}, {}},
      {TGSI_OPCODE_ADD, {5}, {2,4}, {}},
      {TGSI_OPCODE_UIF, {}, {7}, {}},
      {  TGSI_OPCODE_ADD, {8}, {5,4}, {}},
      {TGSI_OPCODE_ENDIF},
      {TGSI_OPCODE_MOV, {out1}, {8}, {}},
      {TGSI_OPCODE_END}
   };
   /* lt: 1: 0-2,2: 1-3 3: u 4: 2-5 5: 3-5 6: u 7: 0-(-1),8: 5-7 */
   run (code, vector<int>({0,1,2,3,1,2,6,7,1}));
}

TEST_F(RegisterLifetimeAndRemappingTest, LifetimeAndRemappingWithUnusedReadOnlyRemappedTo)
{
   const vector<FakeCodeline> code = {
      {TGSI_OPCODE_USEQ, {1}, {in0,in1}, {}},
      {TGSI_OPCODE_UIF, {}, {7}, {}},
      {  TGSI_OPCODE_UCMP, {2}, {1,in1,2}, {}},
      {  TGSI_OPCODE_UCMP, {4}, {2,in1,1}, {}},
      {  TGSI_OPCODE_ADD, {5}, {2,4}, {}},
      {  TGSI_OPCODE_ADD, {8}, {5,4}, {}},
      {TGSI_OPCODE_ENDIF},
      {TGSI_OPCODE_MOV, {out1}, {8}, {}},
      {TGSI_OPCODE_END}
   };
   /* lt: 1: 0-3,2: 2-4 3: u 4: 3-5 5: 4-5 6: u 7: 1-1,8: 5-7 */
   run (code, vector<int>({0,1,2,3,1,2,6,7,1}));
}

TEST_F(RegisterLifetimeAndRemappingTest, LifetimeAndRemappingWithUnusedReadOnlyRemapped)
{
   const vector<FakeCodeline> code = {
      {TGSI_OPCODE_USEQ, {0}, {in0,in1}, {}},
      {TGSI_OPCODE_UCMP, {2}, {0,in1,2}, {}},
      {TGSI_OPCODE_UCMP, {4}, {2,in1,0}, {}},
      {TGSI_OPCODE_UIF, {}, {7}, {}},
      {  TGSI_OPCODE_ADD, {5}, {4,4}, {}},
      {  TGSI_OPCODE_ADD, {8}, {5,4}, {}},
      {TGSI_OPCODE_ENDIF},
      {TGSI_OPCODE_MOV, {out1}, {8}, {}},
      {TGSI_OPCODE_END}
   };
   /* lt: 0: 0-2 1: u 2: 1-2 3: u 4: 2-5 5: 4-5 6: u 7:ro 8: 5-7 */
   run (code, vector<int>({0,1,2,3,0,2,6,7,0}));
}
