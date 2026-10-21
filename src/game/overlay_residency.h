// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/region_profile.h"
#include "runtime/ee/overlay_manager.h"
#include "runtime/ee/overrides.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <string>
#include <string_view>

// Overlay residency through the loader hook (ADR-0012, architecture §4.3). The game's own loader reads an
// overlay file into the window over the native file-I/O service and then calls the empty function
// `overlay_window.loader_hook` with the destination, before it flushes the caches and runs the constructor
// table. An override on that function parses the MWo3 header now in the window, cross-checks it against the
// profile (the id at window + 4, the declared sizes, the destination), makes the overlay resident in the
// OverlayManager, emits `overlay_resident` and returns to the loader. Any mismatch stops the guest.
//
// With `verify`, the bytes are also checked against the profile's container hash at the hook, and the
// resident overlay's text range is re-hashed at every vblank (a change means the guest wrote over code the
// dispatcher is running natively).
namespace dq8::game {

struct OverlayResidencyConfig {
    const core::RegionProfile* profile = nullptr; // required; must outlive the object
    rt::OverlayManager* overlays = nullptr;       // required; units registered under their profile ids
    bool verify = false;                          // --verify-overlays
};

class OverlayResidency final : public rt::IDevice, private rt::IVblankListener {
  public:
    explicit OverlayResidency(OverlayResidencyConfig config);

    std::string_view name() const override { return "game.overlay_residency"; }
    void attach(rt::MachineServices& machine) override;
    void reset() override;

    // The override to install in the dispatcher (unit "main", address loader_hook, replace).
    rt::OverrideSpec hook_override();

    // What the hook did so far.
    std::uint64_t loads() const noexcept { return loads_; }

  private:
    void on_hook(rt::OverrideCall& call);
    void on_vblank_start(std::uint64_t field) override;
    [[noreturn]] void fail(rt::MachineServices& machine, std::uint32_t pc, std::string message) const;

    OverlayResidencyConfig config_;
    rt::MachineServices* machine_ = nullptr;
    std::uint64_t loads_ = 0;
    // --verify-overlays: the resident text range and its hash at load time.
    std::string resident_name_;
    std::uint32_t text_begin_ = 0;
    std::uint32_t text_end_ = 0;
    std::string text_sha1_;
};

} // namespace dq8::game
