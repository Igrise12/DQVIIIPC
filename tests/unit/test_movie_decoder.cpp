// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The movie decoders behind the sceMpeg HLE: the blank one (--movies nodecode) and dq8-movie, the libavcodec
// library loaded at run time (include/dq8/movie/movie_decoder_abi.h). Streams come from the test-time MPEG-2
// writer. The dq8-movie cases SKIP when the build has no library (DQ8_FFMPEG=off, or no FFmpeg found).
#include "dq8/movie/movie_decoder_abi.h"
#include "runtime/movie/movie_decoder.h"
#include "runtime/movie/mpeg_es_model.h"
#include "runtime/platform/dynlib.h"
#include "support/mpeg2_writer.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#if defined(__linux__)
#include <elf.h>
#endif
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

using namespace dq8::rt;
using namespace dq8::test;

namespace {

struct Decoded {
    std::int64_t tag = -1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> y; // tightly packed
    std::vector<std::uint8_t> cb;
    std::vector<std::uint8_t> cr;
};

Decoded copy_out(const PictureYuv& p) {
    Decoded d;
    d.tag = p.pts90k;
    d.width = p.width;
    d.height = p.height;
    for (std::uint32_t row = 0; row < p.height; ++row) {
        d.y.insert(d.y.end(), p.y + std::size_t{row} * p.y_stride,
                   p.y + std::size_t{row} * p.y_stride + p.width);
    }
    for (std::uint32_t row = 0; row < p.height / 2; ++row) {
        const std::size_t at = std::size_t{row} * p.c_stride;
        d.cb.insert(d.cb.end(), p.cb + at, p.cb + at + p.width / 2);
        d.cr.insert(d.cr.end(), p.cr + at, p.cr + at + p.width / 2);
    }
    return d;
}

struct Stream {
    Mpeg2TestStream spec;
    std::vector<std::uint32_t> display_seeds;
    std::vector<std::uint8_t> es;
};

Stream make_stream(std::uint32_t count, std::uint32_t b_frames, std::uint32_t width = 64,
                   std::uint32_t height = 48) {
    Stream s;
    s.spec.width = width;
    s.spec.height = height;
    s.spec.pictures = mpeg2_test_gop(count, b_frames, s.display_seeds);
    s.es = write_mpeg2_stream(s.spec);
    return s;
}

// Splits the stream with the ES model and runs every coded picture through `decoder`, tagged with its
// decode-order index, then flushes: every picture in the order the decoder returns them.
std::vector<Decoded> decode_all(IMovieDecoder& decoder, const Stream& s) {
    MpegEsModel model;
    model.feed(s.es);
    REQUIRE(model.error().empty());
    std::vector<Decoded> out;
    PictureYuv p;
    while (auto coded = model.take_coded()) {
        decoder.decode_picture(coded->bytes, coded->index);
        while (decoder.next_picture(p)) {
            out.push_back(copy_out(p));
        }
    }
    decoder.flush();
    while (decoder.next_picture(p)) {
        out.push_back(copy_out(p));
    }
    return out;
}

// The decode-order index of each display position.
std::vector<std::int64_t> display_tags(const Stream& s) {
    std::vector<std::int64_t> tags;
    for (const std::uint32_t seed : s.display_seeds) {
        for (std::size_t i = 0; i < s.spec.pictures.size(); ++i) {
            if (s.spec.pictures[i].seed == seed) {
                tags.push_back(static_cast<std::int64_t>(i));
            }
        }
    }
    return tags;
}

std::vector<std::int64_t> tags_of(const std::vector<Decoded>& pictures) {
    std::vector<std::int64_t> tags;
    for (const Decoded& d : pictures) {
        tags.push_back(d.tag);
    }
    return tags;
}

#if defined(DQ8_MOVIE_LIBRARY)
const std::filesystem::path library_file = DQ8_MOVIE_LIBRARY;
#endif

#define REQUIRE_MOVIE_LIBRARY()                                                                              \
    do {                                                                                                     \
        if (!movie_library_built()) {                                                                        \
            SKIP("dq8-movie is not built in this configuration (DQ8_FFMPEG, cmake/Movie.cmake)");            \
        }                                                                                                    \
    } while (false)

bool movie_library_built() {
#if defined(DQ8_MOVIE_LIBRARY)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<IMovieDecoder> library_decoder() {
#if defined(DQ8_MOVIE_LIBRARY)
    return make_library_movie_decoder(MovieLibraryConfig{library_file.string()});
#else
    return nullptr;
#endif
}

#if defined(DQ8_MOVIE_LIBRARY)
const std::set<std::string> abi_functions = {
    "dq8_mvd_abi_version", "dq8_mvd_backend_version", "dq8_mvd_create", "dq8_mvd_destroy", "dq8_mvd_reset",
    "dq8_mvd_send",        "dq8_mvd_flush",           "dq8_mvd_receive"};
#endif

#if defined(__linux__) && defined(DQ8_MOVIE_LIBRARY)
// The names of the defined, global or weak symbols in a shared library's dynamic symbol table.
std::set<std::string> defined_dynamic_symbols(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    REQUIRE(in);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto* bytes = reinterpret_cast<const unsigned char*>(raw.data());
    Elf64_Ehdr eh{};
    REQUIRE(raw.size() >= sizeof eh);
    std::memcpy(&eh, bytes, sizeof eh);
    REQUIRE(std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0);
    REQUIRE(eh.e_ident[EI_CLASS] == ELFCLASS64);
    REQUIRE(eh.e_shoff + std::size_t{eh.e_shnum} * sizeof(Elf64_Shdr) <= raw.size());
    std::vector<Elf64_Shdr> sections(eh.e_shnum);
    std::memcpy(sections.data(), bytes + eh.e_shoff, sections.size() * sizeof(Elf64_Shdr));
    std::set<std::string> names;
    for (const Elf64_Shdr& sh : sections) {
        if (sh.sh_type != SHT_DYNSYM) {
            continue;
        }
        REQUIRE(sh.sh_link < sections.size());
        const Elf64_Shdr& strtab = sections[sh.sh_link];
        REQUIRE(sh.sh_offset + sh.sh_size <= raw.size());
        REQUIRE(strtab.sh_offset + strtab.sh_size <= raw.size());
        for (std::size_t i = 0; i < sh.sh_size / sizeof(Elf64_Sym); ++i) {
            Elf64_Sym sym{};
            std::memcpy(&sym, bytes + sh.sh_offset + i * sizeof(Elf64_Sym), sizeof sym);
            const unsigned bind = ELF64_ST_BIND(sym.st_info);
            if (sym.st_shndx == SHN_UNDEF || sym.st_name == 0 || (bind != STB_GLOBAL && bind != STB_WEAK)) {
                continue;
            }
            REQUIRE(sym.st_name < strtab.sh_size);
            names.emplace(reinterpret_cast<const char*>(bytes + strtab.sh_offset + sym.st_name));
        }
    }
    return names;
}
#endif

} // namespace

