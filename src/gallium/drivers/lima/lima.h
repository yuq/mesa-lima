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
 *
 */

#ifndef H_DRM_LIMA
#define H_DRM_LIMA

#include <stdbool.h>
#include <stdint.h>

enum lima_gpu_type {
   GPU_MALI400,
};

enum lima_bo_handle_type {
   lima_bo_handle_type_gem_flink_name = 0,
   lima_bo_handle_type_kms = 1,
};

struct lima_device_info {
   enum lima_gpu_type gpu_type;
   uint32_t num_pp;
};

struct lima_bo_create_request {
   uint32_t size;
   uint32_t flags;
};

typedef struct lima_device *lima_device_handle;
typedef struct lima_bo *lima_bo_handle;
typedef struct lima_submit *lima_submit_handle;

struct lima_bo_import_result {
   lima_bo_handle bo;
   uint32_t size;
};

int lima_device_create(int fd, lima_device_handle *dev);
void lima_device_delete(lima_device_handle dev);

int lima_device_query_info(lima_device_handle dev, struct lima_device_info *info);

int lima_bo_create(lima_device_handle dev, struct lima_bo_create_request *request,
                   lima_bo_handle *bo_handle);
int lima_bo_free(lima_bo_handle bo);
void *lima_bo_map(lima_bo_handle bo);
int lima_bo_unmap(lima_bo_handle bo);
int lima_bo_export(lima_bo_handle bo, enum lima_bo_handle_type type,
                   uint32_t *handle);
int lima_bo_import(lima_device_handle dev, enum lima_bo_handle_type type,
                   uint32_t handle, struct lima_bo_import_result *result);

#define LIMA_BO_WAIT_FLAG_READ   0x01
#define LIMA_BO_WAIT_FLAG_WRITE  0x02

int lima_bo_wait(lima_bo_handle bo, uint32_t op, uint64_t timeout_ns, bool relative);

int lima_va_range_alloc(lima_device_handle dev, uint32_t size, uint32_t *va);
int lima_va_range_free(lima_device_handle dev, uint32_t size, uint32_t va);

int lima_bo_va_map(lima_bo_handle bo, uint32_t va, uint32_t flags);
int lima_bo_va_unmap(lima_bo_handle bo, uint32_t va);

#define LIMA_SUBMIT_BO_FLAG_READ   0x01
#define LIMA_SUBMIT_BO_FLAG_WRITE  0x02

int lima_submit_create(lima_device_handle dev, uint32_t pipe, lima_submit_handle *submit);
void lima_submit_delete(lima_submit_handle submit);
int lima_submit_add_bo(lima_submit_handle submit, lima_bo_handle bo, uint32_t flags);
void lima_submit_remove_bo(lima_submit_handle submit, lima_bo_handle bo);
void lima_submit_set_frame(lima_submit_handle submit, void *frame, uint32_t size);
int lima_submit_start(lima_submit_handle submit);
int lima_submit_wait(lima_submit_handle submit, uint64_t timeout_ns, bool relative);

#endif /* H_DRM_LIMA */
