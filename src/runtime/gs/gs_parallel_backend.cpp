// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/gs/gs_parallel_backend.h"

#include "dq8/gs/gs_parallel_abi.h"
#include "runtime/platform/dynlib.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dq8::gs {

namespace {

namespace fs = std::filesystem;

// The directory of the running executable, or empty when the platform does not say.
fs::path executable_directory() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size()) {
        return {};
    }
    buffer.resize(n);
    return fs::path(buffer).parent_path();
#elif defined(__linux__)
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : exe.parent_path();
#else
    return {};
#endif
}

struct Api {
    dq8_gsp_abi_version_fn abi_version = nullptr;
    dq8_gsp_create_fn create = nullptr;
    dq8_gsp_destroy_fn destroy = nullptr;
    dq8_gsp_reset_fn reset = nullptr;
    dq8_gsp_gif_transfer_fn gif_transfer = nullptr;
    dq8_gsp_vsync_fn vsync = nullptr;
    dq8_gsp_read_scanout_rgba_fn read_scanout_rgba = nullptr;
    dq8_gsp_read_transfer_fn read_transfer = nullptr;
    dq8_gsp_flush_fn flush = nullptr;
};

template <typename Fn> Fn resolve(const rt::DynamicLibrary& library, const char* name, const fs::path& file) {
    void* symbol = library.symbol(name);
    if (symbol == nullptr) {
        throw ParallelGsError(file.string() + " does not export " + name);
    }
    Fn fn = nullptr;
    static_assert(sizeof fn == sizeof symbol);
    std::memcpy(&fn, &symbol, sizeof fn);
    return fn;
}

class ParallelGsBackend final : public IGsBackend {
  public:
    ParallelGsBackend(rt::DynamicLibrary library, const Api& api, dq8_gsp* gs)
        : library_(std::move(library)), api_(api), gs_(gs) {}
    ~ParallelGsBackend() override { api_.destroy(gs_); }
    ParallelGsBackend(const ParallelGsBackend&) = delete;
    ParallelGsBackend& operator=(const ParallelGsBackend&) = delete;

    GsCaps caps() const override {
        GsCaps caps;
        caps.name = "parallel";
        caps.produces_pixels = true;
        caps.readbacks = true;
        caps.max_super_sampling = 16;
        return caps;
    }

    void reset() override {
        api_.reset(gs_);
        frame_width_ = 0;
        frame_height_ = 0;
    }

    void gif_transfer(GifPath path, std::span<const std::uint8_t> qwords) override {
        api_.gif_transfer(gs_, static_cast<std::uint32_t>(path), qwords.data(), qwords.size());
    }

    void vsync(const VsyncInfo& info) override {
        dq8_gsp_frame_info frame{};
        if (api_.vsync(gs_, info.priv.page.data(), info.odd_field ? 1u : 0u, &frame) != 0) {
            frame = dq8_gsp_frame_info{};
        }
        frame_width_ = frame.width;
        frame_height_ = frame.height;
        frame_field_ = info.field;
    }

    bool read_frame(FrameImage& out) override {
        if (frame_width_ == 0 || frame_height_ == 0) {
            return false;
        }
        out.rgba.resize(std::size_t{frame_width_} * frame_height_ * 4);
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (api_.read_scanout_rgba(gs_, out.rgba.data(), out.rgba.size(), &width, &height) != 0) {
            out.rgba.clear();
            out.width = 0;
            out.height = 0;
            return false;
        }
        out.width = width;
        out.height = height;
        out.field = frame_field_;
        return true;
    }

    std::size_t read_transfer(std::span<std::uint8_t> out) override {
        return api_.read_transfer(gs_, out.data(), out.size());
    }

    void flush() override { api_.flush(gs_); }

  private:
    rt::DynamicLibrary library_; // declared first: unloaded after destroy() ran
    Api api_;
    dq8_gsp* gs_ = nullptr;
    std::uint32_t frame_width_ = 0;
    std::uint32_t frame_height_ = 0;
    std::uint64_t frame_field_ = 0;
};

} // namespace

