// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/ps2recomp/r5900_context.h"
#include "dq8/core/intake.h"
#include "host/json_writer.h"
#include "host/options.h"
#include "runtime/ee/dispatcher.h"
#include "runtime/ee/elf_loader.h"
#include "runtime/ee/game_module.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/ee/overlay_manager.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Start-up shared by every run of dq8-game: verify the disc (or take a developer ELF), load and validate the
// unit modules, place the boot ELF in guest memory, register the tables with the dispatcher and the overlay
// manager. Also the parts of the --json report and the text output that describe the setup and a stop.
namespace dq8::host {

// Every problem found before the guest could run (module validation, mostly), reported together.
class SetupProblems : public std::runtime_error {
  public:
    SetupProblems(std::string summary, std::vector<std::string> problems)
        : std::runtime_error(std::move(summary)), problems_(std::move(problems)) {}
    const std::vector<std::string>& problems() const noexcept { return problems_; }

  private:
    std::vector<std::string> problems_;
};

// Everything one run needs. Members are destroyed in reverse order, so the modules (units) outlive the
// runtime objects that point into their tables.
struct Session {
    std::string mode; // "iso" or "elf"
    std::optional<core::VerifiedImage> verified;
    fs::path elf_path;
    std::vector<rt::LoadedUnit> units;
    rt::LoadedElf elf;
    rt::GuestMemory memory;
    rt::OverlayManager overlays;
    std::unique_ptr<rt::Dispatcher> dispatcher;

    const core::RegionProfile* profile() const noexcept { return verified ? &verified->profile : nullptr; }
};

// The directory of the running executable (empty when unknown).
fs::path executable_dir(const char* argv0);

// --iso or --elf set-up. `argv0` locates the default region profile directory.
void setup_session(const Options& o, const char* argv0, Session& s);

// Kernel-call stops (a syscall, or a by-name call of the syscall kind).
bool stopped_at_syscall(const rt::StopInfo& stop);

std::string hex64(std::uint64_t v);

// --json members.
void json_setup(JsonWriter& j, const Session& s);
void json_stop(JsonWriter& j, const rt::StopInfo& stop, std::string_view reason_override = {});
void json_context(JsonWriter& j, const R5900Context& ctx);
void json_mmio(JsonWriter& j, const rt::GuestMemory& memory);

// Text output.
void print_setup(const Session& s);
void print_stop(const rt::StopInfo& stop, std::uint64_t dispatches, const R5900Context& ctx,
                const rt::GuestMemory& memory);
std::string mmio_access_name(const rt::MmioAccess& a);

// The failure report: {"ok": false, "error": {code, message}, "problems": [...], "exit_code"} with --json,
// otherwise lines on stderr. Returns `exit_code`.
int report_failure(bool json, std::string_view code, const std::string& message,
                   const std::vector<std::string>& problems = {}, int exit_code = exit_error);

} // namespace dq8::host
