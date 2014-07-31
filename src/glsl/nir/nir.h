/*
 * Copyright © 2014 Connor Abbott
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
 *
 * Authors:
 *    Connor Abbott (cwabbott0@gmail.com)
 *
 */

#pragma once

#include "util/hash_table.h"
#include "main/set.h"
#include "../list.h"
#include "GL/gl.h" /* GLenum */
#include "util/ralloc.h"
#include "nir_types.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nir_function_overload;
struct nir_function;


/**
 * Description of built-in state associated with a uniform
 *
 * \sa nir_variable::state_slots
 */
typedef struct {
   int tokens[5];
   int swizzle;
} nir_state_slot;

typedef enum {
   nir_var_shader_in,
   nir_var_shader_out,
   nir_var_global,
   nir_var_local,
   nir_var_uniform,
   nir_var_system_value
} nir_variable_mode;

/**
 * Data stored in an nir_constant
 */
union nir_constant_data {
   unsigned u[16];
   int i[16];
   float f[16];
   bool b[16];
};

typedef struct nir_constant {
   /**
    * Value of the constant.
    *
    * The field used to back the values supplied by the constant is determined
    * by the type associated with the \c ir_instruction.  Constants may be
    * scalars, vectors, or matrices.
    */
   union nir_constant_data value;

   /* Array elements / Structure Fields */
   struct nir_constant **elements;
} nir_constant;

/**
 * \brief Layout qualifiers for gl_FragDepth.
 *
 * The AMD/ARB_conservative_depth extensions allow gl_FragDepth to be redeclared
 * with a layout qualifier.
 */
typedef enum {
    nir_depth_layout_none, /**< No depth layout is specified. */
    nir_depth_layout_any,
    nir_depth_layout_greater,
    nir_depth_layout_less,
    nir_depth_layout_unchanged
} nir_depth_layout;

/**
 * Either a uniform, global variable, shader input, or shader output. Based on
 * ir_variable - it should be easy to translate between the two.
 */

