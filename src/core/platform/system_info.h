// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <optional>

// Host facts the build stage sizes itself by (internal to libdq8core).
namespace dq8::core::platform {

// Hardware threads, at least 1.
unsigned hardware_threads() noexcept;

// Physical memory available to new processes without swapping (Linux MemAvailable, Windows ullAvailPhys,
// macOS free + inactive pages), or nullopt when the platform does not say.
std::optional<std::uint64_t> available_memory_bytes();

} // namespace dq8::core::platform
