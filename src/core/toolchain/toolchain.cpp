// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The setup's toolchain (dq8/core/toolchain.h): finding the compiler, cmake and ninja, their versions, and
// advice per platform and Linux distribution family when one is missing or too old.
#include "dq8/core/toolchain.h"

#include "dq8/core/process.h"
#include "dq8/core/recomp/module_build.h"
#include "platform/files.h"
#include "platform/paths.h"
// Session E's bundle layout (installed_toolchain), once its header is in the tree.
#if __has_include("toolchain/download.h")
#include "toolchain/download.h"
#define DQ8_TOOLCHAIN_HAVE_DOWNLOAD 1
#else
#define DQ8_TOOLCHAIN_HAVE_DOWNLOAD 0
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <system_error>

namespace dq8::core::toolchain {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Forward slashes, as cmake wants paths.
std::string generic(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

fs::path executable_name(const fs::path& dir, std::string_view name) {
#if defined(_WIN32)
    return dir / (std::string(name) + ".exe");
#else
    return dir / std::string(name);
#endif
}

// The first candidate that resolves to an executable file.
std::optional<fs::path> first_program(const std::vector<fs::path>& candidates) {
    for (const fs::path& c : candidates) {
        if (c.empty()) {
            continue;
        }
        if (const auto found = find_program(c)) {
            return found;
        }
    }
    return std::nullopt;
}

// `<tool> --version`: the whole output (stdout, then stderr), or empty when it did not run.
std::string version_text(const fs::path& program) {
    ProcessOptions p;
    p.program = program;
    p.args = {"--version"};
    p.timeout = std::chrono::seconds(10);
    p.capture_limit = 64u << 10;
    try {
        const ProcessResult r = run_process(p);
        return r.out + r.err;
    } catch (const Error&) {
        return {};
    }
}

Tool probe(const std::optional<fs::path>& path, std::string* text_out = nullptr) {
    Tool tool;
    if (!path) {
        return tool;
    }
    tool.found = true;
    tool.path = *path;
    const std::string text = version_text(*path);
    const std::string first_line = text.substr(0, text.find('\n'));
    tool.version = parse_version(first_line);
    if (tool.version.empty()) {
        tool.version = parse_version(text);
    }
    if (text_out != nullptr) {
        *text_out = text;
    }
    return tool;
}

bool contains(std::string_view text, std::string_view word) {
    return text.find(word) != std::string_view::npos;
}

std::string family_of(std::string_view id) {
    static const std::vector<std::pair<std::vector<std::string_view>, std::string_view>> families{
        {{"debian", "ubuntu", "linuxmint", "pop", "raspbian", "elementary", "zorin", "kali"}, "debian"},
        {{"fedora", "rhel", "centos", "rocky", "almalinux", "ol", "amzn"}, "fedora"},
        {{"arch", "manjaro", "endeavouros", "cachyos", "garuda", "artix"}, "arch"},
        {{"opensuse", "opensuse-leap", "opensuse-tumbleweed", "suse", "sles"}, "suse"},
        {{"alpine"}, "alpine"},
        {{"gentoo"}, "gentoo"},
        {{"void"}, "void"},
        {{"nixos"}, "nixos"},
    };
    for (const auto& [ids, family] : families) {
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
            return std::string(family);
        }
    }
    return {};
}

// One os-release value: KEY=value, KEY="value" or KEY='value'.
std::string os_release_value(std::string_view text, std::string_view key) {
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == '=') {
            std::string_view value = line.substr(key.size() + 1);
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) {
                value.remove_suffix(1);
            }
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
                value.back() == value.front()) {
                value = value.substr(1, value.size() - 2);
            }
            return std::string(value);
        }
    }
    return {};
}

} // namespace

Distribution parse_os_release(std::string_view text) {
    Distribution d;
    d.id = os_release_value(text, "ID");
    d.pretty_name = os_release_value(text, "PRETTY_NAME");
    d.family = family_of(d.id);
    if (d.family.empty() && d.id.rfind("opensuse", 0) == 0) {
        d.family = "suse";
    }
    const std::string like = os_release_value(text, "ID_LIKE");
    std::size_t start = 0;
    while (d.family.empty() && start < like.size()) {
        std::size_t end = like.find(' ', start);
        if (end == std::string::npos) {
            end = like.size();
        }
        d.family = family_of(std::string_view(like).substr(start, end - start));
        start = end + 1;
    }
    if (d.pretty_name.empty()) {
        d.pretty_name = d.id.empty() ? std::string("Linux") : d.id;
    }
    return d;
}

