// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// dq8-gs-parallel's C ABI (include/dq8/gs/gs_parallel_abi.h) and the backend that loads it
// (runtime/gs/gs_parallel_backend.h). No GPU is needed: the export table is read from the file, creation is
// checked to fail cleanly when the Vulkan loader finds no driver, and bad options are refused before Vulkan
// is touched. The one test that draws needs a GPU and is hidden ([.gpu]); CTest runs it as
// gs_parallel.gpu_render, label "gpu".

#include "dq8/gs/gs_parallel_abi.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_parallel_backend.h"
#include "runtime/gs/priv_regs.h"
#include "runtime/platform/dynlib.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <elf.h>
#endif

namespace {

namespace fs = std::filesystem;

const fs::path library_file = DQ8_GS_PARALLEL_LIBRARY;

const std::set<std::string> abi_functions = {
    "dq8_gsp_abi_version",  "dq8_gsp_create", "dq8_gsp_destroy",           "dq8_gsp_reset",
    "dq8_gsp_gif_transfer", "dq8_gsp_vsync",  "dq8_gsp_read_scanout_rgba", "dq8_gsp_read_transfer",
    "dq8_gsp_flush"};

// Sets an environment variable for the lifetime of the object (POSIX; the library is not built on MSVC).
class ScopedEnv {
  public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name); old != nullptr) {
            old_ = old;
        }
        if (value != nullptr) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~ScopedEnv() {
        if (old_) {
            ::setenv(name_.c_str(), old_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

  private:
    std::string name_;
    std::optional<std::string> old_;
};

// Points the Vulkan loader at a driver manifest that does not exist: instance creation then fails as on a
// machine without a GPU driver, whether or not this one has a GPU.
struct NoVulkanDriver {
    ScopedEnv drivers{"VK_DRIVER_FILES", "/nonexistent/dq8-no-driver.json"};
    ScopedEnv icds{"VK_ICD_FILENAMES", "/nonexistent/dq8-no-driver.json"};
    ScopedEnv add_drivers{"VK_ADD_DRIVER_FILES", nullptr};
};

template <typename Fn> Fn get(const dq8::rt::DynamicLibrary& library, const char* name) {
    void* symbol = library.symbol(name);
    REQUIRE(symbol != nullptr);
    Fn fn = nullptr;
    std::memcpy(&fn, &symbol, sizeof fn);
    return fn;
}

#if defined(__linux__)
// The names of the symbols an ELF64 shared library defines in its dynamic symbol table (what nm -D
// --defined-only prints, without the version-node symbols an anonymous version script never adds).
std::set<std::string> defined_dynamic_symbols(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    REQUIRE(in);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE(bytes.size() > sizeof(Elf64_Ehdr));
    Elf64_Ehdr eh{};
    std::memcpy(&eh, bytes.data(), sizeof eh);
    REQUIRE(std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0);
    REQUIRE(eh.e_ident[EI_CLASS] == ELFCLASS64);
    REQUIRE(eh.e_shentsize == sizeof(Elf64_Shdr));
    REQUIRE(eh.e_shoff + std::size_t{eh.e_shnum} * sizeof(Elf64_Shdr) <= bytes.size());

    std::vector<Elf64_Shdr> sections(eh.e_shnum);
    std::memcpy(sections.data(), bytes.data() + eh.e_shoff, sections.size() * sizeof(Elf64_Shdr));
    std::set<std::string> names;
    for (const Elf64_Shdr& sh : sections) {
        if (sh.sh_type != SHT_DYNSYM) {
            continue;
        }
        REQUIRE(sh.sh_link < sections.size());
        const Elf64_Shdr& strtab = sections[sh.sh_link];
        REQUIRE(sh.sh_offset + sh.sh_size <= bytes.size());
        REQUIRE(strtab.sh_offset + strtab.sh_size <= bytes.size());
        for (std::size_t i = 0; i < sh.sh_size / sizeof(Elf64_Sym); ++i) {
            Elf64_Sym sym{};
            std::memcpy(&sym, bytes.data() + sh.sh_offset + i * sizeof(Elf64_Sym), sizeof sym);
            const unsigned bind = ELF64_ST_BIND(sym.st_info);
            if (sym.st_shndx == SHN_UNDEF || sym.st_name == 0 || (bind != STB_GLOBAL && bind != STB_WEAK)) {
                continue;
            }
            REQUIRE(sym.st_name < strtab.sh_size);
            names.emplace(bytes.data() + strtab.sh_offset + sym.st_name);
        }
    }
    return names;
}
#endif

} // namespace

#if defined(__linux__)
TEST_CASE("dq8-gs-parallel exports exactly the dq8_gsp_* C ABI", "[gs_parallel]") {
    // Nothing of paraLLEl-GS, Granite, volk or the C++ runtime may leak out of the library (ADR-0004).
    CHECK(defined_dynamic_symbols(library_file) == abi_functions);
}
#endif

TEST_CASE("dq8-gs-parallel loads and reports ABI version 1", "[gs_parallel]") {
    const auto library = dq8::rt::DynamicLibrary::open(library_file);
    for (const std::string& name : abi_functions) {
        INFO(name);
        CHECK(library.symbol(name.c_str()) != nullptr);
    }
    CHECK(get<dq8_gsp_abi_version_fn>(library, "dq8_gsp_abi_version")() == DQ8_GSP_ABI_VERSION);
}

TEST_CASE("dq8_gsp_create refuses bad options before touching Vulkan", "[gs_parallel]") {
    const auto library = dq8::rt::DynamicLibrary::open(library_file);
    const auto create = get<dq8_gsp_create_fn>(library, "dq8_gsp_create");
    std::array<char, 256> error{};

    CHECK(create(nullptr, error.data(), error.size()) == nullptr);
    CHECK(std::string(error.data()).find("struct_size") != std::string::npos);

    dq8_gsp_options options{};
    options.struct_size = sizeof options;
    options.super_sampling = 3;
    CHECK(create(&options, error.data(), error.size()) == nullptr);
    CHECK(std::string(error.data()).find("super_sampling") != std::string::npos);

    options.super_sampling = 1;
    options.flags = 1u << 31;
    CHECK(create(&options, error.data(), error.size()) == nullptr);
    CHECK(std::string(error.data()).find("flags") != std::string::npos);

    // A null or empty error buffer is allowed.
    CHECK(create(&options, nullptr, 0) == nullptr);
    CHECK(create(&options, error.data(), 0) == nullptr);
}

TEST_CASE("dq8_gsp_* tolerate a null handle", "[gs_parallel]") {
    const auto library = dq8::rt::DynamicLibrary::open(library_file);
    get<dq8_gsp_destroy_fn>(library, "dq8_gsp_destroy")(nullptr);
    get<dq8_gsp_reset_fn>(library, "dq8_gsp_reset")(nullptr);
    get<dq8_gsp_flush_fn>(library, "dq8_gsp_flush")(nullptr);
    const std::array<std::uint8_t, 16> qword{};
    get<dq8_gsp_gif_transfer_fn>(library, "dq8_gsp_gif_transfer")(nullptr, 3, qword.data(), qword.size());
    const dq8::gs::PrivRegs priv;
    dq8_gsp_frame_info info{1, 1, 1};
    CHECK(get<dq8_gsp_vsync_fn>(library, "dq8_gsp_vsync")(nullptr, priv.page.data(), 0, &info) != 0);
    CHECK(info.width == 0);
    std::uint32_t w = 7;
    std::uint32_t h = 7;
    std::array<std::uint8_t, 16> pixels{};
    CHECK(get<dq8_gsp_read_scanout_rgba_fn>(library, "dq8_gsp_read_scanout_rgba")(
              nullptr, pixels.data(), pixels.size(), &w, &h) != 0);
    CHECK(w == 0);
    CHECK(h == 0);
    CHECK(get<dq8_gsp_read_transfer_fn>(library, "dq8_gsp_read_transfer")(nullptr, pixels.data(), 16) == 0);
}

TEST_CASE("dq8_gsp_create fails cleanly without a Vulkan device", "[gs_parallel]") {
    const NoVulkanDriver no_driver;
    const auto library = dq8::rt::DynamicLibrary::open(library_file);
    const auto create = get<dq8_gsp_create_fn>(library, "dq8_gsp_create");
    dq8_gsp_options options{};
    options.struct_size = sizeof options;
    options.super_sampling = 1;
    std::array<char, 1024> error{};
    dq8_gsp* gs = create(&options, error.data(), error.size());
    if (gs != nullptr) {
        get<dq8_gsp_destroy_fn>(library, "dq8_gsp_destroy")(gs);
    }
    CHECK(gs == nullptr);
    const std::string message(error.data());
    INFO(message);
    CHECK(message.find("Vulkan") != std::string::npos);
}

TEST_CASE("make_parallel_gs_backend reports a missing library and a missing device", "[gs_parallel]") {
    dq8::gs::ParallelGsConfig options;
    options.library = "/nonexistent/libdq8-gs-parallel.so";
    CHECK_THROWS_AS(dq8::gs::make_parallel_gs_backend(options), dq8::gs::ParallelGsError);

    const NoVulkanDriver no_driver;
    options.library = library_file;
    try {
        (void)dq8::gs::make_parallel_gs_backend(options);
        FAIL("a backend was created without a Vulkan driver");
    } catch (const dq8::gs::ParallelGsError& e) {
        INFO(e.what());
        CHECK(std::string(e.what()).find("paraLLEl-GS backend") != std::string::npos);
    }
}

TEST_CASE("default_parallel_gs_library honours DQ8_GS_PARALLEL", "[gs_parallel]") {
    {
        const ScopedEnv env("DQ8_GS_PARALLEL", "/opt/somewhere/libdq8-gs-parallel.so");
        CHECK(dq8::gs::default_parallel_gs_library() == fs::path("/opt/somewhere/libdq8-gs-parallel.so"));
    }
    // Without it, a test executable (not next to the library) finds this build's copy.
    const ScopedEnv env("DQ8_GS_PARALLEL", nullptr);
    const fs::path found = dq8::gs::default_parallel_gs_library();
    CHECK(found.filename() == dq8::gs::parallel_gs_library_file_name());
    CHECK(library_file.filename() == dq8::gs::parallel_gs_library_file_name()); // the name CMake gives it
    CHECK(fs::equivalent(found, library_file));
}

namespace {

// A+D writes as one PACKED GIF packet.
std::vector<std::uint8_t> a_d_packet(const std::vector<std::pair<std::uint8_t, std::uint64_t>>& writes) {
    std::vector<std::uint8_t> out((writes.size() + 1) * 16);
    const std::uint64_t tag_lo =
        writes.size() | (std::uint64_t{1} << 15) | (std::uint64_t{1} << 60); // EOP, NREG 1
    const std::uint64_t tag_hi = 0xE;                                        // A+D
    std::memcpy(out.data(), &tag_lo, 8);
    std::memcpy(out.data() + 8, &tag_hi, 8);
    for (std::size_t i = 0; i < writes.size(); ++i) {
        std::memcpy(out.data() + 16 * (i + 1), &writes[i].second, 8);
        const std::uint64_t reg = writes[i].first;
        std::memcpy(out.data() + 16 * (i + 1) + 8, &reg, 8);
    }
    return out;
}

std::uint64_t sprite_xyz(std::uint32_t x, std::uint32_t y) {
    return std::uint64_t{x << 4} | (std::uint64_t{y << 4} << 16); // 12.4 fixed point, Z 0
}

// Circuit 1 shows a 640x448 PSMCT32 buffer at block 0, NTSC, interlaced frame mode (public GS layouts).
dq8::gs::PrivRegs ntsc_640x448() {
    dq8::gs::PrivRegs priv;
    priv.write64(dq8::gs::PrivReg::pmode, 0x1u | (1u << 2) | (1u << 5) | (0xFFu << 8)); // EN1, MMOD, ALP 255
    priv.write64(dq8::gs::PrivReg::smode1, 0x740834504ull);                             // NTSC, analog
    priv.write64(dq8::gs::PrivReg::smode2, 1);                                          // INT, FRAME
    priv.write64(dq8::gs::PrivReg::dispfb1, std::uint64_t{10} << 9);                    // FBW 10
    priv.write64(dq8::gs::PrivReg::display1,
                 636u | (50u << 12) | (3u << 23) | (std::uint64_t{2559} << 32) | (std::uint64_t{447} << 44));
    return priv;
}

} // namespace

TEST_CASE("the parallel backend draws sprites and scans them out", "[.gpu][gs_parallel]") {
    dq8::gs::ParallelGsConfig options;
    options.library = library_file;
    std::unique_ptr<dq8::gs::IGsBackend> backend;
    try {
        backend = dq8::gs::make_parallel_gs_backend(options);
    } catch (const dq8::gs::ParallelGsError& e) {
        SKIP(e.what());
    }
    CHECK(backend->caps().name == "parallel");
    CHECK(backend->caps().produces_pixels);

    // A 640x448 PSMCT32 frame buffer at block 0: red everywhere, green in the top-left quarter.
    constexpr std::uint8_t prim = 0x00, rgbaq = 0x01, xyz2 = 0x05, xyoffset_1 = 0x18, prmodecont = 0x1A,
                           scissor_1 = 0x40, test_1 = 0x47, frame_1 = 0x4C, zbuf_1 = 0x4E;
    const std::uint64_t one = 0x3F800000ull << 32; // Q = 1.0
    const auto packet = a_d_packet({
        {frame_1, std::uint64_t{10} << 16},                                   // FBP 0, FBW 10 (640), PSMCT32
        {zbuf_1, 0x100u | (std::uint64_t{1} << 32)},                          // Z writes masked
        {scissor_1, (std::uint64_t{639} << 16) | (std::uint64_t{447} << 48)}, // 0..639, 0..447
        {xyoffset_1, 0},
        {test_1, (1u << 16) | (1u << 17)}, // depth test on, always pass
        {prmodecont, 1},
        {prim, 6}, // sprite
        {rgbaq, 0x800000FFu | one},
        {xyz2, sprite_xyz(0, 0)},
        {xyz2, sprite_xyz(640, 448)},
        {rgbaq, 0x8000FF00u | one},
        {xyz2, sprite_xyz(0, 0)},
        {xyz2, sprite_xyz(320, 224)},
    });
    backend->gif_transfer(dq8::gs::GifPath::path3, packet);

    dq8::gs::VsyncInfo vsync;
    vsync.priv = ntsc_640x448();
    backend->vsync(vsync);

    dq8::gs::FrameImage frame;
    REQUIRE(backend->read_frame(frame));
    REQUIRE(frame.width >= 320);
    REQUIRE(frame.height >= 224);
    REQUIRE(frame.rgba.size() == std::size_t{frame.width} * frame.height * 4);
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
        const std::uint8_t* p = frame.rgba.data() + (std::size_t{y} * frame.width + x) * 4;
        return std::array<std::uint8_t, 4>{p[0], p[1], p[2], p[3]};
    };
    const auto green = pixel(frame.width / 4, frame.height / 4);
    const auto red = pixel(frame.width * 3 / 4, frame.height * 3 / 4);
    INFO("size " << frame.width << "x" << frame.height);
    CHECK(green[1] > 200);
    CHECK(green[0] < 50);
    CHECK(red[0] > 200);
    CHECK(red[1] < 50);
    CHECK(red[3] == 0xFF);
}

