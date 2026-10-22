// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8_launcher_fake_game: stands in for dq8-game in the launcher's supervisor tests. It takes dq8-game's
// arguments (--iso X --module-dir Y --window [--config Z] --status -), echoes them on stderr, and plays the
// scenario named by the --iso value's file name:
//   ok        started, first_frame, fps, warning, then exit window-closed with status 0
//   error     started, then exit error smoke-failed with status 2
//   silent    nothing on the status channel; exits 3
//   hang      started, then waits to be stopped
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    std::string iso;
    bool status = false;
    for (int i = 1; i < argc; ++i) {
        std::fprintf(stderr, "arg: %s\n", argv[i]);
        if (std::strcmp(argv[i], "--iso") == 0 && i + 1 < argc) {
            iso = argv[i + 1];
        }
        if (std::strcmp(argv[i], "--status") == 0 && i + 1 < argc && std::strcmp(argv[i + 1], "-") == 0) {
            status = true;
        }
    }
    const std::size_t slash = iso.find_last_of("/\\");
    const std::string scenario = slash == std::string::npos ? iso : iso.substr(slash + 1);
    if (!status) {
        std::fputs("fake game: no --status -\n", stderr);
        return 2;
    }
    if (scenario == "silent") {
        std::puts("an ordinary line on stdout");
        return 3;
    }
    std::puts(R"({"ev":"started","version":"0.0.1","profile":"FAKE","pid":42,"window":true})");
    std::fflush(stdout);
    if (scenario == "error") {
        std::puts(R"({"ev":"exit","status":2,"reason":"error","code":"smoke-failed","message":"no title"})");
        return 2;
    }
    if (scenario == "hang") {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    std::puts(R"({"ev":"first_frame","ms":120.5})");
    std::puts(R"({"ev":"fps","fps":59.9,"speed":0.99})");
    std::puts(R"({"ev":"warning","code":"audio-underrun","message":"the audio buffer ran dry"})");
    std::puts(R"({"ev":"exit","status":0,"reason":"window-closed"})");
    return 0;
}
