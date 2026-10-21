// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_sink.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Machine thread -> bounded command queue -> render thread -> IGsBackend (architecture §12.2). The render
// thread is the only thread that touches the backend; it publishes completed frames to a mailbox the window
// and the PNG writer read. Inline mode runs each command on the calling thread (unit tests, --gs null).
// Implemented by M3 session B (gs_pipeline.cpp and its queue, mailbox and render thread).
namespace dq8::gs {

struct GsPipelineConfig {
    std::unique_ptr<IGsBackend> backend;
    std::size_t queue_bytes = std::size_t{64}
                              << 20; // backpressure blocks the machine thread in wall time only
    bool inline_mode = false;
};

class GsPipeline final : public IGsSink {
  public:
    explicit GsPipeline(GsPipelineConfig config);
    ~GsPipeline() override; // drains and joins the render thread
    GsPipeline(const GsPipeline&) = delete;
    GsPipeline& operator=(const GsPipeline&) = delete;

    void gif_transfer(GifPath path, std::span<const std::uint8_t> qwords) override;
    void vsync(const VsyncInfo& info) override;
    std::size_t read_transfer(std::span<std::uint8_t> out) override;

    // SHA-1 chain over every transfer (path, size, bytes) and vsync (field, privileged page) pushed so far,
    // as 40 hex digits: h_n = SHA1(h_{n-1} || data since the previous vsync), updated at each vsync.
    std::string stream_hash() const;
    std::uint64_t vsyncs() const noexcept;
    // The newest completed frame, from any thread; false when there is none yet.
    bool latest_frame(FrameImage& out) const;
    // Writes the scanout of field `field` to `path` as PNG from the render thread (stb_image_write).
    void request_frame_dump(std::uint64_t field, std::string path);
    const GsCaps& caps() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dq8::gs
