// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// TEMPORARY (M3 integration): M2's single-thread run (EeRuntime over KernelHle, no machine), built only while
// the M3 machine's implementations (sessions A, B and C) are not in dq8_runtime; src/host/CMakeLists.txt
// picks run_machine.cpp as soon as they are, and this file goes with the guard once main has them.
#include "dq8/core/error.h"
#include "host/run.h"
#include "host/session.h"
#include "runtime/ee/context.h"
#include "runtime/ee/ee_runtime.h"
#include "runtime/kernel/kernel_hle.h"

#include <cstdio>

namespace dq8::host {

int run_game(const Options& o, const char* argv0) {
    if (o.uses_machine) {
        throw UsageError("this build of dq8-game has no M3 machine yet (sessions A, B and C are not merged); "
                         "only the M2 options work");
    }
    if (o.display == Display::window) {
        throw UsageError("--window needs the M3 machine, which this build does not have yet");
    }
    Session s;
    setup_session(o, argv0, s);
    s.memory.set_trap_mmio(o.trap_mmio);
    if (!o.json) {
        s.memory.set_mmio_sink([](const rt::MmioAccess& a) {
            std::fprintf(stderr, "mmio: first %s at %s (%s), pc %s\n", mmio_access_name(a).c_str(),
                         rt::hex32(a.address).c_str(), std::string(rt::to_string(a.region)).c_str(),
                         rt::hex32(a.pc).c_str());
        });
        print_setup(s);
        std::fflush(stdout); // before the run's stderr diagnostics
    }
    rt::KernelHle kernel;
    rt::EeRuntime ee(s.memory, *s.dispatcher, kernel);
    rt::reset_context(ee.context(), s.elf.entry);
    rt::RunOptions run;
    run.stop_at_first_syscall = o.stop_at_first_syscall;
    run.max_dispatches = o.max_dispatches;
    run.trace_dispatch = o.trace_dispatch;
    const rt::RunResult result = ee.run(run);
    const int code = stopped_at_syscall(result.stop) && o.stop_at_first_syscall ? exit_ok : exit_guest_stop;
    if (o.json) {
        JsonWriter j;
        j.begin_object();
        j.member("ok", code == exit_ok);
        json_setup(j, s);
        json_stop(j, result.stop);
        j.member("dispatches", result.dispatches);
        json_context(j, ee.context());
        json_mmio(j, s.memory);
        j.member("exit_code", code);
        j.end_object();
        std::printf("%s\n", j.str().c_str());
    } else {
        print_stop(result.stop, result.dispatches, ee.context(), s.memory);
    }
    return code;
}

} // namespace dq8::host
