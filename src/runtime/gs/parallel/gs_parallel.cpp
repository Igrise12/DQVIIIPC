// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// dq8-gs-parallel: the C ABI of include/dq8/gs/gs_parallel_abi.h over paraLLEl-GS's GSInterface (ADR-0004,
// Docs/spikes/S5-parallel-gs-standalone.md "Embedding surface"). This translation unit is the only one of
// ours in the shared library; paraLLEl-GS (LGPL-3.0-or-later) and Granite (MIT) are linked in from static
// archives built with hidden visibility, and the version script dq8_gsp.map exports nothing but the dq8_gsp_*
// functions.
//
// Device: a headless Granite context (no surface, no WSI); presentation is the host's job. Scanout:
// force-progressive with anti-blur and the horizontal resolution adapted to the internal one, the settings
// S5b compared against PCSX2. Threads: every call on one dq8_gsp comes from one thread at a time. Each entry
// point registers that thread as Granite's thread index 0 and routes Granite's log through this library's
// sink, so a handle may be created on one thread and then used on another (the render thread) as long as the
// calls never overlap.

#include "context.hpp"
#include "device.hpp"
#include "dq8/gs/gs_parallel_abi.h"
#include "gs_interface.hpp"
#include "logging.hpp"
#include "thread_id.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
// The header declares the ABI without attributes and the definitions export it, which clang reports
// (llvm-mingw, M5 session E); the export is what we want.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"
#endif
#define DQ8_GSP_EXPORT __declspec(dllexport)
#else
#define DQ8_GSP_EXPORT __attribute__((visibility("default")))
#endif

namespace {

static_assert(
    sizeof(ParallelGS::PrivRegisterState) == 0x2000,
    "paraLLEl-GS's privileged-register state must be the 8 KiB page image runtime/gs/priv_regs.h uses");

bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

// Granite's log for the calling thread. Errors and warnings go to stderr with our prefix, information only
// with DQ8_GSP_VERBOSE=1. While `capture` is set (inside dq8_gsp_create) errors are collected for the
// caller's message instead of printed.
class LogSink final : public Util::LoggingInterface {
  public:
    bool log(const char* tag, const char* fmt, va_list va) override {
        std::array<char, 1024> line{};
        // Granite's own printf-style format strings, forwarded as they are.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#pragma clang diagnostic ignored "-Wmissing-format-attribute"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wsuggest-attribute=format"
#endif
        std::vsnprintf(line.data(), line.size(), fmt, va);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        std::string text(line.data());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        const bool error = std::strncmp(tag, "[ERROR]", 7) == 0;
        const bool warning = std::strncmp(tag, "[WARN]", 6) == 0;
        if (capture && error) {
            if (!errors.empty()) {
                errors += "; ";
            }
            errors += text;
        }
        if (verbose || (!capture && (error || warning))) {
            std::fprintf(stderr, "dq8-gs-parallel: %s%s\n", tag, text.c_str());
        }
        return true;
    }

    bool verbose = env_flag("DQ8_GSP_VERBOSE");
    bool capture = false;
    std::string errors;
};

// Granite keeps its thread index and log interface per thread.
class CallScope {
  public:
    explicit CallScope(LogSink& sink) {
        Util::register_thread_index(0);
        Util::set_thread_logging_interface(&sink);
    }
    ~CallScope() { Util::set_thread_logging_interface(nullptr); }
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;
};

void set_error(char* error, size_t error_size, const std::string& message) {
    if (error == nullptr || error_size == 0) {
        return;
    }
    const size_t n = std::min(message.size(), error_size - 1);
    std::memcpy(error, message.data(), n);
    error[n] = '\0';
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

ParallelGS::SuperSampling super_sampling_from(uint32_t rate) {
    switch (rate) {
    case 2:
        return ParallelGS::SuperSampling::X2;
    case 4:
        return ParallelGS::SuperSampling::X4;
    case 8:
        return ParallelGS::SuperSampling::X8;
    case 16:
        return ParallelGS::SuperSampling::X16;
    default:
        return ParallelGS::SuperSampling::X1;
    }
}

constexpr uint32_t known_flags =
    DQ8_GSP_HIRES_SCANOUT | DQ8_GSP_SUPERSAMPLED_TEXTURES | DQ8_GSP_BACKBUFFER_PROMOTION;

} // namespace

// Members are destroyed in reverse order: Granite objects that hold device resources (the readback buffer,
// the scanout image, the GS interface) go before the device, and the device before the context.
struct dq8_gsp {
    LogSink log;
    Vulkan::Context context;
    std::unique_ptr<Vulkan::Device> device;
    std::unique_ptr<ParallelGS::GSInterface> iface;
    ParallelGS::ScanoutResult scanout{};
    Vulkan::BufferHandle readback;
    std::vector<ParallelGS::GIFTagBits> aligned; // copies of GIF data that is not 16-byte aligned

