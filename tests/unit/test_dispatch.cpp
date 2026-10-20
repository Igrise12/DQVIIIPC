// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/dispatch_table.h"
#include "runtime/ee/dispatcher.h"
#include "runtime/ee/overlay_manager.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace dq8::rt;

// Distinct do-nothing guest functions; only their addresses matter here.
void fn_a(std::uint8_t*, R5900Context*, PS2Runtime*) {}
void fn_b(std::uint8_t*, R5900Context*, PS2Runtime*) {}
void fn_c(std::uint8_t*, R5900Context*, PS2Runtime*) {}

// Synthetic layout: a "main" table at 0x00100000 and a window at 0x00300000.
constexpr std::uint32_t main_base = 0x00100000u;
constexpr OverlayWindowSpec window{0x00300000u, 0x00300400u};

struct Slots {
    std::uint32_t base;
    std::vector<dq8_guest_fn> slots;
    Slots(std::uint32_t b, std::size_t count) : base(b), slots(count, nullptr) {}
    void set(std::uint32_t addr, dq8_guest_fn fn) { slots.at((addr - base) / 4u) = fn; }
    DispatchTable table() const {
        return DispatchTable(base, base + static_cast<std::uint32_t>(slots.size()) * 4u, slots);
    }
};

} // namespace

TEST_CASE("dispatch table: lookup by slot, alignment and range", "[dispatch]") {
    Slots s(main_base, 8);
    s.set(main_base, fn_a);
    s.set(main_base + 0x10, fn_b);
    const DispatchTable t = s.table();
    CHECK(t.base() == main_base);
    CHECK(t.end() == main_base + 0x20);
    CHECK(t.slot_count() == 8);
    CHECK(t.entry_count() == 2);
    CHECK(t.lookup(main_base) == &fn_a);
    CHECK(t.lookup(main_base + 0x10) == &fn_b);
    CHECK(t.lookup(main_base + 0x04) == nullptr); // empty slot
    CHECK(t.lookup(main_base + 0x11) == nullptr); // misaligned
    CHECK(t.lookup(main_base - 4) == nullptr);
    CHECK(t.lookup(main_base + 0x20) == nullptr); // end is exclusive
    CHECK(DispatchTable().lookup(0) == nullptr);
}

TEST_CASE("dispatch table: the constructor refuses inconsistent shapes", "[dispatch]") {
    std::vector<dq8_guest_fn> slots(4, nullptr);
    CHECK_THROWS_AS(DispatchTable(main_base, main_base + 0x20, slots), std::invalid_argument);
    CHECK_THROWS_AS(DispatchTable(main_base + 2, main_base + 0x12, slots), std::invalid_argument);
    CHECK_THROWS_AS(DispatchTable(main_base, main_base, std::span<const dq8_guest_fn>()),
                    std::invalid_argument);
    CHECK_NOTHROW(DispatchTable(main_base, main_base + 0x10, slots));
}

TEST_CASE("overlay manager: registration, residency and the generation counter", "[dispatch]") {
    OverlayManager m(window);
    Slots one(window.base, 32);
    one.set(window.base + 0x40, fn_a);
    Slots two(window.base, 32);
    two.set(window.base + 0x40, fn_b);
    two.set(window.base + 0x60, fn_c);

    m.register_unit(1, "one", one.table());
    m.register_unit(2, "two", two.table());
    CHECK(m.units().size() == 2);
    CHECK(m.resident() == nullptr);
    CHECK(m.lookup(window.base + 0x40) == nullptr); // nothing resident
    CHECK(m.generation() == 0);

    m.set_resident(1);
    CHECK(m.resident()->name == "one");
    CHECK(m.lookup(window.base + 0x40) == &fn_a);
    CHECK(m.generation() == 1);
    m.set_resident(2);
    CHECK(m.lookup(window.base + 0x40) == &fn_b);
    CHECK(m.lookup(window.base + 0x60) == &fn_c);
    CHECK(m.generation() == 2);
    m.clear_resident();
    CHECK(m.resident() == nullptr);
    CHECK(m.generation() == 3);

    CHECK_THROWS_AS(m.set_resident(7), std::invalid_argument);
    CHECK_THROWS_AS(m.register_unit(1, "dup", one.table()), std::invalid_argument);
    CHECK_THROWS_AS(m.register_unit(0, "zero", one.table()), std::invalid_argument);
    Slots outside(window.end, 4);
    CHECK_THROWS_AS(m.register_unit(3, "outside", outside.table()), std::invalid_argument);
    CHECK_THROWS_AS(OverlayManager().register_unit(1, "nowindow", one.table()), std::invalid_argument);
    CHECK_THROWS_AS(OverlayManager(OverlayWindowSpec{0x100, 0x100}), std::invalid_argument);
}

TEST_CASE("overlay manager: describe_miss says what is resident and who has the address", "[dispatch]") {
    OverlayManager m(window);
    Slots one(window.base, 16);
    Slots two(window.base, 32);
    two.set(window.base + 0x40, fn_b);
    m.register_unit(1, "one", one.table());
    m.register_unit(2, "two", two.table());

    const std::string none = m.describe_miss(window.base + 0x40);
    CHECK(none.find("no overlay is resident") != std::string::npos);
    CHECK(none.find("two (id 2)") != std::string::npos);
    m.set_resident(1);
    const std::string wrong = m.describe_miss(window.base + 0x40);
    CHECK(wrong.find("resident overlay one (id 1) has no function there") != std::string::npos);
    CHECK(wrong.find("it is an entry of two (id 2)") != std::string::npos);
    CHECK(m.describe_miss(window.base + 0x44).find("no registered overlay") != std::string::npos);
    CHECK(m.describe_miss(window.end).find("outside the overlay window") != std::string::npos);
}

TEST_CASE("dispatcher: main table first, then the resident overlay, else a miss", "[dispatch]") {
    OverlayManager m(window);
    Slots main(main_base, 16);
    main.set(main_base + 0x08, fn_a);
    Slots ovl(window.base, 16);
    ovl.set(window.base + 0x08, fn_b);
    m.register_unit(5, "ovl", ovl.table());

    Dispatcher d(m);
    d.set_main("main", main.table());
    CHECK(d.lookup(main_base + 0x08) == &fn_a);
    CHECK(d.resolve(main_base + 0x08).unit == "main");
    CHECK(d.lookup(window.base + 0x08) == nullptr);
    m.set_resident(5);
    CHECK(d.lookup(window.base + 0x08) == &fn_b);
    CHECK(d.resolve(window.base + 0x08).unit == "ovl");
    CHECK(d.resolve(0x00500000u).fn == nullptr);
    CHECK(d.resolve(0x00500000u).unit.empty());

    CHECK(d.describe_miss(main_base + 0x0C).find("main unit's table") != std::string::npos);
    CHECK(d.describe_miss(main_base + 0x09).find("not 4-aligned") != std::string::npos);
    CHECK(d.describe_miss(window.base + 0x0C).find("resident overlay ovl (id 5)") != std::string::npos);
    const std::string outside = d.describe_miss(0x00500000u);
    CHECK(outside.find("outside every dispatch table") != std::string::npos);
    CHECK(outside.find("overlay window [0x00300000, 0x00300400)") != std::string::npos);
}