TEST_CASE("movie decoder: the blank decoder orders black pictures like a real one", "[movie]") {
    const Stream s = make_stream(13, 2);
    const auto decoder = make_blank_movie_decoder();
    CHECK(decoder->name() == "blank");
    const std::vector<Decoded> pictures = decode_all(*decoder, s);
    CHECK(tags_of(pictures) == display_tags(s));
    for (const Decoded& d : pictures) {
        CHECK(d.width == 64);
        CHECK(d.height == 48);
        CHECK(d.y == std::vector<std::uint8_t>(64 * 48, 16));
        CHECK(d.cb == std::vector<std::uint8_t>(32 * 24, 128));
    }
    decoder->reset();
    CHECK(tags_of(decode_all(*decoder, s)) == display_tags(s));
    const std::uint8_t junk[] = {0, 0, 1, 0xB3, 1, 2, 3, 4};
    CHECK_THROWS_AS(decoder->decode_picture(junk, 0), MovieDecoderError);
}

TEST_CASE("movie decoder: dq8-movie decodes the writer's stream in display order", "[movie][dq8_movie]") {
    REQUIRE_MOVIE_LIBRARY();
    const Stream s = make_stream(16, 2);
    const auto decoder = library_decoder();
    CHECK(decoder->name().find("libavcodec") != std::string::npos);
    const std::vector<Decoded> pictures = decode_all(*decoder, s);
    REQUIRE(tags_of(pictures) == display_tags(s));

    // Every block decodes to its DC value exactly (bit-exact flags, the simple IDCT).
    for (std::size_t k = 0; k < pictures.size(); ++k) {
        const Decoded& d = pictures[k];
        const Mpeg2TestPicture& spec = s.spec.pictures.at(static_cast<std::size_t>(d.tag));
        REQUIRE(d.width == 64);
        REQUIRE(d.height == 48);
        for (std::uint32_t mby = 0; mby < 3; ++mby) {
            for (std::uint32_t mbx = 0; mbx < 4; ++mbx) {
                INFO("display " << k << " macroblock " << mbx << "," << mby);
                CHECK(d.y[(mby * 16 + 5) * 64 + mbx * 16 + 9] == mpeg2_test_luma(spec.seed, mbx, mby));
                CHECK(d.cb[(mby * 8 + 3) * 32 + mbx * 8 + 2] == spec.cb);
                CHECK(d.cr[(mby * 8 + 3) * 32 + mbx * 8 + 2] == spec.cr);
            }
        }
    }

    // The same stream again after a reset: the same pixels.
    decoder->reset();
    const std::vector<Decoded> again = decode_all(*decoder, s);
    REQUIRE(again.size() == pictures.size());
    for (std::size_t k = 0; k < again.size(); ++k) {
        CHECK(again[k].tag == pictures[k].tag);
        CHECK(again[k].y == pictures[k].y);
        CHECK(again[k].cb == pictures[k].cb);
    }
}