typedef struct {
   struct exec_node node;

   /**
    * Declared type of the variable
    */
   const struct glsl_type *type;

   /**
    * Declared name of the variable
    */
   char *name;

   /**
    * For variables which satisfy the is_interface_instance() predicate, this
    * points to an array of integers such that if the ith member of the
    * interface block is an array, max_ifc_array_access[i] is the maximum
    * array element of that member that has been accessed.  If the ith member
    * of the interface block is not an array, max_ifc_array_access[i] is
    * unused.
    *
    * For variables whose type is not an interface block, this pointer is
    * NULL.
    */
   unsigned *max_ifc_array_access;

   struct nir_variable_data {

      /**
       * Is the variable read-only?
       *
       * This is set for variables declared as \c const, shader inputs,
       * and uniforms.
       */
      unsigned read_only:1;
      unsigned centroid:1;
      unsigned sample:1;
      unsigned invariant:1;

      /**
       * Storage class of the variable.
       *
       * \sa nir_variable_mode
       */
      unsigned mode:4;

      /**
       * Interpolation mode for shader inputs / outputs
       *
       * \sa ir_variable_interpolation
       */
      unsigned interpolation:2;

      /**
       * \name ARB_fragment_coord_conventions
       * @{
       */
      unsigned origin_upper_left:1;
      unsigned pixel_center_integer:1;
      /*@}*/

      /**
       * Was the location explicitly set in the shader?
       *
       * If the location is explicitly set in the shader, it \b cannot be changed
       * by the linker or by the API (e.g., calls to \c glBindAttribLocation have
       * no effect).
       */
      unsigned explicit_location:1;
      unsigned explicit_index:1;

      /**
       * Was an initial binding explicitly set in the shader?
       *
       * If so, constant_value contains an integer ir_constant representing the
       * initial binding point.
       */
      unsigned explicit_binding:1;

      /**
       * Does this variable have an initializer?
       *
       * This is used by the linker to cross-validiate initializers of global
       * variables.
       */
      unsigned has_initializer:1;

      /**
       * Is this variable a generic output or input that has not yet been matched
       * up to a variable in another stage of the pipeline?
       *
       * This is used by the linker as scratch storage while assigning locations
       * to generic inputs and outputs.
       */
      unsigned is_unmatched_generic_inout:1;

      /**
       * If non-zero, then this variable may be packed along with other variables
       * into a single varying slot, so this offset should be applied when
       * accessing components.  For example, an offset of 1 means that the x
       * component of this variable is actually stored in component y of the
       * location specified by \c location.
       */
      unsigned location_frac:2;

      /**
       * Non-zero if this variable was created by lowering a named interface
       * block which was not an array.
       *
       * Note that this variable and \c from_named_ifc_block_array will never
       * both be non-zero.
       */
      unsigned from_named_ifc_block_nonarray:1;

      /**
       * Non-zero if this variable was created by lowering a named interface
       * block which was an array.
       *
       * Note that this variable and \c from_named_ifc_block_nonarray will never
       * both be non-zero.
       */
      unsigned from_named_ifc_block_array:1;

      /**
       * \brief Layout qualifier for gl_FragDepth.
       *
       * This is not equal to \c ir_depth_layout_none if and only if this
       * variable is \c gl_FragDepth and a layout qualifier is specified.
       */
      nir_depth_layout depth_layout;

      /**
       * Storage location of the base of this variable
       *
       * The precise meaning of this field depends on the nature of the variable.
       *
       *   - Vertex shader input: one of the values from \c gl_vert_attrib.
       *   - Vertex shader output: one of the values from \c gl_varying_slot.
       *   - Geometry shader input: one of the values from \c gl_varying_slot.
       *   - Geometry shader output: one of the values from \c gl_varying_slot.
       *   - Fragment shader input: one of the values from \c gl_varying_slot.
       *   - Fragment shader output: one of the values from \c gl_frag_result.
       *   - Uniforms: Per-stage uniform slot number for default uniform block.
       *   - Uniforms: Index within the uniform block definition for UBO members.
       *   - Other: This field is not currently used.
       *
       * If the variable is a uniform, shader input, or shader output, and the
       * slot has not been assigned, the value will be -1.
       */
      int location;

      /**
       * The actual location of the variable in the IR. Only valid for inputs
       * and outputs.
       */
      unsigned int driver_location;

      /**
       * output index for dual source blending.
       */
      int index;

      /**
       * Initial binding point for a sampler or UBO.
       *
       * For array types, this represents the binding point for the first element.
       */
      int binding;

      /**
       * Location an atomic counter is stored at.
       */
      struct {
         unsigned buffer_index;
         unsigned offset;
      } atomic;

      /**
       * ARB_shader_image_load_store qualifiers.
       */
      struct {
         bool read_only; /**< "readonly" qualifier. */
         bool write_only; /**< "writeonly" qualifier. */
         bool coherent;
         bool _volatile;
         bool restrict_flag;

         /** Image internal format if specified explicitly, otherwise GL_NONE. */
         GLenum format;
      } image;

      /**
       * Highest element accessed with a constant expression array index
       *
       * Not used for non-array variables.
       */
      unsigned max_array_access;

   } data;

   /**
    * Built-in state that backs this uniform
    *
    * Once set at variable creation, \c state_slots must remain invariant.
    * This is because, ideally, this array would be shared by all clones of
    * this variable in the IR tree.  In other words, we'd really like for it
    * to be a fly-weight.
    *
    * If the variable is not a uniform, \c num_state_slots will be zero and
    * \c state_slots will be \c NULL.
    */
   /*@{*/
   unsigned num_state_slots;    /**< Number of state slots used */
   nir_state_slot *state_slots;  /**< State descriptors. */
   /*@}*/

   /**
    * Value assigned in the initializer of a variable declared "const"
    */
   nir_constant *constant_value;

   /**
    * Constant expression assigned in the initializer of the variable
    *
    * \warning
    * This field and \c ::constant_value are distinct.  Even if the two fields
    * refer to constants with the same value, they must point to separate
    * objects.
    */
   nir_constant *constant_initializer;

   /**
    * For variables that are in an interface block or are an instance of an
    * interface block, this is the \c GLSL_TYPE_INTERFACE type for that block.
    *
    * \sa ir_variable::location
    */
   const struct glsl_type *interface_type;
} nir_variable;

