// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// dq8-gs-replay: replays a raw GS trace or a PCSX2 GS dump through our paraLLEl-GS backend (the IGsBackend
// the runtime uses, over dq8-gs-parallel's C ABI) and writes the scanout of each vsync to PNG or shows it in
// a window. See tools/gs/README.md. The inputs and the frames it writes are game-derived when they come from
// the game: keep them under work/, never in the repository.

#include "gs_state_restore.h"
#include "gs_stream.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_parallel_backend.h"

#if DQ8_GS_REPLAY_WINDOW
#include "runtime/platform/platform.h"
#include "runtime/platform/sdl/sdl_platform.h"
#endif

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <stb_image_write.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace tgs = dq8::tools::gs;
using Clock = std::chrono::steady_clock;

constexpr const char* usage_text =
    R"(usage: dq8-gs-replay [options] <dump.gs | trace>

Replays a GS packet stream through dq8-gs-parallel (paraLLEl-GS) and writes or shows the scanout of each vsync.

input:
  --raw                  the input is a raw trace (no header); default: detect a PCSX2 dump by its first word
  --max-vsyncs N         stop after N vsyncs
output:
  --png DIR              write DIR/vsync_NNNNN.png for every vsync that displays a picture
  --vsyncs LIST          only these vsyncs for --png, e.g. 2 or 0,2-3 (default: all)
  --window               show the frames in a window (closing it stops the replay)
  --aspect 4:3|16:9      display aspect in the window (default 4:3)
  --rate HZ              window pacing in vsyncs per second (default 59.94; 0 = as fast as possible)
  --loop                 in a window, replay from the start again until the window is closed
  --hold                 in a window, keep showing the last frame until the window is closed
  --window-capture PATH  write what the window shows after the last presented frame to PATH (PNG)
backend:
  --library PATH         the dq8-gs-parallel library to load (default: this build's)
  --ssaa N               super-sampling 1, 2, 4, 8 or 16 (default 1)
  --hires-scanout        scan out at the internal resolution
  --ssaa-textures        super-sampled textures
  --backbuffer-promotion paraLLEl-GS's back-buffer promotion hack
  --pipeline-cache PATH  persistent Vulkan pipeline cache
)";

struct Options {
    fs::path input;
    bool raw = false;
    std::uint64_t max_vsyncs = 0;
    fs::path png_dir;
    std::optional<std::set<std::uint64_t>> vsyncs;
    bool window = false;
    double aspect = 4.0 / 3.0;
    double rate = 60000.0 / 1001.0;
    bool loop = false;
    bool hold = false;
    fs::path window_capture;
    dq8::gs::ParallelGsOptions backend;
};

[[noreturn]] void usage_error(const std::string& message) {
    std::fprintf(stderr, "dq8-gs-replay: %s\n\n%s", message.c_str(), usage_text);
    std::exit(2);
}

