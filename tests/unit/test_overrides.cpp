// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/hash.h"
#include "runtime/ee/context.h"
#include "runtime/ee/dispatch_table.h"
#include "runtime/ee/dispatcher.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/ee/overlay_manager.h"
#include "runtime/ee/overrides.h"
#include "support/fake_machine.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace dq8::rt;

// Synthetic layout, ours: a main table at 0x00100000 and a window at 0x00300000.
constexpr std::uint32_t main_base = 0x00100000u;
constexpr OverlayWindowSpec window{0x00300000u, 0x00300400u};
constexpr std::uint32_t caller_ra = 0x00100200u;

// What the guest functions did, in order.
std::vector<std::string>& journal() {
    static std::vector<std::string> j;
    return j;
}

// A guest function returns by setting pc to $ra, as `jr ra` in generated code does.
void guest_return(R5900Context* ctx) {
    ctx->pc = gpr_u32(*ctx, 31);
}
void fn_entry(std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
    journal().push_back("original@" + hex32(ctx->pc));
    guest_return(ctx);
}
void fn_other(std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
    journal().push_back("other");
    guest_return(ctx);
}
void fn_overlay(std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
    journal().push_back("overlay");
    guest_return(ctx);
}

struct Slots {
    std::uint32_t base;
    std::vector<dq8_guest_fn> slots;
    Slots(std::uint32_t b, std::size_t count) : base(b), slots(count, nullptr) {}
    void set(std::uint32_t addr, dq8_guest_fn fn) { slots.at((addr - base) / 4u) = fn; }
    DispatchTable table() const {
        return DispatchTable(base, base + static_cast<std::uint32_t>(slots.size()) * 4u, slots);
    }
};

// A main unit with a function at +0x10 (resume point at +0x18) and another at +0x40, and one overlay unit
// "ovl" (id 3) with a function at window + 0x40.
struct World {
    Slots main{main_base, 64};
    Slots ovl{window.base, 64};
    OverlayManager overlays{window};
    std::unique_ptr<Dispatcher> dispatcher;
    alignas(16) R5900Context ctx{};

    World() {
        journal().clear();
        main.set(main_base + 0x10, fn_entry);
        main.set(main_base + 0x18, fn_entry); // resume entry of the same function
        main.set(main_base + 0x40, fn_other);
        ovl.set(window.base + 0x40, fn_overlay);
        overlays.register_unit(3, "ovl", ovl.table());
        dispatcher = std::make_unique<Dispatcher>(overlays);
        dispatcher->set_main("main", main.table());
        reset_context(ctx, main_base);
    }
    // Calls `addr` the way dispatch does: pc = target, then the slot's function.
    void call(std::uint32_t addr) {
        set_gpr_lo(ctx, 31, caller_ra);
        ctx.pc = addr;
        const dq8_guest_fn fn = dispatcher->lookup(addr);
        REQUIRE(fn != nullptr);
        fn(nullptr, &ctx, nullptr);
    }
};

OverrideSpec replace_at(std::uint32_t addr, std::string name = "test-replace") {
    OverrideSpec s;
    s.unit = "main";
    s.address = addr;
    s.kind = OverrideKind::replace;
    s.name = std::move(name);
    s.fn = [](OverrideCall& c) {
        journal().push_back("replace@" + hex32(c.address));
        c.return_to_caller();
    };
    return s;
}

} // namespace

TEST_CASE("overrides: a replacement takes the slot without touching the module's table", "[overrides]") {
    World w;
    const dq8_guest_fn before = w.dispatcher->lookup(main_base + 0x10);
    CHECK(before == &fn_entry);
    w.dispatcher->install_override(replace_at(main_base + 0x10));
    CHECK(w.dispatcher->lookup(main_base + 0x10) != &fn_entry);
    CHECK(w.main.slots[4] == &fn_entry); // the module's array is untouched: the dispatcher owns a copy
    CHECK(w.dispatcher->lookup(main_base + 0x18) == &fn_entry); // the resume entry still goes to the guest
    CHECK(w.dispatcher->resolve(main_base + 0x10).unit == "main");

    w.call(main_base + 0x10);
    CHECK(w.ctx.pc == caller_ra);
    w.call(main_base + 0x40);
    CHECK(journal() == std::vector<std::string>{"replace@0x00100010", "other"});
    const auto report = w.dispatcher->overrides();
    REQUIRE(report.size() == 1);
    CHECK(report[0].name == "test-replace");
    CHECK(report[0].calls == 1);
    CHECK(to_string(report[0].kind) == "replace");
}

TEST_CASE("overrides: hooks run before or after the guest function", "[overrides]") {
    World w;
    OverrideSpec before;
    before.unit = "main";
    before.address = main_base + 0x10;
    before.kind = OverrideKind::hook_before;
    before.name = "before";
    before.fn = [](OverrideCall& c) {
        journal().push_back("before");
        c.ctx->pc = 0xDEAD0000u; // ignored: the guest function still starts from its entry
    };
    w.dispatcher->install_override(std::move(before));
    OverrideSpec after;
    after.unit = "main";
    after.address = main_base + 0x40;
    after.kind = OverrideKind::hook_after;
    after.name = "after";
    after.fn = [](OverrideCall& c) {
        CHECK(c.original == &fn_other);
        journal().push_back("after pc=" + hex32(c.ctx->pc));
    };
    w.dispatcher->install_override(std::move(after));

    w.call(main_base + 0x10);
    w.call(main_base + 0x40);
    CHECK(journal() ==
          std::vector<std::string>{"before", "original@0x00100010", "other", "after pc=" + hex32(caller_ra)});
}

