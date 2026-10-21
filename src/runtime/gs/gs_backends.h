// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

// The backends that draw nothing (architecture §6.2): `null` drops the stream; `trace` records it in the GS
// trace format (gs_trace_format.h) for replay and golden tests. Both answer a GS -> host transfer with zeros,
// so the guest sees the same memory under either. The paraLLEl-GS backend lives in gs_parallel_backend.h.
namespace dq8::gs {

std::unique_ptr<IGsBackend> make_null_backend();

// Writes the trace to `path` (created or truncated; std::runtime_error when it cannot be opened).
std::unique_ptr<IGsBackend> make_trace_backend(const std::string& path);
// Hands every chunk of trace bytes to `sink` instead (tests, in-memory capture). Called on the render thread.
std::unique_ptr<IGsBackend> make_trace_backend(std::function<void(std::span<const std::uint8_t>)> sink);

// Writes an RGBA frame as PNG (stb_image_write). False when the file cannot be written or the image is empty.
bool write_png(const std::string& path, const FrameImage& frame);

} // namespace dq8::gs
