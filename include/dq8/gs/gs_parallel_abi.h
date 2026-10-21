/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 DQ8-Native contributors */
#ifndef DQ8_GS_PARALLEL_ABI_H
#define DQ8_GS_PARALLEL_ABI_H

/* The C ABI of dq8-gs-parallel, the shared library that holds paraLLEl-GS (LGPL-3.0-or-later) with Granite
 * and our wrapper (ADR-0004, Docs/milestones/M3-boots.md). The host loads it at run time
 * (dlopen/LoadLibrary), so a user can relink or replace it and dq8-game runs without Vulkan when another
 * backend is chosen. Only the dq8_gsp_* functions below are exported. Every call on one dq8_gsp must come
 * from one thread (the render thread); readbacks block it. Implemented by M3 session D. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DQ8_GSP_ABI_VERSION 1u

typedef struct dq8_gsp dq8_gsp;

enum dq8_gsp_flags {
    DQ8_GSP_HIRES_SCANOUT = 1u << 0,         /* scan out at the internal resolution */
    DQ8_GSP_SUPERSAMPLED_TEXTURES = 1u << 1, /* paraLLEl-GS super-sampled textures */
    DQ8_GSP_BACKBUFFER_PROMOTION = 1u << 2   /* paraLLEl-GS hack: promote the back buffer for 2x+ pictures */
};

typedef struct dq8_gsp_options {
    uint32_t struct_size;            /* sizeof(dq8_gsp_options) */
    uint32_t super_sampling;         /* 1, 2, 4, 8 or 16 */
    uint32_t flags;                  /* dq8_gsp_flags */
    const char* pipeline_cache_path; /* may be NULL: no persistent pipeline cache */
} dq8_gsp_options;

typedef struct dq8_gsp_frame_info {
    uint32_t width; /* scanout size in pixels, 0 when nothing is displayed */
    uint32_t height;
    uint64_t gpu_ns; /* GPU time of the frame when timestamps are available, else 0 */
} dq8_gsp_frame_info;

/* The loader resolves these by name; the typedefs give it the signatures. */
typedef uint32_t (*dq8_gsp_abi_version_fn)(void);
typedef dq8_gsp* (*dq8_gsp_create_fn)(const dq8_gsp_options* options, char* error, size_t error_size);
typedef void (*dq8_gsp_destroy_fn)(dq8_gsp* gs);
typedef void (*dq8_gsp_reset_fn)(dq8_gsp* gs);
typedef void (*dq8_gsp_gif_transfer_fn)(dq8_gsp* gs, uint32_t path, const void* qwords, size_t bytes);
typedef int (*dq8_gsp_vsync_fn)(dq8_gsp* gs, const void* priv_page_8k, uint32_t odd_field,
                                dq8_gsp_frame_info* out);
typedef int (*dq8_gsp_read_scanout_rgba_fn)(dq8_gsp* gs, void* dst, size_t dst_size, uint32_t* width,
                                            uint32_t* height);
typedef size_t (*dq8_gsp_read_transfer_fn)(dq8_gsp* gs, void* dst, size_t bytes);
typedef void (*dq8_gsp_flush_fn)(dq8_gsp* gs);

/* DQ8_GSP_ABI_VERSION of the library. */
uint32_t dq8_gsp_abi_version(void);
/* NULL on failure, with a message in `error` (no Vulkan device, missing features, ...). */
dq8_gsp* dq8_gsp_create(const dq8_gsp_options* options, char* error, size_t error_size);
void dq8_gsp_destroy(dq8_gsp* gs);
void dq8_gsp_reset(dq8_gsp* gs);
/* path: 1, 2 or 3; bytes: a multiple of 16. */
void dq8_gsp_gif_transfer(dq8_gsp* gs, uint32_t path, const void* qwords, size_t bytes);
/* The 8 KiB privileged-register page (runtime/gs/priv_regs.h layout). Returns 0 on success. */
int dq8_gsp_vsync(dq8_gsp* gs, const void* priv_page_8k, uint32_t odd_field, dq8_gsp_frame_info* out);
/* The last scanout as RGBA8, rows top to bottom. Returns 0 on success, non-zero when dst is too small (width
 * and height are still set) or nothing is displayed. Blocks. */
int dq8_gsp_read_scanout_rgba(dq8_gsp* gs, void* dst, size_t dst_size, uint32_t* width, uint32_t* height);
/* Local->host transfer data; returns bytes written. Blocks. */
size_t dq8_gsp_read_transfer(dq8_gsp* gs, void* dst, size_t bytes);
void dq8_gsp_flush(dq8_gsp* gs);

#ifdef __cplusplus
}
#endif

#endif /* DQ8_GS_PARALLEL_ABI_H */
