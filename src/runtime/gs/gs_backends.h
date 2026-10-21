// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

// The backends that draw nothing (architecture §6.2): `null` drops the stream; `trace` records it in the GS
// trace format (gs_trace_format.h) for replay and golden tests. Both answer a GS -> host transfer with zeros,
// so the guest sees the same memory under either. The paraLLEl-GS backend lives in gs_parallel_backend.h.
namespace dq8::gs {

// (Docs/milestones/M3-boots.md, "Contract decisions after day 0", 1.)
std::unique_ptr<IGsBackend> make_null_gs_backend();

// Writes the trace to `file` (created or truncated; std::runtime_error when it cannot be opened). An empty
// path records nothing: the backend still answers as "trace", and the stream hash is GsPipeline's either way.
std::unique_ptr<IGsBackend> make_trace_gs_backend(std::filesystem::path file);
// Hands every chunk of trace bytes to `sink` instead (tests, in-memory capture). Called on the render thread.
using TraceSink = std::function<void(std::span<const std::uint8_t>)>;
std::unique_ptr<IGsBackend> make_trace_gs_backend_sink(TraceSink sink);

// Writes an RGBA frame as PNG (stb_image_write). False when the file cannot be written or the image is empty.
bool write_png(const std::string& path, const FrameImage& frame);

} // namespace dq8::gs
