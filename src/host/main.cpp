// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8-game: the game executable. It verifies the disc (or takes a developer ELF), loads and validates the
// locally built game modules, and runs the machine (src/host/README.md: options, exit status, events).
#include "dq8/core/error.h"
#include "dq8/core/version.h"
#include "host/options.h"
#include "host/run.h"
#include "host/session.h"

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    using namespace dq8::host;
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        json = json || std::string_view(argv[i]) == "--json";
    }
    try {
        const Options o = parse_options(argc, argv);
        if (o.help) {
            return usage(stdout);
        }
        if (o.version) {
            std::printf("dq8-game %s\n", std::string(dq8::core::version()).c_str());
            return 0;
        }
        return run_game(o, argv[0]);
    } catch (const UsageError& e) {
        if (!json) {
            std::fprintf(stderr, "dq8-game: %s\n\n", e.what());
            usage(stderr);
            return exit_error;
        }
        return report_failure(json, "usage", e.what());
    } catch (const SetupProblems& e) {
        return report_failure(json, "module-validation", e.what(), e.problems());
    } catch (const BackendError& e) {
        return report_failure(json, "backend", e.what(), {}, exit_backend);
    } catch (const dq8::core::Error& e) {
        return report_failure(json, dq8::core::to_string(e.code()), e.what());
    } catch (const std::exception& e) {
        return report_failure(json, "error", e.what());
    }
}
