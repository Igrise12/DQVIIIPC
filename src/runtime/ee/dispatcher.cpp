// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/dispatcher.h"

#include "dq8/core/hash.h"
#include "runtime/ee/context.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/machine/machine_services.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace dq8::rt {

namespace {

constexpr unsigned reg_ra = 31;

// The dispatcher whose tables point at the trampoline. Generated code calls the trampoline through a plain
// function pointer with no context of its own, so there is one per process (one machine per process).
std::atomic<Dispatcher*> override_owner{nullptr};

std::string sha1_of_guest(const GuestMemory& memory, std::uint32_t address, std::uint32_t bytes) {
    std::vector<std::uint8_t> code(bytes);
    memory.read_bytes(address, code);
    const core::Sha1::Digest digest = core::Sha1::of(code);
    return core::to_hex(digest);
}

bool is_sha1_hex(std::string_view text) noexcept {
    if (text.size() != 40) {
        return false;
    }
    for (const char c : text) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

std::string label(const OverrideSpec& spec) {
    return "override '" + spec.name + "' (" + spec.unit + " " + hex32(spec.address) + ")";
}

} // namespace

Dispatcher::~Dispatcher() {
    Dispatcher* self = this;
    override_owner.compare_exchange_strong(self, nullptr);
}

void Dispatcher::set_main(std::string name, DispatchTable table) {
    if (!overrides_.empty()) {
        throw std::logic_error("Dispatcher::set_main after overrides were installed would drop them");
    }
    main_name_ = std::move(name);
    // A view until the first override needs to repoint a slot (install_override takes the owned copy then),
    // so a caller that fills its table after set_main still sees its functions, as before overrides existed.
    main_ = std::move(table);
}

std::string Dispatcher::describe_miss(std::uint32_t addr) const {
    const std::string at = "no recompiled function at " + hex32(addr) + ": ";
    if ((addr & 3u) != 0) {
        return at + "the address is not 4-aligned";
    }
    if (main_.covers(addr)) {
        return at + "the " + main_name_ + " unit's table [" + hex32(main_.base()) + ", " +
               hex32(main_.end()) +
               ") has no entry there (not a function start in the map, nor a resume point)";
    }
    if (overlays_.in_window(addr)) {
        return at + overlays_.describe_miss(addr);
    }
    std::string out = at + "outside every dispatch table";
    if (!main_.empty()) {
        out += " (" + main_name_ + " [" + hex32(main_.base()) + ", " + hex32(main_.end()) + ")";
        if (const auto& w = overlays_.window()) {
            out += ", overlay window [" + hex32(w->base) + ", " + hex32(w->end) + ")";
        }
        out += ")";
    }
    return out;
}

void Dispatcher::install_override(OverrideSpec spec, const GuestMemory* memory) {
    const std::string what = label(spec);
    if (!spec.fn) {
        throw std::invalid_argument(what + ": no function");
    }
    const DispatchTable* table = nullptr;
    std::uint32_t overlay_id = 0;
    if (!main_.empty() && spec.unit == main_name_) {
        if (!main_.owned()) {
            main_ = main_.owned_copy();
        }
        table = &main_;
    } else {
        for (const OverlayUnit& u : overlays_.units()) {
            if (u.name == spec.unit) {
                table = &u.table;
                overlay_id = u.overlay_id;
            }
        }
    }
    if (table == nullptr) {
        throw std::invalid_argument(what + ": no unit named '" + spec.unit + "' is registered");
    }
    if (overrides_.count(key(overlay_id, spec.address)) != 0) {
        throw std::invalid_argument(what + ": another override is installed there (" +
                                    overrides_.at(key(overlay_id, spec.address)).spec.name + ")");
    }
    if (!table->is_function_entry(spec.address)) {
        throw std::invalid_argument(what + ": not a function entry of the unit (" +
                                    (table->lookup(spec.address) != nullptr
                                         ? std::string("a resume point inside a function")
                                         : "no function starts there") +
                                    ")");
    }
    if (!spec.code_sha1.empty()) {
        if (!is_sha1_hex(spec.code_sha1) || spec.code_bytes == 0) {
            throw std::invalid_argument(what + ": a digest needs 40 hex digits and a non-zero byte count");
        }
        spec.code_sha1 = core::normalize_hex(spec.code_sha1);
        if (overlay_id == 0) {
            if (memory == nullptr) {
                throw std::invalid_argument(what + ": the digest needs guest memory to check against");
            }
            const std::string actual = sha1_of_guest(*memory, spec.address, spec.code_bytes);
            if (actual != spec.code_sha1) {
                throw std::invalid_argument(what + ": the " + std::to_string(spec.code_bytes) +
                                            " guest bytes there hash to " + actual + ", expected " +
                                            spec.code_sha1);
            }
        }
    }
    Dispatcher* expected = nullptr;
    if (!override_owner.compare_exchange_strong(expected, this) && expected != this) {
        throw std::logic_error(what + ": another dispatcher in this process holds overrides");
    }
    Installed installed;
    installed.overlay_id = overlay_id;
    installed.original = table->replace_slot(spec.address, &Dispatcher::trampoline);
    installed.spec = std::move(spec);
    overrides_.emplace(key(overlay_id, installed.spec.address), std::move(installed));
}

std::vector<OverrideReport> Dispatcher::overrides() const {
    std::vector<OverrideReport> out;
    for (const auto& [k, o] : overrides_) {
        out.push_back({o.spec.unit, o.spec.address, o.spec.kind, o.spec.name, o.calls});
    }
    return out;
}

void Dispatcher::stop(StopInfo info) const {
    if (machine_ != nullptr) {
        machine_->stop(std::move(info));
    }
    throw GuestStop(std::move(info));
}

void Dispatcher::trampoline(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    Dispatcher* self = override_owner.load(std::memory_order_acquire);
    if (self == nullptr) {
        // Only an installed override's slot points here, and the owner outlives its tables' use.
        std::fprintf(stderr, "dq8: override trampoline entered at %s with no dispatcher\n",
                     hex32(ctx->pc).c_str());
        std::abort();
    }
    self->run_override(rdram, ctx, runtime);
}

void Dispatcher::run_override(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const std::uint32_t pc = ctx->pc;
    std::uint32_t overlay_id = 0;
    if (!main_.covers(pc)) {
        const OverlayUnit* res = overlays_.in_window(pc) ? overlays_.resident() : nullptr;
        overlay_id = res != nullptr ? res->overlay_id : ~0u;
    }
    const auto it = overrides_.find(key(overlay_id, pc));
    if (it == overrides_.end()) {
        StopInfo s;
        s.reason = StopReason::guest_error;
        s.pc = pc;
        s.address = pc;
        s.message = "the override trampoline was entered at " + hex32(pc) + ", which has no override";
        stop(std::move(s));
    }
    Installed& o = it->second;
    if (o.overlay_id != 0 && !o.spec.code_sha1.empty() && o.verified_generation != overlays_.generation()) {
        const std::string actual =
            machine_ != nullptr ? sha1_of_guest(machine_->memory(), pc, o.spec.code_bytes) : std::string();
        if (actual != o.spec.code_sha1) {
            StopInfo s;
            s.reason = StopReason::guest_error;
            s.pc = pc;
            s.address = pc;
            s.message = label(o.spec) + ": the resident bytes hash to " +
                        (actual.empty() ? std::string("<no machine to read them>") : actual) + ", expected " +
                        o.spec.code_sha1;
            stop(std::move(s));
        }
        o.verified_generation = overlays_.generation();
    }
    ++o.calls;
    OverrideCall call{rdram, ctx, runtime, machine_, o.original, pc, guest_, 0};
    switch (o.spec.kind) {
    case OverrideKind::replace: {
        [[maybe_unused]] const std::uint32_t ra = gpr_u32(*ctx, reg_ra);
        o.spec.fn(call);
#ifndef NDEBUG
        if (ctx->pc != ra) {
            StopInfo s;
            s.reason = StopReason::guest_error;
            s.pc = ctx->pc;
            s.address = pc;
            s.message = label(o.spec) + " left pc at " + hex32(ctx->pc) + " instead of returning to " +
                        hex32(ra) + " (OverrideCall::return_to_caller)";
            stop(std::move(s));
        }
#endif
        break;
    }
    case OverrideKind::hook_before:
        o.spec.fn(call);
        ctx->pc = pc; // the guest function starts from its entry: its switch (ctx->pc) takes the top
        o.original(rdram, ctx, runtime);
        break;
    case OverrideKind::hook_after:
        o.original(rdram, ctx, runtime);
        o.spec.fn(call);
        break;
    }
}

} // namespace dq8::rt
