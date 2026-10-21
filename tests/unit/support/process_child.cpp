// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8_test_child: a portable stand-in program for the process-runner tests (no shell, no platform tools).
//
//   echo-args ARG...   prints each argument as "[ARG]" on its own line
//   streams            writes "to-stdout" to stdout and "to-stderr" to stderr
//   exit N             exits with status N
//   cwd                prints the current directory
//   env NAME           prints the variable's value, or "<unset>"
//   sleep MS           sleeps, then exits 0
//   flood BYTES        writes BYTES bytes to each of stdout and stderr, alternating 4 KiB pieces
//   CONFIG.toml        stands in for ps2_recomp, called as the driver calls it (`dq8_test_child unit.toml`):
//                      reads `output = "DIR"` and `variant = "V"`, writes DIR/a.cpp, DIR/b.cpp (holding V)
//                      and DIR/register_functions.cpp and prints a report block; with a `fail = true` line it
//                      writes only DIR/a.cpp, prints an error and exits 1
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

int fake_recomp(const char* config) {
    std::ifstream in(config);
    std::string line;
    std::string output;
    std::string variant;
    bool fail = false;
    auto quoted = [](const std::string& l) {
        const auto a = l.find('"');
        const auto b = l.rfind('"');
        return a == std::string::npos || b <= a ? std::string() : l.substr(a + 1, b - a - 1);
    };
    while (std::getline(in, line)) {
        if (line.rfind("output = ", 0) == 0) {
            output = quoted(line);
        } else if (line.rfind("variant = ", 0) == 0) {
            variant = quoted(line);
        } else if (line == "fail = true") {
            fail = true;
        }
    }
    if (output.empty()) {
        std::fputs("fake-recomp: no output in the config\n", stderr);
        return 1;
    }
    const std::filesystem::path dir(output);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "a.cpp") << "// a\n";
    if (fail) {
        std::fputs("Recompilation failed\n", stderr);
        return 1;
    }
    std::ofstream(dir / "b.cpp") << "// b " << variant << "\n";
    std::ofstream(dir / "register_functions.cpp") << "// table\n";
    std::printf("[recompiler] parsing config\n\n========== PS2Recomp report ==========\n"
                "Functions discovered: 3\n"
                "Functions processed: 3, recompiled: 2, stubs: 1, skipped: 0, decode failures: 0\n"
                "Unhandled instructions: 0\nWarnings: 0, errors: 0\n"
                "======================================\nRecompilation completed successfully\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("dq8_test_child: missing mode\n", stderr);
        return 100;
    }
    const std::string mode = argv[1];
    // Invoked the way the recompiler driver invokes ps2_recomp: one argument, the config file.
    if (argc == 2 && mode.size() > 5 && mode.compare(mode.size() - 5, 5, ".toml") == 0) {
        return fake_recomp(argv[1]);
    }
    if (mode == "echo-args") {
        for (int i = 2; i < argc; ++i) {
            std::printf("[%s]\n", argv[i]);
        }
        return 0;
    }
    if (mode == "streams") {
        std::fputs("to-stdout\n", stdout);
        std::fputs("to-stderr\n", stderr);
        return 0;
    }
    if (mode == "exit" && argc == 3) {
        return std::atoi(argv[2]);
    }
    if (mode == "cwd") {
        std::printf("%s\n", std::filesystem::current_path().string().c_str());
        return 0;
    }
    if (mode == "env" && argc == 3) {
        const char* value = std::getenv(argv[2]);
        std::printf("%s\n", value != nullptr ? value : "<unset>");
        return 0;
    }
    if (mode == "sleep" && argc == 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atol(argv[2])));
        return 0;
    }
    if (mode == "flood" && argc == 3) {
        const long total = std::atol(argv[2]);
        std::string piece(4096, 'x');
        for (long done = 0; done < total; done += static_cast<long>(piece.size())) {
            const std::size_t n = static_cast<std::size_t>(std::min<long>(total - done, 4096));
            std::fwrite(piece.data(), 1, n, stdout);
            std::fwrite(piece.data(), 1, n, stderr);
        }
        return 0;
    }
    std::fprintf(stderr, "dq8_test_child: unknown mode '%s'\n", mode.c_str());
    return 100;
}
