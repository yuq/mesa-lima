/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
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

#ifndef RADV_SHADER_H
#define RADV_SHADER_H

#include "radv_debug.h"
#include "radv_private.h"

#include "nir/nir.h"

/* descriptor index into scratch ring offsets */
#define RING_SCRATCH 0
#define RING_ESGS_VS 1
#define RING_ESGS_GS 2
#define RING_GSVS_VS 3
#define RING_GSVS_GS 4
#define RING_HS_TESS_FACTOR 5
#define RING_HS_TESS_OFFCHIP 6
#define RING_PS_SAMPLE_POSITIONS 7

// Match MAX_SETS from radv_descriptor_set.h
#define RADV_UD_MAX_SETS MAX_SETS

struct radv_shader_module {
	struct nir_shader *nir;
	unsigned char sha1[20];
	uint32_t size;
	char data[0];
};

struct radv_userdata_info {
	int8_t sgpr_idx;
	uint8_t num_sgprs;
	bool indirect;
	uint32_t indirect_offset;
};

struct radv_userdata_locations {
	struct radv_userdata_info descriptor_sets[RADV_UD_MAX_SETS];
	struct radv_userdata_info shader_data[AC_UD_MAX_UD];
};

struct radv_vs_output_info {
	uint8_t	vs_output_param_offset[VARYING_SLOT_MAX];
	uint8_t clip_dist_mask;
	uint8_t cull_dist_mask;
	uint8_t param_exports;
	bool writes_pointsize;
	bool writes_layer;
	bool writes_viewport_index;
	bool export_prim_id;
	unsigned pos_exports;
};

struct radv_es_output_info {
	uint32_t esgs_itemsize;
};

struct radv_shader_variant_info {
	struct radv_userdata_locations user_sgprs_locs;
	struct ac_shader_info info;
	unsigned num_user_sgprs;
	unsigned num_input_sgprs;
	unsigned num_input_vgprs;
	unsigned private_mem_vgprs;
	bool need_indirect_descriptor_sets;
	struct {
		struct {
			struct radv_vs_output_info outinfo;
			struct radv_es_output_info es_info;
			unsigned vgpr_comp_cnt;
			bool as_es;
			bool as_ls;
			uint64_t outputs_written;
		} vs;
		struct {
			unsigned num_interp;
			uint32_t input_mask;
			uint32_t flat_shaded_mask;
			bool can_discard;
			bool early_fragment_test;
		} fs;
		struct {
			unsigned block_size[3];
		} cs;
		struct {
			unsigned vertices_in;
			unsigned vertices_out;
			unsigned output_prim;
			unsigned invocations;
			unsigned gsvs_vertex_size;
			unsigned max_gsvs_emit_size;
			unsigned es_type; /* GFX9: VS or TES */
		} gs;
		struct {
			unsigned tcs_vertices_out;
			/* Which outputs are actually written */
			uint64_t outputs_written;
			/* Which patch outputs are actually written */
			uint32_t patch_outputs_written;

		} tcs;
		struct {
			struct radv_vs_output_info outinfo;
			struct radv_es_output_info es_info;
			bool as_es;
			unsigned primitive_mode;
			enum gl_tess_spacing spacing;
			bool ccw;
			bool point_mode;
		} tes;
	};
};

struct radv_shader_variant {
	uint32_t ref_count;

	struct radeon_winsys_bo *bo;
	uint64_t bo_offset;
	struct ac_shader_config config;
	uint32_t code_size;
	struct radv_shader_variant_info info;
	unsigned rsrc1;
	unsigned rsrc2;

	/* debug only */
	uint32_t *spirv;
	uint32_t spirv_size;
	struct nir_shader *nir;
	char *disasm_string;

	struct list_head slab_list;
};

struct radv_shader_slab {
	struct list_head slabs;
	struct list_head shaders;
	struct radeon_winsys_bo *bo;
	uint64_t size;
	char *ptr;
};

void
radv_optimize_nir(struct nir_shader *shader);

nir_shader *
radv_shader_compile_to_nir(struct radv_device *device,
			   struct radv_shader_module *module,
			   const char *entrypoint_name,
			   gl_shader_stage stage,
			   const VkSpecializationInfo *spec_info);

void *
radv_alloc_shader_memory(struct radv_device *device,
			  struct radv_shader_variant *shader);

void
radv_destroy_shader_slabs(struct radv_device *device);

struct radv_shader_variant *
radv_shader_variant_create(struct radv_device *device,
			   struct radv_shader_module *module,
			   struct nir_shader *const *shaders,
			   int shader_count,
			   struct radv_pipeline_layout *layout,
			   const struct ac_shader_variant_key *key,
			   void **code_out,
			   unsigned *code_size_out);

struct radv_shader_variant *
radv_create_gs_copy_shader(struct radv_device *device, struct nir_shader *nir,
			   void **code_out, unsigned *code_size_out,
			   bool multiview);

void
radv_shader_variant_destroy(struct radv_device *device,
			    struct radv_shader_variant *variant);

const char *
radv_get_shader_name(struct radv_shader_variant *var, gl_shader_stage stage);

void
radv_shader_dump_stats(struct radv_device *device,
		       struct radv_shader_variant *variant,
		       gl_shader_stage stage,
		       FILE *file);

static inline bool
radv_can_dump_shader(struct radv_device *device,
		     struct radv_shader_module *module)
{
	/* Only dump non-meta shaders, useful for debugging purposes. */
	return device->instance->debug_flags & RADV_DEBUG_DUMP_SHADERS &&
	       module && !module->nir;
}

static inline bool
radv_can_dump_shader_stats(struct radv_device *device,
			   struct radv_shader_module *module)
{
	/* Only dump non-meta shader stats. */
	return device->instance->debug_flags & RADV_DEBUG_DUMP_SHADER_STATS &&
	       module && !module->nir;
}

#endif
