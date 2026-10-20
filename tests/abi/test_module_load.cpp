// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Loads the sample game module (tests/abi/sample_unit, built through cmake/module) the way the host will:
// dlopen, the one exported symbol, the descriptor; then runs the unit through the fake host. POSIX only for
// now; the host's own loader (src/runtime/platform/dynlib) covers Windows.
#include "support/fake_host.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

#ifndef DQ8_ABI_SAMPLE_MODULE
#error "DQ8_ABI_SAMPLE_MODULE must name the sample module (tests/abi/CMakeLists.txt sets it)"
#endif

using dq8::abi_test::Access;
using dq8::abi_test::FakeHost;
using dq8::abi_test::GuestStop;
using dq8::abi_test::NamedCall;

namespace {

class LoadedModule {
  public:
    explicit LoadedModule(const char* path) : handle_(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {
        if (handle_ == nullptr) {
            const char* why = dlerror();
            error_ = why != nullptr ? why : "dlopen failed";
        }
    }
    ~LoadedModule() {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }
    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;

    bool loaded() const { return handle_ != nullptr; }
    const std::string& error() const { return error_; }
    void* symbol(const char* name) const { return dlsym(handle_, name); }

    const dq8_game_module_desc* describe() const {
        void* sym = symbol(DQ8_GAME_MODULE_DESCRIBE_SYMBOL);
        if (sym == nullptr) {
            return nullptr;
        }
        dq8_game_module_describe_fn fn = nullptr;
        static_assert(sizeof(fn) == sizeof(sym));
        std::memcpy(&fn, &sym, sizeof(fn)); // object pointer to function pointer, as dlsym requires
        return fn();
    }

  private:
    void* handle_;
    std::string error_;
};

constexpr std::uint32_t kEntry = 0x200000u;
constexpr std::uint32_t kAfterSyscall = 0x200044u;
constexpr std::uint32_t kUnhandled = 0x2000E0u;
constexpr std::uint32_t kRamWord = 0x00300000u;

// The sample unit's guest program (sample_start_0x200000.cpp): a0 = 0x12345678, v0 = helper(a0) = a0 + 1, two
// stores the host must see, one RAM load, three by-name calls, then syscall 0x3C.
void check_stopped_at_syscall(const FakeHost& host, const GuestStop& stop) {
    CHECK(stop.pc == kAfterSyscall);
    CHECK(stop.encoded_id == 0u);
    CHECK(host.gpr(3) == 0x3Cu);       // v1: the syscall number
    CHECK(host.gpr(4) == 0x12345678u); // a0
    CHECK(host.gpr(2) == 0x12345679u); // v0, from the helper's loop
    CHECK(host.gpr(5) == 0xCAFEF00Du); // a1, the FAST_READ32 of a RAM literal
    const std::vector<Access> accesses{{true, 4, 0x70000010u, 0x12345679u},
                                       {true, 4, 0x11000010u, 0x12345678u}};
    CHECK(host.accesses == accesses);
    const std::vector<NamedCall> calls{
        {DQ8_CALL_STUB, "puts"}, {DQ8_CALL_SYSCALL, "FlushCache"}, {DQ8_CALL_TODO, "sample_skipped"}};
    CHECK(host.calls == calls);
}

GuestStop run_to_stop(FakeHost& host, std::uint32_t entry) {
    try {
        host.run(entry);
    } catch (const GuestStop& stop) {
        return stop;
    }
    FAIL("the guest did not stop at its syscall");
    return {};
}

} // namespace

TEST_CASE("the sample module exports one symbol with a valid descriptor", "[abi][module]") {
    const LoadedModule module(DQ8_ABI_SAMPLE_MODULE);
    INFO(module.error());
    REQUIRE(module.loaded());

    const dq8_game_module_desc* desc = module.describe();
    REQUIRE(desc != nullptr);
    CHECK(module.describe() == desc);
    CHECK(desc->magic == DQ8_GAME_MODULE_MAGIC);
    CHECK(desc->abi_version == DQ8_GAME_MODULE_ABI_VERSION);
    CHECK(desc->struct_size == sizeof(dq8_game_module_desc));
    CHECK(desc->unit_kind == static_cast<std::uint32_t>(DQ8_UNIT_MAIN));
    CHECK(desc->overlay_id == 0u);
    CHECK(std::string(desc->unit_name) == "sample");
    CHECK(std::string(desc->profile_id) == "SAMPLE");
    CHECK(std::string(desc->source_sha1) == "0123456789abcdef0123456789abcdef01234567");
    CHECK(std::string(desc->generator) == "hand-written sample");
    CHECK(desc->context_size == sizeof(R5900Context));
    CHECK(desc->context_align == alignof(R5900Context));
    CHECK(desc->entry_point == kEntry);
    CHECK(desc->table_base == 0x200000u);
    CHECK(desc->table_end == 0x2000E4u);
    CHECK(desc->table_slot_count == (desc->table_end - desc->table_base) / 4u);
    REQUIRE(desc->table != nullptr);

    // The static initialiser in register_functions.cpp filled the table before dlopen returned: functions,
    // stubs and resume entries, nothing else.
    std::vector<std::uint32_t> filled;
    for (std::uint32_t slot = 0; slot < desc->table_slot_count; ++slot) {
        if (desc->table[slot] != nullptr) {
            filled.push_back(desc->table_base + slot * 4u);
        }
    }
    const std::vector<std::uint32_t> expected{0x200000u, 0x200010u, 0x20002Cu, 0x200034u,
                                              0x20003Cu, 0x200044u, 0x200060u, 0x200068u,
                                              0x200080u, 0x2000A0u, 0x2000C0u, 0x2000E0u};
    CHECK(filled == expected);
    CHECK(desc->table[(0x200044u - desc->table_base) / 4u] == desc->table[0]); // a resume entry: its owner

    // Hidden: neither the renamed table globals nor upstream's names are visible to the host.
    CHECK(module.symbol("dq8_unit_sample_table") == nullptr);
    CHECK(module.symbol("g_ps2RecompiledFunctionTable") == nullptr);
}

TEST_CASE("the sample unit runs to its syscall through the host api", "[abi][module]") {
    const LoadedModule module(DQ8_ABI_SAMPLE_MODULE);
    INFO(module.error());
    REQUIRE(module.loaded());

    FakeHost host;
    host.desc = module.describe();
    REQUIRE(host.desc != nullptr);
    const std::uint32_t ram_word = 0xCAFEF00Du;
    std::memcpy(host.ram_base() + kRamWord, &ram_word, sizeof(ram_word));

    SECTION("calls return through dispatch") {
        const GuestStop stop = run_to_stop(host, host.desc->entry_point);
        check_stopped_at_syscall(host, stop);
    }
    SECTION("calls unwind and re-enter through their resume entries") {
        host.unwind_calls = true;
        const GuestStop stop = run_to_stop(host, host.desc->entry_point);
        check_stopped_at_syscall(host, stop);
        CHECK(host.dispatches > 8u); // four calls, each followed by a re-entry from the run loop
    }
    SECTION("the syscall's resume entry continues after it") {
        const GuestStop stop = run_to_stop(host, host.desc->entry_point);
        host.set_gpr(31, 0u); // return to pc 0, which ends the run loop
        host.run(stop.pc);
        CHECK(host.ctx->pc == 0u);
    }
}

TEST_CASE("an untranslated instruction throws out of the module to the host", "[abi][module]") {
    const LoadedModule module(DQ8_ABI_SAMPLE_MODULE);
    INFO(module.error());
    REQUIRE(module.loaded());

    FakeHost host;
    host.desc = module.describe();
    REQUIRE(host.desc != nullptr);
    CHECK_THROWS_AS(host.run(kUnhandled), std::runtime_error);
    CHECK(host.ctx->pc == kUnhandled);
}
