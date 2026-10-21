// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// SPDX-FileCopyrightText: PCSX2 Dev Team
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp (VIFcode set, the VIF register writes of ps2_memory.cpp).
// UNPACK details follow PCSX2 (GPL-3.0-or-later) pcsx2/Vif_Unpack.cpp at 5b6e6e2.
#pragma once

#include "runtime/dma/register_block.h"
#include "runtime/machine/machine_services.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// VIF0 and VIF1 (architecture §5.3): a VIFcode interpreter over a word stream from DMA or the FIFO. The
// stream is processed as it arrives; a command whose data has not all arrived waits for the next push, so
// VIFcodes and their data may be split anywhere across DMA tags and transfers. UNPACK writes VU data memory
// (all formats, masks, the offset and difference modes, skipping and filling cycles, the TOPS-relative
// address), MPG writes micro memory, MSCAL/MSCALF/MSCNT run a micro program to its end, VIF1 DIRECT and
// DIRECTHL feed PATH2 of the GIF and MSKPATH3 masks PATH3. A VIFcode with the i bit raises INTC 4 or 5 and,
// unless ERR.MII masks it, stalls the VIF after the command until FBRST.STC; input that arrives meanwhile is
// kept and processed after the stall clears.
namespace dq8::rt {

class VuUnits;
class Gif;

class Vif final : public IDevice, public IRegisterBlock {
  public:
    // Register offsets from the VIFn register base (0x10003800 / 0x10003C00).
    static constexpr std::uint32_t reg_stat = 0x00u;
    static constexpr std::uint32_t reg_fbrst = 0x10u;
    static constexpr std::uint32_t reg_err = 0x20u;
    static constexpr std::uint32_t reg_mark = 0x30u;
    static constexpr std::uint32_t reg_cycle = 0x40u;
    static constexpr std::uint32_t reg_mode = 0x50u;
    static constexpr std::uint32_t reg_num = 0x60u;
    static constexpr std::uint32_t reg_mask = 0x70u;
    static constexpr std::uint32_t reg_code = 0x80u;
    static constexpr std::uint32_t reg_itops = 0x90u;
    static constexpr std::uint32_t reg_base = 0xA0u;
    static constexpr std::uint32_t reg_ofst = 0xB0u;
    static constexpr std::uint32_t reg_tops = 0xC0u;
    static constexpr std::uint32_t reg_itop = 0xD0u;
    static constexpr std::uint32_t reg_top = 0xE0u;
    static constexpr std::uint32_t reg_r0 = 0x100u;
    static constexpr std::uint32_t reg_c0 = 0x140u;

    // STAT bits.
    static constexpr std::uint32_t stat_vps_waiting = 1u << 0; // a command waits for data
    static constexpr std::uint32_t stat_mrk = 1u << 6;
    static constexpr std::uint32_t stat_dbf = 1u << 7;
    static constexpr std::uint32_t stat_vss = 1u << 8;  // stopped by FBRST.STP
    static constexpr std::uint32_t stat_vfs = 1u << 9;  // stopped by FBRST.FBK
    static constexpr std::uint32_t stat_vis = 1u << 10; // stalled by an i bit
    static constexpr std::uint32_t stat_int = 1u << 11;
    static constexpr std::uint32_t stat_er0 = 1u << 12;
    static constexpr std::uint32_t stat_er1 = 1u << 13;
    static constexpr std::uint32_t stat_fdr = 1u << 23; // VIF1: FIFO direction VIF1 -> memory
    static constexpr std::uint32_t stat_stalls = stat_vss | stat_vfs | stat_vis;

    // ERR bits.
    static constexpr std::uint32_t err_mii = 1u << 0;
    static constexpr std::uint32_t err_me1 = 1u << 2;

    struct Registers {
        std::uint32_t stat = 0;
        std::uint32_t err = 0;
        std::uint32_t mark = 0;
        std::uint32_t cycle = 0;
        std::uint32_t mode = 0;
        std::uint32_t num = 0;
        std::uint32_t mask = 0;
        std::uint32_t code = 0;
        std::uint32_t itops = 0;
        std::uint32_t base = 0; // VIF1
        std::uint32_t ofst = 0; // VIF1
        std::uint32_t tops = 0; // VIF1
        std::uint32_t itop = 0;
        std::uint32_t top = 0; // VIF1
        std::array<std::uint32_t, 4> row{};
        std::array<std::uint32_t, 4> col{};
    };

    // `gif` is required for VIF1 (DIRECT, MSKPATH3, readback) and ignored for VIF0.
    Vif(unsigned unit, VuUnits& vu, Gif* gif) noexcept : unit_(unit), vu_(vu), gif_(gif) {}

    std::string_view name() const override { return unit_ == 0 ? "vif0" : "vif1"; }
    void attach(MachineServices& machine) override;
    void reset() override;

    bool read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& at) override;
    bool write32(std::uint32_t offset, std::uint32_t value, std::uint32_t mask,
                 const MmioContext& at) override;
    bool read128(std::uint32_t offset, std::uint8_t* out, const MmioContext& at) override;
    bool write128(std::uint32_t offset, const std::uint8_t* in, const MmioContext& at) override;

    // Data for the VIF (DMA or FIFO): processed now, or kept while the VIF is stalled.
    void push(std::span<const std::uint8_t> bytes);
    // True while input is kept because of a stall (the DMAC holds the channel's completion).
    bool holding_input() const noexcept { return in_pos_ < in_.size(); }
    bool stalled() const noexcept { return (regs_.stat & stat_stalls) != 0; }
    // Called when kept input has drained after a stall cleared.
    void set_drained_callback(std::function<void()> callback) { drained_ = std::move(callback); }

    const Registers& registers() const noexcept { return regs_; }
    unsigned unit() const noexcept { return unit_; }

  private:
    enum class Phase : std::uint8_t { command, stmask, strow, stcol, mpg, direct, unpack };

    std::uint32_t regs_offset() const noexcept;
    void process();
    // Runs the command in code_ (decoded from the stream); returns the number of data words it needs.
    std::uint32_t begin_command();
    void finish_command();
    void execute_data();
    void unpack();
    // CL and WL from CYCLE for UNPACK.
    std::pair<std::uint32_t, std::uint32_t> cycle_lengths() const noexcept;
    void start_micro(bool continue_program, std::uint32_t start_pc);
    [[noreturn]] void fail(const std::string& what);

    unsigned unit_;
    VuUnits& vu_;
    Gif* gif_;
    MachineServices* machine_ = nullptr;
    std::optional<RegisterWindow> regs_window_;
    std::optional<RegisterWindow> fifo_window_;

    Registers regs_;
    std::vector<std::uint8_t> in_; // input not yet consumed (whole words), from in_pos_
    std::size_t in_pos_ = 0;
    Phase phase_ = Phase::command;
    std::uint32_t words_needed_ = 0; // data words the current command still needs
    std::vector<std::uint8_t> data_; // the current command's data so far
    std::function<void()> drained_;
    bool processing_ = false;
    bool held_ = false;           // input was kept because of a stall
    bool stop_requested_ = false; // FBRST.STP during a command: stop when it ends
};

} // namespace dq8::rt