typedef struct {
   struct exec_node node;

   unsigned num_components; /** < number of vector components */
   unsigned num_array_elems; /** < size of array (0 for no array) */

   /** for liveness analysis, the index in the bit-array of live variables */
   unsigned index;

   /** only for debug purposes, can be NULL */
   const char *name;

   /** whether this register is local (per-function) or global (per-shader) */
   bool is_global;

   /**
    * If this flag is set to true, then accessing channels >= num_components
    * is well-defined, and simply spills over to the next array element. This
    * is useful for backends that can do per-component accessing, in
    * particular scalar backends. By setting this flag and making
    * num_components equal to 1, structures can be packed tightly into
    * registers and then registers can be accessed per-component to get to
    * each structure member, even if it crosses vec4 boundaries.
    */
   bool is_packed;

   /** set of nir_instr's where this register is used (read from) */
   struct set *uses;

   /** set of nir_instr's where this register is defined (written to) */
   struct set *defs;

   /** set of ifs where this register is used as a condition */
   struct set *if_uses;
} nir_register;

typedef enum {
   nir_instr_type_alu,
   nir_instr_type_call,
   nir_instr_type_texture,
   nir_instr_type_intrinsic,
   nir_instr_type_load_const,
   nir_instr_type_jump,
   nir_instr_type_ssa_undef,
   nir_instr_type_phi,
} nir_instr_type;

typedef struct {
   struct exec_node node;
   nir_instr_type type;
   struct nir_block *block;
} nir_instr;

#define nir_instr_next(instr) \
   exec_node_data(nir_instr, (instr)->node.next, node)

#define nir_instr_prev(instr) \
   exec_node_data(nir_instr, (instr)->node.prev, node)

typedef struct {
   /** for debugging only, can be NULL */
   const char* name;

   /** index into the bit-array for liveness analysis */
   unsigned index;

   nir_instr *parent_instr;

   struct set *uses;
   struct set *if_uses;

   uint8_t num_components;
} nir_ssa_def;

struct nir_src;

typedef struct {
   nir_register *reg;
   struct nir_src *indirect; /** < NULL for no indirect offset */
   unsigned base_offset;

   /* TODO use-def chain goes here */
} nir_reg_src;

typedef struct {
   nir_register *reg;
   struct nir_src *indirect; /** < NULL for no indirect offset */
   unsigned base_offset;

   /* TODO def-use chain goes here */
} nir_reg_dest;

typedef struct nir_src {
   union {
      nir_reg_src reg;
      nir_ssa_def *ssa;
   };

   bool is_ssa;
} nir_src;

typedef struct {
   union {
      nir_reg_dest reg;
      nir_ssa_def ssa;
   };

   bool is_ssa;
} nir_dest;

nir_src nir_src_copy(nir_src src, void *mem_ctx);
nir_dest nir_dest_copy(nir_dest dest, void *mem_ctx);

typedef struct {
   nir_src src;

   /**
    * \name input modifiers
    */
   /*@{*/
   /**
    * For inputs interpreted as a floating point, flips the sign bit. For inputs
    * interpreted as an integer, performs the two's complement negation.
    */
   bool negate;

   /**
    * Clears the sign bit for floating point values, and computes the integer
    * absolute value for integers. Note that the negate modifier acts after
    * the absolute value modifier, therefore if both are set then all inputs
    * will become negative.
    */
   bool abs;
   /*@}*/

   /**
    * For each input component, says which component of the register it is
    * chosen from. Note that which elements of the swizzle are used and which
    * are ignored are based on the write mask for most opcodes - for example,
    * a statement like "foo.xzw = bar.zyx" would have a writemask of 1101b and
    * a swizzle of {2, x, 1, 0} where x means "don't care."
    */
   uint8_t swizzle[4];
} nir_alu_src;

typedef struct {
   nir_dest dest;

   /**
    * \name saturate output modifier
    *
    * Only valid for opcodes that output floating-point numbers. Clamps the
    * output to between 0.0 and 1.0 inclusive.
    */

   bool saturate;

   unsigned write_mask : 4; /* ignored if dest.is_ssa is true */
} nir_alu_dest;

#define OPCODE(name, num_inputs, per_component, output_size, output_type, \
               input_sizes, input_types) \
   nir_op_##name,

#define LAST_OPCODE(name) nir_last_opcode = nir_op_##name,

typedef enum {
#include "nir_opcodes.h"
   nir_num_opcodes = nir_last_opcode + 1
} nir_op;

#undef OPCODE
#undef LAST_OPCODE

typedef enum {
   nir_type_float,
   nir_type_int,
   nir_type_unsigned,
   nir_type_bool
} nir_alu_type;

