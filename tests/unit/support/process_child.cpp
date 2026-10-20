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
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("dq8_test_child: missing mode\n", stderr);
        return 100;
    }
    const std::string mode = argv[1];
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
