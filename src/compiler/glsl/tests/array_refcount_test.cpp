/*
 * Copyright © 2016 Intel Corporation
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
#include <gtest/gtest.h>
#include "ir.h"
#include "ir_array_refcount.h"

class array_refcount_test : public ::testing::Test {
public:
   virtual void SetUp();
   virtual void TearDown();

   void *mem_ctx;

   /**
    * glsl_type for a vec4[3][4][5].
    *
    * The exceptionally verbose name is picked because it matches the syntax
    * of http://cdecl.org/.
    */
   const glsl_type *array_3_of_array_4_of_array_5_of_vec4;

   /**
    * Wrapper to access private member "bits" of ir_array_refcount_entry
    *
    * The test class is a friend to ir_array_refcount_entry, but the
    * individual tests are not part of the class.  Since the friendliness of
    * the test class does not extend to the tests, provide a wrapper.
    */
   const BITSET_WORD *get_bits(const ir_array_refcount_entry &entry)
   {
      return entry.bits;
   }

   /**
    * Wrapper to access private member "num_bits" of ir_array_refcount_entry
    *
    * The test class is a friend to ir_array_refcount_entry, but the
    * individual tests are not part of the class.  Since the friendliness of
    * the test class does not extend to the tests, provide a wrapper.
    */
   unsigned get_num_bits(const ir_array_refcount_entry &entry)
   {
      return entry.num_bits;
   }

   /**
    * Wrapper to access private member "array_depth" of ir_array_refcount_entry
    *
    * The test class is a friend to ir_array_refcount_entry, but the
    * individual tests are not part of the class.  Since the friendliness of
    * the test class does not extend to the tests, provide a wrapper.
    */
   unsigned get_array_depth(const ir_array_refcount_entry &entry)
   {
      return entry.array_depth;
   }
};

void
array_refcount_test::SetUp()
{
   mem_ctx = ralloc_context(NULL);

   /* The type of vec4 x[3][4][5]; */
   const glsl_type *const array_5_of_vec4 =
      glsl_type::get_array_instance(glsl_type::vec4_type, 5);
   const glsl_type *const array_4_of_array_5_of_vec4 =
      glsl_type::get_array_instance(array_5_of_vec4, 4);
   array_3_of_array_4_of_array_5_of_vec4 =
      glsl_type::get_array_instance(array_4_of_array_5_of_vec4, 3);
}

void
array_refcount_test::TearDown()
{
   ralloc_free(mem_ctx);
   mem_ctx = NULL;
}

TEST_F(array_refcount_test, ir_array_refcount_entry_initial_state_for_scalar)
{
   ir_variable *const var =
      new(mem_ctx) ir_variable(glsl_type::int_type, "a", ir_var_auto);

   ir_array_refcount_entry entry(var);

   ASSERT_NE((void *)0, get_bits(entry));
   EXPECT_FALSE(entry.is_referenced);
   EXPECT_EQ(1, get_num_bits(entry));
   EXPECT_EQ(0, get_array_depth(entry));
   EXPECT_FALSE(entry.is_linearized_index_referenced(0));
}

TEST_F(array_refcount_test, ir_array_refcount_entry_initial_state_for_vector)
{
   ir_variable *const var =
      new(mem_ctx) ir_variable(glsl_type::vec4_type, "a", ir_var_auto);

   ir_array_refcount_entry entry(var);

   ASSERT_NE((void *)0, get_bits(entry));
   EXPECT_FALSE(entry.is_referenced);
   EXPECT_EQ(1, get_num_bits(entry));
   EXPECT_EQ(0, get_array_depth(entry));
   EXPECT_FALSE(entry.is_linearized_index_referenced(0));
}

TEST_F(array_refcount_test, ir_array_refcount_entry_initial_state_for_matrix)
{
   ir_variable *const var =
      new(mem_ctx) ir_variable(glsl_type::mat4_type, "a", ir_var_auto);

   ir_array_refcount_entry entry(var);

   ASSERT_NE((void *)0, get_bits(entry));
   EXPECT_FALSE(entry.is_referenced);
   EXPECT_EQ(1, get_num_bits(entry));
   EXPECT_EQ(0, get_array_depth(entry));
   EXPECT_FALSE(entry.is_linearized_index_referenced(0));
}

TEST_F(array_refcount_test, ir_array_refcount_entry_initial_state_for_array)
{
   ir_variable *const var =
      new(mem_ctx) ir_variable(array_3_of_array_4_of_array_5_of_vec4,
                               "a",
                               ir_var_auto);
   const unsigned total_elements = var->type->arrays_of_arrays_size();

   ir_array_refcount_entry entry(var);

   ASSERT_NE((void *)0, get_bits(entry));
   EXPECT_FALSE(entry.is_referenced);
   EXPECT_EQ(total_elements, get_num_bits(entry));
   EXPECT_EQ(3, get_array_depth(entry));

   for (unsigned i = 0; i < total_elements; i++)
      EXPECT_FALSE(entry.is_linearized_index_referenced(i)) << "index = " << i;
}