typedef struct {
   const char *name;

   unsigned num_inputs;

   /**
    * If true, the opcode acts in the standard, per-component manner; the
    * operation is performed on each component (except the ones that are masked
    * out) with the input being taken from the input swizzle for that component.
    *
    * If false, the size of the output and inputs are explicitly given; swizzle
    * and writemask are still in effect, but if the output component is masked
    * out, then the input component may still be in use.
    *
    * The size of some of the inputs may be given (i.e. non-zero) even though
    * per_component is false; in that case, each component of the input acts
    * per-component, while the rest of the inputs and the output are normal.
    * For example, for conditional select the condition is per-component but
    * everything else is normal.
    */
   bool per_component;

   /**
    * If per_component is false, the number of components in the output.
    */
   unsigned output_size;

   /**
    * The type of vector that the instruction outputs. Note that this
    * determines whether the saturate modifier is allowed.
    */

   nir_alu_type output_type;

   /**
    * If per_component is false, the number of components in each input.
    */
   unsigned input_sizes[4];

   /**
    * The type of vector that each input takes. Note that negate is only
    * allowed on inputs with int or float type, and behaves differently on the
    * two, and absolute value is only allowed on float type inputs.
    */
   nir_alu_type input_types[4];
} nir_op_info;

extern const nir_op_info nir_op_infos[nir_num_opcodes];

typedef struct nir_alu_instr {
   nir_instr instr;
   nir_op op;
   bool has_predicate;
   nir_src predicate;
   nir_alu_dest dest;
   nir_alu_src src[];
} nir_alu_instr;

/* is this source channel used? */
static inline bool
nir_alu_instr_channel_used(nir_alu_instr *instr, unsigned src, unsigned channel)
{
   if (nir_op_infos[instr->op].input_sizes[src] > 0)
      return channel < nir_op_infos[instr->op].input_sizes[src];

   return (instr->dest.write_mask >> channel) & 1;
}

typedef enum {
   nir_deref_type_var,
   nir_deref_type_array,
   nir_deref_type_struct
} nir_deref_type;

typedef struct nir_deref {
   nir_deref_type deref_type;
   struct nir_deref *child;
   const struct glsl_type *type;
} nir_deref;

typedef struct {
   nir_deref deref;

   nir_variable *var;
} nir_deref_var;

typedef struct {
   nir_deref deref;

   unsigned base_offset;
   bool has_indirect;
   nir_src indirect;
} nir_deref_array;

typedef struct {
   nir_deref deref;

   const char *elem;
} nir_deref_struct;

#define nir_deref_as_var(_deref) exec_node_data(nir_deref_var, _deref, deref)
#define nir_deref_as_array(_deref) \
   exec_node_data(nir_deref_array, _deref, deref)
#define nir_deref_as_struct(_deref) \
   exec_node_data(nir_deref_struct, _deref, deref)

typedef struct {
   nir_instr instr;

   unsigned num_params;
   nir_deref_var **params;
   nir_deref_var *return_deref;

   bool has_predicate;
   nir_src predicate;

   struct nir_function_overload *callee;
} nir_call_instr;

#define INTRINSIC(name, num_srcs, src_components, has_dest, dest_components, \
                  num_variables, num_indices, flags) \
   nir_intrinsic_##name,

#define LAST_INTRINSIC(name) nir_last_intrinsic = nir_intrinsic_##name,

typedef enum {
#include "nir_intrinsics.h"
   nir_num_intrinsics = nir_last_intrinsic + 1
} nir_intrinsic_op;

#undef INTRINSIC
#undef LAST_INTRINSIC

typedef struct {
   nir_instr instr;

   nir_intrinsic_op intrinsic;

   nir_dest dest;

   int const_index[3];

   nir_deref_var *variables[2];

   bool has_predicate;
   nir_src predicate;

   nir_src src[];
} nir_intrinsic_instr;

/**
 * \name NIR intrinsics semantic flags
 *
 * information about what the compiler can do with the intrinsics.
 *
 * \sa nir_intrinsic_info::flags
 */
/*@{*/
/**
 * whether the intrinsic can be safely eliminated if none of its register
 * outputs are being used.
 */
#define NIR_INTRINSIC_CAN_ELIMINATE (1 << 0)

/**
 * Whether the intrinsic can be reordered with respect to any other intrinsic,
 * i.e. whether the only reodering dependencies of the intrinsic are due to the
 * register reads/writes.
 */
#define NIR_INTRINSIC_CAN_REORDER   (1 << 1)
/*@}*/

#define NIR_INTRINSIC_MAX_INPUTS 4

