// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

// The movie decoder behind the sceMpeg HLE (architecture §7.2, ADR-0015). The HLE's own elementary-stream
// model decides how many pictures there are and when each is due; a decoder only paints pixels, so play and
// "nodecode" runs (black pictures, no FFmpeg) have identical timing. Fixed on M4 day 0; implemented by M4
// session C: the library-backed decoder loads dq8-movie (include/dq8/movie/movie_decoder_abi.h).
namespace dq8::rt {

// A decoded picture in display order, 4:2:0; the planes stay valid until the next call on the decoder.
struct PictureYuv {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t y_stride = 0;
    std::uint32_t c_stride = 0;
    std::int64_t pts90k = 0;
    const std::uint8_t* y = nullptr;
    const std::uint8_t* cb = nullptr;
    const std::uint8_t* cr = nullptr;
};

// Every call comes from the machine thread. The sceMpeg HLE passes each coded picture's decode-order index as
// its timestamp and finds the picture it wants by that tag; decode_picture and next_picture throw
// MovieDecoderError on a decoder error (the HLE then paints the picture black).
class IMovieDecoder {
  public:
    virtual ~IMovieDecoder() = default;
    virtual std::string name() const = 0;
    virtual void reset() = 0;
    // One coded picture (with the sequence and GOP headers before it) and its timestamp.
    virtual void decode_picture(std::span<const std::uint8_t> coded, std::int64_t pts90k) = 0;
    // End of stream: the held reference picture becomes ready.
    virtual void flush() = 0;
    // The next picture in display order, if one is ready; its planes stay valid until the next call.
    virtual bool next_picture(PictureYuv& out) = 0;
};

class MovieDecoderError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

struct MovieLibraryConfig {
    std::string
        path; // empty: the default search (DQ8_MOVIE_LIBRARY, the executable's directory, the build tree)
};

// Black pictures of the stream's size, in display order: --movies nodecode.
std::unique_ptr<IMovieDecoder> make_blank_movie_decoder();
// Loads dq8-movie; throws MovieDecoderError when it is missing or has another ABI version.
std::unique_ptr<IMovieDecoder> make_library_movie_decoder(const MovieLibraryConfig& config);
// Why dq8-movie cannot be used (it does not load, has another ABI version or lacks a function), or nullopt
// when it can: the --movies auto probe. It only loads the library and checks its ABI: no decoder is created
// and no guest state is touched. A refused library is unloaded; an accepted one stays loaded for the HLE that
// uses it next (movie_library.cpp says why it is never unloaded).
std::optional<std::string> movie_library_problem(const MovieLibraryConfig& config);
// "libdq8-movie.so", "libdq8-movie.dylib" or "dq8-movie.dll".
std::filesystem::path movie_library_file_name();
// The library the default search finds: $DQ8_MOVIE_LIBRARY, then the executable's directory and ../lib, then
// the build tree's copy, else the bare file name (the system's library search).
std::filesystem::path default_movie_library();

} // namespace dq8::rt
