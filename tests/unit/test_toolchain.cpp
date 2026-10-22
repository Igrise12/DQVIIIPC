// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M5 session A: finding and checking the setup's toolchain (dq8/core/toolchain.h).
#include "dq8/core/toolchain.h"
#include "support/iso_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>

using Catch::Matchers::ContainsSubstring;
namespace toolchain = dq8::core::toolchain;
namespace fs = std::filesystem;

TEST_CASE("toolchain: versions are read from --version text and compared numerically", "[m5][toolchain]") {
    CHECK(toolchain::parse_version("g++ (GCC) 16.2.1 20260810") == "16.2.1");
    CHECK(toolchain::parse_version("c++ (Ubuntu 13.2.0-23ubuntu4) 13.2.0") == "13.2.0");
    CHECK(toolchain::parse_version("g++-13 (Ubuntu 13.2.0-23ubuntu4) 13.2.0") == "13.2.0");
    CHECK(toolchain::parse_version("Ubuntu clang version 18.1.3 (1ubuntu1)") == "18.1.3");
    CHECK(toolchain::parse_version("cmake version 3.28.3\n\nCMake suite maintained") == "3.28.3");
    CHECK(toolchain::parse_version("1.11.1") == "1.11.1");
    CHECK(toolchain::parse_version("x86_64 build 7") == "");
    CHECK(toolchain::compare_versions("3.9", "3.25") < 0);
    CHECK(toolchain::compare_versions("3.25.0", "3.25") == 0);
    CHECK(toolchain::compare_versions("11", "10.5") > 0);
    CHECK(toolchain::compare_versions("4.4.4", "3.25") > 0);
}

TEST_CASE("toolchain: the distribution family comes from os-release", "[m5][toolchain]") {
    CHECK(toolchain::parse_os_release(
              "NAME=\"Ubuntu\"\nID=ubuntu\nID_LIKE=debian\nPRETTY_NAME=\"Ubuntu 24.04 LTS\"\n")
              .family == "debian");
    const toolchain::Distribution cachy = toolchain::parse_os_release(
        "NAME=\"CachyOS Linux\"\nPRETTY_NAME=\"CachyOS\"\nID=cachyos\nID_LIKE=arch\n");
    CHECK(cachy.family == "arch");
    CHECK(cachy.pretty_name == "CachyOS");
    CHECK(toolchain::parse_os_release("ID=fedora\n").family == "fedora");
    CHECK(toolchain::parse_os_release("ID=\"rocky\"\nID_LIKE=\"rhel centos fedora\"\n").family == "fedora");
    CHECK(toolchain::parse_os_release("ID=\"opensuse-tumbleweed\"\nID_LIKE=\"opensuse suse\"\n").family ==
          "suse");
    CHECK(toolchain::parse_os_release("ID=pop\nID_LIKE=\"ubuntu debian\"\n").family == "debian");
    CHECK(toolchain::parse_os_release("ID=somethingnew\n").family.empty());

    toolchain::Distribution d;
    d.family = "debian";
    CHECK_THAT(toolchain::install_advice("ninja", d), ContainsSubstring("sudo apt install ninja-build"));
    d.family = "fedora";
    CHECK_THAT(toolchain::install_advice("c++ compiler", d), ContainsSubstring("sudo dnf install gcc-c++"));
    d.family = "arch";
    CHECK_THAT(toolchain::install_advice("cmake", d), ContainsSubstring("sudo pacman -S --needed cmake"));
    d.family = "";
    CHECK_THAT(toolchain::install_advice("cmake", d), ContainsSubstring("CMake 3.25 or newer"));
}

TEST_CASE("toolchain: check names every missing or old tool with advice", "[m5][toolchain]") {
    toolchain::Toolchain t;
    t.distribution.family = "debian";
    const auto missing = toolchain::check(t);
    REQUIRE(missing.size() == 3);
    CHECK(missing[0].tool == "c++ compiler");
    CHECK(missing[0].code == dq8::core::ErrorCode::toolchain_incomplete);
    CHECK_THAT(missing[0].advice, ContainsSubstring("apt install g++"));
    CHECK(missing[1].tool == "cmake");
    CHECK(missing[2].tool == "ninja");

    t.cxx = {true, "/usr/bin/g++", "10.2.0"};
    t.compiler_id = "gnu";
    t.cmake = {true, "/usr/bin/cmake", "3.22.1"};
    t.ninja = {true, "/usr/bin/ninja", "1.10.1"};
    const auto old = toolchain::check(t);
    REQUIRE(old.size() == 2);
    CHECK_THAT(old[0].advice, ContainsSubstring("version 10.2.0, older than 11"));
    CHECK_THAT(old[1].advice, ContainsSubstring("CMake 3.22.1 is older than 3.25"));

    t.cxx.version = "13.2.0";
    t.cmake.version = "3.28.3";
    CHECK(toolchain::check(t).empty());
    t.compiler_id = "clang";
    t.cxx.version = "13.0.1";
    CHECK(toolchain::check(t).size() == 1);

    const auto args = toolchain::cmake_arguments(t);
    REQUIRE(args.size() == 2);
    CHECK(args[0] == "-DCMAKE_CXX_COMPILER=/usr/bin/g++");
    CHECK(args[1] == "-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja");
}

#if !defined(_WIN32)
TEST_CASE("toolchain: detect prefers the tools shipped beside the program and reads their versions",
          "[m5][toolchain]") {
    const dq8::test::TempDir tmp;
    const auto script = [&](const char* name, const char* output) {
        const fs::path p = tmp.path() / name;
        std::ofstream(p) << "#!/bin/sh\necho '" << output << "'\n";
        fs::permissions(p, fs::perms::owner_all);
    };
    script("cmake", "cmake version 3.20.0");
    script("ninja", "1.12.1");
    toolchain::DetectOptions o;
    o.executable_dir = tmp.path();
    o.bundled_root = tmp.path() / "no-bundle";
    o.use_path = false;
    o.use_environment = false;
    const toolchain::Toolchain t = toolchain::detect(o);
    CHECK_FALSE(t.cxx.found);
    REQUIRE(t.cmake.found);
    CHECK(t.cmake.path == tmp.path() / "cmake");
    CHECK(t.cmake.version == "3.20.0");
    CHECK(t.ninja.version == "1.12.1");
    const auto problems = toolchain::check(t);
    REQUIRE(problems.size() == 2); // no compiler, cmake too old
    CHECK(problems[0].tool == "c++ compiler");
    CHECK(problems[1].tool == "cmake");
}
#endif