typedef struct {
   const char *name;

   unsigned num_srcs; /** < number of register/SSA inputs */

   /** number of components of each input register */
   unsigned src_components[NIR_INTRINSIC_MAX_INPUTS];

   bool has_dest;

   /** number of components of each output register */
   unsigned dest_components;

   /** the number of inputs/outputs that are variables */
   unsigned num_variables;

   /** the number of constant indices used by the intrinsic */
   unsigned num_indices;

   /** semantic flags for calls to this intrinsic */
   unsigned flags;
} nir_intrinsic_info;

extern const nir_intrinsic_info nir_intrinsic_infos[nir_num_intrinsics];

/**
 * \group texture information
 *
 * This gives semantic information about textures which is useful to the
 * frontend, the backend, and lowering passes, but not the optimizer.
 */

typedef enum {
   nir_tex_src_coord,
   nir_tex_src_projector,
   nir_tex_src_comparitor, /* shadow comparitor */
   nir_tex_src_offset,
   nir_tex_src_bias,
   nir_tex_src_lod,
   nir_tex_src_ms_index, /* MSAA sample index */
   nir_tex_src_ddx,
   nir_tex_src_ddy,
   nir_tex_src_sampler_index, /* < dynamically uniform indirect index */
   nir_num_texinput_types
} nir_texinput_type;

typedef enum {
   nir_texop_tex,                /**< Regular texture look-up */
   nir_texop_txb,                /**< Texture look-up with LOD bias */
   nir_texop_txl,                /**< Texture look-up with explicit LOD */
   nir_texop_txd,                /**< Texture look-up with partial derivatvies */
   nir_texop_txf,                /**< Texel fetch with explicit LOD */
   nir_texop_txf_ms,                /**< Multisample texture fetch */
   nir_texop_txs,                /**< Texture size */
   nir_texop_lod,                /**< Texture lod query */
   nir_texop_tg4,                /**< Texture gather */
   nir_texop_query_levels       /**< Texture levels query */
} nir_texop;

typedef struct {
   nir_instr instr;

   bool has_predicate;
   nir_src predicate;

   enum glsl_sampler_dim sampler_dim;
   nir_alu_type dest_type;

   nir_texop op;
   nir_dest dest;
   nir_src src[4];
   nir_texinput_type src_type[4];
   unsigned num_srcs, coord_components;
   bool is_array, is_shadow;

   /**
    * If is_shadow is true, whether this is the old-style shadow that outputs 4
    * components or the new-style shadow that outputs 1 component.
    */
   bool is_new_style_shadow;

   /* constant offset - must be 0 if the offset source is used */
   int const_offset[4];

   /* gather component selector */
   unsigned component : 2;

   unsigned sampler_index;
   nir_deref_var *sampler; /* if this is NULL, use sampler_index instead */
} nir_tex_instr;

static inline unsigned
nir_tex_instr_dest_size(nir_tex_instr *instr)
{
   if (instr->op == nir_texop_txs) {
      unsigned ret;
      switch (instr->sampler_dim) {
         case GLSL_SAMPLER_DIM_1D:
         case GLSL_SAMPLER_DIM_BUF:
            ret = 1;
            break;
         case GLSL_SAMPLER_DIM_2D:
         case GLSL_SAMPLER_DIM_CUBE:
         case GLSL_SAMPLER_DIM_MS:
         case GLSL_SAMPLER_DIM_RECT:
         case GLSL_SAMPLER_DIM_EXTERNAL:
            ret = 2;
            break;
         case GLSL_SAMPLER_DIM_3D:
            ret = 3;
            break;
         default:
            assert(0);
            break;
      }
      if (instr->is_array)
         ret++;
      return ret;
   }

   if (instr->op == nir_texop_query_levels)
      return 2;

   if (instr->is_shadow && instr->is_new_style_shadow)
      return 1;

   return 4;
}

static inline unsigned
nir_tex_instr_src_size(nir_tex_instr *instr, unsigned src)
{
   if (instr->src_type[src] == nir_tex_src_coord)
      return instr->coord_components;


   if (instr->src_type[src] == nir_tex_src_offset ||
       instr->src_type[src] == nir_tex_src_ddx ||
       instr->src_type[src] == nir_tex_src_ddy) {
      if (instr->is_array)
         return instr->coord_components - 1;
      else
         return instr->coord_components;
   }

   return 1;
}

