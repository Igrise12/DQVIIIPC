// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// A minimal host for the ABI tests: one dq8_runtime_api table that records what generated code asks of it, a
// dispatcher over one module's table that follows the dispatch contract in
// Docs/milestones/M2-first-recompile.md, and a run loop. Not the real runtime (src/runtime); just enough to
// watch the ABI work from the host side.
#pragma once

#include "dq8/abi/game_module.h"
#include "ps2_address.h"
#include "ps2_runtime.h"

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dq8::abi_test {

// handle_syscall throws this to stop the guest, as the real host's GuestStop does. It unwinds through module
// frames.
struct GuestStop {
    std::uint32_t pc;
    std::uint32_t encoded_id;
};

struct Access {
    bool store;
    std::uint32_t size;
    std::uint32_t addr;
    std::uint64_t value;

    bool operator==(const Access&) const = default;
};

struct NamedCall {
    std::uint32_t kind;
    std::string name;

    bool operator==(const NamedCall&) const = default;
};

class FakeHost {
  public:
    FakeHost() : ram(PS2_RAM_SIZE), ctx(std::make_unique<R5900Context>()), runtime{&api_table(), this} {}
    FakeHost(const FakeHost&) = delete; // runtime.host points here
    FakeHost& operator=(const FakeHost&) = delete;

    std::vector<std::uint8_t> ram;
    std::unique_ptr<R5900Context> ctx; // 16-byte aligned, on the heap
    PS2Runtime runtime;

    const dq8_game_module_desc* desc =
        nullptr; // the table dispatch_guest_branch and run() look functions up in
    bool unwind_calls =
        false; // when set, a call returns false after the callee, as a host that switched away would
    std::map<std::uint32_t, std::uint64_t> special_values; // what loads from special addresses return

    std::vector<Access> accesses;
    std::vector<NamedCall> calls;
    std::uint32_t dispatches = 0;

    std::uint8_t* ram_base() { return ram.data(); }
    std::uint32_t gpr(int reg) const { return getRegU32(ctx.get(), reg); }
    void set_gpr(int reg, std::uint32_t value) {
        ctx->r[reg] = _mm_set_epi64x(0, static_cast<long long>(static_cast<std::int32_t>(value)));
    }

    PS2Runtime::RecompiledFunction lookup(std::uint32_t pc) const {
        if (desc == nullptr || pc < desc->table_base || pc >= desc->table_end || (pc & 3u) != 0u) {
            return nullptr;
        }
        return desc->table[(pc - desc->table_base) / 4u];
    }

    // upstream's run loop with one thread: call whatever the table has at pc until pc is 0 or the guest
    // stops.
    void run(std::uint32_t entry, std::uint32_t max_dispatches = 1000) {
        ctx->pc = entry;
        while (ctx->pc != 0u) {
            const auto fn = lookup(ctx->pc);
            if (fn == nullptr) {
                throw std::runtime_error("no function at pc " + std::to_string(ctx->pc));
            }
            if (++dispatches > max_dispatches) {
                throw std::runtime_error("dispatch limit reached");
            }
            fn(ram_base(), ctx.get(), &runtime);
        }
    }

  private:
    static FakeHost& self(void* host) { return *static_cast<FakeHost*>(host); }

    [[noreturn]] static void unexpected(const char* what) {
        throw std::logic_error(std::string("unexpected host call: ") + what);
    }

    static std::uint64_t load(void* host, std::uint32_t size, std::uint32_t vaddr) {
        auto& h = self(host);
        const auto it = h.special_values.find(vaddr);
        const std::uint64_t value = it == h.special_values.end() ? 0u : it->second;
        h.accesses.push_back({false, size, vaddr, value});
        return value;
    }
    static void store(void* host, std::uint32_t size, std::uint32_t vaddr, std::uint64_t value) {
        self(host).accesses.push_back({true, size, vaddr, value});
    }

    static const dq8_runtime_api& api_table() {
        static const dq8_runtime_api api = [] {
            dq8_runtime_api a{};
            a.struct_size = sizeof(dq8_runtime_api);
            a.abi_version = DQ8_GAME_MODULE_ABI_VERSION;
            a.dispatch_guest_branch = [](void* host, std::uint8_t* mem, R5900Context* c, std::uint32_t target,
                                         std::uint32_t /*source*/, std::uint32_t fallthrough,
                                         std::uint32_t kind, const char* /*debug_name*/) -> std::int32_t {
                auto& h = self(host);
                ++h.dispatches;
                c->pc = target;
                if (kind != DQ8_BRANCH_DIRECT_CALL && kind != DQ8_BRANCH_INDIRECT_CALL) {
                    return 0; // a jump: the caller returns and the run loop continues at ctx->pc
                }
                const auto fn = h.lookup(target);
                if (fn == nullptr) {
                    throw std::runtime_error("no function at call target " + std::to_string(target));
                }
                fn(mem, c, &h.runtime);
                if (c->pc == target) {
                    c->pc = fallthrough; // pc unchanged: treat it as returned
                }
                return (!h.unwind_calls && c->pc == fallthrough) ? 1 : 0;
            };
            a.ee_checkpoint_due = [](void*, std::uint32_t) -> std::int32_t { return 0; };
            a.handle_syscall = [](void*, std::uint8_t*, R5900Context* c, std::uint32_t encoded_id) {
                throw GuestStop{c->pc, encoded_id};
            };
            a.call_named = [](void* host, std::uint32_t kind, const char* name, std::uint8_t*,
                              R5900Context*) { self(host).calls.push_back({kind, name}); };
            a.signal_exception = [](void*, R5900Context*, std::uint32_t) { unexpected("signal_exception"); };
            a.handle_trap = [](void*, std::uint8_t*, R5900Context*) { unexpected("handle_trap"); };
            a.handle_break = [](void*, std::uint8_t*, R5900Context*) { unexpected("handle_break"); };
            a.handle_tlbr = [](void*, std::uint8_t*, R5900Context*) { unexpected("handle_tlbr"); };
            a.handle_tlbwi = [](void*, std::uint8_t*, R5900Context*) { unexpected("handle_tlbwi"); };
            a.handle_tlbwr = [](void*, std::uint8_t*, R5900Context*) { unexpected("handle_tlbwr"); };
            a.handle_tlbp = [](void*, std::uint8_t*, R5900Context*) { unexpected("handle_tlbp"); };
            a.clear_llbit = [](void*, R5900Context*) { unexpected("clear_llbit"); };
            a.load8 = [](void* host, std::uint8_t*, R5900Context*, std::uint32_t vaddr) {
                return static_cast<std::uint8_t>(load(host, 1, vaddr));
            };
            a.load16 = [](void* host, std::uint8_t*, R5900Context*, std::uint32_t vaddr) {
                return static_cast<std::uint16_t>(load(host, 2, vaddr));
            };
            a.load32 = [](void* host, std::uint8_t*, R5900Context*, std::uint32_t vaddr) {
                return static_cast<std::uint32_t>(load(host, 4, vaddr));
            };
            a.load64 = [](void* host, std::uint8_t*, R5900Context*, std::uint32_t vaddr) {
                return load(host, 8, vaddr);
            };
            a.load128 = [](void* host, std::uint8_t*, R5900Context*, std::uint32_t vaddr,
                           std::uint64_t out[2]) {
                out[0] = load(host, 16, vaddr);
                out[1] = 0;
            };
            a.store8 = [](void* host, std::uint8_t*, R5900Context*, std::uint32_t vaddr, std::uint8_t value) {
                store(host, 1, vaddr, value);
            };
            a.store16 = [](void* host, std::uint8_t*, R5900Context*, std::uint32_t vaddr,
                           std::uint16_t value) { store(host, 2, vaddr, value); };
            a.store32 = [](void* host, std::uint8_t*, R5900Context*, std::uint32_t vaddr,
                           std::uint32_t value) { store(host, 4, vaddr, value); };
            a.store64 = [](void* host, std::uint8_t*, R5900Context*, std::uint32_t vaddr,
                           std::uint64_t value) { store(host, 8, vaddr, value); };
            a.store128 = [](void* host, std::uint8_t*, R5900Context*, std::uint32_t vaddr, std::uint64_t lo,
                            std::uint64_t /*hi*/) { store(host, 16, vaddr, lo); };
            a.execute_vu0_microprogram = [](void*, std::uint8_t*, R5900Context*, std::uint32_t) {
                unexpected("execute_vu0_microprogram");
            };
            a.vu0_start_microprogram = [](void*, std::uint8_t*, R5900Context*, std::uint32_t) {
                unexpected("vu0_start_microprogram");
            };
            a.kick_gif_dma_chain_from_mmio = [](void*, std::uint8_t*, R5900Context*, std::uint32_t,
                                                std::uint32_t, std::uint32_t, std::uint32_t) {
                unexpected("kick_gif_dma_chain_from_mmio");
            };
            return a;
        }();
        return api;
    }
};

} // namespace dq8::abi_test
