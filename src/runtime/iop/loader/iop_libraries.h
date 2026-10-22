// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The libraries interpreted modules link against (Docs/milestones/M4-playable.md, session A): the HLE
// libraries first (intrman, loadcore, sysmem, ...: every function is an HLE slot), then the export tables
// modules register at start (RegisterLibraryEntries). A guest library may not take the name of an HLE one.
// Names are compared byte for byte, as the IOP loader compares its 8-byte fields; a guest import also needs
// the export's major version (the high byte).
namespace dq8::rt {

class IopMemory;

struct IopLinkTarget {
    enum class Kind : std::uint8_t {
        hle,             // an HLE slot (`slot`)
        guest,           // a guest function (`address`)
        unimplemented,   // the library exists but this ordinal has no implementation or no export
        missing_library, // nobody provides the library (with this major version)
    };
    Kind kind = Kind::missing_library;
    std::uint32_t slot = 0;
    std::uint32_t address = 0;
};

class IopLibraries {
  public:
    struct GuestLibrary {
        std::uint32_t table = 0; // IOP address of the export table
        std::string name;
        std::uint16_t version = 0;
        std::string module; // the module that registered it
        std::vector<std::uint32_t> functions;
    };

    // An HLE library: implemented ordinal -> HLE slot.
    void add_hle(std::string name, std::map<std::uint16_t, std::uint32_t> slots);
    bool is_hle(std::string_view name) const;

    // RegisterLibraryEntries: 0, or a negative IOP error (-212 library found, -214 illegal library).
    std::int32_t register_guest(const IopMemory& ram, std::uint32_t table, std::string module);
    // ReleaseLibraryEntries: 0 or -213 (not found).
    std::int32_t release_guest(std::uint32_t table);
    // Drops every guest library (an IOP reboot); the HLE ones stay.
    void clear_guest() { guest_.clear(); }

    IopLinkTarget resolve(std::string_view library, std::uint16_t version, std::uint16_t ordinal) const;
    const std::map<std::uint32_t, GuestLibrary>& guest() const noexcept { return guest_; }

  private:
    std::map<std::string, std::map<std::uint16_t, std::uint32_t>, std::less<>> hle_;
    std::map<std::uint32_t, GuestLibrary> guest_; // by table address
};

// IOP kernel error codes the loader and the HLE libraries return.
namespace iop_error {
inline constexpr std::int32_t ok = 0;
inline constexpr std::int32_t error = -1;
inline constexpr std::int32_t illegal_context = -100;
inline constexpr std::int32_t illegal_intrcode = -101;
inline constexpr std::int32_t cpudi = -102;
inline constexpr std::int32_t intr_disable = -103;
inline constexpr std::int32_t found_handler = -104;
inline constexpr std::int32_t notfound_handler = -105;
inline constexpr std::int32_t no_timer = -150;
inline constexpr std::int32_t illegal_timerid = -151;
inline constexpr std::int32_t illegal_source = -152;
inline constexpr std::int32_t illegal_prescale = -153;
inline constexpr std::int32_t timer_busy = -154;
inline constexpr std::int32_t timer_not_setup = -155;
inline constexpr std::int32_t timer_not_inuse = -156;
inline constexpr std::int32_t library_found = -212;
inline constexpr std::int32_t library_notfound = -213;
inline constexpr std::int32_t illegal_library = -214;
inline constexpr std::int32_t no_memory = -400;
inline constexpr std::int32_t illegal_attr = -401;
inline constexpr std::int32_t illegal_entry = -402;
inline constexpr std::int32_t illegal_priority = -403;
inline constexpr std::int32_t illegal_stacksize = -404;
inline constexpr std::int32_t illegal_mode = -405;
inline constexpr std::int32_t illegal_thid = -406;
inline constexpr std::int32_t unknown_thid = -407;
inline constexpr std::int32_t unknown_semid = -408;
inline constexpr std::int32_t unknown_evfid = -409;
inline constexpr std::int32_t dormant = -413;
inline constexpr std::int32_t not_dormant = -414;
inline constexpr std::int32_t can_not_wait = -417;
inline constexpr std::int32_t release_wait = -418;
inline constexpr std::int32_t sema_zero = -419;
inline constexpr std::int32_t sema_ovf = -420;
inline constexpr std::int32_t evf_cond = -421;
inline constexpr std::int32_t evf_multi = -422;
inline constexpr std::int32_t evf_ilpat = -423;
inline constexpr std::int32_t wait_delete = -425;
inline constexpr std::int32_t illegal_memblock = -426;
} // namespace iop_error

} // namespace dq8::rt