static inline int
nir_tex_instr_src_index(nir_tex_instr *instr, nir_texinput_type type)
{
   for (unsigned i = 0; i < instr->num_srcs; i++)
      if (instr->src_type[i] == type)
         return (int) i;

   return -1;
}

typedef struct {
   union {
      float f[4];
      int32_t i[4];
      uint32_t u[4];
   };
} nir_const_value;

typedef struct {
   nir_instr instr;

   union {
      nir_const_value value;
      nir_const_value *array;
   };

   unsigned num_components;

   /**
    * The number of constant array elements to be copied into the variable. If
    * this != 0, then value.array holds the array of size array_elems;
    * otherwise, value.value holds the single vector constant (the more common
    * case, and the only case for SSA destinations).
    */
   unsigned array_elems;

   bool has_predicate;
   nir_src predicate;

   nir_dest dest;
} nir_load_const_instr;

typedef enum {
   nir_jump_return,
   nir_jump_break,
   nir_jump_continue,
} nir_jump_type;

typedef struct {
   nir_instr instr;
   nir_jump_type type;
} nir_jump_instr;

/* creates a new SSA variable in an undefined state */

typedef struct {
   nir_instr instr;
   nir_ssa_def def;
} nir_ssa_undef_instr;

typedef struct {
   struct exec_node node;
   struct nir_block *pred;
   nir_src src;
} nir_phi_src;

typedef struct {
   nir_instr instr;

   struct exec_list srcs;
   nir_dest dest;
} nir_phi_instr;

#define nir_instr_as_alu(_instr) exec_node_data(nir_alu_instr, _instr, instr)
#define nir_instr_as_call(_instr) exec_node_data(nir_call_instr, _instr, instr)
#define nir_instr_as_jump(_instr) exec_node_data(nir_jump_instr, _instr, instr)
#define nir_instr_as_texture(_instr) \
   exec_node_data(nir_tex_instr, _instr, instr)
#define nir_instr_as_intrinsic(_instr) \
   exec_node_data(nir_intrinsic_instr, _instr, instr)
#define nir_instr_as_load_const(_instr) \
   exec_node_data(nir_load_const_instr, _instr, instr)
#define nir_instr_as_ssa_undef(_instr) \
   exec_node_data(nir_ssa_undef_instr, _instr, instr)
#define nir_instr_as_phi(_instr) \
   exec_node_data(nir_phi_instr, _instr, instr)


/*
 * Control flow
 *
 * Control flow consists of a tree of control flow nodes, which include
 * if-statements and loops. The leaves of the tree are basic blocks, lists of
 * instructions that always run start-to-finish. Each basic block also keeps
 * track of its successors (blocks which may run immediately after the current
 * block) and predecessors (blocks which could have run immediately before the
 * current block). Each function also has a start block and an end block which
 * all return statements point to (which is always empty). Together, all the
 * blocks with their predecessors and successors make up the control flow
 * graph (CFG) of the function. There are helpers that modify the tree of
 * control flow nodes while modifying the CFG appropriately; these should be
 * used instead of modifying the tree directly.
 */

typedef enum {
   nir_cf_node_block,
   nir_cf_node_if,
   nir_cf_node_loop,
   nir_cf_node_function
} nir_cf_node_type;

typedef struct nir_cf_node {
   struct exec_node node;
   nir_cf_node_type type;
   struct nir_cf_node *parent;
} nir_cf_node;

typedef struct nir_block {
   nir_cf_node cf_node;
   struct exec_list instr_list;

   unsigned index;

   /*
    * Each block can only have up to 2 successors, so we put them in a simple
    * array - no need for anything more complicated.
    */
   struct nir_block *successors[2];

   struct set *predecessors;
} nir_block;

#define nir_block_first_instr(block) \
   exec_node_data(nir_instr, exec_list_get_head(&(block)->instr_list), node)
#define nir_block_last_instr(block) \
   exec_node_data(nir_instr, exec_list_get_tail(&(block)->instr_list), node)

#define nir_foreach_instr(block, instr) \
   foreach_list_typed(nir_instr, instr, node, &(block)->instr_list)
#define nir_foreach_instr_reverse(block, instr) \
   foreach_list_typed_reverse(nir_instr, instr, node, &(block)->instr_list)
#define nir_foreach_instr_safe(block, instr) \
   foreach_list_typed_safe(nir_instr, instr, node, &(block)->instr_list)

typedef struct {
   nir_cf_node cf_node;
   nir_src condition;
   struct exec_list then_list;
   struct exec_list else_list;
} nir_if;