TEST_F(array_refcount_test, mark_array_elements_referenced_simple)
{
   ir_variable *const var =
      new(mem_ctx) ir_variable(array_3_of_array_4_of_array_5_of_vec4,
                               "a",
                               ir_var_auto);
   const unsigned total_elements = var->type->arrays_of_arrays_size();

   ir_array_refcount_entry entry(var);

   static const array_deref_range dr[] = {
      { 0, 5 }, { 1, 4 }, { 2, 3 }
   };
   const unsigned accessed_element = 0 + (1 * 5) + (2 * 4 * 5);

   entry.mark_array_elements_referenced(dr, 3);

   for (unsigned i = 0; i < total_elements; i++)
      EXPECT_EQ(i == accessed_element, entry.is_linearized_index_referenced(i));
}

TEST_F(array_refcount_test, mark_array_elements_referenced_whole_first_array)
{
   ir_variable *const var =
      new(mem_ctx) ir_variable(array_3_of_array_4_of_array_5_of_vec4,
                               "a",
                               ir_var_auto);

   ir_array_refcount_entry entry(var);

   static const array_deref_range dr[] = {
      { 0, 5 }, { 1, 4 }, { 3, 3 }
   };

   entry.mark_array_elements_referenced(dr, 3);

   for (unsigned i = 0; i < 3; i++) {
      for (unsigned j = 0; j < 4; j++) {
         for (unsigned k = 0; k < 5; k++) {
            const bool accessed = (j == 1) && (k == 0);
            const unsigned linearized_index = k + (j * 5) + (i * 4 * 5);

            EXPECT_EQ(accessed,
                      entry.is_linearized_index_referenced(linearized_index));
         }
      }
   }
}

TEST_F(array_refcount_test, mark_array_elements_referenced_whole_second_array)
{
   ir_variable *const var =
      new(mem_ctx) ir_variable(array_3_of_array_4_of_array_5_of_vec4,
                               "a",
                               ir_var_auto);

   ir_array_refcount_entry entry(var);

   static const array_deref_range dr[] = {
      { 0, 5 }, { 4, 4 }, { 1, 3 }
   };

   entry.mark_array_elements_referenced(dr, 3);

   for (unsigned i = 0; i < 3; i++) {
      for (unsigned j = 0; j < 4; j++) {
         for (unsigned k = 0; k < 5; k++) {
            const bool accessed = (i == 1) && (k == 0);
            const unsigned linearized_index = k + (j * 5) + (i * 4 * 5);

            EXPECT_EQ(accessed,
                      entry.is_linearized_index_referenced(linearized_index));
         }
      }
   }
}

TEST_F(array_refcount_test, mark_array_elements_referenced_whole_third_array)
{
   ir_variable *const var =
      new(mem_ctx) ir_variable(array_3_of_array_4_of_array_5_of_vec4,
                               "a",
                               ir_var_auto);

   ir_array_refcount_entry entry(var);

   static const array_deref_range dr[] = {
      { 5, 5 }, { 2, 4 }, { 1, 3 }
   };

   entry.mark_array_elements_referenced(dr, 3);

   for (unsigned i = 0; i < 3; i++) {
      for (unsigned j = 0; j < 4; j++) {
         for (unsigned k = 0; k < 5; k++) {
            const bool accessed = (i == 1) && (j == 2);
            const unsigned linearized_index = k + (j * 5) + (i * 4 * 5);

            EXPECT_EQ(accessed,
                      entry.is_linearized_index_referenced(linearized_index));
         }
      }
   }
}

TEST_F(array_refcount_test, mark_array_elements_referenced_whole_first_and_third_arrays)
{
   ir_variable *const var =
      new(mem_ctx) ir_variable(array_3_of_array_4_of_array_5_of_vec4,
                               "a",
                               ir_var_auto);

   ir_array_refcount_entry entry(var);

   static const array_deref_range dr[] = {
      { 5, 5 }, { 3, 4 }, { 3, 3 }
   };

   entry.mark_array_elements_referenced(dr, 3);

   for (unsigned i = 0; i < 3; i++) {
      for (unsigned j = 0; j < 4; j++) {
         for (unsigned k = 0; k < 5; k++) {
            const bool accessed = (j == 3);
            const unsigned linearized_index = k + (j * 5) + (i * 4 * 5);

            EXPECT_EQ(accessed,
                      entry.is_linearized_index_referenced(linearized_index));
         }
      }
   }
}
