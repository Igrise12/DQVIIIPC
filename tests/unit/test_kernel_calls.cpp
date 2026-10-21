// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/kernel/kernel_calls.h"
#include "runtime/kernel/kernel_hle.h"

#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string>
#include <string_view>

// PS2Recomp's list of by-name kernel calls and stubs: session A's byte-for-byte copy once it lands, the
// pinned submodule's original until then.
#if __has_include("dq8/abi/ps2recomp/ps2_call_list.h")
#include "dq8/abi/ps2recomp/ps2_call_list.h"
#else
#include "../../third_party/PS2Recomp/ps2xRuntime/include/ps2_call_list.h"
#endif

namespace {

using namespace dq8::rt;

#define DQ8_TEST_NAME(name) #name,
const std::set<std::string_view> syscall_list{PS2_SYSCALL_LIST(DQ8_TEST_NAME)};
const std::set<std::string_view> stub_list{PS2_STUB_LIST(DQ8_TEST_NAME)};
#undef DQ8_TEST_NAME

// Kernel calls that PS2Recomp's dispatcher reaches by number but that have no ps2_syscalls:: or ps2_stubs::
// form: they can only arrive through a syscall instruction.
const std::set<std::string_view> number_only{
    "SetupHeap",      "EnableIntcHandler", "DisableIntcHandler", "EnableDmacHandler",    "DisableDmacHandler",
    "FindAddress",    "iDeleteSema",       "LoadExecPS2",        "SetVTLBRefillHandler", "ExecOSD",
    "GetGsDxDyOffset"};

} // namespace

TEST_CASE("kernel calls: numbers are unique and every name is a call PS2Recomp knows", "[kernel]") {
    std::set<std::int32_t> numbers;
    for (const KernelCall& c : kernel_calls()) {
        INFO(std::string(c.name) << " " << c.number);
        CHECK(numbers.insert(c.number).second);
        const bool by_name = syscall_list.count(c.name) != 0 || stub_list.count(c.name) != 0;
        CHECK((by_name || number_only.count(c.name) != 0));
    }
    // The number-only list is honest: none of them gained a by-name form upstream.
    for (const std::string_view n : number_only) {
        INFO(std::string(n));
        CHECK(syscall_list.count(n) == 0);
        CHECK(stub_list.count(n) == 0);
    }
    CHECK(kernel_calls().size() > 100);
}

TEST_CASE("kernel calls: every name has exactly one canonical number", "[kernel]") {
    std::set<std::string_view> names;
    for (const KernelCall& c : kernel_calls()) {
        names.insert(c.name);
    }
    for (const std::string_view n : names) {
        INFO(std::string(n));
        int canonical = 0;
        for (const KernelCall& c : kernel_calls()) {
            canonical += (c.name == n && !c.alias) ? 1 : 0;
        }
        CHECK(canonical == 1);
        const auto number = kernel_call_number(n);
        REQUIRE(number.has_value());
        CHECK(kernel_call_name(static_cast<std::uint32_t>(*number)) == n);
    }
}

TEST_CASE("kernel calls: the public SDK numbering", "[kernel]") {
    CHECK(kernel_call_name(0x3C) == "SetupThread");
    CHECK(kernel_call_name(0x3D) == "SetupHeap");
    CHECK(kernel_call_name(0x40) == "CreateSema");
    CHECK(kernel_call_name(0x64) == "FlushCache");
    CHECK(kernel_call_name(0x77) == "sceSifSetDma");
    // Interrupt-context variants arrive as negative numbers in $v1.
    CHECK(kernel_call_name(static_cast<std::uint32_t>(-0x43)) == "iSignalSema");
    CHECK(kernel_call_name(static_cast<std::uint32_t>(-0x68)) == "iFlushCache");
    CHECK(kernel_call_name(0x04) == "ExitThread"); // an alias of 0x23
    CHECK(kernel_call_number("ExitThread") == 0x23);
    CHECK(kernel_call_number("iSignalSema") == -0x43);
    CHECK_FALSE(kernel_call_name(0x7FFF).has_value());
    CHECK_FALSE(kernel_call_number("NoSuchCall").has_value());
    CHECK(format_kernel_call_number(0x3C) == "0x3C");
    CHECK(format_kernel_call_number(static_cast<std::uint32_t>(-0x1A)) == "-0x1A");
}

TEST_CASE("kernel HLE: empty in M2, handlers by name", "[kernel]") {
    KernelHle hle;
    CHECK_FALSE(hle.handle_syscall(0x3C, nullptr, nullptr));
    CHECK_FALSE(hle.handle_named("FlushCache", nullptr, nullptr));
    int ran = 0;
    hle.register_handler("FlushCache", [&ran](std::uint8_t*, R5900Context*) { ++ran; });
    CHECK(hle.has_handler("FlushCache"));
    CHECK(hle.handle_named("FlushCache", nullptr, nullptr));
    CHECK(hle.handle_syscall(0x64, nullptr, nullptr)); // the number reaches the same handler
    CHECK(ran == 2);
}