#define nir_if_first_then_node(if) \
   exec_node_data(nir_cf_node, exec_list_get_head(&(if)->then_list), node)
#define nir_if_last_then_node(if) \
   exec_node_data(nir_cf_node, exec_list_get_tail(&(if)->then_list), node)
#define nir_if_first_else_node(if) \
   exec_node_data(nir_cf_node, exec_list_get_head(&(if)->else_list), node)
#define nir_if_last_else_node(if) \
   exec_node_data(nir_cf_node, exec_list_get_tail(&(if)->else_list), node)

typedef struct {
   nir_cf_node cf_node;
   struct exec_list body;
} nir_loop;

#define nir_loop_first_cf_node(loop) \
   exec_node_data(nir_cf_node, exec_list_get_head(&(loop)->body), node)
#define nir_loop_last_cf_node(loop) \
   exec_node_data(nir_cf_node, exec_list_get_tail(&(loop)->body), node)

typedef struct {
   nir_cf_node cf_node;

   /** pointer to the overload of which this is an implementation */
   struct nir_function_overload *overload;

   struct exec_list body; /** < list of nir_cf_node */

   nir_block *start_block, *end_block;

   /** list for all local variables in the function */
   struct exec_list locals;

   /** array of variables used as parameters */
   unsigned num_params;
   nir_variable **params;

   /** variable used to hold the result of the function */
   nir_variable *return_var;

   /** list of local registers in the function */
   struct exec_list registers;

   /** next available local register index */
   unsigned reg_alloc;

   /** next available SSA value index */
   unsigned ssa_alloc;

   /* total number of basic blocks, only valid when block_index_dirty = false */
   unsigned num_blocks;

   bool block_index_dirty;
} nir_function_impl;

#define nir_cf_node_next(_node) \
   exec_node_data(nir_cf_node, exec_node_get_next(&(_node)->node), node)

#define nir_cf_node_prev(_node) \
   exec_node_data(nir_cf_node, exec_node_get_prev(&(_node)->node), node)

#define nir_cf_node_is_first(_node) \
   exec_node_is_head_sentinel((_node)->node.prev)

#define nir_cf_node_is_last(_node) \
   exec_node_is_tail_sentinel((_node)->node.next)

#define nir_cf_node_as_block(node) \
   exec_node_data(nir_block, node, cf_node)

#define nir_cf_node_as_if(node) \
   exec_node_data(nir_if, node, cf_node)

#define nir_cf_node_as_loop(node) \
   exec_node_data(nir_loop, node, cf_node)

#define nir_cf_node_as_function(node) \
   exec_node_data(nir_function_impl, node, cf_node)

typedef enum {
   nir_parameter_in,
   nir_parameter_out,
   nir_parameter_inout,
} nir_parameter_type;

typedef struct {
   nir_parameter_type param_type;
   const struct glsl_type *type;
} nir_parameter;

typedef struct nir_function_overload {
   struct exec_node node;

   unsigned num_params;
   nir_parameter *params;
   const struct glsl_type *return_type;

   nir_function_impl *impl; /** < NULL if the overload is only declared yet */

   /** pointer to the function of which this is an overload */
   struct nir_function *function;
} nir_function_overload;

typedef struct nir_function {
   struct exec_node node;

   struct exec_list overload_list;
   const char *name;
} nir_function;

#define nir_function_first_overload(func) \
   exec_node_data(nir_function_overload, \
                  exec_list_get_head(&(func)->overload_list), node)

typedef struct nir_shader {
   /** hash table of name -> uniform */
   struct hash_table *uniforms;

   /** hash table of name -> input */
   struct hash_table *inputs;

   /** hash table of name -> output */
   struct hash_table *outputs;

   /** list of global variables in the shader */
   struct exec_list globals;

   struct exec_list system_values;

   struct exec_list functions;

   /** list of global registers in the shader */
   struct exec_list registers;

   /** structures used in this shader */
   unsigned num_user_structures;
   struct glsl_type **user_structures;

   /** next available global register index */
   unsigned reg_alloc;
} nir_shader;

#define nir_foreach_overload(shader, overload) \
   foreach_list_typed(nir_function, func, node, &(shader)->functions) \
      foreach_list_typed(nir_function_overload, overload, node, \
                         &(func)->overload_list)

#ifdef __cplusplus
} /* extern "C" */
#endif
