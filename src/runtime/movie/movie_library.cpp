// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The library-backed movie decoder: loads dq8-movie at run time (include/dq8/movie/movie_decoder_abi.h), the
// way runtime/gs/gs_parallel_backend.cpp loads dq8-gs-parallel, so dq8-game never links FFmpeg.
#include "dq8/movie/movie_decoder_abi.h"
#include "runtime/movie/movie_decoder.h"
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

namespace dq8::rt {

namespace {

namespace fs = std::filesystem;

// The largest picture the host accepts from the library (MPEG-2 Main profile, high level).
constexpr std::uint32_t max_picture_width = 1920;
constexpr std::uint32_t max_picture_height = 1088;

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
    decltype(&dq8_mvd_abi_version) abi_version = nullptr;
    decltype(&dq8_mvd_backend_version) backend_version = nullptr;
    decltype(&dq8_mvd_create) create = nullptr;
    decltype(&dq8_mvd_destroy) destroy = nullptr;
    decltype(&dq8_mvd_reset) reset = nullptr;
    decltype(&dq8_mvd_send) send = nullptr;
    decltype(&dq8_mvd_flush) flush = nullptr;
    decltype(&dq8_mvd_receive) receive = nullptr;
};

template <typename Fn>
void resolve(const DynamicLibrary& library, const char* name, const fs::path& file, Fn& out) {
    void* symbol = library.symbol(name);
    if (symbol == nullptr) {
        throw MovieDecoderError(file.string() + " does not export " + name);
    }
    static_assert(sizeof out == sizeof symbol);
    std::memcpy(&out, &symbol, sizeof out);
}

class LibraryMovieDecoder final : public IMovieDecoder {
  public:
    LibraryMovieDecoder(DynamicLibrary library, const Api& api, dq8_mvd* decoder, std::string backend)
        : library_(std::move(library)), api_(api), decoder_(decoder), backend_(std::move(backend)) {}
    ~LibraryMovieDecoder() override { api_.destroy(decoder_); }
    LibraryMovieDecoder(const LibraryMovieDecoder&) = delete;
    LibraryMovieDecoder& operator=(const LibraryMovieDecoder&) = delete;

    std::string name() const override { return "dq8-movie (" + backend_ + ")"; }

    void reset() override { api_.reset(decoder_); }

    void decode_picture(std::span<const std::uint8_t> coded, std::int64_t pts90k) override {
        if (api_.send(decoder_, coded.data(), coded.size(), pts90k) < 0) {
            throw MovieDecoderError("dq8-movie could not decode a picture");
        }
    }

    void flush() override {
        if (api_.flush(decoder_) < 0) {
            throw MovieDecoderError("dq8-movie could not flush the stream");
        }
    }

    bool next_picture(PictureYuv& out) override {
        dq8_mvd_picture p{};
        const int r = api_.receive(decoder_, &p);
        if (r < 0) {
            throw MovieDecoderError("dq8-movie could not return a picture");
        }
        if (r == 0) {
            return false;
        }
        out = PictureYuv{};
        out.width = p.width;
        out.height = p.height;
        out.y_stride = p.y_stride;
        out.c_stride = p.c_stride;
        out.pts90k = p.pts90k;
        out.y = p.y;
        out.cb = p.cb;
        out.cr = p.cr;
        return true;
    }

  private:
    DynamicLibrary library_; // declared first: unloaded after destroy() ran
    Api api_;
    dq8_mvd* decoder_ = nullptr;
    std::string backend_;
};

} // namespace

fs::path movie_library_file_name() {
#if defined(_WIN32)
    return "dq8-movie.dll";
#elif defined(__APPLE__)
    return "libdq8-movie.dylib";
#else
    return "libdq8-movie.so";
#endif
}

fs::path default_movie_library() {
    if (const char* env = std::getenv("DQ8_MOVIE_LIBRARY"); env != nullptr && env[0] != '\0') {
        return fs::path(env);
    }
    const fs::path file = movie_library_file_name();
    std::error_code ec;
    if (const fs::path dir = executable_directory(); !dir.empty()) {
        for (const fs::path& candidate : {dir / file, dir.parent_path() / "lib" / file}) {
            if (fs::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
    }
#if defined(DQ8_MOVIE_BUILD_PATH)
    if (const fs::path built = DQ8_MOVIE_BUILD_PATH; fs::is_regular_file(built, ec)) {
        return built;
    }
#endif
    return file;
}

std::unique_ptr<IMovieDecoder> make_library_movie_decoder(const MovieLibraryConfig& config) {
    const fs::path file = config.path.empty() ? default_movie_library() : fs::path(config.path);
    DynamicLibrary library;
    try {
        library = DynamicLibrary::open(file);
    } catch (const DynlibError& e) {
        throw MovieDecoderError(std::string("cannot load the movie decoder library: ") + e.what());
    }
    Api api;
    resolve(library, "dq8_mvd_abi_version", file, api.abi_version);
    if (const std::uint32_t version = api.abi_version(); version != DQ8_MVD_ABI_VERSION) {
        throw MovieDecoderError(file.string() + " has movie decoder ABI version " + std::to_string(version) +
                                ", this host needs version " + std::to_string(DQ8_MVD_ABI_VERSION));
    }
    resolve(library, "dq8_mvd_backend_version", file, api.backend_version);
    resolve(library, "dq8_mvd_create", file, api.create);
    resolve(library, "dq8_mvd_destroy", file, api.destroy);
    resolve(library, "dq8_mvd_reset", file, api.reset);
    resolve(library, "dq8_mvd_send", file, api.send);
    resolve(library, "dq8_mvd_flush", file, api.flush);
    resolve(library, "dq8_mvd_receive", file, api.receive);

    std::array<char, 128> backend{};
    api.backend_version(backend.data(), backend.size());
    backend.back() = '\0';

    dq8_mvd_options options{};
    options.struct_size = sizeof options;
    options.max_width = max_picture_width;
    options.max_height = max_picture_height;
    std::array<char, 512> error{};
    dq8_mvd* decoder = api.create(&options, error.data(), error.size());
    if (decoder == nullptr) {
        error.back() = '\0';
        throw MovieDecoderError(file.string() + ": " + error.data());
    }
    return std::make_unique<LibraryMovieDecoder>(std::move(library), api, decoder,
                                                 std::string(backend.data()));
}

std::optional<std::string> movie_library_problem(const MovieLibraryConfig& config) {
    try {
        make_library_movie_decoder(config);
        return std::nullopt;
    } catch (const MovieDecoderError& e) {
        return std::string(e.what());
    }
}

} // namespace dq8::rt
