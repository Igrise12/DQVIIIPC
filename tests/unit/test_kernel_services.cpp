// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/context.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/kernel/kernel_calls.h"
#include "runtime/kernel/kernel_services.h"
#include "support/fake_machine.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <string>

using namespace dq8::rt;
using dq8::test::FakeMachine;

namespace {

std::unique_ptr<R5900Context> fresh_context() {
    auto ctx = std::make_unique<R5900Context>();
    reset_context(*ctx, 0x00100000);
    return ctx;
}

} // namespace

TEST_CASE("kernel services: arguments, sign-extended results, the upper half kept", "[kernel][services]") {
    FakeMachine m;
    auto ctx = fresh_context();
    for (unsigned r = 4; r < 12; ++r) {
        set_gpr_lo(*ctx, r, 0x1000 + r);
    }
    set_gpr_lo(*ctx, 7, 0xFFFFFFFF80000000ull);
    KernelCallContext call(m, *ctx, "CreateSema", "CreateSema", true, 0x40, false);
    CHECK(call.arg(0) == 0x1004);
    CHECK(call.arg(3) == 0x80000000u);
    CHECK(call.sarg(3) == static_cast<std::int32_t>(0x80000000u));
    CHECK(call.arg(4) == 0x1008); // $t0
    CHECK(call.arg(7) == 0x100B); // $t3
    CHECK(call.arg64(3) == 0xFFFFFFFF80000000ull);
    CHECK_THROWS_AS(call.arg(8), std::out_of_range);

    ctx->r[2] = _mm_set_epi64x(0x1122334455667788ll, 0);
    call.ret(-1);
    CHECK(gpr_lo(*ctx, 2) == 0xFFFFFFFFFFFFFFFFull);
    CHECK(gpr_hi(*ctx, 2) == 0x1122334455667788ull);
    call.ret_u32(0x80000001u);
    CHECK(gpr_lo(*ctx, 2) == 0xFFFFFFFF80000001ull);
    call.ret(5);
    CHECK(gpr_lo(*ctx, 2) == 5);
    call.ret64(0x123456789ull);
    CHECK(gpr_lo(*ctx, 2) == 0x123456789ull);
}

TEST_CASE("kernel services: resolve by number, by name and through aliases", "[kernel][services]") {
    KernelServices k;
    int calls = 0;
    k.add({"GsSetCrt", [&](KernelCallContext&) { ++calls; }, 0, {}, "test"});
    k.add({"sceSifSetDma",
           [&](KernelCallContext& c) { calls += c.interrupt_context() ? 100 : 10; },
           kernel_service_interrupt_ok,
           {},
           "test"});

    const auto by_number = k.resolve_number(0x02);
    CHECK(by_number.kind == KernelResolution::Kind::native);
    CHECK(by_number.name == "GsSetCrt");
    CHECK(by_number.has_number);
    CHECK_FALSE(by_number.interrupt_context);

    const auto by_alias = k.resolve_name("SetGsCrt"); // the call-list spelling
    REQUIRE(by_alias.kind == KernelResolution::Kind::native);
    CHECK(by_alias.service == by_number.service);
    CHECK(by_alias.number == 0x02);

    const auto i_variant = k.resolve_name("isceSifSetDma");
    REQUIRE(i_variant.kind == KernelResolution::Kind::native);
    CHECK(i_variant.interrupt_context);
    CHECK(i_variant.name == "sceSifSetDma");
    const auto negative = k.resolve_number(static_cast<std::uint32_t>(-0x77));
    CHECK(negative.kind == KernelResolution::Kind::native);
    CHECK(negative.interrupt_context);

    const auto missing = k.resolve_name("CreateThread");
    CHECK(missing.kind == KernelResolution::Kind::missing);
    CHECK(missing.name == "CreateThread");
    CHECK(missing.number == 0x20);
    const auto unknown = k.resolve_number(0x7777);
    CHECK(unknown.kind == KernelResolution::Kind::missing);
    CHECK(unknown.name.empty());

    // The numbers PS2Recomp's dispatcher lacks are known by name now.
    CHECK(k.resolve_number(0x6B).name == "sceSifStopDma");
    CHECK(k.resolve_number(0x80).name == "GetGsDxDyOffset");

    FakeMachine m;
    auto ctx = fresh_context();
    KernelCallContext c(m, *ctx, i_variant.name, "isceSifSetDma", true, -0x77, true);
    k.invoke(*i_variant.service, c);
    CHECK(calls == 100);
    CHECK(k.call_count("sceSifSetDma") == 1);
    CHECK(k.services().size() == 2);
}

