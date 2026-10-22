// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/region_profile.h"
#include "host/options.h"
#include "runtime/ee/overrides.h"
#include "runtime/gs/gs_pipeline.h"
#include "runtime/iop/iop.h"
#include "runtime/machine/machine_services.h"
#include "runtime/platform/platform.h"

#include <functional>
#include <memory>
#include <vector>

// What each M4 session adds to a dq8-game run (Docs/milestones/M4-playable.md, "Host wiring"). run_game
// builds one M4Wiring after the EE devices and the IOP configuration exist and before the IOP is built, and
// calls the four wire_* functions once each, in this order. Each session fills only its own function (its own
// .cpp file), so no session edits run_machine.cpp. On day 0 every function adds nothing.
namespace dq8::host {

struct M4Wiring {
    const Options& options;
    const core::RegionProfile* profile; // null in developer mode (--elf)
    gs::GsPipeline& pipeline;
    rt::IopConfig& iop; // still editable here: devices, cards, rtc, pad_feedback, strict_imports
    // The window (main thread only), or null headless and in builds without one.
    rt::IPlatform* platform = nullptr;
    // Devices attached after the IOP and the game layer, in this order; run_game keeps them alive past the
    // machine.
    std::vector<std::unique_ptr<rt::IDevice>> devices;
    // Overrides installed after the game layer's.
    std::vector<rt::OverrideSpec> overrides;
    // Main thread: after every device is attached and before the machine thread starts; after it has joined.
    std::vector<std::function<void()>> on_start;
    std::vector<std::function<void()>> on_stop;
    // Main thread, with --window: once per pass of the window loop, after the platform's events.
    std::vector<std::function<void()>> on_frame;
    // Fields added to the `start` event (what a run chose that a later run needs to repeat it: --rtc host's
    // date), and to the `end` event (deterministic only, after the machine thread has joined).
    std::vector<std::function<void(rt::MachineEvent&)>> start_fields;
    std::vector<std::function<void(rt::MachineEvent&)>> end_fields;
};

void wire_audio(M4Wiring& w);    // audio_wiring.cpp: M4 session B (AudioOutput, the SPU2, the sink)
void wire_movies(M4Wiring& w);   // movie_wiring.cpp: M4 session C (the sceMpeg HLE, the decoder)
void wire_saves(M4Wiring& w);    // save_wiring.cpp: M4 session D (memory cards, the virtual RTC)
void wire_feedback(M4Wiring& w); // feedback_wiring.cpp: M4 session D (pad rumble)

} // namespace dq8::host
