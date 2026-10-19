// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8ctl: command-line front end over libdq8core. Same operations as the launcher, machine-readable output.
#include "dq8/core/version.h"

#include <cstdio>
#include <cstring>

namespace {

int usage(std::FILE* out) {
    std::fputs("usage: dq8ctl <command> [options]\n"
               "\n"
               "commands:\n"
               "  verify <iso>     check a disc image against the known region profiles\n"
               "  extract <iso>    extract the boot ELF, overlays and IOP modules into baserom/\n"
               "  info <path>      describe a disc image, overlay file or boot ELF\n"
               "  --version        print the version\n",
               out);
    return out == stdout ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage(stderr);
    }
    const char* command = argv[1];
    if (std::strcmp(command, "--version") == 0 || std::strcmp(command, "-V") == 0) {
        std::printf("dq8ctl %s\n", dq8::core::version());
        return 0;
    }
    if (std::strcmp(command, "--help") == 0 || std::strcmp(command, "-h") == 0 ||
        std::strcmp(command, "help") == 0) {
        return usage(stdout);
    }
    if (std::strcmp(command, "verify") == 0 || std::strcmp(command, "extract") == 0 ||
        std::strcmp(command, "info") == 0) {
        std::fprintf(stderr, "dq8ctl: '%s' is not implemented yet (milestone M1)\n", command);
        return 2;
    }
    std::fprintf(stderr, "dq8ctl: unknown command '%s'\n", command);
    return usage(stderr);
}