TEST_CASE("kernel services: SetSyscall handlers take precedence over native services", "[kernel][services]") {
    KernelServices k;
    k.add({"SetAlarm", [](KernelCallContext&) {}, 0, {}, "test"});
    k.install_guest_handler(0x18, 0x00080000);
    const auto r = k.resolve_number(0x18);
    CHECK(r.kind == KernelResolution::Kind::guest_handler);
    CHECK(r.guest_address == 0x00080000);
    CHECK(k.resolve_name("SetAlarm").kind == KernelResolution::Kind::guest_handler);
    // A number with no name in the table can still have a guest handler.
    k.install_guest_handler(0x7E, 0x00081000);
    CHECK(k.resolve_number(0x7E).kind == KernelResolution::Kind::guest_handler);
    k.remove_guest_handler(0x18);
    CHECK(k.resolve_number(0x18).kind == KernelResolution::Kind::native);
    CHECK(k.guest_handler(0x7E) == 0x00081000u);
    CHECK_FALSE(k.guest_handler(0x18).has_value());
}

TEST_CASE("kernel services: registration rules", "[kernel][services]") {
    KernelServices k;
    CHECK_THROWS_AS(k.add({"", [](KernelCallContext&) {}, 0, {}, {}}), std::invalid_argument);
    CHECK_THROWS_AS(k.add({"FlushCache", {}, 0, {}, {}}), std::invalid_argument); // no function
    CHECK_THROWS_AS(k.add({"FlushCache", {}, kernel_service_noop, {}, {}}), std::invalid_argument); // no note
    k.add({"FlushCache", {}, kernel_service_noop, "no caches in a native port", "test"});
    CHECK_THROWS_AS(k.add({"FlushCache", {}, kernel_service_noop, "again", "test"}), std::invalid_argument);
    CHECK_THROWS_AS(k.add({"SetGsCrt", [](KernelCallContext&) {}, 0, {}, {}}),
                    std::invalid_argument);                                 // an alias
    CHECK_THROWS_AS(k.alias("FlushCache", "Other"), std::invalid_argument); // a service name
    CHECK_THROWS_AS(k.alias("Same", "Same"), std::invalid_argument);
    REQUIRE(k.find("FlushCache") != nullptr);
    CHECK(k.find("FlushCache")->note == "no caches in a native port");
}

TEST_CASE("kernel services: noops run silently, strict mode and unimplemented stop", "[kernel][services]") {
    FakeMachine m;
    KernelServices& k = m.kernel();
    k.add({"FlushCache", {}, kernel_service_noop, "no caches in a native port", "test"});
    auto ctx = fresh_context();
    set_gpr_lo(*ctx, 4, 2);
    ctx->pc = 0x00116AA8;
    const auto r = k.resolve_number(0x64);
    REQUIRE(r.kind == KernelResolution::Kind::native);
    KernelCallContext call(m, *ctx, r.name, "FlushCache", true, 0x64, false);
    k.invoke(*r.service, call);
    CHECK(k.call_count("FlushCache") == 1);

    k.set_strict(true);
    try {
        k.invoke(*r.service, call);
        FAIL_CHECK("strict mode did not stop");
    } catch (const GuestStop& stop) {
        CHECK(stop.info().reason == StopReason::syscall);
        CHECK(stop.info().number == 0x64);
        CHECK(stop.info().call_name == "FlushCache");
        CHECK(stop.info().args[0] == 2);
        CHECK(stop.info().pc == 0x00116AA8);
        CHECK(std::string(stop.what()).find("strict") != std::string::npos);
    }

    KernelCallContext by_name(m, *ctx, "sceSifStopDma", "sceSifStopDma", false, 0, false);
    try {
        by_name.unimplemented("not in M3");
        FAIL_CHECK("unimplemented returned");
    } catch (const GuestStop& stop) {
        CHECK(stop.info().reason == StopReason::named_call);
        CHECK_FALSE(stop.info().has_number);
    }
}