TEST_CASE("movie decoder: dq8-movie and the blank decoder return the same pictures", "[movie][dq8_movie]") {
    REQUIRE_MOVIE_LIBRARY();
    for (const std::uint32_t b : {0u, 1u, 2u, 3u}) {
        const Stream s = make_stream(11, b, 32, 32);
        const auto library = library_decoder();
        const auto blank = make_blank_movie_decoder();
        INFO("b frames " << b);
        CHECK(tags_of(decode_all(*library, s)) == tags_of(decode_all(*blank, s)));
    }
}

TEST_CASE("movie decoder: a missing or wrong library is reported", "[movie]") {
    const MovieLibraryConfig missing{"/nonexistent/dir/" + movie_library_file_name().string()};
    CHECK_THROWS_AS(make_library_movie_decoder(missing), MovieDecoderError);
    const auto problem = movie_library_problem(missing);
    REQUIRE(problem);
    CHECK(problem->find("cannot load the movie decoder library") != std::string::npos);
#if defined(__linux__)
    CHECK(movie_library_file_name() == "libdq8-movie.so");
#endif
#if defined(DQ8_MOVIE_LIBRARY)
    CHECK_FALSE(movie_library_problem(MovieLibraryConfig{library_file.string()}));
    // The build tree's copy is the default search's last resort.
    if (std::getenv("DQ8_MOVIE_LIBRARY") == nullptr) {
        CHECK_FALSE(movie_library_problem(MovieLibraryConfig{}));
    }
#endif
}

TEST_CASE("movie decoder: dq8-movie exports exactly the dq8_mvd_* C ABI", "[movie][dq8_movie]") {
    REQUIRE_MOVIE_LIBRARY();
#if defined(__linux__) && defined(DQ8_MOVIE_LIBRARY)
    // Nothing of FFmpeg or the C++ runtime may leak out of the library (ADR-0015).
    CHECK(defined_dynamic_symbols(library_file) == abi_functions);
#else
    SKIP("the exports check reads ELF files (Linux)");
#endif
}

