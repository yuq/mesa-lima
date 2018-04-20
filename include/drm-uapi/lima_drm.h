/*
 * Copyright (C) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef __LIMA_DRM_H__
#define __LIMA_DRM_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define LIMA_INFO_GPU_MALI400 0x00
#define LIMA_INFO_GPU_MALI450 0x01

struct drm_lima_info {
	__u32 gpu_id;   /* out */
	__u32 num_pp;   /* out */
	__u64 va_start; /* out */
	__u64 va_end;   /* out */
};

struct drm_lima_gem_create {
	__u32 size;    /* in */
	__u32 flags;   /* in */
	__u32 handle;  /* out */
	__u32 pad;
};

struct drm_lima_gem_info {
	__u32 handle;  /* in */
	__u32 pad;
	__u64 offset;  /* out */
};

#define LIMA_VA_OP_MAP    1
#define LIMA_VA_OP_UNMAP  2

struct drm_lima_gem_va {
	__u32 handle;  /* in */
	__u32 op;      /* in */
	__u32 flags;   /* in */
	__u32 va;      /* in */
};

#define LIMA_SUBMIT_BO_READ   0x01
#define LIMA_SUBMIT_BO_WRITE  0x02

struct drm_lima_gem_submit_bo {
	__u32 handle;  /* in */
	__u32 flags;   /* in */
};

struct drm_lima_m400_gp_frame {
	__u32 vs_cmd_start;
	__u32 vs_cmd_end;
	__u32 plbu_cmd_start;
	__u32 plbu_cmd_end;
	__u32 tile_heap_start;
	__u32 tile_heap_end;
};

struct drm_lima_pp_frame_reg {
	__u32 plbu_array_address;
	__u32 render_address;
	__u32 unused_0;
	__u32 flags;
	__u32 clear_value_depth;
	__u32 clear_value_stencil;
	__u32 clear_value_color;
	__u32 clear_value_color_1;
	__u32 clear_value_color_2;
	__u32 clear_value_color_3;
	__u32 width;
	__u32 height;
	__u32 fragment_stack_address;
	__u32 fragment_stack_size;
	__u32 unused_1;
	__u32 unused_2;
	__u32 one;
	__u32 supersampled_height;
	__u32 dubya;
	__u32 onscreen;
	__u32 blocking;
	__u32 scale;
	__u32 foureight;
	__u32 _pad;
};

struct drm_lima_pp_wb_reg {
	__u32 type;
	__u32 address;
	__u32 pixel_format;
	__u32 downsample_factor;
	__u32 pixel_layout;
	__u32 pitch;
	__u32 mrt_bits;
	__u32 mrt_pitch;
	__u32 zero;
	__u32 unused0;
	__u32 unused1;
	__u32 unused2;
};

struct drm_lima_m400_pp_frame {
	struct drm_lima_pp_frame_reg frame;
	struct drm_lima_pp_wb_reg wb[3];
	__u32 plbu_array_address[4];
	__u32 fragment_stack_address[4];
	__u32 num_pp;
	__u32 _pad;
};

struct drm_lima_m450_pp_frame {
	struct drm_lima_pp_frame_reg frame;
	struct drm_lima_pp_wb_reg wb[3];
	__u32 dlbu_regs[4];
	__u32 fragment_stack_address[8];
};

#define LIMA_PIPE_GP  0x00
#define LIMA_PIPE_PP  0x01

struct drm_lima_gem_submit_in {
	__u32 ctx;
	__u32 pipe;
	__u32 nr_bos;
	__u32 frame_size;
	__u64 bos;
	__u64 frame;
};

struct drm_lima_gem_submit_out {
	__u32 fence;
	__u32 done;
};

union drm_lima_gem_submit {
	struct drm_lima_gem_submit_in in;
	struct drm_lima_gem_submit_out out;
};

struct drm_lima_wait_fence {
	__u32 pipe;        /* in */
	__u32 fence;       /* in */
	__u64 timeout_ns;  /* in */
	__u32 ctx;         /* in */
	__u32 _pad;
};

#define LIMA_GEM_WAIT_READ   0x01
#define LIMA_GEM_WAIT_WRITE  0x02

struct drm_lima_gem_wait {
	__u32 handle;      /* in */
	__u32 op;          /* in */
	__u64 timeout_ns;  /* in */
};

#define LIMA_CTX_OP_CREATE 1
#define LIMA_CTX_OP_FREE   2

struct drm_lima_ctx {
	__u32 op;          /* in */
	__u32 id;          /* in/out */
};

#define DRM_LIMA_INFO        0x00
#define DRM_LIMA_GEM_CREATE  0x01
#define DRM_LIMA_GEM_INFO    0x02
#define DRM_LIMA_GEM_VA      0x03
#define DRM_LIMA_GEM_SUBMIT  0x04
#define DRM_LIMA_WAIT_FENCE  0x05
#define DRM_LIMA_GEM_WAIT    0x06
#define DRM_LIMA_CTX         0x07

#define DRM_IOCTL_LIMA_INFO DRM_IOR(DRM_COMMAND_BASE + DRM_LIMA_INFO, struct drm_lima_info)
#define DRM_IOCTL_LIMA_GEM_CREATE DRM_IOWR(DRM_COMMAND_BASE + DRM_LIMA_GEM_CREATE, struct drm_lima_gem_create)
#define DRM_IOCTL_LIMA_GEM_INFO DRM_IOWR(DRM_COMMAND_BASE + DRM_LIMA_GEM_INFO, struct drm_lima_gem_info)
#define DRM_IOCTL_LIMA_GEM_VA DRM_IOW(DRM_COMMAND_BASE + DRM_LIMA_GEM_VA, struct drm_lima_gem_va)
#define DRM_IOCTL_LIMA_GEM_SUBMIT DRM_IOWR(DRM_COMMAND_BASE + DRM_LIMA_GEM_SUBMIT, union drm_lima_gem_submit)
#define DRM_IOCTL_LIMA_WAIT_FENCE DRM_IOW(DRM_COMMAND_BASE + DRM_LIMA_WAIT_FENCE, struct drm_lima_wait_fence)
#define DRM_IOCTL_LIMA_GEM_WAIT DRM_IOW(DRM_COMMAND_BASE + DRM_LIMA_GEM_WAIT, struct drm_lima_gem_wait)
#define DRM_IOCTL_LIMA_CTX DRM_IOWR(DRM_COMMAND_BASE + DRM_LIMA_CTX, struct drm_lima_ctx)

#if defined(__cplusplus)
}
#endif

#endif /* __LIMA_DRM_H__ */
