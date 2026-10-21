// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>

// The paraLLEl-GS backend (ADR-0004): an IGsBackend over the shared library dq8-gs-parallel, which this file
// loads at run time with rt::DynamicLibrary and drives through its C ABI (include/dq8/gs/gs_parallel_abi.h).
// Nothing here links Vulkan, Granite or paraLLEl-GS, so the runtime builds and runs without them when another
// backend is chosen.
//
// Threads: create the backend on any thread, then use it from one thread at a time (the render thread); the
// library's calls never overlap. read_frame and read_transfer block that thread.
namespace dq8::gs {

class ParallelGsError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// The first three members are M3 contract decision 1; the flags after them are optional extras.
struct ParallelGsConfig {
    // The library file. Empty: default_parallel_gs_library().
    std::filesystem::path library;
    std::uint32_t super_sampling = 1; // 1, 2, 4, 8 or 16
    // Persistent Vulkan pipeline cache, read at creation and written at destruction. Empty: none.
    std::filesystem::path pipeline_cache;
    bool high_resolution_scanout = false;
    bool super_sampled_textures = false;
    // paraLLEl-GS's back-buffer promotion hack; with the two flags above it gives DQ8 a real 2x picture
    // (S5b).
    bool backbuffer_promotion = false;
};

// The library's file name here: libdq8-gs-parallel.so, libdq8-gs-parallel.dylib or dq8-gs-parallel.dll.
std::filesystem::path parallel_gs_library_file_name();

// Where dq8-gs-parallel is looked for, first match wins: the environment variable DQ8_GS_PARALLEL; the
// directory of the running executable; its ../lib (an install); the copy in the build tree this runtime was
// built in (development builds); the bare file name, left to the system loader's search path.
std::filesystem::path default_parallel_gs_library();

// Loads the library, checks its ABI version and creates the renderer on a Vulkan device. Throws
// ParallelGsError, a std::runtime_error, with the reason (library missing, wrong ABI version, no Vulkan
// device, missing GPU features, bad options); the host maps it to exit code 3.
std::unique_ptr<IGsBackend> make_parallel_gs_backend(const ParallelGsConfig& config);

} // namespace dq8::gs
