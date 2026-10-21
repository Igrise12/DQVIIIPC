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
    std::string_view name = "m3-v1";

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

    Cycles rpc_reply(std::uint64_t bytes) const noexcept {
        return cycles_from_us(rpc_latency_us) + cycles_for_bytes(bytes, rpc_bytes_per_second);
    }
    Cycles cdvd_read(std::uint64_t bytes) const noexcept {
        return cycles_from_us(cdvd_latency_us) + cycles_for_bytes(bytes, cdvd_bytes_per_second);
    }
};

} // namespace dq8::rt