    std::string pipeline_cache_path;
    bool high_resolution_scanout = false;
    bool timestamps = false;
    double gpu_seconds = 0.0; // accumulated GPU time at the previous vsync

    void save_pipeline_cache() noexcept;
};

void dq8_gsp::save_pipeline_cache() noexcept {
    if (pipeline_cache_path.empty() || !device) {
        return;
    }
    try {
        const size_t size = device->get_pipeline_cache_size();
        if (size == 0) {
            return;
        }
        std::vector<std::uint8_t> data(size);
        if (!device->get_pipeline_cache_data(data.data(), size)) {
            return;
        }
        // Write a sibling file and rename it, so a crash never leaves a truncated cache behind.
        const std::filesystem::path path(pipeline_cache_path);
        std::error_code ec;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        std::filesystem::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!out) {
                std::fprintf(stderr, "dq8-gs-parallel: cannot write the pipeline cache %s\n",
                             tmp.string().c_str());
                return;
            }
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::fprintf(stderr, "dq8-gs-parallel: cannot replace the pipeline cache %s: %s\n",
                         path.string().c_str(), ec.message().c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dq8-gs-parallel: cannot save the pipeline cache: %s\n", e.what());
    }
}

extern "C" {

DQ8_GSP_EXPORT uint32_t dq8_gsp_abi_version(void) {
    return DQ8_GSP_ABI_VERSION;
}

DQ8_GSP_EXPORT dq8_gsp* dq8_gsp_create(const dq8_gsp_options* options, char* error, size_t error_size) {
    set_error(error, error_size, "");
    if (options == nullptr || options->struct_size < sizeof(dq8_gsp_options)) {
        set_error(error, error_size, "dq8_gsp_create: options missing or struct_size too small");
        return nullptr;
    }
    const uint32_t rate = options->super_sampling;
    if (rate != 1 && rate != 2 && rate != 4 && rate != 8 && rate != 16) {
        set_error(error, error_size,
                  "dq8_gsp_create: super_sampling must be 1, 2, 4, 8 or 16, not " + std::to_string(rate));
        return nullptr;
    }
    if ((options->flags & ~known_flags) != 0) {
        set_error(error, error_size, "dq8_gsp_create: unknown flags " + std::to_string(options->flags));
        return nullptr;
    }

    // Declared before the handle: a half-built handle is torn down while this sink still collects the log.
    LogSink create_log;
    create_log.capture = true;
    CallScope scope(create_log);
    try {
        auto gs = std::make_unique<dq8_gsp>();
        const auto fail = [&](const std::string& what) {
            std::string message = what;
            if (!create_log.errors.empty()) {
                message += " (" + create_log.errors + ")";
            }
            set_error(error, error_size, message);
            return nullptr;
        };

        if (!Vulkan::Context::init_loader(nullptr)) {
            return fail("no Vulkan loader: libvulkan could not be loaded");
        }
        // The flags the paraLLEl-GS tools pass: optional features the renderer uses when present.
        gs->context.set_num_thread_indices(1);
        if (!gs->context.init_instance_and_device(
                nullptr, 0, nullptr, 0,
                Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                    Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                    Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT)) {
            return fail("no usable Vulkan device");
        }

        gs->device = std::make_unique<Vulkan::Device>();
        gs->device->set_context(gs->context);
        gs->device->init_frame_contexts(4);

        // The pipeline cache must exist before GSInterface::init compiles the pipelines (S5a: 1.4 s cold).
        if (options->pipeline_cache_path != nullptr && options->pipeline_cache_path[0] != '\0') {
            gs->pipeline_cache_path = options->pipeline_cache_path;
            const std::vector<std::uint8_t> blob = read_file(gs->pipeline_cache_path);
            if (!gs->device->init_pipeline_cache(blob.empty() ? nullptr : blob.data(), blob.size())) {
                gs->device->init_pipeline_cache(nullptr, 0);
            }
        }

        ParallelGS::GSOptions gs_options;
        gs_options.super_sampling = super_sampling_from(rate);
        gs_options.super_sampled_textures = (options->flags & DQ8_GSP_SUPERSAMPLED_TEXTURES) != 0;
        gs->iface = std::make_unique<ParallelGS::GSInterface>();
        if (!gs->iface->init(gs->device.get(), gs_options)) {
            return fail("the Vulkan device does not meet paraLLEl-GS's requirements");
        }

        ParallelGS::Hacks hacks;
        hacks.backbuffer_promotion = (options->flags & DQ8_GSP_BACKBUFFER_PROMOTION) != 0;
        gs->iface->set_hacks(hacks);
        gs->high_resolution_scanout = (options->flags & DQ8_GSP_HIRES_SCANOUT) != 0;
        if (env_flag("DQ8_GSP_TIMESTAMPS")) {
            ParallelGS::DebugMode debug;
            debug.timestamps = true;
            gs->iface->set_debug_mode(debug);
            gs->timestamps = true;
        }

        return gs.release();
    } catch (const std::bad_alloc&) {
        set_error(error, error_size, "dq8_gsp_create: out of memory");
    } catch (const std::exception& e) {
        set_error(error, error_size, std::string("dq8_gsp_create: ") + e.what());
    } catch (...) {
        set_error(error, error_size, "dq8_gsp_create: unknown exception");
    }
    return nullptr;
}

DQ8_GSP_EXPORT void dq8_gsp_destroy(dq8_gsp* gs) {
    if (gs == nullptr) {
        return;
    }
    {
        CallScope scope(gs->log);
        try {
            gs->iface->flush();
            gs->device->wait_idle();
        } catch (...) {
            // Tear down regardless.
        }
        gs->save_pipeline_cache();
        gs->readback.reset();
        gs->scanout = {};
        gs->iface.reset();
        gs->device.reset();
    }
    delete gs;
}

DQ8_GSP_EXPORT void dq8_gsp_reset(dq8_gsp* gs) {
    if (gs == nullptr) {
        return;
    }
    CallScope scope(gs->log);
    gs->iface->reset_context_state();
}

DQ8_GSP_EXPORT void dq8_gsp_gif_transfer(dq8_gsp* gs, uint32_t path, const void* qwords, size_t bytes) {
    if (gs == nullptr || qwords == nullptr || path < 1 || path > 3) {
        return;
    }
    bytes &= ~size_t{15};
    if (bytes == 0) {
        return;
    }
    CallScope scope(gs->log);
    // GSInterface reads 128-bit GIF words: give it an aligned copy of a buffer that is not 16-byte aligned.
    const void* data = qwords;
    if ((reinterpret_cast<std::uintptr_t>(qwords) & 15u) != 0) {
        gs->aligned.resize(bytes / 16);
        std::memcpy(static_cast<void*>(gs->aligned.data()), qwords, bytes);
        data = gs->aligned.data();
    }
    // Our paths 1..3 are paraLLEl-GS's path slots 0..2; each slot keeps its own GIF tag state.
    gs->iface->gif_transfer(path - 1, data, bytes);
}

DQ8_GSP_EXPORT int dq8_gsp_vsync(dq8_gsp* gs, const void* priv_page_8k, uint32_t odd_field,
                                 dq8_gsp_frame_info* out) {
    if (out != nullptr) {
        *out = dq8_gsp_frame_info{};
    }
    if (gs == nullptr || priv_page_8k == nullptr) {
        return 1;
    }
    CallScope scope(gs->log);
    try {
        std::memcpy(static_cast<void*>(&gs->iface->get_priv_register_state()), priv_page_8k,
                    sizeof(ParallelGS::PrivRegisterState));
        gs->iface->flush();

        ParallelGS::VSyncInfo info = {};
        info.phase = odd_field & 1u;
        // The image stays ready for dq8_gsp_read_scanout_rgba's copy.
        info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        info.dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
        info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
        info.force_progressive = true;
        info.anti_blur = true;
        info.adapt_to_internal_horizontal_resolution = true;
        info.high_resolution_scanout = gs->high_resolution_scanout;
        gs->scanout = gs->iface->vsync(info);

        if (out != nullptr && gs->scanout.image) {
            out->width = gs->scanout.image->get_width();
            out->height = gs->scanout.image->get_height();
        }
        if (out != nullptr && gs->timestamps) {
            // Resolved asynchronously, so this lags a frame or two; the average is what matters.
            double total = 0.0;
            for (int i = 0; i < static_cast<int>(ParallelGS::TimestampType::Count); ++i) {
                total += gs->iface->get_accumulated_timestamps(static_cast<ParallelGS::TimestampType>(i));
            }
            const double delta = std::max(0.0, total - gs->gpu_seconds);
            gs->gpu_seconds = total;
            out->gpu_ns = static_cast<uint64_t>(delta * 1e9);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dq8-gs-parallel: vsync failed: %s\n", e.what());
        return 1;
    }
}

DQ8_GSP_EXPORT int dq8_gsp_read_scanout_rgba(dq8_gsp* gs, void* dst, size_t dst_size, uint32_t* width,
                                             uint32_t* height) {
    if (width != nullptr) {
        *width = 0;
    }
    if (height != nullptr) {
        *height = 0;
    }
    if (gs == nullptr || !gs->scanout.image) {
        return 1;
    }
    const Vulkan::Image& image = *gs->scanout.image;
    const uint32_t w = image.get_width();
    const uint32_t h = image.get_height();
    if (width != nullptr) {
        *width = w;
    }
    if (height != nullptr) {
        *height = h;
    }
    const size_t bytes = size_t{w} * h * 4;
    if (dst == nullptr || dst_size < bytes) {
        return 2;
    }
    if (image.get_format() != VK_FORMAT_R8G8B8A8_UNORM && image.get_format() != VK_FORMAT_R8G8B8A8_SRGB) {
        std::fprintf(stderr, "dq8-gs-parallel: unexpected scanout format %d\n",
                     static_cast<int>(image.get_format()));
        return 3;
    }

    CallScope scope(gs->log);
    try {
        Vulkan::Device& device = *gs->device;
        if (!gs->readback || gs->readback->get_create_info().size < bytes) {
            Vulkan::BufferCreateInfo info = {};
            info.size = bytes;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.domain = Vulkan::BufferDomain::CachedHost;
            gs->readback = device.create_buffer(info);
        }
        // vsync() left the image in TRANSFER_SRC_OPTIMAL behind a barrier to the copy stage.
        auto cmd = device.request_command_buffer();
        cmd->copy_image_to_buffer(*gs->readback, image, 0, {}, {w, h, 1}, 0, 0,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        Vulkan::Fence fence;
        device.submit(cmd, &fence);
        fence->wait();

        const auto* src = static_cast<const std::uint8_t*>(
            device.map_host_buffer(*gs->readback, Vulkan::MEMORY_ACCESS_READ_BIT));
        auto* out = static_cast<std::uint8_t*>(dst);
        std::memcpy(out, src, bytes);
        device.unmap_host_buffer(*gs->readback, Vulkan::MEMORY_ACCESS_READ_BIT);
        // The scanout's alpha carries no display meaning; make the frame opaque.
        for (size_t i = 3; i < bytes; i += 4) {
            out[i] = 0xFF;
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dq8-gs-parallel: scanout readback failed: %s\n", e.what());
        return 4;
    }
}

DQ8_GSP_EXPORT size_t dq8_gsp_read_transfer(dq8_gsp* gs, void* dst, size_t bytes) {
    if (gs == nullptr || dst == nullptr) {
        return 0;
    }
    const size_t words = bytes / 16;
    if (words == 0) {
        return 0;
    }
    CallScope scope(gs->log);
    gs->iface->read_transfer_fifo(dst, static_cast<uint32_t>(std::min<size_t>(words, UINT32_MAX)));
    return std::min<size_t>(words, UINT32_MAX) * 16;
}

DQ8_GSP_EXPORT void dq8_gsp_flush(dq8_gsp* gs) {
    if (gs == nullptr) {
        return;
    }
    CallScope scope(gs->log);
    gs->iface->flush();
}

} // extern "C"
