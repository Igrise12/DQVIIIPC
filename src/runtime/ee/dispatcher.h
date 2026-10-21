// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/ee/dispatch_table.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/ee/overlay_manager.h"
#include "runtime/ee/overrides.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Address -> recompiled function: the main unit's table first, then, for an address in the overlay window,
// the resident overlay's table; anything else is a miss. The dispatcher keeps an owned copy of the main
// table (the overlay manager keeps the overlays'), so overrides (overrides.h) repoint single slots at the
// trampoline without changing resolve().
namespace dq8::rt {

class GuestMemory;
class MachineServices;

struct DispatchTarget {
    dq8_guest_fn fn = nullptr;
    std::string_view unit; // the owning unit's name, for traces; empty on a miss
};

class Dispatcher {
  public:
    explicit Dispatcher(const OverlayManager& overlays) : overlays_(overlays) {}
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    // Keeps `table` (a view of the module's slots) and takes an owned copy when the first main-unit override
    // is installed. Call it before installing overrides (std::logic_error after).
    void set_main(std::string name, DispatchTable table);
    const DispatchTable& main_table() const noexcept { return main_; }
    const std::string& main_name() const noexcept { return main_name_; }
    const OverlayManager& overlays() const noexcept { return overlays_; }

    DispatchTarget resolve(std::uint32_t addr) const noexcept {
        if (const dq8_guest_fn fn = main_.lookup(addr)) {
            return {fn, main_name_};
        }
        if (overlays_.in_window(addr)) {
            if (const OverlayUnit* res = overlays_.resident()) {
                if (const dq8_guest_fn fn = res->table.lookup(addr)) {
                    return {fn, res->name};
                }
            }
        }
        return {};
    }
    dq8_guest_fn lookup(std::uint32_t addr) const noexcept { return resolve(addr).fn; }

    // One sentence on why `addr` has no function: misaligned, an empty slot of the main table, an overlay
    // window miss (OverlayManager::describe_miss), or outside every table.
    std::string describe_miss(std::uint32_t addr) const;

    // --- Overrides --------------------------------------------------------------------------------------
    // The machine overrides receive (OverrideCall::machine) and that the trampoline stops through. Without
    // one a stop throws GuestStop, which is what unit tests catch.
    void set_machine(MachineServices* machine) noexcept { machine_ = machine; }
    MachineServices* machine() const noexcept { return machine_; }

    // Points the slot of `spec.unit` at `spec.address` at the trampoline. Throws std::invalid_argument when
    // the unit is unknown (units are registered first: set_main, OverlayManager::register_unit), the address
    // is not a function entry of it, another override is installed there, `fn` is empty, or a main-unit
    // digest does not match the bytes in `memory` (required when the spec has a digest). Only one dispatcher
    // per process can hold overrides (the trampoline is a plain function pointer); a second one throws
    // std::logic_error until the first is destroyed.
    void install_override(OverrideSpec spec, const GuestMemory* memory = nullptr);
    std::vector<OverrideReport> overrides() const;

  private:
    struct Installed {
        OverrideSpec spec;
        std::uint32_t overlay_id = 0; // 0 for the main unit
        dq8_guest_fn original = nullptr;
        std::uint64_t calls = 0;
        std::uint64_t verified_generation = ~std::uint64_t{0}; // overlay digests: last checked residency
    };
    static void trampoline(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
    void run_override(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
    [[noreturn]] void stop(StopInfo info) const;
    static std::uint64_t key(std::uint32_t overlay_id, std::uint32_t address) noexcept {
        return (std::uint64_t{overlay_id} << 32) | address;
    }

    const OverlayManager& overlays_;
    DispatchTable main_;
    std::string main_name_;
    MachineServices* machine_ = nullptr;
    std::map<std::uint64_t, Installed> overrides_;
};

} // namespace dq8::rt