Distribution this_distribution() {
#if defined(_WIN32)
    return {"windows", "windows", "Windows"};
#elif defined(__APPLE__)
    return {"macos", "macos", "macOS"};
#else
    for (const char* file : {"/etc/os-release", "/usr/lib/os-release"}) {
        try {
            return parse_os_release(platform::read_text_file(file, 64u << 10));
        } catch (const Error&) {
            // Try the next location.
        }
    }
    return {"", "", "Linux"};
#endif
}

std::string parse_version(std::string_view text) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!digit(text[i]) || (i > 0 && (digit(text[i - 1]) || text[i - 1] == '.'))) {
            continue;
        }
        std::size_t j = i;
        int parts = 0;
        for (;;) {
            const std::size_t part = j;
            while (j < text.size() && digit(text[j])) {
                ++j;
            }
            if (j == part) {
                break;
            }
            ++parts;
            if (parts == 3 || j + 1 >= text.size() || text[j] != '.' || !digit(text[j + 1])) {
                break;
            }
            ++j;
        }
        if (parts >= 2) {
            return std::string(text.substr(i, j - i));
        }
        i = j;
    }
    return {};
}

int compare_versions(std::string_view a, std::string_view b) {
    const auto next = [](std::string_view& s) {
        long n = 0;
        while (!s.empty() && s.front() >= '0' && s.front() <= '9') {
            n = n * 10 + (s.front() - '0');
            s.remove_prefix(1);
        }
        if (!s.empty()) {
            s.remove_prefix(1); // the dot
        }
        return n;
    };
    while (!a.empty() || !b.empty()) {
        const long x = next(a);
        const long y = next(b);
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

Toolchain detect(const DetectOptions& o) {
    Toolchain t;
    t.distribution = this_distribution();
    t.bundled_root = o.bundled_root;
    if (t.bundled_root.empty()) {
        try {
            t.bundled_root = platform::user_data_dir() / "toolchain";
        } catch (const Error&) {
            // No home directory: no bundled toolchain either.
        }
    }
    // The installed bundle: session E's <bundled_root>/<version>/ once its download code is in the tree,
    // else <bundled_root> itself.
    fs::path bundle = t.bundled_root;
#if DQ8_TOOLCHAIN_HAVE_DOWNLOAD
    if (!t.bundled_root.empty()) {
        const std::optional<fs::path> installed = installed_toolchain(t.bundled_root);
        bundle = installed ? *installed : fs::path();
    }
#endif
    const fs::path bundled_bin = bundle.empty() ? fs::path() : bundle / "bin";

    // The bundle first (on Windows the only toolchain host and modules may share, ADR-0018), then CC/CXX,
    // then PATH.
    std::vector<fs::path> cxx;
    std::vector<fs::path> cc;
    if (!bundled_bin.empty()) {
        cxx.push_back(executable_name(bundled_bin, "clang++"));
        cxx.push_back(executable_name(bundled_bin, "x86_64-w64-mingw32-clang++"));
        cc.push_back(executable_name(bundled_bin, "clang"));
    }
    if (o.use_environment) {
        if (const std::string v = environment("CXX"); !v.empty()) {
            cxx.emplace_back(v);
        }
        if (const std::string v = environment("CC"); !v.empty()) {
            cc.emplace_back(v);
        }
    }
    if (o.use_path) {
        for (const char* name : {"c++", "g++", "clang++"}) {
            cxx.emplace_back(name);
        }
        for (const char* name : {"cc", "gcc", "clang"}) {
            cc.emplace_back(name);
        }
    }
    const auto shipped = [&](std::string_view name, std::initializer_list<const char*> path_names) {
        std::vector<fs::path> candidates;
        if (!o.executable_dir.empty()) {
            candidates.push_back(executable_name(o.executable_dir, name));
        }
        if (!bundled_bin.empty()) {
            candidates.push_back(executable_name(bundled_bin, name));
        }
        if (o.use_path) {
            for (const char* n : path_names) {
                candidates.emplace_back(n);
            }
        }
        return candidates;
    };

    std::string cxx_text;
    t.cxx = probe(first_program(cxx), &cxx_text);
    t.cc = probe(first_program(cc));
    t.cmake = probe(first_program(shipped("cmake", {"cmake"})));
    t.ninja = probe(first_program(shipped("ninja", {"ninja", "ninja-build", "samu"})));
    if (t.cxx.found) {
        std::error_code ec;
        t.bundled = !bundled_bin.empty() && fs::equivalent(t.cxx.path.parent_path(), bundled_bin, ec);
        if (t.bundled) {
            t.bundle_dir = bundle;
            t.compiler_id = "llvm-mingw";
        } else if (contains(cxx_text, "clang")) {
            t.compiler_id = "clang";
        } else if (contains(cxx_text, "Free Software Foundation") || contains(cxx_text, "GCC") ||
                   contains(cxx_text, "g++")) {
            t.compiler_id = "gnu";
        }
    }
    return t;
}

std::string install_advice(std::string_view tool, const Distribution& d) {
    const bool compiler = tool == "c++ compiler";
    const bool cmake = tool == "cmake";
    const std::string& f = d.family;
    std::string package;
    std::string command;
    if (f == "debian") {
        package = compiler ? "g++" : cmake ? "cmake" : "ninja-build";
        command = "sudo apt install " + package;
    } else if (f == "fedora") {
        package = compiler ? "gcc-c++" : cmake ? "cmake" : "ninja-build";
        command = "sudo dnf install " + package;
    } else if (f == "arch") {
        package = compiler ? "gcc" : cmake ? "cmake" : "ninja";
        command = "sudo pacman -S --needed " + package;
    } else if (f == "suse") {
        package = compiler ? "gcc-c++" : cmake ? "cmake" : "ninja";
        command = "sudo zypper install " + package;
    } else if (f == "alpine") {
        package = compiler ? "g++" : cmake ? "cmake" : "samurai";
        command = "sudo apk add " + package;
    } else if (f == "gentoo") {
        package = compiler ? "sys-devel/gcc" : cmake ? "dev-build/cmake" : "dev-build/ninja";
        command = "sudo emerge --ask " + package;
    } else if (f == "void") {
        package = compiler ? "gcc" : cmake ? "cmake" : "ninja";
        command = "sudo xbps-install " + package;
    } else if (f == "nixos") {
        package = compiler ? "gcc" : cmake ? "cmake" : "ninja";
        command = "nix-shell -p gcc cmake ninja";
    } else if (f == "macos") {
        command =
            compiler ? "xcode-select --install" : std::string("brew install ") + (cmake ? "cmake" : "ninja");
    } else if (f == "windows") {
        return "Let the setup download its toolchain (llvm-mingw with cmake and ninja), or put clang++, "
               "cmake "
               "and ninja on PATH.";
    }
    const std::string what = compiler ? "a C++20 compiler (GCC 11 or Clang 14 or newer)"
                             : cmake  ? "CMake 3.25 or newer"
                                      : "Ninja";
    if (command.empty()) {
        return "Install " + what + " with your distribution's package manager.";
    }
    return "Install " + what + ": " + command;
}

std::vector<Problem> check(const Toolchain& t) {
    std::vector<Problem> problems;
    const auto add = [&](const char* tool, const std::string& what) {
        problems.push_back(
            {ErrorCode::toolchain_incomplete, tool, what + ". " + install_advice(tool, t.distribution)});
    };
    if (!t.cxx.found) {
        add("c++ compiler", "No C++ compiler was found");
    } else if (!t.cxx.version.empty()) {
        const char* minimum = t.compiler_id == "gnu"                                      ? "11"
                              : t.compiler_id == "clang" || t.compiler_id == "llvm-mingw" ? "14"
                                                                                          : nullptr;
        if (minimum != nullptr && compare_versions(t.cxx.version, minimum) < 0) {
            add("c++ compiler", "The C++ compiler " + utf8(t.cxx.path) + " is version " + t.cxx.version +
                                    ", older than " + minimum + " (C++20 is needed)");
        }
    }
    if (!t.cmake.found) {
        add("cmake", "CMake was not found");
    } else if (!t.cmake.version.empty() && compare_versions(t.cmake.version, "3.25") < 0) {
        add("cmake", "CMake " + t.cmake.version + " is older than 3.25");
    }
    if (!t.ninja.found) {
        add("ninja", "Ninja was not found");
    }
    return problems;
}

std::vector<std::string> cmake_arguments(const Toolchain& t) {
    std::vector<std::string> args;
    if (t.cxx.found) {
        args.push_back("-DCMAKE_CXX_COMPILER=" + generic(t.cxx.path));
    }
    if (t.ninja.found) {
        args.push_back("-DCMAKE_MAKE_PROGRAM=" + generic(t.ninja.path));
    }
    return args;
}

unsigned default_jobs() {
    return recomp::default_build_jobs();
}

BundleOffer bundle_offer() {
    BundleOffer offer;
#if defined(_WIN32) && __has_include("toolchain/download.h")
    const DownloadPin& pin = pinned_llvm_mingw();
    offer.available = true;
    offer.version = pin.version;
    offer.download_bytes = pin.size;
#endif
    return offer;
}

} // namespace dq8::core::toolchain
