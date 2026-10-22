// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// dq8-movie: the movie decoder library (include/dq8/movie/movie_decoder_abi.h, ADR-0015). It wraps FFmpeg's
// libavcodec mpeg2video decoder and exports only the dq8_mvd_* C functions (dq8_mvd.map). Decoding is made
// deterministic: one thread, AV_CODEC_FLAG_BITEXACT, the simple IDCT, and AV_CODEC_FLAG2_SHOW_ALL so that
// every coded picture comes out (pictures whose references are missing included), which keeps the output in
// step with the host's elementary-stream model. The decoder set-up and the send/receive/drain flow are
// adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp (MpegFfmpegDecoder).
#include "dq8/movie/movie_decoder_abi.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>

#if defined(_WIN32)
#define DQ8_MVD_EXPORT __declspec(dllexport)
#else
#define DQ8_MVD_EXPORT __attribute__((visibility("default")))
#endif

struct dq8_mvd {
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* current = nullptr; // the picture the last receive handed out
    std::deque<AVFrame*> ready; // decoded, not yet received
    uint32_t max_width = 0;
    uint32_t max_height = 0;
    bool draining = false; // flushed: reset before the next send
};

namespace {

void set_error(char* error, size_t size, const char* message) {
    if (error != nullptr && size > 0) {
        std::snprintf(error, size, "%s", message);
    }
}

void free_frames(dq8_mvd* d) {
    for (AVFrame* f : d->ready) {
        av_frame_free(&f);
    }
    d->ready.clear();
    av_frame_free(&d->current);
}

// Moves every frame the decoder has finished into `ready`. 0, or -1 on a decoder error.
int drain(dq8_mvd* d) {
    for (;;) {
        AVFrame* f = av_frame_alloc();
        if (f == nullptr) {
            return -1;
        }
        const int r = avcodec_receive_frame(d->ctx, f);
        if (r == 0) {
            d->ready.push_back(f);
            continue;
        }
        av_frame_free(&f);
        return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : -1;
    }
}

uint32_t coding_type(AVPictureType t) {
    switch (t) {
    case AV_PICTURE_TYPE_I:
        return 1;
    case AV_PICTURE_TYPE_P:
        return 2;
    case AV_PICTURE_TYPE_B:
        return 3;
    default:
        return 0;
    }
}

} // namespace