fs::path parallel_gs_library_file_name() {
#if defined(_WIN32)
    return "dq8-gs-parallel.dll";
#elif defined(__APPLE__)
    return "libdq8-gs-parallel.dylib";
#else
    return "libdq8-gs-parallel.so";
#endif
}

fs::path default_parallel_gs_library() {
    if (const char* env = std::getenv("DQ8_GS_PARALLEL"); env != nullptr && env[0] != '\0') {
        return fs::path(env);
    }
    const fs::path file = parallel_gs_library_file_name();
    std::error_code ec;
    if (const fs::path dir = executable_directory(); !dir.empty()) {
        for (const fs::path& candidate : {dir / file, dir.parent_path() / "lib" / file}) {
            if (fs::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
    }
#if defined(DQ8_GS_PARALLEL_BUILD_PATH)
    if (const fs::path built = DQ8_GS_PARALLEL_BUILD_PATH; fs::is_regular_file(built, ec)) {
        return built;
    }
#endif
    return file;
}

std::unique_ptr<IGsBackend> make_parallel_gs_backend(const ParallelGsConfig& options) {
    const fs::path file = options.library.empty() ? default_parallel_gs_library() : options.library;
    rt::DynamicLibrary library;
    try {
        library = rt::DynamicLibrary::open(file);
    } catch (const rt::DynlibError& e) {
        throw ParallelGsError(std::string("cannot load the paraLLEl-GS backend library: ") + e.what());
    }

    Api api;
    api.abi_version = resolve<dq8_gsp_abi_version_fn>(library, "dq8_gsp_abi_version", file);
    const std::uint32_t version = api.abi_version();
    if (version != DQ8_GSP_ABI_VERSION) {
        throw ParallelGsError(file.string() + " implements dq8_gsp ABI version " + std::to_string(version) +
                              ", this host needs version " + std::to_string(DQ8_GSP_ABI_VERSION));
    }
    api.create = resolve<dq8_gsp_create_fn>(library, "dq8_gsp_create", file);
    api.destroy = resolve<dq8_gsp_destroy_fn>(library, "dq8_gsp_destroy", file);
    api.reset = resolve<dq8_gsp_reset_fn>(library, "dq8_gsp_reset", file);
    api.gif_transfer = resolve<dq8_gsp_gif_transfer_fn>(library, "dq8_gsp_gif_transfer", file);
    api.vsync = resolve<dq8_gsp_vsync_fn>(library, "dq8_gsp_vsync", file);
    api.read_scanout_rgba = resolve<dq8_gsp_read_scanout_rgba_fn>(library, "dq8_gsp_read_scanout_rgba", file);
    api.read_transfer = resolve<dq8_gsp_read_transfer_fn>(library, "dq8_gsp_read_transfer", file);
    api.flush = resolve<dq8_gsp_flush_fn>(library, "dq8_gsp_flush", file);

    std::uint32_t flags = 0;
    if (options.high_resolution_scanout) {
        flags |= DQ8_GSP_HIRES_SCANOUT;
    }
    if (options.super_sampled_textures) {
        flags |= DQ8_GSP_SUPERSAMPLED_TEXTURES;
    }
    if (options.backbuffer_promotion) {
        flags |= DQ8_GSP_BACKBUFFER_PROMOTION;
    }
    const std::string cache = options.pipeline_cache.string();
    dq8_gsp_options create_options{};
    create_options.struct_size = sizeof create_options;
    create_options.super_sampling = options.super_sampling;
    create_options.flags = flags;
    create_options.pipeline_cache_path = cache.empty() ? nullptr : cache.c_str();

    std::array<char, 1024> error{};
    dq8_gsp* gs = api.create(&create_options, error.data(), error.size());
    if (gs == nullptr) {
        throw ParallelGsError(std::string("paraLLEl-GS backend: ") +
                              (error[0] != '\0' ? error.data() : "dq8_gsp_create failed"));
    }
    return std::make_unique<ParallelGsBackend>(std::move(library), api, gs);
}

} // namespace dq8::gs
