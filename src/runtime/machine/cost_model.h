// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/clock.h"

#include <cstdint>
#include <string_view>

// What guest work costs in virtual time (ADR-0007). Recompiled code has no cycle counts, so the safe points
// charge these amounts; the values change guest-visible timing (how many polling iterations fit in a field,
// when a disc read completes), so they live in one place, carry a name, and every change is recorded in the
// milestone document with its effect. Determinism does not depend on the values, only on their being fixed.
namespace dq8::rt {

struct CostModel {
    std::string_view name = "m4-v1";

    // Safe points.
    Cycles backedge_scale = 1; // multiplies the cycles argument of eeCheckpointDue (the emitter passes 32)
    Cycles dispatch = 12;      // every dispatched jal, jalr or jr
    Cycles kernel_call = 150;  // every syscall or by-name kernel call, plus what the service charges itself
    Cycles mmio_access = 8;    // every handled device-register access

    // Completions scheduled as events (never instantaneous, so the guest always observes a busy state first).
    Cycles dma_per_qword = 4;                         // DMAC transfer time per quadword, from the start
    std::uint64_t rpc_latency_us = 20;                // SIF RPC: fixed part of an IOP reply
    std::uint64_t rpc_bytes_per_second = 40'000'000;  // SIF RPC: payload part
    std::uint64_t cdvd_latency_us = 100;              // disc read in "fast" mode: fixed part
    std::uint64_t cdvd_bytes_per_second = 10'000'000; // disc read in "fast" mode: data part

    // Spin skipping (SpinDetector): identical reads of a volatile register from one pc before the clock
    // jumps, and the longest single jump.
    std::uint32_t spin_reads = 64;
    Cycles spin_quantum = 4'920'115; // about one NTSC field
    // M4 (Docs/milestones/M4-playable.md, "Cost model m4-v1"). Unused until the subsystem runs, so the M3
    // path keeps its timing. The interpreted IOP (session A): every instruction one IOP cycle; every HLE
    // library call, plus a per-byte part for sysclib copies; the longest slice the IOP runs before the EE
    // gets a turn; identical device reads before the IOP's spin detector jumps to the next event.
    Cycles iop_instruction = 8;
    Cycles iop_hle_call = 160;
    Cycles iop_hle_per_byte = 2;
    Cycles iop_slice = 16'384; // 2,048 IOP cycles
    std::uint32_t iop_spin_reads = 64;
    // The SPU2 (session B): samples per output block when nothing touches the SPU2; manual DMA per halfword
    // (session B sets it from PCSX2's timing).
    std::uint32_t spu2_block_samples = 32;
    Cycles spu2_dma_per_halfword = 0;
    // A movie picture the sceMpeg HLE delivers (session C): about 4 ms.
    Cycles movie_picture = 1'179'648;
    // Memory-card replies (session D): fixed part and data part.
    std::uint64_t mc_latency_us = 200;
    std::uint64_t mc_bytes_per_second = 1'000'000;

    Cycles rpc_reply(std::uint64_t bytes) const noexcept {
        return cycles_from_us(rpc_latency_us) + cycles_for_bytes(bytes, rpc_bytes_per_second);
    }
    Cycles cdvd_read(std::uint64_t bytes) const noexcept {
        return cycles_from_us(cdvd_latency_us) + cycles_for_bytes(bytes, cdvd_bytes_per_second);
    }
    Cycles mc_reply(std::uint64_t bytes) const noexcept {
        return cycles_from_us(mc_latency_us) + cycles_for_bytes(bytes, mc_bytes_per_second);
    }
};

} // namespace dq8::rt