TEST_CASE("overrides: installation checks the unit, the entry and duplicates", "[overrides]") {
    World w;
    OverrideSpec bad = replace_at(main_base + 0x18);
    CHECK_THROWS_AS(w.dispatcher->install_override(bad), std::invalid_argument); // resume point
    bad.address = main_base + 0x14;
    CHECK_THROWS_AS(w.dispatcher->install_override(bad), std::invalid_argument); // empty slot
    bad.address = main_base + 0x12;
    CHECK_THROWS_AS(w.dispatcher->install_override(bad), std::invalid_argument); // misaligned
    bad.address = main_base + 0x10;
    bad.unit = "nosuch";
    CHECK_THROWS_AS(w.dispatcher->install_override(bad), std::invalid_argument);
    bad.unit = "main";
    bad.fn = nullptr;
    CHECK_THROWS_AS(w.dispatcher->install_override(bad), std::invalid_argument);

    w.dispatcher->install_override(replace_at(main_base + 0x10, "first"));
    CHECK_THROWS_AS(w.dispatcher->install_override(replace_at(main_base + 0x10, "second")),
                    std::invalid_argument);
    CHECK_THROWS_AS(w.dispatcher->set_main("main", w.main.table()), std::logic_error);
}

TEST_CASE("dispatch table: owned copies and function entries", "[overrides]") {
    Slots s(main_base, 8);
    s.set(main_base + 0x04, fn_entry);
    s.set(main_base + 0x0C, fn_entry);
    s.set(main_base + 0x10, fn_other);
    const DispatchTable view = s.table();
    const DispatchTable copy = view.owned_copy();
    CHECK_FALSE(view.owned());
    CHECK(copy.owned());
    CHECK(copy.lookup(main_base + 0x0C) == &fn_entry);
    CHECK(copy.entry_count() == 3);
    CHECK(copy.is_function_entry(main_base + 0x04));
    CHECK_FALSE(copy.is_function_entry(main_base + 0x0C)); // a resume entry of the function at +0x04
    CHECK(copy.is_function_entry(main_base + 0x10));
    CHECK_FALSE(copy.is_function_entry(main_base + 0x08));
    CHECK(DispatchTable().owned_copy().empty());
}

TEST_CASE("overrides: a digest is checked against guest memory", "[overrides]") {
    World w;
    GuestMemory memory;
    const std::vector<std::uint8_t> code{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    memory.write_bytes(main_base + 0x10, code);
    const std::string good = dq8::core::to_hex(dq8::core::Sha1::of(code));

    OverrideSpec s = replace_at(main_base + 0x10);
    s.code_bytes = 8;
    s.code_sha1 = std::string(40, '0');
    CHECK_THROWS_AS(w.dispatcher->install_override(s, &memory), std::invalid_argument);
    s.code_sha1 = good;
    CHECK_THROWS_AS(w.dispatcher->install_override(s), std::invalid_argument); // no memory to check
    s.code_sha1 = "not hex";
    CHECK_THROWS_AS(w.dispatcher->install_override(s, &memory), std::invalid_argument);
    s.code_sha1 = good;
    for (char& c : s.code_sha1) {
        c = static_cast<char>(c >= 'a' && c <= 'f' ? c - 'a' + 'A' : c); // upper case is accepted
    }
    CHECK_NOTHROW(w.dispatcher->install_override(s, &memory));
}

TEST_CASE("overrides: an overlay override fires only while its unit is resident", "[overrides]") {
    World w;
    dq8::test::FakeMachine machine;
    w.dispatcher->set_machine(&machine);
    const std::vector<std::uint8_t> code{0xAA, 0xBB, 0xCC, 0xDD};
    const std::string digest = dq8::core::to_hex(dq8::core::Sha1::of(code));

    OverrideSpec s;
    s.unit = "ovl";
    s.address = window.base + 0x40;
    s.kind = OverrideKind::hook_before;
    s.name = "ovl-hook";
    s.code_bytes = 4;
    s.code_sha1 = digest;
    s.fn = [](OverrideCall& c) {
        CHECK(c.machine != nullptr);
        journal().push_back("ovl-hook");
    };
    w.dispatcher->install_override(std::move(s)); // no memory needed: checked when resident
    CHECK(w.dispatcher->lookup(window.base + 0x40) == nullptr);

    w.overlays.set_resident(3);
    machine.memory().write_bytes(window.base + 0x40, code);
    w.call(window.base + 0x40);
    CHECK(journal() == std::vector<std::string>{"ovl-hook", "overlay"});

    // A new residency of other bytes fails the digest on its first call.
    w.overlays.set_resident(3);
    machine.memory().fill(window.base + 0x40, 4, 0);
    CHECK_THROWS_AS(w.call(window.base + 0x40), GuestStop);
}

TEST_CASE("overrides: one dispatcher per process holds overrides", "[overrides]") {
    auto a = std::make_unique<World>();
    a->dispatcher->install_override(replace_at(main_base + 0x10));
    {
        World b;
        CHECK_THROWS_AS(b.dispatcher->install_override(replace_at(main_base + 0x40)), std::logic_error);
    }
    a.reset();
    World c;
    CHECK_NOTHROW(c.dispatcher->install_override(replace_at(main_base + 0x40)));
}

#ifndef NDEBUG
TEST_CASE("overrides: a replacement that does not return stops in debug builds", "[overrides]") {
    World w;
    OverrideSpec s = replace_at(main_base + 0x10);
    s.fn = [](OverrideCall&) {};
    w.dispatcher->install_override(std::move(s));
    try {
        w.call(main_base + 0x10);
        FAIL("expected a stop");
    } catch (const GuestStop& stop) {
        CHECK(stop.info().message.find("instead of returning to 0x00100200") != std::string::npos);
    }
}
#endif
