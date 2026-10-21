// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "game/overlay_residency.h"

#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "dq8/core/mwo3.h"
#include "game/unit_specs.h"
#include "runtime/ee/context.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/ee/guest_stop.h"

#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dq8::game {

namespace {

constexpr unsigned reg_a0 = 4;

std::string sha1_of(const rt::GuestMemory& memory, std::uint32_t begin, std::uint32_t end) {
    std::vector<std::uint8_t> bytes(end - begin);
    memory.read_bytes(begin, bytes);
    return core::to_hex(core::Sha1::of(bytes));
}

std::uint32_t le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// "text_size 0x4AC0, the profile says 0x4AC1" when the two differ and the profile records the value.
void expect_field(std::vector<std::string>& problems, std::string_view field, std::uint32_t header,
                  std::uint32_t profile) {
    if (header != profile) {
        problems.push_back(std::string(field) + " " + rt::hex32(header) + ", the profile says " +
                           rt::hex32(profile));
    }
}

} // namespace

OverlayResidency::OverlayResidency(OverlayResidencyConfig config) : config_(config) {
    if (config_.profile == nullptr || config_.overlays == nullptr) {
        throw std::invalid_argument("OverlayResidency needs a profile and an overlay manager");
    }
    if (config_.profile->overlay_window.loader_hook == 0) {
        throw std::invalid_argument("region profile " + config_.profile->id +
                                    " records no overlay_window.loader_hook");
    }
}

void OverlayResidency::attach(rt::MachineServices& machine) {
    machine_ = &machine;
    if (config_.verify) {
        machine.add_vblank_listener(rt::VblankPhase::intc, *this);
    }
}

void OverlayResidency::reset() {
    loads_ = 0;
    resident_name_.clear();
    text_begin_ = text_end_ = 0;
    text_sha1_.clear();
}

rt::OverrideSpec OverlayResidency::hook_override() {
    rt::OverrideSpec spec;
    spec.unit = std::string(main_unit_name);
    spec.address = config_.profile->overlay_window.loader_hook;
    spec.kind = rt::OverrideKind::replace;
    spec.name = "overlay-residency-hook";
    spec.fn = [this](rt::OverrideCall& call) { on_hook(call); };
    return spec;
}

void OverlayResidency::fail(rt::MachineServices& machine, std::uint32_t pc, std::string message) const {
    rt::StopInfo s;
    s.reason = rt::StopReason::unsupported;
    s.pc = pc;
    s.address = config_.profile->overlay_window.base;
    s.message = "overlay residency hook: " + std::move(message);
    machine.stop(std::move(s));
}

void OverlayResidency::on_hook(rt::OverrideCall& call) {
    rt::MachineServices* m = machine_ != nullptr ? machine_ : call.machine;
    if (m == nullptr) {
        throw std::logic_error("the overlay residency hook ran without a machine");
    }
    rt::MachineServices& machine = *m;
    const core::RegionProfile& profile = *config_.profile;
    const auto& w = profile.overlay_window;
    const std::uint32_t pc = call.ctx->pc;

    const std::uint32_t dest = rt::gpr_u32(*call.ctx, reg_a0);
    if (dest != w.base) {
        fail(machine, pc,
             "the loader placed an overlay at " + rt::hex32(dest) + ", the profile's window is at " +
                 rt::hex32(w.base));
    }
    std::array<std::uint8_t, core::mwo3_header_size> header_bytes{};
    machine.memory().read_bytes(w.base, header_bytes);
    const std::uint32_t id = le32(header_bytes.data() + 4);
    const core::ProfileOverlay* overlay = nullptr;
    for (const core::ProfileOverlay& o : profile.overlays) {
        if (o.id == id) {
            overlay = &o;
        }
    }
    if (overlay == nullptr) {
        fail(machine, pc,
             "the window holds overlay id " + std::to_string(id) + " at " + rt::hex32(w.base + 4) +
                 ", which the profile does not list");
    }

    core::Mwo3Header header;
    try {
        const std::uint64_t file_size = overlay->size != 0 ? overlay->size
                                                           : std::uint64_t{core::mwo3_header_size} +
                                                                 le32(header_bytes.data() + 0x0C) +
                                                                 le32(header_bytes.data() + 0x10);
        header = core::parse_mwo3_header(header_bytes, file_size, core::Mwo3Window{w.base, w.end});
    } catch (const core::Error& e) {
        fail(machine, pc, overlay->name + ": the header in the window does not parse: " + e.what());
    }
    std::vector<std::string> problems;
    if (overlay->text_size != 0 || overlay->data_size != 0 || overlay->bss_size != 0) {
        expect_field(problems, "text_size", header.text_size, overlay->text_size);
        expect_field(problems, "data_size", header.data_size, overlay->data_size);
        expect_field(problems, "bss_size", header.bss_size, overlay->bss_size);
    }
    if (overlay->table_entries != 0) {
        expect_field(problems, "constructor table entries", header.table_entry_count(),
                     overlay->table_entries);
    }
    if (overlay->segment_memsz != 0) {
        expect_field(problems, "memsz", header.memsz(), overlay->segment_memsz);
    }
    if (!problems.empty()) {
        std::string joined;
        for (const std::string& p : problems) {
            joined += (joined.empty() ? "" : "; ") + p;
        }
        fail(machine, pc, overlay->name + " (id " + std::to_string(id) + "): " + joined);
    }
    if (config_.verify && !overlay->sha1.empty()) {
        const std::string actual = sha1_of(machine.memory(), w.base, w.base + header.file_size());
        if (actual != core::normalize_hex(overlay->sha1)) {
            fail(machine, pc,
                 overlay->name + ": the " + std::to_string(header.file_size()) + " loaded bytes hash to " +
                     actual + ", the profile says " + overlay->sha1);
        }
    }
    if (config_.overlays->find(overlay->id) == nullptr) {
        fail(machine, pc,
             overlay->name + " (id " + std::to_string(id) + ") has no code unit registered (module " +
                 overlay_unit_name(*overlay) + ")");
    }

    config_.overlays->set_resident(overlay->id);
    ++loads_;
    if (config_.verify) {
        resident_name_ = overlay->name;
        text_begin_ = header.code_start();
        text_end_ = header.data_start();
        text_sha1_ = sha1_of(machine.memory(), text_begin_, text_end_);
    }
    machine.emit(rt::MachineEvent{"overlay_resident", {}}
                     .add("name", overlay->name)
                     .add("unit", overlay_unit_name(*overlay))
                     .add("overlay_id", std::uint64_t{overlay->id})
                     .add("generation", config_.overlays->generation())
                     .add("load", loads_)
                     .add("verified", config_.verify));
    call.return_to_caller();
}

void OverlayResidency::on_vblank_start(std::uint64_t /*field*/) {
    if (machine_ == nullptr || text_sha1_.empty()) {
        return;
    }
    const std::string actual = sha1_of(machine_->memory(), text_begin_, text_end_);
    if (actual == text_sha1_) {
        return;
    }
    machine_->emit(rt::MachineEvent{"overlay_verify", {}}.add("name", resident_name_).add("ok", false));
    rt::StopInfo s;
    s.reason = rt::StopReason::unsupported;
    s.address = text_begin_;
    s.message = "--verify-overlays: the text of resident overlay " + resident_name_ + " [" +
                rt::hex32(text_begin_) + ", " + rt::hex32(text_end_) + ") changed since it was loaded";
    machine_->stop(std::move(s));
}

} // namespace dq8::game
