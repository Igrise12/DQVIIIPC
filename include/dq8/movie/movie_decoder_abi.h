/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 DQ8-Native contributors */
#ifndef DQ8_MOVIE_DECODER_ABI_H
#define DQ8_MOVIE_DECODER_ABI_H

/* The C ABI of dq8-movie, the shared library that decodes the game's MPEG-2 movie pictures for the sceMpeg
 * HLE (architecture §7.2, ADR-0015, Docs/milestones/M4-playable.md). It wraps FFmpeg's libavcodec (mpeg2video
 * only, libavutil; no libavformat, no libswscale) and is loaded at run time (dlopen/LoadLibrary), so a user
 * can replace it and dq8-game runs without it (movies are then skipped). Only the dq8_mvd_* functions below
 * are exported. Every call on one dq8_mvd comes from one thread (the machine thread). Decoding is
 * deterministic: one thread, bit-exact flags, a fixed IDCT. Fixed on M4 day 0; implemented by M4 session C.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DQ8_MVD_ABI_VERSION 1u

typedef struct dq8_mvd dq8_mvd;

typedef struct dq8_mvd_options {
    uint32_t struct_size; /* sizeof(dq8_mvd_options) */
    uint32_t max_width;   /* largest picture the caller accepts (512 for this game) */
    uint32_t max_height;
    uint32_t flags; /* none defined yet: 0 */
} dq8_mvd_options;

/* A decoded picture in display order: 4:2:0 planes, valid until the next send, receive, reset or destroy. */
typedef struct dq8_mvd_picture {
    uint32_t width;
    uint32_t height;
    uint32_t y_stride;
    uint32_t c_stride;
    uint32_t coding_type; /* 1 I, 2 P, 3 B */
    int64_t pts90k;       /* the caller's timestamp, passed through */
    const uint8_t* y;
    const uint8_t* cb;
    const uint8_t* cr;
} dq8_mvd_picture;

/* DQ8_MVD_ABI_VERSION of the library; the host refuses another version. */
uint32_t dq8_mvd_abi_version(void);
/* The backend's description ("libavcodec 61.3.100"), for logs only, never guest-visible. Returns the length.
 */
size_t dq8_mvd_backend_version(char* out, size_t size);
/* NULL on failure, with a message in `error`. */
dq8_mvd* dq8_mvd_create(const dq8_mvd_options* options, char* error, size_t error_size);
void dq8_mvd_destroy(dq8_mvd* decoder);
/* Forgets every picture in flight (a seek or a new stream). */
void dq8_mvd_reset(dq8_mvd* decoder);
/* One coded picture (from its picture start code to the next picture's), with the caller's timestamp.
 * 0 on success, negative on a decoder error. */
int dq8_mvd_send(dq8_mvd* decoder, const void* coded_picture, size_t bytes, int64_t pts90k);
/* End of stream: the held reference picture becomes receivable. */
int dq8_mvd_flush(dq8_mvd* decoder);
/* 1 and a picture, 0 when none is ready, negative on error. */
int dq8_mvd_receive(dq8_mvd* decoder, dq8_mvd_picture* out);

#ifdef __cplusplus
}
#endif

#endif /* DQ8_MOVIE_DECODER_ABI_H */
