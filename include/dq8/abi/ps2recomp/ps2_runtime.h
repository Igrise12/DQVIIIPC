// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// The runtime surface that PS2Recomp-generated code compiles against (it #includes "ps2_runtime.h").
// PS2Runtime here is not PS2Recomp's runtime class: it is a two-pointer adapter whose members forward to the
// host's dq8_runtime_api table (game_module.h), so a game module links against nothing in the host.
// The member set is exactly what the emitter at eb8688b2 calls; Docs/milestones/M2-first-recompile.md lists
// it with call counts. Keep this file in step with the pinned recompiler.
#pragma once

#include "dq8/abi/game_module.h"
#include "ps2_address.h"
#include "r5900_context.h"

#include <cstdint>
#include <type_traits>

// Write tracing hook referenced by the WRITE* macros; a no-op in our runtime.
inline void ps2TraceGuestWrite(std::uint8_t* /*rdram*/, std::uint32_t /*guestAddr*/, std::uint32_t /*size*/,
                               std::uint64_t /*valueLo*/, std::uint64_t /*valueHi*/, const char* /*op*/,
                               const R5900Context* /*ctx*/) {}

class PS2Runtime {
  public:
    const dq8_runtime_api* api; // filled by the host
    void* host;                 // opaque host object, passed back to every api function

    using RecompiledFunction = void (*)(std::uint8_t*, R5900Context*, PS2Runtime*);

    enum class GuestBranchKind {
        DirectJump = DQ8_BRANCH_DIRECT_JUMP,
        DirectCall = DQ8_BRANCH_DIRECT_CALL,
        IndirectJump = DQ8_BRANCH_INDIRECT_JUMP,
        IndirectCall = DQ8_BRANCH_INDIRECT_CALL,
        Return = DQ8_BRANCH_RETURN,
    };

    bool dispatchGuestBranch(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t targetPc,
                             std::uint32_t sourcePc, std::uint32_t fallthroughPc, GuestBranchKind kind,
                             const char* debugName) {
        return api->dispatch_guest_branch(host, rdram, ctx, targetPc, sourcePc, fallthroughPc,
                                          static_cast<std::uint32_t>(kind), debugName) != 0;
    }
    bool eeCheckpointDue(std::uint32_t cycles = 32u) noexcept {
        return api->ee_checkpoint_due(host, cycles) != 0;
    }

    void handleSyscall(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t encodedSyscallId) {
        api->handle_syscall(host, rdram, ctx, encodedSyscallId);
    }
    void SignalException(R5900Context* ctx, PS2Exception exception) {
        api->signal_exception(host, ctx, static_cast<std::uint32_t>(exception));
    }
    void handleTrap(std::uint8_t* rdram, R5900Context* ctx) { api->handle_trap(host, rdram, ctx); }
    void handleBreak(std::uint8_t* rdram, R5900Context* ctx) { api->handle_break(host, rdram, ctx); }
    void handleTLBR(std::uint8_t* rdram, R5900Context* ctx) { api->handle_tlbr(host, rdram, ctx); }
    void handleTLBWI(std::uint8_t* rdram, R5900Context* ctx) { api->handle_tlbwi(host, rdram, ctx); }
    void handleTLBWR(std::uint8_t* rdram, R5900Context* ctx) { api->handle_tlbwr(host, rdram, ctx); }
    void handleTLBP(std::uint8_t* rdram, R5900Context* ctx) { api->handle_tlbp(host, rdram, ctx); }
    void clearLLBit(R5900Context* ctx) { api->clear_llbit(host, ctx); }

    std::uint8_t Load8(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t vaddr) {
        return api->load8(host, rdram, ctx, vaddr);
    }
    std::uint16_t Load16(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t vaddr) {
        return api->load16(host, rdram, ctx, vaddr);
    }
    std::uint32_t Load32(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t vaddr) {
        return api->load32(host, rdram, ctx, vaddr);
    }
    std::uint64_t Load64(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t vaddr) {
        return api->load64(host, rdram, ctx, vaddr);
    }
    __m128i Load128(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t vaddr) {
        std::uint64_t v[2] = {0, 0};
        api->load128(host, rdram, ctx, vaddr, v);
        return _mm_set_epi64x(static_cast<long long>(v[1]), static_cast<long long>(v[0]));
    }
    void Store8(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t vaddr, std::uint8_t value) {
        api->store8(host, rdram, ctx, vaddr, value);
    }
    void Store16(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t vaddr, std::uint16_t value) {
        api->store16(host, rdram, ctx, vaddr, value);
    }
    void Store32(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t vaddr, std::uint32_t value) {
        api->store32(host, rdram, ctx, vaddr, value);
    }
    void Store64(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t vaddr, std::uint64_t value) {
        api->store64(host, rdram, ctx, vaddr, value);
    }
    void Store128(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t vaddr, __m128i value) {
        alignas(16) std::uint64_t v[2];
        _mm_store_si128(reinterpret_cast<__m128i*>(v), value);
        api->store128(host, rdram, ctx, vaddr, v[0], v[1]);
    }

    void executeVU0Microprogram(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t address) {
        api->execute_vu0_microprogram(host, rdram, ctx, address);
    }
    void vu0StartMicroProgram(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t address) {
        api->vu0_start_microprogram(host, rdram, ctx, address);
    }
    void kickGifDmaChainFromMMIO(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t dPcrValue,
                                 std::uint32_t dStatValue, std::uint32_t tadr, std::uint32_t chcr) {
        api->kick_gif_dma_chain_from_mmio(host, rdram, ctx, dPcrValue, dStatValue, tadr, chcr);
    }

    static constexpr bool isSpecialAddress(std::uint32_t addr) noexcept {
        return ps2_is_special_address(addr);
    }
};

static_assert(std::is_standard_layout_v<PS2Runtime>);
static_assert(sizeof(PS2Runtime) == 2 * sizeof(void*));
static_assert(sizeof(dq8_guest_fn) == sizeof(PS2Runtime::RecompiledFunction));
