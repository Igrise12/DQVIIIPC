// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/gs/gs_backend.h"

#include <cstddef>
#include <cstdint>
#include <span>

// The machine-thread side of the GS: the GIF (PATH1 from VU1 XGKICK, PATH2 from VIF1 DIRECT, PATH3 from GIF
// DMA and the FIFO) and the GS privileged registers push into this. The GS pipeline (gs_pipeline.h)
// implements it by queueing commands for the render thread and hashing the stream on the producer side, so
// determinism checks do not depend on the backend.
namespace dq8::gs {

class IGsSink {
  public:
    virtual ~IGsSink() = default;
    virtual void gif_transfer(GifPath path, std::span<const std::uint8_t> qwords) = 0;
    virtual void vsync(const VsyncInfo& info) = 0;
    // Blocks the machine thread (wall time only) until the backend has the data. Returns bytes written.
    virtual std::size_t read_transfer(std::span<std::uint8_t> out) = 0;
};

} // namespace dq8::gs
