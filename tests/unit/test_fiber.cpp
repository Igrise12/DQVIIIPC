// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// rt::Fiber (ADR-0007): asymmetric switches, the two invariants of fiber.h, the guard page and the headroom
// check, and a guest recursion that ends in a clean stop instead of a crash. These run under ASan in CI.
#include "runtime/kernel/fiber.h"
#include "support/contract_unit.h"
#include "support/machine_harness.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using dq8::rt::Fiber;
using dq8::rt::FiberStackSpec;

struct PingPong {
    std::vector<int> trace;
    Fiber* self = nullptr;
};

void ping_pong(void* arg) {
    auto* p = static_cast<PingPong*>(arg);
    p->self = Fiber::current();
    p->trace.push_back(1);
    Fiber::yield();
    p->trace.push_back(3);
    Fiber::yield();
    p->trace.push_back(5);
}

int recurse_to(int depth) {
    volatile char frame[256];
    frame[0] = static_cast<char>(depth);
    return depth == 0 ? frame[0] : recurse_to(depth - 1) + frame[0];
}

struct Headroom {
    std::size_t outer = 0;
    std::size_t inner = 0;
};

void measure_inner(Headroom* h) {
    volatile char frame[4096];
    frame[0] = 1;
    h->inner = Fiber::current()->headroom() + static_cast<std::size_t>(frame[0] - 1);
}

void measure(void* arg) {
    auto* h = static_cast<Headroom*>(arg);
    h->outer = Fiber::current()->headroom();
    measure_inner(h);
}

struct Caught {
    bool caught = false;
    bool resumed = false;
    std::string what;
};

void throw_and_catch(void* arg) {
    auto* c = static_cast<Caught*>(arg);
    std::string message;
    try {
        throw std::runtime_error("inside the fiber");
    } catch (const std::runtime_error& e) {
        message = e.what(); // copy it out; never yield inside the handler (fiber.h, invariant 1)
    }
    c->caught = true;
    c->what = message;
    Fiber::yield();
    c->resumed = true;
}

void yield_forever(void* /*arg*/) {
    for (;;) {
        Fiber::yield();
    }
}

} // namespace

TEST_CASE("fiber: resume and yield alternate, and the entry's return finishes it", "[fiber]") {
    PingPong p;
    auto fiber = Fiber::create(&ping_pong, &p);
    CHECK(Fiber::current() == nullptr);
    CHECK_FALSE(fiber->finished());
    fiber->resume();
    p.trace.push_back(2);
    CHECK(Fiber::current() == nullptr);
    fiber->resume();
    p.trace.push_back(4);
    fiber->resume();
    CHECK(fiber->finished());
    CHECK(p.trace == std::vector<int>{1, 2, 3, 4, 5});
    CHECK(p.self == fiber.get());
    CHECK_THROWS_AS(fiber->resume(), std::logic_error);
}

TEST_CASE("fiber: yield off any fiber and a null entry are errors", "[fiber]") {
    CHECK_THROWS_AS(Fiber::yield(), std::logic_error);
    CHECK_THROWS_AS(Fiber::create(nullptr, nullptr), std::invalid_argument);
    FiberStackSpec tiny;
    tiny.reserve = 4096;
    CHECK_THROWS_AS(Fiber::create(&yield_forever, nullptr, tiny), std::invalid_argument);
}

TEST_CASE("fiber: the stack spans the reserve, and headroom shrinks as frames grow", "[fiber]") {
    FiberStackSpec spec;
    spec.reserve = std::size_t{1} << 20;
    Headroom h;
    auto fiber = Fiber::create(&measure, &h, spec);
    CHECK(fiber->stack_high() > fiber->stack_low());
    CHECK(fiber->stack_high() - fiber->stack_low() > (std::size_t{1} << 20) - (std::size_t{64} << 10));
    fiber->resume();
    CHECK(fiber->finished());
    CHECK(h.outer > 0);
    CHECK(h.inner < h.outer);
    CHECK(h.outer - h.inner >= 4096);
}

TEST_CASE("fiber: an exception thrown and caught inside a fiber stays there", "[fiber]") {
    Caught c;
    auto fiber = Fiber::create(&throw_and_catch, &c);
    fiber->resume();
    CHECK(c.caught);
    CHECK(c.what == "inside the fiber");
    CHECK_FALSE(c.resumed);
    // The scheduler stack can throw and catch while the fiber is suspended.
    CHECK_THROWS_AS(throw std::runtime_error("outside"), std::runtime_error);
    fiber->resume();
    CHECK(c.resumed);
    CHECK(fiber->finished());
}

TEST_CASE("fiber: a suspended fiber is destroyed without unwinding", "[fiber]") {
    for (int i = 0; i < 64; ++i) {
        auto fiber = Fiber::create(&yield_forever, nullptr);
        fiber->resume();
        fiber->resume();
        CHECK_FALSE(fiber->finished());
    } // destroyed suspended, 64 times: no leak, no crash
}

TEST_CASE("fiber: deep host recursion fits the default reserve", "[fiber]") {
    struct Deep {
        int result = -1;
    } deep;
    auto fiber = Fiber::create([](void* arg) { static_cast<Deep*>(arg)->result = recurse_to(2000); }, &deep);
    fiber->resume();
    CHECK(fiber->finished());
    CHECK(deep.result >= 0);
}

#if defined(__linux__)
TEST_CASE("fiber: the page below the stack is a guard page", "[fiber]") {
    FiberStackSpec spec;
    spec.reserve = std::size_t{1} << 20;
    auto fiber = Fiber::create(&yield_forever, nullptr, spec);
    const std::uintptr_t below = fiber->stack_low() - 1;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    std::string perms;
    while (std::getline(maps, line)) {
        std::istringstream in(line);
        std::string range;
        in >> range >> perms;
        const auto dash = range.find('-');
        const std::uintptr_t lo = std::stoull(range.substr(0, dash), nullptr, 16);
        const std::uintptr_t hi = std::stoull(range.substr(dash + 1), nullptr, 16);
        if (below >= lo && below < hi) {
            break;
        }
        perms.clear();
    }
    CHECK(perms.substr(0, 3) == "---");
    // Without a guard the same page is ordinary memory.
    spec.guard_page = false;
    auto open = Fiber::create(&yield_forever, nullptr, spec);
    CHECK(open->stack_low() > 0);
}
#endif

TEST_CASE("fiber: a runaway guest recursion ends in a clean call-depth stop", "[fiber][contract]") {
    using namespace dq8::rt;
    const dq8_game_module_desc* desc = contract_unit_descriptor();
    MachineConfig config;
    config.max_call_depth = 100'000'000; // only the stack headroom check can stop it
    dq8::test::TestMachine m(DispatchTable::from_descriptor(*desc), config, contract::unit_name);
    const SliceResult r = m.run_from(contract::recurse);
    REQUIRE(r.stopped);
    CHECK(r.stop->reason == StopReason::call_depth);
    CHECK(r.stop->message.find("KiB left") != std::string::npos);
    CHECK(r.dispatches > 1000);
}