std::uint64_t parse_u64(std::string_view text, const char* what) {
    try {
        std::size_t used = 0;
        const unsigned long long v = std::stoull(std::string(text), &used, 0);
        if (used != text.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return v;
    } catch (const std::exception&) {
        usage_error(std::string("bad number for ") + what + ": " + std::string(text));
    }
}

std::set<std::uint64_t> parse_list(std::string_view text) {
    std::set<std::uint64_t> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        const std::size_t dash = item.find('-');
        if (dash == std::string_view::npos) {
            out.insert(parse_u64(item, "--vsyncs"));
        } else {
            const std::uint64_t lo = parse_u64(item.substr(0, dash), "--vsyncs");
            const std::uint64_t hi = parse_u64(item.substr(dash + 1), "--vsyncs");
            for (std::uint64_t v = lo; v <= hi; ++v) {
                out.insert(v);
            }
        }
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return out;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string_view {
            if (i + 1 >= argc) {
                usage_error(std::string(arg) + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            std::fputs(usage_text, stdout);
            std::exit(0);
        } else if (arg == "--raw") {
            o.raw = true;
        } else if (arg == "--max-vsyncs") {
            o.max_vsyncs = parse_u64(value(), "--max-vsyncs");
        } else if (arg == "--png") {
            o.png_dir = fs::path(value());
        } else if (arg == "--vsyncs") {
            o.vsyncs = parse_list(value());
        } else if (arg == "--window") {
            o.window = true;
        } else if (arg == "--aspect") {
            const std::string_view a = value();
            if (a == "4:3") {
                o.aspect = 4.0 / 3.0;
            } else if (a == "16:9") {
                o.aspect = 16.0 / 9.0;
            } else {
                usage_error("--aspect is 4:3 or 16:9");
            }
        } else if (arg == "--rate") {
            const std::string_view r = value();
            try {
                o.rate = std::stod(std::string(r));
            } catch (const std::exception&) {
                usage_error("bad number for --rate: " + std::string(r));
            }
        } else if (arg == "--loop") {
            o.loop = true;
        } else if (arg == "--hold") {
            o.hold = true;
        } else if (arg == "--window-capture") {
            o.window_capture = fs::path(value());
        } else if (arg == "--library") {
            o.backend.library = fs::path(value());
        } else if (arg == "--ssaa") {
            o.backend.super_sampling = static_cast<std::uint32_t>(parse_u64(value(), "--ssaa"));
        } else if (arg == "--hires-scanout") {
            o.backend.high_resolution_scanout = true;
        } else if (arg == "--ssaa-textures") {
            o.backend.super_sampled_textures = true;
        } else if (arg == "--backbuffer-promotion") {
            o.backend.backbuffer_promotion = true;
        } else if (arg == "--pipeline-cache") {
            o.backend.pipeline_cache = fs::path(value());
        } else if (!arg.empty() && arg[0] == '-') {
            usage_error("unknown option " + std::string(arg));
        } else if (o.input.empty()) {
            o.input = fs::path(arg);
        } else {
            usage_error("more than one input");
        }
    }
    if (o.input.empty()) {
        usage_error("no input");
    }
#if !DQ8_GS_REPLAY_WINDOW
    if (o.window) {
        usage_error("--window needs a build with DQ8_BUILD_WINDOW=ON");
    }
#endif
    if (!o.window && (o.loop || o.hold || !o.window_capture.empty())) {
        usage_error("--loop, --hold and --window-capture need --window");
    }
    if (o.backend.library.empty()) {
        // This build's library, unless DQ8_GS_PARALLEL points elsewhere.
        const char* env = std::getenv("DQ8_GS_PARALLEL");
        o.backend.library =
            (env != nullptr && env[0] != '\0') ? fs::path(env) : fs::path(DQ8_GS_PARALLEL_BUILD_PATH);
    }
    return o;
}

bool write_png(const fs::path& file, const dq8::gs::FrameImage& image) {
    return stbi_write_png(file.string().c_str(), static_cast<int>(image.width),
                          static_cast<int>(image.height), 4, image.rgba.data(),
                          static_cast<int>(image.width * 4)) != 0;
}

// PCSX2's dump path index (0..2 = PATH1..3, 3 = its merged GIF stream) as our GIF path. The merged stream
// goes through PATH3, so a stream that uses one index throughout keeps one GIF tag state.
dq8::gs::GifPath gif_path_for(std::uint8_t index) {
    switch (index) {
    case 0:
        return dq8::gs::GifPath::path1;
    case 1:
        return dq8::gs::GifPath::path2;
    default:
        return dq8::gs::GifPath::path3;
    }
}

struct Stats {
    std::uint64_t vsyncs = 0;
    std::uint64_t frames = 0;
    std::uint64_t transfers = 0;
    std::uint64_t transfer_bytes = 0;
    double gs_seconds = 0.0; // time spent in backend calls
};

int run(const Options& o) {
    tgs::GsStream stream(o.input, o.raw ? tgs::StreamFormat::raw : tgs::StreamFormat::automatic);
    if (stream.is_dump()) {
        const tgs::DumpState& s = stream.state();
        std::printf("input: PCSX2 GS dump, state version %u, serial '%s', screenshot %ux%u\n", s.version,
                    s.serial.c_str(), s.screenshot_width, s.screenshot_height);
        for (std::size_t i = 0; i < s.paths.size(); ++i) {
            if (s.paths[i].nloop() != 0) {
                std::fprintf(
                    stderr,
                    "dq8-gs-replay: warning: GIF path %zu was mid-packet (NLOOP %u) when the dump started; "
                    "its tag state is not restored\n",
                    i, s.paths[i].nloop());
            }
        }
    } else {
        std::printf("input: raw GS trace\n");
    }

    const auto t_create = Clock::now();
    std::unique_ptr<dq8::gs::IGsBackend> backend = dq8::gs::create_parallel_gs_backend(o.backend);
    std::printf("backend: %s from %s, created in %.0f ms\n", std::string(backend->caps().name).c_str(),
                o.backend.library.string().c_str(),
                std::chrono::duration<double, std::milli>(Clock::now() - t_create).count());

#if DQ8_GS_REPLAY_WINDOW
    std::unique_ptr<dq8::rt::IPlatform> platform;
    if (o.window) {
        dq8::rt::WindowConfig config;
        config.title = "dq8-gs-replay: " + o.input.filename().string();
        platform = dq8::rt::create_sdl_platform(config);
    }
#endif
    if (!o.png_dir.empty()) {
        fs::create_directories(o.png_dir);
    }

    Stats stats;
    dq8::gs::FrameImage frame;
    [[maybe_unused]] dq8::gs::FrameImage last_shown;
    [[maybe_unused]] const auto period =
        o.rate > 0.0 ? std::chrono::duration<double>(1.0 / o.rate) : std::chrono::duration<double>(0);
    [[maybe_unused]] auto deadline = Clock::now();
    bool quit = false;
    const auto t_start = Clock::now();

    do {
        stream.rewind();
        dq8::gs::VsyncInfo vsync;
        std::uint64_t index = 0;
        if (stream.is_dump()) {
            const std::vector<std::uint8_t> restore = tgs::state_restore_packets(stream.state());
            const auto t0 = Clock::now();
            backend->gif_transfer(dq8::gs::GifPath::path3, restore);
            stats.gs_seconds += std::chrono::duration<double>(Clock::now() - t0).count();
            std::memcpy(vsync.priv.page.data(), stream.state().priv.data(), tgs::priv_page_bytes);
        }

        tgs::Packet packet;
        bool pass_done = false;
        while (!quit && !pass_done && stream.next(packet)) {
            const auto t0 = Clock::now();
            switch (packet.type) {
            case tgs::PacketType::transfer:
                backend->gif_transfer(gif_path_for(packet.path), packet.data);
                ++stats.transfers;
                stats.transfer_bytes += packet.data.size();
                stats.gs_seconds += std::chrono::duration<double>(Clock::now() - t0).count();
                break;
            case tgs::PacketType::priv_regs:
                std::memcpy(vsync.priv.page.data(), packet.data.data(), tgs::priv_page_bytes);
                break;
            case tgs::PacketType::read_fifo: {
                std::vector<std::uint8_t> sink(packet.size);
                backend->read_transfer(sink);
                stats.gs_seconds += std::chrono::duration<double>(Clock::now() - t0).count();
                break;
            }
            case tgs::PacketType::vsync: {
                vsync.field = index;
                vsync.odd_field = (packet.field & 1u) != 0;
                backend->vsync(vsync);
                const bool want_png = !o.png_dir.empty() && (!o.vsyncs || o.vsyncs->count(index) != 0);
                const bool have = (want_png || o.window) && backend->read_frame(frame);
                stats.gs_seconds += std::chrono::duration<double>(Clock::now() - t0).count();
                if (have) {
                    ++stats.frames;
                }
                if (have && want_png) {
                    char name[32];
                    std::snprintf(name, sizeof name, "vsync_%05llu.png",
                                  static_cast<unsigned long long>(index));
                    if (!write_png(o.png_dir / name, frame)) {
                        std::fprintf(stderr, "dq8-gs-replay: cannot write %s\n",
                                     (o.png_dir / name).string().c_str());
                        return 1;
                    }
                    std::printf("vsync %llu: field %u, %ux%u -> %s\n", static_cast<unsigned long long>(index),
                                packet.field, frame.width, frame.height, (o.png_dir / name).string().c_str());
                }
#if DQ8_GS_REPLAY_WINDOW
                if (platform) {
                    if (!platform->pump_events()) {
                        quit = true;
                    }
                    if (have) {
                        last_shown = frame;
                    }
                    platform->present(have ? frame : dq8::gs::FrameImage{}, o.aspect);
                    if (period.count() > 0) {
                        deadline += std::chrono::duration_cast<Clock::duration>(period);
                        std::this_thread::sleep_until(deadline);
                    }
                }
#endif
                ++index;
                ++stats.vsyncs;
                if (o.max_vsyncs != 0 && index >= o.max_vsyncs) {
                    pass_done = true;
                }
                break;
            }
            }
        }
    } while (o.loop && !quit);
    const double wall = std::chrono::duration<double>(Clock::now() - t_start).count();

#if DQ8_GS_REPLAY_WINDOW
    if (platform) {
        if (!o.window_capture.empty()) {
            // One more present of the last frame, kept as a copy of what the window shows.
            dq8::rt::sdl::request_window_capture(*platform);
            platform->present(last_shown, o.aspect);
            dq8::gs::FrameImage shot;
            if (!dq8::rt::sdl::take_window_capture(*platform, shot) || !write_png(o.window_capture, shot)) {
                std::fprintf(stderr, "dq8-gs-replay: cannot capture the window to %s\n",
                             o.window_capture.string().c_str());
                return 1;
            }
            std::printf("window capture: %ux%u -> %s\n", shot.width, shot.height,
                        o.window_capture.string().c_str());
        }
        while (o.hold && platform->pump_events()) {
            platform->present(last_shown, o.aspect);
        }
    }
#endif

    std::printf(
        "replayed %llu vsyncs (%llu frames read back), %llu transfers, %.1f MiB of GIF data in %.3f s; "
        "%.3f ms per vsync in the backend\n",
        static_cast<unsigned long long>(stats.vsyncs), static_cast<unsigned long long>(stats.frames),
        static_cast<unsigned long long>(stats.transfers),
        static_cast<double>(stats.transfer_bytes) / (1024.0 * 1024.0), wall,
        stats.vsyncs != 0 ? 1e3 * stats.gs_seconds / static_cast<double>(stats.vsyncs) : 0.0);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    try {
        return run(options);
    } catch (const dq8::gs::ParallelGsError& e) {
        std::fprintf(stderr, "dq8-gs-replay: %s\n", e.what());
        return 3;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dq8-gs-replay: %s\n", e.what());
        return 1;
    }
}