TEST_CASE("movie decoder: dq8-movie's C ABI refuses bad calls", "[movie][dq8_movie]") {
    REQUIRE_MOVIE_LIBRARY();
#if defined(DQ8_MOVIE_LIBRARY)
    const auto library = DynamicLibrary::open(library_file);
#if !defined(_WIN32)
    // Never unloaded, as the host's loader keeps it (movie_library.cpp): FFmpeg's dependencies allocate at
    // load.
    REQUIRE(::dlopen(library_file.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD | RTLD_NODELETE) != nullptr);
#endif
    for (const std::string& name : abi_functions) {
        INFO(name);
        CHECK(library.symbol(name.c_str()) != nullptr);
    }
    const auto get = [&](const char* name) { return library.symbol(name); };
    decltype(&dq8_mvd_abi_version) version = nullptr;
    decltype(&dq8_mvd_create) create = nullptr;
    decltype(&dq8_mvd_destroy) destroy = nullptr;
    decltype(&dq8_mvd_send) send = nullptr;
    decltype(&dq8_mvd_receive) receive = nullptr;
    decltype(&dq8_mvd_flush) flush = nullptr;
    decltype(&dq8_mvd_reset) reset = nullptr;
    void* p = get("dq8_mvd_abi_version");
    std::memcpy(&version, &p, sizeof p);
    p = get("dq8_mvd_create");
    std::memcpy(&create, &p, sizeof p);
    p = get("dq8_mvd_destroy");
    std::memcpy(&destroy, &p, sizeof p);
    p = get("dq8_mvd_send");
    std::memcpy(&send, &p, sizeof p);
    p = get("dq8_mvd_receive");
    std::memcpy(&receive, &p, sizeof p);
    p = get("dq8_mvd_flush");
    std::memcpy(&flush, &p, sizeof p);
    p = get("dq8_mvd_reset");
    std::memcpy(&reset, &p, sizeof p);
    CHECK(version() == DQ8_MVD_ABI_VERSION);

    std::array<char, 256> error{};
    CHECK(create(nullptr, error.data(), error.size()) == nullptr);
    CHECK(std::string(error.data()).find("struct_size") != std::string::npos);
    dq8_mvd_options options{};
    options.struct_size = sizeof options;
    CHECK(create(&options, error.data(), error.size()) == nullptr);
    CHECK(std::string(error.data()).find("max_width") != std::string::npos);
    options.max_width = 64;
    options.max_height = 48;
    options.flags = 1;
    CHECK(create(&options, nullptr, 0) == nullptr);
    options.flags = 0;

    // A picture larger than the caller accepts is an error, not a picture.
    dq8_mvd* small = create(&options, error.data(), error.size());
    REQUIRE(small != nullptr);
    const Stream big = make_stream(2, 0, 80, 48);
    MpegEsModel model;
    model.feed(big.es);
    dq8_mvd_picture out{};
    int received = 0;
    while (auto coded = model.take_coded()) {
        CHECK(send(small, coded->bytes.data(), coded->bytes.size(), coded->index) == 0);
    }
    CHECK(flush(small) == 0);
    int r = 0;
    while ((r = receive(small, &out)) != 0) {
        CHECK(r < 0);
        ++received;
    }
    CHECK(received == 2);
    CHECK(send(small, big.es.data(), big.es.size(), 0) == -1); // after a flush: reset first
    reset(small);
    CHECK(send(small, nullptr, 0, 0) == -1);
    destroy(small);

    // Null handles are tolerated.
    destroy(nullptr);
    reset(nullptr);
    CHECK(send(nullptr, big.es.data(), big.es.size(), 0) == -1);
    CHECK(receive(nullptr, &out) == -1);
    CHECK(flush(nullptr) == -1);
#endif
}