extern "C" {

DQ8_MVD_EXPORT uint32_t dq8_mvd_abi_version(void) {
    return DQ8_MVD_ABI_VERSION;
}

DQ8_MVD_EXPORT size_t dq8_mvd_backend_version(char* out, size_t size) {
    const unsigned v = avcodec_version();
    char text[64];
    const int n =
        std::snprintf(text, sizeof text, "libavcodec %u.%u.%u", v >> 16, (v >> 8) & 0xFFu, v & 0xFFu);
    if (out != nullptr && size > 0) {
        std::snprintf(out, size, "%s", text);
    }
    return n > 0 ? static_cast<size_t>(n) : 0;
}

DQ8_MVD_EXPORT dq8_mvd* dq8_mvd_create(const dq8_mvd_options* options, char* error, size_t error_size) {
    if (options == nullptr || options->struct_size < sizeof(dq8_mvd_options)) {
        set_error(error, error_size, "dq8_mvd_create: options missing or struct_size too small");
        return nullptr;
    }
    if (options->max_width == 0 || options->max_height == 0) {
        set_error(error, error_size, "dq8_mvd_create: max_width and max_height must not be 0");
        return nullptr;
    }
    if (options->flags != 0) {
        set_error(error, error_size, "dq8_mvd_create: unknown flags");
        return nullptr;
    }
    // FFmpeg's own messages: errors only, unless DQ8_MVD_VERBOSE is set.
    const char* verbose = std::getenv("DQ8_MVD_VERBOSE");
    av_log_set_level(verbose != nullptr && verbose[0] != '\0' && verbose[0] != '0' ? AV_LOG_VERBOSE
                                                                                   : AV_LOG_ERROR);

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if (codec == nullptr) {
        set_error(error, error_size, "dq8_mvd_create: this libavcodec has no mpeg2video decoder");
        return nullptr;
    }
    auto* d = new (std::nothrow) dq8_mvd;
    if (d == nullptr) {
        set_error(error, error_size, "dq8_mvd_create: out of memory");
        return nullptr;
    }
    d->max_width = options->max_width;
    d->max_height = options->max_height;
    d->ctx = avcodec_alloc_context3(codec);
    d->packet = av_packet_alloc();
    if (d->ctx == nullptr || d->packet == nullptr) {
        set_error(error, error_size, "dq8_mvd_create: out of memory");
        dq8_mvd_destroy(d);
        return nullptr;
    }
    d->ctx->thread_count = 1;
    d->ctx->thread_type = 0;
    d->ctx->flags |= AV_CODEC_FLAG_BITEXACT;
    d->ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    d->ctx->idct_algo = FF_IDCT_SIMPLE;
    if (const int r = avcodec_open2(d->ctx, codec, nullptr); r < 0) {
        char why[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(r, why, sizeof why);
        char message[256];
        std::snprintf(message, sizeof message, "dq8_mvd_create: avcodec_open2 failed: %s", why);
        set_error(error, error_size, message);
        dq8_mvd_destroy(d);
        return nullptr;
    }
    return d;
}

DQ8_MVD_EXPORT void dq8_mvd_destroy(dq8_mvd* decoder) {
    if (decoder == nullptr) {
        return;
    }
    free_frames(decoder);
    av_packet_free(&decoder->packet);
    avcodec_free_context(&decoder->ctx);
    delete decoder;
}

DQ8_MVD_EXPORT void dq8_mvd_reset(dq8_mvd* decoder) {
    if (decoder == nullptr) {
        return;
    }
    avcodec_flush_buffers(decoder->ctx);
    free_frames(decoder);
    decoder->draining = false;
}

DQ8_MVD_EXPORT int dq8_mvd_send(dq8_mvd* decoder, const void* coded_picture, size_t bytes, int64_t pts90k) {
    if (decoder == nullptr || decoder->draining || (coded_picture == nullptr && bytes != 0) || bytes == 0 ||
        bytes > static_cast<size_t>(INT32_MAX - AV_INPUT_BUFFER_PADDING_SIZE)) {
        return -1;
    }
    AVPacket* pkt = decoder->packet;
    if (av_new_packet(pkt, static_cast<int>(bytes)) < 0) {
        return -1;
    }
    std::memcpy(pkt->data, coded_picture, bytes);
    pkt->pts = pts90k;
    int r = 0;
    // The decoder takes the packet once the frames it holds have been received (the send/receive contract
    // guarantees one of the two calls makes progress).
    for (int attempt = 0; attempt < 4; ++attempt) {
        r = avcodec_send_packet(decoder->ctx, pkt);
        if (r != AVERROR(EAGAIN)) {
            break;
        }
        if (drain(decoder) < 0) {
            break;
        }
    }
    av_packet_unref(pkt);
    if (r < 0) {
        drain(decoder);
        return -1;
    }
    return drain(decoder);
}

DQ8_MVD_EXPORT int dq8_mvd_flush(dq8_mvd* decoder) {
    if (decoder == nullptr) {
        return -1;
    }
    if (!decoder->draining) {
        decoder->draining = true;
        const int r = avcodec_send_packet(decoder->ctx, nullptr);
        if (r < 0 && r != AVERROR_EOF) {
            return -1;
        }
    }
    return drain(decoder);
}

DQ8_MVD_EXPORT int dq8_mvd_receive(dq8_mvd* decoder, dq8_mvd_picture* out) {
    if (decoder == nullptr || out == nullptr) {
        return -1;
    }
    if (decoder->ready.empty() && drain(decoder) < 0) {
        return -1;
    }
    if (decoder->ready.empty()) {
        return 0;
    }
    av_frame_free(&decoder->current);
    decoder->current = decoder->ready.front();
    decoder->ready.pop_front();
    const AVFrame* f = decoder->current;
    if (f->format != AV_PIX_FMT_YUV420P || f->width <= 0 || f->height <= 0 ||
        static_cast<uint32_t>(f->width) > decoder->max_width ||
        static_cast<uint32_t>(f->height) > decoder->max_height || f->linesize[0] <= 0 ||
        f->linesize[1] <= 0 || f->linesize[1] != f->linesize[2]) {
        return -1;
    }
    *out = dq8_mvd_picture{};
    out->width = static_cast<uint32_t>(f->width);
    out->height = static_cast<uint32_t>(f->height);
    out->y_stride = static_cast<uint32_t>(f->linesize[0]);
    out->c_stride = static_cast<uint32_t>(f->linesize[1]);
    out->coding_type = coding_type(f->pict_type);
    out->pts90k = f->pts != AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
    out->y = f->data[0];
    out->cb = f->data[1];
    out->cr = f->data[2];
    return 1;
}

} // extern "C"
