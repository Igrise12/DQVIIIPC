// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/machine_services.h"
#include "runtime/machine/mmio.h"
#include "runtime/vu/vu0_runner.h"
#include "runtime/vu/vu_interpreter.h"

#include <array>
#include <cstdint>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// VU0 and VU1 as the EE and the VIFs see them (architecture §5.4): micro and data memory mapped at
// 0x11000000 (VU0 micro 4 KiB and data 4 KiB, each mirrored over 16 KiB; VU1 micro and data 16 KiB each),
// and one interpreter per unit. A VIF starts a micro program (MSCAL, MSCNT) and it runs to its end before the
// VIF goes on: there is no VU timing in the machine, only in the interpreter's own pipeline model. VU1's
// XGKICK packets go to the GIF's PATH1. A program that does not end within max_program_cycles(), or a
// reserved instruction, stops the machine with a diagnostic.
//
// VU0's registers (M4 session D): EE code keeps them in its thread context (macro mode), so VCALLMS runs on
// the calling context's VU0 block, and VIF0's MSCAL/MSCNT on the block the kernel says EE code sees now
// (EeKernel::vu0_view: the running or interrupted thread's, or the one shared file). Without a kernel (device
// tests) VIF0 runs on the unit's own register file. The first time a different thread (or a handler) runs a
// VU0 program after another, a vu0_cross_thread event names both: the probe for VU0 use across threads.
namespace dq8::rt {

class Gif;
class EeKernel;

class VuUnits final : public IDevice, public IMmioDevice, public IVu0Runner {
  public:
    static constexpr std::uint32_t base = 0x11000000u;
    static constexpr std::uint32_t size = 0x10000u;
    // A micro program that runs longer than this is taken as a hang (about three NTSC fields of VU time).
    static constexpr std::uint64_t default_max_program_cycles = 16ull << 20;

    struct UnitStats {
        std::uint64_t programs = 0; // MSCAL/MSCNT runs
        std::uint64_t cycles = 0;   // interpreter cycles over all runs
        std::uint64_t xgkicks = 0;
    };

    explicit VuUnits(Gif& gif);

    std::string_view name() const override { return "vu"; }
    void attach(MachineServices& machine) override;
    void reset() override;

    bool read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out, const MmioContext& at) override;
    bool write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
               const MmioContext& at) override;

    // unit 0 or 1.
    std::span<std::uint8_t> micro(unsigned unit) noexcept { return units_[unit].micro; }
    std::span<std::uint8_t> data(unsigned unit) noexcept { return units_[unit].data; }
    // Copies into micro memory (VIF MPG), wrapping at its end, and invalidates decoded code.
    void write_micro(unsigned unit, std::uint32_t byte_offset, std::span<const std::uint8_t> bytes);
    // Marks micro memory as changed after a direct write through micro().
    void touch_micro(unsigned unit) noexcept { ++units_[unit].host.generation; }

    // MSCAL: runs from byte address `start_pc` to the end of the program. MSCNT: continues where the last
    // program ended.
    void start(unsigned unit, std::uint32_t start_pc, std::uint32_t top, std::uint32_t itop);
    void resume(unsigned unit, std::uint32_t top, std::uint32_t itop);
    // VCALLMS/VCALLMSR (vu0_runner.h): unit 0 runs with `ctx`'s VU0 registers in and out. Registered with
    // the machine at attach.
    std::uint64_t run_from_ee(R5900Context& ctx, std::uint32_t start) override;

    void set_max_program_cycles(std::uint64_t cycles) noexcept { max_program_cycles_ = cycles; }
    std::uint64_t max_program_cycles() const noexcept { return max_program_cycles_; }

    VuInterpreter& interpreter(unsigned unit) noexcept { return units_[unit].interp; }
    // The unit's host (XGKICK to PATH1, the micro-memory generation), for driving the interpreter directly.
    IVuHost& host(unsigned unit) noexcept { return units_[unit].host; }
    const UnitStats& stats(unsigned unit) const noexcept { return units_[unit].stats; }

  private:
    struct Unit;
    class Host final : public IVuHost {
      public:
        Host(VuUnits& owner, unsigned unit) noexcept : owner_(owner), unit_(unit) {}
        void xgkick(std::span<const std::uint8_t> packet) override;
        std::uint64_t code_generation() const override { return generation; }
        void reserved_instruction(bool upper, std::uint32_t pc, std::uint32_t instruction) override;
        std::uint64_t generation = 1;
        // A reserved instruction the interpreter reported; the device stops the machine once the
        // interpreter has returned (and restored the host rounding mode).
        bool reserved = false;
        bool reserved_upper = false;
        std::uint32_t reserved_pc = 0;
        std::uint32_t reserved_word = 0;

      private:
        VuUnits& owner_;
        unsigned unit_;
    };
    struct Unit {
        Unit(VuUnits& owner, unsigned index, std::uint32_t mem_size)
            : micro(mem_size), data(mem_size),
              interp(index == 0 ? VuInterpreter::Unit::VU0 : VuInterpreter::Unit::VU1), host(owner, index) {}
        std::vector<std::uint8_t> micro;
        std::vector<std::uint8_t> data;
        VuInterpreter interp;
        Host host;
        UnitStats stats;
    };

    // Where an offset in the 64 KiB window lands: the unit, micro or data, and the byte offset in it.
    struct Target {
        unsigned unit;
        bool micro;
        std::uint32_t offset;
    };
    static Target locate(std::uint32_t offset) noexcept;
    void run(unsigned unit, bool fresh, std::uint32_t start_pc, std::uint32_t top, std::uint32_t itop);
    // VIF0's MSCAL (fresh) or MSCNT on the kernel's VU0 view.
    void run_vu0_view(bool fresh, std::uint32_t start_pc, std::uint32_t top, std::uint32_t itop);
    void note_vu0_user();

    Gif& gif_;
    MachineServices* machine_ = nullptr;
    EeKernel* kernel_ = nullptr; // the machine's EE kernel, when there is one
    std::int64_t vu0_user_ = 0;  // the thread (-1: a handler) that ran the last VU0 program; 0: none yet
    std::set<std::pair<std::int64_t, std::int64_t>> vu0_reported_; // (previous, thread) pairs reported
    std::uint64_t max_program_cycles_ = default_max_program_cycles;
    std::array<Unit, 2> units_;
};

} // namespace dq8::rt