TEST_CASE("dq8_gsp_read_scanout_rgba reports the size when the buffer is too small", "[.gpu][gs_parallel]") {
    const auto library = dq8::rt::DynamicLibrary::open(library_file);
    dq8_gsp_options options{};
    options.struct_size = sizeof options;
    options.super_sampling = 1;
    std::array<char, 1024> error{};
    dq8_gsp* gs = get<dq8_gsp_create_fn>(library, "dq8_gsp_create")(&options, error.data(), error.size());
    if (gs == nullptr) {
        SKIP(error.data());
    }
    const dq8::gs::PrivRegs priv = ntsc_640x448();
    dq8_gsp_frame_info info{};
    CHECK(get<dq8_gsp_vsync_fn>(library, "dq8_gsp_vsync")(gs, priv.page.data(), 0, &info) == 0);
    CHECK(info.width >= 320);
    CHECK(info.height >= 224);

    const auto read = get<dq8_gsp_read_scanout_rgba_fn>(library, "dq8_gsp_read_scanout_rgba");
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::vector<std::uint8_t> small(16);
    CHECK(read(gs, small.data(), small.size(), &w, &h) != 0);
    CHECK(w == info.width);
    CHECK(h == info.height);
    std::vector<std::uint8_t> pixels(std::size_t{w} * h * 4);
    CHECK(read(gs, pixels.data(), pixels.size(), &w, &h) == 0);
    get<dq8_gsp_destroy_fn>(library, "dq8_gsp_destroy")(gs);
}
